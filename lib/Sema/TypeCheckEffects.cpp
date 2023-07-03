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
#include "swift/AST/ASTWalker.h"
#include "swift/AST/DiagnosticsSema.h"
#include "swift/AST/Effects.h"
#include "swift/AST/Initializer.h"
#include "swift/AST/ParameterList.h"
#include "swift/AST/Pattern.h"
#include "swift/AST/PrettyStackTrace.h"
#include "swift/AST/ProtocolConformance.h"
#include "swift/AST/TypeCheckRequests.h"

using namespace swift;

using EffectList = SmallVector<EffectKind, 4>;

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
      if (req.getProtocolDecl()->hasPolymorphicEffect(kind)) {
        return PolymorphicEffectKind::ByConformance;
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
static AccessorDecl* getEffectfulGetOnlyAccessor(ConcreteDeclRef cdr) {
  if (!cdr)
    return nullptr;

  if (auto storageDecl = dyn_cast<AbstractStorageDecl>(cdr.getDecl()))
    return storageDecl->getEffectfulGetAccessor();

  return nullptr;
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

  explicit AbstractFunction(ParamDecl *parameter)
    : TheKind(Kind::Parameter) {
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
        return AbstractFunction(param);
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
    return Action::VisitChildrenIf(bool(recurse));
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
    }
    // Error handling validation (via checkTopLevelEffects) happens after
    // type checking. If an unchecked expression is still around, the code was
    // invalid.
#define UNCHECKED_EXPR(KIND, BASE) \
    else if (isa<KIND##Expr>(E)) return Action::Stop();
#include "swift/AST/ExprNodes.def"

    return Action::VisitChildrenIf(bool(recurse), E);
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
      return Action::SkipChildren(S);

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

    /// The the function is rethrows/reasync, and it was called with
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

/// A type expressing the result of classifying whether a call or function
/// throws or is async.
class Classification {
  bool IsInvalid = false;  // The AST is malformed.  Don't diagnose.

  ConditionalEffectKind ThrowKind = ConditionalEffectKind::None;
  llvm::Optional<PotentialEffectReason> ThrowReason;

  ConditionalEffectKind AsyncKind = ConditionalEffectKind::None;
  llvm::Optional<PotentialEffectReason> AsyncReason;

  void print(raw_ostream &out) const {
    out << "{ IsInvalid = " << IsInvalid
        << ", ThrowKind = ";
    
    simple_display(out, ThrowKind);
         
    out << ", ThrowReason = ";
    if (!ThrowReason)
      out << "nil";
    else
      out << PotentialEffectReason::kindToString(ThrowReason->getKind());

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

  /// Return a classification for multiple effects.
  static Classification forEffect(EffectList kinds,
                                  ConditionalEffectKind conditionalKind,
                                  PotentialEffectReason reason) {
    Classification result;
    for (auto k : kinds)

      result.merge(forEffect(k, conditionalKind, reason));

    return result;
  }

  static Classification forEffect(EffectKind kind,
                                  ConditionalEffectKind conditionalKind,
                                  PotentialEffectReason reason) {
    Classification result;
    if (kind == EffectKind::Throws) {
      result.ThrowKind = conditionalKind;
      result.ThrowReason = reason;
    } else {
      result.AsyncKind = conditionalKind;
      result.AsyncReason = reason;
    }
    return result;
  }

  /// Return a classification saying that there's a throw / async operation.
  static Classification forUnconditional(EffectKind kind,
                                         PotentialEffectReason reason) {
    return forEffect(kind, ConditionalEffectKind::Always, reason);
  }

  /// Return a classification saying that there's a rethrowing / reasync site.
  static Classification forConditional(EffectKind kind,
                                       PotentialEffectReason reason) {
    return forEffect(kind, ConditionalEffectKind::Conditional, reason);
  }

  /// Used when invalid AST was detected.
  static Classification forInvalidCode() {
    Classification result;
    result.IsInvalid = true;
    return result;
  }

  void merge(Classification other) {
    if (other.AsyncKind > AsyncKind) {
      AsyncKind = other.AsyncKind;
      AsyncReason = other.AsyncReason;
    }
    
    if (other.ThrowKind > ThrowKind) {
      ThrowKind = other.ThrowKind;
      ThrowReason = other.ThrowReason;
    }
  }

  bool isInvalid() const { return IsInvalid; }
  ConditionalEffectKind getConditionalKind(EffectKind kind) const {
    switch (kind) {
    case EffectKind::Throws: return ThrowKind;
    case EffectKind::Async: return AsyncKind;
    }
    llvm_unreachable("Bad effect kind");
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
  /// The key to this cache is a local function decl or closure. The value
  /// is None when an error detected was detected.
  llvm::DenseMap<AnyFunctionRef, llvm::Optional<ConditionalEffectKind>>
      ThrowsCache;
  llvm::DenseMap<AnyFunctionRef, llvm::Optional<ConditionalEffectKind>>
      AsyncCache;

public:
  DeclContext *RethrowsDC = nullptr;
  DeclContext *ReasyncDC = nullptr;

  DeclContext *getPolymorphicEffectDeclContext(EffectKind kind) const {
    switch (kind) {
    case EffectKind::Throws: return RethrowsDC;
    case EffectKind::Async: return ReasyncDC;
    }
  }

  Classification classifyConformance(ProtocolConformanceRef conformanceRef,
                                     EffectKind kind) {
    if (conformanceRef.hasEffect(kind)) {
      // FIXME: Should be ::Always if its not one of our
      // input conformances
      return Classification::forConditional(kind,
        PotentialEffectReason::forConformance());
    }

    return Classification();
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
    auto conformances = fnRef.getSubstitutions().getConformances();
    const auto hasAnyConformances = !conformances.empty();

    // If the function doesn't have any effects or conformances, we're done
    // here.
    if (!fnType->isThrowing() &&
        !E->implicitlyThrows() &&
        !fnType->isAsync() &&
        !E->isImplicitlyAsync() &&
        !hasAnyConformances) {
      return Classification();
    }

    // Decompose the application.
    auto *args = E->getArgs();

    // If any of the arguments didn't type check, fail.
    for (auto arg : *args) {
      auto *argExpr = arg.getExpr();
      if (!argExpr->getType() || argExpr->getType()->hasError())
        return Classification::forInvalidCode();
    }

    Classification result;

    auto classifyApplyEffect = [&](EffectKind kind) {
      if (!fnType->hasEffect(kind) &&
          !(kind == EffectKind::Async && E->isImplicitlyAsync()) &&
          !(kind == EffectKind::Throws && E->implicitlyThrows())) {
        return;
      }

      // Handle rethrowing and reasync functions.
      switch (fnRef.getPolymorphicEffectKind(kind)) {
      case PolymorphicEffectKind::ByConformance: {
        auto substitutions = fnRef.getSubstitutions();
        for (auto conformanceRef : substitutions.getConformances())
          result.merge(classifyConformance(conformanceRef, kind));

        // 'ByConformance' is a superset of 'ByClosure', so check for
        // closure arguments too.
        LLVM_FALLTHROUGH;
      }

      case PolymorphicEffectKind::ByClosure: {
        // We need to walk the original parameter types in parallel
        // because it only counts for rethrows/reasync purposes if it
        // lines up with a throws/async function parameter in the
        // original type.
        auto *origType = fnRef.getType()->getAs<AnyFunctionType>();
        if (!origType) {
          result.merge(Classification::forInvalidCode());
          return;
        }

        // Use the most significant result from the arguments.
        auto params = origType->getParams();
        if (params.size() != args->size()) {
          result.merge(Classification::forInvalidCode());
          return;
        }

        for (unsigned i = 0, e = params.size(); i < e; ++i) {
          result.merge(classifyArgument(args->getExpr(i),
                                        params[i].getParameterType(),
                                        kind));
        }

        return;
      }

      case PolymorphicEffectKind::None:
      case PolymorphicEffectKind::Always:
      case PolymorphicEffectKind::Invalid:
        break;
      }

      // Try to classify the implementation of functions that we have
      // local knowledge of.
      //
      // An autoclosure callee here only appears in a narrow case where
      // we're in the initializer of an 'async let'.
      if (fnRef.isAutoClosure()) {
        result.merge(Classification::forUnconditional(
            kind, PotentialEffectReason::forApply()));
      } else {
        result.merge(
          classifyFunctionBody(fnRef,
                               PotentialEffectReason::forApply(),
                               kind));
        assert(result.getConditionalKind(kind)
               != ConditionalEffectKind::None &&
               "body classification decided function had no effect?");
      }
    };

    classifyApplyEffect(EffectKind::Throws);
    classifyApplyEffect(EffectKind::Async);

    return result;
  }

  /// Classify a single expression without considering its enclosing context.
  ConditionalEffectKind classifyExpr(Expr *expr, EffectKind kind) {
    switch (kind) {
    case EffectKind::Throws: {
      FunctionThrowsClassifier classifier(*this);
      expr->walk(classifier);
      return classifier.ThrowKind;
    }
    case EffectKind::Async: {
      FunctionAsyncClassifier classifier(*this);
      expr->walk(classifier);
      return classifier.AsyncKind;
    }
    }
    llvm_unreachable("Bad effect");
  }

private:
  /// Classify a throwing or async function according to our local
  /// knowledge of its implementation.
  Classification
  classifyFunctionBody(const AbstractFunction &fn,
                       PotentialEffectReason reason,
                       EffectKind kind) {
    switch (fn.getKind()) {
    case AbstractFunction::Opaque:
      return Classification::forUnconditional(kind, reason);
    case AbstractFunction::Parameter:
      return classifyParameterBody(fn.getParameter(), reason, kind);
    case AbstractFunction::Function:
      return classifyFunctionBody(fn.getFunction(), reason, kind);
    case AbstractFunction::Closure:
      return classifyFunctionBody(fn.getClosure(), reason, kind);
    }
    llvm_unreachable("bad abstract function kind");
  }

  Classification classifyParameterBody(ParamDecl *param,
                                       PotentialEffectReason reason,
                                       EffectKind kind) {
    assert(param->getInterfaceType()
               ->lookThroughAllOptionalTypes()
               ->castTo<AnyFunctionType>()
               ->hasEffect(kind) ||
           !param->getInterfaceType()
               ->lookThroughAllOptionalTypes()
               ->castTo<AnyFunctionType>()
               ->getGlobalActor().isNull());

    // If we're currently doing rethrows-checking on the body of the
    // function which declares the parameter, it's rethrowing-only.
    auto *ParentDC = getPolymorphicEffectDeclContext(kind);
    if (ParentDC == param->getDeclContext())
      return Classification::forConditional(kind, reason);

    // Otherwise, it throws unconditionally.
    return Classification::forUnconditional(kind, reason);
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
                                      PotentialEffectReason reason,
                                      EffectKind kind) {
    // Functions can't be rethrowing-only unless they're defined
    // within the rethrows context.
    if (!isLocallyDefinedInPolymorphicEffectDeclContext(fn, kind) ||
        !fn->hasBody())
      return Classification::forUnconditional(kind, reason);

    auto conditionalKind = classifyFunctionBodyImpl(fn, fn->getBody(),
                                                    /*allowNone*/ false,
                                                    kind);
    if (conditionalKind.has_value()) {
      return Classification::forEffect(kind,
                                       conditionalKind.value(),
                                       reason);
    }
    return Classification::forInvalidCode();
  }

  Classification classifyFunctionBody(AbstractClosureExpr *closure,
                                      PotentialEffectReason reason,
                                      EffectKind kind) {
    bool isAutoClosure = isa<AutoClosureExpr>(closure);

    // Closures can't be rethrowing-only unless they're defined
    // within the rethrows context.
    if (!isAutoClosure &&
        !isLocallyDefinedInPolymorphicEffectDeclContext(closure, kind))
      return Classification::forUnconditional(kind, reason);

    BraceStmt *body;
    if (auto autoclosure = dyn_cast<AutoClosureExpr>(closure)) {
      body = autoclosure->getBody();
    } else {
      body = cast<ClosureExpr>(closure)->getBody();
    }
    if (!body) return Classification::forInvalidCode();

    auto conditionalKind = classifyFunctionBodyImpl(closure, body,
                                                    /*allowNone*/ isAutoClosure,
                                                    kind);
    if (conditionalKind.has_value()) {
      return Classification::forEffect(kind,
                                       conditionalKind.value(),
                                       reason);
    }
    return Classification::forInvalidCode();
  }

  class FunctionThrowsClassifier
      : public EffectsHandlingWalker<FunctionThrowsClassifier> {
    ApplyClassifier &Self;
  public:
    bool IsInvalid = false;
    ConditionalEffectKind ThrowKind = ConditionalEffectKind::None;
    FunctionThrowsClassifier(ApplyClassifier &self) : Self(self) {}

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
      return ShouldNotRecurse;
    }
    ShouldRecurse_t checkOptionalTry(OptionalTryExpr *E) {
      return ShouldNotRecurse;
    }
    ShouldRecurse_t checkApply(ApplyExpr *E) {
      auto classification = Self.classifyApply(E);
      IsInvalid |= classification.isInvalid();
      ThrowKind = std::max(ThrowKind, classification.getConditionalKind(EffectKind::Throws));
      return ShouldRecurse;
    }
    ShouldRecurse_t checkLookup(LookupExpr *E) {
      if (auto getter = getEffectfulGetOnlyAccessor(E->getMember()))
        if (getter->hasThrows())
          ThrowKind = ConditionalEffectKind::Always;

      return ShouldRecurse;
    }
    ShouldRecurse_t checkDeclRef(DeclRefExpr *E) {
      if (auto getter = getEffectfulGetOnlyAccessor(E->getDeclRef()))
        if (getter->hasThrows())
          ThrowKind = ConditionalEffectKind::Always;

      return ShouldNotRecurse;
    }
    ShouldRecurse_t checkAsyncLet(PatternBindingDecl *patternBinding) {
      return ShouldRecurse;
    }
    ShouldRecurse_t checkThrow(ThrowStmt *E) {
      ThrowKind = ConditionalEffectKind::Always;
      return ShouldRecurse;
    }
    ShouldRecurse_t checkInterpolatedStringLiteral(InterpolatedStringLiteralExpr *E) {
      return ShouldRecurse;
    }

    ShouldRecurse_t checkIfConfig(IfConfigDecl *D) {
      return ShouldRecurse;
    }

    ShouldRecurse_t checkForEach(ForEachStmt *S) {
      if (S->getTryLoc().isValid()) {
        auto classification = Self.classifyConformance(
            S->getSequenceConformance(), EffectKind::Throws);
        IsInvalid |= classification.isInvalid();
        ThrowKind = std::max(ThrowKind,
                             classification.getConditionalKind(EffectKind::Throws));
      }

      return ShouldRecurse;
    }

    ConditionalEffectKind checkExhaustiveDoBody(DoCatchStmt *S) {
      // All errors thrown by the do body are caught, but any errors thrown
      // by the catch bodies are bounded by the throwing kind of the do body.
      auto savedResult = ThrowKind;
      ThrowKind = ConditionalEffectKind::None;
      S->getBody()->walk(*this);
      auto doThrowingKind = ThrowKind;
      ThrowKind = savedResult;
      return doThrowingKind;
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
        auto savedResult = ThrowKind;
        ThrowKind = ConditionalEffectKind::None;
        S->getBody()->walk(*this);
        auto boundedResult = std::min(doThrowingKind, ThrowKind);
        ThrowKind = std::max(savedResult, boundedResult);
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
        if (getter->hasAsync())
          AsyncKind = ConditionalEffectKind::Always;
      }

      return ShouldRecurse;
    }
    ShouldRecurse_t checkDeclRef(DeclRefExpr *E) {
      if (E->isImplicitlyAsync()) {
        AsyncKind = ConditionalEffectKind::Always;
      } else if (auto getter = getEffectfulGetOnlyAccessor(E->getDeclRef())) {
        if (getter->hasAsync())
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

    void visitExprPre(Expr *expr) { return; }
  };

  llvm::Optional<ConditionalEffectKind>
  classifyFunctionBodyImpl(AnyFunctionRef key, BraceStmt *body, bool allowNone,
                           EffectKind kind) {
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
    Cache.insert({key, ConditionalEffectKind::Conditional});

    // Walk the body.
    ConditionalEffectKind result;
    switch (kind) {
    case EffectKind::Throws: {
      FunctionThrowsClassifier classifier(*this);
      body->walk(classifier);
      result = classifier.ThrowKind;
      if (classifier.IsInvalid) {
        // Represent invalid code as being null.
        Cache[key] = llvm::Optional<ConditionalEffectKind>();
        return llvm::Optional<ConditionalEffectKind>();
      }
      break;
    }
    case EffectKind::Async: {
      FunctionAsyncClassifier classifier(*this);
      body->walk(classifier);
      result = classifier.AsyncKind;
      if (classifier.IsInvalid) {
        // Represent invalid code as being null.
        Cache[key] = llvm::Optional<ConditionalEffectKind>();
        return llvm::Optional<ConditionalEffectKind>();
      }
      break;
    }
    }

    // The body result cannot be 'none' unless it's an autoclosure.
    if (!allowNone) {
      result = ConditionalEffectKind::Conditional;
    }

    // Remember the result.
    Cache[key] = result;
    return result;
  }

  /// Classify an argument being passed to a rethrows/reasync function.
  Classification classifyArgument(Expr *arg, Type paramType, EffectKind kind) {
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

      return classifyArgumentByType(arg->getType(),
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
        return classifyTupleArgument(tuple, paramTupleType, kind);
      }

      if (paramTupleType->getNumElements() != 1) {
        // Otherwise, we're passing an opaque tuple expression, and we
        // should treat it as contributing to 'rethrows' if the original
        // parameter type included a throwing function type.
        return classifyArgumentByType(
                                    paramType,
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
                                       EffectKind kind) {
    if (paramTupleType->getNumElements() != tuple->getNumElements())
      return Classification::forInvalidCode();

    Classification result;
    for (unsigned i : indices(tuple->getElements())) {
      result.merge(classifyArgument(tuple->getElement(i),
                                    paramTupleType->getElementType(i),
                                    kind));
    }
    return result;
  }

  /// Given the type of an argument, try to determine if it contains
  /// a throws/async function in a way that is permitted to cause a
  /// rethrows/reasync function to throw/async.
  static Classification classifyArgumentByType(Type paramType,
                                               PotentialEffectReason reason,
                                               EffectKind kind) {
    if (!paramType || paramType->hasError())
      return Classification::forInvalidCode();

    if (hasFunctionParameterWithEffect(kind, paramType))
      return Classification::forUnconditional(kind, reason);

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

    /// The initializer for an enum element.
    EnumElementInitializer,

    /// The pattern of a catch.
    CatchPattern,

    /// The guard expression controlling a catch.
    CatchGuard,

    /// A defer body
    DeferBody,

    // A runtime discoverable attribute initialization expression.
    RuntimeAttribute,
  };

private:
  static Context getContextForPatternBinding(PatternBindingDecl *pbd) {
    if (!pbd->isStatic() && pbd->getDeclContext()->isTypeContext()) {
      return Context(Kind::IVarInitializer);
    } else {
      return Context(Kind::GlobalVarInitializer);
    }
  }

  Kind TheKind;
  llvm::Optional<AnyFunctionRef> Function;
  bool HandlesErrors = false;
  bool HandlesAsync = false;

  /// Whether error-handling queries should ignore the function context, e.g.,
  /// for autoclosure and rethrows checks.
  bool ErrorHandlingIgnoresFunction = false;
  bool IsNonExhaustiveCatch = false;
  bool DiagnoseErrorOnTry = false;
  InterpolatedStringLiteralExpr *InterpolatedString = nullptr;

  explicit Context(Kind kind)
      : TheKind(kind), Function(llvm::None), HandlesErrors(false) {
    assert(TheKind != Kind::PotentiallyHandled);
  }

  explicit Context(bool handlesErrors, bool handlesAsync,
                   llvm::Optional<AnyFunctionRef> function)
      : TheKind(Kind::PotentiallyHandled), Function(function),
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
                   /*handlesAsync=*/D->isAsyncContext(), llvm::None);
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

    return Context(D->hasThrows(), D->isAsyncContext(), AnyFunctionRef(D));
  }

  static Context forDeferBody() {
    return Context(Kind::DeferBody);
  }

  static Context forInitializer(Initializer *init) {
    if (isa<DefaultArgumentInitializer>(init)) {
      return Context(Kind::DefaultArgument);
    }

    if (isa<PropertyWrapperInitializer>(init)) {
      return Context(Kind::PropertyWrapper);
    }

    if (isa<RuntimeAttributeInitializer>(init)) {
      return Context(Kind::RuntimeAttribute);
    }

    auto *binding = cast<PatternBindingInitializer>(init)->getBinding();
    assert(!binding->getDeclContext()->isLocalContext() &&
           "setting up error context for local pattern binding?");
    return getContextForPatternBinding(binding);
  }

  static Context forEnumElementInitializer(EnumElementDecl *elt) {
    return Context(Kind::EnumElementInitializer);
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

    return Context(closureTypeThrows, closureTypeIsAsync, AnyFunctionRef(E));
  }

  static Context forCatchPattern(CaseStmt *S) {
    return Context(Kind::CatchPattern);
  }

  static Context forCatchGuard(CaseStmt *S) {
    return Context(Kind::CatchGuard);
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
                                  const PotentialEffectReason &reason) {
    auto &Diags = ctx.Diags;
    auto message = diag::throwing_call_without_try;
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

    Diags.diagnose(loc, message).highlight(highlight);
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
    case Kind::IVarInitializer:
    case Kind::DefaultArgument:
    case Kind::PropertyWrapper:
    case Kind::CatchPattern:
    case Kind::CatchGuard:
    case Kind::DeferBody:
    case Kind::RuntimeAttribute:
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
    case Kind::IVarInitializer:
    case Kind::DefaultArgument:
    case Kind::PropertyWrapper:
    case Kind::CatchPattern:
    case Kind::CatchGuard:
    case Kind::DeferBody:
    case Kind::RuntimeAttribute:
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
    case Kind::IVarInitializer:
    case Kind::DefaultArgument:
    case Kind::PropertyWrapper:
    case Kind::CatchPattern:
    case Kind::CatchGuard:
    case Kind::DeferBody:
    case Kind::RuntimeAttribute:
      assert(!DiagnoseErrorOnTry);
      // Diagnosed at the call sites.
      return;
    }
    llvm_unreachable("bad context kind");
  }
  /// I did not want to add 'await' as a PotentialEffectReason, since it's
  /// not actually an effect. So, we have this odd boolean hanging around.
  unsigned
  effectReasonToIndex(llvm::Optional<PotentialEffectReason> maybeReason,
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
                var->getName(), static_cast<unsigned>(getKind()));
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
  void
  diagnoseUnhandledAsyncSite(DiagnosticEngine &Diags, ASTNode node,
                             llvm::Optional<PotentialEffectReason> maybeReason,
                             bool forAwait = false) {
    if (node.isImplicit())
      return;

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
    case Kind::IVarInitializer:
    case Kind::DefaultArgument:
    case Kind::PropertyWrapper:
    case Kind::CatchPattern:
    case Kind::CatchGuard:
    case Kind::DeferBody:
    case Kind::RuntimeAttribute:
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
                   PotentialEffectReason reason) :
      reason(reason),
      expr(failingExpr) {}

    /// Reason for throwing
    PotentialEffectReason reason;

    /// Failing expression
    Expr &expr;
  };

  SmallVector<Expr *, 4> errorOrder;
  llvm::DenseMap<Expr *, std::vector<DiagnosticInfo>> uncoveredAsync;
  llvm::DenseMap<Expr *, Expr *> parentMap;

  static bool isEffectAnchor(Expr *e) {
    return isa<AbstractClosureExpr>(e) || isa<DiscardAssignmentExpr>(e) ||
           isa<AssignExpr>(e);
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
    ContextScope(CheckEffectsCoverage &self, llvm::Optional<Context> newContext)
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
      Self.Flags.set(ContextFlags::IsTryCovered);
      Self.Flags.set(ContextFlags::IsAsyncCovered);
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
    ContextScope scope(*this, llvm::None);
    assert(!Flags.has(ContextFlags::IsInTry) && "do/catch within try?");
    scope.resetCoverageForDoCatch();

    // If the enclosing context doesn't handle anything, use a
    // specialized diagnostic about non-exhaustive catches.
    if (!CurContext.handlesThrows(ConditionalEffectKind::Conditional)) {
      CurContext.setNonExhaustiveCatch(true);
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
    for (auto &LabelItem : S->getMutableCaseLabelItems()) {
      // The pattern and guard aren't allowed to throw.
      {
        ContextScope scope(*this, Context::forCatchPattern(S));
        LabelItem.getPattern()->walk(*this);
      }
      if (auto guard = LabelItem.getGuardExpr()) {
        ContextScope scope(*this, Context::forCatchGuard(S));
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
    ApplyClassifier classifier;
    classifier.RethrowsDC = RethrowsDC;
    classifier.ReasyncDC = ReasyncDC;
    auto classification = classifier.classifyApply(E);

    checkThrowAsyncSite(E, /*requiresTry*/ true, classification);

    if (!classification.isInvalid()) {
      // HACK: functions can get queued multiple times in
      // definedFunctions, so be sure to be idempotent.
      if (!E->isThrowsSet()) {
        auto throwsKind = classification.getConditionalKind(EffectKind::Throws);
        E->setThrows(throwsKind == ConditionalEffectKind::Conditional ||
                     throwsKind == ConditionalEffectKind::Always);
      }

      auto asyncKind = classification.getConditionalKind(EffectKind::Async);
      E->setNoAsync(asyncKind == ConditionalEffectKind::None);
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


  static EffectList gatherEffects(AbstractFunctionDecl *afd) {
    EffectList effects;
    if (afd->hasAsync()) effects.push_back(EffectKind::Async);
    if (afd->hasThrows()) effects.push_back(EffectKind::Throws);
    return effects;
  }

  /// check the kind of property with an effect to give better diagnostics
  static PotentialEffectReason getKindOfEffectfulProp(ConcreteDeclRef cdr) {
    if (isa<SubscriptDecl>(cdr.getDecl()))
      return PotentialEffectReason::forSubscriptAccess();

    assert(isa<VarDecl>(cdr.getDecl()));
    return PotentialEffectReason::forPropertyAccess();
  }

  ShouldRecurse_t checkLookup(LookupExpr *E) {
    auto member = E->getMember();
    if (auto getter = getEffectfulGetOnlyAccessor(member)) {
      auto effects = gatherEffects(getter);

      // We might have a situation where the getter is just 'throws', but
      // this specific Lookup is implicitly async due to actor-isolation.
      if (E->isImplicitlyAsync()) {
        assert(!getter->hasAsync()
                   && "an explicitly async decl accessed implicitly-async?");
        effects.push_back(EffectKind::Async);
      }

      bool requiresTry = getter->hasThrows();
      checkThrowAsyncSite(E, requiresTry,
                          Classification::forEffect(effects,
                                  ConditionalEffectKind::Always,
                                  getKindOfEffectfulProp(member)));

    } else {
      EffectList effects;
      bool requiresTry = false;
      if (E->isImplicitlyAsync()) {
        effects.push_back(EffectKind::Async);
      }
      if (E->isImplicitlyThrows()) {
        // E.g. it may be a distributed computed property, accessed across actors.
        effects.push_back(EffectKind::Throws);
        requiresTry = true;
      }

      if (!effects.empty()) {
        checkThrowAsyncSite(E, requiresTry,
                            Classification::forEffect(effects,
                                                      ConditionalEffectKind::Always,
                                                      getKindOfEffectfulProp(member)));
      }
    }

    return ShouldRecurse;
  }

  ShouldRecurse_t checkDeclRef(DeclRefExpr *E) {
    if (auto getter = getEffectfulGetOnlyAccessor(E->getDeclRef())) {
      auto effects = gatherEffects(getter);

      // We might have a situation where the getter is just 'throws', but
      // this specific DeclRef is implicitly async due to actor-isolation.
      if (E->isImplicitlyAsync()) {
        assert(!getter->hasAsync()
                && "an explicitly async decl accessed implicitly-async?");
        effects.push_back(EffectKind::Async);
      }

      checkThrowAsyncSite(E, getter->hasThrows(),
                          Classification::forEffect(effects,
                                  ConditionalEffectKind::Always,
                                  PotentialEffectReason::forPropertyAccess()));

    } else if (E->isImplicitlyAsync()) {
      checkThrowAsyncSite(E, /*requiresTry=*/E->isImplicitlyThrows(),
            Classification::forUnconditional(EffectKind::Async,
                                   PotentialEffectReason::forPropertyAccess()));

    } else if (auto decl = E->getDecl()) {
      if (auto var = dyn_cast<VarDecl>(decl)) {
        // "Async let" declarations are treated as an asynchronous call
        // (to the underlying task's "get"). If the initializer was throwing,
        // then the access is also treated as throwing.
        if (var->isAsyncLet()) {
          // If the initializer could throw, we will have a 'try' in the
          // application of its autoclosure.
          bool throws = false;
          if (auto init = var->getParentInitializer()) {
            if (auto await = dyn_cast<AwaitExpr>(init))
              init = await->getSubExpr();
            if (isa<TryExpr>(init))
              throws = true;
          }

          auto result = Classification::forUnconditional(
                       EffectKind::Async,
                       PotentialEffectReason::forAsyncLet());
          if (throws) {
            result.merge(Classification::forUnconditional(
                           EffectKind::Throws,
                           PotentialEffectReason::forAsyncLet()));
          }
          checkThrowAsyncSite(E, /*requiresTry=*/throws, result);

        }
      }
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
    MaxThrowingKind = std::max(MaxThrowingKind, ConditionalEffectKind::Always);

    Flags.set(ContextFlags::HasAnyThrowSite);

    if (!CurContext.handlesThrows(ConditionalEffectKind::Always))
      CurContext.diagnoseUnhandledThrowStmt(Ctx.Diags, S);

    return ShouldRecurse;
  }

  void checkThrowAsyncSite(ASTNode E, bool requiresTry,
                           const Classification &classification) {
    // Suppress all diagnostics when there's an un-analyzable throw/async site.
    if (classification.isInvalid()) {
      Flags.set(ContextFlags::HasAnyThrowSite);
      Flags.set(ContextFlags::HasAnyAsyncSite);
      if (requiresTry) Flags.set(ContextFlags::HasTryThrowSite);
      return;
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
      else if (!Flags.has(ContextFlags::IsAsyncCovered)) {
        Expr *expr = E.dyn_cast<Expr*>();
        Expr *anchor = walkToAnchor(expr, parentMap,
                                    CurContext.isWithinInterpolatedString());
        if (uncoveredAsync.find(anchor) == uncoveredAsync.end())
          errorOrder.push_back(anchor);
        uncoveredAsync[anchor].emplace_back(*expr,
                                            classification.getAsyncReason());
      }
    }

    // Check throwing calls.
    MaxThrowingKind = std::max(MaxThrowingKind, throwsKind);

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
        (!requiresTry || Flags.has(ContextFlags::IsTryCovered));
      if (!CurContext.handlesThrows(throwsKind)) {
        CurContext.diagnoseUnhandledThrowSite(Ctx.Diags, E, isTryCovered,
                                              classification.getThrowReason());
      } else if (!isTryCovered) {
        CurContext.diagnoseUncoveredThrowSite(Ctx, E, // we want this one to trigger
                                              classification.getThrowReason());
      }
      break;
    }
  }

  ShouldRecurse_t checkAwait(AwaitExpr *E) {

    // Walk the operand.
    ContextScope scope(*this, llvm::None);
    scope.enterAwait(E->getAwaitLoc());

    E->getSubExpr()->walk(*this);

    // Warn about 'await' expressions that weren't actually needed, unless of
    // course we're in a context that could never handle an 'async'. Then, we
    // produce an error.
    if (!Flags.has(ContextFlags::HasAnyAsyncSite)) {
      if (CurContext.handlesAsync(ConditionalEffectKind::Conditional)) {
        diagnoseRedundantAwait(E);
      } else {
        CurContext.diagnoseUnhandledAsyncSite(Ctx.Diags, E, llvm::None,
                                              /*forAwait=*/true);
      }
    }

    // Inform the parent of the walk that an 'await' exists here.
    scope.preserveCoverageFromAwaitOperand();
    return ShouldNotRecurse;
  }
  
  ShouldRecurse_t checkTry(TryExpr *E) {
    // Walk the operand.
    ContextScope scope(*this, llvm::None);
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
      CurContext.diagnoseUnhandledAsyncSite(Ctx.Diags, S, llvm::None);

    ApplyClassifier classifier;
    classifier.RethrowsDC = RethrowsDC;
    classifier.ReasyncDC = ReasyncDC;

    // A 'for try await' might be effect polymorphic via the conformance
    // in a 'rethrows' function body.
    if (S->getTryLoc().isValid()) {
      auto classification = classifier.classifyConformance(
          S->getSequenceConformance(), EffectKind::Throws);
      auto throwsKind = classification.getConditionalKind(EffectKind::Throws);

      if (throwsKind != ConditionalEffectKind::None)
        Flags.set(ContextFlags::HasAnyThrowSite);

      if (!CurContext.handlesThrows(throwsKind))
        CurContext.diagnoseUnhandledThrowStmt(Ctx.Diags, S);
    }

    return ShouldRecurse;
  }

  void diagnoseRedundantTry(AnyTryExpr *E) const {
    if (auto *SVE = SingleValueStmtExpr::tryDigOutSingleValueStmtExpr(E)) {
      // For an if/switch expression, produce an error instead of a warning.
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
      // For an if/switch expression, produce an error instead of a warning.
      Ctx.Diags.diagnose(E->getAwaitLoc(),
                         diag::effect_marker_on_single_value_stmt,
                         "await", SVE->getStmt()->getKind())
        .highlight(E->getAwaitLoc());
      return;
    }
    Ctx.Diags.diagnose(E->getAwaitLoc(), diag::no_async_in_await);
  }

  void diagnoseUncoveredAsyncSite(const Expr *anchor) const {
    auto asyncPointIter = uncoveredAsync.find(anchor);
    if (asyncPointIter == uncoveredAsync.end())
      return;
    const std::vector<DiagnosticInfo> &errors = asyncPointIter->getSecond();
    SourceLoc awaitInsertLoc = anchor->getStartLoc();
    if (const AnyTryExpr *tryExpr = dyn_cast<AnyTryExpr>(anchor))
      awaitInsertLoc = tryExpr->getSubExpr()->getStartLoc();
    else if (const AutoClosureExpr *autoClosure = dyn_cast<AutoClosureExpr>(anchor)) {
      if (const AnyTryExpr *tryExpr = dyn_cast<AnyTryExpr>(autoClosure->getSingleExpressionBody()))
        awaitInsertLoc = tryExpr->getSubExpr()->getStartLoc();
    }

    Ctx.Diags.diagnose(anchor->getStartLoc(), diag::async_expr_without_await)
      .fixItInsert(awaitInsertLoc, "await ")
      .highlight(anchor->getSourceRange());

    for (const DiagnosticInfo &diag: errors) {
      switch (diag.reason.getKind()) {
        case PotentialEffectReason::Kind::AsyncLet:
          if (auto declR = dyn_cast<DeclRefExpr>(&diag.expr)) {
            if (auto var = dyn_cast<VarDecl>(declR->getDecl())) {
              if (var->isAsyncLet()) {
                Ctx.Diags.diagnose(declR->getLoc(),
                                   diag::async_let_without_await,
                                   var->getName());
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
                                callee->getDescriptiveKind(), callee->getName());
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

      return Action::SkipChildren();
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
      isDeferBody ? Context::forDeferBody() : Context::forFunction(fn);
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

bool TypeChecker::canThrow(Expr *expr) {
  ApplyClassifier classifier;
  auto effect = classifier.classifyExpr(expr, EffectKind::Throws);
  return (effect != ConditionalEffectKind::None);
}
