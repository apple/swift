//===--- Expr.cpp - Swift Language Expression ASTs ------------------------===//
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
//  This file implements the Expr class and subclasses.
//
//===----------------------------------------------------------------------===//

#include "swift/AST/Expr.h"
#include "swift/Basic/Statistic.h"
#include "swift/Basic/Unicode.h"
#include "swift/AST/ASTContext.h"
#include "swift/AST/ASTVisitor.h"
#include "swift/AST/Decl.h" // FIXME: Bad dependency
#include "swift/AST/MacroDiscriminatorContext.h"
#include "swift/AST/ParameterList.h"
#include "swift/AST/Stmt.h"
#include "swift/AST/ASTWalker.h"
#include "swift/AST/AvailabilitySpec.h"
#include "swift/AST/PrettyStackTrace.h"
#include "swift/AST/TypeCheckRequests.h"
#include "swift/AST/TypeLoc.h"
#include "llvm/ADT/APFloat.h"
#include "llvm/ADT/PointerUnion.h"
#include "llvm/ADT/SetVector.h"
#include "llvm/ADT/Twine.h"
using namespace swift;

#define EXPR(Id, _) \
  static_assert(IsTriviallyDestructible<Id##Expr>::value, \
                "Exprs are BumpPtrAllocated; the destructor is never called");
#include "swift/AST/ExprNodes.def"

StringRef swift::getFunctionRefKindStr(FunctionRefKind refKind) {
  switch (refKind) {
  case FunctionRefKind::Unapplied:
    return "unapplied";
  case FunctionRefKind::SingleApply:
    return "single";
  case FunctionRefKind::DoubleApply:
    return "double";
  case FunctionRefKind::Compound:
    return "compound";
  }

  llvm_unreachable("Unhandled FunctionRefKind in switch.");
}

//===----------------------------------------------------------------------===//
// Expr methods.
//===----------------------------------------------------------------------===//

StringRef Expr::getKindName(ExprKind K) {
  switch (K) {
#define EXPR(Id, Parent) case ExprKind::Id: return #Id;
#include "swift/AST/ExprNodes.def"
  }
  llvm_unreachable("bad ExprKind");
}

template <class T> static SourceLoc getStartLocImpl(const T *E);
template <class T> static SourceLoc getEndLocImpl(const T *E);
template <class T> static SourceLoc getLocImpl(const T *E);

// Helper functions to check statically whether a method has been
// overridden from its implementation in Expr.  The sort of thing you
// need when you're avoiding v-tables.
namespace {
  template <typename ReturnType, typename Class>
  constexpr bool isOverriddenFromExpr(ReturnType (Class::*)() const) {
    return true;
  }
  template <typename ReturnType>
  constexpr bool isOverriddenFromExpr(ReturnType (Expr::*)() const) {
    return false;
  }

  template <bool IsOverridden> struct Dispatch;

  /// Dispatch down to a concrete override.
  template <> struct Dispatch<true> {
    template <class T> static SourceLoc getStartLoc(const T *E) {
      return E->getStartLoc();
    }
    template <class T> static SourceLoc getEndLoc(const T *E) {
      return E->getEndLoc();
    }
    template <class T> static SourceLoc getLoc(const T *E) {
      return E->getLoc();
    }
    template <class T> static SourceRange getSourceRange(const T *E) {
      return E->getSourceRange();
    }
  };

  /// Default implementations for when a method isn't overridden.
  template <> struct Dispatch<false> {
    template <class T> static SourceLoc getStartLoc(const T *E) {
      return E->getSourceRange().Start;
    }
    template <class T> static SourceLoc getEndLoc(const T *E) {
      return E->getSourceRange().End;
    }
    template <class T> static SourceLoc getLoc(const T *E) {
      return getStartLocImpl(E);
    }
    template <class T> static SourceRange getSourceRange(const T *E) {
      if (E->getStartLoc().isInvalid() != E->getEndLoc().isInvalid())
        return SourceRange();
      return { E->getStartLoc(), E->getEndLoc() };
    }
  };
} // end anonymous namespace

void Expr::setType(Type T) {
  assert(!T || !T->hasTypeVariable() || !T->hasPlaceholder());
  Ty = T;
}

void Expr::setImplicit(bool Implicit) {
  assert(!isa<TypeExpr>(this) || getType() &&
         "Cannot make a TypeExpr implicit without a contextual type.");
  Bits.Expr.Implicit = Implicit;
}

template <class T> static SourceRange getSourceRangeImpl(const T *E) {
  static_assert(isOverriddenFromExpr(&T::getSourceRange) ||
                (isOverriddenFromExpr(&T::getStartLoc) &&
                 isOverriddenFromExpr(&T::getEndLoc)),
                "Expr subclass must implement either getSourceRange() "
                "or getStartLoc()/getEndLoc()");
  return Dispatch<isOverriddenFromExpr(&T::getSourceRange)>::getSourceRange(E);
}

SourceRange Expr::getSourceRange() const {
  switch (getKind()) {
#define EXPR(ID, PARENT)                                           \
  case ExprKind::ID: return getSourceRangeImpl(cast<ID##Expr>(this));
#include "swift/AST/ExprNodes.def"
  }
  
  llvm_unreachable("expression type not handled!");
}

template <class T> static SourceLoc getStartLocImpl(const T *E) {
  return Dispatch<isOverriddenFromExpr(&T::getStartLoc)>::getStartLoc(E);
}
SourceLoc Expr::getStartLoc() const {
  switch (getKind()) {
#define EXPR(ID, PARENT)                                           \
  case ExprKind::ID: return getStartLocImpl(cast<ID##Expr>(this));
#include "swift/AST/ExprNodes.def"
  }

  llvm_unreachable("expression type not handled!");
}

template <class T> static SourceLoc getEndLocImpl(const T *E) {
  return Dispatch<isOverriddenFromExpr(&T::getEndLoc)>::getEndLoc(E);
}
SourceLoc Expr::getEndLoc() const {
  switch (getKind()) {
#define EXPR(ID, PARENT)                                           \
  case ExprKind::ID: return getEndLocImpl(cast<ID##Expr>(this));
#include "swift/AST/ExprNodes.def"
  }

  llvm_unreachable("expression type not handled!");
}

template <class T> static SourceLoc getLocImpl(const T *E) {
  return Dispatch<isOverriddenFromExpr(&T::getLoc)>::getLoc(E);
}
SourceLoc Expr::getLoc() const {
  switch (getKind()) {
#define EXPR(ID, PARENT)                                           \
  case ExprKind::ID: return getLocImpl(cast<ID##Expr>(this));
#include "swift/AST/ExprNodes.def"
  }

  llvm_unreachable("expression type not handled!");
}

Expr *Expr::getSemanticsProvidingExpr() {
  if (auto *IE = dyn_cast<IdentityExpr>(this))
    return IE->getSubExpr()->getSemanticsProvidingExpr();
  if (auto *TE = dyn_cast<TryExpr>(this))
    return TE->getSubExpr()->getSemanticsProvidingExpr();

  return this;
}

bool Expr::printConstExprValue(llvm::raw_ostream *OS,
                        llvm::function_ref<bool(Expr*)> additionalCheck) const {
  auto print = [&](StringRef text) {
    if (OS) {
      *OS << text;
    }
  };
  auto *E = getSemanticsProvidingExpr();
  assert(E);
  switch(getKind()) {
  case ExprKind::BooleanLiteral: {
    auto isTrue = cast<BooleanLiteralExpr>(E)->getValue();
    print(isTrue ? "true" : "false");
    return true;
  }
  case ExprKind::IntegerLiteral:
  case ExprKind::FloatLiteral:  {
    const auto *NE = cast<NumberLiteralExpr>(E);
    auto digits = NE->getDigitsText();
    assert(!digits.empty());
    if (NE->getMinusLoc().isValid()) {
      print("-");
    }
    print(digits);
    return true;
  }
  case ExprKind::NilLiteral: {
    print("nil");
    return true;
  }
  case ExprKind::StringLiteral: {
    auto *LE = cast<StringLiteralExpr>(E);
    print("\"");
    print(LE->getValue());
    print("\"");
    return true;
  }
  case ExprKind::KeyPath: {
    // FIXME: print keypath
    print("\\.<NOT_IMPLEMENTED>");
    return true;
  }
  case ExprKind::Array:
  case ExprKind::Dictionary: {
    print("[");
    auto *CE = cast<CollectionExpr>(E);
    for (unsigned N = CE->getNumElements(), I = 0; I != N; I ++) {
      auto Ele = CE->getElement(I);
      auto needComma = I + 1 != N;
      if (!Ele->printConstExprValue(OS, additionalCheck)) {
        return false;
      }
      if (needComma)
        print(", ");
    }
    print("]");
    return true;
  }
  case ExprKind::Tuple: {
    print("(");
    auto *TE = cast<TupleExpr>(E);
    for (unsigned N = TE->getNumElements(), I = 0; I != N; I ++) {
      auto Ele = TE->getElement(I);
      auto needComma = I + 1 != N;
      if (!Ele->printConstExprValue(OS, additionalCheck)) {
        return false;
      }
      if (needComma)
        print(", ");
    }
    print(")");
    return true;
  }
  default:
    return additionalCheck && additionalCheck(const_cast<Expr*>(this));
  }
}

bool Expr::isSemanticallyConstExpr(
    llvm::function_ref<bool(Expr*)> additionalCheck) const {
  return printConstExprValue(nullptr, additionalCheck);
}

Expr *Expr::getValueProvidingExpr() {
  Expr *E = getSemanticsProvidingExpr();

  if (auto TE = dyn_cast<ForceTryExpr>(E))
    return TE->getSubExpr()->getValueProvidingExpr();

  // TODO:
  //   - tuple literal projection, which may become interestingly idiomatic

  return E;
}

DeclRefExpr *Expr::getMemberOperatorRef() {
  auto expr = this;
  if (!expr->isImplicit()) return nullptr;

  auto dotSyntax = dyn_cast<DotSyntaxCallExpr>(expr);
  if (!dotSyntax) return nullptr;

  auto operatorRef = dyn_cast<DeclRefExpr>(dotSyntax->getFn());
  if (!operatorRef) return nullptr;

  auto func = dyn_cast<FuncDecl>(operatorRef->getDecl());
  if (!func) return nullptr;

  if (!func->isOperator()) return nullptr;

  return operatorRef;
}

ConcreteDeclRef Expr::getReferencedDecl(bool stopAtParenExpr) const {
  switch (getKind()) {
  // No declaration reference.
  #define NO_REFERENCE(Id) case ExprKind::Id: return ConcreteDeclRef()
  #define SIMPLE_REFERENCE(Id, Getter)          \
    case ExprKind::Id:                          \
      return cast<Id##Expr>(this)->Getter()
  #define PASS_THROUGH_REFERENCE(Id, GetSubExpr)                      \
    case ExprKind::Id:                                                \
      return cast<Id##Expr>(this)                                     \
                 ->GetSubExpr()->getReferencedDecl(stopAtParenExpr)

  NO_REFERENCE(Error);
  SIMPLE_REFERENCE(NilLiteral, getInitializer);
  SIMPLE_REFERENCE(IntegerLiteral, getInitializer);
  SIMPLE_REFERENCE(FloatLiteral, getInitializer);
  SIMPLE_REFERENCE(BooleanLiteral, getInitializer);
  SIMPLE_REFERENCE(StringLiteral, getInitializer);
  SIMPLE_REFERENCE(InterpolatedStringLiteral, getInitializer);
  NO_REFERENCE(RegexLiteral);
  NO_REFERENCE(ObjectLiteral);
  NO_REFERENCE(MagicIdentifierLiteral);
  NO_REFERENCE(DiscardAssignment);
  NO_REFERENCE(LazyInitializer);
  NO_REFERENCE(SingleValueStmt);

  SIMPLE_REFERENCE(DeclRef, getDeclRef);
  SIMPLE_REFERENCE(SuperRef, getSelf);

  NO_REFERENCE(Type);

  SIMPLE_REFERENCE(OtherConstructorDeclRef, getDeclRef);

  PASS_THROUGH_REFERENCE(DotSyntaxBaseIgnored, getRHS);

  // FIXME: Return multiple results?
  case ExprKind::OverloadedDeclRef:
    return ConcreteDeclRef();

  NO_REFERENCE(UnresolvedDeclRef);

  SIMPLE_REFERENCE(MemberRef, getMember);
  SIMPLE_REFERENCE(DynamicMemberRef, getMember);
  SIMPLE_REFERENCE(DynamicSubscript, getMember);

  PASS_THROUGH_REFERENCE(UnresolvedSpecialize, getSubExpr);

  NO_REFERENCE(UnresolvedMember);
  NO_REFERENCE(UnresolvedDot);
  NO_REFERENCE(Sequence);

  case ExprKind::Paren:
    if (stopAtParenExpr) return ConcreteDeclRef();
    return cast<ParenExpr>(this)
               ->getSubExpr()->getReferencedDecl(stopAtParenExpr);

  PASS_THROUGH_REFERENCE(UnresolvedMemberChainResult, getSubExpr);
  PASS_THROUGH_REFERENCE(DotSelf, getSubExpr);
  PASS_THROUGH_REFERENCE(Await, getSubExpr);
  PASS_THROUGH_REFERENCE(Consume, getSubExpr);
  PASS_THROUGH_REFERENCE(Copy, getSubExpr);
  PASS_THROUGH_REFERENCE(Borrow, getSubExpr);
  PASS_THROUGH_REFERENCE(Try, getSubExpr);
  PASS_THROUGH_REFERENCE(ForceTry, getSubExpr);
  PASS_THROUGH_REFERENCE(OptionalTry, getSubExpr);

  NO_REFERENCE(Tuple);
  SIMPLE_REFERENCE(Array, getInitializer);
  SIMPLE_REFERENCE(Dictionary, getInitializer);

  case ExprKind::Subscript: {
    auto subscript = cast<SubscriptExpr>(this);
    if (subscript->hasDecl()) return subscript->getDecl();
    return ConcreteDeclRef();
  }

  NO_REFERENCE(KeyPathApplication);
  NO_REFERENCE(TupleElement);
  NO_REFERENCE(CaptureList);
  NO_REFERENCE(Closure);

  PASS_THROUGH_REFERENCE(AutoClosure, getSingleExpressionBody);
  PASS_THROUGH_REFERENCE(InOut, getSubExpr);

  NO_REFERENCE(VarargExpansion);
  NO_REFERENCE(PackExpansion);
  NO_REFERENCE(PackElement);
  NO_REFERENCE(MaterializePack);
  NO_REFERENCE(DynamicType);

  PASS_THROUGH_REFERENCE(RebindSelfInConstructor, getSubExpr);

  NO_REFERENCE(OpaqueValue);
  NO_REFERENCE(PropertyWrapperValuePlaceholder);
  NO_REFERENCE(AppliedPropertyWrapper);
  NO_REFERENCE(DefaultArgument);

  PASS_THROUGH_REFERENCE(BindOptional, getSubExpr);
  PASS_THROUGH_REFERENCE(OptionalEvaluation, getSubExpr);
  PASS_THROUGH_REFERENCE(ForceValue, getSubExpr);
  PASS_THROUGH_REFERENCE(OpenExistential, getSubExpr);

  NO_REFERENCE(Call);
  NO_REFERENCE(PrefixUnary);
  NO_REFERENCE(PostfixUnary);
  NO_REFERENCE(Binary);
  NO_REFERENCE(DotSyntaxCall);
  NO_REFERENCE(MakeTemporarilyEscapable);
  NO_REFERENCE(ConstructorRefCall);

  PASS_THROUGH_REFERENCE(Load, getSubExpr);
  NO_REFERENCE(DestructureTuple);
  NO_REFERENCE(UnresolvedTypeConversion);
  PASS_THROUGH_REFERENCE(ABISafeConversion, getSubExpr);
  PASS_THROUGH_REFERENCE(FunctionConversion, getSubExpr);
  PASS_THROUGH_REFERENCE(CovariantFunctionConversion, getSubExpr);
  PASS_THROUGH_REFERENCE(CovariantReturnConversion, getSubExpr);
  PASS_THROUGH_REFERENCE(MetatypeConversion, getSubExpr);
  PASS_THROUGH_REFERENCE(CollectionUpcastConversion, getSubExpr);
  PASS_THROUGH_REFERENCE(Erasure, getSubExpr);
  PASS_THROUGH_REFERENCE(AnyHashableErasure, getSubExpr);
  PASS_THROUGH_REFERENCE(DerivedToBase, getSubExpr);
  PASS_THROUGH_REFERENCE(ArchetypeToSuper, getSubExpr);
  PASS_THROUGH_REFERENCE(InjectIntoOptional, getSubExpr);
  PASS_THROUGH_REFERENCE(ClassMetatypeToObject, getSubExpr);
  PASS_THROUGH_REFERENCE(ExistentialMetatypeToObject, getSubExpr);
  PASS_THROUGH_REFERENCE(ProtocolMetatypeToObject, getSubExpr);
  PASS_THROUGH_REFERENCE(InOutToPointer, getSubExpr);
  PASS_THROUGH_REFERENCE(ArrayToPointer, getSubExpr);
  PASS_THROUGH_REFERENCE(StringToPointer, getSubExpr);
  PASS_THROUGH_REFERENCE(PointerToPointer, getSubExpr);
  PASS_THROUGH_REFERENCE(ForeignObjectConversion, getSubExpr);
  PASS_THROUGH_REFERENCE(UnevaluatedInstance, getSubExpr);
  PASS_THROUGH_REFERENCE(DifferentiableFunction, getSubExpr);
  PASS_THROUGH_REFERENCE(LinearFunction, getSubExpr);
  PASS_THROUGH_REFERENCE(DifferentiableFunctionExtractOriginal, getSubExpr);
  PASS_THROUGH_REFERENCE(LinearFunctionExtractOriginal, getSubExpr);
  PASS_THROUGH_REFERENCE(LinearToDifferentiableFunction, getSubExpr);
  PASS_THROUGH_REFERENCE(BridgeToObjC, getSubExpr);
  PASS_THROUGH_REFERENCE(BridgeFromObjC, getSubExpr);
  PASS_THROUGH_REFERENCE(ConditionalBridgeFromObjC, getSubExpr);
  PASS_THROUGH_REFERENCE(UnderlyingToOpaque, getSubExpr);
  NO_REFERENCE(Coerce);
  NO_REFERENCE(ForcedCheckedCast);
  NO_REFERENCE(ConditionalCheckedCast);
  NO_REFERENCE(Is);

  NO_REFERENCE(Arrow);
  NO_REFERENCE(Ternary);
  NO_REFERENCE(EnumIsCase);
  NO_REFERENCE(Assign);
  NO_REFERENCE(CodeCompletion);
  NO_REFERENCE(UnresolvedPattern);
  NO_REFERENCE(EditorPlaceholder);
  NO_REFERENCE(ObjCSelector);
  NO_REFERENCE(KeyPath);
  NO_REFERENCE(KeyPathDot);
  PASS_THROUGH_REFERENCE(OneWay, getSubExpr);
  NO_REFERENCE(Tap);
  NO_REFERENCE(TypeJoin);
  SIMPLE_REFERENCE(MacroExpansion, getMacroRef);

#undef SIMPLE_REFERENCE
#undef NO_REFERENCE
#undef PASS_THROUGH_REFERENCE
  }

  return ConcreteDeclRef();
}

/// Enumerate each immediate child expression of this node, invoking the
/// specific functor on it.  This ignores statements and other non-expression
/// children.
void Expr::
forEachImmediateChildExpr(llvm::function_ref<Expr *(Expr *)> callback) {
  struct ChildWalker : ASTWalker {
    llvm::function_ref<Expr *(Expr *)> callback;
    Expr *ThisNode;

    /// Only walk the arguments of a macro, to represent the source as written.
    MacroWalking getMacroWalkingBehavior() const override {
      return MacroWalking::Arguments;
    }

    ChildWalker(llvm::function_ref<Expr *(Expr *)> callback, Expr *ThisNode)
      : callback(callback), ThisNode(ThisNode) {}
    
    PreWalkResult<Expr *> walkToExprPre(Expr *E) override {
      // When looking at the current node, of course we want to enter it.  We
      // also don't want to enumerate it.
      if (E == ThisNode)
        return Action::Continue(E);

      // Otherwise we must be a child of our expression, enumerate it!
      E = callback(E);
      if (!E)
        return Action::Stop();

      // We're only interested in the immediate children, so don't walk any
      // further.
      return Action::SkipChildren(E);
    }
    
    PreWalkResult<Stmt *> walkToStmtPre(Stmt *S) override {
      return Action::SkipChildren(S);
    }

    PreWalkResult<Pattern *> walkToPatternPre(Pattern *P) override {
      return Action::SkipChildren(P);
    }
    PreWalkAction walkToDeclPre(Decl *D) override {
      return Action::SkipChildren();
    }
    PreWalkAction walkToTypeReprPre(TypeRepr *T) override {
      return Action::SkipChildren();
    }
  };
  
  this->walk(ChildWalker(callback, this));
}

/// Enumerate each immediate child expression of this node, invoking the
/// specific functor on it.  This ignores statements and other non-expression
/// children.
void Expr::forEachChildExpr(llvm::function_ref<Expr *(Expr *)> callback) {
  struct ChildWalker : ASTWalker {
    llvm::function_ref<Expr *(Expr *)> callback;

    /// Only walk the arguments of a macro, to represent the source as written.
    MacroWalking getMacroWalkingBehavior() const override {
      return MacroWalking::Arguments;
    }

    ChildWalker(llvm::function_ref<Expr *(Expr *)> callback)
    : callback(callback) {}

    PreWalkResult<Expr *> walkToExprPre(Expr *E) override {
      // Enumerate the node!
      E = callback(E);
      if (!E)
        return Action::Stop();

      return Action::Continue(E);
    }

    PreWalkResult<Stmt *> walkToStmtPre(Stmt *S) override {
      return Action::SkipChildren(S);
    }

    PreWalkResult<Pattern *> walkToPatternPre(Pattern *P) override {
      return Action::SkipChildren(P);
    }
    PreWalkAction walkToDeclPre(Decl *D) override {
      return Action::SkipChildren();
    }
    PreWalkAction walkToTypeReprPre(TypeRepr *T) override {
      return Action::SkipChildren();
    }
  };

  this->walk(ChildWalker(callback));
}

bool Expr::isTypeReference(llvm::function_ref<Type(Expr *)> getType,
                           llvm::function_ref<Decl *(Expr *)> getDecl) const {
  Expr *expr = const_cast<Expr *>(this);

  // If the result isn't a metatype, there's nothing else to do.
  if (!getType(expr)->is<AnyMetatypeType>())
    return false;

  do {
    // Skip syntax.
    expr = expr->getSemanticsProvidingExpr();

    // Direct reference to a type.
    if (auto declRef = dyn_cast<DeclRefExpr>(expr))
      if (isa<TypeDecl>(declRef->getDecl()))
        return true;
    if (isa<TypeExpr>(expr))
      return true;

    // A "." expression that refers to a member.
    if (auto memberRef = dyn_cast<MemberRefExpr>(expr))
      return isa<TypeDecl>(memberRef->getMember().getDecl());

    // Any other expressions which might be referencing
    // a declaration e.g. not yet type-checked ones like
    // `UnresolvedDotExpr`.
    if (auto *decl = getDecl(expr))
      return isa<TypeDecl>(decl);

    // When the base of a "." expression is ignored, look at the member.
    if (auto ignoredDot = dyn_cast<DotSyntaxBaseIgnoredExpr>(expr)) {
      expr = ignoredDot->getRHS();
      continue;
    }

    if (auto *USE = dyn_cast<UnresolvedSpecializeExpr>(expr)) {
      expr = USE->getSubExpr();
      continue;
    }

    // Anything else is not statically derived.
    return false;
  } while (true);
}

bool Expr::isStaticallyDerivedMetatype(
    llvm::function_ref<Type(Expr *)> getType,
    llvm::function_ref<bool(Expr *)> isTypeReference) const {
  // The expression must first be a type reference.
  if (!isTypeReference(const_cast<Expr *>(this)))
    return false;

  auto type = getType(const_cast<Expr *>(this))
                  ->castTo<AnyMetatypeType>()
                  ->getInstanceType();

  // Archetypes are never statically derived.
  if (type->is<ArchetypeType>())
    return false;

  // Dynamic Self is never statically derived.
  if (type->is<DynamicSelfType>())
    return false;

  // Everything else is statically derived.
  return true;
}

bool Expr::isSuperExpr() const {
  const Expr *expr = this;
  do {
    expr = expr->getSemanticsProvidingExpr();

    if (isa<SuperRefExpr>(expr))
      return true;

    if (auto derivedToBase = dyn_cast<DerivedToBaseExpr>(expr)) {
      expr = derivedToBase->getSubExpr();
      continue;
    }
    if (auto metatypeConversion = dyn_cast<MetatypeConversionExpr>(expr)) {
      expr = metatypeConversion->getSubExpr();
      continue;
    }

    return false;
  } while (true);
}

bool Expr::canAppendPostfixExpression(bool appendingPostfixOperator) const {
  switch (getKind()) {
  case ExprKind::Error:
  case ExprKind::CodeCompletion:
  case ExprKind::LazyInitializer:
  case ExprKind::OneWay:
    return false;

  case ExprKind::NilLiteral:
  case ExprKind::IntegerLiteral:
  case ExprKind::FloatLiteral:
  case ExprKind::BooleanLiteral:
  case ExprKind::StringLiteral:
  case ExprKind::InterpolatedStringLiteral:
  case ExprKind::RegexLiteral:
  case ExprKind::MagicIdentifierLiteral:
  case ExprKind::ObjCSelector:
  case ExprKind::KeyPath:
  case ExprKind::SingleValueStmt:
    return true;

  case ExprKind::ObjectLiteral:
    return true;

  case ExprKind::DiscardAssignment:
    // Legal but pointless.
    return true;

  case ExprKind::DeclRef:
    return !cast<DeclRefExpr>(this)->getDecl()->isOperator();

  case ExprKind::SuperRef:
  case ExprKind::OtherConstructorDeclRef:
  case ExprKind::DotSyntaxBaseIgnored:
    return true;

  case ExprKind::Type:
    return cast<TypeExpr>(this)->getTypeRepr()->isSimple();

  case ExprKind::OverloadedDeclRef: {
    auto *overloadedExpr = cast<OverloadedDeclRefExpr>(this);
    if (overloadedExpr->getDecls().empty())
      return false;
    return !overloadedExpr->getDecls().front()->isOperator();
  }

  case ExprKind::UnresolvedDeclRef:
    return cast<UnresolvedDeclRefExpr>(this)->getName().isOperator();

  case ExprKind::MemberRef:
  case ExprKind::DynamicMemberRef:
  case ExprKind::DynamicSubscript:
  case ExprKind::UnresolvedSpecialize:
  case ExprKind::UnresolvedMember:
  case ExprKind::UnresolvedDot:
    return true;

  case ExprKind::Sequence:
    return false;

  case ExprKind::Paren:
  case ExprKind::DotSelf:
  case ExprKind::UnresolvedMemberChainResult:
  case ExprKind::Tuple:
  case ExprKind::Array:
  case ExprKind::Dictionary:
  case ExprKind::Subscript:
  case ExprKind::KeyPathApplication:
  case ExprKind::TupleElement:
    return true;

  case ExprKind::CaptureList:
  case ExprKind::Closure:
  case ExprKind::AutoClosure:
    return false;

  case ExprKind::DynamicType:
    return true;

  case ExprKind::Await:
  case ExprKind::Consume:
  case ExprKind::Copy:
  case ExprKind::Borrow:
  case ExprKind::Try:
  case ExprKind::ForceTry:
  case ExprKind::OptionalTry:
  case ExprKind::InOut:
    return false;

  case ExprKind::RebindSelfInConstructor:
  case ExprKind::OpaqueValue:
  case ExprKind::PropertyWrapperValuePlaceholder:
  case ExprKind::AppliedPropertyWrapper:
  case ExprKind::DefaultArgument:
  case ExprKind::BindOptional:
  case ExprKind::OptionalEvaluation:
    return false;

  case ExprKind::ForceValue:
    return true;

  case ExprKind::OpenExistential:
  case ExprKind::MakeTemporarilyEscapable:
  case ExprKind::VarargExpansion:
  case ExprKind::PackExpansion:
  case ExprKind::PackElement:
  case ExprKind::MaterializePack:
    return false;

  case ExprKind::Call:
  case ExprKind::DotSyntaxCall:
  case ExprKind::ConstructorRefCall:
    return true;

  case ExprKind::PostfixUnary:
    return !appendingPostfixOperator;

  case ExprKind::PrefixUnary:
  case ExprKind::Binary:
    return false;

  case ExprKind::Load:
  case ExprKind::ABISafeConversion:
  case ExprKind::DestructureTuple:
  case ExprKind::UnresolvedTypeConversion:
  case ExprKind::FunctionConversion:
  case ExprKind::CovariantFunctionConversion:
  case ExprKind::CovariantReturnConversion:
  case ExprKind::MetatypeConversion:
  case ExprKind::CollectionUpcastConversion:
  case ExprKind::Erasure:
  case ExprKind::AnyHashableErasure:
  case ExprKind::DerivedToBase:
  case ExprKind::ArchetypeToSuper:
  case ExprKind::InjectIntoOptional:
  case ExprKind::ClassMetatypeToObject:
  case ExprKind::ExistentialMetatypeToObject:
  case ExprKind::ProtocolMetatypeToObject:
  case ExprKind::InOutToPointer:
  case ExprKind::ArrayToPointer:
  case ExprKind::StringToPointer:
  case ExprKind::PointerToPointer:
  case ExprKind::ForeignObjectConversion:
  case ExprKind::UnevaluatedInstance:
  case ExprKind::DifferentiableFunction:
  case ExprKind::LinearFunction:
  case ExprKind::DifferentiableFunctionExtractOriginal:
  case ExprKind::LinearFunctionExtractOriginal:
  case ExprKind::LinearToDifferentiableFunction:
  case ExprKind::EnumIsCase:
  case ExprKind::ConditionalBridgeFromObjC:
  case ExprKind::BridgeFromObjC:
  case ExprKind::BridgeToObjC:
  case ExprKind::UnderlyingToOpaque:
    // Implicit conversion nodes have no syntax of their own; defer to the
    // subexpression.
    return cast<ImplicitConversionExpr>(this)->getSubExpr()
      ->canAppendPostfixExpression(appendingPostfixOperator);

  case ExprKind::ForcedCheckedCast:
  case ExprKind::ConditionalCheckedCast:
  case ExprKind::Is:
  case ExprKind::Coerce:
    return false;

  case ExprKind::Arrow:
  case ExprKind::Ternary:
  case ExprKind::Assign:
  case ExprKind::UnresolvedPattern:
  case ExprKind::EditorPlaceholder:
  case ExprKind::KeyPathDot:
  case ExprKind::TypeJoin:
    return false;

  case ExprKind::Tap:
    return true;

  case ExprKind::MacroExpansion:
    return true;
  }

  llvm_unreachable("Unhandled ExprKind in switch.");
}

ArgumentList *Expr::getArgs() const {
  if (auto *AE = dyn_cast<ApplyExpr>(this))
    return AE->getArgs();
  if (auto *SE = dyn_cast<SubscriptExpr>(this))
    return SE->getArgs();
  if (auto *DSE = dyn_cast<DynamicSubscriptExpr>(this))
    return DSE->getArgs();
  if (auto *OLE = dyn_cast<ObjectLiteralExpr>(this))
    return OLE->getArgs();
  if (auto *ME = dyn_cast<MacroExpansionExpr>(this))
    return ME->getArgs();
  return nullptr;
}

llvm::DenseMap<Expr *, Expr *> Expr::getParentMap() {
  class RecordingTraversal : public ASTWalker {
  public:
    llvm::DenseMap<Expr *, Expr *> &ParentMap;

    /// Walk everything that's available.
    MacroWalking getMacroWalkingBehavior() const override {
      return MacroWalking::ArgumentsAndExpansion;
    }

    explicit RecordingTraversal(llvm::DenseMap<Expr *, Expr *> &parentMap)
      : ParentMap(parentMap) { }

    PreWalkResult<Expr *> walkToExprPre(Expr *E) override {
      if (auto parent = Parent.getAsExpr())
        ParentMap[E] = parent;

      return Action::Continue(E);
    }
  };

  llvm::DenseMap<Expr *, Expr *> parentMap;
  RecordingTraversal traversal(parentMap);
  walk(traversal);
  return parentMap;
}

bool Expr::isValidParentOfTypeExpr(Expr *typeExpr) const {
  // Allow references to types as a part of:
  // - member references T.foo, T.Type, T.self, etc.
  // - constructor calls T()
  // - Subscripts T[]
  //
  // This is an exhaustive list of the accepted syntactic forms.
  switch (getKind()) {
  case ExprKind::Error:
  case ExprKind::DotSelf:
  case ExprKind::MemberRef:
  case ExprKind::UnresolvedMember:
  case ExprKind::DotSyntaxCall:
  case ExprKind::ConstructorRefCall:
  case ExprKind::UnresolvedDot:
  case ExprKind::DotSyntaxBaseIgnored:
  case ExprKind::UnresolvedSpecialize:
  case ExprKind::OpenExistential:
    return true;

  // For these cases we need to ensure the type expr is the function or base.
  // We do not permit e.g 'foo(T)'.
  case ExprKind::Call:
    return cast<CallExpr>(this)->getFn() == typeExpr;
  case ExprKind::Subscript:
    return cast<SubscriptExpr>(this)->getBase() == typeExpr;

  case ExprKind::NilLiteral:
  case ExprKind::BooleanLiteral:
  case ExprKind::IntegerLiteral:
  case ExprKind::FloatLiteral:
  case ExprKind::StringLiteral:
  case ExprKind::MagicIdentifierLiteral:
  case ExprKind::InterpolatedStringLiteral:
  case ExprKind::RegexLiteral:
  case ExprKind::ObjectLiteral:
  case ExprKind::DiscardAssignment:
  case ExprKind::DeclRef:
  case ExprKind::SuperRef:
  case ExprKind::Type:
  case ExprKind::OtherConstructorDeclRef:
  case ExprKind::OverloadedDeclRef:
  case ExprKind::UnresolvedDeclRef:
  case ExprKind::DynamicMemberRef:
  case ExprKind::DynamicSubscript:
  case ExprKind::Sequence:
  case ExprKind::Paren:
  case ExprKind::Await:
  case ExprKind::Consume:
  case ExprKind::Copy:
  case ExprKind::Borrow:
  case ExprKind::UnresolvedMemberChainResult:
  case ExprKind::Try:
  case ExprKind::ForceTry:
  case ExprKind::OptionalTry:
  case ExprKind::Tuple:
  case ExprKind::Array:
  case ExprKind::Dictionary:
  case ExprKind::KeyPathApplication:
  case ExprKind::TupleElement:
  case ExprKind::CaptureList:
  case ExprKind::Closure:
  case ExprKind::AutoClosure:
  case ExprKind::InOut:
  case ExprKind::VarargExpansion:
  case ExprKind::PackExpansion:
  case ExprKind::PackElement:
  case ExprKind::MaterializePack:
  case ExprKind::DynamicType:
  case ExprKind::RebindSelfInConstructor:
  case ExprKind::OpaqueValue:
  case ExprKind::PropertyWrapperValuePlaceholder:
  case ExprKind::AppliedPropertyWrapper:
  case ExprKind::DefaultArgument:
  case ExprKind::BindOptional:
  case ExprKind::OptionalEvaluation:
  case ExprKind::ForceValue:
  case ExprKind::MakeTemporarilyEscapable:
  case ExprKind::PrefixUnary:
  case ExprKind::PostfixUnary:
  case ExprKind::Binary:
  case ExprKind::Load:
  case ExprKind::DestructureTuple:
  case ExprKind::UnresolvedTypeConversion:
  case ExprKind::ABISafeConversion:
  case ExprKind::FunctionConversion:
  case ExprKind::CovariantFunctionConversion:
  case ExprKind::CovariantReturnConversion:
  case ExprKind::MetatypeConversion:
  case ExprKind::CollectionUpcastConversion:
  case ExprKind::Erasure:
  case ExprKind::AnyHashableErasure:
  case ExprKind::BridgeToObjC:
  case ExprKind::BridgeFromObjC:
  case ExprKind::ConditionalBridgeFromObjC:
  case ExprKind::DerivedToBase:
  case ExprKind::ArchetypeToSuper:
  case ExprKind::InjectIntoOptional:
  case ExprKind::ClassMetatypeToObject:
  case ExprKind::ExistentialMetatypeToObject:
  case ExprKind::ProtocolMetatypeToObject:
  case ExprKind::InOutToPointer:
  case ExprKind::ArrayToPointer:
  case ExprKind::StringToPointer:
  case ExprKind::PointerToPointer:
  case ExprKind::ForeignObjectConversion:
  case ExprKind::UnevaluatedInstance:
  case ExprKind::UnderlyingToOpaque:
  case ExprKind::DifferentiableFunction:
  case ExprKind::LinearFunction:
  case ExprKind::DifferentiableFunctionExtractOriginal:
  case ExprKind::LinearFunctionExtractOriginal:
  case ExprKind::LinearToDifferentiableFunction:
  case ExprKind::ForcedCheckedCast:
  case ExprKind::ConditionalCheckedCast:
  case ExprKind::Is:
  case ExprKind::Coerce:
  case ExprKind::Arrow:
  case ExprKind::Ternary:
  case ExprKind::EnumIsCase:
  case ExprKind::Assign:
  case ExprKind::CodeCompletion:
  case ExprKind::UnresolvedPattern:
  case ExprKind::LazyInitializer:
  case ExprKind::EditorPlaceholder:
  case ExprKind::ObjCSelector:
  case ExprKind::KeyPath:
  case ExprKind::KeyPathDot:
  case ExprKind::OneWay:
  case ExprKind::Tap:
  case ExprKind::SingleValueStmt:
  case ExprKind::TypeJoin:
  case ExprKind::MacroExpansion:
    return false;
  }

  llvm_unreachable("Unhandled ExprKind in switch.");
}

//===----------------------------------------------------------------------===//
// Support methods for Exprs.
//===----------------------------------------------------------------------===//

StringRef LiteralExpr::getLiteralKindDescription() const {
  switch (getKind()) {
  case ExprKind::IntegerLiteral:
    return "integer";
  case ExprKind::FloatLiteral:
    return "floating-point";
  case ExprKind::BooleanLiteral:
    return "boolean";
  case ExprKind::StringLiteral:
    return "string";
  case ExprKind::InterpolatedStringLiteral:
    return "string";
  case ExprKind::RegexLiteral:
    return "regular expression";
  case ExprKind::MagicIdentifierLiteral:
    return MagicIdentifierLiteralExpr::getKindString(
        cast<MagicIdentifierLiteralExpr>(this)->getKind());
  case ExprKind::NilLiteral:
    return "nil";
  case ExprKind::ObjectLiteral:
    return "object";
  // Define an unreachable case for all non-literal expressions.
  // This way, if a new literal is added in the future, the compiler
  // will warn that a case is missing from this switch.
  #define LITERAL_EXPR(Id, Parent)
  #define EXPR(Id, Parent) case ExprKind::Id:
  #include "swift/AST/ExprNodes.def"
    llvm_unreachable("Not a literal expression");
  }
  llvm_unreachable("Unhandled literal");
}

IntegerLiteralExpr * IntegerLiteralExpr::createFromUnsigned(ASTContext &C, unsigned value, SourceLoc loc) {
  llvm::SmallString<8> Scratch;
  llvm::APInt(sizeof(unsigned)*8, value).toString(Scratch, 10, /*signed*/ false);
  auto Text = C.AllocateCopy(StringRef(Scratch));
  return new (C) IntegerLiteralExpr(Text, loc, /*implicit*/ true);
}

APInt IntegerLiteralExpr::getRawValue() const {
  return BuiltinIntegerWidth::arbitrary().parse(getDigitsText(), /*radix*/0,
                                                isNegative());
}

APInt IntegerLiteralExpr::getValue() const {
  assert(!getType().isNull() && "Semantic analysis has not completed");
  assert(!getType()->hasError() && "Should have a valid type");
  if (!getType()->is<AnyBuiltinIntegerType>())
    return getRawValue();
  auto width = getType()->castTo<AnyBuiltinIntegerType>()->getWidth();
  return width.parse(getDigitsText(), /*radix*/ 0, isNegative());
}

APInt BuiltinIntegerWidth::parse(StringRef text, unsigned radix, bool negate,
                                 bool *hadError) const {
  if (hadError) *hadError = false;

  // Parse an unsigned value from the string.
  APInt value;

  // Swift doesn't treat a leading zero as signifying octal, but
  // StringRef::getAsInteger does.  Force decimal parsing in this case.
  if (radix == 0 && text.size() >= 2 && text[0] == '0' && isdigit(text[1]))
    radix = 10;

  bool error = text.getAsInteger(radix, value);
  if (error) {
    if (hadError) *hadError = true;
    return value;
  }

  // If we're producing an arbitrary-precision value, we don't need to do
  // much additional processing.
  if (isArbitraryWidth()) {
    // The parser above always produces a non-negative value, so if the sign
    // bit is set we need to give it some breathing room.
    if (value.isNegative())
      value = value.zext(value.getBitWidth() + 1);
    assert(!value.isNegative());

    // Now we can safely negate.
    if (negate) {
      value = -value;
      assert(value.isNegative() || value.isZero());
    }

    // Truncate down to the minimum number of bits required to express
    // this value exactly.
    auto requiredBits = value.getSignificantBits();
    if (value.getBitWidth() > requiredBits)
      value = value.trunc(requiredBits);

  // If we have a fixed-width type (including abstract ones), we need to do
  // fixed-width transformations, which can overflow.
  } else {
    unsigned width = getGreatestWidth();

    // The overflow diagnostics in this case can't be fully correct because
    // we don't know whether we're supposed to be producing a signed number
    // or an unsigned one.

    if (hadError && value.getActiveBits() > width)
      *hadError = true;
    value = value.zextOrTrunc(width);

    if (negate) {
      value = -value;

      if (hadError && !value.isNegative())
        *hadError = true;
    }

    assert(value.getBitWidth() == width);
  }

  return value;
}

static APFloat getFloatLiteralValue(bool IsNegative, StringRef Text,
                                    const llvm::fltSemantics &Semantics) {
  APFloat Val(Semantics);
  auto Res =
    Val.convertFromString(Text, llvm::APFloat::rmNearestTiesToEven);
  assert(Res && "Sema didn't reject invalid number");
  consumeError(Res.takeError());
  if (IsNegative) {
    auto NegVal = APFloat::getZero(Semantics, /*negative*/ true);
    Res = NegVal.subtract(Val, llvm::APFloat::rmNearestTiesToEven);
    assert(Res && "Sema didn't reject invalid number");
    consumeError(Res.takeError());
    return NegVal;
  }
  return Val;
}

APFloat FloatLiteralExpr::getValue(StringRef Text,
                                   const llvm::fltSemantics &Semantics,
                                   bool Negative) {
  return getFloatLiteralValue(Negative, Text, Semantics);
}

llvm::APFloat FloatLiteralExpr::getValue() const {
  assert(!getType().isNull() && "Semantic analysis has not completed");
  assert(!getType()->hasError() && "Should have a valid type");

  Type ty = getType();
  if (!ty->is<BuiltinFloatType>()) {
    assert(!getBuiltinType().isNull() && "Semantic analysis has not completed");
    assert(!getBuiltinType()->hasError() && "Should have a valid type");
    ty = getBuiltinType();
  }

  return getFloatLiteralValue(
      isNegative(), getDigitsText(),
      ty->castTo<BuiltinFloatType>()->getAPFloatSemantics());
}

StringLiteralExpr::StringLiteralExpr(StringRef Val, SourceRange Range,
                                     bool Implicit)
    : BuiltinLiteralExpr(ExprKind::StringLiteral, Implicit), Val(Val),
      Range(Range) {
  Bits.StringLiteralExpr.Encoding = static_cast<unsigned>(UTF8);
  Bits.StringLiteralExpr.IsSingleUnicodeScalar =
      unicode::isSingleUnicodeScalar(Val);
  Bits.StringLiteralExpr.IsSingleExtendedGraphemeCluster =
      unicode::isSingleExtendedGraphemeCluster(Val);
}

ObjectLiteralExpr::ObjectLiteralExpr(SourceLoc poundLoc, LiteralKind litKind,
                                     ArgumentList *argList, bool implicit)
    : LiteralExpr(ExprKind::ObjectLiteral, implicit), ArgList(argList),
      PoundLoc(poundLoc) {
  assert(argList);
  Bits.ObjectLiteralExpr.LitKind = static_cast<unsigned>(litKind);
  assert(getLiteralKind() == litKind);
}

ObjectLiteralExpr *
ObjectLiteralExpr::create(ASTContext &ctx, SourceLoc poundLoc, LiteralKind kind,
                          ArgumentList *args, bool implicit) {
  return new (ctx) ObjectLiteralExpr(poundLoc, kind, args, implicit);
}

StringRef ObjectLiteralExpr::getLiteralKindRawName() const {
  switch (getLiteralKind()) {
#define POUND_OBJECT_LITERAL(Name, Desc, Proto) case Name: return #Name;
#include "swift/AST/TokenKinds.def"    
  }
  llvm_unreachable("unspecified literal");
}

StringRef ObjectLiteralExpr::getLiteralKindPlainName() const {
  switch (getLiteralKind()) {
#define POUND_OBJECT_LITERAL(Name, Desc, Proto) case Name: return Desc;
#include "swift/AST/TokenKinds.def"    
  }
  llvm_unreachable("unspecified literal");
}

ConstructorDecl *OtherConstructorDeclRefExpr::getDecl() const {
  return cast_or_null<ConstructorDecl>(Ctor.getDecl());
}

MemberRefExpr::MemberRefExpr(Expr *base, SourceLoc dotLoc,
                             ConcreteDeclRef member, DeclNameLoc nameLoc,
                             bool Implicit, AccessSemantics semantics)
  : LookupExpr(ExprKind::MemberRef, base, member, Implicit),
    DotLoc(dotLoc), NameLoc(nameLoc) {
   
  Bits.MemberRefExpr.Semantics = (unsigned) semantics;
}

Type OverloadSetRefExpr::getBaseType() const {
  if (isa<OverloadedDeclRefExpr>(this))
    return Type();
  
  llvm_unreachable("Unhandled overloaded set reference expression");
}

bool OverloadSetRefExpr::hasBaseObject() const {
  if (Type BaseTy = getBaseType())
    return !BaseTy->is<AnyMetatypeType>();

  return false;
}

InOutExpr::InOutExpr(SourceLoc operLoc, Expr *subExpr, Type baseType,
                     bool isImplicit)
  : Expr(ExprKind::InOut, isImplicit,
         baseType.isNull() ? baseType : InOutType::get(baseType)),
    SubExpr(subExpr), OperLoc(operLoc) {}

VarargExpansionExpr *VarargExpansionExpr::createParamExpansion(ASTContext &ctx, Expr *E) {
  assert(E->getType() && "Expansion must have fully-resolved type!");
  return new (ctx) VarargExpansionExpr(E, /*implicit*/ true, E->getType());
}

VarargExpansionExpr *VarargExpansionExpr::createArrayExpansion(ASTContext &ctx, ArrayExpr *AE) {
  assert(AE->getType() && "Expansion must have fully-resolved type!");
  return new (ctx) VarargExpansionExpr(AE, /*implicit*/ true, AE->getType());
}

PackExpansionExpr *
PackExpansionExpr::create(ASTContext &ctx, SourceLoc repeatLoc,
                          Expr *patternExpr, GenericEnvironment *environment,
                          bool implicit, Type type) {
  return new (ctx) PackExpansionExpr(repeatLoc, patternExpr, environment,
                                     implicit, type);
}

PackElementExpr *
PackElementExpr::create(ASTContext &ctx, SourceLoc eachLoc, Expr *packRefExpr,
                        bool implicit, Type type) {
  return new (ctx) PackElementExpr(eachLoc, packRefExpr, implicit, type);
}

MaterializePackExpr *
MaterializePackExpr::create(ASTContext &ctx, Expr *fromExpr,
                            SourceLoc elementLoc,
                            Type type, bool implicit) {
  return new (ctx) MaterializePackExpr(fromExpr, elementLoc,
                                       type, implicit);
}

SequenceExpr *SequenceExpr::create(ASTContext &ctx, ArrayRef<Expr*> elements) {
  assert(elements.size() & 1 && "even number of elements in sequence");
  size_t bytes = totalSizeToAlloc<Expr *>(elements.size());
  void *Buffer = ctx.Allocate(bytes, alignof(SequenceExpr));
  return ::new (Buffer) SequenceExpr(elements);
}

ErasureExpr *ErasureExpr::create(ASTContext &ctx, Expr *subExpr, Type type,
                                 ArrayRef<ProtocolConformanceRef> conformances,
                                 ArrayRef<ConversionPair> argConversions) {
  auto size = totalSizeToAlloc<ProtocolConformanceRef, ConversionPair>(conformances.size(),
                                                                       argConversions.size());
  auto mem = ctx.Allocate(size, alignof(ErasureExpr));
  return ::new (mem) ErasureExpr(subExpr, type, conformances, argConversions);
}

UnresolvedSpecializeExpr *UnresolvedSpecializeExpr::create(ASTContext &ctx,
                                             Expr *SubExpr, SourceLoc LAngleLoc,
                                             ArrayRef<TypeRepr *> UnresolvedParams,
                                             SourceLoc RAngleLoc) {
  auto size = totalSizeToAlloc<TypeRepr *>(UnresolvedParams.size());
  auto mem = ctx.Allocate(size, alignof(UnresolvedSpecializeExpr));
  return ::new (mem) UnresolvedSpecializeExpr(SubExpr, LAngleLoc,
                                              UnresolvedParams, RAngleLoc);
}

CaptureListEntry::CaptureListEntry(PatternBindingDecl *PBD) : PBD(PBD) {
  assert(PBD);
  assert(PBD->getSingleVar() &&
         "Capture lists only support single-var patterns");
}

VarDecl *CaptureListEntry::getVar() const {
  return PBD->getSingleVar();
}

bool CaptureListEntry::isSimpleSelfCapture() const {
  auto *Var = getVar();
  auto &ctx = Var->getASTContext();

  if (Var->getName() != ctx.Id_self)
    return false;

  if (auto *attr = Var->getAttrs().getAttribute<ReferenceOwnershipAttr>())
    if (attr->get() == ReferenceOwnership::Weak)
      return false;

  if (PBD->getPatternList().size() != 1)
    return false;

  auto *expr = PBD->getInit(0);

  if (auto *DRE = dyn_cast<DeclRefExpr>(expr)) {
    if (auto *VD = dyn_cast<VarDecl>(DRE->getDecl())) {
      return VD->getName() == ctx.Id_self;
    }
  }

  if (auto *UDRE = dyn_cast<UnresolvedDeclRefExpr>(expr)) {
    return UDRE->getName().isSimpleName(ctx.Id_self);
  }

  return false;
}

CaptureListExpr *CaptureListExpr::create(ASTContext &ctx,
                                         ArrayRef<CaptureListEntry> captureList,
                                         AbstractClosureExpr *closureBody) {
  auto size = totalSizeToAlloc<CaptureListEntry>(captureList.size());
  auto mem = ctx.Allocate(size, alignof(CaptureListExpr));
  auto *expr = ::new(mem) CaptureListExpr(captureList, closureBody);

  for (auto capture : captureList)
    capture.getVar()->setParentCaptureList(expr);

  return expr;
}

DestructureTupleExpr *
DestructureTupleExpr::create(ASTContext &ctx,
                             ArrayRef<OpaqueValueExpr *> destructuredElements,
                             Expr *srcExpr, Expr *dstExpr, Type ty) {
  auto size = totalSizeToAlloc<OpaqueValueExpr *>(destructuredElements.size());
  auto mem = ctx.Allocate(size, alignof(DestructureTupleExpr));
  return ::new(mem) DestructureTupleExpr(destructuredElements,
                                         srcExpr, dstExpr, ty);
}

SourceRange TupleExpr::getSourceRange() const {
  auto start = LParenLoc;
  if (start.isInvalid()) {
    // Scan forward for the first valid source loc.
    for (auto *expr : getElements()) {
      start = expr->getStartLoc();
      if (start.isValid())
        break;
    }
  }

  auto end = RParenLoc;
  if (end.isInvalid()) {
    // Scan backwards for a valid source loc.
    for (auto *expr : llvm::reverse(getElements())) {
      end = expr->getEndLoc();
      if (end.isValid())
        break;
    }
  }

  if (start.isInvalid() || end.isInvalid())
    return SourceRange();

  return SourceRange(start, end);
}

TupleExpr::TupleExpr(SourceLoc LParenLoc, SourceLoc RParenLoc,
                     ArrayRef<Expr *> SubExprs,
                     ArrayRef<Identifier> ElementNames, 
                     ArrayRef<SourceLoc> ElementNameLocs,
                     bool Implicit, Type Ty)
  : Expr(ExprKind::Tuple, Implicit, Ty),
    LParenLoc(LParenLoc), RParenLoc(RParenLoc) {
  Bits.TupleExpr.HasElementNames = !ElementNames.empty();
  Bits.TupleExpr.HasElementNameLocations = !ElementNameLocs.empty();
  Bits.TupleExpr.NumElements = SubExprs.size();
  
  assert(LParenLoc.isValid() == RParenLoc.isValid() &&
         "Mismatched parenthesis location information validity");
  assert(ElementNames.empty() || ElementNames.size() == SubExprs.size());
  assert(ElementNameLocs.empty() || 
         ElementNames.size() == ElementNameLocs.size());

  // Copy elements.
  std::uninitialized_copy(SubExprs.begin(), SubExprs.end(),
                          getTrailingObjects<Expr *>());

  // Copy element names, if provided.
  if (hasElementNames()) {
    std::uninitialized_copy(ElementNames.begin(), ElementNames.end(),
                            getTrailingObjects<Identifier>());
  }

  // Copy element name locations, if provided.
  if (hasElementNameLocs()) {
    std::uninitialized_copy(ElementNameLocs.begin(), ElementNameLocs.end(),
                            getTrailingObjects<SourceLoc>());
  }
}

TupleExpr *TupleExpr::create(ASTContext &ctx,
                             SourceLoc LParenLoc,
                             ArrayRef<Expr *> SubExprs,
                             ArrayRef<Identifier> ElementNames,
                             ArrayRef<SourceLoc> ElementNameLocs,
                             SourceLoc RParenLoc, bool Implicit, Type Ty) {
  assert(!Ty || isa<TupleType>(Ty.getPointer()));
  auto hasNonEmptyIdentifier = [](ArrayRef<Identifier> Ids) -> bool {
    for (auto ident : Ids) {
      if (!ident.empty())
        return true;
    }
    return false;
  };
  assert((Implicit || ElementNames.size() == ElementNameLocs.size() ||
          (!hasNonEmptyIdentifier(ElementNames) && ElementNameLocs.empty())) &&
         "trying to create non-implicit tuple-expr without name locations");
  (void)hasNonEmptyIdentifier;

  size_t size =
      totalSizeToAlloc<Expr *, Identifier, SourceLoc>(SubExprs.size(),
                                                      ElementNames.size(),
                                                      ElementNameLocs.size());
  void *mem = ctx.Allocate(size, alignof(TupleExpr));
  return new (mem) TupleExpr(LParenLoc, RParenLoc, SubExprs, ElementNames,
                             ElementNameLocs, Implicit, Ty);
}

TupleExpr *TupleExpr::createEmpty(ASTContext &ctx, SourceLoc LParenLoc, 
                                  SourceLoc RParenLoc, bool Implicit) {
  return create(ctx, LParenLoc, {}, {}, {}, RParenLoc, Implicit,
                TupleType::getEmpty(ctx));
}

TupleExpr *TupleExpr::createImplicit(ASTContext &ctx, ArrayRef<Expr *> SubExprs,
                                     ArrayRef<Identifier> ElementNames) {
  return create(ctx, SourceLoc(), SubExprs, ElementNames, {}, SourceLoc(),
                /*Implicit=*/true, Type());
}

ArrayExpr *ArrayExpr::create(ASTContext &C, SourceLoc LBracketLoc,
                             ArrayRef<Expr*> Elements,
                             ArrayRef<SourceLoc> CommaLocs,
                             SourceLoc RBracketLoc, Type Ty) {
  auto Size = totalSizeToAlloc<Expr *, SourceLoc>(Elements.size(),
                                                  CommaLocs.size());
  auto Mem = C.Allocate(Size, alignof(ArrayExpr));
  return new (Mem) ArrayExpr(LBracketLoc, Elements, CommaLocs, RBracketLoc, Ty);
}

Type ArrayExpr::getElementType() {
  auto init = getInitializer();
  if (!init)
    return Type();

  auto *decl = cast<ConstructorDecl>(init.getDecl());
  return decl->getMethodInterfaceType()
      ->getAs<AnyFunctionType>()
      ->getParams()[0]
      .getPlainType()
      .subst(init.getSubstitutions());
}

Type DictionaryExpr::getElementType() {
  auto init = getInitializer();
  if (!init)
    return Type();

  auto *decl = cast<ConstructorDecl>(init.getDecl());
  return decl->getMethodInterfaceType()
      ->getAs<AnyFunctionType>()
      ->getParams()[0]
      .getPlainType()
      .subst(init.getSubstitutions());
}

DictionaryExpr *DictionaryExpr::create(ASTContext &C, SourceLoc LBracketLoc,
                             ArrayRef<Expr*> Elements,
                             ArrayRef<SourceLoc> CommaLocs,
                             SourceLoc RBracketLoc,
                             Type Ty) {
  auto Size = totalSizeToAlloc<Expr *, SourceLoc>(Elements.size(),
                                                  CommaLocs.size());
  auto Mem = C.Allocate(Size, alignof(DictionaryExpr));
  return new (Mem) DictionaryExpr(LBracketLoc, Elements, CommaLocs, RBracketLoc,
                                  Ty);
}

static ValueDecl *getCalledValue(Expr *E, bool skipFunctionConversions) {
  if (auto *DRE = dyn_cast<DeclRefExpr>(E))
    return DRE->getDecl();

  if (auto *OCRE = dyn_cast<OtherConstructorDeclRefExpr>(E))
    return OCRE->getDecl();

  // Look through SelfApplyExpr.
  if (auto *SAE = dyn_cast<SelfApplyExpr>(E))
    return SAE->getCalledValue(skipFunctionConversions);

  if (skipFunctionConversions) {
    if (auto fnConv = dyn_cast<FunctionConversionExpr>(E))
      return getCalledValue(fnConv->getSubExpr(), skipFunctionConversions);
  }

  Expr *E2 = E->getValueProvidingExpr();
  if (E != E2)
    return getCalledValue(E2, skipFunctionConversions);

  return nullptr;
}

OpaqueValueExpr *OpaqueValueExpr::createImplicit(ASTContext &ctx, Type Ty,
                                                 bool isPlaceholder,
                                                 AllocationArena arena) {
  auto *mem =
      ctx.Allocate(sizeof(OpaqueValueExpr), alignof(OpaqueValueExpr), arena);
  return new (mem) OpaqueValueExpr(
      /*Range=*/SourceRange(), Ty, isPlaceholder);
}

PropertyWrapperValuePlaceholderExpr *
PropertyWrapperValuePlaceholderExpr::create(ASTContext &ctx, SourceRange range,
                                            Type ty, Expr *wrappedValue,
                                            bool isAutoClosure) {
  OpaqueValueExpr *placeholder = nullptr;
  if (ty)
    placeholder = new (ctx) OpaqueValueExpr(range, ty, /*isPlaceholder=*/true);

  return new (ctx) PropertyWrapperValuePlaceholderExpr(
      range, ty, placeholder, wrappedValue, isAutoClosure);
}

AppliedPropertyWrapperExpr *
AppliedPropertyWrapperExpr::create(ASTContext &ctx, ConcreteDeclRef callee,
                                   const ParamDecl *param,
                                   SourceLoc loc, Type Ty, Expr *value,
                                   AppliedPropertyWrapperExpr::ValueKind kind) {
  return new (ctx) AppliedPropertyWrapperExpr(callee, param, loc, Ty, value, kind);
}

const ParamDecl *DefaultArgumentExpr::getParamDecl() const {
  return getParameterAt(DefaultArgsOwner.getDecl(), ParamIndex);
}

bool DefaultArgumentExpr::isCallerSide() const {
  return getParamDecl()->hasCallerSideDefaultExpr();
}

Expr *DefaultArgumentExpr::getCallerSideDefaultExpr() const {
  assert(isCallerSide());
  auto &ctx = DefaultArgsOwner.getDecl()->getASTContext();
  auto *mutableThis = const_cast<DefaultArgumentExpr *>(this);
  return evaluateOrDefault(ctx.evaluator,
                           CallerSideDefaultArgExprRequest{mutableThis},
                           new (ctx) ErrorExpr(getSourceRange(), getType()));
}

ValueDecl *ApplyExpr::getCalledValue(bool skipFunctionConversions) const {
  return ::getCalledValue(Fn, skipFunctionConversions);
}

SubscriptExpr::SubscriptExpr(Expr *base, ArgumentList *argList,
                             ConcreteDeclRef decl, bool implicit,
                             AccessSemantics semantics)
    : LookupExpr(ExprKind::Subscript, base, decl, implicit), ArgList(argList) {
  Bits.SubscriptExpr.Semantics = (unsigned) semantics;
}

SubscriptExpr *SubscriptExpr::create(ASTContext &ctx, Expr *base,
                                     ArgumentList *argList,
                                     ConcreteDeclRef decl,
                                     bool implicit,
                                     AccessSemantics semantics) {
  return new (ctx) SubscriptExpr(base, argList, decl, implicit, semantics);
}

DynamicSubscriptExpr::DynamicSubscriptExpr(Expr *base, ArgumentList *argList,
                                           ConcreteDeclRef member,
                                           bool implicit)
    : DynamicLookupExpr(ExprKind::DynamicSubscript, member, base),
      ArgList(argList) {
  if (implicit) setImplicit(implicit);
}

DynamicSubscriptExpr *DynamicSubscriptExpr::create(ASTContext &ctx, Expr *base,
                                                   ArgumentList *argList,
                                                   ConcreteDeclRef member,
                                                   bool implicit) {
  return new (ctx) DynamicSubscriptExpr(base, argList, member, implicit);
}

CallExpr::CallExpr(Expr *fn, ArgumentList *argList, bool implicit, Type ty)
    : ApplyExpr(ExprKind::Call, fn, argList, implicit, ty) {
#ifndef NDEBUG
  Expr *calleeFn = fn->getSemanticsProvidingExpr();
  if (auto *calleeDRE = dyn_cast<DeclRefExpr>(calleeFn))
    assert(!calleeDRE->getDecl()->isInstanceMember());
#endif
}

CallExpr *CallExpr::create(ASTContext &ctx, Expr *fn, ArgumentList *argList,
                           bool implicit) {
  return new (ctx) CallExpr(fn, argList, implicit, Type());
}

CallExpr *CallExpr::createImplicitEmpty(ASTContext &ctx, Expr *fn) {
  return createImplicit(ctx, fn, ArgumentList::createImplicit(ctx, {}));
}

Expr *CallExpr::getDirectCallee() const {
  auto fn = getFn();
  while (true) {
    fn = fn->getSemanticsProvidingExpr();

    if (auto force = dyn_cast<ForceValueExpr>(fn)) {
      fn = force->getSubExpr();
      continue;
    }

    if (auto bind = dyn_cast<BindOptionalExpr>(fn)) {
      fn = bind->getSubExpr();
      continue;
    }

    if (auto ctorCall = dyn_cast<ConstructorRefCallExpr>(fn)) {
      fn = ctorCall->getFn();
      continue;
    }

    return fn;
  }
}

PrefixUnaryExpr *PrefixUnaryExpr::create(ASTContext &ctx, Expr *fn,
                                         Expr *operand, Type ty) {
  auto *argList = ArgumentList::forImplicitUnlabeled(ctx, {operand});
  return new (ctx) PrefixUnaryExpr(fn, argList, ty);
}

PostfixUnaryExpr *PostfixUnaryExpr::create(ASTContext &ctx, Expr *fn,
                                           Expr *operand, Type ty) {
  auto *argList = ArgumentList::forImplicitUnlabeled(ctx, {operand});
  return new (ctx) PostfixUnaryExpr(fn, argList, ty);
}

BinaryExpr *BinaryExpr::create(ASTContext &ctx, Expr *lhs, Expr *fn, Expr *rhs,
                               bool implicit, Type ty) {
  auto *argList = ArgumentList::forImplicitUnlabeled(ctx, {lhs, rhs});
  return new (ctx) BinaryExpr(fn, argList, implicit, ty);
}

DotSyntaxCallExpr *DotSyntaxCallExpr::create(ASTContext &ctx, Expr *fnExpr,
                                             SourceLoc dotLoc, Argument baseArg,
                                             Type ty) {
  assert(!baseArg.hasLabel());
  auto *argList = ArgumentList::forImplicitUnlabeled(ctx, {baseArg.getExpr()});
  return new (ctx) DotSyntaxCallExpr(fnExpr, dotLoc, argList, ty);
}

SourceLoc DotSyntaxCallExpr::getLoc() const {
  if (isImplicit()) {
    SourceLoc baseLoc = getBase()->getLoc();
    return baseLoc.isValid() ? baseLoc : getFn()->getLoc();
  }

  return getFn()->getLoc();
}

SourceLoc DotSyntaxCallExpr::getStartLoc() const {
  if (isImplicit()) {
    SourceLoc baseLoc = getBase()->getStartLoc();
    return baseLoc.isValid() ? baseLoc : getFn()->getStartLoc();
  }

  return getBase()->getStartLoc();
}

SourceLoc DotSyntaxCallExpr::getEndLoc() const {
  if (isImplicit()) {
    SourceLoc fnLoc = getFn()->getEndLoc();
    return fnLoc.isValid() ? fnLoc : getBase()->getEndLoc();
  }

  return getFn()->getEndLoc();
}

ConstructorRefCallExpr *ConstructorRefCallExpr::create(ASTContext &ctx,
                                                       Expr *fnExpr,
                                                       Expr *baseExpr,
                                                       Type ty) {
  auto *argList = ArgumentList::forImplicitUnlabeled(ctx, {baseExpr});
  return new (ctx) ConstructorRefCallExpr(fnExpr, argList, ty);
}

void ExplicitCastExpr::setCastType(Type type) {
  CastTy->setType(MetatypeType::get(type));
}

ForcedCheckedCastExpr *ForcedCheckedCastExpr::create(ASTContext &ctx,
                                                     SourceLoc asLoc,
                                                     SourceLoc exclaimLoc,
                                                     TypeRepr *tyRepr) {
  return new (ctx) ForcedCheckedCastExpr(nullptr, asLoc, exclaimLoc,
                                         new (ctx) TypeExpr(tyRepr));
}

ForcedCheckedCastExpr *
ForcedCheckedCastExpr::createImplicit(ASTContext &ctx, Expr *sub, Type castTy) {
  auto *const expr = new (ctx) ForcedCheckedCastExpr(
      sub, SourceLoc(), SourceLoc(), TypeExpr::createImplicit(castTy, ctx));
  expr->setType(castTy);
  expr->setImplicit();
  return expr;
}

ConditionalCheckedCastExpr *
ConditionalCheckedCastExpr::create(ASTContext &ctx, SourceLoc asLoc,
                                   SourceLoc questionLoc, TypeRepr *tyRepr) {
  return new (ctx) ConditionalCheckedCastExpr(nullptr, asLoc, questionLoc,
                                              new (ctx) TypeExpr(tyRepr));
}

ConditionalCheckedCastExpr *
ConditionalCheckedCastExpr::createImplicit(ASTContext &ctx, Expr *sub,
                                           Type castTy) {
  auto *const expr = new (ctx) ConditionalCheckedCastExpr(
      sub, SourceLoc(), SourceLoc(), TypeExpr::createImplicit(castTy, ctx));
  expr->setType(OptionalType::get(castTy));
  expr->setImplicit();
  return expr;
}

IsExpr *IsExpr::create(ASTContext &ctx, SourceLoc isLoc, TypeRepr *tyRepr) {
  return new (ctx) IsExpr(nullptr, isLoc, new (ctx) TypeExpr(tyRepr));
}

CoerceExpr *CoerceExpr::create(ASTContext &ctx, SourceLoc asLoc,
                               TypeRepr *tyRepr) {
  return new (ctx) CoerceExpr(nullptr, asLoc, new (ctx) TypeExpr(tyRepr));
}

CoerceExpr *CoerceExpr::createImplicit(ASTContext &ctx, Expr *sub,
                                       Type castTy) {
  auto *const expr = new (ctx)
      CoerceExpr(sub, SourceLoc(), TypeExpr::createImplicit(castTy, ctx));
  expr->setType(castTy);
  expr->setImplicit();
  return expr;
}

CoerceExpr *CoerceExpr::forLiteralInit(ASTContext &ctx, Expr *literal,
                                       SourceRange range,
                                       TypeRepr *literalTyRepr) {
  auto *const expr =
      new (ctx) CoerceExpr(range, literal, new (ctx) TypeExpr(literalTyRepr));
  expr->setImplicit();
  return expr;
}

RebindSelfInConstructorExpr::RebindSelfInConstructorExpr(Expr *SubExpr,
                                                         VarDecl *Self)
  : Expr(ExprKind::RebindSelfInConstructor, /*Implicit=*/true,
         TupleType::getEmpty(Self->getASTContext())),
    SubExpr(SubExpr), Self(Self)
{}

OtherConstructorDeclRefExpr *
RebindSelfInConstructorExpr::getCalledConstructor(bool &isChainToSuper) const {
  // Dig out the OtherConstructorDeclRefExpr. Note that this is the reverse
  // of what we do in pre-checking.
  Expr *candidate = getSubExpr();
  while (true) {
    // Look through identity expressions.
    if (auto identity = dyn_cast<IdentityExpr>(candidate)) {
      candidate = identity->getSubExpr();
      continue;
    }

    // Look through force-value expressions.
    if (auto force = dyn_cast<ForceValueExpr>(candidate)) {
      candidate = force->getSubExpr();
      continue;
    }

    // Look through all try expressions.
    if (auto tryExpr = dyn_cast<AnyTryExpr>(candidate)) {
      candidate = tryExpr->getSubExpr();
      continue;
    }

    // Look through covariant return expressions.
    if (auto covariantExpr
          = dyn_cast<CovariantReturnConversionExpr>(candidate)) {
      candidate = covariantExpr->getSubExpr();
      continue;
    }
    
    // Look through inject into optional expressions
    if (auto injectIntoOptionalExpr
        = dyn_cast<InjectIntoOptionalExpr>(candidate)) {
      candidate = injectIntoOptionalExpr->getSubExpr();
      continue;
    }

    // Look through open existential expressions
    if (auto openExistentialExpr
        = dyn_cast<OpenExistentialExpr>(candidate)) {
      candidate = openExistentialExpr->getSubExpr();
      continue;
    }
    break;
  }

  // We hit an application, find the constructor reference.
  OtherConstructorDeclRefExpr *otherCtorRef;
  const ApplyExpr *apply;
  do {
    apply = cast<ApplyExpr>(candidate);
    candidate = apply->getFn();
    auto candidateUnwrapped = candidate->getSemanticsProvidingExpr();
    otherCtorRef = dyn_cast<OtherConstructorDeclRefExpr>(candidateUnwrapped);
  } while (!otherCtorRef);

  auto *unaryArg = apply->getArgs()->getUnaryExpr();
  assert(unaryArg);
  isChainToSuper = unaryArg->isSuperExpr();
  return otherCtorRef;
}

ActorIsolation ClosureActorIsolation::getActorIsolation() const {
  switch (getKind()) {
  case ClosureActorIsolation::Independent:
    return ActorIsolation::forIndependent().withPreconcurrency(
        preconcurrency());

  case ClosureActorIsolation::GlobalActor: {
    return ActorIsolation::forGlobalActor(getGlobalActor(), /*unsafe=*/false)
        .withPreconcurrency(preconcurrency());
  }

  case ClosureActorIsolation::ActorInstance: {
    auto selfDecl = getActorInstance();
    auto actor =
        selfDecl->getTypeInContext()->getReferenceStorageReferent()->getAnyActor();
    assert(actor && "Bad closure actor isolation?");
    // FIXME: This could be a parameter... or a capture... hmmm.
    return ActorIsolation::forActorInstanceSelf(actor).withPreconcurrency(
        preconcurrency());
  }
  }
}

unsigned AbstractClosureExpr::getDiscriminator() const {
  auto raw = getRawDiscriminator();
  if (raw != InvalidDiscriminator)
    return raw;

  ASTContext &ctx = getASTContext();
  evaluateOrDefault(
      ctx.evaluator, LocalDiscriminatorsRequest{getParent()}, 0);

  // If we don't have a discriminator, and either
  //   1. We have ill-formed code and we're able to assign a discriminator, or
  //   2. We are in a macro expansion buffer
  //
  // then assign the next discriminator now.
  if (getRawDiscriminator() == InvalidDiscriminator &&
      (ctx.Diags.hadAnyError() ||
       getParentSourceFile()->getFulfilledMacroRole() != llvm::None)) {
    auto discriminator = ctx.getNextDiscriminator(getParent());
    ctx.setMaxAssignedDiscriminator(getParent(), discriminator + 1);
    const_cast<AbstractClosureExpr *>(this)->
        Bits.AbstractClosureExpr.Discriminator = discriminator;
  }

  assert(getRawDiscriminator() != InvalidDiscriminator);
  return getRawDiscriminator();
}

void AbstractClosureExpr::setParameterList(ParameterList *P) {
  parameterList = P;
  // Change the DeclContext of any parameters to be this closure.
  if (P)
    P->setDeclContextOfParamDecls(this);
}

BraceStmt * AbstractClosureExpr::getBody() const {
  if (const AutoClosureExpr *autocls = dyn_cast<AutoClosureExpr>(this))
    return autocls->getBody();
  if (const ClosureExpr *cls = dyn_cast<ClosureExpr>(this))
    return cls->getBody();
  llvm_unreachable("Unknown closure expression");
}

Type AbstractClosureExpr::getResultType(
    llvm::function_ref<Type(Expr *)> getType) const {
  auto *E = const_cast<AbstractClosureExpr *>(this);
  Type T = getType(E);
  if (!T || T->hasError())
    return T;

  return T->castTo<FunctionType>()->getResult();
}

bool AbstractClosureExpr::isBodyThrowing() const {
  if (!getType() || getType()->hasError()) {
    // Scan the closure body to infer effects.
    if (auto closure = dyn_cast<ClosureExpr>(this)) {
      return evaluateOrDefault(
          getASTContext().evaluator,
          ClosureEffectsRequest{const_cast<ClosureExpr *>(closure)},
          FunctionType::ExtInfo()).isThrowing();
    }

    return false;
  }
  
  return getType()->castTo<FunctionType>()->getExtInfo().isThrowing();
}

bool AbstractClosureExpr::isSendable() const {
  if (!getType() || getType()->hasError()) {
    // Scan the closure body to infer effects.
    if (auto closure = dyn_cast<ClosureExpr>(this)) {
      return evaluateOrDefault(
          getASTContext().evaluator,
          ClosureEffectsRequest{const_cast<ClosureExpr *>(closure)},
          FunctionType::ExtInfo()).isSendable();
    }

    return false;
  }

  return getType()->castTo<FunctionType>()->getExtInfo().isSendable();
}

bool AbstractClosureExpr::isBodyAsync() const {
  if (!getType() || getType()->hasError()) {
    // Scan the closure body to infer effects.
    if (auto closure = dyn_cast<ClosureExpr>(this)) {
      return evaluateOrDefault(
          getASTContext().evaluator,
          ClosureEffectsRequest{const_cast<ClosureExpr *>(closure)},
          FunctionType::ExtInfo()).isAsync();
    }

    return false;
  }

  return getType()->castTo<FunctionType>()->getExtInfo().isAsync();
}

bool AbstractClosureExpr::hasSingleExpressionBody() const {
  if (auto closure = dyn_cast<ClosureExpr>(this))
    return closure->hasSingleExpressionBody();

  return true;
}

Expr *AbstractClosureExpr::getSingleExpressionBody() const {
  if (auto closure = dyn_cast<ClosureExpr>(this))
    return closure->getSingleExpressionBody();
  else if (auto autoclosure = dyn_cast<AutoClosureExpr>(this))
    return autoclosure->getSingleExpressionBody();

  return nullptr;
}

ClosureActorIsolation
swift::__AbstractClosureExpr_getActorIsolation(AbstractClosureExpr *CE) {
  return CE->getActorIsolation();
}

llvm::function_ref<ClosureActorIsolation(AbstractClosureExpr *)>
swift::_getRef__AbstractClosureExpr_getActorIsolation() {
  return __AbstractClosureExpr_getActorIsolation;
}

#define FORWARD_SOURCE_LOCS_TO(CLASS, NODE) \
  SourceRange CLASS::getSourceRange() const {     \
    if (!NODE) { return SourceRange(); }          \
    return (NODE)->getSourceRange();              \
  }                                               \
  SourceLoc CLASS::getStartLoc() const {          \
    if (!NODE) { return SourceLoc(); }            \
    return (NODE)->getStartLoc();                 \
  }                                               \
  SourceLoc CLASS::getEndLoc() const {            \
    if (!NODE) { return SourceLoc(); }            \
    return (NODE)->getEndLoc();                   \
  }                                               \
  SourceLoc CLASS::getLoc() const {               \
    if (!NODE) { return SourceLoc(); }            \
    return (NODE)->getStartLoc();                 \
  }

FORWARD_SOURCE_LOCS_TO(ClosureExpr, Body.getPointer())

Expr *ClosureExpr::getSingleExpressionBody() const {
  assert(hasSingleExpressionBody() && "Not a single-expression body");
  auto body = getBody()->getLastElement();
  if (auto stmt = body.dyn_cast<Stmt *>()) {
    if (auto braceStmt = dyn_cast<BraceStmt>(stmt))
      return braceStmt->getLastElement().get<Expr *>();

    return cast<ReturnStmt>(stmt)->getResult();
  }
  return body.get<Expr *>();
}

bool ClosureExpr::hasEmptyBody() const {
  return getBody()->empty();
}

void ClosureExpr::setExplicitResultType(Type ty) {
  assert(ty && !ty->hasTypeVariable() && !ty->hasPlaceholder());
  ExplicitResultTypeAndBodyState.getPointer()
      ->setType(MetatypeType::get(ty));
}

FORWARD_SOURCE_LOCS_TO(AutoClosureExpr, Body)

void AutoClosureExpr::setBody(Expr *E) {
  auto &Context = getASTContext();
  auto *RS = new (Context) ReturnStmt(SourceLoc(), E);
  Body = BraceStmt::create(Context, E->getStartLoc(), { RS }, E->getEndLoc());
}

Expr *AutoClosureExpr::getSingleExpressionBody() const {
  return cast<ReturnStmt>(Body->getLastElement().get<Stmt *>())->getResult();
}

Expr *AutoClosureExpr::getUnwrappedCurryThunkExpr() const {
  auto maybeUnwrapOpenExistential = [](Expr *expr) {
    if (auto *openExistential = dyn_cast<OpenExistentialExpr>(expr)) {
      expr = openExistential->getSubExpr()->getSemanticsProvidingExpr();
      if (auto *ICE = dyn_cast<ImplicitConversionExpr>(expr))
        expr = ICE->getSyntacticSubExpr();
    }

    return expr;
  };

  auto maybeUnwrapOptionalEval = [](Expr *expr) {
    if (auto optEval = dyn_cast<OptionalEvaluationExpr>(expr))
      expr = optEval->getSubExpr();
    if (auto inject = dyn_cast<InjectIntoOptionalExpr>(expr))
      expr = inject->getSubExpr();
    if (auto erasure = dyn_cast<ErasureExpr>(expr))
      expr = erasure->getSubExpr();
    if (auto bind = dyn_cast<BindOptionalExpr>(expr))
      expr = bind->getSubExpr();
    return expr;
  };

  auto maybeUnwrapConversions = [](Expr *expr) {
    if (auto *covariantReturn = dyn_cast<CovariantReturnConversionExpr>(expr))
      expr = covariantReturn->getSubExpr();
    if (auto *functionConversion = dyn_cast<FunctionConversionExpr>(expr))
      expr = functionConversion->getSubExpr();
    return expr;
  };

  switch (getThunkKind()) {
  case AutoClosureExpr::Kind::None:
  case AutoClosureExpr::Kind::AsyncLet:
    break;

  case AutoClosureExpr::Kind::SingleCurryThunk: {
    auto *body = getSingleExpressionBody();
    body = body->getSemanticsProvidingExpr();
    body = maybeUnwrapOpenExistential(body);
    body = maybeUnwrapOptionalEval(body);
    body = maybeUnwrapConversions(body);

    if (auto *outerCall = dyn_cast<ApplyExpr>(body)) {
      return outerCall->getFn();
    }

    assert(false && "Malformed curry thunk?");
    break;
  }

  case AutoClosureExpr::Kind::DoubleCurryThunk: {
    auto *body = getSingleExpressionBody();
    if (auto *innerClosure = dyn_cast<AutoClosureExpr>(body)) {
      assert(innerClosure->getThunkKind() ==
               AutoClosureExpr::Kind::SingleCurryThunk);
      auto *innerBody = innerClosure->getSingleExpressionBody();
      innerBody = innerBody->getSemanticsProvidingExpr();
      innerBody = maybeUnwrapOpenExistential(innerBody);
      innerBody = maybeUnwrapOptionalEval(innerBody);
      innerBody = maybeUnwrapConversions(innerBody);

      if (auto *outerCall = dyn_cast<ApplyExpr>(innerBody)) {
        auto outerFn = maybeUnwrapConversions(outerCall->getFn());
        if (auto *innerCall = dyn_cast<ApplyExpr>(outerFn)) {
          return innerCall->getFn();
        }
      }
    }

    assert(false && "Malformed curry thunk?");
    break;
  }
  }

  return nullptr;
}

FORWARD_SOURCE_LOCS_TO(UnresolvedPatternExpr, subPattern)

TypeExpr::TypeExpr(TypeRepr *Repr)
  : Expr(ExprKind::Type, /*implicit*/false), Repr(Repr) {}

TypeExpr *TypeExpr::createImplicit(Type Ty, ASTContext &C) {
  assert(Ty);
  auto *result = new (C) TypeExpr(nullptr);
  result->setType(MetatypeType::get(Ty, Ty->getASTContext()));
  result->setImplicit();
  return result;
}

// The type of a TypeExpr is always a metatype type.  Return the instance
// type or null if not set yet.
Type TypeExpr::getInstanceType() const {
  auto ty = getType();
  if (!ty)
    return Type();

  if (auto metaType = ty->getAs<MetatypeType>())
    return metaType->getInstanceType();

  return ErrorType::get(ty->getASTContext());
}

TypeExpr *TypeExpr::createForDecl(DeclNameLoc Loc, TypeDecl *Decl,
                                  DeclContext *DC) {
  ASTContext &C = Decl->getASTContext();
  assert(Loc.isValid());
  auto *Repr = new (C) SimpleIdentTypeRepr(Loc, Decl->createNameRef());
  Repr->setValue(Decl, DC);
  return new (C) TypeExpr(Repr);
}

TypeExpr *TypeExpr::createImplicitForDecl(DeclNameLoc Loc, TypeDecl *Decl,
                                          DeclContext *DC, Type ty) {
  ASTContext &C = Decl->getASTContext();
  auto *Repr = new (C) SimpleIdentTypeRepr(Loc, Decl->createNameRef());
  Repr->setValue(Decl, DC);
  auto result = new (C) TypeExpr(Repr);
  assert(ty && !ty->hasTypeParameter());
  result->setType(ty);
  result->setImplicit();
  return result;
}

TypeExpr *TypeExpr::createForMemberDecl(DeclNameLoc ParentNameLoc,
                                        TypeDecl *Parent,
                                        DeclNameLoc NameLoc,
                                        TypeDecl *Decl) {
  ASTContext &C = Decl->getASTContext();
  assert(ParentNameLoc.isValid());
  assert(NameLoc.isValid());

  // The base component is the parent type.
  auto *ParentComp = new (C) SimpleIdentTypeRepr(ParentNameLoc,
                                                 Parent->createNameRef());
  ParentComp->setValue(Parent, nullptr);

  // The member component is the member we just found.
  auto *MemberComp =
      new (C) SimpleIdentTypeRepr(NameLoc, Decl->createNameRef());
  MemberComp->setValue(Decl, nullptr);

  auto *TR = MemberTypeRepr::create(C, ParentComp, {MemberComp});
  return new (C) TypeExpr(TR);
}

TypeExpr *TypeExpr::createForMemberDecl(TypeRepr *ParentTR, DeclNameLoc NameLoc,
                                        TypeDecl *Decl) {
  ASTContext &C = Decl->getASTContext();

  // Add a new component for the member we just found.
  auto *NewComp = new (C) SimpleIdentTypeRepr(NameLoc, Decl->createNameRef());
  NewComp->setValue(Decl, nullptr);

  TypeRepr *TR = nullptr;
  if (auto *DeclRefTR = dyn_cast<DeclRefTypeRepr>(ParentTR)) {
    // Create a new list of components.
    SmallVector<IdentTypeRepr *, 4> Components;
    if (auto *MemberTR = dyn_cast<MemberTypeRepr>(ParentTR)) {
      auto MemberComps = MemberTR->getMemberComponents();
      Components.append(MemberComps.begin(), MemberComps.end());
    }

    Components.push_back(NewComp);
    TR = MemberTypeRepr::create(C, DeclRefTR->getBaseComponent(), Components);
  } else {
    TR = MemberTypeRepr::create(C, ParentTR, NewComp);
  }

  return new (C) TypeExpr(TR);
}

TypeExpr *TypeExpr::createForSpecializedDecl(DeclRefTypeRepr *ParentTR,
                                             ArrayRef<TypeRepr *> Args,
                                             SourceRange AngleLocs,
                                             ASTContext &C) {
  auto *lastComp = ParentTR->getLastComponent();

  if (!isa<SimpleIdentTypeRepr>(lastComp) || !lastComp->getBoundDecl())
    return nullptr;

  if (isa<TypeAliasDecl>(lastComp->getBoundDecl())) {
    if (auto *memberTR = dyn_cast<MemberTypeRepr>(ParentTR)) {
      // If any of our parent types are unbound, bail out and let
      // the constraint solver can infer generic parameters for them.
      //
      // This is because a type like GenericClass.GenericAlias<Int>
      // cannot be represented directly.
      //
      // This also means that [GenericClass.GenericAlias<Int>]()
      // won't parse correctly, whereas if we fully specialize
      // GenericClass, it does.
      //
      // FIXME: Once we can model generic typealiases properly, rip
      // this out.
      auto isUnboundGenericComponent = [](IdentTypeRepr *ITR) -> bool {
        if (isa<SimpleIdentTypeRepr>(ITR)) {
          auto *decl = dyn_cast_or_null<GenericTypeDecl>(ITR->getBoundDecl());
          if (decl && decl->isGeneric())
            return true;
        }

        return false;
      };

      for (auto *comp : memberTR->getMemberComponents().drop_back()) {
        if (isUnboundGenericComponent(comp))
          return nullptr;
      }

      if (auto *identBase =
              dyn_cast<IdentTypeRepr>(memberTR->getBaseComponent())) {
        if (isUnboundGenericComponent(identBase))
          return nullptr;
      }
    }
  }

  auto *genericComp = GenericIdentTypeRepr::create(
      C, lastComp->getNameLoc(), lastComp->getNameRef(), Args, AngleLocs);
  genericComp->setValue(lastComp->getBoundDecl(), lastComp->getDeclContext());

  TypeRepr *TR = nullptr;
  if (auto *memberTR = dyn_cast<MemberTypeRepr>(ParentTR)) {
    auto oldMemberComps = memberTR->getMemberComponents().drop_back();

    // Create a new list of member components, replacing the last one with the
    // new specialized one.
    SmallVector<IdentTypeRepr *, 2> newMemberComps;
    newMemberComps.append(oldMemberComps.begin(), oldMemberComps.end());
    newMemberComps.push_back(genericComp);

    TR =
        MemberTypeRepr::create(C, memberTR->getBaseComponent(), newMemberComps);
  } else {
    TR = genericComp;
  }

  return new (C) TypeExpr(TR);
}

// Create an implicit TypeExpr, with location information even though it
// shouldn't have one.  This is presently used to work around other location
// processing bugs.  If you have an implicit location, use createImplicit.
TypeExpr *TypeExpr::createImplicitHack(SourceLoc Loc, Type Ty, ASTContext &C) {
  // FIXME: This is horrible.
  assert(Ty);
  if (Loc.isInvalid()) return createImplicit(Ty, C);
  auto *Repr = new (C) FixedTypeRepr(Ty, Loc);
  auto *Res = new (C) TypeExpr(Repr);
  Res->setType(MetatypeType::get(Ty, C));
  Res->setImplicit();
  return Res;
}

SourceRange TypeExpr::getSourceRange() const {
  if (!getTypeRepr()) return SourceRange();
  return getTypeRepr()->getSourceRange();
}

bool Expr::isSelfExprOf(const AbstractFunctionDecl *AFD, bool sameBase) const {
  auto *E = getSemanticsProvidingExpr();

  if (auto IOE = dyn_cast<InOutExpr>(E))
    E = IOE->getSubExpr();

  while (auto ICE = dyn_cast<ImplicitConversionExpr>(E)) {
    if (sameBase && isa<DerivedToBaseExpr>(ICE))
      return false;
    E = ICE->getSubExpr();
  }

  if (auto DRE = dyn_cast<DeclRefExpr>(E))
    return DRE->getDecl() == AFD->getImplicitSelfDecl();

  return false;
}

OpenedArchetypeType *OpenExistentialExpr::getOpenedArchetype() const {
  auto type = getOpaqueValue()->getType()->getRValueType();
  while (auto metaTy = type->getAs<MetatypeType>())
    type = metaTy->getInstanceType();
  return type->castTo<OpenedArchetypeType>();
}

KeyPathExpr::KeyPathExpr(SourceLoc startLoc, Expr *parsedRoot,
                         Expr *parsedPath, SourceLoc endLoc, bool hasLeadingDot,
                         bool isObjC, bool isImplicit)
    : Expr(ExprKind::KeyPath, isImplicit), StartLoc(startLoc), EndLoc(endLoc),
      ParsedRoot(parsedRoot), ParsedPath(parsedPath),
      HasLeadingDot(hasLeadingDot) {
  assert(!(isObjC && (parsedRoot || parsedPath)) &&
         "Obj-C key paths should only have components");
  Bits.KeyPathExpr.IsObjC = isObjC;
}

KeyPathExpr::KeyPathExpr(SourceLoc backslashLoc, Expr *parsedRoot,
                         Expr *parsedPath, bool hasLeadingDot, bool isImplicit)
    : KeyPathExpr(backslashLoc, parsedRoot, parsedPath,
                  parsedPath ? parsedPath->getEndLoc()
                             : parsedRoot->getEndLoc(),
                  hasLeadingDot, /*isObjC*/ false, isImplicit) {
  assert((parsedRoot || parsedPath) &&
         "Key path must have either root or path");
}

KeyPathExpr::KeyPathExpr(ASTContext &ctx, SourceLoc startLoc,
                         ArrayRef<Component> components, SourceLoc endLoc,
                         bool isObjC, bool isImplicit)
    : KeyPathExpr(startLoc, /*parsedRoot*/ nullptr, /*parsedPath*/ nullptr,
                  endLoc, /*hasLeadingDot*/ false, isObjC, isImplicit) {
  assert(!components.empty());
  Components = ctx.AllocateCopy(components);
}

KeyPathExpr *KeyPathExpr::createParsedPoundKeyPath(
    ASTContext &ctx, SourceLoc keywordLoc, SourceLoc lParenLoc,
    ArrayRef<Component> components, SourceLoc rParenLoc) {
  return new (ctx) KeyPathExpr(ctx, keywordLoc, components, rParenLoc,
                               /*isObjC*/ true, /*isImplicit*/ false);
}

KeyPathExpr *KeyPathExpr::createParsed(ASTContext &ctx, SourceLoc backslashLoc,
                                       Expr *parsedRoot, Expr *parsedPath,
                                       bool hasLeadingDot) {
  return new (ctx) KeyPathExpr(backslashLoc, parsedRoot, parsedPath,
                               hasLeadingDot, /*isImplicit*/ false);
}

KeyPathExpr *KeyPathExpr::createImplicit(ASTContext &ctx,
                                         SourceLoc backslashLoc,
                                         ArrayRef<Component> components,
                                         SourceLoc endLoc) {
  return new (ctx) KeyPathExpr(ctx, backslashLoc, components, endLoc,
                               /*isObjC*/ false, /*isImplicit*/ true);
}

KeyPathExpr *KeyPathExpr::createImplicit(ASTContext &ctx,
                                         SourceLoc backslashLoc,
                                         Expr *parsedRoot, Expr *parsedPath,
                                         bool hasLeadingDot) {
  return new (ctx) KeyPathExpr(backslashLoc, parsedRoot, parsedPath,
           hasLeadingDot, /*isImplicit*/ true);
}

void
KeyPathExpr::setComponents(ASTContext &C,
                           ArrayRef<KeyPathExpr::Component> newComponents) {
  // Reallocate the components array if it needs to be.
  if (Components.size() < newComponents.size()) {
    Components = C.Allocate<Component>(newComponents.size());
    for (unsigned i : indices(Components)) {
      ::new ((void*)&Components[i]) Component{};
    }
  }
  
  for (unsigned i : indices(newComponents)) {
    Components[i] = newComponents[i];
  }
  Components = Components.slice(0, newComponents.size());
}

llvm::Optional<unsigned> KeyPathExpr::findComponentWithSubscriptArg(Expr *arg) {
  for (auto idx : indices(getComponents())) {
    if (auto *args = getComponents()[idx].getSubscriptArgs()) {
      if (args->findArgumentExpr(arg))
        return idx;
    }
  }
  return llvm::None;
}

KeyPathExpr::Component KeyPathExpr::Component::forSubscript(
    ASTContext &ctx, ConcreteDeclRef subscript, ArgumentList *argList,
    Type elementType, ArrayRef<ProtocolConformanceRef> indexHashables) {
  return Component(subscript, argList, indexHashables, Kind::Subscript,
                   elementType, argList->getLParenLoc());
}

KeyPathExpr::Component
KeyPathExpr::Component::forUnresolvedSubscript(ASTContext &ctx,
                                               ArgumentList *argList) {
  return Component({}, argList, {}, Kind::UnresolvedSubscript, Type(),
                   argList->getLParenLoc());
}

KeyPathExpr::Component::Component(
    DeclNameOrRef decl, ArgumentList *argList,
    ArrayRef<ProtocolConformanceRef> indexHashables, Kind kind, Type type,
    SourceLoc loc)
    : Decl(decl), SubscriptArgList(argList), KindValue(kind),
      ComponentType(type), Loc(loc) {
  assert(kind == Kind::Subscript || kind == Kind::UnresolvedSubscript);
  assert(argList);
  assert(argList->size() == indexHashables.size() || indexHashables.empty());
  SubscriptHashableConformancesData = indexHashables.empty()
    ? nullptr : indexHashables.data();
}

SingleValueStmtExpr *SingleValueStmtExpr::create(ASTContext &ctx, Stmt *S,
                                                 DeclContext *DC) {
  return new (ctx) SingleValueStmtExpr(S, DC);
}

SingleValueStmtExpr *SingleValueStmtExpr::createWithWrappedBranches(
    ASTContext &ctx, Stmt *S, DeclContext *DC, bool mustBeExpr) {
  auto *SVE = create(ctx, S, DC);

  // Attempt to wrap any branches that can be wrapped.
  SmallVector<Stmt *, 4> scratch;
  for (auto *branch : SVE->getBranches(scratch)) {
    auto *BS = dyn_cast<BraceStmt>(branch);
    if (!BS)
      continue;

    auto *S = BS->getSingleActiveStatement();
    if (!S)
      continue;

    if (mustBeExpr) {
      // If this must be an expression, we can eagerly wrap any exhaustive if
      // and switch branch.
      if (auto *IS = dyn_cast<IfStmt>(S)) {
        if (!IS->isSyntacticallyExhaustive())
          continue;
      } else if (!isa<SwitchStmt>(S)) {
        continue;
      }
    } else {
      // Otherwise do the semantic checking to verify that we can wrap the
      // branch.
      if (!S->mayProduceSingleValue(ctx))
        continue;
    }
    BS->setLastElement(
        SingleValueStmtExpr::createWithWrappedBranches(ctx, S, DC, mustBeExpr));
  }
  return SVE;
}

SingleValueStmtExpr *
SingleValueStmtExpr::tryDigOutSingleValueStmtExpr(Expr *E) {
  while (true) {
    // Look through implicit conversions.
    if (auto *ICE = dyn_cast<ImplicitConversionExpr>(E)) {
      E = ICE->getSubExpr();
      continue;
    }
    // Look through coercions.
    if (auto *CE = dyn_cast<CoerceExpr>(E)) {
      E = CE->getSubExpr();
      continue;
    }
    // Look through try/await (this is invalid, but we'll error on it in
    // effect checking).
    if (auto *TE = dyn_cast<AnyTryExpr>(E)) {
      E = TE->getSubExpr();
      continue;
    }
    if (auto *AE = dyn_cast<AwaitExpr>(E)) {
      E = AE->getSubExpr();
      continue;
    }
    break;
  }
  return dyn_cast<SingleValueStmtExpr>(E);
}

SourceRange SingleValueStmtExpr::getSourceRange() const {
  return S->getSourceRange();
}

SingleValueStmtExpr::Kind SingleValueStmtExpr::getStmtKind() const {
  switch (getStmt()->getKind()) {
  case StmtKind::If:
    return Kind::If;
  case StmtKind::Switch:
    return Kind::Switch;
  default:
    llvm_unreachable("Unhandled kind!");
  }
}

ArrayRef<Stmt *>
SingleValueStmtExpr::getBranches(SmallVectorImpl<Stmt *> &scratch) const {
  switch (getStmtKind()) {
  case Kind::If:
    return cast<IfStmt>(getStmt())->getBranches(scratch);
  case Kind::Switch:
    return cast<SwitchStmt>(getStmt())->getBranches(scratch);
  }
  llvm_unreachable("Unhandled case in switch!");
}

ArrayRef<Expr *> SingleValueStmtExpr::getSingleExprBranches(
    SmallVectorImpl<Expr *> &scratch) const {
  assert(scratch.empty());
  SmallVector<Stmt *, 4> stmtScratch;
  for (auto *branch : getBranches(stmtScratch)) {
    auto *BS = dyn_cast<BraceStmt>(branch);
    if (!BS)
      continue;
    if (auto *E = BS->getSingleActiveExpression())
      scratch.push_back(E);
  }
  return scratch;
}

void InterpolatedStringLiteralExpr::forEachSegment(ASTContext &Ctx, 
    llvm::function_ref<void(bool, CallExpr *)> callback) {
  auto appendingExpr = getAppendingExpr();
  for (auto stmt : appendingExpr->getBody()->getElements()) {
    if (auto expr = stmt.dyn_cast<Expr*>()) {
      if (auto call = dyn_cast<CallExpr>(expr)) {
        DeclName name;
        if (auto fn = call->getCalledValue()) {
          name = fn->getName();
        } else if (auto unresolvedDot =
                      dyn_cast<UnresolvedDotExpr>(call->getFn())) {
          name = unresolvedDot->getName().getFullName();
        }

        bool isInterpolation = (name.getBaseName() ==
                                Ctx.Id_appendInterpolation);

        callback(isInterpolation, call);
      }
    }
  }
}

TapExpr::TapExpr(Expr * SubExpr, BraceStmt *Body)
    : Expr(ExprKind::Tap, /*Implicit=*/true),
      SubExpr(SubExpr), Body(Body) {
  assert(Body);
  assert(!Body->empty() &&
       Body->getFirstElement().isDecl(DeclKind::Var) &&
       "First element of Body should be a variable to init with the subExpr");
}

VarDecl * TapExpr::getVar() const {
  return dyn_cast<VarDecl>(Body->getFirstElement().dyn_cast<Decl *>());
}

SourceRange TapExpr::getSourceRange() const {
  if (!SubExpr)
    return Body->getSourceRange();

  SourceLoc start = SubExpr->getStartLoc();
  if (!start.isValid())
    start = Body->getStartLoc();
  if (!start.isValid())
    return SourceRange();

  SourceLoc end = Body->getEndLoc();
  if (!end.isValid())
    end = SubExpr->getEndLoc();
  if (!end.isValid())
    return SourceRange();

  return SourceRange(start, end);
}

RegexLiteralExpr *
RegexLiteralExpr::createParsed(ASTContext &ctx, SourceLoc loc,
                               StringRef regexText, unsigned version,
                               ArrayRef<uint8_t> serializedCaps) {
  return new (ctx) RegexLiteralExpr(loc, regexText, version, serializedCaps,
                                    /*implicit*/ false);
}

TypeJoinExpr::TypeJoinExpr(llvm::PointerUnion<DeclRefExpr *, TypeBase *> result,
                           ArrayRef<Expr *> elements, SingleValueStmtExpr *SVE)
    : Expr(ExprKind::TypeJoin, /*implicit=*/true, Type()), Var(nullptr),
      SVE(SVE) {

  if (auto *varRef = result.dyn_cast<DeclRefExpr *>()) {
    assert(varRef);
    Var = varRef;
  } else {
    auto joinType = Type(result.get<TypeBase *>());
    assert(joinType && "expected non-null type");
    setType(joinType);
  }

  Bits.TypeJoinExpr.NumElements = elements.size();
  // Copy elements.
  std::uninitialized_copy(elements.begin(), elements.end(),
                          getTrailingObjects<Expr *>());
}

TypeJoinExpr *TypeJoinExpr::createImpl(
    ASTContext &ctx, llvm::PointerUnion<DeclRefExpr *, TypeBase *> varOrType,
    ArrayRef<Expr *> elements, AllocationArena arena,
    SingleValueStmtExpr *SVE) {
  size_t size = totalSizeToAlloc<Expr *>(elements.size());
  void *mem = ctx.Allocate(size, alignof(TypeJoinExpr), arena);
  return new (mem) TypeJoinExpr(varOrType, elements, SVE);
}

TypeJoinExpr *
TypeJoinExpr::forBranchesOfSingleValueStmtExpr(ASTContext &ctx, Type joinType,
                                               SingleValueStmtExpr *SVE,
                                               AllocationArena arena) {
  return createImpl(ctx, joinType.getPointer(), /*elements*/ {}, arena, SVE);
}

MacroExpansionExpr *MacroExpansionExpr::create(
    DeclContext *dc, SourceLoc sigilLoc, DeclNameRef macroName,
    DeclNameLoc macroNameLoc, SourceLoc leftAngleLoc,
    ArrayRef<TypeRepr *> genericArgs, SourceLoc rightAngleLoc,
    ArgumentList *argList, MacroRoles roles, bool isImplicit,
    Type ty
) {
  ASTContext &ctx = dc->getASTContext();
  MacroExpansionInfo *info = new (ctx) MacroExpansionInfo{
      sigilLoc, macroName, macroNameLoc,
      leftAngleLoc, rightAngleLoc, genericArgs,
      argList ? argList : ArgumentList::createImplicit(ctx, {})
  };
  return new (ctx) MacroExpansionExpr(dc, info, roles, isImplicit, ty);
}

unsigned MacroExpansionExpr::getDiscriminator() const {
  if (getRawDiscriminator() != InvalidDiscriminator)
    return getRawDiscriminator();

  auto mutableThis = const_cast<MacroExpansionExpr *>(this);
  auto dc = getDeclContext();
  ASTContext &ctx = dc->getASTContext();
  auto discriminatorContext =
      MacroDiscriminatorContext::getParentOf(mutableThis);
  mutableThis->setDiscriminator(
      ctx.getNextMacroDiscriminator(
          discriminatorContext, getMacroName().getBaseName()));

  assert(getRawDiscriminator() != InvalidDiscriminator);
  return getRawDiscriminator();
}

MacroExpansionDecl *MacroExpansionExpr::createSubstituteDecl() {
  auto dc = DC;
  if (auto *tlcd = dyn_cast_or_null<TopLevelCodeDecl>(dc->getAsDecl()))
    dc = tlcd->getDeclContext();
  SubstituteDecl =
      new (DC->getASTContext()) MacroExpansionDecl(dc, getExpansionInfo());
  return SubstituteDecl;
}

MacroExpansionDecl *MacroExpansionExpr::getSubstituteDecl() const {
  return SubstituteDecl;
}

void swift::simple_display(llvm::raw_ostream &out, const ClosureExpr *CE) {
  if (!CE) {
    out << "(null)";
    return;
  }

  out << "closure";
}

void swift::simple_display(llvm::raw_ostream &out,
                           const DefaultArgumentExpr *expr) {
  if (!expr) {
    out << "(null)";
    return;
  }

  out << "default arg for param ";
  out << "#" << expr->getParamIndex() + 1 << " ";
  out << "of ";
  simple_display(out, expr->getDefaultArgsOwner().getDecl());
}

void swift::simple_display(llvm::raw_ostream &out,
                           const Expr *expr) {
  out << "expression";
}

SourceLoc swift::extractNearestSourceLoc(const DefaultArgumentExpr *expr) {
  return expr->getLoc();
}

// See swift/Basic/Statistic.h for declaration: this enables tracing Exprs, is
// defined here to avoid too much layering violation / circular linkage
// dependency.

struct ExprTraceFormatter : public UnifiedStatsReporter::TraceFormatter {
  void traceName(const void *Entity, raw_ostream &OS) const override {
    if (!Entity)
      return;
    const Expr *E = static_cast<const Expr *>(Entity);
    OS << Expr::getKindName(E->getKind());
  }
  void traceLoc(const void *Entity, SourceManager *SM,
                clang::SourceManager *CSM, raw_ostream &OS) const override {
    if (!Entity)
      return;
    const Expr *E = static_cast<const Expr *>(Entity);
    E->getSourceRange().print(OS, *SM, false);
  }
};

static ExprTraceFormatter TF;

template<>
const UnifiedStatsReporter::TraceFormatter*
FrontendStatsTracer::getTraceFormatter<const Expr *>() {
  return &TF;
}

