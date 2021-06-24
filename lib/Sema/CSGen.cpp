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
#include "TypeCheckType.h"
#include "TypeChecker.h"
#include "swift/AST/ASTVisitor.h"
#include "swift/AST/ASTWalker.h"
#include "swift/AST/Expr.h"
#include "swift/AST/GenericSignature.h"
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
    ConstraintSystem &CS;

  public:
    LinkedExprCollector(llvm::SmallVectorImpl<Expr *> &linkedExprs,
                        ConstraintSystem &cs)
        : LinkedExprs(linkedExprs), CS(cs) {}

    std::pair<bool, Expr *> walkToExprPre(Expr *expr) override {

      if (CS.shouldReusePrecheckedType() &&
          !CS.getType(expr)->hasTypeVariable()) {
        return { false, expr };
      }

      if (isa<ClosureExpr>(expr))
        return {false, expr};

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
        return {false, expr};
      }
      
      return { true, expr };
    }
    
    Expr *walkToExprPost(Expr *expr) override {
      return expr;
    }
    
    /// Ignore statements.
    std::pair<bool, Stmt *> walkToStmtPre(Stmt *stmt) override {
      return { false, stmt };
    }
    
    /// Ignore declarations.
    bool walkToDeclPre(Decl *decl) override { return false; }

    /// Ignore patterns.
    std::pair<bool, Pattern*> walkToPatternPre(Pattern *pat) override {
      return { false, pat };
    }

    /// Ignore types.
     bool walkToTypeReprPre(TypeRepr *T) override { return false; }
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
    
    std::pair<bool, Expr *> walkToExprPre(Expr *expr) override {

      if (CS.shouldReusePrecheckedType() &&
          !CS.getType(expr)->hasTypeVariable()) {
        return { false, expr };
      }

      if (isa<LiteralExpr>(expr)) {
        LTI.hasLiteral = true;
        return { false, expr };
      }

      if (isa<CollectionExpr>(expr)) {
        return { true, expr };
      }
      
      if (auto UDE = dyn_cast<UnresolvedDotExpr>(expr)) {
        
        if (CS.hasType(UDE))
          LTI.collectedTypes.insert(CS.getType(UDE).getPointer());
        
        // Don't recurse into the base expression.
        return { false, expr };
      }


      if (isa<ClosureExpr>(expr)) {
        return {false, expr};
      }

      if (auto FVE = dyn_cast<ForceValueExpr>(expr)) {
        LTI.collectedTypes.insert(CS.getType(FVE).getPointer());
        return { false, expr };
      }

      if (auto DRE = dyn_cast<DeclRefExpr>(expr)) {
        if (auto varDecl = dyn_cast<VarDecl>(DRE->getDecl())) {
          if (CS.hasType(DRE)) {
            LTI.collectedTypes.insert(CS.getType(DRE).getPointer());
          }
          return { false, expr };
        } 
      }             

      // In the case of a function application, we would have already captured
      // the return type during constraint generation, so there's no use in
      // looking any further.
      if (isa<ApplyExpr>(expr) &&
          !(isa<BinaryExpr>(expr) || isa<PrefixUnaryExpr>(expr) ||
            isa<PostfixUnaryExpr>(expr))) {      
        return { false, expr };
      }

      if (isa<BinaryExpr>(expr)) {
        LTI.binaryExprs.push_back(dyn_cast<BinaryExpr>(expr));
      }  
      
      if (auto favoredType = CS.getFavoredType(expr)) {
        LTI.collectedTypes.insert(favoredType);

        return { false, expr };
      }

      // Optimize branches of a conditional expression separately.
      if (auto IE = dyn_cast<IfExpr>(expr)) {
        CS.optimizeConstraints(IE->getCondExpr());
        CS.optimizeConstraints(IE->getThenExpr());
        CS.optimizeConstraints(IE->getElseExpr());
        return { false, expr };
      }      

      // For exprs of a structural type that are not modeling argument lists,
      // avoid merging the type variables. (We need to allow for cases like
      // (Int, Int32).)
      if (isa<TupleExpr>(expr) && !isa<ApplyExpr>(Parent.getAsExpr())) {
        return { false, expr };
      }

      // Coercion exprs have a rigid type, so there's no use in gathering info
      // about them.
      if (auto *coercion = dyn_cast<CoerceExpr>(expr)) {
        // Let's not collect information about types initialized by
        // coercions just like we don't for regular initializer calls,
        // because that might lead to overly eager type variable merging.
        if (!coercion->isLiteralInit())
          LTI.collectedTypes.insert(CS.getType(expr).getPointer());
        return { false, expr };
      }

      // Don't walk into subscript expressions - to do so would risk factoring
      // the index expression into edge contraction. (We don't want to do this
      // if the index expression is a literal type that differs from the return
      // type of the subscript operation.)
      if (isa<SubscriptExpr>(expr) || isa<DynamicLookupExpr>(expr)) {
        return { false, expr };
      }
      
      // Don't walk into unresolved member expressions - we avoid merging type
      // variables inside UnresolvedMemberExpr and those outside, since they
      // should be allowed to behave independently in CS.
      if (isa<UnresolvedMemberExpr>(expr)) {
        return {false, expr };
      }

      return { true, expr };
    }
    
    /// Ignore statements.
    std::pair<bool, Stmt *> walkToStmtPre(Stmt *stmt) override {
      return { false, stmt };
    }
    
    /// Ignore declarations.
    bool walkToDeclPre(Decl *decl) override { return false; }

    /// Ignore patterns.
    std::pair<bool, Pattern*> walkToPatternPre(Pattern *pat) override {
      return { false, pat };
    }

    /// Ignore types.
    bool walkToTypeReprPre(TypeRepr *T) override { return false; }
  };
  
  /// For a given expression, given information that is global to the
  /// expression, attempt to derive a favored type for it.
  bool computeFavoredTypeForExpr(Expr *expr, ConstraintSystem &CS) {
    LinkedTypeInfo lti;

    expr->walk(LinkedExprAnalyzer(lti, CS));

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

      return true;
    }
    
    return false;
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
    if (argTy->isDouble() && paramTy->isCGFloatType())
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
            (defaultType->isDouble() && paramTy->isCGFloatType()))
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
  
  size_t getOperandCount(Type t) {
    size_t nOperands = 0;
    
    if (auto parenTy = dyn_cast<ParenType>(t.getPointer())) {
      if (parenTy->getDesugaredType())
        nOperands = 1;
    } else if (auto tupleTy = t->getAs<TupleType>()) {
      nOperands = tupleTy->getElementTypes().size();
    }
    
    return nOperands;
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
    // Determine whether the given declaration is favored.
    auto isFavoredDecl = [&](ValueDecl *value, Type type) -> bool {
      auto fnTy = type->getAs<AnyFunctionType>();
      if (!fnTy)
        return false;
      
      Type paramTy = FunctionType::composeInput(CS.getASTContext(),
                                                fnTy->getParams(), false);

      auto argTy = CS.getType(expr->getArg())
                       ->getWithoutParens()
                       ->getWithoutSpecifierType();

      // There are no CGFloat overloads on some of the unary operators, so
      // in order to preserve current behavior, let's not favor overloads
      // which would result in conversion from CGFloat to Double; otherwise
      // it would lead to ambiguities.
      if (argTy->isCGFloatType() && paramTy->isDouble())
        return false;

      return isFavoredParamAndArg(CS, paramTy, argTy) &&
             hasContextuallyFavorableResultType(fnTy,
                                                CS.getContextualType(expr));
    };

    favorCallOverloads(expr, CS, isFavoredDecl);
  }
  
  void favorMatchingOverloadExprs(ApplyExpr *expr,
                                  ConstraintSystem &CS) {
    // Find the argument type.
    size_t nArgs = getOperandCount(CS.getType(expr->getArg()));
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
    
    if (auto favoredTy = CS.getFavoredType(expr->getArg())) {
      // Determine whether the given declaration is favored.
      auto isFavoredDecl = [&](ValueDecl *value, Type type) -> bool {
        auto fnTy = type->getAs<AnyFunctionType>();
        if (!fnTy)
          return false;

        auto paramTy =
            AnyFunctionType::composeInput(CS.getASTContext(), fnTy->getParams(),
                                          /*canonicalVararg*/ false);
        return favoredTy->isEqual(paramTy);
      };

      // This is a hack to ensure we always consider the protocol requirement
      // itself when calling something that has a default implementation in an
      // extension. Otherwise, the extension method might be favored if we're
      // inside an extension context, since any archetypes in the parameter
      // list could match exactly.
      auto mustConsider = [&](ValueDecl *value) -> bool {
        return isa<ProtocolDecl>(value->getDeclContext());
      };

      favorCallOverloads(expr, CS,
                         isFavoredDecl,
                         mustConsider);
    }
  }
  
  /// Favor binary operator constraints where we have exact matches
  /// for the operands and contextual type.
  void favorMatchingBinaryOperators(ApplyExpr *expr,
                                    ConstraintSystem &CS) {
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
    auto argTy = CS.getType(expr->getArg());
    auto argTupleTy = argTy->castTo<TupleType>();
    auto argTupleExpr = dyn_cast<TupleExpr>(expr->getArg());

    Type firstArgTy = argTupleTy->getElement(0).getType()->getWithoutParens();
    Type secondArgTy = argTupleTy->getElement(1).getType()->getWithoutParens();

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

      Expr *firstArg = argTupleExpr->getElement(0);
      auto firstFavoredTy = CS.getFavoredType(firstArg);
      Expr *secondArg = argTupleExpr->getElement(1);
      auto secondFavoredTy = CS.getFavoredType(secondArg);
      
      auto favoredExprTy = CS.getFavoredType(expr);
      
      if (isArithmeticOperatorDecl(value)) {
        // If the parent has been favored on the way down, propagate that
        // information to its children.
        // TODO: This is only valid for arithmetic expressions.
        if (!firstFavoredTy) {
          CS.setFavoredType(argTupleExpr->getElement(0), favoredExprTy);
          firstFavoredTy = favoredExprTy;
        }
        
        if (!secondFavoredTy) {
          CS.setFavoredType(argTupleExpr->getElement(1), favoredExprTy);
          secondFavoredTy = favoredExprTy;
        }
      }
      
      auto params = fnTy->getParams();
      if (params.size() != 2)
        return false;

      auto firstParamTy = params[0].getOldType();
      auto secondParamTy = params[1].getOldType();

      auto contextualTy = CS.getContextualType(expr);

      // Avoid favoring overloads that would require narrowing conversion
      // to match the arguments.
      {
        if (firstArgTy->isDouble() && firstParamTy->isCGFloatType())
          return false;

        if (secondArgTy->isDouble() && secondParamTy->isCGFloatType())
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
  
  class ConstraintOptimizer : public ASTWalker {
    ConstraintSystem &CS;
    
  public:
    
    ConstraintOptimizer(ConstraintSystem &cs) :
      CS(cs) {}
    
    std::pair<bool, Expr *> walkToExprPre(Expr *expr) override {

      if (CS.shouldReusePrecheckedType() &&
          !CS.getType(expr)->hasTypeVariable()) {
        return { false, expr };
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
        return {false, expr};

      return { true, expr };
    }
    
    Expr *walkToExprPost(Expr *expr) override {
      return expr;
    }
    
    /// Ignore statements.
    std::pair<bool, Stmt *> walkToStmtPre(Stmt *stmt) override {
      return { false, stmt };
    }
    
    /// Ignore declarations.
    bool walkToDeclPre(Decl *decl) override { return false; }
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

    /// Returns false and emits the specified diagnostic if the member reference
    /// base is a nil literal. Returns true otherwise.
    bool isValidBaseOfMemberRef(Expr *base, Diag<> diagnostic) {
      if (auto nilLiteral = dyn_cast<NilLiteralExpr>(base)) {
        CS.getASTContext().Diags.diagnose(nilLiteral->getLoc(), diagnostic);
        return false;
      }
      return true;
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
      auto tv = CS.createTypeVariable(
                  CS.getConstraintLocator(expr, ConstraintLocator::Member),
                  TVO_CanBindToLValue | TVO_CanBindToNoEscape);
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
        Expr *anchor, Type baseTy, Expr *index,
        ValueDecl *declOrNull, ArrayRef<Identifier> argLabels,
        Optional<unsigned> unlabeledTrailingClosure,
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

      associateArgumentLabels(memberLocator,
                              {argLabels, unlabeledTrailingClosure});

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
        
        if (CS.isArrayType(baseObjTy.getPointer())) {

          if (auto arraySliceTy = 
                dyn_cast<ArraySliceType>(baseObjTy.getPointer())) {
            baseObjTy = arraySliceTy->getDesugaredType();
          }
          
          auto indexExpr = index;
          
          if (auto parenExpr = dyn_cast<ParenExpr>(indexExpr)) {
            indexExpr = parenExpr->getSubExpr();
          }
          
          if (isa<IntegerLiteralExpr>(indexExpr)) {
            
            outputTy = baseObjTy->getAs<BoundGenericType>()->getGenericArgs()[0];
            
            if (isLValueBase)
              outputTy = LValueType::get(outputTy);
          }
        } else if (auto dictTy = CS.isDictionaryType(baseObjTy)) {
          auto keyTy = dictTy->first;
          auto valueTy = dictTy->second;

          if (isFavoredParamAndArg(CS, keyTy, CS.getType(index))) {
            outputTy = OptionalType::get(valueTy);
            
            if (isLValueBase)
              outputTy = LValueType::get(outputTy);
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

      // FIXME: Redesign the AST so that an ApplyExpr directly stores a list of
      // arguments together with their inout-ness, instead of a single
      // ParenExpr or TupleExpr.
      SmallVector<AnyFunctionType::Param, 8> params;
      AnyFunctionType::decomposeInput(CS.getType(index), params);

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

    virtual Type visitErrorExpr(ErrorExpr *E) {
      if (!CS.isForCodeCompletion())
        return nullptr;

      // For code completion, treat error expressions that don't contain
      // the completion location itself as holes. If an ErrorExpr contains the
      // code completion location, a fallback typecheck is called on the
      // ErrorExpr's OriginalExpr (valid sub-expression) if it had one,
      // independent of the wider expression containing the ErrorExpr, so
      // there's no point attempting to produce a solution for it.
      if (CS.containsCodeCompletionLoc(E))
        return nullptr;

      return PlaceholderType::get(CS.getASTContext(), E);
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
      associateArgumentLabels(
          exprLoc, {expr->getArgumentLabels(),
                    expr->getUnlabeledTrailingClosureIndex()});

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

      auto *memberType =
          CS.createTypeVariable(memberLoc, TVO_CanBindToNoEscape);

      CS.addValueMemberConstraint(MetatypeType::get(witnessType, ctx),
                                  DeclNameRef(constrName), memberType, CurDC,
                                  FunctionRefKind::DoubleApply, {}, memberLoc);

      SmallVector<AnyFunctionType::Param, 8> args;
      AnyFunctionType::decomposeInput(CS.getType(expr->getArg()), args);

      auto resultType = CS.createTypeVariable(
          CS.getConstraintLocator(expr, ConstraintLocator::FunctionResult),
          TVO_CanBindToNoEscape);

      CS.addConstraint(
          ConstraintKind::ApplicableFunction,
          FunctionType::get(args, resultType), memberType,
          CS.getConstraintLocator(expr, ConstraintLocator::ApplyFunction));

      if (constr->isFailable())
        return OptionalType::get(witnessType);

      return witnessType;
    }

    Type visitDeclRefExpr(DeclRefExpr *E) {
      auto locator = CS.getConstraintLocator(E);

      Type knownType;
      if (auto *VD = dyn_cast<VarDecl>(E->getDecl())) {
        knownType = CS.getTypeIfAvailable(VD);
        if (!knownType)
          knownType = CS.getVarType(VD);

        if (knownType) {
          // If the known type has an error, bail out.
          if (knownType->hasError()) {
            auto *hole = CS.createTypeVariable(locator, TVO_CanBindToHole);
            (void)CS.recordFix(AllowRefToInvalidDecl::create(CS, locator));
            if (!CS.hasType(E))
              CS.setType(E, hole);
            return hole;
          }

          if (!knownType->hasPlaceholder()) {
            // Set the favored type for this expression to the known type.
            CS.setFavoredType(E, knownType.getPointer());
          }
        }
      }

      // If declaration is invalid, let's turn it into a potential hole
      // and keep generating constraints.
      if (!knownType && E->getDecl()->isInvalid()) {
        auto *hole = CS.createTypeVariable(locator, TVO_CanBindToHole);
        (void)CS.recordFix(AllowRefToInvalidDecl::create(CS, locator));
        CS.setType(E, hole);
        return hole;
      }

      // Create an overload choice referencing this declaration and immediately
      // resolve it. This records the overload for use later.
      auto tv = CS.createTypeVariable(locator,
                                      TVO_CanBindToLValue |
                                      TVO_CanBindToNoEscape);

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
      // Introduce type variables for unbound generics.
      const auto genericOpener = OpenUnboundGenericType(CS, locator);
      const auto placeholderHandler = HandlePlaceholderType(CS, locator);
      const auto result = TypeResolution::forContextual(CS.DC, resCtx,
                                                        genericOpener,
                                                        placeholderHandler)
              .resolveType(repr);
      if (result->hasError()) {
        return Type();
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
      } else if (CS.hasType(E)) {
        // If there's a type already set into the constraint system, honor it.
        // FIXME: This supports the result builder transform, which sneakily
        // stashes a type in the constraint system through a TypeExpr in order
        // to pass it down to the rest of CSGen. This is a terribly
        // unprincipled thing to do.
        return CS.getType(E);
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
      
      for (unsigned i = 0, n = decls.size(); i != n; ++i) {
        // If the result is invalid, skip it.
        // FIXME: Note this as invalid, in case we don't find a solution,
        // so we don't let errors cascade further.
        if (decls[i]->isInvalid())
          continue;

        OverloadChoice choice =
            OverloadChoice(Type(), decls[i], expr->getFunctionRefKind());
        choices.push_back(choice);
      }

      // If there are no valid overloads, give up.
      if (choices.empty())
        return nullptr;

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
        return ctx.TheAnyType;

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

        auto methodTy = CS.createTypeVariable(
            CS.getConstraintLocator(expr,
                                    ConstraintLocator::ApplyFunction),
            TVO_CanBindToNoEscape);

        // FIXME: Once TVO_PrefersSubtypeBinding is replaced with something
        // better, we won't need the second type variable at all.
        {
          auto argTy = CS.createTypeVariable(
              CS.getConstraintLocator(expr,
                                      ConstraintLocator::ApplyArgument),
              (TVO_CanBindToLValue |
               TVO_CanBindToInOut |
               TVO_CanBindToNoEscape |
               TVO_PrefersSubtypeBinding));
          CS.addConstraint(
              ConstraintKind::FunctionInput, methodTy, argTy,
              CS.getConstraintLocator(expr));
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

    Type visitUnresolvedSpecializeExpr(UnresolvedSpecializeExpr *expr) {
      auto baseTy = CS.getType(expr->getSubExpr());
      
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
        if (BoundGenericType *bgt
              = meta->getInstanceType()->getAs<BoundGenericType>()) {
          ArrayRef<Type> typeVars = bgt->getGenericArgs();
          auto specializations = expr->getUnresolvedParams();

          // If we have too many generic arguments, complain.
          if (specializations.size() > typeVars.size()) {
            de.diagnose(expr->getSubExpr()->getLoc(),
                        diag::type_parameter_count_mismatch,
                        bgt->getDecl()->getName(),
                        typeVars.size(), specializations.size(),
                        false)
              .highlight(SourceRange(expr->getLAngleLoc(),
                                     expr->getRAngleLoc()));
            de.diagnose(bgt->getDecl(), diag::kind_declname_declared_here,
                        DescriptiveDeclKind::GenericType,
                        bgt->getDecl()->getName());
            return Type();
          }

          // Bind the specified generic arguments to the type variables in the
          // open type.
          auto *const locator = CS.getConstraintLocator(expr);
          const auto options =
              TypeResolutionOptions(TypeResolverContext::InExpression);
          for (size_t i = 0, size = specializations.size(); i < size; ++i) {
            const auto resolution = TypeResolution::forContextual(
                CS.DC, options,
                // Introduce type variables for unbound generics.
                OpenUnboundGenericType(CS, locator),
                HandlePlaceholderType(CS, locator));
            const auto result = resolution.resolveType(specializations[i]);
            if (result->hasError())
              return Type();

            CS.addConstraint(ConstraintKind::Bind, typeVars[i], result,
                             locator);
          }
          
          return baseTy;
        } else {
          de.diagnose(expr->getSubExpr()->getLoc(), diag::not_a_generic_type,
                      meta->getInstanceType());
          de.diagnose(expr->getLAngleLoc(),
                      diag::while_parsing_as_left_angle_bracket);
          return Type();
        }
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

      // Prior to Swift 5, 'try?' always adds an additional layer of optionality,
      // even if the sub-expression was already optional.
      if (CS.getASTContext().LangOpts.isSwiftVersionAtLeast(5)) {
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
      if (auto favoredTy = CS.getFavoredType(expr->getSubExpr())) {
        CS.setFavoredType(expr, favoredTy);
      }

      auto &ctx = CS.getASTContext();
      auto parenType = CS.getType(expr->getSubExpr())->getInOutObjectType();
      auto parenFlags = ParameterTypeFlags().withInOut(expr->isSemanticallyInOutExpr());
      return ParenType::get(ctx, parenType, parenFlags);
    }

    Type visitTupleExpr(TupleExpr *expr) {
      // The type of a tuple expression is simply a tuple of the types of
      // its subexpressions.
      SmallVector<TupleTypeElt, 4> elements;
      elements.reserve(expr->getNumElements());
      for (unsigned i = 0, n = expr->getNumElements(); i != n; ++i) {
        auto *elt = expr->getElement(i);
        auto ty = CS.getType(elt);
        auto flags = ParameterTypeFlags()
            .withInOut(elt->isSemanticallyInOutExpr())
            .withVariadic(isa<VarargExpansionExpr>(elt));
        elements.push_back(TupleTypeElt(ty->getInOutObjectType(),
                                        expr->getElementName(i), flags));
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

      return addSubscriptConstraints(expr, CS.getType(base),
                                     expr->getIndex(),
                                     decl, expr->getArgumentLabels(),
                                     expr->getUnlabeledTrailingClosureIndex());
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
      auto contextualType = CS.getContextualType(expr);

      auto joinElementTypes = [&](Optional<Type> elementType) {
        const auto elements = expr->getElements();
        unsigned index = 0;

        using Iterator = decltype(elements)::iterator;
        CS.addJoinConstraint<Iterator>(locator, elements.begin(), elements.end(),
                                       elementType, [&](const auto it) {
          auto *locator = CS.getConstraintLocator(expr, LocatorPathElt::TupleElement(index++));
          return std::make_pair(CS.getType(*it), locator);
        });
      };

      // If a contextual type exists for this expression, apply it directly.
      Optional<Type> arrayElementType;
      if (contextualType &&
          (arrayElementType = ConstraintSystem::isArrayType(contextualType))) {
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
                       CS.getASTContext().TheAnyType, locator);

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
      auto contextualType = CS.getContextualType(expr);
      Type contextualDictionaryType = nullptr;
      Type contextualDictionaryKeyType = nullptr;
      Type contextualDictionaryValueType = nullptr;
      
      // If a contextual type exists for this expression, apply it directly.
      Optional<std::pair<Type, Type>> dictionaryKeyValue;
      if (contextualType &&
          (dictionaryKeyValue = ConstraintSystem::isDictionaryType(contextualType))) {
        // Is the contextual type a dictionary type?
        contextualDictionaryType = contextualType;
        std::tie(contextualDictionaryKeyType,
                 contextualDictionaryValueType) = *dictionaryKeyValue;
        
        // Form an explicit tuple type from the contextual type's key and value types.
        TupleTypeElt tupleElts[2] = { TupleTypeElt(contextualDictionaryKeyType),
                                      TupleTypeElt(contextualDictionaryValueType) };
        Type contextualDictionaryElementType = TupleType::get(tupleElts, C);
        
        CS.addConstraint(ConstraintKind::LiteralConformsTo, contextualType,
                         dictionaryProto->getDeclaredInterfaceType(),
                         locator);
        
        unsigned index = 0;
        for (auto element : expr->getElements()) {
          CS.addConstraint(ConstraintKind::Conversion,
                           CS.getType(element),
                           contextualDictionaryElementType,
                           CS.getConstraintLocator(
                               expr, LocatorPathElt::TupleElement(index++)));
        }
        
        return contextualDictionaryType;
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
      if (!CS.getContextualType(expr)) {
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
                         ctx.TheAnyType, locator);
      }

      return dictionaryTy;
    }

    Type visitDynamicSubscriptExpr(DynamicSubscriptExpr *expr) {
      return addSubscriptConstraints(expr, CS.getType(expr->getBase()),
                                     expr->getIndex(), /*decl*/ nullptr,
                                     expr->getArgumentLabels(),
                                     expr->getUnlabeledTrailingClosureIndex());
    }

    Type visitTupleElementExpr(TupleElementExpr *expr) {
      ASTContext &context = CS.getASTContext();
      DeclNameRef name(
          context.getIdentifier(llvm::utostr(expr->getFieldNumber())));
      return addMemberRefConstraints(expr, expr->getBase(), name,
                                     FunctionRefKind::Unapplied,
                                     /*outerAlternatives=*/{});
    }

    FunctionType *inferClosureType(ClosureExpr *closure) {
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
      }

      auto extInfo = CS.closureEffects(closure);

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
              TypeResolverContext::InExpression,
              CS.getConstraintLocator(closure,
                                      ConstraintLocator::ClosureResult));
          if (resolvedTy)
            return resolvedTy;
        }

        if (auto contextualType = CS.getContextualType(closure)) {
          if (auto fnType = contextualType->getAs<FunctionType>())
            return fnType->getResult();
        }

        // If no return type was specified, create a fresh type
        // variable for it and mark it as possible hole.
        //
        // If this is a multi-statement closure, let's mark result
        // as potential hole right away.
        return Type(CS.createTypeVariable(
            CS.getConstraintLocator(closure, ConstraintLocator::ClosureResult),
            shouldTypeCheckInEnclosingExpression(closure) ? 0
                                                          : TVO_CanBindToHole));
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
    /// \param externalPatternType The type imposed by the enclosing pattern,
    /// if any. This will be non-null in cases where there is, e.g., a
    /// pattern such as "is SubClass".
    ///
    /// \param bindPatternVarsOneWay When true, generate fresh type variables
    /// for the types of each variable declared within the pattern, along
    /// with a one-way constraint binding that to the type to which the
    /// variable will be ascribed or inferred.
    Type getTypeForPattern(
       Pattern *pattern, ConstraintLocatorBuilder locator,
       Type externalPatternType,
       bool bindPatternVarsOneWay,
       PatternBindingDecl *patternBinding = nullptr,
       unsigned patternBindingIndex = 0) {
      // If there's no pattern, then we have an unknown subpattern. Create a
      // type variable.
      if (!pattern) {
        return CS.createTypeVariable(CS.getConstraintLocator(locator),
                                     TVO_CanBindToNoEscape);
      }

      // Local function that must be called for each "return" throughout this
      // function, to set the type of the pattern.
      auto setType = [&](Type type) {
        CS.setType(pattern, type);
        return type;
      };

      switch (pattern->getKind()) {
      case PatternKind::Paren: {
        auto *paren = cast<ParenPattern>(pattern);

        // Parentheses don't affect the canonical type, but record them as
        // type sugar.
        if (externalPatternType &&
            isa<ParenType>(externalPatternType.getPointer())) {
          externalPatternType = cast<ParenType>(externalPatternType.getPointer())
              ->getUnderlyingType();
        }

        auto underlyingType =
            getTypeForPattern(paren->getSubPattern(), locator,
                              externalPatternType, bindPatternVarsOneWay);

        if (!underlyingType)
          return Type();

        return setType(ParenType::get(CS.getASTContext(), underlyingType));
      }
      case PatternKind::Binding: {
        auto *subPattern = cast<BindingPattern>(pattern)->getSubPattern();
        auto type = getTypeForPattern(subPattern, locator, externalPatternType,
                                      bindPatternVarsOneWay);

        if (!type)
          return Type();

        // Var doesn't affect the type.
        return setType(type);
      }
      case PatternKind::Any: {
        return setType(
            externalPatternType
                ? externalPatternType
                : CS.createTypeVariable(CS.getConstraintLocator(locator),
                                        TVO_CanBindToNoEscape));
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

        if (!varType)
          varType = CS.createTypeVariable(CS.getConstraintLocator(locator),
                                          TVO_CanBindToNoEscape);

        // When we are supposed to bind pattern variables, create a fresh
        // type variable and a one-way constraint to assign it to either the
        // deduced type or the externally-imposed type.
        Type oneWayVarType;
        if (bindPatternVarsOneWay) {
          oneWayVarType = CS.createTypeVariable(
              CS.getConstraintLocator(locator), TVO_CanBindToNoEscape);
          CS.addConstraint(
              ConstraintKind::OneWayEqual, oneWayVarType,
              externalPatternType ? externalPatternType : varType, locator);
        }

        // If there is an externally-imposed type.

        switch (optionality) {
        case ReferenceOwnershipOptionality::Required:
          varType = TypeChecker::getOptionalType(var->getLoc(), varType);
          assert(!varType->hasError());

          if (oneWayVarType) {
            oneWayVarType =
                TypeChecker::getOptionalType(var->getLoc(), oneWayVarType);
          }
          break;

        case ReferenceOwnershipOptionality::Allowed:
        case ReferenceOwnershipOptionality::Disallowed:
          break;
        }

        // If we have a type to ascribe to the variable, do so now.
        if (oneWayVarType)
          CS.setType(var, oneWayVarType);

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

        if (!type)
          return Type();

        // Look through reference storage types.
        type = type->getReferenceStorageReferent();

        Type openedType = CS.replaceInferableTypesWithTypeVars(type, locator);
        assert(openedType);

        auto *subPattern = cast<TypedPattern>(pattern)->getSubPattern();
        // Determine the subpattern type. It will be convertible to the
        // ascribed type.
        Type subPatternType = getTypeForPattern(
            subPattern,
            locator.withPathElement(LocatorPathElt::PatternMatch(subPattern)),
            openedType, bindPatternVarsOneWay);

        if (!subPatternType)
          return Type();

        CS.addConstraint(
            ConstraintKind::Conversion, subPatternType, openedType,
            locator.withPathElement(LocatorPathElt::PatternMatch(pattern)));
        return setType(openedType);
      }

      case PatternKind::Tuple: {
        auto tuplePat = cast<TuplePattern>(pattern);

        // If there's an externally-imposed type, decompose it into element
        // types so long as we have the right number of such types.
        SmallVector<AnyFunctionType::Param, 4> externalEltTypes;
        if (externalPatternType) {
          AnyFunctionType::decomposeInput(externalPatternType,
                                          externalEltTypes);

          // If we have the wrong number of elements, we may not be able to
          // provide more specific types.
          if (tuplePat->getNumElements() != externalEltTypes.size()) {
            externalEltTypes.clear();

            // Implicit tupling.
            if (tuplePat->getNumElements() == 1) {
              externalEltTypes.push_back(
                  AnyFunctionType::Param(externalPatternType));
            }
          }
        }

        SmallVector<TupleTypeElt, 4> tupleTypeElts;
        tupleTypeElts.reserve(tuplePat->getNumElements());
        for (unsigned i = 0, e = tuplePat->getNumElements(); i != e; ++i) {
          auto &tupleElt = tuplePat->getElement(i);
          Type externalEltType;
          if (!externalEltTypes.empty())
            externalEltType = externalEltTypes[i].getPlainType();

          auto *eltPattern = tupleElt.getPattern();
          Type eltTy = getTypeForPattern(
              eltPattern,
              locator.withPathElement(LocatorPathElt::PatternMatch(eltPattern)),
              externalEltType, bindPatternVarsOneWay);

          if (!eltTy)
            return Type();

          tupleTypeElts.push_back(TupleTypeElt(eltTy, tupleElt.getLabel()));
        }

        return setType(TupleType::get(tupleTypeElts, CS.getASTContext()));
      }

      case PatternKind::OptionalSome: {
        // Remove an optional from the object type.
        if (externalPatternType) {
          Type objVar = CS.createTypeVariable(
              CS.getConstraintLocator(
                  locator.withPathElement(ConstraintLocator::OptionalPayload)),
              TVO_CanBindToNoEscape);
          CS.addConstraint(
              ConstraintKind::OptionalObject, externalPatternType, objVar,
              locator.withPathElement(LocatorPathElt::PatternMatch(pattern)));

          externalPatternType = objVar;
        }

        auto *subPattern = cast<OptionalSomePattern>(pattern)->getSubPattern();
        // The subpattern must have optional type.
        Type subPatternType = getTypeForPattern(
            subPattern,
            locator.withPathElement(LocatorPathElt::PatternMatch(subPattern)),
            externalPatternType, bindPatternVarsOneWay);

        if (!subPatternType)
          return Type();

        return setType(OptionalType::get(subPatternType));
      }

      case PatternKind::Is: {
        auto isPattern = cast<IsPattern>(pattern);

        const Type castType = resolveTypeReferenceInExpression(
            isPattern->getCastTypeRepr(), TypeResolverContext::InExpression,
            locator.withPathElement(LocatorPathElt::PatternMatch(pattern)));
        if (!castType) return Type();

        auto *subPattern = isPattern->getSubPattern();
        Type subPatternType = getTypeForPattern(
            subPattern,
            locator.withPathElement(LocatorPathElt::PatternMatch(subPattern)),
            castType, bindPatternVarsOneWay);

        if (!subPatternType)
          return Type();

        // Make sure we can cast from the subpattern type to the type we're
        // checking; if it's impossible, fail.
        CS.addConstraint(
            ConstraintKind::CheckedCast, subPatternType, castType,
            locator.withPathElement(LocatorPathElt::PatternMatch(pattern)));

        // Allow `is` pattern to infer type from context which is then going
        // to be propaged down to its sub-pattern via conversion. This enables
        // correct handling of patterns like `_ as Foo` where `_` would
        // get a type of `Foo` but `is` pattern enclosing it could still be
        // inferred from enclosing context.
        auto isType = CS.createTypeVariable(CS.getConstraintLocator(pattern),
                                            TVO_CanBindToNoEscape);
        CS.addConstraint(
            ConstraintKind::Conversion, subPatternType, isType,
            locator.withPathElement(LocatorPathElt::PatternMatch(pattern)));
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
        FunctionRefKind functionRefKind = FunctionRefKind::Compound;
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

          if (!parentType)
            return Type();

          // Perform member lookup into the parent's metatype.
          Type parentMetaType = MetatypeType::get(parentType);
          CS.addValueMemberConstraint(
              parentMetaType, enumPattern->getName(), memberType, CurDC,
              functionRefKind, {},
              CS.getConstraintLocator(locator,
                                      {LocatorPathElt::PatternMatch(pattern),
                                       ConstraintLocator::Member}));

          // Parent type needs to be convertible to the pattern type; this
          // accounts for cases where the pattern type is existential.
          CS.addConstraint(
              ConstraintKind::Conversion, parentType, patternType,
              locator.withPathElement(LocatorPathElt::PatternMatch(pattern)));

          baseType = parentType;
        } else {
          // Use the pattern type for member lookup.
          CS.addUnresolvedValueMemberConstraint(
              MetatypeType::get(patternType), enumPattern->getName(),
              memberType, CurDC, functionRefKind,
              locator.withPathElement(LocatorPathElt::PatternMatch(pattern)));

          baseType = patternType;
        }

        if (auto subPattern = enumPattern->getSubPattern()) {
          // When there is a subpattern, the member will have function type,
          // and we're matching the type of that subpattern to the parameter
          // types.
          Type subPatternType = getTypeForPattern(
              subPattern, locator, Type(), bindPatternVarsOneWay);

          if (!subPatternType)
            return Type();

          SmallVector<AnyFunctionType::Param, 4> params;
          AnyFunctionType::decomposeInput(subPatternType, params);

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
          // TODO: Convert to FunctionInput/FunctionResult constraints.
          CS.addConstraint(
              ConstraintKind::Equal, functionType, memberType,
              locator.withPathElement(LocatorPathElt::PatternMatch(pattern)));

          CS.addConstraint(
              ConstraintKind::Conversion, outputType, baseType,
              locator.withPathElement(LocatorPathElt::PatternMatch(pattern)));
        }

        return setType(patternType);
      }

      // Refutable patterns occur when checking the PatternBindingDecls in an
      // if/let or while/let condition.  They always require an initial value,
      // so they always allow unspecified types.
      case PatternKind::Expr:
        // TODO: we could try harder here, e.g. for enum elements to provide the
        // enum type.
        return setType(
            CS.createTypeVariable(
              CS.getConstraintLocator(locator), TVO_CanBindToNoEscape));
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
        bool hasErrorExprs = false;

        CollectVarRefs(ConstraintSystem &cs) : cs(cs) { }

        bool shouldWalkCaptureInitializerExpressions() override { return true; }

        std::pair<bool, Expr *> walkToExprPre(Expr *expr) override {
          // If there are any error expressions in this closure
          // it wouldn't be possible to infer its type.
          if (isa<ErrorExpr>(expr)) {
            hasErrorExprs = true;
            return {false, nullptr};
          }

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

          return { true, expr };
        }
      } collectVarRefs(CS);

      closure->walk(collectVarRefs);

      // If walker discovered error expressions, let's fail constraint
      // genreation only if closure is going to participate
      // in the type-check. This allows us to delay validation of
      // multi-statement closures until body is opened.
      if (shouldTypeCheckInEnclosingExpression(closure) &&
          collectVarRefs.hasErrorExprs) {
        return Type();
      }

      auto inferredType = inferClosureType(closure);
      if (!inferredType || inferredType->hasError())
        return Type();

      SmallVector<TypeVariableType *, 4> referencedVars{
          collectVarRefs.varRefs.begin(), collectVarRefs.varRefs.end()};

      CS.addUnsolvedConstraint(Constraint::create(
          CS, ConstraintKind::DefaultClosureType, closureType, inferredType,
          locator, referencedVars));

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
      auto array = TypeChecker::getArraySliceType(expr->getLoc(), element);
      if (array->hasError()) return element;

      // Require the operand to be convertible to the array type.
      CS.addConstraint(ConstraintKind::Conversion,
                       CS.getType(expr->getSubExpr()), array,
                       CS.getConstraintLocator(expr));
      return array;
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
      assert(expr->isPlaceholder() && "Already type checked");
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

      SmallVector<Identifier, 4> scratch;
      associateArgumentLabels(
          CS.getConstraintLocator(expr),
          {expr->getArgumentLabels(scratch),
           expr->getUnlabeledTrailingClosureIndex()},
          /*labelsArePermanent=*/isa<CallExpr>(expr));

      if (auto *UDE = dyn_cast<UnresolvedDotExpr>(fnExpr)) {
        auto typeOperation = getTypeOperation(UDE, CS.getASTContext());
        if (typeOperation != TypeOperation::None)
          return resultOfTypeOperation(typeOperation, expr->getArg());
      }

      // The result type is a fresh type variable.
      Type resultType = CS.createTypeVariable(
          CS.getConstraintLocator(expr, ConstraintLocator::FunctionResult),
          TVO_CanBindToNoEscape);

      // A direct call to a ClosureExpr makes it noescape.
      FunctionType::ExtInfo extInfo;
      if (isa<ClosureExpr>(fnExpr->getSemanticsProvidingExpr()))
        extInfo = extInfo.withNoEscape();

      // FIXME: Redesign the AST so that an ApplyExpr directly stores a list of
      // arguments together with their inout-ness, instead of a single
      // ParenExpr or TupleExpr.
      SmallVector<AnyFunctionType::Param, 8> params;
      AnyFunctionType::decomposeInput(CS.getType(expr->getArg()), params);

      CS.addConstraint(ConstraintKind::ApplicableFunction,
                       FunctionType::get(params, resultType, extInfo),
                       CS.getType(expr->getFn()),
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
    
    Type visitIfExpr(IfExpr *expr) {
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

    Type visitForcedCheckedCastExpr(ForcedCheckedCastExpr *expr) {
      auto fromExpr = expr->getSubExpr();
      if (!fromExpr) // Either wasn't constructed correctly or wasn't folded.
        return nullptr;

      auto *const repr = expr->getCastTypeRepr();
      // Validate the resulting type.
      const auto toType = resolveTypeReferenceInExpression(
          repr, TypeResolverContext::ExplicitCastExpr,
          CS.getConstraintLocator(expr));
      if (!toType)
        return nullptr;

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
      auto *const repr = expr->getCastTypeRepr();
      const auto toType = resolveTypeReferenceInExpression(
          repr, TypeResolverContext::ExplicitCastExpr,
          CS.getConstraintLocator(expr));
      if (!toType)
        return nullptr;

      // Cache the type we're casting to.
      if (repr) CS.setType(repr, toType);

      auto fromType = CS.getType(expr->getSubExpr());
      auto locator = CS.getConstraintLocator(expr);

      // Add a conversion constraint for the direct conversion between
      // types.
      CS.addExplicitConversionConstraint(fromType, toType, RememberChoice,
                                         locator);

      // If the result type was declared IUO, add a disjunction for
      // bindings for the result of the coercion.
      if (repr && repr->getKind() == TypeReprKind::ImplicitlyUnwrappedOptional)
        return createTypeVariableAndDisjunctionForIUOCoercion(toType, locator);

      return toType;
    }

    Type visitConditionalCheckedCastExpr(ConditionalCheckedCastExpr *expr) {
      auto fromExpr = expr->getSubExpr();
      if (!fromExpr) // Either wasn't constructed correctly or wasn't folded.
        return nullptr;

      // Validate the resulting type.
      auto *const repr = expr->getCastTypeRepr();
      const auto toType = resolveTypeReferenceInExpression(
          repr, TypeResolverContext::ExplicitCastExpr,
          CS.getConstraintLocator(expr));
      if (!toType)
        return nullptr;

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
      // Validate the type.
      // FIXME: Locator for the cast type?
      auto &ctx = CS.getASTContext();
      const auto toType = resolveTypeReferenceInExpression(
          expr->getCastTypeRepr(), TypeResolverContext::ExplicitCastExpr,
          CS.getConstraintLocator(expr));
      if (!toType)
        return nullptr;

      // Cache the type we're checking.
      CS.setType(expr->getCastTypeRepr(), toType);

      // Add a checked cast constraint.
      auto fromType = CS.getType(expr->getSubExpr());
      
      CS.addConstraint(ConstraintKind::CheckedCast, fromType, toType,
                       CS.getConstraintLocator(expr));

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
      // If there are UnresolvedPatterns floating around after pattern type
      // checking, they are definitely invalid. However, we will
      // diagnose that condition elsewhere; to avoid unnecessary noise errors,
      // just plop an open type variable here.
      
      auto locator = CS.getConstraintLocator(expr);
      auto typeVar = CS.createTypeVariable(locator,
                                           TVO_CanBindToLValue |
                                           TVO_CanBindToNoEscape);
      return typeVar;
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
          auto index = component.getIndexExpr();
          auto unlabeledTrailingClosureIndex =
            index->getUnlabeledTrailingClosureIndexOfPackedArgument();
          base = addSubscriptConstraints(E, base, index,
                                         /*decl*/ nullptr,
                                         component.getSubscriptLabels(),
                                         unlabeledTrailingClosureIndex,
                                         memberLocator,
                                         &componentTypeVars);
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
          continue;
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
        auto objTy = CS.createTypeVariable(locator, TVO_CanBindToNoEscape);
        componentTypeVars.push_back(objTy);

        auto optTy = OptionalType::get(objTy);
        CS.addConstraint(ConstraintKind::Conversion, base, optTy,
                         locator);
        base = optTy;
      }

      auto baseLocator =
          CS.getConstraintLocator(E, ConstraintLocator::KeyPathValue);
      auto rvalueBase = CS.createTypeVariable(baseLocator,
                                              TVO_CanBindToNoEscape);
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

    Type resultOfTypeOperation(TypeOperation op, Expr *Arg) {
      auto *tuple = cast<TupleExpr>(Arg);

      auto *lhs = tuple->getElement(0);
      auto *rhs = tuple->getElement(1);

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
        return MetatypeType::get(ctx.TheAnyType)->getCanonicalType();
      }
      }
      llvm_unreachable("unhandled operation");
    }

    void associateArgumentLabels(ConstraintLocator *locator,
                                 ConstraintSystem::ArgumentInfo info,
                                 bool labelsArePermanent = true) {
      assert(locator && locator->getAnchor());
      // Record the labels.
      if (!labelsArePermanent)
        info.Labels = CS.allocateCopy(info.Labels);
      CS.ArgumentInfos[CS.getArgumentInfoLocator(locator)] = info;
    }
  };

  class ConstraintWalker : public ASTWalker {
    ConstraintGenerator &CG;

  public:
    ConstraintWalker(ConstraintGenerator &CG) : CG(CG) { }

    std::pair<bool, Expr *> walkToExprPre(Expr *expr) override {

      if (CG.getConstraintSystem().shouldReusePrecheckedType()) {
        if (expr->getType()) {
          assert(!expr->getType()->hasTypeVariable());
          assert(!expr->getType()->hasPlaceholder());
          CG.getConstraintSystem().cacheType(expr);
          return { false, expr };
        }
      }

      // Note that the subexpression of a #selector expression is
      // unevaluated.
      if (auto sel = dyn_cast<ObjCSelectorExpr>(expr)) {
        CG.getConstraintSystem().UnevaluatedRootExprs.insert(sel->getSubExpr());
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

        auto &CS = CG.getConstraintSystem();
        for (const auto &capture : captureList->getCaptureList()) {
          SolutionApplicationTarget target(capture.PBD);
          if (CS.generateConstraints(target, FreeTypeVariableBinding::Disallow))
            return {false, nullptr};
        }
      }

      // Both multi- and single-statement closures now behave the same way
      // when it comes to constraint generation.
      if (auto closure = dyn_cast<ClosureExpr>(expr)) {
        auto &CS = CG.getConstraintSystem();
        auto closureType = CG.visitClosureExpr(closure);
        if (!closureType)
          return {false, nullptr};

        CS.setType(expr, closureType);
        return {false, expr};
      }

      // Don't visit CoerceExpr with an empty sub expression. They may occur
      // if the body of a closure was not visited while pre-checking because
      // of an error in the closure's signature.
      if (auto coerceExpr = dyn_cast<CoerceExpr>(expr)) {
        if (!coerceExpr->getSubExpr()) {
          return { false, expr };
        }
      }

      // Don't visit IfExpr with empty sub expressions. They may occur
      // if the body of a closure was not visited while pre-checking because
      // of an error in the closure's signature.
      if (auto ifExpr = dyn_cast<IfExpr>(expr)) {
        if (!ifExpr->getThenExpr() || !ifExpr->getElseExpr())
          return { false, expr };
      }

      return { true, expr };
    }

    /// Once we've visited the children of the given expression,
    /// generate constraints from the expression.
    Expr *walkToExprPost(Expr *expr) override {
      auto &CS = CG.getConstraintSystem();
      // Translate special type-checker Builtin calls into simpler expressions.
      if (auto *apply = dyn_cast<ApplyExpr>(expr)) {
        auto fnExpr = apply->getFn();
        if (auto *UDE = dyn_cast<UnresolvedDotExpr>(fnExpr)) {
          auto typeOperation =
              ConstraintGenerator::getTypeOperation(UDE, CS.getASTContext());

          if (typeOperation == ConstraintGenerator::TypeOperation::OneWay) {
            // For a one-way constraint, create the OneWayExpr node.
            auto *arg = cast<ParenExpr>(apply->getArg())->getSubExpr();
            expr = new (CS.getASTContext()) OneWayExpr(arg);
          } else if (typeOperation !=
                         ConstraintGenerator::TypeOperation::None) {
            // Handle the Builtin.type_join* family of calls by replacing
            // them with dot_self_expr of type_expr with the type being the
            // result of the join.
            auto joinMetaTy =
                CG.resultOfTypeOperation(typeOperation, apply->getArg());
            auto joinTy = joinMetaTy->castTo<MetatypeType>();

            auto *TE = TypeExpr::createImplicit(joinTy->getInstanceType(),
                                                CS.getASTContext());
            CS.cacheType(TE);

            auto *DSE = new (CS.getASTContext())
                DotSelfExpr(TE, SourceLoc(), SourceLoc(), CS.getType(TE));
            DSE->setImplicit();
            CS.cacheType(DSE);

            return DSE;
          }
        }
      }

      if (auto type = CG.visit(expr)) {
        auto simplifiedType = CS.simplifyType(type);

        CS.setType(expr, simplifiedType);

        return expr;
      }

      return nullptr;
    }

    /// Ignore statements.
    std::pair<bool, Stmt *> walkToStmtPre(Stmt *stmt) override {
      return { false, stmt };
    }

    /// Ignore declarations.
    bool walkToDeclPre(Decl *decl) override { return false; }
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

/// Generate constraints to produce the wrapped value type given the property
/// that has an attached property wrapper.
///
/// \param initializerType The type of the adjusted initializer, which
/// initializes the underlying storage variable.
/// \param wrappedVar The property that has a property wrapper.
/// \returns the type of the property.
static bool generateWrappedPropertyTypeConstraints(
    ConstraintSystem &cs, Type initializerType, VarDecl *wrappedVar,
    Type propertyType) {
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
      auto *locator = cs.getConstraintLocator(typeExpr);
      wrapperType =
          cs.replaceInferableTypesWithTypeVars(rawWrapperType, locator);
      if (initializerType)
        cs.addConstraint(ConstraintKind::Equal, wrapperType, initializerType, locator);
    } else {
      // The former wrappedValue type must be equal to the current wrapper type
      auto *locator = cs.getConstraintLocator(
          typeExpr, LocatorPathElt::WrappedValue(wrapperType));
      wrapperType =
          cs.replaceInferableTypesWithTypeVars(rawWrapperType, locator);
      cs.addConstraint(ConstraintKind::Equal, wrapperType, wrappedValueType, locator);
    }

    cs.setType(typeExpr, wrapperType);

    wrappedValueType = wrapperType->getTypeOfMember(
        dc->getParentModule(), wrapperInfo.valueVar);
  }

  // The property type must be equal to the wrapped value type
  cs.addConstraint(
      ConstraintKind::Equal, propertyType, wrappedValueType,
      cs.getConstraintLocator(
          wrappedVar, LocatorPathElt::ContextualType(CTP_WrappedProperty)));
  cs.setContextualType(wrappedVar, TypeLoc::withoutLoc(wrappedValueType),
                       CTP_WrappedProperty);
  return false;
}

/// Generate additional constraints for the pattern of an initialization.
static bool generateInitPatternConstraints(
    ConstraintSystem &cs, SolutionApplicationTarget target, Expr *initializer) {
  auto pattern = target.getInitializationPattern();
  auto locator = cs.getConstraintLocator(
      initializer, LocatorPathElt::ContextualType(CTP_Initialization));
  Type patternType = cs.generateConstraints(
      pattern, locator, target.shouldBindPatternVarsOneWay(),
      target.getInitializationPatternBindingDecl(),
      target.getInitializationPatternBindingIndex());

  if (!patternType)
    return true;

  if (auto wrappedVar = target.getInitializationWrappedVar())
    return generateWrappedPropertyTypeConstraints(
        cs, cs.getType(target.getAsExpr()), wrappedVar, patternType);

  if (!patternType->is<OpaqueTypeArchetypeType>()) {
    // Add a conversion constraint between the types.
    cs.addConstraint(ConstraintKind::Conversion, cs.getType(target.getAsExpr()),
                     patternType, locator, /*isFavored*/true);
  }

  return false;
}

/// Generate constraints for a for-each statement.
static Optional<SolutionApplicationTarget>
generateForEachStmtConstraints(
    ConstraintSystem &cs, SolutionApplicationTarget target, Expr *sequence) {
  auto forEachStmtInfo = target.getForEachStmtInfo();
  ForEachStmt *stmt = forEachStmtInfo.stmt;
  bool isAsync = stmt->getAwaitLoc().isValid();

  auto locator = cs.getConstraintLocator(sequence);
  auto contextualLocator = cs.getConstraintLocator(
      sequence, LocatorPathElt::ContextualType(CTP_ForEachStmt));

  // The expression type must conform to the Sequence protocol.
  auto sequenceProto = TypeChecker::getProtocol(
      cs.getASTContext(), stmt->getForLoc(), 
      isAsync ? 
      KnownProtocolKind::AsyncSequence : KnownProtocolKind::Sequence);
  if (!sequenceProto) {
    return None;
  }

  Type sequenceType = cs.createTypeVariable(locator, TVO_CanBindToNoEscape);
  cs.addConstraint(ConstraintKind::Conversion, cs.getType(sequence),
                   sequenceType, locator);
  cs.addConstraint(ConstraintKind::ConformsTo, sequenceType,
                   sequenceProto->getDeclaredInterfaceType(),
                   contextualLocator);

  // Check the element pattern.
  ASTContext &ctx = cs.getASTContext();
  auto dc = target.getDeclContext();
  Pattern *pattern = TypeChecker::resolvePattern(stmt->getPattern(), dc,
                                                 /*isStmtCondition*/false);
  if (!pattern)
    return None;

  auto contextualPattern =
      ContextualPattern::forRawPattern(pattern, dc);
  Type patternType = TypeChecker::typeCheckPattern(contextualPattern);
  if (patternType->hasError()) {
    return None;
  }

  // Collect constraints from the element pattern.
  auto elementLocator = cs.getConstraintLocator(
    contextualLocator, ConstraintLocator::SequenceElementType);
  Type initType = cs.generateConstraints(
      pattern, contextualLocator, target.shouldBindPatternVarsOneWay(),
      nullptr, 0);
  if (!initType)
    return None;

  // Add a conversion constraint between the element type of the sequence
  // and the type of the element pattern.
  auto elementAssocType =
      sequenceProto->getAssociatedType(cs.getASTContext().Id_Element);
  Type elementType = DependentMemberType::get(sequenceType, elementAssocType);
  cs.addConstraint(ConstraintKind::Conversion, elementType, initType,
                   elementLocator);

  // Determine the iterator type.
  auto iteratorAssocType =
      sequenceProto->getAssociatedType(isAsync ? 
        cs.getASTContext().Id_AsyncIterator : cs.getASTContext().Id_Iterator);
  Type iteratorType = DependentMemberType::get(sequenceType, iteratorAssocType);

  // The iterator type must conform to IteratorProtocol.
  ProtocolDecl *iteratorProto = TypeChecker::getProtocol(
      cs.getASTContext(), stmt->getForLoc(),
      isAsync ? 
        KnownProtocolKind::AsyncIteratorProtocol : KnownProtocolKind::IteratorProtocol);
  if (!iteratorProto)
    return None;

  // Reference the makeIterator witness.
  FuncDecl *makeIterator = isAsync ? 
    ctx.getAsyncSequenceMakeAsyncIterator() : ctx.getSequenceMakeIterator();
  
  Type makeIteratorType =
      cs.createTypeVariable(locator, TVO_CanBindToNoEscape);
  cs.addValueWitnessConstraint(
      LValueType::get(sequenceType), makeIterator,
      makeIteratorType, dc, FunctionRefKind::Compound,
      contextualLocator);

  // Generate constraints for the "where" expression, if there is one.
  if (forEachStmtInfo.whereExpr) {
    auto *boolDecl = dc->getASTContext().getBoolDecl();
    if (!boolDecl)
      return None;

    Type boolType = boolDecl->getDeclaredInterfaceType();
    if (!boolType)
      return None;

    SolutionApplicationTarget whereTarget(
        forEachStmtInfo.whereExpr, dc, CTP_Condition, boolType,
        /*isDiscarded=*/false);
    if (cs.generateConstraints(whereTarget, FreeTypeVariableBinding::Disallow))
      return None;

    cs.setContextualType(forEachStmtInfo.whereExpr,
                         TypeLoc::withoutLoc(boolType), CTP_Condition);

    forEachStmtInfo.whereExpr = whereTarget.getAsExpr();
  }

  // Populate all of the information for a for-each loop.
  forEachStmtInfo.elementType = elementType;
  forEachStmtInfo.iteratorType = iteratorType;
  forEachStmtInfo.initType = initType;
  forEachStmtInfo.sequenceType = sequenceType;
  target.setPattern(pattern);
  target.getForEachStmtInfo() = forEachStmtInfo;
  return target;
}

bool ConstraintSystem::generateConstraints(
    SolutionApplicationTarget &target,
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
      // Determine whether we know more about the contextual type.
      ContextualTypePurpose ctp = target.getExprContextualTypePurpose();
      bool isOpaqueReturnType = target.infersOpaqueReturnType();
      auto *convertTypeLocator =
          getConstraintLocator(expr, LocatorPathElt::ContextualType(ctp));

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
                                        isOpaqueReturnType);
    }

    // For an initialization target, generate constraints for the pattern.
    if (target.getExprContextualTypePurpose() == CTP_Initialization &&
        generateInitPatternConstraints(*this, target, expr)) {
      return true;
    }

    // For a for-each statement, generate constraints for the pattern, where
    // clause, and sequence traversal.
    if (target.getExprContextualTypePurpose() == CTP_ForEachStmt) {
      auto resultTarget = generateForEachStmtConstraints(*this, target, expr);
      if (!resultTarget)
        return true;

      target = *resultTarget;
    }

    if (isDebugMode()) {
      auto &log = llvm::errs();
      log << "---Initial constraints for the given expression---\n";
      print(log, expr);
      log << "\n";
      print(log);
    }

    return false;
  }

  switch (target.kind) {
  case SolutionApplicationTarget::Kind::expression:
    llvm_unreachable("Handled above");

  case SolutionApplicationTarget::Kind::caseLabelItem:
  case SolutionApplicationTarget::Kind::function:
  case SolutionApplicationTarget::Kind::stmtCondition:
    llvm_unreachable("Handled separately");

  case SolutionApplicationTarget::Kind::patternBinding: {
    auto patternBinding = target.getAsPatternBinding();
    auto dc = target.getDeclContext();
    bool hadError = false;

    /// Generate constraints for each pattern binding entry
    for (unsigned index : range(patternBinding->getNumPatternEntries())) {
      // Type check the pattern.
      auto pattern = patternBinding->getPattern(index);
      auto contextualPattern = ContextualPattern::forRawPattern(pattern, dc);
      Type patternType = TypeChecker::typeCheckPattern(contextualPattern);

      auto init = patternBinding->getInit(index);
      if (!init) {
        llvm_unreachable("Unsupported pattern binding entry");
      }

      // Generate constraints for the initialization.
      auto target = SolutionApplicationTarget::forInitialization(
          init, dc, patternType, pattern,
          /*bindPatternVarsOneWay=*/true);
      if (generateConstraints(target, FreeTypeVariableBinding::Disallow)) {
        hadError = true;
        continue;
      }

      // Keep track of this binding entry.
      setSolutionApplicationTarget({patternBinding, index}, target);
    }

    return hadError;
  }

  case SolutionApplicationTarget::Kind::uninitializedWrappedVar: {
    auto *wrappedVar = target.getAsUninitializedWrappedVar();
    auto propertyType = getVarType(wrappedVar);
    if (propertyType->hasError())
      return true;

    return generateWrappedPropertyTypeConstraints(
        *this, /*initializerType=*/Type(), wrappedVar, propertyType);
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
  return cg.getTypeForPattern(pattern, locator, Type(), bindPatternVarsOneWay,
                              patternBinding, patternIndex);
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

    case StmtConditionElement::CK_Boolean: {
      Expr *condExpr = condElement.getBoolean();
      setContextualType(condExpr, TypeLoc::withoutLoc(boolTy), CTP_Condition);

      condExpr = generateConstraints(condExpr, dc);
      if (!condExpr) {
        return true;
      }

      addConstraint(
          ConstraintKind::Conversion, getType(condExpr), boolTy,
          getConstraintLocator(condExpr,
                               LocatorPathElt::ContextualType(CTP_Condition)));
      continue;
    }

    case StmtConditionElement::CK_PatternBinding: {
      auto *pattern = TypeChecker::resolvePattern(
          condElement.getPattern(), dc, /*isStmtCondition*/true);
      if (!pattern)
        return true;

      auto target = SolutionApplicationTarget::forInitialization(
          condElement.getInitializer(), dc, Type(),
          pattern, /*bindPatternVarsOneWay=*/true);
      if (generateConstraints(target, FreeTypeVariableBinding::Disallow))
        return true;

      setSolutionApplicationTarget(&condElement, target);
      continue;
    }
    }
  }

  return false;
}

bool ConstraintSystem::generateConstraints(
    CaseStmt *caseStmt, DeclContext *dc, Type subjectType,
    ConstraintLocator *locator) {
  // Pre-bind all of the pattern variables within the case.
  bindSwitchCasePatternVars(dc, caseStmt);

  for (auto &caseLabelItem : caseStmt->getMutableCaseLabelItems()) {
    // Resolve the pattern.
    auto *pattern = caseLabelItem.getPattern();
    if (!caseLabelItem.isPatternResolved()) {
      pattern = TypeChecker::resolvePattern(
          pattern, dc, /*isStmtCondition=*/false);
      if (!pattern)
        return true;
    }

    // Generate constraints for the pattern, including one-way bindings for
    // any variables that show up in this pattern, because those variables
    // can be referenced in the guard expressions and the body.
    Type patternType = generateConstraints(
        pattern, locator, /* bindPatternVarsOneWay=*/true,
        /*patternBinding=*/nullptr, /*patternBindingIndex=*/0);

    // Convert the subject type to the pattern, which establishes the
    // bindings.
    addConstraint(
        ConstraintKind::Conversion, subjectType, patternType, locator);

    // Generate constraints for the guard expression, if there is one.
    Expr *guardExpr = caseLabelItem.getGuardExpr();
    if (guardExpr) {
      guardExpr = generateConstraints(guardExpr, dc);
      if (!guardExpr)
        return true;
    }

    // Save this info.
    setCaseLabelItemInfo(&caseLabelItem, {pattern, guardExpr});

    // For any pattern variable that has a parent variable (i.e., another
    // pattern variable with the same name in the same case), require that
    // the types be equivalent.
    pattern->forEachNode([&](Pattern *pattern) {
      auto namedPattern = dyn_cast<NamedPattern>(pattern);
      if (!namedPattern)
        return;

      auto var = namedPattern->getDecl();
      if (auto parentVar = var->getParentVarDecl()) {
        addConstraint(
            ConstraintKind::Equal, getType(parentVar), getType(var),
            getConstraintLocator(
              locator,
              LocatorPathElt::PatternMatch(namedPattern)));
      }
    });
  }

  // Bind the types of the case body variables.
  for (auto caseBodyVar : caseStmt->getCaseBodyVariablesOrEmptyArray()) {
    auto parentVar = caseBodyVar->getParentVarDecl();
    assert(parentVar && "Case body variables always have parents");
    setType(caseBodyVar, getType(parentVar));
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

  PropertyWrapperInitKind initKind;
  if (argLabel.hasDollarPrefix()) {
    Type projectionType = computeProjectedValueType(param, wrapperType);
    addConstraint(matchKind, paramType, projectionType, locator);
    if (param->hasImplicitPropertyWrapper()) {
      auto wrappedValueType = getType(param->getPropertyWrapperWrappedValueVar());
      addConstraint(ConstraintKind::PropertyWrapper, projectionType, wrappedValueType,
                    getConstraintLocator(param));
    }

    initKind = PropertyWrapperInitKind::ProjectedValue;
  } else {
    generateWrappedPropertyTypeConstraints(*this, wrapperType, param, paramType);
    initKind = PropertyWrapperInitKind::WrappedValue;
  }

  appliedPropertyWrappers[anchor].push_back({ wrapperType, initKind });
  return getTypeMatchSuccess();
}

void ConstraintSystem::optimizeConstraints(Expr *e) {
  if (getASTContext().TypeCheckerOpts.DisableConstraintSolverPerformanceHacks)
    return;
  
  SmallVector<Expr *, 16> linkedExprs;
  
  // Collect any linked expressions.
  LinkedExprCollector collector(linkedExprs, *this);
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

ResolvedMemberResult::ResolvedMemberResult(): Impl(new Implementation()) {};

ResolvedMemberResult::~ResolvedMemberResult() { delete Impl; };

ResolvedMemberResult::operator bool() const {
  return !Impl->AllDecls.empty();
}

bool ResolvedMemberResult::
hasBestOverload() const { return Impl->BestIdx.hasValue(); }

ValueDecl* ResolvedMemberResult::
getBestOverload() const { return Impl->AllDecls[Impl->BestIdx.getValue()]; }

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

ResolvedMemberResult
swift::resolveValueMember(DeclContext &DC, Type BaseTy, DeclName Name) {
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
  if (OpSolution.hasValue()) {
    Selected = OpSolution.getValue().overloadChoices[Locator].choice.getDecl();
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

OriginalArgumentList
swift::getOriginalArgumentList(Expr *expr) {
  OriginalArgumentList result;

  auto add = [&](Expr *arg, Identifier label, SourceLoc labelLoc) {
    if (isa<DefaultArgumentExpr>(arg)) {
      return;
    }

    if (auto *varargExpr = dyn_cast<VarargExpansionExpr>(arg)) {
      if (auto *arrayExpr = dyn_cast<ArrayExpr>(varargExpr->getSubExpr())) {
        for (auto *elt : arrayExpr->getElements()) {
          result.args.push_back(elt);
          result.labels.push_back(label);
          result.labelLocs.push_back(labelLoc);

          label = Identifier();
          labelLoc = SourceLoc();
        }

        return;
      }
    }

    result.args.push_back(arg);
    result.labels.push_back(label);
    result.labelLocs.push_back(labelLoc);
  };

  if (auto *parenExpr = dyn_cast<ParenExpr>(expr)) {
    result.lParenLoc = parenExpr->getLParenLoc();
    result.rParenLoc = parenExpr->getRParenLoc();
    result.hasTrailingClosure = parenExpr->hasTrailingClosure();
    add(parenExpr->getSubExpr(), Identifier(), SourceLoc());
  } else if (auto *tupleExpr = dyn_cast<TupleExpr>(expr)) {
    result.lParenLoc = tupleExpr->getLParenLoc();
    result.rParenLoc = tupleExpr->getRParenLoc();
    result.hasTrailingClosure = tupleExpr->hasTrailingClosure();

    auto args = tupleExpr->getElements();
    auto labels = tupleExpr->getElementNames();
    auto labelLocs = tupleExpr->getElementNameLocs();
    for (unsigned i = 0, e = args.size(); i != e; ++i) {
      // Implicit TupleExprs don't always store label locations.
      add(args[i], labels[i],
          labelLocs.empty() ? SourceLoc() : labelLocs[i]);
    }
  } else {
    add(expr, Identifier(), SourceLoc());
  }

  return result;
}
