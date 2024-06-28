//===--- TypeCheckEffects.cpp - Type Checking for Effects Coverage --------===//
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
// This file implements semantic analysis to ensure that various effects (such
// as throwing and async) are properly handled.
//
//===----------------------------------------------------------------------===//

#include "TypeChecker.h"
#include "TypeCheckConcurrency.h"
#include "TypeCheckEffects.h"
#include "swift/AST/ASTWalker.h"
#include "swift/AST/DiagnosticsSema.h"
#include "swift/AST/Effects.h"
#include "swift/AST/GenericEnvironment.h"
#include "swift/AST/Initializer.h"
#include "swift/AST/ParameterList.h"
#include "swift/AST/Pattern.h"
#include "swift/AST/PrettyStackTrace.h"
#include "swift/AST/ProtocolConformance.h"
#include "swift/AST/TypeCheckRequests.h"
#include "swift/Basic/Assertions.h"

using namespace swift;

static bool hasFunctionParameterWithEffect(EffectKind kind, Type type) {
  // Look through Optional types.
  type = type->lookThroughAllOptionalTypes();

  // Only consider function types with this effect.
  if (auto fnType = type->getAs<AnyFunctionType>()) {
    return fnType->hasEffect(kind);
  }

  // Look through tuples.
  if (auto tuple = type->getAs<TupleType>()) {
    for (auto eltType : tuple->getElementTypes()) {
      if (hasFunctionParameterWithEffect(kind, eltType))
        return true;
    }
    return false;
  }

  // Suppress diagnostics in the presence of errors.
  if (type->hasError()) {
    return true;
  }

  return false;
}

PolymorphicEffectRequirementList
PolymorphicEffectRequirementsRequest::evaluate(Evaluator &evaluator,
                                               EffectKind kind,
                                               ProtocolDecl *proto) const {
  ASTContext &ctx = proto->getASTContext();

  // only allow rethrowing requirements to be determined from marked protocols
  if (!proto->hasPolymorphicEffect(kind)) {
    return PolymorphicEffectRequirementList();
  }

  SmallVector<AbstractFunctionDecl *, 2> requirements;
  SmallVector<std::pair<Type, ProtocolDecl *>, 2> conformances;
  
  // check if immediate members of protocol are 'throws'
  for (auto member : proto->getMembers()) {
    auto fnDecl = dyn_cast<AbstractFunctionDecl>(member);
    if (!fnDecl || !fnDecl->hasEffect(kind))
      continue;

    requirements.push_back(fnDecl);
  }

  // check associated conformances of associated types or inheritance
  for (auto requirement : proto->getRequirementSignature().getRequirements()) {
    if (requirement.getKind() != RequirementKind::Conformance)
      continue;

    auto *protoDecl = requirement.getProtocolDecl();
    if (!protoDecl->hasPolymorphicEffect(kind))
      continue;

    conformances.emplace_back(requirement.getFirstType(), protoDecl);
  }
  
  return PolymorphicEffectRequirementList(ctx.AllocateCopy(requirements),
                                          ctx.AllocateCopy(conformances));
}

/// Determine whether the given protocol inherits from either
/// AsyncIteratorProtocol or AsyncSequence.
static bool inheritsFromAsyncSequenceProtocol(ProtocolDecl *proto) {
  // If it's exactly one of these, shortcut.
  if (proto->isSpecificProtocol(KnownProtocolKind::AsyncIteratorProtocol) ||
      proto->isSpecificProtocol(KnownProtocolKind::AsyncSequence))
    return false;

  auto &ctx = proto->getASTContext();
  if (auto iter = ctx.getProtocol(KnownProtocolKind::AsyncIteratorProtocol))
    if (proto->inheritsFrom(iter))
      return true;
  if (auto seq = ctx.getProtocol(KnownProtocolKind::AsyncSequence))
    if (proto->inheritsFrom(seq))
      return true;

  return false;
}

PolymorphicEffectKind
PolymorphicEffectKindRequest::evaluate(Evaluator &evaluator,
                                       EffectKind kind,
                                       AbstractFunctionDecl *decl) const {
  if (!decl->hasEffect(kind))
    return PolymorphicEffectKind::None;

  if (!decl->hasPolymorphicEffect(kind)) {
    if (auto proto = dyn_cast<ProtocolDecl>(decl->getDeclContext())) {
      if (proto->hasPolymorphicEffect(kind))
        return PolymorphicEffectKind::ByConformance;
    }

    return PolymorphicEffectKind::Always;
  }

  for (auto req : decl->getGenericSignature().getRequirements()) {
    if (req.getKind() == RequirementKind::Conformance) {
      auto proto = req.getProtocolDecl();

      if (proto->hasPolymorphicEffect(kind)) {
        // @rethrows protocols that inherit from AsyncIteratorProtocol or
        // AsyncSequence should be categorized like AsyncIteratorProtocol or
        // AsyncSequence.
        if (kind == EffectKind::Throws &&
            inheritsFromAsyncSequenceProtocol(proto))
          return PolymorphicEffectKind::AsyncSequenceRethrows;

        return PolymorphicEffectKind::ByConformance;
      }

      // Specifically recognize functions that are rethrows and would
      // have been ByConformance polymorphic when AsyncIteratorProtocol
      // and AsyncSequence were rethrowing protocols.
      if (kind == EffectKind::Throws &&
          (proto->isSpecificProtocol(
              KnownProtocolKind::AsyncIteratorProtocol) ||
           proto->isSpecificProtocol(KnownProtocolKind::AsyncSequence))) {
        // FIXME: We should diagnose that this function should use typed
        // throws instead.

        return PolymorphicEffectKind::AsyncSequenceRethrows;
      }
    }
  }

  for (auto param : *decl->getParameters()) {
    auto interfaceTy = param->getInterfaceType();
    if (hasFunctionParameterWithEffect(kind, interfaceTy)) {
      return PolymorphicEffectKind::ByClosure;
    }
  }

  return PolymorphicEffectKind::Invalid;
}

static bool classifyWitness(ModuleDecl *module, 
                            ProtocolConformance *conformance, 
                            AbstractFunctionDecl *req,
                            EffectKind kind) {
  auto declRef = conformance->getWitnessDeclRef(req);
  if (!declRef) {
    // Invalid conformance.
    return true;
  }

  auto witnessDecl = dyn_cast<AbstractFunctionDecl>(declRef.getDecl());
  if (!witnessDecl) {
    // Enum element constructors do not have effects.
    assert(isa<EnumElementDecl>(declRef.getDecl()));
    return false;
  }

  switch (witnessDecl->getPolymorphicEffectKind(kind)) {
    case PolymorphicEffectKind::None:
      // Witness doesn't have this effect at all, so it contributes nothing.
      return false;

    case PolymorphicEffectKind::AsyncSequenceRethrows: {
      // Witnesses that can only be polymorphic due to an
      // AsyncSequence/AsyncIteratorProtocol conformance don't contribute
      // anything; the thrown error result is captured by the Failure
      // type.
      return false;
    }

    case PolymorphicEffectKind::ByConformance: {
      // Witness has the effect if the concrete type's conformances
      // recursively have the effect.
      auto substitutions = conformance->getSubstitutionMap();
      for (auto conformanceRef : substitutions.getConformances()) {
        if (conformanceRef.hasEffect(kind)) {
          return true;
        }
      }
      return false;
    }

    case PolymorphicEffectKind::ByClosure:
      // Witness only has the effect if a closure argument has the effect,
      // so it contributes nothing to the conformance`s effect.
      return false;

    case PolymorphicEffectKind::Always:
      // Witness always has the effect.

      // If the witness's thrown type is explicitly specified as a type
      // parameter, then check whether the substituted type is `Never`.
      if (kind == EffectKind::Throws) {
        if (Type thrownError = witnessDecl->getThrownInterfaceType()) {
          if (thrownError->hasTypeParameter())
            thrownError = thrownError.subst(declRef.getSubstitutions());
          if (thrownError->isNever())
            return false;
        }
      }
      return true;

    case PolymorphicEffectKind::Invalid:
      // If something was invalid, just assume it has the effect.
      return true;
  }
}

bool ConformanceHasEffectRequest::evaluate(
  Evaluator &evaluator, EffectKind kind,
  ProtocolConformance *conformance) const {
  auto *module = conformance->getDeclContext()->getParentModule();

  llvm::SmallDenseSet<ProtocolConformance *, 2> visited;
  SmallVector<ProtocolConformance *, 2> worklist;

  worklist.push_back(conformance);

  while (!worklist.empty()) {
    auto *current = worklist.back();
    worklist.pop_back();

    if (!visited.insert(current).second)
      continue;

    auto protoDecl = current->getProtocol();

    auto list = protoDecl->getPolymorphicEffectRequirements(kind);
    for (auto req : list.getRequirements()) {
      if (classifyWitness(module, current, req, kind))
        return true;
    }

    for (auto pair : list.getConformances()) {
      auto assocConf = 
          current->getAssociatedConformance(
              pair.first, pair.second);
      if (!assocConf.isConcrete())
        return true;

      worklist.push_back(assocConf.getConcrete());
    }
  }

  return false;
}

/// \returns the getter decl iff its a prop/subscript with an effectful 'get'
static ConcreteDeclRef getEffectfulGetOnlyAccessor(ConcreteDeclRef cdr) {
  if (!cdr)
    return nullptr;

  if (auto storageDecl = dyn_cast<AbstractStorageDecl>(cdr.getDecl())) {
    if (auto getAccessor = storageDecl->getEffectfulGetAccessor())
      return ConcreteDeclRef(getAccessor, cdr.getSubstitutions());
  }

  return nullptr;
}

/// Determine whether this is the "Never" type that's used to indicate that the
/// function never throws.
static bool isNeverThrownError(Type type) {
  if (!type)
    return true;

  return type->isNever();
}

namespace {

/// A function reference.
class AbstractFunction {
public:
  enum Kind : uint8_t {
    Opaque, Function, Closure, Parameter,
  };

private:
  union {
    AbstractFunctionDecl *TheFunction;
    AbstractClosureExpr *TheClosure;
    ParamDecl *TheParameter;
    Expr *TheExpr;
  };
  Kind TheKind;
  PolymorphicEffectKind RethrowsKind = PolymorphicEffectKind::None;
  PolymorphicEffectKind ReasyncKind = PolymorphicEffectKind::None;
  SubstitutionMap Substitutions;

public:
  explicit AbstractFunction(Kind kind, Expr *fn)
    : TheKind(kind) {
    TheExpr = fn;
  }

  explicit AbstractFunction(AbstractFunctionDecl *fn, SubstitutionMap subs)
    : TheKind(Kind::Function),
      RethrowsKind(fn->getPolymorphicEffectKind(EffectKind::Throws)),
      ReasyncKind(fn->getPolymorphicEffectKind(EffectKind::Async)),
      Substitutions(subs) {
    TheFunction = fn;
  }

  explicit AbstractFunction(AbstractClosureExpr *closure)
    : TheKind(Kind::Closure) {
    TheClosure = closure;
  }

  explicit AbstractFunction(ParamDecl *parameter, SubstitutionMap subs)
    : TheKind(Kind::Parameter), Substitutions(subs) {
    TheParameter = parameter;

  }

  Kind getKind() const { return TheKind; }

  PolymorphicEffectKind getPolymorphicEffectKind(EffectKind kind) const {
    switch (kind) {
    case EffectKind::Throws: return RethrowsKind;
    case EffectKind::Async: return ReasyncKind;
    }
    llvm_unreachable("Bad effect kind");
  }

  Type getType() const {
    switch (getKind()) {
    case Kind::Opaque: return getOpaqueFunction()->getType();
    case Kind::Function: {
      auto *AFD = getFunction();
      if (AFD->hasImplicitSelfDecl())
        return AFD->getMethodInterfaceType();
      return AFD->getInterfaceType();
    }
    case Kind::Closure: return getClosure()->getType();
    case Kind::Parameter: return getParameter()->getInterfaceType();
    }
    llvm_unreachable("bad kind");
  }

  /// Retrieve the interface type for a parameter based on an index into the
  /// substituted parameter type. This
  Type getOrigParamInterfaceType(unsigned substIndex) const {
    switch (getKind()) {
    case Kind::Opaque:
    case Kind::Closure:
    case Kind::Parameter:
      return getType()->castTo<AnyFunctionType>()->getParams()[substIndex]
                 .getParameterType();

    case Kind::Function: {
      auto params = getParameterList(static_cast<ValueDecl *>(getFunction()));
      auto origIndex = params->getOrigParamIndex(getSubstitutions(), substIndex);
      return params->get(origIndex)->getInterfaceType();
    }
    }
  }

  bool isAutoClosure() const {
    if (getKind() == Kind::Closure)
      return isa<AutoClosureExpr>(getClosure());
    return false;
  }

  AbstractFunctionDecl *getFunction() const {
    assert(getKind() == Kind::Function);
    return TheFunction;
  }
  AbstractClosureExpr *getClosure() const {
    assert(getKind() == Kind::Closure);
    return TheClosure;
  }
  ParamDecl *getParameter() const {
    assert(getKind() == Kind::Parameter);
    return TheParameter;
  }
  Expr *getOpaqueFunction() const {
    assert(getKind() == Kind::Opaque);
    return TheExpr;
  }

  SubstitutionMap getSubstitutions() const {
    return Substitutions;
  }

  static AbstractFunction getAppliedFn(ApplyExpr *apply) {
    Expr *fn = apply->getFn()->getValueProvidingExpr();

    if (auto *selfCall = dyn_cast<SelfApplyExpr>(fn))
      fn = selfCall->getFn()->getValueProvidingExpr();

    return decomposeFunction(fn);
  }

  bool isPreconcurrency() const {
    switch (getKind()) {
    case Kind::Closure: {
      auto *closure = dyn_cast<ClosureExpr>(getClosure());
      return closure && closure->isIsolatedByPreconcurrency();
    }

    case Kind::Function:
      return getActorIsolation(getFunction()).preconcurrency();

    case Kind::Opaque:
    case Kind::Parameter:
      return false;
    }
  }

  static AbstractFunction decomposeFunction(Expr *fn) {
    assert(fn->getValueProvidingExpr() == fn);

    while (true) {
      // Look through Optional unwraps.
      if (auto conversion = dyn_cast<ForceValueExpr>(fn)) {
        fn = conversion->getSubExpr()->getValueProvidingExpr();
      } else if (auto conversion = dyn_cast<BindOptionalExpr>(fn)) {
        fn = conversion->getSubExpr()->getValueProvidingExpr();
      // Look through optional injections.
      } else if (auto injection = dyn_cast<InjectIntoOptionalExpr>(fn)) {
        fn = injection->getSubExpr()->getValueProvidingExpr();
      // Look through function conversions.
      } else if (auto conversion = dyn_cast<FunctionConversionExpr>(fn)) {
        fn = conversion->getSubExpr()->getValueProvidingExpr();
      // Look through base-ignored qualified references (Module.methodName).
      } else if (auto baseIgnored = dyn_cast<DotSyntaxBaseIgnoredExpr>(fn)) {
        fn = baseIgnored->getRHS();
      // Look through closure capture lists.
      } else if (auto captureList = dyn_cast<CaptureListExpr>(fn)) {
        fn = captureList->getClosureBody();
        // Look through optional evaluations.
      } else if (auto optionalEval = dyn_cast<OptionalEvaluationExpr>(fn)) {
        fn = optionalEval->getSubExpr()->getValueProvidingExpr();
      } else {
        break;
      }
    }
    
    // Constructor delegation.
    if (auto otherCtorDeclRef = dyn_cast<OtherConstructorDeclRefExpr>(fn)) {
      return AbstractFunction(otherCtorDeclRef->getDecl(),
                              otherCtorDeclRef->getDeclRef().getSubstitutions());
    }

    // Normal function references.
    if (auto DRE = dyn_cast<DeclRefExpr>(fn)) {
      ValueDecl *decl = DRE->getDecl();
      if (auto fn = dyn_cast<AbstractFunctionDecl>(decl)) {
        return AbstractFunction(fn, DRE->getDeclRef().getSubstitutions());
      } else if (auto param = dyn_cast<ParamDecl>(decl)) {
        SubstitutionMap subs;
        if (auto genericEnv = param->getDeclContext()->getGenericEnvironmentOfContext())
          subs = genericEnv->getForwardingSubstitutionMap();
        return AbstractFunction(param, subs);
      }

    // Closures.
    } else if (auto closure = dyn_cast<AbstractClosureExpr>(fn)) {
      return AbstractFunction(closure);
    }

    // Everything else is opaque.
    return AbstractFunction(Kind::Opaque, fn);
  }
};

enum ShouldRecurse_t : bool {
  ShouldNotRecurse = false, ShouldRecurse = true
};

/// A CRTP ASTWalker implementation that looks for interesting
/// nodes for effects handling.
template <class Impl>
class EffectsHandlingWalker : public ASTWalker {
  Impl &asImpl() { return *static_cast<Impl*>(this); }
public:
  LazyInitializerWalking getLazyInitializerWalkingBehavior() override {
    return LazyInitializerWalking::InAccessor;
  }

  /// Only look at the expansions for effects checking.
  MacroWalking getMacroWalkingBehavior() const override {
    return MacroWalking::Expansion;
  }

  PreWalkAction walkToDeclPre(Decl *D) override {
    ShouldRecurse_t recurse = ShouldRecurse;
    // Skip the implementations of all local declarations... except
    // PBD.  We should really just have a PatternBindingStmt.
    if (auto ic = dyn_cast<IfConfigDecl>(D)) {
      recurse = asImpl().checkIfConfig(ic);
    } else if (auto patternBinding = dyn_cast<PatternBindingDecl>(D)) {
      if (patternBinding->isAsyncLet())
        recurse = asImpl().checkAsyncLet(patternBinding);
    } else if (auto macroExpansionDecl = dyn_cast<MacroExpansionDecl>(D)) {
      recurse = ShouldRecurse;
    } else {
      recurse = ShouldNotRecurse;
    }
    return Action::VisitNodeIf(bool(recurse));
  }

  PreWalkResult<Expr *> walkToExprPre(Expr *E) override {
    visitExprPre(E);
    ShouldRecurse_t recurse = ShouldRecurse;
    if (isa<ErrorExpr>(E)) {
      asImpl().flagInvalidCode();
    } else if (auto closure = dyn_cast<ClosureExpr>(E)) {
      recurse = asImpl().checkClosure(closure);
    } else if (auto autoclosure = dyn_cast<AutoClosureExpr>(E)) {
      recurse = asImpl().checkAutoClosure(autoclosure);
    } else if (auto awaitExpr = dyn_cast<AwaitExpr>(E)) {
      recurse = asImpl().checkAwait(awaitExpr);
    } else if (auto tryExpr = dyn_cast<TryExpr>(E)) {
      recurse = asImpl().checkTry(tryExpr);
    } else if (auto forceTryExpr = dyn_cast<ForceTryExpr>(E)) {
      recurse = asImpl().checkForceTry(forceTryExpr);
    } else if (auto optionalTryExpr = dyn_cast<OptionalTryExpr>(E)) {
      recurse = asImpl().checkOptionalTry(optionalTryExpr);
    } else if (auto apply = dyn_cast<ApplyExpr>(E)) {
      recurse = asImpl().checkApply(apply);
    } else if (auto lookup = dyn_cast<LookupExpr>(E)) {
      recurse = asImpl().checkLookup(lookup);
    } else if (auto declRef = dyn_cast<DeclRefExpr>(E)) {
      recurse = asImpl().checkDeclRef(declRef);
    } else if (auto interpolated = dyn_cast<InterpolatedStringLiteralExpr>(E)) {
      recurse = asImpl().checkInterpolatedStringLiteral(interpolated);
    } else if (auto macroExpansionExpr = dyn_cast<MacroExpansionExpr>(E)) {
      recurse = ShouldRecurse;
    } else if (auto *SVE = dyn_cast<SingleValueStmtExpr>(E)) {
      recurse = asImpl().checkSingleValueStmtExpr(SVE);
    }
    // Error handling validation (via checkTopLevelEffects) happens after
    // type checking. If an unchecked expression is still around, the code was
    // invalid.
#define UNCHECKED_EXPR(KIND, BASE) \
    else if (isa<KIND##Expr>(E)) return Action::Stop();
#include "swift/AST/ExprNodes.def"

    return Action::VisitNodeIf(bool(recurse), E);
  }

  PreWalkResult<Stmt *> walkToStmtPre(Stmt *S) override {
    ShouldRecurse_t recurse = ShouldRecurse;
    if (auto doCatch = dyn_cast<DoCatchStmt>(S)) {
      recurse = asImpl().checkDoCatch(doCatch);
    } else if (auto thr = dyn_cast<ThrowStmt>(S)) {
      recurse = asImpl().checkThrow(thr);
    } else if (auto forEach = dyn_cast<ForEachStmt>(S)) {
      recurse = asImpl().checkForEach(forEach);
    }
    if (!recurse)
      return Action::SkipNode(S);

    return Action::Continue(S);
  }

  ShouldRecurse_t checkDoCatch(DoCatchStmt *S) {
    auto bodyResult = (S->isSyntacticallyExhaustive()
        ? asImpl().checkExhaustiveDoBody(S)
        : asImpl().checkNonExhaustiveDoBody(S));
    for (auto clause : S->getCatches()) {
      asImpl().checkCatch(clause, bodyResult);
    }
    return ShouldNotRecurse;
  }

  ShouldRecurse_t checkForEach(ForEachStmt *S) {
    return ShouldRecurse;
  }

  void visitExprPre(Expr *expr) { asImpl().visitExprPre(expr); }
};

/// A potential reason why something might have an effect.
class PotentialEffectReason {
public:
  enum class Kind : uint8_t {
    /// The function calls an unconditionally throws/async function.
    Apply,

    /// The function is rethrows/reasync, and it was passed an explicit
    /// argument that was not rethrows/reasync-only in this context.
    ByClosure,

    /// The function is rethrows/reasync, and it was passed a default
    /// argument that was not rethrows/reasync-only in this context.
    ByDefaultClosure,

    /// The function is rethrows/reasync, and it was called with
    /// a throwing conformance as one of its generic arguments.
    ByConformance,

    /// The initializer of an 'async let' can throw.
    AsyncLet,

    /// The function accesses an unconditionally throws/async property.
    PropertyAccess,
    SubscriptAccess
  };

  static StringRef kindToString(Kind k) {
    switch (k) {
      case Kind::Apply:
        return "Apply";
      case Kind::PropertyAccess:
        return "PropertyAccess";
      case Kind::SubscriptAccess:
        return "SubscriptAccess";
      case Kind::ByClosure:
        return "ByClosure";
      case Kind::ByDefaultClosure:
        return "ByDefaultClosure";
      case Kind::ByConformance:
        return "ByConformance";
      case Kind::AsyncLet:
        return "AsyncLet";
    }
  }

private:
  Expr *TheExpression;
  Kind TheKind;

  explicit PotentialEffectReason(Kind kind) : TheKind(kind) {}
public:
  static PotentialEffectReason forApply() {
    return PotentialEffectReason(Kind::Apply);
  }
  static PotentialEffectReason forPropertyAccess() {
    return PotentialEffectReason(Kind::PropertyAccess);
  }
  static PotentialEffectReason forSubscriptAccess() {
    return PotentialEffectReason(Kind::SubscriptAccess);
  }
  static PotentialEffectReason forClosure(Expr *E) {
    PotentialEffectReason result(Kind::ByClosure);
    result.TheExpression = E;
    return result;
  }
  static PotentialEffectReason forDefaultClosure() {
    return PotentialEffectReason(Kind::ByDefaultClosure);
  }
  static PotentialEffectReason forConformance() {
    return PotentialEffectReason(Kind::ByConformance);
  }
  static PotentialEffectReason forAsyncLet() {
    return PotentialEffectReason(Kind::AsyncLet);
  }

  Kind getKind() const { return TheKind; }

  bool hasPolymorphicEffect() const {
    return (getKind() == Kind::ByClosure ||
            getKind() == Kind::ByDefaultClosure ||
            getKind() == Kind::ByConformance);
  }

  /// If this was built with forRethrowsArgument, return the expression.
  Expr *getArgument() const {
    assert(getKind() == Kind::ByClosure);
    return TheExpression;
  }
};

enum class ConditionalEffectKind {
  /// The call/function can't have this effect.
  None,

  /// The call/function can only have this effect if one of the parameters
  /// or conformances in the current context can throw.
  Conditional,

  /// The call/function can have this effect.
  Always,
};

static void simple_display(llvm::raw_ostream &out, ConditionalEffectKind kind) {
  out << "ConditionalEffectKind::";
  switch(kind) {
    case ConditionalEffectKind::None:         out << "None"; return;
    case ConditionalEffectKind::Conditional:  out << "Conditional"; return;
    case ConditionalEffectKind::Always:       out << "Always"; return;
  }
  llvm_unreachable("Bad conditional effect kind");
}

/// Remove the type erasure to an existential error, to extract the
/// underlying error.
static Expr *removeErasureToExistentialError(Expr *expr) {
  Type type = expr->getType();
  if (!type)
    return expr;

  ASTContext &ctx = type->getASTContext();
  if (!ctx.LangOpts.hasFeature(Feature::FullTypedThrows))
    return expr;

  // Look for an outer erasure expression.
  if (auto erasure = dyn_cast<ErasureExpr>(expr)) {
    if (type->isEqual(ctx.getErrorExistentialType()))
      return erasure->getSubExpr();
  }

  return expr;
}
}

bool swift::isRethrowLikeTypedThrows(AbstractFunctionDecl *func) {
  // This notion is only for compatibility in Swift 5 and is disabled
  // when FullTypedThrows is enabled.
  ASTContext &ctx = func->getASTContext();
  if (ctx.LangOpts.hasFeature(Feature::FullTypedThrows))
    return false;

  // It must have a thrown error type...
  auto thrownError = func->getThrownInterfaceType();
  if (!thrownError)
    return false;

  /// ... that is a generic parameter type (call it E)
  auto thrownErrorGP = thrownError->getAs<GenericTypeParamType>();
  if (!thrownErrorGP)
    return false;

  /// ... of the generic function.
  auto genericParams = func->getGenericParams();
  if (!genericParams ||
      thrownErrorGP->getDepth() !=
          genericParams->getParams().front()->getDepth())
    return false;

  // E: Error must be the only conformance requirement on the generic parameter.
  auto genericSig = func->getGenericSignature();
  if (!genericSig)
    return false;

  auto requiredProtocols = genericSig->getRequiredProtocols(thrownErrorGP);
  if (requiredProtocols.size() != 1 ||
      requiredProtocols[0]->getKnownProtocolKind() != KnownProtocolKind::Error)
    return false;

  // Any parameters that are of throwing function type must also throw 'E'.
  for (auto param : *func->getParameters()) {
    auto paramTy = param->getInterfaceType();
    if (auto paramFuncTy = paramTy->getAs<AnyFunctionType>()) {
      if (auto paramThrownErrorTy = paramFuncTy->getEffectiveThrownErrorType())
        if (!(*paramThrownErrorTy)->isEqual(thrownError))
          return false;
    }
  }

  return true;
}

namespace {

/// Determine whether the given rethrows context is only allowed to be
/// rethrowing because of the historically-rethrowing behavior of
/// AsyncSequence and AsyncIteratorProtocol.
static bool isRethrowingDueToAsyncSequence(DeclContext *rethrowsDC) {
  auto rethrowsFunc = dyn_cast<AbstractFunctionDecl>(rethrowsDC);
  if (!rethrowsFunc)
    return false;

  if (rethrowsFunc->getPolymorphicEffectKind(EffectKind::Throws) !=
        PolymorphicEffectKind::AsyncSequenceRethrows)
    return false;

  return true;
}

/// Type-erase the opened archetypes in the given type, if there is one.
static Type typeEraseOpenedArchetypes(Type type) {
  if (!type || !type->hasOpenedExistential())
    return type;

  const OpenedArchetypeType *root = nullptr;
  type.visit([&](Type type) {
    if (auto opened = dyn_cast<OpenedArchetypeType>(type.getPointer())) {
      root = opened->getRoot();
    }
  });

  if (!root)
    return type;

  return constraints::typeEraseOpenedArchetypesWithRoot(type, root);
}

/// A type expressing the result of classifying whether a call or function
/// throws or is async.
class Classification {
  bool IsInvalid = false;  // The AST is malformed.  Don't diagnose.

  bool downgradeToWarning = false;

  // Throwing
  ConditionalEffectKind ThrowKind = ConditionalEffectKind::None;
  std::optional<PotentialEffectReason> ThrowReason;
  Type ThrownError;

  // Async
  ConditionalEffectKind AsyncKind = ConditionalEffectKind::None;
  std::optional<PotentialEffectReason> AsyncReason;

  void print(raw_ostream &out) const {
    out << "{ IsInvalid = " << IsInvalid
        << ", ThrowKind = ";
    
    simple_display(out, ThrowKind);
         
    out << ", ThrowReason = ";
    if (!ThrowReason)
      out << "nil";
    else
      out << PotentialEffectReason::kindToString(ThrowReason->getKind());

    if (ThrownError)
      out << ", ThrownError = " << ThrownError.getString();

    out << ", AsyncKind = ";

    simple_display(out, AsyncKind);

    out << ", AsyncReason = ";
    if (!AsyncReason)
      out << "nil";
    else
      out << PotentialEffectReason::kindToString(AsyncReason->getKind());

    out << " }";
  }
  
public:
  Classification() {}

  /// Whether this classification involves any effects.
  bool hasAnyEffects() const { return hasAsync() || hasThrows(); }

  explicit operator bool() const { return hasAnyEffects(); }

  /// Whether there is an async effect.
  bool hasAsync() const { return AsyncKind != ConditionalEffectKind::None; }

  /// Whether there is a throws effect.
  bool hasThrows() const { return ThrowKind != ConditionalEffectKind::None; }

  /// Return a classification that only retains the async parts of the
  /// given classification.
  Classification onlyAsync() const {
    Classification result(*this);
    result.ThrowKind = ConditionalEffectKind::None;
    result.ThrowReason = std::nullopt;
    result.ThrownError = Type();
    return result;
  }
  
  /// Return a classification that only retains the throwing parts of the
  /// given classification.
  Classification onlyThrowing(std::optional<PotentialEffectReason>
                                  newThrowReason = std::nullopt) const {
    Classification result(*this);
    result.AsyncKind = ConditionalEffectKind::None;
    result.AsyncReason = std::nullopt;

    if (result.hasThrows() && newThrowReason)
      result.ThrowReason = newThrowReason;

    return result;
  }

  /// Return a classification that promotes a typed throws effect to an
  /// untyped throws effect.
  Classification promoteToUntypedThrows() const {
    if (!hasThrows())
      return *this;

    Classification result(*this);
    result.ThrownError = ThrownError->getASTContext().getErrorExistentialType();
    return result;
  }

  /// Return a classification that only retains the parts of this
  /// classification for the requested effect kind.
  Classification onlyEffect(EffectKind kind) const {
    switch (kind) {
    case EffectKind::Async: return onlyAsync();
    case EffectKind::Throws: return onlyThrowing();
    }
  }

  static Classification forConditionalPlaceholder(
      ASTContext &ctx, EffectKind kind,
      PotentialEffectReason reason
  ) {
    switch (kind) {
    case EffectKind::Async:
      return forAsync(ConditionalEffectKind::Conditional, reason);
    case EffectKind::Throws:
      return forThrows(
          ctx.getErrorExistentialType(), ConditionalEffectKind::Conditional,
          reason);
    }
  }

  /// Return a throwing classification.
  static Classification forThrows(Type thrownError,
                                  ConditionalEffectKind conditionalKind,
                                  PotentialEffectReason reason) {
    Classification result;
    if (isNeverThrownError(thrownError))
      return result;

    assert(!thrownError->hasError());

    result.ThrowKind = conditionalKind;
    result.ThrowReason = reason;
    result.ThrownError = typeEraseOpenedArchetypes(thrownError);
    return result;
  }

  /// Return an async classification.
  static Classification forAsync(ConditionalEffectKind conditionalKind,
                                 PotentialEffectReason reason) {
    Classification result;
    result.AsyncKind = conditionalKind;
    result.AsyncReason = reason;
    return result;
  }

  /// Return a classification for a given declaration reference.
  static Classification
  forDeclRef(ConcreteDeclRef declRef, ConditionalEffectKind conditionalKind,
             PotentialEffectReason reason,
             std::optional<EffectKind> onlyEffect = std::nullopt) {
    Classification result;
    bool considerAsync = !onlyEffect || *onlyEffect == EffectKind::Async;
    bool considerThrows = !onlyEffect || *onlyEffect == EffectKind::Throws;

    // Consider functions based on their specified effects.
    if (auto func = dyn_cast<AbstractFunctionDecl>(declRef.getDecl())) {
      if (considerAsync && func->hasAsync()) {
        result.merge(Classification::forAsync(conditionalKind, reason));
      }

      if (considerThrows) {
        if (auto thrownInterfaceType =
                func->getEffectiveThrownErrorType()) {
          Type thrownType =
              thrownInterfaceType->subst(declRef.getSubstitutions());
          result.merge(Classification::forThrows(thrownType,
                                                 conditionalKind,
                                                 reason));
        }
      }

      return result;
    }

    // Consider async let declarations.
    if (auto var = dyn_cast<VarDecl>(declRef.getDecl())) {
      // "Async let" declarations are treated as an asynchronous call
      // (to the underlying task's "get"). If the initializer was throwing,
      // then the access is also treated as throwing.
      if (var->isAsyncLet()) {
        // If the initializer could throw, we will have a 'try' in the
        // application of its autoclosure.
        // FIXME: The type checker should record the thrown error type in
        // the AST.
        bool throws = false;
        if (auto init = var->getParentInitializer()) {
          if (auto await = dyn_cast<AwaitExpr>(init))
            init = await->getSubExpr();
          if (isa<TryExpr>(init))
            throws = true;
        }

        result.merge(Classification::forAsync(
                        ConditionalEffectKind::Always,
                        PotentialEffectReason::forAsyncLet()));
        if (throws) {
          ASTContext &ctx = var->getASTContext();
          result.merge(Classification::forThrows(
                         /*FIXME:*/ctx.getErrorExistentialType(),
                         ConditionalEffectKind::Always,
                         PotentialEffectReason::forAsyncLet()));
        }

        return result;
      }
    }

    return result;
  }

  /// Used when invalid AST was detected.
  static Classification forInvalidCode() {
    Classification result;
    result.IsInvalid = true;
    return result;
  }

  void merge(Classification other) {
    if (other.isInvalid())
      IsInvalid = true;

    if (other.AsyncKind > AsyncKind) {
      AsyncKind = other.AsyncKind;
      AsyncReason = other.AsyncReason;
    }

    if (ThrowKind != ConditionalEffectKind::None ||
        other.ThrowKind != ConditionalEffectKind::None) {
      ThrownError =
          TypeChecker::errorUnion(ThrownError, other.ThrownError, nullptr);
      }

    if (other.ThrowKind > ThrowKind) {
      ThrowKind = other.ThrowKind;
      ThrowReason = other.ThrowReason;
    }
  }

  /// Merge implicitly-added effects.
  void mergeImplicitEffects(ASTContext &ctx,
                            bool implicitlyAsync, bool implicitlyThrows,
                            PotentialEffectReason reason) {
    if (implicitlyAsync) {
      merge(Classification::forAsync(ConditionalEffectKind::Always, reason));
    }

    if (implicitlyThrows) {
      // FIXME: Implicit throwing could be typed?
      merge(Classification::forThrows(ctx.getErrorExistentialType(),
                                      ConditionalEffectKind::Always, reason));
    }
  }

  void limitThrowingKind(ConditionalEffectKind otherKind) {
    ThrowKind = std::min(ThrowKind, otherKind);
  }

  bool isInvalid() const { return IsInvalid; }
  void makeInvalid() { IsInvalid = true; }

  bool shouldDowngradeToWarning() const {
    return downgradeToWarning;
  }
  void setDowngradeToWarning(bool downgrade) {
    downgradeToWarning = downgrade;
  }

  ConditionalEffectKind getConditionalKind(EffectKind kind) const {
    switch (kind) {
    case EffectKind::Throws: return ThrowKind;
    case EffectKind::Async: return AsyncKind;
    }
    llvm_unreachable("Bad effect kind");
  }
  Type getThrownError() const {
    assert(ThrowKind == ConditionalEffectKind::Always ||
           ThrowKind == ConditionalEffectKind::Conditional);
    return ThrownError;
  }
  PotentialEffectReason getThrowReason() const {
    assert(ThrowKind == ConditionalEffectKind::Always ||
           ThrowKind == ConditionalEffectKind::Conditional);
    return *ThrowReason;
  }
  PotentialEffectReason getAsyncReason() const {
    assert(AsyncKind == ConditionalEffectKind::Always ||
           AsyncKind == ConditionalEffectKind::Conditional);
    return *AsyncReason;
  }

#if !defined(NDEBUG) || defined(LLVM_ENABLE_DUMP)
  LLVM_DUMP_METHOD void dump() const { print(llvm::errs()); }
#endif
};


/// A class for collecting information about rethrowing and reasync functions.
class ApplyClassifier {
  /// The key to this cache is a local function decl or closure.
  llvm::DenseMap<AnyFunctionRef, Classification> ThrowsCache;
  llvm::DenseMap<AnyFunctionRef, Classification> AsyncCache;

  /// check the kind of property with an effect to give better diagnostics
  static PotentialEffectReason getKindOfEffectfulProp(ConcreteDeclRef cdr) {
    if (isa<SubscriptDecl>(cdr.getDecl()))
      return PotentialEffectReason::forSubscriptAccess();

    assert(isa<VarDecl>(cdr.getDecl()));
    return PotentialEffectReason::forPropertyAccess();
  }

public:
  ASTContext &Ctx;
  DeclContext *DC = nullptr;
  DeclContext *RethrowsDC = nullptr;
  DeclContext *ReasyncDC = nullptr;

  ApplyClassifier(ASTContext &ctx) : Ctx(ctx) { }

  DeclContext *getPolymorphicEffectDeclContext(EffectKind kind) const {
    switch (kind) {
    case EffectKind::Throws: return RethrowsDC;
    case EffectKind::Async: return ReasyncDC;
    }
  }

  Classification classifyConformance(Type type,
                                     ProtocolConformanceRef conformanceRef,
                                     EffectKind kind) {
    if (conformanceRef.isInvalid())
      return Classification::forInvalidCode();

    auto proto = conformanceRef.getRequirement();
    if (kind == EffectKind::Throws &&
        (proto->isSpecificProtocol(KnownProtocolKind::AsyncSequence) ||
         proto->isSpecificProtocol(
             KnownProtocolKind::AsyncIteratorProtocol))) {
      auto failureAssocType = proto->getAssociatedType(Ctx.Id_Failure);
      if (failureAssocType) {
        // Determine whether the async 'for' loop's thrown error is
        // conditional.
        ConditionalEffectKind conditional;
        if (RethrowsDC && isRethrowingDueToAsyncSequence(RethrowsDC))
          conditional = ConditionalEffectKind::Conditional;
        else
          conditional = ConditionalEffectKind::Always;

        // Use the Failure type witness, when present.
        Type thrownError = conformanceRef.getAssociatedType(
            type, failureAssocType->getDeclaredInterfaceType());
        return Classification::forThrows(
            thrownError, conditional,
            /*FIXME*/PotentialEffectReason::forConformance());
        }
    }

    if (conformanceRef.hasEffect(kind)) {
      assert(kind == EffectKind::Throws); // there is no async
      ASTContext &ctx = conformanceRef.getRequirement()->getASTContext();
      // FIXME: typed throws, if it becomes a thing for conformances
      return Classification::forThrows(
          ctx.getErrorExistentialType(),
          ConditionalEffectKind::Conditional,
          PotentialEffectReason::forConformance());
    }

    return Classification();
  }

  bool isContextPreconcurrency() const {
    if (!DC)
      return false;

    return getActorIsolationOfContext(DC).preconcurrency();
  }

  /// Whether a missing 'await' error on accessing an async var should be
  /// downgraded to a warning.
  ///
  /// Missing 'await' errors are downgraded for synchronous access to isolated
  /// global or static 'let' variables, which was previously accepted in
  /// compiler versions before 5.10, or for declarations marked preconcurrency.
  bool downgradeAsyncAccessToWarning(Decl *decl) {
    if (decl->preconcurrency() || isContextPreconcurrency()) {
      return true;
    }

    if (auto *var = dyn_cast<VarDecl>(decl)) {
      ActorReferenceResult::Options options = std::nullopt;
      ModuleDecl *module;
      if (DC != nullptr) {
        module = DC->getParentModule();
      } else {
        module = var->getDeclContext()->getParentModule();
      }
      if (!isLetAccessibleAnywhere(module, var, options)) {
        return options.contains(ActorReferenceResult::Flags::Preconcurrency);
      }
    }

    return false;
  }

  Classification classifyLookup(LookupExpr *E) {
    auto member = E->getMember();
    if (!member)
      return Classification::forInvalidCode();

    PotentialEffectReason reason =
        PotentialEffectReason::forPropertyAccess();
    Classification classification;
    if (auto getter = getEffectfulGetOnlyAccessor(member)) {
      reason = getKindOfEffectfulProp(member);
      classification = Classification::forDeclRef(
          getter, ConditionalEffectKind::Always, reason);
    } else if (isa<SubscriptExpr>(E) || isa<DynamicSubscriptExpr>(E)) {
      reason = PotentialEffectReason::forSubscriptAccess();
    }

    classification.setDowngradeToWarning(
        downgradeAsyncAccessToWarning(member.getDecl()));

    classification.mergeImplicitEffects(
        member.getDecl()->getASTContext(),
        E->isImplicitlyAsync().has_value(), E->isImplicitlyThrows(),
        reason);

    return classification;
  }

  Classification classifyDeclRef(DeclRefExpr *E) {
    if (!E->getDecl())
      return Classification::forInvalidCode();

    Classification classification;
    PotentialEffectReason reason = PotentialEffectReason::forPropertyAccess();
    ConcreteDeclRef declRef = E->getDeclRef();

    if (auto getter = getEffectfulGetOnlyAccessor(declRef)) {
      reason = getKindOfEffectfulProp(declRef);
      classification = Classification::forDeclRef(
          getter, ConditionalEffectKind::Always, reason);
    } else if (isa<VarDecl>(declRef.getDecl())) {
      // Handle async let.
      reason = PotentialEffectReason::forAsyncLet();
      classification = Classification::forDeclRef(
          declRef, ConditionalEffectKind::Always, reason);
    }

    classification.setDowngradeToWarning(
        downgradeAsyncAccessToWarning(E->getDecl()));

    classification.mergeImplicitEffects(
        E->getDeclRef().getDecl()->getASTContext(),
        E->isImplicitlyAsync().has_value(), E->isImplicitlyThrows(),
        reason);

    return classification;
  }

  Classification classifyThrow(ThrowStmt *S) {
    Expr *thrownValue = S->getSubExpr();
    if (!thrownValue)
      return Classification::forInvalidCode();

    // If we are doing full typed throws, look through an existential
    // conversion to find the underlying type.
    thrownValue = removeErasureToExistentialError(thrownValue);

    Type thrownType = thrownValue->getType();
    if (!thrownType || thrownType->hasError())
      return Classification::forInvalidCode();

    // FIXME: Add a potential effect reason for a throw site.
    return Classification::forThrows(
        thrownType, ConditionalEffectKind::Always,
        PotentialEffectReason::forApply());
  }

  /// Check to see if the given function application throws or is async.
  Classification classifyApply(ApplyExpr *E) {
    // An apply expression is a potential throw site if the function throws.
    // But if the expression didn't type-check, suppress diagnostics.
    if (!E->getType() || E->getType()->hasError())
      return Classification::forInvalidCode();

    if (auto *SAE = dyn_cast<SelfApplyExpr>(E)) {
      assert(!E->isImplicitlyAsync());
    }

    auto type = E->getFn()->getType();
    if (!type) return Classification::forInvalidCode();
    auto fnType = type->getAs<AnyFunctionType>();
    if (!fnType) return Classification::forInvalidCode();

    auto fnRef = AbstractFunction::getAppliedFn(E);
    auto substitutions = fnRef.getSubstitutions();
    const bool hasAnyConformances =
        llvm::any_of(substitutions.getConformances(),
                     [](const ProtocolConformanceRef conformance) {
                       auto *requirement = conformance.getRequirement();
                       return !requirement->getInvertibleProtocolKind();
                     });

    // If the function doesn't have any effects or conformances, we're done
    // here.
    if (!fnType->isThrowing() &&
        !E->implicitlyThrows() &&
        !fnType->isAsync() &&
        !E->isImplicitlyAsync() &&
        !hasAnyConformances) {
      return Classification();
    }

    ASTContext &ctx = type->getASTContext();

    // Decompose the application.
    auto *args = E->getArgs();

    // If any of the arguments didn't type check, fail.
    for (auto arg : *args) {
      auto *argExpr = arg.getExpr();
      if (!argExpr->getType() || argExpr->getType()->hasError())
        return Classification::forInvalidCode();
    }

    Classification result;
    result.mergeImplicitEffects(
        ctx, E->isImplicitlyAsync().has_value(), E->implicitlyThrows(),
        PotentialEffectReason::forApply());

    // Downgrade missing 'await' errors for preconcurrency references.
    result.setDowngradeToWarning(
        result.hasAsync() &&
        (fnRef.isPreconcurrency() || isContextPreconcurrency()));

    auto classifyApplyEffect = [&](EffectKind kind) {
      if (!fnType->hasEffect(kind) &&
          !(kind == EffectKind::Async && E->isImplicitlyAsync()) &&
          !(kind == EffectKind::Throws && E->implicitlyThrows())) {
        return;
      }

      // Handle rethrowing and reasync functions.
      switch (auto polyKind = fnRef.getPolymorphicEffectKind(kind)) {
      case PolymorphicEffectKind::AsyncSequenceRethrows:
      case PolymorphicEffectKind::ByConformance: {
        auto requirements = substitutions.getGenericSignature()
                                .withoutMarkerProtocols()
                                .getRequirements();
        for (const auto &req : requirements) {
          if (req.getKind() != RequirementKind::Conformance)
            continue;

          Type type = req.getFirstType().subst(substitutions);

          auto conformanceRef = substitutions.lookupConformance(
              req.getFirstType()->getCanonicalType(), req.getProtocolDecl());
          assert(conformanceRef);

          result.merge(classifyConformance(type, conformanceRef, kind));
        }

        // 'ByConformance' is a superset of 'ByClosure', so check for
        // closure arguments too.
        LLVM_FALLTHROUGH;
      }

      case PolymorphicEffectKind::Always:
        if (polyKind == PolymorphicEffectKind::ByConformance ||
            polyKind == PolymorphicEffectKind::AsyncSequenceRethrows) {
          LLVM_FALLTHROUGH;
        } else if (RethrowsDC &&
            fnRef.getKind() == AbstractFunction::Function &&
            isRethrowLikeTypedThrows(fnRef.getFunction())) {
          // If we are in a rethrowing context and the function we're referring
          // to is a rethrow-like function using typed throws or we are
          // calling the next() or next(_:) of an async iterator,
          // then look at all of the closure arguments.
          LLVM_FALLTHROUGH;
        } else {
          break;
        }

      case PolymorphicEffectKind::ByClosure: {
        // We need to walk the original parameter types in parallel
        // because it only counts for rethrows/reasync purposes if it
        // lines up with a throws/async function parameter in the
        // original type.
        Type fnInterfaceType = fnRef.getType();
        if (!fnInterfaceType) {
          result.merge(Classification::forInvalidCode());
          return;
        }

        // Use the most significant result from the arguments.
        auto *fnSubstType = fnInterfaceType.subst(fnRef.getSubstitutions())
            ->getAs<AnyFunctionType>();
        if (!fnSubstType)  {
          result.merge(Classification::forInvalidCode());
          return;
        }

        if (fnSubstType->getParams().size() != args->size()) {
          result.merge(Classification::forInvalidCode());
          return;
        }

        for (unsigned i = 0, e = args->size(); i < e; ++i) {
          Type origParamType = fnRef.getOrigParamInterfaceType(i);
          auto argClassification = classifyArgument(
              args->getExpr(i), origParamType, fnRef.getSubstitutions(), kind);

          // Rethrows is untyped, so adjust the thrown error type.
          if (kind == EffectKind::Throws &&
              polyKind == PolymorphicEffectKind::ByClosure) {
            argClassification = argClassification.promoteToUntypedThrows();
          }

          result.merge(argClassification);
        }

        return;
      }

      case PolymorphicEffectKind::None:
      case PolymorphicEffectKind::Invalid:
        break;
      }

      // Try to classify the implementation of functions that we have
      // local knowledge of.
      //
      // An autoclosure callee here only appears in a narrow case where
      // we're in the initializer of an 'async let'.
      // FIXME: handle typed async let
      if (fnRef.isAutoClosure()) {
        switch (kind) {
        case EffectKind::Async:
          result.merge(Classification::forAsync(
                          ConditionalEffectKind::Always,
                          PotentialEffectReason::forApply()));
          break;
        case EffectKind::Throws:
          result.merge(Classification::forThrows(
                         /*FIXME:*/ctx.getErrorExistentialType(),
                         ConditionalEffectKind::Always,
                         PotentialEffectReason::forApply()));
        }
      } else {
        result.merge(
          classifyFunctionBody(fnRef,
                               PotentialEffectReason::forApply(),
                               kind));
      }
    };

    classifyApplyEffect(EffectKind::Throws);
    classifyApplyEffect(EffectKind::Async);

    return result;
  }

  /// Classify a single expression without considering its enclosing context.
  Classification classifyExpr(Expr *expr, EffectKind kind) {
    switch (kind) {
    case EffectKind::Throws: {
      FunctionThrowsClassifier classifier(*this);
      expr->walk(classifier);
      return classifier.classification.onlyThrowing();
    }
    case EffectKind::Async: {
      FunctionAsyncClassifier classifier(*this);
      expr->walk(classifier);
      return Classification::forAsync(
          classifier.AsyncKind, /*FIXME:*/PotentialEffectReason::forApply());
    }
    }
    llvm_unreachable("Bad effect");
  }

  // Classify a single statement without considering its enclosing context.
  Classification classifyStmt(Stmt *stmt, EffectKind kind) {
    switch (kind) {
    case EffectKind::Throws: {
      FunctionThrowsClassifier classifier(*this);
      stmt->walk(classifier);
      return classifier.classification.onlyThrowing();
    }
    case EffectKind::Async: {
      FunctionAsyncClassifier classifier(*this);
      stmt->walk(classifier);
      return Classification::forAsync(
          classifier.AsyncKind, /*FIXME:*/PotentialEffectReason::forApply());
    }
    }
  }

  /// Check to see if the given for-each statement to determine if it
  /// throws or is async.
  Classification classifyForEach(ForEachStmt *stmt) {
    // Only async for-each loops have effects.
    if (!stmt->getAwaitLoc().isValid())
      return Classification();

    // For-each loops with effects are always async.
    Classification result = Classification::forAsync(
        ConditionalEffectKind::Always,
        PotentialEffectReason::forApply());

    if (!stmt->getNextCall())
      return Classification::forInvalidCode();

    // Merge the thrown result from the next/nextElement call.
    result.merge(classifyExpr(stmt->getNextCall(), EffectKind::Throws));

    return result;
  }

private:
  /// Classify a throwing or async function according to our local
  /// knowledge of its implementation.
  Classification
  classifyFunctionBody(const AbstractFunction &fn,
                       PotentialEffectReason reason,
                       EffectKind kind) {
    switch (fn.getKind()) {
    case AbstractFunction::Opaque: {
      return classifyArgumentByType(fn.getType(), fn.getSubstitutions(),
                                    ConditionalEffectKind::Always, reason,
                                    kind);
    }
    case AbstractFunction::Parameter:
      return classifyParameterBody(fn.getParameter(), fn.getSubstitutions(),
                                   reason, kind);
    case AbstractFunction::Function:
      return classifyFunctionBody(fn.getFunction(), fn.getSubstitutions(),
                                  reason, kind);
    case AbstractFunction::Closure:
      return classifyFunctionBody(fn.getClosure(), reason, kind);
    }
    llvm_unreachable("bad abstract function kind");
  }

  Classification classifyParameterBody(ParamDecl *param,
                                       SubstitutionMap subs,
                                       PotentialEffectReason reason,
                                       EffectKind kind) {
    assert(param->getInterfaceType()
               ->lookThroughAllOptionalTypes()
               ->castTo<AnyFunctionType>()
               ->hasEffect(kind) ||
           !param->getInterfaceType()
               ->lookThroughAllOptionalTypes()
               ->castTo<AnyFunctionType>()
               ->getIsolation().isNonIsolated());

    ConditionalEffectKind conditional;

    // If we're currently doing rethrows-checking on the body of the
    // function which declares the parameter, it's rethrowing-only.
    auto *ParentDC = getPolymorphicEffectDeclContext(kind);
    if (ParentDC == param->getDeclContext())
      conditional = ConditionalEffectKind::Conditional;
    else // otherwise, it throws unconditionally.
      conditional = ConditionalEffectKind::Always;

    return classifyArgumentByType(
        param->getInterfaceType(), subs, conditional, reason, kind);
  }

  bool isLocallyDefinedInPolymorphicEffectDeclContext(DeclContext *DC,
                                                      EffectKind kind) {
    auto *ParentDC = getPolymorphicEffectDeclContext(kind);
    if (ParentDC == nullptr)
      return false;

    while (true) {
      assert(DC->isLocalContext());
      if (DC == ParentDC) return true;
      DC = DC->getParent();
      if (!DC->isLocalContext()) return false;
    }
  }

  Classification classifyFunctionBody(AbstractFunctionDecl *fn,
                                      SubstitutionMap subs,
                                      PotentialEffectReason reason,
                                      EffectKind kind) {
    // Functions can't be rethrowing-only unless they're defined
    // within the rethrows context.
    if (!isLocallyDefinedInPolymorphicEffectDeclContext(fn, kind) ||
        !fn->hasBody()) {
      auto conditional = ConditionalEffectKind::Always;

      // If we are within a rethrows context prior, treat some typed-throws
      // functions and async iterator functions as conditionally throwing.
      if (kind == EffectKind::Throws && RethrowsDC &&
          (isRethrowLikeTypedThrows(fn) ||
           isRethrowingDueToAsyncSequence(RethrowsDC)))
        conditional = ConditionalEffectKind::Conditional;

      return Classification::forDeclRef(
          ConcreteDeclRef(fn, subs), conditional, reason)
            .onlyEffect(kind);
    }

    return classifyFunctionBodyImpl(fn, fn->getBody(), /*allowNone*/ false,
                                    kind, reason);
  }

  Classification classifyFunctionBody(AbstractClosureExpr *closure,
                                      PotentialEffectReason reason,
                                      EffectKind kind) {
    bool isAutoClosure = isa<AutoClosureExpr>(closure);

    // Closures can't be rethrowing-only unless they're defined
    // within the rethrows context.
    if (!isAutoClosure &&
        !isLocallyDefinedInPolymorphicEffectDeclContext(closure, kind)) {
      switch (kind) {
      case EffectKind::Async:
        return Classification::forAsync(ConditionalEffectKind::Always, reason);

      case EffectKind::Throws:
        if (auto thrownType = closure->getEffectiveThrownType())
          return Classification::forThrows(*thrownType,
                                           ConditionalEffectKind::Always,
                                           reason);
        return Classification();
      }
    }

    BraceStmt *body;
    if (auto autoclosure = dyn_cast<AutoClosureExpr>(closure)) {
      body = autoclosure->getBody();
    } else {
      body = cast<ClosureExpr>(closure)->getBody();
    }
    if (!body) return Classification::forInvalidCode();

    return classifyFunctionBodyImpl(closure, body, /*allowNone*/ isAutoClosure,
                                    kind, reason);
  }

  class FunctionThrowsClassifier
      : public EffectsHandlingWalker<FunctionThrowsClassifier> {
    ApplyClassifier &Self;
  public:
    Classification classification;

    FunctionThrowsClassifier(ApplyClassifier &self) : Self(self) {}

    void flagInvalidCode() {
      classification.makeInvalid();
    }

    ShouldRecurse_t checkClosure(ClosureExpr *closure) {
      return ShouldNotRecurse;
    }
    ShouldRecurse_t checkAutoClosure(AutoClosureExpr *closure) {
      return ShouldNotRecurse;
    }
    ShouldRecurse_t checkAwait(AwaitExpr *E) {
      return ShouldRecurse;
    }
    ShouldRecurse_t checkTry(TryExpr *E) {
      return ShouldRecurse;
    }
    ShouldRecurse_t checkForceTry(ForceTryExpr *E) {
      return ShouldNotRecurse;
    }
    ShouldRecurse_t checkOptionalTry(OptionalTryExpr *E) {
      return ShouldNotRecurse;
    }
    ShouldRecurse_t checkApply(ApplyExpr *E) {
      classification.merge(Self.classifyApply(E).onlyThrowing());
      return ShouldRecurse;
    }
    ShouldRecurse_t checkLookup(LookupExpr *E) {
      classification.merge(Self.classifyLookup(E).onlyThrowing());
      return ShouldRecurse;
    }
    ShouldRecurse_t checkDeclRef(DeclRefExpr *E) {
      classification.merge(Self.classifyDeclRef(E).onlyThrowing());
      return ShouldNotRecurse;
    }
    ShouldRecurse_t checkAsyncLet(PatternBindingDecl *patternBinding) {
      return ShouldRecurse;
    }
    ShouldRecurse_t checkThrow(ThrowStmt *S) {
      classification.merge(Self.classifyThrow(S).onlyThrowing());
      return ShouldRecurse;
    }
    ShouldRecurse_t checkInterpolatedStringLiteral(InterpolatedStringLiteralExpr *E) {
      return ShouldRecurse;
    }

    ShouldRecurse_t checkIfConfig(IfConfigDecl *D) {
      return ShouldRecurse;
    }

    ShouldRecurse_t checkForEach(ForEachStmt *S) {
      classification.merge(Self.classifyForEach(S));
      return ShouldRecurse;
    }

    ShouldRecurse_t checkSingleValueStmtExpr(SingleValueStmtExpr *SVE) {
      return ShouldRecurse;
    }

    ConditionalEffectKind checkExhaustiveDoBody(DoCatchStmt *S) {
      // All errors thrown by the do body are caught, but any errors thrown
      // by the catch bodies are bounded by the throwing kind of the do body.
      llvm::SaveAndRestore<Classification> savedClassification(
          classification, Classification());
      S->getBody()->walk(*this);
      return classification.getConditionalKind(EffectKind::Throws);
    }

    ConditionalEffectKind checkNonExhaustiveDoBody(DoCatchStmt *S) {
      S->getBody()->walk(*this);
      // Because catch bodies can only be executed if the do body throws an
      // error, and because the do is non-exhaustive, we can skip checking the
      // catch bodies entirely.
      return ConditionalEffectKind::None;
    }

    void checkCatch(CaseStmt *S, ConditionalEffectKind doThrowingKind) {
      if (doThrowingKind != ConditionalEffectKind::None) {
        // This was an exhaustive do body, so bound our throwing kind by its
        // throwing kind.
        auto savedClassification = classification;
        classification = Classification();
        S->getBody()->walk(*this);
        classification.limitThrowingKind(doThrowingKind);
        classification.merge(savedClassification);
      } else {
        // We can skip the catch body, since bounding the result by None is
        // guaranteed to give back None, which leaves our ThrowKind unchanged.
      }
    }

    void visitExprPre(Expr *expr) { return; }
  };

  class FunctionAsyncClassifier
      : public EffectsHandlingWalker<FunctionAsyncClassifier> {
    ApplyClassifier &Self;
  public:
    bool IsInvalid = false;
    ConditionalEffectKind AsyncKind = ConditionalEffectKind::None;
    FunctionAsyncClassifier(ApplyClassifier &self) : Self(self) {}

    void flagInvalidCode() {
      IsInvalid = true;
    }

    ShouldRecurse_t checkClosure(ClosureExpr *closure) {
      return ShouldNotRecurse;
    }
    ShouldRecurse_t checkAutoClosure(AutoClosureExpr *closure) {
      return ShouldNotRecurse;
    }
    ShouldRecurse_t checkAwait(AwaitExpr *E) {
      return ShouldRecurse;
    }
    ShouldRecurse_t checkTry(TryExpr *E) {
      return ShouldRecurse;
    }
    ShouldRecurse_t checkForceTry(ForceTryExpr *E) {
      return ShouldRecurse;
    }
    ShouldRecurse_t checkOptionalTry(OptionalTryExpr *E) {
      return ShouldRecurse;
    }
    ShouldRecurse_t checkApply(ApplyExpr *E) {
      auto classification = Self.classifyApply(E);
      IsInvalid |= classification.isInvalid();
      AsyncKind = std::max(AsyncKind, classification.getConditionalKind(EffectKind::Async));
      return ShouldRecurse;
    }
    ShouldRecurse_t checkLookup(LookupExpr *E) {
      if (E->isImplicitlyAsync()) {
        AsyncKind = ConditionalEffectKind::Always;
      } else if (auto getter = getEffectfulGetOnlyAccessor(E->getMember())) {
        if (cast<AccessorDecl>(getter.getDecl())->hasAsync())
          AsyncKind = ConditionalEffectKind::Always;
      }

      return ShouldRecurse;
    }
    ShouldRecurse_t checkDeclRef(DeclRefExpr *E) {
      if (E->isImplicitlyAsync()) {
        AsyncKind = ConditionalEffectKind::Always;
      } else if (auto getter = getEffectfulGetOnlyAccessor(E->getDeclRef())) {
        if (cast<AccessorDecl>(getter.getDecl())->hasAsync())
          AsyncKind = ConditionalEffectKind::Always;
      }

      return ShouldNotRecurse;
    }
    ShouldRecurse_t checkAsyncLet(PatternBindingDecl *patternBinding) {
      AsyncKind = ConditionalEffectKind::Always;
      return ShouldRecurse;
    }
    ShouldRecurse_t checkThrow(ThrowStmt *E) {
      return ShouldRecurse;
    }
    ShouldRecurse_t checkInterpolatedStringLiteral(InterpolatedStringLiteralExpr *E) {
      return ShouldRecurse;
    }

    ShouldRecurse_t checkIfConfig(IfConfigDecl *D) {
      return ShouldRecurse;
    }

    ShouldRecurse_t checkDoCatch(DoCatchStmt *S) {
      return ShouldRecurse;
    }

    ShouldRecurse_t checkForEach(ForEachStmt *S) {
      if (S->getAwaitLoc().isValid()) {
        AsyncKind = std::max(AsyncKind, ConditionalEffectKind::Always);
      }

      return ShouldRecurse;
    }

    ShouldRecurse_t checkSingleValueStmtExpr(SingleValueStmtExpr *SVE) {
      return ShouldRecurse;
    }

    void visitExprPre(Expr *expr) { return; }
  };

  Classification
  classifyFunctionBodyImpl(AnyFunctionRef key, BraceStmt *body, bool allowNone,
                           EffectKind kind, PotentialEffectReason reason) {
    auto &Cache = (kind == EffectKind::Throws
                   ? ThrowsCache
                   : AsyncCache);

    // Look for the key in the cache.
    auto existingIter = Cache.find(key);
    if (existingIter != Cache.end())
      return existingIter->second;

    // For the purposes of finding a fixed point, consider the
    // function to be rethrowing-only within its body.  Autoclosures
    // aren't recursively referenceable, so their special treatment
    // isn't a problem for this.
    // FIXME: Look up type for typed rethrows?
    Cache.insert({key, Classification::forConditionalPlaceholder(
                          Ctx, kind, reason)});

    // Walk the body.
    Classification result;
    switch (kind) {
    case EffectKind::Throws: {
      FunctionThrowsClassifier classifier(*this);
      body->walk(classifier);
      result = classifier.classification.onlyThrowing(reason);
      break;
    }
    case EffectKind::Async: {
      FunctionAsyncClassifier classifier(*this);
      body->walk(classifier);
      if (classifier.IsInvalid)
        result = Classification::forInvalidCode();
      else {
        result = Classification::forAsync(classifier.AsyncKind, reason);
      }
      break;
    }
    }

    // The body result cannot be 'none' unless it's an autoclosure.
    // FIXME: This actually allows us to accept always-throwing closures
    // within rethrows functions, which is tracked by SR-680
    // (https://bugs.swift.org/browse/SR-680). That can be fixed by adding
    // the following condition:
    // \code
    // result.getConditionalKind(kind) == ConditionalEffectKind::None
    // \endcode
    if (!result.isInvalid() && !allowNone) {
      result = Classification::forConditionalPlaceholder(Ctx, kind, reason);
    }

    // Remember the result.
    Cache[key] = result;
    return result;
  }

  /// Classify an argument being passed to a rethrows/reasync function.
  Classification classifyArgument(
      Expr *arg, Type paramType, SubstitutionMap subs, EffectKind kind) {
    arg = arg->getValueProvidingExpr();

    if (auto *defaultArg = dyn_cast<DefaultArgumentExpr>(arg)) {
      // Special-case a 'nil' default argument, which is known not to throw.
      if (defaultArg->isCallerSide()) {
        auto *callerSideArg = defaultArg->getCallerSideDefaultExpr();
        if (isa<NilLiteralExpr>(callerSideArg)) {
          if (callerSideArg->getType()->getOptionalObjectType())
            return Classification();
        }
      }

      return classifyArgumentByType(arg->getType(), subs,
                                    ConditionalEffectKind::Always,
                                    PotentialEffectReason::forDefaultClosure(),
                                    kind);
    }

    // If this argument is `nil` literal, it doesn't cause the call to throw.
    if (isa<NilLiteralExpr>(arg)) {
      if (arg->getType()->getOptionalObjectType())
        return Classification();
    }

    // Neither does 'Optional<T>.none'.
    if (auto *DSCE = dyn_cast<DotSyntaxCallExpr>(arg)) {
      if (auto *DE = dyn_cast<DeclRefExpr>(DSCE->getFn())) {
        auto &ctx = paramType->getASTContext();
        if (DE->getDecl() == ctx.getOptionalNoneDecl())
          return Classification();
      }
    }

    // If the parameter was structurally a tuple, try to look through the
    // various tuple operations.
    if (auto paramTupleType = dyn_cast<TupleType>(paramType.getPointer())) {
      if (auto tuple = dyn_cast<TupleExpr>(arg)) {
        return classifyTupleArgument(tuple, paramTupleType, subs, kind);
      }

      if (paramTupleType->getNumElements() != 1) {
        // Otherwise, we're passing an opaque tuple expression, and we
        // should treat it as contributing to 'rethrows' if the original
        // parameter type included a throwing function type.
        return classifyArgumentByType(
                                    paramType,
                                    subs,
                                    ConditionalEffectKind::Always,
                                    PotentialEffectReason::forClosure(arg),
                                    kind);
      }

      // FIXME: There's a case where we can end up with an ApplyExpr that
      // has a single-element-tuple argument type, but the argument is just
      // a ClosureExpr and not a TupleExpr.
      paramType = paramTupleType->getElementType(0);
    }

    // Otherwise, if the original parameter type was not a throwing
    // function type, it does not contribute to 'rethrows'.
    auto paramFnType = paramType->lookThroughAllOptionalTypes()->getAs<AnyFunctionType>();
    if (!paramFnType || !paramFnType->hasEffect(kind))
      return Classification();

    PotentialEffectReason reason = PotentialEffectReason::forClosure(arg);

    // TODO: partial applications?

    // Decompose the function reference, then consider the type
    // of the decomposed function.
    AbstractFunction fn = AbstractFunction::decomposeFunction(arg);

    // If it doesn't have function type, we must have invalid code.
    Type argType = fn.getType();
    if (!argType) return Classification::forInvalidCode();

    auto argFnType =
        argType->lookThroughAllOptionalTypes()->getAs<AnyFunctionType>();
    if (!argFnType) return Classification::forInvalidCode();

    // If it doesn't throw, this argument does not cause the call to throw.
    if (!argFnType->hasEffect(kind))
      return Classification();

    // Otherwise, classify the function implementation.
    return classifyFunctionBody(fn, reason, kind);
  }

  /// Classify an argument to a rethrows/reasync function that's a tuple literal.
  Classification classifyTupleArgument(TupleExpr *tuple,
                                       TupleType *paramTupleType,
                                       SubstitutionMap subs,
                                       EffectKind kind) {
    if (paramTupleType->getNumElements() != tuple->getNumElements())
      return Classification::forInvalidCode();

    Classification result;
    for (unsigned i : indices(tuple->getElements())) {
      result.merge(classifyArgument(tuple->getElement(i),
                                    paramTupleType->getElementType(i),
                                    subs, kind));
    }
    return result;
  }

  /// Given the type of an argument, try to determine if it contains
  /// a throws/async function in a way that is permitted to cause a
  /// rethrows/reasync function to throw/async.
  static Classification
  classifyArgumentByType(Type paramType, SubstitutionMap subs,
                         ConditionalEffectKind conditional,
                         PotentialEffectReason reason, EffectKind kind) {
    if (!paramType || paramType->hasError())
      return Classification::forInvalidCode();

    // Look through Optional types.
    paramType = paramType->lookThroughAllOptionalTypes();

    // Consider function types.
    if (auto fnType = paramType->getAs<AnyFunctionType>()) {
      switch (kind) {
      case EffectKind::Async:
        if (fnType->isAsync())
          return Classification::forAsync(conditional, reason);

        return Classification();

      case EffectKind::Throws:
        if (auto thrownError = fnType->getEffectiveThrownErrorType()) {
          Type thrown = *thrownError;
          if (subs)
            thrown = thrown.subst(subs);
          return Classification::forThrows(thrown, conditional, reason);
        }

        return Classification();
      }
    }

    // Look through tuples.
    if (auto tuple = paramType->getAs<TupleType>()) {
      Classification result;

      for (auto eltType : tuple->getElementTypes()) {
        result.merge(
            classifyArgumentByType(eltType, subs, conditional, reason, kind));
      }

      return result;
    }

    return Classification();
  }
};

/// An context in which effects might be handled.
class Context {
public:
  enum class Kind : uint8_t {
    /// A context that potentially handles errors or async calls.
    PotentiallyHandled,

    /// A default argument expression.
    DefaultArgument,

    /// A property wrapper initialization expression.
    PropertyWrapper,

    /// The initializer for an instance variable.
    IVarInitializer,

    /// The initializer for a global variable.
    GlobalVarInitializer,

    /// The initializer for a `lazy` variable.
    LazyVarInitializer,

    /// The initializer for an enum element.
    EnumElementInitializer,

    /// The pattern of a catch.
    CatchPattern,

    /// The guard expression controlling a catch.
    CatchGuard,

    /// A defer body
    DeferBody,
  };

private:
  static Context getContextForPatternBinding(PatternBindingDecl *pbd) {
    auto *var = pbd->getSingleVar();

    if (!pbd->isStatic() && pbd->getDeclContext()->isTypeContext()) {
      return Context(Kind::IVarInitializer, pbd->getDeclContext());
    } else if (var && var->getAttrs().hasAttribute<LazyAttr>()) {
      return Context(Kind::LazyVarInitializer, pbd->getDeclContext());
    } else {
      return Context(Kind::GlobalVarInitializer, pbd->getDeclContext());
    }
  }

  Kind TheKind;
  std::optional<AnyFunctionRef> Function;
  DeclContext *DC;
  bool HandlesErrors = false;
  bool HandlesAsync = false;

  /// Whether error-handling queries should ignore the function context, e.g.,
  /// for autoclosure and rethrows checks.
  bool ErrorHandlingIgnoresFunction = false;
  bool IsNonExhaustiveCatch = false;
  bool DiagnoseErrorOnTry = false;
  InterpolatedStringLiteralExpr *InterpolatedString = nullptr;

  explicit Context(Kind kind, DeclContext *dc)
      : TheKind(kind), Function(std::nullopt), DC(dc), HandlesErrors(false) {
    assert(TheKind != Kind::PotentiallyHandled);
  }

  explicit Context(bool handlesErrors, bool handlesAsync,
                   std::optional<AnyFunctionRef> function, DeclContext *dc)
      : TheKind(Kind::PotentiallyHandled), Function(function), DC(dc),
        HandlesErrors(handlesErrors), HandlesAsync(handlesAsync) {}

public:
  bool shouldDiagnoseErrorOnTry() const {
    return DiagnoseErrorOnTry;
  }
  void setDiagnoseErrorOnTry(bool b) {
    DiagnoseErrorOnTry = b;
  }

  /// Return true when the current context is under an interpolated string
  bool isWithinInterpolatedString() const {
    return InterpolatedString != nullptr;
  }

  /// Stores the location of the innermost await
  SourceLoc awaitLoc = SourceLoc();

  /// Whether this is a function that rethrows.
  bool hasPolymorphicEffect(EffectKind kind) const {
    if (!Function)
      return false;

    auto fn = Function->getAbstractFunctionDecl();
    if (!fn)
      return false;

    switch (kind) {
    case EffectKind::Throws:
      if (!HandlesErrors)
        return false;

      if (ErrorHandlingIgnoresFunction)
        return false;

      break;

    case EffectKind::Async:
      if (!HandlesAsync)
        return false;

      break;
    }

    switch (fn->getPolymorphicEffectKind(kind)) {
    case PolymorphicEffectKind::ByClosure:
    case PolymorphicEffectKind::ByConformance:
    case PolymorphicEffectKind::AsyncSequenceRethrows:
      return true;

    case PolymorphicEffectKind::None:
    case PolymorphicEffectKind::Always:
    case PolymorphicEffectKind::Invalid:
      return false;
    }

    llvm_unreachable("Bad polymorphic effect kind");
  }

  /// Whether this is an autoclosure.
  bool isAutoClosure() const {
    if (!Function)
      return false;

    if (ErrorHandlingIgnoresFunction)
      return false;

    auto closure = Function->getAbstractClosureExpr();
    if (!closure)
      return false;

    return isa<AutoClosureExpr>(closure);
  }

  static Context forTopLevelCode(TopLevelCodeDecl *D) {
    // Top-level code implicitly handles errors.
    return Context(/*handlesErrors=*/true,
                   /*handlesAsync=*/D->isAsyncContext(), std::nullopt, D);
  }

  static Context forFunction(AbstractFunctionDecl *D) {
    // HACK: If the decl is the synthesized getter for a 'lazy' property, then
    // treat the context as a property initializer in order to produce a better
    // diagnostic; the only code we should be diagnosing on is within the
    // initializer expression that has been transplanted from the var's pattern
    // binding decl. We don't perform the analysis on the initializer while it's
    // still a part of that PBD, as it doesn't get a solution applied there.
    if (auto *accessor = dyn_cast<AccessorDecl>(D)) {
      if (auto *var = dyn_cast<VarDecl>(accessor->getStorage())) {
        if (accessor->isGetter() && var->getAttrs().hasAttribute<LazyAttr>()) {
          auto *pbd = var->getParentPatternBinding();
          assert(pbd && "lazy var didn't have a pattern binding decl");
          return getContextForPatternBinding(pbd);
        }
      }
    }

    return Context(D->hasThrows(), D->isAsyncContext(), AnyFunctionRef(D), D);
  }

  static Context forDeferBody(DeclContext *dc) {
    return Context(Kind::DeferBody, dc);
  }

  static Context forInitializer(Initializer *init) {
    if (isa<DefaultArgumentInitializer>(init)) {
      return Context(Kind::DefaultArgument, init);
    }

    if (isa<PropertyWrapperInitializer>(init)) {
      return Context(Kind::PropertyWrapper, init);
    }

    auto *binding = cast<PatternBindingInitializer>(init)->getBinding();
    assert(!binding->getDeclContext()->isLocalContext() &&
           "setting up error context for local pattern binding?");
    return getContextForPatternBinding(binding);
  }

  static Context forDefaultArgument(DeclContext *dc) {
    return Context(Kind::DefaultArgument, dc);
  }

  static Context forEnumElementInitializer(EnumElementDecl *elt) {
    return Context(Kind::EnumElementInitializer, elt);
  }

  static Context forClosure(AbstractClosureExpr *E) {
    // Determine whether the closure has throwing function type.
    bool closureTypeThrows = true;
    bool closureTypeIsAsync = true;
    if (auto closureType = E->getType()) {
      if (auto fnType = closureType->getAs<AnyFunctionType>()) {
        closureTypeThrows = fnType->isThrowing();
        closureTypeIsAsync = fnType->isAsync();
      }
    }

    return Context(closureTypeThrows, closureTypeIsAsync, AnyFunctionRef(E), E);
  }

  static Context forCatchPattern(CaseStmt *S, DeclContext *dc) {
    return Context(Kind::CatchPattern, dc);
  }

  static Context forCatchGuard(CaseStmt *S, DeclContext *dc) {
    return Context(Kind::CatchGuard, dc);
  }

  static Context forPatternBinding(PatternBindingDecl *binding) {
    return getContextForPatternBinding(binding);
  }

  Context withInterpolatedString(InterpolatedStringLiteralExpr *E) const {
    Context copy = *this;
    copy.InterpolatedString = E;
    return copy;
  }

  /// Form a subcontext that handles all errors, e.g., for the body of a
  /// do-catch with exhaustive catch clauses.
  Context withHandlesErrors() const {
    Context copy = *this;
    copy.HandlesErrors = true;
    copy.ErrorHandlingIgnoresFunction = true;
    return copy;
  }

  Kind getKind() const { return TheKind; }

  DeclContext *getDeclContext() const { return DC; }

  bool handlesThrows(ConditionalEffectKind errorKind) const {
    switch (errorKind) {
    case ConditionalEffectKind::None:
      return true;

    // A call that's rethrowing-only can be handled by 'rethrows'.
    case ConditionalEffectKind::Conditional:
      return HandlesErrors;

    // An operation that always throws can only be handled by an
    // all-handling context.
    case ConditionalEffectKind::Always:
      return HandlesErrors && !hasPolymorphicEffect(EffectKind::Throws);
    }
    llvm_unreachable("bad error kind");
  }

  bool handlesAsync(ConditionalEffectKind errorKind) const {
    switch (errorKind) {
    case ConditionalEffectKind::None:
      return true;

    // A call that's rethrowing-only can be handled by 'rethrows'.
    case ConditionalEffectKind::Conditional:
      return HandlesAsync;

    // An operation that always throws can only be handled by an
    // all-handling context.
    case ConditionalEffectKind::Always:
      return HandlesAsync && !hasPolymorphicEffect(EffectKind::Async);
    }
    llvm_unreachable("bad error kind");
  }

  DeclContext *getPolymorphicEffectDeclContext(EffectKind kind) const {
    if (!hasPolymorphicEffect(kind))
      return nullptr;

    return Function->getAbstractFunctionDecl();
  }

  InterpolatedStringLiteralExpr * getInterpolatedString() const {
    return InterpolatedString;
  }

  void setNonExhaustiveCatch(bool value) {
    IsNonExhaustiveCatch = value;
  }

  static void maybeAddRethrowsNote(DiagnosticEngine &Diags, SourceLoc loc,
                                   const PotentialEffectReason &reason) {
    switch (reason.getKind()) {
    case PotentialEffectReason::Kind::Apply:
    case PotentialEffectReason::Kind::PropertyAccess:
    case PotentialEffectReason::Kind::SubscriptAccess:
    case PotentialEffectReason::Kind::AsyncLet:
      // Already fully diagnosed.
      return;
    case PotentialEffectReason::Kind::ByClosure:
      Diags.diagnose(reason.getArgument()->getLoc(),
                     diag::because_rethrows_argument_throws);
      return;
    case PotentialEffectReason::Kind::ByDefaultClosure:
      Diags.diagnose(loc, diag::because_rethrows_default_argument_throws);
      return;
    case PotentialEffectReason::Kind::ByConformance:
      Diags.diagnose(loc, diag::because_rethrows_conformance_throws);
      return;
    }
    llvm_unreachable("bad reason kind");
  }

  /// get a user-friendly name for the source of the effect
  static StringRef getEffectSourceName(const PotentialEffectReason &reason) {
    switch (reason.getKind()) {
    case PotentialEffectReason::Kind::Apply:
    case PotentialEffectReason::Kind::ByClosure:
    case PotentialEffectReason::Kind::ByDefaultClosure:
    case PotentialEffectReason::Kind::ByConformance:
    case PotentialEffectReason::Kind::AsyncLet: // FIXME: not really the right name?
      return "call";

    case PotentialEffectReason::Kind::PropertyAccess:
      return "property access";
    case PotentialEffectReason::Kind::SubscriptAccess:
      return "subscript access";
    }
  }

  void diagnoseUncoveredThrowSite(ASTContext &ctx, ASTNode E,
                                  const Classification &classification) {
    auto &Diags = ctx.Diags;
    auto message = diag::throwing_call_without_try;
    const auto &reason = classification.getThrowReason();
    auto reasonKind = reason.getKind();

    bool suggestTryFixIt = reasonKind == PotentialEffectReason::Kind::Apply;

    if (reasonKind == PotentialEffectReason::Kind::AsyncLet) {
      message = diag::throwing_async_let_without_try;

    } else if (reasonKind == PotentialEffectReason::Kind::PropertyAccess) {
      message = diag::throwing_prop_access_without_try;
      suggestTryFixIt = true;

    } else if (reasonKind == PotentialEffectReason::Kind::SubscriptAccess) {
      message = diag::throwing_subscript_access_without_try;
      suggestTryFixIt = true;
    }

    auto loc = E.getStartLoc();
    SourceLoc insertLoc;
    SourceRange highlight;

    // Generate more specific messages in some cases.
    if (auto e = dyn_cast_or_null<ApplyExpr>(E.dyn_cast<Expr*>())) {
      if (isa<PrefixUnaryExpr>(e) || isa<PostfixUnaryExpr>(e) ||
          isa<BinaryExpr>(e)) {
        loc = e->getFn()->getStartLoc();
        message = diag::throwing_operator_without_try;
      }
      insertLoc = loc;
      highlight = e->getSourceRange();

      if (InterpolatedString &&
          e->getCalledValue() &&
          e->getCalledValue()->getBaseName() ==
          ctx.Id_appendInterpolation) {
        message = diag::throwing_interpolation_without_try;
        insertLoc = InterpolatedString->getLoc();
      }
    }

    Diags.diagnose(loc, message).highlight(highlight)
      .warnUntilSwiftVersionIf(classification.shouldDowngradeToWarning(), 6);
    maybeAddRethrowsNote(Diags, loc, reason);

    // If this is a call without expected 'try[?|!]', like this:
    //
    // func foo() throws {}
    // [let _ = ]foo()
    //
    // Let's suggest couple of alternative fix-its
    // because complete context is unavailable.
    if (!suggestTryFixIt)
      return;

    // 'try' should go before 'await'
    if (awaitLoc.isValid())
      insertLoc = awaitLoc;

    Diags.diagnose(loc, diag::note_forgot_try)
        .fixItInsert(insertLoc, "try ");
    Diags.diagnose(loc, diag::note_error_to_optional)
        .fixItInsert(insertLoc, "try? ");
    Diags.diagnose(loc, diag::note_disable_error_propagation)
        .fixItInsert(insertLoc, "try! ");
  }

  void diagnoseThrowInLegalContext(DiagnosticEngine &Diags, ASTNode node,
                                   bool isTryCovered,
                                   const PotentialEffectReason &reason,
                                   Diag<StringRef> diagForThrowingCall,
                                   Diag<StringRef> diagForTrylessThrowingCall) {
    auto loc = node.getStartLoc();

    // Allow the diagnostic to fire on the 'try' if we don't have
    // anything else to say.
    if (isTryCovered &&
        !reason.hasPolymorphicEffect() &&
        !hasPolymorphicEffect(EffectKind::Throws) &&
        !isAutoClosure()) {
      DiagnoseErrorOnTry = true;
      return;
    }

    auto effectSource = getEffectSourceName(reason);

    if (isTryCovered) {
      Diags.diagnose(loc, diagForThrowingCall, effectSource);
    } else {
      Diags.diagnose(loc, diagForTrylessThrowingCall, effectSource);
    }
    maybeAddRethrowsNote(Diags, loc, reason);
  }

  void diagnoseUnhandledThrowSite(DiagnosticEngine &Diags, ASTNode E,
                                  bool isTryCovered,
                                  const PotentialEffectReason &reason) {
    switch (getKind()) {
    case Kind::PotentiallyHandled:
      if (IsNonExhaustiveCatch) {
        diagnoseThrowInLegalContext(Diags, E, isTryCovered, reason,
                                    diag::throwing_call_in_nonexhaustive_catch,
                            diag::tryless_throwing_call_in_nonexhaustive_catch);
        return;
      }

      if (isAutoClosure()) {
        diagnoseThrowInLegalContext(Diags, E, isTryCovered, reason,
                              diag::throwing_call_in_nonthrowing_autoclosure,
                      diag::tryless_throwing_call_in_nonthrowing_autoclosure);
        return;
      }

      if (hasPolymorphicEffect(EffectKind::Throws)) {
        diagnoseThrowInLegalContext(Diags, E, isTryCovered, reason,
                                    diag::throwing_call_in_rethrows_function,
                            diag::tryless_throwing_call_in_rethrows_function);
        return;
      }

      diagnoseThrowInLegalContext(Diags, E, isTryCovered, reason,
                                  diag::throwing_call_unhandled,
                                  diag::tryless_throwing_call_unhandled);
      return;

    case Kind::EnumElementInitializer:
    case Kind::GlobalVarInitializer:
    case Kind::LazyVarInitializer:
    case Kind::IVarInitializer:
    case Kind::DefaultArgument:
    case Kind::PropertyWrapper:
    case Kind::CatchPattern:
    case Kind::CatchGuard:
    case Kind::DeferBody:
      Diags.diagnose(E.getStartLoc(), diag::throwing_op_in_illegal_context,
                 static_cast<unsigned>(getKind()), getEffectSourceName(reason));
      return;
    }
    llvm_unreachable("bad context kind");
  }

  void diagnoseUnhandledThrowStmt(DiagnosticEngine &Diags, Stmt *S) {
    switch (getKind()) {
    case Kind::PotentiallyHandled:
      if (IsNonExhaustiveCatch) {
        Diags.diagnose(S->getStartLoc(), diag::throw_in_nonexhaustive_catch);
        return;
      }

      if (isAutoClosure()) {
        Diags.diagnose(S->getStartLoc(), diag::throw_in_nonthrowing_autoclosure);
        return;
      }

      if (hasPolymorphicEffect(EffectKind::Throws)) {
        Diags.diagnose(S->getStartLoc(), diag::throw_in_rethrows_function);
        return;
      }

      Diags.diagnose(S->getStartLoc(), diag::throw_in_nonthrowing_function);
      return;

    case Kind::EnumElementInitializer:
    case Kind::GlobalVarInitializer:
    case Kind::LazyVarInitializer:
    case Kind::IVarInitializer:
    case Kind::DefaultArgument:
    case Kind::PropertyWrapper:
    case Kind::CatchPattern:
    case Kind::CatchGuard:
    case Kind::DeferBody:
      Diags.diagnose(S->getStartLoc(), diag::throw_in_illegal_context,
                     static_cast<unsigned>(getKind()));
      return;
    }
    llvm_unreachable("bad context kind");
  }

  void diagnoseUnhandledTry(DiagnosticEngine &Diags, TryExpr *E) {
    switch (getKind()) {
    case Kind::PotentiallyHandled:
      if (DiagnoseErrorOnTry) {
        Diags.diagnose(
            E->getTryLoc(),
            IsNonExhaustiveCatch ? diag::try_unhandled_in_nonexhaustive_catch
                                 : diag::try_unhandled);
      }
      return;

    case Kind::EnumElementInitializer:
    case Kind::GlobalVarInitializer:
    case Kind::LazyVarInitializer:
    case Kind::IVarInitializer:
    case Kind::DefaultArgument:
    case Kind::PropertyWrapper:
    case Kind::CatchPattern:
    case Kind::CatchGuard:
    case Kind::DeferBody:
      assert(!DiagnoseErrorOnTry);
      // Diagnosed at the call sites.
      return;
    }
    llvm_unreachable("bad context kind");
  }
  /// I did not want to add 'await' as a PotentialEffectReason, since it's
  /// not actually an effect. So, we have this odd boolean hanging around.
  unsigned effectReasonToIndex(std::optional<PotentialEffectReason> maybeReason,
                               bool forAwait = false) {
    // while not actually an effect, in some instances we diagnose the
    // appearance of an await within a non-async context.
    if (forAwait)
      return 2;

    if (!maybeReason.has_value())
      return 0; // Unspecified

    switch(maybeReason.value().getKind()) {
    case PotentialEffectReason::Kind::ByClosure:
    case PotentialEffectReason::Kind::ByDefaultClosure:
    case PotentialEffectReason::Kind::ByConformance:
    case PotentialEffectReason::Kind::Apply:
      return 1;

    case PotentialEffectReason::Kind::AsyncLet:
      return 3;

    case PotentialEffectReason::Kind::PropertyAccess:
      return 4;

    case PotentialEffectReason::Kind::SubscriptAccess:
      return 5;
    }
  }

  void diagnoseAsyncInIllegalContext(DiagnosticEngine &Diags, ASTNode node) {
    if (auto *e = node.dyn_cast<Expr*>()) {
      if (isa<ApplyExpr>(e)) {
        Diags.diagnose(e->getLoc(), diag::async_call_in_illegal_context,
                       static_cast<unsigned>(getKind()));
        return;
      }

      if (auto declRef = dyn_cast<DeclRefExpr>(e)) {
        if (auto var = dyn_cast<VarDecl>(declRef->getDecl())) {
          if (var->isAsyncLet()) {
            Diags.diagnose(
                e->getLoc(), diag::async_let_in_illegal_context,
                var, static_cast<unsigned>(getKind()));
            return;
          }
        }
      }
    } else if (auto patternBinding = dyn_cast_or_null<PatternBindingDecl>(
                   node.dyn_cast<Decl *>())) {
      if (patternBinding->isAsyncLet()) {
        Diags.diagnose(patternBinding->getLoc(),
                       diag::async_let_binding_illegal_context,
                       static_cast<unsigned>(getKind()));
        return;
      }
    }

    Diags.diagnose(node.getStartLoc(), diag::await_in_illegal_context,
                   static_cast<unsigned>(getKind()));
  }

  void maybeAddAsyncNote(DiagnosticEngine &Diags) {
    if (!Function)
      return;

    if (auto func = Function->getAbstractFunctionDecl())
      addAsyncNotes(func);
  }

  /// providing a \c kind helps tailor the emitted message.
  void diagnoseUnhandledAsyncSite(DiagnosticEngine &Diags, ASTNode node,
                             std::optional<PotentialEffectReason> maybeReason,
                             bool forAwait = false) {
    if (node.isImplicit()) {
      // The reason we return early on implicit nodes is that sometimes we
      // inject implicit closures, e.g. in 'async let' and we'd end up
      // "double reporting" some errors, with no great way to make sure the
      // "more specific diagnostic" is emitted. So instead, we avoid emitting
      // about implicit code.
      //
      // Some synthesized code, like macros, are NOT marked implicit, so we will
      // report about errors in them properly.
      return;
    }

    switch (getKind()) {
    case Kind::PotentiallyHandled: {
      Diags.diagnose(node.getStartLoc(), diag::async_in_nonasync_function,
                     effectReasonToIndex(maybeReason, forAwait),
                     isAutoClosure());
      maybeAddAsyncNote(Diags);
      return;
    }

    case Kind::EnumElementInitializer:
    case Kind::GlobalVarInitializer:
    case Kind::LazyVarInitializer:
    case Kind::IVarInitializer:
    case Kind::DefaultArgument:
    case Kind::PropertyWrapper:
    case Kind::CatchPattern:
    case Kind::CatchGuard:
    case Kind::DeferBody:
      diagnoseAsyncInIllegalContext(Diags, node);
      return;
    }
  }

};

/// A class to walk over a local context and validate the correctness
/// of its error coverage.
class CheckEffectsCoverage : public EffectsHandlingWalker<CheckEffectsCoverage> {
  friend class EffectsHandlingWalker<CheckEffectsCoverage>;

  ASTContext &Ctx;

  DeclContext *RethrowsDC = nullptr;
  DeclContext *ReasyncDC = nullptr;
  Context CurContext;

  class ContextFlags {
  public:
    enum ContextFlag : unsigned {
      /// Is the current context considered 'try'-covered?
      IsTryCovered = 0x1,

      /// Is the current context within a 'try' expression?
      IsInTry = 0x2,

      /// Is the current context top-level in a debugger function?  This
      /// causes 'try' suppression to apply recursively within a single
      /// level of do/catch.
      IsTopLevelDebuggerFunction = 0x4,

      /// Do we have any throw site in this context?
      HasAnyThrowSite = 0x8,

      /// Do we have a throw site using 'try' in this context?
      HasTryThrowSite = 0x10,

      /// Are we in the context of an 'await'?
      IsAsyncCovered = 0x20,

      /// Do we have any calls to 'async' functions in this context?
      HasAnyAsyncSite = 0x40,

      /// Do we have any 'await's in this context?
      HasAnyAwait = 0x80,

      /// Are we in an 'async let' initializer context?
      InAsyncLet = 0x100,

      /// Does an enclosing 'if' or 'switch' expr have a 'try'?
      StmtExprCoversTry = 0x200,

      /// Does an enclosing 'if' or 'switch' expr have an 'await'?
      StmtExprCoversAwait = 0x400,
    };
  private:
    unsigned Bits;
  public:
    ContextFlags() : Bits(0) {}

    void reset() { Bits = 0; }
    bool has(ContextFlag flag) const { return Bits & flag; }
    void set(ContextFlag flag) { Bits |= flag; }
    void clear(ContextFlag flag) { Bits &= ~flag; }
    void mergeFrom(ContextFlag flag, ContextFlags other) {
      Bits |= (other.Bits & flag);
    }

    void mergeFrom(ContextFlags flags, ContextFlags other) {
      Bits |= (other.Bits & flags.Bits);
    }

    // All of the flags that can be set by throw checking.
    static ContextFlags throwFlags() {
      ContextFlags result;
      result.set(IsTryCovered);
      result.set(IsInTry);
      result.set(HasAnyThrowSite);
      result.set(HasTryThrowSite);
      return result;
    }

    // All of the flags that can be set by async/await checking.
    static ContextFlags asyncAwaitFlags() {
      ContextFlags result;
      result.set(IsAsyncCovered);
      result.set(HasAnyAsyncSite);
      result.set(HasAnyAwait);
      return result;
    }
  };

  ContextFlags Flags;

  /// The maximum combined value of all throwing expressions in the current
  /// context.
  ConditionalEffectKind MaxThrowingKind;

  struct DiagnosticInfo {
    DiagnosticInfo(Expr &failingExpr,
                   PotentialEffectReason reason,
                   bool downgradeToWarning) :
      reason(reason),
      expr(failingExpr),
      downgradeToWarning(downgradeToWarning) {}

    /// Reason for throwing
    PotentialEffectReason reason;

    /// Failing expression
    Expr &expr;

    /// Whether the error should be downgraded to a warning.
    bool downgradeToWarning;
  };

  SmallVector<Expr *, 4> errorOrder;
  llvm::DenseMap<Expr *, std::vector<DiagnosticInfo>> uncoveredAsync;
  llvm::DenseMap<Expr *, Expr *> parentMap;

  static bool isEffectAnchor(Expr *e) {
    return isa<AbstractClosureExpr>(e) || isa<DiscardAssignmentExpr>(e) ||
           isa<AssignExpr>(e) || (isa<DeclRefExpr>(e) && e->isImplicit());
  }

  static bool isAnchorTooEarly(Expr *e) {
    return isa<AssignExpr>(e) || isa<DiscardAssignmentExpr>(e);
  }

  /// Find the top location where we should put the await
  static Expr *walkToAnchor(Expr *e, llvm::DenseMap<Expr *, Expr *> &parentMap,
                            bool isInterpolatedString) {
    Expr *parent = e;
    Expr *lastParent = e;
    while (parent && !isEffectAnchor(parent)) {
      lastParent = parent;
      parent = parentMap[parent];
    }

    if (parent && !isAnchorTooEarly(parent)) {
      return parent;
    }
    if (isInterpolatedString) {
      assert(parent == nullptr && "Expected to be at top of expression");
      if (ArgumentList *args = lastParent->getArgs()) {
        if (Expr *unaryArg = args->getUnlabeledUnaryExpr())
          return unaryArg;
      }
    }
    return lastParent;
  }

  void flagInvalidCode() {
    // Suppress warnings about useless try or catch.
    Flags.set(ContextFlags::HasAnyThrowSite);
    Flags.set(ContextFlags::HasTryThrowSite);
  }

  /// An RAII object for restoring all the interesting state in an
  /// error-coverage.
  class ContextScope {
    CheckEffectsCoverage &Self;
    Context OldContext;
    DeclContext *OldRethrowsDC;
    DeclContext *OldReasyncDC;
    ContextFlags OldFlags;
    ConditionalEffectKind OldMaxThrowingKind;
    SourceLoc OldAwaitLoc;

  public:
    ContextScope(CheckEffectsCoverage &self, std::optional<Context> newContext)
        : Self(self), OldContext(self.CurContext),
          OldRethrowsDC(self.RethrowsDC), OldReasyncDC(self.ReasyncDC),
          OldFlags(self.Flags), OldMaxThrowingKind(self.MaxThrowingKind),
          OldAwaitLoc(self.CurContext.awaitLoc) {
      if (newContext) self.CurContext = *newContext;
    }

    ContextScope(const ContextScope &) = delete;
    ContextScope &operator=(const ContextScope &) = delete;

    void enterSubFunction() {
      Self.RethrowsDC = nullptr;
      Self.ReasyncDC = nullptr;
    }

    void enterTry() {
      Self.Flags.set(ContextFlags::IsInTry);
      Self.Flags.set(ContextFlags::IsTryCovered);
      Self.Flags.clear(ContextFlags::HasTryThrowSite);
    }

    void enterAwait(SourceLoc awaitLoc) {
      Self.Flags.set(ContextFlags::IsAsyncCovered);
      Self.Flags.clear(ContextFlags::HasAnyAsyncSite);
      Self.CurContext.awaitLoc = awaitLoc;
    }

    void enterAsyncLet() {
      Self.Flags.set(ContextFlags::InAsyncLet);
    }

    void refineLocalContext(Context newContext) {
      Self.CurContext = newContext;
    }

    void resetCoverage() {
      Self.Flags.reset();
      Self.MaxThrowingKind = ConditionalEffectKind::None;
    }

    void resetCoverageForAutoclosureBody() {
      Self.Flags.clear(ContextFlags::IsAsyncCovered);
      Self.Flags.clear(ContextFlags::HasAnyAsyncSite);
      Self.Flags.clear(ContextFlags::HasAnyAwait);
    }

    void resetCoverageForDoCatch() {
      Self.Flags.reset();
      Self.MaxThrowingKind = ConditionalEffectKind::None;

      Self.Flags.mergeFrom(ContextFlags::StmtExprCoversTry, OldFlags);
      Self.Flags.mergeFrom(ContextFlags::StmtExprCoversAwait, OldFlags);

      // Suppress 'try' coverage checking within a single level of
      // do/catch in debugger functions.
      if (OldFlags.has(ContextFlags::IsTopLevelDebuggerFunction))
        Self.Flags.set(ContextFlags::IsTryCovered);
    }

    void preserveCoverageFromAutoclosureBody() {
      // An autoclosure body is the part of the enclosing function
      // body for the purposes of deciding whether a try contained
      // a throwing call.
      OldFlags.mergeFrom(ContextFlags::HasTryThrowSite, Self.Flags);

      // "await" doesn't work this way; the "await" needs to be part of
      // the autoclosure expression itself, and the autoclosure must be
      // 'async'.
    }

    void setCoverageForSingleValueStmtExpr() {
      resetCoverage();
      Self.Flags.mergeFrom(ContextFlags::InAsyncLet, OldFlags);

      if (OldFlags.has(ContextFlags::IsTryCovered))
        Self.Flags.set(ContextFlags::StmtExprCoversTry);

      if (OldFlags.has(ContextFlags::IsAsyncCovered))
        Self.Flags.set(ContextFlags::StmtExprCoversAwait);
    }

    void preserveCoverageFromSingleValueStmtExpr() {
      // We need to preserve whether we saw any throwing sites, to avoid warning
      // on 'do { let x = if .random() { try ... } else { ... } } catch { ... }'
      OldFlags.mergeFrom(ContextFlags::HasAnyThrowSite, Self.Flags);

      // We need to preserve the throwing kind to correctly handle rethrows.
      OldMaxThrowingKind = std::max(OldMaxThrowingKind, Self.MaxThrowingKind);
    }

    void preserveCoverageFromNonExhaustiveCatch() {
      OldFlags.mergeFrom(ContextFlags::HasAnyThrowSite, Self.Flags);
      OldMaxThrowingKind = std::max(OldMaxThrowingKind, Self.MaxThrowingKind);
    }

    void preserveDiagnoseErrorOnTryFlag() {
      // The "DiagnoseErrorOnTry" flag is a bit of mutable state
      // in the Context itself, used to postpone diagnostic emission
      // to a parent "try" expression. If something was diagnosed
      // during this ContextScope, the flag may have been set, and
      // we need to preserve its value when restoring the old Context.
      bool DiagnoseErrorOnTry = Self.CurContext.shouldDiagnoseErrorOnTry();
      OldContext.setDiagnoseErrorOnTry(DiagnoseErrorOnTry);
    }

    void preserveCoverageFromAwaitOperand() {
      OldFlags.mergeFrom(ContextFlags::HasAnyAwait, Self.Flags);
      OldFlags.mergeFrom(ContextFlags::throwFlags(), Self.Flags);
      OldMaxThrowingKind = std::max(OldMaxThrowingKind, Self.MaxThrowingKind);

      preserveDiagnoseErrorOnTryFlag();
    }

    void preserveCoverageFromTryOperand() {
      OldFlags.mergeFrom(ContextFlags::HasAnyThrowSite, Self.Flags);
      OldFlags.mergeFrom(ContextFlags::asyncAwaitFlags(), Self.Flags);
      OldMaxThrowingKind = std::max(OldMaxThrowingKind, Self.MaxThrowingKind);
    }

    void preserveCoverageFromOptionalOrForcedTryOperand() {
      OldFlags.mergeFrom(ContextFlags::asyncAwaitFlags(), Self.Flags);
    }

    void preserveCoverageFromInterpolatedString() {
      OldFlags.mergeFrom(ContextFlags::HasAnyThrowSite, Self.Flags);
      OldFlags.mergeFrom(ContextFlags::HasTryThrowSite, Self.Flags);
      OldFlags.mergeFrom(ContextFlags::HasAnyAsyncSite, Self.Flags);
      OldFlags.mergeFrom(ContextFlags::HasAnyAwait, Self.Flags);
      OldMaxThrowingKind = std::max(OldMaxThrowingKind, Self.MaxThrowingKind);

      preserveDiagnoseErrorOnTryFlag();
    }

    bool wasTopLevelDebuggerFunction() const {
      return OldFlags.has(ContextFlags::IsTopLevelDebuggerFunction);
    }

    ~ContextScope() {
      Self.CurContext = OldContext;
      Self.RethrowsDC = OldRethrowsDC;
      Self.ReasyncDC = OldReasyncDC;
      Self.Flags = OldFlags;
      Self.MaxThrowingKind = OldMaxThrowingKind;
      Self.CurContext.awaitLoc = OldAwaitLoc;
    }
  };

  /// Retrieve the type of the error that can be caught when an error is
  /// thrown from the given location.
  Type getCaughtErrorTypeAt(SourceLoc loc) {
    auto dc = CurContext.getDeclContext();
    auto module = dc->getParentModule();

    // Autoclosures can't be found via ASTScope lookup.
    if (CurContext.isAutoClosure()) {
      auto *closure = dyn_cast<AutoClosureExpr>(CurContext.getDeclContext());
      if (auto type = closure->getEffectiveThrownType())
        return *type;

      // Otherwise, the closure does not throw.
      return Ctx.getNeverType();
    }

    if (CatchNode catchNode = ASTScope::lookupCatchNode(module, loc)) {
      if (auto caughtType = catchNode.getThrownErrorTypeInContext(Ctx))
        return *caughtType;

      // If a catch node returns null for its thrown error type, we're
      // in a non-throwing context.
      return Ctx.getNeverType();
    }

    // Fall back to the error existential.
    return Ctx.getErrorExistentialType();
  }

public:
  CheckEffectsCoverage(ASTContext &ctx, Context initialContext)
    : Ctx(ctx), CurContext(initialContext),
      MaxThrowingKind(ConditionalEffectKind::None) {

    if (auto rethrowsDC = initialContext.getPolymorphicEffectDeclContext(
          EffectKind::Throws)) {
      RethrowsDC = rethrowsDC;
    }
    if (auto reasyncDC = initialContext.getPolymorphicEffectDeclContext(
          EffectKind::Async)) {
      ReasyncDC = reasyncDC;
    }
  }

  ~CheckEffectsCoverage() {
    for (Expr *anchor: errorOrder) {
      diagnoseUncoveredAsyncSite(anchor);
    }
  }


  /// Mark that the current context is top-level code with
  /// throw-without-try enabled.
  void setTopLevelThrowWithoutTry() {
    Flags.set(ContextFlags::IsTryCovered);
  }

  /// Mark that the current context is covered by a 'try', as
  /// appropriate for a debugger function.
  ///
  /// Top level code in the debugger is actually implicitly wrapped in
  /// a function with a do/catch block.
  void setTopLevelDebuggerFunction() {
    Flags.set(ContextFlags::IsTryCovered);
    Flags.set(ContextFlags::IsTopLevelDebuggerFunction);
  }

private:
  void visitExprPre(Expr *expr) {
    if (parentMap.count(expr) == 0)
      parentMap = expr->getParentMap();
    return;
  }

  ShouldRecurse_t checkClosure(ClosureExpr *E) {
    ContextScope scope(*this, Context::forClosure(E));
    scope.enterSubFunction();
    scope.resetCoverage();
    E->getBody()->walk(*this);
    return ShouldNotRecurse;
  }

  ShouldRecurse_t checkAutoClosure(AutoClosureExpr *E) {
    ContextScope scope(*this, Context::forClosure(E));
    scope.enterSubFunction();

    bool shouldPreserveCoverage = true;
    switch (E->getThunkKind()) {
    case AutoClosureExpr::Kind::DoubleCurryThunk:
    case AutoClosureExpr::Kind::SingleCurryThunk:
      // Curry thunks aren't actually a call to the asynchronous function.
      // Assume that async is covered in such contexts.
      scope.resetCoverageForAutoclosureBody();
      Flags.set(ContextFlags::IsAsyncCovered);
      break;

    case AutoClosureExpr::Kind::None:
      scope.resetCoverageForAutoclosureBody();
      break;

    case AutoClosureExpr::Kind::AsyncLet:
      scope.resetCoverage();
      scope.enterAsyncLet();
      shouldPreserveCoverage = false;
      break;
    }

    E->getBody()->walk(*this);

    if (shouldPreserveCoverage)
      scope.preserveCoverageFromAutoclosureBody();

    return ShouldNotRecurse;
  }

  ShouldRecurse_t
  checkSingleValueStmtExpr(SingleValueStmtExpr *SVE) {
    // For an if/switch expression, we reset coverage such that a 'try'/'await'
    // does not cover the branches.
    ContextScope scope(*this, /*newContext*/ std::nullopt);
    scope.setCoverageForSingleValueStmtExpr();
    SVE->getStmt()->walk(*this);
    scope.preserveCoverageFromSingleValueStmtExpr();
    return ShouldNotRecurse;
  }

  ConditionalEffectKind checkExhaustiveDoBody(DoCatchStmt *S) {
    // This is a context where errors are handled.
    ContextScope scope(*this, CurContext.withHandlesErrors());
    assert(!Flags.has(ContextFlags::IsInTry) && "do/catch within try?");
    scope.resetCoverageForDoCatch();

    S->getBody()->walk(*this);

    diagnoseNoThrowInDo(S, scope);

    return MaxThrowingKind;
  }

  ConditionalEffectKind checkNonExhaustiveDoBody(DoCatchStmt *S) {
    ContextScope scope(*this, std::nullopt);
    assert(!Flags.has(ContextFlags::IsInTry) && "do/catch within try?");
    scope.resetCoverageForDoCatch();

    // If the enclosing context doesn't handle anything, use a
    // specialized diagnostic about non-exhaustive catches.
    if (!CurContext.handlesThrows(ConditionalEffectKind::Conditional)) {
      CurContext.setNonExhaustiveCatch(true);
    } else if (Type rethrownErrorType = S->getCaughtErrorType()) {
      // We're implicitly rethrowing the error out of this do..catch, so make
      // sure that we can throw an error of this type out of this context.
      auto catches = S->getCatches();
      S->setRethrows(
          checkThrownErrorType(catches.back()->getEndLoc(), rethrownErrorType));
    }

    S->getBody()->walk(*this);

    diagnoseNoThrowInDo(S, scope);

    scope.preserveCoverageFromNonExhaustiveCatch();
    return MaxThrowingKind;
  }

  void diagnoseNoThrowInDo(DoCatchStmt *S, ContextScope &scope) {
    // Warn if nothing threw within the body, unless this is the
    // implicit do/catch in a debugger function.
    if (!Flags.has(ContextFlags::HasAnyThrowSite) &&
        !scope.wasTopLevelDebuggerFunction()) {
      Ctx.Diags.diagnose(S->getCatches().front()->getStartLoc(),
                         diag::no_throw_in_do_with_catch);
    }
  }

  void checkCatch(CaseStmt *S, ConditionalEffectKind doThrowingKind) {
    auto dc = CurContext.getDeclContext();
    for (auto &LabelItem : S->getMutableCaseLabelItems()) {
      // The pattern and guard aren't allowed to throw.
      {
        ContextScope scope(*this, Context::forCatchPattern(S, dc));
        LabelItem.getPattern()->walk(*this);
      }
      if (auto guard = LabelItem.getGuardExpr()) {
        ContextScope scope(*this, Context::forCatchGuard(S, dc));
        guard->walk(*this);
      }
    }

    auto savedContext = CurContext;
    if (doThrowingKind != ConditionalEffectKind::Always &&
        CurContext.hasPolymorphicEffect(EffectKind::Throws)) {
      // If this catch clause is reachable at all, it's because a function
      // parameter throws. So let's temporarily state that the body is allowed
      // to throw.
      CurContext = CurContext.withHandlesErrors();
    }

    // The catch body just happens in the enclosing context.
    S->getBody()->walk(*this);

    CurContext = savedContext;
  }

  ShouldRecurse_t checkApply(ApplyExpr *E) {
    // An apply expression is a potential throw site if the function throws.
    // But if the expression didn't type-check, suppress diagnostics.
    auto classification = getApplyClassifier().classifyApply(E);

    auto throwDest = checkThrowAsyncSite(
        E, /*requiresTry*/ true, classification);

    if (!classification.isInvalid()) {
      // HACK: functions can get queued multiple times in
      // definedFunctions, so be sure to be idempotent.
      if (!E->isThrowsSet()) {
        E->setThrows(throwDest);
      }

      auto asyncKind = classification.getConditionalKind(EffectKind::Async);
      E->setNoAsync(asyncKind == ConditionalEffectKind::None);
    } else {
      // HACK: functions can get queued multiple times in
      // definedFunctions, so be sure to be idempotent.
      if (!E->isThrowsSet()) {
        E->setThrows(ThrownErrorDestination());
      }

      E->setNoAsync(true);
    }

    // If current apply expression did not type-check, don't attempt
    // walking inside of it. This accounts for the fact that we don't
    // erase types without type variables to enable better code complication,
    // so DeclRefExpr(s) or ApplyExpr with DeclRefExpr as function contained
    // inside would have their types preserved, which makes classification
    // incorrect.
    auto type = E->getType();
    return !type || type->hasError() ? ShouldNotRecurse : ShouldRecurse;
  }

  ApplyClassifier getApplyClassifier() const {
    ApplyClassifier classifier(Ctx);
    classifier.DC = CurContext.getDeclContext();
    classifier.RethrowsDC = RethrowsDC;
    classifier.ReasyncDC = ReasyncDC;
    return classifier;
  }

  ShouldRecurse_t checkLookup(LookupExpr *E) {
    if (auto classification = getApplyClassifier().classifyLookup(E)) {
      auto throwDest = checkThrowAsyncSite(
          E, classification.hasThrows(), classification);
      E->setThrows(throwDest);
    }

    return ShouldRecurse;
  }

  ShouldRecurse_t checkDeclRef(DeclRefExpr *E) {
    if (auto classification = getApplyClassifier().classifyDeclRef(E)) {
      auto throwDest = checkThrowAsyncSite(
          E, classification.hasThrows(), classification);
      E->setThrows(throwDest);
    }

    return ShouldNotRecurse;
  }

  ShouldRecurse_t checkAsyncLet(PatternBindingDecl *patternBinding) {
    // Diagnose async let in a context that doesn't handle async.
    if (!CurContext.handlesAsync(ConditionalEffectKind::Always)) {
      CurContext.diagnoseUnhandledAsyncSite(Ctx.Diags, patternBinding,
                                          PotentialEffectReason::forAsyncLet());
    }

    return ShouldRecurse;
  }

  ShouldRecurse_t
  checkInterpolatedStringLiteral(InterpolatedStringLiteralExpr *E) {
    ContextScope scope(*this, CurContext.withInterpolatedString(E));
    if (E->getAppendingExpr())
      E->getAppendingExpr()->walk(*this);
    scope.preserveCoverageFromInterpolatedString();
    return ShouldNotRecurse;
  }

  ShouldRecurse_t checkIfConfig(IfConfigDecl *ICD) {
    // Check the inactive regions of a #if block to disable warnings that may
    // be due to platform specific code.
    struct ConservativeThrowChecker : public ASTWalker {
      CheckEffectsCoverage &CEC;
      ConservativeThrowChecker(CheckEffectsCoverage &CEC) : CEC(CEC) {}

      MacroWalking getMacroWalkingBehavior() const override {
        return MacroWalking::Arguments;
      }

      PostWalkResult<Expr *> walkToExprPost(Expr *E) override {
        if (isa<TryExpr>(E))
          CEC.Flags.set(ContextFlags::HasAnyThrowSite);
        return Action::Continue(E);
      }

      PostWalkResult<Stmt *> walkToStmtPost(Stmt *S) override {
        if (isa<ThrowStmt>(S))
          CEC.Flags.set(ContextFlags::HasAnyThrowSite);

        return Action::Continue(S);
      }
    };

    for (auto &clause : ICD->getClauses()) {
      // Active clauses are handled by the normal AST walk.
      if (clause.isActive) continue;

      for (auto elt : clause.Elements)
        elt.walk(ConservativeThrowChecker(*this));
    }
    return ShouldRecurse;
  }

  ShouldRecurse_t checkThrow(ThrowStmt *S) {
    if (auto classification = getApplyClassifier().classifyThrow(S)) {
      Flags.set(ContextFlags::HasAnyThrowSite);
      MaxThrowingKind = std::max(MaxThrowingKind, ConditionalEffectKind::Always);

      if (!CurContext.handlesThrows(ConditionalEffectKind::Always))
        CurContext.diagnoseUnhandledThrowStmt(Ctx.Diags, S);
      else {
        SourceLoc loc = S->getThrowLoc();
        Expr *thrownValue = S->getSubExpr();
        Type thrownErrorType = thrownValue->getType();
        Type caughtErrorType = getCaughtErrorTypeAt(loc);
        if (!caughtErrorType->isEqual(thrownErrorType)) {
          thrownValue = removeErasureToExistentialError(thrownValue);
          Type thrownErrorType = thrownValue->getType();
          if (checkThrownErrorType(loc, thrownErrorType))
            S->setSubExpr(thrownValue);
        }
      }
    }

    return ShouldRecurse;
  }

  ThrownErrorDestination
  checkThrowAsyncSite(ASTNode E, bool requiresTry,
                      Classification &classification) {
    // Suppress all diagnostics when there's an un-analyzable throw/async site.
    if (classification.isInvalid()) {
      Flags.set(ContextFlags::HasAnyThrowSite);
      Flags.set(ContextFlags::HasAnyAsyncSite);
      if (requiresTry) Flags.set(ContextFlags::HasTryThrowSite);
      return ThrownErrorDestination();
    }

    auto asyncKind = classification.getConditionalKind(EffectKind::Async);
    auto throwsKind = classification.getConditionalKind(EffectKind::Throws);

    // Check async calls.
    switch (asyncKind) {
    case ConditionalEffectKind::None:
      break;

    case ConditionalEffectKind::Conditional:
    case ConditionalEffectKind::Always:
      // Remember that we've seen an async call.
      Flags.set(ContextFlags::HasAnyAsyncSite);

      // Diagnose async calls in a context that doesn't handle async.
      if (!CurContext.handlesAsync(asyncKind)) {
        CurContext.diagnoseUnhandledAsyncSite(Ctx.Diags, E,
                                              classification.getAsyncReason());
      }
      // Diagnose async calls that are outside of an await context.
      else if (!(Flags.has(ContextFlags::IsAsyncCovered) ||
                 Flags.has(ContextFlags::InAsyncLet))) {
        Expr *expr = E.dyn_cast<Expr*>();
        Expr *anchor = walkToAnchor(expr, parentMap,
                                    CurContext.isWithinInterpolatedString());
        if (Flags.has(ContextFlags::StmtExprCoversAwait))
          classification.setDowngradeToWarning(true);
        if (uncoveredAsync.find(anchor) == uncoveredAsync.end())
          errorOrder.push_back(anchor);
        uncoveredAsync[anchor].emplace_back(
            *expr, classification.getAsyncReason(),
            classification.shouldDowngradeToWarning());
      }
    }

    // Check throwing calls.
    MaxThrowingKind = std::max(MaxThrowingKind, throwsKind); // FIXME: why is the left Never for property?

    switch (throwsKind) {
    // Completely ignores sites that don't throw.
    case ConditionalEffectKind::None:
      break;

    // For the purposes of handling and try-coverage diagnostics,
    // being rethrowing-only still makes this a throw site.
    case ConditionalEffectKind::Conditional:
    case ConditionalEffectKind::Always:
      Flags.set(ContextFlags::HasAnyThrowSite);
      if (requiresTry) Flags.set(ContextFlags::HasTryThrowSite);

      // We set the throwing bit of an apply expr after performing this
      // analysis, so ensure we don't emit duplicate diagnostics for functions
      // that have been queued multiple times.
      if (auto expr = E.dyn_cast<Expr*>())
        if (auto apply = dyn_cast<ApplyExpr>(expr))
          if (apply->isThrowsSet())
            break;

      bool isTryCovered =
        (!requiresTry || Flags.has(ContextFlags::IsTryCovered) ||
         Flags.has(ContextFlags::InAsyncLet));
      if (!CurContext.handlesThrows(throwsKind)) {
        CurContext.diagnoseUnhandledThrowSite(Ctx.Diags, E, isTryCovered,
                                              classification.getThrowReason());
      } else if (!isTryCovered) {
        if (Flags.has(ContextFlags::StmtExprCoversTry))
          classification.setDowngradeToWarning(true);
        CurContext.diagnoseUncoveredThrowSite(Ctx, E, // we want this one to trigger
                                              classification);
      } else {
        return checkThrownErrorType(
            E.getStartLoc(), classification.getThrownError());
      }
      break;
    }

    return ThrownErrorDestination();
  }

  /// Check the thrown error type against the type that can be caught or
  /// rethrown by the context.
  ///
  /// Returns a thrown error destination, which will be non-throwing if there
  /// was an error.
  ThrownErrorDestination
  checkThrownErrorType(SourceLoc loc, Type thrownErrorType) {
    Type caughtErrorType = getCaughtErrorTypeAt(loc);
    if (caughtErrorType->isEqual(thrownErrorType))
      return ThrownErrorDestination::forMatchingContextType(thrownErrorType);

    OpaqueValueExpr *opaque = new (Ctx) OpaqueValueExpr(loc, thrownErrorType);
    Expr *rethrowExpr = opaque;
    Type resultType = TypeChecker::typeCheckExpression(
        rethrowExpr, CurContext.getDeclContext(),
        {caughtErrorType, /*FIXME:*/CTP_ThrowStmt});
    if (resultType.isNull())
      return ThrownErrorDestination();

    return ThrownErrorDestination::forConversion(opaque, rethrowExpr);
  }

  ShouldRecurse_t checkAwait(AwaitExpr *E) {

    // Walk the operand.
    ContextScope scope(*this, std::nullopt);
    scope.enterAwait(E->getAwaitLoc());

    E->getSubExpr()->walk(*this);

    // Warn about 'await' expressions that weren't actually needed, unless of
    // course we're in a context that could never handle an 'async'. Then, we
    // produce an error.
    if (!Flags.has(ContextFlags::HasAnyAsyncSite)) {
      if (CurContext.handlesAsync(ConditionalEffectKind::Conditional)) {
        diagnoseRedundantAwait(E);
      } else {
        CurContext.diagnoseUnhandledAsyncSite(Ctx.Diags, E, std::nullopt,
                                              /*forAwait=*/true);
      }
    }

    // Inform the parent of the walk that an 'await' exists here.
    scope.preserveCoverageFromAwaitOperand();
    return ShouldNotRecurse;
  }
  
  ShouldRecurse_t checkTry(TryExpr *E) {
    // Walk the operand.
    ContextScope scope(*this, std::nullopt);
    scope.enterTry();

    E->getSubExpr()->walk(*this);

    // Warn about 'try' expressions that weren't actually needed.
    if (!Flags.has(ContextFlags::HasTryThrowSite)) {
      if (!E->isImplicit())
        diagnoseRedundantTry(E);

    // Diagnose all the call sites within a single unhandled 'try'
    // at the same time.
    } else if (!CurContext.handlesThrows(ConditionalEffectKind::Conditional)) {
      CurContext.diagnoseUnhandledTry(Ctx.Diags, E);
    }

    scope.preserveCoverageFromTryOperand();
    return ShouldNotRecurse;
  }

  ShouldRecurse_t checkForceTry(ForceTryExpr *E) {
    // Walk the operand.  'try!' handles errors.
    ContextScope scope(*this, CurContext.withHandlesErrors());
    scope.enterTry();

    E->getSubExpr()->walk(*this);

    // Warn about 'try' expressions that weren't actually needed.
    if (!Flags.has(ContextFlags::HasTryThrowSite))
      diagnoseRedundantTry(E);

    if (auto thrownError = TypeChecker::canThrow(Ctx, E->getSubExpr())) {
      E->setThrownError(*thrownError);
    } else {
      E->setThrownError(Ctx.getNeverType());
    }

    scope.preserveCoverageFromOptionalOrForcedTryOperand();
    return ShouldNotRecurse;
  }

  ShouldRecurse_t checkOptionalTry(OptionalTryExpr *E) {
    // Walk the operand.  'try?' handles errors.
    ContextScope scope(*this, CurContext.withHandlesErrors());
    scope.enterTry();

    E->getSubExpr()->walk(*this);

    // Warn about 'try' expressions that weren't actually needed.
    if (!Flags.has(ContextFlags::HasTryThrowSite))
      diagnoseRedundantTry(E);

    if (auto thrownError = TypeChecker::canThrow(Ctx, E->getSubExpr())) {
      E->setThrownError(*thrownError);
    } else {
      E->setThrownError(Ctx.getNeverType());
    }

    scope.preserveCoverageFromOptionalOrForcedTryOperand();
    return ShouldNotRecurse;
  }

  ShouldRecurse_t checkForEach(ForEachStmt *S) {
    if (!S->getAwaitLoc().isValid())
      return ShouldRecurse;

    // A 'for await' is always async. There's no effect polymorphism
    // via the conformance in a 'reasync' function body.
    Flags.set(ContextFlags::HasAnyAsyncSite);

    if (!CurContext.handlesAsync(ConditionalEffectKind::Always))
      CurContext.diagnoseUnhandledAsyncSite(Ctx.Diags, S, std::nullopt);

    // A 'for try await' has a thrown error type that depends on the
    // AsyncSequence conformance.
    auto classification =
        getApplyClassifier().classifyForEach(S).onlyThrowing();
    if (classification) {
      auto throwsKind = classification.getConditionalKind(EffectKind::Throws);

      if (throwsKind != ConditionalEffectKind::None)
        Flags.set(ContextFlags::HasAnyThrowSite);

      // Note: we don't need to check whether the throw error is handled,
      // because we will also be checking the generated next/nextElement
      // call.
    }

    return ShouldRecurse;
  }

  void diagnoseRedundantTry(AnyTryExpr *E) const {
    if (auto *SVE = SingleValueStmtExpr::tryDigOutSingleValueStmtExpr(E)) {
      // For an if/switch expression, produce a tailored warning.
      Ctx.Diags.diagnose(E->getTryLoc(),
                         diag::effect_marker_on_single_value_stmt,
                         "try", SVE->getStmt()->getKind())
        .highlight(E->getTryLoc());
      return;
    }
    Ctx.Diags.diagnose(E->getTryLoc(), diag::no_throw_in_try);
  }

  void diagnoseRedundantAwait(AwaitExpr *E) const {
    if (auto *SVE = SingleValueStmtExpr::tryDigOutSingleValueStmtExpr(E)) {
      // For an if/switch expression, produce a tailored warning.
      Ctx.Diags.diagnose(E->getAwaitLoc(),
                         diag::effect_marker_on_single_value_stmt,
                         "await", SVE->getStmt()->getKind())
        .highlight(E->getAwaitLoc());
      return;
    }
    Ctx.Diags.diagnose(E->getAwaitLoc(), diag::no_async_in_await);
  }

  std::pair<SourceLoc, std::string>
  getFixItForUncoveredAsyncSite(const Expr *anchor) const {
    SourceLoc awaitInsertLoc = anchor->getStartLoc();
    std::string insertText = "await ";
    if (auto *tryExpr = dyn_cast<AnyTryExpr>(anchor))
      awaitInsertLoc = tryExpr->getSubExpr()->getStartLoc();
    else if (auto *autoClosure = dyn_cast<AutoClosureExpr>(anchor)) {
      if (auto *tryExpr =
              dyn_cast<AnyTryExpr>(autoClosure->getSingleExpressionBody()))
        awaitInsertLoc = tryExpr->getSubExpr()->getStartLoc();
      // Supply a tailored fixIt including the identifier if we are
      // looking at a shorthand optional binding.
    } else if (anchor->isImplicit()) {
      if (auto declRef = dyn_cast<DeclRefExpr>(anchor))
        if (auto var = dyn_cast_or_null<VarDecl>(declRef->getDecl())) {
          insertText = " = await " + var->getNameStr().str();
          awaitInsertLoc = Lexer::getLocForEndOfToken(Ctx.Diags.SourceMgr,
                                                       anchor->getStartLoc());
        }
    }
    return std::make_pair(awaitInsertLoc, insertText);
  }

  void diagnoseUncoveredAsyncSite(const Expr *anchor) const {
    auto asyncPointIter = uncoveredAsync.find(anchor);
    if (asyncPointIter == uncoveredAsync.end())
      return;
    const auto &errors = asyncPointIter->getSecond();
    const auto &[loc, insertText] = getFixItForUncoveredAsyncSite(anchor);
    bool downgradeToWarning = llvm::all_of(errors,
        [&](DiagnosticInfo diag) -> bool {
          return diag.downgradeToWarning;
        });

    Ctx.Diags.diagnose(anchor->getStartLoc(), diag::async_expr_without_await)
      .warnUntilSwiftVersionIf(downgradeToWarning, 6)
      .fixItInsert(loc, insertText)
      .highlight(anchor->getSourceRange());

    for (const DiagnosticInfo &diag: errors) {
      switch (diag.reason.getKind()) {
        case PotentialEffectReason::Kind::AsyncLet:
          if (auto declR = dyn_cast<DeclRefExpr>(&diag.expr)) {
            if (auto var = dyn_cast<VarDecl>(declR->getDecl())) {
              if (var->isAsyncLet()) {
                Ctx.Diags.diagnose(declR->getLoc(),
                                   diag::async_let_without_await, var);
                continue;
              }
            }
          }
          LLVM_FALLTHROUGH; // fallthrough to a message about PropertyAccess
        case PotentialEffectReason::Kind::PropertyAccess:
          Ctx.Diags.diagnose(diag.expr.getStartLoc(),
                             diag::async_access_without_await, 1);
          continue;

        case PotentialEffectReason::Kind::SubscriptAccess:
          Ctx.Diags.diagnose(diag.expr.getStartLoc(),
                             diag::async_access_without_await, 2);
          continue;

        case PotentialEffectReason::Kind::ByClosure:
        case PotentialEffectReason::Kind::ByDefaultClosure:
        case PotentialEffectReason::Kind::ByConformance:
        case PotentialEffectReason::Kind::Apply: {
         if (auto autoclosure = dyn_cast<AutoClosureExpr>(anchor)) {
           switch(autoclosure->getThunkKind()) {
             case AutoClosureExpr::Kind::None:
               Ctx.Diags.diagnose(diag.expr.getStartLoc(),
                                  diag::async_call_without_await_in_autoclosure);
               break;
             case AutoClosureExpr::Kind::AsyncLet:
               Ctx.Diags.diagnose(diag.expr.getStartLoc(),
                                  diag::async_call_without_await_in_async_let);
               break;
             case AutoClosureExpr::Kind::SingleCurryThunk:
             case AutoClosureExpr::Kind::DoubleCurryThunk:
               Ctx.Diags.diagnose(diag.expr.getStartLoc(),
                                  diag::async_access_without_await, 0);
               break;
           }
          continue;
         }

         auto *call = dyn_cast<ApplyExpr>(&diag.expr);
         if (call && call->isImplicitlyAsync()) {
           // Emit a tailored note if the call is implicitly async, meaning the
           // callee is isolated to an actor.
           auto callee = call->getCalledValue(/*skipFunctionConversions=*/true);
           if (callee) {
             Ctx.Diags.diagnose(diag.expr.getStartLoc(), diag::actor_isolated_sync_func,
                                callee);
           } else {
             Ctx.Diags.diagnose(
                 diag.expr.getStartLoc(), diag::actor_isolated_sync_func_value,
                 call->getFn()->getType());
           }
         } else {
           Ctx.Diags.diagnose(diag.expr.getStartLoc(),
                              diag::async_access_without_await, 0);
         }

         continue;
        }
      }
    }
  }
};

// Find nested functions and perform effects checking on them.
struct LocalFunctionEffectsChecker : ASTWalker {
  MacroWalking getMacroWalkingBehavior() const override {
    return MacroWalking::Expansion;
  }

  PreWalkAction walkToDeclPre(Decl *D) override {
    if (auto func = dyn_cast<AbstractFunctionDecl>(D)) {
      if (func->getDeclContext()->isLocalContext())
        TypeChecker::checkFunctionEffects(func);

      return Action::SkipNode();
    }

    return Action::Continue();
  }
};

} // end anonymous namespace

void TypeChecker::checkTopLevelEffects(TopLevelCodeDecl *code) {
  auto &ctx = code->getDeclContext()->getASTContext();
  CheckEffectsCoverage checker(ctx, Context::forTopLevelCode(code));

  // In some language modes, we allow top-level code to omit 'try' marking.
  if (ctx.LangOpts.EnableThrowWithoutTry)
    checker.setTopLevelThrowWithoutTry();

  if (auto *body = code->getBody()) {
    body->walk(checker);
    body->walk(LocalFunctionEffectsChecker());
  }
}

void TypeChecker::checkFunctionEffects(AbstractFunctionDecl *fn) {
#ifndef NDEBUG
  PrettyStackTraceDecl debugStack("checking effects handling for", fn);
#endif

  auto isDeferBody = isa<FuncDecl>(fn) && cast<FuncDecl>(fn)->isDeferBody();
  auto context =
      isDeferBody ? Context::forDeferBody(fn) : Context::forFunction(fn);
  auto &ctx = fn->getASTContext();
  CheckEffectsCoverage checker(ctx, context);

  // If this is a debugger function, suppress 'try' marking at the top level.
  if (fn->getAttrs().hasAttribute<LLDBDebuggerFunctionAttr>())
    checker.setTopLevelDebuggerFunction();

  if (auto body = fn->getBody()) {
    body->walk(checker);
    body->walk(LocalFunctionEffectsChecker());
  }

  if (auto ctor = dyn_cast<ConstructorDecl>(fn))
    if (auto superInit = ctor->getSuperInitCall())
      superInit->walk(checker);
}

void TypeChecker::checkInitializerEffects(Initializer *initCtx,
                                                Expr *init) {
  auto &ctx = initCtx->getASTContext();
  CheckEffectsCoverage checker(ctx, Context::forInitializer(initCtx));
  init->walk(checker);
  init->walk(LocalFunctionEffectsChecker());
}

void TypeChecker::checkCallerSideDefaultArgumentEffects(DeclContext *initCtx,
                                                        Expr *init) {
  auto &ctx = initCtx->getASTContext();
  CheckEffectsCoverage checker(ctx, Context::forDefaultArgument(initCtx));
  init->walk(checker);
  init->walk(LocalFunctionEffectsChecker());
}

/// Check the correctness of effects within the given enum
/// element's raw value expression.
///
/// The syntactic restrictions on such expressions should make it
/// impossible for errors to ever arise, but checking them anyway (1)
/// ensures correctness if those restrictions are ever loosened,
/// perhaps accidentally, and (2) allows the verifier to assert that
/// all calls have been checked.
void TypeChecker::checkEnumElementEffects(EnumElementDecl *elt, Expr *E) {
  auto &ctx = elt->getASTContext();
  CheckEffectsCoverage checker(ctx, Context::forEnumElementInitializer(elt));
  E->walk(checker);
  E->walk(LocalFunctionEffectsChecker());
}

void TypeChecker::checkPropertyWrapperEffects(
    PatternBindingDecl *binding, Expr *expr) {
  auto &ctx = binding->getASTContext();
  CheckEffectsCoverage checker(ctx, Context::forPatternBinding(binding));
  expr->walk(checker);
  expr->walk(LocalFunctionEffectsChecker());
}

std::optional<Type> TypeChecker::canThrow(ASTContext &ctx, Expr *expr) {
  ApplyClassifier classifier(ctx);
  auto classification = classifier.classifyExpr(expr, EffectKind::Throws);
  if (classification.getConditionalKind(EffectKind::Throws) ==
        ConditionalEffectKind::None)
    return std::nullopt;

  return classification.getThrownError();
}

std::optional<Type> TypeChecker::canThrow(ASTContext &ctx,
                                          ForEachStmt *forEach) {
  ApplyClassifier classifier(ctx);
  auto classification = classifier.classifyForEach(forEach).onlyThrowing();
  if (classification.getConditionalKind(EffectKind::Throws) ==
        ConditionalEffectKind::None)
    return std::nullopt;

  return classification.getThrownError();
}

Type TypeChecker::catchErrorType(DeclContext *dc, DoCatchStmt *stmt) {
  ASTContext &ctx = dc->getASTContext();

  // If the do..catch statement explicitly specifies that it throws, use
  // that type.
  if (Type explicitError = stmt->getExplicitCaughtType()) {
    return explicitError;
  }

  // Otherwise, infer the thrown error type from the "do" body.
  ApplyClassifier classifier(ctx);
  Classification classification = classifier.classifyStmt(
      stmt->getBody(), EffectKind::Throws);

  // If it doesn't throw at all, the type is Never.
  if (!classification.hasThrows()) {
    // Source compatibility: if the do..catch was already exhaustive,
    // and we aren't doing full typed throws, treat the caught error
    // type as 'any Error' to allow pattern-matches to continue to
    // type check.
    if (!ctx.LangOpts.hasFeature(Feature::FullTypedThrows) &&
        stmt->isSyntacticallyExhaustive())
      return ctx.getErrorExistentialType();

    return ctx.getNeverType();
  }

  return classification.getThrownError();
}

/// Explode the given type into the set of error unions.
///
/// \returns \c true if any of the types is the error existential type, which
/// means the entire error union is any Error.
static bool expandErrorUnions(Type type,
                              llvm::function_ref<Type(Type)> simplifyType,
                              SmallVectorImpl<Type> &terms) {
  // If we have a type variable in the type and a type simplification function,
  // apply it first.
  if (type->hasTypeVariable() && simplifyType)
    type = simplifyType(type);

  // If we have an error union type, handle it's terms individually.
  if (auto errorUnionType = type->getAs<ErrorUnionType>()) {
    for (auto term : errorUnionType->getTerms())
      if (expandErrorUnions(term, simplifyType, terms))
        return true;

    return false;
  }

  // If we have 'any Error', we're done.
  if (type->isErrorExistentialType())
    return true;

  // If we have anything other than 'Never', record it.
  if (!isNeverThrownError(type))
    terms.push_back(type);

  return false;
}

Type TypeChecker::errorUnion(Type type1, Type type2,
                             llvm::function_ref<Type(Type)> simplifyType) {
  // If one type is NULL, return the other.
  if (!type1)
    return type2;
  if (!type2)
    return type1;

  // Expand the error types we're given.
  //   - If any term is 'any Error', early return 'any Error'
  //   - Every 'Never' term is dropped.
  SmallVector<Type, 2> terms;
  if (expandErrorUnions(type1, simplifyType, terms))
    return type1->getASTContext().getErrorExistentialType();
  if (expandErrorUnions(type2, simplifyType, terms))
    return type1->getASTContext().getErrorExistentialType();

  // If we have more than one term, filter out duplicates and look to see if
  // we have obviously-different types.
  if (terms.size() > 1) {
    llvm::SmallDenseMap<CanType, Type> knownTypes;
    unsigned distinctConcreteTypes = 0;
    auto newEnd = std::remove_if(terms.begin(), terms.end(),
                                 [&](Type type) -> bool {
      // If we have already seen this type, remove it from the list of terms.
      if (!knownTypes.insert({type->getCanonicalType(), type}).second)
        return true;

      // We have not seen this type before. If it doesn't involve any
      // type variables, note that we've seen another concrete type.
      if (!type->hasTypeVariable())
        ++distinctConcreteTypes;

      return false;
    });

    // If we saw more than one distinct concrete type, return 'any Error'.
    if (distinctConcreteTypes > 1)
      return type1->getASTContext().getErrorExistentialType();

    // Remove any duplicated terms.
    terms.erase(newEnd, terms.end());
  }

  return ErrorUnionType::get(type1->getASTContext(), terms);
}

namespace {

/// Classifies a thrown error kind as Never, a specific type, or 'any Error'.
enum class ThrownErrorClassification {
  /// The `Never` type, which represents a non-throwing function.
  Never,

  /// A specific error type that is neither `Never` nor `any Error`.
  Specific,

  /// A specific error type that depends on a type variable or type parameter,
  /// and therefore we cannot determine whether it is a subtype of another
  /// type or not.
  Dependent,

  /// The type `any Error`, used for untyped throws.
  AnyError,
};

}

/// Classify the given thrown error type.
static ThrownErrorClassification classifyThrownErrorType(Type type) {
  if (type->isNever())
    return ThrownErrorClassification::Never;

  if (type->isExistentialType()) {
    Type anyError = type->getASTContext().getErrorExistentialType();
    if (anyError->isEqual(type))
      return ThrownErrorClassification::AnyError;
  }

  if (type->hasTypeVariable() || type->hasTypeParameter())
    return ThrownErrorClassification::Dependent;

  return ThrownErrorClassification::Specific;
}

ThrownErrorSubtyping
swift::compareThrownErrorsForSubtyping(
    Type subThrownError, Type superThrownError, DeclContext *dc
) {
  // Deal with NULL errors. This should only occur when there is no standard
  // library.
  if (!subThrownError || !superThrownError) {
    assert(!dc->getASTContext().getStdlibModule() && "NULL thrown error type");
    return ThrownErrorSubtyping::ExactMatch;
  }

  // Easy case: exact match.
  if (superThrownError->isEqual(subThrownError))
    return ThrownErrorSubtyping::ExactMatch;

  auto superThrownErrorKind = classifyThrownErrorType(superThrownError);
  auto subThrownErrorKind = classifyThrownErrorType(subThrownError);

  switch (subThrownErrorKind) {
  case ThrownErrorClassification::Dependent:
    switch (superThrownErrorKind) {
    case ThrownErrorClassification::AnyError:
      // This is a clear subtype relationship, because the supertype throws
      // anything.
      return ThrownErrorSubtyping::Subtype;

    case ThrownErrorClassification::Never:
    case ThrownErrorClassification::Dependent:
    case ThrownErrorClassification::Specific:
      // We have to compare the types. Do so below.
      break;
    }
    break;

  case ThrownErrorClassification::Specific:
    switch (superThrownErrorKind) {
    case ThrownErrorClassification::AnyError:
      // This is a clear subtype relationship, because the supertype throws
      // anything.
      return ThrownErrorSubtyping::Subtype;

    case ThrownErrorClassification::Never:
      // The supertype doesn't throw, so this has to drop 'throws' to work.
      return ThrownErrorSubtyping::DropsThrows;

    case ThrownErrorClassification::Dependent:
    case ThrownErrorClassification::Specific:
      // We have to compare the types. Do so below.
      break;
    }
    break;

  case ThrownErrorClassification::Never:
    // A function type throwing 'Never' is a subtype of all function types.
    return ThrownErrorSubtyping::Subtype;

  case ThrownErrorClassification::AnyError:
    switch (superThrownErrorKind) {
    case ThrownErrorClassification::Dependent:
    case ThrownErrorClassification::Specific:
      // We have to compare the types. Do so below.
      break;

    case ThrownErrorClassification::Never:
      // We're going to have to drop the "throws" entirely.
      return ThrownErrorSubtyping::DropsThrows;

    case ThrownErrorClassification::AnyError:
      llvm_unreachable("The thrown error types should have been equal");
    }
    break;
  }

  // If either of the types was dependent on a type variable or type parameter,
  // we can't do the comparison at all.
  if (superThrownErrorKind == ThrownErrorClassification::Dependent ||
      subThrownErrorKind == ThrownErrorClassification::Dependent)
    return ThrownErrorSubtyping::Dependent;

  // Check whether the subtype's thrown error type is convertible to the
  // supertype's thrown error type.
  if (TypeChecker::isConvertibleTo(subThrownError, superThrownError, dc))
    return ThrownErrorSubtyping::Subtype;

  // We know it doesn't work.
  return ThrownErrorSubtyping::Mismatch;
}
