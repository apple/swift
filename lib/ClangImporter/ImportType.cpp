//===--- ImportType.cpp - Import Clang Types ------------------------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2018 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//
//
// This file implements support for importing Clang types as Swift types.
//
//===----------------------------------------------------------------------===//

#include "CFTypeInfo.h"
#include "ClangDiagnosticConsumer.h"
#include "ImporterImpl.h"
#include "swift/ABI/MetadataValues.h"
#include "swift/AST/ASTContext.h"
#include "swift/AST/Decl.h"
#include "swift/AST/DefaultArgumentKind.h"
#include "swift/AST/DiagnosticEngine.h"
#include "swift/AST/DiagnosticsClangImporter.h"
#include "swift/AST/ExistentialLayout.h"
#include "swift/AST/GenericEnvironment.h"
#include "swift/AST/GenericParamList.h"
#include "swift/AST/GenericSignature.h"
#include "swift/AST/Module.h"
#include "swift/AST/NameLookup.h"
#include "swift/AST/ParameterList.h"
#include "swift/AST/PrettyStackTrace.h"
#include "swift/AST/Type.h"
#include "swift/AST/TypeVisitor.h"
#include "swift/AST/Types.h"
#include "swift/ClangImporter/ClangImporterRequests.h"
#include "swift/ClangImporter/ClangModule.h"
#include "swift/Parse/Token.h"
#include "swift/Strings.h"
#include "clang/AST/ASTContext.h"
#include "clang/AST/DeclCXX.h"
#include "clang/AST/DeclObjCCommon.h"
#include "clang/AST/DeclTemplate.h"
#include "clang/AST/TypeVisitor.h"
#include "clang/Basic/Builtins.h"
#include "clang/Lex/Preprocessor.h"
#include "clang/Sema/Lookup.h"
#include "clang/Sema/Sema.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/Compiler.h"

using namespace swift;
using namespace importer;

/// Given that a type is the result of a special typedef import, was
/// it originally a CF pointer?
static bool isImportedCFPointer(clang::QualType clangType, Type type) {
  return (clangType->isPointerType() &&
          (type->is<ClassType>() || type->isClassExistentialType()));
}

bool ClangImporter::Implementation::isOverAligned(const clang::TypeDecl *decl) {
  auto type = getClangASTContext().getTypeDeclType(decl);
  return isOverAligned(type);
}

bool ClangImporter::Implementation::isOverAligned(clang::QualType type) {
  // Do not check type layout for a clang type in symbolic mode as the
  // type could be a dependent type.
  if (importSymbolicCXXDecls)
    return false;
  auto align = getClangASTContext().getTypeAlignInChars(type);
  return align > clang::CharUnits::fromQuantity(MaximumAlignment);
}

namespace {
  /// Various types that we want to do something interesting to after
  /// importing them.
  struct ImportHint {
    enum ImportHintKind {
      /// There is nothing special about the source type.
      None,

      /// The source type is 'void'.
      Void,

      /// The source type is 'BOOL' or 'Boolean' -- a type mapped to Swift's
      /// 'Bool'.
      Boolean,

      /// The source type is 'NSUInteger'.
      NSUInteger,

      /// The source type is 'va_list'.
      VAList,

      /// The source type is an Objective-C class type bridged to a Swift
      /// type.
      ObjCBridged,

      /// The source type is an Objective-C object pointer type.
      ObjCPointer,

      /// The source type is a CF object pointer type.
      CFPointer,

      /// The source type is a block pointer type.
      Block,

      /// The source type is a function pointer type.
      CFunctionPointer,

      /// The source type is any other pointer type.
      OtherPointer,
    };

    ImportHintKind Kind;

    /// The type to which the imported type is bridged.
    Type BridgedType;

    /// Allow conversion from an import hint to an import hint kind,
    /// which is useful for switches and comparisons.
    operator ImportHintKind() const { return Kind; }

    ImportHint(ImportHintKind kind) : Kind(kind) {
      assert(kind != ObjCBridged &&
             "Bridged entry point requires a bridged type");
    }

    ImportHint(ImportHintKind kind, Type bridgedType)
        : Kind(kind), BridgedType(bridgedType) {
      assert(kind == ImportHint::ObjCBridged && "Wrong kind for bridged type");
    }
  };

  bool canImportAsOptional(ImportHint hint) {
    // See also ClangImporter.cpp's canImportAsOptional.
    switch (hint) {
    case ImportHint::None:
    case ImportHint::Boolean:
    case ImportHint::NSUInteger:
    case ImportHint::Void:
      return false;

    case ImportHint::Block:
    case ImportHint::CFPointer:
    case ImportHint::ObjCBridged:
    case ImportHint::ObjCPointer:
    case ImportHint::CFunctionPointer:
    case ImportHint::OtherPointer:
    case ImportHint::VAList:
      return true;
    }

    llvm_unreachable("Invalid ImportHint.");
  }

  struct ImportResult {
    Type AbstractType;
    ImportHint Hint;

    /*implicit*/ ImportResult(Type type = Type(),
                              ImportHint hint = ImportHint::None)
      : AbstractType(type), Hint(hint) {}

    /*implicit*/ ImportResult(TypeBase *type,
                              ImportHint hint = ImportHint::None)
      : AbstractType(type), Hint(hint) {}

    explicit operator bool() const { return (bool) AbstractType; }
  };

  static ImportResult importFunctionPointerLikeType(const clang::Type &type,
                                                    const Type &pointeeType) {
    auto funcTy = pointeeType->castTo<FunctionType>();
    return {FunctionType::get(
                funcTy->getParams(), funcTy->getResult(),
                funcTy->getExtInfo()
                    .intoBuilder()
                    .withRepresentation(
                        AnyFunctionType::Representation::CFunctionPointer)
                    .withClangFunctionType(&type)
                    .build()),
            type.isReferenceType() ? ImportHint::None
                                   : ImportHint::CFunctionPointer};
  }

  static ImportResult importOverAlignedFunctionPointerLikeType(
      const clang::Type &type, ClangImporter::Implementation &Impl) {
    auto opaquePointer = Impl.SwiftContext.getOpaquePointerDecl();
    if (!opaquePointer) {
      return Type();
    }
    return {opaquePointer->getDeclaredInterfaceType(),
            type.isReferenceType() ? ImportHint::None
                                    : ImportHint::OtherPointer};
  }

  class SwiftTypeConverter :
    public clang::TypeVisitor<SwiftTypeConverter, ImportResult>
  {
    ClangImporter::Implementation &Impl;
    llvm::function_ref<void(Diagnostic &&)> addImportDiagnostic;
    bool AllowNSUIntegerAsInt;
    Bridgeability Bridging;
    const clang::FunctionType *CompletionHandlerType;
    Optional<unsigned> CompletionHandlerErrorParamIndex;

  public:
    SwiftTypeConverter(ClangImporter::Implementation &impl,
                       llvm::function_ref<void(Diagnostic &&)> addDiag,
                       bool allowNSUIntegerAsInt,
                       Bridgeability bridging,
                       const clang::FunctionType *completionHandlerType,
                       Optional<unsigned> completionHandlerErrorParamIndex)
      : Impl(impl), addImportDiagnostic(addDiag),
        AllowNSUIntegerAsInt(allowNSUIntegerAsInt), Bridging(bridging),
        CompletionHandlerType(completionHandlerType),
        CompletionHandlerErrorParamIndex(completionHandlerErrorParamIndex) {}

    using TypeVisitor::Visit;
    ImportResult Visit(clang::QualType type) {
      PrettyStackTraceClangType trace(Impl.getClangASTContext(),
                                      "importing a clang type",
                                      type.getTypePtr());

      auto IR = Visit(type.getTypePtr());
      return IR;
    }

    ImportResult VisitType(const Type*) = delete;

    // TODO(https://github.com/apple/swift/issues/56206): Add support for dependent types.
#define DEPENDENT_TYPE(Class, Base)                                            \
  ImportResult Visit##Class##Type(const clang::Class##Type *) { return Impl.SwiftContext.getAnyExistentialType(); }
#define TYPE(Class, Base)
#include "clang/AST/TypeNodes.inc"

    // Given a loaded type like CInt, look through the type alias sugar that the
    // stdlib uses to show the underlying type.  We want to import the signature
    // of the exit(3) libc function as "func exit(Int32)", not as
    // "func exit(CInt)".
    static Type unwrapCType(Type T) {
      // Handle missing or invalid stdlib declarations
      if (!T || T->hasError())
        return Type();
      if (auto *NAT = dyn_cast<TypeAliasType>(T.getPointer()))
        return NAT->getSinglyDesugaredType();
      return T;
    }

    ImportResult VisitBuiltinType(const clang::BuiltinType *type) {
      switch (type->getKind()) {
      case clang::BuiltinType::Void:
        return { Type(), ImportHint::Void };

#define MAP_BUILTIN_TYPE(CLANG_BUILTIN_KIND, SWIFT_TYPE_NAME)             \
      case clang::BuiltinType::CLANG_BUILTIN_KIND:                        \
        return unwrapCType(Impl.getNamedSwiftType(Impl.getStdlibModule(), \
                                        #SWIFT_TYPE_NAME));
#define MAP_BUILTIN_CCHAR_TYPE(CLANG_BUILTIN_KIND, SWIFT_TYPE_NAME)       \
      case clang::BuiltinType::CLANG_BUILTIN_KIND:                        \
        return Impl.getNamedSwiftType(Impl.getStdlibModule(), #SWIFT_TYPE_NAME);
#include "swift/ClangImporter/BuiltinMappedTypes.def"

      // Types that cannot be mapped into Swift, and probably won't ever be.
      case clang::BuiltinType::Dependent:
      case clang::BuiltinType::ARCUnbridgedCast:
      case clang::BuiltinType::BoundMember:
      case clang::BuiltinType::BuiltinFn:
      case clang::BuiltinType::IncompleteMatrixIdx:
      case clang::BuiltinType::Overload:
      case clang::BuiltinType::PseudoObject:
      case clang::BuiltinType::UnknownAny:
        return Type();

      // FIXME: Types that can be mapped, but aren't yet.
      case clang::BuiltinType::ShortAccum:
      case clang::BuiltinType::Accum:
      case clang::BuiltinType::LongAccum:
      case clang::BuiltinType::UShortAccum:
      case clang::BuiltinType::UAccum:
      case clang::BuiltinType::ULongAccum:
      case clang::BuiltinType::ShortFract:
      case clang::BuiltinType::Fract:
      case clang::BuiltinType::LongFract:
      case clang::BuiltinType::UShortFract:
      case clang::BuiltinType::UFract:
      case clang::BuiltinType::ULongFract:
      case clang::BuiltinType::SatShortAccum:
      case clang::BuiltinType::SatAccum:
      case clang::BuiltinType::SatLongAccum:
      case clang::BuiltinType::SatUShortAccum:
      case clang::BuiltinType::SatUAccum:
      case clang::BuiltinType::SatULongAccum:
      case clang::BuiltinType::SatShortFract:
      case clang::BuiltinType::SatFract:
      case clang::BuiltinType::SatLongFract:
      case clang::BuiltinType::SatUShortFract:
      case clang::BuiltinType::SatUFract:
      case clang::BuiltinType::SatULongFract:
      case clang::BuiltinType::BFloat16:
      case clang::BuiltinType::Float128:
      case clang::BuiltinType::NullPtr:
      case clang::BuiltinType::Char8:
      case clang::BuiltinType::Ibm128:
        return Type();

      // Objective-C types that aren't mapped directly; rather, pointers to
      // these types will be mapped.
      case clang::BuiltinType::ObjCClass:
      case clang::BuiltinType::ObjCId:
      case clang::BuiltinType::ObjCSel:
        return Type();

      // OpenCL types that don't have Swift equivalents.
      case clang::BuiltinType::OCLImage1dRO:
      case clang::BuiltinType::OCLImage1dRW:
      case clang::BuiltinType::OCLImage1dWO:
      case clang::BuiltinType::OCLImage1dArrayRO:
      case clang::BuiltinType::OCLImage1dArrayRW:
      case clang::BuiltinType::OCLImage1dArrayWO:
      case clang::BuiltinType::OCLImage1dBufferRO:
      case clang::BuiltinType::OCLImage1dBufferRW:
      case clang::BuiltinType::OCLImage1dBufferWO:
      case clang::BuiltinType::OCLImage2dRO:
      case clang::BuiltinType::OCLImage2dRW:
      case clang::BuiltinType::OCLImage2dWO:
      case clang::BuiltinType::OCLImage2dArrayRO:
      case clang::BuiltinType::OCLImage2dArrayRW:
      case clang::BuiltinType::OCLImage2dArrayWO:
      case clang::BuiltinType::OCLImage2dDepthRO:
      case clang::BuiltinType::OCLImage2dDepthRW:
      case clang::BuiltinType::OCLImage2dDepthWO:
      case clang::BuiltinType::OCLImage2dArrayDepthRO:
      case clang::BuiltinType::OCLImage2dArrayDepthRW:
      case clang::BuiltinType::OCLImage2dArrayDepthWO:
      case clang::BuiltinType::OCLImage2dMSAARO:
      case clang::BuiltinType::OCLImage2dMSAARW:
      case clang::BuiltinType::OCLImage2dMSAAWO:
      case clang::BuiltinType::OCLImage2dArrayMSAARO:
      case clang::BuiltinType::OCLImage2dArrayMSAARW:
      case clang::BuiltinType::OCLImage2dArrayMSAAWO:
      case clang::BuiltinType::OCLImage2dMSAADepthRO:
      case clang::BuiltinType::OCLImage2dMSAADepthRW:
      case clang::BuiltinType::OCLImage2dMSAADepthWO:
      case clang::BuiltinType::OCLImage2dArrayMSAADepthRO:
      case clang::BuiltinType::OCLImage2dArrayMSAADepthRW:
      case clang::BuiltinType::OCLImage2dArrayMSAADepthWO:
      case clang::BuiltinType::OCLImage3dRO:
      case clang::BuiltinType::OCLImage3dRW:
      case clang::BuiltinType::OCLImage3dWO:
      case clang::BuiltinType::OCLSampler:
      case clang::BuiltinType::OCLEvent:
      case clang::BuiltinType::OCLClkEvent:
      case clang::BuiltinType::OCLQueue:
      case clang::BuiltinType::OCLReserveID:
      case clang::BuiltinType::OCLIntelSubgroupAVCMcePayload:
      case clang::BuiltinType::OCLIntelSubgroupAVCImePayload:
      case clang::BuiltinType::OCLIntelSubgroupAVCRefPayload:
      case clang::BuiltinType::OCLIntelSubgroupAVCSicPayload:
      case clang::BuiltinType::OCLIntelSubgroupAVCMceResult:
      case clang::BuiltinType::OCLIntelSubgroupAVCImeResult:
      case clang::BuiltinType::OCLIntelSubgroupAVCRefResult:
      case clang::BuiltinType::OCLIntelSubgroupAVCSicResult:
      case clang::BuiltinType::OCLIntelSubgroupAVCImeResultSingleRefStreamout:
      case clang::BuiltinType::OCLIntelSubgroupAVCImeResultDualRefStreamout:
      case clang::BuiltinType::OCLIntelSubgroupAVCImeSingleRefStreamin:
      case clang::BuiltinType::OCLIntelSubgroupAVCImeDualRefStreamin:
        return Type();

      // OpenMP types that don't have Swift equivalents.
      case clang::BuiltinType::OMPArraySection:
      case clang::BuiltinType::OMPArrayShaping:
      case clang::BuiltinType::OMPIterator:
        return Type();

      // ARM SVE builtin types that don't have Swift equivalents.
#define SVE_TYPE(Name, Id, ...) case clang::BuiltinType::Id:
#include "clang/Basic/AArch64SVEACLETypes.def"
        return Type();

      // PPC SVE builtin types that don't have Swift equivalents.
#define PPC_VECTOR_TYPE(Name, Id, Size) case clang::BuiltinType::Id:
#include "clang/Basic/PPCTypes.def"
        return Type();

      // RISC-V V builtin types that don't have Swift equivalents.
#define RVV_TYPE(Name, Id, Size) case clang::BuiltinType::Id:
#include "clang/Basic/RISCVVTypes.def"
        return Type();
      }

      llvm_unreachable("Invalid BuiltinType.");
    }

    ImportResult VisitBitIntType(const clang::BitIntType *type) {
      Impl.addImportDiagnostic(type, Diagnostic(diag::unsupported_builtin_type,
                                                type->getTypeClassName()),
                               clang::SourceLocation());
      // BitInt is not supported in Swift.
      return Type();
    }

    ImportResult VisitPipeType(const clang::PipeType *type) {
      Impl.addImportDiagnostic(type, Diagnostic(diag::unsupported_builtin_type,
                                                type->getTypeClassName()),
                               clang::SourceLocation());
      // OpenCL types are not supported in Swift.
      return Type();
    }

    ImportResult VisitMatrixType(const clang::MatrixType *ty) {
      Impl.addImportDiagnostic(ty, Diagnostic(diag::unsupported_builtin_type,
                                              ty->getTypeClassName()),
                               clang::SourceLocation());
      // Matrix types are not supported in Swift.
      return Type();
    }

    ImportResult VisitComplexType(const clang::ComplexType *type) {
      Impl.addImportDiagnostic(type, Diagnostic(diag::unsupported_builtin_type,
                                                type->getTypeClassName()),
                               clang::SourceLocation());
      // FIXME: Implement once Complex is in the library.
      return Type();
    }

    ImportResult VisitAtomicType(const clang::AtomicType *type) {
      Impl.addImportDiagnostic(type, Diagnostic(diag::unsupported_builtin_type,
                                                type->getTypeClassName()),
                               clang::SourceLocation());
      // FIXME: handle pointers and fields of atomic type
      return Type();
    }

    ImportResult VisitMemberPointerType(const clang::MemberPointerType *type) {
      return Type();
    }

    ImportResult VisitPointerType(const clang::PointerType *type) {
      auto pointeeQualType = type->getPointeeType();
      auto quals = pointeeQualType.getQualifiers();

      // Special case for NSZone*, which has its own Swift wrapper.
      if (const clang::RecordType *pointee =
            pointeeQualType->getAsStructureType()) {
        if (pointee && !pointee->getDecl()->isCompleteDefinition() &&
            pointee->getDecl()->getName() == "_NSZone") {
          Identifier Id_ObjectiveC = Impl.SwiftContext.Id_ObjectiveC;
          ModuleDecl *objCModule = Impl.SwiftContext.getLoadedModule(Id_ObjectiveC);
          Type wrapperTy = Impl.getNamedSwiftType(
                             objCModule,
                             swift::getSwiftName(
                               KnownFoundationEntity::NSZone));
          if (wrapperTy)
            return {wrapperTy, ImportHint::OtherPointer};
        }
      }

      // Import 'void*' as 'UnsafeMutableRawPointer' and 'const void*' as
      // 'UnsafeRawPointer'. This is Swift's version of an untyped pointer. Note
      // that 'Unsafe[Mutable]Pointer<T>' implicitly converts to
      // 'Unsafe[Mutable]RawPointer' for interoperability.
      if (pointeeQualType->isVoidType()) {
        auto pointerTypeDecl =
          (quals.hasConst()
           ? Impl.SwiftContext.getUnsafeRawPointerDecl()
           : Impl.SwiftContext.getUnsafeMutableRawPointerDecl());
        if (!pointerTypeDecl)
          return Type();
        return {pointerTypeDecl->getDeclaredInterfaceType(),
                ImportHint::OtherPointer};
      }

      if (pointeeQualType->isDependentType())
        return Type();

      // All other C pointers to concrete types map to
      // UnsafeMutablePointer<T> or OpaquePointer.

      // With pointer conversions enabled, map to the normal pointer types
      // without special hints.
      Type pointeeType = Impl.importTypeIgnoreIUO(
          pointeeQualType, ImportTypeKind::Value, addImportDiagnostic,
          AllowNSUIntegerAsInt, Bridgeability::None, ImportTypeAttrs());

      // If this is imported as a reference type, ignore the pointer.
      if (pointeeType && pointeeType->isForeignReferenceType())
         return {pointeeType, ImportHint::OtherPointer};

      // If the pointed-to type is unrepresentable in Swift, or its C
      // alignment is greater than the maximum Swift alignment, import as
      // OpaquePointer.
      if (!pointeeType || Impl.isOverAligned(pointeeQualType)) {
        return importOverAlignedFunctionPointerLikeType(*type, Impl);
      }

      if (pointeeQualType->isFunctionType()) {
        return importFunctionPointerLikeType(*type, pointeeType);
      }

      PointerTypeKind pointerKind;
      if (quals.hasConst()) {
        pointerKind = PTK_UnsafePointer;
      } else {
        switch (quals.getObjCLifetime()) {
        case clang::Qualifiers::OCL_Autoreleasing:
        case clang::Qualifiers::OCL_ExplicitNone:
          // Mutable pointers with __autoreleasing or __unsafe_unretained
          // ownership map to AutoreleasingUnsafeMutablePointer<T>.
          pointerKind = PTK_AutoreleasingUnsafeMutablePointer;

          // FIXME: We have tests using a non-Apple stdlib that nevertheless
          // exercise ObjC interop. Fail softly for those tests.
          if (!Impl.SwiftContext.getAutoreleasingUnsafeMutablePointerDecl())
            return Type();
          break;

        case clang::Qualifiers::OCL_Weak:
          // FIXME: We should refuse to import this.
          LLVM_FALLTHROUGH;

        case clang::Qualifiers::OCL_None:
        case clang::Qualifiers::OCL_Strong:
          // All other mutable pointers map to UnsafeMutablePointer.
          pointerKind = PTK_UnsafeMutablePointer;
        }
      }
      if (auto wrapped = pointeeType->wrapInPointer(pointerKind)) {
        return {wrapped, ImportHint::OtherPointer};
      } else {
        return Type();
      }
    }

    ImportResult VisitBlockPointerType(const clang::BlockPointerType *type) {
      // Block pointer types are mapped to function types.
      Type pointeeType = Visit(type->getPointeeType()).AbstractType;
      if (!pointeeType)
        return Type();
      FunctionType *fTy = pointeeType->castTo<FunctionType>();

      auto extInfo =
          fTy->getExtInfo()
              .intoBuilder()
              .withRepresentation(FunctionType::Representation::Block)
              .withClangFunctionType(type)
              .build();
      auto funcTy =
          FunctionType::get(fTy->getParams(), fTy->getResult(), extInfo);
      return { funcTy, ImportHint::Block };
    }

    ImportResult VisitReferenceType(const clang::ReferenceType *type) {
      auto pointeeQualType = type->getPointeeType();
      auto quals = pointeeQualType.getQualifiers();
      Type pointeeType =
          Impl.importTypeIgnoreIUO(pointeeQualType, ImportTypeKind::Value,
                                   addImportDiagnostic,
                                   AllowNSUIntegerAsInt, Bridgeability::None,
                                   ImportTypeAttrs());
      if (!pointeeType)
        return Type();

      if (pointeeType->isForeignReferenceType())
        return {pointeeType, ImportHint::None};

      if (pointeeQualType->isFunctionType()) {
        return importFunctionPointerLikeType(*type, pointeeType);
      }

      // Currently, we can't generate thunks for references to dependent types
      // because there's no way to cast without a copy (without writing the SIL
      // manually).
      if (pointeeQualType->isDependentType())
        return Type();

      if (Impl.isOverAligned(pointeeQualType)) {
        return importOverAlignedFunctionPointerLikeType(*type, Impl);
      }

      PointerTypeKind pointerKind;
      if (quals.hasConst()) {
        pointerKind = PTK_UnsafePointer;
      } else {
        pointerKind = PTK_UnsafeMutablePointer;
      }

      return {pointeeType->wrapInPointer(pointerKind),
              ImportHint::None};
    }

    ImportResult VisitMemberPointer(const clang::MemberPointerType *type) {
      // FIXME: Member function pointers can be mapped to curried functions,
      // but only when we can express the notion of a function that does
      // not capture anything from its enclosing context.
      return Type();
    }

    ImportResult VisitArrayType(const clang::ArrayType *type) {
      // FIXME: Array types will need to be mapped differently depending on
      // context.
      return Type();
    }

    ImportResult VisitConstantArrayType(const clang::ConstantArrayType *type) {
      // FIXME: Map to a real fixed-size Swift array type when we have those.
      // Importing as a tuple at least fills the right amount of space, and
      // we can cheese static-offset "indexing" using .$n operations.

      Type elementType = Impl.importTypeIgnoreIUO(
          type->getElementType(), ImportTypeKind::Value, addImportDiagnostic,
          AllowNSUIntegerAsInt, Bridgeability::None, ImportTypeAttrs());
      if (!elementType)
        return Type();

      auto size = type->getSize().getZExtValue();
      // An array of size N is imported as an N-element tuple which
      // takes very long to compile. We chose 4096 as the upper limit because
      // we don't want to break arrays of size PATH_MAX.
      if (size > 4096)
        return Type();
      
      if (size == 1)
        return ParenType::get(elementType->getASTContext(), elementType);

      SmallVector<TupleTypeElt, 8> elts{static_cast<size_t>(size), elementType};
      return TupleType::get(elts, elementType->getASTContext());
    }

    ImportResult VisitVectorType(const clang::VectorType *type) {
      // Get the imported element type and count.
      Type element = Impl.importTypeIgnoreIUO(
        type->getElementType(), ImportTypeKind::Abstract, addImportDiagnostic,
        false /* No NSUIntegerAsInt */, Bridgeability::None, ImportTypeAttrs(),
        OptionalTypeKind::OTK_None);
      if (!element) { return Type(); }
      unsigned count = type->getNumElements();
      // Import vector-of-one as the element type.
      if (count == 1) { return element; }
      // Imported element type needs to conform to SIMDScalar.
      auto nominal = element->getAnyNominal();
      auto simdscalar = Impl.SwiftContext.getProtocol(KnownProtocolKind::SIMDScalar);
      SmallVector<ProtocolConformance *, 2> conformances;
      if (simdscalar && nominal->lookupConformance(simdscalar, conformances)) {
        // Element type conforms to SIMDScalar. Get the SIMDn generic type
        // if it exists.
        SmallString<8> name("SIMD");
        name.append(std::to_string(count));
        if (auto vector = Impl.getNamedSwiftType(Impl.getStdlibModule(), name)) {
          if (auto unbound = vector->getAs<UnboundGenericType>()) {
            // All checks passed: the imported element type is SIMDScalar,
            // and a generic SIMDn type exists with n == count. Construct the
            // bound generic type and return that.
            return BoundGenericType::get(
              cast<NominalTypeDecl>(unbound->getDecl()), Type(), { element }
            );
          }
        }
      }
      return Type();
    }

    ImportResult VisitFunctionProtoType(const clang::FunctionProtoType *type) {
      // C-style variadic functions cannot be called from Swift.
      if (type->isVariadic())
        return Type();

      // Import the result type.
      auto resultTy = Impl.importTypeIgnoreIUO(
          type->getReturnType(), ImportTypeKind::Result, addImportDiagnostic,
          AllowNSUIntegerAsInt, Bridging, ImportTypeAttrs(), OTK_Optional);
      if (!resultTy)
        return Type();

      SmallVector<FunctionType::Param, 4> params;
      for (auto param = type->param_type_begin(),
             paramEnd = type->param_type_end();
           param != paramEnd; ++param) {
        // Determine whether we have a result parameter of a completion
        // handler that can also express a thrown error.
        ImportTypeKind paramImportKind = ImportTypeKind::Parameter;
        unsigned paramIdx = param - type->param_type_begin();
        if (CompletionHandlerType &&
            Impl.getClangASTContext().hasSameType(
                CompletionHandlerType, type) &&
            paramIdx != CompletionHandlerErrorParamIndex) {
          paramImportKind = ImportTypeKind::CompletionHandlerResultParameter;
        }

        auto paramQualType = *param;
        if (paramQualType->isReferenceType() &&
            paramQualType->getPointeeType().isConstQualified())
          paramQualType = paramQualType->getPointeeType();

        auto swiftParamTy = Impl.importTypeIgnoreIUO(
            paramQualType, paramImportKind, addImportDiagnostic,
            AllowNSUIntegerAsInt, Bridging, ImportTypeAttrs(), OTK_Optional);
        if (!swiftParamTy)
          return Type();

        // FIXME(https://github.com/apple/swift/issues/45134): If we were walking TypeLocs, we could actually get parameter names.
        // The probably doesn't matter outside of a FuncDecl, which we'll have
        // to special-case, but it's an interesting bit of data loss.
        params.push_back(FunctionType::Param(swiftParamTy));
      }

      // Form the function type.
      return FunctionType::get(params, resultTy, FunctionType::ExtInfo());
    }

    ImportResult
    VisitFunctionNoProtoType(const clang::FunctionNoProtoType *type) {
      // Import functions without prototypes as functions with no parameters.
      auto resultTy = Impl.importTypeIgnoreIUO(
          type->getReturnType(), ImportTypeKind::Result, addImportDiagnostic,
          AllowNSUIntegerAsInt, Bridging, ImportTypeAttrs(), OTK_Optional);
      if (!resultTy)
        return Type();

      // FIXME: Verify ExtInfo state is correct, not working by accident.
      FunctionType::ExtInfo info;
      return FunctionType::get({}, resultTy, info);
    }

    ImportResult VisitParenType(const clang::ParenType *type) {
      auto inner = Visit(type->getInnerType());
      if (!inner)
        return Type();

      return { ParenType::get(Impl.SwiftContext, inner.AbstractType),
               inner.Hint };
    }

    /// Imports the type defined by \p objcTypeParamDecl.
    ///
    /// If the type parameter is not imported for some reason, returns \c None.
    /// This is different from a failure; it means the caller should try
    /// importing the underlying type instead.
    Optional<ImportResult>
    importObjCTypeParamDecl(const clang::ObjCTypeParamDecl *objcTypeParamDecl) {
      // Pull the corresponding generic type parameter from the imported class.
      const auto *typeParamContext = objcTypeParamDecl->getDeclContext();
      GenericSignature genericSig;
      if (auto *category =
            dyn_cast<clang::ObjCCategoryDecl>(typeParamContext)) {
        auto ext = cast_or_null<ExtensionDecl>(
            Impl.importDecl(category, Impl.CurrentVersion));
        if (!ext)
          return ImportResult();
        genericSig = ext->getGenericSignature();
      } else if (auto *interface =
          dyn_cast<clang::ObjCInterfaceDecl>(typeParamContext)) {
        auto cls = castIgnoringCompatibilityAlias<ClassDecl>(
            Impl.importDecl(interface, Impl.CurrentVersion));
        if (!cls)
          return ImportResult();
        genericSig = cls->getGenericSignature();
      }
      unsigned index = objcTypeParamDecl->getIndex();
      // Pull the generic param decl out of the imported class.
      if (!genericSig) {
        // The ObjC type param didn't get imported, possibly because it was
        // suppressed. Treat it as a typedef.
        return None;
      }
      if (index > genericSig.getGenericParams().size()) {
        return ImportResult();
      }

      return ImportResult(genericSig.getGenericParams()[index],
                          ImportHint::ObjCPointer);
    }

    ImportResult VisitObjCTypeParamType(const clang::ObjCTypeParamType *type) {
      // FIXME: This drops any added protocols on the floor, which is the whole
      // point of ObjCTypeParamType. Fixing this might be source-breaking,
      // though. rdar://problem/29763975
      if (auto result = importObjCTypeParamDecl(type->getDecl()))
        return result.value();
      // Fall back to importing the desugared type, which uses the parameter's
      // bound. This isn't perfect but it's better than dropping the type.
      return Visit(type->getLocallyUnqualifiedSingleStepDesugaredType());
    }

    ImportResult VisitTypedefType(const clang::TypedefType *type) {
      // If the underlying declaration is an Objective-C type parameter,
      // pull the corresponding generic type parameter from the imported class.
      if (auto *objcTypeParamDecl =
            dyn_cast<clang::ObjCTypeParamDecl>(type->getDecl())) {
        if (auto result = importObjCTypeParamDecl(objcTypeParamDecl))
          return result.value();
        return Visit(type->getLocallyUnqualifiedSingleStepDesugaredType());
      }

      // Import the underlying declaration.
      auto decl = dyn_cast_or_null<TypeDecl>(
          Impl.importDecl(type->getDecl(), Impl.CurrentVersion));

      // If that fails, fall back on importing the underlying type.
      if (!decl) return Visit(type->desugar());

      Type mappedType = decl->getDeclaredInterfaceType();

      if (getSwiftNewtypeAttr(type->getDecl(), Impl.CurrentVersion)) {
        auto underlying = Visit(type->getDecl()->getUnderlyingType());
        switch (underlying.Hint) {
        case ImportHint::None:
        case ImportHint::Void:
        case ImportHint::Block:
        case ImportHint::CFPointer:
        case ImportHint::ObjCPointer:
        case ImportHint::CFunctionPointer:
        case ImportHint::OtherPointer:
        case ImportHint::VAList:
          return {mappedType, underlying.Hint};

        case ImportHint::Boolean:
        case ImportHint::NSUInteger:
          // Toss out the special rules for these types; we still want to
          // import as a wrapper.
          return {mappedType, ImportHint::None};

        case ImportHint::ObjCBridged:
          // If the underlying type was bridged, the wrapper type is
          // only useful in bridged cases. Exit early.
          return { underlying.AbstractType,
                   ImportHint(ImportHint::ObjCBridged, mappedType) };
        }
      }

      // For certain special typedefs, we don't want to use the imported type.
      if (auto specialKind = Impl.getSpecialTypedefKind(type->getDecl())) {
        switch (specialKind.value()) {
        case MappedTypeNameKind::DoNothing:
        case MappedTypeNameKind::DefineAndUse:
          break;
        case MappedTypeNameKind::DefineOnly:
          if (auto typealias = dyn_cast<TypeAliasDecl>(decl))
            mappedType = typealias->getDeclaredInterfaceType()
              ->getDesugaredType();
          break;
        }

        static const llvm::StringLiteral vaListNames[] = {
          "va_list", "__gnuc_va_list", "__va_list"
        };

        ImportHint hint = ImportHint::None;
        if (type->getDecl()->getName() == "BOOL") {
          hint = ImportHint::Boolean;
        } else if (type->getDecl()->getName() == "Boolean") {
          // FIXME: Darwin only?
          hint = ImportHint::Boolean;
        } else if (type->getDecl()->getName() == "NSUInteger") {
          hint = ImportHint::NSUInteger;
        } else if (llvm::is_contained(vaListNames,
                                      type->getDecl()->getName())) {
          hint = ImportHint::VAList;
        } else if (isImportedCFPointer(type->desugar(), mappedType)) {
          hint = ImportHint::CFPointer;
        } else if (mappedType->isAnyExistentialType()) { // id, Class
          hint = ImportHint::ObjCPointer;
        } else if (type->isPointerType() || type->isBlockPointerType()) {
          hint = ImportHint::OtherPointer;
        }
        // Any other interesting mapped types should be hinted here.
        return { mappedType, hint };
      }

      // Otherwise, recurse on the underlying type.  We need to recompute
      // the hint, and if the typedef uses different bridgeability than the
      // context then we may also need to bypass the typedef.
      auto underlyingResult = Visit(type->desugar());

      // If we used different bridgeability than this typedef normally
      // would because we're in a non-bridgeable context, and therefore
      // the underlying type is different from the mapping of the typedef,
      // use the underlying type.
      if (Bridging != getTypedefBridgeability(type->getDecl()) &&
          !underlyingResult.AbstractType->isEqual(mappedType)) {
        return underlyingResult;
      }

#ifndef NDEBUG
      switch (underlyingResult.Hint) {
      case ImportHint::Block:
      case ImportHint::ObjCBridged:
        // Bridging is fine for Objective-C and blocks.
        break;
      case ImportHint::NSUInteger:
        // NSUInteger might be imported as Int rather than UInt depending
        // on where the import lives.
        if (underlyingResult.AbstractType->isInt())
          break;
        LLVM_FALLTHROUGH;
      default:
        if (!underlyingResult.AbstractType->isEqual(mappedType)) {
          underlyingResult.AbstractType->dump(llvm::errs());
          mappedType->dump(llvm::errs());
        }
        assert(underlyingResult.AbstractType->isEqual(mappedType) &&
               "typedef without special typedef kind was mapped "
               "differently from its underlying type?");
      }
#endif

      // If the imported typealias is unavailable, return the underlying type.
      if (decl->getAttrs().isUnavailable(Impl.SwiftContext))
        return underlyingResult;

      return { mappedType, underlyingResult.Hint };
    }

    // TODO: add custom visitors for these types.
#define MAYBE_SUGAR_TYPE(KIND)                                                 \
    ImportResult Visit##KIND##Type(const clang::KIND##Type *type) {            \
      if (type->isSugared())                                                   \
        return Visit(type->desugar());                                         \
      return Type();                                                           \
    }
    MAYBE_SUGAR_TYPE(TypeOfExpr)
    MAYBE_SUGAR_TYPE(TypeOf)
    MAYBE_SUGAR_TYPE(Decltype)
    MAYBE_SUGAR_TYPE(UnaryTransform)
    MAYBE_SUGAR_TYPE(TemplateSpecialization)
    MAYBE_SUGAR_TYPE(Auto)
    MAYBE_SUGAR_TYPE(DeducedTemplateSpecialization)

    // These types are ALWAYS sugared.
#define SUGAR_TYPE(KIND)                                                       \
    ImportResult Visit##KIND##Type(const clang::KIND##Type *type) {            \
      return Visit(type->desugar());                                           \
    }
    SUGAR_TYPE(MacroQualified)
    SUGAR_TYPE(Attributed)
    SUGAR_TYPE(Adjusted)
    SUGAR_TYPE(SubstTemplateTypeParm)
    SUGAR_TYPE(Elaborated)
    SUGAR_TYPE(Using)
    SUGAR_TYPE(BTFTagAttributed)

    ImportResult VisitDecayedType(const clang::DecayedType *type) {
      clang::ASTContext &clangCtx = Impl.getClangASTContext();
      if (clangCtx.hasSameType(type->getOriginalType(),
                               clangCtx.getBuiltinVaListType())) {
        return {Impl.getNamedSwiftType(Impl.getStdlibModule(),
                                       "CVaListPointer"),
                ImportHint::VAList};
      }
      return Visit(type->desugar());
    }

    ImportResult VisitRecordType(const clang::RecordType *type) {
      auto decl = dyn_cast_or_null<TypeDecl>(
          Impl.importDecl(type->getDecl(), Impl.CurrentVersion));
      if (!decl)
        return nullptr;

      return decl->getDeclaredInterfaceType();
    }

    ImportResult VisitEnumType(const clang::EnumType *type) {
      auto clangDecl = type->getDecl()->getDefinition();
      if (!clangDecl) {
        // FIXME: If the enum has a fixed underlying type, can we use that
        // instead? Or import it opaquely somehow?
        return nullptr;
      }
      switch (Impl.getEnumKind(clangDecl)) {
      case EnumKind::Constants: {
        // Map anonymous enums with no fixed underlying type to Int /if/
        // they fit in an Int32. If not, this mapping isn't guaranteed to be
        // consistent for all platforms we care about.
        if (!clangDecl->isFixed() && clangDecl->isFreeStanding() &&
            clangDecl->getNumPositiveBits() < 32 &&
            clangDecl->getNumNegativeBits() <= 32)
          return Impl.getNamedSwiftType(Impl.getStdlibModule(), "Int");

        // Import the underlying integer type.
        return Visit(clangDecl->getIntegerType());
      }
      case EnumKind::NonFrozenEnum:
      case EnumKind::FrozenEnum:
      case EnumKind::Unknown:
      case EnumKind::Options: {
        auto decl = dyn_cast_or_null<TypeDecl>(
            Impl.importDecl(clangDecl, Impl.CurrentVersion));
        if (!decl)
          return nullptr;

        return decl->getDeclaredInterfaceType();
      }
      }

      llvm_unreachable("Invalid EnumKind.");
    }

    ImportResult VisitObjCObjectType(const clang::ObjCObjectType *type) {
      // We only handle pointers to objects.
      return nullptr;
    }

    /// Map the Clang swift_bridge attribute to a specific type.
    Type mapSwiftBridgeAttr(const clang::NamedDecl *clangDecl) {
      // Check whether there is a swift_bridge attribute.
      if (Impl.DisableSwiftBridgeAttr)
        return Type();
      auto bridgeAttr = clangDecl->getAttr<clang::SwiftBridgeAttr>();
      if (!bridgeAttr) return Type();

      // Determine the module and Swift declaration names.
      StringRef moduleName;
      StringRef name = bridgeAttr->getSwiftType();
      auto dotPos = name.find('.');
      if (dotPos == StringRef::npos) {
        // Determine the module name from the Clang declaration.
        if (auto module = clangDecl->getImportedOwningModule())
          moduleName = module->getTopLevelModuleName();
        else
          moduleName = clangDecl->getASTContext().getLangOpts().CurrentModule;
      } else {
        // The string is ModuleName.TypeName.
        moduleName = name.substr(0, dotPos);
        name = name.substr(dotPos + 1);
      }

      return Impl.getNamedSwiftType(moduleName, name);
    }

    ImportResult
    VisitObjCObjectPointerType(const clang::ObjCObjectPointerType *type) {
      Type importedType = Impl.SwiftContext.getAnyObjectType();

      if (!type->qual_empty()) {
        for (auto cp = type->qual_begin(), end = type->qual_end(); cp != end;
             ++cp) {
          if (!(*cp)->hasDefinition())
            Impl.addImportDiagnostic(
                type, Diagnostic(diag::incomplete_protocol, *cp),
                clang::SourceLocation());
        }
      }

      // If this object pointer refers to an Objective-C class (possibly
      // qualified),
      if (auto objcClass = type->getInterfaceDecl()) {
        auto imported = castIgnoringCompatibilityAlias<ClassDecl>(
            Impl.importDecl(objcClass, Impl.CurrentVersion));
        if (!imported && !objcClass->hasDefinition())
          Impl.addImportDiagnostic(
              type, Diagnostic(diag::incomplete_interface, objcClass),
              clang::SourceLocation());

        if (!imported)
          return nullptr;

        // If the objc type has any generic args, convert them and bind them to
        // the imported class type.
        if (imported->getGenericParams()) {
          unsigned typeParamCount = imported->getGenericParams()->size();
          auto typeArgs = type->getObjectType()->getTypeArgs();
          assert(typeArgs.empty() || typeArgs.size() == typeParamCount);
          SmallVector<Type, 2> importedTypeArgs;
          importedTypeArgs.reserve(typeParamCount);
          if (!typeArgs.empty()) {
            for (auto typeArg : typeArgs) {
              Type importedTypeArg = Visit(typeArg).AbstractType;
              if (!importedTypeArg)
                return nullptr;
              importedTypeArgs.push_back(importedTypeArg);
            }
          } else {
            auto *genericEnv = imported->getGenericEnvironment();

            for (auto typeParam : imported->getGenericParams()->getParams()) {
              Type importedTypeArg = genericEnv->mapTypeIntoContext(
                  typeParam->getDeclaredInterfaceType())
                  ->castTo<ArchetypeType>()
                  ->getExistentialType();
              importedTypeArgs.push_back(importedTypeArg);
            }
          }
          assert(importedTypeArgs.size() == typeParamCount);
          importedType = BoundGenericClassType::get(
            imported, nullptr, importedTypeArgs);
        } else {
          importedType = imported->getDeclaredInterfaceType();
        }

        if (!type->qual_empty()) {
          // As a special case, turn 'NSObject <NSCopying>' into
          // 'id <NSObject, NSCopying>', which can be imported more usefully.
          Type nsObjectTy = Impl.getNSObjectType();
          if (!nsObjectTy) {
            // Input is malformed
            return {};
          }
          if (nsObjectTy && importedType->isEqual(nsObjectTy)) {
            // Skip if there is no NSObject protocol.
            auto nsObjectProtoType =
                Impl.getNSObjectProtocolType();
            if (nsObjectProtoType) {
              auto *nsObjectProto = nsObjectProtoType->getAnyNominal();
              if (!nsObjectProto) {
                // Input is malformed
                return {};
              }

              SmallVector<clang::ObjCProtocolDecl *, 4> protocols{
                type->qual_begin(), type->qual_end()
              };
              auto *clangProto =
                  cast<clang::ObjCProtocolDecl>(nsObjectProto->getClangDecl());
              protocols.push_back(
                  const_cast<clang::ObjCProtocolDecl *>(clangProto));

              clang::ASTContext &clangCtx = Impl.getClangASTContext();
              clang::QualType protosOnlyType =
                  clangCtx.getObjCObjectType(clangCtx.ObjCBuiltinIdTy,
                                             /*type args*/{},
                                             protocols,
                                             /*kindof*/false);
              return Visit(clangCtx.getObjCObjectPointerType(protosOnlyType));
            }
          }
        }

        // Determine whether this Objective-C class type is bridged to
        // a Swift type. Hardcode "NSString" since it's referenced from
        // the ObjectiveC module (a level below Foundation).
        Type bridgedType;
        if (auto objcClassDef = objcClass->getDefinition())
          bridgedType = mapSwiftBridgeAttr(objcClassDef);
        else if (objcClass->getName() == "NSString")
          bridgedType = Impl.SwiftContext.getStringType();

        if (bridgedType) {
          // Gather the type arguments.
          SmallVector<Type, 2> importedTypeArgs;
          ArrayRef<clang::QualType> typeArgs = type->getTypeArgs();
          SmallVector<clang::QualType, 2> typeArgsScratch;

          // If we have an unspecialized form of a parameterized
          // Objective-C class type, fill in the defaults.
          if (typeArgs.empty()) {
            if (auto objcGenericParams = objcClass->getTypeParamList()) {
              objcGenericParams->gatherDefaultTypeArgs(typeArgsScratch);
              typeArgs = typeArgsScratch;
            }
          }

          // Convert the type arguments.
          for (auto typeArg : typeArgs) {
            Type importedTypeArg = Impl.importTypeIgnoreIUO(
                typeArg, ImportTypeKind::ObjCCollectionElement,
                addImportDiagnostic, AllowNSUIntegerAsInt, Bridging,
                ImportTypeAttrs(), OTK_None);
            if (!importedTypeArg) {
              importedTypeArgs.clear();
              break;
            }

            importedTypeArgs.push_back(importedTypeArg);
          }

          // If we have an unbound generic bridged type, get the arguments.
          if (auto unboundType = bridgedType->getAs<UnboundGenericType>()) {
            auto unboundDecl = unboundType->getDecl();
            auto bridgedSig = unboundDecl->getGenericSignature();
            assert(bridgedSig && "Bridged signature");
            unsigned numExpectedTypeArgs = bridgedSig.getGenericParams().size();
            if (importedTypeArgs.size() != numExpectedTypeArgs)
              return Type();

            // The first type argument for Dictionary or Set needs
            // to be Hashable. If something isn't Hashable, fall back
            // to AnyHashable as a key type.
            if (unboundType->isDictionary() || unboundType->isSet()) {
              auto &keyType = importedTypeArgs[0];
              if (!Impl.matchesHashableBound(keyType) ||
                  // Dictionary and Array conditionally conform to Hashable,
                  // but the conformance doesn't necessarily apply with the
                  // imported versions of their type arguments.
                  // FIXME: Import their non-Hashable type parameters as
                  // AnyHashable in this context.
                  keyType->isDictionary() || keyType->isArray()) {
                keyType = Impl.SwiftContext.getAnyHashableType();
              }
            }

            // Form the specialized type.
            if (unboundType->isArray()) {
              // Type sugar for arrays.
              assert(importedTypeArgs.size() == 1);
              bridgedType = ArraySliceType::get(importedTypeArgs[0]);
            } else if (unboundType->isDictionary()) {
              // Type sugar for dictionaries.
              assert(importedTypeArgs.size() == 2);
              bridgedType = DictionaryType::get(importedTypeArgs[0],
                                                importedTypeArgs[1]);
            } else {
              // Everything else.
              bridgedType =
                  BoundGenericType::get(cast<NominalTypeDecl>(unboundDecl),
                                        Type(), importedTypeArgs);
            }
          }

          if (bridgedType->isConstraintType())
            bridgedType = ExistentialType::get(bridgedType);

          return { importedType,
                   ImportHint(ImportHint::ObjCBridged, bridgedType) };
        }
      }

      if (!type->qual_empty()) {
        SmallVector<Type, 4> members;
        if (!importedType->isAnyObject())
          members.push_back(importedType);

        for (auto cp = type->qual_begin(), cpEnd = type->qual_end();
             cp != cpEnd; ++cp) {
          auto proto = castIgnoringCompatibilityAlias<ProtocolDecl>(
            Impl.importDecl(*cp, Impl.CurrentVersion));
          if (!proto)
            return Type();

          members.push_back(proto->getDeclaredInterfaceType());
        }

        importedType = ExistentialType::get(
            ProtocolCompositionType::get(Impl.SwiftContext, members,
                                         /*HasExplicitAnyObject=*/false));
      }

      // Class or Class<P> maps to an existential metatype.
      if (type->isObjCClassType() ||
          type->isObjCQualifiedClassType()) {
        importedType = ExistentialMetatypeType::get(importedType);
        return { importedType, ImportHint::ObjCPointer };
      }

      // Beyond here, we're using AnyObject.

      // id maps to Any in bridgeable contexts, AnyObject otherwise.
      if (type->isObjCIdType()) {
        return { Impl.SwiftContext.getAnyObjectType(),
                 ImportHint(ImportHint::ObjCBridged,
                            Impl.SwiftContext.getAnyExistentialType())};
      }

      return { importedType, ImportHint::ObjCPointer };
    }
  };
} // end anonymous namespace

/// True if we're converting a function parameter, property type, or
/// function result type, and can thus safely apply representation
/// conversions for bridged types.
static bool canBridgeTypes(ImportTypeKind importKind) {
  switch (importKind) {
  case ImportTypeKind::Abstract:
  case ImportTypeKind::Value:
  case ImportTypeKind::Variable:
  case ImportTypeKind::AuditedVariable:
  case ImportTypeKind::Enum:
  case ImportTypeKind::RecordField:
    return false;
  case ImportTypeKind::Result:
  case ImportTypeKind::AuditedResult:
  case ImportTypeKind::Parameter:
  case ImportTypeKind::CompletionHandlerResultParameter:
  case ImportTypeKind::CFRetainedOutParameter:
  case ImportTypeKind::CFUnretainedOutParameter:
  case ImportTypeKind::Property:
  case ImportTypeKind::PropertyWithReferenceSemantics:
  case ImportTypeKind::ObjCCollectionElement:
  case ImportTypeKind::Typedef:
    return true;
  }

  llvm_unreachable("Invalid ImportTypeKind.");
}

/// True if the type has known CoreFoundation reference counting semantics.
static bool isCFAudited(ImportTypeKind importKind) {
  switch (importKind) {
  case ImportTypeKind::Abstract:
  case ImportTypeKind::Typedef:
  case ImportTypeKind::Value:
  case ImportTypeKind::ObjCCollectionElement:
  case ImportTypeKind::Variable:
  case ImportTypeKind::Result:
  case ImportTypeKind::Enum:
  case ImportTypeKind::RecordField:
    return false;
  case ImportTypeKind::AuditedVariable:
  case ImportTypeKind::AuditedResult:
  case ImportTypeKind::Parameter:
  case ImportTypeKind::CompletionHandlerResultParameter:
  case ImportTypeKind::CFRetainedOutParameter:
  case ImportTypeKind::CFUnretainedOutParameter:
  case ImportTypeKind::Property:
  case ImportTypeKind::PropertyWithReferenceSemantics:
    return true;
  }

  llvm_unreachable("Invalid ImportTypeKind.");
}

/// Turn T into Unmanaged<T>.
static Type getUnmanagedType(ClangImporter::Implementation &impl,
                             Type payloadType) {
  NominalTypeDecl *unmanagedDecl = impl.SwiftContext.getUnmanagedDecl();
  if (!unmanagedDecl || unmanagedDecl->getGenericParams()->size() != 1)
    return payloadType;

  Type unmanagedClassType = BoundGenericType::get(unmanagedDecl,
                                                  /*parent*/ Type(),
                                                  payloadType);
  return unmanagedClassType;
}

/// Determine whether type is 'NSString.
static bool isNSString(Type type) {
  if (auto classType = type->getAs<ClassType>()) {
    if (auto clangDecl = classType->getDecl()->getClangDecl()) {
      if (auto objcClass = dyn_cast<clang::ObjCInterfaceDecl>(clangDecl)) {
        return objcClass->getName() == "NSString";
      }
    }
  }

  return false;
}

static Type maybeImportNSErrorOutParameter(ClangImporter::Implementation &impl,
                                           Type importedType,
                                           bool resugarNSErrorPointer) {
  PointerTypeKind PTK;
  auto elementType = importedType->getAnyPointerElementType(PTK);
  if (!elementType || PTK != PTK_AutoreleasingUnsafeMutablePointer)
    return Type();

  auto elementObj = elementType->getOptionalObjectType();
  if (!elementObj)
    return Type();

  auto elementClass = elementObj->getClassOrBoundGenericClass();
  if (!elementClass)
    return Type();

  // FIXME: Avoid string comparison by caching this identifier.
  if (elementClass->getName().str() !=
        swift::getSwiftName(KnownFoundationEntity::NSError))
    return Type();

  if (!impl.canImportFoundationModule() ||
      !elementClass->getModuleContext()->isFoundationModule())
    return Type();

  ModuleDecl *foundationModule = impl.tryLoadFoundationModule();
  if (resugarNSErrorPointer)
    return impl.getNamedSwiftType(
      foundationModule,
        swift::getSwiftName(
          KnownFoundationEntity::NSErrorPointer));

  // The imported type is AUMP<NSError?>, but the typealias is AUMP<NSError?>?
  // so we have to manually make them match. We also want to assume this in
  // general for error out-parameters even if they weren't marked nullable in C.
  // Or at least we do for source-compatibility reasons...
  return OptionalType::get(importedType);
}

static Type maybeImportCFOutParameter(ClangImporter::Implementation &impl,
                                      Type importedType,
                                      ImportTypeKind importKind) {
  PointerTypeKind PTK;
  auto elementType = importedType->getAnyPointerElementType(PTK);
  if (!elementType || PTK != PTK_UnsafeMutablePointer)
    return Type();

  auto insideOptionalType = elementType->getOptionalObjectType();
  bool isOptional = (bool) insideOptionalType;
  if (!insideOptionalType)
    insideOptionalType = elementType;

  auto boundGenericTy = insideOptionalType->getAs<BoundGenericType>();
  if (!boundGenericTy)
    return Type();

  if (!boundGenericTy->isUnmanaged())
    return Type();

  assert(boundGenericTy->getGenericArgs().size() == 1 &&
         "signature of Unmanaged has changed");

  auto resultTy = boundGenericTy->getGenericArgs().front();
  if (isOptional)
    resultTy = OptionalType::get(resultTy);

  PointerTypeKind pointerKind;
  if (importKind == ImportTypeKind::CFRetainedOutParameter)
    pointerKind = PTK_UnsafeMutablePointer;
  else
    pointerKind = PTK_AutoreleasingUnsafeMutablePointer;

  resultTy = resultTy->wrapInPointer(pointerKind);
  return resultTy;
}

static ImportedType adjustTypeForConcreteImport(
    ClangImporter::Implementation &impl,
    ImportResult importResult, ImportTypeKind importKind,
    bool allowNSUIntegerAsInt, Bridgeability bridging,
    llvm::function_ref<void(Diagnostic &&)> addImportDiagnostic,
    ImportTypeAttrs attrs, OptionalTypeKind optKind,
    bool resugarNSErrorPointer,
    clang::Qualifiers::ObjCLifetime objCLifetime) {
  Type importedType = importResult.AbstractType;
  ImportHint hint = importResult.Hint;

  if (importKind == ImportTypeKind::Abstract) {
    return {importedType, false};
  }

  // If we completely failed to import the type, give up now.
  // Special-case for 'void' which is valid in result positions.
  if (!importedType && hint != ImportHint::Void)
    return {Type(), false};

  switch (hint) {
  case ImportHint::None:
    break;

  case ImportHint::ObjCPointer:
  case ImportHint::CFunctionPointer:
    break;

  case ImportHint::Void:
    // 'void' can only be imported as a function result type.
    if (importKind != ImportTypeKind::AuditedResult &&
        importKind != ImportTypeKind::Result) {
      return {Type(), false};
    }
    importedType = impl.getNamedSwiftType(impl.getStdlibModule(), "Void");
    break;

  case ImportHint::ObjCBridged:
    // Import NSString * globals as non-optional String.
    if (isNSString(importedType)) {
      if (importKind == ImportTypeKind::Variable ||
          importKind == ImportTypeKind::AuditedVariable) {
        importedType = hint.BridgedType;
        optKind = OTK_None;
        break;
      }
    }

    // If we have a bridged Objective-C type and we are allowed to
    // bridge, do so.
    if (canBridgeTypes(importKind) &&
        importKind != ImportTypeKind::PropertyWithReferenceSemantics &&
        !(importKind == ImportTypeKind::RecordField &&
          objCLifetime <= clang::Qualifiers::OCL_ExplicitNone) &&
        !(importKind == ImportTypeKind::Typedef &&
          bridging == Bridgeability::None)) {
      // id and Any can be bridged without Foundation. There would be
      // bootstrapping issues with the ObjectiveC module otherwise.
      if (hint.BridgedType->isAny()
          || impl.canImportFoundationModule()
          || impl.ImportForwardDeclarations) {

        // Set the bridged type if it wasn't done already.
        if (!importedType->isEqual(hint.BridgedType))
          importedType = hint.BridgedType;
      }
    }
    break;

  case ImportHint::Block: {
    // SwiftTypeConverter turns block pointers into @convention(block) types.
    // In some contexts, we bridge them to use the Swift function type
    // representation. This includes typedefs of block types, which use the
    // Swift function type representation.
    // FIXME: Do not bridge on RecordFields to keep previous behaviour for
    // the time being.
    if (!canBridgeTypes(importKind) ||
        importKind == ImportTypeKind::RecordField)
      break;

    // Determine the function type representation we need.
    //
    // For Objective-C collection arguments, we cannot bridge from a block
    // to a Swift function type, so force the block representation. Normally
    // the mapped type will have a block representation (making this a no-op),
    // but in cases where the Clang type was written as a typedef of a
    // block type, that typedef will have a Swift function type
    // representation. This code will then break down the imported type
    // alias and produce a function type with block representation.
    auto requiredFunctionTypeRepr = FunctionTypeRepresentation::Swift;
    if (importKind == ImportTypeKind::ObjCCollectionElement) {
      requiredFunctionTypeRepr = FunctionTypeRepresentation::Block;
    }

    auto fTy = importedType->castTo<FunctionType>();
    FunctionType::ExtInfo einfo = fTy->getExtInfo();
    if (einfo.getRepresentation() != requiredFunctionTypeRepr) {
      const clang::Type *clangType = nullptr;
      if (shouldStoreClangType(requiredFunctionTypeRepr))
        clangType = fTy->getASTContext().getClangFunctionType(
            fTy->getParams(), fTy->getResult(), requiredFunctionTypeRepr);
      einfo = einfo.intoBuilder()
                  .withRepresentation(requiredFunctionTypeRepr)
                  .withClangFunctionType(clangType)
                  .build();
      importedType = fTy->withExtInfo(einfo);
    }
    break;
  }

  case ImportHint::Boolean:
    // Turn BOOL and DarwinBoolean into Bool in contexts that can bridge types
    // losslessly.
    if (bridging == Bridgeability::Full && canBridgeTypes(importKind))
      importedType = impl.SwiftContext.getBoolType();
    break;

  case ImportHint::NSUInteger:
    // When NSUInteger is used as an enum's underlying type or if it does not
    // come from a system module, make sure it stays unsigned.
    if (importKind == ImportTypeKind::Enum || !allowNSUIntegerAsInt)
      importedType = impl.SwiftContext.getUIntType();
    break;

  case ImportHint::CFPointer:
    // Wrap CF pointers up as unmanaged types, unless this is an audited
    // context.
    if (!isCFAudited(importKind)) {
      Type underlyingType = importedType->getSwiftNewtypeUnderlyingType();
      if (!underlyingType)
        underlyingType = importedType;
      importedType = getUnmanagedType(impl, underlyingType);
    }
    break;

  case ImportHint::VAList:
    // Treat va_list specially: null-unspecified va_list parameters should be
    // assumed to be non-optional. (Most people don't even think of va_list as a
    // pointer, and it's not a portable assumption anyway.)
    if (importKind == ImportTypeKind::Parameter &&
        optKind == OTK_ImplicitlyUnwrappedOptional) {
      optKind = OTK_None;
    }
    break;

  case ImportHint::OtherPointer:
    // Special-case AutoreleasingUnsafeMutablePointer<NSError?> parameters.
    if (importKind == ImportTypeKind::Parameter) {
      if (Type result = maybeImportNSErrorOutParameter(impl, importedType,
                                                       resugarNSErrorPointer)) {
        importedType = result;
        optKind = OTK_None;
        break;
      }
    }

    // Remove 'Unmanaged' from audited CF out-parameters.
    if (importKind == ImportTypeKind::CFRetainedOutParameter ||
        importKind == ImportTypeKind::CFUnretainedOutParameter) {
      if (Type outParamTy = maybeImportCFOutParameter(impl, importedType,
                                                      importKind)) {
        importedType = outParamTy;
        break;
      }
    }

    break;
  }

  assert(importedType);

  if (importKind == ImportTypeKind::RecordField &&
      !importedType->isForeignReferenceType()) {
    switch (objCLifetime) {
      // Wrap retainable struct fields in Unmanaged.
      case clang::Qualifiers::OCL_None:
      case clang::Qualifiers::OCL_ExplicitNone:
        // FIXME: This should apply to blocks as well, but Unmanaged is constrained
        // to AnyObject.
        if (importedType->isAnyClassReferenceType()) {
          importedType = getUnmanagedType(impl, importedType);
        }
        break;
      // FIXME: Eventually we might get C++-like support for strong pointers in
      // structs, at which point we should really be checking the lifetime
      // qualifiers.
      case clang::Qualifiers::OCL_Strong:
        if (!impl.SwiftContext.LangOpts.EnableCXXInterop) {
          return {Type(), false};
        }
        break;
      case clang::Qualifiers::OCL_Weak:
        return {Type(), false};
      case clang::Qualifiers::OCL_Autoreleasing:
        llvm_unreachable("invalid Objective-C lifetime");
    }
  }

  // Apply attrs.
  importedType =
      impl.applyImportTypeAttrs(attrs, importedType, addImportDiagnostic);

  // Wrap class, class protocol, function, and metatype types in an
  // optional type.
  bool isIUO = false;
  if (importKind != ImportTypeKind::Typedef && optKind != OTK_None &&
      canImportAsOptional(hint)) {
    isIUO = optKind == OTK_ImplicitlyUnwrappedOptional;
    importedType = OptionalType::get(importedType);
  }

  return {importedType, isIUO};
}

ImportedType ClangImporter::Implementation::importType(
    clang::QualType type, ImportTypeKind importKind,
    llvm::function_ref<void(Diagnostic &&)> addImportDiagnosticFn,
    bool allowNSUIntegerAsInt, Bridgeability bridging,
    ImportTypeAttrs attrs, OptionalTypeKind optionality,
    bool resugarNSErrorPointer,
    Optional<unsigned> completionHandlerErrorParamIndex) {
  if (type.isNull())
    return {Type(), false};

  // The "built-in" Objective-C types id, Class, and SEL can actually be (and
  // are) defined within the library. Clang tracks the redefinition types
  // separately, so it can provide fallbacks in certain cases. For Swift, we
  // map the redefinition types back to the equivalent of the built-in types.
  // This bans some trickery that the redefinition types enable, but is a more
  // sane model overall.
  auto &clangContext = getClangASTContext();
  if (clangContext.getLangOpts().ObjC) {
    if (clangContext.hasSameUnqualifiedType(
          type, clangContext.getObjCIdRedefinitionType()) &&
        !clangContext.hasSameUnqualifiedType(
           clangContext.getObjCIdType(),
           clangContext.getObjCIdRedefinitionType()))
      type = clangContext.getObjCIdType();
    else if (clangContext.hasSameUnqualifiedType(
                type, clangContext.getObjCClassRedefinitionType()) &&
             !clangContext.hasSameUnqualifiedType(
                clangContext.getObjCClassType(),
                clangContext.getObjCClassRedefinitionType()))
      type = clangContext.getObjCClassType();
    else if (clangContext.hasSameUnqualifiedType(
               type, clangContext.getObjCSelRedefinitionType()) &&
             !clangContext.hasSameUnqualifiedType(
                clangContext.getObjCSelType(),
                clangContext.getObjCSelRedefinitionType()))
      type = clangContext.getObjCSelType();
  }

  // If nullability is provided as part of the type, that overrides
  // optionality provided externally.
  if (auto nullability = type->getNullability(clangContext)) {
    bool stripNonResultOptionality =
        importKind == ImportTypeKind::CompletionHandlerResultParameter;
    
    optionality = translateNullability(*nullability, stripNonResultOptionality);
  }

  // If this is a completion handler parameter, record the function type whose
  // parameters will act as the results of the completion handler.
  const clang::FunctionType *completionHandlerType = nullptr;
  if (completionHandlerErrorParamIndex) {
    if (auto blockPtrType = type->getAs<clang::BlockPointerType>()) {
      completionHandlerType =
          blockPtrType->getPointeeType()->castAs<clang::FunctionType>();

      type = clang::QualType(blockPtrType, 0);
    }
  }

  clang::Qualifiers::ObjCLifetime objCLifetime = type.getObjCLifetime();

  // Perform abstract conversion, ignoring how the type is actually used.
  SwiftTypeConverter converter(
      *this, addImportDiagnosticFn, allowNSUIntegerAsInt, bridging,
      completionHandlerType, completionHandlerErrorParamIndex);
  auto importResult = converter.Visit(type);

  // Now fix up the type based on how we're concretely using it.
  auto adjustedType = adjustTypeForConcreteImport(
      *this, importResult, importKind, allowNSUIntegerAsInt, bridging,
      addImportDiagnosticFn, attrs, optionality, resugarNSErrorPointer,
      objCLifetime);

  return adjustedType;
}

Type ClangImporter::Implementation::importTypeIgnoreIUO(
    clang::QualType type, ImportTypeKind importKind,
    llvm::function_ref<void(Diagnostic &&)> addImportDiagnosticFn,
    bool allowNSUIntegerAsInt, Bridgeability bridging,
    ImportTypeAttrs attrs, OptionalTypeKind optionality,
    bool resugarNSErrorPointer) {

  auto importedType = importType(type, importKind, addImportDiagnosticFn,
                                 allowNSUIntegerAsInt, bridging, attrs,
                                 optionality, resugarNSErrorPointer);

  return importedType.getType();
}

bool ClangImporter::Implementation::shouldImportGlobalAsLet(
       clang::QualType type)
{
  // Const variables should be imported as 'let'.
  if (type.isConstQualified()) {
    return true;
  }
  // Globals of type NSString * should be imported as 'let'.
  if (isNSString(type))
    return true;

  return false;
}

/// Returns true if \p name contains the substring "Unsigned" or "unsigned".
static bool nameContainsUnsigned(StringRef name) {
  size_t pos = name.find("nsigned");
  if (pos == StringRef::npos || pos == 0)
    return false;
  --pos;
  return (name[pos] == 'u') || (name[pos] == 'U');
}

bool ClangImporter::Implementation::shouldAllowNSUIntegerAsInt(
    bool isFromSystemModule, const clang::NamedDecl *decl) {
  if (isFromSystemModule)
    if (auto identInfo = decl->getIdentifier())
      return !nameContainsUnsigned(identInfo->getName());
  return false;
}

ImportedType ClangImporter::Implementation::importPropertyType(
    const clang::ObjCPropertyDecl *decl, bool isFromSystemModule) {
  const auto assignOrUnsafeUnretained =
      clang::ObjCPropertyAttribute::kind_assign |
      clang::ObjCPropertyAttribute::kind_unsafe_unretained;

  ImportTypeKind importKind;
  // HACK: Certain decls are always imported using bridged types,
  // because that's what a standalone method would do.
  if (shouldImportPropertyAsAccessors(decl)) {
    importKind = ImportTypeKind::Property;
  } else {
    switch (decl->getSetterKind()) {
    case clang::ObjCPropertyDecl::Assign:
      // If it's readonly, this might just be returned as a default.
      if (decl->isReadOnly() &&
          (decl->getPropertyAttributes() & assignOrUnsafeUnretained) == 0) {
        importKind = ImportTypeKind::Property;
      } else {
        importKind = ImportTypeKind::PropertyWithReferenceSemantics;
      }
      break;
    case clang::ObjCPropertyDecl::Retain:
    case clang::ObjCPropertyDecl::Copy:
      importKind = ImportTypeKind::Property;
      break;
    case clang::ObjCPropertyDecl::Weak:
      importKind = ImportTypeKind::PropertyWithReferenceSemantics;
      break;
    }
  }

  OptionalTypeKind optionality = OTK_ImplicitlyUnwrappedOptional;
  return importType(decl->getType(), importKind,
                    ImportDiagnosticAdder(*this, decl, decl->getLocation()),
                    shouldAllowNSUIntegerAsInt(isFromSystemModule, decl),
                    Bridgeability::Full, getImportTypeAttrs(decl),
                    optionality);
}

namespace {

class GetSendableType :
    private TypeVisitor<GetSendableType, std::pair<Type, bool>> {
  ASTContext &ctx;

public:
  GetSendableType(ASTContext &ctx) : ctx(ctx) {}

  /// The result of a conversion. Contains the converted type and a \c bool that
  /// is \c true if the operation found something to change, or \c false
  /// otherwise.
  using Result = std::pair<Type, /*found=*/bool>;

  /// Returns a modified version of \p type that has been made explicitly
  /// \c Sendable by adding an \c \@Sendable attribute to a function type
  /// or forming a protocol composition with \c & \c Sendable.
  Result convert(Type type) { return visit(type); }

private:
  /// Decide how to represent the given type in a protocol composition. This
  /// is specialized for \c ProtocolCompositionType to avoid nesting
  /// compositions.
  ///
  /// \param members The types to include in the composition.
  /// \return \c true if the composition should include \c AnyObject, \c false
  ///         otherwise.
  bool getAsComposition(ProtocolCompositionType *ty,
                        SmallVectorImpl<Type> &members) {
    llvm::append_range(members, ty->getMembers());
    return ty->hasExplicitAnyObject();
  }

  /// Decide how to represent the given type in a protocol composition. This
  /// is specialized for \c ProtocolCompositionType to avoid nesting
  /// compositions.
  ///
  /// \param members The types to include in the composition.
  /// \return \c true if the composition should include \c AnyObject, \c false
  ///         otherwise.
  bool getAsComposition(TypeBase *ty, SmallVectorImpl<Type> &members) {
    members.push_back(ty);
    return false;
  }

  // MARK: Visitor Actions

  /// Visitor action: Replace this type with a protocol composition that
  /// includes \c Sendable.
  template <typename Ty> Result compose(Ty *ty) {
    SmallVector<Type, 8> members;
    bool explicitAnyObject = getAsComposition(ty, members);

    auto proto = ctx.getProtocol(KnownProtocolKind::Sendable);
    members.push_back(proto->getDeclaredInterfaceType());

    return {
      ProtocolCompositionType::get(ctx, members, explicitAnyObject), true };
  }

  /// Visitor action: Recurse into the children of this type and try to add
  /// \c Sendable to them.
  Result recurse(Type ty) {
    bool anyFound = false;

    Type newTy = ty.transformRec([&](TypeBase *childTy) -> Optional<Type> {
      // We want to visit the first level of children.
      if (childTy == ty.getPointer())
        return None;

      auto result = this->visit(childTy);
      anyFound |= result.second;
      return result.first;
    });

    return { newTy, anyFound };
  }

  /// Visitor action: Ignore this type; do not modify it and do not recurse into
  /// it to find other types to modify.
  Result pass(Type ty, bool found = false) {
    return { ty, found };
  }

  // Macros to define visitors based on these actions.
#define VISIT(CLASS, ACT)  Result visit##CLASS(CLASS *ty) { return ACT(ty); }
#define NEVER_VISIT(CLASS) Result visit##CLASS(CLASS *ty) { \
    llvm_unreachable("can't have " #CLASS " in imported clang type"); \
    return pass(ty); \
  }

  // MARK: Visitors

  friend TypeVisitor<GetSendableType, Result>;

  Result visitErrorType(ErrorType *ty) {
    // Pass, but suppress diagnostic about not finding anything `Sendable`.
    return pass(ty, /*found=*/true);
  }

  NEVER_VISIT(UnresolvedType)
  NEVER_VISIT(PlaceholderType)
  NEVER_VISIT(BuiltinType)
  NEVER_VISIT(BuiltinTupleType)

  VISIT(TupleType, recurse)

  NEVER_VISIT(ReferenceStorageType)

  VISIT(EnumType, pass)
  VISIT(StructType, pass)
  VISIT(ClassType, compose)
  VISIT(ProtocolType, compose)

  Result visitBoundGenericType(BoundGenericType *ty) {
    assert(!isa<BoundGenericClassType>(ty) && "classes handled elsewhere");

    // These types are produced during bridging and have conditional
    // conformances to Sendable depending on their generic parameters, so we
    // want to make their generic parameters `Sendable`.
    // FIXME: `Unmanaged` ought to be here too, but `AnyObject & Sendable`
    //        doesn't satisfy the generic parameter's `AnyObject` requirement.
    //        (rdar://90946615)
    if (ty->isOptional() || ty->isArray() || ty->isSet() ||
        ty->isDictionary())
      return recurse(ty);

    // Other non-class generic types (e.g. pointers) cannot be made Sendable.
    return pass(ty);
  }

  VISIT(BoundGenericClassType, compose)
  NEVER_VISIT(UnboundGenericType)

  VISIT(AnyMetatypeType, recurse)

  VISIT(ModuleType, pass)
  VISIT(DynamicSelfType, pass)

  NEVER_VISIT(SubstitutableType)
  NEVER_VISIT(DependentMemberType)

  Result visitAnyFunctionType(AnyFunctionType *ty) {
    auto newFn = applyToFunctionType(ty, [](ASTExtInfo extInfo) {
      return extInfo.withConcurrent();
    });
    return { newFn, true };
  }

  NEVER_VISIT(SILFunctionType)
  NEVER_VISIT(SILBlockStorageType)
  NEVER_VISIT(SILBoxType)
  NEVER_VISIT(SILPackType)
  NEVER_VISIT(SILTokenType)
  NEVER_VISIT(SILMoveOnlyWrappedType)

  VISIT(ProtocolCompositionType, compose)

  // ProtocolCompositionType doesn't handle ParameterizedProtocolType
  // correctly, but we currently never import anything with it, so forbid it
  // until we find we need it.
  NEVER_VISIT(ParameterizedProtocolType)

  VISIT(ExistentialType, recurse)
  NEVER_VISIT(LValueType)
  VISIT(InOutType, recurse)

  NEVER_VISIT(PackType)
  NEVER_VISIT(PackExpansionType)
  NEVER_VISIT(PackElementType)
  NEVER_VISIT(TypeVariableType)

  VISIT(SugarType, recurse)

  Result visitTypeAliasType(TypeAliasType *ty) {
    // Try converting the underlying type.
    Type underlying = ty->getSinglyDesugaredType();
    auto result = visit(underlying);

    // If nothing that could be made Sendable was found in the underlying type,
    // keep the sugar.
    if (!result.second)
      return pass(ty);

    // If something Sendable-capable *was* found but the operation was a no-op,
    // keep the sugar but indicate that we did find something to avoid a
    // diagnostic.
    if (result.first->getCanonicalType() == underlying->getCanonicalType())
      return pass(ty, /*found=*/true);

    // We found something and it did change the type. Desugar to the converted
    // underlying type.
    return result;
  }
};

} // anonymous namespace

ImportTypeAttrs swift::getImportTypeAttrs(const clang::Decl *D, bool isParam,
                                          bool sendableByDefault) {
  ImportTypeAttrs attrs;

  if (sendableByDefault)
    attrs |= ImportTypeAttr::DefaultsToSendable;

  bool sendableRequested = sendableByDefault;
  bool sendableDisqualified = false;

  if (D->hasAttrs()) {
    for (auto attr : D->getAttrs()) {
      // Map __attribute__((noescape)) to @noescape.
      if (isParam && isa<clang::NoEscapeAttr>(attr)) {
        attrs |= ImportTypeAttr::NoEscape;
        continue;
      }

      auto swiftAttr = dyn_cast<clang::SwiftAttrAttr>(attr);
      if (!swiftAttr)
        continue;

      // Map the main-actor attribute.
      if (isParam && isMainActorAttr(swiftAttr)) {
        attrs |= ImportTypeAttr::MainActor;
        sendableDisqualified = true;
        continue;
      }

      // Map @Sendable.
      if (swiftAttr->getAttribute() == "@Sendable") {
        sendableRequested = true;
        continue;
      }

      // Map @_nonSendable.
      if (swiftAttr->getAttribute() == "@_nonSendable") {
        sendableDisqualified = true;
        continue;
      }
    }
  }

  if (!sendableDisqualified && sendableRequested) {
    attrs |= ImportTypeAttr::Sendable;
  }

  return attrs;
}

Type ClangImporter::Implementation::
applyImportTypeAttrs(ImportTypeAttrs attrs, Type type,
                     llvm::function_ref<void(Diagnostic &&)> addDiag) {
  if (attrs.contains(ImportTypeAttr::NoEscape)) {
    type = applyToFunctionType(type, [](ASTExtInfo extInfo) {
      return extInfo.withNoEscape();
    });
  }

  if (attrs.contains(ImportTypeAttr::MainActor)) {
    if (Type mainActor = SwiftContext.getMainActorType()) {
      type = applyToFunctionType(type, [&](ASTExtInfo extInfo) {
        return extInfo.withGlobalActor(mainActor);
      });
    } else {
      // If we can't use @MainActor, fall back to at least using @Sendable.
      attrs |= ImportTypeAttr::Sendable;
    }
  }

  if (attrs.contains(ImportTypeAttr::Sendable)) {
    bool changed;
    std::tie(type, changed) = GetSendableType(SwiftContext).convert(type);

    // Diagnose if we couldn't find a place to add `Sendable` to the type.
    if (!changed) {
      addDiag(Diagnostic(diag::clang_ignored_sendable_attr, type));

      if (attrs.contains(ImportTypeAttr::DefaultsToSendable))
        addDiag(Diagnostic(diag::clang_param_should_be_implicitly_sendable));
    }
  }

  return type;
}

ImportedType ClangImporter::Implementation::importFunctionReturnType(
    const clang::FunctionDecl *clangDecl, DeclContext *dc) {
  bool isInSystemModule =
      cast<ClangModuleUnit>(dc->getModuleScopeContext())->isSystemModule();
  bool allowNSUIntegerAsInt =
      shouldAllowNSUIntegerAsInt(isInSystemModule, clangDecl);
  return importFunctionReturnType(dc, clangDecl, allowNSUIntegerAsInt);
}

ImportedType ClangImporter::Implementation::importFunctionReturnType(
    DeclContext *dc, const clang::FunctionDecl *clangDecl,
    bool allowNSUIntegerAsInt) {

  // Hardcode handling of certain result types for builtins.
  if (auto builtinID = clangDecl->getBuiltinID()) {
    switch (getClangASTContext().BuiltinInfo.getTypeString(builtinID)[0]) {
    case 'z': // size_t
    case 'Y': // ptrdiff_t
      return {SwiftContext.getIntType(), false};
    default:
      break;
    }
  }

  // CF function results can be managed if they are audited or
  // the ownership convention is explicitly declared.
  assert(clangDecl && "expected to have a decl to import");
  bool isAuditedResult =
    (clangDecl->hasAttr<clang::CFAuditedTransferAttr>() ||
     clangDecl->hasAttr<clang::CFReturnsRetainedAttr>() ||
     clangDecl->hasAttr<clang::CFReturnsNotRetainedAttr>());

  // C++ operators +=, -=, *=, /= may return a reference to self. This is not
  // idiomatic in Swift, let's drop these return values.
  clang::OverloadedOperatorKind op = clangDecl->getOverloadedOperator();
  if (op == clang::OverloadedOperatorKind::OO_PlusEqual ||
      op == clang::OverloadedOperatorKind::OO_MinusEqual ||
      op == clang::OverloadedOperatorKind::OO_StarEqual ||
      op == clang::OverloadedOperatorKind::OO_SlashEqual)
    return {SwiftContext.getVoidType(), false};

  // Fix up optionality.
  OptionalTypeKind OptionalityOfReturn;
  if (clangDecl->hasAttr<clang::ReturnsNonNullAttr>()) {
    OptionalityOfReturn = OTK_None;
  } else {
    OptionalityOfReturn = OTK_ImplicitlyUnwrappedOptional;
  }

  clang::QualType returnType = clangDecl->getReturnType();
  if (auto elaborated =
          dyn_cast<clang::ElaboratedType>(returnType))
    returnType = elaborated->desugar();

  // Specialized templates need to match the args/result exactly (i.e.,
  // ptr -> ptr not ptr -> Optional<ptr>).
  if (returnType->isPointerType() && clangDecl->getPrimaryTemplate() &&
      clangDecl
          ->getPrimaryTemplate()
          ->getAsFunction()
          ->getReturnType()
          ->isTemplateTypeParmType())
    OptionalityOfReturn = OTK_None;

  if (auto typedefType = dyn_cast<clang::TypedefType>(returnType)) {
    if (isUnavailableInSwift(typedefType->getDecl())) {
      if (auto clangEnum = findAnonymousEnumForTypedef(SwiftContext, typedefType)) {
        // If this fails, it means that we need a stronger predicate for
        // determining the relationship between an enum and typedef.
        assert(clangEnum.value()->getIntegerType()->getCanonicalTypeInternal() ==
               typedefType->getCanonicalTypeInternal());
        if (auto swiftEnum = importDecl(*clangEnum, CurrentVersion)) {
          return {cast<TypeDecl>(swiftEnum)->getDeclaredInterfaceType(), false};
        }
      }
    }
  }

  // Import the underlying result type.
  if (clangDecl) {
    if (auto recordType = returnType->getAsCXXRecordDecl()) {
      if (auto *vd = evaluateOrDefault(
              SwiftContext.evaluator,
              CxxRecordAsSwiftType({recordType, SwiftContext}), nullptr)) {
        if (auto *cd = dyn_cast<ClassDecl>(vd)) {
          Type t = ClassType::get(cd, Type(), SwiftContext);
          return ImportedType(t, /*implicitlyUnwraps=*/false);
        }
      }
    }
  }

  // Import the result type.
  return importType(returnType,
                    (isAuditedResult ? ImportTypeKind::AuditedResult
                                     : ImportTypeKind::Result),
                    ImportDiagnosticAdder(*this, clangDecl,
                                          clangDecl->getLocation()),
                    allowNSUIntegerAsInt, Bridgeability::Full,
                    getImportTypeAttrs(clangDecl), OptionalityOfReturn);
}

static Type
findGenericTypeInGenericDecls(ClangImporter::Implementation &impl,
                              const clang::TemplateTypeParmType *templateParam,
                              ArrayRef<GenericTypeParamDecl *> genericParams,
                              ImportTypeAttrs attrs,
                              llvm::function_ref<void(Diagnostic &&)> addDiag) {
  StringRef name = templateParam->getIdentifier()->getName();
  auto genericParamIter =
      llvm::find_if(genericParams, [name](GenericTypeParamDecl *generic) {
        return generic->getName().str() == name;
      });
  // TODO: once we support generics in class types, replace this with
  // "return nullptr". Once support for template classes, this will need to
  // be updated, though. I'm leaving the assert here to make it easier to
  // find.
  assert(genericParamIter != genericParams.end() &&
         "Could not find generic param type in generic params.");
  auto *genericParamDecl = *genericParamIter;
  auto metatype =
      cast<MetatypeType>(genericParamDecl->getInterfaceType().getPointer());
  return impl.applyImportTypeAttrs(attrs, metatype->getMetatypeInstanceType(),
                                   addDiag);
}

ImportedType ClangImporter::Implementation::importFunctionParamsAndReturnType(
    DeclContext *dc, const clang::FunctionDecl *clangDecl,
    ArrayRef<const clang::ParmVarDecl *> params, bool isVariadic,
    bool isFromSystemModule, DeclName name, ParameterList *&parameterList,
    ArrayRef<GenericTypeParamDecl *> genericParams) {

  bool allowNSUIntegerAsInt =
      shouldAllowNSUIntegerAsInt(isFromSystemModule, clangDecl);

  // Only eagerly import the return type if it's not too expensive (the current
  // heuristic for that is if it's not a record type).
  ImportedType importedType;
  ImportDiagnosticAdder addDiag(*this, clangDecl,
                                clangDecl->getSourceRange().getBegin());
  clang::QualType returnType = clangDecl->getReturnType();
  if (auto elaborated = dyn_cast<clang::ElaboratedType>(returnType))
    returnType = elaborated->desugar();

  if (auto typedefType = dyn_cast<clang::TypedefType>(returnType)) {
    if (isUnavailableInSwift(typedefType->getDecl())) {
      if (auto clangEnum = findAnonymousEnumForTypedef(SwiftContext, typedefType)) {
        // If this fails, it means that we need a stronger predicate for
        // determining the relationship between an enum and typedef.
        assert(clangEnum.value()->getIntegerType()->getCanonicalTypeInternal() ==
               typedefType->getCanonicalTypeInternal());
        if (auto swiftEnum = importDecl(*clangEnum, CurrentVersion)) {
          importedType = {cast<TypeDecl>(swiftEnum)->getDeclaredInterfaceType(),
                          false};
        }
      }
    }
  }

  if (auto templateType =
          dyn_cast<clang::TemplateTypeParmType>(returnType)) {
    importedType = {findGenericTypeInGenericDecls(
                        *this, templateType, genericParams,
                        getImportTypeAttrs(clangDecl), addDiag),
                    false};
  } else if ((isa<clang::PointerType>(returnType) ||
          isa<clang::ReferenceType>(returnType)) &&
         isa<clang::TemplateTypeParmType>(returnType->getPointeeType())) {
    auto pointeeType = returnType->getPointeeType();
    auto templateParamType = cast<clang::TemplateTypeParmType>(pointeeType);
    PointerTypeKind pointerKind = pointeeType.getQualifiers().hasConst()
                                      ? PTK_UnsafePointer
                                      : PTK_UnsafeMutablePointer;
    auto genericType =
        findGenericTypeInGenericDecls(*this, templateParamType, genericParams,
                                      getImportTypeAttrs(clangDecl), addDiag);
    importedType = {genericType->wrapInPointer(pointerKind), false};
  } else if (!(isa<clang::RecordType>(returnType) ||
               isa<clang::TemplateSpecializationType>(returnType)) ||
             // TODO: we currently don't lazily load operator return types, but
             // this should be trivial to add.
             clangDecl->isOverloadedOperator() ||
             // Dependant types are trivially mapped as Any.
             returnType->isDependentType()) {
    // If importedType is already initialized, it means we found the enum that
    // was supposed to be used (instead of the typedef type).
    if (!importedType) {
      importedType =
          importFunctionReturnType(dc, clangDecl, allowNSUIntegerAsInt);
      if (!importedType) {
        addDiag(Diagnostic(diag::return_type_not_imported));
        return {Type(), false};
      }
    }
  }

  Type swiftResultTy = importedType.getType();
  ArrayRef<Identifier> argNames = name.getArgumentNames();
  parameterList = importFunctionParameterList(dc, clangDecl, params, isVariadic,
                                              allowNSUIntegerAsInt, argNames,
                                              genericParams, swiftResultTy);
  if (!parameterList)
    return {Type(), false};

  if (clangDecl->isNoReturn())
    swiftResultTy = SwiftContext.getNeverType();

  return {swiftResultTy, importedType.isImplicitlyUnwrapped()};
}

static ImportTypeKind
getImportTypeKindForParam(const clang::ParmVarDecl *param) {
  ImportTypeKind importKind = ImportTypeKind::Parameter;
  if (param->hasAttr<clang::CFReturnsRetainedAttr>())
    importKind = ImportTypeKind::CFRetainedOutParameter;
  else if (param->hasAttr<clang::CFReturnsNotRetainedAttr>())
    importKind = ImportTypeKind::CFUnretainedOutParameter;

  return importKind;
}

Optional<ClangImporter::Implementation::ImportParameterTypeResult>
ClangImporter::Implementation::importParameterType(
    const clang::ParmVarDecl *param, OptionalTypeKind optionalityOfParam,
    bool allowNSUIntegerAsInt, bool isNSDictionarySubscriptGetter,
    bool paramIsError, bool paramIsCompletionHandler,
    Optional<unsigned> completionHandlerErrorParamIndex,
    ArrayRef<GenericTypeParamDecl *> genericParams,
    llvm::function_ref<void(Diagnostic &&)> addImportDiagnosticFn) {
  auto paramTy = param->getType();
  if (auto elaborated = dyn_cast<clang::ElaboratedType>(paramTy))
    paramTy = elaborated->desugar();

  ImportTypeKind importKind = getImportTypeKindForParam(param);

  // Import the parameter type into Swift.
  auto attrs = getImportTypeAttrs(param, /*isParam=*/true);
  Type swiftParamTy;
  bool isInOut = false;
  bool isParamTypeImplicitlyUnwrapped = false;

  // Sometimes we import unavailable typedefs as enums. If that's the case,
  // use the enum, not the typedef here.
  if (auto typedefType = dyn_cast<clang::TypedefType>(paramTy.getTypePtr())) {
    if (isUnavailableInSwift(typedefType->getDecl())) {
      if (auto clangEnum =
              findAnonymousEnumForTypedef(SwiftContext, typedefType)) {
        // If this fails, it means that we need a stronger predicate for
        // determining the relationship between an enum and typedef.
        assert(clangEnum.value()
                   ->getIntegerType()
                   ->getCanonicalTypeInternal() ==
               typedefType->getCanonicalTypeInternal());
        if (auto swiftEnum = importDecl(*clangEnum, CurrentVersion)) {
          swiftParamTy = cast<TypeDecl>(swiftEnum)->getDeclaredInterfaceType();
        }
      }
    }
  } else if (isa<clang::PointerType>(paramTy) &&
             isa<clang::TemplateTypeParmType>(paramTy->getPointeeType())) {
    auto pointeeType = paramTy->getPointeeType();
    auto templateParamType = cast<clang::TemplateTypeParmType>(pointeeType);
    PointerTypeKind pointerKind = pointeeType.getQualifiers().hasConst()
                                      ? PTK_UnsafePointer
                                      : PTK_UnsafeMutablePointer;
    auto genericType = findGenericTypeInGenericDecls(
        *this, templateParamType, genericParams, attrs, addImportDiagnosticFn);
    swiftParamTy = genericType->wrapInPointer(pointerKind);
    if (!swiftParamTy)
      return None;
  } else if (isa<clang::ReferenceType>(paramTy) &&
             isa<clang::TemplateTypeParmType>(paramTy->getPointeeType())) {
    // We don't support rvalue reference / universal perfect ref, bail.
    if (paramTy->isRValueReferenceType()) {
      addImportDiagnosticFn(Diagnostic(diag::rvalue_ref_params_not_imported));
      return None;
    }

    auto templateParamType =
        cast<clang::TemplateTypeParmType>(paramTy->getPointeeType());
    swiftParamTy = findGenericTypeInGenericDecls(
        *this, templateParamType, genericParams, attrs, addImportDiagnosticFn);
    if (!paramTy->getPointeeType().isConstQualified())
      isInOut = true;
  } else if (auto *templateParamType =
                 dyn_cast<clang::TemplateTypeParmType>(paramTy)) {
    swiftParamTy = findGenericTypeInGenericDecls(
        *this, templateParamType, genericParams, attrs, addImportDiagnosticFn);
  }

  if (!swiftParamTy) {
    // C++ reference types are brought in as direct
    // types most commonly.
    auto refPointeeType =
        importer::getCxxReferencePointeeTypeOrNone(paramTy.getTypePtr());
    if (refPointeeType) {
      // We don't support reference type to a dependent type, just bail.
      if ((*refPointeeType)->isDependentType()) {
        return None;
      }

      // We don't support rvalue reference types, just bail.
      if (paramTy->isRValueReferenceType()) {
        addImportDiagnosticFn(Diagnostic(diag::rvalue_ref_params_not_imported));
        return None;
      }

      // A C++ parameter of type `const <type> &` or `<type> &` becomes `<type>`
      // or `inout <type>` in Swift. Note that SILGen will use the indirect
      // parameter convention for such a type.
      paramTy = *refPointeeType;
      if (!paramTy.isConstQualified())
        isInOut = true;
    }
  }

  // Special case for NSDictionary's subscript.
  if (isNSDictionarySubscriptGetter && paramTy->isObjCIdType()) {
    // Not using `getImportTypeAttrs()` is unprincipled but OK for this hack.
    auto nsCopying = SwiftContext.getNSCopyingType();
    if (!nsCopying)
      return None;

    swiftParamTy = ExistentialType::get(nsCopying);
    if (optionalityOfParam != OTK_None)
      swiftParamTy = OptionalType::get(swiftParamTy);

    isParamTypeImplicitlyUnwrapped =
        optionalityOfParam == OTK_ImplicitlyUnwrappedOptional;
  }

  if (!swiftParamTy) {
    if (auto recordType = paramTy->getAsCXXRecordDecl()) {

      if (auto *vd = evaluateOrDefault(
              SwiftContext.evaluator,
              CxxRecordAsSwiftType({recordType, SwiftContext}), nullptr)) {

        if (auto *cd = dyn_cast<ClassDecl>(vd)) {

          swiftParamTy = ClassType::get(cd, Type(), SwiftContext);
        }
      }
    }
  }

  if (!swiftParamTy) {
    bool sendableByDefault =
        paramIsCompletionHandler &&
        SwiftContext.LangOpts.hasFeature(Feature::SendableCompletionHandlers);

    auto attrs = getImportTypeAttrs(param, /*isParam=*/true, sendableByDefault);

    // If this is the throws error parameter, we don't need to convert any
    // NSError** arguments to the sugared NSErrorPointer typealias form,
    // because all that is done with it is retrieving the canonical
    // type. Avoiding the sugar breaks a loop in Foundation caused by method
    // on NSString that has an error parameter. FIXME: This is a work-around
    // for the specific case when the throws conversion works, but is not
    // sufficient if it fails. (The correct, overarching fix is ClangImporter
    // being lazier.)
    auto importedType = importType(paramTy, importKind, addImportDiagnosticFn,
                                   allowNSUIntegerAsInt, Bridgeability::Full,
                                   attrs, optionalityOfParam,
                                   /*resugarNSErrorPointer=*/!paramIsError,
                                   completionHandlerErrorParamIndex);
    if (!importedType)
      return None;

    isParamTypeImplicitlyUnwrapped = importedType.isImplicitlyUnwrapped();
    swiftParamTy = importedType.getType();
  }

  return ImportParameterTypeResult{swiftParamTy, isInOut,
                                   isParamTypeImplicitlyUnwrapped};
}

static ParamDecl *getParameterInfo(ClangImporter::Implementation *impl,
                                   const clang::ParmVarDecl *param,
                                   const Identifier &name,
                                   const swift::Type &swiftParamTy,
                                   const bool isInOut,
                                   const bool isParamTypeImplicitlyUnwrapped) {
  // Figure out the name for this parameter.
  Identifier bodyName = impl->importFullName(param, impl->CurrentVersion)
                            .getDeclName()
                            .getBaseIdentifier();

  // It doesn't actually matter which DeclContext we use, so just use the
  // imported header unit.
  auto paramInfo = impl->createDeclWithClangNode<ParamDecl>(
      param, AccessLevel::Private, SourceLoc(), SourceLoc(), name,
      impl->importSourceLoc(param->getLocation()), bodyName,
      impl->ImportedHeaderUnit);
  // Foreign references are already references so they don't need to be passed
  // as inout.
  paramInfo->setSpecifier(isInOut && !swiftParamTy->isForeignReferenceType()
                              ? ParamSpecifier::InOut
                              : ParamSpecifier::Default);
  paramInfo->setInterfaceType(swiftParamTy);
  impl->recordImplicitUnwrapForDecl(paramInfo, isParamTypeImplicitlyUnwrapped);

  return paramInfo;
}

ParameterList *ClangImporter::Implementation::importFunctionParameterList(
    DeclContext *dc, const clang::FunctionDecl *clangDecl,
    ArrayRef<const clang::ParmVarDecl *> params, bool isVariadic,
    bool allowNSUIntegerAsInt, ArrayRef<Identifier> argNames,
    ArrayRef<GenericTypeParamDecl *> genericParams, Type resultType) {
  // Import the parameters.
  SmallVector<ParamDecl *, 4> parameters;
  unsigned index = 0;
  SmallBitVector nonNullArgs = getNonNullArgs(clangDecl, params);

  for (auto param : params) {
    auto paramTy = param->getType();
    if (paramTy->isVoidType()) {
      ++index;
      continue;
    }

    bool knownNonNull = !nonNullArgs.empty() && nonNullArgs[index];
    // Specialized templates need to match the args/result exactly.
    knownNonNull |= clangDecl->isFunctionTemplateSpecialization();

    // Check nullability of the parameter.
    OptionalTypeKind optionalityOfParam =
        getParamOptionality(param, knownNonNull);

    ImportDiagnosticAdder paramAddDiag(*this, clangDecl, param->getLocation());

    auto swiftParamTyOpt = importParameterType(
        param, optionalityOfParam, allowNSUIntegerAsInt,
        /*isNSDictionarySubscriptGetter=*/false,
        /*paramIsError=*/false,
        /*paramIsCompletionHandler=*/false,
        /*completionHandlerErrorParamIndex=*/None, genericParams, paramAddDiag);
    if (!swiftParamTyOpt) {
      addImportDiagnostic(param,
                          Diagnostic(diag::parameter_type_not_imported, param),
                          param->getSourceRange().getBegin());
      return nullptr;
    }
    auto swiftParamTy = swiftParamTyOpt->swiftTy;
    bool isInOut = swiftParamTyOpt->isInOut;
    bool isParamTypeImplicitlyUnwrapped =
        swiftParamTyOpt->isParamTypeImplicitlyUnwrapped;

    // Retrieve the argument name.
    Identifier name;
    if (index < argNames.size())
      name = argNames[index];

    auto paramInfo = getParameterInfo(this, param, name, swiftParamTy, isInOut,
                                      isParamTypeImplicitlyUnwrapped);
    parameters.push_back(paramInfo);
    ++index;
  }

  auto genericParamTypeUsedInSignature =
      [params, resultType](GenericTypeParamDecl *genericParam,
                           bool shouldCheckResultType) -> bool {
    auto paramDecl = genericParam->getClangDecl();
    auto templateTypeParam = cast<clang::TemplateTypeParmDecl>(paramDecl);
    // TODO(https://github.com/apple/swift/issues/56206): This won't work when we support importing dependent types.
    // We'll have to change this logic to traverse the type tree of the imported
    // Swift type (basically whatever ends up in the parameters variable).
    // Check if genericParam's corresponding clang template type is used by
    // the clang function's parameters.
    for (auto param : params) {
      if (hasSameUnderlyingType(param->getType().getTypePtr(),
                                templateTypeParam)) {
        return true;
      }
    }

    // Check if genericParam is used as a type parameter in the result type.
    return shouldCheckResultType &&
        resultType.findIf([genericParam](Type typePart) -> bool {
          return typePart->isEqual(genericParam->getDeclaredInterfaceType());
        });
  };
  
  // Make sure all generic parameters are accounted for in the function signature.
  for (auto genericParam : genericParams) {
    bool shouldCheckResultType = resultType && resultType->hasTypeParameter();
    if (genericParamTypeUsedInSignature(genericParam, shouldCheckResultType))
      continue;
    
    // If this generic parameter is not used in the function signature,
    // add a new parameter that accepts a metatype corresponding to that
    // generic parameter.
    Identifier name = genericParam->getName();
    auto param = new (SwiftContext)
        ParamDecl(SourceLoc(), SourceLoc(), name, SourceLoc(),
                  name, dc);
    auto metatype =
      cast<MetatypeType>(genericParam->getInterfaceType().getPointer());
    param->setInterfaceType(metatype);
    param->setSpecifier(ParamSpecifier::Default);
    parameters.push_back(param);
  }

  // Append an additional argument to represent varargs.
  if (isVariadic) {
    auto paramTy = VariadicSequenceType::get(
        SwiftContext.getAnyExistentialType());
    auto name = SwiftContext.getIdentifier("varargs");
    auto param = new (SwiftContext) ParamDecl(SourceLoc(), SourceLoc(),
                                              Identifier(), SourceLoc(),
                                              name,
                                              ImportedHeaderUnit);
    param->setSpecifier(ParamSpecifier::Default);
    param->setInterfaceType(paramTy);

    param->setVariadic();
    parameters.push_back(param);
  }

  // Form the parameter list.
  return ParameterList::create(SwiftContext, parameters);
}

static bool isObjCMethodResultAudited(const clang::Decl *decl) {
  if (!decl)
    return false;
  return (decl->hasAttr<clang::CFReturnsRetainedAttr>() ||
          decl->hasAttr<clang::CFReturnsNotRetainedAttr>() ||
          decl->hasAttr<clang::ObjCReturnsInnerPointerAttr>());
}

ArgumentAttrs ClangImporter::Implementation::inferDefaultArgument(
    clang::QualType type, OptionalTypeKind clangOptionality,
    DeclBaseName baseName, StringRef argumentLabel, bool isFirstParameter,
    bool isLastParameter, NameImporter &nameImporter) {
  auto baseNameStr = baseName.userFacingName();

  // Don't introduce a default argument for the first parameter of setters.
  if (isFirstParameter && camel_case::getFirstWord(baseNameStr) == "set")
    return DefaultArgumentKind::None;

  // Some nullable parameters default to 'nil'.
  if (clangOptionality == OTK_Optional) {
    // Nullable trailing closure parameters default to 'nil'.
    if (isLastParameter &&
        (type->isFunctionPointerType() || type->isBlockPointerType()))
      return DefaultArgumentKind::NilLiteral;

    // NSZone parameters default to 'nil'.
    if (auto ptrType = type->getAs<clang::PointerType>()) {
      if (auto recType
            = ptrType->getPointeeType()->getAs<clang::RecordType>()) {
        if (recType->isStructureOrClassType() &&
            recType->getDecl()->getName() == "_NSZone")
          return DefaultArgumentKind::NilLiteral;
      }
    }
  }

  // Option sets default to "[]" if they have "Options" in their name.
  if (const clang::EnumType *enumTy = type->getAs<clang::EnumType>()) {
    const clang::EnumDecl *enumDef = enumTy->getDecl()->getDefinition();
    if (enumDef && nameImporter.getEnumKind(enumDef) == EnumKind::Options) {
      auto enumName = enumDef->getName();
      for (auto word : llvm::reverse(camel_case::getWords(enumName))) {
        if (camel_case::sameWordIgnoreFirstCase(word, "options"))
          return DefaultArgumentKind::EmptyArray;
      }
    }
  } else if (const clang::TypedefType *typedefType =
                 type->getAs<clang::TypedefType>()) {
    // Get the AvailabilityAttr that would be set from CF/NS_OPTIONS
    if (importer::isUnavailableInSwift(typedefType->getDecl(), nullptr, true)) {
      // If we've taken this branch it means we have an enum type, and it is
      // likely an integer or NSInteger that is being used by NS/CF_OPTIONS to
      // behave like a C enum in the presence of C++.
      auto enumName = typedefType->getDecl()->getName();
      ArgumentAttrs argumentAttrs(DefaultArgumentKind::None, true, enumName);
      auto camelCaseWords = camel_case::getWords(enumName);
      for (auto it = camelCaseWords.rbegin(); it != camelCaseWords.rend();
           ++it) {
        auto word = *it;
        auto next = std::next(it);
        if (camel_case::sameWordIgnoreFirstCase(word, "options")) {
          argumentAttrs.argumentKind = DefaultArgumentKind::EmptyArray;
          return argumentAttrs;
        }
        if (camel_case::sameWordIgnoreFirstCase(word, "units"))
          return argumentAttrs;
        if (camel_case::sameWordIgnoreFirstCase(word, "domain"))
          return argumentAttrs;
        if (camel_case::sameWordIgnoreFirstCase(word, "action"))
          return argumentAttrs;
        if (camel_case::sameWordIgnoreFirstCase(word, "events") &&
            next != camelCaseWords.rend() &&
            camel_case::sameWordIgnoreFirstCase(*next, "control"))
          return argumentAttrs;
        if (camel_case::sameWordIgnoreFirstCase(word, "state"))
          return argumentAttrs;
        if (camel_case::sameWordIgnoreFirstCase(word, "unit"))
          return argumentAttrs;
        if (camel_case::sameWordIgnoreFirstCase(word, "position") &&
            next != camelCaseWords.rend() &&
            camel_case::sameWordIgnoreFirstCase(*next, "scroll"))
          return argumentAttrs;
        if (camel_case::sameWordIgnoreFirstCase(word, "edge"))
          return argumentAttrs;
      }
    }
  }

  // NSDictionary arguments default to [:] (or nil, if nullable) if "options",
  // "attributes", or "userInfo" occur in the argument label or (if there is no
  // argument label) at the end of the base name.
  if (auto objcPtrTy = type->getAs<clang::ObjCObjectPointerType>()) {
    if (auto objcClass = objcPtrTy->getInterfaceDecl()) {
      if (objcClass->getName() == "NSDictionary") {
        StringRef searchStr = argumentLabel;
        if (searchStr.empty() && !baseNameStr.empty())
          searchStr = baseNameStr;

        auto emptyDictionaryKind = DefaultArgumentKind::EmptyDictionary;
        if (clangOptionality == OTK_Optional)
          emptyDictionaryKind = DefaultArgumentKind::NilLiteral;

        bool sawInfo = false;
        for (auto word : llvm::reverse(camel_case::getWords(searchStr))) {
          if (camel_case::sameWordIgnoreFirstCase(word, "options"))
            return emptyDictionaryKind;

          if (camel_case::sameWordIgnoreFirstCase(word, "attributes"))
            return emptyDictionaryKind;

          if (camel_case::sameWordIgnoreFirstCase(word, "info")) {
            sawInfo = true;
            continue;
          }

          if (sawInfo && camel_case::sameWordIgnoreFirstCase(word, "user"))
            return emptyDictionaryKind;

          if (argumentLabel.empty())
            break;

          sawInfo = false;
        }
      }
    }
  }

  return DefaultArgumentKind::None;
}

/// Adjust the result type of a throwing function based on the
/// imported error information.
static ImportedType
adjustResultTypeForThrowingFunction(ForeignErrorConvention::Info errorInfo,
                                    ImportedType importedType) {
  switch (errorInfo.TheKind) {
  case ForeignErrorConvention::ZeroResult:
  case ForeignErrorConvention::NonZeroResult:
    // Check for a bad override.
    if (importedType.getType()->isVoid())
      return {Type(), false};
    return {TupleType::getEmpty(importedType.getType()->getASTContext()),
            false};

  case ForeignErrorConvention::NilResult:
    if (Type unwrappedTy = importedType.getType()->getOptionalObjectType())
      return {unwrappedTy, false};
    // Check for a bad override.
    if (importedType.getType()->isVoid())
      return {Type(), false};
    // It's possible an Objective-C method overrides the base method to never
    // fail, and marks the method _Nonnull to indicate that. Swift can't
    // represent that, but it shouldn't fall over either.
    return importedType;

  case ForeignErrorConvention::ZeroPreservedResult:
    // Check for a bad override.
    if (importedType.getType()->isVoid())
      return {Type(), false};
    return importedType;

  case ForeignErrorConvention::NonNilError:
    return importedType;
  }

  llvm_unreachable("Invalid ForeignErrorConvention.");
}

/// Produce the foreign error convention from the imported error info,
/// error parameter type, and original result type.
static ForeignErrorConvention
getForeignErrorInfo(ForeignErrorConvention::Info errorInfo,
                    CanType errorParamTy, CanType origResultTy) {
  assert(errorParamTy && "not fully initialized!");
  using FEC = ForeignErrorConvention;
  auto ParamIndex = errorInfo.ErrorParameterIndex;
  auto IsOwned = (FEC::IsOwned_t) errorInfo.ErrorIsOwned;
  auto ReplaceParamWithVoid = errorInfo.ErrorParameterIsReplaced
                                ? FEC::IsReplaced
                                : FEC::IsNotReplaced;
  switch (errorInfo.TheKind) {
  case FEC::ZeroResult:
    return FEC::getZeroResult(ParamIndex, IsOwned, ReplaceParamWithVoid,
                              errorParamTy, origResultTy);
  case FEC::NonZeroResult:
    return FEC::getNonZeroResult(ParamIndex, IsOwned, ReplaceParamWithVoid,
                                 errorParamTy, origResultTy);
  case FEC::ZeroPreservedResult:
    return FEC::getZeroPreservedResult(ParamIndex, IsOwned,
                                       ReplaceParamWithVoid, errorParamTy);
  case FEC::NilResult:
    return FEC::getNilResult(ParamIndex, IsOwned, ReplaceParamWithVoid,
                             errorParamTy);
  case FEC::NonNilError:
    return FEC::getNonNilError(ParamIndex, IsOwned, ReplaceParamWithVoid,
                               errorParamTy);
  }
  llvm_unreachable("bad error convention");
}

// 'toDC' must be a subclass or a type conforming to the protocol
// 'fromDC'.
static Type mapGenericArgs(const DeclContext *fromDC,
                           const DeclContext *toDC, Type type) {
  if (fromDC == toDC)
    return type;

  auto subs = toDC->getDeclaredInterfaceType()->getContextSubstitutionMap(
                                            toDC->getParentModule(), fromDC);
  return type.subst(subs);
}

/// Decompose the type of a completion handler parameter in a function
/// imported as 'async' and produce the result type of the 'async' function.
static Type decomposeCompletionHandlerType(
   Type paramTy, ForeignAsyncConvention::Info info) {
  auto fnType = paramTy->lookThroughAllOptionalTypes()->getAs<FunctionType>();
  if (!fnType)
    return Type();

  SmallVector<TupleTypeElt, 2> resultTypeElts;
  auto params = fnType->getParams();
  for (unsigned paramIdx : indices(params)) {
    const auto &param = params[paramIdx];
    if (param.isInOut() || param.isVariadic())
      return Type();

    // If there are error-related parameters to the completion handler, they are
    // not part of the result type of the asynchronous function.
    if (info.completionHandlerErrorParamIndex() &&
        paramIdx == *info.completionHandlerErrorParamIndex())
      continue;
    if (info.completionHandlerFlagParamIndex() &&
        paramIdx == *info.completionHandlerFlagParamIndex())
      continue;

    resultTypeElts.push_back(param.getPlainType());
  }

  switch (resultTypeElts.size()) {
  case 0:
    return paramTy->getASTContext().getVoidType();

  case 1:
    return resultTypeElts.front().getType();

  default:
    return TupleType::get(resultTypeElts, paramTy->getASTContext());
  }
}

ImportedType ClangImporter::Implementation::importEffectfulPropertyType(
                                        const clang::ObjCMethodDecl *decl,
                                        DeclContext *dc,
                                        importer::ImportedName name,
                                        bool isFromSystemModule) {
  // here we expect a method that is being imported as an effectful property.
  // thus, we currently require async info.
  if (!name.getAsyncInfo())
    return ImportedType();

  // a variadic method doesn't make sense here
  if (decl->isVariadic())
    return ImportedType();

  // Our strategy here is to determine what the return type of the method would
  // be, had we imported it as a method.

  Optional<ForeignAsyncConvention> asyncConvention;
  Optional<ForeignErrorConvention> errorConvention;

  const auto kind = SpecialMethodKind::Regular;

  // Import the parameter list and result type.
  ParameterList *bodyParams = nullptr;
  auto methodReturnType = importMethodParamsAndReturnType(
      dc, decl, decl->parameters(), false,
      isFromSystemModule, &bodyParams, name,
      asyncConvention, errorConvention, kind);

  // was there a problem during import?
  if (!methodReturnType)
    return ImportedType();

  assert(bodyParams);

  // getter mustn't have any parameters!
  if (bodyParams->size() != 0) {
    return ImportedType();
  }

  // We expect that the method, after import, will have only an async convention
  if (!asyncConvention || errorConvention)
    return ImportedType();

  return methodReturnType;
}

ImportedType ClangImporter::Implementation::importMethodParamsAndReturnType(
    const DeclContext *dc, const clang::ObjCMethodDecl *clangDecl,
    ArrayRef<const clang::ParmVarDecl *> params, bool isVariadic,
    bool isFromSystemModule, ParameterList **bodyParams,
    ImportedName importedName,
    Optional<ForeignAsyncConvention> &asyncConvention,
    Optional<ForeignErrorConvention> &foreignErrorInfo,
    SpecialMethodKind kind) {

  // Cannot import variadic types unless specially handled before calling this
  // function.
  if (isVariadic || clangDecl->sel_param_end() != clangDecl->param_end())
    return {Type(), false};

  // Clang doesn't provide pragmas for auditing the CF behavior of
  // ObjC methods, but it does have attributes for declaring
  // return-type management:
  //   - cf_returns_retained and cf_returns_not_retained are obvious
  //   - objc_returns_inner_pointer is sometimes used on methods
  //     returning CF types as a workaround for ARC not managing CF
  //     objects
  ImportTypeKind resultKind;
  if (isObjCMethodResultAudited(clangDecl))
    resultKind = ImportTypeKind::AuditedResult;
  else
    resultKind = ImportTypeKind::Result;

  // The member was defined in 'origDC', but is being imported into 'dc'.
  // 'dc' must be a subclass or a type conforming to protocol.
  DeclContext *origDC = importDeclContextOf(clangDecl,
                                            clangDecl->getDeclContext());
  assert(origDC);

  // Import the result type.
  CanType origSwiftResultTy;
  Optional<ForeignErrorConvention::Info> errorInfo =
      importedName.getErrorInfo();
  auto asyncInfo = importedName.getAsyncInfo();
  bool isAsync = asyncInfo.has_value();
  if (!isAsync)
    asyncInfo = importedName.getAsyncAlternateInfo();

  OptionalTypeKind OptionalityOfReturn;
  if (clangDecl->hasAttr<clang::ReturnsNonNullAttr>()) {
    OptionalityOfReturn = OTK_None;
  } else {
    OptionalityOfReturn = OTK_ImplicitlyUnwrappedOptional;
  }

  bool allowNSUIntegerAsIntInResult = isFromSystemModule;
  if (allowNSUIntegerAsIntInResult) {
    clang::Selector sel = clangDecl->getSelector();
    StringRef name = sel.getNameForSlot(0);
    if (!name.empty()) {
      allowNSUIntegerAsIntInResult = !nameContainsUnsigned(name);
    }
  }

  ImportDiagnosticAdder addImportDiag(*this, clangDecl,
                                      clangDecl->getLocation());
  clang::QualType resultType = clangDecl->getReturnType();
  if (auto elaborated = dyn_cast<clang::ElaboratedType>(resultType))
    resultType = elaborated->desugar();

  ImportedType importedType;
  if (auto typedefType = dyn_cast<clang::TypedefType>(resultType.getTypePtr())) {
    if (isUnavailableInSwift(typedefType->getDecl())) {
      if (auto clangEnum = findAnonymousEnumForTypedef(SwiftContext, typedefType)) {
        // If this fails, it means that we need a stronger predicate for
        // determining the relationship between an enum and typedef.
        assert(clangEnum.value()->getIntegerType()->getCanonicalTypeInternal() ==
               typedefType->getCanonicalTypeInternal());
        if (auto swiftEnum = importDecl(*clangEnum, CurrentVersion)) {
          importedType = {cast<TypeDecl>(swiftEnum)->getDeclaredInterfaceType(),
                          false};
        }
      }
    }
  }

  if (!importedType)
    importedType = importType(resultType, resultKind, addImportDiag,
                              allowNSUIntegerAsIntInResult, Bridgeability::Full,
                              getImportTypeAttrs(clangDecl),
                              OptionalityOfReturn);

  // Adjust the result type for a throwing function.
  if (importedType.getType() && errorInfo) {

    // Get the original unbridged result type.
    auto origImportedType =
        importType(resultType, resultKind, addImportDiag,
                   allowNSUIntegerAsIntInResult, Bridgeability::None,
                   getImportTypeAttrs(clangDecl), OptionalityOfReturn);
    origSwiftResultTy = origImportedType.getType()->getCanonicalType();

    importedType =
        adjustResultTypeForThrowingFunction(*errorInfo, importedType);
  }

  auto swiftResultTy = importedType.getType();

  if (swiftResultTy &&
      clangDecl->getMethodFamily() == clang::OMF_performSelector) {
    // performSelector methods that return 'id' should be imported into Swift
    // as returning Unmanaged<AnyObject>.
    Type nonOptionalTy = swiftResultTy->getOptionalObjectType();
    bool resultIsOptional = (bool) nonOptionalTy;
    if (!nonOptionalTy)
      nonOptionalTy = swiftResultTy;

    // Undo 'Any' bridging.
    if (nonOptionalTy->isAny())
      nonOptionalTy = SwiftContext.getAnyObjectType();

    if (nonOptionalTy->isAnyClassReferenceType()) {
      swiftResultTy = getUnmanagedType(*this, nonOptionalTy);
      if (resultIsOptional)
        swiftResultTy = OptionalType::get(swiftResultTy);
    }
  }

  if (!swiftResultTy) {
    addImportDiagnostic(clangDecl, Diagnostic(diag::return_type_not_imported),
                        clangDecl->getSourceRange().getBegin());
    return {Type(), false};
  }

  swiftResultTy = mapGenericArgs(origDC, dc, swiftResultTy);

  CanType errorParamType;
  CanType completionHandlerType;

  SmallBitVector nonNullArgs = getNonNullArgs(clangDecl, params);

  // Import the parameters.
  SmallVector<ParamDecl*, 4> swiftParams;

  auto addEmptyTupleParameter = [&](Identifier argName) {
    // It doesn't actually matter which DeclContext we use, so just
    // use the imported header unit.
    auto type = TupleType::getEmpty(SwiftContext);
    auto var = new (SwiftContext) ParamDecl(SourceLoc(),
                                            SourceLoc(), argName,
                                            SourceLoc(), argName,
                                            ImportedHeaderUnit);
    var->setSpecifier(ParamSpecifier::Default);
    var->setInterfaceType(type);
    swiftParams.push_back(var);
  };

  auto argNames = importedName.getDeclName().getArgumentNames();
  unsigned nameIndex = 0;
  for (size_t paramIndex = 0, e = params.size(); paramIndex != e;
       ++paramIndex) {
    auto param = params[paramIndex];
    auto paramTy = param->getType();
    auto paramIsError = errorInfo && paramIndex == errorInfo->ErrorParameterIndex;
    if (paramTy->isVoidType()) {
      assert(!paramIsError);
      ++nameIndex;
      continue;
    }

    bool paramIsCompletionHandler =
        asyncInfo && paramIndex == asyncInfo->completionHandlerParamIndex();
    // Figure out if this is a completion handler parameter whose error
    // parameter is used to indicate throwing.
    Optional<unsigned> completionHandlerErrorParamIndex;
    if (isAsync && paramIsCompletionHandler) {
      completionHandlerErrorParamIndex =
          asyncInfo->completionHandlerErrorParamIndex();
    }

    // Check nullability of the parameter.
    bool knownNonNull = !nonNullArgs.empty() && nonNullArgs[paramIndex];
    OptionalTypeKind optionalityOfParam =
        getParamOptionality(param, knownNonNull);

    bool allowNSUIntegerAsIntInParam = isFromSystemModule;
    if (allowNSUIntegerAsIntInParam) {
      StringRef name;
      clang::Selector sel = clangDecl->getSelector();
      if (nameIndex < sel.getNumArgs())
        name = sel.getNameForSlot(nameIndex);
      if (name.empty() && nameIndex == 0)
        name = sel.getNameForSlot(0);
      if (!name.empty())
        allowNSUIntegerAsIntInParam = !nameContainsUnsigned(name);
    }

    ImportDiagnosticAdder paramAddDiag(*this, clangDecl, param->getLocation());

    auto swiftParamTyOpt = importParameterType(
        param, optionalityOfParam, allowNSUIntegerAsIntInParam,
        kind == SpecialMethodKind::NSDictionarySubscriptGetter, paramIsError,
        paramIsCompletionHandler, completionHandlerErrorParamIndex,
        ArrayRef<GenericTypeParamDecl *>(), paramAddDiag);
    if (!swiftParamTyOpt) {
      addImportDiagnostic(param,
                          Diagnostic(diag::parameter_type_not_imported, param),
                          param->getSourceRange().getBegin());
      return {Type(), false};
    }
    auto swiftParamTy = swiftParamTyOpt->swiftTy;
    bool isInOut = swiftParamTyOpt->isInOut;
    bool isParamTypeImplicitlyUnwrapped =
        swiftParamTyOpt->isParamTypeImplicitlyUnwrapped;

    swiftParamTy = mapGenericArgs(origDC, dc, swiftParamTy);

    // If this is the error parameter, remember it, but don't build it
    // into the parameter type.
    if (paramIsError) {
      errorParamType = swiftParamTy->getCanonicalType();

      // ...unless we're supposed to replace it with ().
      if (errorInfo->ErrorParameterIsReplaced) {
        addEmptyTupleParameter(argNames[nameIndex]);
        ++nameIndex;
      }
      continue;
    }

    // If this is a completion handler, figure out it's effect on the result
    // type but don't build it into the parameter type.
    if (isAsync && paramIsCompletionHandler) {
      if (Type replacedSwiftResultTy =
              decomposeCompletionHandlerType(swiftParamTy, *asyncInfo)) {
        swiftResultTy = replacedSwiftResultTy;

        ImportTypeKind importKind = getImportTypeKindForParam(param);

        // Import the original completion handler type without adjustments.
        Type origSwiftParamTy = importType(
            paramTy, importKind, paramAddDiag, allowNSUIntegerAsIntInParam,
            Bridgeability::Full, ImportTypeAttrs(), optionalityOfParam,
            /*resugarNSErrorPointer=*/!paramIsError, None).getType();
        completionHandlerType = mapGenericArgs(origDC, dc, origSwiftParamTy)
            ->getCanonicalType();
        continue;
      }

      llvm_unreachable("async info computed incorrectly?");
    }

    // Figure out the name for this argument, which comes from the method name.
    Identifier name;
    if (nameIndex < argNames.size()) {
      name = argNames[nameIndex];
    }
    ++nameIndex;

    // Set up the parameter info
    auto paramInfo = getParameterInfo(this, param, name, swiftParamTy, isInOut,
                                      isParamTypeImplicitlyUnwrapped);

    // Determine whether we have a default argument.
    if (kind == SpecialMethodKind::Regular ||
        kind == SpecialMethodKind::Constructor) {
      bool isLastParameter =
          (paramIndex == params.size() - 1) ||
          (paramIndex == params.size() - 2 && errorInfo &&
           errorInfo->ErrorParameterIndex == params.size() - 1);

      auto defaultArg = inferDefaultArgument(
          param->getType(), optionalityOfParam,
          importedName.getDeclName().getBaseName(), name.str(), paramIndex == 0,
          isLastParameter, getNameImporter());
      if (defaultArg != DefaultArgumentKind::None)
        paramInfo->setDefaultArgumentKind(defaultArg);
    }
    swiftParams.push_back(paramInfo);
  }

  // If we have a constructor with no parameters and a name with an
  // argument name, synthesize a Void parameter with that name.
  if (kind == SpecialMethodKind::Constructor && params.empty() &&
      argNames.size() == 1) {
    addEmptyTupleParameter(argNames[0]);
  }

  if (importedName.hasCustomName() && argNames.size() != swiftParams.size()) {
    // Note carefully: we're emitting a warning in the /Clang/ buffer.
    if (clangDecl->getLocation().isValid()) {
      HeaderLoc methodLoc(clangDecl->getLocation());
      diagnose(methodLoc, diag::invalid_swift_name_method,
               swiftParams.size() < argNames.size(),
               swiftParams.size(), argNames.size());
      ModuleDecl *parentModule = dc->getParentModule();
      if (parentModule != ImportedHeaderUnit->getParentModule()) {
        diagnose(methodLoc, diag::unresolvable_clang_decl_is_a_framework_bug,
                 parentModule->getName().str());
      }
    }
    return {Type(), false};
  }


  // Form the parameter list.
  *bodyParams = ParameterList::create(SwiftContext, swiftParams);

  if (clangDecl->hasAttr<clang::NoReturnAttr>()) {
    origSwiftResultTy = SwiftContext.getNeverType()->getCanonicalType();
    swiftResultTy = SwiftContext.getNeverType();
  }

  if (isAsync) {
    asyncConvention = ForeignAsyncConvention(
        completionHandlerType, asyncInfo->completionHandlerParamIndex(),
        asyncInfo->completionHandlerErrorParamIndex(),
        asyncInfo->completionHandlerFlagParamIndex(),
        asyncInfo->completionHandlerFlagIsErrorOnZero());
  }

  if (errorInfo) {
    foreignErrorInfo = getForeignErrorInfo(*errorInfo, errorParamType,
                                           origSwiftResultTy);
  }

  return {swiftResultTy,
          importedType.isImplicitlyUnwrapped()};
}

ImportedType ClangImporter::Implementation::importAccessorParamsAndReturnType(
    const DeclContext *dc, const clang::ObjCPropertyDecl *property,
    const clang::ObjCMethodDecl *clangDecl, bool isFromSystemModule,
    ImportedName functionName, swift::ParameterList **params) {
  // Note: We're using a pointer instead of a reference here to make it clear
  // at the call site that this is an out-parameter.
  assert(params && "'params' is a required out-parameter");

  // Determine if the method is a property getter or setter.
  bool isGetter;
  if (clangDecl->parameters().empty())
    isGetter = true;
  else if (clangDecl->parameters().size() == 1)
    isGetter = false;
  else
    llvm_unreachable("not a valid accessor");

  // The member was defined in 'origDC', but is being imported into 'dc'.
  // 'dc' must be a subclass or a type conforming to protocol.
  // FIXME: Duplicated from importMethodParamsAndReturnType.
  DeclContext *origDC = importDeclContextOf(property,
                                            property->getDeclContext());
  assert(origDC);

  // Import the property type, independent of what kind of accessor this is.
  auto importedType = importPropertyType(property, isFromSystemModule);
  if (!importedType)
    return {Type(), false};

  auto propertyTy = mapGenericArgs(origDC, dc, importedType.getType());
  bool isIUO = importedType.isImplicitlyUnwrapped();

  // Now build up the resulting FunctionType and parameters.
  Type resultTy;
  if (isGetter) {
    *params = ParameterList::createEmpty(SwiftContext);
    resultTy = propertyTy;
  } else {
    const clang::ParmVarDecl *param = clangDecl->parameters().front();
    ImportedName fullBodyName = importFullName(param, CurrentVersion);
    Identifier bodyName = fullBodyName.getDeclName().getBaseIdentifier();
    SourceLoc nameLoc = importSourceLoc(param->getLocation());
    Identifier argLabel = functionName.getDeclName().getArgumentNames().front();
    auto paramInfo
      = createDeclWithClangNode<ParamDecl>(param, AccessLevel::Private,
                                           /*let loc*/SourceLoc(),
                                           /*label loc*/SourceLoc(),
                                           argLabel, nameLoc, bodyName,
                                           /*dummy DC*/ImportedHeaderUnit);
    paramInfo->setSpecifier(ParamSpecifier::Default);
    paramInfo->setInterfaceType(propertyTy);

    *params = ParameterList::create(SwiftContext, paramInfo);
    resultTy = SwiftContext.getVoidType();
    isIUO = false;
  }

  return {resultTy, isIUO};
}


ModuleDecl *ClangImporter::Implementation::getStdlibModule() {
  return SwiftContext.getStdlibModule(true);
}

ModuleDecl *ClangImporter::Implementation::getNamedModule(StringRef name) {
  return SwiftContext.getLoadedModule(SwiftContext.getIdentifier(name));
}

static ModuleDecl *tryLoadModule(ASTContext &C,
                             Identifier moduleName,
                             bool importForwardDeclarations,
                             llvm::DenseMap<Identifier, ModuleDecl *>
                               &checkedModules) {
  // If we've already done this check, return the cached result.
  auto known = checkedModules.find(moduleName);
  if (known != checkedModules.end())
    return known->second;

  ModuleDecl *module;

  // If we're synthesizing forward declarations, we don't want to pull in
  // the module too eagerly.
  if (importForwardDeclarations)
    module = C.getLoadedModule(moduleName);
  else
    module = C.getModuleByIdentifier(moduleName);

  checkedModules[moduleName] = module;
  return module;
}

ModuleDecl *ClangImporter::Implementation::tryLoadFoundationModule() {
  return tryLoadModule(SwiftContext, SwiftContext.Id_Foundation,
                       ImportForwardDeclarations, checkedModules);
}

bool ClangImporter::Implementation::canImportFoundationModule() {
  ImportPath::Module::Builder builder(SwiftContext.Id_Foundation);
  auto modulePath = builder.get();
  return SwiftContext.canImportModule(modulePath);
}

Type ClangImporter::Implementation::getNamedSwiftType(ModuleDecl *module,
                                                      StringRef name) {
  return SwiftContext.getNamedSwiftType(module, name);
}

Type ClangImporter::Implementation::getNamedSwiftType(StringRef moduleName,
                                                      StringRef name) {
  // Try to load the module.
  auto module = tryLoadModule(SwiftContext,
                              SwiftContext.getIdentifier(moduleName),
                              ImportForwardDeclarations, checkedModules);
  if (!module) return Type();

  return getNamedSwiftType(module, name);
}

Decl *ClangImporter::Implementation::importDeclByName(StringRef name) {
  auto &sema = Instance->getSema();

  // Map the name. If we can't represent the Swift name in Clang, bail out now.
  auto clangName = &getClangASTContext().Idents.get(name);

  // Perform name lookup into the global scope.
  // FIXME: Map source locations over.
  clang::LookupResult lookupResult(sema, clangName, clang::SourceLocation(),
                                   clang::Sema::LookupOrdinaryName);
  lookupResult.setAllowHidden(true);
  if (!sema.LookupName(lookupResult, /*Scope=*/sema.TUScope)) {
    return nullptr;
  }

  for (auto decl : lookupResult) {
    if (auto swiftDecl =
            importDecl(decl->getUnderlyingDecl(), CurrentVersion)) {
      return swiftDecl;
    }
  }

  return nullptr;
}

Type ClangImporter::Implementation::getNSObjectType() {
  if (NSObjectTy)
    return NSObjectTy;

  if (auto decl = dyn_cast_or_null<ClassDecl>(importDeclByName("NSObject"))) {
    NSObjectTy = decl->getDeclaredInterfaceType();
    return NSObjectTy;
  }

  return Type();
}

bool ClangImporter::Implementation::matchesHashableBound(Type type) {
  Type NSObjectType = getNSObjectType();
  if (!NSObjectType)
    return false;

  // Match generic parameters against their bounds.
  if (auto *genericTy = type->getAs<GenericTypeParamType>()) {
    if (auto *generic = genericTy->getDecl()) {
      auto genericSig =
        generic->getDeclContext()->getGenericSignatureOfContext();
      if (genericSig && genericSig->getRequiredProtocols(type).empty()) {
        type = genericSig->getSuperclassBound(type);
        if (!type)
          return false;
      }
    }
  }

  // Existentials cannot match the Hashable bound.
  if (type->isAnyExistentialType())
    return false;

  // Class type that inherits from NSObject.
  if (NSObjectType->isExactSuperclassOf(type))
    return true;

  // Struct or enum type must have been bridged.
  // TODO: Check that the bridged type is Hashable?
  if (type->getStructOrBoundGenericStruct() ||
      type->getEnumOrBoundGenericEnum()) {
    auto nominal = type->getAnyNominal();
    auto hashable = SwiftContext.getProtocol(KnownProtocolKind::Hashable);
    SmallVector<ProtocolConformance *, 2> conformances;
    return hashable &&
      nominal->lookupConformance(hashable, conformances);
  }

  return false;
}

static Type getNamedProtocolType(ClangImporter::Implementation &impl,
                                 StringRef name) {
  auto &sema = impl.getClangSema();
  auto clangName = &sema.getASTContext().Idents.get(name);
  assert(clangName);

  // Perform name lookup into the global scope.
  clang::LookupResult lookupResult(sema, clangName, clang::SourceLocation(),
                                   clang::Sema::LookupObjCProtocolName);
  lookupResult.setAllowHidden(true);
  if (!sema.LookupName(lookupResult, /*Scope=*/nullptr))
    return Type();

  for (auto decl : lookupResult) {
    if (auto swiftDecl =
            impl.importDecl(decl->getUnderlyingDecl(), impl.CurrentVersion)) {
      if (auto protoDecl =
              dynCastIgnoringCompatibilityAlias<ProtocolDecl>(swiftDecl)) {
        return protoDecl->getDeclaredInterfaceType();
      }
    }
  }

  return Type();
}

Type ClangImporter::Implementation::getNSObjectProtocolType() {
  return getNamedProtocolType(*this, "NSObject");
}
