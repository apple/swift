//===--- PrintClangFunction.cpp - Printer for C/C++ functions ---*- C++ -*-===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2022 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//

#include "PrintClangFunction.h"
#include "ClangSyntaxPrinter.h"
#include "DeclAndTypePrinter.h"
#include "OutputLanguageMode.h"
#include "PrimitiveTypeMapping.h"
#include "PrintClangClassType.h"
#include "PrintClangValueType.h"
#include "SwiftToClangInteropContext.h"
#include "swift/ABI/MetadataValues.h"
#include "swift/AST/ASTContext.h"
#include "swift/AST/Decl.h"
#include "swift/AST/GenericParamList.h"
#include "swift/AST/Module.h"
#include "swift/AST/ParameterList.h"
#include "swift/AST/SwiftNameTranslation.h"
#include "swift/AST/Type.h"
#include "swift/AST/TypeVisitor.h"
#include "swift/Basic/Assertions.h"
#include "swift/ClangImporter/ClangImporter.h"
#include "swift/IRGen/IRABIDetailsProvider.h"
#include "clang/AST/ASTContext.h"
#include "clang/AST/Attr.h"
#include "clang/AST/Decl.h"
#include "clang/AST/DeclObjC.h"
#include "llvm/ADT/STLExtras.h"

using namespace swift;

namespace {

// FIXME: RENAME.
enum class FunctionSignatureTypeUse { TypeReference, ParamType, ReturnType };

std::optional<PrimitiveTypeMapping::ClangTypeInfo>
getKnownTypeInfo(const TypeDecl *typeDecl, PrimitiveTypeMapping &typeMapping,
                 OutputLanguageMode languageMode) {
  return languageMode == OutputLanguageMode::Cxx
             ? typeMapping.getKnownCxxTypeInfo(typeDecl)
             : typeMapping.getKnownCTypeInfo(typeDecl);
}

bool isKnownType(Type t, PrimitiveTypeMapping &typeMapping,
                 OutputLanguageMode languageMode) {
  if (auto *typeAliasType = dyn_cast<TypeAliasType>(t.getPointer())) {
    auto aliasInfo =
        getKnownTypeInfo(typeAliasType->getDecl(), typeMapping, languageMode);
    if (aliasInfo != std::nullopt)
      return true;
    return isKnownType(typeAliasType->getSinglyDesugaredType(), typeMapping,
                       languageMode);
  }

  const TypeDecl *typeDecl;
  auto *tPtr = t->isOptional() ? t->getOptionalObjectType()->getDesugaredType()
                               : t->getDesugaredType();
  if (auto *bgt = dyn_cast<BoundGenericStructType>(tPtr)) {
    return bgt->isUnsafePointer() || bgt->isUnsafeMutablePointer();
  }
  if (auto *structType = dyn_cast<StructType>(tPtr)) {
    auto nullableInfo =
        getKnownTypeInfo(structType->getDecl(), typeMapping, languageMode);
    if (nullableInfo && nullableInfo->canBeNullable)
      return true;
  }
  if (auto *classType = dyn_cast<ClassType>(tPtr)) {
    return classType->getClassOrBoundGenericClass()->hasClangNode() &&
           isa<clang::ObjCInterfaceDecl>(
               classType->getClassOrBoundGenericClass()->getClangDecl());
  }

  if (auto *structDecl = t->getStructOrBoundGenericStruct())
    typeDecl = structDecl;
  else
    return false;
  return getKnownTypeInfo(typeDecl, typeMapping, languageMode) != std::nullopt;
}

bool isKnownCxxType(Type t, PrimitiveTypeMapping &typeMapping) {
  return isKnownType(t, typeMapping, OutputLanguageMode::Cxx);
}

bool isKnownCType(Type t, PrimitiveTypeMapping &typeMapping) {
  return isKnownType(t, typeMapping, OutputLanguageMode::ObjC);
}

struct CFunctionSignatureTypePrinterModifierDelegate {
  /// Prefix the initially printed value type.
  std::optional<llvm::function_ref<ClangValueTypePrinter::TypeUseKind(
      ClangValueTypePrinter::TypeUseKind)>>
      mapValueTypeUseKind = std::nullopt;
};

class ClangTypeHandler {
public:
  ClangTypeHandler(const clang::Decl *typeDecl)
      : typeDecl(dyn_cast<clang::TagDecl>(typeDecl)) {}

  bool isRepresentable() const {
    // We can only return tag types.
    if (typeDecl) {
      // We can return trivial types.
      if (isTrivial(typeDecl))
        return true;

      // We can return nontrivial types iff they can be moved or copied.
      if (auto *record = dyn_cast<clang::CXXRecordDecl>(typeDecl)) {
        return record->hasMoveConstructor() ||
               record->hasCopyConstructorWithConstParam();
      }
    }

    // Otherwise, we can't return this type.
    return false;
  }

private:
  /// Is the tag type trivial?
  static bool isTrivial(const clang::TagDecl *typeDecl) {
    if (!typeDecl)
      return false;

    if (auto *record = dyn_cast<clang::CXXRecordDecl>(typeDecl))
      return record->isTrivial();

    // FIXME: If we can get plain clang::RecordDecls here, we need to figure out
    //        how nontrivial (i.e. ARC) fields work.
    assert(!isa<clang::RecordDecl>(typeDecl));

    // C-family enums are always trivial.
    return isa<clang::EnumDecl>(typeDecl);
  }

public:
  void printTypeName(raw_ostream &os) const {
    ClangSyntaxPrinter(os).printClangTypeReference(typeDecl);
  }

  static void
  printGenericReturnScaffold(raw_ostream &os, StringRef templateParamName,
                             llvm::function_ref<void(StringRef)> bodyOfReturn) {
    printReturnScaffold(nullptr, os, templateParamName, templateParamName,
                        bodyOfReturn);
  }

  void printReturnScaffold(raw_ostream &os,
                           llvm::function_ref<void(StringRef)> bodyOfReturn) {
    std::string fullQualifiedType;
    std::string typeName;
    {
      llvm::raw_string_ostream typeNameOS(fullQualifiedType);
      printTypeName(typeNameOS);
      llvm::raw_string_ostream unqualTypeNameOS(typeName);
      unqualTypeNameOS << typeDecl->getName();
    }
    printReturnScaffold(typeDecl, os, fullQualifiedType, typeName,
                        bodyOfReturn);
  }

private:
  static void
  printReturnScaffold(const clang::TagDecl *typeDecl, raw_ostream &os,
                      StringRef fullQualifiedType, StringRef typeName,
                      llvm::function_ref<void(StringRef)> bodyOfReturn) {
    os << "alignas(alignof(" << fullQualifiedType << ")) char storage[sizeof("
       << fullQualifiedType << ")];\n";
    os << "auto * _Nonnull storageObjectPtr = reinterpret_cast<"
       << fullQualifiedType << " *>(storage);\n";
    bodyOfReturn("storage");
    os << ";\n";
    if (isTrivial(typeDecl)) {
      // Trivial object can be just copied and not destroyed.
      os << "return *storageObjectPtr;\n";
      return;
    }
    // Force a `std::move` on the resulting object.
    os << fullQualifiedType << " result(static_cast<" << fullQualifiedType
       << " &&>(*storageObjectPtr));\n";
    os << "storageObjectPtr->~" << typeName << "();\n";
    os << "return result;\n";
  }

  const clang::TagDecl *typeDecl;
};

// Prints types in the C function signature that corresponds to the
// native Swift function/method.
class CFunctionSignatureTypePrinter
    : public TypeVisitor<CFunctionSignatureTypePrinter, ClangRepresentation,
                         std::optional<OptionalTypeKind>, bool>,
      private ClangSyntaxPrinter {
public:
  CFunctionSignatureTypePrinter(
      raw_ostream &os, raw_ostream &cPrologueOS,
      PrimitiveTypeMapping &typeMapping, OutputLanguageMode languageMode,
      SwiftToClangInteropContext &interopContext,
      CFunctionSignatureTypePrinterModifierDelegate modifiersDelegate,
      const ModuleDecl *moduleContext, DeclAndTypePrinter &declPrinter,
      FunctionSignatureTypeUse typeUseKind =
          FunctionSignatureTypeUse::ParamType)
      : ClangSyntaxPrinter(os), cPrologueOS(cPrologueOS),
        typeMapping(typeMapping), interopContext(interopContext),
        languageMode(languageMode), modifiersDelegate(modifiersDelegate),
        moduleContext(moduleContext), declPrinter(declPrinter),
        typeUseKind(typeUseKind) {}

  void printInoutTypeModifier() {
    os << (languageMode == swift::OutputLanguageMode::Cxx ? " &"
                                                          : " * _Nonnull");
  }

  bool printIfKnownSimpleType(const TypeDecl *typeDecl,
                              std::optional<OptionalTypeKind> optionalKind,
                              bool isInOutParam) {
    auto knownTypeInfo = getKnownTypeInfo(typeDecl, typeMapping, languageMode);
    if (!knownTypeInfo)
      return false;
    bool shouldPrintOptional = optionalKind && *optionalKind != OTK_None &&
                               !knownTypeInfo->canBeNullable;
    if (!isInOutParam && shouldPrintOptional &&
        typeUseKind == FunctionSignatureTypeUse::ParamType)
      os << "const ";
    printOptional(shouldPrintOptional ? optionalKind : std::nullopt, [&]() {
      os << knownTypeInfo->name;
      if (knownTypeInfo->canBeNullable) {
        printNullability(optionalKind);
      }
    });
    if (!isInOutParam && shouldPrintOptional &&
        typeUseKind == FunctionSignatureTypeUse::ParamType)
      os << '&';
    if (isInOutParam)
      printInoutTypeModifier();
    return true;
  }

  void printOptional(std::optional<OptionalTypeKind> optionalKind,
                     llvm::function_ref<void()> body) {
    if (!optionalKind || optionalKind == OTK_None)
      return body();
    printBaseName(moduleContext->getASTContext().getStdlibModule());
    os << "::Optional<";
    body();
    os << '>';
  }

  ClangRepresentation visitType(TypeBase *Ty,
                                std::optional<OptionalTypeKind> optionalKind,
                                bool isInOutParam) {
    assert(Ty->getDesugaredType() == Ty && "unhandled sugared type");
    os << "/* ";
    Ty->print(os);
    os << " */";
    return ClangRepresentation::unsupported;
  }

  ClangRepresentation
  visitTupleType(TupleType *TT, std::optional<OptionalTypeKind> optionalKind,
                 bool isInOutParam) {
    if (TT->getNumElements() > 0)
      // FIXME: Handle non-void type.
      return ClangRepresentation::unsupported;
    // FIXME: how to support `()` parameters.
    if (typeUseKind != FunctionSignatureTypeUse::ReturnType)
      return ClangRepresentation::unsupported;
    os << "void";
    return ClangRepresentation::representable;
  }

  ClangRepresentation
  visitTypeAliasType(TypeAliasType *aliasTy,
                     std::optional<OptionalTypeKind> optionalKind,
                     bool isInOutParam) {
    const TypeAliasDecl *alias = aliasTy->getDecl();
    if (printIfKnownSimpleType(alias, optionalKind, isInOutParam))
      return ClangRepresentation::representable;

    return visitSugarType(aliasTy, optionalKind, isInOutParam);
  }

  ClangRepresentation
  visitSugarType(SugarType *sugarTy,
                 std::optional<OptionalTypeKind> optionalKind,
                 bool isInOutParam) {
    return visitPart(sugarTy->getSinglyDesugaredType(), optionalKind,
                     isInOutParam);
  }

  ClangRepresentation
  visitClassType(ClassType *CT, std::optional<OptionalTypeKind> optionalKind,
                 bool isInOutParam) {
    auto *cd = CT->getDecl();
    if (cd->hasClangNode()) {
      const auto *clangDecl = cd->getClangDecl();
      ClangSyntaxPrinter(os).printClangTypeReference(clangDecl);
      bool alreadyPointer = false;
      if (const auto *typedefDecl = dyn_cast<clang::TypedefNameDecl>(clangDecl))
        if (importer::isCFTypeDecl(typedefDecl))
          alreadyPointer = true;
      os << (alreadyPointer ? " " : " *")
         << (!optionalKind || *optionalKind == OTK_None ? "_Nonnull"
                                                        : "_Nullable");
      if (isInOutParam) {
        if (isa<clang::ObjCContainerDecl>(cd->getClangDecl()))
          os << " __strong";
        printInoutTypeModifier();
      }
      // FIXME: Mark that this is only ObjC representable.
      return ClangRepresentation::representable;
    }
    // FIXME: handle optionalKind.
    if (languageMode != OutputLanguageMode::Cxx) {
      os << "void * "
         << (!optionalKind || *optionalKind == OTK_None ? "_Nonnull"
                                                        : "_Nullable");
      if (isInOutParam)
        os << " * _Nonnull";
      return ClangRepresentation::representable;
    }
    if (typeUseKind == FunctionSignatureTypeUse::ParamType && !isInOutParam)
      os << "const ";
    printOptional(optionalKind, [&]() {
      ClangSyntaxPrinter(os).printBaseName(CT->getDecl());
    });
    if (typeUseKind == FunctionSignatureTypeUse::ParamType)
      os << "&";
    return ClangRepresentation::representable;
  }

  ClangRepresentation
  visitEnumType(EnumType *ET, std::optional<OptionalTypeKind> optionalKind,
                bool isInOutParam) {
    return visitValueType(ET, ET->getNominalOrBoundGenericNominal(),
                          optionalKind, isInOutParam);
  }

  ClangRepresentation
  visitStructType(StructType *ST, std::optional<OptionalTypeKind> optionalKind,
                  bool isInOutParam) {
    return visitValueType(ST, ST->getNominalOrBoundGenericNominal(),
                          optionalKind, isInOutParam);
  }

  ClangRepresentation visitGenericArgs(ArrayRef<Type> genericArgs) {
    if (genericArgs.empty())
      return ClangRepresentation::representable;
    os << '<';
    llvm::SaveAndRestore<FunctionSignatureTypeUse> typeUseNormal(
        typeUseKind, FunctionSignatureTypeUse::TypeReference);
    decltype(modifiersDelegate) emptyModifiersDelegate;
    llvm::SaveAndRestore<decltype(modifiersDelegate)> modReset(
        modifiersDelegate, emptyModifiersDelegate);
    ClangRepresentation result = ClangRepresentation::representable;
    llvm::interleaveComma(genericArgs, os, [&](Type t) {
      result.merge(visitPart(t, std::nullopt, false));
    });
    os << '>';
    return result;
  }

  ClangRepresentation
  visitValueType(TypeBase *type, const NominalTypeDecl *decl,
                 std::optional<OptionalTypeKind> optionalKind,
                 bool isInOutParam, ArrayRef<Type> genericArgs = {}) {
    assert(isa<StructDecl>(decl) || isa<EnumDecl>(decl));

    // Handle known type names.
    if (printIfKnownSimpleType(decl, optionalKind, isInOutParam))
      return ClangRepresentation::representable;
    if (!declPrinter.shouldInclude(decl))
      return ClangRepresentation::unsupported; // FIXME: propagate why it's not
                                               // exposed.
    // Only C++ mode supports struct types.
    if (languageMode != OutputLanguageMode::Cxx)
      return ClangRepresentation::unsupported;

    if (decl->hasClangNode()) {
      assert(genericArgs.empty() && "this path doesn't support generic args");
      ClangTypeHandler handler(decl->getClangDecl());
      if (!handler.isRepresentable())
        return ClangRepresentation::unsupported;
      if (typeUseKind == FunctionSignatureTypeUse::ParamType &&
          !isInOutParam)
        os << "const ";
      printOptional(optionalKind, [&]() { handler.printTypeName(os); });
      if (typeUseKind == FunctionSignatureTypeUse::ParamType)
        os << '&';
      return ClangRepresentation::representable;
    }

    if (typeUseKind == FunctionSignatureTypeUse::ParamType) {
      if (!isInOutParam) {
        os << "const ";
      }
      ClangRepresentation result = ClangRepresentation::representable;
      printOptional(optionalKind, [&]() {
        ClangSyntaxPrinter(os).printPrimaryCxxTypeName(decl, moduleContext);
        result = visitGenericArgs(genericArgs);
      });
      os << '&';
      return result;
    }

    ClangRepresentation result = ClangRepresentation::representable;
    printOptional(optionalKind, [&]() {
      ClangValueTypePrinter printer(os, cPrologueOS, interopContext);
      printer.printValueTypeReturnType(
          decl, languageMode,
          modifiersDelegate.mapValueTypeUseKind
              ? (*modifiersDelegate.mapValueTypeUseKind)(
                    ClangValueTypePrinter::TypeUseKind::CxxTypeName)
              : ClangValueTypePrinter::TypeUseKind::CxxTypeName,
          moduleContext);
      result = visitGenericArgs(genericArgs);
    });
    return result;
  }

  std::optional<ClangRepresentation>
  printIfKnownGenericStruct(const BoundGenericStructType *BGT,
                            std::optional<OptionalTypeKind> optionalKind,
                            bool isInOutParam) {
    auto bgsTy = Type(const_cast<BoundGenericStructType *>(BGT));
    bool isConst;
    if (bgsTy->isUnsafePointer())
      isConst = true;
    else if (bgsTy->isUnsafeMutablePointer())
      isConst = false;
    else
      return std::nullopt;

    auto args = BGT->getGenericArgs();
    assert(args.size() == 1);
    llvm::SaveAndRestore<FunctionSignatureTypeUse> typeUseNormal(
        typeUseKind, FunctionSignatureTypeUse::TypeReference);
    // FIXME: We can definitely support pointers to known Clang types.
    if (!isKnownCType(args.front(), typeMapping))
      return ClangRepresentation(ClangRepresentation::unsupported);
    auto partRepr = visitPart(args.front(), OTK_None, /*isInOutParam=*/false);
    if (partRepr.isUnsupported())
      return partRepr;
    if (isConst)
      os << " const";
    os << " *";
    printNullability(optionalKind);
    if (isInOutParam)
      printInoutTypeModifier();
    return ClangRepresentation(ClangRepresentation::representable);
  }

  ClangRepresentation
  visitBoundGenericStructType(BoundGenericStructType *BGT,
                              std::optional<OptionalTypeKind> optionalKind,
                              bool isInOutParam) {
    if (auto result =
            printIfKnownGenericStruct(BGT, optionalKind, isInOutParam))
      return *result;
    return visitValueType(BGT, BGT->getDecl(), optionalKind, isInOutParam,
                          BGT->getGenericArgs());
  }

  ClangRepresentation
  visitBoundGenericEnumType(BoundGenericEnumType *BGT,
                            std::optional<OptionalTypeKind> optionalKind,
                            bool isInOutParam) {
    return visitValueType(BGT, BGT->getDecl(), optionalKind, isInOutParam,
                          BGT->getGenericArgs());
  }

  ClangRepresentation
  visitGenericTypeParamType(GenericTypeParamType *genericTpt,
                            std::optional<OptionalTypeKind> optionalKind,
                            bool isInOutParam) {
    bool isParam = typeUseKind == FunctionSignatureTypeUse::ParamType;
    if (isParam && !isInOutParam)
      os << "const ";

    if (languageMode != OutputLanguageMode::Cxx) {
      // Note: This can happen for UnsafeMutablePointer<T>.
      if (typeUseKind != FunctionSignatureTypeUse::ParamType)
        return ClangRepresentation::unsupported;
      assert(typeUseKind == FunctionSignatureTypeUse::ParamType);
      // Pass an opaque param in C mode.
      os << "void * _Nonnull";
      return ClangRepresentation::representable;
    }
    printOptional(optionalKind, [&]() {
      ClangSyntaxPrinter(os).printGenericTypeParamTypeName(genericTpt);
    });
    // Pass a reference to the template type.
    if (isParam)
      os << '&';
    return ClangRepresentation::representable;
  }

  ClangRepresentation
  visitDynamicSelfType(DynamicSelfType *ds,
                       std::optional<OptionalTypeKind> optionalKind,
                       bool isInOutParam) {
    return visitPart(ds->getSelfType(), optionalKind, isInOutParam);
  }

  ClangRepresentation
  visitMetatypeType(MetatypeType *mt,
                    std::optional<OptionalTypeKind> optionalKind,
                    bool isInOutParam) {
    if (typeUseKind == FunctionSignatureTypeUse::TypeReference)
      return visitPart(mt->getInstanceType(), optionalKind, isInOutParam);
    return ClangRepresentation::unsupported;
  }

  ClangRepresentation visitPart(Type Ty,
                                std::optional<OptionalTypeKind> optionalKind,
                                bool isInOutParam) {
    return TypeVisitor::visit(Ty, optionalKind, isInOutParam);
  }

private:
  raw_ostream &cPrologueOS;
  PrimitiveTypeMapping &typeMapping;
  SwiftToClangInteropContext &interopContext;
  OutputLanguageMode languageMode;
  CFunctionSignatureTypePrinterModifierDelegate modifiersDelegate;
  const ModuleDecl *moduleContext;
  DeclAndTypePrinter &declPrinter;
  FunctionSignatureTypeUse typeUseKind;
};

} // end namespace

ClangRepresentation
DeclAndTypeClangFunctionPrinter::printClangFunctionReturnType(
    raw_ostream &stream, Type ty, OptionalTypeKind optKind,
    ModuleDecl *moduleContext, OutputLanguageMode outputLang) {
  CFunctionSignatureTypePrinter typePrinter(
      stream, cPrologueOS, typeMapping, outputLang, interopContext,
      CFunctionSignatureTypePrinterModifierDelegate(), moduleContext,
      declPrinter, FunctionSignatureTypeUse::ReturnType);
  // Param for indirect return cannot be marked as inout
  return typePrinter.visit(ty, optKind, /*isInOutParam=*/false);
}

static void addABIRecordToTypeEncoding(llvm::raw_ostream &typeEncodingOS,
                                       clang::CharUnits offset,
                                       clang::CharUnits end, Type t,
                                       PrimitiveTypeMapping &typeMapping) {
  auto info =
      typeMapping.getKnownCTypeInfo(t->getNominalOrBoundGenericNominal());
  assert(info);
  typeEncodingOS << '_';
  for (char c : info->name) {
    if (c == ' ')
      typeEncodingOS << '_';
    else if (c == '*')
      typeEncodingOS << "ptr";
    else
      typeEncodingOS << c;
  }
  // Express the offset and end in terms of target word size.
  // This ensures that tests are able to use the stub struct name in target
  // independent manner.
  auto emitUnit = [&](const clang::CharUnits &unit) {
    typeEncodingOS << '_' << unit.getQuantity();
  };
  emitUnit(offset);
  emitUnit(end);
}

template <class T>
static std::string encodeTypeInfo(const T &abiTypeInfo,
                                  const ModuleDecl *moduleContext,
                                  PrimitiveTypeMapping &typeMapping) {
  std::string typeEncoding;
  llvm::raw_string_ostream typeEncodingOS(typeEncoding);

  ClangSyntaxPrinter(typeEncodingOS).printBaseName(moduleContext);
  abiTypeInfo.enumerateRecordMembers(
      [&](clang::CharUnits offset, clang::CharUnits end, Type t) {
        addABIRecordToTypeEncoding(typeEncodingOS, offset, end, t, typeMapping);
      });
  return std::move(typeEncodingOS.str());
}

// Returns false if the given direct type is not yet supported because
// of its ABI.
template <class T>
static bool printDirectReturnOrParamCType(
    const T &abiTypeInfo, Type valueType, const ModuleDecl *emittedModule,
    raw_ostream &os, raw_ostream &cPrologueOS,
    PrimitiveTypeMapping &typeMapping,
    SwiftToClangInteropContext &interopContext,
    llvm::function_ref<void()> prettifiedValuePrinter) {
  const bool isResultType =
      std::is_same<T, LoweredFunctionSignature::DirectResultType>::value;
  StringRef stubTypeName =
      isResultType ? "swift_interop_returnStub_" : "swift_interop_passStub_";

  std::string typeEncoding;
  llvm::raw_string_ostream typeEncodingOS(typeEncoding);
  typeEncodingOS << stubTypeName;
  ClangSyntaxPrinter(typeEncodingOS).printBaseName(emittedModule);

  unsigned Count = 0;
  clang::CharUnits lastOffset;
  if (abiTypeInfo.enumerateRecordMembers([&](clang::CharUnits offset,
                                             clang::CharUnits end, Type t) {
        lastOffset = offset;
        ++Count;
        addABIRecordToTypeEncoding(typeEncodingOS, offset, end, t, typeMapping);
      }))
    return false;
  if (isResultType && Count == 0) {
    // A direct result with no record members can happen for uninhabited result
    // types like `Never`.
    os << "void";
    return true;
  }
  assert(Count > 0 && "missing return values");

  // FIXME: is this "prettyfying" logic sound for multiple return values?
  if (isKnownCType(valueType, typeMapping) ||
      (Count == 1 && lastOffset.isZero() && !valueType->hasTypeParameter() &&
       valueType->isAnyClassReferenceType())) {
    prettifiedValuePrinter();
    return true;
  }

  os << "struct " << typeEncodingOS.str();
  llvm::SmallVector<std::pair<clang::CharUnits, clang::CharUnits>, 8> fields;
  auto printStub = [&](raw_ostream &os, StringRef stubName) {
    // Print out a C stub for this value type.
    os << "// Stub struct to be used to pass/return values to/from Swift "
          "functions.\n";
    os << "struct " << stubName << " {\n";
    abiTypeInfo.enumerateRecordMembers([&](clang::CharUnits offset,
                                           clang::CharUnits end, Type t) {
      auto info =
          typeMapping.getKnownCTypeInfo(t->getNominalOrBoundGenericNominal());
      os << "  " << info->name;
      if (info->canBeNullable)
        os << " _Nullable";
      os << " _" << (fields.size() + 1) << ";\n";
      fields.push_back(std::make_pair(offset, end));
    });
    os << "};\n\n";
    auto minimalStubName = stubName;
    minimalStubName.consume_front(stubTypeName);
    if (isResultType) {
      // Emit a stub that returns a value directly from swiftcc function.
      os << "static SWIFT_C_INLINE_THUNK void swift_interop_returnDirect_"
         << minimalStubName;
      os << "(char * _Nonnull result, struct " << stubName << " value";
      os << ") {\n";
      for (size_t i = 0; i < fields.size(); ++i) {
        os << "  memcpy(result + " << fields[i].first.getQuantity() << ", "
           << "&value._" << (i + 1) << ", "
           << (fields[i].second - fields[i].first).getQuantity() << ");\n";
      }
    } else {
      // Emit a stub that is used to pass value type directly to swiftcc
      // function.
      os << "static SWIFT_C_INLINE_THUNK struct " << stubName
         << " swift_interop_passDirect_" << minimalStubName;
      os << "(const char * _Nonnull value) {\n";
      os << "  struct " << stubName << " result;\n";
      for (size_t i = 0; i < fields.size(); ++i) {
        os << "  memcpy(&result._" << (i + 1) << ", value + "
           << fields[i].first.getQuantity() << ", "
           << (fields[i].second - fields[i].first).getQuantity() << ");\n";
      }
      os << "  return result;\n";
    }
    os << "}\n\n";
  };

  interopContext.runIfStubForDeclNotEmitted(typeEncodingOS.str(), [&]() {
    printStub(cPrologueOS, typeEncodingOS.str());
  });
  return true;
}

/// Make adjustments to the Swift parameter name in generated C++, to
/// avoid things like additional warnings.
static void renameCxxParameterIfNeeded(const AbstractFunctionDecl *FD,
                                       std::string &paramName) {
  if (paramName.empty())
    return;
  const auto *enumDecl = FD->getDeclContext()->getSelfEnumDecl();
  if (!enumDecl)
    return;
  // Rename a parameter in an enum method that shadows an existing case name,
  // to avoid a -Wshadow warning in Clang.
  for (const auto *Case : enumDecl->getAllElements()) {
    if (Case->getNameStr() == paramName) {
      paramName = (llvm::Twine(paramName) + "_").str();
      return;
    }
  }
}

ClangRepresentation DeclAndTypeClangFunctionPrinter::printFunctionSignature(
    const AbstractFunctionDecl *FD, const LoweredFunctionSignature &signature,
    StringRef name, Type resultTy, FunctionSignatureKind kind,
    FunctionSignatureModifiers modifiers) {
  std::string functionSignature;
  llvm::raw_string_ostream functionSignatureOS(functionSignature);
  // Print any template and requires clauses for the
  // C++ class context to which this C++ member will belong to.
  if (const auto *typeDecl = modifiers.qualifierContext) {
    assert(kind == FunctionSignatureKind::CxxInlineThunk);
    ClangSyntaxPrinter(functionSignatureOS)
        .printNominalTypeOutsideMemberDeclTemplateSpecifiers(typeDecl);
  }
  if (FD->isGeneric()) {
    auto Signature = FD->getGenericSignature().getCanonicalSignature();
    if (!cxx_translation::isExposableToCxx(Signature))
      return ClangRepresentation::unsupported;

    // Print the template and requires clauses for this function.
    if (kind == FunctionSignatureKind::CxxInlineThunk)
      ClangSyntaxPrinter(functionSignatureOS).printGenericSignature(Signature);
  }
  if (const auto *enumDecl = FD->getDeclContext()->getSelfEnumDecl()) {
    // We cannot emit functions with the same name as an enum case yet, the resulting header
    // does not compiler.
    // FIXME: either do not emit cases as inline members, or rename the cases or the
    //        colliding functions.
    for (const auto *enumElement : enumDecl->getAllElements()) {
      auto elementName = enumElement->getName();
      if (!elementName.isSpecial() && elementName.getBaseIdentifier().is(name))
        return ClangRepresentation::unsupported;
    }
  }
  auto emittedModule = FD->getModuleContext();
  OutputLanguageMode outputLang = kind == FunctionSignatureKind::CFunctionProto
                                      ? OutputLanguageMode::ObjC
                                      : OutputLanguageMode::Cxx;
  // FIXME: Might need a PrintMultiPartType here.
  auto print =
      [&, this](Type ty, std::optional<OptionalTypeKind> optionalKind,
                StringRef name, bool isInOutParam,
                CFunctionSignatureTypePrinterModifierDelegate delegate = {})
      -> ClangRepresentation {
    // FIXME: add support for noescape and PrintMultiPartType,
    // see DeclAndTypePrinter::print.
    CFunctionSignatureTypePrinter typePrinter(
        functionSignatureOS, cPrologueOS, typeMapping, outputLang,
        interopContext, delegate, emittedModule, declPrinter);
    auto result = typePrinter.visit(ty, optionalKind, isInOutParam);

    if (!name.empty()) {
      functionSignatureOS << ' ';
      ClangSyntaxPrinter(functionSignatureOS).printIdentifier(name);
    }
    return result;
  };

  // Print any modifiers before the signature.
  if (modifiers.isStatic) {
    assert(!modifiers.isConst);
    functionSignatureOS << "static ";
  }
  if (modifiers.isInline)
    ClangSyntaxPrinter(functionSignatureOS).printInlineForThunk();

  ClangRepresentation resultingRepresentation =
      ClangRepresentation::representable;

  // Print out the return type.
  if (FD->hasThrows() && outputLang == OutputLanguageMode::Cxx)
    functionSignatureOS << "swift::ThrowingResult<";
  if (kind == FunctionSignatureKind::CFunctionProto) {
    // First, verify that the C++ return type is representable.
    {
      OptionalTypeKind optKind;
      Type objTy;
      std::tie(objTy, optKind) =
          DeclAndTypePrinter::getObjectTypeAndOptionality(FD, resultTy);
      CFunctionSignatureTypePrinter typePrinter(
          llvm::nulls(), llvm::nulls(), typeMapping, OutputLanguageMode::Cxx,
          interopContext, CFunctionSignatureTypePrinterModifierDelegate(),
          emittedModule, declPrinter, FunctionSignatureTypeUse::ReturnType);
      if (resultingRepresentation
              .merge(typePrinter.visit(objTy, optKind, /*isInOutParam=*/false))
              .isUnsupported())
        return resultingRepresentation;
    }

    auto directResultType = signature.getDirectResultType();
    // FIXME: support direct + indirect results.
    if (directResultType && signature.getNumIndirectResultValues() > 0)
      return ClangRepresentation::unsupported;
    // FIXME: support multiple indirect results.
    if (signature.getNumIndirectResultValues() > 1)
      return ClangRepresentation::unsupported;

    if (!directResultType) {
      functionSignatureOS << "void";
    } else {
      if (!printDirectReturnOrParamCType(
              *directResultType, resultTy, emittedModule, functionSignatureOS,
              cPrologueOS, typeMapping, interopContext, [&]() {
                OptionalTypeKind retKind;
                Type objTy;
                std::tie(objTy, retKind) =
                    DeclAndTypePrinter::getObjectTypeAndOptionality(FD,
                                                                    resultTy);

                auto s = printClangFunctionReturnType(
                    functionSignatureOS, objTy, retKind, emittedModule,
                    outputLang);
                assert(!s.isUnsupported());
              }))
        return ClangRepresentation::unsupported;
    }
  } else {
    OptionalTypeKind retKind;
    Type objTy;
    std::tie(objTy, retKind) =
        DeclAndTypePrinter::getObjectTypeAndOptionality(FD, resultTy);
    if (resultingRepresentation
            .merge(printClangFunctionReturnType(
                functionSignatureOS, objTy, retKind, emittedModule, outputLang))
            .isUnsupported())
      return resultingRepresentation;
  }
  if (FD->hasThrows() && outputLang == OutputLanguageMode::Cxx)
    functionSignatureOS << ">";
  functionSignatureOS << ' ';
  if (const auto *typeDecl = modifiers.qualifierContext)
    ClangSyntaxPrinter(functionSignatureOS)
        .printNominalTypeQualifier(typeDecl, typeDecl->getModuleContext());
  ClangSyntaxPrinter(functionSignatureOS).printIdentifier(name);
  functionSignatureOS << '(';

  bool HasParams = false;

  if (kind == FunctionSignatureKind::CFunctionProto) {
    // First, verify that the C++ param types are representable.
    for (auto param : *FD->getParameters()) {
      OptionalTypeKind optKind;
      Type objTy;
      std::tie(objTy, optKind) =
          DeclAndTypePrinter::getObjectTypeAndOptionality(
              FD, param->getInterfaceType());
      CFunctionSignatureTypePrinter typePrinter(
          llvm::nulls(), llvm::nulls(), typeMapping, OutputLanguageMode::Cxx,
          interopContext, CFunctionSignatureTypePrinterModifierDelegate(),
          emittedModule, declPrinter, FunctionSignatureTypeUse::ParamType);
      if (resultingRepresentation
              .merge(typePrinter.visit(objTy, optKind,
                                       /*isInOutParam=*/param->isInOut()))
              .isUnsupported())
        return resultingRepresentation;
    }

    bool needsComma = false;
    auto emitNewParam = [&]() {
      if (needsComma)
        functionSignatureOS << ", ";
      needsComma = true;
    };
    auto printParamName = [&](const ParamDecl &param) {
      std::string paramName =
          param.getName().empty() ? "" : param.getName().str().str();
      if (param.isSelfParameter())
        paramName = "_self";
      if (!paramName.empty()) {
        functionSignatureOS << ' ';
        ClangSyntaxPrinter(functionSignatureOS).printIdentifier(paramName);
      }
    };
    auto printParamCType = [&](const ParamDecl &param) {
      OptionalTypeKind optionalKind;
      Type ty;
      std::tie(ty, optionalKind) =
          DeclAndTypePrinter::getObjectTypeAndOptionality(
              &param, param.getInterfaceType());
      CFunctionSignatureTypePrinter typePrinter(
          functionSignatureOS, cPrologueOS, typeMapping, outputLang,
          interopContext, CFunctionSignatureTypePrinterModifierDelegate(),
          emittedModule, declPrinter);
      auto s = typePrinter.visit(ty, optionalKind, param.isInOut());
      assert(!s.isUnsupported());
    };
    signature.visitParameterList(
        [&](const LoweredFunctionSignature::IndirectResultValue
                &indirectResult) {
          emitNewParam();
          if (indirectResult.hasSRet())
            functionSignatureOS << "SWIFT_INDIRECT_RESULT ";
          // FIXME: it would be nice to print out the C struct type here.
          functionSignatureOS << "void * _Nonnull";
        },
        [&](const LoweredFunctionSignature::DirectParameter &param) {
          emitNewParam();
          printDirectReturnOrParamCType(
              param, param.getParamDecl().getInterfaceType(), emittedModule,
              functionSignatureOS, cPrologueOS, typeMapping, interopContext,
              [&]() { printParamCType(param.getParamDecl()); });
          printParamName(param.getParamDecl());
        },
        [&](const LoweredFunctionSignature::IndirectParameter &param) {
          emitNewParam();
          if (param.getParamDecl().isSelfParameter())
            functionSignatureOS << "SWIFT_CONTEXT ";
          bool isConst =
              !param.getParamDecl().isInOut() &&
              !(param.getParamDecl().isSelfParameter() &&
                !param.getParamDecl().getInterfaceType()->hasTypeParameter() &&
                param.getParamDecl()
                    .getInterfaceType()
                    ->isAnyClassReferenceType());
          if (isConst)
            functionSignatureOS << "const ";
          if (isKnownCType(param.getParamDecl().getInterfaceType(),
                           typeMapping) ||
              (!param.getParamDecl().getInterfaceType()->hasTypeParameter() &&
               param.getParamDecl()
                   .getInterfaceType()
                   ->isAnyClassReferenceType()))
            printParamCType(param.getParamDecl());
          else
            functionSignatureOS << "void * _Nonnull";
          printParamName(param.getParamDecl());
        },
        [&](const LoweredFunctionSignature::GenericRequirementParameter
                &genericRequirementParam) {
          emitNewParam();
          functionSignatureOS << "void * _Nonnull ";
          auto reqt = genericRequirementParam.getRequirement();
          if (reqt.isAnyWitnessTable())
            ClangSyntaxPrinter(functionSignatureOS)
                .printBaseName(reqt.getProtocol());
          else
            assert(reqt.isAnyMetadata());
        },
        [&](const LoweredFunctionSignature::MetadataSourceParameter
                &metadataSrcParam) {
          emitNewParam();
          functionSignatureOS << "void * _Nonnull ";
        },
        [&](const LoweredFunctionSignature::ContextParameter &) {
          emitNewParam();
          functionSignatureOS << "SWIFT_CONTEXT void * _Nonnull _ctx";
        },
        [&](const LoweredFunctionSignature::ErrorResultValue &) {
          emitNewParam();
          functionSignatureOS
              << "SWIFT_ERROR_RESULT void * _Nullable * _Nullable _error";
        });
    if (needsComma == false)
      // Emit 'void' in an empty parameter list for C function declarations.
      functionSignatureOS << "void";
    functionSignatureOS << ')';
    if (!resultingRepresentation.isUnsupported())
      os << functionSignatureOS.str();
    return resultingRepresentation;
  }

  // Print out the C++ parameter types.
  auto params = FD->getParameters();
  if (params->size()) {
    if (HasParams)
      functionSignatureOS << ", ";
    HasParams = true;
    size_t paramIndex = 1;
    llvm::interleaveComma(
        *params, functionSignatureOS, [&](const ParamDecl *param) {
          OptionalTypeKind argKind;
          Type objTy;
          std::tie(objTy, argKind) =
              DeclAndTypePrinter::getObjectTypeAndOptionality(
                  param, param->getInterfaceType());
          std::string paramName =
              param->getName().empty() ? "" : param->getName().str().str();
          renameCxxParameterIfNeeded(FD, paramName);
          // Always emit a named parameter for the C++ inline thunk to ensure it
          // can be referenced in the body.
          if (kind == FunctionSignatureKind::CxxInlineThunk &&
              paramName.empty()) {
            llvm::raw_string_ostream os(paramName);
            os << "_" << paramIndex;
          }
          resultingRepresentation.merge(
              print(objTy, argKind, paramName, param->isInOut()));
          ++paramIndex;
        });
    if (resultingRepresentation.isUnsupported()) {
      return resultingRepresentation;
    }
  }
  functionSignatureOS << ')';
  if (modifiers.isConst)
    functionSignatureOS << " const";
  if (modifiers.isNoexcept)
    functionSignatureOS << " noexcept";
  if (modifiers.hasSymbolUSR)
    ClangSyntaxPrinter(functionSignatureOS)
        .printSymbolUSRAttribute(
            modifiers.symbolUSROverride ? modifiers.symbolUSROverride : FD);
  if (!resultingRepresentation.isUnsupported())
    os << functionSignatureOS.str();
  return resultingRepresentation;
}

void DeclAndTypeClangFunctionPrinter::printTypeImplTypeSpecifier(
    Type type, const ModuleDecl *moduleContext) {
  CFunctionSignatureTypePrinterModifierDelegate delegate;
  delegate.mapValueTypeUseKind = [](ClangValueTypePrinter::TypeUseKind kind) {
    return ClangValueTypePrinter::TypeUseKind::CxxImplTypeName;
  };
  CFunctionSignatureTypePrinter typePrinter(
      os, cPrologueOS, typeMapping, OutputLanguageMode::Cxx, interopContext,
      delegate, moduleContext, declPrinter,
      FunctionSignatureTypeUse::TypeReference);
  auto result = typePrinter.visit(type, std::nullopt, /*isInOut=*/false);
  assert(!result.isUnsupported());
}

void DeclAndTypeClangFunctionPrinter::printCxxToCFunctionParameterUse(
    Type type, StringRef name, const ModuleDecl *moduleContext, bool isInOut,
    bool isIndirect, std::string directTypeEncoding, bool forceSelf) {
  auto namePrinter = [&]() { ClangSyntaxPrinter(os).printIdentifier(name); };
  if (!isKnownCxxType(type, typeMapping) &&
      !hasKnownOptionalNullableCxxMapping(type)) {
    if (type->is<GenericTypeParamType>()) {
      os << "swift::" << cxx_synthesis::getCxxImplNamespaceName()
         << "::getOpaquePointer(";
      namePrinter();
      os << ')';
      return;
    }

    if (auto *classDecl = type->getClassOrBoundGenericClass()) {
      if (classDecl->hasClangNode()) {
        if (isInOut)
          os << '&';
        namePrinter();
        return;
      }
      ClangClassTypePrinter::printParameterCxxtoCUseScaffold(
          os, classDecl, moduleContext, namePrinter, isInOut);
      return;
    }

    if (auto *decl = type->getNominalOrBoundGenericNominal()) {
      if ((isa<StructDecl>(decl) || isa<EnumDecl>(decl))) {
        if (!directTypeEncoding.empty())
          os << cxx_synthesis::getCxxImplNamespaceName()
             << "::swift_interop_passDirect_" << directTypeEncoding << '(';
        if (decl->hasClangNode()) {
            if (!directTypeEncoding.empty())
                os << "reinterpret_cast<const char *>(";
            os << "swift::" << cxx_synthesis::getCxxImplNamespaceName()
               << "::getOpaquePointer(";
            namePrinter();
            os << ')';
            if (!directTypeEncoding.empty())
                os << ')';
        } else {
            ClangValueTypePrinter(os, cPrologueOS, interopContext)
                .printParameterCxxToCUseScaffold(
                    moduleContext,
                    [&]() { printTypeImplTypeSpecifier(type, moduleContext); },
                    namePrinter, forceSelf);
        }
        if (!directTypeEncoding.empty())
          os << ')';
        return;
      }
    }
  }
  // Primitive types are passed directly without any conversions.
  if (isInOut) {
    os << "&";
  }
  namePrinter();
}

void DeclAndTypeClangFunctionPrinter::printGenericReturnSequence(
    raw_ostream &os, const GenericTypeParamType *gtpt,
    llvm::function_ref<void(StringRef)> invocationPrinter,
    std::optional<StringRef> initializeWithTakeFromValue) {
  std::string returnAddress;
  llvm::raw_string_ostream ros(returnAddress);
  ros << "reinterpret_cast<void *>(&returnValue)";
  std::string resultTyName;
  {
    llvm::raw_string_ostream os(resultTyName);
    ClangSyntaxPrinter(os).printGenericTypeParamTypeName(gtpt);
  }
  ClangSyntaxPrinter(os).printIgnoredCxx17ExtensionDiagnosticBlock([&]() {
    os << "  if constexpr (std::is_base_of<::swift::"
       << cxx_synthesis::getCxxImplNamespaceName() << "::RefCountedClass, "
       << resultTyName << ">::value) {\n";
    os << "  void *returnValue;\n  ";
    if (!initializeWithTakeFromValue) {
      invocationPrinter(/*additionalParam=*/StringRef(ros.str()));
    } else {
      os << "returnValue = *reinterpret_cast<void **>("
         << *initializeWithTakeFromValue << ")";
    }
    os << ";\n";
    os << "  return ::swift::" << cxx_synthesis::getCxxImplNamespaceName()
       << "::implClassFor<" << resultTyName
       << ">::type::makeRetained(returnValue);\n";
    os << "  } else if constexpr (::swift::"
       << cxx_synthesis::getCxxImplNamespaceName() << "::isValueType<"
       << resultTyName << ">) {\n";

    os << "  return ::swift::" << cxx_synthesis::getCxxImplNamespaceName()
       << "::implClassFor<" << resultTyName
       << ">::type::returnNewValue([&](void * _Nonnull returnValue) "
          "SWIFT_INLINE_THUNK_ATTRIBUTES {\n";
    if (!initializeWithTakeFromValue) {
      invocationPrinter(/*additionalParam=*/StringRef("returnValue"));
    } else {
      os << "  return ::swift::" << cxx_synthesis::getCxxImplNamespaceName()
         << "::implClassFor<" << resultTyName
         << ">::type::initializeWithTake(reinterpret_cast<char * "
            "_Nonnull>(returnValue), "
         << *initializeWithTakeFromValue << ")";
    }
    os << ";\n  });\n";
    os << "  } else if constexpr (::swift::"
       << cxx_synthesis::getCxxImplNamespaceName()
       << "::isSwiftBridgedCxxRecord<" << resultTyName << ">) {\n";
    if (!initializeWithTakeFromValue) {
      ClangTypeHandler::printGenericReturnScaffold(os, resultTyName,
                                                   invocationPrinter);
    } else {
      // FIXME: support taking a C++ record type.
      os << "abort();\n";
    }
    os << "  } else {\n";
    os << "  " << resultTyName << " returnValue;\n";
    if (!initializeWithTakeFromValue) {
      invocationPrinter(/*additionalParam=*/StringRef(ros.str()));
    } else {
      os << "memcpy(&returnValue, " << *initializeWithTakeFromValue
         << ", sizeof(returnValue))";
    }
    os << ";\n  return returnValue;\n";
    os << "  }\n";
  });
}

void DeclAndTypeClangFunctionPrinter::printCxxThunkBody(
    const AbstractFunctionDecl *FD, const LoweredFunctionSignature &signature,
    StringRef swiftSymbolName, const NominalTypeDecl *typeDeclContext,
    const ModuleDecl *moduleContext, Type resultTy, const ParameterList *params,
    bool hasThrows, const AnyFunctionType *funcType, bool isStaticMethod,
    std::optional<IRABIDetailsProvider::MethodDispatchInfo> dispatchInfo) {
  if (typeDeclContext)
    ClangSyntaxPrinter(os).printNominalTypeOutsideMemberDeclInnerStaticAssert(
        typeDeclContext);
  if (FD->isGeneric()) {
    auto Signature = FD->getGenericSignature().getCanonicalSignature();
    ClangSyntaxPrinter(os).printGenericSignatureInnerStaticAsserts(Signature);
  }
  if (hasThrows) {
    os << "  void* opaqueError = nullptr;\n";
    os << "  void* _ctx = nullptr;\n";
  }
  std::optional<StringRef> indirectFunctionVar;
  using DispatchKindTy = IRABIDetailsProvider::MethodDispatchInfo::Kind;
  if (dispatchInfo && !isStaticMethod) {
    // Always dispatch class methods directly to the concrete class instance.
    switch (dispatchInfo->getKind()) {
    case DispatchKindTy::Direct:
      break;
    case DispatchKindTy::IndirectVTableStaticOffset:
    case DispatchKindTy::IndirectVTableRelativeOffset:
      os << "void ***selfPtr_ = reinterpret_cast<void ***>( "
            "::swift::_impl::_impl_RefCountedClass::getOpaquePointer(*this));"
            "\n";

      os << "#ifdef __arm64e__\n";
      os << "void **vtable_ = ptrauth_auth_data(*selfPtr_, "
            "ptrauth_key_process_independent_data, "
            "ptrauth_blend_discriminator(selfPtr_,"
         << SpecialPointerAuthDiscriminators::ObjCISA << "));\n";
      os << "#else\n";
      os << "void **vtable_ = *selfPtr_;\n";
      os << "#endif\n";
      os << "struct FTypeAddress {\n";
      os << "decltype(" << cxx_synthesis::getCxxImplNamespaceName()
         << "::" << swiftSymbolName << ") *";
      if (auto ptrAuthDisc = dispatchInfo->getPointerAuthDiscriminator())
        os << " __ptrauth_swift_class_method_pointer(" << ptrAuthDisc->value
           << ')';
      os << " func;\n";
      os << "};\n";
      os << "FTypeAddress *fptrptr_ = reinterpret_cast<FTypeAddress *>(vtable_ "
            "+ ";
      if (dispatchInfo->getKind() == DispatchKindTy::IndirectVTableStaticOffset)
        os << dispatchInfo->getStaticOffset();
      else
        os << '(' << cxx_synthesis::getCxxImplNamespaceName()
           << "::" << dispatchInfo->getBaseOffsetSymbolName() << " + "
           << dispatchInfo->getRelativeOffset() << ')';
      os << " / sizeof(void *));\n";
      indirectFunctionVar = StringRef("fptrptr_->func");
      break;
    case DispatchKindTy::Thunk:
      swiftSymbolName = dispatchInfo->getThunkSymbolName();
      break;
    }
  }

  auto getParamName = [&](const ParamDecl &param, size_t paramIndex,
                          bool isConsumed) {
    std::string paramName;
    if (isConsumed)
      paramName = "consumedParamCopy_";
    if (param.isSelfParameter()) {
      if (isConsumed)
        paramName += "this";
      else
        paramName = "*this";
    } else if (param.getName().empty()) {
      llvm::raw_string_ostream paramOS(paramName);
      if (!isConsumed)
        paramOS << "_";
      paramOS << paramIndex;
    } else {
      StringRef nameStr = param.getName().str();
      if (isConsumed)
        paramName += nameStr.str();
      else
        paramName = nameStr;
      renameCxxParameterIfNeeded(FD, paramName);
    }
    return paramName;
  };

  // Check if we need to copy any parameters that are consumed by Swift,
  // to ensure that Swift does not destroy the value that's owned by C++.
  // FIXME: Support non-copyable types here as well between C++ -> Swift.
  // FIXME: class types can be optimized down to an additional retain right
  // here.
  size_t paramIndex = 1;
  auto emitParamCopyForConsume = [&](const ParamDecl &param) {
    auto name = getParamName(param, paramIndex, /*isConsumed=*/false);
    auto consumedName = getParamName(param, paramIndex, /*isConsumed=*/true);
    std::string paramType;

    llvm::raw_string_ostream typeOS(paramType);

    CFunctionSignatureTypePrinter typePrinter(
        typeOS, cPrologueOS, typeMapping, OutputLanguageMode::Cxx,
        interopContext, CFunctionSignatureTypePrinterModifierDelegate(),
        moduleContext, declPrinter, FunctionSignatureTypeUse::TypeReference);
    auto result = typePrinter.visit(param.getInterfaceType(), OTK_None,
                                    /*isInOutParam=*/false);
    assert(!result.isUnsupported());
    typeOS.flush();

    os << "  alignas(alignof(" << paramType << ")) char copyBuffer_"
       << consumedName << "[sizeof(" << paramType << ")];\n";
    os << "  auto &" << consumedName << " = *(new(copyBuffer_" << consumedName
       << ") " << paramType << "(" << name << "));\n";
    os << "  swift::" << cxx_synthesis::getCxxImplNamespaceName()
       << "::ConsumedValueStorageDestroyer<" << paramType << "> storageGuard_"
       << consumedName << "(" << consumedName << ");\n";
  };
  signature.visitParameterList(
      [&](const LoweredFunctionSignature::IndirectResultValue &) {},
      [&](const LoweredFunctionSignature::DirectParameter &param) {
        if (isConsumedParameterInCaller(param.getConvention()))
          emitParamCopyForConsume(param.getParamDecl());
        ++paramIndex;
      },
      [&](const LoweredFunctionSignature::IndirectParameter &param) {
        if (isConsumedParameterInCaller(param.getConvention()))
          emitParamCopyForConsume(param.getParamDecl());
        ++paramIndex;
      },
      [&](const LoweredFunctionSignature::GenericRequirementParameter
              &genericRequirementParam) {},
      [&](const LoweredFunctionSignature::MetadataSourceParameter
              &metadataSrcParam) {},
      [&](const LoweredFunctionSignature::ContextParameter &) {},
      [&](const LoweredFunctionSignature::ErrorResultValue &) {});

  auto printCallToCFunc = [&](std::optional<StringRef> additionalParam) {
    if (indirectFunctionVar)
      os << "(* " << *indirectFunctionVar << ')';
    else
      os << cxx_synthesis::getCxxImplNamespaceName() << "::" << swiftSymbolName;
    os << '(';

    bool needsComma = false;
    size_t paramIndex = 1;
    auto emitNewParam = [&]() {
      if (needsComma)
        os << ", ";
      needsComma = true;
    };
    auto printParamUse = [&](const ParamDecl &param, bool isIndirect,
                             bool isConsumed,

                             std::string directTypeEncoding) {
      emitNewParam();
      if (param.isSelfParameter()) {
        bool needsStaticSelf = isa<ConstructorDecl>(FD) || isStaticMethod;
        if (needsStaticSelf) {
          // Static self value is just the type's metadata value.
          os << "swift::TypeMetadataTrait<";
          CFunctionSignatureTypePrinter typePrinter(
              os, cPrologueOS, typeMapping, OutputLanguageMode::Cxx,
              interopContext, CFunctionSignatureTypePrinterModifierDelegate(),
              moduleContext, declPrinter,
              FunctionSignatureTypeUse::TypeReference);
          auto result = typePrinter.visit(param.getInterfaceType(), OTK_None,
                                          /*isInOutParam=*/false);
          assert(!result.isUnsupported());
          os << ">::getTypeMetadata()";
          return;
        }
      }
      auto paramName = getParamName(param, paramIndex, isConsumed);
      ++paramIndex;
      printCxxToCFunctionParameterUse(param.getInterfaceType(), paramName,
                                      param.getModuleContext(), param.isInOut(),
                                      isIndirect, directTypeEncoding,
                                      !isConsumed && param.isSelfParameter());
    };

    signature.visitParameterList(
        [&](const LoweredFunctionSignature::IndirectResultValue &) {
          emitNewParam();
          assert(additionalParam);
          os << *additionalParam;
          additionalParam = std::nullopt;
        },
        [&](const LoweredFunctionSignature::DirectParameter &param) {
          printParamUse(param.getParamDecl(), /*isIndirect=*/false,
                        isConsumedParameterInCaller(param.getConvention()),
                        encodeTypeInfo(param, moduleContext, typeMapping));
        },
        [&](const LoweredFunctionSignature::IndirectParameter &param) {
          printParamUse(param.getParamDecl(), /*isIndirect=*/true,
                        isConsumedParameterInCaller(param.getConvention()),
                        /*directTypeEncoding=*/"");
        },
        [&](const LoweredFunctionSignature::GenericRequirementParameter
                &genericRequirementParam) {
          emitNewParam();
          auto genericRequirement = genericRequirementParam.getRequirement();
          // FIXME: Add protocol requirement support.
          assert(genericRequirement.isAnyMetadata());
          if (auto *gtpt = genericRequirement.getTypeParameter()
                               ->getAs<GenericTypeParamType>()) {
            os << "swift::TypeMetadataTrait<";
            ClangSyntaxPrinter(os).printGenericTypeParamTypeName(gtpt);
            os << ">::getTypeMetadata()";
            return;
          }
          os << "ERROR";
        },
        [&](const LoweredFunctionSignature::MetadataSourceParameter
                &metadataSrcParam) {
          emitNewParam();
          os << "swift::TypeMetadataTrait<";
          CFunctionSignatureTypePrinterModifierDelegate delegate;
          CFunctionSignatureTypePrinter typePrinter(
              os, cPrologueOS, typeMapping, OutputLanguageMode::Cxx,
              interopContext, delegate, moduleContext, declPrinter,
              FunctionSignatureTypeUse::TypeReference);
          auto result =
              typePrinter.visit(metadataSrcParam.getType(), std::nullopt,
                                /*isInOut=*/false);
          assert(!result.isUnsupported());
          os << ">::getTypeMetadata()";
        },
        [&](const LoweredFunctionSignature::ContextParameter &) {
          emitNewParam();
          os << "_ctx";
        },
        [&](const LoweredFunctionSignature::ErrorResultValue &) {
          emitNewParam();
          os << "&opaqueError";
        });
    os << ')';
  };

  // Values types are returned either direcly in their C representation, or
  // indirectly by a pointer.
  if (!isKnownCxxType(resultTy, typeMapping) &&
      !hasKnownOptionalNullableCxxMapping(resultTy)) {
    if (const auto *gtpt = resultTy->getAs<GenericTypeParamType>()) {
      printGenericReturnSequence(os, gtpt, printCallToCFunc);
      return;
    }
    if (auto *classDecl = resultTy->getClassOrBoundGenericClass()) {
      if (classDecl->hasClangNode()) {
        assert(!isa<clang::ObjCContainerDecl>(classDecl->getClangDecl()));
        os << "return ";
        printCallToCFunc(/*additionalParam=*/std::nullopt);
        os << ";\n";
        return;
      }
      ClangClassTypePrinter::printClassTypeReturnScaffold(
          os, classDecl, moduleContext,
          [&]() { printCallToCFunc(/*additionalParam=*/std::nullopt); });
      return;
    }
    if (auto *decl = resultTy->getNominalOrBoundGenericNominal()) {
      auto valueTypeReturnThunker = [&](StringRef resultPointerName) {
        if (auto directResultType = signature.getDirectResultType()) {
          std::string typeEncoding =
              encodeTypeInfo(*directResultType, moduleContext, typeMapping);
          os << cxx_synthesis::getCxxImplNamespaceName()
             << "::swift_interop_returnDirect_" << typeEncoding << '('
             << resultPointerName << ", ";
          printCallToCFunc(std::nullopt);
          os << ')';
        } else {
          printCallToCFunc(/*firstParam=*/resultPointerName);
        }
      };
      if (decl->hasClangNode()) {
        ClangTypeHandler handler(decl->getClangDecl());
        assert(handler.isRepresentable());
        handler.printReturnScaffold(os, valueTypeReturnThunker);
        return;
      }
      ClangValueTypePrinter valueTypePrinter(os, cPrologueOS, interopContext);

      valueTypePrinter.printValueTypeReturnScaffold(
          decl, moduleContext,
          [&]() { printTypeImplTypeSpecifier(resultTy, moduleContext); },
          valueTypeReturnThunker);
      return;
    }
  }

  auto nonOptResultType = resultTy->getOptionalObjectType();
  if (!nonOptResultType)
    nonOptResultType = resultTy;
  if (auto *classDecl = nonOptResultType->getClassOrBoundGenericClass()) {
    assert(classDecl->hasClangNode());
    assert(isa<clang::ObjCContainerDecl>(classDecl->getClangDecl()));
    os << "return (__bridge_transfer ";
    ClangSyntaxPrinter(os).printIdentifier(
        cast<clang::NamedDecl>(classDecl->getClangDecl())->getName());
    os << " *)(__bridge void *)";
    printCallToCFunc(/*additionalParam=*/std::nullopt);
    os << ";\n";
    return;
  }
  // Primitive values are returned directly without any conversions.
  // Assign the function return value to a variable if the function can throw.
  if (!resultTy->isVoid() && hasThrows)
    os << "  auto returnValue = ";
  // If the function doesn't have a return value just call it.
  else if (resultTy->isVoid())
    os << "  ";
  // If the function can't throw just return its value result.
  else if (!hasThrows)
    os << "  return ";
  printCallToCFunc(/*additionalParam=*/std::nullopt);
  os << ";\n";

  // Create the condition and the statement to throw an exception.
  if (hasThrows) {
    os << "  if (opaqueError != nullptr)\n";
    os << "#ifdef __cpp_exceptions\n";
    os << "    throw (swift::Error(opaqueError));\n";
    os << "#else\n";
    if (resultTy->isVoid()) {
      os << "    return swift::Expected<void>(swift::Error(opaqueError));\n";
      os << "#endif\n";
      if (FD->getInterfaceType()->castTo<FunctionType>()->getResult()->isUninhabited())
        os << "  abort();\n";
    } else {
      auto directResultType = signature.getDirectResultType();
      printDirectReturnOrParamCType(
          *directResultType, resultTy, moduleContext, os, cPrologueOS,
          typeMapping, interopContext, [&]() {
            os << "    return swift::Expected<";
            OptionalTypeKind retKind;
            Type objTy;
            std::tie(objTy, retKind) =
                DeclAndTypePrinter::getObjectTypeAndOptionality(FD, resultTy);

            auto s = printClangFunctionReturnType(
                os, objTy, retKind, const_cast<ModuleDecl *>(moduleContext),
                OutputLanguageMode::Cxx);
            os << ">(swift::Error(opaqueError));\n";
            os << "#endif\n";

            // Return the function result value if it doesn't throw.
            if (!resultTy->isVoid() && hasThrows) {
              os << "\n";
              os << "  return SWIFT_RETURN_THUNK(";
              printClangFunctionReturnType(
                  os, objTy, retKind, const_cast<ModuleDecl *>(moduleContext),
                  OutputLanguageMode::Cxx);
              os << ", returnValue);\n";
            }

            assert(!s.isUnsupported());
          });
    }
  }
}

static bool checkDuplicatedMethodName(StringRef funcName,
                                      const AccessorDecl *AD,
                                      DeclAndTypePrinter &declAndTypePrinter,
                                      raw_ostream &os) {
  auto *&decl = declAndTypePrinter.getCxxDeclEmissionScope()
                    .emittedAccessorMethodNames[funcName];

  if (!decl) {
    // This is the first time an accessor with this name has been emitted.
    decl = AD;
  } else if (decl != AD) {
    // An accessor for another property had the same name.
    os << "  // skip emitting accessor method for \'"
       << AD->getStorage()->getBaseIdentifier().str() << "\'. \'" << funcName
       << "\' already declared.\n";
    return false;
  }

  return true;
}

void DeclAndTypeClangFunctionPrinter::printCxxMethod(
    DeclAndTypePrinter &declAndTypePrinter,
    const NominalTypeDecl *typeDeclContext, const AbstractFunctionDecl *FD,
    const LoweredFunctionSignature &signature, StringRef swiftSymbolName,
    Type resultTy, bool isStatic, bool isDefinition,
    std::optional<IRABIDetailsProvider::MethodDispatchInfo> dispatchInfo) {
  bool isConstructor = isa<ConstructorDecl>(FD);
  os << "  ";

  FunctionSignatureModifiers modifiers;
  if (isDefinition)
    modifiers.qualifierContext = typeDeclContext;
  modifiers.isStatic = (isStatic || isConstructor) && !isDefinition;
  modifiers.isInline = true;
  bool isMutating =
      isa<FuncDecl>(FD) ? cast<FuncDecl>(FD)->isMutating() : false;
  modifiers.isConst = !isa<ClassDecl>(typeDeclContext) && !isMutating &&
                      !isConstructor && !isStatic;
  modifiers.hasSymbolUSR = !isDefinition;
  auto result = printFunctionSignature(
      FD, signature, cxx_translation::getNameForCxx(FD), resultTy,
      FunctionSignatureKind::CxxInlineThunk, modifiers);
  if (result.isUnsupported())
    return;

  declAndTypePrinter.printAvailability(os, FD);
  if (!isDefinition) {
    os << ";\n";
    return;
  }

  os << " {\n";
  // FIXME: should it be objTy for resultTy?
  printCxxThunkBody(FD, signature, swiftSymbolName, typeDeclContext,
                    FD->getModuleContext(), resultTy, FD->getParameters(),
                    FD->hasThrows(),
                    FD->getInterfaceType()->castTo<AnyFunctionType>(), isStatic,
                    dispatchInfo);
  os << "  }\n";
}

/// Returns true if the given property name like `isEmpty` can be remapped
/// directly to a C++ method.
static bool canRemapBoolPropertyNameDirectly(StringRef name) {
  auto startsWithAndLonger = [&](StringRef prefix) -> bool {
    return name.starts_with(prefix) && name.size() > prefix.size();
  };
  return startsWithAndLonger("is") || startsWithAndLonger("has");
}

static std::string remapPropertyName(const AccessorDecl *accessor,
                                     Type resultTy) {
  // For a getter or setter, go through the variable or subscript decl.
  StringRef propertyName =
      cxx_translation::getNameForCxx(accessor->getStorage());

  // Boolean property getters can be remapped directly in certain cases.
  if (accessor->isGetter() && resultTy->isBool() &&
      canRemapBoolPropertyNameDirectly(propertyName)) {
    return propertyName.str();
  }

  std::string name;
  llvm::raw_string_ostream nameOS(name);
  nameOS << (accessor->isSetter() ? "set" : "get")
         << char(std::toupper(propertyName[0])) << propertyName.drop_front();
  nameOS.flush();
  return name;
}

void DeclAndTypeClangFunctionPrinter::printCxxPropertyAccessorMethod(
    DeclAndTypePrinter &declAndTypePrinter,
    const NominalTypeDecl *typeDeclContext, const AccessorDecl *accessor,
    const LoweredFunctionSignature &signature, StringRef swiftSymbolName,
    Type resultTy, bool isStatic, bool isDefinition,
    std::optional<IRABIDetailsProvider::MethodDispatchInfo> dispatchInfo) {
  assert(accessor->isSetter() || accessor->getParameters()->size() == 0);
  std::string accessorName = remapPropertyName(accessor, resultTy);

  if (!checkDuplicatedMethodName(accessorName, accessor, declAndTypePrinter,
                                 os))
    return;

  os << "  ";

  FunctionSignatureModifiers modifiers;
  if (isDefinition)
    modifiers.qualifierContext = typeDeclContext;
  modifiers.isStatic = isStatic && !isDefinition;
  modifiers.isInline = true;
  modifiers.isConst =
      !isStatic && accessor->isGetter() && !isa<ClassDecl>(typeDeclContext);
  modifiers.hasSymbolUSR = !isDefinition;
  modifiers.symbolUSROverride = accessor->getStorage();
  auto result =
      printFunctionSignature(accessor, signature, accessorName, resultTy,
                             FunctionSignatureKind::CxxInlineThunk, modifiers);
  assert(!result.isUnsupported() && "C signature should be unsupported too!");
  declAndTypePrinter.printAvailability(os, accessor->getStorage());
  if (!isDefinition) {
    os << ";\n";
    return;
  }
  os << " {\n";
  // FIXME: should it be objTy for resultTy?
  printCxxThunkBody(accessor, signature, swiftSymbolName, typeDeclContext,
                    accessor->getModuleContext(), resultTy,
                    accessor->getParameters(),
                    /*hasThrows=*/false, nullptr, isStatic, dispatchInfo);
  os << "  }\n";
}

void DeclAndTypeClangFunctionPrinter::printCxxSubscriptAccessorMethod(
    DeclAndTypePrinter &declAndTypePrinter,
    const NominalTypeDecl *typeDeclContext, const AccessorDecl *accessor,
    const LoweredFunctionSignature &signature, StringRef swiftSymbolName,
    Type resultTy, bool isDefinition,
    std::optional<IRABIDetailsProvider::MethodDispatchInfo> dispatchInfo) {
  assert(accessor->isGetter());
  FunctionSignatureModifiers modifiers;
  if (isDefinition)
    modifiers.qualifierContext = typeDeclContext;
  modifiers.isInline = true;
  modifiers.isConst = true;
  auto result =
      printFunctionSignature(accessor, signature, "operator []", resultTy,
                             FunctionSignatureKind::CxxInlineThunk, modifiers);
  assert(!result.isUnsupported() && "C signature should be unsupported too!");
  declAndTypePrinter.printAvailability(os, accessor->getStorage());
  if (!isDefinition) {
    os << ";\n";
    return;
  }
  os << " {\n";
  // FIXME: should it be objTy for resultTy?
  printCxxThunkBody(
      accessor, signature, swiftSymbolName, typeDeclContext,
      accessor->getModuleContext(), resultTy, accessor->getParameters(),
      /*hasThrows=*/false, nullptr, /*isStatic=*/false, dispatchInfo);
  os << "  }\n";
}

bool DeclAndTypeClangFunctionPrinter::hasKnownOptionalNullableCxxMapping(
    Type type) {
  if (auto optionalObjectType = type->getOptionalObjectType()) {
    if (optionalObjectType->getNominalOrBoundGenericNominal()) {
      if (auto typeInfo = typeMapping.getKnownCxxTypeInfo(
              optionalObjectType->getNominalOrBoundGenericNominal())) {
        return typeInfo->canBeNullable;
      }
    }
  }
  return false;
}

void DeclAndTypeClangFunctionPrinter::printCustomCxxFunction(
    const SmallVector<Type> &neededTypes, bool NeedsReturnTypes,
    PrinterTy retTypeAndNamePrinter, PrinterTy paramPrinter, bool isConstFunc,
    PrinterTy bodyPrinter, ValueDecl *valueDecl, ModuleDecl *emittedModule,
    raw_ostream &outOfLineOS) {
  llvm::MapVector<Type, std::string> types;
  llvm::MapVector<Type, std::string> typeRefs;

  for (auto &type : neededTypes) {
    std::string typeStr;
    llvm::raw_string_ostream typeOS(typeStr);
    OptionalTypeKind optKind;
    Type objectType;
    std::tie(objectType, optKind) =
        DeclAndTypePrinter::getObjectTypeAndOptionality(valueDecl, type);

    CFunctionSignatureTypePrinter typePrinter(
        typeOS, cPrologueOS, typeMapping, OutputLanguageMode::Cxx,
        interopContext, CFunctionSignatureTypePrinterModifierDelegate(),
        emittedModule, declPrinter,
        NeedsReturnTypes ? FunctionSignatureTypeUse::ReturnType
                         : FunctionSignatureTypeUse::ParamType);
    auto support =
        typePrinter.visit(objectType, optKind, /* isInOutParam */ false);
    (void)support;
    assert(!support.isUnsupported());
    types.insert({type, typeOS.str()});

    std::string typeRefStr;
    llvm::raw_string_ostream typeRefOS(typeRefStr);
    CFunctionSignatureTypePrinter typeRefPrinter(
        typeRefOS, cPrologueOS, typeMapping, OutputLanguageMode::Cxx,
        interopContext, CFunctionSignatureTypePrinterModifierDelegate(),
        emittedModule, declPrinter, FunctionSignatureTypeUse::TypeReference);
    typeRefPrinter.visit(objectType, optKind, /* isInOutParam */ false);
    typeRefs.insert({type, typeRefOS.str()});
  }

  retTypeAndNamePrinter(types);
  os << '(';
  outOfLineOS << '(';
  paramPrinter(types);
  os << ')';
  outOfLineOS << ')';
  if (isConstFunc) {
    os << " const;\n";
    outOfLineOS << " const";
  }
  outOfLineOS << " {\n";
  bodyPrinter(typeRefs);
  outOfLineOS << "}\n";
}

ClangRepresentation DeclAndTypeClangFunctionPrinter::getTypeRepresentation(
    PrimitiveTypeMapping &typeMapping,
    SwiftToClangInteropContext &interopContext, DeclAndTypePrinter &declPrinter,
    const ModuleDecl *emittedModule, Type ty) {
  CFunctionSignatureTypePrinterModifierDelegate delegate;
  CFunctionSignatureTypePrinter typePrinter(
      llvm::nulls(), llvm::nulls(), typeMapping, OutputLanguageMode::Cxx,
      interopContext, delegate, emittedModule, declPrinter,
      FunctionSignatureTypeUse::TypeReference);
  return typePrinter.visit(ty, OptionalTypeKind::OTK_None,
                           /*isInOutParam=*/false);
}
