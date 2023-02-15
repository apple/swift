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
#include "swift/AST/GenericSignature.h"
#include "swift/Sema/ConstraintGraph.h"
#include "swift/Sema/ConstraintSystem.h"
#include "llvm/ADT/BitVector.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/TinyPtrVector.h"
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

          ParameterListInfo paramListInfo(
              overloadType->getParams(), decl,
              hasAppliedSelf(cs, choice->getOverloadChoice()));

          double score = 0.0;
          for (unsigned i = 0, n = overloadType->getNumParams(); i != n; ++i) {
            const auto &param = overloadType->getParams()[i];

            if (i >= candidateArgumentTypes.size()) {
              // If parameter has a default - continue matching,
              // all of the subsequence parameters should have defaults
              // as well, if they don't the overload choice in not viable.
              if (paramListInfo.hasDefaultArgument(i))
                continue;

              // Early return because this overload choice is not viable
              // without default value for the current parameter.
              return;
            }

            if (candidateArgumentTypes[i].empty())
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

            if (candidateArgumentTypes[i].empty())
              continue;

            // Check protocol requirement(s) if this parameter is a
            // generic parameter type.
            GenericSignature::RequiredProtocols protocolRequirements;
            if (genericSig) {
              if (auto *GP = paramType->getAs<GenericTypeParamType>()) {
                protocolRequirements = genericSig->getRequiredProtocols(GP);
                // It's a generic parameter which might be connected via
                // same-type constraints to other generic parameters but
                // we cannot check that here, so let's ignore it.
                if (protocolRequirements.empty())
                  continue;
              }

              if (paramType->getAs<DependentMemberType>())
                return;
            }

            // The idea here is to match the parameter type against
            // all of the argument candidate types and pick the best
            // match (i.e. exact equality one).
            //
            // If none of the candidates match exactly and they are
            // all bound concrete types, we consider this is mismatch
            // at this parameter position and remove the overload choice
            // from consideration.

            double bestCandidateScore = 0;
            llvm::BitVector mismatches(candidateArgumentTypes[i].size());

            for (unsigned candidateIdx : indices(candidateArgumentTypes[i])) {
              // If one of the candidates matched exactly there is no reason
              // to continue checking.
              if (bestCandidateScore == 1)
                break;

              Type candidateType;
              bool isLiteralDefault;

              std::tie(candidateType, isLiteralDefault) =
                  candidateArgumentTypes[i][candidateIdx];

              // `inout` parameter accepts only l-value argument.
              if (paramFlags.isInOut() && !candidateType->is<LValueType>()) {
                mismatches.set(candidateIdx);
                continue;
              }

              // The specifier only matters for `inout` check.
              candidateType = candidateType->getWithoutSpecifierType();

              // We don't check generic requirements against literal default
              // types because it creates more noise than signal for operators.
              if (!protocolRequirements.empty() && !isLiteralDefault) {
                if (llvm::all_of(
                        protocolRequirements, [&](ProtocolDecl *protocol) {
                          return TypeChecker::conformsToProtocol(
                              candidateType, protocol, cs.DC->getParentModule(),
                              /*allowMissing=*/false);
                        })) {
                  // Score is lower here because we still prefer concrete
                  // overloads over the generic ones when possible.
                  bestCandidateScore = std::max(bestCandidateScore, 0.7);
                  continue;
                }
              } else if (paramType->hasTypeParameter()) {
                // i.e. Array<Element> or Optional<Wrapped> as a parameter.
                // This is slightly better than all of the conformances matching
                // because the parameter is concrete and could split the graph.
                if (paramType->getAnyNominal() == candidateType->getAnyNominal()) {
                  bestCandidateScore = std::max(bestCandidateScore, 0.8);
                  continue;
                }
              } else if (candidateType->isEqual(paramType)) {
                // Exact match on one of the candidate bindings.
                bestCandidateScore =
                    std::max(bestCandidateScore, isLiteralDefault ? 0.3 : 1.0);
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
            if (llvm::any_of(
                    resultTypes, [&overloadType](const Type candidateResultTy) {
                      auto overloadResultTy = overloadType->getResult();
                      return candidateResultTy->isEqual(overloadResultTy);
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

Optional<std::pair<Constraint *, llvm::TinyPtrVector<Constraint *>>>
ConstraintSystem::selectDisjunction() {
  SmallVector<Constraint *, 4> disjunctions;

  collectDisjunctions(disjunctions);
  if (disjunctions.empty())
    return None;

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

  return None;
}
