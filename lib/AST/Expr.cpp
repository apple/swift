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

// Only allow allocation of Stmts using the allocator in ASTContext.
void *Expr::operator new(size_t Bytes, ASTContext &C,
                         unsigned Alignment) {
  return C.Allocate(Bytes, Alignment);
}

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
  NO_REFERENCE(NilLiteral);
  NO_REFERENCE(IntegerLiteral);
  NO_REFERENCE(FloatLiteral);
  NO_REFERENCE(BooleanLiteral);
  NO_REFERENCE(StringLiteral);
  NO_REFERENCE(InterpolatedStringLiteral);
  NO_REFERENCE(ObjectLiteral);
  NO_REFERENCE(MagicIdentifierLiteral);
  NO_REFERENCE(DiscardAssignment);
  NO_REFERENCE(LazyInitializer);

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
  PASS_THROUGH_REFERENCE(Try, getSubExpr);
  PASS_THROUGH_REFERENCE(ForceTry, getSubExpr);
  PASS_THROUGH_REFERENCE(OptionalTry, getSubExpr);

  NO_REFERENCE(Tuple);
  NO_REFERENCE(Array);
  NO_REFERENCE(Dictionary);

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
  PASS_THROUGH_REFERENCE(FunctionConversion, getSubExpr);
  PASS_THROUGH_REFERENCE(CovariantFunctionConversion, getSubExpr);
  PASS_THROUGH_REFERENCE(CovariantReturnConversion, getSubExpr);
  PASS_THROUGH_REFERENCE(ImplicitlyUnwrappedFunctionConversion, getSubExpr);
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
  NO_REFERENCE(If);
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
    
    ChildWalker(llvm::function_ref<Expr *(Expr *)> callback, Expr *ThisNode)
      : callback(callback), ThisNode(ThisNode) {}
    
    std::pair<bool, Expr *> walkToExprPre(Expr *E) override {
      // When looking at the current node, of course we want to enter it.  We
      // also don't want to enumerate it.
      if (E == ThisNode)
        return { true, E };

      // Otherwise we must be a child of our expression, enumerate it!
      return { false, callback(E) };
    }
    
    std::pair<bool, Stmt *> walkToStmtPre(Stmt *S) override {
      return { false, S };
    }

    std::pair<bool, Pattern*> walkToPatternPre(Pattern *P) override {
      return { false, P };
    }
    bool walkToDeclPre(Decl *D) override { return false; }
    bool walkToTypeReprPre(TypeRepr *T) override { return false; }
  };
  
  this->walk(ChildWalker(callback, this));
}

/// Enumerate each immediate child expression of this node, invoking the
/// specific functor on it.  This ignores statements and other non-expression
/// children.
void Expr::forEachChildExpr(llvm::function_ref<Expr *(Expr *)> callback) {
  struct ChildWalker : ASTWalker {
    llvm::function_ref<Expr *(Expr *)> callback;

    ChildWalker(llvm::function_ref<Expr *(Expr *)> callback)
    : callback(callback) {}

    std::pair<bool, Expr *> walkToExprPre(Expr *E) override {
      // Enumerate the node!
      return { true, callback(E) };
    }

    std::pair<bool, Stmt *> walkToStmtPre(Stmt *S) override {
      return { false, S };
    }

    std::pair<bool, Pattern*> walkToPatternPre(Pattern *P) override {
      return { false, P };
    }
    bool walkToDeclPre(Decl *D) override { return false; }
    bool walkToTypeReprPre(TypeRepr *T) override { return false; }
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
  case ExprKind::MagicIdentifierLiteral:
  case ExprKind::ObjCSelector:
  case ExprKind::KeyPath:
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
  case ExprKind::DestructureTuple:
  case ExprKind::UnresolvedTypeConversion:
  case ExprKind::FunctionConversion:
  case ExprKind::CovariantFunctionConversion:
  case ExprKind::CovariantReturnConversion:
  case ExprKind::ImplicitlyUnwrappedFunctionConversion:
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
  case ExprKind::If:
  case ExprKind::Assign:
  case ExprKind::UnresolvedPattern:
  case ExprKind::EditorPlaceholder:
  case ExprKind::KeyPathDot:
    return false;

  case ExprKind::Tap:
    return true;
  }

  llvm_unreachable("Unhandled ExprKind in switch.");
}

llvm::DenseMap<Expr *, Expr *> Expr::getParentMap() {
  class RecordingTraversal : public ASTWalker {
  public:
    llvm::DenseMap<Expr *, Expr *> &ParentMap;

    explicit RecordingTraversal(llvm::DenseMap<Expr *, Expr *> &parentMap)
      : ParentMap(parentMap) { }

    std::pair<bool, Expr *> walkToExprPre(Expr *E) override {
      if (auto parent = Parent.getAsExpr())
        ParentMap[E] = parent;

      return { true, E };
    }
  };

  llvm::DenseMap<Expr *, Expr *> parentMap;
  RecordingTraversal traversal(parentMap);
  walk(traversal);
  return parentMap;
}

//===----------------------------------------------------------------------===//
// Support methods for Exprs.
//===----------------------------------------------------------------------===//

IntegerLiteralExpr * IntegerLiteralExpr::createFromUnsigned(ASTContext &C, unsigned value) {
  llvm::SmallString<8> Scratch;
  llvm::APInt(sizeof(unsigned)*8, value).toString(Scratch, 10, /*signed*/ false);
  auto Text = C.AllocateCopy(StringRef(Scratch));
  return new (C) IntegerLiteralExpr(Text, SourceLoc(), /*implicit*/ true);
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
      assert(value.isNegative() || value.isNullValue());
    }

    // Truncate down to the minimum number of bits required to express
    // this value exactly.
    auto requiredBits = value.getMinSignedBits();
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

static ArrayRef<Identifier> getArgumentLabelsFromArgument(
    Expr *arg, SmallVectorImpl<Identifier> &scratch,
    SmallVectorImpl<SourceLoc> *sourceLocs = nullptr,
    bool *hasTrailingClosure = nullptr,
    llvm::function_ref<Type(Expr *)> getType = [](Expr *E) -> Type {
      return E->getType();
    }) {
  if (sourceLocs) sourceLocs->clear();
  if (hasTrailingClosure) *hasTrailingClosure = false;

  // A parenthesized expression is a single, unlabeled argument.
  if (auto paren = dyn_cast<ParenExpr>(arg)) {
    scratch.clear();
    scratch.push_back(Identifier());
    if (hasTrailingClosure) *hasTrailingClosure = paren->hasTrailingClosure();
    return scratch;
  }

  // A tuple expression stores its element names, if they exist.
  if (auto tuple = dyn_cast<TupleExpr>(arg)) {
    if (sourceLocs && tuple->hasElementNameLocs()) {
      sourceLocs->append(tuple->getElementNameLocs().begin(),
                         tuple->getElementNameLocs().end());
    }

    if (hasTrailingClosure) *hasTrailingClosure = tuple->hasTrailingClosure();

    if (tuple->hasElementNames()) {
      assert(tuple->getElementNames().size() == tuple->getNumElements());
      return tuple->getElementNames();
    }

    scratch.assign(tuple->getNumElements(), Identifier());
    return scratch;
  }

  // Otherwise, use the type information.
  auto type = getType(arg);
  if (type->hasParenSugar()) {
    scratch.clear();
    scratch.push_back(Identifier());
    return scratch;    
  }

  // FIXME: Should be a dyn_cast.
  if (auto tupleTy = type->getAs<TupleType>()) {
    scratch.clear();
    for (const auto &elt : tupleTy->getElements())
      scratch.push_back(elt.getName());
    return scratch;
  }

  // FIXME: Shouldn't get here.
  scratch.clear();
  scratch.push_back(Identifier());
  return scratch;
}

/// Compute the type of an argument to a call (or call-like) AST
static void
computeSingleArgumentType(ASTContext &ctx, Expr *arg, bool implicit,
                          llvm::function_ref<Type(Expr *)> getType) {
  // Propagate 'implicit' to the argument.
  if (implicit) {
    arg->setImplicit(true);
  }

  // Handle parenthesized expressions.
  if (auto paren = dyn_cast<ParenExpr>(arg)) {
    if (auto type = getType(paren->getSubExpr())) {
      auto parenFlags = ParameterTypeFlags().withInOut(type->is<InOutType>());
      arg->setType(ParenType::get(ctx, type->getInOutObjectType(), parenFlags));
    }
    return;
  }

  // Handle tuples.
  auto tuple = dyn_cast<TupleExpr>(arg);
  SmallVector<TupleTypeElt, 4> typeElements;
  for (unsigned i = 0, n = tuple->getNumElements(); i != n; ++i) {
    auto type = getType(tuple->getElement(i));
    if (!type) return;

    bool isInOut = tuple->getElement(i)->isSemanticallyInOutExpr();
    typeElements.push_back(TupleTypeElt(type->getInOutObjectType(),
                                        tuple->getElementName(i),
                                        ParameterTypeFlags().withInOut(isInOut)));
  }
  arg->setType(TupleType::get(typeElements, ctx));
}

Expr *swift::packSingleArgument(ASTContext &ctx, SourceLoc lParenLoc,
                                ArrayRef<Expr *> args,
                                ArrayRef<Identifier> &argLabels,
                                ArrayRef<SourceLoc> &argLabelLocs,
                                SourceLoc rParenLoc,
                                ArrayRef<TrailingClosure> trailingClosures,
                                bool implicit,
                                SmallVectorImpl<Identifier> &argLabelsScratch,
                                SmallVectorImpl<SourceLoc> &argLabelLocsScratch,
                                llvm::function_ref<Type(Expr *)> getType) {
  // Clear out our scratch space.
  argLabelsScratch.clear();
  argLabelLocsScratch.clear();

  // Construct a TupleExpr or ParenExpr, as appropriate, for the argument.
  if (trailingClosures.empty()) {
    // Do we have a single, unlabeled argument?
    if (args.size() == 1 && (argLabels.empty() || argLabels[0].empty())) {
      auto arg = new (ctx) ParenExpr(lParenLoc, args[0], rParenLoc,
                                     /*hasTrailingClosure=*/false);
      computeSingleArgumentType(ctx, arg, implicit, getType);
      argLabelsScratch.push_back(Identifier());
      argLabels = argLabelsScratch;
      argLabelLocs = { };
      return arg;
    }

    // Make sure we have argument labels.
    if (argLabels.empty()) {
      argLabelsScratch.assign(args.size(), Identifier());
      argLabels = argLabelsScratch;
    }

    // Construct the argument tuple.
    if (argLabels.empty() && !args.empty()) {
      argLabelsScratch.assign(args.size(), Identifier());
      argLabels = argLabelsScratch;
    }
      
    auto arg = TupleExpr::create(ctx, lParenLoc, args, argLabels, argLabelLocs,
                                 rParenLoc, /*HasTrailingClosure=*/false,
                                 implicit);
    computeSingleArgumentType(ctx, arg, implicit, getType);
    return arg;
  }

  // If we have no other arguments, represent the a single trailing closure as a
  // parenthesized expression.
  if (args.empty() && trailingClosures.size() == 1 &&
      trailingClosures.front().LabelLoc.isInvalid()) {
    auto &trailingClosure = trailingClosures.front();
    auto arg =
        new (ctx) ParenExpr(lParenLoc, trailingClosure.ClosureExpr, rParenLoc,
                            /*hasTrailingClosure=*/true);
    computeSingleArgumentType(ctx, arg, implicit, getType);
    argLabelsScratch.push_back(Identifier());
    argLabels = argLabelsScratch;
    argLabelLocs = { };
    return arg;
  }

  assert(argLabels.empty() || args.size() == argLabels.size());

  unsigned numRegularArgs = args.size();

  // Form a tuple, including the trailing closure.
  SmallVector<Expr *, 4> argsScratch;
  argsScratch.reserve(numRegularArgs + trailingClosures.size());
  argsScratch.append(args.begin(), args.end());
  for (const auto &closure : trailingClosures)
    argsScratch.push_back(closure.ClosureExpr);
  args = argsScratch;

  {
    if (argLabels.empty()) {
      argLabelsScratch.resize(numRegularArgs);
    } else {
      argLabelsScratch.append(argLabels.begin(), argLabels.end());
    }

    for (const auto &closure : trailingClosures)
      argLabelsScratch.push_back(closure.Label);

    argLabels = argLabelsScratch;
  }

  {
    if (argLabelLocs.empty()) {
      argLabelLocsScratch.resize(numRegularArgs);
    } else {
      argLabelLocsScratch.append(argLabelLocs.begin(), argLabelLocs.end());
    }

    for (const auto &closure : trailingClosures)
      argLabelLocsScratch.push_back(closure.LabelLoc);

    argLabelLocs = argLabelLocsScratch;
  }

  Optional<unsigned> unlabeledTrailingClosureIndex;
  if (!trailingClosures.empty() && trailingClosures[0].Label.empty())
    unlabeledTrailingClosureIndex = args.size() - trailingClosures.size();

  auto arg = TupleExpr::create(ctx, lParenLoc, rParenLoc, args, argLabels,
                               argLabelLocs,
                               unlabeledTrailingClosureIndex,
                               /*Implicit=*/false);
  computeSingleArgumentType(ctx, arg, implicit, getType);
  return arg;
}

Optional<unsigned>
Expr::getUnlabeledTrailingClosureIndexOfPackedArgument() const {
  if (auto PE = dyn_cast<ParenExpr>(this))
    return PE->getUnlabeledTrailingClosureIndexOfPackedArgument();
  if (auto TE = dyn_cast<TupleExpr>(this))
    return TE->getUnlabeledTrailingClosureIndexOfPackedArgument();
  return None;
}

ObjectLiteralExpr::ObjectLiteralExpr(SourceLoc PoundLoc, LiteralKind LitKind,
                                     Expr *Arg,
                                     ArrayRef<Identifier> argLabels,
                                     ArrayRef<SourceLoc> argLabelLocs,
                                     bool hasTrailingClosure,
                                     bool implicit)
    : LiteralExpr(ExprKind::ObjectLiteral, implicit), 
      Arg(Arg), PoundLoc(PoundLoc) {
  Bits.ObjectLiteralExpr.LitKind = static_cast<unsigned>(LitKind);
  assert(getLiteralKind() == LitKind);
  Bits.ObjectLiteralExpr.NumArgLabels = argLabels.size();
  Bits.ObjectLiteralExpr.HasArgLabelLocs = !argLabelLocs.empty();
  Bits.ObjectLiteralExpr.HasTrailingClosure = hasTrailingClosure;
  initializeCallArguments(argLabels, argLabelLocs);
}

ObjectLiteralExpr *
ObjectLiteralExpr::create(ASTContext &ctx, SourceLoc poundLoc, LiteralKind kind,
                          Expr *arg, bool implicit,
                          llvm::function_ref<Type(Expr *)> getType) {
  // Inspect the argument to dig out the argument labels, their location, and
  // whether there is a trailing closure.
  SmallVector<Identifier, 4> argLabelsScratch;
  SmallVector<SourceLoc, 4> argLabelLocs;
  bool hasTrailingClosure = false;
  auto argLabels = getArgumentLabelsFromArgument(arg, argLabelsScratch,
                                                 &argLabelLocs,
                                                 &hasTrailingClosure,
                                                 getType);

  size_t size = totalSizeToAlloc(argLabels, argLabelLocs);

  void *memory = ctx.Allocate(size, alignof(ObjectLiteralExpr));
  return new (memory) ObjectLiteralExpr(poundLoc, kind, arg, argLabels,
                                        argLabelLocs, hasTrailingClosure,
                                        implicit);
}

ObjectLiteralExpr *ObjectLiteralExpr::create(ASTContext &ctx,
                                             SourceLoc poundLoc,
                                             LiteralKind kind,
                                             SourceLoc lParenLoc,
                                             ArrayRef<Expr *> args,
                                             ArrayRef<Identifier> argLabels,
                                             ArrayRef<SourceLoc> argLabelLocs,
                                             SourceLoc rParenLoc,
                                             ArrayRef<TrailingClosure> trailingClosures,
                                             bool implicit) {
  SmallVector<Identifier, 4> argLabelsScratch;
  SmallVector<SourceLoc, 4> argLabelLocsScratch;
  Expr *arg = packSingleArgument(ctx, lParenLoc, args, argLabels, argLabelLocs,
                                 rParenLoc,
                                 trailingClosures, implicit, argLabelsScratch,
                                 argLabelLocsScratch);

  size_t size = totalSizeToAlloc(argLabels, argLabelLocs);

  void *memory = ctx.Allocate(size, alignof(ObjectLiteralExpr));
  return new (memory) ObjectLiteralExpr(poundLoc, kind, arg, argLabels,
                                        argLabelLocs,
                                        trailingClosures.size() == 1, implicit);
}

StringRef ObjectLiteralExpr::getLiteralKindRawName() const {
  switch (getLiteralKind()) {
#define POUND_OBJECT_LITERAL(Name, Desc, Proto) case Name: return #Name;
#include "swift/Syntax/TokenKinds.def"    
  }
  llvm_unreachable("unspecified literal");
}

StringRef ObjectLiteralExpr::getLiteralKindPlainName() const {
  switch (getLiteralKind()) {
#define POUND_OBJECT_LITERAL(Name, Desc, Proto) case Name: return Desc;
#include "swift/Syntax/TokenKinds.def"    
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

SequenceExpr *SequenceExpr::create(ASTContext &ctx, ArrayRef<Expr*> elements) {
  assert(elements.size() & 1 && "even number of elements in sequence");
  size_t bytes = totalSizeToAlloc<Expr *>(elements.size());
  void *Buffer = ctx.Allocate(bytes, alignof(SequenceExpr));
  return ::new(Buffer) SequenceExpr(elements);
}

ErasureExpr *ErasureExpr::create(ASTContext &ctx, Expr *subExpr, Type type,
                                 ArrayRef<ProtocolConformanceRef> conformances){
  auto size = totalSizeToAlloc<ProtocolConformanceRef>(conformances.size());
  auto mem = ctx.Allocate(size, alignof(ErasureExpr));
  return ::new(mem) ErasureExpr(subExpr, type, conformances);
}

UnresolvedSpecializeExpr *UnresolvedSpecializeExpr::create(ASTContext &ctx,
                                             Expr *SubExpr, SourceLoc LAngleLoc,
                                             ArrayRef<TypeRepr *> UnresolvedParams,
                                             SourceLoc RAngleLoc) {
  auto size = totalSizeToAlloc<TypeRepr *>(UnresolvedParams.size());
  auto mem = ctx.Allocate(size, alignof(UnresolvedSpecializeExpr));
  return ::new(mem) UnresolvedSpecializeExpr(SubExpr, LAngleLoc,
                                             UnresolvedParams, RAngleLoc);
}

bool CaptureListEntry::isSimpleSelfCapture() const {
  auto &ctx = Var->getASTContext();

  if (Var->getName() != ctx.Id_self)
    return false;

  if (auto *attr = Var->getAttrs().getAttribute<ReferenceOwnershipAttr>())
    if (attr->get() == ReferenceOwnership::Weak)
      return false;

  if (Init->getPatternList().size() != 1)
    return false;

  auto *expr = Init->getInit(0);

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
                                         ClosureExpr *closureBody) {
  auto size = totalSizeToAlloc<CaptureListEntry>(captureList.size());
  auto mem = ctx.Allocate(size, alignof(CaptureListExpr));
  auto *expr = ::new(mem) CaptureListExpr(captureList, closureBody);

  for (auto capture : captureList)
    capture.Var->setParentCaptureList(expr);

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
  SourceLoc start = SourceLoc();
  SourceLoc end = SourceLoc();
  if (LParenLoc.isValid()) {
    start = LParenLoc;
  } else if (getNumElements() == 0) {
    return { SourceLoc(), SourceLoc() };
  } else {
    // Scan forward for the first valid source loc.
    for (Expr *expr : getElements()) {
      start = expr->getStartLoc();
      if (start.isValid()) {
        break;
      }
    }
  }
  
  if (hasAnyTrailingClosures() || RParenLoc.isInvalid()) {
    if (getNumElements() == 0) {
      return { SourceLoc(), SourceLoc() };
    } else {
      // Scan backwards for a valid source loc.
      bool hasSingleTrailingClosure = hasTrailingClosure();
      for (Expr *expr : llvm::reverse(getElements())) {
        // Default arguments are located at the start of their parent tuple, so
        // skip over them.
        if (isa<DefaultArgumentExpr>(expr))
          continue;

        SourceLoc newEnd = expr->getEndLoc();
        if (newEnd.isInvalid())
          continue;

        // There is a quirk with the backward scan logic for trailing
        // closures that can cause arguments to be flipped. If there is a
        // single trailing closure, only stop when the "end" point we hit comes
        // after the close parenthesis (if there is one).
        if (end.isInvalid() ||
            end.getOpaquePointerValue() < newEnd.getOpaquePointerValue()) {
          end = newEnd;
        }

        if (!hasSingleTrailingClosure || RParenLoc.isInvalid() ||
            RParenLoc.getOpaquePointerValue() < end.getOpaquePointerValue())
          break;
      }
    }
  } else {
    end = RParenLoc;
  }
  
  if (start.isValid() && end.isValid()) {
    return { start, end };
  } else {
    return { SourceLoc(), SourceLoc() };
  }
}

TupleExpr::TupleExpr(SourceLoc LParenLoc, SourceLoc RParenLoc,
                     ArrayRef<Expr *> SubExprs,
                     ArrayRef<Identifier> ElementNames, 
                     ArrayRef<SourceLoc> ElementNameLocs,
                     Optional<unsigned> FirstTrailingArgumentAt,
                     bool Implicit, Type Ty)
  : Expr(ExprKind::Tuple, Implicit, Ty),
    LParenLoc(LParenLoc), RParenLoc(RParenLoc),
    FirstTrailingArgumentAt(FirstTrailingArgumentAt) {
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
                             SourceLoc RParenLoc,
                             bool HasTrailingClosure,
                             bool Implicit, Type Ty) {
  Optional<unsigned> FirstTrailingArgumentAt =
      HasTrailingClosure ? SubExprs.size() - 1 : Optional<unsigned>();

  return create(ctx, LParenLoc, RParenLoc, SubExprs, ElementNames,
                ElementNameLocs, FirstTrailingArgumentAt, Implicit, Ty);
}

TupleExpr *TupleExpr::create(ASTContext &ctx,
                             SourceLoc LParenLoc,
                             SourceLoc RParenLoc,
                             ArrayRef<Expr *> SubExprs,
                             ArrayRef<Identifier> ElementNames,
                             ArrayRef<SourceLoc> ElementNameLocs,
                             Optional<unsigned> FirstTrailingArgumentAt,
                             bool Implicit, Type Ty) {
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
                             ElementNameLocs,
                             FirstTrailingArgumentAt, Implicit, Ty);
}

TupleExpr *TupleExpr::createEmpty(ASTContext &ctx, SourceLoc LParenLoc, 
                                  SourceLoc RParenLoc, bool Implicit) {
  return create(ctx, LParenLoc, RParenLoc, {}, {}, {},
                /*FirstTrailingArgumentAt=*/None, Implicit,
                TupleType::getEmpty(ctx));
}

TupleExpr *TupleExpr::createImplicit(ASTContext &ctx, ArrayRef<Expr *> SubExprs,
                                     ArrayRef<Identifier> ElementNames) {
  return create(ctx, SourceLoc(), SourceLoc(), SubExprs, ElementNames, {},
                /*FirstTrailingArgumentAt=*/None, /*Implicit=*/true, Type());
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

static ValueDecl *getCalledValue(Expr *E) {
  if (auto *DRE = dyn_cast<DeclRefExpr>(E))
    return DRE->getDecl();

  if (auto *OCRE = dyn_cast<OtherConstructorDeclRefExpr>(E))
    return OCRE->getDecl();

  // Look through SelfApplyExpr.
  if (auto *SAE = dyn_cast<SelfApplyExpr>(E))
    return SAE->getCalledValue();

  Expr *E2 = E->getValueProvidingExpr();
  if (E != E2)
    return getCalledValue(E2);

  return nullptr;
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

ValueDecl *ApplyExpr::getCalledValue() const {
  return ::getCalledValue(Fn);
}

SubscriptExpr::SubscriptExpr(Expr *base, Expr *index,
                             ArrayRef<Identifier> argLabels,
                             ArrayRef<SourceLoc> argLabelLocs,
                             bool hasTrailingClosure,
                             ConcreteDeclRef decl,
                             bool implicit, AccessSemantics semantics)
    : LookupExpr(ExprKind::Subscript, base, decl, implicit),
      Index(index) {
  Bits.SubscriptExpr.Semantics = (unsigned) semantics;
  Bits.SubscriptExpr.NumArgLabels = argLabels.size();
  Bits.SubscriptExpr.HasArgLabelLocs = !argLabelLocs.empty();
  Bits.SubscriptExpr.HasTrailingClosure = hasTrailingClosure;
  initializeCallArguments(argLabels, argLabelLocs);
}

SubscriptExpr *SubscriptExpr::create(ASTContext &ctx, Expr *base, Expr *index,
                                     ConcreteDeclRef decl, bool implicit,
                                     AccessSemantics semantics,
                                     llvm::function_ref<Type(Expr *)> getType) {
  // Inspect the argument to dig out the argument labels, their location, and
  // whether there is a trailing closure.
  SmallVector<Identifier, 4> argLabelsScratch;
  SmallVector<SourceLoc, 4> argLabelLocs;
  bool hasTrailingClosure = false;
  auto argLabels = getArgumentLabelsFromArgument(index, argLabelsScratch,
                                                 &argLabelLocs,
                                                 &hasTrailingClosure,
                                                 getType);

  size_t size = totalSizeToAlloc(argLabels, argLabelLocs);

  void *memory = ctx.Allocate(size, alignof(SubscriptExpr));
  return new (memory) SubscriptExpr(base, index, argLabels, argLabelLocs,
                                    hasTrailingClosure, decl, implicit,
                                    semantics);
}

SubscriptExpr *SubscriptExpr::create(ASTContext &ctx, Expr *base,
                                     SourceLoc lSquareLoc,
                                     ArrayRef<Expr *> indexArgs,
                                     ArrayRef<Identifier> indexArgLabels,
                                     ArrayRef<SourceLoc> indexArgLabelLocs,
                                     SourceLoc rSquareLoc,
                                     ArrayRef<TrailingClosure> trailingClosures,
                                     ConcreteDeclRef decl,
                                     bool implicit,
                                     AccessSemantics semantics) {
  SmallVector<Identifier, 4> indexArgLabelsScratch;
  SmallVector<SourceLoc, 4> indexArgLabelLocsScratch;
  Expr *index = packSingleArgument(ctx, lSquareLoc, indexArgs, indexArgLabels,
                                   indexArgLabelLocs, rSquareLoc,
                                   trailingClosures, implicit,
                                   indexArgLabelsScratch,
                                   indexArgLabelLocsScratch);

  size_t size = totalSizeToAlloc(indexArgLabels, indexArgLabelLocs);

  void *memory = ctx.Allocate(size, alignof(SubscriptExpr));
  return new (memory) SubscriptExpr(base, index, indexArgLabels,
                                    indexArgLabelLocs,
                                    trailingClosures.size() == 1,
                                    decl, implicit, semantics);
}

DynamicSubscriptExpr::DynamicSubscriptExpr(Expr *base, Expr *index,
                                           ArrayRef<Identifier> argLabels,
                                           ArrayRef<SourceLoc> argLabelLocs,
                                           bool hasTrailingClosure,
                                           ConcreteDeclRef member,
                                           bool implicit)
    : DynamicLookupExpr(ExprKind::DynamicSubscript, member, base),
      Index(index) {
  Bits.DynamicSubscriptExpr.NumArgLabels = argLabels.size();
  Bits.DynamicSubscriptExpr.HasArgLabelLocs = !argLabelLocs.empty();
  Bits.DynamicSubscriptExpr.HasTrailingClosure = hasTrailingClosure;
  initializeCallArguments(argLabels, argLabelLocs);
  if (implicit) setImplicit(implicit);
}

DynamicSubscriptExpr *
DynamicSubscriptExpr::create(ASTContext &ctx, Expr *base, Expr *index,
                             ConcreteDeclRef decl, bool implicit,
                             llvm::function_ref<Type(Expr *)> getType) {
  // Inspect the argument to dig out the argument labels, their location, and
  // whether there is a trailing closure.
  SmallVector<Identifier, 4> argLabelsScratch;
  SmallVector<SourceLoc, 4> argLabelLocs;
  bool hasTrailingClosure = false;
  auto argLabels = getArgumentLabelsFromArgument(index, argLabelsScratch,
                                                 &argLabelLocs,
                                                 &hasTrailingClosure,
                                                 getType);

  size_t size = totalSizeToAlloc(argLabels, argLabelLocs);

  void *memory = ctx.Allocate(size, alignof(DynamicSubscriptExpr));
  return new (memory) DynamicSubscriptExpr(base, index, argLabels, argLabelLocs,
                                           hasTrailingClosure, decl, implicit);
}

ArrayRef<Identifier> ApplyExpr::getArgumentLabels(
    SmallVectorImpl<Identifier> &scratch) const {
  // Unary operators and 'self' applications have a single, unlabeled argument.
  if (isa<PrefixUnaryExpr>(this) || isa<PostfixUnaryExpr>(this) ||
      isa<SelfApplyExpr>(this)) {
    scratch.clear();
    scratch.push_back(Identifier());
    return scratch;
  }

  // Binary operators have two unlabeled arguments.
  if (isa<BinaryExpr>(this)) {
    scratch.clear();
    scratch.reserve(2);
    scratch.push_back(Identifier());
    scratch.push_back(Identifier());
    return scratch;    
  }

  // For calls, get the argument labels directly.
  auto call = cast<CallExpr>(this);
  return call->getArgumentLabels();
}

bool ApplyExpr::hasTrailingClosure() const {
  if (auto call = dyn_cast<CallExpr>(this))
    return call->hasTrailingClosure();

  return false;
}

Optional<unsigned> ApplyExpr::getUnlabeledTrailingClosureIndex() const {
  if (auto call = dyn_cast<CallExpr>(this))
    return call->getUnlabeledTrailingClosureIndex();

  return None;
}

CallExpr::CallExpr(Expr *fn, Expr *arg, bool Implicit,
                   ArrayRef<Identifier> argLabels,
                   ArrayRef<SourceLoc> argLabelLocs,
                   bool hasTrailingClosure,
                   Type ty)
    : ApplyExpr(ExprKind::Call, fn, arg, Implicit, ty)
{
  Bits.CallExpr.NumArgLabels = argLabels.size();
  Bits.CallExpr.HasArgLabelLocs = !argLabelLocs.empty();
  Bits.CallExpr.HasTrailingClosure = hasTrailingClosure;
  initializeCallArguments(argLabels, argLabelLocs);

#ifndef NDEBUG
  Expr *calleeFn = fn->getSemanticsProvidingExpr();
  if (auto *calleeDRE = dyn_cast<DeclRefExpr>(calleeFn))
    assert(!calleeDRE->getDecl()->isInstanceMember());
#endif
}

CallExpr *CallExpr::create(ASTContext &ctx, Expr *fn, Expr *arg,
                           ArrayRef<Identifier> argLabels,
                           ArrayRef<SourceLoc> argLabelLocs,
                           bool hasTrailingClosure,
                           bool implicit, Type type,
                           llvm::function_ref<Type(Expr *)> getType) {
  SmallVector<Identifier, 4> argLabelsScratch;
  SmallVector<SourceLoc, 4> argLabelLocsScratch;
  if (argLabels.empty()) {
    // Inspect the argument to dig out the argument labels, their location, and
    // whether there is a trailing closure.
    argLabels = getArgumentLabelsFromArgument(arg, argLabelsScratch,
                                              &argLabelLocsScratch,
                                              &hasTrailingClosure,
                                              getType);
    argLabelLocs = argLabelLocsScratch;
  }

  size_t size = totalSizeToAlloc(argLabels, argLabelLocs);

  void *memory = ctx.Allocate(size, alignof(CallExpr));
  return new (memory) CallExpr(fn, arg, implicit, argLabels, argLabelLocs,
                               hasTrailingClosure, type);
}

CallExpr *CallExpr::create(ASTContext &ctx, Expr *fn, SourceLoc lParenLoc,
                           ArrayRef<Expr *> args,
                           ArrayRef<Identifier> argLabels,
                           ArrayRef<SourceLoc> argLabelLocs,
                           SourceLoc rParenLoc,
                           ArrayRef<TrailingClosure> trailingClosures,
                           bool implicit,
                           llvm::function_ref<Type(Expr *)> getType) {
  SmallVector<Identifier, 4> argLabelsScratch;
  SmallVector<SourceLoc, 4> argLabelLocsScratch;
  Expr *arg = packSingleArgument(ctx, lParenLoc, args, argLabels, argLabelLocs,
                                 rParenLoc,  trailingClosures, implicit,
                                 argLabelsScratch, argLabelLocsScratch,
                                 getType);

  size_t size = totalSizeToAlloc(argLabels, argLabelLocs);

  void *memory = ctx.Allocate(size, alignof(CallExpr));
  return new (memory) CallExpr(fn, arg, implicit, argLabels, argLabelLocs,
                               trailingClosures.size() == 1, Type());
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

  isChainToSuper = apply->getArg()->isSuperExpr();
  return otherCtorRef;
}

void AbstractClosureExpr::setParameterList(ParameterList *P) {
  parameterList = P;
  // Change the DeclContext of any parameters to be this closure.
  if (P)
    P->setDeclContextOfParamDecls(this);
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
  if (getType()->hasError())
    return false;
  
  return getType()->castTo<FunctionType>()->getExtInfo().isThrowing();
}

bool AbstractClosureExpr::isBodyAsync() const {
  if (getType()->hasError())
    return false;

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

#define FORWARD_SOURCE_LOCS_TO(CLASS, NODE) \
  SourceRange CLASS::getSourceRange() const {     \
    return (NODE)->getSourceRange();              \
  }                                               \
  SourceLoc CLASS::getStartLoc() const {          \
    return (NODE)->getStartLoc();                 \
  }                                               \
  SourceLoc CLASS::getEndLoc() const {            \
    return (NODE)->getEndLoc();                   \
  }                                               \
  SourceLoc CLASS::getLoc() const {               \
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

  switch (getThunkKind()) {
  case AutoClosureExpr::Kind::None:
  case AutoClosureExpr::Kind::AsyncLet:
    break;

  case AutoClosureExpr::Kind::SingleCurryThunk: {
    auto *body = getSingleExpressionBody();
    body = body->getSemanticsProvidingExpr();
    body = maybeUnwrapOpenExistential(body);
    body = maybeUnwrapOptionalEval(body);

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

      if (auto *outerCall = dyn_cast<ApplyExpr>(innerBody)) {
        if (auto *innerCall = dyn_cast<ApplyExpr>(outerCall->getFn())) {
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

  // Create a new list of components.
  SmallVector<ComponentIdentTypeRepr *, 2> Components;

  // The first component is the parent type.
  auto *ParentComp = new (C) SimpleIdentTypeRepr(ParentNameLoc,
                                                 Parent->createNameRef());
  ParentComp->setValue(Parent, nullptr);
  Components.push_back(ParentComp);

  // The second component is the member we just found.
  auto *NewComp = new (C) SimpleIdentTypeRepr(NameLoc,
                                              Decl->createNameRef());
  NewComp->setValue(Decl, nullptr);
  Components.push_back(NewComp);

  auto *NewTypeRepr = IdentTypeRepr::create(C, Components);
  return new (C) TypeExpr(NewTypeRepr);
}

TypeExpr *TypeExpr::createForMemberDecl(IdentTypeRepr *ParentTR,
                                        DeclNameLoc NameLoc,
                                        TypeDecl *Decl) {
  ASTContext &C = Decl->getASTContext();

  // Create a new list of components.
  SmallVector<ComponentIdentTypeRepr *, 2> Components;
  for (auto *Component : ParentTR->getComponentRange())
    Components.push_back(Component);

  assert(!Components.empty());

  // Add a new component for the member we just found.
  auto *NewComp = new (C) SimpleIdentTypeRepr(NameLoc, Decl->createNameRef());
  NewComp->setValue(Decl, nullptr);
  Components.push_back(NewComp);

  auto *NewTypeRepr = IdentTypeRepr::create(C, Components);
  return new (C) TypeExpr(NewTypeRepr);
}

TypeExpr *TypeExpr::createForSpecializedDecl(IdentTypeRepr *ParentTR,
                                             ArrayRef<TypeRepr*> Args,
                                             SourceRange AngleLocs,
                                             ASTContext &C) {
  // Create a new list of components.
  SmallVector<ComponentIdentTypeRepr *, 2> components;
  for (auto *component : ParentTR->getComponentRange()) {
    components.push_back(component);
  }

  auto *last = components.back();
  components.pop_back();

  if (isa<SimpleIdentTypeRepr>(last) &&
      last->getBoundDecl()) {
    if (isa<TypeAliasDecl>(last->getBoundDecl())) {
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
      for (auto *component : components) {
        auto *componentDecl = dyn_cast_or_null<GenericTypeDecl>(
          component->getBoundDecl());

        if (isa<SimpleIdentTypeRepr>(component) &&
            componentDecl &&
            componentDecl->isGeneric())
          return nullptr;
      }
    }

    auto *genericComp = GenericIdentTypeRepr::create(C,
      last->getNameLoc(), last->getNameRef(),
      Args, AngleLocs);
    genericComp->setValue(last->getBoundDecl(), last->getDeclContext());
    components.push_back(genericComp);

    auto *genericRepr = IdentTypeRepr::create(C, components);
    return new (C) TypeExpr(genericRepr);
  }

  return nullptr;
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

KeyPathExpr::KeyPathExpr(ASTContext &C, SourceLoc keywordLoc,
                         SourceLoc lParenLoc, ArrayRef<Component> components,
                         SourceLoc rParenLoc, bool isImplicit)
    : Expr(ExprKind::KeyPath, isImplicit), StartLoc(keywordLoc),
      LParenLoc(lParenLoc), EndLoc(rParenLoc),
      Components(C.AllocateUninitialized<Component>(components.size())) {
  // Copy components into the AST context.
  std::uninitialized_copy(components.begin(), components.end(),
                          Components.begin());

  Bits.KeyPathExpr.IsObjC = true;
}

void
KeyPathExpr::resolveComponents(ASTContext &C,
                          ArrayRef<KeyPathExpr::Component> resolvedComponents) {
  // Reallocate the components array if it needs to be.
  if (Components.size() < resolvedComponents.size()) {
    Components = C.Allocate<Component>(resolvedComponents.size());
    for (unsigned i : indices(Components)) {
      ::new ((void*)&Components[i]) Component{};
    }
  }
  
  for (unsigned i : indices(resolvedComponents)) {
    Components[i] = resolvedComponents[i];
  }
  Components = Components.slice(0, resolvedComponents.size());
}

KeyPathExpr::Component
KeyPathExpr::Component::forSubscript(ASTContext &ctx,
                             ConcreteDeclRef subscript,
                             SourceLoc lSquareLoc,
                             ArrayRef<Expr *> indexArgs,
                             ArrayRef<Identifier> indexArgLabels,
                             ArrayRef<SourceLoc> indexArgLabelLocs,
                             SourceLoc rSquareLoc,
                             ArrayRef<TrailingClosure> trailingClosures,
                             Type elementType,
                             ArrayRef<ProtocolConformanceRef> indexHashables) {
  SmallVector<Identifier, 4> indexArgLabelsScratch;
  SmallVector<SourceLoc, 4> indexArgLabelLocsScratch;
  Expr *index = packSingleArgument(ctx, lSquareLoc, indexArgs, indexArgLabels,
                                   indexArgLabelLocs, rSquareLoc,
                                   trailingClosures, /*implicit*/ false,
                                   indexArgLabelsScratch,
                                   indexArgLabelLocsScratch);
  return forSubscriptWithPrebuiltIndexExpr(subscript, index,
                                           indexArgLabels,
                                           elementType,
                                           lSquareLoc,
                                           indexHashables);
}

KeyPathExpr::Component
KeyPathExpr::Component::forUnresolvedSubscript(ASTContext &ctx,
                                         SourceLoc lSquareLoc,
                                         ArrayRef<Expr *> indexArgs,
                                         ArrayRef<Identifier> indexArgLabels,
                                         ArrayRef<SourceLoc> indexArgLabelLocs,
                                         SourceLoc rSquareLoc,
                                         ArrayRef<TrailingClosure> trailingClosures) {
  SmallVector<Identifier, 4> indexArgLabelsScratch;
  SmallVector<SourceLoc, 4> indexArgLabelLocsScratch;
  Expr *index = packSingleArgument(
      ctx, lSquareLoc, indexArgs, indexArgLabels, indexArgLabelLocs, rSquareLoc,
      trailingClosures, /*implicit*/ false,
      indexArgLabelsScratch, indexArgLabelLocsScratch);
  return forUnresolvedSubscriptWithPrebuiltIndexExpr(ctx, index, indexArgLabels,
                                                     lSquareLoc);
}

KeyPathExpr::Component::Component(ASTContext *ctxForCopyingLabels,
                     DeclNameOrRef decl,
                     Expr *indexExpr,
                     ArrayRef<Identifier> subscriptLabels,
                     ArrayRef<ProtocolConformanceRef> indexHashables,
                     Kind kind,
                     Type type,
                     SourceLoc loc)
    : Decl(decl), SubscriptIndexExpr(indexExpr), KindValue(kind),
      ComponentType(type), Loc(loc)
{
  assert(kind != Kind::TupleElement || subscriptLabels.empty());
  assert(subscriptLabels.size() == indexHashables.size()
         || indexHashables.empty());
  SubscriptLabelsData = subscriptLabels.data();
  SubscriptHashableConformancesData = indexHashables.empty()
    ? nullptr : indexHashables.data();
  SubscriptSize = subscriptLabels.size();
}

KeyPathExpr::Component
KeyPathExpr::Component::forSubscriptWithPrebuiltIndexExpr(
       ConcreteDeclRef subscript, Expr *index, ArrayRef<Identifier> labels,
       Type elementType, SourceLoc loc,
       ArrayRef<ProtocolConformanceRef> indexHashables) {
  return Component(&elementType->getASTContext(),
                   subscript, index, labels, indexHashables,
                   Kind::Subscript, elementType, loc);
}

void KeyPathExpr::Component::setSubscriptIndexHashableConformances(
    ArrayRef<ProtocolConformanceRef> hashables) {
  switch (getKind()) {
  case Kind::Subscript:
    assert(hashables.size() == SubscriptSize);
    SubscriptHashableConformancesData = getComponentType()->getASTContext()
      .AllocateCopy(hashables)
      .data();
    return;
    
  case Kind::UnresolvedSubscript:
  case Kind::Invalid:
  case Kind::OptionalChain:
  case Kind::OptionalWrap:
  case Kind::OptionalForce:
  case Kind::UnresolvedProperty:
  case Kind::Property:
  case Kind::Identity:
  case Kind::TupleElement:
  case Kind::DictionaryKey:
    llvm_unreachable("no hashable conformances for this kind");
  }
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
  if (Body) {
    assert(!Body->empty() &&
         Body->getFirstElement().isDecl(DeclKind::Var) &&
         "First element of Body should be a variable to init with the subExpr");
  }
}

VarDecl * TapExpr::getVar() const {
  return dyn_cast<VarDecl>(Body->getFirstElement().dyn_cast<Decl *>());
}

SourceLoc TapExpr::getEndLoc() const {
  // Include the body in the range, assuming the body follows the SubExpr.
  // Also, be (perhaps overly) defensive about null pointers & invalid
  // locations.
  if (auto *const b = getBody()) {
    const auto be = b->getEndLoc();
    if (be.isValid())
      return be;
  }
  if (auto *const se = getSubExpr())
    return se->getEndLoc();
  return SourceLoc();
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

