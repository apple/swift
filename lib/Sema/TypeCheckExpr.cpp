//===--- TypeCheckExpr.cpp - Type Checking for Expressions ----------------===//
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
// This file implements semantic analysis for expressions, analyzing an
// expression tree in post-order, bottom-up, from leaves up to the root.
//
//===----------------------------------------------------------------------===//

#include "TypeChecker.h"
#include "swift/AST/NameLookup.h"
#include "swift/AST/NameLookupRequests.h"
#include "swift/AST/OperatorNameLookup.h"
#include "swift/AST/Decl.h"
#include "swift/AST/Initializer.h"
#include "swift/AST/ParameterList.h"
#include "swift/AST/SourceFile.h"
#include "swift/AST/TypeCheckRequests.h"
#include "swift/Parse/Lexer.h"

using namespace swift;

//===----------------------------------------------------------------------===//
// Expression Semantic Analysis Routines
//===----------------------------------------------------------------------===//

static Type getArgListUniqueSugarType(ArgumentList *args, CanType resultTy) {
  Type uniqueSugarTy;
  for (auto arg : *args) {
    auto argTy = arg.getExpr()->getType();
    if (!argTy)
      return Type();

    if (argTy->getCanonicalType() != resultTy) {
      // If the argument is a metatype of what we're looking for, propagate
      // that.
      if (auto MTT = argTy->getAs<MetatypeType>())
        argTy = MTT->getInstanceType();

      if (argTy->getCanonicalType() != resultTy)
        return Type();
    }

    // If this type is parenthesized, remove the parens.  We don't want to
    // propagate parens from arguments to the result type.
    argTy = argTy->getWithoutParens();

    // If this is the first match against the sugar type we found, use it.
    if (!uniqueSugarTy) {
      uniqueSugarTy = argTy;
      continue;
    }

    // Make sure this argument's sugar is consistent with the sugar we
    // already found.
    if (argTy.getPointer() != uniqueSugarTy.getPointer())
      return Type();
  }
  return uniqueSugarTy;
}

/// If we can propagate type sugar from input arguments types to the result of
/// an apply, do so.
Expr *TypeChecker::substituteInputSugarTypeForResult(ApplyExpr *E) {
  if (!E->getType() || E->getType()->hasError())
    return E;

  /// Check to see if you have "x+y" (where x and y are type aliases) that match
  // the canonical result type.  If so, propagate the sugar.
  auto resultTy = E->getType();
  auto resultSugarTy = getArgListUniqueSugarType(E->getArgs(),
                                                 resultTy->getCanonicalType());
  if (resultSugarTy && resultTy->isCanonical()) {
    E->setType(resultSugarTy);
    return E;
  }

  // Otherwise check to see if this is a ConstructorRefExpr on a TypeExpr with
  // sugar on it.  If so, propagate the sugar to the curried result function
  // type.
  if (auto *CRCE = dyn_cast<ConstructorRefCallExpr>(E)) {
    if (auto *TE = dyn_cast<TypeExpr>(CRCE->getBase())) {
      auto resultSugar = TE->getInstanceType();

    // The result of this apply is "(args) -> T" where T is the type being
    // constructed.  Apply the sugar onto it.
    if (auto FT = E->getType()->getAs<FunctionType>())
      if (FT->getResult()->isEqual(resultSugar) && !resultSugar->isCanonical()){
        auto NFT = FunctionType::get(FT->getParams(), resultSugar,
                                     FT->getExtInfo());
        E->setType(NFT);
        return E;
      }
    }
  }
  return E;
}

static PrecedenceGroupDecl *lookupPrecedenceGroupForOperator(DeclContext *DC,
                                                             Identifier name,
                                                             SourceLoc loc) {
  auto result = DC->lookupInfixOperator(name);
  auto *op =
      loc.isValid() ? result.getSingleOrDiagnose(loc) : result.getSingle();
  return op ? op->getPrecedenceGroup() : nullptr;
}

PrecedenceGroupDecl *
TypeChecker::lookupPrecedenceGroupForInfixOperator(DeclContext *DC, Expr *E,
                                                   bool diagnose) {
  /// Look up the builtin precedence group with the given name.

  auto getBuiltinPrecedenceGroup = [&](DeclContext *DC, Identifier name,
                                       SourceLoc loc) -> PrecedenceGroupDecl * {
    auto groups = TypeChecker::lookupPrecedenceGroup(DC, name, loc);
    return loc.isValid() ? groups.getSingleOrDiagnose(loc, /*forBuiltin*/ true)
                         : groups.getSingle();
  };
  
  auto &Context = DC->getASTContext();
  if (auto *ternary = dyn_cast<TernaryExpr>(E)) {
    // Ternary has fixed precedence.
    return getBuiltinPrecedenceGroup(DC, Context.Id_TernaryPrecedence,
                                     diagnose ? ternary->getQuestionLoc()
                                              : SourceLoc());
  }

  if (auto assignExpr = dyn_cast<AssignExpr>(E)) {
    // Assignment has fixed precedence.
    return getBuiltinPrecedenceGroup(DC, Context.Id_AssignmentPrecedence,
                                     diagnose ? assignExpr->getEqualLoc()
                                              : SourceLoc());
  }

  if (auto castExpr = dyn_cast<ExplicitCastExpr>(E)) {
    // 'as' and 'is' casts have fixed precedence.
    return getBuiltinPrecedenceGroup(DC, Context.Id_CastingPrecedence,
                                     diagnose ? castExpr->getAsLoc()
                                              : SourceLoc());
  }

  if (auto *DRE = dyn_cast<DeclRefExpr>(E)) {
    Identifier name = DRE->getDecl()->getBaseIdentifier();
    return lookupPrecedenceGroupForOperator(
        DC, name, diagnose ? DRE->getLoc() : SourceLoc());
  }

  if (auto *OO = dyn_cast<OverloadedDeclRefExpr>(E)) {
    Identifier name = OO->getDecls()[0]->getBaseIdentifier();
    return lookupPrecedenceGroupForOperator(
        DC, name, diagnose ? OO->getLoc() : SourceLoc());
  }

  if (auto arrowExpr = dyn_cast<ArrowExpr>(E)) {
    return getBuiltinPrecedenceGroup(DC, Context.Id_FunctionArrowPrecedence,
                                     diagnose ? arrowExpr->getArrowLoc()
                                              : SourceLoc());
  }

  // An already-folded binary operator comes up for non-primary use cases
  // of this function.
  if (auto binaryExpr = dyn_cast<BinaryExpr>(E)) {
    return lookupPrecedenceGroupForInfixOperator(DC, binaryExpr->getFn(),
                                                 diagnose);
  }

  if (auto *DSCE = dyn_cast<DotSyntaxCallExpr>(E)) {
    return lookupPrecedenceGroupForInfixOperator(DC, DSCE->getFn(), diagnose);
  }

  if (auto *MRE = dyn_cast<MemberRefExpr>(E)) {
    Identifier name = MRE->getDecl().getDecl()->getBaseIdentifier();
    return lookupPrecedenceGroupForOperator(
        DC, name, diagnose ? MRE->getLoc() : SourceLoc());
  }

  // If E is already an ErrorExpr, then we've diagnosed it as invalid already,
  // otherwise emit an error.
  if (diagnose && !isa<ErrorExpr>(E))
    Context.Diags.diagnose(E->getLoc(), diag::unknown_binop);

  return nullptr;
}

/// Find LHS as if we append binary operator to existing pre-folded expression.
/// Returns found expression, or \c nullptr if the operator is not applicable.
///
/// For example, given '(== R (* A B))':
/// 'findLHS(DC, expr, "+")' returns '(* A B)'.
/// 'findLHS(DC, expr, "<<")' returns 'B'.
/// 'findLHS(DC, expr, '==')' returns nullptr.
Expr *TypeChecker::findLHS(DeclContext *DC, Expr *E, Identifier name) {
  auto right = lookupPrecedenceGroupForOperator(DC, name, E->getEndLoc());
  if (!right)
    return nullptr;

  while (true) {

    // Look through implicit conversions.
    if (auto ICE = dyn_cast<ImplicitConversionExpr>(E)) {
      E = ICE->getSyntacticSubExpr();
      continue;
    }
    if (auto ACE = dyn_cast<AutoClosureExpr>(E)) {
      E = ACE->getSingleExpressionBody();
      continue;
    }

    auto left = lookupPrecedenceGroupForInfixOperator(DC, E, /*diagnose=*/true);
    if (!left)
      // LHS is not binary expression.
      return E;
    switch (DC->getASTContext().associateInfixOperators(left, right)) {
      case swift::Associativity::None:
        return nullptr;
      case swift::Associativity::Left:
        return E;
      case swift::Associativity::Right:
        break;
    }
    // Find the RHS of the current binary expr.
    if (auto *assignExpr = dyn_cast<AssignExpr>(E)) {
      E = assignExpr->getSrc();
    } else if (auto *ternary = dyn_cast<TernaryExpr>(E)) {
      E = ternary->getElseExpr();
    } else if (auto *binaryExpr = dyn_cast<BinaryExpr>(E)) {
      E = binaryExpr->getRHS();
    } else {
      // E.g. 'fn() as Int << 2'.
      // In this case '<<' has higher precedence than 'as', but the LHS should
      // be 'fn() as Int' instead of 'Int'.
      return E;
    }
  }
}

// The way we compute isEndOfSequence relies on the assumption that
// the sequence-folding algorithm never recurses with a prefix of the
// entire sequence.
static Expr *makeBinOp(ASTContext &Ctx, Expr *Op, Expr *LHS, Expr *RHS,
                       PrecedenceGroupDecl *opPrecedence,
                       bool isEndOfSequence) {
  if (!LHS || !RHS)
    return nullptr;

  // If the left-hand-side is a 'try' or 'await', hoist it up turning
  // "(try x) + y" into try (x + y).
  if (auto *tryEval = dyn_cast<AnyTryExpr>(LHS)) {
    auto sub = makeBinOp(Ctx, Op, tryEval->getSubExpr(), RHS,
                         opPrecedence, isEndOfSequence);
    tryEval->setSubExpr(sub);
    return tryEval;
  }
  
  if (auto *await = dyn_cast<AwaitExpr>(LHS)) {
    auto sub = makeBinOp(Ctx, Op, await->getSubExpr(), RHS,
                         opPrecedence, isEndOfSequence);
    await->setSubExpr(sub);
    return await;
  }
  
  // If this is an assignment operator, and the left operand is an optional
  // evaluation, pull the operator into the chain.
  if (opPrecedence && opPrecedence->isAssignment()) {
    if (auto optEval = dyn_cast<OptionalEvaluationExpr>(LHS)) {
      auto sub = makeBinOp(Ctx, Op, optEval->getSubExpr(), RHS,
                           opPrecedence, isEndOfSequence);
      optEval->setSubExpr(sub);
      return optEval;
    }
  }

  // If the right operand is a try or await, it's an error unless the operator
  // is an assignment or conditional operator and there's nothing to
  // the right that didn't parse as part of the right operand.
  //
  // Generally, nothing to the right will fail to parse as part of the
  // right operand because there are no standard operators that have
  // lower precedence than assignment operators or the conditional
  // operator.
  //
  // We allow the right operand of the conditional operator to begin
  // with 'try' for consistency with the middle operand.  This allows:
  //   x ? try foo() : try bar()
  // but not:
  //   x ? try foo() : try bar() $#! 1
  // assuming $#! is some crazy operator with lower precedence
  // than the conditional operator.
  if (isa<AnyTryExpr>(RHS) || isa<AwaitExpr>(RHS)) {
    // If you change this, also change TRY_KIND_SELECT in diagnostics.
    enum class TryKindForDiagnostics : unsigned {
      Try,
      ForceTry,
      OptionalTry,
      Await
    };
    TryKindForDiagnostics tryKind;
    switch (RHS->getKind()) {
    case ExprKind::Try:
      tryKind = TryKindForDiagnostics::Try;
      break;
    case ExprKind::ForceTry:
      tryKind = TryKindForDiagnostics::ForceTry;
      break;
    case ExprKind::OptionalTry:
      tryKind = TryKindForDiagnostics::OptionalTry;
      break;
    case ExprKind::Await:
      tryKind = TryKindForDiagnostics::Await;
      break;
    default:
      llvm_unreachable("unknown try-like expression");
    }

    if (isa<TernaryExpr>(Op) ||
        (opPrecedence && opPrecedence->isAssignment())) {
      if (!isEndOfSequence) {
        if (isa<TernaryExpr>(Op)) {
          Ctx.Diags.diagnose(RHS->getStartLoc(), diag::try_if_rhs_noncovering,
                             static_cast<unsigned>(tryKind));
        } else {
          Ctx.Diags.diagnose(RHS->getStartLoc(),
                             diag::try_assign_rhs_noncovering,
                             static_cast<unsigned>(tryKind));
        }
      }
    } else {
      Ctx.Diags.diagnose(RHS->getStartLoc(), diag::try_rhs,
                         static_cast<unsigned>(tryKind));
    }
  }

  if (auto *ternary = dyn_cast<TernaryExpr>(Op)) {
    // Resolve the ternary expression.
    if (!Ctx.CompletionCallback) {
      // In code completion we might call preCheckExpression twice - once for
      // the first pass and once for the second pass. This is fine since
      // preCheckExpression idempotent.
      assert(!ternary->isFolded() && "already folded if expr in sequence?!");
    }
    ternary->setCondExpr(LHS);
    ternary->setElseExpr(RHS);
    return ternary;
  }

  if (auto *assign = dyn_cast<AssignExpr>(Op)) {
    // Resolve the assignment expression.
    if (!Ctx.CompletionCallback) {
      // In code completion we might call preCheckExpression twice - once for
      // the first pass and once for the second pass. This is fine since
      // preCheckExpression idempotent.
      assert(!assign->isFolded() && "already folded assign expr in sequence?!");
    }
    assign->setDest(LHS);
    assign->setSrc(RHS);
    return assign;
  }
  
  if (auto *as = dyn_cast<ExplicitCastExpr>(Op)) {
    // Resolve the 'as' or 'is' expression.
    if (!Ctx.CompletionCallback) {
      // In code completion we might call preCheckExpression twice - once for
      // the first pass and once for the second pass. This is fine since
      // preCheckExpression idempotent.
      assert(!as->isFolded() && "already folded 'as' expr in sequence?!");
    }
    assert(RHS == as && "'as' with non-type RHS?!");
    as->setSubExpr(LHS);    
    return as;
  }

  if (auto *arrow = dyn_cast<ArrowExpr>(Op)) {
    // Resolve the '->' expression.
    if (!Ctx.CompletionCallback) {
      // In code completion we might call preCheckExpression twice - once for
      // the first pass and once for the second pass. This is fine since
      // preCheckExpression idempotent.
      assert(!arrow->isFolded() && "already folded '->' expr in sequence?!");
    }
    arrow->setArgsExpr(LHS);
    arrow->setResultExpr(RHS);
    return arrow;
  }
  
  // Build the operation.
  return BinaryExpr::create(Ctx, LHS, Op, RHS, Op->isImplicit());
}

namespace {
  class PrecedenceBound {
    llvm::PointerIntPair<PrecedenceGroupDecl*,1,bool> GroupAndIsStrict;
  public:
    PrecedenceBound() {}
    PrecedenceBound(PrecedenceGroupDecl *decl, bool isStrict)
      : GroupAndIsStrict(decl, isStrict) {}

    bool shouldConsider(PrecedenceGroupDecl *group) {
      auto storedGroup = GroupAndIsStrict.getPointer();
      if (!storedGroup) return true;
      if (!group) return false;
      if (storedGroup == group) return !GroupAndIsStrict.getInt();
      return group->getASTContext().associateInfixOperators(group, storedGroup)
               != Associativity::Right;
    }
  };
} // end anonymous namespace

/// foldSequence - Take a sequence of expressions and fold a prefix of
/// it into a tree of BinaryExprs using precedence parsing.
static Expr *foldSequence(DeclContext *DC,
                          Expr *LHS,
                          ArrayRef<Expr*> &S,
                          PrecedenceBound precedenceBound) {
  // Invariant: S is even-sized.
  // Invariant: All elements at even indices are operator references.
  assert(!S.empty());
  assert((S.size() & 1) == 0);
  
  struct Op {
    Expr *op;
    PrecedenceGroupDecl *precedence;
    
    explicit operator bool() const { return op != nullptr; }
  };
  
  /// Get the operator, if appropriate to this pass.
  auto getNextOperator = [&]() -> Op {
    Expr *op = S[0];

    // If the operator's precedence is lower than the minimum, stop here.
    auto opPrecedence = TypeChecker::lookupPrecedenceGroupForInfixOperator(
        DC, op, /*diagnose=*/true);
    if (!precedenceBound.shouldConsider(opPrecedence))
      return {nullptr, nullptr};
    return {op, opPrecedence};
  };

  // Extract out the first operator.
  Op op1 = getNextOperator();
  if (!op1) return LHS;
  
  // We will definitely be consuming at least one operator.
  // Pull out the prospective RHS and slice off the first two elements.
  Expr *RHS = S[1];
  S = S.slice(2);

  auto &Ctx = DC->getASTContext();
  while (!S.empty()) {
    assert((S.size() & 1) == 0);
    assert(precedenceBound.shouldConsider(op1.precedence));

    // If the operator is a cast operator, the RHS can't extend past the type
    // that's part of the cast production.
    if (isa<ExplicitCastExpr>(op1.op)) {
      LHS = makeBinOp(Ctx, op1.op, LHS, RHS, op1.precedence, S.empty());
      op1 = getNextOperator();
      if (!op1) return LHS;
      RHS = S[1];
      S = S.slice(2);
      continue;
    }
    
    // Pull out the next binary operator.
    Op op2{S[0], TypeChecker::lookupPrecedenceGroupForInfixOperator(
                     DC, S[0], /*diagnose=*/true)};

    // If the second operator's precedence is lower than the
    // precedence bound, break out of the loop.
    if (!precedenceBound.shouldConsider(op2.precedence)) break;

    // If we're missing precedence info for either operator, treat them
    // as non-associative.
    Associativity associativity;
    if (!op1.precedence || !op2.precedence) {
      associativity = Associativity::None;
    } else {
      associativity =
        Ctx.associateInfixOperators(op1.precedence, op2.precedence);
    }

    // Apply left-associativity immediately by folding the first two
    // operands.
    if (associativity == Associativity::Left) {
      LHS = makeBinOp(Ctx, op1.op, LHS, RHS, op1.precedence, S.empty());
      op1 = op2;
      RHS = S[1];
      S = S.slice(2);
      continue;
    }

    // If the first operator's precedence is lower than the second
    // operator's precedence, recursively fold all such
    // higher-precedence operators starting from this point, then
    // repeat.
    if (associativity == Associativity::Right &&
        op1.precedence != op2.precedence) {
      RHS = foldSequence(DC, RHS, S,
                         PrecedenceBound(op1.precedence, /*strict*/ true));
      continue;
    }

    // Apply right-associativity by recursively folding operators
    // starting from this point, then immediately folding the LHS and RHS.
    if (associativity == Associativity::Right) {
      RHS = foldSequence(DC, RHS, S,
                         PrecedenceBound(op1.precedence, /*strict*/ false));
      LHS = makeBinOp(Ctx, op1.op, LHS, RHS, op1.precedence, S.empty());

      // If we've drained the entire sequence, we're done.
      if (S.empty()) return LHS;

      // Otherwise, start all over with our new LHS.
      return foldSequence(DC, LHS, S, precedenceBound);
    }

    // If we ended up here, it's because we're either:
    //   - missing precedence groups,
    //   - have unordered precedence groups, or
    //   - have the same precedence group with no associativity.
    assert(associativity == Associativity::None);

    // Don't diagnose if we're missing a precedence group; this is
    // an invalid-code situation.
    if (!op1.precedence || !op2.precedence) {
      // do nothing
    } else if (op1.precedence == op2.precedence) {
      assert(op1.precedence->isNonAssociative());
      // FIXME: QoI ranges
      Ctx.Diags.diagnose(op1.op->getLoc(),
                         diag::non_associative_adjacent_operators,
                         op1.precedence->getName())
        .highlight(SourceRange(op2.op->getLoc(), op2.op->getLoc()));

    } else {
      Ctx.Diags.diagnose(op1.op->getLoc(),
                         diag::unordered_adjacent_operators,
                         op1.precedence->getName(), op2.precedence->getName())
        .highlight(SourceRange(op2.op->getLoc(), op2.op->getLoc()));      
    }
    
    // Recover by arbitrarily binding the first two.
    LHS = makeBinOp(Ctx, op1.op, LHS, RHS, op1.precedence, S.empty());
    return foldSequence(DC, LHS, S, precedenceBound);
  }

  // Fold LHS and RHS together and declare completion.
  return makeBinOp(Ctx, op1.op, LHS, RHS, op1.precedence, S.empty());
}

bool TypeChecker::requireOptionalIntrinsics(ASTContext &ctx, SourceLoc loc) {
  if (ctx.hasOptionalIntrinsics())
    return false;

  ctx.Diags.diagnose(loc, diag::optional_intrinsics_not_found);
  return true;
}

bool TypeChecker::requirePointerArgumentIntrinsics(ASTContext &ctx,
                                                   SourceLoc loc) {
  if (ctx.hasPointerArgumentIntrinsics())
    return false;

  ctx.Diags.diagnose(loc, diag::pointer_argument_intrinsics_not_found);
  return true;
}

bool TypeChecker::requireArrayLiteralIntrinsics(ASTContext &ctx,
                                                SourceLoc loc) {
  if (ctx.hasArrayLiteralIntrinsics())
    return false;

  ctx.Diags.diagnose(loc, diag::array_literal_intrinsics_not_found);
  return true;
}

Expr *TypeChecker::buildCheckedRefExpr(VarDecl *value, DeclContext *UseDC,
                                       DeclNameLoc loc, bool Implicit) {
  auto type = constraints::ConstraintSystem::getUnopenedTypeOfReference(
      value, Type(), UseDC,
      [&](VarDecl *var) -> Type { return value->getType(); });
  auto semantics = value->getAccessSemanticsFromContext(UseDC,
                                                       /*isAccessOnSelf*/false);
  return new (value->getASTContext())
      DeclRefExpr(value, loc, Implicit, semantics, type);
}

Expr *TypeChecker::buildRefExpr(ArrayRef<ValueDecl *> Decls,
                                DeclContext *UseDC, DeclNameLoc NameLoc,
                                bool Implicit, FunctionRefKind functionRefKind) {
  assert(!Decls.empty() && "Must have at least one declaration");

  auto &Context = UseDC->getASTContext();

  if (Decls.size() == 1) {
    return new (Context) DeclRefExpr(Decls[0], NameLoc, Implicit,
                                     AccessSemantics::Ordinary);
  }

  Decls = Context.AllocateCopy(Decls);
  auto result = new (Context) OverloadedDeclRefExpr(Decls, NameLoc, 
                                                    functionRefKind,
                                                    Implicit);
  return result;
}

static Type lookupDefaultLiteralType(const DeclContext *dc,
                                     StringRef name) {
  auto &ctx = dc->getASTContext();
  DeclNameRef nameRef(ctx.getIdentifier(name));
  auto lookup = TypeChecker::lookupUnqualified(
      dc->getModuleScopeContext(),
      nameRef, SourceLoc(),
      defaultUnqualifiedLookupOptions | NameLookupFlags::ExcludeMacroExpansions
  );
  TypeDecl *TD = lookup.getSingleTypeResult();
  if (!TD)
    return Type();
  
  if (TD->isInvalid())
    return Type();

  if (auto *NTD = dyn_cast<NominalTypeDecl>(TD))
    return NTD->getDeclaredType();
  return cast<TypeAliasDecl>(TD)->getDeclaredInterfaceType();
}

static llvm::Optional<KnownProtocolKind>
getKnownProtocolKindIfAny(const ProtocolDecl *protocol) {
#define EXPRESSIBLE_BY_LITERAL_PROTOCOL_WITH_NAME(Id, _, __, ___)              \
  if (protocol == TypeChecker::getProtocol(protocol->getASTContext(),          \
                                           SourceLoc(),                        \
                                           KnownProtocolKind::Id))             \
    return KnownProtocolKind::Id;
#include "swift/AST/KnownProtocols.def"
#undef EXPRESSIBLE_BY_LITERAL_PROTOCOL_WITH_NAME

  return llvm::None;
}

Type TypeChecker::getDefaultType(ProtocolDecl *protocol, DeclContext *dc) {
  if (auto knownProtocolKindIfAny = getKnownProtocolKindIfAny(protocol)) {
    return evaluateOrDefault(
        protocol->getASTContext().evaluator,
        DefaultTypeRequest{knownProtocolKindIfAny.value(), dc}, nullptr);
  }
  return Type();
}

static std::pair<const char *, bool> lookupDefaultTypeInfoForKnownProtocol(
    const KnownProtocolKind knownProtocolKind) {
  switch (knownProtocolKind) {
#define EXPRESSIBLE_BY_LITERAL_PROTOCOL_WITH_NAME(Id, Name, typeName,          \
                                                  performLocalLookup)          \
  case KnownProtocolKind::Id:                                                  \
    return {typeName, performLocalLookup};
#include "swift/AST/KnownProtocols.def"
#undef EXPRESSIBLE_BY_LITERAL_PROTOCOL_WITH_NAME
  default:
    return {nullptr, false};
  }
}

Type
swift::DefaultTypeRequest::evaluate(Evaluator &evaluator,
                                    KnownProtocolKind knownProtocolKind,
                                    const DeclContext *dc) const {
  const char *name;
  bool performLocalLookup;
  std::tie(name, performLocalLookup) =
      lookupDefaultTypeInfoForKnownProtocol(knownProtocolKind);
  if (!name)
    return nullptr;

  Type type;
  if (performLocalLookup)
    type = lookupDefaultLiteralType(dc, name);

  if (!type)
    type = lookupDefaultLiteralType(TypeChecker::getStdlibModule(dc), name);

  // Strip off one level of sugar; we don't actually want to print
  // the name of the typealias itself anywhere.
  if (type) {
    if (auto boundTypeAlias = dyn_cast<TypeAliasType>(type.getPointer()))
      type = boundTypeAlias->getSinglyDesugaredType();
  }
  return type;
}

Expr *TypeChecker::foldSequence(SequenceExpr *expr, DeclContext *dc) {
  ArrayRef<Expr*> Elts = expr->getElements();
  assert(Elts.size() > 1 && "inadequate number of elements in sequence");
  assert((Elts.size() & 1) == 1 && "even number of elements in sequence");

  Expr *LHS = Elts[0];
  Elts = Elts.slice(1);

  Expr *Result = ::foldSequence(dc, LHS, Elts, PrecedenceBound());
  assert(Elts.empty());

  return Result;
}

static Expr *synthesizeCallerSideDefault(const ParamDecl *param,
                                         SourceLoc loc) {
  auto &ctx = param->getASTContext();
  switch (param->getDefaultArgumentKind()) {
#define MAGIC_IDENTIFIER(NAME, STRING, SYNTAX_KIND) \
  case DefaultArgumentKind::NAME: \
    return new (ctx) \
        MagicIdentifierLiteralExpr(MagicIdentifierLiteralExpr::NAME, loc, \
                                   /*implicit=*/true);
#include "swift/AST/MagicIdentifierKinds.def"

  case DefaultArgumentKind::NilLiteral:
    return new (ctx) NilLiteralExpr(loc, /*Implicit=*/true);
    break;

  case DefaultArgumentKind::EmptyArray: {
    auto *initExpr = ArrayExpr::create(ctx, loc, {}, {}, loc);
    initExpr->setImplicit();
    return initExpr;
  }
  case DefaultArgumentKind::EmptyDictionary: {
    auto *initExpr = DictionaryExpr::create(ctx, loc, {}, {}, loc);
    initExpr->setImplicit();
    return initExpr;
  }
  case DefaultArgumentKind::None:
  case DefaultArgumentKind::Normal:
  case DefaultArgumentKind::Inherited:
  case DefaultArgumentKind::StoredProperty:
    llvm_unreachable("Not a caller-side default");
  }
  llvm_unreachable("Unhandled case in switch");
}

Expr *CallerSideDefaultArgExprRequest::evaluate(
    Evaluator &evaluator, DefaultArgumentExpr *defaultExpr) const {
  auto *param = defaultExpr->getParamDecl();
  auto paramTy = defaultExpr->getType();

  // Re-create the default argument using the location info of the call site.
  auto *initExpr =
      synthesizeCallerSideDefault(param, defaultExpr->getLoc());
  auto *dc = defaultExpr->ContextOrCallerSideExpr.get<DeclContext *>();
  assert(dc && "Expected a DeclContext before type-checking caller-side arg");

  auto &ctx = param->getASTContext();
  DiagnosticTransaction transaction(ctx.Diags);
  if (!TypeChecker::typeCheckParameterDefault(initExpr, dc, paramTy,
                                              param->isAutoClosure())) {
    if (param->hasDefaultExpr()) {
      // HACK: If we were unable to type-check the default argument in context,
      // then retry by type-checking it within the parameter decl, which should
      // also fail. This will present the user with a better error message and
      // allow us to avoid diagnosing on each call site.
      transaction.abort();
      (void)param->getTypeCheckedDefaultExpr();
      assert(ctx.Diags.hadAnyError());
    }
    return new (ctx) ErrorExpr(initExpr->getSourceRange(), paramTy);
  }
  return initExpr;
}

bool ClosureHasExplicitResultRequest::evaluate(Evaluator &evaluator,
                                               ClosureExpr *closure) const {
  // A walker that looks for 'return' statements that aren't
  // nested within closures or nested declarations.
  class FindReturns : public ASTWalker {
    bool FoundResultReturn = false;
    bool FoundNoResultReturn = false;

    MacroWalking getMacroWalkingBehavior() const override {
      return MacroWalking::Expansion;
    }

    PreWalkResult<Expr *> walkToExprPre(Expr *expr) override {
      return Action::SkipChildren(expr);
    }

    PreWalkAction walkToDeclPre(Decl *decl) override {
      return Action::SkipChildren();
    }

    PreWalkResult<Stmt *> walkToStmtPre(Stmt *stmt) override {
      // Record return statements.
      if (auto ret = dyn_cast<ReturnStmt>(stmt)) {
        if (ret->isImplicit())
          return Action::Continue(stmt);

        // If it has a result, remember that we saw one, but keep
        // traversing in case there's a no-result return somewhere.
        if (ret->hasResult()) {
          FoundResultReturn = true;

          // Otherwise, stop traversing.
        } else {
          FoundNoResultReturn = true;
          return Action::Stop();
        }
      }
      return Action::Continue(stmt);
    }

  public:
    bool hasResult() const { return !FoundNoResultReturn && FoundResultReturn; }
  };

  auto body = closure->getBody();
  if (!body)
    return false;

  FindReturns finder;
  body->walk(finder);
  return finder.hasResult();
}
