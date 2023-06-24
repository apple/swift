//===--- ClangAdapter.cpp - Interfaces with Clang entities ----------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2017 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//
//
// This file provides convenient and canonical interfaces with Clang entities,
// serving as both a useful place to put utility functions and a canonical
// interface that can abstract nitty gritty Clang internal details.
//
//===----------------------------------------------------------------------===//

#include "CFTypeInfo.h"
#include "ClangAdapter.h"
#include "ImportName.h"
#include "ImporterImpl.h"
#include "clang/AST/Decl.h"
#include "clang/AST/DeclObjC.h"
#include "clang/Lex/Lexer.h"
#include "clang/Sema/Lookup.h"
#include "clang/Sema/Sema.h"

using namespace swift;
using namespace importer;

/// Get a bit vector indicating which arguments are non-null for a
/// given function or method.
SmallBitVector
importer::getNonNullArgs(const clang::Decl *decl,
                         ArrayRef<const clang::ParmVarDecl *> params) {
  SmallBitVector result;
  if (!decl)
    return result;

  for (const auto *nonnull : decl->specific_attrs<clang::NonNullAttr>()) {
    if (!nonnull->args_size()) {
      // Easy case: all pointer arguments are non-null.
      if (result.empty())
        result.resize(params.size(), true);
      else
        result.set(0, params.size());

      return result;
    }

    // Mark each of the listed parameters as non-null.
    if (result.empty())
      result.resize(params.size(), false);

    for (auto paramIdx : nonnull->args()) {
      unsigned idx = paramIdx.getASTIndex();
      if (idx < result.size())
        result.set(idx);
    }
  }

  return result;
}

Optional<const clang::Decl *>
importer::getDefinitionForClangTypeDecl(const clang::Decl *D) {
  if (auto OID = dyn_cast<clang::ObjCInterfaceDecl>(D))
    return OID->getDefinition();

  if (auto TD = dyn_cast<clang::TagDecl>(D))
    return TD->getDefinition();

  if (auto OPD = dyn_cast<clang::ObjCProtocolDecl>(D))
    return OPD->getDefinition();

  return None;
}

static bool isInLocalScope(const clang::Decl *D) {
  const clang::DeclContext *LDC = D->getLexicalDeclContext();
  while (true) {
    if (LDC->isFunctionOrMethod())
      return true;
    if (!isa<clang::TagDecl>(LDC))
      return false;
    if (const auto *CRD = dyn_cast<clang::CXXRecordDecl>(LDC))
      if (CRD->isLambda())
        return true;
    LDC = LDC->getLexicalParent();
  }
  return false;
}

const clang::Decl *
importer::getFirstNonLocalDecl(const clang::Decl *D) {
  D = D->getCanonicalDecl();
  auto iter = llvm::find_if(D->redecls(), [](const clang::Decl *next) -> bool {
    return !isInLocalScope(next);
  });
  if (iter == D->redecls_end())
    return nullptr;
  return *iter;
}

Optional<clang::Module *>
importer::getClangSubmoduleForDecl(const clang::Decl *D,
                                   bool allowForwardDeclaration) {
  const clang::Decl *actual = nullptr;

  // Put an Objective-C class into the module that contains the @interface
  // definition, not just some @class forward declaration.
  if (auto maybeDefinition = getDefinitionForClangTypeDecl(D)) {
    actual = maybeDefinition.value();
    if (!actual && !allowForwardDeclaration)
      return None;
  }

  if (!actual)
    actual = getFirstNonLocalDecl(D);

  return actual->getImportedOwningModule();
}

/// Retrieve the instance type of the given Clang declaration context.
clang::QualType
importer::getClangDeclContextType(const clang::DeclContext *dc) {
  auto &ctx = dc->getParentASTContext();
  if (auto objcClass = dyn_cast<clang::ObjCInterfaceDecl>(dc))
    return ctx.getObjCObjectPointerType(ctx.getObjCInterfaceType(objcClass));

  if (auto objcCategory = dyn_cast<clang::ObjCCategoryDecl>(dc)) {
    if (objcCategory->isInvalidDecl())
      return clang::QualType();

    return ctx.getObjCObjectPointerType(
        ctx.getObjCInterfaceType(objcCategory->getClassInterface()));
  }

  if (auto constProto = dyn_cast<clang::ObjCProtocolDecl>(dc)) {
    auto proto = const_cast<clang::ObjCProtocolDecl *>(constProto);
    auto type = ctx.getObjCObjectType(ctx.ObjCBuiltinIdTy, {}, {proto}, false);
    return ctx.getObjCObjectPointerType(type);
  }

  if (auto tag = dyn_cast<clang::TagDecl>(dc)) {
    return ctx.getTagDeclType(tag);
  }

  return clang::QualType();
}

/// Determine whether this is the name of a collection with a single
/// element type.
static bool isCollectionName(StringRef typeName) {
  auto lastWord = camel_case::getLastWord(typeName);
  return lastWord == "Array" || lastWord == "Set";
}

/// Retrieve the name of the given Clang type for use when omitting
/// needless words.
OmissionTypeName importer::getClangTypeNameForOmission(clang::ASTContext &ctx,
                                                       clang::QualType type) {
  if (type.isNull())
    return OmissionTypeName();

  // Dig through the type, looking for a typedef-name and stripping
  // references along the way.
  StringRef lastTypedefName;
  do {
    // The name of a typedef-name.
    auto typePtr = type.getTypePtr();
    if (auto typedefType = dyn_cast<clang::TypedefType>(typePtr)) {
      auto name = typedefType->getDecl()->getName();

      // Objective-C selector type.
      if (ctx.hasSameUnqualifiedType(type, ctx.getObjCSelType()) &&
          name == "SEL")
        return "Selector";

      // Objective-C "id" type.
      if (type->isObjCIdType() && name == "id")
        return "Object";

      // Objective-C "Class" type.
      if (type->isObjCClassType() && name == "Class")
        return "Class";

      // Objective-C "BOOL" type.
      if (name == "BOOL")
        return OmissionTypeName("Bool", OmissionTypeFlags::Boolean);

      // If this is an imported CF type, use that name.
      StringRef CFName = getCFTypeName(typedefType->getDecl());
      if (!CFName.empty())
        return CFName;

      // If we have NS(U)Integer or CGFloat, return it.
      if (name == "NSInteger" || name == "NSUInteger" || name == "CGFloat")
        return name;

      // If it's a collection name and of pointer type, call it an
      // array of the pointee type.
      if (isCollectionName(name)) {
        if (auto ptrType = type->getAs<clang::PointerType>()) {
          return OmissionTypeName(
              name, None,
              getClangTypeNameForOmission(ctx, ptrType->getPointeeType()).Name);
        }
      }

      // Otherwise, desugar one level...
      lastTypedefName = name;
      type = typedefType->getDecl()->getUnderlyingType();
      continue;
    }

    // For array types, convert the element type and treat this an as array.
    if (auto arrayType = dyn_cast<clang::ArrayType>(typePtr)) {
      return OmissionTypeName(
          "Array", None,
          getClangTypeNameForOmission(ctx, arrayType->getElementType()).Name);
    }

    // Look through reference types.
    if (auto refType = dyn_cast<clang::ReferenceType>(typePtr)) {
      type = refType->getPointeeTypeAsWritten();
      continue;
    }

    // Look through pointer types.
    if (auto ptrType = dyn_cast<clang::PointerType>(typePtr)) {
      type = ptrType->getPointeeType();
      continue;
    }

    // Try to desugar one level...
    clang::QualType desugared = type.getSingleStepDesugaredType(ctx);
    if (desugared.getTypePtr() == type.getTypePtr())
      break;

    type = desugared;
  } while (true);

  // Objective-C object pointers.
  if (auto objcObjectPtr = type->getAs<clang::ObjCObjectPointerType>()) {
    auto objcClass = objcObjectPtr->getInterfaceDecl();

    // For id<Proto> or NSObject<Proto>, retrieve the name of "Proto".
    if (objcObjectPtr->getNumProtocols() == 1 &&
        (!objcClass || objcClass->getName() == "NSObject"))
      return (*objcObjectPtr->qual_begin())->getName();

    // If there is a class, use it.
    if (objcClass) {
      // If this isn't the name of an Objective-C collection, we're done.
      auto className = objcClass->getName();
      if (!isCollectionName(className))
        return className;

      // If we don't have type parameters, use the prefix of the type
      // name as the collection element type.
      if (objcClass && !objcClass->getTypeParamList()) {
        unsigned lastWordSize = camel_case::getLastWord(className).size();
        StringRef elementName =
            className.substr(0, className.size() - lastWordSize);
        return OmissionTypeName(className, None, elementName);
      }

      // If we don't have type arguments, the collection element type
      // is "Object".
      auto typeArgs = objcObjectPtr->getTypeArgs();
      if (typeArgs.empty())
        return OmissionTypeName(className, None, "Object");

      return OmissionTypeName(
          className, None, getClangTypeNameForOmission(ctx, typeArgs[0]).Name);
    }

    // Objective-C "id" type.
    if (objcObjectPtr->isObjCIdType())
      return "Object";

    // Objective-C "Class" type.
    if (objcObjectPtr->isObjCClassType())
      return "Class";

    return StringRef();
  }

  // Handle builtin types by importing them and getting the Swift name.
  if (auto builtinTy = type->getAs<clang::BuiltinType>()) {
    // Names of integer types.
    static const char *intTypeNames[] = {"UInt8", "UInt16", "UInt32", "UInt64",
                                         "UInt128"};

    /// Retrieve the name for an integer type based on its size.
    auto getIntTypeName = [&](bool isSigned) -> StringRef {
      switch (ctx.getTypeSize(builtinTy)) {
      case 8:
        return StringRef(intTypeNames[0]).substr(isSigned ? 1 : 0);
      case 16:
        return StringRef(intTypeNames[1]).substr(isSigned ? 1 : 0);
      case 32:
        return StringRef(intTypeNames[2]).substr(isSigned ? 1 : 0);
      case 64:
        return StringRef(intTypeNames[3]).substr(isSigned ? 1 : 0);
      case 128:
        return StringRef(intTypeNames[4]).substr(isSigned ? 1 : 0);
      default:
        llvm_unreachable("bad integer type size");
      }
    };

    switch (builtinTy->getKind()) {
    case clang::BuiltinType::Void:
      return "Void";

    case clang::BuiltinType::Bool:
      return OmissionTypeName("Bool", OmissionTypeFlags::Boolean);

    case clang::BuiltinType::Float:
      return "Float";

    case clang::BuiltinType::Double:
      return "Double";

    case clang::BuiltinType::Char8:
      return "UInt8";

    case clang::BuiltinType::Char16:
      return "UInt16";

    case clang::BuiltinType::Char32:
      return "UnicodeScalar";

    case clang::BuiltinType::Char_U:
    case clang::BuiltinType::UChar:
    case clang::BuiltinType::UShort:
    case clang::BuiltinType::UInt:
    case clang::BuiltinType::ULong:
    case clang::BuiltinType::ULongLong:
    case clang::BuiltinType::UInt128:
    case clang::BuiltinType::WChar_U:
      return getIntTypeName(false);

    case clang::BuiltinType::Char_S:
    case clang::BuiltinType::SChar:
    case clang::BuiltinType::Short:
    case clang::BuiltinType::Int:
    case clang::BuiltinType::Long:
    case clang::BuiltinType::LongLong:
    case clang::BuiltinType::Int128:
    case clang::BuiltinType::WChar_S:
      return getIntTypeName(true);

    // Types that cannot be mapped into Swift, and probably won't ever be.
    case clang::BuiltinType::Dependent:
    case clang::BuiltinType::ARCUnbridgedCast:
    case clang::BuiltinType::BoundMember:
    case clang::BuiltinType::BuiltinFn:
    case clang::BuiltinType::IncompleteMatrixIdx:
    case clang::BuiltinType::Overload:
    case clang::BuiltinType::PseudoObject:
    case clang::BuiltinType::UnknownAny:
      return OmissionTypeName();

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
    case clang::BuiltinType::Half:
    case clang::BuiltinType::LongDouble:
    case clang::BuiltinType::BFloat16:
    case clang::BuiltinType::Float16:
    case clang::BuiltinType::Float128:
    case clang::BuiltinType::NullPtr:
    case clang::BuiltinType::Ibm128:
      return OmissionTypeName();

    // Objective-C types that aren't mapped directly; rather, pointers to
    // these types will be mapped.
    case clang::BuiltinType::ObjCClass:
    case clang::BuiltinType::ObjCId:
    case clang::BuiltinType::ObjCSel:
      return OmissionTypeName();

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
      return OmissionTypeName();

    // OpenMP types that don't have Swift equivalents.
    case clang::BuiltinType::OMPArraySection:
    case clang::BuiltinType::OMPArrayShaping:
    case clang::BuiltinType::OMPIterator:
      return OmissionTypeName();

    // ARM SVE builtin types that don't have Swift equivalents.
#define SVE_TYPE(Name, Id, ...) case clang::BuiltinType::Id:
#include "clang/Basic/AArch64SVEACLETypes.def"
      return OmissionTypeName();

    // PPC MMA builtin types that don't have Swift equivalents.
#define PPC_VECTOR_TYPE(Name, Id, Size) case clang::BuiltinType::Id:
#include "clang/Basic/PPCTypes.def"
      return OmissionTypeName();

    // RISC-V V builtin types that don't have Swift equivalents.
#define RVV_TYPE(Name, Id, Size) case clang::BuiltinType::Id:
#include "clang/Basic/RISCVVTypes.def"
      return OmissionTypeName();
    }
  }

  // Tag types.
  if (auto tagType = type->getAs<clang::TagType>()) {
    if (tagType->getDecl()->getName().empty())
      return lastTypedefName;

    return tagType->getDecl()->getName();
  }

  // Block pointers.
  if (type->getAs<clang::BlockPointerType>())
    return OmissionTypeName("Block", OmissionTypeFlags::Function);

  // Function pointers.
  if (type->isFunctionType())
    return OmissionTypeName("Function", OmissionTypeFlags::Function);

  return StringRef();
}

static clang::SwiftNewTypeAttr *
retrieveNewTypeAttr(const clang::TypedefNameDecl *decl) {
  // Retrieve the attribute.
  auto attr = decl->getAttr<clang::SwiftNewTypeAttr>();
  if (!attr)
    return nullptr;

  // FIXME: CFErrorDomain is marked as CF_EXTENSIBLE_STRING_ENUM, but it turned
  // out to be more disruptive than not to leave it that way.
  auto name = decl->getName();
  if (name == "CFErrorDomain")
    return nullptr;

  return attr;
}

clang::SwiftNewTypeAttr *
importer::getSwiftNewtypeAttr(const clang::TypedefNameDecl *decl,
                              ImportNameVersion version) {
  // Newtype was introduced in Swift 3
  if (version <= ImportNameVersion::swift2())
    return nullptr;
  return retrieveNewTypeAttr(decl);
}

// If this decl is associated with a swift_newtype typedef, return it, otherwise
// null
clang::TypedefNameDecl *importer::findSwiftNewtype(const clang::NamedDecl *decl,
                                                   clang::Sema &clangSema,
                                                   ImportNameVersion version) {
  // Newtype was introduced in Swift 3
  if (version <= ImportNameVersion::swift2())
    return nullptr;

  auto varDecl = dyn_cast<clang::VarDecl>(decl);
  if (!varDecl)
    return nullptr;

  if (auto typedefTy = varDecl->getType()->getAs<clang::TypedefType>())
    if (retrieveNewTypeAttr(typedefTy->getDecl()))
      return typedefTy->getDecl();

  // Special case: "extern NSString * fooNotification" adopts
  // NSNotificationName type, and is a member of NSNotificationName
  if (isNSNotificationGlobal(decl)) {
    clang::IdentifierInfo *notificationName =
        &clangSema.getASTContext().Idents.get("NSNotificationName");
    clang::LookupResult lookupResult(clangSema, notificationName,
                                     clang::SourceLocation(),
                                     clang::Sema::LookupOrdinaryName);
    if (!clangSema.LookupName(lookupResult, clangSema.TUScope))
      return nullptr;
    auto nsDecl = lookupResult.getAsSingle<clang::TypedefNameDecl>();
    if (!nsDecl)
      return nullptr;

    // Make sure it also has a newtype decl on it
    if (retrieveNewTypeAttr(nsDecl))
      return nsDecl;

    return nullptr;
  }

  return nullptr;
}

bool importer::isNSString(const clang::Type *type) {
  if (auto ptrType = type->getAs<clang::ObjCObjectPointerType>())
    if (auto interfaceType = ptrType->getInterfaceType())
      if (interfaceType->getDecl()->getName() == "NSString")
        return true;
  return false;
}

bool importer::isNSString(clang::QualType qt) {
  return qt.getTypePtrOrNull() && isNSString(qt.getTypePtrOrNull());
}

bool importer::isNSNotificationGlobal(const clang::NamedDecl *decl) {
  // Looking for: extern NSString *fooNotification;

  // Must be extern global variable
  auto vDecl = dyn_cast<clang::VarDecl>(decl);
  if (!vDecl || !vDecl->hasExternalFormalLinkage())
    return false;

  // No explicit swift_name
  if (decl->getAttr<clang::SwiftNameAttr>())
    return false;

  // Must end in Notification
  if (!vDecl->getDeclName().isIdentifier())
    return false;
  if (stripNotification(vDecl->getName()).empty())
    return false;

  // Must be NSString *
  if (!isNSString(vDecl->getType()))
    return false;

  // We're a match!
  return true;
}

bool importer::hasNativeSwiftDecl(const clang::Decl *decl) {
  if (auto *attr = decl->getAttr<clang::ExternalSourceSymbolAttr>())
    if (attr->getGeneratedDeclaration() && attr->getLanguage() == "Swift")
      return true;
  return false;
}

/// Translate the "nullability" notion from API notes into an optional type
/// kind.
OptionalTypeKind importer::translateNullability(
    clang::NullabilityKind kind, bool stripNonResultOptionality) {
  if (stripNonResultOptionality &&
      kind != clang::NullabilityKind::NullableResult)
    return OptionalTypeKind::OTK_None;

  switch (kind) {
  case clang::NullabilityKind::NonNull:
    return OptionalTypeKind::OTK_None;

  case clang::NullabilityKind::Nullable:
  case clang::NullabilityKind::NullableResult:
    return OptionalTypeKind::OTK_Optional;

  case clang::NullabilityKind::Unspecified:
    return OptionalTypeKind::OTK_ImplicitlyUnwrappedOptional;
  }

  llvm_unreachable("Invalid NullabilityKind.");
  return OptionalTypeKind::OTK_Optional;
}

bool importer::isRequiredInitializer(const clang::ObjCMethodDecl *method) {
  // FIXME: No way to express this in Objective-C.
  return false;
}

/// Check if this method is declared in the context that conforms to
/// NSAccessibility.
static bool isAccessibilityConformingContext(const clang::DeclContext *ctx) {
  const clang::ObjCProtocolList *protocols = nullptr;

  if (auto protocol = dyn_cast<clang::ObjCProtocolDecl>(ctx)) {
    if (protocol->getName() == "NSAccessibility")
      return true;
    return false;
  } else if (auto interface = dyn_cast<clang::ObjCInterfaceDecl>(ctx))
    protocols = &interface->getReferencedProtocols();
  else if (auto category = dyn_cast<clang::ObjCCategoryDecl>(ctx))
    protocols = &category->getReferencedProtocols();
  else
    return false;

  for (auto pi : *protocols) {
    if (pi->getName() == "NSAccessibility")
      return true;
  }
  return false;
}

bool
importer::shouldImportPropertyAsAccessors(const clang::ObjCPropertyDecl *prop) {
  if (prop->hasAttr<clang::SwiftImportPropertyAsAccessorsAttr>())
    return true;

  // Check if the property is one of the specially handled accessibility APIs.
  //
  // These appear as both properties and methods in ObjC and should be
  // imported as methods into Swift, as a sort of least-common-denominator
  // compromise.
  if (!prop->getName().startswith("accessibility"))
    return false;
  if (isAccessibilityConformingContext(prop->getDeclContext()))
    return true;

  return false;
}

bool importer::isInitMethod(const clang::ObjCMethodDecl *method) {
  // init methods are always instance methods.
  if (!method->isInstanceMethod())
    return false;

  // init methods must be classified as such by Clang.
  if (method->getMethodFamily() != clang::OMF_init)
    return false;

  // Swift restriction: init methods must start with the word "init".
  auto selector = method->getSelector();
  return camel_case::getFirstWord(selector.getNameForSlot(0)) == "init";
}

bool importer::isObjCId(const clang::Decl *decl) {
  auto typedefDecl = dyn_cast<clang::TypedefNameDecl>(decl);
  if (!typedefDecl)
    return false;

  if (!typedefDecl->getDeclContext()->getRedeclContext()->isTranslationUnit())
    return false;

  return typedefDecl->getName() == "id";
}

bool importer::isUnavailableInSwift(
    const clang::Decl *decl,
    const PlatformAvailability *platformAvailability,
    bool enableObjCInterop) {
  // 'id' is always unavailable in Swift.
  if (enableObjCInterop && isObjCId(decl))
    return true;

  if (decl->isUnavailable())
    return true;

  for (auto *attr : decl->specific_attrs<clang::AvailabilityAttr>()) {
    if (attr->getPlatform()->getName() == "swift")
      return true;

    if (!platformAvailability)
      continue;

    if (!platformAvailability->isPlatformRelevant(
            attr->getPlatform()->getName())) {
      continue;
    }


    llvm::VersionTuple version = attr->getDeprecated();
    if (version.empty())
      continue;
    if (platformAvailability->treatDeprecatedAsUnavailable(
            decl, version, /*isAsync=*/false)) {
      return true;
    }
  }

  return false;
}

OptionalTypeKind importer::getParamOptionality(const clang::ParmVarDecl *param,
                                               bool knownNonNull) {
  auto &clangCtx = param->getASTContext();

  // If nullability is available on the type, use it.
  clang::QualType paramTy = param->getType();
  if (auto nullability = paramTy->getNullability(clangCtx)) {
    return translateNullability(*nullability);
  }

  // If it's known non-null, use that.
  if (knownNonNull || param->hasAttr<clang::NonNullAttr>())
    return OTK_None;

  // Check for the 'static' annotation on C arrays.
  if (const auto *DT = dyn_cast<clang::DecayedType>(paramTy))
    if (const auto *AT = DT->getOriginalType()->getAsArrayTypeUnsafe())
      if (AT->getSizeModifier() == clang::ArrayType::Static)
        return OTK_None;

  // Default to implicitly unwrapped optionals.
  return OTK_ImplicitlyUnwrappedOptional;
}
