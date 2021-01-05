//===--- TypeCheckDecl.cpp - Type Checking for Declarations ---------------===//
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
// This file implements semantic analysis for declarations.
//
//===----------------------------------------------------------------------===//

#include "CodeSynthesis.h"
#include "DerivedConformances.h"
#include "TypeChecker.h"
#include "TypeCheckAccess.h"
#include "TypeCheckAvailability.h"
#include "TypeCheckConcurrency.h"
#include "TypeCheckDecl.h"
#include "TypeCheckObjC.h"
#include "TypeCheckType.h"
#include "MiscDiagnostics.h"
#include "swift/AST/AccessScope.h"
#include "swift/AST/ASTPrinter.h"
#include "swift/AST/ASTVisitor.h"
#include "swift/AST/ASTWalker.h"
#include "swift/AST/ExistentialLayout.h"
#include "swift/AST/Expr.h"
#include "swift/AST/ForeignErrorConvention.h"
#include "swift/AST/GenericEnvironment.h"
#include "swift/AST/GenericSignatureBuilder.h"
#include "swift/AST/Initializer.h"
#include "swift/AST/NameLookup.h"
#include "swift/AST/OperatorNameLookup.h"
#include "swift/AST/PrettyStackTrace.h"
#include "swift/AST/PropertyWrappers.h"
#include "swift/AST/ProtocolConformance.h"
#include "swift/AST/SourceFile.h"
#include "swift/AST/TypeWalker.h"
#include "swift/Basic/Statistic.h"
#include "swift/Parse/Lexer.h"
#include "swift/Parse/Parser.h"
#include "swift/Sema/IDETypeChecking.h"
#include "swift/Serialization/SerializedModuleLoader.h"
#include "swift/Strings.h"
#include "swift/AST/NameLookupRequests.h"
#include "swift/AST/TypeCheckRequests.h"
#include "swift/Basic/Defer.h"
#include "llvm/ADT/APFloat.h"
#include "llvm/ADT/APInt.h"
#include "llvm/ADT/APSInt.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/ADT/Twine.h"
#include "llvm/Support/Compiler.h"
#include "llvm/Support/DJB.h"

using namespace swift;

#define DEBUG_TYPE "Serialization"

STATISTIC(NumLazyRequirementSignaturesLoaded,
          "# of lazily-deserialized requirement signatures loaded");

#undef DEBUG_TYPE
#define DEBUG_TYPE "TypeCheckDecl"

namespace {

/// Used during enum raw value checking to identify duplicate raw values.
/// Character, string, float, and integer literals are all keyed by value.
/// Float and integer literals are additionally keyed by numeric equivalence.
struct RawValueKey {
  enum class Kind : uint8_t {
    String, Float, Int, Bool, Tombstone, Empty
  } kind;
  
  struct IntValueTy {
    uint64_t v0;
    uint64_t v1;

    IntValueTy(const APInt &bits) {
      APInt bits128 = bits.sextOrTrunc(128);
      assert(bits128.getBitWidth() <= 128);
      const uint64_t *data = bits128.getRawData();
      v0 = data[0];
      v1 = data[1];
    }
  };

  struct FloatValueTy {
    uint64_t v0;
    uint64_t v1;
  };

  // FIXME: doesn't accommodate >64-bit or signed raw integer or float values.
  union {
    StringRef stringValue;
    IntValueTy intValue;
    FloatValueTy floatValue;
    bool boolValue;
  };
  
  explicit RawValueKey(LiteralExpr *expr) {
    switch (expr->getKind()) {
    case ExprKind::IntegerLiteral:
      kind = Kind::Int;
      intValue = IntValueTy(cast<IntegerLiteralExpr>(expr)->getValue());
      return;
    case ExprKind::FloatLiteral: {
      APFloat value = cast<FloatLiteralExpr>(expr)->getValue();
      llvm::APSInt asInt(127, /*isUnsigned=*/false);
      bool isExact = false;
      APFloat::opStatus status =
          value.convertToInteger(asInt, APFloat::rmTowardZero, &isExact);
      if (asInt.getBitWidth() <= 128 && status == APFloat::opOK && isExact) {
        kind = Kind::Int;
        intValue = IntValueTy(asInt);
        return;
      }
      APInt bits = value.bitcastToAPInt();
      const uint64_t *data = bits.getRawData();
      if (bits.getBitWidth() == 80) {
        kind = Kind::Float;
        floatValue = FloatValueTy{ data[0], data[1] };
      } else {
        assert(bits.getBitWidth() == 64);
        kind = Kind::Float;
        floatValue = FloatValueTy{ data[0], 0 };
      }
      return;
    }
    case ExprKind::StringLiteral:
      kind = Kind::String;
      stringValue = cast<StringLiteralExpr>(expr)->getValue();
      return;

    case ExprKind::BooleanLiteral:
      kind = Kind::Bool;
      boolValue = cast<BooleanLiteralExpr>(expr)->getValue();
      return;

    default:
      llvm_unreachable("not a valid literal expr for raw value");
    }
  }
  
  explicit RawValueKey(Kind k) : kind(k) {
    assert((k == Kind::Tombstone || k == Kind::Empty)
           && "this ctor is only for creating DenseMap special values");
  }
};
  
/// Used during enum raw value checking to identify the source of a raw value,
/// which may have been derived by auto-incrementing, for diagnostic purposes.
struct RawValueSource {
  /// The decl that has the raw value.
  EnumElementDecl *sourceElt;
  /// If the sourceDecl didn't explicitly name a raw value, this is the most
  /// recent preceding decl with an explicit raw value. This is used to
  /// diagnose 'autoincrementing from' messages.
  EnumElementDecl *lastExplicitValueElt;
};

} // end anonymous namespace

namespace llvm {

template<>
class DenseMapInfo<RawValueKey> {
public:
  static RawValueKey getEmptyKey() {
    return RawValueKey(RawValueKey::Kind::Empty);
  }
  static RawValueKey getTombstoneKey() {
    return RawValueKey(RawValueKey::Kind::Tombstone);
  }
  static unsigned getHashValue(RawValueKey k) {
    switch (k.kind) {
    case RawValueKey::Kind::Float:
      // Hash as bits. We want to treat distinct but IEEE-equal values as not
      // equal.
      return DenseMapInfo<uint64_t>::getHashValue(k.floatValue.v0) ^
             DenseMapInfo<uint64_t>::getHashValue(k.floatValue.v1);
    case RawValueKey::Kind::Int:
      return DenseMapInfo<uint64_t>::getHashValue(k.intValue.v0) &
             DenseMapInfo<uint64_t>::getHashValue(k.intValue.v1);
    case RawValueKey::Kind::String:
      return DenseMapInfo<StringRef>::getHashValue(k.stringValue);
    case RawValueKey::Kind::Bool:
      return DenseMapInfo<uint64_t>::getHashValue(k.boolValue);
    case RawValueKey::Kind::Empty:
    case RawValueKey::Kind::Tombstone:
      return 0;
    }

    llvm_unreachable("Unhandled RawValueKey in switch.");
  }
  static bool isEqual(RawValueKey a, RawValueKey b) {
    if (a.kind != b.kind)
      return false;
    switch (a.kind) {
    case RawValueKey::Kind::Float:
      // Hash as bits. We want to treat distinct but IEEE-equal values as not
      // equal.
      return a.floatValue.v0 == b.floatValue.v0 &&
             a.floatValue.v1 == b.floatValue.v1;
    case RawValueKey::Kind::Int:
      return a.intValue.v0 == b.intValue.v0 &&
             a.intValue.v1 == b.intValue.v1;
    case RawValueKey::Kind::String:
      return a.stringValue.equals(b.stringValue);
    case RawValueKey::Kind::Bool:
      return a.boolValue == b.boolValue;
    case RawValueKey::Kind::Empty:
    case RawValueKey::Kind::Tombstone:
      return true;
    }

    llvm_unreachable("Unhandled RawValueKey in switch.");
  }
};
  
} // namespace llvm

static bool canSkipCircularityCheck(NominalTypeDecl *decl) {
  // Don't bother checking imported or deserialized decls.
  return decl->hasClangNode() || decl->wasDeserialized();
}

bool
HasCircularInheritedProtocolsRequest::evaluate(Evaluator &evaluator,
                                               ProtocolDecl *decl) const {
  if (canSkipCircularityCheck(decl))
    return false;

  bool anyObject = false;
  auto inherited = getDirectlyInheritedNominalTypeDecls(decl, anyObject);
  for (auto &found : inherited) {
    auto *protoDecl = dyn_cast<ProtocolDecl>(found.Item);
    if (!protoDecl)
      continue;

    // If we have a cycle, handle it and return true.
    auto result = evaluator(HasCircularInheritedProtocolsRequest{protoDecl});
    if (!result) {
      using Error = CyclicalRequestError<HasCircularInheritedProtocolsRequest>;
      llvm::handleAllErrors(result.takeError(), [](const Error &E) {});
      return true;
    }

    // If the underlying request handled a cycle and returned true, bail.
    if (*result)
      return true;
  }
  return false;
}

bool
HasCircularRawValueRequest::evaluate(Evaluator &evaluator,
                                     EnumDecl *decl) const {
  if (canSkipCircularityCheck(decl) || !decl->hasRawType())
    return false;

  auto *inherited = decl->getRawType()->getEnumOrBoundGenericEnum();
  if (!inherited)
    return false;

  // If we have a cycle, handle it and return true.
  auto result = evaluator(HasCircularRawValueRequest{inherited});
  if (!result) {
    using Error = CyclicalRequestError<HasCircularRawValueRequest>;
    llvm::handleAllErrors(result.takeError(), [](const Error &E) {});
    return true;
  }
  return result.get();
}

namespace {
// The raw values of this enum must be kept in sync with
// diag::implicitly_final_cannot_be_open.
enum class ImplicitlyFinalReason : unsigned {
  /// A property was declared with 'let'.
  Let,
  /// The containing class is final.
  FinalClass,
  /// A member was declared as 'static'.
  Static
};
}

static bool inferFinalAndDiagnoseIfNeeded(ValueDecl *D, ClassDecl *cls,
                                          StaticSpellingKind staticSpelling) {
  // Are there any reasons to infer 'final'? Prefer 'static' over the class
  // being final for the purposes of diagnostics.
  Optional<ImplicitlyFinalReason> reason;
  if (staticSpelling == StaticSpellingKind::KeywordStatic) {
    reason = ImplicitlyFinalReason::Static;

    if (auto finalAttr = D->getAttrs().getAttribute<FinalAttr>()) {
      auto finalRange = finalAttr->getRange();
      if (finalRange.isValid()) {
        auto &context = D->getASTContext();
        context.Diags.diagnose(finalRange.Start, diag::static_decl_already_final)
        .fixItRemove(finalRange);
      }
    }
  } else if (cls->isFinal()) {
    reason = ImplicitlyFinalReason::FinalClass;
  }

  if (!reason)
    return false;

  if (D->getFormalAccess() == AccessLevel::Open) {
    auto &context = D->getASTContext();
    auto diagID = diag::implicitly_final_cannot_be_open;
    if (!context.isSwiftVersionAtLeast(5))
      diagID = diag::implicitly_final_cannot_be_open_swift4;
    auto inFlightDiag = context.Diags.diagnose(D, diagID,
                                    static_cast<unsigned>(reason.getValue()));
    fixItAccess(inFlightDiag, D, AccessLevel::Public);
  }

  return true;
}

/// Runtime-replacable accessors are dynamic when their storage declaration
/// is dynamic and they were explicitly defined or they are implicitly defined
/// getter/setter because no accessor was defined.
static bool doesAccessorNeedDynamicAttribute(AccessorDecl *accessor) {
  auto kind = accessor->getAccessorKind();
  auto storage = accessor->getStorage();
  bool isObjC = storage->isObjC();

  switch (kind) {
  case AccessorKind::Get: {
    auto readImpl = storage->getReadImpl();
    if (!isObjC &&
        (readImpl == ReadImplKind::Read || readImpl == ReadImplKind::Address))
      return false;
    return storage->isDynamic();
  }
  case AccessorKind::Set: {
    auto writeImpl = storage->getWriteImpl();
    if (!isObjC && (writeImpl == WriteImplKind::Modify ||
                    writeImpl == WriteImplKind::MutableAddress ||
                    writeImpl == WriteImplKind::StoredWithObservers))
      return false;
    return storage->isDynamic();
  }
  case AccessorKind::Read:
    if (!isObjC && storage->getReadImpl() == ReadImplKind::Read)
      return storage->isDynamic();
    return false;
  case AccessorKind::Modify: {
    if (!isObjC && storage->getWriteImpl() == WriteImplKind::Modify)
      return storage->isDynamic();
    return false;
  }
  case AccessorKind::MutableAddress: {
    if (!isObjC && storage->getWriteImpl() == WriteImplKind::MutableAddress)
      return storage->isDynamic();
    return false;
  }
  case AccessorKind::Address: {
    if (!isObjC && storage->getReadImpl() == ReadImplKind::Address)
      return storage->isDynamic();
    return false;
  }
  case AccessorKind::DidSet:
  case AccessorKind::WillSet:
    if (!isObjC &&
        storage->getWriteImpl() == WriteImplKind::StoredWithObservers)
      return storage->isDynamic();
    return false;
  }
  llvm_unreachable("covered switch");
}

CtorInitializerKind
InitKindRequest::evaluate(Evaluator &evaluator, ConstructorDecl *decl) const {
  auto &diags = decl->getASTContext().Diags;

  // Convenience inits are only allowed on classes and in extensions thereof.
  if (decl->getAttrs().hasAttribute<ConvenienceAttr>()) {
    if (auto nominal = decl->getDeclContext()->getSelfNominalTypeDecl()) {
      auto classDecl = dyn_cast<ClassDecl>(nominal);

      // Forbid convenience inits on Foreign CF types, as Swift does not yet
      // support user-defined factory inits.
      if (classDecl &&
          classDecl->getForeignClassKind() == ClassDecl::ForeignKind::CFType) {
        diags.diagnose(decl->getLoc(), diag::cfclass_convenience_init);
      }

      if (!classDecl) {
        auto ConvenienceLoc =
          decl->getAttrs().getAttribute<ConvenienceAttr>()->getLocation();

        // Produce a tailored diagnostic for structs and enums.
        bool isStruct = dyn_cast<StructDecl>(nominal) != nullptr;
        if (isStruct || dyn_cast<EnumDecl>(nominal)) {
          diags.diagnose(decl->getLoc(), diag::enumstruct_convenience_init,
                         isStruct ? "structs" : "enums")
            .fixItRemove(ConvenienceLoc);
        } else {
          diags.diagnose(decl->getLoc(), diag::nonclass_convenience_init,
                         nominal->getName())
            .fixItRemove(ConvenienceLoc);
        }
        return CtorInitializerKind::Designated;
      }
    }

    return CtorInitializerKind::Convenience;

  } else if (auto nominal = decl->getDeclContext()->getSelfNominalTypeDecl()) {
    // A designated init for a class must be written within the class itself.
    //
    // This is because designated initializers of classes get a vtable entry,
    // and extensions cannot add vtable entries to the extended type.
    //
    // If we implement the ability for extensions defined in the same module
    // (or the same file) to add vtable entries, we can re-evaluate this
    // restriction.
    if (isa<ClassDecl>(nominal) && !decl->isSynthesized() &&
        isa<ExtensionDecl>(decl->getDeclContext()) &&
        !(decl->getAttrs().hasAttribute<DynamicReplacementAttr>())) {
      if (cast<ClassDecl>(nominal)->getForeignClassKind() == ClassDecl::ForeignKind::CFType) {
        diags.diagnose(decl->getLoc(),
                       diag::cfclass_designated_init_in_extension,
                       nominal->getName());
        return CtorInitializerKind::Designated;
      } else {
        diags.diagnose(decl->getLoc(),
                       diag::designated_init_in_extension,
                       nominal->getName())
            .fixItInsert(decl->getLoc(), "convenience ");
        return CtorInitializerKind::Convenience;
      }
    }

    if (decl->getDeclContext()->getExtendedProtocolDecl()) {
      return CtorInitializerKind::Convenience;
    }
  }

  return CtorInitializerKind::Designated;
}

BodyInitKindAndExpr
BodyInitKindRequest::evaluate(Evaluator &evaluator,
                              ConstructorDecl *decl) const {

  struct FindReferenceToInitializer : ASTWalker {
    const ConstructorDecl *Decl;
    BodyInitKind Kind = BodyInitKind::None;
    ApplyExpr *InitExpr = nullptr;
    ASTContext &ctx;

    FindReferenceToInitializer(const ConstructorDecl *decl,
                               ASTContext &ctx)
        : Decl(decl), ctx(ctx) { }

    bool walkToDeclPre(class Decl *D) override {
      // Don't walk into further nominal decls.
      return !isa<NominalTypeDecl>(D);
    }
    
    std::pair<bool, Expr*> walkToExprPre(Expr *E) override {
      // Don't walk into closures.
      if (isa<ClosureExpr>(E))
        return { false, E };
      
      // Look for calls of a constructor on self or super.
      auto apply = dyn_cast<ApplyExpr>(E);
      if (!apply)
        return { true, E };

      auto Callee = apply->getSemanticFn();
      
      Expr *arg;

      if (isa<OtherConstructorDeclRefExpr>(Callee)) {
        arg = apply->getArg();
      } else if (auto *CRE = dyn_cast<ConstructorRefCallExpr>(Callee)) {
        arg = CRE->getArg();
      } else if (auto *dotExpr = dyn_cast<UnresolvedDotExpr>(Callee)) {
        if (dotExpr->getName().getBaseName() != DeclBaseName::createConstructor())
          return { true, E };

        arg = dotExpr->getBase();
      } else {
        // Not a constructor call.
        return { true, E };
      }

      // Look for a base of 'self' or 'super'.
      arg = arg->getSemanticsProvidingExpr();

      auto myKind = BodyInitKind::None;
      if (arg->isSuperExpr())
        myKind = BodyInitKind::Chained;
      else if (arg->isSelfExprOf(Decl, /*sameBase*/true))
        myKind = BodyInitKind::Delegating;
      else if (auto *declRef = dyn_cast<UnresolvedDeclRefExpr>(arg)) {
        // FIXME: We can see UnresolvedDeclRefExprs here because we have
        // not yet run preCheckExpression() on the entire function body
        // yet.
        //
        // We could consider pre-checking more eagerly.
        auto name = declRef->getName();
        auto loc = declRef->getLoc();
        if (name.isSimpleName(ctx.Id_self)) {
          auto *otherSelfDecl =
            ASTScope::lookupSingleLocalDecl(Decl->getParentSourceFile(),
                                            name.getFullName(), loc);
          if (otherSelfDecl == Decl->getImplicitSelfDecl())
            myKind = BodyInitKind::Delegating;
        }
      }
      
      if (myKind == BodyInitKind::None)
        return { true, E };

      if (Kind == BodyInitKind::None) {
        Kind = myKind;

        InitExpr = apply;
        return { true, E };
      }

      // If the kind changed, complain.
      if (Kind != myKind) {
        // The kind changed. Complain.
        ctx.Diags.diagnose(E->getLoc(), diag::init_delegates_and_chains);
        ctx.Diags.diagnose(InitExpr->getLoc(), diag::init_delegation_or_chain,
                           Kind == BodyInitKind::Chained);
      }

      return { true, E };
    }
  };

  auto &ctx = decl->getASTContext();
  FindReferenceToInitializer finder(decl, ctx);
  decl->getBody()->walk(finder);

  // get the kind out of the finder.
  auto Kind = finder.Kind;

  auto *NTD = decl->getDeclContext()->getSelfNominalTypeDecl();

  // Protocol extension and enum initializers are always delegating.
  if (Kind == BodyInitKind::None) {
    if (isa<ProtocolDecl>(NTD) || isa<EnumDecl>(NTD)) {
      Kind = BodyInitKind::Delegating;
    }
  }

  // Struct initializers that cannot see the layout of the struct type are
  // always delegating. This occurs if the struct type is not fixed layout,
  // and the constructor is either inlinable or defined in another module.
  if (Kind == BodyInitKind::None && isa<StructDecl>(NTD)) {
    // Note: This is specifically not using isFormallyResilient. We relax this
    // rule for structs in non-resilient modules so that they can have inlinable
    // constructors, as long as those constructors don't reference private
    // declarations.
    if (NTD->isResilient() &&
        decl->getResilienceExpansion() == ResilienceExpansion::Minimal) {
      Kind = BodyInitKind::Delegating;

    } else if (isa<ExtensionDecl>(decl->getDeclContext())) {
      // Prior to Swift 5, cross-module initializers were permitted to be
      // non-delegating. However, if the struct isn't fixed-layout, we have to
      // be delegating because, well, we don't know the layout.
      // A dynamic replacement is permitted to be non-delegating.
      if (NTD->isResilient() ||
          (ctx.isSwiftVersionAtLeast(5) &&
           !decl->getAttrs().getAttribute<DynamicReplacementAttr>())) {
        if (decl->getParentModule() != NTD->getParentModule())
          Kind = BodyInitKind::Delegating;
      }
    }
  }

  // If we didn't find any delegating or chained initializers, check whether
  // the initializer was explicitly marked 'convenience'.
  if (Kind == BodyInitKind::None &&
      decl->getAttrs().hasAttribute<ConvenienceAttr>())
    Kind = BodyInitKind::Delegating;

  // If we still don't know, check whether we have a class with a superclass: it
  // gets an implicit chained initializer.
  if (Kind == BodyInitKind::None) {
    if (auto classDecl = decl->getDeclContext()->getSelfClassDecl()) {
      if (classDecl->hasSuperclass())
        Kind = BodyInitKind::ImplicitChained;
    }
  }

  return BodyInitKindAndExpr(Kind, finder.InitExpr);
}

bool
ProtocolRequiresClassRequest::evaluate(Evaluator &evaluator,
                                       ProtocolDecl *decl) const {
  // Quick check: @objc protocols require a class.
  if (decl->isObjC())
    return true;

  // Determine the set of nominal types that this protocol inherits.
  bool anyObject = false;
  auto allInheritedNominals =
    getDirectlyInheritedNominalTypeDecls(decl, anyObject);

  // Quick check: do we inherit AnyObject?
  if (anyObject)
    return true;

  // Look through all of the inherited nominals for a superclass or a
  // class-bound protocol.
  for (const auto &found : allInheritedNominals) {
    // Superclass bound.
    if (isa<ClassDecl>(found.Item))
      return true;

    // A protocol that might be class-constrained.
    if (auto proto = dyn_cast<ProtocolDecl>(found.Item)) {
      if (proto->requiresClass())
        return true;
    }
  }

  return false;
}

bool
ExistentialConformsToSelfRequest::evaluate(Evaluator &evaluator,
                                           ProtocolDecl *decl) const {
  // If it's not @objc, it conforms to itself only if it has a self-conformance
  // witness table.
  if (!decl->isObjC())
    return decl->requiresSelfConformanceWitnessTable();

  // Check whether this protocol conforms to itself.
  for (auto member : decl->getMembers()) {
    if (member->isInvalid()) continue;

    if (auto vd = dyn_cast<ValueDecl>(member)) {
      // A protocol cannot conform to itself if it has static members.
      if (!vd->isInstanceMember())
        return false;
    }
  }

  // Check whether any of the inherited protocols fail to conform to themselves.
  for (auto proto : decl->getInheritedProtocols()) {
    if (!proto->existentialConformsToSelf())
      return false;
  }

  return true;
}

bool
ExistentialTypeSupportedRequest::evaluate(Evaluator &evaluator,
                                          ProtocolDecl *decl) const {
  // ObjC protocols can always be existential.
  if (decl->isObjC())
    return true;

  for (auto member : decl->getMembers()) {
    // Existential types cannot be used if the protocol has an associated type.
    if (isa<AssociatedTypeDecl>(member))
      return false;

    // For value members, look at their type signatures.
    if (auto valueMember = dyn_cast<ValueDecl>(member)) {
      if (!decl->getASTContext().isAvailableOnExistential(
              decl->getDeclaredInterfaceType(), valueMember))
        return false;
    }
  }

  // Check whether all of the inherited protocols support existential types.
  for (auto proto : decl->getInheritedProtocols()) {
    if (!proto->existentialTypeSupported())
      return false;
  }

  return true;
}

bool
IsFinalRequest::evaluate(Evaluator &evaluator, ValueDecl *decl) const {
  if (isa<ClassDecl>(decl))
    return decl->getAttrs().hasAttribute<FinalAttr>();

  auto cls = decl->getDeclContext()->getSelfClassDecl();
  if (!cls)
    return false;

  switch (decl->getKind()) {
    case DeclKind::Var: {
      // Properties are final if they are declared 'static' or a 'let'
      auto *VD = cast<VarDecl>(decl);

      // Backing storage for 'lazy' or property wrappers is always final.
      if (VD->isLazyStorageProperty() ||
          VD->getOriginalWrappedProperty(PropertyWrapperSynthesizedPropertyKind::Backing))
        return true;

      // Property wrapper storage wrappers are final if the original property
      // is final.
      if (auto *original = VD->getOriginalWrappedProperty(
            PropertyWrapperSynthesizedPropertyKind::Projection)) {
        if (original->isFinal())
          return true;
      }

      if (VD->getDeclContext()->getSelfClassDecl()) {
        // If this variable is a class member, mark it final if the
        // class is final, or if it was declared with 'let'.
        auto *PBD = VD->getParentPatternBinding();
        if (PBD && inferFinalAndDiagnoseIfNeeded(decl, cls, PBD->getStaticSpelling()))
          return true;

        if (VD->isLet()) {
          if (VD->getFormalAccess() == AccessLevel::Open) {
            auto &context = decl->getASTContext();
            auto diagID = diag::implicitly_final_cannot_be_open;
            if (!context.isSwiftVersionAtLeast(5))
              diagID = diag::implicitly_final_cannot_be_open_swift4;
            auto inFlightDiag =
              context.Diags.diagnose(decl, diagID,
                                     static_cast<unsigned>(ImplicitlyFinalReason::Let));
            fixItAccess(inFlightDiag, decl, AccessLevel::Public);
          }

          return true;
        }
      }

      break;
    }

    case DeclKind::Func: {
      // Methods declared 'static' are final.
      auto staticSpelling = cast<FuncDecl>(decl)->getStaticSpelling();
      if (inferFinalAndDiagnoseIfNeeded(decl, cls, staticSpelling))
        return true;
      break;
    }

    case DeclKind::Accessor:
      if (auto accessor = dyn_cast<AccessorDecl>(decl)) {
        switch (accessor->getAccessorKind()) {
          case AccessorKind::DidSet:
          case AccessorKind::WillSet:
            // Observing accessors are marked final if in a class.
            return true;

          case AccessorKind::Read:
          case AccessorKind::Modify:
          case AccessorKind::Get:
          case AccessorKind::Set: {
            // Coroutines and accessors are final if their storage is.
            auto storage = accessor->getStorage();
            if (storage->isFinal())
              return true;
            break;
          }

          default:
            break;
        }
      }
      break;

    case DeclKind::Subscript: {
      // Member subscripts.
      auto staticSpelling = cast<SubscriptDecl>(decl)->getStaticSpelling();
      if (inferFinalAndDiagnoseIfNeeded(decl, cls, staticSpelling))
        return true;
      break;
    }

    default:
      break;
  }

  if (decl->getAttrs().hasAttribute<FinalAttr>())
    return true;

  return false;
}

bool
IsStaticRequest::evaluate(Evaluator &evaluator, FuncDecl *decl) const {
  if (auto *accessor = dyn_cast<AccessorDecl>(decl))
    return accessor->getStorage()->isStatic();

  bool result = (decl->getStaticLoc().isValid() ||
                 decl->getStaticSpelling() != StaticSpellingKind::None);
  auto *dc = decl->getDeclContext();
  if (!result &&
      decl->isOperator() &&
      dc->isTypeContext()) {
    const auto operatorName = decl->getBaseIdentifier();
    if (auto ED = dyn_cast<ExtensionDecl>(dc->getAsDecl())) {
      decl->diagnose(diag::nonstatic_operator_in_extension, operatorName,
                     ED->getExtendedTypeRepr() != nullptr,
                     ED->getExtendedTypeRepr())
          .fixItInsert(decl->getAttributeInsertionLoc(/*forModifier=*/true),
                       "static ");
    } else {
      auto *NTD = cast<NominalTypeDecl>(dc->getAsDecl());
      decl->diagnose(diag::nonstatic_operator_in_nominal, operatorName,
                     NTD->getName())
          .fixItInsert(decl->getAttributeInsertionLoc(/*forModifier=*/true),
                       "static ");
    }
    result = true;
  }

  return result;
}

bool
IsDynamicRequest::evaluate(Evaluator &evaluator, ValueDecl *decl) const {
  // If we can't infer dynamic here, don't.
  if (!DeclAttribute::canAttributeAppearOnDecl(DAK_Dynamic, decl))
    return false;

  // Add dynamic if -enable-implicit-dynamic was requested.
  TypeChecker::addImplicitDynamicAttribute(decl);

  // If 'dynamic' was explicitly specified, check it.
  if (decl->getAttrs().hasAttribute<DynamicAttr>()) {
    return true;
  }

  if (auto accessor = dyn_cast<AccessorDecl>(decl)) {
    // Runtime-replacable accessors are dynamic when their storage declaration
    // is dynamic and they were explicitly defined or they are implicitly defined
    // getter/setter because no accessor was defined.
    return doesAccessorNeedDynamicAttribute(accessor);
  }

  // The 'NSManaged' attribute implies 'dynamic'.
  // FIXME: Use a semantic check for NSManaged rather than looking for the
  // attribute (which could be ill-formed).
  if (decl->getAttrs().hasAttribute<NSManagedAttr>())
    return true;

  // The presence of 'final' blocks the inference of 'dynamic'.
  if (decl->isFinal())
    return false;

  // Types are never 'dynamic'.
  if (isa<TypeDecl>(decl))
    return false;

  // A non-@objc entity is never 'dynamic'.
  if (!decl->isObjC())
    return false;

  // @objc declarations in class extensions are implicitly dynamic.
  // This is intended to enable overriding the declarations.
  auto dc = decl->getDeclContext();
  if (isa<ExtensionDecl>(dc) && dc->getSelfClassDecl())
    return true;

  // If any of the declarations overridden by this declaration are dynamic
  // or were imported from Objective-C, this declaration is dynamic.
  // Don't do this if the declaration is not exposed to Objective-C; that's
  // currently the (only) manner in which one can make an override of a
  // dynamic declaration non-dynamic.
  auto overriddenDecls = evaluateOrDefault(evaluator,
    OverriddenDeclsRequest{decl}, {});
  for (auto overridden : overriddenDecls) {
    if (overridden->isDynamic() || overridden->hasClangNode())
      return true;
  }

  return false;
}

ArrayRef<Requirement>
RequirementSignatureRequest::evaluate(Evaluator &evaluator,
                                      ProtocolDecl *proto) const {
  ASTContext &ctx = proto->getASTContext();

  // First check if we have a deserializable requirement signature.
  if (proto->hasLazyRequirementSignature()) {
    ++NumLazyRequirementSignaturesLoaded;
    // FIXME: (transitional) increment the redundant "always-on" counter.
    if (ctx.Stats)
      ++ctx.Stats->getFrontendCounters().NumLazyRequirementSignaturesLoaded;

    auto contextData = static_cast<LazyProtocolData *>(
        ctx.getOrCreateLazyContextData(proto, nullptr));

    SmallVector<Requirement, 8> requirements;
    contextData->loader->loadRequirementSignature(
        proto, contextData->requirementSignatureData, requirements);
    if (requirements.empty())
      return None;
    return ctx.AllocateCopy(requirements);
  }

  GenericSignatureBuilder builder(proto->getASTContext());

  // Add all of the generic parameters.
  for (auto gp : *proto->getGenericParams())
    builder.addGenericParameter(gp);

  // Add the conformance of 'self' to the protocol.
  auto selfType =
    proto->getSelfInterfaceType()->castTo<GenericTypeParamType>();
  auto requirement =
    Requirement(RequirementKind::Conformance, selfType,
              proto->getDeclaredInterfaceType());

  builder.addRequirement(
          requirement,
          GenericSignatureBuilder::RequirementSource::forRequirementSignature(
                                                      builder, selfType, proto),
          nullptr);

  auto reqSignature = std::move(builder).computeGenericSignature(
                        SourceLoc(),
                        /*allowConcreteGenericPArams=*/false,
                        /*allowBuilderToMove=*/false);
  return reqSignature->getRequirements();
}

Type
DefaultDefinitionTypeRequest::evaluate(Evaluator &evaluator,
                                       AssociatedTypeDecl *assocType) const {
  if (assocType->Resolver) {
    auto defaultType = assocType->Resolver->loadAssociatedTypeDefault(
                                    assocType, assocType->ResolverContextData);
    assocType->Resolver = nullptr;
    return defaultType;
  }

  TypeRepr *defaultDefinition = assocType->getDefaultDefinitionTypeRepr();
  if (defaultDefinition) {
    return TypeResolution::forInterface(assocType->getDeclContext(), None,
                                        // Diagnose unbound generics.
                                        /*unboundTyOpener*/ nullptr)
        .resolveType(defaultDefinition);
  }

  return Type();
}

bool
NeedsNewVTableEntryRequest::evaluate(Evaluator &evaluator,
                                     AbstractFunctionDecl *decl) const {
  auto *dc = decl->getDeclContext();
  if (!isa<ClassDecl>(dc))
    return true;

  // Destructors always use a fixed vtable entry.
  if (isa<DestructorDecl>(decl))
    return false;
  
  assert(isa<FuncDecl>(decl) || isa<ConstructorDecl>(decl));

  // Final members are always be called directly.
  // Dynamic methods are always accessed by objc_msgSend().
  if (decl->isFinal() || decl->shouldUseObjCDispatch() || decl->hasClangNode())
    return false;

  auto &ctx = dc->getASTContext();

  // Initializers are not normally inherited, but required initializers can
  // be overridden for invocation from dynamic types, and convenience initializers
  // are conditionally inherited when all designated initializers are available,
  // working by dynamically invoking the designated initializer implementation
  // from the subclass. Convenience initializers can also override designated
  // initializer implementations from their superclass.
  if (auto ctor = dyn_cast<ConstructorDecl>(decl)) {
    if (!ctor->isRequired() && !ctor->isDesignatedInit()) {
      return false;
    }

    // Stub constructors don't appear in the vtable.
    if (ctor->hasStubImplementation())
      return false;
  }

  if (auto *accessor = dyn_cast<AccessorDecl>(decl)) {
    // Check to see if it's one of the opaque accessors for the declaration.
    auto storage = accessor->getStorage();
    if (!storage->requiresOpaqueAccessor(accessor->getAccessorKind()))
      return false;
  }

  auto base = decl->getOverriddenDecl();

  if (!base || base->hasClangNode() || base->shouldUseObjCDispatch())
    return true;

  // As above, convenience initializers are not formally overridable in Swift
  // vtables, although same-named initializers are modeled as overriding for
  // various QoI and objc interop reasons. Even if we "override" a non-required
  // convenience init, we still need a distinct vtable entry.
  if (auto baseCtor = dyn_cast<ConstructorDecl>(base)) {
    if (!baseCtor->isRequired() && !baseCtor->isDesignatedInit()) {
      return true;
    }
  }

  // If the base is less visible than the override, we might need a vtable
  // entry since callers of the override might not be able to see the base
  // at all.
  if (decl->isEffectiveLinkageMoreVisibleThan(base))
    return true;

  using Direction = ASTContext::OverrideGenericSignatureReqCheck;
  if (!ctx.overrideGenericSignatureReqsSatisfied(
          base, decl, Direction::BaseReqSatisfiedByDerived)) {
    return true;
  }

  // If this method is an ABI compatible override, then we don't need a new
  // vtable entry. Otherwise, if it's not ABI compatible, for example if the
  // base has a more general AST type, then we need a new entry. Note that an
  // abstraction change is OK; we don't want to add a whole new vtable entry
  // just because an @in parameter becomes @owned, or whatever.
  auto isABICompatibleOverride =
      evaluateOrDefault(evaluator, IsABICompatibleOverrideRequest{decl}, false);
  return !isABICompatibleOverride;
}

/// Given the raw value literal expression for an enum case, produces the
/// auto-incremented raw value for the subsequent case, or returns null if
/// the value is not auto-incrementable.
static LiteralExpr *getAutomaticRawValueExpr(AutomaticEnumValueKind valueKind,
                                             EnumElementDecl *forElt,
                                             LiteralExpr *prevValue) {
  auto &Ctx = forElt->getASTContext();
  switch (valueKind) {
  case AutomaticEnumValueKind::None:
    Ctx.Diags.diagnose(forElt->getLoc(),
                       diag::enum_non_integer_convertible_raw_type_no_value);
    return nullptr;

  case AutomaticEnumValueKind::String:
    return new (Ctx) StringLiteralExpr(forElt->getNameStr(), SourceLoc(),
                                              /*Implicit=*/true);

  case AutomaticEnumValueKind::Integer:
    // If there was no previous value, start from zero.
    if (!prevValue) {
      return new (Ctx) IntegerLiteralExpr("0", SourceLoc(),
                                                 /*Implicit=*/true);
    }

    if (auto intLit = dyn_cast<IntegerLiteralExpr>(prevValue)) {
      APInt nextVal = intLit->getRawValue().sextOrSelf(128) + 1;
      bool negative = nextVal.slt(0);
      if (negative)
        nextVal = -nextVal;

      llvm::SmallString<10> nextValStr;
      nextVal.toStringSigned(nextValStr);
      auto expr = new (Ctx)
        IntegerLiteralExpr(Ctx.AllocateCopy(StringRef(nextValStr)),
                           forElt->getLoc(), /*Implicit=*/true);
      if (negative)
        expr->setNegative(forElt->getLoc());

      return expr;
    }

    Ctx.Diags.diagnose(forElt->getLoc(),
                       diag::enum_non_integer_raw_value_auto_increment);
    return nullptr;
  }

  llvm_unreachable("Unhandled AutomaticEnumValueKind in switch.");
}

Optional<AutomaticEnumValueKind>
swift::computeAutomaticEnumValueKind(EnumDecl *ED) {
  Type rawTy = ED->getRawType();
  assert(rawTy && "Cannot compute value kind without raw type!");
  
  if (ED->getGenericEnvironmentOfContext() != nullptr)
    rawTy = ED->mapTypeIntoContext(rawTy);
  
  // Swift enums require that the raw type is convertible from one of the
  // primitive literal protocols.
  auto conformsToProtocol = [&](KnownProtocolKind protoKind) {
    ProtocolDecl *proto = ED->getASTContext().getProtocol(protoKind);
    return TypeChecker::conformsToProtocol(rawTy, proto, ED->getDeclContext());
  };

  static auto otherLiteralProtocolKinds = {
    KnownProtocolKind::ExpressibleByFloatLiteral,
    KnownProtocolKind::ExpressibleByUnicodeScalarLiteral,
    KnownProtocolKind::ExpressibleByExtendedGraphemeClusterLiteral,
  };
  
  if (conformsToProtocol(KnownProtocolKind::ExpressibleByIntegerLiteral)) {
    return AutomaticEnumValueKind::Integer;
  } else if (conformsToProtocol(KnownProtocolKind::ExpressibleByStringLiteral)){
    return AutomaticEnumValueKind::String;
  } else if (std::any_of(otherLiteralProtocolKinds.begin(),
                         otherLiteralProtocolKinds.end(),
                         conformsToProtocol)) {
    return AutomaticEnumValueKind::None;
  } else {
    return None;
  }
}

evaluator::SideEffect
EnumRawValuesRequest::evaluate(Evaluator &eval, EnumDecl *ED,
                               TypeResolutionStage stage) const {
  Type rawTy = ED->getRawType();
  if (!rawTy) {
    return std::make_tuple<>();
  }

  if (!computeAutomaticEnumValueKind(ED)) {
    return std::make_tuple<>();
  }

  if (ED->getGenericEnvironmentOfContext() != nullptr)
    rawTy = ED->mapTypeIntoContext(rawTy);
  if (rawTy->hasError())
    return std::make_tuple<>();

  // Check the raw values of the cases.
  LiteralExpr *prevValue = nullptr;
  EnumElementDecl *lastExplicitValueElt = nullptr;

  // Keep a map we can use to check for duplicate case values.
  llvm::SmallDenseMap<RawValueKey, RawValueSource, 8> uniqueRawValues;

  // Make the raw member accesses explicit.
  auto uncheckedRawValueOf = [](EnumElementDecl *EED) -> LiteralExpr * {
    return EED->RawValueExpr;
  };
  
  Optional<AutomaticEnumValueKind> valueKind;
  for (auto elt : ED->getAllElements()) {
    // If the element has been diagnosed up to now, skip it.
    if (elt->isInvalid())
      continue;

    if (uncheckedRawValueOf(elt)) {
      if (!uncheckedRawValueOf(elt)->isImplicit())
        lastExplicitValueElt = elt;
    } else if (!ED->SemanticFlags.contains(EnumDecl::HasFixedRawValues)) {
      // Try to pull out the automatic enum value kind.  If that fails, bail.
      if (!valueKind) {
        valueKind = computeAutomaticEnumValueKind(ED);
        if (!valueKind) {
          elt->setInvalid();
          return std::make_tuple<>();
        }
      }
      
      // If the enum element has no explicit raw value, try to
      // autoincrement from the previous value, or start from zero if this
      // is the first element.
      auto nextValue = getAutomaticRawValueExpr(*valueKind, elt, prevValue);
      if (!nextValue) {
        elt->setInvalid();
        break;
      }
      elt->setRawValueExpr(nextValue);
    }
    prevValue = uncheckedRawValueOf(elt);
    assert(prevValue && "continued without setting raw value of enum case");

    switch (stage) {
    case TypeResolutionStage::Structural:
      // We're only interested in computing the complete set of raw values,
      // so we can skip type checking.
      continue;
    default:
      // Continue on to type check the raw value.
      break;
    }

    
    {
      Expr *exprToCheck = prevValue;
      if (TypeChecker::typeCheckExpression(
              exprToCheck, ED,
              /*contextualInfo=*/{rawTy, CTP_EnumCaseRawValue})) {
        checkEnumElementActorIsolation(elt, exprToCheck);
        TypeChecker::checkEnumElementEffects(elt, exprToCheck);
      }
    }

    // If we didn't find a valid initializer (maybe the initial value was
    // incompatible with the raw value type) mark the entry as being erroneous.
    if (!prevValue->getType() || prevValue->getType()->hasError()) {
      elt->setInvalid();
      continue;
    }


    // If the raw values of the enum case are fixed, then we trust our callers
    // to have set things up correctly.  This comes up with imported enums
    // and deserialized @objc enums which always have their raw values setup
    // beforehand.
    if (ED->SemanticFlags.contains(EnumDecl::HasFixedRawValues))
      continue;

    // Using magic literals like #file as raw value is not supported right now.
    // TODO: We could potentially support #file, #function, #line and #column.
    auto &Diags = ED->getASTContext().Diags;
    SourceLoc diagLoc = uncheckedRawValueOf(elt)->isImplicit()
                            ? elt->getLoc()
                            : uncheckedRawValueOf(elt)->getLoc();
    if (auto magicLiteralExpr =
            dyn_cast<MagicIdentifierLiteralExpr>(prevValue)) {
      auto kindString =
          magicLiteralExpr->getKindString(magicLiteralExpr->getKind());
      Diags.diagnose(diagLoc, diag::enum_raw_value_magic_literal, kindString);
      elt->setInvalid();
      continue;
    }

    // Check that the raw value is unique.
    RawValueKey key{prevValue};
    RawValueSource source{elt, lastExplicitValueElt};

    auto insertIterPair = uniqueRawValues.insert({key, source});
    if (insertIterPair.second)
      continue;

    // Diagnose the duplicate value.
    Diags.diagnose(diagLoc, diag::enum_raw_value_not_unique);
    assert(lastExplicitValueElt &&
           "should not be able to have non-unique raw values when "
           "relying on autoincrement");
    if (lastExplicitValueElt != elt &&
        valueKind == AutomaticEnumValueKind::Integer) {
      Diags.diagnose(uncheckedRawValueOf(lastExplicitValueElt)->getLoc(),
                     diag::enum_raw_value_incrementing_from_here);
    }

    RawValueSource prevSource = insertIterPair.first->second;
    auto foundElt = prevSource.sourceElt;
    diagLoc = uncheckedRawValueOf(foundElt)->isImplicit()
        ? foundElt->getLoc() : uncheckedRawValueOf(foundElt)->getLoc();
    Diags.diagnose(diagLoc, diag::enum_raw_value_used_here);
    if (foundElt != prevSource.lastExplicitValueElt &&
        valueKind == AutomaticEnumValueKind::Integer) {
      if (prevSource.lastExplicitValueElt)
        Diags.diagnose(uncheckedRawValueOf(prevSource.lastExplicitValueElt)
                         ->getLoc(),
                       diag::enum_raw_value_incrementing_from_here);
      else
        Diags.diagnose(ED->getAllElements().front()->getLoc(),
                       diag::enum_raw_value_incrementing_from_zero);
    }
  }
  return std::make_tuple<>();
}

const ConstructorDecl *
swift::findNonImplicitRequiredInit(const ConstructorDecl *CD) {
  while (CD->isImplicit()) {
    auto *overridden = CD->getOverriddenDecl();
    if (!overridden || !overridden->isRequired())
      break;
    CD = overridden;
  }
  return CD;
}

/// For building the higher-than component of the diagnostic path,
/// we use the visited set, which we've embellished with information
/// about how we reached a particular node.  This is reasonable because
/// we need to maintain the set anyway.
static void buildHigherThanPath(
    PrecedenceGroupDecl *last,
    const llvm::DenseMap<PrecedenceGroupDecl *, PrecedenceGroupDecl *>
        &visitedFrom,
    raw_ostream &out) {
  auto it = visitedFrom.find(last);
  assert(it != visitedFrom.end());
  auto from = it->second;
  if (from) {
    buildHigherThanPath(from, visitedFrom, out);
  }
  out << last->getName() << " -> ";
}

/// For building the lower-than component of the diagnostic path,
/// we just do a depth-first search to find a path.
static bool buildLowerThanPath(PrecedenceGroupDecl *start,
                               PrecedenceGroupDecl *target, raw_ostream &out) {
  if (start == target) {
    out << start->getName();
    return true;
  }

  if (start->isInvalid())
    return false;

  for (auto &rel : start->getLowerThan()) {
    if (rel.Group && buildLowerThanPath(rel.Group, target, out)) {
      out << " -> " << start->getName();
      return true;
    }
  }

  return false;
}

static void checkPrecedenceCircularity(DiagnosticEngine &D,
                                       PrecedenceGroupDecl *PGD) {
  // Don't diagnose if this group is already marked invalid.
  if (PGD->isInvalid())
    return;

  // The cycle doesn't necessarily go through this specific group,
  // so we need a proper visited set to avoid infinite loops.  We
  // also record a back-reference so that we can easily reconstruct
  // the cycle.
  llvm::DenseMap<PrecedenceGroupDecl *, PrecedenceGroupDecl *> visitedFrom;
  SmallVector<PrecedenceGroupDecl *, 4> stack;

  // Fill out the targets set.
  llvm::SmallPtrSet<PrecedenceGroupDecl *, 4> targets;
  stack.push_back(PGD);
  do {
    auto cur = stack.pop_back_val();

    // If we reach an invalid node, just bail out.
    if (cur->isInvalid()) {
      PGD->setInvalid();
      return;
    }

    targets.insert(cur);

    for (auto &rel : cur->getLowerThan()) {
      if (!rel.Group)
        continue;

      // We can't have cycles in the lower-than relationship
      // because it has to point outside of the module.

      stack.push_back(rel.Group);
    }
  } while (!stack.empty());

  // Make sure that the PGD is its own source.
  visitedFrom.insert({PGD, nullptr});

  stack.push_back(PGD);
  do {
    auto cur = stack.pop_back_val();

    // If we reach an invalid node, just bail out.
    if (cur->isInvalid()) {
      PGD->setInvalid();
      return;
    }

    for (auto &rel : cur->getHigherThan()) {
      if (!rel.Group)
        continue;

      // Check whether we've reached a target declaration.
      if (!targets.count(rel.Group)) {
        // If not, check whether we've visited this group before.
        if (visitedFrom.insert({rel.Group, cur}).second) {
          // If not, add it to the queue.
          stack.push_back(rel.Group);
        }

        // Note that we'll silently ignore cycles that don't go through PGD.
        // We should eventually process the groups that are involved.
        continue;
      }

      // Otherwise, we have something to report.
      SmallString<128> path;
      {
        llvm::raw_svector_ostream str(path);

        // Build the higherThan portion of the path (PGD -> cur).
        buildHigherThanPath(cur, visitedFrom, str);

        // Build the lowerThan portion of the path (rel.Group -> PGD).
        buildLowerThanPath(PGD, rel.Group, str);
      }

      D.diagnose(PGD->getHigherThanLoc(),
                 diag::higher_than_precedence_group_cycle, path);
      PGD->setInvalid();
      return;
    }
  } while (!stack.empty());
}

static PrecedenceGroupDecl *lookupPrecedenceGroupForRelation(
    DeclContext *dc, PrecedenceGroupDecl::Relation rel,
    PrecedenceGroupDescriptor::PathDirection direction) {
  auto &ctx = dc->getASTContext();
  PrecedenceGroupDescriptor desc{dc, rel.Name, rel.NameLoc, direction};
  auto result = ctx.evaluator(ValidatePrecedenceGroupRequest{desc});
  if (!result) {
    // Handle a cycle error specially. We don't want to default to an empty
    // result, as we don't want to emit an error about not finding a precedence
    // group.
    using Error = CyclicalRequestError<ValidatePrecedenceGroupRequest>;
    llvm::handleAllErrors(result.takeError(), [](const Error &E) {});
    return nullptr;
  }
  return PrecedenceGroupLookupResult(dc, rel.Name, std::move(*result))
      .getSingleOrDiagnose(rel.NameLoc);
}

void swift::validatePrecedenceGroup(PrecedenceGroupDecl *PGD) {
  assert(PGD && "Cannot validate a null precedence group!");
  if (PGD->isInvalid())
    return;

  auto &Diags = PGD->getASTContext().Diags;
  auto *dc = PGD->getDeclContext();

  // Validate the higherThan relationships.
  bool addedHigherThan = false;
  for (auto &rel : PGD->getMutableHigherThan()) {
    if (rel.Group)
      continue;

    // TODO: Requestify the lookup of a relation's group.
    rel.Group = lookupPrecedenceGroupForRelation(
        dc, rel, PrecedenceGroupDescriptor::HigherThan);
    if (rel.Group) {
      addedHigherThan = true;
    } else {
      PGD->setInvalid();
    }
  }

  // Validate the lowerThan relationships.
  for (auto &rel : PGD->getMutableLowerThan()) {
    if (rel.Group)
      continue;

    auto *group = lookupPrecedenceGroupForRelation(
        dc, rel, PrecedenceGroupDescriptor::LowerThan);
    rel.Group = group;

    // If we didn't find anything, try doing a raw lookup for the group before
    // diagnosing the 'lowerThan' within the same-module restriction. This can
    // allow us to diagnose even if we have a precedence group cycle.
    if (!group)
      group = dc->lookupPrecedenceGroup(rel.Name).getSingle();

    if (group &&
        group->getDeclContext()->getParentModule() == dc->getParentModule()) {
      if (!PGD->isInvalid()) {
        Diags.diagnose(rel.NameLoc, diag::precedence_group_lower_within_module);
        Diags.diagnose(group->getNameLoc(), diag::kind_declared_here,
                       DescriptiveDeclKind::PrecedenceGroup);
      }
      PGD->setInvalid();
    }

    if (!rel.Group)
      PGD->setInvalid();
  }

  // Try to diagnose trickier cycles that request evaluation alone can't catch.
  if (addedHigherThan)
    checkPrecedenceCircularity(Diags, PGD);
}

TinyPtrVector<PrecedenceGroupDecl *> ValidatePrecedenceGroupRequest::evaluate(
    Evaluator &eval, PrecedenceGroupDescriptor descriptor) const {
  auto groups = descriptor.dc->lookupPrecedenceGroup(descriptor.ident);
  for (auto *group : groups)
    validatePrecedenceGroup(group);

  // Return the raw results vector, which will get wrapped back in a
  // PrecedenceGroupLookupResult by the TypeChecker entry point. This dance
  // avoids unnecessarily caching the name and context for the lookup.
  return std::move(groups).get();
}

PrecedenceGroupLookupResult
TypeChecker::lookupPrecedenceGroup(DeclContext *dc, Identifier name,
                                   SourceLoc nameLoc) {
  auto groups = evaluateOrDefault(
      dc->getASTContext().evaluator,
      ValidatePrecedenceGroupRequest({dc, name, nameLoc, None}), {});
  return PrecedenceGroupLookupResult(dc, name, std::move(groups));
}

static NominalTypeDecl *resolveSingleNominalTypeDecl(
    DeclContext *DC, SourceLoc loc, Identifier ident, ASTContext &Ctx,
    TypeResolutionFlags flags = TypeResolutionFlags(0)) {
  auto *TyR = new (Ctx) SimpleIdentTypeRepr(DeclNameLoc(loc),
                                            DeclNameRef(ident));

  const auto options =
      TypeResolutionOptions(TypeResolverContext::TypeAliasDecl) | flags;

  const auto result =
      TypeResolution::forInterface(DC, options,
                                   // FIXME: Should unbound generics be allowed
                                   // to appear amongst designated types?
                                   /*unboundTyOpener*/ nullptr)
          .resolveType(TyR);

  if (result->hasError())
    return nullptr;
  return result->getAnyNominal();
}

bool swift::checkDesignatedTypes(OperatorDecl *OD,
                                 ArrayRef<Located<Identifier>> identifiers) {
  auto &ctx = OD->getASTContext();
  auto *DC = OD->getDeclContext();

  SmallVector<NominalTypeDecl *, 1> designatedNominalTypes;
  for (auto ident : identifiers) {
    auto *decl = resolveSingleNominalTypeDecl(DC, ident.Loc, ident.Item, ctx);
    if (!decl)
      return true;

    designatedNominalTypes.push_back(decl);
  }

  OD->setDesignatedNominalTypes(ctx.AllocateCopy(designatedNominalTypes));
  return false;
}

/// Validate the given operator declaration.
///
/// This establishes key invariants, such as an InfixOperatorDecl's
/// reference to its precedence group and the transitive validity of that
/// group.
PrecedenceGroupDecl *
OperatorPrecedenceGroupRequest::evaluate(Evaluator &evaluator,
                                         InfixOperatorDecl *IOD) const {
  auto &ctx = IOD->getASTContext();
  auto *dc = IOD->getDeclContext();

  auto enableOperatorDesignatedTypes =
      ctx.TypeCheckerOpts.EnableOperatorDesignatedTypes;

  PrecedenceGroupDecl *group = nullptr;

  auto identifiers = IOD->getIdentifiers();
  if (!identifiers.empty()) {
    auto name = identifiers[0].Item;
    auto loc = identifiers[0].Loc;

    auto canResolveType = [&]() -> bool {
      return enableOperatorDesignatedTypes &&
             resolveSingleNominalTypeDecl(dc, loc, name, ctx,
                                          TypeResolutionFlags::SilenceErrors);
    };

    // Make sure not to diagnose in the case where we failed to find a
    // precedencegroup, but we could resolve a type instead.
    auto groups = TypeChecker::lookupPrecedenceGroup(dc, name, loc);
    if (groups.hasResults() || !canResolveType()) {
      group = groups.getSingleOrDiagnose(loc);
      identifiers = identifiers.slice(1);
    }
  }

  // Unless operator designed types are enabled, the parser will ensure that
  // only one identifier is allowed in the clause, which we should have just
  // handled.
  assert(identifiers.empty() || enableOperatorDesignatedTypes);

  if (!group) {
    auto groups = TypeChecker::lookupPrecedenceGroup(
        dc, ctx.Id_DefaultPrecedence, SourceLoc());
    group = groups.getSingleOrDiagnose(IOD->getLoc(), /*forBuiltin*/ true);
  }

  auto nominalTypes = IOD->getDesignatedNominalTypes();
  if (nominalTypes.empty() && enableOperatorDesignatedTypes) {
    if (checkDesignatedTypes(IOD, identifiers)) {
      IOD->setInvalid();
    }
  }
  return group;
}

SelfAccessKind
SelfAccessKindRequest::evaluate(Evaluator &evaluator, FuncDecl *FD) const {
  if (FD->getAttrs().getAttribute<MutatingAttr>(true)) {
    if (!FD->isInstanceMember() || !FD->getDeclContext()->hasValueSemantics()) {
      // If this decl is on a class-constrained protocol extension, then
      // respect the explicit mutatingness. Otherwise, we would throw an
      // error.
      if (FD->getDeclContext()->isClassConstrainedProtocolExtension())
        return SelfAccessKind::Mutating;
      return SelfAccessKind::NonMutating;
    }
    return SelfAccessKind::Mutating;
  } else if (FD->getAttrs().hasAttribute<NonMutatingAttr>()) {
    return SelfAccessKind::NonMutating;
  } else if (FD->getAttrs().hasAttribute<ConsumingAttr>()) {
    return SelfAccessKind::Consuming;
  }

  if (auto *AD = dyn_cast<AccessorDecl>(FD)) {
    // Non-static set/willSet/didSet/mutableAddress default to mutating.
    // get/address default to non-mutating.
    switch (AD->getAccessorKind()) {
    case AccessorKind::Address:
    case AccessorKind::Get:
    case AccessorKind::Read:
      break;

    case AccessorKind::MutableAddress:
    case AccessorKind::Set:
    case AccessorKind::Modify:
      if (AD->isInstanceMember() && AD->getDeclContext()->hasValueSemantics())
        return SelfAccessKind::Mutating;
      break;

    case AccessorKind::WillSet:
    case AccessorKind::DidSet: {
      auto *storage =AD->getStorage();
      if (storage->isSetterMutating())
        return SelfAccessKind::Mutating;

      break;
    }
    }
  }

  return SelfAccessKind::NonMutating;
}

bool TypeChecker::isAvailabilitySafeForConformance(
    ProtocolDecl *proto, ValueDecl *requirement, ValueDecl *witness,
    DeclContext *dc, AvailabilityContext &requirementInfo) {

  // We assume conformances in
  // non-SourceFiles have already been checked for availability.
  if (!dc->getParentSourceFile())
    return true;

  auto &Context = proto->getASTContext();
  NominalTypeDecl *conformingDecl = dc->getSelfNominalTypeDecl();
  assert(conformingDecl && "Must have conforming declaration");

  // Make sure that any access of the witness through the protocol
  // can only occur when the witness is available. That is, make sure that
  // on every version where the conforming declaration is available, if the
  // requirement is available then the witness is available as well.
  // We do this by checking that (an over-approximation of) the intersection of
  // the requirement's available range with both the conforming declaration's
  // available range and the protocol's available range is fully contained in
  // (an over-approximation of) the intersection of the witnesses's available
  // range with both the conforming type's available range and the protocol
  // declaration's available range.
  AvailabilityContext witnessInfo =
      AvailabilityInference::availableRange(witness, Context);
  requirementInfo = AvailabilityInference::availableRange(requirement, Context);

  AvailabilityContext infoForConformingDecl =
      overApproximateAvailabilityAtLocation(conformingDecl->getLoc(),
                                            conformingDecl);

  // Constrain over-approximates intersection of version ranges.
  witnessInfo.constrainWith(infoForConformingDecl);
  requirementInfo.constrainWith(infoForConformingDecl);

  AvailabilityContext infoForProtocolDecl =
      overApproximateAvailabilityAtLocation(proto->getLoc(), proto);

  witnessInfo.constrainWith(infoForProtocolDecl);
  requirementInfo.constrainWith(infoForProtocolDecl);

  return requirementInfo.isContainedIn(witnessInfo);
}

// Returns 'nullptr' if this is the setter's 'newValue' parameter;
// otherwise, returns the corresponding parameter of the subscript
// declaration.
static ParamDecl *getOriginalParamFromAccessor(AbstractStorageDecl *storage,
                                               AccessorDecl *accessor,
                                               ParamDecl *param) {
  auto *accessorParams = accessor->getParameters();
  unsigned startIndex = 0;

  switch (accessor->getAccessorKind()) {
  case AccessorKind::DidSet:
  case AccessorKind::WillSet:
      return nullptr;

  case AccessorKind::Set:
    if (param == accessorParams->get(0)) {
      // This is the 'newValue' parameter.
      return nullptr;
    }

    startIndex = 1;
    break;

  default:
    startIndex = 0;
    break;
  }

  // If the parameter is not the 'newValue' parameter to a setter, it
  // must be a subscript index parameter (or we have an invalid AST).
  auto *subscript = cast<SubscriptDecl>(storage);
  auto *subscriptParams = subscript->getIndices();

  auto where = llvm::find_if(*accessorParams,
                              [param](ParamDecl *other) {
                                return other == param;
                              });
  assert(where != accessorParams->end());
  unsigned index = where - accessorParams->begin();

  return subscriptParams->get(index - startIndex);
}

bool
IsImplicitlyUnwrappedOptionalRequest::evaluate(Evaluator &evaluator,
                                               ValueDecl *decl) const {
  TypeRepr *TyR = nullptr;

  switch (decl->getKind()) {
  case DeclKind::Func: {
    TyR = cast<FuncDecl>(decl)->getResultTypeRepr();
    break;
  }

  case DeclKind::Accessor: {
    auto *accessor = cast<AccessorDecl>(decl);
    if (!accessor->isGetter())
      break;

    auto *storage = accessor->getStorage();
    if (auto *subscript = dyn_cast<SubscriptDecl>(storage))
      TyR = subscript->getElementTypeRepr();
    else
      TyR = cast<VarDecl>(storage)->getTypeReprOrParentPatternTypeRepr();
    break;
  }

  case DeclKind::Subscript:
    TyR = cast<SubscriptDecl>(decl)->getElementTypeRepr();
    break;

  case DeclKind::Param: {
    auto *param = cast<ParamDecl>(decl);
    if (param->isSelfParameter())
      return false;

    if (auto *accessor = dyn_cast<AccessorDecl>(param->getDeclContext())) {
      auto *storage = accessor->getStorage();
      auto *originalParam = getOriginalParamFromAccessor(
        storage, accessor, param);
      if (originalParam == nullptr) {
        // This is the setter's newValue parameter.
        return storage->isImplicitlyUnwrappedOptional();
      }

      if (param != originalParam) {
        // This is the 'subscript(...) { get { ... } set { ... } }' case.
        // This means we cloned the parameter list for each accessor.
        // Delegate to the original parameter.
        return originalParam->isImplicitlyUnwrappedOptional();
      }

      // This is the 'subscript(...) { <<body of getter>> }' case.
      // The subscript and the getter share their ParamDecls.
      // Fall through.
    }

    // Handle eg, 'inout Int!' or '__owned NSObject!'.
    TyR = param->getTypeRepr();
    if (auto *STR = dyn_cast_or_null<SpecifierTypeRepr>(TyR))
      TyR = STR->getBase();
    break;
  }

  case DeclKind::Var:
    TyR = cast<VarDecl>(decl)->getTypeReprOrParentPatternTypeRepr();
    break;

  default:
    break;
  }

  return (TyR && TyR->getKind() == TypeReprKind::ImplicitlyUnwrappedOptional);
}

/// Validate the underlying type of the given typealias.
Type
UnderlyingTypeRequest::evaluate(Evaluator &evaluator,
                                TypeAliasDecl *typeAlias) const {
  TypeResolutionOptions options((typeAlias->getGenericParams()
                                     ? TypeResolverContext::GenericTypeAliasDecl
                                     : TypeResolverContext::TypeAliasDecl));

  // This can happen when code completion is attempted inside
  // of typealias underlying type e.g. `typealias F = () -> Int#^TOK^#`
  auto *underlyingRepr = typeAlias->getUnderlyingTypeRepr();
  if (!underlyingRepr) {
    typeAlias->setInvalid();
    return ErrorType::get(typeAlias->getASTContext());
  }

  const auto result = TypeResolution::forInterface(typeAlias, options,
                                                   /*unboundTyOpener*/ nullptr)
                          .resolveType(underlyingRepr);

  if (result->hasError()) {
    typeAlias->setInvalid();
    return ErrorType::get(typeAlias->getASTContext());
  }
  return result;
}

/// Bind the given function declaration, which declares an operator, to the
/// corresponding operator declaration.
OperatorDecl *
FunctionOperatorRequest::evaluate(Evaluator &evaluator, FuncDecl *FD) const {  
  auto &C = FD->getASTContext();
  auto &diags = C.Diags;
  const auto operatorName = FD->getBaseIdentifier();

  // Check for static/final/class when we're in a type.
  auto dc = FD->getDeclContext();
  if (dc->isTypeContext()) {
    if (auto classDecl = dc->getSelfClassDecl()) {
      // For a class, we also need the function or class to be 'final'.
      if (!classDecl->isFinal() && !FD->isFinal() &&
          FD->getStaticLoc().isValid() &&
          FD->getStaticSpelling() != StaticSpellingKind::KeywordStatic) {
        FD->diagnose(diag::nonfinal_operator_in_class,
                     operatorName, dc->getDeclaredInterfaceType())
          .fixItInsert(FD->getAttributeInsertionLoc(/*forModifier=*/true),
                       "final ");
        FD->getAttrs().add(new (C) FinalAttr(/*IsImplicit=*/true));
      }
    }
  } else if (!dc->isModuleScopeContext()) {
    FD->diagnose(diag::operator_in_local_scope);
  }

  NullablePtr<OperatorDecl> op;
  if (FD->isUnaryOperator()) {
    if (FD->getAttrs().hasAttribute<PrefixAttr>()) {
      op = FD->lookupPrefixOperator(operatorName);
    } else if (FD->getAttrs().hasAttribute<PostfixAttr>()) {
      op = FD->lookupPostfixOperator(operatorName);
    } else {
      auto *prefixOp = FD->lookupPrefixOperator(operatorName);
      auto *postfixOp = FD->lookupPostfixOperator(operatorName);

      // If we found both prefix and postfix, or neither prefix nor postfix,
      // complain. We can't fix this situation.
      if (static_cast<bool>(prefixOp) == static_cast<bool>(postfixOp)) {
        diags.diagnose(FD, diag::declared_unary_op_without_attribute);

        // If we found both, point at them.
        if (prefixOp) {
          diags.diagnose(prefixOp, diag::unary_operator_declaration_here,
                         /*isPostfix*/ false)
            .fixItInsert(FD->getLoc(), "prefix ");
          diags.diagnose(postfixOp, diag::unary_operator_declaration_here,
                         /*isPostfix*/ true)
            .fixItInsert(FD->getLoc(), "postfix ");
        } else {
          // FIXME: Introduce a Fix-It that adds the operator declaration?
        }

        // FIXME: Errors could cascade here, because name lookup for this
        // operator won't find this declaration.
        return nullptr;
      }

      // We found only one operator declaration, so we know whether this
      // should be a prefix or a postfix operator.

      // Fix the AST and determine the insertion text.
      const char *insertionText;
      auto &C = FD->getASTContext();
      auto isPostfix = static_cast<bool>(postfixOp);
      if (isPostfix) {
        insertionText = "postfix ";
        op = postfixOp;
        FD->getAttrs().add(new (C) PostfixAttr(/*implicit*/false));
      } else {
        insertionText = "prefix ";
        op = prefixOp;
        FD->getAttrs().add(new (C) PrefixAttr(/*implicit*/false));
      }

      // Emit diagnostic with the Fix-It.
      diags.diagnose(FD->getFuncLoc(), diag::unary_op_missing_prepos_attribute,
                     isPostfix)
        .fixItInsert(FD->getFuncLoc(), insertionText);
      op.get()->diagnose(diag::unary_operator_declaration_here, isPostfix);
    }
  } else if (FD->isBinaryOperator()) {
    auto results = FD->lookupInfixOperator(operatorName);

    // If we have an ambiguity, diagnose and return. Otherwise fall through, as
    // we have a custom diagnostic for missing operator decls.
    if (results.isAmbiguous()) {
      results.diagnoseAmbiguity(FD->getLoc());
      return nullptr;
    }
    op = results.getSingle();
  } else {
    diags.diagnose(FD, diag::invalid_arg_count_for_operator);
    return nullptr;
  }

  if (!op) {
    SourceLoc insertionLoc;
    if (isa<SourceFile>(FD->getParent())) {
      // Parent context is SourceFile, insertion location is start of func
      // declaration or unary operator
      if (FD->isUnaryOperator()) {
        insertionLoc = FD->getAttrs().getStartLoc();
      } else {
        insertionLoc = FD->getStartLoc();
      }
    } else {
      // Find the topmost non-file decl context and insert there.
      for (DeclContext *CurContext = FD->getLocalContext();
           !isa<SourceFile>(CurContext);
           CurContext = CurContext->getParent()) {
        // Skip over non-decl contexts (e.g. closure expresssions)
        if (auto *D = CurContext->getAsDecl())
            insertionLoc = D->getStartLoc();
      }
    }

    SmallString<128> insertion;
    {
      llvm::raw_svector_ostream str(insertion);
      assert(FD->isUnaryOperator() || FD->isBinaryOperator());
      if (FD->isUnaryOperator()) {
        if (FD->getAttrs().hasAttribute<PrefixAttr>())
          str << "prefix operator ";
        else
          str << "postfix operator ";
      } else {
        str << "infix operator ";
      }

       str << operatorName.str() << " : <# Precedence Group #>\n";
    }
    InFlightDiagnostic opDiagnostic =
        diags.diagnose(FD, diag::declared_operator_without_operator_decl);
    if (insertionLoc.isValid())
      opDiagnostic.fixItInsert(insertionLoc, insertion);
    return nullptr;
  }
  return op.get();
}

bool swift::isMemberOperator(FuncDecl *decl, Type type) {
  // Check that member operators reference the type of 'Self'.
  if (decl->isInvalid())
    return true;

  auto *DC = decl->getDeclContext();
  auto selfNominal = DC->getSelfNominalTypeDecl();

  // Check the parameters for a reference to 'Self'.
  bool isProtocol = selfNominal && isa<ProtocolDecl>(selfNominal);
  for (auto param : *decl->getParameters()) {
    // Look through a metatype reference, if there is one.
    auto paramType = param->getInterfaceType()->getMetatypeInstanceType();

    auto nominal = paramType->getAnyNominal();
    if (type.isNull()) {
      // Is it the same nominal type?
      if (selfNominal && nominal == selfNominal)
        return true;
    } else {
      // Is it the same nominal type? Or a generic (which may or may not match)?
      if (paramType->is<GenericTypeParamType>() ||
          nominal == type->getAnyNominal())
        return true;
    }

    if (isProtocol) {
      // For a protocol, is it the 'Self' type parameter?
      if (auto genericParam = paramType->getAs<GenericTypeParamType>())
        if (genericParam->isEqual(DC->getSelfInterfaceType()))
          return true;
    }
  }

  return false;
}

static Type buildAddressorResultType(AccessorDecl *addressor,
                                     Type valueType) {
  assert(addressor->getAccessorKind() == AccessorKind::Address ||
         addressor->getAccessorKind() == AccessorKind::MutableAddress);

  PointerTypeKind pointerKind =
    (addressor->getAccessorKind() == AccessorKind::Address)
      ? PTK_UnsafePointer
      : PTK_UnsafeMutablePointer;
  return valueType->wrapInPointer(pointerKind);
}

Type
ResultTypeRequest::evaluate(Evaluator &evaluator, ValueDecl *decl) const {
  auto &ctx = decl->getASTContext();

  // Accessors always inherit their result type from their storage.
  if (auto *accessor = dyn_cast<AccessorDecl>(decl)) {
    auto *storage = accessor->getStorage();

    switch (accessor->getAccessorKind()) {
    // For getters, set the result type to the value type.
    case AccessorKind::Get:
      return storage->getValueInterfaceType();

    // For setters and observers, set the old/new value parameter's type
    // to the value type.
    case AccessorKind::DidSet:
    case AccessorKind::WillSet:
    case AccessorKind::Set:
      return TupleType::getEmpty(ctx);

    // Addressor result types can get complicated because of the owner.
    case AccessorKind::Address:
    case AccessorKind::MutableAddress:
      return buildAddressorResultType(accessor, storage->getValueInterfaceType());

    // Coroutine accessors don't mention the value type directly.
    // If we add yield types to the function type, we'll need to update this.
    case AccessorKind::Read:
    case AccessorKind::Modify:
      return TupleType::getEmpty(ctx);
    }
  }

  TypeRepr *resultTyRepr = nullptr;
  if (const auto *const funcDecl = dyn_cast<FuncDecl>(decl)) {
    resultTyRepr = funcDecl->getResultTypeRepr();
  } else {
    resultTyRepr = cast<SubscriptDecl>(decl)->getElementTypeRepr();
  }

  // Nothing to do if there's no result type.
  if (resultTyRepr == nullptr)
    return TupleType::getEmpty(ctx);

  // Handle opaque types.
  if (decl->getOpaqueResultTypeRepr()) {
    auto *opaqueDecl = decl->getOpaqueResultTypeDecl();
    return (opaqueDecl
            ? opaqueDecl->getDeclaredInterfaceType()
            : ErrorType::get(ctx));
  }

  const auto options =
      TypeResolutionOptions(TypeResolverContext::FunctionResult);
  auto *const dc = decl->getInnermostDeclContext();
  return TypeResolution::forInterface(dc, options, /*unboundTyOpener*/ nullptr)
      .resolveType(resultTyRepr);
}

ParamSpecifier
ParamSpecifierRequest::evaluate(Evaluator &evaluator,
                                ParamDecl *param) const {
  auto *dc = param->getDeclContext();

  if (param->isSelfParameter()) {
    auto selfParam = computeSelfParam(cast<AbstractFunctionDecl>(dc),
                                      /*isInitializingCtor*/true,
                                      /*wantDynamicSelf*/false);
    return (selfParam.getParameterFlags().isInOut()
            ? ParamSpecifier::InOut
            : ParamSpecifier::Default);
  }

  if (auto *accessor = dyn_cast<AccessorDecl>(dc)) {
    auto *storage = accessor->getStorage();
    auto *originalParam = getOriginalParamFromAccessor(
      storage, accessor, param);
    if (originalParam == nullptr) {
      // This is the setter's newValue parameter. Note that even though
      // the AST uses the 'Default' specifier, SIL will lower this to a
      // +1 parameter.
      return ParamSpecifier::Default;
    }

    if (param != originalParam) {
      // This is the 'subscript(...) { get { ... } set { ... } }' case.
      // This means we cloned the parameter list for each accessor.
      // Delegate to the original parameter.
      return originalParam->getSpecifier();
    }

    // This is the 'subscript(...) { <<body of getter>> }' case.
    // The subscript and the getter share their ParamDecls.
    // Fall through.
  }

  auto typeRepr = param->getTypeRepr();
  assert(typeRepr != nullptr && "Should call setSpecifier() on "
         "synthesized parameter declarations");

  auto *nestedRepr = typeRepr;

  // Look through parens here; other than parens, specifiers
  // must appear at the top level of a parameter type.
  while (auto *tupleRepr = dyn_cast<TupleTypeRepr>(nestedRepr)) {
    if (!tupleRepr->isParenType())
      break;
    nestedRepr = tupleRepr->getElementType(0);
  }

  if (isa<InOutTypeRepr>(nestedRepr) &&
      param->isDefaultArgument()) {
    auto &ctx = param->getASTContext();
    ctx.Diags.diagnose(param->getStructuralDefaultExpr()->getLoc(),
                       swift::diag::cannot_provide_default_value_inout,
                       param->getName());
    return ParamSpecifier::Default;
  }

  if (isa<InOutTypeRepr>(nestedRepr)) {
    return ParamSpecifier::InOut;
  } else if (isa<SharedTypeRepr>(nestedRepr)) {
    return ParamSpecifier::Shared;
  } else if (isa<OwnedTypeRepr>(nestedRepr)) {
    return ParamSpecifier::Owned;
  }

  return ParamSpecifier::Default;
}

static Type validateParameterType(ParamDecl *decl) {
  auto *dc = decl->getDeclContext();

  TypeResolutionOptions options(None);
  OpenUnboundGenericTypeFn unboundTyOpener = nullptr;
  if (isa<AbstractClosureExpr>(dc)) {
    options = TypeResolutionOptions(TypeResolverContext::ClosureExpr);
    options |= TypeResolutionFlags::AllowUnspecifiedTypes;
    unboundTyOpener = [](auto unboundTy) {
      // FIXME: Don't let unbound generic types escape type resolution.
      // For now, just return the unbound generic type.
      return unboundTy;
    };
  } else if (isa<AbstractFunctionDecl>(dc)) {
    options = TypeResolutionOptions(TypeResolverContext::AbstractFunctionDecl);
  } else if (isa<SubscriptDecl>(dc)) {
    options = TypeResolutionOptions(TypeResolverContext::SubscriptDecl);
  } else {
    assert(isa<EnumElementDecl>(dc));
    options = TypeResolutionOptions(TypeResolverContext::EnumElementDecl);
  }

  // If the element is a variadic parameter, resolve the parameter type as if
  // it were in non-parameter position, since we want functions to be
  // @escaping in this case.
  options.setContext(decl->isVariadic() ?
                       TypeResolverContext::VariadicFunctionInput :
                       TypeResolverContext::FunctionInput);
  options |= TypeResolutionFlags::Direct;

  if (dc->isInSpecializeExtensionContext())
    options |= TypeResolutionFlags::AllowUsableFromInline;

  const auto resolution =
      TypeResolution::forInterface(dc, options, unboundTyOpener);
  auto Ty = resolution.resolveType(decl->getTypeRepr());

  auto &ctx = dc->getASTContext();
  if (Ty->hasError()) {
    decl->setInvalid();
    return ErrorType::get(ctx);
  }

  if (decl->isVariadic()) {
    Ty = TypeChecker::getArraySliceType(decl->getStartLoc(), Ty);
    if (Ty->hasError()) {
      decl->setInvalid();
      return ErrorType::get(ctx);
    }

    // Disallow variadic parameters in enum elements.
    if (options.getBaseContext() == TypeResolverContext::EnumElementDecl) {
      decl->diagnose(diag::enum_element_ellipsis);
      decl->setInvalid();
      return ErrorType::get(ctx);
    }
  }

  return Ty;
}

Type
InterfaceTypeRequest::evaluate(Evaluator &eval, ValueDecl *D) const {
  auto &Context = D->getASTContext();

  TypeChecker::checkForForbiddenPrefix(Context, D->getBaseName());

  switch (D->getKind()) {
  case DeclKind::Import:
  case DeclKind::Extension:
  case DeclKind::PatternBinding:
  case DeclKind::EnumCase:
  case DeclKind::TopLevelCode:
  case DeclKind::InfixOperator:
  case DeclKind::PrefixOperator:
  case DeclKind::PostfixOperator:
  case DeclKind::PrecedenceGroup:
  case DeclKind::IfConfig:
  case DeclKind::PoundDiagnostic:
  case DeclKind::MissingMember:
  case DeclKind::Module:
  case DeclKind::OpaqueType:
  case DeclKind::GenericTypeParam:
    llvm_unreachable("should not get here");
    return Type();

  case DeclKind::AssociatedType: {
    auto assocType = cast<AssociatedTypeDecl>(D);
    auto interfaceTy = assocType->getDeclaredInterfaceType();
    return MetatypeType::get(interfaceTy, Context);
  }

  case DeclKind::TypeAlias: {
    auto typeAlias = cast<TypeAliasDecl>(D);

    auto genericSig = typeAlias->getGenericSignature();
    SubstitutionMap subs;
    if (genericSig)
      subs = genericSig->getIdentitySubstitutionMap();

    Type parent;
    auto parentDC = typeAlias->getDeclContext();
    if (parentDC->isTypeContext())
      parent = parentDC->getSelfInterfaceType();
    auto sugaredType = TypeAliasType::get(typeAlias, parent, subs,
                                          typeAlias->getUnderlyingType());
    return MetatypeType::get(sugaredType, Context);
  }

  case DeclKind::Enum:
  case DeclKind::Struct:
  case DeclKind::Class:
  case DeclKind::Protocol: {
    auto nominal = cast<NominalTypeDecl>(D);
    Type declaredInterfaceTy = nominal->getDeclaredInterfaceType();
    return MetatypeType::get(declaredInterfaceTy, Context);
  }

  case DeclKind::Param: {
    auto *PD = cast<ParamDecl>(D);
    if (PD->isSelfParameter()) {
      auto *AFD = cast<AbstractFunctionDecl>(PD->getDeclContext());
      auto selfParam = computeSelfParam(AFD,
                                        /*isInitializingCtor*/true,
                                        /*wantDynamicSelf*/true);
      return selfParam.getPlainType();
    }

    if (auto *accessor = dyn_cast<AccessorDecl>(PD->getDeclContext())) {
      auto *storage = accessor->getStorage();
      auto *originalParam = getOriginalParamFromAccessor(
        storage, accessor, PD);
      if (originalParam == nullptr) {
        return storage->getValueInterfaceType();
      }

      if (originalParam != PD) {
        return originalParam->getInterfaceType();
      }
    }

    if (!PD->getTypeRepr())
      return Type();

    return validateParameterType(PD);
  }

  case DeclKind::Var: {
    auto *VD = cast<VarDecl>(D);
    auto *namingPattern = VD->getNamingPattern();
    if (!namingPattern) {
      return ErrorType::get(Context);
    }

    Type interfaceType = namingPattern->getType();
    if (interfaceType->hasArchetype())
      interfaceType = interfaceType->mapTypeOutOfContext();

    // In SIL mode, VarDecls are written as having reference storage types.
    if (!interfaceType->is<ReferenceStorageType>()) {
      if (auto *attr = VD->getAttrs().getAttribute<ReferenceOwnershipAttr>())
        interfaceType =
            TypeChecker::checkReferenceOwnershipAttr(VD, interfaceType, attr);
    }

    return interfaceType;
  }

  case DeclKind::Func:
  case DeclKind::Accessor:
  case DeclKind::Constructor:
  case DeclKind::Destructor: {
    // If this is a didSet, then we need to check whether the body references
    // the implicit 'oldValue' parameter or not, in order to correctly
    // compute the interface type.
    if (auto AD = dyn_cast<AccessorDecl>(D)) {
      (void)AD->isSimpleDidSet();
    }

    auto *AFD = cast<AbstractFunctionDecl>(D);

    auto sig = AFD->getGenericSignature();
    bool hasSelf = AFD->hasImplicitSelfDecl();

    AnyFunctionType::ExtInfoBuilder infoBuilder;

    // Result
    Type resultTy;
    if (auto fn = dyn_cast<FuncDecl>(D)) {
      resultTy = fn->getResultInterfaceType();
    } else if (auto ctor = dyn_cast<ConstructorDecl>(D)) {
      resultTy = ctor->getResultInterfaceType();
    } else {
      assert(isa<DestructorDecl>(D));
      resultTy = TupleType::getEmpty(AFD->getASTContext());
    }

    // (Args...) -> Result
    Type funcTy;

    {
      SmallVector<AnyFunctionType::Param, 4> argTy;
      AFD->getParameters()->getParams(argTy);

      infoBuilder = infoBuilder.withAsync(AFD->hasAsync());
      // 'throws' only applies to the innermost function.
      infoBuilder = infoBuilder.withThrows(AFD->hasThrows());
      // Defer bodies must not escape.
      if (auto fd = dyn_cast<FuncDecl>(D))
        infoBuilder = infoBuilder.withNoEscape(fd->isDeferBody());
      auto info = infoBuilder.build();

      if (sig && !hasSelf) {
        funcTy = GenericFunctionType::get(sig, argTy, resultTy, info);
      } else {
        funcTy = FunctionType::get(argTy, resultTy, info);
      }
    }

    // (Self) -> (Args...) -> Result
    if (hasSelf) {
      // Substitute in our own 'self' parameter.
      auto selfParam = computeSelfParam(AFD);
      if (sig)
        funcTy = GenericFunctionType::get(sig, {selfParam}, funcTy);
      else
        funcTy = FunctionType::get({selfParam}, funcTy);
    }

    return funcTy;
  }

  case DeclKind::Subscript: {
    auto *SD = cast<SubscriptDecl>(D);

    auto elementTy = SD->getElementInterfaceType();

    SmallVector<AnyFunctionType::Param, 2> argTy;
    SD->getIndices()->getParams(argTy);

    Type funcTy;
    if (auto sig = SD->getGenericSignature())
      funcTy = GenericFunctionType::get(sig, argTy, elementTy);
    else
      funcTy = FunctionType::get(argTy, elementTy);

    return funcTy;
  }

  case DeclKind::EnumElement: {
    auto *EED = cast<EnumElementDecl>(D);

    auto *ED = EED->getParentEnum();

    // The type of the enum element is either (Self.Type) -> Self
    // or (Self.Type) -> (Args...) -> Self.
    auto resultTy = ED->getDeclaredInterfaceType();

    AnyFunctionType::Param selfTy(MetatypeType::get(resultTy, Context));

    if (auto *PL = EED->getParameterList()) {
      SmallVector<AnyFunctionType::Param, 4> argTy;
      PL->getParams(argTy);

      resultTy = FunctionType::get(argTy, resultTy);
    }

    if (auto genericSig = ED->getGenericSignature())
      resultTy = GenericFunctionType::get(genericSig, {selfTy}, resultTy);
    else
      resultTy = FunctionType::get({selfTy}, resultTy);

    return resultTy;
  }
  }
  llvm_unreachable("invalid decl kind");
}

NamedPattern *
NamingPatternRequest::evaluate(Evaluator &evaluator, VarDecl *VD) const {
  auto &Context = VD->getASTContext();
  auto *PBD = VD->getParentPatternBinding();
  // FIXME: In order for this request to properly express its dependencies,
  // all of the places that allow variable bindings need to also use pattern
  // binding decls. Otherwise, we'll have to go digging around in case
  // statements and patterns to find named patterns.
  if (PBD) {
    // FIXME: For now, this works because PatternBindingEntryRequest fills in
    // the naming pattern as a side effect in this case, and TypeCheckStmt
    // and TypeCheckPattern handle the others. But that's all really gross.
    unsigned i = PBD->getPatternEntryIndexForVarDecl(VD);
    (void)evaluateOrDefault(evaluator,
                            PatternBindingEntryRequest{PBD, i},
                            nullptr);
    if (PBD->isInvalid()) {
      VD->getParentPattern()->setType(ErrorType::get(Context));
      setBoundVarsTypeError(VD->getParentPattern(), Context);
      return nullptr;
    }
  } else if (!VD->getParentPatternStmt() && !VD->getParentVarDecl()) {
    // No parent?  That's an error.
    return nullptr;
  }

  // Go digging for the named pattern that declares this variable.
  auto *namingPattern = VD->NamingPattern;
  if (!namingPattern) {
    auto *canVD = VD->getCanonicalVarDecl();
    namingPattern = canVD->NamingPattern;
  }

  if (!namingPattern) {
    // Try type checking parent control statement.
    if (auto parentStmt = VD->getParentPatternStmt()) {
      if (auto CS = dyn_cast<CaseStmt>(parentStmt))
        parentStmt = CS->getParentStmt();
      ASTNode node(parentStmt);
      TypeChecker::typeCheckASTNode(node, VD->getDeclContext(),
                                    /*LeaveBodyUnchecked=*/true);
      namingPattern = VD->getCanonicalVarDecl()->NamingPattern;
    }
  }

  if (!namingPattern) {
    // HACK: If no other diagnostic applies, emit a generic diagnostic about
    // a variable being unbound. We can't do better than this at the
    // moment because TypeCheckPattern does not reliably invalidate parts of
    // the pattern AST on failure.
    //
    // Once that's through, this will only fire during circular validation.
    if (VD->hasInterfaceType() &&
        !VD->isInvalid() && !VD->getParentPattern()->isImplicit()) {
      VD->diagnose(diag::variable_bound_by_no_pattern, VD->getName());
    }

    VD->getParentPattern()->setType(ErrorType::get(Context));
    setBoundVarsTypeError(VD->getParentPattern(), Context);
    return nullptr;
  }

  if (!namingPattern->hasType()) {
    namingPattern->setType(ErrorType::get(Context));
    setBoundVarsTypeError(namingPattern, Context);
  }

  return namingPattern;
}

namespace {

// Utility class for deterministically ordering vtable entries for
// synthesized methods.
struct SortedFuncList {
  using Key = std::tuple<DeclName, std::string>;
  using Entry = std::pair<Key, AbstractFunctionDecl *>;
  SmallVector<Entry, 2> elts;
  bool sorted = false;

  void add(AbstractFunctionDecl *afd) {
    assert(!isa<AccessorDecl>(afd));

    Key key{afd->getName(), afd->getInterfaceType().getString()};
    elts.emplace_back(key, afd);
  }

  bool empty() { return elts.empty(); }

  void sort() {
    assert(!sorted);
    sorted = true;
    std::sort(elts.begin(),
              elts.end(),
              [](const Entry &lhs, const Entry &rhs) -> bool {
                return lhs.first < rhs.first;
              });
  }

  decltype(elts)::const_iterator begin() const {
    assert(sorted);
    return elts.begin();
  }

  decltype(elts)::const_iterator end() const {
    assert(sorted);
    return elts.end();
  }
};

} // end namespace

ArrayRef<Decl *>
SemanticMembersRequest::evaluate(Evaluator &evaluator,
                                IterableDeclContext *idc) const {
  auto dc = cast<DeclContext>(idc->getDecl());
  auto &Context = dc->getASTContext();
  SmallVector<Decl *, 8> result;

  if (!dc->getParentSourceFile()) {
    auto members = idc->getMembers();
    result.append(members.begin(), members.end());
    return Context.AllocateCopy(result);
  }

  auto nominal = dyn_cast<NominalTypeDecl>(idc);

  if (nominal) {
    // We need to add implicit initializers because they
    // affect vtable layout.
    TypeChecker::addImplicitConstructors(nominal);
  }

  // Force any derivable conformances in this context. This ensures that any
  // synthesized members will approach in the member list.
  for (auto conformance : idc->getLocalConformances()) {
    if (conformance->getState() == ProtocolConformanceState::Incomplete &&
        conformance->getProtocol()->getKnownDerivableProtocolKind())
      TypeChecker::checkConformance(conformance->getRootNormalConformance());
  }

  // If the type conforms to Encodable or Decodable, even via an extension,
  // the CodingKeys enum is synthesized as a member of the type itself.
  // Force it into existence.
  if (nominal) {
    (void) evaluateOrDefault(Context.evaluator,
                             ResolveImplicitMemberRequest{nominal,
                                        ImplicitMemberAction::ResolveCodingKeys},
                             {});
  }

  // If the decl has a @main attribute, we need to force synthesis of the
  // $main function.
  (void) evaluateOrDefault(
      Context.evaluator,
      SynthesizeMainFunctionRequest{const_cast<Decl *>(idc->getDecl())},
      nullptr);

  for (auto *member : idc->getMembers()) {
    if (auto *var = dyn_cast<VarDecl>(member)) {
      // The projected storage wrapper ($foo) might have dynamically-dispatched
      // accessors, so force them to be synthesized.
      if (var->hasAttachedPropertyWrapper())
        (void) var->getPropertyWrapperBackingProperty();
    }
  }

  SortedFuncList synthesizedMembers;

  for (auto *member : idc->getMembers()) {
    if (auto *afd = dyn_cast<AbstractFunctionDecl>(member)) {
      // If this is a witness to Actor.enqueue(partialTask:), put it at the
      // beginning of the vtable.
      if (auto func = dyn_cast<FuncDecl>(afd)) {
        if (func->isActorEnqueuePartialTaskWitness()) {
          result.insert(result.begin(), func);
          continue;
        }
      }

      // Add synthesized members to a side table and sort them by their mangled
      // name, since they could have been added to the class in any order.
      if (afd->isSynthesized()) {
        synthesizedMembers.add(afd);
        continue;
      }
    }

    result.push_back(member);
  }

  if (!synthesizedMembers.empty()) {
    synthesizedMembers.sort();

    for (const auto &pair : synthesizedMembers)
      result.push_back(pair.second);
  }

  return Context.AllocateCopy(result);
}

bool TypeChecker::isPassThroughTypealias(TypeAliasDecl *typealias,
                                         Type underlyingType,
                                         NominalTypeDecl *nominal) {
  // Pass-through only makes sense when the typealias refers to a nominal
  // type.
  if (!nominal) return false;

  // Check that the nominal type and the typealias are either both generic
  // at this level or neither are.
  if (nominal->isGeneric() != typealias->isGeneric())
    return false;

  // Make sure either both have generic signatures or neither do.
  auto nominalSig = nominal->getGenericSignature();
  auto typealiasSig = typealias->getGenericSignature();
  if (static_cast<bool>(nominalSig) != static_cast<bool>(typealiasSig))
    return false;

  // If neither is generic, we're done: it's a pass-through alias.
  if (!nominalSig) return true;

  // Check that the type parameters are the same the whole way through.
  auto nominalGenericParams = nominalSig->getGenericParams();
  auto typealiasGenericParams = typealiasSig->getGenericParams();
  if (nominalGenericParams.size() != typealiasGenericParams.size())
    return false;
  if (!std::equal(nominalGenericParams.begin(), nominalGenericParams.end(),
                  typealiasGenericParams.begin(),
                  [](GenericTypeParamType *gp1, GenericTypeParamType *gp2) {
                    return gp1->isEqual(gp2);
                  }))
    return false;

  // If neither is generic at this level, we have a pass-through typealias.
  if (!typealias->isGeneric()) return true;

  auto boundGenericType = underlyingType->getAs<BoundGenericType>();
  if (!boundGenericType) return false;

  // If our arguments line up with our innermost generic parameters, it's
  // a passthrough typealias.
  auto innermostGenericParams = typealiasSig->getInnermostGenericParams();
  auto boundArgs = boundGenericType->getGenericArgs();
  if (boundArgs.size() != innermostGenericParams.size())
    return false;

  return std::equal(boundArgs.begin(), boundArgs.end(),
                    innermostGenericParams.begin(),
                    [](Type arg, GenericTypeParamType *gp) {
                      return arg->isEqual(gp);
                    });
}

static bool isNonGenericTypeAliasType(Type type) {
  // A non-generic typealias can extend a specialized type.
  if (auto *aliasType = dyn_cast<TypeAliasType>(type.getPointer()))
    return aliasType->getDecl()->getGenericContextDepth() == (unsigned)-1;

  return false;
}

Type
ExtendedTypeRequest::evaluate(Evaluator &eval, ExtensionDecl *ext) const {
  auto error = [&ext]() {
    ext->setInvalid();
    return ErrorType::get(ext->getASTContext());
  };

  // If we didn't parse a type, fill in an error type and bail out.
  auto *extendedRepr = ext->getExtendedTypeRepr();
  if (!extendedRepr)
    return error();

  // Compute the extended type.
  TypeResolutionOptions options(TypeResolverContext::ExtensionBinding);
  if (ext->isInSpecializeExtensionContext())
    options |= TypeResolutionFlags::AllowUsableFromInline;
  const auto resolution = TypeResolution::forStructural(
      ext->getDeclContext(), options,
      [](auto unboundTy) {
        // FIXME: Don't let unbound generic types escape type resolution.
        // For now, just return the unbound generic type.
        return unboundTy;
      });

  const auto extendedType = resolution.resolveType(extendedRepr);

  if (extendedType->hasError())
    return error();

  // Hack to allow extending a generic typealias.
  if (auto *unboundGeneric = extendedType->getAs<UnboundGenericType>()) {
    if (auto *aliasDecl = dyn_cast<TypeAliasDecl>(unboundGeneric->getDecl())) {
      // Nested Hack to break cycles if this is called before validation has
      // finished.
      if (aliasDecl->hasInterfaceType()) {
        auto extendedNominal =
            aliasDecl->getDeclaredInterfaceType()->getAnyNominal();
        if (extendedNominal)
          return TypeChecker::isPassThroughTypealias(
                     aliasDecl, aliasDecl->getUnderlyingType(), extendedNominal)
                     ? extendedType
                     : extendedNominal->getDeclaredType();
      } else {
        if (auto ty = aliasDecl->getStructuralType()
                          ->getAs<NominalOrBoundGenericNominalType>())
          return TypeChecker::isPassThroughTypealias(aliasDecl, ty,
                                                     ty->getDecl())
                     ? extendedType
                     : ty->getDecl()->getDeclaredType();
      }
    }
  }

  auto &diags = ext->getASTContext().Diags;

  // Cannot extend a metatype.
  if (extendedType->is<AnyMetatypeType>()) {
    diags.diagnose(ext->getLoc(), diag::extension_metatype, extendedType)
         .highlight(extendedRepr->getSourceRange());
    return error();
  }

  // Cannot extend function types, tuple types, etc.
  if (!extendedType->getAnyNominal()) {
    diags.diagnose(ext->getLoc(), diag::non_nominal_extension, extendedType)
         .highlight(extendedRepr->getSourceRange());
    return error();
  }

  // Cannot extend a bound generic type, unless it's referenced via a
  // non-generic typealias type.
  if (extendedType->isSpecialized() &&
      !isNonGenericTypeAliasType(extendedType)) {
    diags.diagnose(ext->getLoc(), diag::extension_specialization,
                   extendedType->getAnyNominal()->getName())
         .highlight(extendedRepr->getSourceRange());
    return error();
  }

  return extendedType;
}
