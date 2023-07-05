//===--- ImportDecl.cpp - Import Clang Declarations -----------------------===//
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
// This file implements support for importing Clang declarations into Swift.
//
//===----------------------------------------------------------------------===//

#include "CFTypeInfo.h"
#include "ImporterImpl.h"
#include "ClangDerivedConformances.h"
#include "SwiftDeclSynthesizer.h"
#include "swift/AST/ASTContext.h"
#include "swift/AST/Attr.h"
#include "swift/AST/Builtins.h"
#include "swift/AST/ClangModuleLoader.h"
#include "swift/AST/Decl.h"
#include "swift/AST/DiagnosticsClangImporter.h"
#include "swift/AST/ExistentialLayout.h"
#include "swift/AST/Expr.h"
#include "swift/AST/GenericEnvironment.h"
#include "swift/AST/GenericSignature.h"
#include "swift/AST/Module.h"
#include "swift/AST/NameLookup.h"
#include "swift/AST/NameLookupRequests.h"
#include "swift/AST/ParameterList.h"
#include "swift/AST/Pattern.h"
#include "swift/AST/PrettyStackTrace.h"
#include "swift/AST/ProtocolConformance.h"
#include "swift/AST/Stmt.h"
#include "swift/AST/TypeCheckRequests.h"
#include "swift/AST/Types.h"
#include "swift/Basic/Defer.h"
#include "swift/Basic/PrettyStackTrace.h"
#include "swift/Basic/Statistic.h"
#include "swift/Basic/StringExtras.h"
#include "swift/ClangImporter/CXXMethodBridging.h"
#include "swift/ClangImporter/ClangImporterRequests.h"
#include "swift/ClangImporter/ClangModule.h"
#include "swift/Parse/Lexer.h"
#include "swift/Parse/Parser.h"
#include "swift/Strings.h"
#include "clang/AST/ASTContext.h"
#include "clang/AST/Attr.h"
#include "clang/AST/DeclCXX.h"
#include "clang/AST/DeclObjCCommon.h"
#include "clang/Basic/CharInfo.h"
#include "clang/Basic/Specifiers.h"
#include "clang/Basic/TargetInfo.h"
#include "clang/Lex/Preprocessor.h"
#include "clang/Sema/Lookup.h"

#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/Statistic.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/ADT/StringSwitch.h"
#include "llvm/Support/Path.h"

#include <algorithm>

#define DEBUG_TYPE "Clang module importer"

STATISTIC(NumTotalImportedEntities, "# of imported clang entities");
STATISTIC(NumFactoryMethodsAsInitializers,
          "# of factory methods mapped to initializers");

using namespace swift;
using namespace importer;

namespace swift {
namespace inferred_attributes {
  enum {
    requires_stored_property_inits = 0x01
  };
} // end namespace inferred_attributes
} // end namespace swift

namespace {

struct AccessorInfo {
  AbstractStorageDecl *Storage;
  AccessorKind Kind;
};

} // end anonymous namespace

static bool isInSystemModule(const DeclContext *D) {
  return cast<ClangModuleUnit>(D->getModuleScopeContext())->isSystemModule();
}

static FuncDecl *
createFuncOrAccessor(ClangImporter::Implementation &impl, SourceLoc funcLoc,
                     llvm::Optional<AccessorInfo> accessorInfo, DeclName name,
                     SourceLoc nameLoc, GenericParamList *genericParams,
                     ParameterList *bodyParams, Type resultTy, bool async,
                     bool throws, DeclContext *dc, ClangNode clangNode) {
  FuncDecl *decl;
  if (accessorInfo) {
    decl = AccessorDecl::create(
        impl.SwiftContext, funcLoc,
        /*accessorKeywordLoc*/ SourceLoc(), accessorInfo->Kind,
        accessorInfo->Storage,
        /*StaticLoc*/ SourceLoc(), StaticSpellingKind::None, async,
        /*AsyncLoc=*/SourceLoc(), throws, /*ThrowsLoc=*/SourceLoc(), bodyParams,
        resultTy, dc, clangNode);
  } else {
    decl = FuncDecl::createImported(impl.SwiftContext, funcLoc, name, nameLoc,
                                    async, throws, bodyParams, resultTy,
                                    genericParams, dc, clangNode);
  }
  impl.importSwiftAttrAttributes(decl);
  return decl;
}

void ClangImporter::Implementation::makeComputed(AbstractStorageDecl *storage,
                                                 AccessorDecl *getter,
                                                 AccessorDecl *setter) {
  assert(getter);
  storage->getASTContext().evaluator.cacheOutput(HasStorageRequest{storage}, false);
  if (setter) {
    storage->setImplInfo(StorageImplInfo::getMutableComputed());
    storage->setAccessors(SourceLoc(), {getter, setter}, SourceLoc());
  } else {
    storage->setImplInfo(StorageImplInfo::getImmutableComputed());
    storage->setAccessors(SourceLoc(), {getter}, SourceLoc());
  }
}

bool ClangImporter::Implementation::recordHasReferenceSemantics(
    const clang::RecordDecl *decl, ASTContext &ctx) {
  if (!isa<clang::CXXRecordDecl>(decl) && !ctx.LangOpts.CForeignReferenceTypes)
    return false;

  return decl->hasAttrs() && llvm::any_of(decl->getAttrs(), [](auto *attr) {
           if (auto swiftAttr = dyn_cast<clang::SwiftAttrAttr>(attr))
             return swiftAttr->getAttribute() == "import_reference" ||
                    // TODO: Remove this once libSwift hosttools no longer
                    // requires it.
                    swiftAttr->getAttribute() == "import_as_ref";
           return false;
         });
}

#ifndef NDEBUG
static bool verifyNameMapping(MappedTypeNameKind NameMapping,
                              StringRef left, StringRef right) {
  return NameMapping == MappedTypeNameKind::DoNothing || left != right;
}
#endif

/// Map a well-known C type to a swift type from the standard library.
///
/// \param IsError set to true when we know the corresponding swift type name,
/// but we could not find it.  (For example, the type was not defined in the
/// standard library or the required standard library module was not imported.)
/// This should be a hard error, we don't want to map the type only sometimes.
///
/// \returns A pair of a swift type and its name that corresponds to a given
/// C type.
static std::pair<Type, StringRef>
getSwiftStdlibType(const clang::TypedefNameDecl *D,
                   Identifier Name,
                   ClangImporter::Implementation &Impl,
                   bool *IsError, MappedTypeNameKind &NameMapping) {
  *IsError = false;

  MappedCTypeKind CTypeKind;
  unsigned Bitwidth;
  StringRef SwiftModuleName;
  bool IsSwiftModule; // True if SwiftModuleName == STDLIB_NAME.
  StringRef SwiftTypeName;
  bool CanBeMissing;


  do {
#define MAP_TYPE(C_TYPE_NAME, C_TYPE_KIND, C_TYPE_BITWIDTH,        \
                 SWIFT_MODULE_NAME, SWIFT_TYPE_NAME,               \
                 CAN_BE_MISSING, C_NAME_MAPPING)                   \
    if (Name.str() == C_TYPE_NAME) {                               \
      CTypeKind = MappedCTypeKind::C_TYPE_KIND;                    \
      Bitwidth = C_TYPE_BITWIDTH;                                  \
      SwiftModuleName = SWIFT_MODULE_NAME;                         \
      IsSwiftModule = SwiftModuleName == STDLIB_NAME;              \
      SwiftTypeName = SWIFT_TYPE_NAME;                             \
      CanBeMissing = CAN_BE_MISSING;                               \
      NameMapping = MappedTypeNameKind::C_NAME_MAPPING;            \
      assert(verifyNameMapping(MappedTypeNameKind::C_NAME_MAPPING, \
                               C_TYPE_NAME, SWIFT_TYPE_NAME) &&    \
             "MappedTypes.def: Identical names must use DoNothing"); \
      break;                                                       \
    }
#include "MappedTypes.def"

    // We handle `BOOL` as a special case because the selection here is more
    // complicated as the type alias exists on multiple platforms as different
    // types.  It appears in an Objective-C context where it is a `signed char`
    // and appears in Windows as an `int`.  Furthermore, you can actually have
    // the two interoperate, which requires a further bit of logic to
    // disambiguate the type aliasing behaviour.  To complicate things, the two
    // aliases bridge to different types - `ObjCBool` for Objective-C and
    // `WindowsBool` for Windows's `BOOL` type.
    if (Name.str() == "BOOL") {
      auto &CASTContext = Impl.getClangASTContext();
      auto &SwiftASTContext = Impl.SwiftContext;

      // Default to Objective-C `BOOL`
      CTypeKind = MappedCTypeKind::ObjCBool;
      if (CASTContext.getTargetInfo().getTriple().isOSWindows()) {
        // On Windows fall back to Windows `BOOL`
        CTypeKind = MappedCTypeKind::SignedInt;
        // If Objective-C interop is enabled, and we match the Objective-C
        // `BOOL` type, then switch back to `ObjCBool`.
        if (SwiftASTContext.LangOpts.EnableObjCInterop &&
            CASTContext.hasSameType(D->getUnderlyingType(),
                                    CASTContext.ObjCBuiltinBoolTy))
          CTypeKind = MappedCTypeKind::ObjCBool;
      }

      if (CTypeKind == MappedCTypeKind::ObjCBool) {
        Bitwidth = 8;
        SwiftModuleName = StringRef("ObjectiveC");
        IsSwiftModule = false;
        SwiftTypeName = "ObjCBool";
        NameMapping = MappedTypeNameKind::DoNothing;
        CanBeMissing = false;
        assert(verifyNameMapping(MappedTypeNameKind::DoNothing,
                                 "BOOL", "ObjCBool") &&
               "MappedTypes.def: Identical names must use DoNothing");
      } else {
        assert(CTypeKind == MappedCTypeKind::SignedInt &&
               "expected Windows `BOOL` desugared to `int`");
        Bitwidth = 32;
        SwiftModuleName = StringRef("WinSDK");
        IsSwiftModule = false;
        SwiftTypeName = "WindowsBool";
        NameMapping = MappedTypeNameKind::DoNothing;
        CanBeMissing = true;
        assert(verifyNameMapping(MappedTypeNameKind::DoNothing,
                                 "BOOL", "WindowsBool") &&
               "MappedTypes.def: Identical names must use DoNothing");
      }

      break;
    }

    // We did not find this type, thus it is not mapped.
    return std::make_pair(Type(), "");
  } while (0);

  clang::ASTContext &ClangCtx = Impl.getClangASTContext();

  auto ClangType = D->getUnderlyingType();

  // If the C type does not have the expected size, don't import it as a stdlib
  // type.
  unsigned ClangTypeSize = ClangCtx.getTypeSize(ClangType);
  if (Bitwidth != 0 && Bitwidth != ClangTypeSize)
    return std::make_pair(Type(), "");

  // Check other expected properties of the C type.
  switch(CTypeKind) {
  case MappedCTypeKind::UnsignedInt:
    if (!ClangType->isUnsignedIntegerType())
      return std::make_pair(Type(), "");
    break;

  case MappedCTypeKind::SignedInt:
    if (!ClangType->isSignedIntegerType())
      return std::make_pair(Type(), "");
    break;

  case MappedCTypeKind::UnsignedWord:
    if (ClangTypeSize != 64 && ClangTypeSize != 32)
      return std::make_pair(Type(), "");
    if (!ClangType->isUnsignedIntegerType())
      return std::make_pair(Type(), "");
    break;

  case MappedCTypeKind::SignedWord:
    if (ClangTypeSize != 64 && ClangTypeSize != 32)
      return std::make_pair(Type(), "");
    if (!ClangType->isSignedIntegerType())
      return std::make_pair(Type(), "");
    break;

  case MappedCTypeKind::FloatIEEEsingle:
  case MappedCTypeKind::FloatIEEEdouble:
  case MappedCTypeKind::FloatX87DoubleExtended: {
    if (!ClangType->isFloatingType())
      return std::make_pair(Type(), "");

    const llvm::fltSemantics &Sem = ClangCtx.getFloatTypeSemantics(ClangType);
    switch(CTypeKind) {
    case MappedCTypeKind::FloatIEEEsingle:
      assert(Bitwidth == 32 && "FloatIEEEsingle should be 32 bits wide");
      if (&Sem != &APFloat::IEEEsingle())
        return std::make_pair(Type(), "");
      break;

    case MappedCTypeKind::FloatIEEEdouble:
      assert(Bitwidth == 64 && "FloatIEEEdouble should be 64 bits wide");
      if (&Sem != &APFloat::IEEEdouble())
        return std::make_pair(Type(), "");
      break;

    case MappedCTypeKind::FloatX87DoubleExtended:
      assert(Bitwidth == 80 && "FloatX87DoubleExtended should be 80 bits wide");
      if (&Sem != &APFloat::x87DoubleExtended())
        return std::make_pair(Type(), "");
      break;

    default:
      llvm_unreachable("should see only floating point types here");
    }
    }
    break;

  case MappedCTypeKind::VaList:
    switch (ClangCtx.getTargetInfo().getBuiltinVaListKind()) {
      case clang::TargetInfo::CharPtrBuiltinVaList:
      case clang::TargetInfo::VoidPtrBuiltinVaList:
      case clang::TargetInfo::PowerABIBuiltinVaList:
      case clang::TargetInfo::AAPCSABIBuiltinVaList:
      case clang::TargetInfo::HexagonBuiltinVaList:
        assert(ClangCtx.getTypeSize(ClangCtx.VoidPtrTy) == ClangTypeSize &&
               "expected va_list type to be sizeof(void *)");
        break;
      case clang::TargetInfo::AArch64ABIBuiltinVaList:
        break;
      case clang::TargetInfo::PNaClABIBuiltinVaList:
      case clang::TargetInfo::SystemZBuiltinVaList:
      case clang::TargetInfo::X86_64ABIBuiltinVaList:
        return std::make_pair(Type(), "");
    }
    break;

  case MappedCTypeKind::ObjCBool:
    if (!ClangCtx.hasSameType(ClangType, ClangCtx.ObjCBuiltinBoolTy) &&
        !(ClangCtx.getBOOLDecl() &&
          ClangCtx.hasSameType(ClangType, ClangCtx.getBOOLType())))
      return std::make_pair(Type(), "");
    break;

  case MappedCTypeKind::ObjCSel:
    if (!ClangCtx.hasSameType(ClangType, ClangCtx.getObjCSelType()) &&
        !ClangCtx.hasSameType(ClangType,
                              ClangCtx.getObjCSelRedefinitionType()))
      return std::make_pair(Type(), "");
    break;

  case MappedCTypeKind::ObjCId:
    if (!ClangCtx.hasSameType(ClangType, ClangCtx.getObjCIdType()) &&
        !ClangCtx.hasSameType(ClangType,
                              ClangCtx.getObjCIdRedefinitionType()))
      return std::make_pair(Type(), "");
    break;

  case MappedCTypeKind::ObjCClass:
    if (!ClangCtx.hasSameType(ClangType, ClangCtx.getObjCClassType()) &&
        !ClangCtx.hasSameType(ClangType,
                              ClangCtx.getObjCClassRedefinitionType()))
      return std::make_pair(Type(), "");
    break;

  case MappedCTypeKind::CGFloat:
    if (!ClangType->isFloatingType())
      return std::make_pair(Type(), "");
    break;

  case MappedCTypeKind::Block:
    if (!ClangType->isBlockPointerType())
      return std::make_pair(Type(), "");
    break;
  }

  ModuleDecl *M;
  if (IsSwiftModule)
    M = Impl.getStdlibModule();
  else
    M = Impl.getNamedModule(SwiftModuleName);

  if (!M) {
    // User did not import the library module that contains the type we want to
    // substitute.
    *IsError = true;
    return std::make_pair(Type(), "");
  }

  Type SwiftType = Impl.getNamedSwiftType(M, SwiftTypeName);

  if (!SwiftType && CTypeKind == MappedCTypeKind::CGFloat) {
    // Look for CGFloat in CoreFoundation.
    M = Impl.getNamedModule("CoreFoundation");
    SwiftType = Impl.getNamedSwiftType(M, SwiftTypeName);
  }

  if (!SwiftType && !CanBeMissing) {
    // The required type is not defined in the standard library.
    // The required type is not defined in the library, or the user has not
    // imported the library that defines it (so `M` was null and
    // `getNamedSwiftType()` returned early).
    *IsError = true;
    return std::make_pair(Type(), "");
  }
  return std::make_pair(SwiftType, SwiftTypeName);
}

static bool isNSDictionaryMethod(const clang::ObjCMethodDecl *MD,
                                 clang::Selector cmd) {
  if (MD->getSelector() != cmd)
    return false;
  if (isa<clang::ObjCProtocolDecl>(MD->getDeclContext()))
    return false;
  if (MD->getClassInterface()->getName() != "NSDictionary")
    return false;
  return true;
}

void ClangImporter::Implementation::addSynthesizedTypealias(
    NominalTypeDecl *nominal, Identifier name, Type underlyingType) {
  auto &ctx = nominal->getASTContext();

  auto typealias = new (ctx) TypeAliasDecl(SourceLoc(), SourceLoc(), name,
                                           SourceLoc(), nullptr, nominal);
  typealias->setUnderlyingType(underlyingType);
  typealias->setAccess(AccessLevel::Public);
  typealias->setImplicit();

  nominal->addMember(typealias);
}

void ClangImporter::Implementation::addSynthesizedProtocolAttrs(
    NominalTypeDecl *nominal,
    ArrayRef<KnownProtocolKind> synthesizedProtocolAttrs, bool isUnchecked) {
  auto &ctx = nominal->getASTContext();

  for (auto kind : synthesizedProtocolAttrs) {
    // This is unfortunately not an error because some test use mock protocols.
    // If those tests were updated, we could assert that
    // ctx.getProtocol(kind) != nulltpr which would be nice.
    if (auto proto = ctx.getProtocol(kind))
      nominal->getAttrs().add(
          new (ctx) SynthesizedProtocolAttr(ctx.getProtocol(kind), this,
                                            isUnchecked));
  }
}

/// Retrieve the element interface type and key param decl of a subscript
/// setter.
static std::pair<Type, ParamDecl *> decomposeSubscriptSetter(FuncDecl *setter) {
  auto *PL = setter->getParameters();
  if (PL->size() != 2)
    return {nullptr, nullptr};

  // Setter type is (self) -> (elem_type, key_type) -> ()
  Type elementType = setter->getInterfaceType()
                         ->castTo<AnyFunctionType>()
                         ->getResult()
                         ->castTo<AnyFunctionType>()
                         ->getParams().front().getParameterType();
  ParamDecl *keyDecl = PL->get(1);

  return {elementType, keyDecl};
}

/// Rectify the (possibly different) types determined by the
/// getter and setter for a subscript.
///
/// \param canUpdateType whether the type of subscript can be
/// changed from the getter type to something compatible with both
/// the getter and the setter.
///
/// \returns the type to be used for the subscript, or a null type
/// if the types cannot be rectified.
static ImportedType rectifySubscriptTypes(Type getterType, bool getterIsIUO,
                                          Type setterType, bool canUpdateType) {
  // If the caller couldn't provide a setter type, there is
  // nothing to rectify.
  if (!setterType)
    return {nullptr, false};

  // Trivial case: same type in both cases.
  if (getterType->isEqual(setterType))
    return {getterType, getterIsIUO};

  // The getter/setter types are different. If we cannot update
  // the type, we have to fail.
  if (!canUpdateType)
    return {nullptr, false};

  // Unwrap one level of optionality from each.
  if (Type getterObjectType = getterType->getOptionalObjectType())
    getterType = getterObjectType;
  if (Type setterObjectType = setterType->getOptionalObjectType())
    setterType = setterObjectType;

  // If they are still different, fail.
  // FIXME: We could produce the greatest common supertype of the
  // two types.
  if (!getterType->isEqual(setterType))
    return {nullptr, false};

  // Create an optional of the object type that can be implicitly
  // unwrapped which subsumes both behaviors.
  return {OptionalType::get(setterType), true};
}

/// Add an AvailableAttr to the declaration for the given
/// version range.
static void applyAvailableAttribute(Decl *decl, AvailabilityContext &info,
                                    ASTContext &C) {
  // If the range is "all", this is the same as not having an available
  // attribute.
  if (info.isAlwaysAvailable())
    return;

  llvm::VersionTuple noVersion;
  auto AvAttr = new (C) AvailableAttr(SourceLoc(), SourceRange(),
                                      targetPlatform(C.LangOpts),
                                      /*Message=*/StringRef(),
                                      /*Rename=*/StringRef(),
                                      /*RenameDecl=*/nullptr,
                                      info.getOSVersion().getLowerEndpoint(),
                                      /*IntroducedRange*/SourceRange(),
                                      /*Deprecated=*/noVersion,
                                      /*DeprecatedRange*/SourceRange(),
                                      /*Obsoleted=*/noVersion,
                                      /*ObsoletedRange*/SourceRange(),
                                      PlatformAgnosticAvailabilityKind::None,
                                      /*Implicit=*/false,
                                      /*SPI*/false);

  decl->getAttrs().add(AvAttr);
}

/// Synthesize availability attributes for protocol requirements
/// based on availability of the types mentioned in the requirements.
static void inferProtocolMemberAvailability(ClangImporter::Implementation &impl,
                                            DeclContext *dc, Decl *member) {
  // Don't synthesize attributes if there is already an
  // availability annotation.
  if (member->getAttrs().hasAttribute<AvailableAttr>())
    return;

  auto *valueDecl = dyn_cast<ValueDecl>(member);
  if (!valueDecl)
    return;

  AvailabilityContext requiredRange =
      AvailabilityInference::inferForType(valueDecl->getInterfaceType());

  ASTContext &C = impl.SwiftContext;

  const Decl *innermostDecl = dc->getInnermostDeclarationDeclContext();
  AvailabilityContext containingDeclRange =
      AvailabilityInference::availableRange(innermostDecl, C);

  requiredRange.intersectWith(containingDeclRange);

  applyAvailableAttribute(valueDecl, requiredRange, C);
}

/// Synthesizer callback for the error domain property getter.
static std::pair<BraceStmt *, bool>
synthesizeErrorDomainGetterBody(AbstractFunctionDecl *afd, void *context) {
  auto getterDecl = cast<AccessorDecl>(afd);
  ASTContext &ctx = getterDecl->getASTContext();

  auto contextData =
      llvm::PointerIntPair<ValueDecl *, 1, bool>::getFromOpaqueValue(context);
  auto swiftValueDecl = contextData.getPointer();
  bool isImplicit = contextData.getInt();
  DeclRefExpr *domainDeclRef = new (ctx)
      DeclRefExpr(ConcreteDeclRef(swiftValueDecl), {}, isImplicit);
  domainDeclRef->setType(
    getterDecl->mapTypeIntoContext(swiftValueDecl->getInterfaceType()));

  auto ret = new (ctx) ReturnStmt(SourceLoc(), domainDeclRef);
  return { BraceStmt::create(ctx, SourceLoc(), {ret}, SourceLoc(), isImplicit),
           /*isTypeChecked=*/true };
}

/// Add a domain error member, as required by conformance to
/// _BridgedStoredNSError.
/// \returns true on success, false on failure
static bool addErrorDomain(NominalTypeDecl *swiftDecl,
                           clang::NamedDecl *errorDomainDecl,
                           ClangImporter::Implementation &importer) {
  auto &C = importer.SwiftContext;
  auto swiftValueDecl = dyn_cast_or_null<ValueDecl>(
      importer.importDecl(errorDomainDecl, importer.CurrentVersion));
  auto stringTy = C.getStringType();
  assert(stringTy && "no string type available");
  if (!swiftValueDecl || !swiftValueDecl->getInterfaceType()->isString()) {
    // Couldn't actually import it as an error enum, fall back to enum
    return false;
  }

  bool isStatic = true;
  bool isImplicit = true;

  // Make the property decl
  auto errorDomainPropertyDecl = new (C) VarDecl(
      /*IsStatic*/isStatic, VarDecl::Introducer::Var,
      SourceLoc(), C.Id_errorDomain, swiftDecl);
  errorDomainPropertyDecl->setInterfaceType(stringTy);
  errorDomainPropertyDecl->setAccess(AccessLevel::Public);

  auto *params = ParameterList::createEmpty(C);

  auto getterDecl = AccessorDecl::create(
      C,
      /*FuncLoc=*/SourceLoc(),
      /*AccessorKeywordLoc=*/SourceLoc(), AccessorKind::Get,
      errorDomainPropertyDecl,
      /*StaticLoc=*/SourceLoc(), StaticSpellingKind::None,
      /*Async=*/false, /*AsyncLoc=*/SourceLoc(),
      /*Throws=*/false,
      /*ThrowsLoc=*/SourceLoc(), params, stringTy, swiftDecl);
  getterDecl->setIsObjC(false);
  getterDecl->setIsDynamic(false);
  getterDecl->setIsTransparent(false);

  swiftDecl->addMember(errorDomainPropertyDecl);
  importer.makeComputed(errorDomainPropertyDecl, getterDecl, nullptr);

  getterDecl->setImplicit();
  getterDecl->setAccess(AccessLevel::Public);

  llvm::PointerIntPair<ValueDecl *, 1, bool> contextData(swiftValueDecl,
                                                         isImplicit);
  getterDecl->setBodySynthesizer(synthesizeErrorDomainGetterBody,
                                 contextData.getOpaqueValue());

  return true;
}

/// As addErrorDomain above, but performs a lookup
static bool addErrorDomain(NominalTypeDecl *swiftDecl,
                           StringRef errorDomainName,
                           ClangImporter::Implementation &importer) {
  auto &clangSema = importer.getClangSema();
  clang::IdentifierInfo *errorDomainDeclName =
    &clangSema.getASTContext().Idents.get(errorDomainName);
  clang::LookupResult lookupResult(
      clangSema, clang::DeclarationName(errorDomainDeclName),
      clang::SourceLocation(), clang::Sema::LookupNameKind::LookupOrdinaryName);

  if (!clangSema.LookupName(lookupResult, clangSema.TUScope)) {
    // Couldn't actually import it as an error enum, fall back to enum
    return false;
  }

  auto clangNamedDecl = lookupResult.getAsSingle<clang::NamedDecl>();
  if (!clangNamedDecl) {
    // Couldn't actually import it as an error enum, fall back to enum
    return false;
  }

  return addErrorDomain(swiftDecl, clangNamedDecl, importer);
}

/// Retrieve the property type as determined by the given accessor.
static clang::QualType
getAccessorPropertyType(const clang::FunctionDecl *accessor, bool isSetter,
                        llvm::Optional<unsigned> selfIndex) {
  // Simple case: the property type of the getter is in the return
  // type.
  if (!isSetter) return accessor->getReturnType();

  // For the setter, first check that we have the right number of
  // parameters.
  unsigned numExpectedParams = selfIndex ? 2 : 1;
  if (accessor->getNumParams() != numExpectedParams)
    return clang::QualType();

  // Dig out the parameter for the value.
  unsigned valueIdx = selfIndex ? (1 - *selfIndex) : 0;
  auto param = accessor->getParamDecl(valueIdx);
  return param->getType();
}

/// Whether we should suppress importing the Objective-C generic type params
/// of this class as Swift generic type params.
static bool
shouldSuppressGenericParamsImport(const LangOptions &langOpts,
                                  const clang::ObjCInterfaceDecl *decl) {
  if (decl->hasAttr<clang::SwiftImportAsNonGenericAttr>())
    return true;

  // FIXME: This check is only necessary to keep things working even without
  // the SwiftImportAsNonGeneric API note. Once we can guarantee that that
  // attribute is present in all contexts, we can remove this check.
  auto isFromFoundationModule = [](const clang::Decl *decl) -> bool {
    clang::Module *module = getClangSubmoduleForDecl(decl).value();
    if (!module)
      return false;
    return module->getTopLevelModuleName() == "Foundation";
  };

  if (isFromFoundationModule(decl)) {
    // In Swift 3 we used a hardcoded list of declarations, and made all of
    // their subclasses drop their generic parameters when imported.
    while (decl) {
      StringRef name = decl->getName();
      if (name == "NSArray" || name == "NSDictionary" || name == "NSSet" ||
          name == "NSOrderedSet" || name == "NSEnumerator" ||
          name == "NSMeasurement") {
        return true;
      }
      decl = decl->getSuperClass();
    }
  }

  return false;
}

/// Determine if the given Objective-C instance method should also
/// be imported as a class method.
///
/// Objective-C root class instance methods are also reflected as
/// class methods.
static bool shouldAlsoImportAsClassMethod(FuncDecl *method) {
  // Only instance methods.
  if (!method->isInstanceMember())
    return false;

  // Must be a method within a class or extension thereof.
  auto classDecl = method->getDeclContext()->getSelfClassDecl();
  if (!classDecl)
    return false;

  // The class must not have a superclass.
  if (classDecl->hasSuperclass())
    return false;

  // There must not already be a class method with the same
  // selector.
  auto objcClass =
      cast_or_null<clang::ObjCInterfaceDecl>(classDecl->getClangDecl());
  if (!objcClass)
    return false;

  auto objcMethod = cast_or_null<clang::ObjCMethodDecl>(method->getClangDecl());
  if (!objcMethod)
    return false;
  return !objcClass->getClassMethod(objcMethod->getSelector(),
                                    /*AllowHidden=*/true);
}

static bool
classImplementsProtocol(const clang::ObjCInterfaceDecl *constInterface,
                        const clang::ObjCProtocolDecl *constProto,
                        bool checkCategories) {
  auto interface = const_cast<clang::ObjCInterfaceDecl *>(constInterface);
  auto proto = const_cast<clang::ObjCProtocolDecl *>(constProto);
  return interface->ClassImplementsProtocol(proto, checkCategories);
}

static void
applyPropertyOwnership(VarDecl *prop,
                       clang::ObjCPropertyAttribute::Kind attrs) {
  Type ty = prop->getInterfaceType();
  if (auto innerTy = ty->getOptionalObjectType())
    ty = innerTy;
  if (!ty->is<GenericTypeParamType>() && !ty->isAnyClassReferenceType())
    return;

  ASTContext &ctx = prop->getASTContext();
  if (attrs & clang::ObjCPropertyAttribute::kind_copy) {
    prop->getAttrs().add(new (ctx) NSCopyingAttr(false));
    return;
  }
  if (attrs & clang::ObjCPropertyAttribute::kind_weak) {
    prop->getAttrs().add(new (ctx)
                             ReferenceOwnershipAttr(ReferenceOwnership::Weak));
    prop->setInterfaceType(WeakStorageType::get(
        prop->getInterfaceType(), ctx));
    return;
  }
  if ((attrs & clang::ObjCPropertyAttribute::kind_assign) ||
      (attrs & clang::ObjCPropertyAttribute::kind_unsafe_unretained)) {
    prop->getAttrs().add(
        new (ctx) ReferenceOwnershipAttr(ReferenceOwnership::Unmanaged));
    prop->setInterfaceType(UnmanagedStorageType::get(
        prop->getInterfaceType(), ctx));
    return;
  }
}

/// Does this name refer to a method that might shadow Swift.print?
///
/// As a heuristic, methods that have a base name of 'print' but more than
/// one argument are left alone. These can still shadow Swift.print but are
/// less likely to be confused for it, at least.
static bool isPrintLikeMethod(DeclName name, const DeclContext *dc) {
  if (!name || name.isSpecial() || name.isSimpleName())
    return false;
  if (name.getBaseIdentifier().str() != "print")
    return false;
  if (!dc->isTypeContext())
    return false;
  if (name.getArgumentNames().size() > 1)
    return false;
  return true;
}

using MirroredMethodEntry =
  std::tuple<const clang::ObjCMethodDecl*, ProtocolDecl*, bool /*isAsync*/>;

ImportedType findOptionSetType(clang::QualType type,
                               ClangImporter::Implementation &Impl) {
  ImportedType importedType;
  auto fieldType = type;
  if (auto elaborated = dyn_cast<clang::ElaboratedType>(fieldType))
    fieldType = elaborated->desugar();
  if (auto typedefType = dyn_cast<clang::TypedefType>(fieldType)) {
    if (Impl.isUnavailableInSwift(typedefType->getDecl())) {
      if (auto clangEnum =
              findAnonymousEnumForTypedef(Impl.SwiftContext, typedefType)) {
        // If this fails, it means that we need a stronger predicate for
        // determining the relationship between an enum and typedef.
        assert(
            clangEnum.value()->getIntegerType()->getCanonicalTypeInternal() ==
            typedefType->getCanonicalTypeInternal());
        if (auto swiftEnum = Impl.importDecl(*clangEnum, Impl.CurrentVersion)) {
          importedType = {cast<TypeDecl>(swiftEnum)->getDeclaredInterfaceType(),
                          false};
        }
      }
    }
  }
  return importedType;
}

namespace {
  /// Customized llvm::DenseMapInfo for storing borrowed APSInts.
  struct APSIntRefDenseMapInfo {
    static inline const llvm::APSInt *getEmptyKey() {
      return llvm::DenseMapInfo<const llvm::APSInt *>::getEmptyKey();
    }
    static inline const llvm::APSInt *getTombstoneKey() {
      return llvm::DenseMapInfo<const llvm::APSInt *>::getTombstoneKey();
    }
    static unsigned getHashValue(const llvm::APSInt *ptrVal) {
      assert(ptrVal != getEmptyKey() && ptrVal != getTombstoneKey());
      return llvm::hash_value(*ptrVal);
    }
    static bool isEqual(const llvm::APSInt *lhs, const llvm::APSInt *rhs) {
      if (lhs == rhs) return true;
      if (lhs == getEmptyKey() || rhs == getEmptyKey()) return false;
      if (lhs == getTombstoneKey() || rhs == getTombstoneKey()) return false;
      return *lhs == *rhs;
    }
  };

  /// Search the member tables for this class and its superclasses and try to
  /// identify the nearest VarDecl that serves as a base for an override.  We
  /// have to do this ourselves because Objective-C has no semantic notion of
  /// overrides, and freely allows users to refine the type of any member
  /// property in a derived class.
  ///
  /// The override must be the nearest possible one so there are not breaks
  /// in the override chain. That is, suppose C refines B refines A and each
  /// successively redeclares a member with a different type.  It should be
  /// the case that the nearest override from C is B and from B is A. If the
  /// override point from C were A, then B would record an override on A as
  /// well and we would introduce a semantic ambiguity.
  ///
  /// There is also a special case for finding a method that stomps over a
  /// getter.  If this is the case and no override point is identified, we will
  /// not import the property to force users to explicitly call the method.
  static std::pair<VarDecl *, bool>
  identifyNearestOverriddenDecl(ClangImporter::Implementation &Impl,
                                DeclContext *dc,
                                const clang::ObjCPropertyDecl *decl,
                                Identifier name,
                                ClassDecl *subject) {
    bool foundMethod = false;
    for (; subject; (subject = subject->getSuperclassDecl())) {
      llvm::SmallVector<ValueDecl *, 8> lookup;
      auto foundNames = Impl.MembersForNominal.find(subject);
      if (foundNames != Impl.MembersForNominal.end()) {
        auto foundDecls = foundNames->second.find(name);
        if (foundDecls != foundNames->second.end()) {
          lookup.append(foundDecls->second.begin(), foundDecls->second.end());
        }
      }

      for (auto *&result : lookup) {
        if (auto *fd = dyn_cast<FuncDecl>(result)) {
          if (fd->isInstanceMember() != decl->isInstanceProperty())
            continue;

          // We only care about methods with no arguments, because they can
          // shadow imported properties.
          if (!fd->getName().getArgumentNames().empty())
            continue;

          // async methods don't conflict with properties because of sync/async
          // overloading.
          if (fd->hasAsync())
            continue;

          foundMethod = true;
        } else if (auto *var = dyn_cast<VarDecl>(result)) {
          if (var->isInstanceMember() != decl->isInstanceProperty())
            continue;

          // If the selectors of the getter match in Objective-C, we have an
          // override.
          if (var->getObjCGetterSelector() ==
              Impl.importSelector(decl->getGetterName())) {
            return {var, foundMethod};
          }
        }
      }
    }

    return {nullptr, foundMethod};
  }

  // Attempt to identify the redeclaration of a property.
  //
  // Note that this function does not perform any additional member loading and
  // is therefore subject to the relativistic effects of module import order.
  // That is, suppose that a Clang Module and an Overlay module are in play.
  // Depending on which module loads members first, a redeclaration point may
  // or may not be identifiable.
  VarDecl *
  identifyPropertyRedeclarationPoint(ClangImporter::Implementation &Impl,
                                     const clang::ObjCPropertyDecl *decl,
                                     ClassDecl *subject, Identifier name) {
    llvm::SetVector<Decl *> lookup;
    // First, pull in all available members of the base class so we can catch
    // redeclarations of APIs that are refined for Swift.
    auto currentMembers = subject->getCurrentMembersWithoutLoading();
    lookup.insert(currentMembers.begin(), currentMembers.end());

    // Now pull in any just-imported members from the overrides table.
    auto foundNames = Impl.MembersForNominal.find(subject);
    if (foundNames != Impl.MembersForNominal.end()) {
      auto foundDecls = foundNames->second.find(name);
      if (foundDecls != foundNames->second.end()) {
        lookup.insert(foundDecls->second.begin(), foundDecls->second.end());
      }
    }

    for (auto *result : lookup) {
      auto *var = dyn_cast<VarDecl>(result);
      if (!var)
        continue;

      if (var->isInstanceMember() != decl->isInstanceProperty())
        continue;

      // If the selectors of the getter match in Objective-C, we have a
      // redeclaration.
      if (var->getObjCGetterSelector() ==
          Impl.importSelector(decl->getGetterName())) {
        return var;
      }
    }
    return nullptr;
  }

  /// Convert Clang declarations into the corresponding Swift
  /// declarations.
  class SwiftDeclConverter
    : public clang::ConstDeclVisitor<SwiftDeclConverter, Decl *>
  {
    ClangImporter::Implementation &Impl;
    bool forwardDeclaration = false;
    ImportNameVersion version;
    SwiftDeclSynthesizer synthesizer;

    /// The version that we're being asked to import for. May not be the version
    /// the user requested, as we may be forming an alternate for diagnostic
    /// purposes.
    ImportNameVersion getVersion() const { return version; }

    /// The actual language version the user requested we compile for.
    ImportNameVersion getActiveSwiftVersion() const {
      return Impl.CurrentVersion;
    }

    /// Whether the names we're importing are from the language version the user
    /// requested, or if these are decls from another version
    bool isActiveSwiftVersion() const {
      return getVersion().withConcurrency(false) == getActiveSwiftVersion().withConcurrency(false);
    }

    void recordMemberInContext(const DeclContext *dc, ValueDecl *member) {
      assert(member && "Attempted to record null member!");
      auto *nominal = dc->getSelfNominalTypeDecl();
      auto name = member->getBaseName();
      Impl.MembersForNominal[nominal][name].push_back(member);
    }

    /// Import the name of the given entity.
    ///
    /// This version of importFullName introduces any context-specific
    /// name importing options (e.g., if we're importing the Swift 2 version).
    ///
    /// Note: Use this rather than calling Impl.importFullName directly!
    std::pair<ImportedName, llvm::Optional<ImportedName>>
    importFullName(const clang::NamedDecl *D) {
      ImportNameVersion canonicalVersion = getActiveSwiftVersion();
      if (isa<clang::TypeDecl>(D) || isa<clang::ObjCContainerDecl>(D)) {
        canonicalVersion = ImportNameVersion::forTypes();
      }

      // First, import based on the Swift name of the canonical declaration:
      // the latest version for types and the current version for non-type
      // values. If that fails, we won't do anything.
      auto canonicalName = Impl.importFullName(D, canonicalVersion);
      if (!canonicalName)
        return {ImportedName(), llvm::None};

      if (getVersion() == canonicalVersion) {
        // Make sure we don't try to import the same type twice as canonical.
        if (canonicalVersion != getActiveSwiftVersion()) {
          auto activeName = Impl.importFullName(D, getActiveSwiftVersion());
          if (activeName &&
              activeName.getDeclName() == canonicalName.getDeclName() &&
              activeName.getEffectiveContext().equalsWithoutResolving(
                  canonicalName.getEffectiveContext())) {
            return {ImportedName(), llvm::None};
          }
        }

        return {canonicalName, llvm::None};
      }

      // Special handling when we import using the alternate Swift name.
      //
      // Import using the alternate Swift name. If that fails, or if it's
      // identical to the active Swift name, we won't introduce an alternate
      // Swift name stub declaration.
      auto alternateName = Impl.importFullName(D, getVersion());
      if (!alternateName)
        return {ImportedName(), llvm::None};

      // Importing for concurrency is special in that the same declaration
      // is imported both with a completion handler parameter and as 'async',
      // creating two separate declarations.
      if (getVersion().supportsConcurrency()) {
        // If the resulting name isn't special for concurrency, it's not
        // different.
        if (!alternateName.getAsyncInfo())
          return {ImportedName(), llvm::None};

        // Otherwise, it's a legitimately different import.
        return {alternateName, llvm::None};
      }

      if (alternateName.getDeclName() == canonicalName.getDeclName() &&
          alternateName.getEffectiveContext().equalsWithoutResolving(
              canonicalName.getEffectiveContext())) {
        if (getVersion() == getActiveSwiftVersion()) {
          assert(canonicalVersion != getActiveSwiftVersion());
          return {alternateName, llvm::None};
        }
        return {ImportedName(), llvm::None};
      }

      // Always use the active version as the preferred name, even if the
      // canonical name is a different version.
      ImportedName correctSwiftName =
          Impl.importFullName(D, getActiveSwiftVersion());
      assert(correctSwiftName);

      return {alternateName, correctSwiftName};
    }

    /// Create a declaration name for anonymous enums, unions and
    /// structs.
    ///
    /// Since Swift does not natively support these features, we fake them by
    /// importing them as declarations with generated names. The generated name
    /// is derived from the name of the field in the outer type. Since the
    /// anonymous type is imported as a nested type of the outer type, this
    /// generated name will most likely be unique.
    std::pair<ImportedName, llvm::Optional<ImportedName>>
    getClangDeclName(const clang::TagDecl *decl) {
      // If we have a name for this declaration, use it.
      auto result = importFullName(decl);
      if (result.first)
        return result;

      // If that didn't succeed, check whether this is an anonymous tag declaration
      // with a corresponding typedef-name declaration.
      if (decl->getDeclName().isEmpty()) {
        if (auto *typedefForAnon = decl->getTypedefNameForAnonDecl())
          return importFullName(typedefForAnon);
      }

      return {ImportedName(), llvm::None};
    }

    bool isFactoryInit(ImportedName &name) {
      return name &&
             name.getDeclName().getBaseName() == DeclBaseName::createConstructor() &&
             (name.getInitKind() == CtorInitializerKind::Factory ||
              name.getInitKind() == CtorInitializerKind::ConvenienceFactory);
    }

  public:
    explicit SwiftDeclConverter(ClangImporter::Implementation &impl,
                                ImportNameVersion vers)
      : Impl(impl), version(vers), synthesizer(Impl) { }

    bool hadForwardDeclaration() const {
      return forwardDeclaration;
    }

    Decl *VisitDecl(const clang::Decl *decl) {
      return nullptr;
    }

    Decl *VisitTranslationUnitDecl(const clang::TranslationUnitDecl *decl) {
      // Note: translation units are handled specially by importDeclContext.
      return nullptr;
    }

    Decl *VisitNamespaceDecl(const clang::NamespaceDecl *decl) {
      DeclContext *dc = nullptr;
      // Do not import namespace declarations marked as 'swift_private'.
      if (decl->hasAttr<clang::SwiftPrivateAttr>())
        return nullptr;
      // If this is a top-level namespace, don't put it in the module we're
      // importing, put it in the "__ObjC" module that is implicitly imported.
      if (!decl->getParent()->isNamespace())
        dc = Impl.ImportedHeaderUnit;
      else {
        // This is a nested namespace, so just lookup it's parent normally.
        auto parentNS = cast<clang::NamespaceDecl>(decl->getParent());
        auto parent =
            Impl.importDecl(parentNS, getVersion(), /*UseCanonicalDecl*/ false);
        // The parent namespace might not be imported if it's `swift_private`.
        if (!parent)
            return nullptr;
        dc = cast<EnumDecl>(parent);
      }

      ImportedName importedName;
      std::tie(importedName, std::ignore) = importFullName(decl);
      // If we don't have a name for this declaration, bail. We can't import it.
      if (!importedName)
        return nullptr;

      auto *enumDecl = Impl.createDeclWithClangNode<EnumDecl>(
          decl, AccessLevel::Public, Impl.importSourceLoc(decl->getBeginLoc()),
          importedName.getDeclName().getBaseIdentifier(),
          Impl.importSourceLoc(decl->getLocation()), llvm::None, nullptr, dc);
      // TODO: we only have this for the sid effect of calling
      // "FirstDeclAndLazyMembers.setInt(true)".
      // This should never actually try to use Impl as the member loader,
      // that should all be done via requests.
      enumDecl->setMemberLoader(&Impl, 0);

      // Only import one enum for all redecls of a namespace. Because members
      // are loaded lazily, we can cache all the redecls to prevent the creation
      // of multiple enums.
      for (auto redecl : decl->redecls())
        Impl.ImportedDecls[{redecl, getVersion()}] = enumDecl;

      for (auto redecl : decl->redecls()) {
        // Because a namespaces's decl context is the bridging header, make sure
        // we add them to the bridging header lookup table.
        addEntryToLookupTable(*Impl.BridgingHeaderLookupTable,
                              const_cast<clang::NamespaceDecl *>(redecl),
                              Impl.getNameImporter());
      }

      return enumDecl;
    }

    Decl *VisitUsingDirectiveDecl(const clang::UsingDirectiveDecl *decl) {
      // Never imported.
      return nullptr;
    }

    Decl *VisitNamespaceAliasDecl(const clang::NamespaceAliasDecl *decl) {
      ImportedName importedName;
      llvm::Optional<ImportedName> correctSwiftName;
      std::tie(importedName, correctSwiftName) = importFullName(decl);
      auto name = importedName.getDeclName().getBaseIdentifier();
      if (name.empty())
        return nullptr;

      if (correctSwiftName)
        return importCompatibilityTypeAlias(decl, importedName,
                                            *correctSwiftName);

      auto dc =
          Impl.importDeclContextOf(decl, importedName.getEffectiveContext());
      if (!dc)
        return nullptr;

      auto aliasedDecl =
          Impl.importDecl(decl->getAliasedNamespace(), getActiveSwiftVersion());
      if (!aliasedDecl)
        return nullptr;

      Type aliasedType;
      if (auto aliasedTypeDecl = dyn_cast<TypeDecl>(aliasedDecl))
        aliasedType = aliasedTypeDecl->getDeclaredInterfaceType();
      else if (auto aliasedExtDecl = dyn_cast<ExtensionDecl>(aliasedDecl))
        // This happens if the alias points to its parent namespace.
        aliasedType = aliasedExtDecl->getExtendedType();
      else
        return nullptr;

      auto result = Impl.createDeclWithClangNode<TypeAliasDecl>(
          decl, AccessLevel::Public, Impl.importSourceLoc(decl->getBeginLoc()),
          SourceLoc(), name, Impl.importSourceLoc(decl->getLocation()),
          /*GenericParams=*/nullptr, dc);
      result->setUnderlyingType(aliasedType);

      return result;
    }

    Decl *VisitLabelDecl(const clang::LabelDecl *decl) {
      // Labels are function-local, and therefore never imported.
      return nullptr;
    }

    ClassDecl *importCFClassType(const clang::TypedefNameDecl *decl,
                                 Identifier className, CFPointeeInfo info,
                                 EffectiveClangContext effectiveContext);

    /// Mark the given declaration as an older Swift version variant of the
    /// current name.
    void markAsVariant(Decl *decl, ImportedName correctSwiftName) {
      // Types always import using the latest version. Make sure all names up
      // to that version are considered available.
      if (isa<TypeDecl>(decl)) {
        cast<TypeAliasDecl>(decl)->markAsCompatibilityAlias();

        if (getVersion() >= getActiveSwiftVersion())
          return;
      }

      // If this the active and current Swift versions differ based on
      // concurrency, it's not actually a variant.
      if (getVersion().supportsConcurrency() !=
            getActiveSwiftVersion().supportsConcurrency()) {
        return;
      }

      // TODO: some versions should be deprecated instead of unavailable

      ASTContext &ctx = decl->getASTContext();
      llvm::SmallString<64> renamed;
      {
        // Render a swift_name string.
        llvm::raw_svector_ostream os(renamed);

        // If we're importing a global as a member, we need to provide the
        // effective context.
        Impl.printSwiftName(
            correctSwiftName, getActiveSwiftVersion(),
            /*fullyQualified=*/correctSwiftName.importAsMember(), os);
      }

      DeclAttribute *attr;
      if (isActiveSwiftVersion() || getVersion() == ImportNameVersion::raw()) {
        // "Raw" is the Objective-C name, which was never available in Swift.
        // Variants within the active version are usually declarations that
        // have been superseded, like the accessors of a property.
        attr = AvailableAttr::createPlatformAgnostic(
            ctx, /*Message*/StringRef(), ctx.AllocateCopy(renamed.str()),
            PlatformAgnosticAvailabilityKind::UnavailableInSwift);
      } else {
        unsigned majorVersion = getVersion().majorVersionNumber();
        unsigned minorVersion = getVersion().minorVersionNumber();
        if (getVersion() < getActiveSwiftVersion()) {
          // A Swift 2 name, for example, was obsoleted in Swift 3.
          // However, a Swift 4 name is obsoleted in Swift 4.2.
          // FIXME: it would be better to have a unified place
          // to represent Swift versions for API versioning.
          llvm::VersionTuple obsoletedVersion =
            (majorVersion == 4 && minorVersion < 2)
                ? llvm::VersionTuple(4, 2)
                : llvm::VersionTuple(majorVersion + 1);
          attr = AvailableAttr::createPlatformAgnostic(
              ctx, /*Message*/StringRef(), ctx.AllocateCopy(renamed.str()),
              PlatformAgnosticAvailabilityKind::SwiftVersionSpecific,
              obsoletedVersion);
        } else {
          // Future names are introduced in their future version.
          assert(getVersion() > getActiveSwiftVersion());
          llvm::VersionTuple introducedVersion =
            (majorVersion == 4 && minorVersion == 2)
                ? llvm::VersionTuple(4, 2)
                : llvm::VersionTuple(majorVersion);
          attr = new (ctx) AvailableAttr(
              SourceLoc(), SourceRange(), PlatformKind::none,
              /*Message*/StringRef(), ctx.AllocateCopy(renamed.str()),
              /*RenameDecl=*/nullptr,
              /*Introduced*/introducedVersion, SourceRange(),
              /*Deprecated*/llvm::VersionTuple(), SourceRange(),
              /*Obsoleted*/llvm::VersionTuple(), SourceRange(),
              PlatformAgnosticAvailabilityKind::SwiftVersionSpecific,
              /*Implicit*/false,
              /*SPI*/false);
        }
      }

      decl->getAttrs().add(attr);
      decl->setImplicit();
    }

    /// Create a typealias for the name of a Clang type declaration in an
    /// alternate version of Swift.
    Decl *importCompatibilityTypeAlias(const clang::NamedDecl *decl,
                                       ImportedName compatibilityName,
                                       ImportedName correctSwiftName);

    /// Create a swift_newtype struct corresponding to a typedef. Returns
    /// nullptr if unable.
    Decl *importSwiftNewtype(const clang::TypedefNameDecl *decl,
                             clang::SwiftNewTypeAttr *newtypeAttr,
                             DeclContext *dc, Identifier name);

    Decl *VisitTypedefNameDecl(const clang::TypedefNameDecl *Decl) {
      ImportedName importedName;
      llvm::Optional<ImportedName> correctSwiftName;
      std::tie(importedName, correctSwiftName) = importFullName(Decl);
      auto Name = importedName.getDeclName().getBaseIdentifier();
      if (Name.empty())
        return nullptr;

      // If we've been asked to produce a compatibility stub, handle it via a
      // typealias.
      if (correctSwiftName)
        return importCompatibilityTypeAlias(Decl, importedName,
                                            *correctSwiftName);

      Type SwiftType;
      auto clangDC = Decl->getDeclContext()->getRedeclContext();
      if (clangDC->isTranslationUnit() || clangDC->isStdNamespace()) {
        bool IsError;
        StringRef StdlibTypeName;
        MappedTypeNameKind NameMapping;
        std::tie(SwiftType, StdlibTypeName) =
            getSwiftStdlibType(Decl, Name, Impl, &IsError, NameMapping);

        if (IsError)
          return nullptr;

        // Import 'typedef struct __Blah *BlahRef;' and
        // 'typedef const void *FooRef;' as CF types if they have the
        // right attributes or match our list of known types.
        if (!SwiftType) {
          auto DC = Impl.importDeclContextOf(
              Decl, importedName.getEffectiveContext());
          if (!DC)
            return nullptr;

          if (auto pointee = CFPointeeInfo::classifyTypedef(Decl)) {
            // If the pointee is a record, consider creating a class type.
            if (pointee.isRecord()) {
              auto swiftClass = importCFClassType(
                  Decl, Name, pointee, importedName.getEffectiveContext());
              if (!swiftClass) return nullptr;

              Impl.SpecialTypedefNames[Decl->getCanonicalDecl()] =
                MappedTypeNameKind::DefineAndUse;
              return swiftClass;
            }

            // If the pointee is another CF typedef, create an extra typealias
            // for the name without "Ref", but not a separate type.
            if (pointee.isTypedef()) {
              auto underlying = cast_or_null<TypeDecl>(Impl.importDecl(
                  pointee.getTypedef(), getActiveSwiftVersion()));
              if (!underlying)
                return nullptr;

              // Check for a newtype
              if (auto newtypeAttr =
                      getSwiftNewtypeAttr(Decl, getVersion()))
                if (auto newtype =
                        importSwiftNewtype(Decl, newtypeAttr, DC, Name))
                  return newtype;

              // Create a typealias for this CF typedef.
              TypeAliasDecl *typealias = nullptr;
              typealias = Impl.createDeclWithClangNode<TypeAliasDecl>(
                            Decl, AccessLevel::Public,
                            Impl.importSourceLoc(Decl->getBeginLoc()),
                            SourceLoc(), Name,
                            Impl.importSourceLoc(Decl->getLocation()),
                            /*genericparams*/nullptr, DC);
              typealias->setUnderlyingType(
                  underlying->getDeclaredInterfaceType());

              Impl.SpecialTypedefNames[Decl->getCanonicalDecl()] =
                MappedTypeNameKind::DefineAndUse;
              return typealias;
            }

            // If the pointee is 'void', 'CFTypeRef', bring it
            // in specifically as AnyObject.
            if (pointee.isVoid()) {
              // Create a typealias for this CF typedef.
              TypeAliasDecl *typealias = nullptr;
              typealias = Impl.createDeclWithClangNode<TypeAliasDecl>(
                            Decl, AccessLevel::Public,
                            Impl.importSourceLoc(Decl->getBeginLoc()),
                            SourceLoc(), Name,
                            Impl.importSourceLoc(Decl->getLocation()),
                            /*genericparams*/nullptr, DC);
              typealias->setUnderlyingType(
                Impl.SwiftContext.getAnyObjectType());

              Impl.SpecialTypedefNames[Decl->getCanonicalDecl()] =
                MappedTypeNameKind::DefineAndUse;
              return typealias;
            }
          }
        }

        if (SwiftType) {
          // Note that this typedef-name is special.
          Impl.SpecialTypedefNames[Decl->getCanonicalDecl()] = NameMapping;

          if (NameMapping == MappedTypeNameKind::DoNothing) {
            // Record the remapping using the name of the Clang declaration.
            // This will be useful for type checker diagnostics when
            // a user tries to use the Objective-C/C type instead of the
            // Swift type.
            Impl.SwiftContext.RemappedTypes[Decl->getNameAsString()]
              = SwiftType;

            // Don't create an extra typealias in the imported module because
            // doing so will cause confusion (or even lookup ambiguity) between
            // the name in the imported module and the same name in the
            // standard library.
            if (auto *NAT =
                  dyn_cast<TypeAliasType>(SwiftType.getPointer()))
              return NAT->getDecl();

            auto *NTD = SwiftType->getAnyNominal();
            assert(NTD);
            return NTD;
          }
        }
      }

      auto DC =
          Impl.importDeclContextOf(Decl, importedName.getEffectiveContext());
      if (!DC)
        return nullptr;

      // Check for swift_newtype
      if (!SwiftType)
        if (auto newtypeAttr = getSwiftNewtypeAttr(Decl, getVersion()))
          if (auto newtype = importSwiftNewtype(Decl, newtypeAttr, DC, Name))
            return newtype;

      if (!SwiftType) {
        // Note that the code below checks to see if the typedef allows
        // bridging, i.e. if the imported typealias should name a bridged type
        // or the original C type.
        clang::QualType ClangType = Decl->getUnderlyingType();
        SwiftType = Impl.importTypeIgnoreIUO(
            ClangType, ImportTypeKind::Typedef,
            ImportDiagnosticAdder(Impl, Decl, Decl->getLocation()),
            isInSystemModule(DC), getTypedefBridgeability(Decl),
            getImportTypeAttrs(Decl), OTK_Optional);
      }

      if (!SwiftType)
        return nullptr;

      auto Loc = Impl.importSourceLoc(Decl->getLocation());
      auto Result = Impl.createDeclWithClangNode<TypeAliasDecl>(Decl,
                                      AccessLevel::Public,
                                      Impl.importSourceLoc(Decl->getBeginLoc()),
                                      SourceLoc(), Name,
                                      Loc,
                                      /*genericparams*/nullptr, DC);

      Result->setUnderlyingType(SwiftType);

      // Make Objective-C's 'id' unavailable.
      if (Impl.SwiftContext.LangOpts.EnableObjCInterop && isObjCId(Decl)) {
        auto attr = AvailableAttr::createPlatformAgnostic(
                      Impl.SwiftContext,
                      "'id' is not available in Swift; use 'Any'", "",
                      PlatformAgnosticAvailabilityKind::UnavailableInSwift);
        Result->getAttrs().add(attr);
      }

      return Result;
    }

    Decl *
    VisitUnresolvedUsingTypenameDecl(const
                                     clang::UnresolvedUsingTypenameDecl *decl) {
      // Note: only occurs in templates.
      return nullptr;
    }

    /// Import an NS_ENUM constant as a case of a Swift enum.
    Decl *importEnumCase(const clang::EnumConstantDecl *decl,
                         const clang::EnumDecl *clangEnum,
                         EnumDecl *theEnum,
                         Decl *swift3Decl = nullptr);

    /// Import an NS_OPTIONS constant as a static property of a Swift struct.
    ///
    /// This is also used to import enum case aliases.
    Decl *importOptionConstant(const clang::EnumConstantDecl *decl,
                               const clang::EnumDecl *clangEnum,
                               NominalTypeDecl *theStruct);

    /// Import \p alias as an alias for the imported constant \p original.
    ///
    /// This builds the getter in a way that's compatible with switch
    /// statements. Changing the body here may require changing
    /// TypeCheckPattern.cpp as well.
    Decl *importEnumCaseAlias(Identifier name,
                              const clang::EnumConstantDecl *alias,
                              ValueDecl *original,
                              const clang::EnumDecl *clangEnum,
                              NominalTypeDecl *importedEnum,
                              DeclContext *importIntoDC = nullptr);

    NominalTypeDecl *importAsOptionSetType(DeclContext *dc,
                                           Identifier name,
                                           const clang::EnumDecl *decl);

    Decl *VisitEnumDecl(const clang::EnumDecl *decl) {
      decl = decl->getDefinition();
      if (!decl) {
        forwardDeclaration = true;
        return nullptr;
      }

      // Don't import nominal types that are over-aligned.
      if (Impl.isOverAligned(decl))
        return nullptr;

      ImportedName importedName;
      llvm::Optional<ImportedName> correctSwiftName;
      std::tie(importedName, correctSwiftName) = getClangDeclName(decl);
      if (!importedName)
        return nullptr;

      // If we've been asked to produce a compatibility stub, handle it via a
      // typealias.
      if (correctSwiftName)
        return importCompatibilityTypeAlias(decl, importedName,
                                            *correctSwiftName);

      auto dc =
          Impl.importDeclContextOf(decl, importedName.getEffectiveContext());
      if (!dc)
        return nullptr;

      auto name = importedName.getDeclName().getBaseIdentifier();

      // Create the enum declaration and record it.
      ImportDiagnosticAdder addDiag(Impl, decl, decl->getLocation());
      StructDecl *errorWrapper = nullptr;
      NominalTypeDecl *result;
      auto enumInfo = Impl.getEnumInfo(decl);
      auto enumKind = enumInfo.getKind();
      switch (enumKind) {
      case EnumKind::Constants: {
        // There is no declaration. Rather, the type is mapped to the
        // underlying type.
        return nullptr;
      }

      case EnumKind::Unknown: {
        // Compute the underlying type of the enumeration.
        auto underlyingType = Impl.importTypeIgnoreIUO(
            decl->getIntegerType(), ImportTypeKind::Enum, addDiag,
            isInSystemModule(dc), Bridgeability::None, ImportTypeAttrs());
        if (!underlyingType)
          return nullptr;

        auto Loc = Impl.importSourceLoc(decl->getLocation());
        auto structDecl = Impl.createDeclWithClangNode<StructDecl>(
            decl, AccessLevel::Public, Loc, name, Loc, llvm::None, nullptr, dc);

        auto options = getDefaultMakeStructRawValuedOptions();
        options |= MakeStructRawValuedFlags::MakeUnlabeledValueInit;
        options -= MakeStructRawValuedFlags::IsLet;
        options -= MakeStructRawValuedFlags::IsImplicit;

        synthesizer.makeStructRawValued(
            structDecl, underlyingType,
            {KnownProtocolKind::RawRepresentable, KnownProtocolKind::Equatable},
            options, /*setterAccess=*/AccessLevel::Public);

        result = structDecl;
        break;
      }

      case EnumKind::NonFrozenEnum:
      case EnumKind::FrozenEnum: {
        auto &C = Impl.SwiftContext;
        EnumDecl *nativeDecl;
        bool declaredNative = hasNativeSwiftDecl(decl, name, dc, nativeDecl);
        if (declaredNative && nativeDecl)
          return nativeDecl;

        // Compute the underlying type.
        auto underlyingType = Impl.importTypeIgnoreIUO(
            decl->getIntegerType(), ImportTypeKind::Enum, addDiag,
            isInSystemModule(dc), Bridgeability::None, ImportTypeAttrs());
        if (!underlyingType)
          return nullptr;

        /// Basic information about the enum type we're building.
        Identifier enumName = name;
        DeclContext *enumDC = dc;
        SourceLoc loc = Impl.importSourceLoc(decl->getBeginLoc());

        // If this is an error enum, form the error wrapper type,
        // which is a struct containing an NSError instance.
        ProtocolDecl *bridgedNSError = nullptr;
        ClassDecl *nsErrorDecl = nullptr;
        ProtocolDecl *errorCodeProto = nullptr;
        if (enumInfo.isErrorEnum() &&
            (bridgedNSError =
               C.getProtocol(KnownProtocolKind::BridgedStoredNSError)) &&
            (nsErrorDecl = C.getNSErrorDecl()) &&
            (errorCodeProto =
               C.getProtocol(KnownProtocolKind::ErrorCodeProtocol))) {
          // Create the wrapper struct.
          errorWrapper =
              new (C) StructDecl(loc, name, loc, llvm::None, nullptr, dc);
          errorWrapper->setAccess(AccessLevel::Public);
          errorWrapper->getAttrs().add(
            new (Impl.SwiftContext) FrozenAttr(/*IsImplicit*/true));

          StringRef nameForMangling;
          ClangImporterSynthesizedTypeAttr::Kind relatedEntityKind;
          if (decl->getDeclName().isEmpty()) {
            nameForMangling = decl->getTypedefNameForAnonDecl()->getName();
            relatedEntityKind =
                ClangImporterSynthesizedTypeAttr::Kind::NSErrorWrapperAnon;
          } else {
            nameForMangling = decl->getName();
            relatedEntityKind =
                ClangImporterSynthesizedTypeAttr::Kind::NSErrorWrapper;
          }
          errorWrapper->getAttrs().add(new (C) ClangImporterSynthesizedTypeAttr(
              nameForMangling, relatedEntityKind));

          // Add inheritance clause.
          Impl.addSynthesizedProtocolAttrs(
              errorWrapper, {KnownProtocolKind::BridgedStoredNSError});

          // Create the _nsError member.
          //   public let _nsError: NSError
          auto nsErrorType = nsErrorDecl->getDeclaredInterfaceType();
          auto nsErrorProp = new (C) VarDecl(/*IsStatic*/false,
                                             VarDecl::Introducer::Let,
                                             loc, C.Id_nsError,
                                             errorWrapper);
          nsErrorProp->setImplicit();
          nsErrorProp->setAccess(AccessLevel::Public);
          nsErrorProp->setInterfaceType(nsErrorType);

          // Create a pattern binding to describe the variable.
          Pattern *nsErrorPattern =
              synthesizer.createTypedNamedPattern(nsErrorProp);

          auto *nsErrorBinding = PatternBindingDecl::createImplicit(
              C, StaticSpellingKind::None, nsErrorPattern, /*InitExpr*/ nullptr,
              /*ParentDC*/ errorWrapper, /*VarLoc*/ loc);
          errorWrapper->addMember(nsErrorProp);
          errorWrapper->addMember(nsErrorBinding);

          // Create the _nsError initializer.
          //   public init(_nsError error: NSError)
          VarDecl *members[1] = {nsErrorProp};
          auto nsErrorInit =
              synthesizer.createValueConstructor(errorWrapper, members,
                                                 /*wantCtorParamNames=*/true,
                                                 /*wantBody=*/true);
          errorWrapper->addMember(nsErrorInit);

          // Add the domain error member.
          //   public static var errorDomain: String { return error-domain }
          addErrorDomain(errorWrapper, enumInfo.getErrorDomain(), Impl);

          // Note: the Code will be added after it's created.

          // The enum itself will be nested within the error wrapper,
          // and be named Code.
          enumDC = errorWrapper;
          enumName = C.Id_Code;
        }

        // Create the enumeration.
        auto enumDecl = Impl.createDeclWithClangNode<EnumDecl>(
            decl, AccessLevel::Public, loc, enumName,
            Impl.importSourceLoc(decl->getLocation()), llvm::None, nullptr,
            enumDC);
        enumDecl->setHasFixedRawValues();

        // Annotate as 'frozen' if appropriate.
        if (enumKind == EnumKind::FrozenEnum)
          enumDecl->getAttrs().add(new (C) FrozenAttr(/*implicit*/false));

        // Set up the C underlying type as its Swift raw type.
        enumDecl->setRawType(underlyingType);

        // Add the C name.
        addObjCAttribute(enumDecl,
                         Impl.importIdentifier(decl->getIdentifier()));

        // Add protocol declarations to the enum declaration.
        SmallVector<InheritedEntry, 2> inheritedTypes;
        inheritedTypes.push_back(
            InheritedEntry(TypeLoc::withoutLoc(underlyingType)));
        enumDecl->setInherited(C.AllocateCopy(inheritedTypes));

        if (errorWrapper) {
          Impl.addSynthesizedProtocolAttrs(
              enumDecl, {KnownProtocolKind::ErrorCodeProtocol,
                         KnownProtocolKind::RawRepresentable});
        } else {
          Impl.addSynthesizedProtocolAttrs(
              enumDecl, {KnownProtocolKind::RawRepresentable});
        }

        // Provide custom implementations of the init(rawValue:) and rawValue
        // conversions that just do a bitcast. We can't reliably filter a
        // C enum without additional knowledge that the type has no
        // undeclared values, and won't ever add cases.
        auto rawValueConstructor =
            synthesizer.makeEnumRawValueConstructor(enumDecl);

        auto varName = C.Id_rawValue;
        auto rawValue = new (C) VarDecl(/*IsStatic*/false,
                                        VarDecl::Introducer::Var,
                                        SourceLoc(), varName,
                                        enumDecl);
        rawValue->setImplicit();
        rawValue->setAccess(AccessLevel::Public);
        rawValue->setSetterAccess(AccessLevel::Private);
        rawValue->setInterfaceType(underlyingType);

        // Create a pattern binding to describe the variable.
        Pattern *varPattern = synthesizer.createTypedNamedPattern(rawValue);

        auto *rawValueBinding = PatternBindingDecl::createImplicit(
            C, StaticSpellingKind::None, varPattern, /*InitExpr*/ nullptr,
            enumDecl);

        synthesizer.makeEnumRawValueGetter(enumDecl, rawValue);

        enumDecl->addMember(rawValueConstructor);
        enumDecl->addMember(rawValue);
        enumDecl->addMember(rawValueBinding);

        Impl.addSynthesizedTypealias(enumDecl, C.Id_RawValue, underlyingType);
        Impl.RawTypes[enumDecl] = underlyingType;

        // If we have an error wrapper, finish it up now that its
        // nested enum has been constructed.
        if (errorWrapper) {
          // Add the ErrorType alias:
          //   public typealias ErrorType
          auto alias = Impl.createDeclWithClangNode<TypeAliasDecl>(
                         decl,
                         AccessLevel::Public, loc, SourceLoc(),
                         C.Id_ErrorType, loc,
                         /*genericparams=*/nullptr, enumDecl);
          alias->setUnderlyingType(errorWrapper->getDeclaredInterfaceType());
          enumDecl->addMember(alias);

          // Add the 'Code' enum to the error wrapper.
          errorWrapper->addMember(enumDecl);
          Impl.addAlternateDecl(enumDecl, errorWrapper);

          // Stash the 'Code' enum so we can find it later.
          Impl.ErrorCodeEnums[errorWrapper] = enumDecl;
        }

        // The enumerators go into this enumeration.
        result = enumDecl;
        break;
      }

      case EnumKind::Options: {
        result = importAsOptionSetType(dc, name, decl);
        if (!result)
          return nullptr;

        // HACK: Make sure PrintAsClang always omits the 'enum' tag for
        // option set enums.
        Impl.DeclsWithSuperfluousTypedefs.insert(decl);
        break;
      }
      }

      const clang::EnumDecl *canonicalClangDecl = decl->getCanonicalDecl();
      Impl.ImportedDecls[{canonicalClangDecl, getVersion()}] = result;

      // Import each of the enumerators.

      bool addEnumeratorsAsMembers;
      switch (enumKind) {
      case EnumKind::Constants:
      case EnumKind::Unknown:
        addEnumeratorsAsMembers = false;
        break;
      case EnumKind::Options:
      case EnumKind::NonFrozenEnum:
      case EnumKind::FrozenEnum:
        addEnumeratorsAsMembers = true;
        break;
      }

      llvm::SmallDenseMap<const llvm::APSInt *,
                          PointerUnion<const clang::EnumConstantDecl *,
                                       EnumElementDecl *>, 8,
                          APSIntRefDenseMapInfo> canonicalEnumConstants;

      if (enumKind == EnumKind::NonFrozenEnum ||
          enumKind == EnumKind::FrozenEnum) {
        for (auto constant : decl->enumerators()) {
          if (Impl.isUnavailableInSwift(constant))
            continue;
          canonicalEnumConstants.insert({&constant->getInitVal(), constant});
        }
      }

      auto contextIsEnum = [&](const ImportedName &name) -> bool {
        EffectiveClangContext importContext = name.getEffectiveContext();
        switch (importContext.getKind()) {
        case EffectiveClangContext::DeclContext:
          return importContext.getAsDeclContext() == canonicalClangDecl;
        case EffectiveClangContext::TypedefContext: {
          auto *typedefName = importContext.getTypedefName();
          clang::QualType underlyingTy = typedefName->getUnderlyingType();
          return underlyingTy->getAsTagDecl() == canonicalClangDecl;
        }
        case EffectiveClangContext::UnresolvedContext:
          // Assume this is a context other than the enum.
          return false;
        }
        llvm_unreachable("unhandled kind");
      };

      for (auto constant : decl->enumerators()) {
        Decl *enumeratorDecl = nullptr;
        TinyPtrVector<Decl *> variantDecls;
        switch (enumKind) {
        case EnumKind::Constants:
        case EnumKind::Unknown:
          Impl.forEachDistinctName(constant,
                                   [&](ImportedName newName,
                                       ImportNameVersion nameVersion) -> bool {
            Decl *imported = Impl.importDecl(constant, nameVersion);
            if (!imported)
              return false;
            if (nameVersion == getActiveSwiftVersion())
              enumeratorDecl = imported;
            else
              variantDecls.push_back(imported);
            return true;
          });
          break;
        case EnumKind::Options:
          Impl.forEachDistinctName(constant,
                                   [&](ImportedName newName,
                                       ImportNameVersion nameVersion) -> bool {
            if (!contextIsEnum(newName))
              return true;
            SwiftDeclConverter converter(Impl, nameVersion);
            Decl *imported =
                converter.importOptionConstant(constant, decl, result);
            if (!imported)
              return false;
            if (nameVersion == getActiveSwiftVersion())
              enumeratorDecl = imported;
            else
              variantDecls.push_back(imported);
            return true;
          });
          break;
        case EnumKind::NonFrozenEnum:
        case EnumKind::FrozenEnum: {
          auto canonicalCaseIter =
            canonicalEnumConstants.find(&constant->getInitVal());

          if (canonicalCaseIter == canonicalEnumConstants.end()) {
            // Unavailable declarations get no special treatment.
            enumeratorDecl =
                SwiftDeclConverter(Impl, getActiveSwiftVersion())
                    .importEnumCase(constant, decl, cast<EnumDecl>(result));
          } else {
            const clang::EnumConstantDecl *unimported =
                canonicalCaseIter->
                  second.dyn_cast<const clang::EnumConstantDecl *>();

            // Import the canonical enumerator for this case first.
            if (unimported) {
              enumeratorDecl = SwiftDeclConverter(Impl, getActiveSwiftVersion())
                  .importEnumCase(unimported, decl, cast<EnumDecl>(result));
              if (enumeratorDecl) {
                canonicalCaseIter->getSecond() =
                    cast<EnumElementDecl>(enumeratorDecl);
              }
            } else {
              enumeratorDecl =
                  canonicalCaseIter->second.get<EnumElementDecl *>();
            }

            if (unimported != constant && enumeratorDecl) {
              ImportedName importedName =
                  Impl.importFullName(constant, getActiveSwiftVersion());
              Identifier name = importedName.getDeclName().getBaseIdentifier();
              if (name.empty()) {
                // Clear the existing declaration so we don't try to process it
                // twice later.
                enumeratorDecl = nullptr;
              } else {
                auto original = cast<ValueDecl>(enumeratorDecl);
                enumeratorDecl = importEnumCaseAlias(name, constant, original,
                                                     decl, result);
              }
            }
          }

          Impl.forEachDistinctName(constant,
                                   [&](ImportedName newName,
                                       ImportNameVersion nameVersion) -> bool {
            if (nameVersion == getActiveSwiftVersion())
              return true;
            if (!contextIsEnum(newName))
              return true;
            SwiftDeclConverter converter(Impl, nameVersion);
            Decl *imported =
                converter.importEnumCase(constant, decl, cast<EnumDecl>(result),
                                         enumeratorDecl);
            if (!imported)
              return false;
            variantDecls.push_back(imported);
            return true;
          });
          break;
        }
        }
        if (!enumeratorDecl)
          continue;

        if (addEnumeratorsAsMembers) {
          // Add a member enumerator to the given nominal type.
          auto addDecl = [&](NominalTypeDecl *nominal, Decl *decl) {
            if (!decl) return;
            nominal->addMember(decl);
          };

          addDecl(result, enumeratorDecl);
          for (auto *variant : variantDecls)
            addDecl(result, variant);

          // If there is an error wrapper, add an alias within the
          // wrapper to the corresponding value within the enumerator
          // context.
          if (errorWrapper) {
            auto enumeratorValue = cast<ValueDecl>(enumeratorDecl);
            auto name = enumeratorValue->getBaseIdentifier();
            auto alias = importEnumCaseAlias(name,
                                             constant,
                                             enumeratorValue,
                                             decl,
                                             result,
                                             errorWrapper);
            addDecl(errorWrapper, alias);
          }
        }
      }

      return result;
    }

    bool recordHasReferenceSemantics(const clang::RecordDecl *decl) {
      return ClangImporter::Implementation::recordHasReferenceSemantics(
          decl, Impl.SwiftContext);
    }

    bool recordHasMoveOnlySemantics(const clang::RecordDecl *decl) {
      auto semanticsKind = evaluateOrDefault(
          Impl.SwiftContext.evaluator,
          CxxRecordSemantics({decl, Impl.SwiftContext}), {});
      return semanticsKind == CxxRecordSemanticsKind::MoveOnly;
    }

    Decl *VisitRecordDecl(const clang::RecordDecl *decl) {
      // Track whether this record contains fields we can't reference in Swift
      // as stored properties.
      bool hasUnreferenceableStorage = false;

      // Track whether this record contains fields that can't be zero-
      // initialized.
      bool hasZeroInitializableStorage = true;

      // Track whether all fields in this record can be referenced in Swift,
      // either as stored or computed properties, in which case the record type
      // gets a memberwise initializer.
      bool hasMemberwiseInitializer = true;

      if (decl->isUnion()) {
        hasUnreferenceableStorage = true;

        // We generate initializers specially for unions below.
        hasMemberwiseInitializer = false;
      }

      // FIXME: Skip Microsoft __interfaces.
      if (decl->isInterface())
        return nullptr;

      if (!decl->getDefinition()) {
        Impl.addImportDiagnostic(
            decl,
            Diagnostic(diag::incomplete_record, Impl.SwiftContext.AllocateCopy(
                                                    decl->getNameAsString())),
            decl->getLocation());
      }

      // FIXME: Figure out how to deal with incomplete types, since that
      // notion doesn't exist in Swift.
      decl = decl->getDefinition();
      if (!decl) {
        forwardDeclaration = true;
        return nullptr;
      }

      // TODO(https://github.com/apple/swift/issues/56206): Fix this once we support dependent types.
      if (decl->getTypeForDecl()->isDependentType() &&
          !Impl.importSymbolicCXXDecls) {
        Impl.addImportDiagnostic(
            decl, Diagnostic(
                      diag::record_is_dependent,
                      Impl.SwiftContext.AllocateCopy(decl->getNameAsString())),
            decl->getLocation());
        return nullptr;
      }

      // Don't import nominal types that are over-aligned.
      if (Impl.isOverAligned(decl)) {
        Impl.addImportDiagnostic(
            decl, Diagnostic(
                      diag::record_over_aligned,
                      Impl.SwiftContext.AllocateCopy(decl->getNameAsString())),
            decl->getLocation());
        return nullptr;
      }

      auto isNonTrivialDueToAddressDiversifiedPtrAuth =
          [](const clang::RecordDecl *decl) {
            for (auto *field : decl->fields()) {
              if (!field->getType().isNonTrivialToPrimitiveCopy()) {
                continue;
              }
              if (field->getType().isNonTrivialToPrimitiveCopy() !=
                  clang::QualType::PCK_PtrAuth) {
                return false;
              }
            }
            return true;
          };

      bool isNonTrivialPtrAuth = false;
      // FIXME: We should actually support strong ARC references and similar in
      // C structs. That'll require some SIL and IRGen work, though.
      if (decl->isNonTrivialToPrimitiveCopy() ||
          decl->isNonTrivialToPrimitiveDestroy()) {
        isNonTrivialPtrAuth = Impl.SwiftContext.SILOpts
                                  .EnableImportPtrauthFieldFunctionPointers &&
                              isNonTrivialDueToAddressDiversifiedPtrAuth(decl);
        if (!isNonTrivialPtrAuth) {
          // Note that there is a third predicate related to these,
          // isNonTrivialToPrimitiveDefaultInitialize. That one's not important
          // for us because Swift never "trivially default-initializes" a struct
          // (i.e. uses whatever bits were lying around as an initial value).

          // FIXME: It would be nice to instead import the declaration but mark
          // it as unavailable, but then it might get used as a type for an
          // imported function and the developer would be able to use it without
          // referencing the name, which would sidestep our availability
          // diagnostics.
          Impl.addImportDiagnostic(
              decl,
              Diagnostic(
                  diag::record_non_trivial_copy_destroy,
                  Impl.SwiftContext.AllocateCopy(decl->getNameAsString())),
              decl->getLocation());
          return nullptr;
        }
      }

      // Import the name.
      ImportedName importedName;
      llvm::Optional<ImportedName> correctSwiftName;
      std::tie(importedName, correctSwiftName) = getClangDeclName(decl);
      if (!importedName)
        return nullptr;

      // If we've been asked to produce a compatibility stub, handle it via a
      // typealias.
      if (correctSwiftName)
        return importCompatibilityTypeAlias(decl, importedName,
                                            *correctSwiftName);

      auto dc =
          Impl.importDeclContextOf(decl, importedName.getEffectiveContext());
      if (!dc) {
        Impl.addImportDiagnostic(
            decl, Diagnostic(
                      diag::record_parent_unimportable,
                      Impl.SwiftContext.AllocateCopy(decl->getNameAsString())),
            decl->getLocation());
        return nullptr;
      }

      // Create the struct declaration and record it.
      auto name = importedName.getDeclName().getBaseIdentifier();
      NominalTypeDecl *result = nullptr;
      // Try to find an already-imported struct. This case happens any time
      // there are nested structs. The "Parent" struct will import the "Child"
      // struct at which point it attempts to import its decl context which is
      // the "Parent" struct. Without trying to look up already-imported structs
      // this will cause an infinite loop.
      auto alreadyImportedResult =
          Impl.ImportedDecls.find({decl->getCanonicalDecl(), getVersion()});
      if (alreadyImportedResult != Impl.ImportedDecls.end())
        return alreadyImportedResult->second;

      auto loc = Impl.importSourceLoc(decl->getLocation());
      if (recordHasReferenceSemantics(decl))
        result = Impl.createDeclWithClangNode<ClassDecl>(
            decl, AccessLevel::Public, loc, name, loc,
            ArrayRef<InheritedEntry>{}, nullptr, dc, false);
      else
        result = Impl.createDeclWithClangNode<StructDecl>(
            decl, AccessLevel::Public, loc, name, loc, llvm::None, nullptr, dc);
      Impl.ImportedDecls[{decl->getCanonicalDecl(), getVersion()}] = result;

      if (recordHasMoveOnlySemantics(decl)) {
        if (!Impl.SwiftContext.LangOpts.hasFeature(Feature::MoveOnly)) {
          Impl.addImportDiagnostic(
              decl, Diagnostic(
                        diag::move_only_requires_move_only,
                        Impl.SwiftContext.AllocateCopy(decl->getNameAsString())),
              decl->getLocation());
          return nullptr;
        }

        result->getAttrs().add(new (Impl.SwiftContext)
                                   MoveOnlyAttr(/*Implicit=*/true));
      }

      // FIXME: Figure out what to do with superclasses in C++. One possible
      // solution would be to turn them into members and add conversion
      // functions.

      // Import each of the members.
      SmallVector<VarDecl *, 4> members;
      SmallVector<FuncDecl *, 4> methods;
      SmallVector<ConstructorDecl *, 4> ctors;

      // The name of every member.
      llvm::DenseSet<StringRef> allMemberNames;

      bool hasConstOperatorStar = false;
      for (auto member : decl->decls()) {
        if (auto method = dyn_cast<clang::CXXMethodDecl>(member)) {
          if (method->getOverloadedOperator() ==
                  clang::OverloadedOperatorKind::OO_Star &&
              method->param_empty() && method->isConst())
            hasConstOperatorStar = true;
        }
      }
      bool hasSynthesizedPointeeProperty = false;

      // FIXME: Import anonymous union fields and support field access when
      // it is nested in a struct.
      for (auto m : decl->decls()) {
        if (isa<clang::AccessSpecDecl>(m)) {
          // The presence of AccessSpecDecls themselves does not influence
          // whether we can generate a member-wise initializer.
          continue;
        }

        if (auto friendDecl = dyn_cast<clang::FriendDecl>(m)) {
          if (friendDecl->getFriendDecl()) {
            m = friendDecl->getFriendDecl();

            auto lookupTable = Impl.findLookupTable(decl);
            addEntryToLookupTable(*lookupTable, friendDecl->getFriendDecl(),
                                  Impl.getNameImporter());
          }
        }

        auto nd = dyn_cast<clang::NamedDecl>(m);
        if (!nd) {
          // We couldn't import the member, so we can't reference it in Swift.
          hasUnreferenceableStorage = true;
          hasMemberwiseInitializer = false;
          continue;
        }

        if (auto field = dyn_cast<clang::FieldDecl>(nd)) {
          // Non-nullable pointers can't be zero-initialized.
          if (auto nullability =
                  field->getType()->getNullability(Impl.getClangASTContext())) {
            if (*nullability == clang::NullabilityKind::NonNull)
              hasZeroInitializableStorage = false;
          }
          // TODO: If we had the notion of a closed enum with no private
          // cases or resilience concerns, then complete NS_ENUMs with
          // no case corresponding to zero would also not be zero-
          // initializable.

          // Unnamed bitfields are just for padding and should not
          // inhibit creation of a memberwise initializer.
          if (field->isUnnamedBitfield()) {
            hasUnreferenceableStorage = true;
            continue;
          }
        }

        Decl *member = Impl.importDecl(nd, getActiveSwiftVersion());

        if (!member) {
          if (!isa<clang::TypeDecl>(nd) && !isa<clang::FunctionDecl>(nd)) {
            // We don't know what this member is.
            // Assume it may be important in C.
            hasUnreferenceableStorage = true;
            hasMemberwiseInitializer = false;
          }
          continue;
        }

        if (nd->getDeclName().isIdentifier())
          allMemberNames.insert(nd->getName());

        if (isa<TypeDecl>(member)) {
          // TODO: we have a problem lazily looking up unnamed members, so we
          // add them here.
          if (isa<clang::RecordDecl>(nd) &&
              !cast<clang::RecordDecl>(nd)->hasNameForLinkage())
            result->addMemberToLookupTable(member);
          continue;
        }

        if (auto CD = dyn_cast<ConstructorDecl>(member)) {
          ctors.push_back(CD);
          continue;
        }

        if (auto MD = dyn_cast<FuncDecl>(member)) {
          if (auto cxxMethod = dyn_cast<clang::CXXMethodDecl>(m)) {
            auto cxxOperatorKind = cxxMethod->getOverloadedOperator();

            if (cxxOperatorKind == clang::OO_Star && cxxMethod->param_empty()) {
              // This is a dereference operator. We synthesize a computed
              // property called `pointee` for it.

              // If this record has multiple overloads of `operator*`, prefer
              // the const overload if it exists.
              if ((cxxMethod->isConst() || !hasConstOperatorStar) &&
                  !hasSynthesizedPointeeProperty) {
                VarDecl *pointeeProperty =
                    synthesizer.makeDereferencedPointeeProperty(MD);
                result->addMember(pointeeProperty);
                hasSynthesizedPointeeProperty = true;
              }

              Impl.markUnavailable(MD, "use .pointee property");
              MD->overwriteAccess(AccessLevel::Private);
            } else if (cxxOperatorKind ==
                       clang::OverloadedOperatorKind::OO_PlusPlus) {
              // Make sure the type is not a foreign reference type.
              // We cannot handle `operator++` for those types, since the
              // current implementation creates a new instance of the type.
              if (cxxMethod->param_empty() && !isa<ClassDecl>(result)) {
                // This is a pre-increment operator. We synthesize a
                // non-mutating function called `successor() -> Self`.
                FuncDecl *successorFunc = synthesizer.makeSuccessorFunc(MD);
                result->addMember(successorFunc);

                Impl.markUnavailable(MD, "use .successor()");
              } else {
                Impl.markUnavailable(MD, "unable to create .successor() func");
              }
              MD->overwriteAccess(AccessLevel::Private);
            }
            // Check if this method _is_ an overloaded operator but is not a
            // call / subscript / dereference / increment. Those
            // operators do not need static versions.
            else if (cxxOperatorKind !=
                         clang::OverloadedOperatorKind::OO_None &&
                     cxxOperatorKind !=
                         clang::OverloadedOperatorKind::OO_PlusPlus &&
                     cxxOperatorKind !=
                         clang::OverloadedOperatorKind::OO_Call &&
                     cxxOperatorKind !=
                         clang::OverloadedOperatorKind::OO_Subscript) {

              auto opFuncDecl = synthesizer.makeOperator(MD, cxxMethod);

              Impl.addAlternateDecl(MD, opFuncDecl);

              auto msg = "use " + std::string{clang::getOperatorSpelling(cxxOperatorKind)} + " instead";
              Impl.markUnavailable(MD,msg);

              // Make the actual member operator private.
              MD->overwriteAccess(AccessLevel::Private);

              // Make sure the synthesized decl can be found by lookupDirect.
              result->addMemberToLookupTable(opFuncDecl);

              addEntryToLookupTable(*Impl.findLookupTable(decl), cxxMethod,
                                    Impl.getNameImporter());
            }
          }
          methods.push_back(MD);
          continue;
        }

        if (isa<VarDecl>(member) && isa<clang::CXXMethodDecl>(nd)) {
          result->addMember(member);
          continue;
        }

        members.push_back(cast<VarDecl>(member));
      }

      bool hasReferenceableFields = !members.empty();
      for (auto member : members) {
        auto nd = cast<clang::NamedDecl>(member->getClangDecl());
        // Bitfields are imported as computed properties with Clang-generated
        // accessors.
        bool isBitField = false;
        if (auto field = dyn_cast<clang::FieldDecl>(nd)) {
          if (field->isBitField()) {
            // We can't represent this struct completely in SIL anymore,
            // but we're still able to define a memberwise initializer.
            hasUnreferenceableStorage = true;
            isBitField = true;

            synthesizer.makeBitFieldAccessors(
                const_cast<clang::RecordDecl *>(decl), result,
                const_cast<clang::FieldDecl *>(field), member);
          }
        }

        if (auto ind = dyn_cast<clang::IndirectFieldDecl>(nd)) {
          // Indirect fields are created as computed property accessible the
          // fields on the anonymous field from which they are injected.
          synthesizer.makeIndirectFieldAccessors(ind, members, result, member);
        } else if (decl->isUnion() && !isBitField) {
          // Union fields should only be available indirectly via a computed
          // property. Since the union is made of all of the fields at once,
          // this is a trivial accessor that casts self to the correct
          // field type.
          synthesizer.makeUnionFieldAccessors(result, member);

          // Create labeled initializers for unions that take one of the
          // fields, which only initializes the data for that field.
          auto valueCtor =
              synthesizer.createValueConstructor(result, member,
                                                 /*want param names*/ true,
                                                 /*wantBody=*/true);
          ctors.push_back(valueCtor);
        }
        // TODO: we have a problem lazily looking up members of an unnamed
        // record, so we add them here. To fix this `translateContext` needs to
        // somehow translate unnamed contexts so that `SwiftLookupTable::lookup`
        // can find members in unnamed contexts.
        if (!decl->hasNameForLinkage())
          result->addMemberToLookupTable(member);
      }

      const clang::CXXRecordDecl *cxxRecordDecl =
          dyn_cast<clang::CXXRecordDecl>(decl);
      bool hasBaseClasses = cxxRecordDecl && !cxxRecordDecl->bases().empty();
      if (hasBaseClasses) {
        hasUnreferenceableStorage = true;
        hasMemberwiseInitializer = false;
      }

      if (hasZeroInitializableStorage &&
          !(cxxRecordDecl && cxxRecordDecl->hasDefaultConstructor())) {
        // Add default constructor for the struct if compiling in C mode.
        // If we're compiling for C++:
        // 1. If a default constructor is declared, don't synthesize one.
        // 2. If a default constructor is deleted, don't try to synthesize one.
        // 3. If there is no default constructor, synthesize a C-like default
        //    constructor that zero-initializes the backing memory of the
        //    struct. This is important to maintain source compatibility when a
        //    client enables C++ interop in an existing project that uses C
        //    interop and might rely on the fact that C structs have a default
        //    constructor available in Swift.
        ConstructorDecl *defaultCtor =
            synthesizer.createDefaultConstructor(result);
        ctors.push_back(defaultCtor);
        if (cxxRecordDecl) {
          auto attr = AvailableAttr::createPlatformAgnostic(
              defaultCtor->getASTContext(),
              "This zero-initializes the backing memory of the struct, which "
              "is unsafe for some C++ structs. Consider adding an explicit "
              "default initializer for this C++ struct.",
              "", PlatformAgnosticAvailabilityKind::Deprecated);
          defaultCtor->getAttrs().add(attr);
        }
      }

      // We can assume that it is possible to correctly construct the object by
      // simply initializing its member variables to arbitrary supplied values
      // only when the same is possible in C++. While we could check for that
      // exactly, checking whether the C++ class is an aggregate
      // (C++ [dcl.init.aggr]) has the same effect.
      bool isAggregate = !cxxRecordDecl || cxxRecordDecl->isAggregate();
      if (hasReferenceableFields && hasMemberwiseInitializer && isAggregate) {
        // The default zero initializer suppresses the implicit value
        // constructor that would normally be formed, so we have to add that
        // explicitly as well.
        //
        // If we can completely represent the struct in SIL, leave the body
        // implicit, otherwise synthesize one to call property setters.
        auto valueCtor = synthesizer.createValueConstructor(
            result, members,
            /*want param names*/ true,
            /*want body*/ hasUnreferenceableStorage);
        if (!hasUnreferenceableStorage)
          valueCtor->setIsMemberwiseInitializer();

        ctors.push_back(valueCtor);
      }

      // Do not allow Swift to construct foreign reference types (at least, not
      // yet).
      if (isa<StructDecl>(result)) {
        for (auto ctor : ctors) {
          // Add ctors directly as they cannot always be looked up from the
          // clang decl (some are synthesized by Swift).
          result->addMember(ctor);
        }
      }

      if (auto structResult = dyn_cast<StructDecl>(result)) {
        structResult->setHasUnreferenceableStorage(hasUnreferenceableStorage);
        if (isNonTrivialPtrAuth) {
          structResult->setHasNonTrivialPtrAuth(true);
        }
      }

      if (cxxRecordDecl) {
        // FIXME: Swift right now uses AddressOnly type layout
        // in a way that conflates C++ types
        // that need to be destroyed or copied explicitly with C++
        // types that have to be passed indirectly, because
        // only AddressOnly types can be copied or destroyed using C++
        // semantics. However, in actuality these two concepts are
        // separate and don't map to one notion of AddressOnly type
        // layout cleanly. We should reserve the use of AddressOnly
        // type layout when types have to use C++ copy/move/destroy
        // operations, but allow AddressOnly types to be passed
        // directly as well. This will help unify the MSVC and
        // Itanium difference here, and will allow us to support
        // trivial_abi C++ types as well.
        auto isNonTrivialForPurposeOfCalls =
            [](const clang::CXXRecordDecl *decl) -> bool {
          return decl->hasNonTrivialCopyConstructor() ||
                 decl->hasNonTrivialMoveConstructor() ||
                 !decl->hasTrivialDestructor();
        };
        auto isAddressOnlySwiftStruct =
            [&](const clang::CXXRecordDecl *decl) -> bool {
          // MSVC ABI allows non-trivially destroyed C++ types
          // to be passed in register. This is not supported, as such
          // type wouldn't be destroyed in Swift correctly. Therefore,
          // force AddressOnly type layout using the old heuristic.
          // FIXME: Support can pass in registers for MSVC correctly.
          if (Impl.SwiftContext.LangOpts.Target.isWindowsMSVCEnvironment())
            return isNonTrivialForPurposeOfCalls(decl);
          return !decl->canPassInRegisters();
        };
        if (auto structResult = dyn_cast<StructDecl>(result))
          structResult->setIsCxxNonTrivial(
              isAddressOnlySwiftStruct(cxxRecordDecl));

        for (auto &getterAndSetter : Impl.GetterSetterMap[result]) {
          auto getter = getterAndSetter.second.first;
          auto setter = getterAndSetter.second.second;
          // We cannot make a computed property without a getter.
          if (!getter || getter->getDeclContext() != result)
            continue;

          // If we have a getter and a setter make sure the types line up.
          if (setter && !getter->getResultInterfaceType()->isEqual(
                            setter->getParameters()->get(0)->getType()))
            continue;

          // If the name that we would import this as already exists, then don't
          // add a computed property, because it will conflict with an existing
          // name and make both APIs unusable.
          CXXMethodBridging cxxMethodBridging(
              cast<clang::CXXMethodDecl>(getter->getClangDecl()));
          if (allMemberNames.contains(
                  cxxMethodBridging.importNameAsCamelCaseName()))
            continue;

          auto p =
              synthesizer.makeComputedPropertyFromCXXMethods(getter, setter);
          // Add computed properties directly because they won't be found from
          // the clang decl during lazy member lookup.
          result->addMember(p);
        }

        for (auto &subscriptInfo : Impl.cxxSubscripts) {
          auto declAndParameterType = subscriptInfo.first;
          if (declAndParameterType.first != result)
            continue;

          auto getterAndSetter = subscriptInfo.second;
          auto subscript = synthesizer.makeSubscript(getterAndSetter.first,
                                                     getterAndSetter.second);
          // Also add subscripts directly because they won't be found from the
          // clang decl.
          result->addMember(subscript);

          // Add the subscript as an alternative for the getter so that it gets
          // carried into derived classes.
          auto *subscriptImpl = getterAndSetter.first ? getterAndSetter.first : getterAndSetter.second;
          Impl.addAlternateDecl(subscriptImpl, subscript);
        }
      }

      result->setMemberLoader(&Impl, 0);
      return result;
    }

    void validateForeignReferenceType(const clang::CXXRecordDecl *decl,
                                      ClassDecl *classDecl) {
      auto isValidOperation = [&](ValueDecl *operation) -> bool {
        auto operationFn = dyn_cast<FuncDecl>(operation);
        if (!operationFn)
          return false;

        if (!operationFn->getResultInterfaceType()->isVoid())
          return false;

        if (operationFn->getParameters()->size() != 1)
          return false;

        if (operationFn->getParameters()->get(0)->getInterfaceType()->isEqual(
                classDecl->getInterfaceType()))
          return false;

        return true;
      };

      auto retainOperation = evaluateOrDefault(
          Impl.SwiftContext.evaluator,
          CustomRefCountingOperation(
              {classDecl, CustomRefCountingOperationKind::retain}),
          {});
      if (retainOperation.kind ==
          CustomRefCountingOperationResult::noAttribute) {
        HeaderLoc loc(decl->getLocation());
        Impl.diagnose(loc, diag::reference_type_must_have_retain_attr,
                      decl->getNameAsString());
      } else if (retainOperation.kind ==
                 CustomRefCountingOperationResult::notFound) {
        HeaderLoc loc(decl->getLocation());
        Impl.diagnose(loc, diag::foreign_reference_types_cannot_find_retain,
                      retainOperation.name, decl->getNameAsString());
      } else if (retainOperation.kind ==
                 CustomRefCountingOperationResult::tooManyFound) {
        HeaderLoc loc(decl->getLocation());
        Impl.diagnose(loc, diag::too_many_reference_type_retain_operations,
                      retainOperation.name, decl->getNameAsString());
      } else if (retainOperation.kind ==
                 CustomRefCountingOperationResult::foundOperation) {
        if (!isValidOperation(retainOperation.operation)) {
          HeaderLoc loc(decl->getLocation());
          Impl.diagnose(loc, diag::foreign_reference_types_invalid_retain,
                        retainOperation.name, decl->getNameAsString());
        }
      } else {
        // Nothing to do.
        assert(retainOperation.kind ==
               CustomRefCountingOperationResult::immortal);
      }

      auto releaseOperation = evaluateOrDefault(
          Impl.SwiftContext.evaluator,
          CustomRefCountingOperation(
              {classDecl, CustomRefCountingOperationKind::release}),
          {});
      if (releaseOperation.kind ==
          CustomRefCountingOperationResult::noAttribute) {
        HeaderLoc loc(decl->getLocation());
        Impl.diagnose(loc, diag::reference_type_must_have_release_attr,
                      decl->getNameAsString());
      } else if (releaseOperation.kind ==
                 CustomRefCountingOperationResult::notFound) {
        HeaderLoc loc(decl->getLocation());
        Impl.diagnose(loc, diag::foreign_reference_types_cannot_find_release,
                      releaseOperation.name, decl->getNameAsString());
      } else if (releaseOperation.kind ==
                 CustomRefCountingOperationResult::tooManyFound) {
        HeaderLoc loc(decl->getLocation());
        Impl.diagnose(loc, diag::too_many_reference_type_release_operations,
                      releaseOperation.name, decl->getNameAsString());
      } else if (releaseOperation.kind ==
                 CustomRefCountingOperationResult::foundOperation) {
        if (!isValidOperation(releaseOperation.operation)) {
          HeaderLoc loc(decl->getLocation());
          Impl.diagnose(loc, diag::foreign_reference_types_invalid_release,
                        releaseOperation.name, decl->getNameAsString());
        }
      } else {
        // Nothing to do.
        assert(releaseOperation.kind ==
               CustomRefCountingOperationResult::immortal);
      }
    }

    Decl *VisitCXXRecordDecl(const clang::CXXRecordDecl *decl) {
      // This can be called from lldb without C++ interop being enabled: There
      // may be C++ declarations in imported modules, but the interface for
      // those modules may be a pure C or Objective-C interface.
      // To avoid crashing in Clang's Sema, fall back to importing this as a
      // plain RecordDecl.
      if (!Impl.SwiftContext.LangOpts.EnableCXXInterop)
        return VisitRecordDecl(decl);

      if (!decl->getDefinition()) {
        Impl.addImportDiagnostic(
            decl,
            Diagnostic(diag::incomplete_record, Impl.SwiftContext.AllocateCopy(
                                                    decl->getNameAsString())),
            decl->getLocation());
      }

      decl = decl->getDefinition();
      if (!decl) {
        forwardDeclaration = true;
        return nullptr;
      }

      auto &clangSema = Impl.getClangSema();
      // Make Clang define any implicit constructors it may need (copy,
      // default). Make sure we only do this if the class has been fully defined
      // and we're not in a dependent context (this is equivalent to the logic
      // in CanDeclareSpecialMemberFunction in Clang's SemaLookup.cpp).
      // TODO: I suspect this if-statement does not need to be here.
      if (!decl->isBeingDefined() && !decl->isDependentContext()) {
        if (decl->needsImplicitDefaultConstructor()) {
          clang::CXXConstructorDecl *ctor =
              clangSema.DeclareImplicitDefaultConstructor(
                  const_cast<clang::CXXRecordDecl *>(decl));
          if (!ctor->isDeleted())
            clangSema.DefineImplicitDefaultConstructor(clang::SourceLocation(),
                                                       ctor);
        }
        clang::CXXConstructorDecl *copyCtor = nullptr;
        clang::CXXConstructorDecl *moveCtor = nullptr;
        clang::CXXConstructorDecl *defaultCtor = nullptr;
        if (decl->needsImplicitCopyConstructor()) {
          copyCtor = clangSema.DeclareImplicitCopyConstructor(
              const_cast<clang::CXXRecordDecl *>(decl));
        }
        if (decl->needsImplicitMoveConstructor()) {
          moveCtor = clangSema.DeclareImplicitMoveConstructor(
              const_cast<clang::CXXRecordDecl *>(decl));
        }
        if (decl->needsImplicitDefaultConstructor()) {
          defaultCtor = clangSema.DeclareImplicitDefaultConstructor(
              const_cast<clang::CXXRecordDecl *>(decl));
        }
        // We may have a defaulted copy/move/default constructor that needs to
        // be defined. Try to find it.
        for (auto methods : decl->methods()) {
          if (auto declCtor = dyn_cast<clang::CXXConstructorDecl>(methods)) {
            if (declCtor->isDefaulted() &&
                declCtor->getAccess() == clang::AS_public &&
                !declCtor->isDeleted() &&
                // Note: we use "doesThisDeclarationHaveABody" here because
                // that's what "DefineImplicitCopyConstructor" checks.
                !declCtor->doesThisDeclarationHaveABody()) {
              if (declCtor->isCopyConstructor()) {
                if (!copyCtor)
                  copyCtor = declCtor;
              } else if (declCtor->isMoveConstructor()) {
                if (!moveCtor)
                  moveCtor = declCtor;
              } else if (declCtor->isDefaultConstructor()) {
                if (!defaultCtor)
                  defaultCtor = declCtor;
              }
            }
          }
        }
        if (copyCtor) {
          clangSema.DefineImplicitCopyConstructor(clang::SourceLocation(),
                                                  copyCtor);
        }
        if (moveCtor) {
          clangSema.DefineImplicitMoveConstructor(clang::SourceLocation(),
                                                  moveCtor);
        }
        if (defaultCtor) {
          clangSema.DefineImplicitDefaultConstructor(clang::SourceLocation(),
                                                     defaultCtor);
        }

        if (decl->needsImplicitDestructor()) {
          auto dtor = clangSema.DeclareImplicitDestructor(
              const_cast<clang::CXXRecordDecl *>(decl));
          clangSema.DefineImplicitDestructor(clang::SourceLocation(), dtor);
        }
      }

      // It is import that we bail on an unimportable record *before* we import
      // any of its members or cache the decl.
      auto semanticsKind =
          evaluateOrDefault(Impl.SwiftContext.evaluator,
                            CxxRecordSemantics({decl, Impl.SwiftContext}), {});
      if (semanticsKind == CxxRecordSemanticsKind::MissingLifetimeOperation &&
          // Let un-specialized class templates through. We'll sort out their
          // members once they're instranciated.
          !Impl.importSymbolicCXXDecls) {
        Impl.addImportDiagnostic(
            decl,
            Diagnostic(diag::record_not_automatically_importable,
                       Impl.SwiftContext.AllocateCopy(decl->getNameAsString()),
                       "does not have a copy constructor or destructor"),
            decl->getLocation());
        return nullptr;
      }
      if (semanticsKind == CxxRecordSemanticsKind::SwiftClassType) {
        // FIXME: add a diagnostic here for unsupported imported use of Swift
        // type?
        return nullptr;
      }

      auto result = VisitRecordDecl(decl);
      if (!result)
        return nullptr;

      if (decl->hasAttr<clang::TrivialABIAttr>()) {
        // We cannot yet represent trivial_abi C++ records in Swift.
        // Clang tells us such type can be passed in registers, so
        // we avoid using AddressOnly type-layout for such type, which means
        // that it then does not use C++'s copy and destroy semantics from
        // Swift.
        Impl.markUnavailable(cast<ValueDecl>(result),
                             "C++ classes with `trivial_abi` Clang attribute "
                             "are not yet available in Swift");
      }

      if (auto classDecl = dyn_cast<ClassDecl>(result)) {
        validateForeignReferenceType(decl, classDecl);

        auto ctx = Impl.SwiftContext.getSwift58Availability();
        if (!ctx.isAlwaysAvailable()) {
          assert(ctx.getOSVersion().hasLowerEndpoint());
          auto AvAttr = new (Impl.SwiftContext) AvailableAttr(
              SourceLoc(), SourceRange(),
              targetPlatform(Impl.SwiftContext.LangOpts), "", "",
              /*RenameDecl=*/nullptr, ctx.getOSVersion().getLowerEndpoint(),
              /*IntroducedRange=*/SourceRange(), {},
              /*DeprecatedRange=*/SourceRange(), {},
              /*ObsoletedRange=*/SourceRange(),
              PlatformAgnosticAvailabilityKind::None, /*Implicit=*/false,
              false);
          classDecl->getAttrs().add(AvAttr);
        }
      }

      // If this module is declared as a C++ module, try to synthesize
      // conformances to Swift protocols from the Cxx module.
      auto clangModule = decl->getOwningModule();
      if (clangModule && requiresCPlusPlus(clangModule)) {
        auto nominalDecl = cast<NominalTypeDecl>(result);
        conformToCxxIteratorIfNeeded(Impl, nominalDecl, decl);
        conformToCxxSequenceIfNeeded(Impl, nominalDecl, decl);
        conformToCxxSetIfNeeded(Impl, nominalDecl, decl);
        conformToCxxDictionaryIfNeeded(Impl, nominalDecl, decl);
        conformToCxxPairIfNeeded(Impl, nominalDecl, decl);
        conformToCxxOptionalIfNeeded(Impl, nominalDecl, decl);
      }

      if (auto *ntd = dyn_cast<NominalTypeDecl>(result))
        addExplicitProtocolConformances(ntd);

      return result;
    }

    void addExplicitProtocolConformances(NominalTypeDecl *decl) {
      auto clangDecl = decl->getClangDecl();

      if (!clangDecl->hasAttrs())
        return;

      SmallVector<ValueDecl *, 1> results;
      auto conformsToAttr =
          llvm::find_if(clangDecl->getAttrs(), [](auto *attr) {
            if (auto swiftAttr = dyn_cast<clang::SwiftAttrAttr>(attr))
              return swiftAttr->getAttribute().startswith("conforms_to:");
            return false;
          });
      if (conformsToAttr == clangDecl->getAttrs().end())
        return;

      auto conformsToValue = cast<clang::SwiftAttrAttr>(*conformsToAttr)
                                 ->getAttribute()
                                 .drop_front(StringRef("conforms_to:").size())
                                 .str();
      auto names = StringRef(conformsToValue).split('.');
      auto moduleName = names.first;
      auto protocolName = names.second;
      if (protocolName.empty()) {
        HeaderLoc attrLoc((*conformsToAttr)->getLocation());
        Impl.diagnose(attrLoc, diag::conforms_to_missing_dot, conformsToValue);
        return;
      }

      auto *mod = Impl.SwiftContext.getModuleByIdentifier(
          Impl.SwiftContext.getIdentifier(moduleName));
      if (!mod) {
        HeaderLoc attrLoc((*conformsToAttr)->getLocation());
        Impl.diagnose(attrLoc, diag::cannot_find_conforms_to_module,
                      conformsToValue, moduleName);
        return;
      }
      mod->lookupValue(Impl.SwiftContext.getIdentifier(protocolName),
                       NLKind::UnqualifiedLookup, results);
      if (results.empty()) {
        HeaderLoc attrLoc((*conformsToAttr)->getLocation());
        Impl.diagnose(attrLoc, diag::cannot_find_conforms_to, protocolName,
                      moduleName);
        return;
      } else if (results.size() != 1) {
        HeaderLoc attrLoc((*conformsToAttr)->getLocation());
        Impl.diagnose(attrLoc, diag::conforms_to_ambiguous, protocolName,
                      moduleName);
        return;
      }

      auto result = results.front();
      if (auto protocol = dyn_cast<ProtocolDecl>(result)) {
        decl->getAttrs().add(
            new (Impl.SwiftContext) SynthesizedProtocolAttr(protocol, &Impl, false));
      } else {
        HeaderLoc attrLoc((*conformsToAttr)->getLocation());
        Impl.diagnose(attrLoc, diag::conforms_to_not_protocol,
                      result->getDescriptiveKind(), result, conformsToValue);
      }
    }

    bool isSpecializationDepthGreaterThan(
        const clang::ClassTemplateSpecializationDecl *decl, unsigned maxDepth) {
      for (auto arg : decl->getTemplateArgs().asArray()) {
        if (arg.getKind() == clang::TemplateArgument::Type) {
          if (auto classSpec =
                  dyn_cast_or_null<clang::ClassTemplateSpecializationDecl>(
                      arg.getAsType()->getAsCXXRecordDecl())) {
            if (maxDepth == 0 ||
                isSpecializationDepthGreaterThan(classSpec, maxDepth - 1))
              return true;
          }
        }
      }
      return false;
    }

    Decl *VisitClassTemplateSpecializationDecl(
                 const clang::ClassTemplateSpecializationDecl *decl) {
      // Treat a specific specialization like the unspecialized class template
      // when importing it in symbolic mode.
      if (Impl.importSymbolicCXXDecls)
        return Impl.importDecl(decl->getSpecializedTemplate(),
                               Impl.CurrentVersion);

      bool isPair = decl->getSpecializedTemplate()->isInStdNamespace() &&
                    decl->getSpecializedTemplate()->getName() == "pair";

      // Before we go any further, check if we've already got tens of thousands
      // of specializations. If so, it means we're likely instantiating a very
      // deep/complex template, or we've run into an infinite loop. In either
      // case, its not worth the compile time, so bail.
      // TODO: this could be configurable at some point.
      size_t specializationLimit = !isPair ? 1000 : 10000;
      if (size_t(
              llvm::size(decl->getSpecializedTemplate()->specializations())) >
          specializationLimit) {
        std::string name;
        llvm::raw_string_ostream os(name);
        decl->printQualifiedName(os);
        // Emit a warning if we haven't warned about this decl yet.
        if (Impl.tooDeepTemplateSpecializations.insert(name).second)
          Impl.diagnose({}, diag::too_many_class_template_instantiations, name);
        return nullptr;
      }

      // `decl->getDefinition()` can return nullptr before the call to sema and
      // return its definition afterwards.
      clang::Sema &clangSema = Impl.getClangSema();
      if (!decl->getDefinition()) {
        bool notInstantiated = clangSema.InstantiateClassTemplateSpecialization(
            decl->getLocation(),
            const_cast<clang::ClassTemplateSpecializationDecl *>(decl),
            clang::TemplateSpecializationKind::TSK_ImplicitInstantiation,
            /*Complain*/ false);
        // If the template can't be instantiated, bail.
        if (notInstantiated)
          return nullptr;
      }
      if (!clangSema.isCompleteType(
              decl->getLocation(),
              Impl.getClangASTContext().getRecordType(decl))) {
        // If we got nullptr definition now it means the type is not complete.
        // We don't import incomplete types.
        return nullptr;
      }
      auto def = dyn_cast<clang::ClassTemplateSpecializationDecl>(
          decl->getDefinition());
      assert(def && "Class template instantiation didn't have definition");

      // Currently this is a relatively low number, in the future we might
      // consider increasing it, but this should keep compile time down,
      // especially for types that become exponentially large when
      // instantiating.
      if (isSpecializationDepthGreaterThan(def, 8))
        return nullptr;

      return VisitCXXRecordDecl(def);
    }

    Decl *VisitClassTemplatePartialSpecializationDecl(
        const clang::ClassTemplatePartialSpecializationDecl *decl) {
      // Note: partial template specializations are not imported.
      return nullptr;
    }

    Decl *VisitTemplateTypeParmDecl(const clang::TemplateTypeParmDecl *decl) {
      // Note: templates are not imported.
      return nullptr;
    }

    Decl *VisitEnumConstantDecl(const clang::EnumConstantDecl *decl) {
      auto clangEnum = cast<clang::EnumDecl>(decl->getDeclContext());

      ImportedName importedName;
      llvm::Optional<ImportedName> correctSwiftName;
      std::tie(importedName, correctSwiftName) = importFullName(decl);
      if (!importedName) return nullptr;

      auto name = importedName.getDeclName().getBaseIdentifier();
      if (name.empty())
        return nullptr;

      auto enumKind = Impl.getEnumKind(clangEnum);
      switch (enumKind) {
      case EnumKind::Constants:
      case EnumKind::Unknown: {
        // The enumeration was simply mapped to an integral type. Create a
        // constant with that integral type.

        // The context where the constant will be introduced.
        auto dc =
            Impl.importDeclContextOf(decl, importedName.getEffectiveContext());
        if (!dc)
          return nullptr;

        // Enumeration type.
        auto &clangContext = Impl.getClangASTContext();
        auto type = Impl.importTypeIgnoreIUO(
            clangContext.getTagDeclType(clangEnum), ImportTypeKind::Value,
            ImportDiagnosticAdder(Impl, clangEnum, clangEnum->getLocation()),
            isInSystemModule(dc), Bridgeability::None, ImportTypeAttrs());
        if (!type)
          return nullptr;

        // Create the global constant.
        bool isStatic = enumKind != EnumKind::Unknown && dc->isTypeContext();
        auto result = synthesizer.createConstant(
            name, dc, type, clang::APValue(decl->getInitVal()),
            enumKind == EnumKind::Unknown ? ConstantConvertKind::Construction
                                          : ConstantConvertKind::None,
            isStatic, decl);
        Impl.ImportedDecls[{decl->getCanonicalDecl(), getVersion()}] = result;

        // If this is a compatibility stub, mark it as such.
        if (correctSwiftName)
          markAsVariant(result, *correctSwiftName);

        return result;
      }

      case EnumKind::NonFrozenEnum:
      case EnumKind::FrozenEnum:
      case EnumKind::Options: {
        // The enumeration was mapped to a high-level Swift type, and its
        // elements were created as children of that enum. They aren't available
        // independently.

        // FIXME: This is gross. We shouldn't have to import
        // everything to get at the individual constants.
        return nullptr;
      }
      }

      llvm_unreachable("Invalid EnumKind.");
    }


    Decl *
    VisitUnresolvedUsingValueDecl(const clang::UnresolvedUsingValueDecl *decl) {
      // Note: templates are not imported.
      return nullptr;
    }

    Decl *VisitIndirectFieldDecl(const clang::IndirectFieldDecl *decl) {
      ImportedName importedName;
      llvm::Optional<ImportedName> correctSwiftName;
      std::tie(importedName, correctSwiftName) = importFullName(decl);
      if (!importedName) return nullptr;

      auto name = importedName.getDeclName().getBaseIdentifier();

      auto dc =
          Impl.importDeclContextOf(decl, importedName.getEffectiveContext());
      if (!dc)
        return nullptr;

      // If we encounter an IndirectFieldDecl, ensure that its parent is
      // importable before attempting to import it because they are dependent
      // when it comes to getter/setter generation.
      if (auto parent = dyn_cast<clang::CXXRecordDecl>(
              decl->getAnonField()->getParent())) {
        auto semanticsKind = evaluateOrDefault(
            Impl.SwiftContext.evaluator,
            CxxRecordSemantics({parent, Impl.SwiftContext}), {});
        if (semanticsKind == CxxRecordSemanticsKind::MissingLifetimeOperation)
          return nullptr;
      }

      auto importedType =
          Impl.importType(decl->getType(), ImportTypeKind::Variable,
                          ImportDiagnosticAdder(Impl, decl, decl->getLocation()),
                          isInSystemModule(dc), Bridgeability::None,
                          getImportTypeAttrs(decl));
      if (!importedType)
        return nullptr;

      auto type = importedType.getType();

      // Map this indirect field to a Swift variable.
      auto result = Impl.createDeclWithClangNode<VarDecl>(decl,
                       AccessLevel::Public,
                       /*IsStatic*/false,
                       VarDecl::Introducer::Var,
                       Impl.importSourceLoc(decl->getBeginLoc()),
                       name, dc);
      result->setInterfaceType(type);
      result->setIsObjC(false);
      result->setIsDynamic(false);
      Impl.recordImplicitUnwrapForDecl(result,
                                       importedType.isImplicitlyUnwrapped());

      // If this is a compatibility stub, mark is as such.
      if (correctSwiftName)
        markAsVariant(result, *correctSwiftName);

      // Don't import unavailable fields that have no associated storage.
      // TODO: is there any way we could bail here before we allocate/construct
      // the VarDecl?
      if (result->getAttrs().isUnavailable(Impl.SwiftContext))
        return nullptr;

      return result;
    }

    ParameterList *
    getNonSelfParamList(DeclContext *dc, const clang::FunctionDecl *decl,
                        llvm::Optional<unsigned> selfIdx,
                        ArrayRef<Identifier> argNames,
                        bool allowNSUIntegerAsInt, bool isAccessor,
                        ArrayRef<GenericTypeParamDecl *> genericParams) {
      if (bool(selfIdx)) {
        assert(((decl->getNumParams() == argNames.size() + 1) || isAccessor) &&
               (*selfIdx < decl->getNumParams()) && "where's self?");
      } else {
        unsigned numParamsAdjusted =
            decl->getNumParams() + (decl->isVariadic() ? 1 : 0);
        assert(numParamsAdjusted == argNames.size() || isAccessor);
      }

      SmallVector<const clang::ParmVarDecl *, 4> nonSelfParams;
      for (unsigned i = 0; i < decl->getNumParams(); ++i) {
        if (selfIdx && i == *selfIdx)
          continue;
        nonSelfParams.push_back(decl->getParamDecl(i));
      }
      return Impl.importFunctionParameterList(
          dc, decl, nonSelfParams, decl->isVariadic(), allowNSUIntegerAsInt,
          argNames, genericParams, /*resultType=*/nullptr);
    }

    Decl *
    importGlobalAsInitializer(const clang::FunctionDecl *decl, DeclName name,
                              DeclContext *dc, CtorInitializerKind initKind,
                              llvm::Optional<ImportedName> correctSwiftName);

    /// Create an implicit property given the imported name of one of
    /// the accessors.
    VarDecl *getImplicitProperty(ImportedName importedName,
                                 const clang::FunctionDecl *accessor);

    bool foreignReferenceTypePassedByRef(const clang::FunctionDecl *decl) {
      bool anyParamPassesByVal =
          llvm::any_of(decl->parameters(), [this, decl](auto *param) {
            if (auto recordType = dyn_cast<clang::RecordType>(
                    param->getType().getCanonicalType())) {
              if (recordHasReferenceSemantics(recordType->getDecl())) {
                Impl.addImportDiagnostic(
                    decl,
                    Diagnostic(diag::reference_passed_by_value,
                               Impl.SwiftContext.AllocateCopy(
                                   recordType->getDecl()->getNameAsString()),
                               "a parameter"),
                    decl->getLocation());
                return true;
              }
            }
            return false;
          });

      if (anyParamPassesByVal)
        return true;

      if (auto recordType = dyn_cast<clang::RecordType>(
              decl->getReturnType().getCanonicalType())) {
        if (recordHasReferenceSemantics(recordType->getDecl())) {
          Impl.addImportDiagnostic(
              decl,
              Diagnostic(diag::reference_passed_by_value,
                         Impl.SwiftContext.AllocateCopy(
                             recordType->getDecl()->getNameAsString()),
                         "the return"),
              decl->getLocation());
          return true;
        }
      }

      return false;
    }

    Decl *VisitFunctionDecl(const clang::FunctionDecl *decl) {
      // Import the name of the function.
      ImportedName importedName;
      llvm::Optional<ImportedName> correctSwiftName;
      std::tie(importedName, correctSwiftName) = importFullName(decl);
      if (!importedName)
        return nullptr;

      // Don't import functions that pass a foreign reference type by value
      // (either as a parameter or return type).
      if (foreignReferenceTypePassedByRef(decl))
        return nullptr;

      switch (importedName.getAccessorKind()) {
      case ImportedAccessorKind::None:
      case ImportedAccessorKind::SubscriptGetter:
      case ImportedAccessorKind::SubscriptSetter:
        break;

      case ImportedAccessorKind::PropertyGetter: {
        auto property = getImplicitProperty(importedName, decl);
        if (!property) return nullptr;
        return property->getParsedAccessor(AccessorKind::Get);
      }

      case ImportedAccessorKind::PropertySetter:
        auto property = getImplicitProperty(importedName, decl);
        if (!property) return nullptr;
        return property->getParsedAccessor(AccessorKind::Set);
      }

      return importFunctionDecl(decl, importedName, correctSwiftName,
                                llvm::None);
    }

    Decl *importFunctionDecl(
        const clang::FunctionDecl *decl, ImportedName importedName,
        llvm::Optional<ImportedName> correctSwiftName,
        llvm::Optional<AccessorInfo> accessorInfo,
        const clang::FunctionTemplateDecl *funcTemplate = nullptr) {
      if (decl->isDeleted())
        return nullptr;

      if (Impl.SwiftContext.LangOpts.EnableCXXInterop &&
          !isa<clang::CXXMethodDecl>(decl)) {
        // Do not import math functions from the C++ standard library, as
        // they're also imported from the Darwin/Glibc module, and their
        // presence in the C++ standard library will cause overloading
        // ambiguities or other type checking errors in Swift.
        auto isAlternativeCStdlibFunctionFromTextualHeader =
            [this](const clang::FunctionDecl *d) -> bool {
          // stdlib.h might be a textual header in libc++'s module map.
          // in this case, check for known ambiguous functions by their name
          // instead of checking if they come from the `std` module.
          if (!d->getDeclName().isIdentifier())
            return false;
          if (d->getName() == "abs" || d->getName() == "div")
            return true;
          if (Impl.SwiftContext.LangOpts.Target.isOSDarwin())
            return d->getName() == "strstr" || d->getName() == "sin" ||
                   d->getName() == "cos" || d->getName() == "exit";
          return false;
        };
        auto topLevelModuleEq =
            [](const clang::FunctionDecl *d, StringRef n) -> bool {
          return d->getOwningModule() &&
                 d->getOwningModule()
                    ->getTopLevelModule()
                    ->getFullModuleName() == n;
        };
        if (topLevelModuleEq(decl, "std")) {
          if (isAlternativeCStdlibFunctionFromTextualHeader(decl)) {
            return nullptr;
          }
          auto filename =
              Impl.getClangPreprocessor().getSourceManager().getFilename(
                  decl->getLocation());
          if (filename.endswith("cmath") || filename.endswith("math.h") ||
              filename.endswith("stdlib.h") || filename.endswith("cstdlib")) {
            return nullptr;
          }
        }
        // Use the exit function from _SwiftConcurrency.h as it is platform
        // agnostic.
        if ((topLevelModuleEq(decl, "Darwin") ||
             topLevelModuleEq(decl, "SwiftGlibc")) &&
            decl->getDeclName().isIdentifier() && decl->getName() == "exit") {
          return nullptr;
        }
      }

      auto dc =
          Impl.importDeclContextOf(decl, importedName.getEffectiveContext());
      if (!dc)
        return nullptr;

      // We may have already imported this function decl before we imported the
      // parent record. In such a case it's important we don't re-import.
      auto known = Impl.ImportedDecls.find({decl, getVersion()});
      if (known != Impl.ImportedDecls.end()) {
        return known->second;
      }

      bool isOperator = decl->getDeclName().getNameKind() ==
                        clang::DeclarationName::CXXOperatorName;
      bool isNonSubscriptOperator =
          isOperator && (decl->getDeclName().getCXXOverloadedOperator() !=
                         clang::OO_Subscript);

      // For now, we don't support non-subscript operators which are templated
      if (isNonSubscriptOperator && decl->isTemplated()) {
        return nullptr;
      }

      DeclName name = accessorInfo ? DeclName() : importedName.getDeclName();
      auto selfIdx = importedName.getSelfIndex();

      auto templateParamTypeUsedInSignature =
          [decl](clang::TemplateTypeParmDecl *type) -> bool {
        // TODO(https://github.com/apple/swift/issues/56206): We will want to update this to handle dependent types when those are supported.
        if (hasSameUnderlyingType(decl->getReturnType().getTypePtr(), type))
          return true;

        for (unsigned i : range(0, decl->getNumParams())) {
          if (hasSameUnderlyingType(
                  decl->getParamDecl(i)->getType().getTypePtr(), type))
            return true;
        }

        return false;
      };

      ImportedType importedType;
      bool selfIsInOut = false;
      ParameterList *bodyParams = nullptr;
      GenericParamList *genericParams = nullptr;
      SmallVector<GenericTypeParamDecl *, 4> templateParams;
      if (funcTemplate) {
        unsigned i = 0;
        for (auto param : *funcTemplate->getTemplateParameters()) {
          auto templateTypeParam = cast<clang::TemplateTypeParmDecl>(param);
          // If the template type parameter isn't used in the signature then we
          // won't be able to deduce what it is when the function template is
          // called in Swift code. This is OK if there's a defaulted type we can
          // use (in which case we just don't add a correspond generic). This
          // also means sometimes we will import a function template as a
          // "normal" (non-generic) Swift function.
          //
          // If the defaulted template type parameter is used in the signature,
          // then still add a generic so that it can be overrieded.
          // TODO(https://github.com/apple/swift/issues/57184): In the future we might want to import two overloads in this case so that the default type could still be used.
          if (templateTypeParam->hasDefaultArgument() &&
              !templateParamTypeUsedInSignature(templateTypeParam))
            continue;

          auto *typeParam = Impl.createDeclWithClangNode<GenericTypeParamDecl>(
              param, AccessLevel::Public, dc,
              Impl.SwiftContext.getIdentifier(param->getName()),
              /*nameLoc*/ SourceLoc(),
              /*ellipsisLoc*/ SourceLoc(),
              /*depth*/ 0, /*index*/ i, /*isParameterPack*/ false);
          templateParams.push_back(typeParam);
          (void)++i;
        }
        if (!templateParams.empty())
          genericParams = GenericParamList::create(
              Impl.SwiftContext, SourceLoc(), templateParams, SourceLoc());
      }

      bool importFuncWithoutSignature =
          isa<clang::CXXMethodDecl>(decl) && Impl.importSymbolicCXXDecls;
      if (!dc->isModuleScopeContext() && !isa<clang::CXXMethodDecl>(decl)) {
        // Handle initializers.
        if (name.getBaseName() == DeclBaseName::createConstructor()) {
          assert(!accessorInfo);
          return importGlobalAsInitializer(decl, name, dc,
                                           importedName.getInitKind(),
                                           correctSwiftName);
        }

        if (dc->getSelfProtocolDecl() && !selfIdx) {
          // FIXME: source location...
          Impl.diagnose({}, diag::swift_name_protocol_static, /*isInit=*/false);
          Impl.diagnose({}, diag::note_while_importing, decl->getName());
          return nullptr;
        }

        if (!decl->hasPrototype()) {
          // FIXME: source location...
          Impl.diagnose({}, diag::swift_name_no_prototype);
          Impl.diagnose({}, diag::note_while_importing, decl->getName());
          return nullptr;
        }

        // There is an inout 'self' when the parameter is a pointer to a
        // non-const instance of the type we're importing onto. Importing this
        // as a method means that the method should be treated as mutating in
        // this situation.
        if (selfIdx &&
            !dc->getDeclaredInterfaceType()->hasReferenceSemantics()) {
          auto selfParam = decl->getParamDecl(*selfIdx);
          auto selfParamTy = selfParam->getType();
          if ((selfParamTy->isPointerType() ||
               selfParamTy->isReferenceType()) &&
              !selfParamTy->getPointeeType().isConstQualified()) {
            selfIsInOut = true;

            // If there's a swift_newtype, check the levels of indirection: self
            // is only inout if this is a pointer to the typedef type (which
            // itself is a pointer).
            if (auto nominalTypeDecl = dc->getSelfNominalTypeDecl()) {
              if (auto clangDCTy = dyn_cast_or_null<clang::TypedefNameDecl>(
                      nominalTypeDecl->getClangDecl()))
                if (getSwiftNewtypeAttr(clangDCTy, getVersion()))
                  if (clangDCTy->getUnderlyingType().getCanonicalType() !=
                      selfParamTy->getPointeeType().getCanonicalType())
                    selfIsInOut = false;
            }
          }
        }

        bool allowNSUIntegerAsInt =
            Impl.shouldAllowNSUIntegerAsInt(isInSystemModule(dc), decl);

        bodyParams =
            getNonSelfParamList(dc, decl, selfIdx, name.getArgumentNames(),
                                allowNSUIntegerAsInt, !name, templateParams);
        // If we can't import a param for some reason (ex. it's a dependent
        // type), bail.
        if (!bodyParams)
          return nullptr;

        if (decl->getReturnType()->isScalarType())
          importedType =
              Impl.importFunctionReturnType(dc, decl, allowNSUIntegerAsInt);
      } else {
        if (importFuncWithoutSignature) {
          importedType = ImportedType{Impl.SwiftContext.getVoidType(), false};
          if (decl->param_empty())
            bodyParams = ParameterList::createEmpty(Impl.SwiftContext);
          else {
            llvm::SmallVector<ParamDecl *, 4> params;
            for (const auto &param : decl->parameters()) {

              Identifier bodyName =
                  Impl.importFullName(param, Impl.CurrentVersion)
                      .getDeclName()
                      .getBaseIdentifier();
              auto paramInfo = Impl.createDeclWithClangNode<ParamDecl>(
                  param, AccessLevel::Private, SourceLoc(), SourceLoc(),
                  Identifier(), Impl.importSourceLoc(param->getLocation()),
                  bodyName, Impl.ImportedHeaderUnit);
              paramInfo->setSpecifier(ParamSpecifier::Default);
              paramInfo->setInterfaceType(Impl.SwiftContext.TheAnyType);
              params.push_back(paramInfo);
            }
            bodyParams = ParameterList::create(Impl.SwiftContext, params);
          }
        } else {
          // Import the function type. If we have parameters, make sure their
          // names get into the resulting function type.
          importedType = Impl.importFunctionParamsAndReturnType(
              dc, decl, {decl->param_begin(), decl->param_size()},
              decl->isVariadic(), isInSystemModule(dc), name, bodyParams,
              templateParams);
        }

        if (auto *mdecl = dyn_cast<clang::CXXMethodDecl>(decl)) {
          if (mdecl->isStatic()) {
            selfIdx = llvm::None;
          } else {
            // Swift imports the "self" param last, even for clang functions.
            selfIdx = bodyParams ? bodyParams->size() : 0;
            // If the method is imported as mutating, this implicitly makes the
            // parameter indirect.
            selfIsInOut =
                !isa<ClassDecl>(dc) &&
                Impl.SwiftContext.getClangModuleLoader()->isCXXMethodMutating(
                    mdecl);
          }
        }
      }

      if (name && name.isSimpleName()) {
        assert(importedName.hasCustomName() &&
               "imported function with simple name?");
        // Just fill in empty argument labels.
        name = DeclName(Impl.SwiftContext, name.getBaseName(), bodyParams);
      }

      if (!bodyParams) {
        Impl.addImportDiagnostic(
            decl, Diagnostic(diag::invoked_func_not_imported, decl),
            decl->getSourceRange().getBegin());
        return nullptr;
      }

      if (name && name.getArgumentNames().size() != bodyParams->size()) {
        // We synthesized additional parameters so rebuild the DeclName.
        name = DeclName(Impl.SwiftContext, name.getBaseName(), bodyParams);
      }

      auto loc = Impl.importSourceLoc(decl->getLocation());

      ClangNode clangNode = decl;
      if (funcTemplate)
        clangNode = funcTemplate;

      // FIXME: Poor location info.
      auto nameLoc = Impl.importSourceLoc(decl->getLocation());

      AbstractFunctionDecl *result = nullptr;
      if (auto *ctordecl = dyn_cast<clang::CXXConstructorDecl>(decl)) {
        // Don't import copy constructor or move constructor -- these will be
        // provided through the value witness table.
        if (ctordecl->isCopyConstructor() || ctordecl->isMoveConstructor())
          return nullptr;

        DeclName ctorName(Impl.SwiftContext, DeclBaseName::createConstructor(),
                          bodyParams);
        result = Impl.createDeclWithClangNode<ConstructorDecl>(
            clangNode, AccessLevel::Public, ctorName, loc,
            /*failable=*/false, /*FailabilityLoc=*/SourceLoc(),
            /*Async=*/false, /*AsyncLoc=*/SourceLoc(),
            /*Throws=*/false, /*ThrowsLoc=*/SourceLoc(),
            bodyParams, genericParams, dc);
      } else {
        auto resultTy = importedType.getType();

        FuncDecl *func =
            createFuncOrAccessor(Impl, loc, accessorInfo, name,
                                 nameLoc, genericParams, bodyParams, resultTy,
                                 /*async=*/false, /*throws=*/false, dc,
                                 clangNode);
        result = func;

        if (!dc->isModuleScopeContext()) {
          if (selfIsInOut)
            func->setSelfAccessKind(SelfAccessKind::Mutating);
          else
            func->setSelfAccessKind(SelfAccessKind::NonMutating);
          if (selfIdx) {
            func->setSelfIndex(selfIdx.value());
          } else {
            func->setStatic();
            func->setImportAsStaticMember();
          }
        }

        if (importedName.isSubscriptAccessor() && !importFuncWithoutSignature) {
          assert(func->getParameters()->size() == 1);
          auto typeDecl = dc->getSelfNominalTypeDecl();
          auto parameter = func->getParameters()->get(0);
          auto parameterType = parameter->getType();
          if (!typeDecl || !parameterType)
            return nullptr;
          if (parameter->isInOut())
            // Subscripts with inout parameters are not allowed in Swift.
            return nullptr;

          auto &getterAndSetter = Impl.cxxSubscripts[{ typeDecl,
                                                       parameterType }];

          switch (importedName.getAccessorKind()) {
          case ImportedAccessorKind::SubscriptGetter:
            getterAndSetter.first = func;
            break;
          case ImportedAccessorKind::SubscriptSetter:
            getterAndSetter.second = func;
            break;
          default:
            llvm_unreachable("invalid subscript kind");
          }

          Impl.markUnavailable(func, "use subscript");
        }
        // Someday, maybe this will need to be 'open' for C++ virtual methods.
        func->setAccess(AccessLevel::Public);
      }

      if (!isa<clang::CXXConstructorDecl>(decl) && !importedType) {
        if (!Impl.importFunctionReturnType(decl, result->getDeclContext())) {
          Impl.addImportDiagnostic(
              decl, Diagnostic(diag::unsupported_return_type, decl),
              decl->getSourceRange().getBegin());
          return nullptr;
        }
      }
      result->setIsObjC(false);
      result->setIsDynamic(false);

      Impl.recordImplicitUnwrapForDecl(result,
                                       importedType.isImplicitlyUnwrapped());

      if (dc->getSelfClassDecl())
        // FIXME: only if the class itself is not marked final
        result->getAttrs().add(new (Impl.SwiftContext)
                                   FinalAttr(/*IsImplicit=*/true));

      finishFuncDecl(decl, result);

      // If this is a compatibility stub, mark it as such.
      if (correctSwiftName)
        markAsVariant(result, *correctSwiftName);

      return result;
    }

    void finishFuncDecl(const clang::FunctionDecl *decl,
                        AbstractFunctionDecl *result) {
      // Set availability.
      if (decl->isVariadic()) {
        Impl.markUnavailable(result, "Variadic function is unavailable");
      }

      if (decl->hasAttr<clang::ReturnsTwiceAttr>()) {
        // The Clang 'returns_twice' attribute is used for functions like
        // 'vfork' or 'setjmp'. Because these functions may return control flow
        // of a Swift program to an arbitrary point, Swift's guarantees of
        // definitive initialization of variables cannot be upheld. As a result,
        // functions like these cannot be used in Swift.
        Impl.markUnavailable(
          result,
          "Functions that may return more than one time (annotated with the "
          "'returns_twice' attribute) are unavailable in Swift");
      }

      recordObjCOverride(result);
    }

    static bool hasUnsafeAPIAttr(const clang::Decl *decl) {
      return decl->hasAttrs() && llvm::any_of(decl->getAttrs(), [](auto *attr) {
               if (auto swiftAttr = dyn_cast<clang::SwiftAttrAttr>(attr))
                 return swiftAttr->getAttribute() == "import_unsafe";
               return false;
             });
    }

    static bool hasComputedPropertyAttr(const clang::Decl *decl) {
      return decl->hasAttrs() && llvm::any_of(decl->getAttrs(), [](auto *attr) {
               if (auto swiftAttr = dyn_cast<clang::SwiftAttrAttr>(attr))
                 return swiftAttr->getAttribute() == "import_computed_property";
               return false;
             });
    }

    Decl *VisitCXXMethodDecl(const clang::CXXMethodDecl *decl) {
      auto method = VisitFunctionDecl(decl);
      if (decl->isVirtual() && isa_and_nonnull<ValueDecl>(method)) {
        Impl.markUnavailable(
            cast<ValueDecl>(method),
            "virtual functions are not yet available in Swift");
      }

      if (Impl.SwiftContext.LangOpts.CxxInteropGettersSettersAsProperties ||
          hasComputedPropertyAttr(decl)) {
        if (auto funcDecl = dyn_cast_or_null<FuncDecl>(method)) {
          auto parent = funcDecl->getParent()->getSelfNominalTypeDecl();
          CXXMethodBridging bridgingInfo(decl);
          if (bridgingInfo.classify() == CXXMethodBridging::Kind::getter) {
            auto name = bridgingInfo.getClangName().drop_front(3);
            Impl.GetterSetterMap[parent][name].first = funcDecl;
          } else if (bridgingInfo.classify() ==
                     CXXMethodBridging::Kind::setter) {
            auto name = bridgingInfo.getClangName().drop_front(3);
            Impl.GetterSetterMap[parent][name].second = funcDecl;
          }
        }
      }

      return method;
    }

    Decl *VisitFieldDecl(const clang::FieldDecl *decl) {
      // Fields are imported as variables.
      llvm::Optional<ImportedName> correctSwiftName;
      ImportedName importedName;

      std::tie(importedName, correctSwiftName) = importFullName(decl);
      if (!importedName) {
        return nullptr;
      }
      if (correctSwiftName) {
        // FIXME: We should import this as a variant, but to do that, we'll also
        // need to make this a computed variable or otherwise fix how the rest
        // of the compiler thinks about stored properties in imported structs.
        // For now, just don't import it at all. (rdar://86069786)
        return nullptr;
      }

      auto name = importedName.getDeclName().getBaseIdentifier();

      auto dc =
          Impl.importDeclContextOf(decl, importedName.getEffectiveContext());
      if (!dc)
        return nullptr;

      // While importing the DeclContext, we might have imported the decl
      // itself.
      auto known = Impl.importDeclCached(decl, getVersion());
      if (known.has_value())
        return known.value();

      // TODO: do we want to emit a diagnostic here?
      // Types that are marked as foreign references cannot be stored by value.
      if (auto recordType =
              dyn_cast<clang::RecordType>(decl->getType().getCanonicalType())) {
        if (recordHasReferenceSemantics(recordType->getDecl()))
          return nullptr;
      }

      auto fieldType = decl->getType();
      ImportedType importedType = findOptionSetType(fieldType, Impl);

      if (!importedType)
        importedType =
            Impl.importType(decl->getType(), ImportTypeKind::RecordField,
                            ImportDiagnosticAdder(Impl, decl, decl->getLocation()),
                            isInSystemModule(dc), Bridgeability::None,
                            getImportTypeAttrs(decl));
      if (!importedType) {
        Impl.addImportDiagnostic(
            decl, Diagnostic(diag::record_field_not_imported, decl),
            decl->getSourceRange().getBegin());
        return nullptr;
      }

      auto type = importedType.getType();

      auto result =
        Impl.createDeclWithClangNode<VarDecl>(decl, AccessLevel::Public,
                              /*IsStatic*/ false,
                              VarDecl::Introducer::Var,
                              Impl.importSourceLoc(decl->getLocation()),
                              name, dc);
      if (decl->getType().isConstQualified()) {
        // Note that in C++ there are ways to change the values of const
        // members, so we don't use WriteImplKind::Immutable storage.
        assert(result->supportsMutation());
        result->overwriteSetterAccess(AccessLevel::Private);
      }
      result->setIsObjC(false);
      result->setIsDynamic(false);
      result->setInterfaceType(type);
      Impl.recordImplicitUnwrapForDecl(result,
                                       importedType.isImplicitlyUnwrapped());

      // Handle attributes.
      if (decl->hasAttr<clang::IBOutletAttr>())
        result->getAttrs().add(
            new (Impl.SwiftContext) IBOutletAttr(/*IsImplicit=*/false));
      // FIXME: Handle IBOutletCollection.

      // If this is a compatibility stub, handle it as such.
      if (correctSwiftName)
        // FIXME: Temporarily unreachable because of check above.
        markAsVariant(result, *correctSwiftName);

      return result;
    }

    Decl *VisitObjCIvarDecl(const clang::ObjCIvarDecl *decl) {
      // Disallow direct ivar access (and avoid conflicts with property names).
      return nullptr;
    }

    Decl *VisitObjCAtDefsFieldDecl(const clang::ObjCAtDefsFieldDecl *decl) {
      // @defs is an anachronism; ignore it.
      return nullptr;
    }

    Decl *VisitVarDecl(const clang::VarDecl *decl) {
      // Variables are imported as... variables.
      ImportedName importedName;
      llvm::Optional<ImportedName> correctSwiftName;
      std::tie(importedName, correctSwiftName) = importFullName(decl);
      if (!importedName) return nullptr;

      auto name = importedName.getDeclName().getBaseIdentifier();
      auto dc =
          Impl.importDeclContextOf(decl, importedName.getEffectiveContext());
      if (!dc)
        return nullptr;

      // If we've imported this variable as a member, it's a static
      // member.
      bool isStatic = false;
      if (dc->isTypeContext())
        isStatic = true;

      // For now we don't import static constexpr
      if (isStatic && decl->isConstexpr())
        return nullptr;

      auto introducer = Impl.shouldImportGlobalAsLet(decl->getType())
                        ? VarDecl::Introducer::Let
                        : VarDecl::Introducer::Var;
      auto result = Impl.createDeclWithClangNode<VarDecl>(decl,
                       AccessLevel::Public,
                       /*IsStatic*/isStatic, introducer,
                       Impl.importSourceLoc(decl->getLocation()),
                       name, dc);
      result->setIsObjC(false);
      result->setIsDynamic(false);

      // If imported as member, the member should be final.
      if (dc->getSelfClassDecl())
        result->getAttrs().add(new (Impl.SwiftContext)
                                 FinalAttr(/*IsImplicit=*/true));

      // If this is a compatibility stub, mark it as such.
      if (correctSwiftName)
        markAsVariant(result, *correctSwiftName);

      return result;
    }

    Decl *VisitImplicitParamDecl(const clang::ImplicitParamDecl *decl) {
      // Parameters are never directly imported.
      return nullptr;
    }

    Decl *VisitParmVarDecl(const clang::ParmVarDecl *decl) {
      // Parameters are never directly imported.
      return nullptr;
    }

    Decl *
    VisitNonTypeTemplateParmDecl(const clang::NonTypeTemplateParmDecl *decl) {
      // Note: templates are not imported.
      return nullptr;
    }

    Decl *VisitTemplateDecl(const clang::TemplateDecl *decl) {
      // Note: templates are not imported.
      return nullptr;
    }

    Decl *VisitFunctionTemplateDecl(const clang::FunctionTemplateDecl *decl) {
      ImportedName importedName;
      llvm::Optional<ImportedName> correctSwiftName;
      std::tie(importedName, correctSwiftName) =
          importFullName(decl->getAsFunction());
      if (!importedName)
        return nullptr;
      // All template parameters must be template type parameters.
      if (!llvm::all_of(*decl->getTemplateParameters(), [](auto param) {
            return isa<clang::TemplateTypeParmDecl>(param);
          }))
        return nullptr;
      return importFunctionDecl(decl->getAsFunction(), importedName,
                                correctSwiftName, llvm::None, decl);
    }

    Decl *VisitClassTemplateDecl(const clang::ClassTemplateDecl *decl) {
      ImportedName importedName;
      std::tie(importedName, std::ignore) = importFullName(decl);
      auto name = importedName.getDeclName().getBaseIdentifier();
      if (name.empty())
        return nullptr;

      if (Impl.importSymbolicCXXDecls)
        // Import an unspecialized C++ class template as a Swift value/class
        // type in symbolic mode.
        return Impl.importDecl(decl->getTemplatedDecl(), Impl.CurrentVersion);

      auto loc = Impl.importSourceLoc(decl->getLocation());
      auto dc = Impl.importDeclContextOf(
          decl, importedName.getEffectiveContext());

      SmallVector<GenericTypeParamDecl *, 4> genericParams;
      for (auto &param : *decl->getTemplateParameters()) {
        auto genericParamDecl =
            Impl.createDeclWithClangNode<GenericTypeParamDecl>(
                param, AccessLevel::Public, dc,
                Impl.SwiftContext.getIdentifier(param->getName()),
                Impl.importSourceLoc(param->getLocation()),
                /*ellipsisLoc*/ SourceLoc(), /*depth*/ 0,
                /*index*/ genericParams.size(), /*isParameterPack*/ false);
        genericParams.push_back(genericParamDecl);
      }
      auto genericParamList = GenericParamList::create(
          Impl.SwiftContext, loc, genericParams, loc);

      auto structDecl = Impl.createDeclWithClangNode<StructDecl>(
          decl, AccessLevel::Public, loc, name, loc, llvm::None,
          genericParamList, dc);

      auto attr = AvailableAttr::createPlatformAgnostic(
          Impl.SwiftContext, "Un-specialized class templates are not currently "
                             "supported. Please use a specialization of this "
                             "type.");
      structDecl->getAttrs().add(attr);

      return structDecl;
    }

    Decl *VisitUsingDecl(const clang::UsingDecl *decl) {
      // Using declarations are not imported.
      return nullptr;
    }

    Decl *VisitUsingShadowDecl(const clang::UsingShadowDecl *decl) {
      // Only import types for now.
      if (!isa<clang::TypeDecl>(decl->getUnderlyingDecl()))
        return nullptr;

      ImportedName importedName;
      llvm::Optional<ImportedName> correctSwiftName;
      std::tie(importedName, correctSwiftName) = importFullName(decl);
      auto Name = importedName.getDeclName().getBaseIdentifier();
      if (Name.empty())
        return nullptr;

      // If we've been asked to produce a compatibility stub, handle it via a
      // typealias.
      if (correctSwiftName)
        return importCompatibilityTypeAlias(decl, importedName,
                                            *correctSwiftName);

      auto DC =
          Impl.importDeclContextOf(decl, importedName.getEffectiveContext());
      if (!DC)
        return nullptr;

      Decl *SwiftDecl = Impl.importDecl(decl->getUnderlyingDecl(), getActiveSwiftVersion());
      if (!SwiftDecl)
        return nullptr;

      const TypeDecl *SwiftTypeDecl = dyn_cast<TypeDecl>(SwiftDecl);
      if (!SwiftTypeDecl)
        return nullptr;

      auto Loc = Impl.importSourceLoc(decl->getLocation());
      auto Result = Impl.createDeclWithClangNode<TypeAliasDecl>(
          decl,
          AccessLevel::Public,
          Impl.importSourceLoc(decl->getBeginLoc()),
          SourceLoc(), Name,
          Loc,
          /*genericparams*/nullptr, DC);
      Result->setUnderlyingType(SwiftTypeDecl->getDeclaredInterfaceType());

      return Result;
    }

    /// Add an @objc(name) attribute with the given, optional name expressed as
    /// selector.
    ///
    /// The importer should use this rather than adding the attribute directly.
    void addObjCAttribute(ValueDecl *decl, llvm::Optional<ObjCSelector> name) {
      auto &ctx = Impl.SwiftContext;
      if (name) {
        decl->getAttrs().add(ObjCAttr::create(ctx, name,
                                              /*implicitName=*/true));
      }
      decl->setIsObjC(true);
      decl->setIsDynamic(true);

      // If the declaration we attached the 'objc' attribute to is within a
      // type, record it in the type.
      if (auto contextTy = decl->getDeclContext()->getDeclaredInterfaceType()) {
        if (auto tyDecl = contextTy->getNominalOrBoundGenericNominal()) {
          if (auto method = dyn_cast<AbstractFunctionDecl>(decl)) {
            if (name)
              tyDecl->recordObjCMethod(method, *name);
          }
        }
      }
    }

    /// Add an @objc(name) attribute with the given, optional name expressed as
    /// selector.
    ///
    /// The importer should use this rather than adding the attribute directly.
    void addObjCAttribute(ValueDecl *decl, Identifier name) {
      addObjCAttribute(decl, ObjCSelector(Impl.SwiftContext, 0, name));
    }

    Decl *VisitObjCMethodDecl(const clang::ObjCMethodDecl *decl) {
      auto dc = Impl.importDeclContextOf(decl, decl->getDeclContext());
      if (!dc)
        return nullptr;

      // While importing the DeclContext, we might have imported the decl
      // itself.
      auto Known = Impl.importDeclCached(decl, getVersion());
      if (Known.has_value())
        return Known.value();

      ImportedName importedName;
      std::tie(importedName, std::ignore) = importFullName(decl);
      if (!importedName)
        return nullptr;

      // some ObjC method decls are imported as computed properties.
      switch(importedName.getAccessorKind()) {
      case ImportedAccessorKind::PropertyGetter:
        if (importedName.getAsyncInfo())
          return importObjCMethodAsEffectfulProp(decl, dc, importedName);

        // if there is no valid async info, then fall-back to method import.
      LLVM_FALLTHROUGH;

      case ImportedAccessorKind::PropertySetter:
      case ImportedAccessorKind::SubscriptGetter:
      case ImportedAccessorKind::SubscriptSetter:
      case ImportedAccessorKind::None:
        return importObjCMethodDecl(decl, dc, llvm::None);
      }
    }

    /// Check whether we have already imported a method with the given
    /// selector in the given context.
    bool isMethodAlreadyImported(ObjCSelector selector, ImportedName importedName,
                                 bool isInstance, const DeclContext *dc,
                    llvm::function_ref<bool(AbstractFunctionDecl *fn)> filter) {
      // We only need to perform this check for classes.
      auto *classDecl = dc->getSelfClassDecl();
      if (!classDecl)
        return false;

      auto matchesImportedDecl = [&](Decl *member) -> bool {
        auto *afd = dyn_cast<AbstractFunctionDecl>(member);
        if (!afd)
          return false;

        // Instance-ness must match.
        if (afd->isObjCInstanceMethod() != isInstance)
          return false;

        // Both the selector and imported name must match.
        if (afd->getObjCSelector() != selector ||
            importedName.getDeclName() != afd->getName()) {
          return false;
        }

        // Finally, the provided filter must match.
        return filter(afd);
      };

      // First check to see if we've already imported a method with the same
      // selector.
      auto importedMembers = Impl.MembersForNominal.find(classDecl);
      if (importedMembers != Impl.MembersForNominal.end()) {
        auto baseName = importedName.getDeclName().getBaseName();
        auto membersForName = importedMembers->second.find(baseName);
        if (membersForName != importedMembers->second.end()) {
          return llvm::any_of(membersForName->second, matchesImportedDecl);
        }
      }

      // Then, for a deserialized Swift class, check to see if it has brought in
      // any matching @objc methods.
      if (classDecl->wasDeserialized()) {
        auto &ctx = Impl.SwiftContext;
        TinyPtrVector<AbstractFunctionDecl *> deserializedMethods;
        ctx.loadObjCMethods(classDecl, selector, isInstance,
                            /*prevGeneration*/ 0, deserializedMethods,
                            /*swiftOnly*/ true);
        return llvm::any_of(deserializedMethods, matchesImportedDecl);
      }
      return false;
    }

    Decl *importObjCMethodDecl(const clang::ObjCMethodDecl *decl,
                               DeclContext *dc,
                               llvm::Optional<AccessorInfo> accessorInfo) {
      return importObjCMethodDecl(decl, dc, false, accessorInfo);
    }

  private:
    static bool
    isAcceptableResultOrNull(Decl *fn,
                             llvm::Optional<AccessorInfo> accessorInfo) {
      if (nullptr == fn)
        return true;

      // We can't safely re-use the same declaration if it disagrees
      // in accessor-ness.
      auto accessor = dyn_cast<AccessorDecl>(fn);
      if (!accessorInfo)
        return accessor == nullptr;

      // For consistency with previous behavior, allow it even if it's been
      // imported for some other property.
      return (accessor && accessor->getAccessorKind() == accessorInfo->Kind);
    }

    /// Creates a fresh VarDecl with a single 'get' accessor to represent
    /// an ObjC method that takes no arguments other than a completion-handler
    /// (where the handler may have an NSError argument).
    Decl *importObjCMethodAsEffectfulProp(const clang::ObjCMethodDecl *decl,
                                         DeclContext *dc,
                                         ImportedName name) {
      assert(name.getAsyncInfo() && "expected to be for an effectful prop!");

      if (name.getAccessorKind() != ImportedAccessorKind::PropertyGetter) {
         assert(false && "unexpected accessor kind as a computed prop");
         // NOTE: to handle setters, we would need to search for an existing
         // VarDecl corresponding to the one we might have already created
         // for the 'get' accessor, and tack this accessor onto it.
         return nullptr;
      }

      auto importedType = Impl.importEffectfulPropertyType(decl, dc, name,
                                                  isInSystemModule(dc));
      if (!importedType)
        return nullptr;

      auto type = importedType.getType();
      const auto access = getOverridableAccessLevel(dc);
      auto ident = name.getDeclName().getBaseIdentifier();
      auto propDecl = Impl.createDeclWithClangNode<VarDecl>(decl, access,
          /*IsStatic*/decl->isClassMethod(), VarDecl::Introducer::Var,
                        Impl.importSourceLoc(decl->getLocation()), ident, dc);
      propDecl->setInterfaceType(type);
      Impl.recordImplicitUnwrapForDecl(propDecl,
                                       importedType.isImplicitlyUnwrapped());

      ////
      // Build the getter
      AccessorInfo info{propDecl, AccessorKind::Get};
      auto *getter = cast_or_null<AccessorDecl>(
                      importObjCMethodDecl(decl, dc, info));
      if (!getter)
        return nullptr;

      Impl.importAttributes(decl, getter);

      ////
      // Combine the getter and the VarDecl into a computed property.

      // NOTE: since it's an ObjC method we're turning into a Swift computed
      // property, we infer that it has no ObjC 'atomic' guarantees.
      auto inferredObjCPropertyAttrs =
          static_cast<clang::ObjCPropertyAttribute::Kind>
          ( clang::ObjCPropertyAttribute::Kind::kind_readonly
          | clang::ObjCPropertyAttribute::Kind::kind_nonatomic
          | (decl->isInstanceMethod()
              ? clang::ObjCPropertyAttribute::Kind::kind_class
              : clang::ObjCPropertyAttribute::Kind::kind_noattr)
          );

      // FIXME: Fake locations for '{' and '}'?
      propDecl->setIsSetterMutating(false);
      Impl.makeComputed(propDecl, getter, /*setter=*/nullptr);
      addObjCAttribute(propDecl, Impl.importIdentifier(decl->getIdentifier()));
      applyPropertyOwnership(propDecl, inferredObjCPropertyAttrs);

      ////
      // Check correctness

      if (getter->getParameters()->size() != 0) {
        assert(false && "this should not happen!");
        return nullptr;
      }

      return propDecl;
    }

    Decl *importObjCMethodDecl(const clang::ObjCMethodDecl *decl,
                               DeclContext *dc, bool forceClassMethod,
                               llvm::Optional<AccessorInfo> accessorInfo) {
      // If we have an init method, import it as an initializer.
      if (isInitMethod(decl)) {
        // Cannot import initializers as accessors.
        if (accessorInfo)
          return nullptr;

        // Cannot force initializers into class methods.
        if (forceClassMethod)
          return nullptr;

        return importConstructor(decl, dc, /*implicit=*/false, llvm::None,
                                 /*required=*/false);
      }

      // Check whether we already imported this method.
      if (!forceClassMethod &&
          dc == Impl.importDeclContextOf(decl, decl->getDeclContext())) {
        // FIXME: Should also be able to do this for forced class
        // methods.
        auto known = Impl.ImportedDecls.find({decl->getCanonicalDecl(),
                                              getVersion()});
        if (known != Impl.ImportedDecls.end()) {
          auto decl = known->second;
          if (isAcceptableResultOrNull(decl, accessorInfo))
            return decl;
        }
      }

      ImportedName importedName;
      llvm::Optional<ImportedName> correctSwiftName;
      std::tie(importedName, correctSwiftName) = importFullName(decl);
      if (!importedName)
        return nullptr;

      // Check whether another method with the same selector has already been
      // imported into this context.
      ObjCSelector selector = Impl.importSelector(decl->getSelector());
      bool isInstance = decl->isInstanceMethod() && !forceClassMethod;
      if (isActiveSwiftVersion()) {
        if (isMethodAlreadyImported(selector, importedName, isInstance, dc,
                                    [&](AbstractFunctionDecl *fn) {
              return isAcceptableResultOrNull(fn, accessorInfo);
            })) {
          return nullptr;
        }
      }

      // Normal case applies when we're importing an older name, or when we're
      // not an init
      if (!isFactoryInit(importedName)) {
        auto result = importNonInitObjCMethodDecl(decl, dc, importedName,
                                                  selector, forceClassMethod,
                                                  accessorInfo);

        if (!isActiveSwiftVersion() && result)
          markAsVariant(result, *correctSwiftName);

        return result;
      }

      // We can't import a factory-initializer as an accessor.
      if (accessorInfo)
        return nullptr;

      // We don't want to suppress init formation in Swift 3 names. Instead, we
      // want the normal Swift 3 name, and a "raw" name for diagnostics. The
      // "raw" name will be imported as unavailable with a more helpful and
      // specific message.
      ++NumFactoryMethodsAsInitializers;
      ConstructorDecl *existing = nullptr;
      auto result =
          importConstructor(decl, dc, false, importedName.getInitKind(),
                            /*required=*/false, selector, importedName,
                            {decl->param_begin(), decl->param_size()},
                            decl->isVariadic(), existing);

      if (!isActiveSwiftVersion() && result)
        markAsVariant(result, *correctSwiftName);

      return result;
    }

    Decl *
    importNonInitObjCMethodDecl(const clang::ObjCMethodDecl *decl,
                                DeclContext *dc, ImportedName importedName,
                                ObjCSelector selector, bool forceClassMethod,
                                llvm::Optional<AccessorInfo> accessorInfo) {
      assert(dc->isTypeContext() && "Method in non-type context?");
      assert(isa<ClangModuleUnit>(dc->getModuleScopeContext()) &&
             "Clang method in Swift context?");

      // FIXME: We should support returning "Self.Type" for a root class
      // instance method mirrored as a class method, but it currently causes
      // problems for the type checker.
      if (forceClassMethod && decl->hasRelatedResultType())
        return nullptr;

      // Hack: avoid importing methods named "print" that aren't available in
      // the current version of Swift. We'd rather just let the user use
      // Swift.print in that case.
      if (!isActiveSwiftVersion() &&
          isPrintLikeMethod(importedName.getDeclName(), dc)) {
        return nullptr;
      }

      SpecialMethodKind kind = SpecialMethodKind::Regular;
      if (isNSDictionaryMethod(decl, Impl.objectForKeyedSubscript))
        kind = SpecialMethodKind::NSDictionarySubscriptGetter;

      // Import the type that this method will have.
      llvm::Optional<ForeignAsyncConvention> asyncConvention;
      llvm::Optional<ForeignErrorConvention> errorConvention;

      // If we have a property accessor, find the corresponding property
      // declaration.
      const clang::ObjCPropertyDecl *prop = nullptr;
      if (decl->isPropertyAccessor()) {
        prop = decl->findPropertyDecl();
        if (!prop) return nullptr;

        // If we're importing just the accessors (not the property), ignore
        // the property.
        if (shouldImportPropertyAsAccessors(prop))
          prop = nullptr;
      }

      const bool nameImportIsGetter =
        importedName.getAccessorKind() == ImportedAccessorKind::PropertyGetter;

      const bool needAccessorDecl = prop || nameImportIsGetter;

      // If we have an accessor-import request, but didn't find a property
      // or it's ImportedName doesn't indicate a getter,
      // then reject the import request.
      if (accessorInfo && !needAccessorDecl)
        return nullptr;

      // Import the parameter list and result type.
      ParameterList *bodyParams = nullptr;
      ImportedType importedType;
      if (prop) {
        // If the matching property is in a superclass, or if the getter and
        // setter are redeclared in a potentially incompatible way, bail out.
        if (prop->getGetterMethodDecl() != decl &&
            prop->getSetterMethodDecl() != decl)
          return nullptr;
        importedType =
            Impl.importAccessorParamsAndReturnType(dc, prop, decl,
                                                   isInSystemModule(dc),
                                                   importedName, &bodyParams);
      } else {
        importedType = Impl.importMethodParamsAndReturnType(
            dc, decl, decl->parameters(), decl->isVariadic(),
            isInSystemModule(dc), &bodyParams, importedName,
            asyncConvention, errorConvention, kind);

        if (!importedType) {
          Impl.addImportDiagnostic(
              decl, Diagnostic(diag::record_method_not_imported, decl),
              decl->getSourceRange().getBegin());
        }
      }
      if (!importedType)
        return nullptr;

      // Check whether we recursively imported this method
      if (!forceClassMethod &&
          dc == Impl.importDeclContextOf(decl, decl->getDeclContext())) {
        // FIXME: Should also be able to do this for forced class
        // methods.
        auto known = Impl.ImportedDecls.find({decl->getCanonicalDecl(),
                                              getVersion()});
        if (known != Impl.ImportedDecls.end()) {
          auto decl = known->second;
          if (isAcceptableResultOrNull(decl, accessorInfo))
            return decl;
        }
      }

      // Determine whether the function is throwing and/or async.
      bool throws = importedName.getErrorInfo().has_value();
      bool async = false;
      auto asyncInfo = importedName.getAsyncInfo();
      if (asyncInfo) {
        async = true;
        if (asyncInfo->isThrowing())
          throws = true;
      }

      auto resultTy = importedType.getType();
      auto isIUO = importedType.isImplicitlyUnwrapped();

      // If the method has a related result type that is representable
      // in Swift as DynamicSelf, do so.
      if (!needAccessorDecl && decl->hasRelatedResultType()) {
        resultTy = dc->getSelfInterfaceType();
        if (dc->getSelfClassDecl())
          resultTy = DynamicSelfType::get(resultTy, Impl.SwiftContext);
        isIUO = false;

        OptionalTypeKind nullability = OTK_ImplicitlyUnwrappedOptional;
        if (auto typeNullability = decl->getReturnType()->getNullability(
                                     Impl.getClangASTContext())) {
          // If the return type has nullability, use it.
          nullability = translateNullability(*typeNullability);
        }
        if (nullability != OTK_None && !errorConvention.has_value()) {
          resultTy = OptionalType::get(resultTy);
          isIUO = nullability == OTK_ImplicitlyUnwrappedOptional;
        }
      }

      auto result = createFuncOrAccessor(Impl,
                                         /*funcLoc*/ SourceLoc(), accessorInfo,
                                         importedName.getDeclName(),
                                         /*nameLoc*/ SourceLoc(),
                                         /*genericParams=*/nullptr, bodyParams,
                                         resultTy, async, throws, dc, decl);

      result->setAccess(decl->isDirectMethod() ? AccessLevel::Public
                                               : getOverridableAccessLevel(dc));

      // Optional methods in protocols.
      if (decl->getImplementationControl() == clang::ObjCMethodDecl::Optional &&
          isa<ProtocolDecl>(dc))
        result->getAttrs().add(new (Impl.SwiftContext)
                                      OptionalAttr(/*implicit*/false));

      // Mark class methods as static.
      if (decl->isClassMethod() || forceClassMethod)
        result->setStatic();
      if (forceClassMethod)
        result->setImplicit();

      Impl.recordImplicitUnwrapForDecl(result, isIUO);

      // Mark this method @objc.
      addObjCAttribute(result, selector);

      // If this method overrides another method, mark it as such.
      recordObjCOverride(result);

      // Make a note that we've imported this method into this context.
      recordMemberInContext(dc, result);

      // Record the error convention.
      if (errorConvention) {
        result->setForeignErrorConvention(*errorConvention);
      }

      // Record the async convention.
      if (asyncConvention) {
        result->setForeignAsyncConvention(*asyncConvention);
      }

      // Handle attributes.
      if (decl->hasAttr<clang::IBActionAttr>() &&
          isa<FuncDecl>(result) &&
          cast<FuncDecl>(result)->isPotentialIBActionTarget()) {
        result->getAttrs().add(
            new (Impl.SwiftContext) IBActionAttr(/*IsImplicit=*/false));
      }

      // FIXME: Is there an IBSegueAction equivalent?

      // Check whether there's some special method to import.
      if (!forceClassMethod) {
        if (dc == Impl.importDeclContextOf(decl, decl->getDeclContext()) &&
            !Impl.ImportedDecls[{decl->getCanonicalDecl(), getVersion()}])
          Impl.ImportedDecls[{decl->getCanonicalDecl(), getVersion()}]
            = result;

        if (importedName.isSubscriptAccessor()) {
          // If this was a subscript accessor, try to create a
          // corresponding subscript declaration.
          (void)importSubscript(result, decl);
        } else if (shouldAlsoImportAsClassMethod(result)) {
          // If we should import this instance method also as a class
          // method, do so and mark the result as an alternate
          // declaration.
          if (auto imported = importObjCMethodDecl(decl, dc,
                                                   /*forceClassMethod=*/true,
                                                   /*accessor*/ llvm::None))
            Impl.addAlternateDecl(result, cast<ValueDecl>(imported));
        }
      }

      return result;
    }

  public:
    /// Record the function or initializer overridden by the given Swift method.
    void recordObjCOverride(AbstractFunctionDecl *decl);

    /// Given an imported method, try to import it as a constructor.
    ///
    /// Objective-C methods in the 'init' family are imported as
    /// constructors in Swift, enabling object construction syntax, e.g.,
    ///
    /// \code
    /// // in objc: [[NSArray alloc] initWithCapacity:1024]
    /// NSArray(capacity: 1024)
    /// \endcode
    ConstructorDecl *importConstructor(const clang::ObjCMethodDecl *objcMethod,
                                       const DeclContext *dc, bool implicit,
                                       llvm::Optional<CtorInitializerKind> kind,
                                       bool required);

    /// Returns the latest "introduced" version on the current platform for
    /// \p D.
    llvm::VersionTuple findLatestIntroduction(const clang::Decl *D);

    /// Returns true if importing \p objcMethod will produce a "better"
    /// initializer than \p existingCtor.
    bool
    existingConstructorIsWorse(const ConstructorDecl *existingCtor,
                               const clang::ObjCMethodDecl *objcMethod,
                               CtorInitializerKind kind);

    /// Given an imported method, try to import it as a constructor.
    ///
    /// Objective-C methods in the 'init' family are imported as
    /// constructors in Swift, enabling object construction syntax, e.g.,
    ///
    /// \code
    /// // in objc: [[NSArray alloc] initWithCapacity:1024]
    /// NSArray(capacity: 1024)
    /// \endcode
    ///
    /// This variant of the function is responsible for actually binding the
    /// constructor declaration appropriately.
    ConstructorDecl *importConstructor(const clang::ObjCMethodDecl *objcMethod,
                                       const DeclContext *dc,
                                       bool implicit,
                                       CtorInitializerKind kind,
                                       bool required,
                                       ObjCSelector selector,
                                       ImportedName importedName,
                                       ArrayRef<const clang::ParmVarDecl*> args,
                                       bool variadic,
                                       ConstructorDecl *&existing);

    void recordObjCOverride(SubscriptDecl *subscript);

    /// Given either the getter or setter for a subscript operation,
    /// create the Swift subscript declaration.
    SubscriptDecl *importSubscript(Decl *decl,
                                   const clang::ObjCMethodDecl *objcMethod);

    /// Import the accessor and its attributes.
    AccessorDecl *importAccessor(const clang::ObjCMethodDecl *clangAccessor,
                                 AbstractStorageDecl *storage,
                                 AccessorKind accessorKind,
                                 DeclContext *dc);

  public:

    /// Recursively add the given protocol and its inherited protocols to the
    /// given vector, guarded by the known set of protocols.
    void addProtocols(ProtocolDecl *protocol,
                      SmallVectorImpl<ProtocolDecl *> &protocols,
                      llvm::SmallPtrSetImpl<ProtocolDecl *> &known);

    // Import the given Objective-C protocol list, along with any
    // implicitly-provided protocols, and attach them to the given
    // declaration.
    void importObjCProtocols(Decl *decl,
                             const clang::ObjCProtocolList &clangProtocols,
                             SmallVectorImpl<InheritedEntry> &inheritedTypes);

    // Returns None on error. Returns nullptr if there is no type param list to
    // import or we suppress its import, as in the case of NSArray, NSSet, and
    // NSDictionary.
    llvm::Optional<GenericParamList *>
    importObjCGenericParams(const clang::ObjCInterfaceDecl *decl,
                            DeclContext *dc);

    /// Import the members of all of the protocols to which the given
    /// Objective-C class, category, or extension explicitly conforms into
    /// the given list of members, so long as the method was not already
    /// declared in the class.
    ///
    /// FIXME: This whole thing is a hack, because name lookup should really
    /// just find these members when it looks in the protocol. Unfortunately,
    /// that's not something the name lookup code can handle right now, and
    /// it may still be necessary when the protocol's instance methods become
    /// class methods on a root class (e.g. NSObject-the-protocol's instance
    /// methods become class methods on NSObject).
    void importMirroredProtocolMembers(const clang::ObjCContainerDecl *decl,
                                       DeclContext *dc,
                                       llvm::Optional<DeclBaseName> name,
                                       SmallVectorImpl<Decl *> &newMembers);

    void importNonOverriddenMirroredMethods(DeclContext *dc,
                                  MutableArrayRef<MirroredMethodEntry> entries,
                                  SmallVectorImpl<Decl *> &newMembers);

    /// Import constructors from our superclasses (and their
    /// categories/extensions), effectively "inheriting" constructors.
    void importInheritedConstructors(const ClassDecl *classDecl,
                                     SmallVectorImpl<Decl *> &newMembers);

    Decl *VisitObjCCategoryDecl(const clang::ObjCCategoryDecl *decl) {
      // If the declaration is invalid, fail.
      if (decl->isInvalidDecl()) return nullptr;

      // Objective-C categories and extensions map to Swift extensions.
      if (importer::hasNativeSwiftDecl(decl))
        return nullptr;

      // Find the Swift class being extended.
      auto objcClass = castIgnoringCompatibilityAlias<ClassDecl>(
          Impl.importDecl(decl->getClassInterface(), getActiveSwiftVersion()));
      if (!objcClass)
        return nullptr;

      auto dc = Impl.importDeclContextOf(decl, decl->getDeclContext());
      if (!dc)
        return nullptr;

      auto loc = Impl.importSourceLoc(decl->getBeginLoc());
      auto result = ExtensionDecl::create(
                      Impl.SwiftContext, loc,
                      nullptr,
                      { }, dc, nullptr, decl);
      Impl.SwiftContext.evaluator.cacheOutput(ExtendedTypeRequest{result},
                                              objcClass->getDeclaredType());
      Impl.SwiftContext.evaluator.cacheOutput(ExtendedNominalRequest{result},
                                              std::move(objcClass));

      // Determine the type and generic args of the extension.
      if (objcClass->getGenericParams()) {
        result->setGenericSignature(objcClass->getGenericSignature());
      }

      // Create the extension declaration and record it.
      objcClass->addExtension(result);
      Impl.ImportedDecls[{decl, getVersion()}] = result;
      SmallVector<InheritedEntry, 4> inheritedTypes;
      importObjCProtocols(result, decl->getReferencedProtocols(),
                          inheritedTypes);
      result->setInherited(Impl.SwiftContext.AllocateCopy(inheritedTypes));
      result->setMemberLoader(&Impl, 0);

      return result;
    }

    template <typename T, typename U>
    T *resolveSwiftDeclImpl(const U *decl, Identifier name,
                            bool hasKnownSwiftName, ModuleDecl *module,
                            bool allowObjCMismatchFallback,
                            bool cacheResult) {
      const auto &languageVersion =
          Impl.SwiftContext.LangOpts.EffectiveLanguageVersion;

      auto isMatch = [&](const T *singleResult, bool baseNameMatches,
                         bool allowObjCMismatch) -> bool {
        const DeclAttributes &attrs = singleResult->getAttrs();

        // Skip versioned variants.
        if (attrs.isUnavailableInSwiftVersion(languageVersion))
          return false;

        // If Clang decl has a custom Swift name, then we know that the name we
        // did direct lookup for is correct.
        // 'allowObjCMismatch' shouldn't exist, but we need it for source
        // compatibility where a previous version of the compiler didn't check
        // @objc-ness at all.
        if (hasKnownSwiftName || allowObjCMismatch) {
          assert(baseNameMatches);
          return allowObjCMismatch || singleResult->isObjC();
        }

        // Skip if a different name is used for Objective-C.
        if (auto objcAttr = attrs.getAttribute<ObjCAttr>())
          if (auto objcName = objcAttr->getName())
            return objcName->getSimpleName() == name;

        return baseNameMatches && singleResult->isObjC();
      };

      // First look at Swift types with the same name.
      SmallVector<ValueDecl *, 4> swiftDeclsByName;
      module->lookupValue(name, NLKind::QualifiedLookup, swiftDeclsByName);
      T *found = nullptr;
      for (auto result : swiftDeclsByName) {
        if (auto singleResult = dyn_cast<T>(result)) {
          if (isMatch(singleResult, /*baseNameMatches=*/true,
                      /*allowObjCMismatch=*/false)) {
            if (found)
              return nullptr;
            found = singleResult;
          }
        }
      }

      if (!found && hasKnownSwiftName)
        return nullptr;

      if (!found) {
        // Try harder to find a match looking at just custom Objective-C names.
        // Limit what we deserialize to decls with an @objc attribute.
        SmallVector<Decl *, 4> matchingTopLevelDecls;

        // Get decls with a matching @objc attribute
        module->getTopLevelDeclsWhereAttributesMatch(
            matchingTopLevelDecls, [&name](const DeclAttributes attrs) -> bool {
              if (auto objcAttr = attrs.getAttribute<ObjCAttr>())
                if (auto objcName = objcAttr->getName())
                  return objcName->getSimpleName() == name;
              return false;
            });

        // Filter by decl kind
        for (auto result : matchingTopLevelDecls) {
          if (auto singleResult = dyn_cast<T>(result)) {
            if (found)
              return nullptr;
            found = singleResult;
          }
        }
      }

      if (!found && allowObjCMismatchFallback) {
        // Go back to the first list and find classes with matching Swift names
        // *even if the ObjC name doesn't match.*
        // This shouldn't be allowed but we need it for source compatibility;
        // people used `\@class SwiftNameOfClass` as a workaround for not
        // having the previous loop, and it "worked".
        for (auto result : swiftDeclsByName) {
          if (auto singleResult = dyn_cast<T>(result)) {
            if (isMatch(singleResult, /*baseNameMatches=*/true,
                        /*allowObjCMismatch=*/true)) {
              if (found)
                return nullptr;
              found = singleResult;
            }
          }
        }
      }

      if (found && cacheResult)
        Impl.ImportedDecls[{decl->getCanonicalDecl(),
                            getActiveSwiftVersion()}] = found;

      return found;
    }

    template <typename T, typename U>
    T *resolveSwiftDecl(const U *decl, Identifier name,
                        bool hasKnownSwiftName, ClangModuleUnit *clangModule) {
      if (auto overlay = clangModule->getOverlayModule())
        return resolveSwiftDeclImpl<T>(decl, name, hasKnownSwiftName, overlay,
                                       /*allowObjCMismatchFallback*/ true, /*cacheResult*/ true);
      if (clangModule == Impl.ImportedHeaderUnit) {
        // Use an index-based loop because new owners can come in as we're
        // iterating.
        for (size_t i = 0; i < Impl.ImportedHeaderOwners.size(); ++i) {
          ModuleDecl *owner = Impl.ImportedHeaderOwners[i];
          if (T *result =
                  resolveSwiftDeclImpl<T>(decl, name, hasKnownSwiftName, owner,
                                          /*allowObjCMismatchFallback*/ true, /*cacheResult*/ true))
            return result;
        }
      }
      return nullptr;
    }

    /// Given some forward declared Objective-C type `\@class Foo` or `\@protocol Bar`, this
    /// method attempts to find a matching @objc annotated Swift declaration `@objc class Foo {}`
    /// or `@objc protocol Bar {}`, in an imported Swift module. That is if the Clang node is in
    /// a Clang module, the Swift overlay for that module does not count as "non-local". Similarly,
    /// if the Clang node is in a bridging header, any owners of that header also do not count as
    /// "non-local". This is intended to find @objc exposed Swift declarations in a different module
    /// that share the name as the forward declaration.
    ///
    /// Pass \p hasKnownSwiftName when the Clang declaration is annotated with NS_SWIFT_NAME or similar,
    /// such that the @objc provided name is known.
    template <typename T, typename U>
    T* hasNonLocalNativeSwiftDecl(U *decl, Identifier name, bool hasKnownSwiftName) {
      assert(!decl->hasDefinition() && "This method is only intended to be used on incomplete Clang types");

      // We intentionally do not consider if the declaration has a clang::ExternalSourceSymbolAttr
      // attribute, since we can't know if the corresponding Swift definition is "local" (ie.
      // in the overlay or bridging header owner) or not.

      // Check first if the Swift definition is "local"
      auto owningClangModule = Impl.getClangModuleForDecl(decl, /*allowForwardDeclaration*/ true);
      if (owningClangModule && resolveSwiftDecl<T>(decl, name, hasKnownSwiftName, owningClangModule))
        return nullptr;

      // If not, check all imported Swift modules for a definition
      if (auto mainModule = Impl.SwiftContext.MainModule) {
        llvm::SmallVector<ValueDecl *> results;
        llvm::SmallVector<ImportedModule> importedModules;

        mainModule->getImportedModules(importedModules,
                                       ModuleDecl::getImportFilterAll());

        for (auto &import : importedModules) {
          if (import.importedModule->isNonSwiftModule())
            continue;

          if (T *result = resolveSwiftDeclImpl<T>(
                  decl, name, hasKnownSwiftName, import.importedModule,
                  /*allowObjCMismatchFallback*/ false, /*cacheResult*/ false))
            return result;
        }
      }

      return nullptr;
    }

    template <typename T, typename U>
    bool hasNativeSwiftDecl(const U *decl, Identifier name,
                            const DeclContext *dc, T *&swiftDecl,
                            bool hasKnownSwiftName = true) {
      if (!importer::hasNativeSwiftDecl(decl))
        return false;
      auto wrapperUnit = cast<ClangModuleUnit>(dc->getModuleScopeContext());
      swiftDecl = resolveSwiftDecl<T>(decl, name, hasKnownSwiftName,
                                      wrapperUnit);
      return true;
    }

    void markMissingSwiftDecl(ValueDecl *VD) {
      const char *message;
      if (isa<ClassDecl>(VD))
        message = "cannot find Swift declaration for this class";
      else if (isa<ProtocolDecl>(VD))
        message = "cannot find Swift declaration for this protocol";
      else
        llvm_unreachable("unknown bridged decl kind");
      auto attr = AvailableAttr::createPlatformAgnostic(Impl.SwiftContext,
                                                        message);
      VD->getAttrs().add(attr);
    }

    Decl *VisitObjCProtocolDecl(const clang::ObjCProtocolDecl *decl) {
      ImportedName importedName;
      llvm::Optional<ImportedName> correctSwiftName;
      std::tie(importedName, correctSwiftName) = importFullName(decl);
      if (!importedName) return nullptr;

      // If we've been asked to produce a compatibility stub, handle it via a
      // typealias.
      if (correctSwiftName)
        return importCompatibilityTypeAlias(decl, importedName,
                                            *correctSwiftName);

      Identifier name = importedName.getDeclName().getBaseIdentifier();
      bool hasKnownSwiftName = importedName.hasCustomName();

      if (!decl->hasDefinition()) {
        // Check if this protocol is implemented in its overlay.
        if (auto clangModule = Impl.getClangModuleForDecl(decl, true))
          if (auto native = resolveSwiftDecl<ProtocolDecl>(decl, name,
                                                           hasKnownSwiftName,
                                                           clangModule))
            return native;

        Impl.addImportDiagnostic(
            decl, Diagnostic(diag::forward_declared_protocol_label, decl),
            decl->getSourceRange().getBegin());

        if (Impl.ImportForwardDeclarations) {
          if (auto native = hasNonLocalNativeSwiftDecl<ProtocolDecl>(decl, name, hasKnownSwiftName)) {
            const ModuleDecl* moduleForNativeDecl = native->getParentModule();
            assert(moduleForNativeDecl);
            Impl.addImportDiagnostic(decl, Diagnostic(diag::forward_declared_protocol_clashes_with_imported_objc_Swift_protocol,
                  decl, Decl::getDescriptiveKindName(native->getDescriptiveKind()), moduleForNativeDecl->getNameStr()),
                decl->getSourceRange().getBegin());
          } else {
            auto result = Impl.createDeclWithClangNode<ProtocolDecl>(
                decl, AccessLevel::Public,
                Impl.getClangModuleForDecl(decl->getCanonicalDecl(),
                                           /*allowForwardDeclaration=*/true),
                Impl.importSourceLoc(decl->getBeginLoc()),
                Impl.importSourceLoc(decl->getLocation()), name,
                ArrayRef<PrimaryAssociatedTypeName>(), llvm::None,
                /*TrailingWhere=*/nullptr);

            Impl.ImportedDecls[{decl->getCanonicalDecl(), getVersion()}] = result;
            result->setAddedImplicitInitializers(); // suppress all initializers
            addObjCAttribute(result,
                            Impl.importIdentifier(decl->getIdentifier()));
            result->setImplicit();
            auto attr = AvailableAttr::createPlatformAgnostic(
                Impl.SwiftContext,
                "This Objective-C protocol has only been forward-declared; "
                "import its owning module to use it");
            result->getAttrs().add(attr);
            result->getAttrs().add(new (Impl.SwiftContext)
                                      ForbidSerializingReferenceAttr(true));
            return result;
          }
        }

        forwardDeclaration = true;
        return nullptr;
      }

      decl = decl->getDefinition();

      auto dc =
          Impl.importDeclContextOf(decl, importedName.getEffectiveContext());
      if (!dc)
        return nullptr;

      ProtocolDecl *nativeDecl;
      bool declaredNative = hasNativeSwiftDecl(decl, name, dc, nativeDecl);
      if (declaredNative && nativeDecl)
        return nativeDecl;

      // Create the protocol declaration and record it.
      auto result = Impl.createDeclWithClangNode<ProtocolDecl>(
          decl, AccessLevel::Public, dc,
          Impl.importSourceLoc(decl->getBeginLoc()),
          Impl.importSourceLoc(decl->getLocation()), name,
          ArrayRef<PrimaryAssociatedTypeName>(), llvm::None,
          /*TrailingWhere=*/nullptr);

      addObjCAttribute(result, Impl.importIdentifier(decl->getIdentifier()));

      if (declaredNative)
        markMissingSwiftDecl(result);

      Impl.ImportedDecls[{decl->getCanonicalDecl(), getVersion()}] = result;

      // Import protocols this protocol conforms to.
      SmallVector<InheritedEntry, 4> inheritedTypes;
      importObjCProtocols(result, decl->getReferencedProtocols(),
                          inheritedTypes);
      result->setInherited(Impl.SwiftContext.AllocateCopy(inheritedTypes));

      result->setMemberLoader(&Impl, 0);

      return result;
    }

    // Add inferred attributes.
    void addInferredAttributes(Decl *decl, unsigned attributes) {
      using namespace inferred_attributes;
      if (attributes & requires_stored_property_inits) {
        auto a = new (Impl.SwiftContext)
          RequiresStoredPropertyInitsAttr(/*IsImplicit=*/true);
        decl->getAttrs().add(a);
      }
    }

    Decl *VisitObjCInterfaceDecl(const clang::ObjCInterfaceDecl *decl) {

      auto createFakeClass = [=](Identifier name, bool cacheResult,
                                     bool inheritFromNSObject,
                                     DeclContext *dc = nullptr) -> ClassDecl * {
        if (!dc) {
          dc = Impl.getClangModuleForDecl(decl->getCanonicalDecl(),
                                          /*allowForwardDeclaration=*/true);
        }

        auto result = Impl.createDeclWithClangNode<ClassDecl>(
            decl, AccessLevel::Public, SourceLoc(), name, SourceLoc(),
            llvm::None, nullptr, dc,
            /*isActor*/ false);
        if (cacheResult)
          Impl.ImportedDecls[{decl->getCanonicalDecl(), getVersion()}] = result;

        if (inheritFromNSObject)
          result->setSuperclass(Impl.getNSObjectType());
        else
          result->setSuperclass(Type());

        result->setAddedImplicitInitializers(); // suppress all initializers
        result->setHasMissingVTableEntries(false);
        addObjCAttribute(result, Impl.importIdentifier(decl->getIdentifier()));
        return result;
      };

      // Special case for Protocol, which gets forward-declared as an ObjC
      // class which is hidden in modern Objective-C runtimes.
      // We treat it as a foreign class (like a CF type) because it doesn't
      // have a real public class object.
      clang::ASTContext &clangCtx = Impl.getClangASTContext();
      if (decl->getCanonicalDecl() ==
          clangCtx.getObjCProtocolDecl()->getCanonicalDecl()) {
        Type nsObjectTy = Impl.getNSObjectType();
        if (!nsObjectTy)
          return nullptr;
        const ClassDecl *nsObjectDecl =
          nsObjectTy->getClassOrBoundGenericClass();

        auto result = createFakeClass(Impl.SwiftContext.Id_Protocol,
                                          /* cacheResult */ false,
                                          /* inheritFromNSObject */ false,
                                          nsObjectDecl->getDeclContext());
        result->setForeignClassKind(ClassDecl::ForeignKind::RuntimeOnly);
        return result;
      }

      if (auto *definition = decl->getDefinition())
        decl = definition;

      ImportedName importedName;
      llvm::Optional<ImportedName> correctSwiftName;
      std::tie(importedName, correctSwiftName) = importFullName(decl);
      if (!importedName) return nullptr;

      // If we've been asked to produce a compatibility stub, handle it via a
      // typealias.
      if (correctSwiftName)
        return importCompatibilityTypeAlias(decl, importedName,
                                            *correctSwiftName);

      auto name = importedName.getDeclName().getBaseIdentifier();
      bool hasKnownSwiftName = importedName.hasCustomName();

      if (!decl->hasDefinition()) {
        // Check if this class is implemented in its overlay.
        if (auto clangModule = Impl.getClangModuleForDecl(decl, true)) {
          if (auto native = resolveSwiftDecl<ClassDecl>(decl, name,
                                                        hasKnownSwiftName,
                                                        clangModule)) {
            return native;
          }
        }

        Impl.addImportDiagnostic(
            decl, Diagnostic(diag::forward_declared_interface_label, decl),
            decl->getSourceRange().getBegin());

        if (Impl.ImportForwardDeclarations) {
          if (auto native = hasNonLocalNativeSwiftDecl<ClassDecl>(decl, name, hasKnownSwiftName)) {
            const ModuleDecl* moduleForNativeDecl = native->getParentModule();
            assert(moduleForNativeDecl);
            Impl.addImportDiagnostic(decl, Diagnostic(diag::forward_declared_interface_clashes_with_imported_objc_Swift_interface,
                  decl, Decl::getDescriptiveKindName(native->getDescriptiveKind()), moduleForNativeDecl->getNameStr()),
                decl->getSourceRange().getBegin());
          } else {
            // Fake it by making an unavailable opaque @objc root class.
            auto result = createFakeClass(name, /* cacheResult */ true,
                                              /* inheritFromNSObject */ true);
            result->setImplicit();
            auto attr = AvailableAttr::createPlatformAgnostic(Impl.SwiftContext,
                "This Objective-C class has only been forward-declared; "
                "import its owning module to use it");
            result->getAttrs().add(attr);
            result->getAttrs().add(
                new (Impl.SwiftContext) ForbidSerializingReferenceAttr(true));
            return result;
          }
        }

        forwardDeclaration = true;
        return nullptr;
      }

      auto dc =
          Impl.importDeclContextOf(decl, importedName.getEffectiveContext());
      if (!dc)
        return nullptr;

      ClassDecl *nativeDecl;
      bool declaredNative = hasNativeSwiftDecl(decl, name, dc, nativeDecl);
      if (declaredNative && nativeDecl)
        return nativeDecl;

      auto access = AccessLevel::Open;
      if (decl->hasAttr<clang::ObjCSubclassingRestrictedAttr>() &&
          Impl.SwiftContext.isSwiftVersionAtLeast(5)) {
        access = AccessLevel::Public;
      }

      // Create the class declaration and record it.
      auto result = Impl.createDeclWithClangNode<ClassDecl>(
          decl, access, Impl.importSourceLoc(decl->getBeginLoc()), name,
          Impl.importSourceLoc(decl->getLocation()), llvm::None, nullptr, dc,
          /*isActor*/ false);

      // Import generic arguments, if any.
      if (auto gpImportResult = importObjCGenericParams(decl, dc)) {
        auto genericParams = *gpImportResult;
        if (genericParams) {
          result->getASTContext().evaluator.cacheOutput(
                GenericParamListRequest{result}, std::move(genericParams));

          auto sig = Impl.buildGenericSignature(genericParams, dc);
          result->setGenericSignature(sig);
        }
      } else {
        return nullptr;
      }

      Impl.ImportedDecls[{decl->getCanonicalDecl(), getVersion()}] = result;
      addObjCAttribute(result, Impl.importIdentifier(decl->getIdentifier()));

      if (declaredNative)
        markMissingSwiftDecl(result);
      if (decl->getAttr<clang::ObjCRuntimeVisibleAttr>()) {
        result->setForeignClassKind(ClassDecl::ForeignKind::RuntimeOnly);
      }

      // If this Objective-C class has a supertype, import it.
      SmallVector<InheritedEntry, 4> inheritedTypes;
      Type superclassType;
      if (decl->getSuperClass()) {
        clang::QualType clangSuperclassType =
          decl->getSuperClassType()->stripObjCKindOfTypeAndQuals(clangCtx);
        clangSuperclassType =
          clangCtx.getObjCObjectPointerType(clangSuperclassType);
        superclassType = Impl.importTypeIgnoreIUO(
            clangSuperclassType, ImportTypeKind::Abstract,
            ImportDiagnosticAdder(Impl, decl, decl->getLocation()),
            isInSystemModule(dc), Bridgeability::None, ImportTypeAttrs());
        if (superclassType) {
          assert(superclassType->is<ClassType>() ||
                 superclassType->is<BoundGenericClassType>());
          inheritedTypes.push_back(TypeLoc::withoutLoc(superclassType));
        }
      }
      result->setSuperclass(superclassType);

      // Import protocols this class conforms to.
      importObjCProtocols(result, decl->getReferencedProtocols(),
                          inheritedTypes);
      result->setInherited(Impl.SwiftContext.AllocateCopy(inheritedTypes));

      // Add inferred attributes.
#define INFERRED_ATTRIBUTES(ModuleName, ClassName, AttributeSet)               \
  if (name.str().equals(#ClassName) &&                                         \
      result->getParentModule()->getName().str().equals(#ModuleName)) {        \
    using namespace inferred_attributes;                                       \
    addInferredAttributes(result, AttributeSet);                               \
  }
#include "InferredAttributes.def"

      if (decl->isArcWeakrefUnavailable())
        result->setIsIncompatibleWithWeakReferences();

      result->setHasMissingVTableEntries(false);
      result->setMemberLoader(&Impl, 0);

      return result;
    }

    Decl *VisitObjCImplDecl(const clang::ObjCImplDecl *decl) {
      // Implementations of Objective-C classes and categories are not
      // reflected into Swift.
      return nullptr;
    }

    Decl *VisitObjCPropertyDecl(const clang::ObjCPropertyDecl *decl) {
      auto dc = Impl.importDeclContextOf(decl, decl->getDeclContext());
      if (!dc)
        return nullptr;

      // While importing the DeclContext, we might have imported the decl
      // itself.
      auto Known = Impl.importDeclCached(decl, getVersion());
      if (Known.has_value())
        return Known.value();

      return importObjCPropertyDecl(decl, dc);
    }

    /// Hack: Handle the case where a property is declared \c readonly in the
    /// main class interface (either explicitly or because of an adopted
    /// protocol) and then \c readwrite in a category/extension.
    ///
    /// \see VisitObjCPropertyDecl
    void handlePropertyRedeclaration(VarDecl *original,
                                     const clang::ObjCPropertyDecl *redecl) {
      // If the property isn't from Clang, we can't safely update it.
      if (!original->hasClangNode())
        return;

      // If the original declaration was implicit, we may want to change that.
      if (original->isImplicit() && !redecl->isImplicit() &&
          !isa<clang::ObjCProtocolDecl>(redecl->getDeclContext()))
        original->setImplicit(false);

      if (!original->getAttrs().hasAttribute<ReferenceOwnershipAttr>() &&
          !original->getAttrs().hasAttribute<NSCopyingAttr>()) {
        applyPropertyOwnership(original,
                               redecl->getPropertyAttributesAsWritten());
      }

      auto clangSetter = redecl->getSetterMethodDecl();
      if (!clangSetter)
        return;

      // The only other transformation we know how to do safely is add a
      // setter. If the property is already settable, we're done.
      if (original->isSettable(nullptr))
        return;

      AccessorDecl *setter = importAccessor(clangSetter,
                                            original, AccessorKind::Set,
                                            original->getDeclContext());
      if (!setter)
        return;

      // Check that the redeclared property's setter uses the same type as the
      // original property. Objective-C can get away with the types being
      // different (usually in something like nullability), but for Swift it's
      // an AST invariant that's assumed and asserted elsewhere. If the type is
      // different, just drop the setter, and leave the property as get-only.
      assert(setter->getParameters()->size() == 1);
      const ParamDecl *param = setter->getParameters()->get(0);
      if (!param->getInterfaceType()->isEqual(original->getInterfaceType()))
        return;

      original->setComputedSetter(setter);
    }

    Decl *importObjCPropertyDecl(const clang::ObjCPropertyDecl *decl,
                                DeclContext *dc) {
      assert(dc);

      ImportedName importedName;
      llvm::Optional<ImportedName> correctSwiftName;
      std::tie(importedName, correctSwiftName) = importFullName(decl);
      auto name = importedName.getDeclName().getBaseIdentifier();
      if (name.empty())
        return nullptr;

      if (shouldImportPropertyAsAccessors(decl))
        return nullptr;

      VarDecl *overridden = nullptr;
      // Check whether there is a function with the same name as this
      // property. If so, suppress the property; the user will have to use
      // the methods directly, to avoid ambiguities.
      if (auto *subject = dc->getSelfClassDecl()) {
        if (auto *classDecl = dyn_cast<ClassDecl>(dc)) {
          // Start looking into the superclass.
          subject = classDecl->getSuperclassDecl();
        }

        bool foundMethod = false;
        std::tie(overridden, foundMethod)
          = identifyNearestOverriddenDecl(Impl, dc, decl, name, subject);

        if (foundMethod && !overridden)
          return nullptr;

        if (overridden) {
          const DeclContext *overrideContext = overridden->getDeclContext();
          // It's okay to compare interface types directly because Objective-C
          // does not have constrained extensions.
          if (overrideContext != dc && overridden->hasClangNode() &&
              overrideContext->getSelfNominalTypeDecl()
                == dc->getSelfNominalTypeDecl()) {
            // We've encountered a redeclaration of the property.
            handlePropertyRedeclaration(overridden, decl);
            return nullptr;
          }
        }

        // Try searching the class for a property redeclaration. We can use
        // the redeclaration to refine the already-imported property with a
        // setter and also cut off any double-importing behavior.
        auto *redecl
            = identifyPropertyRedeclarationPoint(Impl, decl,
                                                 dc->getSelfClassDecl(), name);
        if (redecl) {
          handlePropertyRedeclaration(redecl, decl);
          return nullptr;
        }
      }

      auto fieldType = decl->getType();
      ImportedType importedType = findOptionSetType(fieldType, Impl);

      if (!importedType)
        importedType = Impl.importPropertyType(decl, isInSystemModule(dc));
      if (!importedType) {
        Impl.addImportDiagnostic(
            decl, Diagnostic(diag::objc_property_not_imported, decl),
            decl->getSourceRange().getBegin());
        return nullptr;
      }

      // Check whether the property already got imported.
      if (dc == Impl.importDeclContextOf(decl, decl->getDeclContext())) {
        auto known = Impl.ImportedDecls.find({decl->getCanonicalDecl(),
                                              getVersion()});
        if (known != Impl.ImportedDecls.end())
          return known->second;
      }

      auto type = importedType.getType();
      const auto access = decl->isDirectProperty() ? AccessLevel::Public
                                                   : getOverridableAccessLevel(dc);
      auto result = Impl.createDeclWithClangNode<VarDecl>(decl, access,
          /*IsStatic*/decl->isClassProperty(), VarDecl::Introducer::Var,
          Impl.importSourceLoc(decl->getLocation()), name, dc);
      result->setInterfaceType(type);
      Impl.recordImplicitUnwrapForDecl(result,
                                       importedType.isImplicitlyUnwrapped());

      // Recover from a missing getter in no-asserts builds. We're still not
      // sure under what circumstances this occurs, but we shouldn't crash.
      auto clangGetter = decl->getGetterMethodDecl();
      assert(clangGetter && "ObjC property without getter");
      if (!clangGetter)
        return nullptr;

      // Import the getter.
      AccessorDecl *getter = importAccessor(clangGetter, result,
                                            AccessorKind::Get, dc);
      if (!getter)
        return nullptr;

      // Import the setter, if there is one.
      AccessorDecl *setter = nullptr;
      if (auto clangSetter = decl->getSetterMethodDecl()) {
        setter = importAccessor(clangSetter, result, AccessorKind::Set, dc);
        if (!setter)
          return nullptr;
      }

      // Turn this into a computed property.
      // FIXME: Fake locations for '{' and '}'?
      result->setIsSetterMutating(false);
      Impl.makeComputed(result, getter, setter);
      addObjCAttribute(result, Impl.importIdentifier(decl->getIdentifier()));
      applyPropertyOwnership(result, decl->getPropertyAttributesAsWritten());

      // Handle attributes.
      if (decl->hasAttr<clang::IBOutletAttr>())
        result->getAttrs().add(
            new (Impl.SwiftContext) IBOutletAttr(/*IsImplicit=*/false));
      if (decl->getPropertyImplementation() == clang::ObjCPropertyDecl::Optional
          && isa<ProtocolDecl>(dc) &&
          !result->getAttrs().hasAttribute<OptionalAttr>())
        result->getAttrs().add(new (Impl.SwiftContext)
                                      OptionalAttr(/*implicit*/false));
      // FIXME: Handle IBOutletCollection.

      // Only record overrides of class members.
      if (overridden) {
        result->setOverriddenDecl(overridden);
        getter->setOverriddenDecl(overridden->getParsedAccessor(AccessorKind::Get));
        if (auto parentSetter = overridden->getParsedAccessor(AccessorKind::Set))
          if (setter)
            setter->setOverriddenDecl(parentSetter);
      }

      // If this is a compatibility stub, mark it as such.
      if (correctSwiftName)
        markAsVariant(result, *correctSwiftName);

      recordMemberInContext(dc, result);
      return result;
    }

    Decl *
    VisitObjCCompatibleAliasDecl(const clang::ObjCCompatibleAliasDecl *decl) {
      // Import Objective-C's @compatibility_alias as typealias.
      EffectiveClangContext effectiveContext(decl->getDeclContext()->getRedeclContext());
      auto dc = Impl.importDeclContextOf(decl, effectiveContext);
      if (!dc) return nullptr;

      ImportedName importedName;
      std::tie(importedName, std::ignore) = importFullName(decl);
      auto name = importedName.getDeclName().getBaseIdentifier();

      if (name.empty()) return nullptr;
      Decl *importedDecl =
          Impl.importDecl(decl->getClassInterface(), getActiveSwiftVersion());
      auto typeDecl = dyn_cast_or_null<TypeDecl>(importedDecl);
      if (!typeDecl) return nullptr;

      // Create typealias.
      TypeAliasDecl *typealias = nullptr;
      typealias = Impl.createDeclWithClangNode<TypeAliasDecl>(
                    decl, AccessLevel::Public,
                    Impl.importSourceLoc(decl->getBeginLoc()),
                    SourceLoc(), name,
                    Impl.importSourceLoc(decl->getLocation()),
                    /*genericparams=*/nullptr, dc);

      if (auto *GTD = dyn_cast<GenericTypeDecl>(typeDecl)) {
        typealias->setGenericSignature(GTD->getGenericSignature());
        if (GTD->isGeneric()) {
          typealias->getASTContext().evaluator.cacheOutput(
                GenericParamListRequest{typealias},
                std::move(GTD->getGenericParams()->clone(typealias)));
        }
      }

      typealias->setUnderlyingType(typeDecl->getDeclaredInterfaceType());
      return typealias;
    }

    Decl *VisitLinkageSpecDecl(const clang::LinkageSpecDecl *decl) {
      // Linkage specifications are not imported.
      return nullptr;
    }

    Decl *VisitObjCPropertyImplDecl(const clang::ObjCPropertyImplDecl *decl) {
      // @synthesize and @dynamic are not imported, since they are not part
      // of the interface to a class.
      return nullptr;
    }

    Decl *VisitFileScopeAsmDecl(const clang::FileScopeAsmDecl *decl) {
      return nullptr;
    }

    Decl *VisitAccessSpecDecl(const clang::AccessSpecDecl *decl) {
      return nullptr;
    }

    Decl *VisitFriendTemplateDecl(const clang::FriendTemplateDecl *decl) {
      // Friends are not imported; Swift has a different access control
      // mechanism.
      return nullptr;
    }

    Decl *VisitStaticAssertDecl(const clang::StaticAssertDecl *decl) {
      // Static assertions are an implementation detail.
      return nullptr;
    }

    Decl *VisitBlockDecl(const clang::BlockDecl *decl) {
      // Blocks are not imported (although block types can be imported).
      return nullptr;
    }

    Decl *VisitClassScopeFunctionSpecializationDecl(
                 const clang::ClassScopeFunctionSpecializationDecl *decl) {
      // Note: templates are not imported.
      return nullptr;
    }

    Decl *VisitImportDecl(const clang::ImportDecl *decl) {
      // Transitive module imports are not handled at the declaration level.
      // Rather, they are understood from the module itself.
      return nullptr;
    }
  };
} // end anonymous namespace

/// Try to strip "Mutable" out of a type name.
static clang::IdentifierInfo *
getImmutableCFSuperclassName(const clang::TypedefNameDecl *decl, clang::ASTContext &ctx) {
  StringRef name = decl->getName();

  // Split at the first occurrence of "Mutable".
  StringRef _mutable = "Mutable";
  auto mutableIndex = camel_case::findWord(name, _mutable);
  if (mutableIndex == StringRef::npos)
    return nullptr;

  StringRef namePrefix = name.substr(0, mutableIndex);
  StringRef nameSuffix = name.substr(mutableIndex + _mutable.size());

  // Abort if "Mutable" appears twice.
  if (camel_case::findWord(nameSuffix, _mutable) != StringRef::npos)
    return nullptr;

  llvm::SmallString<128> buffer;
  buffer += namePrefix;
  buffer += nameSuffix;
  return &ctx.Idents.get(buffer.str());
}

/// Check whether this CF typedef is a Mutable type, and if so,
/// look for a non-Mutable typedef.
///
/// If the "subclass" is:
///   typedef struct __foo *XXXMutableYYY;
/// then we look for a "superclass" that matches:
///   typedef const struct __foo *XXXYYY;
static Type findImmutableCFSuperclass(ClangImporter::Implementation &impl,
                                      const clang::TypedefNameDecl *decl,
                                      CFPointeeInfo subclassInfo) {
  // If this type is already immutable, it has no immutable
  // superclass.
  if (subclassInfo.isConst())
    return Type();

  // If this typedef name does not contain "Mutable", it has no
  // immutable superclass.
  auto superclassName =
      getImmutableCFSuperclassName(decl, impl.getClangASTContext());
  if (!superclassName)
    return Type();

  // Look for a typedef that successfully classifies as a CF
  // typedef with the same underlying record.
  auto superclassTypedef = impl.lookupTypedef(superclassName);
  if (!superclassTypedef)
    return Type();
  auto superclassInfo = CFPointeeInfo::classifyTypedef(superclassTypedef);
  if (!superclassInfo || !superclassInfo.isRecord() ||
      !declaresSameEntity(superclassInfo.getRecord(), subclassInfo.getRecord()))
    return Type();

  // Try to import the superclass.
  Decl *importedSuperclassDecl =
      impl.importDeclReal(superclassTypedef, impl.CurrentVersion);
  if (!importedSuperclassDecl)
    return Type();

  auto importedSuperclass =
      cast<TypeDecl>(importedSuperclassDecl)->getDeclaredInterfaceType();
  assert(importedSuperclass->is<ClassType>() && "must have class type");
  return importedSuperclass;
}

/// Attempt to find a superclass for the given CF typedef.
static Type findCFSuperclass(ClangImporter::Implementation &impl,
                             const clang::TypedefNameDecl *decl,
                             CFPointeeInfo info) {
  if (Type immutable = findImmutableCFSuperclass(impl, decl, info))
    return immutable;

  // TODO: use NSObject if it exists?
  return Type();
}

ClassDecl *
SwiftDeclConverter::importCFClassType(const clang::TypedefNameDecl *decl,
                                      Identifier className, CFPointeeInfo info,
                                      EffectiveClangContext effectiveContext) {
  auto dc = Impl.importDeclContextOf(decl, effectiveContext);
  if (!dc)
    return nullptr;

  Type superclass = findCFSuperclass(Impl, decl, info);

  // TODO: maybe use NSObject as the superclass if we can find it?
  // TODO: try to find a non-mutable type to use as the superclass.

  auto theClass = Impl.createDeclWithClangNode<ClassDecl>(
      decl, AccessLevel::Public, SourceLoc(), className, SourceLoc(),
      llvm::None, nullptr, dc, /*isActor*/ false);
  theClass->setSuperclass(superclass);
  theClass->setAddedImplicitInitializers(); // suppress all initializers
  theClass->setHasMissingVTableEntries(false);
  theClass->setForeignClassKind(ClassDecl::ForeignKind::CFType);
  addObjCAttribute(theClass, llvm::None);

  if (superclass) {
    SmallVector<InheritedEntry, 4> inheritedTypes;
    inheritedTypes.push_back(TypeLoc::withoutLoc(superclass));
    theClass->setInherited(Impl.SwiftContext.AllocateCopy(inheritedTypes));
  }

  Impl.addSynthesizedProtocolAttrs(theClass, {KnownProtocolKind::CFObject});

  // Look for bridging attributes on the clang record.  We can
  // just check the most recent redeclaration, which will inherit
  // any attributes from earlier declarations.
  auto record = info.getRecord()->getMostRecentDecl();
  if (info.isConst()) {
    if (auto attr = record->getAttr<clang::ObjCBridgeAttr>()) {
      // Record the Objective-C class to which this CF type is toll-free
      // bridged.
      if (ClassDecl *objcClass = dynCastIgnoringCompatibilityAlias<ClassDecl>(
              Impl.importDeclByName(attr->getBridgedType()->getName()))) {
        theClass->getAttrs().add(new (Impl.SwiftContext)
                                     ObjCBridgedAttr(objcClass));
      }
    }
  } else {
    if (auto attr = record->getAttr<clang::ObjCBridgeMutableAttr>()) {
      // Record the Objective-C class to which this CF type is toll-free
      // bridged.
      if (ClassDecl *objcClass = dynCastIgnoringCompatibilityAlias<ClassDecl>(
              Impl.importDeclByName(attr->getBridgedType()->getName()))) {
        theClass->getAttrs().add(new (Impl.SwiftContext)
                                     ObjCBridgedAttr(objcClass));
      }
    }
  }

  return theClass;
}

Decl *SwiftDeclConverter::importCompatibilityTypeAlias(
    const clang::NamedDecl *decl,
    ImportedName compatibilityName,
    ImportedName correctSwiftName) {
  // Import the referenced declaration. If it doesn't come in as a type,
  // we don't care.
  Decl *importedDecl = nullptr;
  if (getVersion() >= getActiveSwiftVersion())
    importedDecl = Impl.importDecl(decl, ImportNameVersion::forTypes());
  if (!importedDecl && getVersion() != getActiveSwiftVersion())
    importedDecl = Impl.importDecl(decl, getActiveSwiftVersion());
  auto typeDecl = dyn_cast_or_null<TypeDecl>(importedDecl);
  if (!typeDecl)
    return nullptr;

  auto dc = Impl.importDeclContextOf(decl,
                                     compatibilityName.getEffectiveContext());
  if (!dc)
    return nullptr;

  // Create the type alias.
  auto alias = Impl.createDeclWithClangNode<TypeAliasDecl>(
      decl, AccessLevel::Public, Impl.importSourceLoc(decl->getBeginLoc()),
      SourceLoc(), compatibilityName.getDeclName().getBaseIdentifier(),
      Impl.importSourceLoc(decl->getLocation()), /*generic params*/nullptr, dc);

  auto *GTD = dyn_cast<GenericTypeDecl>(typeDecl);
  if (GTD && !isa<ProtocolDecl>(GTD)) {
    alias->setGenericSignature(GTD->getGenericSignature());
    if (GTD->isGeneric()) {
      alias->getASTContext().evaluator.cacheOutput(
            GenericParamListRequest{alias},
            std::move(GTD->getGenericParams()->clone(alias)));
    }
  }

  alias->setUnderlyingType(typeDecl->getDeclaredInterfaceType());

  // Record that this is the official version of this declaration.
  Impl.ImportedDecls[{decl->getCanonicalDecl(), getVersion()}] = alias;
  markAsVariant(alias, correctSwiftName);
  return alias;
}

namespace {
  template<typename D>
  bool inheritanceListContainsProtocol(D decl, const ProtocolDecl *proto) {
    bool anyObject = false;
    for (const auto &found :
            getDirectlyInheritedNominalTypeDecls(decl, anyObject)) {
      if (auto protoDecl = dyn_cast<ProtocolDecl>(found.Item))
        if (protoDecl == proto || protoDecl->inheritsFrom(proto))
          return true;
    }

    return false;
  }
}

static bool conformsToProtocolInOriginalModule(NominalTypeDecl *nominal,
                                               const ProtocolDecl *proto) {
  if (inheritanceListContainsProtocol(nominal, proto))
    return true;

  for (auto attr : nominal->getAttrs().getAttributes<SynthesizedProtocolAttr>()) {
    auto *otherProto = attr->getProtocol();
    if (otherProto == proto || otherProto->inheritsFrom(proto))
      return true;
  }

  // Only consider extensions from the original module...or from an overlay
  // or the Swift half of a mixed-source framework.
  const DeclContext *containingFile = nominal->getModuleScopeContext();
  ModuleDecl *originalModule = containingFile->getParentModule();

  ModuleDecl *overlayModule = nullptr;
  if (auto *clangUnit = dyn_cast<ClangModuleUnit>(containingFile))
    overlayModule = clangUnit->getOverlayModule();

  for (ExtensionDecl *extension : nominal->getExtensions()) {
    ModuleDecl *extensionModule = extension->getParentModule();
    if (extensionModule != originalModule && extensionModule != overlayModule &&
        !extensionModule->isFoundationModule()) {
      continue;
    }
    if (inheritanceListContainsProtocol(extension, proto))
      return true;
  }

  return false;
}

Decl *
SwiftDeclConverter::importSwiftNewtype(const clang::TypedefNameDecl *decl,
                                       clang::SwiftNewTypeAttr *newtypeAttr,
                                       DeclContext *dc, Identifier name) {
  // The only (current) difference between swift_newtype(struct) and
  // swift_newtype(enum), until we can get real enum support, is that enums
  // have no un-labeled inits(). This is because enums are to be considered
  // closed, and if constructed from a rawValue, should be very explicit.
  bool unlabeledCtor = false;

  switch (newtypeAttr->getNewtypeKind()) {
  case clang::SwiftNewTypeAttr::NK_Enum:
    unlabeledCtor = false;
    // TODO: import as enum instead
    break;

  case clang::SwiftNewTypeAttr::NK_Struct:
    unlabeledCtor = true;
    break;
    // No other cases yet
  }

  auto &ctx = Impl.SwiftContext;
  auto Loc = Impl.importSourceLoc(decl->getLocation());

  auto structDecl = Impl.createDeclWithClangNode<StructDecl>(
      decl, AccessLevel::Public, Loc, name, Loc, llvm::None, nullptr, dc);

  // Import the type of the underlying storage
  ImportDiagnosticAdder addImportDiag(Impl, decl, decl->getLocation());
  auto storedUnderlyingType = Impl.importTypeIgnoreIUO(
      decl->getUnderlyingType(), ImportTypeKind::Value, addImportDiag,
      isInSystemModule(dc), Bridgeability::None, ImportTypeAttrs(), OTK_None);

  if (!storedUnderlyingType)
    return nullptr;

  if (auto objTy = storedUnderlyingType->getOptionalObjectType())
    storedUnderlyingType = objTy;

  // If the type is Unmanaged, that is it is not CF ARC audited,
  // we will store the underlying type and leave it up to the use site
  // to determine whether to use this new_type, or an Unmanaged<CF...> type.
  if (auto genericType = storedUnderlyingType->getAs<BoundGenericType>()) {
    if (genericType->isUnmanaged()) {
      assert(genericType->getGenericArgs().size() == 1 && "other args?");
      storedUnderlyingType = genericType->getGenericArgs()[0];
    }
  }

  // Find a bridged type, which may be different
  auto computedPropertyUnderlyingType = Impl.importTypeIgnoreIUO(
      decl->getUnderlyingType(), ImportTypeKind::Property, addImportDiag,
      isInSystemModule(dc), Bridgeability::Full, ImportTypeAttrs(), OTK_None);
  if (auto objTy = computedPropertyUnderlyingType->getOptionalObjectType())
    computedPropertyUnderlyingType = objTy;

  bool isBridged =
      !storedUnderlyingType->isEqual(computedPropertyUnderlyingType);

  // Determine the set of protocols to which the synthesized
  // type will conform.
  SmallVector<KnownProtocolKind, 4> synthesizedProtocols;

  // Local function to add a known protocol.
  auto addKnown = [&](KnownProtocolKind kind) {
    synthesizedProtocols.push_back(kind);
  };

  // Add conformances that are always available.
  addKnown(KnownProtocolKind::RawRepresentable);
  addKnown(KnownProtocolKind::SwiftNewtypeWrapper);

  // Local function to add a known protocol only when the
  // underlying type conforms to it.
  auto computedNominal = computedPropertyUnderlyingType->getAnyNominal();
  if (auto existential =
          computedPropertyUnderlyingType->getAs<ExistentialType>())
    computedNominal = existential->getConstraintType()->getAnyNominal();
  auto transferKnown = [&](KnownProtocolKind kind) {
    if (!computedNominal)
      return false;

    auto proto = ctx.getProtocol(kind);
    if (!proto)
      return false;

    // Break circularity by only looking for declared conformances in the
    // original module, or possibly its overlay.
    if (conformsToProtocolInOriginalModule(computedNominal, proto)) {
      synthesizedProtocols.push_back(kind);
      return true;
    }

    return false;
  };

  // Transfer conformances. Each of these needs a forwarding
  // implementation in the standard library.
  transferKnown(KnownProtocolKind::Equatable);
  transferKnown(KnownProtocolKind::Hashable);
  bool hasObjCBridgeable =
      transferKnown(KnownProtocolKind::ObjectiveCBridgeable);
  bool wantsObjCBridgeableTypealias = hasObjCBridgeable && isBridged;

  // Wrappers around ObjC classes and protocols are also bridgeable.
  if (!hasObjCBridgeable) {
    if (isBridged) {
      if (auto *proto = dyn_cast_or_null<ProtocolDecl>(computedNominal))
        if (proto->getKnownProtocolKind() == KnownProtocolKind::Error)
          hasObjCBridgeable = true;
    } else {
      if (auto *objcClass = dyn_cast_or_null<ClassDecl>(computedNominal)) {
        switch (objcClass->getForeignClassKind()) {
        case ClassDecl::ForeignKind::Normal:
        case ClassDecl::ForeignKind::RuntimeOnly:
          if (objcClass->hasClangNode())
            hasObjCBridgeable = true;
          break;
        case ClassDecl::ForeignKind::CFType:
          break;
        }
      } else if (storedUnderlyingType->isObjCExistentialType()) {
        hasObjCBridgeable = true;
      }
    }

    if (hasObjCBridgeable) {
      addKnown(KnownProtocolKind::ObjectiveCBridgeable);
      wantsObjCBridgeableTypealias = true;
    }
  }

  if (!isBridged) {
    // Simple, our stored type is equivalent to our computed
    // type.
    auto options = getDefaultMakeStructRawValuedOptions();
    if (unlabeledCtor)
      options |= MakeStructRawValuedFlags::MakeUnlabeledValueInit;

    synthesizer.makeStructRawValued(structDecl, storedUnderlyingType,
                                    synthesizedProtocols, options);
  } else {
    // We need to make a stored rawValue or storage type, and a
    // computed one of bridged type.
    synthesizer.makeStructRawValuedWithBridge(
        structDecl, storedUnderlyingType, computedPropertyUnderlyingType,
        synthesizedProtocols,
        /*makeUnlabeledValueInit=*/unlabeledCtor);
  }

  if (wantsObjCBridgeableTypealias) {
    Impl.addSynthesizedTypealias(structDecl, ctx.Id_ObjectiveCType,
                                 storedUnderlyingType);
  }

  Impl.ImportedDecls[{decl->getCanonicalDecl(), getVersion()}] = structDecl;
  return structDecl;
}

Decl *SwiftDeclConverter::importEnumCase(const clang::EnumConstantDecl *decl,
                                         const clang::EnumDecl *clangEnum,
                                         EnumDecl *theEnum,
                                         Decl *correctDecl) {
  auto &context = Impl.SwiftContext;
  ImportedName importedName;
  llvm::Optional<ImportedName> correctSwiftName;
  std::tie(importedName, correctSwiftName) = importFullName(decl);
  auto name = importedName.getDeclName().getBaseIdentifier();
  if (name.empty())
    return nullptr;

  if (correctSwiftName) {
    // We're creating a compatibility stub. Treat it as an enum case alias.
    auto correctCase = dyn_cast_or_null<EnumElementDecl>(correctDecl);
    if (!correctCase)
      return nullptr;

    // If the correct declaration was unavailable, don't map to it.
    // FIXME: This eliminates spurious errors, but affects QoI.
    if (correctCase->getAttrs().isUnavailable(Impl.SwiftContext))
      return nullptr;

    auto compatibilityCase =
        importEnumCaseAlias(name, decl, correctCase, clangEnum, theEnum);
    if (compatibilityCase)
      markAsVariant(compatibilityCase, *correctSwiftName);

    return compatibilityCase;
  }

  // Use the constant's underlying value as its raw value in Swift.
  bool negative = false;
  llvm::APSInt rawValue = decl->getInitVal();

  if (clangEnum->getIntegerType()->isSignedIntegerOrEnumerationType() &&
      rawValue.slt(0)) {
    rawValue = -rawValue;
    negative = true;
  }
  llvm::SmallString<12> rawValueText;
  rawValue.toString(rawValueText, 10, /*signed*/ false);
  StringRef rawValueTextC = context.AllocateCopy(StringRef(rawValueText));
  auto rawValueExpr =
      new (context) IntegerLiteralExpr(rawValueTextC, SourceLoc(),
                                       /*implicit*/ false);
  if (negative)
    rawValueExpr->setNegative(SourceLoc());

  auto element = Impl.createDeclWithClangNode<EnumElementDecl>(
      decl, AccessLevel::Public, SourceLoc(), name, nullptr,
      SourceLoc(), rawValueExpr, theEnum);

  Impl.importAttributes(decl, element);

  return element;
}

Decl *
SwiftDeclConverter::importOptionConstant(const clang::EnumConstantDecl *decl,
                                         const clang::EnumDecl *clangEnum,
                                         NominalTypeDecl *theStruct) {
  ImportedName nameInfo;
  llvm::Optional<ImportedName> correctSwiftName;
  std::tie(nameInfo, correctSwiftName) = importFullName(decl);
  Identifier name = nameInfo.getDeclName().getBaseIdentifier();
  if (name.empty())
    return nullptr;

  // Create the constant.
  auto convertKind = ConstantConvertKind::Construction;
  if (isa<EnumDecl>(theStruct))
    convertKind = ConstantConvertKind::ConstructionWithUnwrap;
  Decl *CD = synthesizer.createConstant(
      name, theStruct, theStruct->getDeclaredInterfaceType(),
      clang::APValue(decl->getInitVal()), convertKind, /*isStatic*/ true, decl);
  Impl.importAttributes(decl, CD);

  // NS_OPTIONS members that have a value of 0 (typically named "None") do
  // not operate as a set-like member.  Mark them unavailable with a message
  // that says that they should be used as [].
  if (decl->getInitVal() == 0 && !nameInfo.hasCustomName() &&
      !CD->getAttrs().isUnavailable(Impl.SwiftContext)) {
    /// Create an AvailableAttr that indicates specific availability
    /// for all platforms.
    auto attr = AvailableAttr::createPlatformAgnostic(
        Impl.SwiftContext, "use [] to construct an empty option set");
    CD->getAttrs().add(attr);
  }

  // If this is a compatibility stub, mark it as such.
  if (correctSwiftName)
    markAsVariant(CD, *correctSwiftName);

  return CD;
}

Decl *SwiftDeclConverter::importEnumCaseAlias(
    Identifier name, const clang::EnumConstantDecl *alias, ValueDecl *original,
    const clang::EnumDecl *clangEnum, NominalTypeDecl *importedEnum,
    DeclContext *importIntoDC) {
  if (name.empty())
    return nullptr;

  // Default the DeclContext to the enum type.
  if (!importIntoDC)
    importIntoDC = importedEnum;

  Type importedEnumTy = importedEnum->getDeclaredInterfaceType();
  auto typeRef = TypeExpr::createImplicit(importedEnumTy, Impl.SwiftContext);

  Expr *result = nullptr;
  if (auto *enumElt = dyn_cast<EnumElementDecl>(original)) {
    assert(!enumElt->hasAssociatedValues());

    // Construct the original constant. Enum constants without payloads look
    // like simple values, but actually have type 'MyEnum.Type -> MyEnum'.
    auto constantRef =
        new (Impl.SwiftContext) DeclRefExpr(enumElt, DeclNameLoc(),
                                            /*implicit*/ true);
    constantRef->setType(enumElt->getInterfaceType());

    auto instantiate =
        DotSyntaxCallExpr::create(Impl.SwiftContext, constantRef, SourceLoc(),
                                  Argument::unlabeled(typeRef));
    instantiate->setType(importedEnumTy);
    instantiate->setThrows(false);

    result = instantiate;
  } else {
    assert(isa<VarDecl>(original));

    result =
        new (Impl.SwiftContext) MemberRefExpr(typeRef, SourceLoc(),
                                              original, DeclNameLoc(),
                                              /*implicit*/ true);
    result->setType(original->getInterfaceType());
  }

  Decl *CD = synthesizer.createConstant(name, importIntoDC, importedEnumTy,
                                 result, ConstantConvertKind::None,
                                 /*isStatic*/ true, alias);
  Impl.importAttributes(alias, CD);
  return CD;
}

NominalTypeDecl *
SwiftDeclConverter::importAsOptionSetType(DeclContext *dc, Identifier name,
                                          const clang::EnumDecl *decl) {
  ASTContext &ctx = Impl.SwiftContext;

  auto Loc = Impl.importSourceLoc(decl->getLocation());

  // Create a struct with the underlying type as a field.
  auto structDecl = Impl.createDeclWithClangNode<StructDecl>(
      decl, AccessLevel::Public, Loc, name, Loc, llvm::None, nullptr, dc);
  Impl.ImportedDecls[{decl->getCanonicalDecl(), getVersion()}] = structDecl;

  // Compute the underlying type.
  auto underlyingType = Impl.importTypeIgnoreIUO(
      decl->getIntegerType(), ImportTypeKind::Enum,
      ImportDiagnosticAdder(Impl, decl, decl->getLocation()),
      isInSystemModule(dc), Bridgeability::None, ImportTypeAttrs());
  if (!underlyingType)
    return nullptr;

  synthesizer.makeStructRawValued(structDecl, underlyingType,
                                  {KnownProtocolKind::OptionSet});
  auto selfType = structDecl->getDeclaredInterfaceType();
  Impl.addSynthesizedTypealias(structDecl, ctx.Id_Element, selfType);
  Impl.addSynthesizedTypealias(structDecl, ctx.Id_ArrayLiteralElement,
                               selfType);
  return structDecl;
}

Decl *SwiftDeclConverter::importGlobalAsInitializer(
    const clang::FunctionDecl *decl, DeclName name, DeclContext *dc,
    CtorInitializerKind initKind,
    llvm::Optional<ImportedName> correctSwiftName) {
  // TODO: Should this be an error? How can this come up?
  assert(dc->isTypeContext() && "cannot import as member onto non-type");

  // Check for some invalid imports
  if (dc->getSelfProtocolDecl()) {
    // FIXME: clang source location
    Impl.diagnose({}, diag::swift_name_protocol_static, /*isInit=*/true);
    Impl.diagnose({}, diag::note_while_importing, decl->getName());
    return nullptr;
  }

  bool allowNSUIntegerAsInt =
      Impl.shouldAllowNSUIntegerAsInt(isInSystemModule(dc), decl);

  ArrayRef<Identifier> argNames = name.getArgumentNames();

  ParameterList *parameterList = nullptr;
  if (argNames.size() == 1 && decl->getNumParams() == 0) {
    // Special case: We need to create an empty first parameter for our
    // argument label
    auto *paramDecl =
        new (Impl.SwiftContext) ParamDecl(
            SourceLoc(), SourceLoc(), argNames.front(),
            SourceLoc(), argNames.front(), dc);
    paramDecl->setSpecifier(ParamSpecifier::Default);
    paramDecl->setInterfaceType(Impl.SwiftContext.TheEmptyTupleType);

    parameterList = ParameterList::createWithoutLoc(paramDecl);
  } else {
    parameterList = Impl.importFunctionParameterList(
        dc, decl, {decl->param_begin(), decl->param_end()}, decl->isVariadic(),
        allowNSUIntegerAsInt, argNames, /*genericParams=*/{}, /*resultType=*/nullptr);
  }
  if (!parameterList)
    return nullptr;

  auto importedType =
      Impl.importFunctionReturnType(dc, decl, allowNSUIntegerAsInt);

  // Update the failability appropriately based on the imported method type.
  bool failable = false, isIUO = false;
  if (!importedType.getType().isNull() &&
      importedType.isImplicitlyUnwrapped()) {
    assert(importedType.getType()->getOptionalObjectType());
    failable = true;
    isIUO = true;
  } else if (importedType.getType()->getOptionalObjectType()) {
    failable = true;
  }

  auto result = Impl.createDeclWithClangNode<ConstructorDecl>(
      decl, AccessLevel::Public, name, /*NameLoc=*/SourceLoc(),
      failable, /*FailabilityLoc=*/SourceLoc(),
      /*Async=*/false, /*AsyncLoc=*/SourceLoc(),
      /*Throws=*/false, /*ThrowsLoc=*/SourceLoc(), parameterList,
      /*GenericParams=*/nullptr, dc);
  result->setImplicitlyUnwrappedOptional(isIUO);
  result->getASTContext().evaluator.cacheOutput(InitKindRequest{result},
                                                std::move(initKind));
  result->setImportAsStaticMember();

  Impl.recordImplicitUnwrapForDecl(result,
                                   importedType.isImplicitlyUnwrapped());
  result->setOverriddenDecls({ });
  result->setIsObjC(false);
  result->setIsDynamic(false);

  finishFuncDecl(decl, result);
  if (correctSwiftName)
    markAsVariant(result, *correctSwiftName);
  return result;
}

/// Create an implicit property given the imported name of one of
/// the accessors.
VarDecl *
SwiftDeclConverter::getImplicitProperty(ImportedName importedName,
                                         const clang::FunctionDecl *accessor) {
  // Check whether we already know about the property.
  auto knownProperty = Impl.FunctionsAsProperties.find(accessor);
  if (knownProperty != Impl.FunctionsAsProperties.end())
    return knownProperty->second;

  // Determine whether we have the getter or setter.
  const clang::FunctionDecl *getter = nullptr;
  ImportedName getterName;
  llvm::Optional<ImportedName> swift3GetterName;
  const clang::FunctionDecl *setter = nullptr;
  ImportedName setterName;
  llvm::Optional<ImportedName> swift3SetterName;
  switch (importedName.getAccessorKind()) {
  case ImportedAccessorKind::None:
  case ImportedAccessorKind::SubscriptGetter:
  case ImportedAccessorKind::SubscriptSetter:
    llvm_unreachable("Not a property accessor");

  case ImportedAccessorKind::PropertyGetter:
    getter = accessor;
    getterName = importedName;
    break;

  case ImportedAccessorKind::PropertySetter:
    setter = accessor;
    setterName = importedName;
    break;
  }

  // Find the other accessor, if it exists.
  auto propertyName = importedName.getDeclName().getBaseIdentifier();
  auto lookupTable =
      Impl.findLookupTable(*getClangSubmoduleForDecl(accessor));
  assert(lookupTable && "No lookup table?");
  bool foundAccessor = false;
  for (auto entry : lookupTable->lookup(SerializedSwiftName(propertyName),
                                        importedName.getEffectiveContext())) {
    auto decl = entry.dyn_cast<clang::NamedDecl *>();
    if (!decl)
      continue;

    auto function = dyn_cast<clang::FunctionDecl>(decl);
    if (!function)
      continue;

    if (function->getCanonicalDecl() == accessor->getCanonicalDecl()) {
      foundAccessor = true;
      continue;
    }

    if (!getter) {
      // Find the self index for the getter.
      std::tie(getterName, swift3GetterName) = importFullName(function);
      if (!getterName)
        continue;

      getter = function;
      continue;
    }

    if (!setter) {
      // Find the self index for the setter.
      std::tie(setterName, swift3SetterName) = importFullName(function);
      if (!setterName)
        continue;

      setter = function;
      continue;
    }

    // We already have both a getter and a setter; something is
    // amiss, so bail out.
    return nullptr;
  }

  assert(foundAccessor && "Didn't find the original accessor? "
                          "Try clearing your module cache");

  // If there is no getter, there's nothing we can do.
  if (!getter)
    return nullptr;

  // Retrieve the type of the property that is implied by the getter.
  auto propertyType =
      getAccessorPropertyType(getter, false, getterName.getSelfIndex());
  if (propertyType.isNull())
    return nullptr;
  if (auto elaborated = dyn_cast<clang::ElaboratedType>(propertyType))
    propertyType = elaborated->desugar();

  // If there is a setter, check that the property it implies
  // matches that of the getter.
  if (setter) {
    auto setterPropertyType =
        getAccessorPropertyType(setter, true, setterName.getSelfIndex());
    if (setterPropertyType.isNull())
      return nullptr;

    // If the inferred property types don't match up, we can't
    // form a property.
    if (!getter->getASTContext().hasSameType(propertyType, setterPropertyType))
      return nullptr;
  }

  // Import the property's context.
  auto dc = Impl.importDeclContextOf(getter, getterName.getEffectiveContext());
  if (!dc)
    return nullptr;

  // Is this a static property?
  bool isStatic = false;
  if (dc->isTypeContext() && !getterName.getSelfIndex())
    isStatic = true;

  ImportedType importedType;

  // Sometimes we import unavailable typedefs as enums. If that's the case,
  // use the enum, not the typedef here.
  if (auto typedefType = dyn_cast<clang::TypedefType>(propertyType.getTypePtr())) {
    if (Impl.isUnavailableInSwift(typedefType->getDecl())) {
      if (auto clangEnum = findAnonymousEnumForTypedef(Impl.SwiftContext, typedefType)) {
        // If this fails, it means that we need a stronger predicate for
        // determining the relationship between an enum and typedef.
        assert(clangEnum.value()->getIntegerType()->getCanonicalTypeInternal() ==
               typedefType->getCanonicalTypeInternal());
        if (auto swiftEnum = Impl.importDecl(*clangEnum, Impl.CurrentVersion)) {
          importedType = {cast<TypeDecl>(swiftEnum)->getDeclaredInterfaceType(),
                          false};
        }
      }
    }
  }

  if (!importedType) {
    // Compute the property type.
    bool isFromSystemModule = isInSystemModule(dc);
    importedType = Impl.importType(
        propertyType, ImportTypeKind::Property,
        ImportDiagnosticAdder(Impl, getter, getter->getLocation()),
        Impl.shouldAllowNSUIntegerAsInt(isFromSystemModule, getter),
        Bridgeability::Full, getImportTypeAttrs(accessor),
        OTK_ImplicitlyUnwrappedOptional);
  }

  if (!importedType)
    return nullptr;

  Type swiftPropertyType = importedType.getType();

  auto property = Impl.createDeclWithClangNode<VarDecl>(
      getter, AccessLevel::Public, /*IsStatic*/isStatic,
      VarDecl::Introducer::Var, SourceLoc(),
      propertyName, dc);
  property->setInterfaceType(swiftPropertyType);
  property->setIsObjC(false);
  property->setIsDynamic(false);

  Impl.recordImplicitUnwrapForDecl(property,
                                   importedType.isImplicitlyUnwrapped());

  // Note that we've formed this property.
  Impl.FunctionsAsProperties[getter] = property;
  if (setter)
    Impl.FunctionsAsProperties[setter] = property;

  // If this property is in a class or class extension context,
  // add "final".
  if (dc->getSelfClassDecl())
    property->getAttrs().add(new (Impl.SwiftContext)
                                 FinalAttr(/*IsImplicit=*/true));

  // Import the getter.
  auto *swiftGetter = dyn_cast_or_null<AccessorDecl>(
      importFunctionDecl(getter, getterName, llvm::None,
                         AccessorInfo{property, AccessorKind::Get}));
  if (!swiftGetter)
    return nullptr;

  Impl.importAttributes(getter, swiftGetter);
  Impl.ImportedDecls[{getter, getVersion()}] = swiftGetter;
  if (swift3GetterName)
    markAsVariant(swiftGetter, *swift3GetterName);

  // Import the setter.
  AccessorDecl *swiftSetter = nullptr;
  if (setter) {
    swiftSetter = dyn_cast_or_null<AccessorDecl>(
        importFunctionDecl(setter, setterName, llvm::None,
                           AccessorInfo{property, AccessorKind::Set}));
    if (!swiftSetter)
      return nullptr;

    Impl.importAttributes(setter, swiftSetter);
    Impl.ImportedDecls[{setter, getVersion()}] = swiftSetter;
    if (swift3SetterName)
      markAsVariant(swiftSetter, *swift3SetterName);
  }

  if (swiftGetter) property->setIsGetterMutating(swiftGetter->isMutating());
  if (swiftSetter) property->setIsSetterMutating(swiftSetter->isMutating());

  // Make this a computed property.
  Impl.makeComputed(property, swiftGetter, swiftSetter);

  // Make the property the alternate declaration for the getter.
  Impl.addAlternateDecl(swiftGetter, property);

  return property;
}

ConstructorDecl *SwiftDeclConverter::importConstructor(
    const clang::ObjCMethodDecl *objcMethod, const DeclContext *dc,
    bool implicit, llvm::Optional<CtorInitializerKind> kind, bool required) {
  // Only methods in the 'init' family can become constructors.
  assert(isInitMethod(objcMethod) && "Not a real init method");

  // Check whether we've already created the constructor.
  auto known =
      Impl.Constructors.find(std::make_tuple(objcMethod, dc, getVersion()));
  if (known != Impl.Constructors.end())
    return known->second;

  ImportedName importedName;
  llvm::Optional<ImportedName> correctSwiftName;
  std::tie(importedName, correctSwiftName) = importFullName(objcMethod);
  if (!importedName)
    return nullptr;

  // Check whether there is already a method with this selector.
  auto selector = Impl.importSelector(objcMethod->getSelector());
  if (isActiveSwiftVersion() &&
      isMethodAlreadyImported(selector, importedName, /*isInstance=*/true, dc,
                              [](AbstractFunctionDecl *fn) {
        return true;
      }))
    return nullptr;

  // Map the name and complete the import.
  ArrayRef<const clang::ParmVarDecl *> params{objcMethod->param_begin(),
                                              objcMethod->param_end()};

  bool variadic = objcMethod->isVariadic();

  // If we dropped the variadic, handle it now.
  if (importedName.droppedVariadic()) {
    selector = ObjCSelector(Impl.SwiftContext, selector.getNumArgs() - 1,
                            selector.getSelectorPieces().drop_back());
    params = params.drop_back(1);
    variadic = false;
  }

  ConstructorDecl *existing;
  auto result = importConstructor(objcMethod, dc, implicit,
                                  kind.value_or(importedName.getInitKind()),
                                  required, selector, importedName, params,
                                  variadic, existing);

  // If this is a compatibility stub, mark it as such.
  if (result && correctSwiftName)
    markAsVariant(result, *correctSwiftName);

  return result;
}

/// Returns the latest "introduced" version on the current platform for
/// \p D.
llvm::VersionTuple
SwiftDeclConverter::findLatestIntroduction(const clang::Decl *D) {
  llvm::VersionTuple result;

  for (auto *attr : D->specific_attrs<clang::AvailabilityAttr>()) {
    if (attr->getPlatform()->getName() == "swift") {
      llvm::VersionTuple maxVersion{~0U, ~0U, ~0U};
      return maxVersion;
    }

    // Does this availability attribute map to the platform we are
    // currently targeting?
    if (!Impl.platformAvailability.isPlatformRelevant(
            attr->getPlatform()->getName())) {
      continue;
    }
    // Take advantage of the empty version being 0.0.0.0.
    result = std::max(result, attr->getIntroduced());
  }

  return result;
}

/// Returns true if importing \p objcMethod will produce a "better"
/// initializer than \p existingCtor.
bool SwiftDeclConverter::existingConstructorIsWorse(
    const ConstructorDecl *existingCtor,
    const clang::ObjCMethodDecl *objcMethod, CtorInitializerKind kind) {
  CtorInitializerKind existingKind = existingCtor->getInitKind();

  // If one constructor is unavailable in Swift and the other is
  // not, keep the available one.
  bool existingIsUnavailable =
      existingCtor->getAttrs().isUnavailable(Impl.SwiftContext);
  bool newIsUnavailable = Impl.isUnavailableInSwift(objcMethod);
  if (existingIsUnavailable != newIsUnavailable)
    return existingIsUnavailable;

  // If the new kind is the same as the existing kind, stick with
  // the existing constructor.
  if (existingKind == kind)
    return false;

  // Check for cases that are obviously better or obviously worse.
  if (kind == CtorInitializerKind::Designated ||
      existingKind == CtorInitializerKind::Factory)
    return true;

  if (kind == CtorInitializerKind::Factory ||
      existingKind == CtorInitializerKind::Designated)
    return false;

  assert(kind == CtorInitializerKind::Convenience ||
         kind == CtorInitializerKind::ConvenienceFactory);
  assert(existingKind == CtorInitializerKind::Convenience ||
         existingKind == CtorInitializerKind::ConvenienceFactory);

  // Between different kinds of convenience initializers, keep the one that
  // was introduced first.
  // FIXME: But if one of them is now deprecated, should we prefer the
  // other?
  llvm::VersionTuple introduced = findLatestIntroduction(objcMethod);
  AvailabilityContext existingAvailability =
      AvailabilityInference::availableRange(existingCtor, Impl.SwiftContext);
  assert(!existingAvailability.isKnownUnreachable());

  if (existingAvailability.isAlwaysAvailable()) {
    if (!introduced.empty())
      return false;
  } else {
    VersionRange existingIntroduced = existingAvailability.getOSVersion();
    if (introduced != existingIntroduced.getLowerEndpoint()) {
      return introduced < existingIntroduced.getLowerEndpoint();
    }
  }

  // The "introduced" versions are the same. Prefer Convenience over
  // ConvenienceFactory, but otherwise prefer leaving things as they are.
  if (kind == CtorInitializerKind::Convenience &&
      existingKind == CtorInitializerKind::ConvenienceFactory)
    return true;

  return false;
}

/// Given an imported method, try to import it as a constructor.
///
/// Objective-C methods in the 'init' family are imported as
/// constructors in Swift, enabling object construction syntax, e.g.,
///
/// \code
/// // in objc: [[NSArray alloc] initWithCapacity:1024]
/// NSArray(capacity: 1024)
/// \endcode
///
/// This variant of the function is responsible for actually binding the
/// constructor declaration appropriately.
ConstructorDecl *SwiftDeclConverter::importConstructor(
    const clang::ObjCMethodDecl *objcMethod, const DeclContext *dc, bool implicit,
    CtorInitializerKind kind, bool required, ObjCSelector selector,
    ImportedName importedName, ArrayRef<const clang::ParmVarDecl *> args,
    bool variadic, ConstructorDecl *&existing) {
  existing = nullptr;

  // Figure out the type of the container.
  auto ownerNominal = dc->getSelfNominalTypeDecl();
  assert(ownerNominal && "Method in non-type context?");

  // Import the type that this method will have.
  llvm::Optional<ForeignAsyncConvention> asyncConvention;
  llvm::Optional<ForeignErrorConvention> errorConvention;
  ParameterList *bodyParams;
  auto importedType = Impl.importMethodParamsAndReturnType(
      dc, objcMethod, args, variadic, isInSystemModule(dc), &bodyParams,
      importedName, asyncConvention, errorConvention,
      SpecialMethodKind::Constructor);
  assert(!asyncConvention && "Initializers don't have async conventions");
  if (!importedType)
    return nullptr;

  // Determine the failability of this initializer.
  bool resultIsOptional = (bool) importedType.getType()->getOptionalObjectType();

  // Update the failability appropriately based on the imported method type.
  assert(resultIsOptional || !importedType.isImplicitlyUnwrapped());
  OptionalTypeKind failability = OTK_None;
  if (resultIsOptional) {
    failability = OTK_Optional;
    if (importedType.isImplicitlyUnwrapped())
      failability = OTK_ImplicitlyUnwrappedOptional;
  }

  // Rebuild the function type with the appropriate result type;
  Type resultTy = dc->getSelfInterfaceType();
  if (resultIsOptional)
    resultTy = OptionalType::get(resultTy);

  // Look for other imported constructors that occur in this context with
  // the same name.
  SmallVector<AnyFunctionType::Param, 4> allocParams;
  bodyParams->getParams(allocParams);

  TinyPtrVector<ConstructorDecl *> ctors;
  auto found = Impl.ConstructorsForNominal.find(ownerNominal);
  if (found != Impl.ConstructorsForNominal.end())
    ctors = found->second;

  for (auto ctor : ctors) {
    if (ctor->isInvalid() ||
        ctor->getAttrs().isUnavailable(Impl.SwiftContext) ||
        !ctor->getClangDecl())
      continue;

    // If the types don't match, this is a different constructor with
    // the same selector. This can happen when an overlay overloads an
    // existing selector with a Swift-only signature.
    auto ctorParams = ctor->getInterfaceType()
                          ->castTo<AnyFunctionType>()
                          ->getResult()
                          ->castTo<AnyFunctionType>()
                          ->getParams();
    if (!AnyFunctionType::equalParams(ctorParams, allocParams)) {
      continue;
    }

    // If the existing constructor has a less-desirable kind, mark
    // the existing constructor unavailable.
    if (existingConstructorIsWorse(ctor, objcMethod, kind)) {
      // Show exactly where this constructor came from.
      llvm::SmallString<32> errorStr;
      errorStr += "superseded by import of ";
      if (objcMethod->isClassMethod())
        errorStr += "+[";
      else
        errorStr += "-[";

      auto objcDC = objcMethod->getDeclContext();
      if (auto objcClass = dyn_cast<clang::ObjCInterfaceDecl>(objcDC)) {
        errorStr += objcClass->getName();
        errorStr += ' ';
      } else if (auto objcCat = dyn_cast<clang::ObjCCategoryDecl>(objcDC)) {
        errorStr += objcCat->getClassInterface()->getName();
        auto catName = objcCat->getName();
        if (!catName.empty()) {
          errorStr += '(';
          errorStr += catName;
          errorStr += ')';
        }
        errorStr += ' ';
      } else if (auto objcProto = dyn_cast<clang::ObjCProtocolDecl>(objcDC)) {
        errorStr += objcProto->getName();
        errorStr += ' ';
      }

      errorStr += objcMethod->getSelector().getAsString();
      errorStr += ']';

      auto attr = AvailableAttr::createPlatformAgnostic(
          Impl.SwiftContext, Impl.SwiftContext.AllocateCopy(errorStr.str()));
      ctor->getAttrs().add(attr);
      continue;
    }

    // Otherwise, we shouldn't create a new constructor, because
    // it will be no better than the existing one.
    existing = ctor;
    return nullptr;
  }

  // Check whether we've already created the constructor.
  auto known =
      Impl.Constructors.find(std::make_tuple(objcMethod, dc, getVersion()));
  if (known != Impl.Constructors.end())
    return known->second;

  // Create the actual constructor.
  assert(!importedName.getAsyncInfo());
  auto result = Impl.createDeclWithClangNode<ConstructorDecl>(
      objcMethod, AccessLevel::Public, importedName.getDeclName(),
      /*NameLoc=*/SourceLoc(), failability, /*FailabilityLoc=*/SourceLoc(),
      /*Async=*/false, /*AsyncLoc=*/SourceLoc(),
      /*Throws=*/importedName.getErrorInfo().has_value(),
      /*ThrowsLoc=*/SourceLoc(), bodyParams,
      /*GenericParams=*/nullptr, const_cast<DeclContext *>(dc));

  addObjCAttribute(result, selector);
  recordMemberInContext(dc, result);

  Impl.recordImplicitUnwrapForDecl(result,
                                   importedType.isImplicitlyUnwrapped());

  if (implicit)
    result->setImplicit();

  // Set the kind of initializer.
  result->getASTContext().evaluator.cacheOutput(InitKindRequest{result},
                                                std::move(kind));

  // Consult API notes to determine whether this initializer is required.
  if (!required && isRequiredInitializer(objcMethod))
    required = true;

  // Check whether this initializer satisfies a requirement in a protocol.
  if (!required && !isa<ProtocolDecl>(dc) && objcMethod->isInstanceMethod()) {
    auto objcParent =
        cast<clang::ObjCContainerDecl>(objcMethod->getDeclContext());

    if (isa<clang::ObjCProtocolDecl>(objcParent)) {
      // An initializer declared in a protocol is required.
      required = true;
    } else {
      // If the class in which this initializer was declared conforms to a
      // protocol that requires this initializer, then this initializer is
      // required.
      SmallPtrSet<clang::ObjCProtocolDecl *, 8> objcProtocols;
      objcParent->getASTContext().CollectInheritedProtocols(objcParent,
                                                            objcProtocols);
      for (auto objcProto : objcProtocols) {
        for (auto decl : objcProto->lookup(objcMethod->getSelector())) {
          if (cast<clang::ObjCMethodDecl>(decl)->isInstanceMethod()) {
            required = true;
            break;
          }
        }

        if (required)
          break;
      }
    }
  }

  // If this initializer is required, add the appropriate attribute.
  if (required) {
    result->getAttrs().add(new (Impl.SwiftContext)
                               RequiredAttr(/*IsImplicit=*/true));
  }

  // Record the error convention.
  if (errorConvention) {
    result->setForeignErrorConvention(*errorConvention);
  }

  // Record the constructor for future re-use.
  Impl.Constructors[std::make_tuple(objcMethod, dc, getVersion())] = result;
  Impl.ConstructorsForNominal[ownerNominal].push_back(result);

  // If this constructor overrides another constructor, mark it as such.
  recordObjCOverride(result);

  return result;
}

void SwiftDeclConverter::recordObjCOverride(AbstractFunctionDecl *decl) {
  // Make sure that we always set the overridden declarations.
  SWIFT_DEFER {
    if (!decl->overriddenDeclsComputed())
      decl->setOverriddenDecls({ });
  };

  // Figure out the class in which this method occurs.
  auto classDecl = decl->getDeclContext()->getSelfClassDecl();
  if (!classDecl)
    return;
  auto superDecl = classDecl->getSuperclassDecl();
  if (!superDecl)
    return;
  // Dig out the Objective-C superclass.
  SmallVector<ValueDecl *, 4> results;
  superDecl->lookupQualified(superDecl, DeclNameRef(decl->getName()),
                             decl->getLoc(), NL_QualifiedDefault,
                             results);
  for (auto member : results) {
    if (member->getKind() != decl->getKind() ||
        member->isInstanceMember() != decl->isInstanceMember() ||
        member->isObjC() != decl->isObjC())
      continue;
    // Set function override.
    if (auto func = dyn_cast<FuncDecl>(decl)) {
      auto foundFunc = cast<FuncDecl>(member);
      // Require a selector match.
      if (foundFunc->isObjC() &&
          func->getObjCSelector() != foundFunc->getObjCSelector())
        continue;
      func->setOverriddenDecl(foundFunc);
      func->getAttrs().add(new (func->getASTContext()) OverrideAttr(true));
      return;
    }
    // Set constructor override.
    auto ctor = cast<ConstructorDecl>(decl);
    auto memberCtor = cast<ConstructorDecl>(member);
    // Require a selector match.
    if (ctor->isObjC() &&
        ctor->getObjCSelector() != memberCtor->getObjCSelector())
      continue;
    ctor->setOverriddenDecl(memberCtor);
    ctor->getAttrs().add(new (ctor->getASTContext()) OverrideAttr(true));

    // Propagate 'required' to subclass initializers.
    if (memberCtor->isRequired() &&
        !ctor->getAttrs().hasAttribute<RequiredAttr>()) {
      ctor->getAttrs().add(new (Impl.SwiftContext)
                               RequiredAttr(/*IsImplicit=*/true));
    }
  }
}

// Note: This function ignores labels.
static bool areParameterTypesEqual(const ParameterList &params1,
                                   const ParameterList &params2) {
  if (params1.size() != params2.size())
    return false;

  for (unsigned i : indices(params1)) {
    if (!params1[i]->getInterfaceType()->isEqual(
          params2[i]->getInterfaceType())) {
      return false;
    }

    if (params1[i]->getValueOwnership() !=
        params2[i]->getValueOwnership()) {
      return false;
    }
  }

  return true;
}

void SwiftDeclConverter::recordObjCOverride(SubscriptDecl *subscript) {
  // Figure out the class in which this subscript occurs.
  auto classTy = subscript->getDeclContext()->getSelfClassDecl();
  if (!classTy)
    return;

  auto superDecl = classTy->getSuperclassDecl();
  if (!superDecl)
    return;

  // Determine whether this subscript operation overrides another subscript
  // operation.
  SmallVector<ValueDecl *, 2> lookup;
  subscript->getModuleContext()->lookupQualified(
      superDecl, DeclNameRef(subscript->getName()),
      subscript->getLoc(), NL_QualifiedDefault, lookup);

  for (auto result : lookup) {
    auto parentSub = dyn_cast<SubscriptDecl>(result);
    if (!parentSub)
      continue;

    if (!areParameterTypesEqual(*subscript->getIndices(),
                                *parentSub->getIndices()))
      continue;

    // The index types match. This is an override, so mark it as such.
    subscript->setOverriddenDecl(parentSub);
    auto getterThunk = subscript->getParsedAccessor(AccessorKind::Get);
    getterThunk->setOverriddenDecl(parentSub->getParsedAccessor(AccessorKind::Get));
    if (auto parentSetter = parentSub->getParsedAccessor(AccessorKind::Set)) {
      if (auto setterThunk = subscript->getParsedAccessor(AccessorKind::Set))
        setterThunk->setOverriddenDecl(parentSetter);
    }

    // FIXME: Eventually, deal with multiple overrides.
    break;
  }
}

/// Given either the getter or setter for a subscript operation,
/// create the Swift subscript declaration.
SubscriptDecl *
SwiftDeclConverter::importSubscript(Decl *decl,
                                    const clang::ObjCMethodDecl *objcMethod) {
  assert(objcMethod->isInstanceMethod() && "Caller must filter");

  // If the method we're attempting to import has the
  // swift_private attribute, don't import as a subscript.
  if (objcMethod->hasAttr<clang::SwiftPrivateAttr>())
    return nullptr;

  // Figure out where to look for the counterpart.
  const clang::ObjCInterfaceDecl *interface = nullptr;
  const clang::ObjCProtocolDecl *protocol =
      dyn_cast<clang::ObjCProtocolDecl>(objcMethod->getDeclContext());
  if (!protocol)
    interface = objcMethod->getClassInterface();
  auto lookupInstanceMethod = [&](
      clang::Selector Sel) -> const clang::ObjCMethodDecl * {
    if (interface)
      return interface->lookupInstanceMethod(Sel);

    return protocol->lookupInstanceMethod(Sel);
  };

  auto findCounterpart = [&](clang::Selector sel) -> FuncDecl * {
    // If the declaration we're starting from is in a class, first check to see
    // if we've already imported an instance method with a matching selector.
    if (auto classDecl = decl->getDeclContext()->getSelfClassDecl()) {
      auto swiftSel = Impl.importSelector(sel);
      auto importedMembers = Impl.MembersForNominal.find(classDecl);
      if (importedMembers != Impl.MembersForNominal.end()) {
        for (auto membersForName : importedMembers->second) {
          for (auto *member : membersForName.second) {
            // Must be an instance method.
            auto *afd = dyn_cast<FuncDecl>(member);
            if (!afd || !afd->isInstanceMember())
              continue;

            // Selector must match.
            if (afd->getObjCSelector() == swiftSel)
              return afd;
          }
        }
      }
    }

    // Find based on selector within the current type.
    auto counterpart = lookupInstanceMethod(sel);
    if (!counterpart)
      return nullptr;

    // If we're looking at a class but the getter was found in a protocol,
    // we're going to build the subscript later when we mirror the protocol
    // member. Bail out here, otherwise we'll build it twice.
    if (interface &&
        isa<clang::ObjCProtocolDecl>(counterpart->getDeclContext()))
      return nullptr;

    return cast_or_null<FuncDecl>(
        Impl.importDecl(counterpart, getActiveSwiftVersion()));
  };

  // Determine the selector of the counterpart.
  FuncDecl *getter = nullptr, *setter = nullptr;
  const clang::ObjCMethodDecl *getterObjCMethod = nullptr,
                              *setterObjCMethod = nullptr;
  clang::Selector counterpartSelector;
  if (objcMethod->getSelector() == Impl.objectAtIndexedSubscript) {
    getter = cast<FuncDecl>(decl);
    getterObjCMethod = objcMethod;
    counterpartSelector = Impl.setObjectAtIndexedSubscript;
  } else if (objcMethod->getSelector() == Impl.setObjectAtIndexedSubscript) {
    setter = cast<FuncDecl>(decl);
    setterObjCMethod = objcMethod;
    counterpartSelector = Impl.objectAtIndexedSubscript;
  } else if (objcMethod->getSelector() == Impl.objectForKeyedSubscript) {
    getter = cast<FuncDecl>(decl);
    getterObjCMethod = objcMethod;
    counterpartSelector = Impl.setObjectForKeyedSubscript;
  } else if (objcMethod->getSelector() == Impl.setObjectForKeyedSubscript) {
    setter = cast<FuncDecl>(decl);
    setterObjCMethod = objcMethod;
    counterpartSelector = Impl.objectForKeyedSubscript;
  } else {
    llvm_unreachable("Unknown getter/setter selector");
  }

  // Find the counterpart.
  bool optionalMethods = (objcMethod->getImplementationControl() ==
                          clang::ObjCMethodDecl::Optional);

  if (auto *counterpart = findCounterpart(counterpartSelector)) {
    const clang::ObjCMethodDecl *counterpartMethod = nullptr;

    // If the counterpart to the method we're attempting to import has the
    // swift_private attribute, don't import as a subscript.
    if (auto importedFrom = counterpart->getClangDecl()) {
      if (importedFrom->hasAttr<clang::SwiftPrivateAttr>())
        return nullptr;

      counterpartMethod = cast<clang::ObjCMethodDecl>(importedFrom);
      if (optionalMethods)
        optionalMethods = (counterpartMethod->getImplementationControl() ==
                           clang::ObjCMethodDecl::Optional);
    }

    assert(!counterpart || !counterpart->isStatic());

    if (getter) {
      setter = counterpart;
      setterObjCMethod = counterpartMethod;
    } else {
      getter = counterpart;
      getterObjCMethod = counterpartMethod;
    }
  }

  // Swift doesn't have write-only subscripting.
  if (!getter)
    return nullptr;

  // Check whether we've already created a subscript operation for
  // this getter/setter pair.
  if (auto subscript = Impl.Subscripts[{getter, setter}]) {
    return subscript->getDeclContext() == decl->getDeclContext() ? subscript
                                                                 : nullptr;
  }

  // Find the getter indices and make sure they match.
  ParamDecl *getterIndex;
  {
    auto params = getter->getParameters();
    if (params->size() != 1)
      return nullptr;
    getterIndex = params->get(0);
  }

  // Compute the element type based on the getter, looking through
  // the implicit 'self' parameter and the normal function
  // parameters.
  auto elementTy = getter->getResultInterfaceType();

  // Local function to mark the setter unavailable.
  auto makeSetterUnavailable = [&] {
    if (setter && !setter->getAttrs().isUnavailable(Impl.SwiftContext))
      Impl.markUnavailable(setter, "use subscripting");
  };

  // If we have a setter, rectify it with the getter.
  ParamDecl *setterIndex;
  bool getterAndSetterInSameType = false;
  bool isIUO = getter->isImplicitlyUnwrappedOptional();
  if (setter) {
    // Whether there is an existing read-only subscript for which
    // we have now found a setter.
    SubscriptDecl *existingSubscript = Impl.Subscripts[{getter, nullptr}];

    // Are the getter and the setter in the same type.
    getterAndSetterInSameType =
        (getter->getDeclContext()->getSelfNominalTypeDecl() ==
         setter->getDeclContext()->getSelfNominalTypeDecl());

    // Whether we can update the types involved in the subscript
    // operation.
    bool canUpdateSubscriptType =
        !existingSubscript && getterAndSetterInSameType;

    // Determine the setter's element type and indices.
    Type setterElementTy;
    std::tie(setterElementTy, setterIndex) = decomposeSubscriptSetter(setter);

    // Rectify the setter element type with the getter's element type,
    // and determine if the result is an implicitly unwrapped optional
    // type.
    auto importedType = rectifySubscriptTypes(elementTy, isIUO, setterElementTy,
                                              canUpdateSubscriptType);
    if (!importedType)
      return decl == getter ? existingSubscript : nullptr;

    isIUO = importedType.isImplicitlyUnwrapped();

    // Update the element type.
    elementTy = importedType.getType();

    // Make sure that the index types are equivalent.
    // FIXME: Rectify these the same way we do for element types.
    if (!setterIndex->getType()->isEqual(getterIndex->getType())) {
      // If there is an existing subscript operation, we're done.
      if (existingSubscript)
        return decl == getter ? existingSubscript : nullptr;

      // Otherwise, just forget we had a setter.
      // FIXME: This feels very, very wrong.
      setter = nullptr;
      setterObjCMethod = nullptr;
      setterIndex = nullptr;
    }

    // If there is an existing subscript within this context, we
    // cannot create a new subscript. Update it if possible.
    if (setter && existingSubscript && getterAndSetterInSameType) {
      // Can we update the subscript by adding the setter?
      if (existingSubscript->hasClangNode() &&
          !existingSubscript->supportsMutation()) {
        // Create the setter thunk.
        auto setterThunk = synthesizer.buildSubscriptSetterDecl(
            existingSubscript, setter, elementTy, setter->getDeclContext(),
            setterIndex);

        // Set the computed setter.
        existingSubscript->setComputedSetter(setterThunk);

        // Mark the setter as unavailable; one should use
        // subscripting when it is present.
        makeSetterUnavailable();
      }

      return decl == getter ? existingSubscript : nullptr;
    }
  }

  // The context into which the subscript should go. We prefer wherever the
  // getter is declared unless the two accessors are in different types and the
  // one we started with is the setter. This happens when:
  // - A read-only subscript is made read/write is a subclass.
  // - A setter is redeclared in a subclass, but not the getter.
  // And not when:
  // - A getter is redeclared in a subclass, but not the setter.
  // - The getter and setter are part of the same type.
  // - There is no setter.
  bool associateWithSetter = !getterAndSetterInSameType && setter == decl;
  DeclContext *dc =
      associateWithSetter ? setter->getDeclContext() : getter->getDeclContext();

  // Build the subscript declaration.
  auto &C = Impl.SwiftContext;
  auto bodyParams = ParameterList::create(C, getterIndex);
  DeclName name(C, DeclBaseName::createSubscript(), {Identifier()});
  auto *const subscript = SubscriptDecl::createImported(C,
                                                        name, decl->getLoc(),
                                                        bodyParams, decl->getLoc(),
                                                        elementTy, dc,
                                                        getter->getClangNode());

  bool IsObjCDirect = false;
  if (auto objCDecl = dyn_cast<clang::ObjCMethodDecl>(getter->getClangDecl())) {
    IsObjCDirect = objCDecl->isDirectMethod();
  }
  const auto access = IsObjCDirect ? AccessLevel::Public
                                   : getOverridableAccessLevel(dc);
  subscript->setAccess(access);
  subscript->setSetterAccess(access);

  // Build the thunks.
  AccessorDecl *getterThunk = synthesizer.buildSubscriptGetterDecl(
      subscript, getter, elementTy, dc, getterIndex);

  AccessorDecl *setterThunk = nullptr;
  if (setter)
    setterThunk = synthesizer.buildSubscriptSetterDecl(
        subscript, setter, elementTy, dc, setterIndex);

  // Record the subscript as an alternative declaration.
  Impl.addAlternateDecl(associateWithSetter ? setter : getter, subscript);

  // Import attributes for the accessors if there is a pair.
  Impl.importAttributes(getterObjCMethod, getterThunk);
  if (setterObjCMethod)
    Impl.importAttributes(setterObjCMethod, setterThunk);

  subscript->setIsSetterMutating(false);
  Impl.makeComputed(subscript, getterThunk, setterThunk);

  Impl.recordImplicitUnwrapForDecl(subscript, isIUO);

  addObjCAttribute(subscript, llvm::None);

  // Optional subscripts in protocols.
  if (optionalMethods && isa<ProtocolDecl>(dc))
    subscript->getAttrs().add(new (Impl.SwiftContext) OptionalAttr(true));

  // Note that we've created this subscript.
  Impl.Subscripts[{getter, setter}] = subscript;
  if (setter && !Impl.Subscripts[{getter, nullptr}])
    Impl.Subscripts[{getter, nullptr}] = subscript;

  // Make the getter/setter methods unavailable.
  if (!getter->getAttrs().isUnavailable(Impl.SwiftContext))
    Impl.markUnavailable(getter, "use subscripting");
  makeSetterUnavailable();

  // Wire up overrides.
  recordObjCOverride(subscript);

  return subscript;
}

AccessorDecl *
SwiftDeclConverter::importAccessor(const clang::ObjCMethodDecl *clangAccessor,
                                   AbstractStorageDecl *storage,
                                   AccessorKind accessorKind,
                                   DeclContext *dc) {
  SwiftDeclConverter converter(Impl, getActiveSwiftVersion());
  auto *accessor = cast_or_null<AccessorDecl>(
    converter.importObjCMethodDecl(clangAccessor, dc,
                                   AccessorInfo{storage, accessorKind}));
  if (!accessor) {
    return nullptr;
  }

  Impl.importAttributes(clangAccessor, accessor);

  return accessor;
}

void SwiftDeclConverter::addProtocols(
    ProtocolDecl *protocol, SmallVectorImpl<ProtocolDecl *> &protocols,
    llvm::SmallPtrSetImpl<ProtocolDecl *> &known) {
  if (!known.insert(protocol).second)
    return;

  protocols.push_back(protocol);
  for (auto inherited : protocol->getInheritedProtocols())
    addProtocols(inherited, protocols, known);
}

void SwiftDeclConverter::importObjCProtocols(
    Decl *decl, const clang::ObjCProtocolList &clangProtocols,
    SmallVectorImpl<InheritedEntry> &inheritedTypes) {
  SmallVector<ProtocolDecl *, 4> protocols;
  llvm::SmallPtrSet<ProtocolDecl *, 4> knownProtocols;
  if (auto classDecl = dyn_cast<ClassDecl>(decl)) {
    classDecl->getImplicitProtocols(protocols);
    knownProtocols.insert(protocols.begin(), protocols.end());
  }

  for (auto cp = clangProtocols.begin(), cpEnd = clangProtocols.end();
       cp != cpEnd; ++cp) {
    if (auto proto = castIgnoringCompatibilityAlias<ProtocolDecl>(
            Impl.importDecl(*cp, getActiveSwiftVersion()))) {
      addProtocols(proto, protocols, knownProtocols);
      inheritedTypes.push_back(
        InheritedEntry(
          TypeLoc::withoutLoc(proto->getDeclaredInterfaceType()),
          /*isUnchecked=*/false));
    }
  }

  Impl.recordImportedProtocols(decl, protocols);
}

llvm::Optional<GenericParamList *> SwiftDeclConverter::importObjCGenericParams(
    const clang::ObjCInterfaceDecl *decl, DeclContext *dc) {
  auto typeParamList = decl->getTypeParamList();
  if (!typeParamList) {
    return nullptr;
  }
  if (shouldSuppressGenericParamsImport(Impl.SwiftContext.LangOpts, decl)) {
    return nullptr;
  }
  assert(typeParamList->size() > 0);
  SmallVector<GenericTypeParamDecl *, 4> genericParams;
  for (auto *objcGenericParam : *typeParamList) {
    auto genericParamDecl = Impl.createDeclWithClangNode<GenericTypeParamDecl>(
        objcGenericParam, AccessLevel::Public, dc,
        Impl.SwiftContext.getIdentifier(objcGenericParam->getName()),
        Impl.importSourceLoc(objcGenericParam->getLocation()),
        /*ellipsisLoc*/ SourceLoc(),
        /*depth*/ 0, /*index*/ genericParams.size(), /*isParameterPack*/ false);
    // NOTE: depth is always 0 for ObjC generic type arguments, since only
    // classes may have generic types in ObjC, and ObjC classes cannot be
    // nested.

    // Import parameter constraints.
    SmallVector<InheritedEntry, 1> inherited;
    if (objcGenericParam->hasExplicitBound()) {
      assert(!objcGenericParam->getUnderlyingType().isNull());
      auto clangBound = objcGenericParam->getUnderlyingType()
                            ->castAs<clang::ObjCObjectPointerType>();
      if (clangBound->getInterfaceDecl()) {
        auto unqualifiedClangBound =
            clangBound->stripObjCKindOfTypeAndQuals(Impl.getClangASTContext());
        assert(!objcGenericParam->hasAttrs()
               && "ObjC generics can have attributes now--we should use 'em");
        Type superclassType = Impl.importTypeIgnoreIUO(
            clang::QualType(unqualifiedClangBound, 0), ImportTypeKind::Abstract,
            ImportDiagnosticAdder(Impl, decl, decl->getLocation()),
            false, Bridgeability::None, ImportTypeAttrs());
        if (!superclassType) {
          return llvm::None;
        }
        inherited.push_back(TypeLoc::withoutLoc(superclassType));
      }
      for (clang::ObjCProtocolDecl *clangProto : clangBound->quals()) {
        ProtocolDecl *proto = castIgnoringCompatibilityAlias<ProtocolDecl>(
            Impl.importDecl(clangProto, getActiveSwiftVersion()));
        if (!proto) {
          return llvm::None;
        }
        inherited.push_back(
          TypeLoc::withoutLoc(proto->getDeclaredInterfaceType()));
      }
    }
    if (inherited.empty()) {
      inherited.push_back(
        TypeLoc::withoutLoc(Impl.SwiftContext.getAnyObjectConstraint()));
    }
    genericParamDecl->setInherited(Impl.SwiftContext.AllocateCopy(inherited));

    genericParams.push_back(genericParamDecl);
  }
  return GenericParamList::create(
      Impl.SwiftContext, Impl.importSourceLoc(typeParamList->getLAngleLoc()),
      genericParams, Impl.importSourceLoc(typeParamList->getRAngleLoc()));
}

void ClangImporter::Implementation::importMirroredProtocolMembers(
    const clang::ObjCContainerDecl *decl, DeclContext *dc,
    llvm::Optional<DeclBaseName> name, SmallVectorImpl<Decl *> &members) {
  SwiftDeclConverter converter(*this, CurrentVersion);
  converter.importMirroredProtocolMembers(decl, dc, name, members);
}

void SwiftDeclConverter::importMirroredProtocolMembers(
    const clang::ObjCContainerDecl *decl, DeclContext *dc,
    llvm::Optional<DeclBaseName> name, SmallVectorImpl<Decl *> &members) {
  assert(dc);
  const clang::ObjCInterfaceDecl *interfaceDecl = nullptr;
  const ClangModuleUnit *declModule;
  const ClangModuleUnit *interfaceModule;

  // Try to import only the most specific methods with a particular name.
  // We use a MapVector to get deterministic iteration order later.
  llvm::MapVector<clang::Selector, std::vector<MirroredMethodEntry>>
    methodsByName;

  for (auto proto : Impl.getImportedProtocols(dc->getAsDecl())) {
    auto clangProto =
        cast_or_null<clang::ObjCProtocolDecl>(proto->getClangDecl());
    if (!clangProto)
      continue;

    if (!interfaceDecl) {
      declModule = Impl.getClangModuleForDecl(decl);
      if ((interfaceDecl = dyn_cast<clang::ObjCInterfaceDecl>(decl))) {
        interfaceModule = declModule;
      } else {
        auto category = cast<clang::ObjCCategoryDecl>(decl);
        interfaceDecl = category->getClassInterface();
        interfaceModule = Impl.getClangModuleForDecl(interfaceDecl);
      }
    }

    // Don't import a protocol's members if the superclass already adopts
    // the protocol, or (for categories) if the class itself adopts it
    // in its main @interface.
    if (decl != interfaceDecl)
      if (classImplementsProtocol(interfaceDecl, clangProto, false))
        continue;
    if (auto superInterface = interfaceDecl->getSuperClass())
      if (classImplementsProtocol(superInterface, clangProto, true))
        continue;

    const auto &languageVersion =
        Impl.SwiftContext.LangOpts.EffectiveLanguageVersion;
    auto importProtocolRequirement = [&](Decl *member) {
      // Skip compatibility stubs; there's no reason to mirror them.
      if (member->getAttrs().isUnavailableInSwiftVersion(languageVersion))
        return;

      if (auto prop = dyn_cast<VarDecl>(member)) {
        auto objcProp =
            dyn_cast_or_null<clang::ObjCPropertyDecl>(prop->getClangDecl());
        if (!objcProp)
          return;

        // We can't import a property if there's already a method with this
        // name. (This also covers other properties with that same name.)
        // FIXME: We should still mirror the setter as a method if it's
        // not already there.
        clang::Selector sel = objcProp->getGetterName();
        if (interfaceDecl->getInstanceMethod(sel))
          return;

        bool inNearbyCategory =
            std::any_of(interfaceDecl->known_categories_begin(),
                        interfaceDecl->known_categories_end(),
                        [=](const clang::ObjCCategoryDecl *category) -> bool {
                          if (!Impl.getClangSema().isVisible(category)) {
                            return false;
                          }
                          if (category != decl) {
                            auto *categoryModule =
                                Impl.getClangModuleForDecl(category);
                            if (categoryModule != declModule &&
                                categoryModule != interfaceModule) {
                              return false;
                            }
                          }
                          return category->getInstanceMethod(sel);
                        });
        if (inNearbyCategory)
          return;

        if (auto imported =
                Impl.importMirroredDecl(objcProp, dc, getVersion(), proto)) {
          members.push_back(imported);
          // FIXME: We should mirror properties of the root class onto the
          // metatype.
        }

        return;
      }

      auto afd = dyn_cast<AbstractFunctionDecl>(member);
      if (!afd)
        return;

      if (isa<AccessorDecl>(afd))
        return;

      auto objcMethod =
          dyn_cast_or_null<clang::ObjCMethodDecl>(member->getClangDecl());
      if (!objcMethod)
        return;

      // For now, just remember that we saw this method.
      methodsByName[objcMethod->getSelector()]
        .push_back(std::make_tuple(objcMethod, proto, afd->hasAsync()));
    };

    if (name) {
      // If we're asked to import a specific name only, look for that in the
      // protocol.
      auto results = proto->lookupDirect(*name);
      for (auto *member : results)
        if (member->getDeclContext() == proto)
          importProtocolRequirement(member);

    } else {
      // Otherwise, import all mirrored members.
      for (auto *member : proto->getMembers())
        importProtocolRequirement(member);
    }
  }

  // Process all the methods, now that we've arranged them by selector.
  for (auto &mapEntry : methodsByName) {
    importNonOverriddenMirroredMethods(dc, mapEntry.second, members);
  }
}

enum MirrorImportComparison {
  // There's no suppression relationship between the methods.
  NoSuppression,

  // The first method suppresses the second.
  Suppresses,

  // The second method suppresses the first.
  IsSuppressed,
};

/// Should the mirror import of the first method be suppressed in favor
/// of the second method?  The methods are known to have the same selector
/// and (because this is mirror-import) to be declared on protocols.
///
/// The algorithm that uses this assumes that it is transitive.
static bool isMirrorImportSuppressedBy(ClangImporter::Implementation &importer,
                                       const clang::ObjCMethodDecl *first,
                                       const clang::ObjCMethodDecl *second) {
  if (first->isInstanceMethod() != second->isInstanceMethod())
    return false;

  auto firstProto = cast<clang::ObjCProtocolDecl>(first->getDeclContext());
  auto secondProto = cast<clang::ObjCProtocolDecl>(second->getDeclContext());

  // If the first method's protocol is a super-protocol of the second's,
  // then the second method overrides the first and we should suppress.
  // Clang provides a function to check that, phrased in terms of whether
  // a value of one protocol (the RHS) can be assigned to an l-value of
  // the other (the LHS).
  auto &ctx = importer.getClangASTContext();
  return ctx.ProtocolCompatibleWithProtocol(
                          const_cast<clang::ObjCProtocolDecl*>(firstProto),
                          const_cast<clang::ObjCProtocolDecl*>(secondProto));
}

/// Compare two methods for mirror-import purposes.
static MirrorImportComparison
compareMethodsForMirrorImport(ClangImporter::Implementation &importer,
                              const clang::ObjCMethodDecl *first,
                              const clang::ObjCMethodDecl *second) {
  if (isMirrorImportSuppressedBy(importer, first, second))
    return IsSuppressed;
  if (isMirrorImportSuppressedBy(importer, second, first))
    return Suppresses;
  return NoSuppression;
}

/// Mark any methods in the given array that are overridden by this method
/// as suppressed by nulling their entries out.
/// Return true if this method is overridden by any methods in the array.
static bool suppressOverriddenMethods(ClangImporter::Implementation &importer,
                                      const clang::ObjCMethodDecl *method,
                                      bool isAsync,
                               MutableArrayRef<MirroredMethodEntry> entries) {
  assert(method && "method was already suppressed");

  for (auto &entry: entries) {
    auto otherMethod = std::get<0>(entry);
    if (!otherMethod) continue;
    if (isAsync != std::get<2>(entry)) continue;

    assert(method != otherMethod && "found same method twice?");
    switch (compareMethodsForMirrorImport(importer, method, otherMethod)) {
    // If the second method is suppressed, null it out.
    case Suppresses:
        std::get<0>(entry) = nullptr;
      continue;

    // If the first method is suppressed, return immediately.  We should
    // be able to suppress any following methods.
    case IsSuppressed:
      return true;

    case NoSuppression:
      continue;
    }
    llvm_unreachable("bad comparison result");
  }

  return false;
}

void addCompletionHandlerAttribute(Decl *asyncImport,
                                   ArrayRef<Decl *> members,
                                   ASTContext &SwiftContext) {
  auto *asyncFunc = dyn_cast_or_null<AbstractFunctionDecl>(asyncImport);
  // Completion handler functions can be imported as getters, but the decl
  // given back from the import is the property. Grab the underlying getter
  if (auto *property = dyn_cast_or_null<AbstractStorageDecl>(asyncImport))
    asyncFunc = property->getAccessor(AccessorKind::Get);

  if (!asyncFunc)
    return;

  for (auto *member : members) {
    // Only add the attribute to functions that don't already have availability
    if (member != asyncImport && isa<AbstractFunctionDecl>(member) &&
        !member->getAttrs().hasAttribute<AvailableAttr>()) {
      member->getAttrs().add(
          AvailableAttr::createForAlternative(SwiftContext, asyncFunc));
    }
  }
}

/// Given a set of methods with the same selector, each taken from a
/// different protocol in the protocol hierarchy of a class into which
/// we want to introduce mirror imports, import only the methods which
/// are not overridden by another method in the set.
///
/// It's possible that we'll end up selecting multiple methods to import
/// here, in the cases where there's no hierarchical relationship between
/// two methods.  The importer already has code to handle this case.
void SwiftDeclConverter::importNonOverriddenMirroredMethods(DeclContext *dc,
                               MutableArrayRef<MirroredMethodEntry> entries,
                                           SmallVectorImpl<Decl *> &members) {
  // Keep track of the async imports. We'll come back to them.
  llvm::SmallMapVector<const clang::ObjCMethodDecl*, Decl *, 4> asyncImports;

  // Keep track of all of the synchronous imports.
  llvm::SmallMapVector<
      const clang::ObjCMethodDecl*, llvm::TinyPtrVector<Decl *>, 4>
    syncImports;

  for (size_t i = 0, e = entries.size(); i != e; ++i) {
    auto objcMethod = std::get<0>(entries[i]);
    bool isAsync = std::get<2>(entries[i]);

    // If the method was suppressed by a previous method, ignore it.
    if (!objcMethod)
      continue;

    // Compare this method to all the following methods, suppressing any
    // that it overrides.  If it is overridden by any of them, suppress it
    // instead; but there's no need to mark that in the array, just continue
    // on to the next method.
    if (suppressOverriddenMethods(
            Impl, objcMethod, isAsync, entries.slice(i + 1)))
      continue;

    // Okay, the method wasn't suppressed, import it.

    // When mirroring an initializer, make it designated and required.
    if (isInitMethod(objcMethod)) {
      // Import the constructor.
      if (auto imported = importConstructor(objcMethod, dc, /*implicit=*/true,
                                            CtorInitializerKind::Designated,
                                            /*required=*/true)) {
        members.push_back(imported);
      }
      continue;
    }

    // Import the method.
    auto proto = std::get<1>(entries[i]);
    if (auto imported =
            Impl.importMirroredDecl(objcMethod, dc,
                                    getVersion().withConcurrency(isAsync),
                                    proto)) {
      size_t start = members.size();

      members.push_back(imported);

      for (auto alternate : Impl.getAlternateDecls(imported)) {
        if (imported->getDeclContext() == alternate->getDeclContext())
          members.push_back(alternate);
      }

      if (isAsync) {
        asyncImports[objcMethod] = imported;
      } else {
        syncImports[objcMethod] = llvm::TinyPtrVector<Decl *>(
            llvm::makeArrayRef(members).drop_front(start + 1));
      }
    }
  }

  // Write up sync and async versions.
  for (const auto &asyncImport : asyncImports) {
    addCompletionHandlerAttribute(
        asyncImport.second,
        syncImports[asyncImport.first],
        Impl.SwiftContext);
  }
}

void SwiftDeclConverter::importInheritedConstructors(
    const ClassDecl *classDecl, SmallVectorImpl<Decl *> &newMembers) {
  auto superclassDecl = classDecl->getSuperclassDecl();
  if (!superclassDecl)
    return;

  auto superclassClangDecl = superclassDecl->getClangDecl();
  if (!superclassClangDecl ||
      !isa<clang::ObjCInterfaceDecl>(superclassClangDecl))
    return;

  auto curObjCClass = cast<clang::ObjCInterfaceDecl>(classDecl->getClangDecl());

  // The kind of initializer to import. If this class has designated
  // initializers, everything it inherits is a convenience initializer.
  llvm::Optional<CtorInitializerKind> kind;
  if (curObjCClass->hasDesignatedInitializers())
    kind = CtorInitializerKind::Convenience;

  const auto &languageVersion =
      Impl.SwiftContext.LangOpts.EffectiveLanguageVersion;

  auto members = superclassDecl->lookupDirect(
      DeclBaseName::createConstructor());

  for (auto member : members) {
    auto ctor = dyn_cast<ConstructorDecl>(member);
    if (!ctor)
      continue;

    // Don't inherit compatibility stubs.
    if (ctor->getAttrs().isUnavailableInSwiftVersion(languageVersion))
      continue;

    // Don't inherit (non-convenience) factory initializers.
    // Note that convenience factories return instancetype and can be
    // inherited.
    switch (ctor->getInitKind()) {
    case CtorInitializerKind::Factory:
      continue;
    case CtorInitializerKind::ConvenienceFactory:
    case CtorInitializerKind::Convenience:
    case CtorInitializerKind::Designated:
      break;
    }

    auto objcMethod =
        dyn_cast_or_null<clang::ObjCMethodDecl>(ctor->getClangDecl());
    if (!objcMethod)
      continue;

    auto &clangSourceMgr = Impl.getClangASTContext().getSourceManager();
    clang::PrettyStackTraceDecl trace(objcMethod, clang::SourceLocation(),
                                      clangSourceMgr,
                                      "importing (inherited)");

    // If this initializer came from a factory method, inherit
    // it as an initializer.
    if (objcMethod->isClassMethod()) {
      assert(ctor->getInitKind() == CtorInitializerKind::ConvenienceFactory);

      ImportedName importedName;
      llvm::Optional<ImportedName> correctSwiftName;
      std::tie(importedName, correctSwiftName) = importFullName(objcMethod);
      assert(
          !correctSwiftName &&
          "Import inherited initializers never references correctSwiftName");
      importedName.setHasCustomName();
      ConstructorDecl *existing;
      if (auto newCtor =
              importConstructor(objcMethod, classDecl,
                                /*implicit=*/true, ctor->getInitKind(),
                                /*required=*/false, ctor->getObjCSelector(),
                                importedName, objcMethod->parameters(),
                                objcMethod->isVariadic(), existing)) {
        // If this is a compatibility stub, mark it as such.
        if (correctSwiftName)
          markAsVariant(newCtor, *correctSwiftName);

        Impl.importAttributes(objcMethod, newCtor, curObjCClass);
        newMembers.push_back(newCtor);
      } else if (existing && existing->getInitKind() ==
                   CtorInitializerKind::ConvenienceFactory &&
                 existing->getClangDecl()) {
        // Check that the existing constructor the prevented new creation is
        // really an inherited factory initializer and not a class member.
        auto existingMD = cast<clang::ObjCMethodDecl>(existing->getClangDecl());
        if (existingMD->getClassInterface() != curObjCClass) {
          newMembers.push_back(existing);
        }
      }
      continue;
    }

    // Figure out what kind of constructor this will be.
    CtorInitializerKind myKind;
    bool isRequired = false;
    if (ctor->isRequired()) {
      // Required initializers are always considered designated.
      isRequired = true;
      myKind = CtorInitializerKind::Designated;
    } else if (kind) {
      myKind = *kind;
    } else {
      myKind = ctor->getInitKind();
    }

    // Import the constructor into this context.
    if (auto newCtor =
            importConstructor(objcMethod, classDecl,
                              /*implicit=*/true, myKind, isRequired)) {
      Impl.importAttributes(objcMethod, newCtor, curObjCClass);
      newMembers.push_back(newCtor);
    }
  }
}

llvm::Optional<Decl *> ClangImporter::Implementation::importDeclCached(
    const clang::NamedDecl *ClangDecl, ImportNameVersion version,
    bool UseCanonical) {
  auto Known = ImportedDecls.find(
    { UseCanonical? ClangDecl->getCanonicalDecl(): ClangDecl, version });
  if (Known == ImportedDecls.end())
    return llvm::None;

  return Known->second;
}

/// Checks if we don't need to import the typedef itself.  If the typedef
/// should be skipped, returns the underlying declaration that the typedef
/// refers to -- this declaration should be imported instead.
static const clang::TagDecl *
canSkipOverTypedef(ClangImporter::Implementation &Impl,
                   const clang::NamedDecl *D,
                   bool &TypedefIsSuperfluous) {
  // If we have a typedef that refers to a tag type of the same name,
  // skip the typedef and import the tag type directly.

  TypedefIsSuperfluous = false;

  auto *ClangTypedef = dyn_cast<clang::TypedefNameDecl>(D);
  if (!ClangTypedef)
    return nullptr;

  const clang::DeclContext *RedeclContext =
      ClangTypedef->getDeclContext()->getRedeclContext();
  if (!RedeclContext->isTranslationUnit())
    return nullptr;

  clang::QualType UnderlyingType = ClangTypedef->getUnderlyingType();
  if (auto elaborated = dyn_cast<clang::ElaboratedType>(UnderlyingType))
    UnderlyingType = elaborated->desugar();

  // A typedef to a typedef should get imported as a typealias.
  auto *TypedefT = UnderlyingType->getAs<clang::TypedefType>();
  if (TypedefT)
    return nullptr;

  auto *TT = UnderlyingType->getAs<clang::TagType>();
  if (!TT)
    return nullptr;

  clang::TagDecl *UnderlyingDecl = TT->getDecl();
  if (UnderlyingDecl->getDeclContext()->getRedeclContext() != RedeclContext)
    return nullptr;

  if (UnderlyingDecl->getDeclName().isEmpty())
    return UnderlyingDecl;

  auto TypedefName = ClangTypedef->getDeclName();
  auto TagDeclName = UnderlyingDecl->getDeclName();
  if (TypedefName != TagDeclName)
    return nullptr;

  TypedefIsSuperfluous = true;
  return UnderlyingDecl;
}

StringRef ClangImporter::Implementation::
getSwiftNameFromClangName(StringRef replacement) {
  auto &clangSema = getClangSema();

  clang::IdentifierInfo *identifier =
      &clangSema.getASTContext().Idents.get(replacement);
  clang::LookupResult lookupResult(clangSema, identifier,
                                   clang::SourceLocation(),
                                   clang::Sema::LookupOrdinaryName);
  if (!clangSema.LookupName(lookupResult, clangSema.TUScope))
    return "";

  auto clangDecl = lookupResult.getAsSingle<clang::NamedDecl>();
  if (!clangDecl)
    return "";

  auto importedName = importFullName(clangDecl, CurrentVersion);
  if (!importedName)
    return "";

  llvm::SmallString<64> renamed;
  {
    // Render a swift_name string.
    llvm::raw_svector_ostream os(renamed);
    printSwiftName(importedName, CurrentVersion, /*fullyQualified=*/true, os);
  }

  return SwiftContext.AllocateCopy(StringRef(renamed));
}

bool importer::isSpecialUIKitStructZeroProperty(const clang::NamedDecl *decl) {
  // FIXME: Once UIKit removes the "nonswift" availability in their versioned
  // API notes, this workaround can go away.
  auto *constant = dyn_cast<clang::VarDecl>(decl);
  if (!constant)
    return false;

  clang::DeclarationName name = constant->getDeclName();
  const clang::IdentifierInfo *ident = name.getAsIdentifierInfo();
  if (!ident)
    return false;

  return ident->isStr("UIEdgeInsetsZero") || ident->isStr("UIOffsetZero");
}

bool importer::hasSameUnderlyingType(const clang::Type *a,
                                     const clang::TemplateTypeParmDecl *b) {
  while (a->isPointerType() || a->isReferenceType())
    a = a->getPointeeType().getTypePtr();
  return a == b->getTypeForDecl();
}

unsigned ClangImporter::Implementation::getClangSwiftAttrSourceBuffer(
    StringRef attributeText) {
  auto known = ClangSwiftAttrSourceBuffers.find(attributeText);
  if (known != ClangSwiftAttrSourceBuffers.end())
    return known->second;

  // Create a new buffer with a copy of the attribute text, so we don't need to
  // rely on Clang keeping it around.
  auto &sourceMgr = SwiftContext.SourceMgr;
  auto bufferID = sourceMgr.addMemBufferCopy(attributeText);
  ClangSwiftAttrSourceBuffers.insert({attributeText, bufferID});
  return bufferID;
}

SourceFile &ClangImporter::Implementation::getClangSwiftAttrSourceFile(
    ModuleDecl &module) {
  auto known = ClangSwiftAttrSourceFiles.find(&module);
  if (known != ClangSwiftAttrSourceFiles.end())
    return *known->second;

  auto sourceFile = new (SwiftContext)
      SourceFile(module, SourceFileKind::Library, llvm::None);
  ClangSwiftAttrSourceFiles.insert({&module, sourceFile});
  return *sourceFile;
}

bool swift::importer::isMainActorAttr(const clang::SwiftAttrAttr *swiftAttr) {
  return swiftAttr->getAttribute() == "@MainActor" ||
         swiftAttr->getAttribute() == "@UIActor";
}

bool swift::importer::isMutabilityAttr(const clang::SwiftAttrAttr *swiftAttr) {
  return swiftAttr->getAttribute() == "mutating" ||
         swiftAttr->getAttribute() == "nonmutating";
}

void
ClangImporter::Implementation::importSwiftAttrAttributes(Decl *MappedDecl) {
  auto ClangDecl =
      dyn_cast_or_null<clang::NamedDecl>(MappedDecl->getClangDecl());
  if (!ClangDecl)
    return;

  // Subscripts are special-cased since there isn't a 1:1 mapping
  // from its accessor(s) to the subscript declaration.
  if (isa<SubscriptDecl>(MappedDecl))
    return;

  if (auto maybeDefinition = getDefinitionForClangTypeDecl(ClangDecl))
    if (maybeDefinition.value())
      ClangDecl = cast<clang::NamedDecl>(maybeDefinition.value());

  llvm::Optional<const clang::SwiftAttrAttr *> seenMainActorAttr;
  const clang::SwiftAttrAttr *seenMutabilityAttr = nullptr;
  PatternBindingInitializer *initContext = nullptr;

  auto importAttrsFromDecl = [&](const clang::NamedDecl *ClangDecl) {
    //
    // __attribute__((swift_attr("attribute")))
    //
    for (auto swiftAttr : ClangDecl->specific_attrs<clang::SwiftAttrAttr>()) {
      // FIXME: Hard-code @MainActor and @UIActor, because we don't have a
      // point at which to do name lookup for imported entities.
      if (isMainActorAttr(swiftAttr)) {
        if (seenMainActorAttr) {
          // Cannot add main actor annotation twice. We'll keep the first
          // one and raise a warning about the duplicate.
          HeaderLoc attrLoc(swiftAttr->getLocation());
          diagnose(attrLoc, diag::import_multiple_mainactor_attr,
                   swiftAttr->getAttribute(),
                   seenMainActorAttr.value()->getAttribute());
          continue;
        }

        if (Type mainActorType = SwiftContext.getMainActorType()) {
          auto typeExpr = TypeExpr::createImplicit(mainActorType, SwiftContext);
          auto attr = CustomAttr::create(SwiftContext, SourceLoc(), typeExpr);
          MappedDecl->getAttrs().add(attr);
          seenMainActorAttr = swiftAttr;
        }

        continue;
      }

      if (isMutabilityAttr(swiftAttr)) {
        StringRef attr = swiftAttr->getAttribute();

        // Check if 'nonmutating' attr is applicable
        if (attr == "nonmutating") {
          if (auto *method = dyn_cast<clang::CXXMethodDecl>(ClangDecl)) {
            if (!method->isConst()) {
              diagnose(HeaderLoc(swiftAttr->getLocation()),
                       diag::nonmutating_without_const);
            }

            if (!method->getParent()->hasMutableFields()) {
              diagnose(HeaderLoc(swiftAttr->getLocation()),
                       diag::nonmutating_without_mutable_fields);
            }
          }
        }

        // Check for contradicting mutability attr
        if (seenMutabilityAttr) {
          StringRef previous = seenMutabilityAttr->getAttribute();

          if (previous != attr) {
            diagnose(HeaderLoc(swiftAttr->getLocation()),
                     diag::contradicting_mutation_attrs, attr, previous);
            continue;
          }
        }

        seenMutabilityAttr = swiftAttr;
      }

      // Hard-code @actorIndependent, until Objective-C clients start
      // using nonisolated.
      if (swiftAttr->getAttribute() == "@actorIndependent") {
        auto attr = new (SwiftContext) NonisolatedAttr(/*isImplicit=*/true);
        MappedDecl->getAttrs().add(attr);
        continue;
      }

      // Dig out a buffer with the attribute text.
      unsigned bufferID = getClangSwiftAttrSourceBuffer(
          swiftAttr->getAttribute());

      // Dig out a source file we can use for parsing.
      auto &sourceFile = getClangSwiftAttrSourceFile(
          *MappedDecl->getDeclContext()->getParentModule());

      // Spin up a parser.
      swift::Parser parser(
          bufferID, sourceFile, &SwiftContext.Diags, nullptr, nullptr);
      // Prime the lexer.
      parser.consumeTokenWithoutFeedingReceiver();

      bool hadError = false;
      SourceLoc atLoc;
      if (parser.consumeIf(tok::at_sign, atLoc)) {
        hadError = parser.parseDeclAttribute(
            MappedDecl->getAttrs(), atLoc, initContext,
            /*isFromClangAttribute=*/true).isError();
      } else {
        SourceLoc staticLoc;
        StaticSpellingKind staticSpelling;
        hadError = parser
                       .parseDeclModifierList(MappedDecl->getAttrs(), staticLoc,
                                              staticSpelling,
                                              /*isFromClangAttribute=*/true)
                       .isError();
      }

      if (hadError) {
        // Complain about the unhandled attribute or modifier.
        HeaderLoc attrLoc(swiftAttr->getLocation());
        diagnose(attrLoc, diag::clang_swift_attr_unhandled,
                 swiftAttr->getAttribute());
      }
    }
  };
  importAttrsFromDecl(ClangDecl);

  // If the Clang declaration is from an anonymous tag that was given a
  // name via a typedef, look for attributes on the typedef as well.
  if (auto tag = dyn_cast<clang::TagDecl>(ClangDecl)) {
    if (tag->getName().empty()) {
      if (auto typedefDecl = tag->getTypedefNameForAnonDecl())
        importAttrsFromDecl(typedefDecl);
    }
  }

  // The rest of this concerns '@Sendable' and '@_nonSendable`. These don't
  // affect typealiases, even when there's an underlying nominal type in clang.
  if (isa<TypeAliasDecl>(MappedDecl))
    return;

  // `@Sendable` on non-types is treated as an `ImportTypeAttr` and shouldn't
  // be treated as an attribute on the declaration. (Particularly, @Sendable on
  // a function or method should be treated as making the return value Sendable,
  // *not* as making the function/method itself Sendable, because
  // `@Sendable func` is primarily meant for local functions.)
  if (!isa<TypeDecl>(MappedDecl))
    while (auto attr = MappedDecl->getAttrs().getEffectiveSendableAttr())
      MappedDecl->getAttrs().removeAttribute(attr);

  // Some types have an implicit '@Sendable' attribute.
  if (ClangDecl->hasAttr<clang::SwiftNewTypeAttr>() ||
      ClangDecl->hasAttr<clang::EnumExtensibilityAttr>() ||
      ClangDecl->hasAttr<clang::FlagEnumAttr>() ||
      ClangDecl->hasAttr<clang::NSErrorDomainAttr>())
    MappedDecl->getAttrs().add(
                          new (SwiftContext) SendableAttr(/*isImplicit=*/true));

  // 'Error' conforms to 'Sendable', so error wrappers have to be 'Sendable'
  // and it doesn't make sense for the 'Code' enum to be non-'Sendable'.
  if (ClangDecl->hasAttr<clang::NSErrorDomainAttr>()) {
    // If any @_nonSendable attributes are running the show, invalidate and
    // diagnose them.
    while (NonSendableAttr *attr = dyn_cast_or_null<NonSendableAttr>(
                           MappedDecl->getAttrs().getEffectiveSendableAttr())) {
      assert(attr->Specificity == NonSendableKind::Specific &&
             "didn't we just add an '@Sendable' that should beat this "
             "'@_nonSendable(_assumed)'?");
      attr->setInvalid();
      diagnose(HeaderLoc(ClangDecl->getLocation()),
               diag::clang_error_code_must_be_sendable,
               ClangDecl->getNameAsString());
    }
  }

  // Now that we've collected all @Sendable and @_nonSendable attributes, we
  // can see if we should synthesize a Sendable conformance.
  if (auto nominal = dyn_cast<NominalTypeDecl>(MappedDecl)) {
    auto sendability = nominal->getAttrs().getEffectiveSendableAttr();
    if (isa_and_nonnull<SendableAttr>(sendability)) {
      addSynthesizedProtocolAttrs(nominal, {KnownProtocolKind::Sendable},
                                  /*isUnchecked=*/true);
    }
  }
}

static bool isUsingMacroName(clang::SourceManager &SM,
                             clang::SourceLocation loc,
                             StringRef MacroName) {
  if (!loc.isMacroID())
    return false;
  auto Sloc = SM.getExpansionLoc(loc);
  if (Sloc.isInvalid())
    return false;
  auto Eloc = Sloc.getLocWithOffset(MacroName.size());
  if (Eloc.isInvalid())
    return false;
  StringRef content(SM.getCharacterData(Sloc), MacroName.size());
  return content == MacroName;
}

/// Import Clang attributes as Swift attributes.
void ClangImporter::Implementation::importAttributes(
    const clang::NamedDecl *ClangDecl,
    Decl *MappedDecl,
    const clang::ObjCContainerDecl *NewContext)
{
  // Subscripts are special-cased since there isn't a 1:1 mapping
  // from its accessor(s) to the subscript declaration.
  if (isa<SubscriptDecl>(MappedDecl))
    return;

  ASTContext &C = SwiftContext;

  if (auto maybeDefinition = getDefinitionForClangTypeDecl(ClangDecl))
    if (maybeDefinition.value())
      ClangDecl = cast<clang::NamedDecl>(maybeDefinition.value());

  // Determine whether this is an async import.
  bool isAsync = false;
  if (auto func = dyn_cast<AbstractFunctionDecl>(MappedDecl))
    isAsync = func->hasAsync();

  // Scan through Clang attributes and map them onto Swift
  // equivalents.
  bool AnyUnavailable = MappedDecl->getAttrs().isUnavailable(C);
  for (clang::NamedDecl::attr_iterator AI = ClangDecl->attr_begin(),
       AE = ClangDecl->attr_end(); AI != AE; ++AI) {
    //
    // __attribute__((unavailable))
    //
    // Mapping: @available(*,unavailable)
    //
    if (auto unavailable = dyn_cast<clang::UnavailableAttr>(*AI)) {
      auto Message = unavailable->getMessage();
      auto attr = AvailableAttr::createPlatformAgnostic(C, Message);
      MappedDecl->getAttrs().add(attr);
      AnyUnavailable = true;
      continue;
    }

    //
    // __attribute__((annotate(swift1_unavailable)))
    //
    // Mapping: @available(*, unavailable)
    //
    if (auto unavailable_annot = dyn_cast<clang::AnnotateAttr>(*AI))
      if (unavailable_annot->getAnnotation() == "swift1_unavailable") {
        auto attr = AvailableAttr::createPlatformAgnostic(
            C, "", "", PlatformAgnosticAvailabilityKind::UnavailableInSwift);
        MappedDecl->getAttrs().add(attr);
        AnyUnavailable = true;
        continue;
      }

    //
    // __attribute__((deprecated))
    //
    // Mapping: @available(*,deprecated)
    //
    if (auto deprecated = dyn_cast<clang::DeprecatedAttr>(*AI)) {
      auto Message = deprecated->getMessage();
      auto attr = AvailableAttr::createPlatformAgnostic(C, Message, "",
                    PlatformAgnosticAvailabilityKind::Deprecated);
      MappedDecl->getAttrs().add(attr);
      continue;
    }

    // __attribute__((availability))
    //
    if (auto avail = dyn_cast<clang::AvailabilityAttr>(*AI)) {
      StringRef Platform = avail->getPlatform()->getName();

      // Is this our special "availability(swift, unavailable)" attribute?
      if (Platform == "swift") {
        // FIXME: Until Apple gets a chance to update UIKit's API notes, ignore
        // the Swift-unavailability for certain properties.
        if (isSpecialUIKitStructZeroProperty(ClangDecl))
          continue;

        auto replacement = avail->getReplacement();
        StringRef swiftReplacement = "";
        if (!replacement.empty())
          swiftReplacement = getSwiftNameFromClangName(replacement);

        auto attr = AvailableAttr::createPlatformAgnostic(
            C, avail->getMessage(), swiftReplacement,
            PlatformAgnosticAvailabilityKind::UnavailableInSwift);
        MappedDecl->getAttrs().add(attr);
        AnyUnavailable = true;
        continue;
      }

      // Does this availability attribute map to the platform we are
      // currently targeting?
      if (!platformAvailability.isPlatformRelevant(Platform))
        continue;

      auto platformK =
          llvm::StringSwitch<llvm::Optional<PlatformKind>>(Platform)
              .Case("ios", PlatformKind::iOS)
              .Case("macos", PlatformKind::macOS)
              .Case("maccatalyst", PlatformKind::macCatalyst)
              .Case("tvos", PlatformKind::tvOS)
              .Case("watchos", PlatformKind::watchOS)
              .Case("ios_app_extension", PlatformKind::iOSApplicationExtension)
              .Case("maccatalyst_app_extension",
                    PlatformKind::macCatalystApplicationExtension)
              .Case("macos_app_extension",
                    PlatformKind::macOSApplicationExtension)
              .Case("tvos_app_extension",
                    PlatformKind::tvOSApplicationExtension)
              .Case("watchos_app_extension",
                    PlatformKind::watchOSApplicationExtension)
              .Default(llvm::None);
      if (!platformK)
        continue;

      // Is this declaration marked platform-agnostically unavailable?
      auto PlatformAgnostic = PlatformAgnosticAvailabilityKind::None;
      if (avail->getUnavailable()) {
        PlatformAgnostic = PlatformAgnosticAvailabilityKind::Unavailable;
        AnyUnavailable = true;
      }

      auto IsSPI = isUsingMacroName(getClangASTContext().getSourceManager(),
                              avail->getLoc(), "SPI_AVAILABLE") ||
                   isUsingMacroName(getClangASTContext().getSourceManager(),
                              avail->getLoc(), "__SPI_AVAILABLE");

      StringRef message = avail->getMessage();

      llvm::VersionTuple deprecated = avail->getDeprecated();

      if (!deprecated.empty()) {
        if (platformAvailability.treatDeprecatedAsUnavailable(
                ClangDecl, deprecated, isAsync)) {
          AnyUnavailable = true;
          PlatformAgnostic = PlatformAgnosticAvailabilityKind::Unavailable;
          if (message.empty()) {
            if (isAsync) {
              message =
                  platformAvailability.asyncDeprecatedAsUnavailableMessage;
            } else {
              message = platformAvailability.deprecatedAsUnavailableMessage;
            }
          }
        }
      }

      llvm::VersionTuple obsoleted = avail->getObsoleted();
      llvm::VersionTuple introduced = avail->getIntroduced();

      const auto &replacement = avail->getReplacement();

      StringRef swiftReplacement = "";
      if (!replacement.empty())
        swiftReplacement = getSwiftNameFromClangName(replacement);

      auto AvAttr = new (C) AvailableAttr(SourceLoc(), SourceRange(),
                                          platformK.value(),
                                          message, swiftReplacement,
                                          /*RenameDecl=*/nullptr,
                                          introduced,
                                          /*IntroducedRange=*/SourceRange(),
                                          deprecated,
                                          /*DeprecatedRange=*/SourceRange(),
                                          obsoleted,
                                          /*ObsoletedRange=*/SourceRange(),
                                          PlatformAgnostic, /*Implicit=*/false,
                                          EnableClangSPI && IsSPI);

      MappedDecl->getAttrs().add(AvAttr);
    }

    // __attribute__((swift_attr("attribute"))) are handled by
    // importSwiftAttrAttributes(). Other attributes are ignored.
  }

  if (auto method = dyn_cast<clang::ObjCMethodDecl>(ClangDecl)) {
    if (method->isDirectMethod() && !AnyUnavailable) {
      assert(isa<AbstractFunctionDecl>(MappedDecl) &&
             "objc_direct declarations are expected to be an AbstractFunctionDecl");
      MappedDecl->getAttrs().add(new (C) FinalAttr(/*IsImplicit=*/true));
      if (auto accessorDecl = dyn_cast<AccessorDecl>(MappedDecl)) {
        auto attr = new (C) FinalAttr(/*isImplicit=*/true);
        accessorDecl->getStorage()->getAttrs().add(attr);
      }
    }
  }

  // If the declaration is unavailable, we're done.
  if (AnyUnavailable)
    return;

  if (auto ID = dyn_cast<clang::ObjCInterfaceDecl>(ClangDecl)) {
    // Ban NSInvocation.
    if (ID->getName() == "NSInvocation") {
      auto attr = AvailableAttr::createPlatformAgnostic(C, "");
      MappedDecl->getAttrs().add(attr);
      return;
    }

    // Map Clang's swift_objc_members attribute to @objcMembers.
    if (ID->hasAttr<clang::SwiftObjCMembersAttr>() &&
        isa<ClassDecl>(MappedDecl)) {
      if (!MappedDecl->getAttrs().hasAttribute<ObjCMembersAttr>()) {
        auto attr = new (C) ObjCMembersAttr(/*IsImplicit=*/true);
        MappedDecl->getAttrs().add(attr);
      }
    }

    // Infer @objcMembers on XCTestCase.
    if (ID->getName() == "XCTestCase") {
      if (!MappedDecl->getAttrs().hasAttribute<ObjCMembersAttr>()) {
        auto attr = new (C) ObjCMembersAttr(/*IsImplicit=*/true);
        MappedDecl->getAttrs().add(attr);
      }
    }
  }

  // Ban CFRelease|CFRetain|CFAutorelease(CFTypeRef) as well as custom ones
  // such as CGColorRelease(CGColorRef).
  if (auto FD = dyn_cast<clang::FunctionDecl>(ClangDecl)) {
    if (FD->getNumParams() == 1 && FD->getDeclName().isIdentifier() &&
         (FD->getName().endswith("Release") ||
          FD->getName().endswith("Retain") ||
          FD->getName().endswith("Autorelease")) &&
        !FD->getAttr<clang::SwiftNameAttr>()) {
      if (auto t = FD->getParamDecl(0)->getType()->getAs<clang::TypedefType>()){
        if (isCFTypeDecl(t->getDecl())) {
          auto attr = AvailableAttr::createPlatformAgnostic(C,
            "Core Foundation objects are automatically memory managed");
          MappedDecl->getAttrs().add(attr);
          return;
        }
      }
    }
  }

  // Hack: mark any method named "print" with less than two parameters as
  // warn_unqualified_access.
  if (auto MD = dyn_cast<FuncDecl>(MappedDecl)) {
    if (isPrintLikeMethod(MD->getName(), MD->getDeclContext())) {
      // Use a non-implicit attribute so it shows up in the generated
      // interface.
      MD->getAttrs().add(new (C) WarnUnqualifiedAccessAttr(/*implicit*/false));
    }
  }

  // Map __attribute__((warn_unused_result)).
  if (!ClangDecl->hasAttr<clang::WarnUnusedResultAttr>()) {
    if (auto MD = dyn_cast<FuncDecl>(MappedDecl)) {
      // Ask if the clang function's return type is void to prevent eagerly
      // loading the result type of the imported function.
      bool hasVoidReturnType = false;
      if (auto clangFunction = dyn_cast<clang::FunctionDecl>(ClangDecl))
        hasVoidReturnType = clangFunction->getReturnType()->isVoidType();
      if (auto clangMethod = dyn_cast<clang::ObjCMethodDecl>(ClangDecl))
        hasVoidReturnType = clangMethod->getReturnType()->isVoidType();
      // Async functions might get re-written to be non-void, so if this is an
      // async function, eagerly load the result type and check.
      if (MD->hasAsync())
        hasVoidReturnType = MD->getResultInterfaceType()->isVoid();
      if (!hasVoidReturnType)
        MD->getAttrs().add(new (C) DiscardableResultAttr(/*implicit*/true));
    }
  }
  // Map __attribute__((const)).
  if (ClangDecl->hasAttr<clang::ConstAttr>()) {
    MappedDecl->getAttrs().add(new (C) EffectsAttr(EffectsKind::ReadNone));
  }
  // Map __attribute__((pure)).
  if (ClangDecl->hasAttr<clang::PureAttr>()) {
    MappedDecl->getAttrs().add(new (C) EffectsAttr(EffectsKind::ReadOnly));
  }
}

Decl *
ClangImporter::Implementation::importDeclImpl(const clang::NamedDecl *ClangDecl,
                                              ImportNameVersion version,
                                              bool &TypedefIsSuperfluous,
                                              bool &HadForwardDeclaration) {
  assert(ClangDecl);

  // If this decl isn't valid, don't import it. Bail now.
  if (ClangDecl->isInvalidDecl())
    return nullptr;

  // Private and protected C++ class members should never be used, so we skip
  // them entirely (instead of importing them with a corresponding Swift access
  // level) to remove clutter from the module interface.
  //
  // We omit protected members in addition to private members because Swift
  // structs can't inherit from C++ classes, so there's effectively no way to
  // access them.
  clang::AccessSpecifier access = ClangDecl->getAccess();
  if (access == clang::AS_protected || access == clang::AS_private)
    return nullptr;

  bool SkippedOverTypedef = false;
  Decl *Result = nullptr;
  if (auto *UnderlyingDecl = canSkipOverTypedef(*this, ClangDecl,
                                                TypedefIsSuperfluous)) {
    Result = importDecl(UnderlyingDecl, version);
    SkippedOverTypedef = true;
  }
  
  if (!Result) {
    SwiftDeclConverter converter(*this, version);
    Result = converter.Visit(ClangDecl);
    HadForwardDeclaration = converter.hadForwardDeclaration();
  }
  if (!Result && version == CurrentVersion) {
    // If we couldn't import this Objective-C entity, determine
    // whether it was a required member of a protocol, or a designated
    // initializer of a class.
    bool hasMissingRequiredMember = false;
    if (auto clangProto
          = dyn_cast<clang::ObjCProtocolDecl>(ClangDecl->getDeclContext())) {
      if (auto method = dyn_cast<clang::ObjCMethodDecl>(ClangDecl)) {
        if (method->getImplementationControl()
              == clang::ObjCMethodDecl::Required)
          hasMissingRequiredMember = true;
      } else if (auto prop = dyn_cast<clang::ObjCPropertyDecl>(ClangDecl)) {
        if (prop->getPropertyImplementation()
              == clang::ObjCPropertyDecl::Required)
          hasMissingRequiredMember = true;
      }

      if (hasMissingRequiredMember) {
        // Mark the protocol as having missing requirements.
        if (auto proto = castIgnoringCompatibilityAlias<ProtocolDecl>(
                importDecl(clangProto, CurrentVersion))) {
          proto->setHasMissingRequirements(true);
        }
      }
    }
    if (auto method = dyn_cast<clang::ObjCMethodDecl>(ClangDecl)) {
      if (method->isDesignatedInitializerForTheInterface()) {
        const clang::ObjCInterfaceDecl *theClass = method->getClassInterface();
        assert(theClass && "cannot be a protocol method here");
        // Only allow this to affect declarations in the same top-level module
        // as the original class.
        if (getClangModuleForDecl(theClass) == getClangModuleForDecl(method)) {
          if (auto swiftClass = castIgnoringCompatibilityAlias<ClassDecl>(
                  importDecl(theClass, CurrentVersion))) {
            SwiftContext.evaluator.cacheOutput(
                HasMissingDesignatedInitializersRequest{swiftClass}, true);
          }
        }
      }
    }

    return nullptr;
  }

  // Finalize the imported declaration.
  auto finalizeDecl = [&](Decl *result) {
    importAttributes(ClangDecl, result);

    // Hack to deal with Objective-C protocols without availability annotation.
    // If the protocol comes from clang and is not annotated and the protocol
    // requirement itself is not annotated, then infer availability of the
    // requirement based on its types. This makes it possible for a type to
    // conform to an Objective-C protocol that is missing annotations but whose
    // requirements use types that are less available than the conforming type.
    auto dc = result->getDeclContext();
    auto *proto = dyn_cast<ProtocolDecl>(dc);
    if (!proto || proto->getAttrs().hasAttribute<AvailableAttr>())
      return;

    inferProtocolMemberAvailability(*this, dc, result);
  };

  if (Result) {
    finalizeDecl(Result);

    for (auto alternate : getAlternateDecls(Result))
      finalizeDecl(alternate);
  }

#ifndef NDEBUG
  auto Canon = cast<clang::NamedDecl>(ClangDecl->getCanonicalDecl());

  // Note that the decl was imported from Clang.  Don't mark Swift decls as
  // imported.
  if (Result &&
      (!Result->getDeclContext()->isModuleScopeContext() ||
       isa<ClangModuleUnit>(Result->getDeclContext()))) {
    // Either the Swift declaration was from stdlib,
    // or we imported the underlying decl of the typedef,
    // or we imported the decl itself.
    bool ImportedCorrectly =
        !Result->getClangDecl() || SkippedOverTypedef ||
        Result->getClangDecl()->getCanonicalDecl() == Canon;

    // Or the other type is a typedef,
    if (!ImportedCorrectly &&
        isa<clang::TypedefNameDecl>(Result->getClangDecl())) {
      // both types are ValueDecls:
      if (isa<clang::ValueDecl>(Result->getClangDecl())) {
        ImportedCorrectly =
            getClangASTContext().hasSameType(
                cast<clang::ValueDecl>(Result->getClangDecl())->getType(),
                cast<clang::ValueDecl>(Canon)->getType());
      } else if (isa<clang::TypeDecl>(Result->getClangDecl())) {
        // both types are TypeDecls:
        ImportedCorrectly =
            getClangASTContext().hasSameUnqualifiedType(
                getClangASTContext().getTypeDeclType(
                    cast<clang::TypeDecl>(Result->getClangDecl())),
                getClangASTContext().getTypeDeclType(
                    cast<clang::TypeDecl>(Canon)));
      }
      assert(ImportedCorrectly);
    }
    assert(Result->hasClangNode());
  }
#else
  (void)SkippedOverTypedef;
#endif

  return Result;
}

void ClangImporter::Implementation::startedImportingEntity() {
  ++NumTotalImportedEntities;
  // FIXME: (transitional) increment the redundant "always-on" counter.
  if (auto *Stats = SwiftContext.Stats)
    ++Stats->getFrontendCounters().NumTotalClangImportedEntities;
}

/// Look up associated type requirements in the conforming type.
static void finishTypeWitnesses(
    NormalProtocolConformance *conformance) {
  auto *dc = conformance->getDeclContext();
  auto nominal = dc->getSelfNominalTypeDecl();
  auto *module = dc->getParentModule();

  auto *proto = conformance->getProtocol();
  auto selfType = conformance->getType();

  for (auto *assocType : proto->getAssociatedTypeMembers()) {
    // FIXME: This should not happen?
    if (conformance->hasTypeWitness(assocType)) continue;

    bool satisfied = false;

    SmallVector<ValueDecl *, 4> lookupResults;
    NLOptions options = (NL_QualifiedDefault |
                          NL_OnlyTypes |
                          NL_ProtocolMembers);

    dc->lookupQualified(nominal, DeclNameRef(assocType->getName()),
                        nominal->getLoc(), options,
                        lookupResults);
    for (auto member : lookupResults) {
      auto typeDecl = cast<TypeDecl>(member);
      if (isa<AssociatedTypeDecl>(typeDecl)) continue;

      auto memberType = typeDecl->getDeclaredInterfaceType();
      auto subMap = selfType->getContextSubstitutionMap(
          module, typeDecl->getDeclContext());
      memberType = memberType.subst(subMap);
      conformance->setTypeWitness(assocType, memberType, typeDecl);
      satisfied = true;
      break;
    }

    if (!satisfied) {
      llvm::errs() << ("Cannot look up associated type for "
                        "imported conformance:\n");
      conformance->getType().dump(llvm::errs());
      assocType->dump(llvm::errs());
      abort();
    }
  }
}

/// Create witnesses for requirements not already met.
static void finishMissingOptionalWitnesses(
    NormalProtocolConformance *conformance) {
  auto *proto = conformance->getProtocol();

  for (auto req : proto->getMembers()) {
    auto valueReq = dyn_cast<ValueDecl>(req);
    if (!valueReq)
      continue;

    if (!conformance->hasWitness(valueReq)) {
      if (auto func = dyn_cast<AbstractFunctionDecl>(valueReq)){
        // For an optional requirement, record an empty witness:
        // we'll end up querying this at runtime.
        auto Attrs = func->getAttrs();
        if (Attrs.hasAttribute<OptionalAttr>()) {
          conformance->setWitness(valueReq, Witness());
          continue;
        }
      }

      conformance->setWitness(valueReq, valueReq);
    } else {
      // An initializer that conforms to a requirement is required.
      auto witness = conformance->getWitness(valueReq).getDecl();
      if (auto ctor = dyn_cast_or_null<ConstructorDecl>(witness)) {
        if (!ctor->getAttrs().hasAttribute<RequiredAttr>()) {
          auto &ctx = proto->getASTContext();
          ctor->getAttrs().add(new (ctx) RequiredAttr(/*IsImplicit=*/true));
        }
      }
    }
  }
}

void ClangImporter::Implementation::finishNormalConformance(
    NormalProtocolConformance *conformance,
    uint64_t unused) {
  (void)unused;

  auto *proto = conformance->getProtocol();
  PrettyStackTraceConformance trace("completing import of", conformance);

  finishTypeWitnesses(conformance);
  conformance->finishSignatureConformances();

  // Imported conformances to @objc protocols also require additional
  // initialization to complete the requirement to witness mapping.
  if (!proto->isObjC())
    return;

  assert(conformance->isComplete());
  conformance->setState(ProtocolConformanceState::Incomplete);

  finishMissingOptionalWitnesses(conformance);

  conformance->setState(ProtocolConformanceState::Complete);
}

Decl *ClangImporter::Implementation::importDeclAndCacheImpl(
    const clang::NamedDecl *ClangDecl, ImportNameVersion version,
    bool SuperfluousTypedefsAreTransparent, bool UseCanonicalDecl) {
  if (!ClangDecl)
    return nullptr;

  FrontendStatsTracer StatsTracer(SwiftContext.Stats,
                                  "import-clang-decl", ClangDecl);
  clang::PrettyStackTraceDecl trace(ClangDecl, clang::SourceLocation(),
                                    Instance->getSourceManager(), "importing");

  auto Canon = cast<clang::NamedDecl>(UseCanonicalDecl? ClangDecl->getCanonicalDecl(): ClangDecl);

  auto Known = importDeclCached(Canon, version, UseCanonicalDecl);
  if (Known.has_value()) {
    if (!SuperfluousTypedefsAreTransparent &&
        SuperfluousTypedefs.count(Canon))
      return nullptr;
    return Known.value();
  }

  bool TypedefIsSuperfluous = false;
  bool HadForwardDeclaration = false;

  startedImportingEntity();
  Decl *Result = importDeclImpl(ClangDecl, version, TypedefIsSuperfluous,
                                HadForwardDeclaration);
  if (!Result) {
    ImportedDecls[{Canon, version}] = nullptr;
    return nullptr;
  }

  if (TypedefIsSuperfluous) {
    SuperfluousTypedefs.insert(Canon);
    if (auto tagDecl = dyn_cast_or_null<clang::TagDecl>(Result->getClangDecl()))
      DeclsWithSuperfluousTypedefs.insert(tagDecl);
  }

  if (!HadForwardDeclaration)
    ImportedDecls[{Canon, version}] = Result;

  if (!SuperfluousTypedefsAreTransparent && TypedefIsSuperfluous)
    return nullptr;

  return Result;
}

Decl *
ClangImporter::Implementation::importMirroredDecl(const clang::NamedDecl *decl,
                                                  DeclContext *dc,
                                                  ImportNameVersion version,
                                                  ProtocolDecl *proto) {
  assert(dc);
  if (!decl)
    return nullptr;

  clang::PrettyStackTraceDecl trace(decl, clang::SourceLocation(),
                                    Instance->getSourceManager(),
                                    "importing (mirrored)");

  auto canon = decl->getCanonicalDecl();
  auto known = ImportedProtocolDecls.find(std::make_tuple(canon, dc, version));
  if (known != ImportedProtocolDecls.end())
    return known->second;

  SwiftDeclConverter converter(*this, version);
  Decl *result;
  if (auto method = dyn_cast<clang::ObjCMethodDecl>(decl)) {
    result =
        converter.importObjCMethodDecl(method, dc, /*accessor*/ llvm::None);
  } else if (auto prop = dyn_cast<clang::ObjCPropertyDecl>(decl)) {
    result = converter.importObjCPropertyDecl(prop, dc);
  } else {
    llvm_unreachable("unexpected mirrored decl");
  }

  if (result) {
    assert(result->getClangDecl() && result->getClangDecl() == canon);

    auto updateMirroredDecl = [&](Decl *result) {
      result->setImplicit();

      // Map the Clang attributes onto Swift attributes.
      importAttributes(decl, result);

      if (proto->getAttrs().hasAttribute<AvailableAttr>()) {
        if (!result->getAttrs().hasAttribute<AvailableAttr>()) {
          AvailabilityContext protoRange =
            AvailabilityInference::availableRange(proto, SwiftContext);
          applyAvailableAttribute(result, protoRange, SwiftContext);
        }
      } else {
        // Infer the same availability for the mirrored declaration as
        // we would for the protocol member it is mirroring.
        inferProtocolMemberAvailability(*this, dc, result);
      }
    };

    updateMirroredDecl(result);

    // Update the alternate declaration as well.
    for (auto alternate : getAlternateDecls(result))
      updateMirroredDecl(alternate);
  }
  if (result || !converter.hadForwardDeclaration())
    ImportedProtocolDecls[std::make_tuple(canon, dc, version)] = result;
  return result;
}

DeclContext *ClangImporter::Implementation::importDeclContextImpl(
    const clang::Decl *ImportingDecl, const clang::DeclContext *dc) {
  // Every declaration should come from a module, so we should not see the
  // TranslationUnit DeclContext here.
  assert(!dc->isTranslationUnit());

  auto decl = dyn_cast<clang::NamedDecl>(dc);
  if (!decl)
    return nullptr;

  // Category decls with same name can be merged and using canonical decl always
  // leads to the first category of the given name. We'd like to keep these
  // categories separated.
  auto useCanonical =
      !isa<clang::ObjCCategoryDecl>(decl) && !isa<clang::NamespaceDecl>(decl);
  auto swiftDecl = importDeclForDeclContext(ImportingDecl, decl->getName(),
                                            decl, CurrentVersion, useCanonical);
  if (!swiftDecl)
    return nullptr;

  if (auto nominal = dynCastIgnoringCompatibilityAlias<NominalTypeDecl>(swiftDecl))
    return nominal;
  if (auto extension = dyn_cast<ExtensionDecl>(swiftDecl))
    return extension;
  if (auto constructor = dyn_cast<ConstructorDecl>(swiftDecl))
    return constructor;
  if (auto destructor = dyn_cast<DestructorDecl>(swiftDecl))
    return destructor;
  return nullptr;
}

GenericSignature ClangImporter::Implementation::buildGenericSignature(
    GenericParamList *genericParams, DeclContext *dc) {
  SmallVector<GenericTypeParamType *, 2> genericParamTypes;
  for (auto param : *genericParams) {
    genericParamTypes.push_back(
        param->getDeclaredInterfaceType()->castTo<GenericTypeParamType>());
  }

  SmallVector<Requirement, 2> requirements;
  for (auto param : *genericParams) {
    Type paramType = param->getDeclaredInterfaceType();
    for (const auto &inherited : param->getInherited()) {
      Type inheritedType = inherited.getType();
      if (inheritedType->isAnyObject()) {
        requirements.push_back(
            Requirement(
              RequirementKind::Layout, paramType,
              LayoutConstraint::getLayoutConstraint(LayoutConstraintKind::Class)));
        continue;
      }
      if (inheritedType->getClassOrBoundGenericClass()) {
        requirements.push_back(
            Requirement(RequirementKind::Superclass, paramType, inheritedType));
        continue;
      }
      assert(inheritedType->isExistentialType());
      requirements.push_back(
          Requirement(RequirementKind::Conformance, paramType, inheritedType));
    }
  }

  return swift::buildGenericSignature(
      SwiftContext, GenericSignature(),
      std::move(genericParamTypes),
      std::move(requirements));
}

Decl *
ClangImporter::Implementation::importDeclForDeclContext(
    const clang::Decl *importingDecl,
    StringRef writtenName,
    const clang::NamedDecl *contextDecl,
    Version version,
    bool useCanonicalDecl)
{
  auto key = std::make_tuple(importingDecl, writtenName, contextDecl, version,
                             useCanonicalDecl);
  auto iter = find(llvm::reverse(contextDeclsBeingImported), key);

  // No cycle? Remember that we're importing this, then import normally.
  if (iter == contextDeclsBeingImported.rend()) {
    contextDeclsBeingImported.push_back(key);
    auto imported = importDecl(contextDecl, version, useCanonicalDecl);
    contextDeclsBeingImported.pop_back();
    return imported;
  }

  // There's a cycle. Is the declaration imported enough to break the cycle
  // gracefully? If so, we'll have it in the decl cache.
  auto cached = importDeclCached(contextDecl, version, useCanonicalDecl);
  if (cached.has_value())
    return cached.value();

  // Can't break it? Warn and return nullptr, which is at least better than
  // stack overflow by recursion.

  // Avoid emitting warnings repeatedly.
  if (!contextDeclsWarnedAbout.insert(contextDecl).second)
    return nullptr;

  auto getDeclName = [](const clang::Decl *D) -> std::string {
    if (auto ND = dyn_cast<clang::NamedDecl>(D)) {
      std::string name;
      llvm::raw_string_ostream os(name);
      ND->printName(os);
      return name;
    }
    return "<anonymous>";
  };

  HeaderLoc loc(importingDecl->getLocation());
  diagnose(loc, diag::swift_name_circular_context_import,
           writtenName, getDeclName(importingDecl));

  // Diagnose other decls involved in the cycle.
  for (auto entry : make_range(contextDeclsBeingImported.rbegin(), iter)) {
    auto otherDecl = std::get<0>(entry);
    auto otherWrittenName = std::get<1>(entry);
    diagnose(HeaderLoc(otherDecl->getLocation()),
             diag::swift_name_circular_context_import_other,
             otherWrittenName, getDeclName(otherDecl));
  }

  if (auto *parentModule = contextDecl->getOwningModule()) {
    diagnose(loc, diag::unresolvable_clang_decl_is_a_framework_bug,
             parentModule->getFullModuleName());
  }

  return nullptr;
}

DeclContext *
ClangImporter::Implementation::importDeclContextOf(
  const clang::Decl *decl,
  EffectiveClangContext context)
{
  DeclContext *importedDC = nullptr;
  switch (context.getKind()) {
  case EffectiveClangContext::DeclContext: {
    auto dc = context.getAsDeclContext();

    // For C++-Interop in cases where #ifdef __cplusplus surround an extern "C"
    // you want to first check if the TU decl is the parent of this extern "C"
    // decl (aka LinkageSpecDecl) and then proceed.
    if (dc->getDeclKind() == clang::Decl::LinkageSpec)
      dc = dc->getParent();

    if (auto functionDecl = dyn_cast<clang::FunctionDecl>(decl)) {
      // Treat friend decls like top-level decls.
      if (functionDecl->getFriendObjectKind()) {
        // Find the top-level decl context.
        while (isa<clang::NamedDecl>(dc))
          dc = dc->getParent();
      }

      // If this is a non-member operator, import it as a top-level function.
      if (functionDecl->isOverloadedOperator()) {
        while (dc->isNamespace())
          dc = dc->getParent();
      }
    }

    if (dc->isTranslationUnit()) {
      if (auto *module = getClangModuleForDecl(decl))
        return module;
      else
        return nullptr;
    }

    // Import the DeclContext.
    importedDC = importDeclContextImpl(decl, dc);
    break;
  }

  case EffectiveClangContext::TypedefContext: {
    // Import the typedef-name as a declaration.
    auto importedDecl = importDeclForDeclContext(
        decl, context.getTypedefName()->getName(), context.getTypedefName(),
        CurrentVersion);
    if (!importedDecl) return nullptr;

    // Dig out the imported DeclContext.
    importedDC = dynCastIgnoringCompatibilityAlias<NominalTypeDecl>(importedDecl);
    break;
  }

  case EffectiveClangContext::UnresolvedContext: {
    // FIXME: Resolve through name lookup. This is brittle.
    auto submodule =
      getClangSubmoduleForDecl(decl, /*allowForwardDeclaration=*/false);
    if (!submodule) return nullptr;

    if (auto lookupTable = findLookupTable(*submodule)) {
      if (auto clangDecl
            = lookupTable->resolveContext(context.getUnresolvedName())) {
        // Import the Clang declaration.
        auto swiftDecl = importDeclForDeclContext(decl,
                                                  context.getUnresolvedName(),
                                                  clangDecl, CurrentVersion);
        if (!swiftDecl) return nullptr;

        // Look through typealiases.
        if (auto typealias = dyn_cast<TypeAliasDecl>(swiftDecl))
          importedDC = typealias->getDeclaredInterfaceType()->getAnyNominal();
        else // Map to a nominal type declaration.
          importedDC = dyn_cast<NominalTypeDecl>(swiftDecl);
      }
    }
    break;
  }
  }

  // If we didn't manage to import the declaration context, we're done.
  if (!importedDC) return nullptr;

  // If the declaration was not global to start with, we're done.
  bool isGlobal =
    decl->getDeclContext()->getRedeclContext()->isTranslationUnit();
  if (!isGlobal) return importedDC;

  // If the resulting declaration context is not a nominal type,
  // we're done.
  auto nominal = dyn_cast<NominalTypeDecl>(importedDC);
  if (!nominal) return importedDC;

  // Look for the extension for the given nominal type within the
  // Clang submodule of the declaration.
  const clang::Module *declSubmodule = *getClangSubmoduleForDecl(decl);
  auto extensionKey = std::make_pair(nominal, declSubmodule);
  auto knownExtension = extensionPoints.find(extensionKey);
  if (knownExtension != extensionPoints.end())
    return knownExtension->second;

  // Create a new extension for this nominal type/Clang submodule pair.
  auto ext = ExtensionDecl::create(SwiftContext, SourceLoc(), nullptr, {},
                                   getClangModuleForDecl(decl), nullptr);
  SwiftContext.evaluator.cacheOutput(ExtendedTypeRequest{ext},
                                     nominal->getDeclaredType());
  SwiftContext.evaluator.cacheOutput(ExtendedNominalRequest{ext},
                                     std::move(nominal));
  ext->setMemberLoader(this, reinterpret_cast<uintptr_t>(declSubmodule));

  if (auto protoDecl = ext->getExtendedProtocolDecl()) {
    ext->setGenericSignature(protoDecl->getGenericSignature());
  }

  // Add the extension to the nominal type.
  nominal->addExtension(ext);

  // Record this extension so we can find it later.
  extensionPoints[extensionKey] = ext;
  return ext;
}

/// Create a decl with error type and an "unavailable" attribute on it
/// with the specified message.
void ClangImporter::Implementation::
markUnavailable(ValueDecl *decl, StringRef unavailabilityMsgRef) {

  unavailabilityMsgRef = SwiftContext.AllocateCopy(unavailabilityMsgRef);
  auto ua = AvailableAttr::createPlatformAgnostic(SwiftContext,
                                                  unavailabilityMsgRef);
  decl->getAttrs().add(ua);
}

/// Create a decl with error type and an "unavailable" attribute on it
/// with the specified message.
ValueDecl *ClangImporter::Implementation::
createUnavailableDecl(Identifier name, DeclContext *dc, Type type,
                      StringRef UnavailableMessage, bool isStatic,
                      ClangNode ClangN) {

  // Create a new VarDecl with dummy type.
  auto var = createDeclWithClangNode<VarDecl>(ClangN, AccessLevel::Public,
                                              /*IsStatic*/isStatic,
                                              VarDecl::Introducer::Var,
                                              SourceLoc(), name, dc);
  var->setIsObjC(false);
  var->setIsDynamic(false);
  var->setInterfaceType(type);
  markUnavailable(var, UnavailableMessage);

  return var;
}

// Force the members of the entire inheritance hierarchy to be loaded and
// deserialized before loading the members of this class. This allows the
// decl members table to be warmed up and enables the correct identification of
// overrides.
static void loadAllMembersOfSuperclassIfNeeded(ClassDecl *CD) {
  if (!CD)
    return;

  CD = CD->getSuperclassDecl();
  if (!CD || !CD->hasClangNode())
    return;

  CD->loadAllMembers();

  for (auto E : CD->getExtensions())
    E->loadAllMembers();
}

void ClangImporter::Implementation::loadAllMembersOfRecordDecl(
    NominalTypeDecl *swiftDecl, const clang::RecordDecl *clangRecord) {
  // Import all of the members.
  llvm::SmallVector<Decl *, 16> members;
  for (const clang::Decl *m : clangRecord->decls()) {
    auto nd = dyn_cast<clang::NamedDecl>(m);
    if (!nd)
      continue;

    // Currently, we don't import unnamed bitfields.
    if (isa<clang::FieldDecl>(m) &&
        cast<clang::FieldDecl>(m)->isUnnamedBitfield())
      continue;

    // Make sure we always pull in record fields. Everything else had better
    // be canonical. Note that this check mostly catches nested C++ types since
    // we import nested C struct types by C's usual convention of chucking them
    // into the global namespace.
    const bool isCanonicalInContext =
        (isa<clang::FieldDecl>(nd) || nd == nd->getCanonicalDecl());
    if (isCanonicalInContext && nd->getDeclContext() == clangRecord &&
        isVisibleClangEntry(nd))
      insertMembersAndAlternates(nd, members);
  }

  // Add the members here.
  for (auto member: members) {
    // This means we found a member in a C++ record's base class.
    if (swiftDecl->getClangDecl() != clangRecord) {
      // So we need to clone the member into the derived class.
      if (auto newDecl = importBaseMemberDecl(cast<ValueDecl>(member), swiftDecl)) {
        swiftDecl->addMember(newDecl);
      }
      continue;
    }

    // FIXME: constructors are added eagerly, but shouldn't be
    // FIXME: subscripts are added eagerly, but shouldn't be
    if (!isa<AccessorDecl>(member) &&
        !isa<SubscriptDecl>(member) &&
        !isa<ConstructorDecl>(member)) {
      swiftDecl->addMember(member);
    }
  }

  // If this is a C++ record, look through the base classes too.
  if (auto cxxRecord = dyn_cast<clang::CXXRecordDecl>(clangRecord)) {
    for (auto base : cxxRecord->bases()) {
      if (base.getAccessSpecifier() != clang::AccessSpecifier::AS_public)
        continue;

      clang::QualType baseType = base.getType();
      if (auto spectType = dyn_cast<clang::TemplateSpecializationType>(baseType))
        baseType = spectType->desugar();
      if (auto elaborated = dyn_cast<clang::ElaboratedType>(baseType))
        baseType = elaborated->desugar();
      if (!isa<clang::RecordType>(baseType))
        continue;

      auto *baseRecord = cast<clang::RecordType>(baseType)->getDecl();
      loadAllMembersOfRecordDecl(swiftDecl, baseRecord);
    }
  }
}

void
ClangImporter::Implementation::loadAllMembers(Decl *D, uint64_t extra) {

  FrontendStatsTracer tracer(D->getASTContext().Stats,
                             "load-all-members", D);
  assert(D);

  // If a Clang decl has no owning module, then it needs to be added to the
  // bridging header lookup table. This has most likely already been done, but
  // in some cases, such as when processing DWARF imported AST nodes from LLDB,
  // it has not. Do it here just to be safe.
  if (auto namedDecl = dyn_cast_or_null<clang::NamedDecl>(D->getClangDecl())) {
    if (!namedDecl->hasOwningModule()) {
      auto mutableNamedDecl = const_cast<clang::NamedDecl *>(namedDecl);
      addBridgeHeaderTopLevelDecls(mutableNamedDecl);
      addEntryToLookupTable(*BridgingHeaderLookupTable,
                            mutableNamedDecl, *nameImporter);
    }
  }

  // Check whether we're importing an Objective-C container of some sort.
  auto objcContainer =
    dyn_cast_or_null<clang::ObjCContainerDecl>(D->getClangDecl());

  // If not, we're importing globals-as-members into an extension.
  if (objcContainer) {
    loadAllMembersOfSuperclassIfNeeded(dyn_cast<ClassDecl>(D));
    loadAllMembersOfObjcContainer(D, objcContainer);
    return;
  }

  if (isa_and_nonnull<clang::RecordDecl>(D->getClangDecl())) {
    loadAllMembersOfRecordDecl(cast<NominalTypeDecl>(D),
                               cast<clang::RecordDecl>(D->getClangDecl()));
    return;
  }

  if (isa_and_nonnull<clang::NamespaceDecl>(D->getClangDecl())) {
    // Namespace members will only be loaded lazily.
    cast<EnumDecl>(D)->setHasLazyMembers(true);
    return;
  }

  loadAllMembersIntoExtension(D, extra);
}

void ClangImporter::Implementation::loadAllMembersIntoExtension(
    Decl *D, uint64_t extra) {
  // We have extension.
  auto ext = cast<ExtensionDecl>(D);
  auto nominal = ext->getExtendedNominal();

  // The submodule of the extension is encoded in the extra data.
  clang::Module *submodule =
      reinterpret_cast<clang::Module *>(static_cast<uintptr_t>(extra));

  // Find the lookup table.
  auto topLevelModule = submodule;
  if (topLevelModule)
    topLevelModule = topLevelModule->getTopLevelModule();
  auto table = findLookupTable(topLevelModule);
  if (!table)
    return;

  PrettyStackTraceStringAction trace(
      "loading import-as-members from",
      topLevelModule ? topLevelModule->getTopLevelModuleName()
                     : "(bridging header)");
  PrettyStackTraceDecl trace2("...for", nominal);

  // Dig out the effective Clang context for this nominal type.
  auto effectiveClangContext = getEffectiveClangContext(nominal);
  if (!effectiveClangContext)
    return;

  // Get ready to actually load the members.
  startedImportingEntity();

  // Load the members.
  for (auto entry : table->allGlobalsAsMembersInContext(effectiveClangContext)) {
    auto decl = entry.get<clang::NamedDecl *>();

    // Only include members in the same submodule as this extension.
    if (getClangSubmoduleForDecl(decl) != submodule)
      continue;

    forEachDistinctName(
        decl, [&](ImportedName newName, ImportNameVersion nameVersion) -> bool {
      return addMemberAndAlternatesToExtension(decl, newName, nameVersion, ext);
    });
  }
}

static Decl *findMemberThatWillLandInAnExtensionContext(Decl *member) {
  Decl *result = member;
  while (!isa<ExtensionDecl>(result->getDeclContext())) {
    auto nominal = dyn_cast<NominalTypeDecl>(result->getDeclContext());
    if (!nominal)
      return nullptr;

    result = nominal;
    if (result->hasClangNode())
      return nullptr;
  }
  return result;
}

bool ClangImporter::Implementation::addMemberAndAlternatesToExtension(
    clang::NamedDecl *decl, ImportedName newName, ImportNameVersion nameVersion,
    ExtensionDecl *ext) {
  // Quickly check the context and bail out if it obviously doesn't
  // belong here.
  if (auto *importDC = newName.getEffectiveContext().getAsDeclContext())
    if (importDC->isFileContext())
      return true;

  // Then try to import the decl under the specified name.
  Decl *member = importDecl(decl, nameVersion);
  if (!member)
    return false;

  member = findMemberThatWillLandInAnExtensionContext(member);
  if (!member || member->getDeclContext() != ext)
    return true;
  if (!isa<AccessorDecl>(member))
    ext->addMember(member);

  for (auto alternate : getAlternateDecls(member)) {
    if (alternate->getDeclContext() == ext)
      if (!isa<AccessorDecl>(alternate))
        ext->addMember(alternate);
  }
  return true;
}

static void loadMembersOfBaseImportedFromClang(ExtensionDecl *ext) {
  const NominalTypeDecl *base = ext->getExtendedNominal();
  auto *clangBase = base->getClangDecl();
  if (!clangBase)
    return;
  base->loadAllMembers();

  // Sanity check: make sure we don't jump over to a category /while/
  // loading the original class's members. Right now we only check if this
  // happens on the first member.
  if (auto *clangContainer = dyn_cast<clang::ObjCContainerDecl>(clangBase))
    assert((clangContainer->decls_empty() || !base->getMembers().empty()) &&
           "can't load extension members before base has finished");
}

void ClangImporter::Implementation::loadAllMembersOfObjcContainer(
    Decl *D, const clang::ObjCContainerDecl *objcContainer) {
  clang::PrettyStackTraceDecl trace(objcContainer, clang::SourceLocation(),
                                    Instance->getSourceManager(),
                                    "loading members for");

  assert(isa<ExtensionDecl>(D) || isa<NominalTypeDecl>(D));
  if (auto *ext = dyn_cast<ExtensionDecl>(D)) {
    // If the extended type is also imported from Clang, load its members first.
    loadMembersOfBaseImportedFromClang(ext);
  }

  startedImportingEntity();

  SmallVector<Decl *, 16> members;
  collectMembersToAdd(objcContainer, D, cast<DeclContext>(D), members);

  auto *IDC = cast<IterableDeclContext>(D);
  for (auto member : members) {
    if (!isa<AccessorDecl>(member))
      IDC->addMember(member);
  }
}

void ClangImporter::Implementation::insertMembersAndAlternates(
    const clang::NamedDecl *nd, SmallVectorImpl<Decl *> &members) {

  size_t start = members.size();
  llvm::SmallPtrSet<Decl *, 4> knownAlternateMembers;
  Decl *asyncImport = nullptr;
  forEachDistinctName(
      nd, [&](ImportedName name, ImportNameVersion nameVersion) -> bool {
    auto member = importDecl(nd, nameVersion);
    if (!member) {
      if (SwiftContext.LangOpts.EnableExperimentalEagerClangModuleDiagnostics) {
        diagnoseTargetDirectly(nd);
      }
      return false;
    }

    // If there are alternate declarations for this member, add them.
    for (auto alternate : getAlternateDecls(member)) {
      if (alternate->getDeclContext() == member->getDeclContext() &&
          knownAlternateMembers.insert(alternate).second) {
        members.push_back(alternate);
      }
    }

    // If this declaration shouldn't be visible, don't add it to
    // the list.
    if (shouldSuppressDeclImport(nd))
      return true;

    members.push_back(member);
    if (nameVersion.supportsConcurrency()) {
      assert(!asyncImport &&
             "Should only have a single version with concurrency enabled");
      asyncImport = member;
    }

    return true;
  });

  addCompletionHandlerAttribute(asyncImport,
                                llvm::makeArrayRef(members).drop_front(start),
                                SwiftContext);
}

void ClangImporter::Implementation::importInheritedConstructors(
     const clang::ObjCInterfaceDecl *curObjCClass,
     const ClassDecl *classDecl, SmallVectorImpl<Decl *> &newMembers) {
  if (curObjCClass->getName() != "Protocol") {
    SwiftDeclConverter converter(*this, CurrentVersion);
    converter.importInheritedConstructors(classDecl, newMembers);
  }
}

void ClangImporter::Implementation::collectMembersToAdd(
    const clang::ObjCContainerDecl *objcContainer, Decl *D, DeclContext *DC,
    SmallVectorImpl<Decl *> &members) {
  for (const clang::Decl *m : objcContainer->decls()) {
    auto nd = dyn_cast<clang::NamedDecl>(m);
    if (nd && nd == nd->getCanonicalDecl() &&
        nd->getDeclContext() == objcContainer &&
        isVisibleClangEntry(nd))
      insertMembersAndAlternates(nd, members);
  }

  // Objective-C protocols don't require any special handling.
  if (isa<clang::ObjCProtocolDecl>(objcContainer))
    return;

  // Objective-C interfaces can inherit constructors from their superclass,
  // which we must model explicitly.
  if (auto clangClass = dyn_cast<clang::ObjCInterfaceDecl>(objcContainer)) {
    objcContainer = clangClass = clangClass->getDefinition();
    importInheritedConstructors(clangClass, cast<ClassDecl>(D), members);
  } else if (auto clangProto
               = dyn_cast<clang::ObjCProtocolDecl>(objcContainer)) {
    objcContainer = clangProto->getDefinition();
  }

  // Interfaces and categories can declare protocol conformances, and
  // members of those protocols are mirrored into the interface or
  // category.
  // FIXME: This is supposed to be a short-term hack.
  importMirroredProtocolMembers(objcContainer, DC, llvm::None, members);
}

void ClangImporter::Implementation::loadAllConformances(
       const Decl *decl, uint64_t contextData,
       SmallVectorImpl<ProtocolConformance *> &Conformances) {
  auto dc = decl->getInnermostDeclContext();

  // Synthesize trivial conformances for each of the protocols.
  for (auto *protocol : getImportedProtocols(decl)) {
    // FIXME: Build a superclass conformance if the superclass
    // conforms.
    auto conformance = SwiftContext.getConformance(
        dc->getDeclaredInterfaceType(),
        protocol, SourceLoc(), dc,
        ProtocolConformanceState::Incomplete,
        protocol->isSpecificProtocol(KnownProtocolKind::Sendable));
    conformance->setLazyLoader(this, /*context*/0);
    conformance->setState(ProtocolConformanceState::Complete);
    Conformances.push_back(conformance);
  }
}

llvm::Optional<MappedTypeNameKind>
ClangImporter::Implementation::getSpecialTypedefKind(
    clang::TypedefNameDecl *decl) {
  auto iter = SpecialTypedefNames.find(decl->getCanonicalDecl());
  if (iter == SpecialTypedefNames.end())
    return llvm::None;
  return iter->second;
}

Identifier
ClangImporter::getEnumConstantName(const clang::EnumConstantDecl *enumConstant){
  return Impl.importFullName(enumConstant, Impl.CurrentVersion)
      .getDeclName()
      .getBaseIdentifier();
}

// See swift/Basic/Statistic.h for declaration: this enables tracing
// clang::Decls, is defined here to avoid too much layering violation / circular
// linkage dependency.

struct ClangDeclTraceFormatter : public UnifiedStatsReporter::TraceFormatter {
  void traceName(const void *Entity, raw_ostream &OS) const override {
    if (!Entity)
      return;
    const clang::Decl *CD = static_cast<const clang::Decl *>(Entity);
    if (auto const *ND = dyn_cast<const clang::NamedDecl>(CD)) {
      ND->printName(OS);
    } else {
      OS << "<unnamed-clang-decl>";
    }
  }

  static inline bool printClangShortLoc(raw_ostream &OS,
                                        clang::SourceManager *CSM,
                                        clang::SourceLocation L) {
    if (!L.isValid() || !L.isFileID())
      return false;
    auto PLoc = CSM->getPresumedLoc(L);
    OS << llvm::sys::path::filename(PLoc.getFilename()) << ':' << PLoc.getLine()
       << ':' << PLoc.getColumn();
    return true;
  }

  void traceLoc(const void *Entity, SourceManager *SM,
                clang::SourceManager *CSM, raw_ostream &OS) const override {
    if (!Entity)
      return;
    if (CSM) {
      const clang::Decl *CD = static_cast<const clang::Decl *>(Entity);
      auto Range = CD->getSourceRange();
      if (printClangShortLoc(OS, CSM, Range.getBegin()))
        OS << '-';
      printClangShortLoc(OS, CSM, Range.getEnd());
    }
  }
};

static ClangDeclTraceFormatter TF;

template<>
const UnifiedStatsReporter::TraceFormatter*
FrontendStatsTracer::getTraceFormatter<const clang::Decl *>() {
  return &TF;
}
