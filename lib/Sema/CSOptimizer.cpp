//===--- CSOptimizer.cpp - Constraint Optimizer ---------------------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2023 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//
//
// This file implements disjunction and other constraint optimizations.
//
//===----------------------------------------------------------------------===//

#include "TypeChecker.h"
#include "swift/AST/ExistentialLayout.h"
#include "swift/AST/GenericSignature.h"
#include "swift/Basic/OptionSet.h"
#include "swift/Sema/ConstraintGraph.h"
#include "swift/Sema/ConstraintSystem.h"
#include "llvm/ADT/BitVector.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/TinyPtrVector.h"
#include "llvm/Support/SaveAndRestore.h"
#include "llvm/Support/raw_ostream.h"
#include <cstddef>
#include <functional>

using namespace swift;
using namespace constraints;

namespace {

NullablePtr<Constraint> getApplicableFnConstraint(ConstraintGraph &CG,
                                                  Constraint *disjunction) {
  auto *boundVar = disjunction->getNestedConstraints()[0]
                       ->getFirstType()
                       ->getAs<TypeVariableType>();
  if (!boundVar)
    return nullptr;

  auto constraints = CG.gatherConstraints(
      boundVar, ConstraintGraph::GatheringKind::EquivalenceClass,
      [](Constraint *constraint) {
        return constraint->getKind() == ConstraintKind::ApplicableFunction;
      });

  if (constraints.size() != 1)
    return nullptr;

  auto *applicableFn = constraints.front();
  // Unapplied disjunction could appear as a argument to applicable function,
  // we are not interested in that.
  return applicableFn->getSecondType()->isEqual(boundVar) ? applicableFn
                                                          : nullptr;
}

void forEachDisjunctionChoice(
    ConstraintSystem &cs, Constraint *disjunction,
    llvm::function_ref<void(Constraint *, ValueDecl *decl, FunctionType *)>
        callback) {
  for (auto constraint : disjunction->getNestedConstraints()) {
    if (constraint->isDisabled())
      continue;

    if (constraint->getKind() != ConstraintKind::BindOverload)
      continue;

    auto choice = constraint->getOverloadChoice();
    auto *decl = choice.getDeclOrNull();
    if (!decl)
      continue;

    // If disjunction choice is unavailable or disfavored we cannot
    // do anything with it.
    if (decl->getAttrs().hasAttribute<DisfavoredOverloadAttr>() ||
        cs.isDeclUnavailable(decl, disjunction->getLocator()))
      continue;

    Type overloadType =
        cs.getEffectiveOverloadType(disjunction->getLocator(), choice,
                                    /*allowMembers=*/true, cs.DC);

    if (!overloadType || !overloadType->is<FunctionType>())
      continue;

    callback(constraint, decl, overloadType->castTo<FunctionType>());
  }
}

} // end anonymous namespace

/// Given a set of disjunctions, attempt to determine
/// favored choices in the current context.
static void determineBestChoicesInContext(
    ConstraintSystem &cs, SmallVectorImpl<Constraint *> &disjunctions,
    llvm::DenseMap<Constraint *, llvm::TinyPtrVector<Constraint *>>
        &favorings) {
  double bestOverallScore = 0.0;
  // Tops scores across all of the disjunctions.
  llvm::DenseMap<Constraint *, double> disjunctionScores;
  llvm::DenseMap<Constraint *, llvm::TinyPtrVector<Constraint *>>
      favoredChoicesPerDisjunction;

  for (auto *disjunction : disjunctions) {
    auto applicableFn =
        getApplicableFnConstraint(cs.getConstraintGraph(), disjunction);

    if (applicableFn.isNull())
      continue;

    auto argFuncType =
        applicableFn.get()->getFirstType()->getAs<FunctionType>();

    auto argumentList = cs.getArgumentList(applicableFn.get()->getLocator());
    if (!argumentList || cs.containsIDEInspectionTarget(argumentList))
      return;

    SmallVector<FunctionType::Param, 8> argsWithLabels;
    {
      argsWithLabels.append(argFuncType->getParams().begin(),
                            argFuncType->getParams().end());
      FunctionType::relabelParams(argsWithLabels, argumentList);
    }

    SmallVector<SmallVector<std::pair<Type, /*fromLiteral=*/bool>, 2>, 2>
        candidateArgumentTypes;
    candidateArgumentTypes.resize(argFuncType->getNumParams());

    llvm::TinyPtrVector<Type> resultTypes;

    for (unsigned i = 0, n = argFuncType->getNumParams(); i != n; ++i) {
      const auto &param = argFuncType->getParams()[i];
      auto argType = cs.simplifyType(param.getPlainType());

      SmallVector<std::pair<Type, bool>, 2> types;
      if (auto *typeVar = argType->getAs<TypeVariableType>()) {
        auto bindingSet = cs.getBindingsFor(typeVar, /*finalize=*/true);

        for (const auto &binding : bindingSet.Bindings) {
          types.push_back({binding.BindingType, /*fromLiteral=*/false});
        }

        for (const auto &literal : bindingSet.Literals) {
          if (literal.second.hasDefaultType()) {
            // Add primary default type
            types.push_back(
                {literal.second.getDefaultType(), /*fromLiteral=*/true});
          }
        }
      } else {
        types.push_back({argType, /*fromLiteral=*/false});
      }

      candidateArgumentTypes[i].append(types);
    }

    auto resultType = cs.simplifyType(argFuncType->getResult());
    if (auto *typeVar = resultType->getAs<TypeVariableType>()) {
      auto bindingSet = cs.getBindingsFor(typeVar, /*finalize=*/true);

      for (const auto &binding : bindingSet.Bindings) {
        resultTypes.push_back(binding.BindingType);
      }
    } else {
      resultTypes.push_back(resultType);
    }

    // Match arguments to the given overload choice.
    auto matchArguments = [&](OverloadChoice choice, FunctionType *overloadType)
        -> llvm::Optional<MatchCallArgumentResult> {
      auto *decl = choice.getDeclOrNull();
      assert(decl);

      auto hasAppliedSelf =
          decl->hasCurriedSelf() &&
          doesMemberRefApplyCurriedSelf(choice.getBaseType(), decl);

      ParameterListInfo paramListInfo(overloadType->getParams(), decl,
                                      hasAppliedSelf);

      MatchCallArgumentListener listener;
      return matchCallArguments(argsWithLabels, overloadType->getParams(),
                                paramListInfo,
                                argumentList->getFirstTrailingClosureIndex(),
                                /*allow fixes*/ false, listener, llvm::None);
    };

    // Determine whether the candidate type is a subclass of the superclass
    // type.
    std::function<bool(Type, Type)> isSubclassOf = [&](Type candidateType,
                                                       Type superclassType) {
      // Conversion from a concrete type to its existential value.
      if (superclassType->isExistentialType() && !superclassType->isAny()) {
        auto layout = superclassType->getExistentialLayout();

        if (auto layoutConstraint = layout.getLayoutConstraint()) {
          if (layoutConstraint->isClass() &&
              !(candidateType->isClassExistentialType() ||
                candidateType->mayHaveSuperclass()))
            return false;
        }

        if (layout.explicitSuperclass &&
            !isSubclassOf(candidateType, layout.explicitSuperclass))
          return false;

        return llvm::all_of(layout.getProtocols(), [&](ProtocolDecl *P) {
          if (auto superclass = P->getSuperclass()) {
            if (!isSubclassOf(candidateType, superclass))
              return false;
          }

          return bool(TypeChecker::containsProtocol(
              candidateType, P, cs.DC->getParentModule(),
              /*skipConditionalRequirements=*/true,
              /*allowMissing=*/false));
        });
      }

      auto *subclassDecl = candidateType->getClassOrBoundGenericClass();
      auto *superclassDecl = superclassType->getClassOrBoundGenericClass();

      if (!(subclassDecl && superclassDecl))
        return false;

      return superclassDecl->isSuperclassOf(subclassDecl);
    };

    enum class MatchFlag {
      OnParam,
      Literal,
    };

    using MatchOptions = OptionSet<MatchFlag>;

    // Perform a limited set of checks to determine whether the candidate
    // could possibly match the parameter type:
    //
    // - Equality
    // - Protocol conformance(s)
    // - Optional injection
    // - Superclass conversion
    // - Array-to-pointer conversion
    // - Value to existential conversion
    // - Exact match on top-level types
    std::function<double(GenericSignature, Type, Type, MatchOptions)>
        scoreCandidateMatch = [&](GenericSignature genericSig,
                                  Type candidateType, Type paramType,
                                  MatchOptions options) -> double {
      // Exact match between candidate and parameter types.
      if (candidateType->isEqual(paramType))
        return options.contains(MatchFlag::Literal) ? 0.3 : 1;

      if (options.contains(MatchFlag::Literal))
        return 0;

      // Check whether match would require optional injection.
      {
        SmallVector<Type, 2> candidateOptionals;
        SmallVector<Type, 2> paramOptionals;

        candidateType =
            candidateType->lookThroughAllOptionalTypes(candidateOptionals);
        paramType = paramType->lookThroughAllOptionalTypes(paramOptionals);

        if (!candidateOptionals.empty() || !paramOptionals.empty()) {
          if (paramOptionals.size() >= candidateOptionals.size())
            return scoreCandidateMatch(genericSig, candidateType, paramType,
                                       options);

          // Optionality mismatch.
          return 0;
        }
      }

      // Candidate could be injected into optional parameter type
      // or converted to a superclass.
      if (isSubclassOf(candidateType, paramType))
        return 1;

      // Possible Array<T> -> Unsafe*Pointer conversion.
      if (options.contains(MatchFlag::OnParam)) {
        if (candidateType->isArrayType() &&
            paramType->getAnyPointerElementType())
          return 1;
      }

      // If both argument and parameter are tuples of the same arity,
      // it's a match.
      {
        if (auto *candidateTuple = candidateType->getAs<TupleType>()) {
          auto *paramTuple = paramType->getAs<TupleType>();
          if (paramTuple &&
              candidateTuple->getNumElements() == paramTuple->getNumElements())
            return 1;
        }
      }

      // Check protocol requirement(s) if this parameter is a
      // generic parameter type.
      if (genericSig && paramType->isTypeParameter()) {
        auto protocolRequirements = genericSig->getRequiredProtocols(paramType);
        // It's a generic parameter or dependent member which might
        // be connected via ame-type constraints to other generic
        // parameters or dependent member but we cannot check that here,
        // so let's add a tiny score just to acknowledge that it could
        // possibly match.
        if (protocolRequirements.empty())
          return 0.01;

        if (llvm::all_of(protocolRequirements, [&](ProtocolDecl *protocol) {
              return bool(cs.lookupConformance(candidateType, protocol));
            }))
          return 0.7;
      }

      // Parameter is generic, let's check whether top-level
      // types match i.e. Array<Element> as a parameter.
      //
      // This is slightly better than all of the conformances matching
      // because the parameter is concrete and could split the graph.
      if (paramType->hasTypeParameter()) {
        auto *candidateDecl = candidateType->getAnyNominal();
        auto *paramDecl = paramType->getAnyNominal();

        if (candidateDecl && paramDecl && candidateDecl == paramDecl)
          return 0.8;
      }

      return 0;
    };

    // The choice with the best score.
    double bestScore = 0.0;
    SmallVector<std::pair<Constraint *, double>, 2> favoredChoices;

    forEachDisjunctionChoice(
        cs, disjunction,
        [&](Constraint *choice, ValueDecl *decl, FunctionType *overloadType) {
          GenericSignature genericSig;
          {
            if (auto *GF = dyn_cast<AbstractFunctionDecl>(decl)) {
              genericSig = GF->getGenericSignature();
            } else if (auto *SD = dyn_cast<SubscriptDecl>(decl)) {
              genericSig = SD->getGenericSignature();
            }

            // Let's not consider non-operator generic overloads because we
            // need conformance checking functionality to determine best
            // favoring, preferring such overloads based on concrete types
            // alone leads to subpar choices due to missed information.
            if (genericSig && !decl->isOperator())
              return;
          }

          auto matchings =
              matchArguments(choice->getOverloadChoice(), overloadType);
          if (!matchings)
            return;

          double score = 0.0;
          for (unsigned paramIdx = 0, n = overloadType->getNumParams();
               paramIdx != n; ++paramIdx) {
            const auto &param = overloadType->getParams()[paramIdx];

            auto argIndices = matchings->parameterBindings[paramIdx];
            switch (argIndices.size()) {
            case 0:
              // Current parameter is defaulted.
              continue;

            case 1:
              // One-to-one match between argument and parameter.
              break;

            default:
              // Cannot deal with multiple possible matchings at the moment.
              return;
            }

            auto argIdx = argIndices.front();

            // Looks like there is nothing know about the argument.
            if (candidateArgumentTypes[argIdx].empty())
              continue;

            const auto paramFlags = param.getParameterFlags();

            // If parameter is variadic we cannot compare because we don't know
            // real arity.
            if (paramFlags.isVariadic())
              continue;

            auto paramType = param.getPlainType();

            // FIXME: Let's skip matching function types for now
            // because they have special rules for e.g. Concurrency
            // (around @Sendable) and @convention(c).
            if (paramType->is<FunctionType>())
              continue;

            // The idea here is to match the parameter type against
            // all of the argument candidate types and pick the best
            // match (i.e. exact equality one).
            //
            // If none of the candidates match exactly and they are
            // all bound concrete types, we consider this is mismatch
            // at this parameter position and remove the overload choice
            // from consideration.
            double bestCandidateScore = 0;
            llvm::BitVector mismatches(candidateArgumentTypes[argIdx].size());

            for (unsigned candidateIdx :
                 indices(candidateArgumentTypes[argIdx])) {
              // If one of the candidates matched exactly there is no reason
              // to continue checking.
              if (bestCandidateScore == 1)
                break;

              Type candidateType;
              bool isLiteralDefault;

              std::tie(candidateType, isLiteralDefault) =
                  candidateArgumentTypes[argIdx][candidateIdx];

              // `inout` parameter accepts only l-value argument.
              if (paramFlags.isInOut() && !candidateType->is<LValueType>()) {
                mismatches.set(candidateIdx);
                continue;
              }

              // The specifier only matters for `inout` check.
              candidateType = candidateType->getWithoutSpecifierType();

              MatchOptions options(MatchFlag::OnParam);
              if (isLiteralDefault)
                options |= MatchFlag::Literal;

              auto score = scoreCandidateMatch(genericSig, candidateType,
                                               paramType, options);
              if (score > 0) {
                bestCandidateScore = std::max(bestCandidateScore, score);
                continue;
              }

              // Only established arguments could be considered mismatches,
              // literal default types should be regarded as holes if they
              // didn't match.
              if (!isLiteralDefault && !candidateType->hasTypeVariable())
                mismatches.set(candidateIdx);
            }

            // If none of the candidates for this parameter matched, let's
            // drop this overload from any further consideration.
            if (mismatches.all())
              return;

            score += bestCandidateScore;
          }

          // Average the score to avoid disfavoring disjunctions with fewer
          // parameters.
          score /= overloadType->getNumParams();

          // If one of the result types matches exactly, that's a good
          // indication that overload choice should be favored.
          //
          // If nothing is known about the arguments it's only safe to
          // check result for operators (except to standard comparison
          // ones that all have the same result type), regular
          // functions/methods and especially initializers could end up
          // with a lot of favored overloads because on the result type alone.
          if (score > 0 ||
              (decl->isOperator() &&
               !decl->getBaseIdentifier().isStandardComparisonOperator())) {
            if (llvm::any_of(resultTypes, [&](const Type candidateResultTy) {
                  return scoreCandidateMatch(genericSig,
                                             overloadType->getResult(),
                                             candidateResultTy,
                                             /*options=*/{}) > 0;
                })) {
              score += 1.0;
            }
          }

          if (score > 0) {
            favoredChoices.push_back({choice, score});
            bestScore = std::max(bestScore, score);
          }
        });

    if (cs.isDebugMode()) {
      PrintOptions PO;
      PO.PrintTypesForDebugging = true;

      llvm::errs().indent(cs.solverState->getCurrentIndent())
          << "<<< Disjunction "
          << disjunction->getNestedConstraints()[0]->getFirstType()->getString(
                 PO)
          << " with score " << bestScore << "\n";
    }

    // No matching overload choices to favor.
    if (bestScore == 0.0)
      continue;

    bestOverallScore = std::max(bestOverallScore, bestScore);

    disjunctionScores[disjunction] = bestScore;
    for (const auto &choice : favoredChoices) {
      if (choice.second == bestScore)
        favoredChoicesPerDisjunction[disjunction].push_back(choice.first);
    }
  }

  if (cs.isDebugMode() && bestOverallScore > 0) {
    PrintOptions PO;
    PO.PrintTypesForDebugging = true;

    auto getLogger = [&](unsigned extraIndent = 0) -> llvm::raw_ostream & {
      return llvm::errs().indent(cs.solverState->getCurrentIndent() +
                                 extraIndent);
    };

    {
      auto &log = getLogger();
      log << "(Optimizing disjunctions: [";

      interleave(
          disjunctions,
          [&](const auto *disjunction) {
            log << disjunction->getNestedConstraints()[0]
                       ->getFirstType()
                       ->getString(PO);
          },
          [&]() { log << ", "; });

      log << "]\n";
    }

    getLogger(/*extraIndent=*/4)
        << "Best overall score = " << bestOverallScore << '\n';

    for (const auto &entry : disjunctionScores) {
      getLogger(/*extraIndent=*/4)
          << "[Disjunction '"
          << entry.first->getNestedConstraints()[0]->getFirstType()->getString(
                 PO)
          << "' with score = " << entry.second << '\n';

      for (const auto *choice : favoredChoicesPerDisjunction[entry.first]) {
        auto &log = getLogger(/*extraIndent=*/6);

        log << "- ";
        choice->print(log, &cs.getASTContext().SourceMgr);
        log << '\n';
      }

      getLogger(/*extraIdent=*/4) << "]\n";
    }

    getLogger() << ")\n";
  }

  for (auto &entry : disjunctionScores) {
    if (entry.second != bestOverallScore)
      continue;

    for (auto *choice : favoredChoicesPerDisjunction[entry.first])
      favorings[entry.first].push_back(choice);
  }
}

// Attempt to find a disjunction of bind constraints where all options
// in the disjunction are binding the same type variable.
//
// Prefer disjunctions where the bound type variable is also the
// right-hand side of a conversion constraint, since having a concrete
// type that we're converting to can make it possible to split the
// constraint system into multiple ones.
static Constraint *
selectBestBindingDisjunction(ConstraintSystem &cs,
                             SmallVectorImpl<Constraint *> &disjunctions) {

  if (disjunctions.empty())
    return nullptr;

  auto getAsTypeVar = [&cs](Type type) {
    return cs.simplifyType(type)->getRValueType()->getAs<TypeVariableType>();
  };

  Constraint *firstBindDisjunction = nullptr;
  for (auto *disjunction : disjunctions) {
    auto choices = disjunction->getNestedConstraints();
    assert(!choices.empty());

    auto *choice = choices.front();
    if (choice->getKind() != ConstraintKind::Bind)
      continue;

    // We can judge disjunction based on the single choice
    // because all of choices (of bind overload set) should
    // have the same left-hand side.
    // Only do this for simple type variable bindings, not for
    // bindings like: ($T1) -> $T2 bind String -> Int
    auto *typeVar = getAsTypeVar(choice->getFirstType());
    if (!typeVar)
      continue;

    if (!firstBindDisjunction)
      firstBindDisjunction = disjunction;

    auto constraints = cs.getConstraintGraph().gatherConstraints(
        typeVar, ConstraintGraph::GatheringKind::EquivalenceClass,
        [](Constraint *constraint) {
          return constraint->getKind() == ConstraintKind::Conversion;
        });

    for (auto *constraint : constraints) {
      if (typeVar == getAsTypeVar(constraint->getSecondType()))
        return disjunction;
    }
  }

  // If we had any binding disjunctions, return the first of
  // those. These ensure that we attempt to bind types earlier than
  // trying the elements of other disjunctions, which can often mean
  // we fail faster.
  return firstBindDisjunction;
}

llvm::Optional<std::pair<Constraint *, llvm::TinyPtrVector<Constraint *>>>
ConstraintSystem::selectDisjunction() {
  SmallVector<Constraint *, 4> disjunctions;

  collectDisjunctions(disjunctions);
  if (disjunctions.empty())
    return llvm::None;

  if (auto *disjunction = selectBestBindingDisjunction(*this, disjunctions))
    return std::make_pair(disjunction, llvm::TinyPtrVector<Constraint *>());

  llvm::DenseMap<Constraint *, llvm::TinyPtrVector<Constraint *>> favorings;
  determineBestChoicesInContext(*this, disjunctions, favorings);

  // Pick the disjunction with the smallest number of favored, then active
  // choices.
  auto bestDisjunction = std::min_element(
      disjunctions.begin(), disjunctions.end(),
      [&](Constraint *first, Constraint *second) -> bool {
        unsigned firstActive = first->countActiveNestedConstraints();
        unsigned secondActive = second->countActiveNestedConstraints();
        unsigned firstFavored = favorings[first].size();
        unsigned secondFavored = favorings[second].size();

        // Everything else equal, choose the disjunction with the greatest
        // number of resolved argument types. The number of resolved argument
        // types is always zero for disjunctions that don't represent applied
        // overloads.
        if (firstFavored == secondFavored) {
          if (firstActive != secondActive)
            return firstActive < secondActive;

          return first->countResolvedArgumentTypes(*this) >
                 second->countResolvedArgumentTypes(*this);
        }

        firstFavored = firstFavored ? firstFavored : firstActive;
        secondFavored = secondFavored ? secondFavored : secondActive;
        return firstFavored < secondFavored;
      });

  if (bestDisjunction != disjunctions.end())
    return std::make_pair(*bestDisjunction, favorings[*bestDisjunction]);

  return llvm::None;
}
