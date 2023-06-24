//===--- CSGen.cpp - Constraint Generator ---------------------------------===//
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
// This file implements constraint generation for the type checker.
//
//===----------------------------------------------------------------------===//
#include "TypeCheckConcurrency.h"
#include "TypeCheckDecl.h"
#include "TypeCheckMacros.h"
#include "TypeCheckRegex.h"
#include "TypeCheckType.h"
#include "TypeChecker.h"
#include "swift/AST/ASTVisitor.h"
#include "swift/AST/ASTWalker.h"
#include "swift/AST/Expr.h"
#include "swift/AST/GenericSignature.h"
#include "swift/AST/GenericEnvironment.h"
#include "swift/AST/ParameterList.h"
#include "swift/AST/PrettyStackTrace.h"
#include "swift/AST/SubstitutionMap.h"
#include "swift/AST/TypeCheckRequests.h"
#include "swift/Sema/ConstraintGraph.h"
#include "swift/Sema/ConstraintSystem.h"
#include "swift/Sema/IDETypeChecking.h"
#include "swift/Subsystems.h"
#include "llvm/ADT/APInt.h"
#include "llvm/ADT/SetVector.h"
#include "llvm/ADT/SmallSet.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/ADT/StringSwitch.h"
#include <utility>

using namespace swift;
using namespace swift::constraints;

static bool isArithmeticOperatorDecl(ValueDecl *vd) {
  return vd && vd->getBaseIdentifier().isArithmeticOperator();
}

static bool mergeRepresentativeEquivalenceClasses(ConstraintSystem &CS,
                                                  TypeVariableType* tyvar1,
                                                  TypeVariableType* tyvar2) {
  if (tyvar1 && tyvar2) {
    auto rep1 = CS.getRepresentative(tyvar1);
    auto rep2 = CS.getRepresentative(tyvar2);

    if (rep1 != rep2) {
      auto fixedType2 = CS.getFixedType(rep2);

      // If the there exists fixed type associated with the second
      // type variable, and we simply merge two types together it would
      // mean that portion of the constraint graph previously associated
      // with that (second) variable is going to be disconnected from its
      // new equivalence class, which is going to lead to incorrect solutions,
      // so we need to make sure to re-bind fixed to the new representative.
      if (fixedType2) {
        CS.addConstraint(ConstraintKind::Bind, fixedType2, rep1,
                         rep1->getImpl().getLocator());
      }

      CS.mergeEquivalenceClasses(rep1, rep2, /*updateWorkList*/ false);
      return true;
    }
  }

  return false;
}

namespace {
  
  /// Internal struct for tracking information about types within a series
  /// of "linked" expressions. (Such as a chain of binary operator invocations.)
  struct LinkedTypeInfo {
    bool hasLiteral = false;

    llvm::SmallSet<TypeBase*, 16> collectedTypes;
    llvm::SmallVector<BinaryExpr *, 4> binaryExprs;
  };

  /// Walks an expression sub-tree, and collects information about expressions
  /// whose types are mutually dependent upon one another.
  class LinkedExprCollector : public ASTWalker {
    
    llvm::SmallVectorImpl<Expr*> &LinkedExprs;

  public:
    LinkedExprCollector(llvm::SmallVectorImpl<Expr *> &linkedExprs)
        : LinkedExprs(linkedExprs) {}

    MacroWalking getMacroWalkingBehavior() const override {
      return MacroWalking::Arguments;
    }

    PreWalkResult<Expr *> walkToExprPre(Expr *expr) override {
      if (isa<ClosureExpr>(expr))
        return Action::SkipChildren(expr);

      // Store top-level binary exprs for further analysis.
      if (isa<BinaryExpr>(expr) ||
          
          // Literal exprs are contextually typed, so store them off as well.
          isa<LiteralExpr>(expr) ||

          // We'd like to look at the elements of arrays and dictionaries.
          isa<ArrayExpr>(expr) ||
          isa<DictionaryExpr>(expr) ||

          // assignment expression can involve anonymous closure parameters
          // as source and destination, so it's beneficial for diagnostics if
          // we look at the assignment.
          isa<AssignExpr>(expr)) {
        LinkedExprs.push_back(expr);
        return Action::SkipChildren(expr);
      }
      
      return Action::Continue(expr);
    }
    
    PostWalkResult<Expr *> walkToExprPost(Expr *expr) override {
      return Action::Continue(expr);
    }
    
    /// Ignore statements.
    PreWalkResult<Stmt *> walkToStmtPre(Stmt *stmt) override {
      return Action::SkipChildren(stmt);
    }
    
    /// Ignore declarations.
    PreWalkAction walkToDeclPre(Decl *decl) override {
      return Action::SkipChildren();
    }

    /// Ignore patterns.
    PreWalkResult<Pattern *> walkToPatternPre(Pattern *pat) override {
      return Action::SkipChildren(pat);
    }

    /// Ignore types.
    PreWalkAction walkToTypeReprPre(TypeRepr *T) override {
      return Action::SkipChildren();
    }
  };
  
  /// Given a collection of "linked" expressions, analyzes them for
  /// commonalities regarding their types. This will help us compute a
  /// "best common type" from the expression types.
  class LinkedExprAnalyzer : public ASTWalker {
    
    LinkedTypeInfo &LTI;
    ConstraintSystem &CS;
    
  public:
    
    LinkedExprAnalyzer(LinkedTypeInfo &lti, ConstraintSystem &cs) :
        LTI(lti), CS(cs) {}

    MacroWalking getMacroWalkingBehavior() const override {
      return MacroWalking::Arguments;
    }

    PreWalkResult<Expr *> walkToExprPre(Expr *expr) override {
      if (isa<LiteralExpr>(expr)) {
        LTI.hasLiteral = true;
        return Action::SkipChildren(expr);
      }

      if (isa<CollectionExpr>(expr)) {
        return Action::Continue(expr);
      }
      
      if (auto UDE = dyn_cast<UnresolvedDotExpr>(expr)) {
        
        if (CS.hasType(UDE))
          LTI.collectedTypes.insert(CS.getType(UDE).getPointer());
        
        // Don't recurse into the base expression.
        return Action::SkipChildren(expr);
      }


      if (isa<ClosureExpr>(expr)) {
        return Action::SkipChildren(expr);
      }

      if (auto FVE = dyn_cast<ForceValueExpr>(expr)) {
        LTI.collectedTypes.insert(CS.getType(FVE).getPointer());
        return Action::SkipChildren(expr);
      }

      if (auto DRE = dyn_cast<DeclRefExpr>(expr)) {
        if (auto varDecl = dyn_cast<VarDecl>(DRE->getDecl())) {
          if (CS.hasType(DRE)) {
            LTI.collectedTypes.insert(CS.getType(DRE).getPointer());
          }
          return Action::SkipChildren(expr);
        } 
      }             

      // In the case of a function application, we would have already captured
      // the return type during constraint generation, so there's no use in
      // looking any further.
      if (isa<ApplyExpr>(expr) &&
          !(isa<BinaryExpr>(expr) || isa<PrefixUnaryExpr>(expr) ||
            isa<PostfixUnaryExpr>(expr))) {      
        return Action::SkipChildren(expr);
      }

      if (auto *binaryExpr = dyn_cast<BinaryExpr>(expr)) {
        LTI.binaryExprs.push_back(binaryExpr);
      }  
      
      if (auto favoredType = CS.getFavoredType(expr)) {
        LTI.collectedTypes.insert(favoredType);

        return Action::SkipChildren(expr);
      }

      // Optimize branches of a conditional expression separately.
      if (auto IE = dyn_cast<TernaryExpr>(expr)) {
        CS.optimizeConstraints(IE->getCondExpr());
        CS.optimizeConstraints(IE->getThenExpr());
        CS.optimizeConstraints(IE->getElseExpr());
        return Action::SkipChildren(expr);
      }      

      // For exprs of a tuple, avoid favoring. (We need to allow for cases like
      // (Int, Int32).)
      if (isa<TupleExpr>(expr)) {
        return Action::SkipChildren(expr);
      }

      // Coercion exprs have a rigid type, so there's no use in gathering info
      // about them.
      if (auto *coercion = dyn_cast<CoerceExpr>(expr)) {
        // Let's not collect information about types initialized by
        // coercions just like we don't for regular initializer calls,
        // because that might lead to overly eager type variable merging.
        if (!coercion->isLiteralInit())
          LTI.collectedTypes.insert(CS.getType(expr).getPointer());
        return Action::SkipChildren(expr);
      }

      // Don't walk into subscript expressions - to do so would risk factoring
      // the index expression into edge contraction. (We don't want to do this
      // if the index expression is a literal type that differs from the return
      // type of the subscript operation.)
      if (isa<SubscriptExpr>(expr) || isa<DynamicLookupExpr>(expr)) {
        return Action::SkipChildren(expr);
      }
      
      // Don't walk into unresolved member expressions - we avoid merging type
      // variables inside UnresolvedMemberExpr and those outside, since they
      // should be allowed to behave independently in CS.
      if (isa<UnresolvedMemberExpr>(expr)) {
        return Action::SkipChildren(expr);
      }

      return Action::Continue(expr);
    }
    
    /// Ignore statements.
    PreWalkResult<Stmt *> walkToStmtPre(Stmt *stmt) override {
      return Action::SkipChildren(stmt);
    }
    
    /// Ignore declarations.
    PreWalkAction walkToDeclPre(Decl *decl) override {
      return Action::SkipChildren();
    }

    /// Ignore patterns.
    PreWalkResult<Pattern *> walkToPatternPre(Pattern *pat) override {
      return Action::SkipChildren(pat);
    }

    /// Ignore types.
    PreWalkAction walkToTypeReprPre(TypeRepr *T) override {
      return Action::SkipChildren();
    }
  };
  
  /// For a given expression, given information that is global to the
  /// expression, attempt to derive a favored type for it.
  void computeFavoredTypeForExpr(Expr *expr, ConstraintSystem &CS) {
    LinkedTypeInfo lti;

    expr->walk(LinkedExprAnalyzer(lti, CS));

    // Check whether we can proceed with favoring.
    if (llvm::any_of(lti.binaryExprs, [](const BinaryExpr *op) {
          auto *ODRE = dyn_cast<OverloadedDeclRefExpr>(op->getFn());
          if (!ODRE)
            return false;

          // Attempting to favor based on operand types is wrong for
          // nil-coalescing operator.
          auto identifier = ODRE->getDecls().front()->getBaseIdentifier();
          return identifier.isNilCoalescingOperator();
        })) {
      return;
    }

    if (lti.collectedTypes.size() == 1) {
      // TODO: Compute the BCT.

      // It's only useful to favor the type instead of
      // binding it directly to arguments/result types,
      // which means in case it has been miscalculated
      // solver can still make progress.
      auto favoredTy = (*lti.collectedTypes.begin())->getWithoutSpecifierType();
      CS.setFavoredType(expr, favoredTy.getPointer());

      // If we have a chain of identical binop expressions with homogeneous
      // argument types, we can directly simplify the associated constraint
      // graph.
      auto simplifyBinOpExprTyVars = [&]() {
        // Don't attempt to do linking if there are
        // literals intermingled with other inferred types.
        if (lti.hasLiteral)
          return;

        for (auto binExp1 : lti.binaryExprs) {
          for (auto binExp2 : lti.binaryExprs) {
            if (binExp1 == binExp2)
              continue;

            auto fnTy1 = CS.getType(binExp1)->getAs<TypeVariableType>();
            auto fnTy2 = CS.getType(binExp2)->getAs<TypeVariableType>();

            if (!(fnTy1 && fnTy2))
              return;

            auto ODR1 = dyn_cast<OverloadedDeclRefExpr>(binExp1->getFn());
            auto ODR2 = dyn_cast<OverloadedDeclRefExpr>(binExp2->getFn());

            if (!(ODR1 && ODR2))
              return;

            // TODO: We currently limit this optimization to known arithmetic
            // operators, but we should be able to broaden this out to
            // logical operators as well.
            if (!isArithmeticOperatorDecl(ODR1->getDecls()[0]))
              return;

            if (ODR1->getDecls()[0]->getBaseName() !=
                ODR2->getDecls()[0]->getBaseName())
              return;

            // All things equal, we can merge the tyvars for the function
            // types.
            auto rep1 = CS.getRepresentative(fnTy1);
            auto rep2 = CS.getRepresentative(fnTy2);

            if (rep1 != rep2) {
              CS.mergeEquivalenceClasses(rep1, rep2,
                                         /*updateWorkList*/ false);
            }

            auto odTy1 = CS.getType(ODR1)->getAs<TypeVariableType>();
            auto odTy2 = CS.getType(ODR2)->getAs<TypeVariableType>();

            if (odTy1 && odTy2) {
              auto odRep1 = CS.getRepresentative(odTy1);
              auto odRep2 = CS.getRepresentative(odTy2);

              // Since we'll be choosing the same overload, we can merge
              // the overload tyvar as well.
              if (odRep1 != odRep2)
                CS.mergeEquivalenceClasses(odRep1, odRep2,
                                           /*updateWorkList*/ false);
            }
          }
        }
      };

      simplifyBinOpExprTyVars();
    }
  }

  /// Determine whether the given parameter type and argument should be
  /// "favored" because they match exactly.
  bool isFavoredParamAndArg(ConstraintSystem &CS, Type paramTy, Type argTy,
                            Type otherArgTy = Type()) {
    // Determine the argument type.
    argTy = argTy->getWithoutSpecifierType();

    // Do the types match exactly?
    if (paramTy->isEqual(argTy))
      return true;

    // Don't favor narrowing conversions.
    if (argTy->isDouble() && paramTy->isCGFloat())
      return false;

    llvm::SmallSetVector<ProtocolDecl *, 2> literalProtos;
    if (auto argTypeVar = argTy->getAs<TypeVariableType>()) {
      auto constraints = CS.getConstraintGraph().gatherConstraints(
          argTypeVar, ConstraintGraph::GatheringKind::EquivalenceClass,
          [](Constraint *constraint) {
            return constraint->getKind() == ConstraintKind::LiteralConformsTo;
          });

      for (auto constraint : constraints) {
        literalProtos.insert(constraint->getProtocol());
      }
    }

    // Dig out the second argument type.
    if (otherArgTy)
      otherArgTy = otherArgTy->getWithoutSpecifierType();

    for (auto literalProto : literalProtos) {
      // If there is another, concrete argument, check whether it's type
      // conforms to the literal protocol and test against it directly.
      // This helps to avoid 'widening' the favored type to the default type for
      // the literal.
      if (otherArgTy && otherArgTy->getAnyNominal()) {
        if (otherArgTy->isEqual(paramTy) &&
            TypeChecker::conformsToProtocol(
                otherArgTy, literalProto, CS.DC->getParentModule())) {
          return true;
        }
      } else if (Type defaultType =
                     TypeChecker::getDefaultType(literalProto, CS.DC)) {
        // If there is a default type for the literal protocol, check whether
        // it is the same as the parameter type.
        // Check whether there is a default type to compare against.
        if (paramTy->isEqual(defaultType) ||
            (defaultType->isDouble() && paramTy->isCGFloat()))
          return true;
      }
    }

    return false;
  }

  /// Favor certain overloads in a call based on some basic analysis
  /// of the overload set and call arguments.
  ///
  /// \param expr The application.
  /// \param isFavored Determine whether the given overload is favored, passing
  /// it the "effective" overload type when it's being called.
  /// \param mustConsider If provided, a function to detect the presence of
  /// overloads which inhibit any overload from being favored.
  void favorCallOverloads(ApplyExpr *expr,
                          ConstraintSystem &CS,
                          llvm::function_ref<bool(ValueDecl *, Type)> isFavored,
                          std::function<bool(ValueDecl *)>
                              mustConsider = nullptr) {
    // Find the type variable associated with the function, if any.
    auto tyvarType = CS.getType(expr->getFn())->getAs<TypeVariableType>();
    if (!tyvarType || CS.getFixedType(tyvarType))
      return;
    
    // This type variable is only currently associated with the function
    // being applied, and the only constraint attached to it should
    // be the disjunction constraint for the overload group.
    auto disjunction = CS.getUnboundBindOverloadDisjunction(tyvarType);
    if (!disjunction)
      return;
    
    // Find the favored constraints and mark them.
    SmallVector<Constraint *, 4> newlyFavoredConstraints;
    unsigned numFavoredConstraints = 0;
    Constraint *firstFavored = nullptr;
    for (auto constraint : disjunction->getNestedConstraints()) {
      auto *decl = constraint->getOverloadChoice().getDeclOrNull();
      if (!decl)
        continue;

      if (mustConsider && mustConsider(decl)) {
        // Roll back any constraints we favored.
        for (auto favored : newlyFavoredConstraints)
          favored->setFavored(false);

        return;
      }

      Type overloadType = CS.getEffectiveOverloadType(
          constraint->getLocator(), constraint->getOverloadChoice(),
          /*allowMembers=*/true, CS.DC);
      if (!overloadType)
        continue;

      if (!CS.isDeclUnavailable(decl, constraint->getLocator()) &&
          !decl->getAttrs().hasAttribute<DisfavoredOverloadAttr>() &&
          isFavored(decl, overloadType)) {
        // If we might need to roll back the favored constraints, keep
        // track of those we are favoring.
        if (mustConsider && !constraint->isFavored())
          newlyFavoredConstraints.push_back(constraint);

        constraint->setFavored();
        ++numFavoredConstraints;
        if (!firstFavored)
          firstFavored = constraint;
      }
    }

    // If there was one favored constraint, set the favored type based on its
    // result type.
    if (numFavoredConstraints == 1) {
      auto overloadChoice = firstFavored->getOverloadChoice();
      auto overloadType = CS.getEffectiveOverloadType(
          firstFavored->getLocator(), overloadChoice, /*allowMembers=*/true,
          CS.DC);
      auto resultType = overloadType->castTo<AnyFunctionType>()->getResult();
      if (!resultType->hasTypeParameter())
        CS.setFavoredType(expr, resultType.getPointer());
    }
  }
  
  /// Return a pair, containing the total parameter count of a function, coupled
  /// with the number of non-default parameters.
  std::pair<size_t, size_t> getParamCount(ValueDecl *VD) {
    auto fTy = VD->getInterfaceType()->castTo<AnyFunctionType>();
    
    size_t nOperands = fTy->getParams().size();
    size_t nNoDefault = 0;
    
    if (auto AFD = dyn_cast<AbstractFunctionDecl>(VD)) {
      assert(!AFD->hasImplicitSelfDecl());
      for (auto param : *AFD->getParameters()) {
        if (!param->isDefaultArgument())
          ++nNoDefault;
      }
    } else {
      nNoDefault = nOperands;
    }
    
    return { nOperands, nNoDefault };
  }

  bool hasContextuallyFavorableResultType(AnyFunctionType *choice,
                                          Type contextualTy) {
    // No restrictions of what result could be.
    if (!contextualTy)
      return true;

    auto resultTy = choice->getResult();
    // Result type of the call matches expected contextual type.
    return contextualTy->isEqual(resultTy);
  }

  /// Favor unary operator constraints where we have exact matches
  /// for the operand and contextual type.
  void favorMatchingUnaryOperators(ApplyExpr *expr,
                                   ConstraintSystem &CS) {
    auto *unaryArg = expr->getArgs()->getUnaryExpr();
    assert(unaryArg);

    // Determine whether the given declaration is favored.
    auto isFavoredDecl = [&](ValueDecl *value, Type type) -> bool {
      auto fnTy = type->getAs<AnyFunctionType>();
      if (!fnTy)
        return false;

      auto params = fnTy->getParams();
      if (params.size() != 1)
        return false;

      auto paramTy = params[0].getPlainType();
      auto argTy = CS.getType(unaryArg);

      // There are no CGFloat overloads on some of the unary operators, so
      // in order to preserve current behavior, let's not favor overloads
      // which would result in conversion from CGFloat to Double; otherwise
      // it would lead to ambiguities.
      if (argTy->isCGFloat() && paramTy->isDouble())
        return false;

      return isFavoredParamAndArg(CS, paramTy, argTy) &&
             hasContextuallyFavorableResultType(
                 fnTy,
                 CS.getContextualType(expr, /*forConstraint=*/false));
    };

    favorCallOverloads(expr, CS, isFavoredDecl);
  }
  
  void favorMatchingOverloadExprs(ApplyExpr *expr,
                                  ConstraintSystem &CS) {
    // Find the argument type.
    size_t nArgs = expr->getArgs()->size();
    auto fnExpr = expr->getFn();
    
    // Check to ensure that we have an OverloadedDeclRef, and that we're not
    // favoring multiple overload constraints. (Otherwise, in this case
    // favoring is useless.
    if (auto ODR = dyn_cast<OverloadedDeclRefExpr>(fnExpr)) {
      bool haveMultipleApplicableOverloads = false;
      
      for (auto VD : ODR->getDecls()) {
        if (VD->getInterfaceType()->is<AnyFunctionType>()) {
          auto nParams = getParamCount(VD);
          
          if (nArgs == nParams.first) {
            if (haveMultipleApplicableOverloads) {
              return;
            } else {
              haveMultipleApplicableOverloads = true;
            }
          }
        }
      }
      
      // Determine whether the given declaration is favored.
      auto isFavoredDecl = [&](ValueDecl *value, Type type) -> bool {
        // We want to consider all options for calls that might contain the code
        // completion location, as missing arguments after the completion
        // location are valid (since it might be that they just haven't been
        // written yet).
        if (CS.isForCodeCompletion())
          return false;

        if (!type->is<AnyFunctionType>())
          return false;

        auto paramCount = getParamCount(value);
        
        return nArgs == paramCount.first ||
               nArgs == paramCount.second;
      };
      
      favorCallOverloads(expr, CS, isFavoredDecl);
    }

    // We only currently perform favoring for unary args.
    auto *unaryArg = expr->getArgs()->getUnlabeledUnaryExpr();
    if (!unaryArg)
      return;

    if (auto favoredTy = CS.getFavoredType(unaryArg)) {
      // Determine whether the given declaration is favored.
      auto isFavoredDecl = [&](ValueDecl *value, Type type) -> bool {
        auto fnTy = type->getAs<AnyFunctionType>();
        if (!fnTy || fnTy->getParams().size() != 1)
          return false;

        return favoredTy->isEqual(fnTy->getParams()[0].getPlainType());
      };

      // This is a hack to ensure we always consider the protocol requirement
      // itself when calling something that has a default implementation in an
      // extension. Otherwise, the extension method might be favored if we're
      // inside an extension context, since any archetypes in the parameter
      // list could match exactly.
      auto mustConsider = [&](ValueDecl *value) -> bool {
        return isa<ProtocolDecl>(value->getDeclContext());
      };

      favorCallOverloads(expr, CS, isFavoredDecl, mustConsider);
    }
  }
  
  /// Favor binary operator constraints where we have exact matches
  /// for the operands and contextual type.
  void favorMatchingBinaryOperators(ApplyExpr *expr, ConstraintSystem &CS) {
    // If we're generating constraints for a binary operator application,
    // there are two special situations to consider:
    //  1. If the type checker has any newly created functions with the
    //     operator's name. If it does, the overloads were created after the
    //     associated overloaded id expression was created, and we'll need to
    //     add a new disjunction constraint for the new set of overloads.
    //  2. If any component argument expressions (nested or otherwise) are
    //     literals, we can favor operator overloads whose argument types are
    //     identical to the literal type, or whose return types are identical
    //     to any contextual type associated with the application expression.
    
    // Find the argument types.
    auto *args = expr->getArgs();
    auto *lhs = args->getExpr(0);
    auto *rhs = args->getExpr(1);

    auto firstArgTy = CS.getType(lhs)->getWithoutParens();
    auto secondArgTy = CS.getType(rhs)->getWithoutParens();

    auto isOptionalWithMatchingObjectType = [](Type optional,
                                               Type object) -> bool {
      if (auto objTy = optional->getRValueType()->getOptionalObjectType())
        return objTy->getRValueType()->isEqual(object->getRValueType());

      return false;
    };

    auto isPotentialForcingOpportunity = [&](Type first, Type second) -> bool {
      return isOptionalWithMatchingObjectType(first, second) ||
             isOptionalWithMatchingObjectType(second, first);
    };

    // Determine whether the given declaration is favored.
    auto isFavoredDecl = [&](ValueDecl *value, Type type) -> bool {
      auto fnTy = type->getAs<AnyFunctionType>();
      if (!fnTy)
        return false;

      auto firstFavoredTy = CS.getFavoredType(lhs);
      auto secondFavoredTy = CS.getFavoredType(rhs);
      
      auto favoredExprTy = CS.getFavoredType(expr);
      
      if (isArithmeticOperatorDecl(value)) {
        // If the parent has been favored on the way down, propagate that
        // information to its children.
        // TODO: This is only valid for arithmetic expressions.
        if (!firstFavoredTy) {
          CS.setFavoredType(lhs, favoredExprTy);
          firstFavoredTy = favoredExprTy;
        }
        
        if (!secondFavoredTy) {
          CS.setFavoredType(rhs, favoredExprTy);
          secondFavoredTy = favoredExprTy;
        }
      }
      
      auto params = fnTy->getParams();
      if (params.size() != 2)
        return false;

      auto firstParamTy = params[0].getOldType();
      auto secondParamTy = params[1].getOldType();

      auto contextualTy = CS.getContextualType(expr, /*forConstraint=*/false);

      // Avoid favoring overloads that would require narrowing conversion
      // to match the arguments.
      {
        if (firstArgTy->isDouble() && firstParamTy->isCGFloat())
          return false;

        if (secondArgTy->isDouble() && secondParamTy->isCGFloat())
          return false;
      }

      return (isFavoredParamAndArg(CS, firstParamTy, firstArgTy, secondArgTy) ||
              isFavoredParamAndArg(CS, secondParamTy, secondArgTy,
                                   firstArgTy)) &&
             firstParamTy->isEqual(secondParamTy) &&
             !isPotentialForcingOpportunity(firstArgTy, secondArgTy) &&
             hasContextuallyFavorableResultType(fnTy, contextualTy);
    };
    
    favorCallOverloads(expr, CS, isFavoredDecl);
  }

  /// If \p expr is a call and that call contains the code completion token,
  /// add the expressions of all arguments after the code completion token to
  /// \p ignoredArguments.
  /// Otherwise, returns an empty vector.
  /// Assumes that we are solving for code completion.
  void getArgumentsAfterCodeCompletionToken(
      Expr *expr, ConstraintSystem &CS,
      SmallVectorImpl<Expr *> &ignoredArguments) {
    assert(CS.isForCodeCompletion());

    /// Don't ignore the rhs argument if the code completion token is the lhs of
    /// an operator call. Main use case is the implicit `<complete> ~= $match`
    /// call created for pattern matching, in which we need to type-check
    /// `$match` to get a contextual type for `<complete>`
    if (isa<BinaryExpr>(expr)) {
      return;
    }

    auto args = expr->getArgs();
    auto argInfo = getCompletionArgInfo(expr, CS);
    if (!args || !argInfo) {
      return;
    }

    for (auto argIndex : indices(*args)) {
      if (argInfo->isBefore(argIndex)) {
        ignoredArguments.push_back(args->get(argIndex).getExpr());
      }
    }
  }

  class ConstraintOptimizer : public ASTWalker {
    ConstraintSystem &CS;
    
  public:
    
    ConstraintOptimizer(ConstraintSystem &cs) :
      CS(cs) {}

    MacroWalking getMacroWalkingBehavior() const override {
      return MacroWalking::Arguments;
    }

    PreWalkResult<Expr *> walkToExprPre(Expr *expr) override {
      if (CS.isArgumentIgnoredForCodeCompletion(expr)) {
        return Action::SkipChildren(expr);
      }
      
      if (auto applyExpr = dyn_cast<ApplyExpr>(expr)) {
        if (isa<PrefixUnaryExpr>(applyExpr) ||
            isa<PostfixUnaryExpr>(applyExpr)) {
          favorMatchingUnaryOperators(applyExpr, CS);
        } else if (isa<BinaryExpr>(applyExpr)) {
          favorMatchingBinaryOperators(applyExpr, CS);
        } else {
          favorMatchingOverloadExprs(applyExpr, CS);
        }
      }
      
      // If the paren expr has a favored type, and the subExpr doesn't,
      // propagate downwards. Otherwise, propagate upwards.
      if (auto parenExpr = dyn_cast<ParenExpr>(expr)) {
        if (!CS.getFavoredType(parenExpr->getSubExpr())) {
          CS.setFavoredType(parenExpr->getSubExpr(),
                            CS.getFavoredType(parenExpr));
        } else if (!CS.getFavoredType(parenExpr)) {
          CS.setFavoredType(parenExpr,
                            CS.getFavoredType(parenExpr->getSubExpr()));
        }
      }

      if (isa<ClosureExpr>(expr))
        return Action::SkipChildren(expr);

      return Action::Continue(expr);
    }
    
    PostWalkResult<Expr *> walkToExprPost(Expr *expr) override {
      return Action::Continue(expr);
    }
    
    /// Ignore statements.
    PreWalkResult<Stmt *> walkToStmtPre(Stmt *stmt) override {
      return Action::SkipChildren(stmt);
    }
    
    /// Ignore declarations.
    PreWalkAction walkToDeclPre(Decl *decl) override {
      return Action::SkipChildren();
    }
  };
} // end anonymous namespace

namespace {

  class ConstraintGenerator : public ExprVisitor<ConstraintGenerator, Type> {
    ConstraintSystem &CS;
    DeclContext *CurDC;
    ConstraintSystemPhase CurrPhase;

    /// A map from each UnresolvedMemberExpr to the respective (implicit) base
    /// found during our walk.
    llvm::MapVector<UnresolvedMemberExpr *, Type> UnresolvedBaseTypes;

    /// A stack of pack expansions that can open pack elements.
    llvm::SmallVector<PackExpansionExpr *, 2> PackElementEnvironments;

    /// Returns false and emits the specified diagnostic if the member reference
    /// base is a nil literal. Returns true otherwise.
    bool isValidBaseOfMemberRef(Expr *base, Diag<> diagnostic) {
      if (auto nilLiteral = dyn_cast<NilLiteralExpr>(base)) {
        CS.getASTContext().Diags.diagnose(nilLiteral->getLoc(), diagnostic);
        return false;
      }
      return true;
    }

    /// Retrieves a matching set of function params for an argument list.
    void getMatchingParams(ArgumentList *argList,
                           SmallVectorImpl<AnyFunctionType::Param> &result) {
      for (auto arg : *argList) {
        ParameterTypeFlags flags;
        auto ty = CS.getType(arg.getExpr());
        if (arg.isInOut()) {
          ty = ty->getInOutObjectType();
          flags = flags.withInOut(true);
        }
        if (arg.isConst()) {
          flags = flags.withCompileTimeConst(true);
        }
        result.emplace_back(ty, arg.getLabel(), flags);
      }
    }

    /// If the provided type is a tuple, decomposes it into a matching set of
    /// function params. Otherwise produces a single parameter of the type,
    /// stripping a ParenType if needed.
    void decomposeTuple(Type ty,
                        SmallVectorImpl<AnyFunctionType::Param> &result) {
      switch (ty->getKind()) {
      case TypeKind::Tuple: {
        auto tupleTy = cast<TupleType>(ty.getPointer());
        for (auto &elt : tupleTy->getElements())
          result.emplace_back(elt.getType(), elt.getName());
        return;
      }
      case TypeKind::Paren: {
        auto pty = cast<ParenType>(ty.getPointer());
        result.emplace_back(pty->getUnderlyingType(), Identifier());
        return;
      }
      default:
        result.emplace_back(ty, Identifier());
      }
    }

    /// Add constraints for a reference to a named member of the given
    /// base type, and return the type of such a reference.
    Type addMemberRefConstraints(Expr *expr, Expr *base, DeclNameRef name,
                                 FunctionRefKind functionRefKind,
                                 ArrayRef<ValueDecl *> outerAlternatives) {
      // The base must have a member of the given name, such that accessing
      // that member through the base returns a value convertible to the type
      // of this expression.
      auto baseTy = CS.getType(base);
      if (isa<ErrorExpr>(base)) {
        return CS.createTypeVariable(
            CS.getConstraintLocator(expr, ConstraintLocator::Member),
            TVO_CanBindToHole);
      }
      unsigned options = (TVO_CanBindToLValue |
                          TVO_CanBindToNoEscape);
      if (!PackElementEnvironments.empty())
        options |= TVO_CanBindToPack;

      auto tv = CS.createTypeVariable(
                  CS.getConstraintLocator(expr, ConstraintLocator::Member),
                  options);
      SmallVector<OverloadChoice, 4> outerChoices;
      for (auto decl : outerAlternatives) {
        outerChoices.push_back(OverloadChoice(Type(), decl, functionRefKind));
      }
      CS.addValueMemberConstraint(
          baseTy, name, tv, CurDC, functionRefKind, outerChoices,
          CS.getConstraintLocator(expr, ConstraintLocator::Member));
      return tv;
    }

    /// Add constraints for a reference to a specific member of the given
    /// base type, and return the type of such a reference.
    Type addMemberRefConstraints(Expr *expr, Expr *base, ValueDecl *decl,
                                 FunctionRefKind functionRefKind) {
      // If we're referring to an invalid declaration, fail.
      if (!decl)
        return nullptr;
      
      if (decl->isInvalid())
        return nullptr;

      auto memberLocator =
        CS.getConstraintLocator(expr, ConstraintLocator::Member);
      auto tv = CS.createTypeVariable(memberLocator,
                                      TVO_CanBindToLValue | TVO_CanBindToNoEscape);

      OverloadChoice choice =
          OverloadChoice(CS.getType(base), decl, functionRefKind);

      auto locator = CS.getConstraintLocator(expr, ConstraintLocator::Member);
      CS.addBindOverloadConstraint(tv, choice, locator, CurDC);
      return tv;
    }

    /// Add constraints for a subscript operation.
    Type addSubscriptConstraints(
        Expr *anchor, Type baseTy, ValueDecl *declOrNull, ArgumentList *argList,
        ConstraintLocator *locator = nullptr,
        SmallVectorImpl<TypeVariableType *> *addedTypeVars = nullptr) {
      // Locators used in this expression.
      if (locator == nullptr)
        locator = CS.getConstraintLocator(anchor);

      auto fnLocator =
        CS.getConstraintLocator(locator,
                                ConstraintLocator::ApplyFunction);
      auto memberLocator =
        CS.getConstraintLocator(locator,
                                ConstraintLocator::SubscriptMember);
      auto resultLocator =
        CS.getConstraintLocator(locator,
                                ConstraintLocator::FunctionResult);

      CS.associateArgumentList(memberLocator, argList);

      Type outputTy;

      // For an integer subscript expression on an array slice type, instead of
      // introducing a new type variable we can easily obtain the element type.
      if (isa<SubscriptExpr>(anchor)) {

        auto isLValueBase = false;
        auto baseObjTy = baseTy;
        if (baseObjTy->is<LValueType>()) {
          isLValueBase = true;
          baseObjTy = baseObjTy->getWithoutSpecifierType();
        }

        if (baseObjTy->isArrayType()) {

          if (auto arraySliceTy = 
                dyn_cast<ArraySliceType>(baseObjTy.getPointer())) {
            baseObjTy = arraySliceTy->getDesugaredType();
          }

          if (argList->isUnlabeledUnary() &&
              isa<IntegerLiteralExpr>(argList->getExpr(0))) {

            outputTy = baseObjTy->getAs<BoundGenericType>()->getGenericArgs()[0];
            
            if (isLValueBase)
              outputTy = LValueType::get(outputTy);
          }
        } else if (auto dictTy = CS.isDictionaryType(baseObjTy)) {
          auto keyTy = dictTy->first;
          auto valueTy = dictTy->second;

          if (argList->isUnlabeledUnary()) {
            auto argTy = CS.getType(argList->getExpr(0));
            if (isFavoredParamAndArg(CS, keyTy, argTy)) {
              outputTy = OptionalType::get(valueTy);
              if (isLValueBase)
                outputTy = LValueType::get(outputTy);
            }
          }
        }
      }
      
      if (outputTy.isNull()) {
        outputTy = CS.createTypeVariable(resultLocator,
                                         TVO_CanBindToLValue | TVO_CanBindToNoEscape);
        if (addedTypeVars)
          addedTypeVars->push_back(outputTy->castTo<TypeVariableType>());
      }

      // FIXME: This can only happen when diagnostics successfully type-checked
      // sub-expression of the subscript and mutated AST, but under normal
      // circumstances subscript should never have InOutExpr as a direct child
      // until type checking is complete and expression is re-written.
      // Proper fix for such situation requires preventing diagnostics from
      // re-writing AST after successful type checking of the sub-expressions.
      if (auto inoutTy = baseTy->getAs<InOutType>()) {
        baseTy = LValueType::get(inoutTy->getObjectType());
      }

      // Add the member constraint for a subscript declaration.
      // FIXME: weak name!
      auto memberTy = CS.createTypeVariable(
          memberLocator, TVO_CanBindToLValue | TVO_CanBindToNoEscape);
      if (addedTypeVars)
        addedTypeVars->push_back(memberTy);

      // FIXME: synthesizeMaterializeForSet() wants to statically dispatch to
      // a known subscript here. This might be cleaner if we split off a new
      // UnresolvedSubscriptExpr from SubscriptExpr.
      if (auto decl = declOrNull) {
        OverloadChoice choice =
            OverloadChoice(baseTy, decl, FunctionRefKind::DoubleApply);
        CS.addBindOverloadConstraint(memberTy, choice, memberLocator,
                                     CurDC);
      } else {
        CS.addValueMemberConstraint(baseTy, DeclNameRef::createSubscript(),
                                    memberTy, CurDC,
                                    FunctionRefKind::DoubleApply,
                                    /*outerAlternatives=*/{},
                                    memberLocator);
      }

      SmallVector<AnyFunctionType::Param, 8> params;
      getMatchingParams(argList, params);

      // Add the constraint that the index expression's type be convertible
      // to the input type of the subscript operator.
      CS.addConstraint(ConstraintKind::ApplicableFunction,
                       FunctionType::get(params, outputTy),
                       memberTy,
                       fnLocator);

      Type fixedOutputType =
          CS.getFixedTypeRecursive(outputTy, /*wantRValue=*/false);
      if (!fixedOutputType->isTypeVariableOrMember()) {
        CS.setFavoredType(anchor, fixedOutputType.getPointer());
        outputTy = fixedOutputType;
      }

      return outputTy;
    }

    Type openPackElement(Type packType, ConstraintLocator *locator) {
      // If 'each t' is written outside of a pack expansion expression, allow the
      // type to bind to a hole. The invalid pack reference will be diagnosed when
      // attempting to bind the type variable for the underlying pack reference to
      // a pack type without TVO_CanBindToPack.
      if (PackElementEnvironments.empty()) {
        return CS.createTypeVariable(locator,
                                     TVO_CanBindToHole | TVO_CanBindToNoEscape);
      }

      // The type of a PackElementExpr is the opened pack element archetype
      // of the pack reference.
      OpenPackElementType openPackElement(CS, locator,
                                          PackElementEnvironments.back());
      return openPackElement(packType, /*packRepr*/ nullptr);
    }

  public:
    ConstraintGenerator(ConstraintSystem &CS, DeclContext *DC)
        : CS(CS), CurDC(DC ? DC : CS.DC), CurrPhase(CS.getPhase()) {
      // Although constraint system is initialized in `constraint
      // generation` phase, we have to set it here manually because e.g.
      // result builders could generate constraints for its body
      // in the middle of the solving.
      CS.setPhase(ConstraintSystemPhase::ConstraintGeneration);
    }

    virtual ~ConstraintGenerator() {
      CS.setPhase(CurrPhase);
    }

    ConstraintSystem &getConstraintSystem() const { return CS; }

    void addPackElementEnvironment(PackExpansionExpr *expr) {
      PackElementEnvironments.push_back(expr);
    }

    virtual Type visitErrorExpr(ErrorExpr *E) {
      CS.recordFix(
          IgnoreInvalidASTNode::create(CS, CS.getConstraintLocator(E)));

      return CS.createTypeVariable(CS.getConstraintLocator(E),
                                   TVO_CanBindToHole);
    }

    virtual Type visitCodeCompletionExpr(CodeCompletionExpr *E) {
      CS.Options |= ConstraintSystemFlags::SuppressDiagnostics;
      auto locator = CS.getConstraintLocator(E);
      return CS.createTypeVariable(locator, TVO_CanBindToLValue |
                                                TVO_CanBindToNoEscape |
                                                TVO_CanBindToHole);
    }

    Type visitNilLiteralExpr(NilLiteralExpr *expr) {
      auto literalTy = visitLiteralExpr(expr);
      // Allow `nil` to be a hole so we can diagnose it via a fix
      // if it turns out that there is no contextual information.
      if (auto *typeVar = literalTy->getAs<TypeVariableType>())
        CS.recordPotentialHole(typeVar);

      return literalTy;
    }

    Type visitFloatLiteralExpr(FloatLiteralExpr *expr) {
      auto &ctx = CS.getASTContext();
      // Get the _MaxBuiltinFloatType decl, or look for it if it's not cached.
      auto maxFloatTypeDecl = ctx.get_MaxBuiltinFloatTypeDecl();

      if (!maxFloatTypeDecl ||
          !maxFloatTypeDecl->getDeclaredInterfaceType()->is<BuiltinFloatType>()) {
        ctx.Diags.diagnose(expr->getLoc(), diag::no_MaxBuiltinFloatType_found);
        return nullptr;
      }

      return visitLiteralExpr(expr);
    }

    Type visitLiteralExpr(LiteralExpr *expr) {
      // If the expression has already been assigned a type; just use that type.
      if (expr->getType())
        return expr->getType();

      auto protocol = TypeChecker::getLiteralProtocol(CS.getASTContext(), expr);
      if (!protocol)
        return nullptr;

      auto tv = CS.createTypeVariable(CS.getConstraintLocator(expr),
                                      TVO_PrefersSubtypeBinding |
                                      TVO_CanBindToNoEscape);
      CS.addConstraint(ConstraintKind::LiteralConformsTo, tv,
                       protocol->getDeclaredInterfaceType(),
                       CS.getConstraintLocator(expr));
      return tv;
    }

    Type
    visitInterpolatedStringLiteralExpr(InterpolatedStringLiteralExpr *expr) {
      // Dig out the ExpressibleByStringInterpolation protocol.
      auto &ctx = CS.getASTContext();
      auto interpolationProto = TypeChecker::getProtocol(
          ctx, expr->getLoc(),
          KnownProtocolKind::ExpressibleByStringInterpolation);
      if (!interpolationProto) {
        ctx.Diags.diagnose(expr->getStartLoc(),
                           diag::interpolation_missing_proto);
        return nullptr;
      }

      // The type of the expression must conform to the
      // ExpressibleByStringInterpolation protocol.
      auto locator = CS.getConstraintLocator(expr);
      auto tv = CS.createTypeVariable(locator,
                                      TVO_PrefersSubtypeBinding |
                                      TVO_CanBindToNoEscape);
      CS.addConstraint(ConstraintKind::LiteralConformsTo, tv,
                       interpolationProto->getDeclaredInterfaceType(),
                       locator);

      if (auto appendingExpr = expr->getAppendingExpr()) {
        auto associatedTypeDecl = interpolationProto->getAssociatedType(
          ctx.Id_StringInterpolation);
        if (associatedTypeDecl == nullptr) {
          ctx.Diags.diagnose(expr->getStartLoc(),
                             diag::interpolation_broken_proto);
          return nullptr;
        }

        auto interpolationTV = DependentMemberType::get(tv, associatedTypeDecl);

        auto appendingExprType = CS.getType(appendingExpr);
        auto appendingLocator = CS.getConstraintLocator(appendingExpr);

        // Must be Conversion; if it's Equal, then in semi-rare cases, the 
        // interpolation temporary variable cannot be @lvalue.
        CS.addConstraint(ConstraintKind::Conversion, appendingExprType,
                         interpolationTV, appendingLocator);
      }

      return tv;
    }

    Type visitMagicIdentifierLiteralExpr(MagicIdentifierLiteralExpr *expr) {
#ifdef SWIFT_SWIFT_PARSER
      auto &ctx = CS.getASTContext();
      if (ctx.LangOpts.hasFeature(Feature::BuiltinMacros)) {
        auto kind = MagicIdentifierLiteralExpr::getKindString(expr->getKind())
                        .drop_front();

        auto protocol =
            TypeChecker::getLiteralProtocol(CS.getASTContext(), expr);
        if (!protocol)
          return Type();

        auto macroIdent = ctx.getIdentifier(kind);
        auto macros = lookupMacros(
            macroIdent, FunctionRefKind::Unapplied,
            MacroRole::Expression);
        if (!macros.empty()) {
          // Introduce an overload set for the macro reference.
          auto locator = CS.getConstraintLocator(expr);
          auto macroRefType = Type(CS.createTypeVariable(locator, 0));
          CS.addOverloadSet(macroRefType, macros, CurDC, locator);

          // FIXME: Can this be encoded in the macro definition somehow?
          CS.addConstraint(ConstraintKind::LiteralConformsTo, macroRefType,
                           protocol->getDeclaredInterfaceType(),
                           CS.getConstraintLocator(expr));

          return macroRefType;
        }
      }

      // Fall through to use old implementation.
#endif

      switch (expr->getKind()) {
      // Magic pointer identifiers are of type UnsafeMutableRawPointer.
#define MAGIC_POINTER_IDENTIFIER(NAME, STRING, SYNTAX_KIND) \
      case MagicIdentifierLiteralExpr::NAME:
#include "swift/AST/MagicIdentifierKinds.def"
      {
        auto &ctx = CS.getASTContext();
        if (TypeChecker::requirePointerArgumentIntrinsics(ctx, expr->getLoc()))
          return nullptr;

        return ctx.getUnsafeRawPointerType();
      }

      default:
        // Others are actual literals and should be handled like any literal.
        return visitLiteralExpr(expr);
      }

      llvm_unreachable("Unhandled MagicIdentifierLiteralExpr in switch.");
    }

    Type visitObjectLiteralExpr(ObjectLiteralExpr *expr) {
      auto *exprLoc = CS.getConstraintLocator(expr);
      CS.associateArgumentList(exprLoc, expr->getArgs());

      // If the expression has already been assigned a type; just use that type.
      if (expr->getType())
        return expr->getType();

      auto &ctx = CS.getASTContext();
      auto &de = ctx.Diags;
      auto protocol = TypeChecker::getLiteralProtocol(ctx, expr);
      if (!protocol) {
        de.diagnose(expr->getLoc(), diag::use_unknown_object_literal_protocol,
                    expr->getLiteralKindPlainName());
        return nullptr;
      }

      auto witnessType = CS.createTypeVariable(
          exprLoc, TVO_PrefersSubtypeBinding | TVO_CanBindToNoEscape |
                       TVO_CanBindToHole);

      CS.addConstraint(ConstraintKind::LiteralConformsTo, witnessType,
                       protocol->getDeclaredInterfaceType(), exprLoc);

      // The arguments are required to be argument-convertible to the
      // idealized parameter type of the initializer, which generally
      // simplifies the first label (e.g. "colorLiteralRed:") by stripping
      // all the redundant stuff about literals (leaving e.g. "red:").
      // Constraint application will quietly rewrite the type of 'args' to
      // use the right labels before forming the call to the initializer.
      auto constrName = TypeChecker::getObjectLiteralConstructorName(ctx, expr);
      assert(constrName);
      auto *constr = dyn_cast_or_null<ConstructorDecl>(
          protocol->getSingleRequirement(constrName));
      if (!constr) {
        de.diagnose(protocol, diag::object_literal_broken_proto);
        return nullptr;
      }

      auto *memberLoc =
          CS.getConstraintLocator(expr, ConstraintLocator::ConstructorMember);

      auto *fnLoc =
          CS.getConstraintLocator(expr, ConstraintLocator::ApplyFunction);

      auto *memberTypeLoc = CS.getConstraintLocator(
          fnLoc, LocatorPathElt::ConstructorMemberType());

      auto *memberType =
          CS.createTypeVariable(memberTypeLoc, TVO_CanBindToNoEscape);

      CS.addValueMemberConstraint(MetatypeType::get(witnessType, ctx),
                                  DeclNameRef(constrName), memberType, CurDC,
                                  FunctionRefKind::DoubleApply, {}, memberLoc);

      SmallVector<AnyFunctionType::Param, 8> params;
      getMatchingParams(expr->getArgs(), params);

      auto resultType = CS.createTypeVariable(
          CS.getConstraintLocator(expr, ConstraintLocator::FunctionResult),
          TVO_CanBindToNoEscape);

      CS.addConstraint(ConstraintKind::ApplicableFunction,
                       FunctionType::get(params, resultType), memberType,
                       fnLoc);

      if (constr->isFailable())
        return OptionalType::get(witnessType);

      return witnessType;
    }

    Type visitRegexLiteralExpr(RegexLiteralExpr *E) {
      auto &ctx = CS.getASTContext();
      auto *regexDecl = ctx.getRegexDecl();
      if (!regexDecl) {
        ctx.Diags.diagnose(E->getLoc(),
                           diag::string_processing_lib_missing,
                           ctx.Id_Regex.str());
        return Type();
      }
      SmallVector<TupleTypeElt, 4> matchElements;
      if (decodeRegexCaptureTypes(ctx,
                                  E->getSerializedCaptureStructure(),
                                  /*atomType*/ ctx.getSubstringType(),
                                  matchElements)) {
        ctx.Diags.diagnose(E->getLoc(),
                           diag::regex_capture_types_failed_to_decode);
        return Type();
      }
      assert(!matchElements.empty() && "Should have decoded at least an atom");
      if (matchElements.size() == 1)
        return BoundGenericStructType::get(
            regexDecl, Type(), matchElements.front().getType());
      // Form a tuple.
      auto matchType = TupleType::get(matchElements, ctx);
      return BoundGenericStructType::get(regexDecl, Type(), {matchType});
    }

    Type visitDeclRefExpr(DeclRefExpr *E) {
      auto locator = CS.getConstraintLocator(E);

      auto invalidateReference = [&]() -> Type {
        auto *hole = CS.createTypeVariable(locator, TVO_CanBindToHole);
        (void)CS.recordFix(AllowRefToInvalidDecl::create(CS, locator));
        CS.setType(E, hole);
        return hole;
      };

      Type knownType;
      if (auto *VD = dyn_cast<VarDecl>(E->getDecl())) {
        knownType = CS.getTypeIfAvailable(VD);
        if (!knownType)
          knownType = CS.getVarType(VD);

        if (knownType) {
          // An out-of-scope type variable(s) could appear the type of
          // a declaration only in diagnostic mode when invalid variable
          // declaration is recursively referenced inside of a multi-statement
          // closure located somewhere within its initializer e.g.:
          // `let x = [<call>] { ... print(x) }`. It happens because the
          // variable assumes the result type of its initializer unless
          // its specified explicitly.
          if (isa<ClosureExpr>(CurDC) && knownType->hasTypeVariable()) {
            if (knownType.findIf([&](Type type) {
                  auto *typeVar = type->getAs<TypeVariableType>();
                  if (!typeVar || CS.getFixedType(typeVar))
                    return false;

                  return !CS.isActiveTypeVariable(typeVar);
                }))
              return invalidateReference();
          }

          // If the known type has an error, bail out.
          if (knownType->hasError()) {
            return invalidateReference();
          }

          // value packs cannot be referenced without `each` immediately
          // preceding them.
          if (auto *expansion = knownType->getAs<PackExpansionType>()) {
            if (!PackElementEnvironments.empty() &&
                !isExpr<PackElementExpr>(CS.getParentExpr(E))) {
              auto packType = expansion->getPatternType();
              (void)CS.recordFix(
                  IgnoreMissingEachKeyword::create(CS, packType, locator));
              auto eltType = openPackElement(packType, locator);
              CS.setType(E, eltType);
              return eltType;
            }
          }

          if (!knownType->hasPlaceholder()) {
            // Set the favored type for this expression to the known type.
            CS.setFavoredType(E, knownType.getPointer());
          }
        }
      }

      // If declaration is invalid, let's turn it into a potential hole
      // and keep generating constraints.
      // For code completion, we still resolve the overload and replace error
      // types inside the function decl with placeholders
      // (in getTypeOfReference) so we can match non-error param types.
      if (!knownType && E->getDecl()->isInvalid() &&
          !CS.isForCodeCompletion()) {
        return invalidateReference();
      }

      unsigned options = (TVO_CanBindToLValue |
                          TVO_CanBindToNoEscape);
      if (!PackElementEnvironments.empty())
        options |= TVO_CanBindToPack;

      // Create an overload choice referencing this declaration and immediately
      // resolve it. This records the overload for use later.
      auto tv = CS.createTypeVariable(locator, options);

      OverloadChoice choice =
          OverloadChoice(Type(), E->getDecl(), E->getFunctionRefKind());
      CS.resolveOverload(locator, tv, choice, CurDC);
      return tv;
    }

    Type visitOtherConstructorDeclRefExpr(OtherConstructorDeclRefExpr *E) {
      return E->getType();
    }

    Type visitSuperRefExpr(SuperRefExpr *E) {
      if (E->getType())
        return E->getType();

      // Resolve the super type of 'self'.
      return getSuperType(E->getSelf(), E->getLoc(),
                          diag::super_not_in_class_method,
                          diag::super_with_no_base_class);
    }

    Type
    resolveTypeReferenceInExpression(TypeRepr *repr, TypeResolverContext resCtx,
                                     const ConstraintLocatorBuilder &locator) {
      TypeResolutionOptions options(resCtx);

      // Introduce type variables for unbound generics.
      const auto genericOpener = OpenUnboundGenericType(CS, locator);
      const auto placeholderHandler = HandlePlaceholderType(CS, locator);

      // Add a PackElementOf constraint for 'each T' type reprs.
      PackExpansionExpr *elementEnv = nullptr;
      if (!PackElementEnvironments.empty()) {
        options |= TypeResolutionFlags::AllowPackReferences;
        elementEnv = PackElementEnvironments.back();
      }
      const auto packElementOpener = OpenPackElementType(CS, locator, elementEnv);

      const auto result = TypeResolution::resolveContextualType(
          repr, CS.DC, options, genericOpener, placeholderHandler,
          packElementOpener);
      if (result->hasError()) {
        CS.recordFix(
            IgnoreInvalidASTNode::create(CS, CS.getConstraintLocator(locator)));

        return CS.createTypeVariable(CS.getConstraintLocator(repr),
                                     TVO_CanBindToHole);
      }
      // Diagnose top-level usages of placeholder types.
      if (isa<PlaceholderTypeRepr>(repr->getWithoutParens())) {
        CS.getASTContext().Diags.diagnose(repr->getLoc(),
                                          diag::placeholder_type_not_allowed);
      }
      return result;
    }

    Type visitTypeExpr(TypeExpr *E) {
      Type type;
      // If this is an implicit TypeExpr, don't validate its contents.
      auto *const locator = CS.getConstraintLocator(E);
      if (E->isImplicit()) {
        type = CS.getInstanceType(CS.cacheType(E));
        assert(type && "Implicit type expr must have type set!");
        type = CS.replaceInferableTypesWithTypeVars(type, locator);
      } else {
        auto *repr = E->getTypeRepr();
        assert(repr && "Explicit node has no type repr!");
        type = resolveTypeReferenceInExpression(
            repr, TypeResolverContext::InExpression, locator);
      }

      if (!type || type->hasError()) return Type();

      return MetatypeType::get(type);
    }

    Type visitDotSyntaxBaseIgnoredExpr(DotSyntaxBaseIgnoredExpr *expr) {
      llvm_unreachable("Already type-checked");
    }

    Type visitOverloadedDeclRefExpr(OverloadedDeclRefExpr *expr) {
      // For a reference to an overloaded declaration, we create a type variable
      // that will be equal to different types depending on which overload
      // is selected.
      auto locator = CS.getConstraintLocator(expr);
      auto tv = CS.createTypeVariable(locator,
                                      TVO_CanBindToLValue | TVO_CanBindToNoEscape);
      ArrayRef<ValueDecl*> decls = expr->getDecls();
      SmallVector<OverloadChoice, 4> choices;
      bool anyMacros = false;
      auto addChoices = [&](bool skipMacros) {
        for (unsigned i = 0, n = decls.size(); i != n; ++i) {
          // If the result is invalid, skip it.
          // FIXME: Note this as invalid, in case we don't find a solution,
          // so we don't let errors cascade further.
          if (decls[i]->isInvalid())
            continue;

          // If the result is a macro, skip it if we're supposed to.
          if (skipMacros && isa<MacroDecl>(decls[i])) {
            anyMacros = true;
            continue;
          }

          OverloadChoice choice =
              OverloadChoice(Type(), decls[i], expr->getFunctionRefKind());
          choices.push_back(choice);
        }
      };

      addChoices(/*skipMacros=*/true);

      if (choices.empty()) {
        // If there are no valid overloads, but we ignored some macros, add
        // the macros. This improves recovery when the user forgot the leading
        // '#'.
        if (anyMacros) {
          addChoices(/*skipMacros=*/false);
          assert(!choices.empty());
        } else {
          // There are no suitable overloads. Just fail.
          return nullptr;
        }
      }

      // Record this overload set.
      CS.addOverloadSet(tv, choices, CurDC, locator);
      return tv;
    }

    Type visitUnresolvedDeclRefExpr(UnresolvedDeclRefExpr *expr) {
      // This is an error case, where we're trying to use type inference
      // to help us determine which declaration the user meant to refer to.
      // FIXME: Do we need to note that we're doing some kind of recovery?
      return CS.createTypeVariable(CS.getConstraintLocator(expr),
                                   TVO_CanBindToLValue |
                                   TVO_CanBindToNoEscape);
    }
    
    Type visitMemberRefExpr(MemberRefExpr *expr) {
      return addMemberRefConstraints(expr, expr->getBase(),
                                     expr->getMember().getDecl(),
                                     /*FIXME:*/FunctionRefKind::DoubleApply);
    }
    
    Type visitDynamicMemberRefExpr(DynamicMemberRefExpr *expr) {
      llvm_unreachable("Already typechecked");
    }

    void setUnresolvedBaseType(UnresolvedMemberExpr *UME, Type ty) {
      UnresolvedBaseTypes.insert({UME, ty});
    }

    Type getUnresolvedBaseType(UnresolvedMemberExpr *UME) {
      auto result = UnresolvedBaseTypes.find(UME);
      assert(result != UnresolvedBaseTypes.end());
      return result->second;
    }
    
    virtual Type visitUnresolvedMemberExpr(UnresolvedMemberExpr *expr) {
      auto baseLocator = CS.getConstraintLocator(
                            expr,
                            ConstraintLocator::MemberRefBase);
      auto memberLocator
        = CS.getConstraintLocator(expr, ConstraintLocator::UnresolvedMember);

      // Since base type in this case is completely dependent on context it
      // should be marked as a potential hole.
      auto baseTy = CS.createTypeVariable(baseLocator, TVO_CanBindToNoEscape |
                                                           TVO_CanBindToHole);
      setUnresolvedBaseType(expr, baseTy);

      auto memberTy = CS.createTypeVariable(
          memberLocator, TVO_CanBindToLValue | TVO_CanBindToNoEscape);

      // An unresolved member expression '.member' is modeled as a value member
      // constraint
      //
      //   T0.Type[.member] == T1
      //
      // for fresh type variables T0 and T1, which pulls out a static
      // member, i.e., an enum case or a static variable.
      auto baseMetaTy = MetatypeType::get(baseTy);
      CS.addUnresolvedValueMemberConstraint(baseMetaTy, expr->getName(),
                                            memberTy, CurDC,
                                            expr->getFunctionRefKind(),
                                            memberLocator);
      return memberTy;
    }

    Type visitUnresolvedMemberChainResultExpr(
        UnresolvedMemberChainResultExpr *expr) {
      auto *tail = expr->getSubExpr();
      auto memberTy = CS.getType(tail);
      auto *base = expr->getChainBase();
      assert(base == TypeChecker::getUnresolvedMemberChainBase(tail));

      // The result type of the chain is is represented by a new type variable.
      auto locator = CS.getConstraintLocator(
          expr, ConstraintLocator::UnresolvedMemberChainResult);
      auto chainResultTy = CS.createTypeVariable(
          locator,
          TVO_CanBindToLValue | TVO_CanBindToHole | TVO_CanBindToNoEscape);
      auto chainBaseTy = getUnresolvedBaseType(base);

      // The result of the last element of the chain must be convertible to the
      // whole chain, and the type of the whole chain must be equal to the base.
      CS.addConstraint(ConstraintKind::Conversion, memberTy, chainResultTy,
                       locator);
      CS.addConstraint(ConstraintKind::UnresolvedMemberChainBase, chainResultTy,
                       chainBaseTy, locator);

      return chainResultTy;
    }

    Type visitUnresolvedDotExpr(UnresolvedDotExpr *expr) {
      // UnresolvedDot applies the base to remove a single curry level from a
      // member reference without using an applicable function constraint so
      // we record the call argument matching here so it can be found later when
      // a solution is applied to the AST.
      CS.recordMatchCallArgumentResult(
          CS.getConstraintLocator(expr, ConstraintLocator::ApplyArgument),
          MatchCallArgumentResult::forArity(1));

      // If this is Builtin.type_join*, just return any type and move
      // on since we're going to discard this, and creating any type
      // variables for the reference will cause problems.
      auto &ctx = CS.getASTContext();
      auto typeOperation = getTypeOperation(expr, ctx);
      if (typeOperation != TypeOperation::None)
        return ctx.getAnyExistentialType();

      // If this is `Builtin.trigger_fallback_diagnostic()`, fail
      // without producing any diagnostics, in order to test fallback error.
      if (isTriggerFallbackDiagnosticBuiltin(expr, ctx))
        return Type();

      // Open a member constraint for constructor delegations on the
      // subexpr type.
      if (TypeChecker::getSelfForInitDelegationInConstructor(CS.DC, expr)) {
        auto baseTy = CS.getType(expr->getBase())
                        ->getWithoutSpecifierType();

        // 'self' or 'super' will reference an instance, but the constructor
        // is semantically a member of the metatype. This:
        //   self.init()
        //   super.init()
        // is really more like:
        //   self = Self.init()
        //   self.super = Super.init()
        baseTy = MetatypeType::get(baseTy, ctx);

        auto memberTypeLoc = CS.getConstraintLocator(
            expr, LocatorPathElt::ConstructorMemberType(
                      /*shortFormOrSelfDelegating*/ true));

        auto methodTy =
            CS.createTypeVariable(memberTypeLoc, TVO_CanBindToNoEscape);

        // HACK: Bind the function's parameter list as a tuple to a type
        // variable. This only exists to preserve compatibility with
        // rdar://85263844, as it can affect the prioritization of bindings,
        // which can affect behavior for tuple matching as tuple subtyping is
        // currently a *weaker* constraint than tuple conversion.
        if (!CS.getASTContext().isSwiftVersionAtLeast(6)) {
          auto paramTypeVar = CS.createTypeVariable(
              CS.getConstraintLocator(expr, ConstraintLocator::ApplyArgument),
              TVO_CanBindToLValue | TVO_CanBindToInOut | TVO_CanBindToNoEscape |
                  TVO_CanBindToPack);
          CS.addConstraint(ConstraintKind::BindTupleOfFunctionParams, methodTy,
                           paramTypeVar, CS.getConstraintLocator(expr));
        }

        CS.addValueMemberConstraint(
            baseTy, expr->getName(), methodTy, CurDC,
            expr->getFunctionRefKind(),
            /*outerAlternatives=*/{},
            CS.getConstraintLocator(expr,
                                    ConstraintLocator::ConstructorMember));

        // The result of the expression is the partial application of the
        // constructor to the subexpression.
        return methodTy;
      }

      return addMemberRefConstraints(expr, expr->getBase(), expr->getName(),
                                     expr->getFunctionRefKind(),
                                     expr->getOuterAlternatives());
    }

    /// Given a set of specialization arguments, resolve those arguments and
    /// introduce them as an explicit generic arguments constraint.
    ///
    /// \returns true if resolving any of the specialization types failed.
    bool addSpecializationConstraint(
        ConstraintLocator *locator, Type boundType,
        ArrayRef<TypeRepr *> specializationArgs) {
      // Resolve each type.
      SmallVector<Type, 2> specializationArgTypes;
      auto options =
          TypeResolutionOptions(TypeResolverContext::InExpression);
      for (auto specializationArg : specializationArgs) {
        PackExpansionExpr *elementEnv = nullptr;
        if (!PackElementEnvironments.empty()) {
          options |= TypeResolutionFlags::AllowPackReferences;
          elementEnv = PackElementEnvironments.back();
        }
        const auto result = TypeResolution::resolveContextualType(
            specializationArg, CurDC, options,
            // Introduce type variables for unbound generics.
            OpenUnboundGenericType(CS, locator),
            HandlePlaceholderType(CS, locator),
            OpenPackElementType(CS, locator, elementEnv));
        if (result->hasError())
          return true;

        specializationArgTypes.push_back(result);
      }

      CS.addConstraint(
          ConstraintKind::ExplicitGenericArguments, boundType,
          PackType::get(CS.getASTContext(), specializationArgTypes),
          locator);
      return false;
    }

    Type visitUnresolvedSpecializeExpr(UnresolvedSpecializeExpr *expr) {
      auto baseTy = CS.getType(expr->getSubExpr());

      if (baseTy->isTypeVariableOrMember()) {
        return baseTy;
      }

      // We currently only support explicit specialization of generic types.
      // FIXME: We could support explicit function specialization.
      auto &de = CS.getASTContext().Diags;
      if (baseTy->is<AnyFunctionType>()) {
        de.diagnose(expr->getSubExpr()->getLoc(),
                    diag::cannot_explicitly_specialize_generic_function);
        de.diagnose(expr->getLAngleLoc(),
                    diag::while_parsing_as_left_angle_bracket);
        return Type();
      }
      
      if (AnyMetatypeType *meta = baseTy->getAs<AnyMetatypeType>()) {
        auto *overloadLocator = CS.getConstraintLocator(expr->getSubExpr());
        if (addSpecializationConstraint(overloadLocator,
                                        meta->getInstanceType(),
                                        expr->getUnresolvedParams())) {
          return Type();
        }

        return baseTy;
      }

      // FIXME: If the base type is a type variable, constrain it to a metatype
      // of a bound generic type.
      de.diagnose(expr->getSubExpr()->getLoc(),
                  diag::not_a_generic_definition);
      de.diagnose(expr->getLAngleLoc(),
                  diag::while_parsing_as_left_angle_bracket);
      return Type();
    }
    
    Type visitSequenceExpr(SequenceExpr *expr) {
      // If a SequenceExpr survived until CSGen, then there was an upstream
      // error that was already reported.
      return Type();
    }

    Type visitArrowExpr(ArrowExpr *expr) {
      // If an ArrowExpr survived until CSGen, then there was an upstream
      // error that was already reported.
      return Type();
    }

    Type visitIdentityExpr(IdentityExpr *expr) {
      return CS.getType(expr->getSubExpr());
    }

    Type visitCopyExpr(CopyExpr *expr) {
      auto valueTy = CS.createTypeVariable(CS.getConstraintLocator(expr),
                                           TVO_PrefersSubtypeBinding |
                                               TVO_CanBindToNoEscape);
      CS.addConstraint(ConstraintKind::Equal, valueTy,
                       CS.getType(expr->getSubExpr()),
                       CS.getConstraintLocator(expr));
      return valueTy;
    }

    Type visitConsumeExpr(ConsumeExpr *expr) {
      auto valueTy = CS.createTypeVariable(CS.getConstraintLocator(expr),
                                           TVO_PrefersSubtypeBinding |
                                               TVO_CanBindToNoEscape);
      CS.addConstraint(ConstraintKind::Equal, valueTy,
                       CS.getType(expr->getSubExpr()),
                       CS.getConstraintLocator(expr));
      return valueTy;
    }

    Type visitAnyTryExpr(AnyTryExpr *expr) {
      return CS.getType(expr->getSubExpr());
    }

    Type visitOptionalTryExpr(OptionalTryExpr *expr) {
      auto valueTy = CS.createTypeVariable(CS.getConstraintLocator(expr),
                                           TVO_PrefersSubtypeBinding |
                                           TVO_CanBindToNoEscape);

      Type optTy = getOptionalType(expr->getSubExpr()->getLoc(), valueTy);
      if (!optTy)
        return Type();

      bool isDelegationToOptionalInit = false;
      {
        Expr *e = expr->getSubExpr();
        while (true) {
          e = e->getSemanticsProvidingExpr();

          // Look through force-value expressions.
          if (auto *FVE = dyn_cast<ForceValueExpr>(e)) {
            e = FVE->getSubExpr();
            continue;
          }

          break;
        }

        if (auto *CE = dyn_cast<CallExpr>(e)) {
          if (auto *UDE = dyn_cast<UnresolvedDotExpr>(CE->getFn())) {
            if (!CS.getType(UDE->getBase())->is<AnyMetatypeType>()) {
              auto overload =
                  CS.findSelectedOverloadFor(CS.getConstraintLocator(
                      UDE, ConstraintLocator::ConstructorMember));
              if (overload) {
                auto *decl = overload->choice.getDeclOrNull();
                if (decl && isa<ConstructorDecl>(decl) &&
                    decl->getDeclContext()
                        ->getSelfNominalTypeDecl()
                        ->isOptionalDecl())
                  isDelegationToOptionalInit = true;
              }
            }
          }
        }
      }

      // Prior to Swift 5, 'try?' always adds an additional layer of
      // optionality, even if the sub-expression was already optional.
      //
      // NB Keep adding the additional layer in Swift 5 and on if this 'try?'
      // applies to a delegation to an 'Optional' initializer, or else we won't
      // discern the difference between a failure and a constructed value.
      if (CS.getASTContext().LangOpts.isSwiftVersionAtLeast(5) &&
          !isDelegationToOptionalInit) {
        CS.addConstraint(ConstraintKind::Conversion,
                         CS.getType(expr->getSubExpr()), optTy,
                         CS.getConstraintLocator(expr));
      } else {
        CS.addConstraint(ConstraintKind::OptionalObject,
                         optTy, CS.getType(expr->getSubExpr()),
                         CS.getConstraintLocator(expr));
      }
      return optTy;
    }

    virtual Type visitParenExpr(ParenExpr *expr) {
      // If the ParenExpr contains a pack expansion, generate a tuple
      // type containing the pack expansion type.
      if (CS.getType(expr->getSubExpr())->getAs<PackExpansionType>()) {
        return TupleType::get({CS.getType(expr->getSubExpr())},
                              CS.getASTContext());
      }

      if (auto favoredTy = CS.getFavoredType(expr->getSubExpr())) {
        CS.setFavoredType(expr, favoredTy);
      }
      return ParenType::get(CS.getASTContext(), CS.getType(expr->getSubExpr()));
    }

    Type visitTupleExpr(TupleExpr *expr) {
      // The type of a tuple expression is simply a tuple of the types of
      // its subexpressions.
      SmallVector<TupleTypeElt, 4> elements;
      elements.reserve(expr->getNumElements());
      for (unsigned i = 0, n = expr->getNumElements(); i != n; ++i) {
        elements.emplace_back(CS.getType(expr->getElement(i)),
                              expr->getElementName(i));
      }

      return TupleType::get(elements, CS.getASTContext());
    }

    Type visitSubscriptExpr(SubscriptExpr *expr) {
      ValueDecl *decl = nullptr;
      if (expr->hasDecl()) {
        decl = expr->getDecl().getDecl();
        if (decl->isInvalid())
          return Type();
      }

      auto *base = expr->getBase();
      if (!isValidBaseOfMemberRef(base, diag::cannot_subscript_nil_literal))
        return nullptr;

      return addSubscriptConstraints(expr, CS.getType(base), decl,
                                     expr->getArgs());
    }
    
    Type visitArrayExpr(ArrayExpr *expr) {
      // An array expression can be of a type T that conforms to the
      // ExpressibleByArrayLiteral protocol.
      ProtocolDecl *arrayProto = TypeChecker::getProtocol(
          CS.getASTContext(), expr->getLoc(),
          KnownProtocolKind::ExpressibleByArrayLiteral);
      if (!arrayProto) {
        return Type();
      }

      // Assume that ExpressibleByArrayLiteral contains a single associated type.
      auto *elementAssocTy = arrayProto->getAssociatedTypeMembers()[0];
      if (!elementAssocTy)
        return Type();

      auto locator = CS.getConstraintLocator(expr);
      auto contextualType = CS.getContextualType(expr, /*forConstraint=*/false);
      auto contextualPurpose = CS.getContextualTypePurpose(expr);

      auto joinElementTypes = [&](Optional<Type> elementType) {
        const auto elements = expr->getElements();
        unsigned index = 0;

        using Iterator = decltype(elements)::iterator;
        CS.addJoinConstraint<Iterator>(
            locator, elements.begin(), elements.end(), elementType,
            [&](const auto it) {
              auto *locator = CS.getConstraintLocator(
                  expr, LocatorPathElt::TupleElement(index++));
              return std::make_pair(CS.getType(*it), locator);
            });
      };

      // If a contextual type exists for this expression, apply it directly.
      if (contextualType && contextualType->isArrayType()) {
        // Now that we know we're actually going to use the type, get the
        // version for use in a constraint.
        contextualType = CS.getContextualType(expr, /*forConstraint=*/true);
        contextualType = CS.openOpaqueType(
            contextualType, contextualPurpose, locator);
        Type arrayElementType = contextualType->isArrayType();
        CS.addConstraint(ConstraintKind::LiteralConformsTo, contextualType,
                         arrayProto->getDeclaredInterfaceType(),
                         locator);
        joinElementTypes(arrayElementType);
        return contextualType;
      }

      // Produce a specialized diagnostic if this is an attempt to initialize
      // or convert an array literal to a dictionary e.g.
      // `let _: [String: Int] = ["A", 0]`
      auto isDictionaryContextualType = [&](Type contextualType) -> bool {
        if (!contextualType)
          return false;

        auto *M = CS.DC->getParentModule();

        auto type = contextualType->lookThroughAllOptionalTypes();
        if (TypeChecker::conformsToKnownProtocol(
                type, KnownProtocolKind::ExpressibleByArrayLiteral, M))
          return false;

        return TypeChecker::conformsToKnownProtocol(
            type, KnownProtocolKind::ExpressibleByDictionaryLiteral, M);
      };

      if (isDictionaryContextualType(contextualType)) {
        auto &DE = CS.getASTContext().Diags;
        auto numElements = expr->getNumElements();

        // Empty and single element array literals with dictionary contextual
        // types are fixed during solving, so continue as normal in those
        // cases.
        if (numElements > 1) {
          bool isIniting =
              CS.getContextualTypePurpose(expr) == CTP_Initialization;
          DE.diagnose(expr->getStartLoc(), diag::should_use_dictionary_literal,
                      contextualType->lookThroughAllOptionalTypes(), isIniting);

          auto diagnostic =
              DE.diagnose(expr->getStartLoc(), diag::meant_dictionary_lit);

          // If there is an even number of elements in the array, let's produce
          // a fix-it which suggests to replace "," with ":" to form a dictionary
          // literal.
          if ((numElements & 1) == 0) {
            const auto commaLocs = expr->getCommaLocs();
            if (commaLocs.size() == numElements - 1) {
              for (unsigned i = 0, e = numElements / 2; i != e; ++i)
                diagnostic.fixItReplace(commaLocs[i * 2], ":");
            }
          }

          return nullptr;
        }
      }

      auto arrayTy = CS.createTypeVariable(locator,
                                           TVO_PrefersSubtypeBinding |
                                           TVO_CanBindToNoEscape);

      // The array must be an array literal type.
      CS.addConstraint(ConstraintKind::LiteralConformsTo, arrayTy,
                       arrayProto->getDeclaredInterfaceType(),
                       locator);
      
      // Its subexpression should be convertible to a tuple (T.Element...).
      Type arrayElementTy = DependentMemberType::get(arrayTy, elementAssocTy);

      // Introduce conversions from each element to the element type of the
      // array.
      joinElementTypes(arrayElementTy);

      // The array element type defaults to 'Any'.
      CS.addConstraint(ConstraintKind::Defaultable, arrayElementTy,
                       CS.getASTContext().getAnyExistentialType(), locator);

      return arrayTy;
    }

    static bool isMergeableValueKind(Expr *expr) {
      return isa<StringLiteralExpr>(expr) || isa<IntegerLiteralExpr>(expr) ||
             isa<FloatLiteralExpr>(expr);
    }

    Type visitDictionaryExpr(DictionaryExpr *expr) {
      ASTContext &C = CS.getASTContext();
      // A dictionary expression can be of a type T that conforms to the
      // ExpressibleByDictionaryLiteral protocol.
      // FIXME: This isn't actually used for anything at the moment.
      ProtocolDecl *dictionaryProto = TypeChecker::getProtocol(
          C, expr->getLoc(), KnownProtocolKind::ExpressibleByDictionaryLiteral);
      if (!dictionaryProto) {
        return Type();
      }

      // FIXME: Protect against broken standard library.
      auto keyAssocTy = dictionaryProto->getAssociatedType(C.Id_Key);
      auto valueAssocTy = dictionaryProto->getAssociatedType(C.Id_Value);

      auto locator = CS.getConstraintLocator(expr);
      auto contextualType = CS.getContextualType(expr, /*forConstraint=*/false);
      auto contextualPurpose = CS.getContextualTypePurpose(expr);

      // If a contextual type exists for this expression and is a dictionary
      // type, apply it directly.
      if (contextualType && ConstraintSystem::isDictionaryType(contextualType)) {
        // Now that we know we're actually going to use the type, get the
        // version for use in a constraint.
        contextualType = CS.getContextualType(expr, /*forConstraint=*/true);
        auto openedType =
            CS.openOpaqueType(contextualType, contextualPurpose, locator);
        auto dictionaryKeyValue =
            ConstraintSystem::isDictionaryType(openedType);
        Type contextualDictionaryKeyType;
        Type contextualDictionaryValueType;
        std::tie(contextualDictionaryKeyType,
                 contextualDictionaryValueType) = *dictionaryKeyValue;
        
        // Form an explicit tuple type from the contextual type's key and value types.
        TupleTypeElt tupleElts[2] = { TupleTypeElt(contextualDictionaryKeyType),
                                      TupleTypeElt(contextualDictionaryValueType) };
        Type contextualDictionaryElementType = TupleType::get(tupleElts, C);

        CS.addConstraint(ConstraintKind::LiteralConformsTo, openedType,
                         dictionaryProto->getDeclaredInterfaceType(), locator);

        unsigned index = 0;
        for (auto element : expr->getElements()) {
          CS.addConstraint(ConstraintKind::Conversion,
                           CS.getType(element),
                           contextualDictionaryElementType,
                           CS.getConstraintLocator(
                               expr, LocatorPathElt::TupleElement(index++)));
        }

        return openedType;
      }

      auto dictionaryTy = CS.createTypeVariable(locator,
                                                TVO_PrefersSubtypeBinding |
                                                TVO_CanBindToNoEscape);

      // The dictionary must be a dictionary literal type.
      CS.addConstraint(ConstraintKind::LiteralConformsTo, dictionaryTy,
                       dictionaryProto->getDeclaredInterfaceType(),
                       locator);


      // Its subexpression should be convertible to a tuple ((T.Key,T.Value)...).
      ConstraintLocatorBuilder locatorBuilder(locator);
      auto dictionaryKeyTy = DependentMemberType::get(dictionaryTy,
                                                      keyAssocTy);
      auto dictionaryValueTy = DependentMemberType::get(dictionaryTy,
                                                        valueAssocTy);
      TupleTypeElt tupleElts[2] = { TupleTypeElt(dictionaryKeyTy),
                                    TupleTypeElt(dictionaryValueTy) };
      Type elementTy = TupleType::get(tupleElts, C);

      // Keep track of which elements have been "merged". This way, we won't create
      // needless conversion constraints for elements whose equivalence classes have
      // been merged.
      llvm::DenseSet<Expr *> mergedElements;

      // If no contextual type is present, Merge equivalence classes of key 
      // and value types as necessary.
      if (!CS.getContextualType(expr, /*forConstraint=*/false)) {
        for (auto element1 : expr->getElements()) {
          for (auto element2 : expr->getElements()) {
            if (element1 == element2)
              continue;

            auto tty1 = CS.getType(element1)->getAs<TupleType>();
            auto tty2 = CS.getType(element2)->getAs<TupleType>();

            if (tty1 && tty2) {
              auto mergedKey = false;
              auto mergedValue = false;

              auto keyTyvar1 = tty1->getElementTypes()[0]->
                                getAs<TypeVariableType>();
              auto keyTyvar2 = tty2->getElementTypes()[0]->
                                getAs<TypeVariableType>();

              auto keyExpr1 = cast<TupleExpr>(element1)->getElements()[0];
              auto keyExpr2 = cast<TupleExpr>(element2)->getElements()[0];

              if (keyExpr1->getKind() == keyExpr2->getKind() &&
                  isMergeableValueKind(keyExpr1)) {
                mergedKey = mergeRepresentativeEquivalenceClasses(CS,
                            keyTyvar1, keyTyvar2);
              }

              auto valueTyvar1 = tty1->getElementTypes()[1]->
                                  getAs<TypeVariableType>();
              auto valueTyvar2 = tty2->getElementTypes()[1]->
                                  getAs<TypeVariableType>();

              auto elemExpr1 = cast<TupleExpr>(element1)->getElements()[1];
              auto elemExpr2 = cast<TupleExpr>(element2)->getElements()[1];

              if (elemExpr1->getKind() == elemExpr2->getKind() &&
                isMergeableValueKind(elemExpr1)) {
                mergedValue = mergeRepresentativeEquivalenceClasses(CS, 
                                valueTyvar1, valueTyvar2);
              }

              if (mergedKey && mergedValue)
                mergedElements.insert(element2);
            }
          }
        }
      }      

      // Introduce conversions from each element to the element type of the
      // dictionary. (If the equivalence class of an element has already been
      // merged with a previous one, skip it.)
      unsigned index = 0;
      for (auto element : expr->getElements()) {
        if (!mergedElements.count(element))
          CS.addConstraint(ConstraintKind::Conversion,
                           CS.getType(element),
                           elementTy,
                           CS.getConstraintLocator(
                               expr, LocatorPathElt::TupleElement(index++)));
      }

      // The dictionary key type defaults to 'AnyHashable'.
      auto &ctx = CS.getASTContext();
      if (dictionaryKeyTy->isTypeVariableOrMember() &&
          ctx.getAnyHashableDecl()) {
        auto anyHashable = ctx.getAnyHashableDecl();
        CS.addConstraint(ConstraintKind::Defaultable, dictionaryKeyTy,
                         anyHashable->getDeclaredInterfaceType(), locator);
      }

      // The dictionary value type defaults to 'Any'.
      if (dictionaryValueTy->isTypeVariableOrMember()) {
        CS.addConstraint(ConstraintKind::Defaultable, dictionaryValueTy,
                         ctx.getAnyExistentialType(), locator);
      }

      return dictionaryTy;
    }

    Type visitDynamicSubscriptExpr(DynamicSubscriptExpr *expr) {
      return addSubscriptConstraints(expr, CS.getType(expr->getBase()),
                                     /*decl*/ nullptr, expr->getArgs());
    }

    Type visitTupleElementExpr(TupleElementExpr *expr) {
      ASTContext &context = CS.getASTContext();
      DeclNameRef name(
          context.getIdentifier(llvm::utostr(expr->getFieldNumber())));
      return addMemberRefConstraints(expr, expr->getBase(), name,
                                     FunctionRefKind::Unapplied,
                                     /*outerAlternatives=*/{});
    }

    /// If \p allowResultBindToHole is \c true, we always allow the closure's
    /// result type to bind to a hole, otherwise the result type may only bind
    /// to a hole if the closure does not participate in type inference. Setting
    /// \p allowResultBindToHole to \c true is useful when ignoring a closure
    /// argument in a function call after the code completion token and thus
    /// wanting to ignore the closure's type.
    FunctionType *inferClosureType(ClosureExpr *closure,
                                   bool allowResultBindToHole = false) {
      SmallVector<AnyFunctionType::Param, 4> closureParams;

      if (auto *paramList = closure->getParameters()) {
        for (unsigned i = 0, n = paramList->size(); i != n; ++i) {
          auto *param = paramList->get(i);
          auto *paramLoc =
              CS.getConstraintLocator(closure, LocatorPathElt::TupleElement(i));

          // If one of the parameters represents a destructured tuple
          // e.g. `{ (x: Int, (y: Int, z: Int)) in ... }` let's fail
          // inference here and not attempt to solve the system because:
          //
          // a. Destructuring has already been diagnosed by the parser;
          // b. Body of the closure would have error expressions for
          //    each incorrect parameter reference and solver wouldn't
          //    be able to produce any viable solutions.
          if (param->isDestructured())
            return nullptr;

          Type externalType;
          if (param->getTypeRepr()) {
            auto declaredTy = CS.getVarType(param);

            // If closure parameter couldn't be resolved, let's record
            // a fix to make sure that type resolution diagnosed the
            // problem and replace it with a placeholder, so that solver
            // can make forward progress (especially important for result
            // builders).
            if (declaredTy->hasError()) {
              CS.recordFix(AllowRefToInvalidDecl::create(
                  CS, CS.getConstraintLocator(param)));
              declaredTy = PlaceholderType::get(CS.getASTContext(), param);
            }

            externalType = CS.replaceInferableTypesWithTypeVars(declaredTy,
                                                                paramLoc);
          } else {
            // Let's allow parameters which haven't been explicitly typed
            // to become holes by default, this helps in situations like
            // `foo { a in }` where `foo` doesn't exist.
            externalType = CS.createTypeVariable(
                paramLoc,
                TVO_CanBindToInOut | TVO_CanBindToNoEscape | TVO_CanBindToHole);
          }

          closureParams.push_back(param->toFunctionParam(externalType));
        }

        checkVariadicParameters(paramList, closure);
      }

      auto extInfo = CS.closureEffects(closure);
      auto resultLocator =
          CS.getConstraintLocator(closure, ConstraintLocator::ClosureResult);

      // Closure expressions always have function type. In cases where a
      // parameter or return type is omitted, a fresh type variable is used to
      // stand in for that parameter or return type, allowing it to be inferred
      // from context.
      Type resultTy = [&] {
        if (closure->hasExplicitResultType()) {
          if (auto declaredTy = closure->getExplicitResultType()) {
            return declaredTy;
          }

          const auto resolvedTy = resolveTypeReferenceInExpression(
              closure->getExplicitResultTypeRepr(),
              TypeResolverContext::InExpression, resultLocator);
          if (resolvedTy)
            return resolvedTy;
        }

        // Because we are only pulling out the result type from the contextual
        // type, we avoid prematurely converting any inferrable types by setting
        // forConstraint=false. Later on in inferClosureType we call
        // replaceInferableTypesWithTypeVars before returning to ensure we don't
        // introduce any placeholders into the constraint system.
        if (auto contextualType =
                CS.getContextualType(closure, /*forConstraint=*/false)) {
          if (auto fnType = contextualType->getAs<FunctionType>())
            return fnType->getResult();
        }

        // If no return type was specified, create a fresh type
        // variable for it and mark it as possible hole.
        //
        // If this is a multi-statement closure, let's mark result
        // as potential hole right away.
        return Type(CS.createTypeVariable(
            resultLocator,
            (!CS.participatesInInference(closure) || allowResultBindToHole)
                ? TVO_CanBindToHole
                : 0));
      }();

      // For a non-async function type, add the global actor if present.
      if (!extInfo.isAsync()) {
        extInfo = extInfo.withGlobalActor(getExplicitGlobalActor(closure));
      }

      auto *fnTy = FunctionType::get(closureParams, resultTy, extInfo);
      return CS.replaceInferableTypesWithTypeVars(
          fnTy, CS.getConstraintLocator(closure))->castTo<FunctionType>();
    }

    /// Produces a type for the given pattern, filling in any missing
    /// type information with fresh type variables.
    ///
    /// \param pattern The pattern.
    ///
    /// \param locator The locator to use for generated constraints and
    /// type variables.
    ///
    /// \param bindPatternVarsOneWay When true, generate fresh type variables
    /// for the types of each variable declared within the pattern, along
    /// with a one-way constraint binding that to the type to which the
    /// variable will be ascribed or inferred.
    Type getTypeForPattern(
       Pattern *pattern, ConstraintLocatorBuilder locator,
       bool bindPatternVarsOneWay,
       PatternBindingDecl *patternBinding = nullptr,
       unsigned patternBindingIndex = 0) {
      assert(pattern);

      // Local function that must be called for each "return" throughout this
      // function, to set the type of the pattern.
      auto setType = [&](Type type) {
        CS.setType(pattern, type);
        return type;
      };

      switch (pattern->getKind()) {
      case PatternKind::Paren: {
        auto *paren = cast<ParenPattern>(pattern);

        auto *subPattern = paren->getSubPattern();
        auto underlyingType = getTypeForPattern(
            subPattern,
            locator.withPathElement(LocatorPathElt::PatternMatch(subPattern)),
            bindPatternVarsOneWay);

        return setType(ParenType::get(CS.getASTContext(), underlyingType));
      }
      case PatternKind::Binding: {
        auto *subPattern = cast<BindingPattern>(pattern)->getSubPattern();
        auto type = getTypeForPattern(subPattern, locator,
                                      bindPatternVarsOneWay);
        // Var doesn't affect the type.
        return setType(type);
      }
      case PatternKind::Any: {
        Type type;

        // If this is a situation like `[let] _ = <expr>`, return
        // initializer expression.
        auto getInitializerExpr = [&locator]() -> Expr * {
          auto last = locator.last();
          if (!last)
            return nullptr;

          auto contextualTy = last->getAs<LocatorPathElt::ContextualType>();
          return (contextualTy && contextualTy->isFor(CTP_Initialization))
                     ? locator.trySimplifyToExpr()
                     : nullptr;
        };

        auto matchLoc =
            locator.withPathElement(LocatorPathElt::PatternMatch(pattern));

        // Always prefer a contextual type when it's available.
        if (auto *initializer = getInitializerExpr()) {
          // For initialization always assume a type of initializer.
          type = CS.getType(initializer)->getRValueType();
        } else {
          type = CS.createTypeVariable(
              CS.getConstraintLocator(matchLoc,
                                      LocatorPathElt::AnyPatternDecl()),
              TVO_CanBindToNoEscape | TVO_CanBindToHole);
        }
        return setType(type);
      }

      case PatternKind::Named: {
        auto var = cast<NamedPattern>(pattern)->getDecl();

        Type varType;

        // Determine whether optionality will be required.
        auto ROK = ReferenceOwnership::Strong;
        if (auto *OA = var->getAttrs().getAttribute<ReferenceOwnershipAttr>())
          ROK = OA->get();
        auto optionality = optionalityOf(ROK);

        // If we have a type from an initializer expression, and that
        // expression does not produce an InOut type, use it.  This
        // will avoid exponential typecheck behavior in the case of
        // tuples, nested arrays, and dictionary literals.
        //
        // FIXME: This should be handled in the solver, not here.
        //
        // Otherwise, create a new type variable.
        if (var->getParentPatternBinding() &&
            !var->hasAttachedPropertyWrapper() &&
            optionality != ReferenceOwnershipOptionality::Required) {
          if (auto boundExpr = locator.trySimplifyToExpr()) {
            if (!boundExpr->isSemanticallyInOutExpr()) {
              varType = CS.getType(boundExpr)->getRValueType();
            }
          }
        }

        auto matchLoc =
            locator.withPathElement(LocatorPathElt::PatternMatch(pattern));

        if (!varType) {
          varType = CS.createTypeVariable(
              CS.getConstraintLocator(matchLoc,
                                      LocatorPathElt::NamedPatternDecl()),
              TVO_CanBindToNoEscape | TVO_CanBindToHole);

          // If this is either a `weak` declaration or capture e.g.
          // `weak var ...` or `[weak self]`. Let's wrap type variable
          // into an optional.
          if (optionality == ReferenceOwnershipOptionality::Required)
            varType = TypeChecker::getOptionalType(var->getLoc(), varType);
        }

        // When we are supposed to bind pattern variables, create a fresh
        // type variable and a one-way constraint to assign it to either the
        // deduced type or the externally-imposed type.
        Type oneWayVarType;
        if (bindPatternVarsOneWay) {
          oneWayVarType = CS.createTypeVariable(
              CS.getConstraintLocator(locator), TVO_CanBindToNoEscape);

          // If there is externally-imposed type, and the variable
          // is marked as `weak`, let's fallthrough and allow the
          // `one-way` constraint to be fixed in diagnostic mode.
          //
          // That would make sure that type of this variable is
          // recorded in the constraint  system, which would then
          // be used instead of `getVarType` upon discovering a
          // reference to this variable in subsequent expression(s).
          //
          // If we let constraint generation fail here, it would trigger
          // interface type request via `var->getType()` that would
          // attempt to validate `weak` attribute, and produce a
          // diagnostic in the middle of the solver path.

          CS.addConstraint(ConstraintKind::OneWayEqual, oneWayVarType,
                           varType, locator);
        }

        // Ascribe a type to the declaration so it's always available to
        // constraint system.
        if (oneWayVarType) {
          CS.setType(var, oneWayVarType);
        } else {
          // Otherwise, let's use the type of the pattern. The type
          // of the declaration has to be r-value, so let's add an
          // equality constraint if pattern type has any type variables
          // that are allowed to be l-value.
          bool foundLValueVars = false;

          // Note that it wouldn't be always correct to allocate a single type
          // variable, that disallows l-value types, to use as a declaration
          // type because equality constraint would drop TVO_CanBindToLValue
          // from the right-hand side (which is not the case for `OneWayEqual`)
          // e.g.:
          //
          // struct S { var x, y: Int }
          //
          // func test(s: S) {
          //   let (x, y) = (s.x, s.y)
          // }
          //
          // Single type variable approach results in the following constraint:
          // `$T_x_y = ($T_s_x, $T_s_y)` where both `$T_s_x` and `$T_s_y` have
          // to allow l-value, but `$T_x_y` does not. Early simplification of `=`
          // constraint (due to right-hand side being a "concrete" tuple type)
          // would drop l-value option from `$T_s_x` and `$T_s_y` which leads to
          // a failure during member lookup because `x` and `y` are both
          // `@lvalue Int`. To avoid that, declaration type would mimic pattern
          // type with all l-value options stripped, so the equality constraint
          // becomes `($T_x, $_T_y) = ($T_s_x, $T_s_y)` which doesn't result in
          // stripping of l-value flag from the right-hand side since
          // simplification can only happen when either side is resolved.
          auto declTy = varType.transform([&](Type type) -> Type {
            if (auto *typeVar = type->getAs<TypeVariableType>()) {
              if (typeVar->getImpl().canBindToLValue()) {
                foundLValueVars = true;

                // Drop l-value from the options but preserve the rest.
                auto options = typeVar->getImpl().getRawOptions();
                options &= ~TVO_CanBindToLValue;

                return CS.createTypeVariable(typeVar->getImpl().getLocator(),
                                             options);
              }
            }
            return type;
          });

          // If pattern types allows l-value types, let's create an
          // equality constraint between r-value only declaration type
          // and l-value pattern type that would take care of looking
          // through l-values when necessary.
          if (foundLValueVars) {
            CS.addConstraint(ConstraintKind::Equal, declTy, varType,
                             CS.getConstraintLocator(locator));
          }

          CS.setType(var, declTy);
        }

        return setType(varType);
      }

      case PatternKind::Typed: {
        // FIXME: Need a better locator for a pattern as a base.
        // Compute the type ascribed to the pattern.
        auto contextualPattern = patternBinding
            ? ContextualPattern::forPatternBindingDecl(
                patternBinding, patternBindingIndex)
            : ContextualPattern::forRawPattern(pattern, CurDC);

        Type type = TypeChecker::typeCheckPattern(contextualPattern);

        // Look through reference storage types.
        type = type->getReferenceStorageReferent();

        Type replacedType = CS.replaceInferableTypesWithTypeVars(type, locator);
        Type openedType =
            CS.openOpaqueType(replacedType, CTP_Initialization, locator);
        assert(openedType);

        auto *subPattern = cast<TypedPattern>(pattern)->getSubPattern();
        // Determine the subpattern type. It will be convertible to the
        // ascribed type.
        Type subPatternType = getTypeForPattern(
            subPattern,
            locator.withPathElement(LocatorPathElt::PatternMatch(subPattern)),
            bindPatternVarsOneWay);

        // NOTE: The order here is important! Pattern matching equality is
        // not symmetric (we need to fix that either by using a different
        // constraint, or actually making it symmetric).
        CS.addConstraint(
            ConstraintKind::Equal, openedType, subPatternType,
            locator.withPathElement(LocatorPathElt::PatternMatch(pattern)));

        // FIXME [OPAQUE SUPPORT]: the distinction between where we want opaque
        // types in opened vs. un-unopened form is *very* tricky. The pattern
        // ultimately needs the un-opened type and it gets this from the set
        // type of the expression.
        CS.setType(pattern, replacedType);
        return openedType;
      }

      case PatternKind::Tuple: {
        auto tuplePat = cast<TuplePattern>(pattern);

        SmallVector<TupleTypeElt, 4> tupleTypeElts;
        tupleTypeElts.reserve(tuplePat->getNumElements());
        for (unsigned i = 0, e = tuplePat->getNumElements(); i != e; ++i) {
          auto &tupleElt = tuplePat->getElement(i);

          auto *eltPattern = tupleElt.getPattern();
          Type eltTy = getTypeForPattern(
              eltPattern,
              locator.withPathElement(LocatorPathElt::PatternMatch(eltPattern)),
              bindPatternVarsOneWay);

          tupleTypeElts.push_back(TupleTypeElt(eltTy, tupleElt.getLabel()));
        }

        return setType(TupleType::get(tupleTypeElts, CS.getASTContext()));
      }

      case PatternKind::OptionalSome: {
        auto *subPattern = cast<OptionalSomePattern>(pattern)->getSubPattern();
        // The subpattern must have optional type.
        Type subPatternType = getTypeForPattern(
            subPattern,
            locator.withPathElement(LocatorPathElt::PatternMatch(subPattern)),
            bindPatternVarsOneWay);

        return setType(OptionalType::get(subPatternType));
      }

      case PatternKind::Is: {
        auto isPattern = cast<IsPattern>(pattern);

        const Type castType = resolveTypeReferenceInExpression(
            isPattern->getCastTypeRepr(), TypeResolverContext::InExpression,
            locator.withPathElement(LocatorPathElt::PatternMatch(pattern)));

        // Allow `is` pattern to infer type from context which is then going
        // to be propaged down to its sub-pattern via conversion. This enables
        // correct handling of patterns like `_ as Foo` where `_` would
        // get a type of `Foo` but `is` pattern enclosing it could still be
        // inferred from enclosing context.
        auto isType =
            CS.createTypeVariable(CS.getConstraintLocator(pattern),
                                  TVO_CanBindToNoEscape | TVO_CanBindToHole);

        // Make sure we can cast from the subpattern type to the type we're
        // checking; if it's impossible, fail.
        CS.addConstraint(
            ConstraintKind::CheckedCast, isType, castType,
            locator.withPathElement(LocatorPathElt::PatternMatch(pattern)));

        if (auto *subPattern = isPattern->getSubPattern()) {
          auto subPatternType = getTypeForPattern(
              subPattern,
              locator.withPathElement(LocatorPathElt::PatternMatch(subPattern)),
              bindPatternVarsOneWay);

          // NOTE: The order here is important! Pattern matching equality is
          // not symmetric (we need to fix that either by using a different
          // constraint, or actually making it symmetric).
          CS.addConstraint(
              ConstraintKind::Equal, castType, subPatternType,
              locator.withPathElement(LocatorPathElt::PatternMatch(pattern)));
        }
        return setType(isType);
      }

      case PatternKind::Bool:
        return setType(CS.getASTContext().getBoolType());

      case PatternKind::EnumElement: {
        auto enumPattern = cast<EnumElementPattern>(pattern);

        // Create a type variable to represent the pattern.
        Type patternType =
            CS.createTypeVariable(CS.getConstraintLocator(locator),
                                  TVO_CanBindToNoEscape);

        // Form the member constraint for a reference to a member of this
        // type.
        Type baseType;
        Type memberType = CS.createTypeVariable(
            CS.getConstraintLocator(locator),
            TVO_CanBindToLValue | TVO_CanBindToNoEscape);

        // Tuple splat is still allowed for patterns (with a warning in Swift 5)
        // so we need to start here from single-apply to make sure that e.g.
        // `case test(x: Int, y: Int)` gets the labels preserved when matched
        // with `case let .test(tuple)`.
        FunctionRefKind functionRefKind = FunctionRefKind::SingleApply;
        // If sub-pattern is a tuple we'd need to mark reference as compound,
        // that would make sure that the labels are dropped in cases
        // when `case` has a single tuple argument (tuple explosion) or multiple
        // arguments (tuple-to-tuple conversion).
        if (dyn_cast_or_null<TuplePattern>(enumPattern->getSubPattern()))
          functionRefKind = FunctionRefKind::Compound;

        auto patternLocator =
            locator.withPathElement(LocatorPathElt::PatternMatch(pattern));

        if (enumPattern->getParentType() || enumPattern->getParentTypeRepr()) {
          // Resolve the parent type.
          const auto parentType = [&] {
            auto *const patternMatchLoc = CS.getConstraintLocator(
                locator, {LocatorPathElt::PatternMatch(pattern),
                          ConstraintLocator::ParentType});

            // FIXME: Sometimes the parent type is realized eagerly in
            // ResolvePattern::visitUnresolvedDotExpr, so we have to open it
            // ex post facto. Remove this once we learn how to resolve patterns
            // while generating constraints to keep the opening of generic types
            // contained within the type resolver.
            if (const auto preresolvedTy = enumPattern->getParentType()) {
              const auto openedTy =
                  CS.replaceInferableTypesWithTypeVars(preresolvedTy,
                                                       patternMatchLoc);
              assert(openedTy);
              return openedTy;
            }

            return resolveTypeReferenceInExpression(
                enumPattern->getParentTypeRepr(),
                TypeResolverContext::InExpression, patternMatchLoc);
          }();

          // Perform member lookup into the parent's metatype.
          Type parentMetaType = MetatypeType::get(parentType);
          CS.addValueMemberConstraint(parentMetaType, enumPattern->getName(),
                                      memberType, CurDC, functionRefKind, {},
                                      patternLocator);

          // Parent type needs to be convertible to the pattern type; this
          // accounts for cases where the pattern type is existential.
          CS.addConstraint(
              ConstraintKind::Conversion, parentType, patternType,
              patternLocator.withPathElement(
                  ConstraintLocator::EnumPatternImplicitCastMatch));

          baseType = parentType;
        } else {
          // Use the pattern type for member lookup.
          CS.addUnresolvedValueMemberConstraint(
              MetatypeType::get(patternType), enumPattern->getName(),
              memberType, CurDC, functionRefKind, patternLocator);

          baseType = patternType;
        }

        if (auto subPattern = enumPattern->getSubPattern()) {
          // When there is a subpattern, the member will have function type,
          // and we're matching the type of that subpattern to the parameter
          // types.
          Type subPatternType = getTypeForPattern(
              subPattern,
              locator.withPathElement(LocatorPathElt::PatternMatch(subPattern)),
              bindPatternVarsOneWay);

          SmallVector<AnyFunctionType::Param, 4> params;
          decomposeTuple(subPatternType, params);

          // Remove parameter labels; they aren't used when matching cases,
          // but outright conflicts will be checked during coercion.
          for (auto &param : params) {
            param = param.getWithoutLabels();
          }

          Type outputType = CS.createTypeVariable(
              CS.getConstraintLocator(locator),
              TVO_CanBindToNoEscape);
          // Equal constraints require ExtInfo comparison.
          // FIXME: Verify ExtInfo state is correct, not working by accident.
          FunctionType::ExtInfo info;
          Type functionType = FunctionType::get(params, outputType, info);

          // TODO: Convert to own constraint? Note that ApplicableFn isn't quite
          // right, as pattern matching has data flowing *into* the apply result
          // and call arguments, not the other way around.
          // NOTE: The order here is important! Pattern matching equality is
          // not symmetric (we need to fix that either by using a different
          // constraint, or actually making it symmetric).
          CS.addConstraint(ConstraintKind::Equal, functionType, memberType,
                           patternLocator);

          CS.addConstraint(ConstraintKind::Conversion, outputType, baseType,
                           patternLocator);
        }

        return setType(patternType);
      }

      case PatternKind::Expr: {
        // We generate constraints for ExprPatterns in a separate pass. For
        // now, just create a type variable.
        return setType(CS.createTypeVariable(CS.getConstraintLocator(locator),
                                             TVO_CanBindToNoEscape));
      }
      }

      llvm_unreachable("Unhandled pattern kind");
    }

    Type visitCaptureListExpr(CaptureListExpr *expr) {
      // The type of the capture list is just the type of its closure.
      return CS.getType(expr->getClosureBody());
    }

    Type visitClosureExpr(ClosureExpr *closure) {
      auto *locator = CS.getConstraintLocator(closure);
      auto closureType = CS.createTypeVariable(locator, TVO_CanBindToNoEscape);

      // Collect any variable references whose types involve type variables,
      // because there will be a dependency on those type variables once we have
      // generated constraints for the closure body. This includes references
      // to other closure params such as in `{ x in { x }}` where the inner
      // closure is dependent on the outer closure's param type, as well as
      // cases like `for i in x where bar({ i })` where there's a dependency on
      // the type variable for the pattern `i`.
      struct CollectVarRefs : public ASTWalker {
        ConstraintSystem &cs;
        llvm::SmallPtrSet<TypeVariableType *, 4> varRefs;

        CollectVarRefs(ConstraintSystem &cs) : cs(cs) { }

        bool shouldWalkCaptureInitializerExpressions() override { return true; }

        MacroWalking getMacroWalkingBehavior() const override {
          return MacroWalking::Arguments;
        }

        PreWalkResult<Expr *> walkToExprPre(Expr *expr) override {
          // Retrieve type variables from references to var decls.
          if (auto *declRef = dyn_cast<DeclRefExpr>(expr)) {
            if (auto *varDecl = dyn_cast<VarDecl>(declRef->getDecl())) {
              if (auto varType = cs.getTypeIfAvailable(varDecl)) {
                varType->getTypeVariables(varRefs);
              }
            }
          }

          // FIXME: We can see UnresolvedDeclRefExprs here because we have
          // not yet run preCheckExpression() on the entire closure body
          // yet.
          //
          // We could consider pre-checking more eagerly.
          if (auto *declRef = dyn_cast<UnresolvedDeclRefExpr>(expr)) {
            auto name = declRef->getName();
            auto loc = declRef->getLoc();
            if (name.isSimpleName() && loc.isValid()) {
              auto *varDecl = dyn_cast_or_null<VarDecl>(
                ASTScope::lookupSingleLocalDecl(cs.DC->getParentSourceFile(),
                                                name.getFullName(), loc));
              if (varDecl)
                if (auto varType = cs.getTypeIfAvailable(varDecl))
                  varType->getTypeVariables(varRefs);
            }
          }

          return Action::Continue(expr);
        }

        PreWalkAction walkToDeclPre(Decl *D) override {
          return Action::VisitChildrenIf(isa<PatternBindingDecl>(D));
        }
      } collectVarRefs(CS);

      // Walk the capture list if this closure has one,  because it could
      // reference declarations from the outer closure.
      if (auto *captureList =
              getAsExpr<CaptureListExpr>(CS.getParentExpr(closure))) {
        captureList->walk(collectVarRefs);
      } else {
        closure->walk(collectVarRefs);
      }

      auto inferredType = inferClosureType(closure);
      if (!inferredType || inferredType->hasError())
        return Type();

      SmallVector<TypeVariableType *, 4> referencedVars{
          collectVarRefs.varRefs.begin(), collectVarRefs.varRefs.end()};

      CS.addUnsolvedConstraint(
          Constraint::create(CS, ConstraintKind::FallbackType, closureType,
                             inferredType, locator, referencedVars));

      CS.setClosureType(closure, inferredType);
      return closureType;
    }

    Type visitAutoClosureExpr(AutoClosureExpr *expr) {
      // AutoClosureExpr is introduced by CSApply.
      llvm_unreachable("Already type-checked");
    }

    Type visitInOutExpr(InOutExpr *expr) {
      // The address-of operator produces an explicit inout T from an lvalue T.
      // We model this with the constraint
      //
      //     S < lvalue T
      //
      // where T is a fresh type variable.
      auto lvalue = CS.createTypeVariable(CS.getConstraintLocator(expr),
                                          TVO_CanBindToNoEscape);
      auto bound = LValueType::get(lvalue);
      auto result = InOutType::get(lvalue);
      CS.addConstraint(ConstraintKind::Conversion,
                       CS.getType(expr->getSubExpr()), bound,
                       CS.getConstraintLocator(expr));
      return result;
    }

    Type visitVarargExpansionExpr(VarargExpansionExpr *expr) {
      // Create a fresh type variable.
      auto element = CS.createTypeVariable(CS.getConstraintLocator(expr),
                                           TVO_CanBindToNoEscape);

      // Try to build the appropriate type for a variadic argument list of
      // the fresh element type.  If that failed, just bail out.
      auto variadicSeq = VariadicSequenceType::get(element);

      // Require the operand to be convertible to the array type.
      CS.addConstraint(ConstraintKind::Conversion,
                       CS.getType(expr->getSubExpr()), variadicSeq,
                       CS.getConstraintLocator(expr));
      return variadicSeq;
    }

    void collectExpandedPacks(PackExpansionExpr *expansion,
                              SmallVectorImpl<ASTNode> &packs) {
      struct PackCollector : public ASTWalker {
      private:
        ConstraintSystem &CS;
        SmallVectorImpl<ASTNode> &Packs;

      public:
        PackCollector(ConstraintSystem &cs, SmallVectorImpl<ASTNode> &packs)
            : CS(cs), Packs(packs) {}

        /// Walk everything that's available.
        MacroWalking getMacroWalkingBehavior() const override {
          return MacroWalking::ArgumentsAndExpansion;
        }

        virtual PreWalkResult<Expr *> walkToExprPre(Expr *E) override {
          // Don't walk into nested pack expansions
          if (isa<PackExpansionExpr>(E)) {
            return Action::SkipChildren(E);
          }

          if (isa<PackElementExpr>(E)) {
            Packs.push_back(E);
          }

          if (auto *declRef = dyn_cast<DeclRefExpr>(E)) {
            auto type = CS.getTypeIfAvailable(declRef);
            if (!type)
              return Action::Continue(E);

            if (type->is<ElementArchetypeType>() &&
                CS.hasFixFor(CS.getConstraintLocator(declRef),
                             FixKind::IgnoreMissingEachKeyword)) {
              auto *packElementExpr =
                  PackElementExpr::create(CS.getASTContext(),
                                          /*eachLoc=*/{}, declRef,
                                          /*implicit=*/true, type);
              Packs.push_back(CS.cacheType(packElementExpr));
            }
          }

          return Action::Continue(E);
        }

        virtual PreWalkAction walkToTypeReprPre(TypeRepr *T) override {
          // Don't walk into nested pack expansions
          if (isa<PackExpansionTypeRepr>(T)) {
            return Action::SkipChildren();
          }

          if (isa<PackElementTypeRepr>(T)) {
            Packs.push_back(T);
          }

          return Action::Continue();
        }
      } packCollector(CS, packs);

      expansion->getPatternExpr()->walk(packCollector);
    }

    Type visitPackExpansionExpr(PackExpansionExpr *expr) {
      assert(PackElementEnvironments.back() == expr);
      PackElementEnvironments.pop_back();

      auto *patternLoc =
          CS.getConstraintLocator(expr, ConstraintLocator::PackExpansionPattern);
      auto patternTy = CS.createTypeVariable(patternLoc,
                                             TVO_CanBindToPack |
                                             TVO_CanBindToNoEscape |
                                             TVO_CanBindToHole);
      auto elementResultType = CS.getType(expr->getPatternExpr());
      CS.addConstraint(ConstraintKind::PackElementOf, elementResultType,
                       patternTy, CS.getConstraintLocator(expr));

      auto *shapeLoc =
          CS.getConstraintLocator(expr, ConstraintLocator::PackShape);
      auto *shapeTypeVar = CS.createTypeVariable(shapeLoc,
                                                 TVO_CanBindToPack |
                                                 TVO_CanBindToHole);

      // Generate ShapeOf constraints between all packs expanded by this
      // pack expansion expression through the shape type variable.
      SmallVector<ASTNode, 2> expandedPacks;
      collectExpandedPacks(expr, expandedPacks);

      if (expandedPacks.empty()) {
        (void)CS.recordFix(AllowValueExpansionWithoutPackReferences::create(
            CS, CS.getConstraintLocator(expr)));
      }

      for (auto pack : expandedPacks) {
        Type packType;
        if (auto *elementExpr = getAsExpr<PackElementExpr>(pack)) {
          packType = CS.getType(elementExpr->getPackRefExpr());
        } else if (auto *elementType = getAsTypeRepr<PackElementTypeRepr>(pack)) {
          // OpenPackElementType sets types for 'each T' type reprs in
          // expressions. Some invalid code won't make it there, and
          // the constraint system won't have recorded a type.
          if (!CS.hasType(elementType->getPackType()))
            return Type();

          packType = CS.getType(elementType->getPackType());
        } else {
          llvm_unreachable("unsupported pack reference ASTNode");
        }

        CS.addConstraint(
            ConstraintKind::ShapeOf, shapeTypeVar, packType,
            CS.getConstraintLocator(expr, ConstraintLocator::PackShape));
      }

      return PackExpansionType::get(patternTy, shapeTypeVar);
    }

    Type visitPackElementExpr(PackElementExpr *expr) {
      auto packType = CS.getType(expr->getPackRefExpr());

      if (isSingleUnlabeledPackExpansionTuple(packType)) {
        packType =
            addMemberRefConstraints(expr, expr->getPackRefExpr(),
                                    DeclNameRef(CS.getASTContext().Id_element),
                                    FunctionRefKind::Unapplied, {});
        CS.setType(expr->getPackRefExpr(), packType);
      }

      return openPackElement(packType, CS.getConstraintLocator(expr));
    }

    Type visitMaterializePackExpr(MaterializePackExpr *expr) {
      llvm_unreachable("MaterializePackExpr already type-checked");
    }

    Type visitDynamicTypeExpr(DynamicTypeExpr *expr) {
      auto tv = CS.createTypeVariable(CS.getConstraintLocator(expr),
                                      TVO_CanBindToNoEscape);
      CS.addConstraint(
          ConstraintKind::DynamicTypeOf, tv, CS.getType(expr->getBase()),
          CS.getConstraintLocator(expr, ConstraintLocator::DynamicType));
      return tv;
    }

    Type visitOpaqueValueExpr(OpaqueValueExpr *expr) {
      return expr->getType();
    }

    Type visitPropertyWrapperValuePlaceholderExpr(
        PropertyWrapperValuePlaceholderExpr *expr) {
      if (auto ty = expr->getType()) {
        CS.cacheType(expr);
        return ty;
      }

      assert(CS.getType(expr));
      return CS.getType(expr);
    }

    Type visitAppliedPropertyWrapperExpr(AppliedPropertyWrapperExpr *expr) {
      return expr->getType();
    }

    Type visitDefaultArgumentExpr(DefaultArgumentExpr *expr) {
      return expr->getType();
    }

    Type visitApplyExpr(ApplyExpr *expr) {
      auto fnExpr = expr->getFn();

      CS.associateArgumentList(CS.getConstraintLocator(expr), expr->getArgs());

      if (auto *UDE = dyn_cast<UnresolvedDotExpr>(fnExpr)) {
        auto typeOperation = getTypeOperation(UDE, CS.getASTContext());
        if (typeOperation != TypeOperation::None)
          return resultOfTypeOperation(typeOperation, expr->getArgs());
      }

      // The result type is a fresh type variable.
      Type resultType = CS.createTypeVariable(
          CS.getConstraintLocator(expr, ConstraintLocator::FunctionResult),
          TVO_CanBindToNoEscape);

      // A direct call to a ClosureExpr makes it noescape.
      FunctionType::ExtInfo extInfo;
      if (isa<ClosureExpr>(fnExpr->getSemanticsProvidingExpr()))
        extInfo = extInfo.withNoEscape();

      SmallVector<AnyFunctionType::Param, 8> params;
      getMatchingParams(expr->getArgs(), params);

      CS.addConstraint(ConstraintKind::ApplicableFunction,
                       FunctionType::get(params, resultType, extInfo),
                       CS.getType(fnExpr),
        CS.getConstraintLocator(expr, ConstraintLocator::ApplyFunction));

      // If we ended up resolving the result type variable to a concrete type,
      // set it as the favored type for this expression.
      Type fixedType =
          CS.getFixedTypeRecursive(resultType, /*wantRvalue=*/true);
      if (!fixedType->isTypeVariableOrMember()) {
        CS.setFavoredType(expr, fixedType.getPointer());
        resultType = fixedType;
      }

      return resultType;
    }

    Type getSuperType(VarDecl *selfDecl,
                      SourceLoc diagLoc,
                      Diag<> diag_not_in_class,
                      Diag<> diag_no_base_class) {
      DeclContext *typeContext = selfDecl->getDeclContext()->getParent();
      assert(typeContext && "constructor without parent context?!");

      auto &de = CS.getASTContext().Diags;
      ClassDecl *classDecl = typeContext->getSelfClassDecl();
      if (!classDecl) {
        de.diagnose(diagLoc, diag_not_in_class);
        return Type();
      }
      if (!classDecl->hasSuperclass()) {
        de.diagnose(diagLoc, diag_no_base_class);
        return Type();
      }

      auto selfTy = CS.DC->mapTypeIntoContext(
        typeContext->getDeclaredInterfaceType());
      auto superclassTy = selfTy->getSuperclass();

      if (!superclassTy)
        return Type();

      if (selfDecl->getInterfaceType()->is<MetatypeType>())
        superclassTy = MetatypeType::get(superclassTy);

      return superclassTy;
    }
    
    Type visitRebindSelfInConstructorExpr(RebindSelfInConstructorExpr *expr) {
      // The result is void.
      return TupleType::getEmpty(CS.getASTContext());
    }

    Type visitTernaryExpr(TernaryExpr *expr) {
      // Condition must convert to Bool.
      auto boolDecl = CS.getASTContext().getBoolDecl();
      if (!boolDecl)
        return Type();

      CS.addConstraint(
          ConstraintKind::Conversion, CS.getType(expr->getCondExpr()),
          boolDecl->getDeclaredInterfaceType(),
          CS.getConstraintLocator(expr, ConstraintLocator::Condition));

      // The branches must be convertible to a common type.
      return CS.addJoinConstraint(
          CS.getConstraintLocator(expr),
          {{CS.getType(expr->getThenExpr()),
            CS.getConstraintLocator(expr, LocatorPathElt::TernaryBranch(true))},
           {CS.getType(expr->getElseExpr()),
            CS.getConstraintLocator(expr,
                                    LocatorPathElt::TernaryBranch(false))}});
    }

    virtual Type visitImplicitConversionExpr(ImplicitConversionExpr *expr) {
      llvm_unreachable("Already type-checked");
    }

    Type
    createTypeVariableAndDisjunctionForIUOCoercion(Type toType,
                                                   ConstraintLocator *locator) {
      auto typeVar = CS.createTypeVariable(locator, TVO_CanBindToNoEscape);
      CS.buildDisjunctionForImplicitlyUnwrappedOptional(typeVar, toType,
                                                        locator);
      return typeVar;
    }

    Type getTypeForCast(ExplicitCastExpr *E) {
      if (auto *const repr = E->getCastTypeRepr()) {
        // Validate the resulting type.
        return resolveTypeReferenceInExpression(
            repr, TypeResolverContext::ExplicitCastExpr,
            CS.getConstraintLocator(E));
      }
      assert(E->isImplicit());
      return E->getCastType();
    }

    Type visitForcedCheckedCastExpr(ForcedCheckedCastExpr *expr) {
      auto fromExpr = expr->getSubExpr();
      if (!fromExpr) // Either wasn't constructed correctly or wasn't folded.
        return nullptr;

      auto toType = getTypeForCast(expr);
      if (!toType)
        return Type();

      auto *const repr = expr->getCastTypeRepr();

      // Cache the type we're casting to.
      if (repr) CS.setType(repr, toType);

      auto fromType = CS.getType(fromExpr);
      auto locator = CS.getConstraintLocator(expr);

      // The source type can be checked-cast to the destination type.
      CS.addConstraint(ConstraintKind::CheckedCast, fromType, toType, locator);

      // If the result type was declared IUO, add a disjunction for
      // bindings for the result of the coercion.
      if (repr && repr->getKind() == TypeReprKind::ImplicitlyUnwrappedOptional)
        return createTypeVariableAndDisjunctionForIUOCoercion(toType, locator);

      return toType;
    }

    Type visitCoerceExpr(CoerceExpr *expr) {
      // Validate the resulting type.
      auto toType = getTypeForCast(expr);
      if (!toType)
        return nullptr;

      auto *const repr = expr->getCastTypeRepr();

      // Cache the type we're casting to.
      if (repr) CS.setType(repr, toType);

      auto fromType = CS.getType(expr->getSubExpr());
      auto locator =
          CS.getConstraintLocator(expr, ConstraintLocator::CoercionOperand);

      // Literal initialization (e.g. `UInt32(0)`) doesn't require
      // a conversion because the literal is supposed to assume the
      // `to` type.
      //
      // `to` type could be a type variable if i.e. the repr is invalid,
      // in such cases a slower conversion path is a better choice to
      // let it be inferred from the context and/or from the literal itself.
      if (expr->isLiteralInit() && !toType->isTypeVariableOrMember()) {
        CS.addConstraint(ConstraintKind::Equal, fromType, toType, locator);
      } else {
        // Add a conversion constraint for the direct conversion between
        // types.
        CS.addExplicitConversionConstraint(fromType, toType, RememberChoice,
                                           locator);
      }

      // If the result type was declared IUO, add a disjunction for
      // bindings for the result of the coercion.
      if (repr &&
          repr->getKind() == TypeReprKind::ImplicitlyUnwrappedOptional) {
        return createTypeVariableAndDisjunctionForIUOCoercion(
            toType, CS.getConstraintLocator(expr));
      }

      return toType;
    }

    Type visitConditionalCheckedCastExpr(ConditionalCheckedCastExpr *expr) {
      auto fromExpr = expr->getSubExpr();
      if (!fromExpr) // Either wasn't constructed correctly or wasn't folded.
        return nullptr;

      // Validate the resulting type.
      const auto toType = getTypeForCast(expr);
      if (!toType)
        return nullptr;

      auto *const repr = expr->getCastTypeRepr();

      // Cache the type we're casting to.
      if (repr) CS.setType(repr, toType);

      auto fromType = CS.getType(fromExpr);
      auto locator = CS.getConstraintLocator(expr);

      CS.addConstraint(ConstraintKind::CheckedCast, fromType, toType, locator);

      // If the result type was declared IUO, add a disjunction for
      // bindings for the result of the coercion.
      if (repr && repr->getKind() == TypeReprKind::ImplicitlyUnwrappedOptional)
        return createTypeVariableAndDisjunctionForIUOCoercion(
            OptionalType::get(toType), locator);

      return OptionalType::get(toType);
    }

    Type visitIsExpr(IsExpr *expr) {
      auto toType = getTypeForCast(expr);
      if (!toType)
        return nullptr;

      auto *const repr = expr->getCastTypeRepr();
      // Cache the type we're checking.
      if (repr)
        CS.setType(repr, toType);

      // Add a checked cast constraint.
      auto fromType = CS.getType(expr->getSubExpr());
      
      CS.addConstraint(ConstraintKind::CheckedCast, fromType, toType,
                       CS.getConstraintLocator(expr));

      auto &ctx = CS.getASTContext();
      // The result is Bool.
      auto boolDecl = ctx.getBoolDecl();

      if (!boolDecl) {
        ctx.Diags.diagnose(SourceLoc(), diag::broken_bool);
        return Type();
      }

      return boolDecl->getDeclaredInterfaceType();
    }

    Type visitDiscardAssignmentExpr(DiscardAssignmentExpr *expr) {
      auto locator = CS.getConstraintLocator(expr);
      auto typeVar = CS.createTypeVariable(locator, TVO_CanBindToNoEscape);
      return LValueType::get(typeVar);
    }

    static Type genAssignDestType(Expr *expr, ConstraintSystem &CS) {
      if (auto *TE = dyn_cast<TupleExpr>(expr)) {
        SmallVector<TupleTypeElt, 4> destTupleTypes;
        for (unsigned i = 0; i !=  TE->getNumElements(); ++i) {
          Type subType = genAssignDestType(TE->getElement(i), CS);
          destTupleTypes.push_back(TupleTypeElt(subType, TE->getElementName(i)));
        }
        return TupleType::get(destTupleTypes, CS.getASTContext());
      } else {
        auto *locator = CS.getConstraintLocator(expr);

        auto isOrCanBeLValueType = [](Type type) {
          if (auto *typeVar = type->getAs<TypeVariableType>()) {
            return typeVar->getImpl().canBindToLValue();
          }
          return type->is<LValueType>();
        };

        auto exprType = CS.getType(expr);
        if (!isOrCanBeLValueType(exprType)) {
          // Pretend that destination is an l-value type.
          exprType = LValueType::get(exprType);
          (void)CS.recordFix(TreatRValueAsLValue::create(CS, locator));
        }

        auto *destTy = CS.createTypeVariable(locator, TVO_CanBindToNoEscape);
        CS.addConstraint(ConstraintKind::Bind, LValueType::get(destTy),
                         exprType, locator);
        return destTy;
      }
    }

    Type visitAssignExpr(AssignExpr *expr) {
      // Handle invalid code.
      if (!expr->getDest() || !expr->getSrc())
        return Type();
      Type destTy = genAssignDestType(expr->getDest(), CS);
      CS.addConstraint(ConstraintKind::Conversion,
                       CS.getType(expr->getSrc()), destTy,
                       CS.getConstraintLocator(expr));
      return TupleType::getEmpty(CS.getASTContext());
    }
    
    Type visitUnresolvedPatternExpr(UnresolvedPatternExpr *expr) {
      // Encountering an UnresolvedPatternExpr here means we have an invalid
      // ExprPattern with a Pattern node like 'let x' nested in it. Record a
      // fix, and assign ErrorTypes to any VarDecls bound.
      auto *locator = CS.getConstraintLocator(expr);
      auto *P = expr->getSubPattern();
      CS.recordFix(IgnoreInvalidPatternInExpr::create(CS, P, locator));

      P->forEachVariable([&](VarDecl *VD) {
        CS.setType(VD, ErrorType::get(CS.getASTContext()));
      });

      return CS.createTypeVariable(locator, TVO_CanBindToHole);
    }

    /// Get the type T?
    ///
    ///  This is not the ideal source location, but it's only used for
    /// diagnosing ill-formed standard libraries, so it really isn't
    /// worth QoI efforts.
    Type getOptionalType(SourceLoc optLoc, Type valueTy) {
      auto optTy = TypeChecker::getOptionalType(optLoc, valueTy);
      if (optTy->hasError() ||
          TypeChecker::requireOptionalIntrinsics(CS.getASTContext(), optLoc))
        return Type();

      return optTy;
    }

    Type visitBindOptionalExpr(BindOptionalExpr *expr) {
      // The operand must be coercible to T?, and we will have type T.
      auto locator = CS.getConstraintLocator(expr);

      auto objectTy = CS.createTypeVariable(locator,
                                            TVO_PrefersSubtypeBinding |
                                            TVO_CanBindToLValue |
                                            TVO_CanBindToNoEscape);
      
      // The result is the object type of the optional subexpression.
      CS.addConstraint(ConstraintKind::OptionalObject,
                       CS.getType(expr->getSubExpr()), objectTy,
                       locator);
      return objectTy;
    }
    
    Type visitOptionalEvaluationExpr(OptionalEvaluationExpr *expr) {
      // The operand must be coercible to T? for some type T.  We'd
      // like this to be the smallest possible nesting level of
      // optional types, e.g. T? over T??; otherwise we don't really
      // have a preference.
      auto valueTy = CS.createTypeVariable(CS.getConstraintLocator(expr),
                                           TVO_PrefersSubtypeBinding |
                                           TVO_CanBindToNoEscape);

      Type optTy = getOptionalType(expr->getSubExpr()->getLoc(), valueTy);
      if (!optTy)
        return Type();

      CS.addConstraint(ConstraintKind::Conversion,
                       CS.getType(expr->getSubExpr()), optTy,
                       CS.getConstraintLocator(expr));
      return optTy;
    }

    Type visitForceValueExpr(ForceValueExpr *expr) {
      // Force-unwrap an optional of type T? to produce a T.
      auto locator = CS.getConstraintLocator(expr);

      auto objectTy = CS.createTypeVariable(locator,
                                            TVO_PrefersSubtypeBinding |
                                            TVO_CanBindToLValue |
                                            TVO_CanBindToNoEscape);

      auto *valueExpr = expr->getSubExpr();
      // It's invalid to force unwrap `nil` literal e.g. `_ = nil!` or
      // `_ = (try nil)!` and similar constructs.
      if (auto *nilLiteral = dyn_cast<NilLiteralExpr>(
              valueExpr->getSemanticsProvidingExpr())) {
        CS.recordFix(SpecifyContextualTypeForNil::create(
            CS, CS.getConstraintLocator(nilLiteral)));
      }

      // The result is the object type of the optional subexpression.
      CS.addConstraint(ConstraintKind::OptionalObject, CS.getType(valueExpr),
                       objectTy, locator);
      return objectTy;
    }

    Type visitOpenExistentialExpr(OpenExistentialExpr *expr) {
      llvm_unreachable("Already type-checked");
    }
    Type visitMakeTemporarilyEscapableExpr(MakeTemporarilyEscapableExpr *expr) {
      llvm_unreachable("Already type-checked");
    }
    Type visitKeyPathApplicationExpr(KeyPathApplicationExpr *expr) {
      // This should only appear in already-type-checked solutions, but we may
      // need to re-check for failure diagnosis.
      auto locator = CS.getConstraintLocator(expr);
      auto projectedTy = CS.createTypeVariable(locator,
                                               TVO_CanBindToLValue |
                                               TVO_CanBindToNoEscape);
      CS.addKeyPathApplicationConstraint(CS.getType(expr->getKeyPath()),
                                         CS.getType(expr->getBase()),
                                         projectedTy,
                                         locator);
      return projectedTy;
    }
    
    Type visitEnumIsCaseExpr(EnumIsCaseExpr *expr) {
      return CS.getASTContext().getBoolType();
    }

    Type visitLazyInitializerExpr(LazyInitializerExpr *expr) {
      llvm_unreachable("Already type-checked");
    }

    Type visitEditorPlaceholderExpr(EditorPlaceholderExpr *E) {
      auto *locator = CS.getConstraintLocator(E);

      if (auto *placeholderRepr = E->getPlaceholderTypeRepr()) {
        // Let's try to use specified type, if that's impossible,
        // fallback to a type variable.
        if (auto preferredTy = resolveTypeReferenceInExpression(
                placeholderRepr, TypeResolverContext::InExpression, locator))
          return preferredTy;
      }

      // A placeholder may have any type, but default to Void type if
      // otherwise unconstrained.
      auto *placeholderTy =
          CS.createTypeVariable(locator, TVO_CanBindToNoEscape);

      CS.addConstraint(ConstraintKind::Defaultable, placeholderTy,
                       TupleType::getEmpty(CS.getASTContext()), locator);

      return placeholderTy;
    }

    Type visitObjCSelectorExpr(ObjCSelectorExpr *E) {
      // #selector only makes sense when we have the Objective-C
      // runtime.
      auto &ctx = CS.getASTContext();
      if (!ctx.LangOpts.EnableObjCInterop) {
        ctx.Diags.diagnose(E->getLoc(), diag::expr_selector_no_objc_runtime);
        return nullptr;
      }

      
      // Make sure we can reference ObjectiveC.Selector.
      // FIXME: Fix-It to add the import?
      auto type = CS.getASTContext().getSelectorType();
      if (!type) {
        ctx.Diags.diagnose(E->getLoc(), diag::expr_selector_module_missing);
        return nullptr;
      }

      return type;
    }

    Type visitKeyPathExpr(KeyPathExpr *E) {
      if (E->isObjC())
        return CS.getType(E->getObjCStringLiteralExpr());
      
      auto kpDecl = CS.getASTContext().getKeyPathDecl();
      
      if (!kpDecl) {
        auto &de = CS.getASTContext().Diags;
        de.diagnose(E->getLoc(), diag::expr_keypath_no_keypath_type);
        return ErrorType::get(CS.getASTContext());
      }
      
      // For native key paths, traverse the key path components to set up
      // appropriate type relationships at each level.
      auto rootLocator =
          CS.getConstraintLocator(E, ConstraintLocator::KeyPathRoot);
      auto locator = CS.getConstraintLocator(E);
      Type root = CS.createTypeVariable(rootLocator, TVO_CanBindToNoEscape |
                                        TVO_CanBindToHole);

      // If a root type was explicitly given, then resolve it now.
      if (auto rootRepr = E->getRootType()) {
        const auto rootObjectTy = resolveTypeReferenceInExpression(
            rootRepr, TypeResolverContext::InExpression, locator);
        if (!rootObjectTy || rootObjectTy->hasError())
          return Type();

        CS.setType(rootRepr, rootObjectTy);
        // Allow \Derived.property to be inferred as \Base.property to
        // simulate a sort of covariant conversion from
        // KeyPath<Derived, T> to KeyPath<Base, T>.
        CS.addConstraint(ConstraintKind::Subtype, rootObjectTy, root, locator);
      }
      
      bool didOptionalChain = false;
      // We start optimistically from an lvalue base.
      Type base = LValueType::get(root);

      SmallVector<TypeVariableType *, 2> componentTypeVars;
      for (unsigned i : indices(E->getComponents())) {
        auto &component = E->getComponents()[i];
        auto memberLocator = CS.getConstraintLocator(
            locator, LocatorPathElt::KeyPathComponent(i));
        auto resultLocator = CS.getConstraintLocator(
            memberLocator, ConstraintLocator::KeyPathComponentResult);

        switch (auto kind = component.getKind()) {
        case KeyPathExpr::Component::Kind::Invalid:
          break;
        case KeyPathExpr::Component::Kind::CodeCompletion:
          // We don't know what the code completion might resolve to, so we are
          // creating a new type variable for its result, which might be a hole.
          base = CS.createTypeVariable(
              resultLocator,
              TVO_CanBindToLValue | TVO_CanBindToNoEscape | TVO_CanBindToHole);
          break;
        case KeyPathExpr::Component::Kind::UnresolvedProperty:
        // This should only appear in resolved ASTs, but we may need to
        // re-type-check the constraints during failure diagnosis.
        case KeyPathExpr::Component::Kind::Property: {
          auto memberTy = CS.createTypeVariable(resultLocator,
                                                TVO_CanBindToLValue |
                                                TVO_CanBindToNoEscape);
          componentTypeVars.push_back(memberTy);
          auto lookupName = kind == KeyPathExpr::Component::Kind::UnresolvedProperty
            ? DeclNameRef(component.getUnresolvedDeclName()) // FIXME: type change needed
            : component.getDeclRef().getDecl()->createNameRef();
          
          auto refKind = lookupName.isSimpleName()
            ? FunctionRefKind::Unapplied
            : FunctionRefKind::Compound;
          CS.addValueMemberConstraint(base, lookupName,
                                      memberTy,
                                      CurDC,
                                      refKind,
                                      /*outerAlternatives=*/{},
                                      memberLocator);
          base = memberTy;
          break;
        }
          
        case KeyPathExpr::Component::Kind::UnresolvedSubscript:
        // Subscript should only appear in resolved ASTs, but we may need to
        // re-type-check the constraints during failure diagnosis.
        case KeyPathExpr::Component::Kind::Subscript: {
          auto *args = component.getSubscriptArgs();
          base = addSubscriptConstraints(E, base, /*decl*/ nullptr, args,
                                         memberLocator, &componentTypeVars);
          break;
        }

        case KeyPathExpr::Component::Kind::TupleElement: {
          // Note: If implemented, the logic in `getCalleeLocator` will need
          // updating to return the correct callee locator for this.
          llvm_unreachable("not implemented");
          break;
        }
                
        case KeyPathExpr::Component::Kind::OptionalChain: {
          didOptionalChain = true;

          // We can't assign an optional back through an optional chain
          // today. Force the base to an rvalue.
          auto rvalueTy = CS.createTypeVariable(resultLocator,
                                                TVO_CanBindToNoEscape);
          componentTypeVars.push_back(rvalueTy);
          CS.addConstraint(ConstraintKind::Equal, base, rvalueTy,
                           resultLocator);

          base = rvalueTy;
          LLVM_FALLTHROUGH;
        }
        case KeyPathExpr::Component::Kind::OptionalForce: {
          auto optionalObjTy = CS.createTypeVariable(resultLocator,
                                                     TVO_CanBindToLValue |
                                                     TVO_CanBindToNoEscape);
          componentTypeVars.push_back(optionalObjTy);

          CS.addConstraint(ConstraintKind::OptionalObject, base, optionalObjTy,
                           resultLocator);
          base = optionalObjTy;
          break;
        }
        
        case KeyPathExpr::Component::Kind::OptionalWrap: {
          // This should only appear in resolved ASTs, but we may need to
          // re-type-check the constraints during failure diagnosis.
          base = OptionalType::get(base);
          break;
        }
        case KeyPathExpr::Component::Kind::Identity:
          break;
        case KeyPathExpr::Component::Kind::DictionaryKey:
          llvm_unreachable("DictionaryKey only valid in #keyPath");
          break;
        }

        // By now, `base` is the result type of this component. Set it in the
        // constraint system so we can find it later.
        CS.setType(E, i, base);
      }
      
      // If there was an optional chaining component, the end result must be
      // optional.
      if (didOptionalChain) {
        auto objTy = CS.createTypeVariable(locator, TVO_CanBindToNoEscape |
                                                        TVO_CanBindToHole);
        componentTypeVars.push_back(objTy);

        auto optTy = OptionalType::get(objTy);
        CS.addConstraint(ConstraintKind::Conversion, base, optTy,
                         locator);
        base = optTy;
      }

      auto baseLocator =
          CS.getConstraintLocator(E, ConstraintLocator::KeyPathValue);
      auto rvalueBase = CS.createTypeVariable(
          baseLocator, TVO_CanBindToNoEscape | TVO_CanBindToHole);
      CS.addConstraint(ConstraintKind::Equal, base, rvalueBase, locator);

      // The result is a KeyPath from the root to the end component.
      // The type of key path depends on the overloads chosen for the key
      // path components.
      auto typeLoc = CS.getConstraintLocator(
          locator, LocatorPathElt::KeyPathType(rvalueBase));

      Type kpTy = CS.createTypeVariable(typeLoc, TVO_CanBindToNoEscape |
                                                     TVO_CanBindToHole);
      CS.addKeyPathConstraint(kpTy, root, rvalueBase, componentTypeVars,
                              locator);
      return kpTy;
    }

    Type visitKeyPathDotExpr(KeyPathDotExpr *E) {
      llvm_unreachable("found KeyPathDotExpr in CSGen");
    }

    Type visitSingleValueStmtExpr(SingleValueStmtExpr *E) {
      llvm_unreachable("Handled by the walker directly");
    }

    Type visitOneWayExpr(OneWayExpr *expr) {
      auto locator = CS.getConstraintLocator(expr);
      auto resultTypeVar = CS.createTypeVariable(locator, 0);
      CS.addConstraint(ConstraintKind::OneWayEqual, resultTypeVar,
                       CS.getType(expr->getSubExpr()), locator);
      return resultTypeVar;
    }

    Type visitTapExpr(TapExpr *expr) {
      DeclContext *varDC = expr->getVar()->getDeclContext();
      assert(varDC == CS.DC || (varDC && isa<AbstractClosureExpr>(varDC)) &&
             "TapExpr var should be in the same DeclContext we're checking it in!");
      
      auto locator = CS.getConstraintLocator(expr);
      auto tv = CS.createTypeVariable(locator, TVO_CanBindToNoEscape);

      if (auto subExpr = expr->getSubExpr()) {
        auto subExprType = CS.getType(subExpr);
        CS.addConstraint(ConstraintKind::Bind, subExprType, tv, locator);
      }

      return tv;
    }

    Type visitTypeJoinExpr(TypeJoinExpr *expr) {
      auto *locator = CS.getConstraintLocator(expr);
      SmallVector<std::pair<Type, ConstraintLocator *>, 4> elements;
      elements.reserve(expr->getNumElements());

      if (auto *SVE = expr->getSingleValueStmtExpr()) {
        // If we have a SingleValueStmtExpr, form a join of the branch types.
        SmallVector<Expr *, 4> scratch;
        auto branches = SVE->getSingleExprBranches(scratch);
        for (auto idx : indices(branches)) {
          auto *eltLoc = CS.getConstraintLocator(
              SVE, {LocatorPathElt::SingleValueStmtBranch(idx)});
          elements.emplace_back(CS.getType(branches[idx]), eltLoc);
        }
      } else {
        for (auto *element : expr->getElements()) {
          elements.emplace_back(CS.getType(element),
                                CS.getConstraintLocator(element));
        }
      }

      Type resultTy;

      if (auto *var = expr->getVar()) {
        resultTy = CS.getType(var);
      } else {
        resultTy = expr->getType();
      }

      assert(resultTy);

      // If we have a single branch of a SingleValueStmtExpr, we want a
      // conversion of the result, not a join, which would skip the conversion.
      // This is needed to ensure we apply the Void/Never conversions.
      if (elements.size() == 1 && expr->getSingleValueStmtExpr()) {
        auto &elt = elements[0];
        CS.addConstraint(ConstraintKind::Conversion, elt.first,
                         resultTy, elt.second);
        return resultTy;
      }

      // The type of a join expression is obtained by performing
      // a "join-meet" operation on deduced types of its elements
      // and the underlying variable.
      auto joinedTy = CS.addJoinConstraint(locator, elements);

      CS.addConstraint(ConstraintKind::Equal, resultTy, joinedTy, locator);
      return resultTy;
    }

    /// Lookup all macros with the given macro name.
    SmallVector<OverloadChoice, 1>
    lookupMacros(Identifier macroName,
                 FunctionRefKind functionRefKind,
                 MacroRoles roles) {
      SmallVector<OverloadChoice, 1> choices;
      auto results = namelookup::lookupMacros(
          CurDC, DeclNameRef(macroName), roles);
      for (const auto &result : results) {
        OverloadChoice choice = OverloadChoice(Type(), result, functionRefKind);
        choices.push_back(choice);
      }

      // FIXME: At some point, we need to check for function-like macros without
      // arguments and vice-versa.

      return choices;
    }

    Type visitMacroExpansionExpr(MacroExpansionExpr *expr) {
      // Assign a discriminator.
      (void)expr->getDiscriminator();

      auto &ctx = CS.getASTContext();
      auto locator = CS.getConstraintLocator(expr);

      CS.associateArgumentList(locator, expr->getArgs());

      // Look up the macros with this name.
      auto macroIdent = expr->getMacroName().getBaseIdentifier();
      FunctionRefKind functionRefKind = FunctionRefKind::SingleApply;
      auto macros = lookupMacros(
          macroIdent, functionRefKind, expr->getMacroRoles());
      if (macros.empty()) {
        ctx.Diags.diagnose(expr->getMacroNameLoc(), diag::macro_undefined,
                           macroIdent)
            .highlight(expr->getMacroNameLoc().getSourceRange());
        return Type();
      }

      // Introduce an overload set for the macro reference.
      auto macroRefType = Type(CS.createTypeVariable(locator, 0));
      CS.addOverloadSet(macroRefType, macros, CurDC, locator);

      // Add explicit generic arguments, if there were any.
      if (expr->getGenericArgsRange().isValid()) {
        if (addSpecializationConstraint(
              CS.getConstraintLocator(expr), macroRefType,
              expr->getGenericArgs()))
          return Type();
      }

      // Form the applicable-function constraint. The result type
      // is the result of that call.
      SmallVector<AnyFunctionType::Param, 8> params;
      getMatchingParams(expr->getArgs(), params);

      Type resultType = CS.createTypeVariable(
          CS.getConstraintLocator(expr, ConstraintLocator::FunctionResult),
          TVO_CanBindToNoEscape);

      CS.addConstraint(
          ConstraintKind::ApplicableFunction,
          FunctionType::get(params, resultType),
          macroRefType,
          CS.getConstraintLocator(
            expr, ConstraintLocator::ApplyFunction));

      return resultType;
    }

    static bool isTriggerFallbackDiagnosticBuiltin(UnresolvedDotExpr *UDE,
                                                   ASTContext &Context) {
      auto *DRE = dyn_cast<DeclRefExpr>(UDE->getBase());
      if (!DRE)
        return false;

      if (DRE->getDecl() != Context.TheBuiltinModule)
        return false;

      auto member = UDE->getName().getBaseName().userFacingName();
      return member.equals("trigger_fallback_diagnostic");
    }

    enum class TypeOperation { None,
                               Join,
                               JoinInout,
                               JoinMeta,
                               JoinNonexistent,
                               OneWay,
    };

    static TypeOperation getTypeOperation(UnresolvedDotExpr *UDE,
                                          ASTContext &Context) {
      auto *DRE = dyn_cast<DeclRefExpr>(UDE->getBase());
      if (!DRE)
        return TypeOperation::None;

      if (DRE->getDecl() != Context.TheBuiltinModule)
        return TypeOperation::None;

      return llvm::StringSwitch<TypeOperation>(
                 UDE->getName().getBaseIdentifier().str())
          .Case("one_way", TypeOperation::OneWay)
          .Case("type_join", TypeOperation::Join)
          .Case("type_join_inout", TypeOperation::JoinInout)
          .Case("type_join_meta", TypeOperation::JoinMeta)
          .Case("type_join_nonexistent", TypeOperation::JoinNonexistent)
          .Default(TypeOperation::None);
    }

    Type resultOfTypeOperation(TypeOperation op, ArgumentList *Args) {
      auto *lhs = Args->getExpr(0);
      auto *rhs = Args->getExpr(1);

      switch (op) {
      case TypeOperation::None:
      case TypeOperation::OneWay:
        llvm_unreachable(
            "We should have a valid type operation at this point!");

      case TypeOperation::Join: {
        auto lhsMeta = CS.getType(lhs)->getAs<MetatypeType>();
        auto rhsMeta = CS.getType(rhs)->getAs<MetatypeType>();
        if (!lhsMeta || !rhsMeta)
          llvm_unreachable("Unexpected argument types for Builtin.type_join!");

        auto &ctx = lhsMeta->getASTContext();

        auto join =
            Type::join(lhsMeta->getInstanceType(), rhsMeta->getInstanceType());

        if (!join)
          return ErrorType::get(ctx);

        return MetatypeType::get(*join, ctx)->getCanonicalType();
      }

      case TypeOperation::JoinInout: {
        auto lhsInOut = CS.getType(lhs)->getAs<InOutType>();
        auto rhsMeta = CS.getType(rhs)->getAs<MetatypeType>();
        if (!lhsInOut || !rhsMeta)
          llvm_unreachable("Unexpected argument types for Builtin.type_join!");

        auto &ctx = lhsInOut->getASTContext();

        auto join =
            Type::join(lhsInOut, rhsMeta->getInstanceType());

        if (!join)
          return ErrorType::get(ctx);

        return MetatypeType::get(*join, ctx)->getCanonicalType();
      }

      case TypeOperation::JoinMeta: {
        auto lhsMeta = CS.getType(lhs)->getAs<MetatypeType>();
        auto rhsMeta = CS.getType(rhs)->getAs<MetatypeType>();
        if (!lhsMeta || !rhsMeta)
          llvm_unreachable("Unexpected argument types for Builtin.type_join!");

        auto &ctx = lhsMeta->getASTContext();

        auto join = Type::join(lhsMeta, rhsMeta);

        if (!join)
          return ErrorType::get(ctx);

        return *join;
      }

      case TypeOperation::JoinNonexistent: {
        auto lhsMeta = CS.getType(lhs)->getAs<MetatypeType>();
        auto rhsMeta = CS.getType(rhs)->getAs<MetatypeType>();
        if (!lhsMeta || !rhsMeta)
          llvm_unreachable("Unexpected argument types for Builtin.type_join_nonexistent!");

        auto &ctx = lhsMeta->getASTContext();

        auto join =
            Type::join(lhsMeta->getInstanceType(), rhsMeta->getInstanceType());

        // Verify that we could not compute a join.
        if (join)
          llvm_unreachable("Unexpected result from join - it should not have been computable!");

        // The return value is unimportant.
        return MetatypeType::get(ctx.getAnyExistentialType())->getCanonicalType();
      }
      }
      llvm_unreachable("unhandled operation");
    }

    /// Assuming that we are solving for code completion, assign \p expr a fresh
    /// and unconstrained type variable as its type.
    void setTypeForArgumentIgnoredForCompletion(Expr *expr) {
      assert(CS.isForCodeCompletion());
      ConstraintSystem &CS = getConstraintSystem();

      if (auto closure = dyn_cast<ClosureExpr>(expr)) {
        FunctionType *closureTy =
            inferClosureType(closure, /*allowResultBindToHole=*/true);
        CS.setClosureType(closure, closureTy);
        CS.setType(closure, closureTy);
      } else {
        TypeVariableType *exprType = CS.createTypeVariable(
            CS.getConstraintLocator(expr),
            TVO_CanBindToLValue | TVO_CanBindToInOut | TVO_CanBindToNoEscape |
                TVO_CanBindToHole);
        CS.setType(expr, exprType);
      }
    }
  };

  class ConstraintWalker : public ASTWalker {
    ConstraintGenerator &CG;

  public:
    ConstraintWalker(ConstraintGenerator &CG) : CG(CG) { }

    MacroWalking getMacroWalkingBehavior() const override {
      return MacroWalking::Arguments;
    }

    PreWalkResult<Expr *> walkToExprPre(Expr *expr) override {
      auto &CS = CG.getConstraintSystem();

      if (CS.isArgumentIgnoredForCodeCompletion(expr)) {
        CG.setTypeForArgumentIgnoredForCompletion(expr);
        return Action::SkipChildren(expr);
      }

      if (auto *SVE = dyn_cast<SingleValueStmtExpr>(expr)) {
        if (CS.generateConstraints(SVE))
          return Action::Stop();
        return Action::SkipChildren(expr);
      }

      // Note that the subexpression of a #selector expression is
      // unevaluated.
      if (auto sel = dyn_cast<ObjCSelectorExpr>(expr)) {
        auto *subExpr = sel->getSubExpr()->getSemanticsProvidingExpr();
        CG.getConstraintSystem().UnevaluatedRootExprs.insert(subExpr);
      }

      // Check an objc key-path expression, which fills in its semantic
      // expression as a string literal.
      if (auto keyPath = dyn_cast<KeyPathExpr>(expr)) {
        if (keyPath->isObjC()) {
          auto &cs = CG.getConstraintSystem();
          (void)TypeChecker::checkObjCKeyPathExpr(cs.DC, keyPath);
        }
      }

      // Generate constraints for each of the entries in the capture list.
      if (auto captureList = dyn_cast<CaptureListExpr>(expr)) {
        TypeChecker::diagnoseDuplicateCaptureVars(captureList);
      }

      // Both multi- and single-statement closures now behave the same way
      // when it comes to constraint generation.
      if (auto closure = dyn_cast<ClosureExpr>(expr)) {
        auto &CS = CG.getConstraintSystem();
        auto closureType = CG.visitClosureExpr(closure);
        if (!closureType)
          return Action::Stop();

        CS.setType(expr, closureType);
        return Action::SkipChildren(expr);
      }

      // Don't visit CoerceExpr with an empty sub expression. They may occur
      // if the body of a closure was not visited while pre-checking because
      // of an error in the closure's signature.
      if (auto coerceExpr = dyn_cast<CoerceExpr>(expr)) {
        if (!coerceExpr->getSubExpr()) {
          return Action::SkipChildren(expr);
        }
      }

      // Don't visit TernaryExpr with empty sub expressions. They may occur
      // if the body of a closure was not visited while pre-checking because
      // of an error in the closure's signature.
      if (auto *ternary = dyn_cast<TernaryExpr>(expr)) {
        if (!ternary->getThenExpr() || !ternary->getElseExpr())
          return Action::SkipChildren(expr);
      }

      if (CS.isForCodeCompletion()) {
        SmallVector<Expr *, 2> ignoredArgs;
        getArgumentsAfterCodeCompletionToken(expr, CS, ignoredArgs);
        for (auto ignoredArg : ignoredArgs) {
          CS.markArgumentIgnoredForCodeCompletion(ignoredArg);
        }
      }

      if (auto *expansion = dyn_cast<PackExpansionExpr>(expr)) {
        CG.addPackElementEnvironment(expansion);
      }

      return Action::Continue(expr);
    }

    /// Once we've visited the children of the given expression,
    /// generate constraints from the expression.
    PostWalkResult<Expr *> walkToExprPost(Expr *expr) override {
      auto &CS = CG.getConstraintSystem();
      // Translate special type-checker Builtin calls into simpler expressions.
      if (auto *apply = dyn_cast<ApplyExpr>(expr)) {
        auto fnExpr = apply->getFn();
        if (auto *UDE = dyn_cast<UnresolvedDotExpr>(fnExpr)) {
          auto typeOperation =
              ConstraintGenerator::getTypeOperation(UDE, CS.getASTContext());

          if (typeOperation == ConstraintGenerator::TypeOperation::OneWay) {
            // For a one-way constraint, create the OneWayExpr node.
            auto *unaryArg = apply->getArgs()->getUnlabeledUnaryExpr();
            assert(unaryArg);
            expr = new (CS.getASTContext()) OneWayExpr(unaryArg);
          } else if (typeOperation !=
                         ConstraintGenerator::TypeOperation::None) {
            // Handle the Builtin.type_join* family of calls by replacing
            // them with dot_self_expr of type_expr with the type being the
            // result of the join.
            auto joinMetaTy =
                CG.resultOfTypeOperation(typeOperation, apply->getArgs());
            auto joinTy = joinMetaTy->castTo<MetatypeType>();

            auto *TE = TypeExpr::createImplicit(joinTy->getInstanceType(),
                                                CS.getASTContext());
            CS.cacheType(TE);

            auto *DSE = new (CS.getASTContext())
                DotSelfExpr(TE, SourceLoc(), SourceLoc(), CS.getType(TE));
            DSE->setImplicit();
            CS.cacheType(DSE);

            return Action::Continue(DSE);
          }
        }
      }
      if (auto type = CG.visit(expr)) {
        auto simplifiedType = CS.simplifyType(type);
        CS.setType(expr, simplifiedType);
        return Action::Continue(expr);
      }
      return Action::Stop();
    }

    /// Ignore statements.
    PreWalkResult<Stmt *> walkToStmtPre(Stmt *stmt) override {
      return Action::SkipChildren(stmt);
    }

    /// Ignore declarations.
    PreWalkAction walkToDeclPre(Decl *decl) override {
      return Action::SkipChildren();
    }
  };
} // end anonymous namespace

static Expr *generateConstraintsFor(ConstraintSystem &cs, Expr *expr,
                                    DeclContext *DC) {
  // Walk the expression, generating constraints.
  ConstraintGenerator cg(cs, DC);
  ConstraintWalker cw(cg);

  Expr *result = expr->walk(cw);

  if (result)
    cs.optimizeConstraints(result);

  return result;
}

bool ConstraintSystem::generateWrappedPropertyTypeConstraints(
    VarDecl *wrappedVar, Type initializerType, Type propertyType) {
  auto dc = wrappedVar->getInnermostDeclContext();

  Type wrappedValueType;
  Type wrapperType;
  auto wrapperAttributes = wrappedVar->getAttachedPropertyWrappers();
  for (unsigned i : indices(wrapperAttributes)) {
    // FIXME: We should somehow pass an OpenUnboundGenericTypeFn to
    // AttachedPropertyWrapperTypeRequest::evaluate to open up unbound
    // generics on the fly.
    Type rawWrapperType = wrappedVar->getAttachedPropertyWrapperType(i);
    auto wrapperInfo = wrappedVar->getAttachedPropertyWrapperTypeInfo(i);
    if (rawWrapperType->hasError() || !wrapperInfo)
      return true;

    auto *typeExpr = wrapperAttributes[i]->getTypeExpr();

    if (!wrappedValueType) {
      // Equate the outermost wrapper type to the initializer type.
      auto *locator = getConstraintLocator(typeExpr);
      wrapperType =
          replaceInferableTypesWithTypeVars(rawWrapperType, locator);
      if (initializerType)
        addConstraint(ConstraintKind::Equal, wrapperType, initializerType, locator);
    } else {
      // The former wrappedValue type must be equal to the current wrapper type
      auto *locator = getConstraintLocator(
          typeExpr, LocatorPathElt::WrappedValue(wrapperType));
      wrapperType =
          replaceInferableTypesWithTypeVars(rawWrapperType, locator);
      addConstraint(ConstraintKind::Equal, wrapperType, wrappedValueType, locator);
    }

    setType(typeExpr, wrapperType);

    wrappedValueType = wrapperType->getTypeOfMember(
        dc->getParentModule(), wrapperInfo.valueVar);
  }

  // The property type must be equal to the wrapped value type
  addConstraint(
      ConstraintKind::Equal, propertyType, wrappedValueType,
      getConstraintLocator(
          wrappedVar, LocatorPathElt::ContextualType(CTP_WrappedProperty)));
  setContextualType(wrappedVar, TypeLoc::withoutLoc(wrappedValueType),
                    CTP_WrappedProperty);
  return false;
}

/// Generate additional constraints for the pattern of an initialization.
static bool generateInitPatternConstraints(ConstraintSystem &cs,
                                           SyntacticElementTarget target,
                                           Expr *initializer) {
  auto locator = cs.getConstraintLocator(
      initializer, LocatorPathElt::ContextualType(CTP_Initialization));

  Type patternType;
  if (auto pattern = target.getInitializationPattern()) {
    patternType = cs.generateConstraints(
        pattern, locator, target.shouldBindPatternVarsOneWay(),
        target.getInitializationPatternBindingDecl(),
        target.getInitializationPatternBindingIndex());
  } else {
    patternType = cs.createTypeVariable(locator, TVO_CanBindToNoEscape);
  }

  if (auto wrappedVar = target.getInitializationWrappedVar())
    return cs.generateWrappedPropertyTypeConstraints(
        wrappedVar, cs.getType(target.getAsExpr()), patternType);

  // Add a conversion constraint between the types.
  cs.addConstraint(ConstraintKind::Conversion, cs.getType(target.getAsExpr()),
                   patternType, locator, /*isFavored*/true);

  return false;
}

static Optional<SyntacticElementTarget>
generateForEachStmtConstraints(ConstraintSystem &cs,
                               SyntacticElementTarget target) {
  ASTContext &ctx = cs.getASTContext();
  auto forEachStmtInfo = target.getForEachStmtInfo();
  ForEachStmt *stmt = target.getAsForEachStmt();
  bool isAsync = stmt->getAwaitLoc().isValid();
  auto *sequenceExpr = stmt->getParsedSequence();
  auto *dc = target.getDeclContext();
  auto contextualLocator = cs.getConstraintLocator(
      sequenceExpr, LocatorPathElt::ContextualType(CTP_ForEachSequence));

  // The expression type must conform to the Sequence protocol.
  auto sequenceProto = TypeChecker::getProtocol(
      cs.getASTContext(), stmt->getForLoc(),
      isAsync ? KnownProtocolKind::AsyncSequence : KnownProtocolKind::Sequence);
  if (!sequenceProto)
    return None;

  std::string name;
  {
    if (auto np = dyn_cast_or_null<NamedPattern>(stmt->getPattern()))
      name = "$"+np->getBoundName().str().str();
    name += "$generator";
  }

  auto *makeIteratorVar = new (ctx)
      VarDecl(/*isStatic=*/false, VarDecl::Introducer::Var,
              sequenceExpr->getStartLoc(), ctx.getIdentifier(name), dc);
  makeIteratorVar->setImplicit();

  // First, let's form a call from sequence to `.makeIterator()` and save
  // that in a special variable which is going to be used by SILGen.
  {
    FuncDecl *makeIterator = isAsync ? ctx.getAsyncSequenceMakeAsyncIterator()
                                     : ctx.getSequenceMakeIterator();

    auto *makeIteratorRef = UnresolvedDotExpr::createImplicit(
        ctx, sequenceExpr, makeIterator->getName());
    makeIteratorRef->setFunctionRefKind(FunctionRefKind::SingleApply);

    auto *makeIteratorCall =
        CallExpr::createImplicitEmpty(ctx, makeIteratorRef);

    Pattern *pattern = NamedPattern::createImplicit(ctx, makeIteratorVar);
    auto *PB = PatternBindingDecl::createImplicit(
        ctx, StaticSpellingKind::None, pattern, makeIteratorCall, dc);

    auto makeIteratorTarget = SyntacticElementTarget::forInitialization(
        makeIteratorCall, dc, /*patternType=*/Type(), PB, /*index=*/0,
        /*shouldBindPatternsOneWay=*/false);

    cs.setContextualType(
        sequenceExpr,
        TypeLoc::withoutLoc(sequenceProto->getDeclaredInterfaceType()),
        CTP_ForEachSequence);

    if (cs.generateConstraints(makeIteratorTarget))
      return None;

    forEachStmtInfo.makeIteratorVar = PB;

    // Type of sequence expression has to conform to Sequence protocol.
    //
    // Note that the following emulates having `$generator` separately
    // type-checked by introducing a `TVO_PrefersSubtypeBinding` type
    // variable that would make sure that result of `.makeIterator` would
    // get ranked standalone.
    {
      auto *externalIteratorType = cs.createTypeVariable(
          cs.getConstraintLocator(sequenceExpr), TVO_PrefersSubtypeBinding);

      cs.addConstraint(ConstraintKind::Equal, externalIteratorType,
                       cs.getType(sequenceExpr),
                       externalIteratorType->getImpl().getLocator());

      cs.addConstraint(ConstraintKind::ConformsTo, externalIteratorType,
                       sequenceProto->getDeclaredInterfaceType(),
                       contextualLocator);

      forEachStmtInfo.sequenceType = cs.getType(sequenceExpr);
    }

    cs.setTargetFor({PB, /*index=*/0}, makeIteratorTarget);
  }

  // Now, result type of `.makeIterator()` is used to form a call to
  // `.next()`. `next()` is called on each iteration of the loop.
  {
    auto *nextRef = UnresolvedDotExpr::createImplicit(
        ctx,
        new (ctx) DeclRefExpr(makeIteratorVar, DeclNameLoc(stmt->getForLoc()),
                              /*Implicit=*/true),
        ctx.Id_next, /*labels=*/ArrayRef<Identifier>());
    nextRef->setFunctionRefKind(FunctionRefKind::SingleApply);

    Expr *nextCall = CallExpr::createImplicitEmpty(ctx, nextRef);

    // `next` is always async but witness might not be throwing
    if (isAsync) {
      nextCall =
          AwaitExpr::createImplicit(ctx, /*awaitLoc=*/SourceLoc(), nextCall);
    }

    // The iterator type must conform to IteratorProtocol.
    {
      ProtocolDecl *iteratorProto = TypeChecker::getProtocol(
          cs.getASTContext(), stmt->getForLoc(),
          isAsync ? KnownProtocolKind::AsyncIteratorProtocol
                  : KnownProtocolKind::IteratorProtocol);
      if (!iteratorProto)
        return None;

      cs.setContextualType(
          nextRef->getBase(),
          TypeLoc::withoutLoc(iteratorProto->getDeclaredInterfaceType()),
          CTP_ForEachSequence);
    }

    SyntacticElementTarget nextTarget(nextCall, dc, CTP_Unused,
                                      /*contextualType=*/Type(),
                                      /*isDiscarded=*/false);
    if (cs.generateConstraints(nextTarget, FreeTypeVariableBinding::Disallow))
      return None;

    forEachStmtInfo.nextCall = nextTarget.getAsExpr();
    cs.setTargetFor(forEachStmtInfo.nextCall, nextTarget);
  }

  Pattern *pattern = TypeChecker::resolvePattern(stmt->getPattern(), dc,
                                                 /*isStmtCondition*/ false);
  if (!pattern)
    return None;

  auto contextualPattern = ContextualPattern::forRawPattern(pattern, dc);
  Type patternType = TypeChecker::typeCheckPattern(contextualPattern);
  if (patternType->hasError()) {
    return None;
  }

  // Collect constraints from the element pattern.
  auto elementLocator = cs.getConstraintLocator(
      sequenceExpr, ConstraintLocator::SequenceElementType);
  Type initType =
      cs.generateConstraints(pattern, elementLocator,
                             target.shouldBindPatternVarsOneWay(), nullptr, 0);
  if (!initType)
    return None;

  // Add a conversion constraint between the element type of the sequence
  // and the type of the element pattern.
  auto *elementTypeLoc = cs.getConstraintLocator(
      elementLocator, ConstraintLocator::OptionalPayload);
  auto elementType = cs.createTypeVariable(elementTypeLoc,
                                           /*flags=*/0);
  {
    auto nextType = cs.getType(forEachStmtInfo.nextCall);
    // Note that `OptionalObject` is not used here. This is due to inference
    // behavior where it would bind `elementType` to the `initType` before
    // resolving `optional object` constraint which is sometimes too eager.
    cs.addConstraint(ConstraintKind::Conversion, nextType,
                     OptionalType::get(elementType), elementTypeLoc);
    cs.addConstraint(ConstraintKind::Conversion, elementType, initType,
                     elementLocator);
  }

  // Generate constraints for the "where" expression, if there is one.
  if (auto *whereExpr = stmt->getWhere()) {
    auto *boolDecl = dc->getASTContext().getBoolDecl();
    if (!boolDecl)
      return None;

    Type boolType = boolDecl->getDeclaredInterfaceType();
    if (!boolType)
      return None;

    SyntacticElementTarget whereTarget(whereExpr, dc, CTP_Condition, boolType,
                                       /*isDiscarded=*/false);
    if (cs.generateConstraints(whereTarget, FreeTypeVariableBinding::Disallow))
      return None;

    cs.setTargetFor(whereExpr, whereTarget);
    cs.setContextualType(whereExpr, TypeLoc::withoutLoc(boolType),
                         CTP_Condition);
  }

  // Populate all of the information for a for-each loop.
  forEachStmtInfo.elementType = elementType;
  forEachStmtInfo.initType = initType;
  target.setPattern(pattern);
  target.getForEachStmtInfo() = forEachStmtInfo;
  return target;
}

bool ConstraintSystem::generateConstraints(
    SyntacticElementTarget &target,
    FreeTypeVariableBinding allowFreeTypeVariables) {
  if (Expr *expr = target.getAsExpr()) {
    // If the target requires an optional of some type, form a new appropriate
    // type variable and update the target's type with an optional of that
    // type variable.
    if (target.isOptionalSomePatternInit()) {
      assert(!target.getExprContextualType() &&
             "some pattern cannot have contextual type pre-configured");
      auto *convertTypeLocator = getConstraintLocator(
          expr, LocatorPathElt::ContextualType(
                    target.getExprContextualTypePurpose()));
      Type var = createTypeVariable(convertTypeLocator, TVO_CanBindToNoEscape);
      target.setExprConversionType(TypeChecker::getOptionalType(expr->getLoc(), var));
    }

    expr = buildTypeErasedExpr(expr, target.getDeclContext(),
                               target.getExprContextualType(),
                               target.getExprContextualTypePurpose());

    // Generate constraints for the main system.
    expr = generateConstraints(expr, target.getDeclContext());
    if (!expr)
      return true;
    target.setExpr(expr);

    // If there is a type that we're expected to convert to, add the conversion
    // constraint.
    if (Type convertType = target.getExprConversionType()) {
      ContextualTypePurpose ctp = target.getExprContextualTypePurpose();

      // If a custom locator wasn't specified, create a locator anchored on
      // the expression itself.
      auto *convertTypeLocator = target.getExprConvertTypeLocator();
      if (!convertTypeLocator) {
        convertTypeLocator =
            getConstraintLocator(expr, LocatorPathElt::ContextualType(ctp));
      }

      auto getLocator = [&](Type ty) -> ConstraintLocator * {
        // If we have a placeholder originating from a PlaceholderTypeRepr,
        // tack that on to the locator.
        if (auto *placeholderTy = ty->getAs<PlaceholderType>())
          if (auto *placeholderRepr = placeholderTy->getOriginator()
                                          .dyn_cast<PlaceholderTypeRepr *>())
            return getConstraintLocator(
                convertTypeLocator,
                LocatorPathElt::PlaceholderType(placeholderRepr));
        return convertTypeLocator;
      };

      // Substitute type variables in for placeholder types (and unresolved
      // types, if allowed).
      if (allowFreeTypeVariables == FreeTypeVariableBinding::UnresolvedType) {
        convertType = convertType.transform([&](Type type) -> Type {
          if (type->is<UnresolvedType>() || type->is<PlaceholderType>()) {
            return createTypeVariable(getLocator(type),
                                      TVO_CanBindToNoEscape |
                                          TVO_PrefersSubtypeBinding |
                                          TVO_CanBindToHole);
          }
          return type;
        });
      } else {
        convertType = convertType.transform([&](Type type) -> Type {
          if (type->is<PlaceholderType>()) {
            return createTypeVariable(getLocator(type),
                                      TVO_CanBindToNoEscape |
                                          TVO_PrefersSubtypeBinding |
                                          TVO_CanBindToHole);
          }
          return type;
        });
      }

      addContextualConversionConstraint(expr, convertType, ctp,
                                        convertTypeLocator);
    }

    // For an initialization target, generate constraints for the pattern.
    if (target.isForInitialization() &&
        generateInitPatternConstraints(*this, target, expr)) {
      return true;
    }

    if (isDebugMode()) {
      auto &log = llvm::errs();
      log << "\n---Initial constraints for the given expression---\n";
      print(log, expr);
      log << "\n";
      print(log);
      log << "\n";
    }

    return false;
  }

  switch (target.kind) {
  case SyntacticElementTarget::Kind::expression:
    llvm_unreachable("Handled above");

  case SyntacticElementTarget::Kind::closure:
  case SyntacticElementTarget::Kind::caseLabelItem:
  case SyntacticElementTarget::Kind::function:
  case SyntacticElementTarget::Kind::stmtCondition:
    llvm_unreachable("Handled separately");

  case SyntacticElementTarget::Kind::patternBinding: {
    auto patternBinding = target.getAsPatternBinding();
    auto dc = target.getDeclContext();
    bool hadError = false;

    /// Generate constraints for each pattern binding entry
    for (unsigned index : range(patternBinding->getNumPatternEntries())) {
      auto *pattern = TypeChecker::resolvePattern(
          patternBinding->getPattern(index), dc, /*isStmtCondition=*/true);

      if (!pattern)
        return true;

      // Reset binding to point to the resolved pattern. This is required
      // before calling `forPatternBindingDecl`.
      patternBinding->setPattern(index, pattern,
                                 patternBinding->getInitContext(index));

      auto contextualPattern =
          ContextualPattern::forPatternBindingDecl(patternBinding, index);
      Type patternType = TypeChecker::typeCheckPattern(contextualPattern);

      // Fail early if pattern couldn't be type-checked.
      if (!patternType || patternType->hasError())
        return true;

      auto *init = patternBinding->getInit(index);

      if (!init && patternBinding->isDefaultInitializable(index) &&
          pattern->hasStorage()) {
        init = TypeChecker::buildDefaultInitializer(patternType);
      }

      auto target = init ? SyntacticElementTarget::forInitialization(
                               init, dc, patternType, patternBinding, index,
                               /*bindPatternVarsOneWay=*/true)
                         : SyntacticElementTarget::forUninitializedVar(
                               patternBinding, index, patternType);

      if (generateConstraints(target)) {
        hadError = true;
        continue;
      }

      // Keep track of this binding entry.
      setTargetFor({patternBinding, index}, target);
    }

    return hadError;
  }

  case SyntacticElementTarget::Kind::uninitializedVar: {
    if (auto *wrappedVar = target.getAsUninitializedWrappedVar()) {
      auto propertyType = getVarType(wrappedVar);
      if (propertyType->hasError())
        return true;

      return generateWrappedPropertyTypeConstraints(
        wrappedVar, /*initializerType=*/Type(), propertyType);
    } else {
      auto pattern = target.getAsUninitializedVar();
      auto locator = getConstraintLocator(
          pattern, LocatorPathElt::ContextualType(CTP_Initialization));

      // Generate constraints to bind all of the internal declarations
      // and verify the pattern.
      Type patternType = generateConstraints(
          pattern, locator, /*shouldBindPatternVarsOneWay*/ true,
          target.getPatternBindingOfUninitializedVar(),
          target.getIndexOfUninitializedVar());

      return !patternType;
    }
  }

  case SyntacticElementTarget::Kind::forEachStmt: {
    // For a for-each statement, generate constraints for the pattern, where
    // clause, and sequence traversal.
    auto resultTarget = generateForEachStmtConstraints(*this, target);
    if (!resultTarget)
      return true;

    target = *resultTarget;
    return false;
  }
  }
}

Expr *ConstraintSystem::generateConstraints(
    Expr *expr, DeclContext *dc, bool isInputExpression) {
  if (isInputExpression)
    InputExprs.insert(expr);
  return generateConstraintsFor(*this, expr, dc);
}

Type ConstraintSystem::generateConstraints(
    Pattern *pattern, ConstraintLocatorBuilder locator,
    bool bindPatternVarsOneWay, PatternBindingDecl *patternBinding,
    unsigned patternIndex) {
  ConstraintGenerator cg(*this, nullptr);
  auto ty = cg.getTypeForPattern(pattern, locator, bindPatternVarsOneWay,
                                 patternBinding, patternIndex);
  assert(ty);

  // Gather the ExprPatterns, and form a conjunction for their expressions.
  SmallVector<ExprPattern *, 4> exprPatterns;
  pattern->forEachNode([&](Pattern *P) {
    if (auto *EP = dyn_cast<ExprPattern>(P))
      exprPatterns.push_back(EP);
  });
  if (!exprPatterns.empty())
    generateConstraints(exprPatterns, getConstraintLocator(pattern));

  return ty;
}

bool ConstraintSystem::generateConstraints(StmtCondition condition,
                                           DeclContext *dc) {
  // FIXME: This should be folded into constraint generation for conditions.
  auto boolDecl = getASTContext().getBoolDecl();
  if (!boolDecl) {
    return true;
  }

  Type boolTy = boolDecl->getDeclaredInterfaceType();
  for (const auto &condElement : condition) {
    switch (condElement.getKind()) {
    case StmtConditionElement::CK_Availability:
      // Nothing to do here.
      continue;

    case StmtConditionElement::CK_HasSymbol: {
      Expr *symbolExpr = condElement.getHasSymbolInfo()->getSymbolExpr();
      auto target = SyntacticElementTarget(symbolExpr, dc, CTP_Unused, Type(),
                                           /*isDiscarded=*/false);

      if (generateConstraints(target))
        return true;

      setTargetFor(&condElement, target);
      continue;
    }

    case StmtConditionElement::CK_Boolean: {
      Expr *condExpr = condElement.getBoolean();
      auto target = SyntacticElementTarget(condExpr, dc, CTP_Condition, boolTy,
                                           /*isDiscarded=*/false);

      if (generateConstraints(target, FreeTypeVariableBinding::Disallow))
        return true;

      setTargetFor(&condElement, target);
      continue;
    }

    case StmtConditionElement::CK_PatternBinding: {
      auto *pattern = TypeChecker::resolvePattern(
          condElement.getPattern(), dc, /*isStmtCondition*/true);
      if (!pattern)
        return true;

      auto target = SyntacticElementTarget::forInitialization(
          condElement.getInitializer(), dc, Type(), pattern,
          /*bindPatternVarsOneWay=*/true);
      if (generateConstraints(target, FreeTypeVariableBinding::Disallow))
        return true;

      setTargetFor(&condElement, target);
      continue;
    }
    }
  }

  return false;
}

ConstraintSystem::TypeMatchResult
ConstraintSystem::applyPropertyWrapperToParameter(
    Type wrapperType, Type paramType, ParamDecl *param, Identifier argLabel,
    ConstraintKind matchKind, ConstraintLocatorBuilder locator) {
  Expr *anchor = getAsExpr(locator.getAnchor());
  if (auto *apply = dyn_cast<ApplyExpr>(anchor)) {
    anchor = apply->getFn();
  }

  if (argLabel.hasDollarPrefix() && (!param || !param->hasExternalPropertyWrapper())) {
    if (!shouldAttemptFixes())
      return getTypeMatchFailure(locator);

    recordAnyTypeVarAsPotentialHole(paramType);

    auto *loc = getConstraintLocator(locator);
    auto *fix = RemoveProjectedValueArgument::create(*this, wrapperType, param, loc);
    if (recordFix(fix))
      return getTypeMatchFailure(locator);

    return getTypeMatchSuccess();
  }

  if (argLabel.hasDollarPrefix()) {
    Type projectionType = computeProjectedValueType(param, wrapperType);
    addConstraint(matchKind, paramType, projectionType, locator);
    if (param->hasImplicitPropertyWrapper()) {
      auto wrappedValueType = getType(param->getPropertyWrapperWrappedValueVar());
      addConstraint(ConstraintKind::PropertyWrapper, projectionType, wrappedValueType,
                    getConstraintLocator(param));
      setType(param->getPropertyWrapperProjectionVar(), projectionType);
    }

    appliedPropertyWrappers[anchor].push_back({ wrapperType, PropertyWrapperInitKind::ProjectedValue });
  } else if (param->hasExternalPropertyWrapper()) {
    Type wrappedValueType = computeWrappedValueType(param, wrapperType);
    addConstraint(matchKind, paramType, wrappedValueType, locator);
    setType(param->getPropertyWrapperWrappedValueVar(), wrappedValueType);

    appliedPropertyWrappers[anchor].push_back({ wrapperType, PropertyWrapperInitKind::WrappedValue });
  } else {
    return getTypeMatchFailure(locator);
  }

  return getTypeMatchSuccess();
}

void ConstraintSystem::optimizeConstraints(Expr *e) {
  if (getASTContext().TypeCheckerOpts.DisableConstraintSolverPerformanceHacks)
    return;
  
  SmallVector<Expr *, 16> linkedExprs;
  
  // Collect any linked expressions.
  LinkedExprCollector collector(linkedExprs);
  e->walk(collector);
  
  // Favor types, as appropriate.
  for (auto linkedExpr : linkedExprs) {
    computeFavoredTypeForExpr(linkedExpr, *this);
  }
  
  // Optimize the constraints.
  ConstraintOptimizer optimizer(*this);
  e->walk(optimizer);
}

struct ResolvedMemberResult::Implementation {
  llvm::SmallVector<ValueDecl*, 4> AllDecls;
  unsigned ViableStartIdx;
  Optional<unsigned> BestIdx;
};

ResolvedMemberResult::ResolvedMemberResult(): Impl(new Implementation()) {}

ResolvedMemberResult::~ResolvedMemberResult() { delete Impl; }

ResolvedMemberResult::operator bool() const {
  return !Impl->AllDecls.empty();
}

bool ResolvedMemberResult::
hasBestOverload() const { return Impl->BestIdx.has_value(); }

ValueDecl* ResolvedMemberResult::
getBestOverload() const { return Impl->AllDecls[Impl->BestIdx.value()]; }

ArrayRef<ValueDecl*> ResolvedMemberResult::
getMemberDecls(InterestedMemberKind Kind) {
  auto Result = llvm::makeArrayRef(Impl->AllDecls);
  switch (Kind) {
  case InterestedMemberKind::Viable:
    return Result.slice(Impl->ViableStartIdx);
  case InterestedMemberKind::Unviable:
    return Result.slice(0, Impl->ViableStartIdx);
  case InterestedMemberKind::All:
    return Result;
  }
  llvm_unreachable("unhandled kind");
}

ResolvedMemberResult swift::resolveValueMember(DeclContext &DC, Type BaseTy,
                                               DeclName Name) {
  ResolvedMemberResult Result;
  ConstraintSystem CS(&DC, None);

  // Look up all members of BaseTy with the given Name.
  MemberLookupResult LookupResult = CS.performMemberLookup(
      ConstraintKind::ValueMember, DeclNameRef(Name), BaseTy,
      FunctionRefKind::SingleApply, CS.getConstraintLocator({}), false);

  // Keep track of all the unviable members.
  for (auto Can : LookupResult.UnviableCandidates)
    Result.Impl->AllDecls.push_back(Can.getDecl());

  // Keep track of the start of viable choices.
  Result.Impl->ViableStartIdx = Result.Impl->AllDecls.size();

  // If no viable members, we are done.
  if (LookupResult.ViableCandidates.empty())
    return Result;

  // If there's only one viable member, that is the best one.
  if (LookupResult.ViableCandidates.size() == 1) {
    Result.Impl->BestIdx = Result.Impl->AllDecls.size();
    Result.Impl->AllDecls.push_back(LookupResult.ViableCandidates[0].getDecl());
    return Result;
  }

  // Try to figure out the best overload.
  ConstraintLocator *Locator = CS.getConstraintLocator({});
  TypeVariableType *TV = CS.createTypeVariable(Locator,
                                               TVO_CanBindToLValue |
                                               TVO_CanBindToNoEscape);
  CS.addOverloadSet(TV, LookupResult.ViableCandidates, &DC, Locator);
  Optional<Solution> OpSolution = CS.solveSingle();
  ValueDecl *Selected = nullptr;
  if (OpSolution.has_value()) {
    Selected = OpSolution.value().overloadChoices[Locator].choice.getDecl();
  }
  for (OverloadChoice& Choice : LookupResult.ViableCandidates) {
    ValueDecl *VD = Choice.getDecl();

    // If this VD is the best overload, keep track of its index.
    if (VD == Selected)
      Result.Impl->BestIdx = Result.Impl->AllDecls.size();
    Result.Impl->AllDecls.push_back(VD);
  }
  return Result;
}
