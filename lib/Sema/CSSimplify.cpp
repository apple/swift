//===--- CSSimplify.cpp - Constraint Simplification -----------------------===//
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
// This file implements simplifications of constraints within the constraint
// system.
//
//===----------------------------------------------------------------------===//

#include "CSDiagnostics.h"
#include "TypeCheckConcurrency.h"
#include "swift/AST/ASTPrinter.h"
#include "swift/AST/Decl.h"
#include "swift/AST/ExistentialLayout.h"
#include "swift/AST/GenericEnvironment.h"
#include "swift/AST/GenericSignature.h"
#include "swift/AST/Initializer.h"
#include "swift/AST/NameLookupRequests.h"
#include "swift/AST/PackExpansionMatcher.h"
#include "swift/AST/ParameterList.h"
#include "swift/AST/PropertyWrappers.h"
#include "swift/AST/ProtocolConformance.h"
#include "swift/AST/SourceFile.h"
#include "swift/Basic/StringExtras.h"
#include "swift/ClangImporter/ClangModule.h"
#include "swift/Sema/CSFix.h"
#include "swift/Sema/ConstraintSystem.h"
#include "swift/Sema/IDETypeChecking.h"
#include "llvm/ADT/SetVector.h"
#include "llvm/Support/Compiler.h"

using namespace swift;
using namespace constraints;

MatchCallArgumentListener::~MatchCallArgumentListener() { }

bool MatchCallArgumentListener::extraArgument(unsigned argIdx) { return true; }

Optional<unsigned>
MatchCallArgumentListener::missingArgument(unsigned paramIdx,
                                           unsigned argInsertIdx) {
  return None;
}

bool MatchCallArgumentListener::missingLabel(unsigned paramIdx) { return true; }
bool MatchCallArgumentListener::extraneousLabel(unsigned paramIdx) {
  return true;
}
bool MatchCallArgumentListener::incorrectLabel(unsigned paramIdx) {
  return true;
}

bool MatchCallArgumentListener::outOfOrderArgument(
    unsigned argIdx, unsigned prevArgIdx, ArrayRef<ParamBinding> bindings) {
  return true;
}

bool MatchCallArgumentListener::relabelArguments(ArrayRef<Identifier> newNames){
  return true;
}

bool MatchCallArgumentListener::shouldClaimArgDuringRecovery(unsigned argIdx) {
  return true;
}

bool MatchCallArgumentListener::canClaimArgIgnoringNameMismatch(
    const AnyFunctionType::Param &arg) {
  return false;
}

/// Produce a score (smaller is better) comparing a parameter name and
/// potentially-typo'd argument name.
///
/// \param paramName The name of the parameter.
/// \param argName The name of the argument.
/// \param maxScore The maximum score permitted by this comparison, or
/// 0 if there is no limit.
///
/// \returns the score, if it is good enough to even consider this a match.
/// Otherwise, an empty optional.
///
static Optional<unsigned> scoreParamAndArgNameTypo(StringRef paramName,
                                                   StringRef argName,
                                                   unsigned maxScore) {
  using namespace camel_case;

  // Compute the edit distance.
  unsigned dist = argName.edit_distance(paramName, /*AllowReplacements=*/true,
                                        /*MaxEditDistance=*/maxScore);

  // If the edit distance would be too long, we're done.
  if (maxScore != 0 && dist > maxScore)
    return None;

  // The distance can be zero due to the "with" transformation above.
  if (dist == 0)
    return 1;

  // If this is just a single character label on both sides,
  // simply return distance.
  if (paramName.size() == 1 && argName.size() == 1)
    return dist;

  // Only allow about one typo for every two properly-typed characters, which
  // prevents completely-wacky suggestions in many cases.
  if (dist > (argName.size() + 1) / 3)
    return None;

  return dist;
}

static bool isPackExpansionType(Type type) {
  if (type->is<PackExpansionType>())
    return true;

  if (auto *typeVar = type->getAs<TypeVariableType>())
    return typeVar->getImpl().isPackExpansion();

  return false;
}

static bool containsPackExpansionType(ArrayRef<AnyFunctionType::Param> params) {
  return llvm::any_of(params, [&](const auto &param) {
    return isPackExpansionType(param.getPlainType());
  });
}

static bool containsPackExpansionType(TupleType *tuple) {
  return llvm::any_of(tuple->getElements(), [&](const auto &elt) {
    return isPackExpansionType(elt.getType());
  });
}

bool constraints::doesMemberRefApplyCurriedSelf(Type baseTy,
                                                const ValueDecl *decl) {
  assert(decl->getDeclContext()->isTypeContext() &&
         "Expected a member reference");

  // For a reference to an instance method on a metatype, we want to keep the
  // curried self.
  if (decl->isInstanceMember()) {
    assert(baseTy);
    if (isa<AbstractFunctionDecl>(decl) &&
        baseTy->getRValueType()->is<AnyMetatypeType>())
      return false;
  }

  // Otherwise the reference applies self.
  return true;
}

static bool areConservativelyCompatibleArgumentLabels(
    ConstraintSystem &cs, OverloadChoice choice,
    SmallVectorImpl<FunctionType::Param> &args,
    MatchCallArgumentListener &listener,
    Optional<unsigned> unlabeledTrailingClosureArgIndex) {
  ValueDecl *decl = nullptr;
  switch (choice.getKind()) {
  case OverloadChoiceKind::Decl:
  case OverloadChoiceKind::DeclViaBridge:
  case OverloadChoiceKind::DeclViaDynamic:
  case OverloadChoiceKind::DeclViaUnwrappedOptional:
    decl = choice.getDecl();
    break;

  // KeyPath application is not filtered in `performMemberLookup`.
  case OverloadChoiceKind::KeyPathApplication:
  case OverloadChoiceKind::DynamicMemberLookup:
  case OverloadChoiceKind::KeyPathDynamicMemberLookup:
  case OverloadChoiceKind::TupleIndex:
  case OverloadChoiceKind::MaterializePack:
    return true;
  }

  // If this is a member lookup, the call arguments (if we have any) will
  // generally be applied to the second level of parameters, with the member
  // lookup applying the curried self at the first level. But there are cases
  // where we can get an unapplied declaration reference back.
  auto hasAppliedSelf =
      decl->hasCurriedSelf() &&
      doesMemberRefApplyCurriedSelf(choice.getBaseType(), decl);

  AnyFunctionType *fnType = nullptr;
  if (decl->hasParameterList()) {
    fnType = decl->getInterfaceType()->castTo<AnyFunctionType>();
    if (hasAppliedSelf) {
      fnType = fnType->getResult()->getAs<AnyFunctionType>();
      assert(fnType && "Parameter list curry level does not match type");
    }
  } else if (auto *VD = dyn_cast<VarDecl>(decl)) {
    // For variables, we can reject any type that we know cannot be callable.
    auto varTy = VD->getValueInterfaceType()->lookThroughAllOptionalTypes();
    if (!varTy->mayBeCallable(cs.DC))
      return false;
    fnType = varTy->getAs<AnyFunctionType>();
  } else if (auto *MD = dyn_cast<MacroDecl>(decl)) {
    fnType = MD->getInterfaceType()->getAs<AnyFunctionType>();
  }

  // Given we want to be conservative with this checking, if there's any case
  // we can't match arguments for (e.g callable nominals, type parameters),
  // default to returning true.
  if (!fnType)
    return true;

  auto params = fnType->getParams();
  ParameterListInfo paramInfo(params, decl, hasAppliedSelf);

  return matchCallArguments(args, params, paramInfo,
                            unlabeledTrailingClosureArgIndex,
                            /*allow fixes*/ false, listener,
                            None).has_value();
}

Expr *constraints::getArgumentLabelTargetExpr(Expr *fn) {
  // Dig out the function, looking through, parentheses, ?, and !.
  do {
    fn = fn->getSemanticsProvidingExpr();

    if (auto force = dyn_cast<ForceValueExpr>(fn)) {
      fn = force->getSubExpr();
      continue;
    }

    if (auto bind = dyn_cast<BindOptionalExpr>(fn)) {
      fn = bind->getSubExpr();
      continue;
    }

    return fn;
  } while (true);
}

/// Determine the default type-matching options to use when decomposing a
/// constraint into smaller constraints.
static ConstraintSystem::TypeMatchOptions getDefaultDecompositionOptions(
         ConstraintSystem::TypeMatchOptions flags) {
  return flags | ConstraintSystem::TMF_GenerateConstraints;
}

/// Whether the given parameter requires an argument.
bool swift::parameterRequiresArgument(
    ArrayRef<AnyFunctionType::Param> params,
    const ParameterListInfo &paramInfo,
    unsigned paramIdx) {
  return !paramInfo.hasDefaultArgument(paramIdx)
      && !params[paramIdx].isVariadic();
}

/// Determine whether the given parameter can accept a trailing closure for the
/// "backward" logic.
static bool backwardScanAcceptsTrailingClosure(
    const AnyFunctionType::Param &param) {
  Type paramTy = param.getPlainType();
  if (!paramTy)
    return true;

  paramTy = paramTy->lookThroughAllOptionalTypes();
  return paramTy->isTypeParameter() ||
      paramTy->is<ArchetypeType>() ||
      paramTy->is<AnyFunctionType>() ||
      paramTy->isTypeVariableOrMember() ||
      paramTy->is<UnresolvedType>() ||
      paramTy->isAny();
}

/// Determine whether any parameter from the given index up until the end
/// requires an argument to be provided.
///
/// \param params The parameters themselves.
/// \param paramInfo Declaration-provided information about the parameters.
/// \param firstParamIdx The first parameter to examine to determine whether any
/// parameter in the range \c [paramIdx, params.size()) requires an argument.
/// \param beforeLabel If non-empty, stop examining parameters when we reach
/// a parameter with this label.
static bool anyParameterRequiresArgument(
    ArrayRef<AnyFunctionType::Param> params,
    const ParameterListInfo &paramInfo,
    unsigned firstParamIdx,
    Optional<Identifier> beforeLabel) {
  for (unsigned paramIdx : range(firstParamIdx, params.size())) {
    // If have been asked to stop when we reach a parameter with a particular
    // label, and we see a parameter with that label, we're done: no parameter
    // requires an argument.
    if (beforeLabel && *beforeLabel == params[paramIdx].getLabel())
      break;

    // If this parameter requires an argument, tell the caller.
    if (parameterRequiresArgument(params, paramInfo, paramIdx))
      return true;
  }

  // No parameters required arguments.
  return false;
}

static bool isCodeCompletionTypeVar(Type type) {
  if (auto *TVT = type->getAs<TypeVariableType>()) {
    if (TVT->getImpl().isCodeCompletionToken()) {
      return true;
    }
  }
  return false;
}

static bool matchCallArgumentsImpl(
    SmallVectorImpl<AnyFunctionType::Param> &args,
    ArrayRef<AnyFunctionType::Param> params,
    const ParameterListInfo &paramInfo,
    Optional<unsigned> unlabeledTrailingClosureArgIndex,
    bool allowFixes,
    TrailingClosureMatching trailingClosureMatching,
    MatchCallArgumentListener &listener,
    SmallVectorImpl<ParamBinding> &parameterBindings) {
  assert(params.size() == paramInfo.size() && "Default map does not match");
  assert(!unlabeledTrailingClosureArgIndex ||
         *unlabeledTrailingClosureArgIndex < args.size());

  // Keep track of the parameter we're matching and what argument indices
  // got bound to each parameter.
  unsigned numParams = params.size();
  parameterBindings.clear();
  parameterBindings.resize(numParams);

  // Keep track of which arguments we have claimed from the argument tuple.
  unsigned numArgs = args.size();
  SmallVector<bool, 4> claimedArgs(numArgs, false);
  SmallVector<Identifier, 4> actualArgNames;
  unsigned numClaimedArgs = 0;

  // Indicates whether any of the arguments are potentially out-of-order,
  // requiring further checking at the end.
  bool potentiallyOutOfOrder = false;

  // Local function that claims the argument at \c argIdx, returning the
  // index of the claimed argument. This is primarily a helper for
  // \c claimNextNamed.
  auto claim = [&](Identifier expectedName, unsigned argIdx,
                   bool ignoreNameClash = false)  -> unsigned {
    // Make sure we can claim this argument.
    assert(argIdx != numArgs && "Must have a valid index to claim");
    assert(!claimedArgs[argIdx] && "Argument already claimed");

    if (!actualArgNames.empty()) {
      // We're recording argument names; record this one.
      actualArgNames[argIdx] = expectedName;
    } else if (!ignoreNameClash && !args[argIdx].matchParameterLabel(expectedName)) {
      // We have an argument name mismatch. Start recording argument names.
      actualArgNames.resize(numArgs);

      // Figure out previous argument names from the parameter bindings.
      for (auto i : indices(params)) {
        const auto &param = params[i];
        bool firstArg = true;

        for (auto argIdx : parameterBindings[i]) {
          actualArgNames[argIdx] = firstArg ? param.getLabel() : Identifier();
          firstArg = false;
        }
      }

      // Record this argument name.
      actualArgNames[argIdx] = expectedName;
    }

    claimedArgs[argIdx] = true;
    ++numClaimedArgs;
    return argIdx;
  };

  // Local function that skips over any claimed arguments.
  auto skipClaimedArgs = [&](unsigned &nextArgIdx) {
    while (nextArgIdx != numArgs && claimedArgs[nextArgIdx])
      ++nextArgIdx;
    return nextArgIdx;
  };

  // Local function that retrieves the next unclaimed argument with the given
  // name (which may be empty). This routine claims the argument.
  auto claimNextNamed = [&](unsigned &nextArgIdx, Identifier paramLabel,
                            bool ignoreNameMismatch,
                            bool forVariadic = false) -> Optional<unsigned> {
    // Skip over any claimed arguments.
    skipClaimedArgs(nextArgIdx);

    // If we've claimed all of the arguments, there's nothing more to do.
    if (numClaimedArgs == numArgs)
      return None;

    // Go hunting for an unclaimed argument whose name does match.
    Optional<unsigned> claimedWithSameName;
    unsigned firstArgIdx = nextArgIdx;
    for (unsigned i = nextArgIdx; i != numArgs; ++i) {
      auto argLabel = args[i].getLabel();
      bool claimIgnoringNameMismatch = false;

      if (!args[i].matchParameterLabel(paramLabel)) {
        // If this is an attempt to claim additional unlabeled arguments
        // for variadic parameter, we have to stop at first labeled argument.
        if (forVariadic)
          return None;

        if ((i == firstArgIdx || ignoreNameMismatch) &&
            listener.canClaimArgIgnoringNameMismatch(args[i])) {
          // Avoid triggering relabelling fixes about the completion arg.
          claimIgnoringNameMismatch = true;
        } else {
          // Otherwise we can continue trying to find argument which
          // matches parameter with or without label.
          continue;
        }
      }

      // Skip claimed arguments.
      if (claimedArgs[i]) {
        assert(!forVariadic && "Cannot be for a variadic claim");
        // Note that we have already claimed an argument with the same name.
        if (!claimedWithSameName)
          claimedWithSameName = i;
        continue;
      }

      // We found a match.  If the match wasn't the next one, we have
      // potentially out of order arguments.
      if (i != nextArgIdx) {
        assert(!forVariadic && "Cannot be for a variadic claim");
        // Avoid claiming un-labeled defaulted parameters
        // by out-of-order un-labeled arguments or parts
        // of variadic argument sequence, because that might
        // be incorrect:
        // ```swift
        // func foo(_ a: Int, _ b: Int = 0, c: Int = 0, _ d: Int) {}
        // foo(1, c: 2, 3) // -> `3` will be claimed as '_ b:'.
        // ```
        if (argLabel.empty() && !claimIgnoringNameMismatch)
          continue;

        potentiallyOutOfOrder = true;
      }

      // Claim it.
      return claim(paramLabel, i, claimIgnoringNameMismatch);
    }

    // If we're not supposed to attempt any fixes, we're done.
    if (!allowFixes)
      return None;

    // Several things could have gone wrong here, and we'll check for each
    // of them at some point:
    //   - The keyword argument might be redundant, in which case we can point
    //     out the issue.
    //   - The argument might be unnamed, in which case we try to fix the
    //     problem by adding the name.
    //   - The argument might have extraneous label, in which case we try to
    //     fix the problem by removing such label.
    //   - The keyword argument might be a typo for an actual argument name, in
    //     which case we should find the closest match to correct to.

    // Missing or extraneous label.
    if (nextArgIdx != numArgs && ignoreNameMismatch) {
      auto argLabel = args[nextArgIdx].getLabel();
      // Claim this argument if we are asked to ignore labeling failure,
      // only if argument doesn't have a label when parameter expected
      // it to, or vice versa.
      if (paramLabel.empty() || argLabel.empty())
        return claim(paramLabel, nextArgIdx);
    }

    // Redundant keyword arguments.
    if (claimedWithSameName) {
      // FIXME: We can provide better diagnostics here.
      return None;
    }

    // Typo correction is handled in a later pass.
    return None;
  };

  // Local function that attempts to bind the given parameter to arguments in
  // the list.
  bool haveUnfulfilledParams = false;
  auto bindNextParameter = [&](unsigned paramIdx, unsigned &nextArgIdx,
                               bool ignoreNameMismatch) {
    const auto &param = params[paramIdx];
    Identifier paramLabel = param.getLabel();

    // If we have the trailing closure argument and are performing a forward
    // match, look for the matching parameter.
    if (trailingClosureMatching == TrailingClosureMatching::Forward &&
        unlabeledTrailingClosureArgIndex &&
        skipClaimedArgs(nextArgIdx) == *unlabeledTrailingClosureArgIndex) {
      // If the parameter we are looking at does not support the (unlabeled)
      // trailing closure argument, this parameter is unfulfilled.
      if (!paramInfo.acceptsUnlabeledTrailingClosureArgument(paramIdx) &&
          !ignoreNameMismatch) {
        haveUnfulfilledParams = true;
        return;
      }

      // Let's consider current closure to be extraneous if:
      //
      // - current parameter has a default value and doesn't accept a trailing
      //   closure; and
      // - no other free parameter after this one accepts a trailing closure via
      //   forward or backward scan. This check makes sure that it's safe to
      //   reject and push it to the next parameter without affecting backward
      //   scan logic.
      //
      // In other words - let's push the closure argument through defaulted
      // parameters until it can be considered extraneous if no parameters
      // could possibly match it.
      if (!paramInfo.acceptsUnlabeledTrailingClosureArgument(paramIdx) &&
          !parameterRequiresArgument(params, paramInfo, paramIdx)) {
        if (llvm::none_of(
                range(paramIdx + 1, params.size()), [&](unsigned idx) {
                  return parameterBindings[idx].empty() &&
                         (paramInfo.acceptsUnlabeledTrailingClosureArgument(
                              idx) ||
                          backwardScanAcceptsTrailingClosure(params[idx]));
                })) {
          haveUnfulfilledParams = true;
          return;
        }

        // If one or more parameters can match the closure, let's check
        // whether backward scan is applicable here.
      }

      // If this parameter does not require an argument, consider applying a
      // backward-match rule that skips this parameter if doing so is the only
      // way to successfully match arguments to parameters.
      if (!parameterRequiresArgument(params, paramInfo, paramIdx) &&
          !param.getPlainType()->getASTContext().LangOpts
              .hasFeature(Feature::ForwardTrailingClosures) &&
          anyParameterRequiresArgument(
              params, paramInfo, paramIdx + 1,
              nextArgIdx + 1 < numArgs
                ? Optional<Identifier>(args[nextArgIdx + 1].getLabel())
                : Optional<Identifier>(None))) {
        haveUnfulfilledParams = true;
        return;
      }

      // The argument is unlabeled, so mark the parameter as unlabeled as
      // well.
      paramLabel = Identifier();
    }

    // Handle variadic parameters.
    if (param.isVariadic() || isPackExpansionType(param.getPlainType())) {
      // Claim the next argument with the name of this parameter.
      auto claimed =
          claimNextNamed(nextArgIdx, paramLabel, ignoreNameMismatch);

      // If there was no such argument, leave the parameter unfulfilled.
      if (!claimed) {
        haveUnfulfilledParams = true;
        return;
      }

      // Record the first argument for the variadic.
      parameterBindings[paramIdx].push_back(*claimed);

      auto currentNextArgIdx = nextArgIdx;
      {
        nextArgIdx = *claimed;

        // Claim any additional unnamed arguments.
        while (true) {
          // If the next argument is the unlabeled trailing closure and the
          // variadic parameter does not accept the unlabeled trailing closure
          // argument, we're done.
          if (trailingClosureMatching == TrailingClosureMatching::Forward &&
              unlabeledTrailingClosureArgIndex &&
              skipClaimedArgs(nextArgIdx)
                  == *unlabeledTrailingClosureArgIndex &&
              !paramInfo.acceptsUnlabeledTrailingClosureArgument(paramIdx))
            break;

          if ((claimed = claimNextNamed(nextArgIdx, Identifier(), false, true)))
            parameterBindings[paramIdx].push_back(*claimed);
          else
            break;
        }
      }

      nextArgIdx = currentNextArgIdx;
      return;
    }

    // Try to claim an argument for this parameter.
    if (auto claimed =
            claimNextNamed(nextArgIdx, paramLabel, ignoreNameMismatch)) {
      parameterBindings[paramIdx].push_back(*claimed);
      return;
    }

    // There was no argument to claim. Leave the argument unfulfilled.
    haveUnfulfilledParams = true;
  };

  // If we have an unlabeled trailing closure and are matching backward, match
  // the trailing closure argument near the end.
  if (unlabeledTrailingClosureArgIndex &&
      trailingClosureMatching == TrailingClosureMatching::Backward) {
    assert(!claimedArgs[*unlabeledTrailingClosureArgIndex]);

    // One past the next parameter index to look at.
    unsigned prevParamIdx = numParams;

    // Scan backwards from the end to match the unlabeled trailing closure.
    Optional<unsigned> unlabeledParamIdx;
    if (prevParamIdx > 0) {
      unsigned paramIdx = prevParamIdx - 1;

      bool lastAcceptsTrailingClosure =
          backwardScanAcceptsTrailingClosure(params[paramIdx]);

      // If the last parameter is defaulted, this might be
      // an attempt to use a trailing closure with previous
      // parameter that accepts a function type e.g.
      //
      // func foo(_: () -> Int, _ x: Int = 0) {}
      // foo { 42 }
      if (!lastAcceptsTrailingClosure && paramIdx > 0 &&
          paramInfo.hasDefaultArgument(paramIdx)) {
        auto paramType = params[paramIdx - 1].getPlainType();
        // If the parameter before defaulted last accepts.
        if (paramType->is<AnyFunctionType>()) {
          lastAcceptsTrailingClosure = true;
          paramIdx -= 1;
        }
      }

      if (lastAcceptsTrailingClosure)
        unlabeledParamIdx = paramIdx;
    }

    // Trailing closure argument couldn't be matched to anything. Fail fast.
    if (!unlabeledParamIdx) {
      return true;
    }

    // Claim the parameter/argument pair.
    claim(
        params[*unlabeledParamIdx].getLabel(),
        *unlabeledTrailingClosureArgIndex,
        /*ignoreNameClash=*/true);
    parameterBindings[*unlabeledParamIdx].push_back(
        *unlabeledTrailingClosureArgIndex);
  }

  {
    unsigned nextArgIdx = 0;
    // Mark through the parameters, binding them to their arguments.
    for (auto paramIdx : indices(params)) {
      if (parameterBindings[paramIdx].empty())
        bindNextParameter(paramIdx, nextArgIdx, false);
    }
  }

  // If we have any unclaimed arguments, complain about those.
  if (numClaimedArgs != numArgs) {
    // Find all of the named, unclaimed arguments.
    llvm::SmallVector<unsigned, 4> unclaimedNamedArgs;
    for (auto argIdx : indices(args)) {
      if (claimedArgs[argIdx]) continue;

      if (!listener.shouldClaimArgDuringRecovery(argIdx))
        continue;

      if (!args[argIdx].getLabel().empty())
        unclaimedNamedArgs.push_back(argIdx);
    }

    if (!unclaimedNamedArgs.empty()) {
      // Find all of the named, unfulfilled parameters.
      llvm::SmallVector<unsigned, 4> unfulfilledNamedParams;
      bool hasUnfulfilledUnnamedParams = false;
      for (auto paramIdx : indices(params)) {
        if (parameterBindings[paramIdx].empty()) {
          if (params[paramIdx].getLabel().empty())
            hasUnfulfilledUnnamedParams = true;
          else
            unfulfilledNamedParams.push_back(paramIdx);
        }
      }

      if (!unfulfilledNamedParams.empty()) {
        // Use typo correction to find the best matches.
        // FIXME: There is undoubtedly a good dynamic-programming algorithm
        // to find the best assignment here.
        for (auto argIdx : unclaimedNamedArgs) {
          auto argName = args[argIdx].getLabel();

          // Find the closest matching unfulfilled named parameter.
          unsigned bestScore = 0;
          unsigned best = 0;
          for (auto i : indices(unfulfilledNamedParams)) {
            unsigned param = unfulfilledNamedParams[i];
            auto paramName = params[param].getLabel();

            if (auto score = scoreParamAndArgNameTypo(paramName.str(),
                                                      argName.str(),
                                                      bestScore)) {
              if (*score < bestScore || bestScore == 0) {
                bestScore = *score;
                best = i;
              }
            }
          }

          // If we found a parameter to fulfill, do it.
          if (bestScore > 0) {
            // Bind this parameter to the argument.
            auto paramIdx = unfulfilledNamedParams[best];
            auto paramLabel = params[paramIdx].getLabel();

            parameterBindings[paramIdx].push_back(claim(paramLabel, argIdx));

            // Erase this parameter from the list of unfulfilled named
            // parameters, so we don't try to fulfill it again.
            unfulfilledNamedParams.erase(unfulfilledNamedParams.begin() + best);
            if (unfulfilledNamedParams.empty())
              break;
          }
        }

        // Update haveUnfulfilledParams, because we may have fulfilled some
        // parameters above.
        haveUnfulfilledParams = hasUnfulfilledUnnamedParams ||
                                !unfulfilledNamedParams.empty();
      }
    }

    // Find all of the unfulfilled parameters, and match them up
    // semi-positionally.
    if (numClaimedArgs != numArgs) {
      // Restart at the first argument/parameter.
      unsigned nextArgIdx = 0;
      haveUnfulfilledParams = false;
      for (auto paramIdx : indices(params)) {
        // Skip fulfilled parameters.
        if (!parameterBindings[paramIdx].empty())
          continue;

        bindNextParameter(paramIdx, nextArgIdx, true);

        if (!listener.shouldClaimArgDuringRecovery(nextArgIdx))
          continue;
      }
    }

    // If there are as many arguments as parameters but we still
    // haven't claimed all of the arguments, it could mean that
    // labels don't line up, if so let's try to claim arguments
    // with incorrect labels, and let OoO/re-labeling logic diagnose that.
    if (numArgs == numParams && numClaimedArgs != numArgs) {
      for (auto i : indices(args)) {
        if (claimedArgs[i] || !parameterBindings[i].empty())
          continue;

        // If parameter has a default value, we don't really
        // know if label doesn't match because it's incorrect
        // or argument belongs to some other parameter, so
        // we just leave this parameter unfulfilled.
        if (paramInfo.hasDefaultArgument(i))
          continue;

        if (!listener.shouldClaimArgDuringRecovery(i))
          continue;

        // Looks like there was no parameter claimed at the same
        // position, it could only mean that label is completely
        // different, because typo correction has been attempted already.
        parameterBindings[i].push_back(claim(params[i].getLabel(), i));
      }
    }

    // If we still haven't claimed all of the arguments,
    // fail if there is no recovery.
    if (numClaimedArgs != numArgs) {
      for (auto index : indices(claimedArgs)) {
        if (claimedArgs[index])
          continue;

        if (listener.extraArgument(index))
          return true;
      }
    }

    // FIXME: If we had the actual parameters and knew the body names, those
    // matches would be best.
    potentiallyOutOfOrder = true;
  }

  // If we have any unfulfilled parameters, check them now.
  Optional<unsigned> prevArgIdx;
  if (haveUnfulfilledParams) {
    for (auto paramIdx : indices(params)) {
      // If we have a binding for this parameter, we're done.
      if (!parameterBindings[paramIdx].empty()) {
        prevArgIdx = parameterBindings[paramIdx].back();
        continue;
      }

      const auto &param = params[paramIdx];

      // Variadic parameters can be unfulfilled.
      if (param.isVariadic() || isPackExpansionType(param.getPlainType()))
        continue;

      // Parameters with defaults can be unfulfilled.
      if (paramInfo.hasDefaultArgument(paramIdx))
        continue;

      unsigned argInsertIdx = prevArgIdx ? *prevArgIdx + 1 : 0;
      if (auto newArgIdx = listener.missingArgument(paramIdx, argInsertIdx)) {
        parameterBindings[paramIdx].push_back(*newArgIdx);
        continue;
      }

      return true;
    }
  }

  // If any arguments were provided out-of-order, check whether we have
  // violated any of the reordering rules.
  if (potentiallyOutOfOrder) {
    // If we've seen label failures and now there is an out-of-order
    // parameter (or even worse - OoO parameter with label re-naming),
    // we most likely have no idea what would be the best
    // diagnostic for this situation, so let's just try to re-label.
    auto isOutOfOrderArgument = [&](unsigned toParamIdx, unsigned fromArgIdx,
                                    unsigned toArgIdx) {
      if (fromArgIdx <= toArgIdx) {
        return false;
      }

      auto newLabel = args[fromArgIdx].getLabel();
      auto oldLabel = args[toArgIdx].getLabel();

      if (newLabel != params[toParamIdx].getLabel()) {
        return false;
      }

      auto paramIdx = toParamIdx + 1;
      for (; paramIdx < params.size(); ++paramIdx) {
        // Looks like new position (excluding defaulted parameters),
        // has a valid label.
        if (oldLabel == params[paramIdx].getLabel())
          break;

        // If we are moving the position with a different label
        // and there is no default value for it, can't diagnose the
        // problem as a simple re-ordering.
        if (!paramInfo.hasDefaultArgument(paramIdx))
          return false;
      }

      // label was not found
      if (paramIdx == params.size()) {
        return false;
      }

      return true;
    };

    SmallVector<unsigned, 4> paramToArgMap;
    paramToArgMap.reserve(params.size());
    {
      unsigned argIdx = 0;
      for (const auto &binding : parameterBindings) {
        paramToArgMap.push_back(argIdx);
        // Ignore argument bindings that were synthesized due to missing args.
        argIdx += llvm::count_if(
            binding, [numArgs](unsigned argIdx) { return argIdx < numArgs; });
      }
    }

    // Enumerate the parameters and their bindings to see if any arguments are
    // our of order
    bool hadLabelMismatch = false;
    for (const auto paramIdx : indices(params)) {
      const auto toArgIdx = paramToArgMap[paramIdx];
      const auto &binding = parameterBindings[paramIdx];
      for (const auto paramBindIdx : indices(binding)) {
        // We've found the parameter that has an out of order
        // argument, and know the indices of the argument that
        // needs to move (fromArgIdx) and the argument location
        // it should move to (toArgIdx).
        const auto fromArgIdx = binding[paramBindIdx];

        // Ignore argument bindings that were synthesized due to missing args.
        if (fromArgIdx >= numArgs)
          continue;

        // Does nothing for variadic tail.
        if ((params[paramIdx].isVariadic() ||
             isPackExpansionType(params[paramIdx].getPlainType())) &&
            paramBindIdx > 0) {
          assert(args[fromArgIdx].getLabel().empty());
          continue;
        }

        // First let's double check if out-of-order argument is nothing
        // more than a simple label mismatch, because in situation where
        // one argument requires label and another one doesn't, but caller
        // doesn't provide either, problem is going to be identified as
        // out-of-order argument instead of label mismatch.
        const auto expectedLabel =
          fromArgIdx == unlabeledTrailingClosureArgIndex
            ? Identifier()
            : params[paramIdx].getLabel();
        const auto argumentLabel = args[fromArgIdx].getLabel();

        if (argumentLabel != expectedLabel) {
          // - The parameter is unnamed, in which case we try to fix the
          //   problem by removing the name.
          if (expectedLabel.empty()) {
            hadLabelMismatch = true;
            if (listener.extraneousLabel(paramIdx))
              return true;
          // - The argument is unnamed, in which case we try to fix the
          //   problem by adding the name.
          } else if (argumentLabel.empty()) {
            hadLabelMismatch = true;
            if (listener.missingLabel(paramIdx))
              return true;
          // - The argument label has a typo at the same position.
          } else if (fromArgIdx == toArgIdx) {
            hadLabelMismatch = true;
            if (listener.incorrectLabel(paramIdx))
              return true;
          }
        }

        if (fromArgIdx == toArgIdx) {
          // If the argument is in the right location, just continue
          continue;
        }

        // This situation looks like out-of-order argument but it's hard
        // to say exactly without considering other factors, because it
        // could be invalid labeling too.
        if (!hadLabelMismatch &&
            isOutOfOrderArgument(paramIdx, fromArgIdx, toArgIdx)) {
          return listener.outOfOrderArgument(
              fromArgIdx, toArgIdx, parameterBindings);
        }

        SmallVector<Identifier, 8> expectedLabels;
        llvm::transform(params, std::back_inserter(expectedLabels),
                        [](const AnyFunctionType::Param &param) {
                          return param.getLabel();
                        });
        return listener.relabelArguments(expectedLabels);
      }
    }
  }

  // If no arguments were renamed, the call arguments match up with the
  // parameters.
  if (actualArgNames.empty())
    return false;

  // The arguments were relabeled; notify the listener.
  return listener.relabelArguments(actualArgNames);
}

/// Determine whether call-argument matching requires us to try both the
/// forward and backward scanning directions to succeed.
static bool requiresBothTrailingClosureDirections(
    ArrayRef<AnyFunctionType::Param> args,
    ArrayRef<AnyFunctionType::Param> params,
    const ParameterListInfo &paramInfo,
    Optional<unsigned> unlabeledTrailingClosureArgIndex) {
  // If there's no unlabeled trailing closure, direction doesn't matter.
  if (!unlabeledTrailingClosureArgIndex)
    return false;

  // If there are labeled trailing closure arguments, only scan forward.
  if (*unlabeledTrailingClosureArgIndex < args.size() - 1)
    return false;

  // If there are no parameters, it doesn't matter; only scan forward.
  if (params.empty())
    return false;

  // If backward matching is disabled, only scan forward.
  ASTContext &ctx = params.front().getPlainType()->getASTContext();
  if (ctx.LangOpts.hasFeature(Feature::ForwardTrailingClosures))
    return false;

  // If there are at least two parameters that meet the backward scan's
  // definition of "accepts trailing closure", or there is one such parameter
  // with a defaulted parameter after it, we'll need to do the scan
  // in both directions.
  bool sawAnyTrailingClosureParam = false;
  for (unsigned paramIdx : indices(params)) {
    const auto &param = params[paramIdx];
    if (backwardScanAcceptsTrailingClosure(param)) {
      if (sawAnyTrailingClosureParam)
        return true;

      sawAnyTrailingClosureParam = true;
      continue;
    }

    if (sawAnyTrailingClosureParam && paramInfo.hasDefaultArgument(paramIdx))
      return true;
  }

  // Only one parameter can match the trailing closure anyway, so don't bother
  // scanning twice.
  return false;
}

Optional<MatchCallArgumentResult>
constraints::matchCallArguments(
    SmallVectorImpl<AnyFunctionType::Param> &args,
    ArrayRef<AnyFunctionType::Param> params,
    const ParameterListInfo &paramInfo,
    Optional<unsigned> unlabeledTrailingClosureArgIndex,
    bool allowFixes,
    MatchCallArgumentListener &listener,
    Optional<TrailingClosureMatching> trailingClosureMatching) {

  /// Perform a single call to the implementation of matchCallArguments,
  /// invoking the listener and using the results from that match.
  auto singleMatchCall = [&](TrailingClosureMatching scanDirection)
      -> Optional<MatchCallArgumentResult> {
    SmallVector<ParamBinding, 4> paramBindings;
    if (matchCallArgumentsImpl(
            args, params, paramInfo, unlabeledTrailingClosureArgIndex,
            allowFixes, scanDirection, listener, paramBindings))
      return None;

    return MatchCallArgumentResult{
        scanDirection, std::move(paramBindings), None};
  };

  // If we know that we won't have to perform both forward and backward
  // scanning for trailing closures, fast-path by performing just the
  // appropriate scan.
  if (trailingClosureMatching ||
      !requiresBothTrailingClosureDirections(
          args, params, paramInfo, unlabeledTrailingClosureArgIndex)) {
    return singleMatchCall(
        trailingClosureMatching.value_or(TrailingClosureMatching::Forward));
  }

  MatchCallArgumentListener noOpListener;

  // Try the forward direction first.
  SmallVector<ParamBinding, 4> forwardParamBindings;
  bool forwardFailed = matchCallArgumentsImpl(
      args, params, paramInfo, unlabeledTrailingClosureArgIndex, allowFixes,
      TrailingClosureMatching::Forward, noOpListener, forwardParamBindings);

  // Try the backward direction.
  SmallVector<ParamBinding, 4> backwardParamBindings;
  bool backwardFailed = matchCallArgumentsImpl(
      args, params, paramInfo, unlabeledTrailingClosureArgIndex, allowFixes,
      TrailingClosureMatching::Backward, noOpListener, backwardParamBindings);

  // If at least one of them failed, or they produced the same results, run
  // call argument matching again with the real visitor.
  if (forwardFailed || backwardFailed ||
      forwardParamBindings == backwardParamBindings) {
    // Run the forward scan unless the backward scan is the only one that
    // succeeded.
    auto scanDirection = backwardFailed || !forwardFailed
        ? TrailingClosureMatching::Forward
        : TrailingClosureMatching::Backward;
    return singleMatchCall(scanDirection);
  }

  // Both forward and backward succeeded, and produced different results.
  // Bundle them up and return both---without invoking the listener---so the
  // solver can choose.
  return MatchCallArgumentResult{
      TrailingClosureMatching::Forward,
      std::move(forwardParamBindings),
      std::move(backwardParamBindings)
  };
}

bool CompletionArgInfo::allowsMissingArgAt(unsigned argInsertIdx,
                                           AnyFunctionType::Param param) {
  // If the argument is before or at the index of the argument containing the
  // completion, the user would likely have already written it if they
  // intended this overload.
  if (completionIdx >= argInsertIdx) {
    return false;
  }

  // If the argument is after the first trailing closure, the user can only
  // continue on to write more trailing arguments, so only allow this overload
  // if the missing argument is of function type.
  if (firstTrailingIdx && argInsertIdx > *firstTrailingIdx) {
    if (param.isInOut()) {
      return false;
    }

    Type expectedTy = param.getPlainType()->lookThroughAllOptionalTypes();
    return expectedTy->is<FunctionType>() || expectedTy->isAny() ||
           expectedTy->isTypeVariableOrMember();
  }
  return true;
}

Optional<CompletionArgInfo>
constraints::getCompletionArgInfo(ASTNode anchor, ConstraintSystem &CS) {
  auto *exprAnchor = getAsExpr(anchor);
  if (!exprAnchor)
    return None;

  auto *args = exprAnchor->getArgs();
  if (!args)
    return None;

  for (unsigned i : indices(*args)) {
    if (CS.containsIDEInspectionTarget(args->getExpr(i)))
      return CompletionArgInfo{i, args->getFirstTrailingClosureIndex(),
                               args->size()};
  }
  return None;
}

class ArgumentFailureTracker : public MatchCallArgumentListener {
protected:
  ConstraintSystem &CS;
  NullablePtr<ValueDecl> Callee;
  SmallVectorImpl<AnyFunctionType::Param> &Arguments;
  ArrayRef<AnyFunctionType::Param> Parameters;
  Optional<unsigned> UnlabeledTrailingClosureArgIndex;
  ConstraintLocatorBuilder Locator;

private:
  SmallVector<SynthesizedArg, 4> MissingArguments;
  SmallVector<std::pair<unsigned, AnyFunctionType::Param>, 4> ExtraArguments;

protected:
  /// Synthesizes an argument that is intended to match against a missing
  /// argument for the parameter at \p paramIdx.
  /// \returns The index of the new argument in \c Arguments.
  unsigned synthesizeArgument(unsigned paramIdx,
                              bool isAfterCodeCompletionLoc) {
    const auto &param = Parameters[paramIdx];

    unsigned newArgIdx = Arguments.size();
    auto *argLoc = CS.getConstraintLocator(
        Locator, {LocatorPathElt::ApplyArgToParam(newArgIdx, paramIdx,
                                                  param.getParameterFlags()),
                  LocatorPathElt::SynthesizedArgument(
                      newArgIdx, isAfterCodeCompletionLoc)});

    auto *argType = CS.createTypeVariable(
        argLoc, TVO_CanBindToInOut | TVO_CanBindToLValue |
                    TVO_CanBindToNoEscape | TVO_CanBindToHole);

    auto synthesizedArg = param.withType(argType);
    Arguments.push_back(synthesizedArg);
    return newArgIdx;
  }

public:
  ArgumentFailureTracker(ConstraintSystem &cs, ValueDecl *callee,
                         SmallVectorImpl<AnyFunctionType::Param> &args,
                         ArrayRef<AnyFunctionType::Param> params,
                         Optional<unsigned> unlabeledTrailingClosureArgIndex,
                         ConstraintLocatorBuilder locator)
      : CS(cs), Callee(callee), Arguments(args), Parameters(params),
        UnlabeledTrailingClosureArgIndex(unlabeledTrailingClosureArgIndex),
        Locator(locator) {}

  ~ArgumentFailureTracker() override {
    if (!MissingArguments.empty()) {
      auto *fix = AddMissingArguments::create(CS, MissingArguments,
                                              CS.getConstraintLocator(Locator));

      // Not having an argument is the same impact as having a type mismatch.
      (void)CS.recordFix(fix, /*impact=*/MissingArguments.size() * 2);
    }
  }

  Optional<unsigned> missingArgument(unsigned paramIdx,
                                     unsigned argInsertIdx) override {
    if (!CS.shouldAttemptFixes())
      return None;

    unsigned newArgIdx =
        synthesizeArgument(paramIdx, /*isAfterCodeCompletionLoc=*/false);
    auto synthesizedArg = Arguments[newArgIdx];

    MissingArguments.push_back(SynthesizedArg{paramIdx, synthesizedArg});

    return newArgIdx;
  }

  bool extraArgument(unsigned argIdx) override {
    if (!CS.shouldAttemptFixes())
      return true;

    // If this is a trailing closure, let's check if the call is
    // to an init of a callable type. If so, let's not record it
    // as extraneous since it would be matched against implicitly
    // injected `.callAsFunction` call.
    if (UnlabeledTrailingClosureArgIndex &&
        argIdx == *UnlabeledTrailingClosureArgIndex && Callee) {
      if (auto *ctor = dyn_cast<ConstructorDecl>(Callee.get())) {
        auto resultTy = ctor->getResultInterfaceType();
        if (resultTy->isCallAsFunctionType(CS.DC))
          return true;
      }
    }

    ExtraArguments.push_back(std::make_pair(argIdx, Arguments[argIdx]));
    return false;
  }

  bool missingLabel(unsigned paramIndex) override {
    return !CS.shouldAttemptFixes();
  }

  bool extraneousLabel(unsigned paramIndex) override {
    return !CS.shouldAttemptFixes();
  }

  bool incorrectLabel(unsigned paramIndex) override {
    return !CS.shouldAttemptFixes();
  }

  bool outOfOrderArgument(
      unsigned argIdx, unsigned prevArgIdx,
      ArrayRef<ParamBinding> bindings) override {
    if (CS.shouldAttemptFixes()) {
      // If some of the arguments are missing/extraneous, no reason to
      // record a fix for this, increase the score so there is a way
      // to identify that there is something going on besides just missing
      // arguments.
      if (!MissingArguments.empty() || !ExtraArguments.empty()) {
        CS.increaseScore(SK_Fix);
        return false;
      }

      auto *fix = MoveOutOfOrderArgument::create(
          CS, argIdx, prevArgIdx, bindings, CS.getConstraintLocator(Locator));
      return CS.recordFix(fix);
    }

    return true;
  }

  bool relabelArguments(ArrayRef<Identifier> newLabels) override {
    if (!CS.shouldAttemptFixes())
      return true;

    // TODO(diagnostics): If re-labeling is mixed with extra arguments,
    // let's produce a fix only for extraneous arguments for now,
    // because they'd share a locator path which (currently) means
    // one fix would overwrite another.
    if (!ExtraArguments.empty()) {
      CS.increaseScore(SK_Fix);
      return false;
    }

    auto anchor = Locator.getBaseLocator()->getAnchor();
    if (!anchor)
      return true;

    unsigned numExtraneous = 0;
    unsigned numRenames = 0;
    unsigned numOutOfOrder = 0;

    for (unsigned i : indices(newLabels)) {
      // It's already known how many arguments are missing,
      // it would be accounted for in the impact.
      if (i >= Arguments.size())
        continue;

      auto argLabel = Arguments[i].getLabel();
      auto paramLabel = newLabels[i];

      if (argLabel == paramLabel)
        continue;

      if (!argLabel.empty()) {
        // Instead of this being a label mismatch which requires
        // re-labeling, this could be an out-of-order argument
        // instead which has a completely different impact.
        if (llvm::count(newLabels, argLabel) == 1) {
          ++numOutOfOrder;
        } else if (paramLabel.empty()) {
          ++numExtraneous;
        } else {
          ++numRenames;
        }
      }
    }

    auto *locator = CS.getConstraintLocator(Locator);
    auto *fix = RelabelArguments::create(CS, newLabels, locator);
    // Re-labeling fixes with extraneous/incorrect labels should be
    // lower priority vs. other fixes on same/different overload(s)
    // where labels did line up correctly.
    //
    // If there are not only labeling problems but also some of the
    // arguments are missing, let's account of that in the impact.
    auto impact = 1 + numOutOfOrder + numExtraneous * 2 + numRenames * 3 +
                  MissingArguments.size() * 2;
    return CS.recordFix(fix, impact);
  }

  ArrayRef<std::pair<unsigned, AnyFunctionType::Param>>
  getExtraneousArguments() const {
    return ExtraArguments;
  }
};

/// Ignores any failures after the code completion token.
class CompletionArgumentTracker : public ArgumentFailureTracker {
  struct CompletionArgInfo ArgInfo;

public:
  CompletionArgumentTracker(ConstraintSystem &cs, ValueDecl *callee,
                            SmallVectorImpl<AnyFunctionType::Param> &args,
                            ArrayRef<AnyFunctionType::Param> params,
                            Optional<unsigned> unlabeledTrailingClosureArgIndex,
                            ConstraintLocatorBuilder locator,
                            struct CompletionArgInfo ArgInfo)
      : ArgumentFailureTracker(cs, callee, args, params,
                               unlabeledTrailingClosureArgIndex, locator),
        ArgInfo(ArgInfo) {}

  Optional<unsigned> missingArgument(unsigned paramIdx,
                                     unsigned argInsertIdx) override {
    // When solving for code completion, if any argument contains the
    // completion location, later arguments shouldn't be considered missing
    // (causing the solution to have a worse score) as the user just hasn't
    // written them yet. Early exit to avoid recording them in this case.
    if (ArgInfo.allowsMissingArgAt(argInsertIdx, Parameters[paramIdx])) {
      return synthesizeArgument(paramIdx, /*isAfterCodeCompletionLoc=*/true);
    }

    return ArgumentFailureTracker::missingArgument(paramIdx, argInsertIdx);
  }

  bool extraArgument(unsigned argIdx) override {
    if (ArgInfo.isBefore(argIdx)) {
      return false;
    }
    if (argIdx == 0 && ArgInfo.completionIdx == 0) {
      return false;
    }
    return ArgumentFailureTracker::extraArgument(argIdx);
  }

  bool outOfOrderArgument(unsigned argIdx, unsigned prevArgIdx,
                          ArrayRef<ParamBinding> bindings) override {
    if (ArgInfo.isBefore(argIdx)) {
      return false;
    }

    return ArgumentFailureTracker::outOfOrderArgument(argIdx, prevArgIdx,
                                                      bindings);
  }

  bool shouldClaimArgDuringRecovery(unsigned argIdx) override {
    return !ArgInfo.isBefore(argIdx);
  }

  bool
  canClaimArgIgnoringNameMismatch(const AnyFunctionType::Param &arg) override {
    if (!isCodeCompletionTypeVar(arg.getPlainType())) {
      return false;
    }
    if (!arg.getLabel().empty()) {
      return false;
    }
    return true;
  }
};

class AllowLabelMismatches : public MatchCallArgumentListener {
  SmallVector<Identifier, 4> NewLabels;
  bool HadLabelingIssues = false;

public:
  bool missingLabel(unsigned paramIndex) override {
    HadLabelingIssues = true;
    return false;
  }

  bool relabelArguments(ArrayRef<Identifier> newLabels) override {
    NewLabels.append(newLabels.begin(), newLabels.end());
    HadLabelingIssues = true;
    return false;
  }

  bool hadLabelingIssues() const { return HadLabelingIssues; }

  Optional<ArrayRef<Identifier>> getLabelReplacements() const {
    if (!hadLabelingIssues() || NewLabels.empty())
      return None;

    return {NewLabels};
  }
};

namespace {
  /// Flags that should be applied to the existential argument type after
  /// opening.
  enum class OpenedExistentialAdjustmentFlags {
    /// The argument should be made inout after opening.
    InOut = 0x01,
    LValue = 0x02,
  };

  using OpenedExistentialAdjustments =
    OptionSet<OpenedExistentialAdjustmentFlags>;
}

/// Determine whether we should open up the existential argument to the
/// given parameters.
///
/// \param callee The function or subscript being called.
/// \param paramIdx The index specifying which function parameter is being
/// initialized.
/// \param paramTy The type of the parameter as it was opened in the constraint
/// system.
/// \param argTy The type of the argument.
///
/// \returns If the argument type is existential and opening it can bind a
/// generic parameter in the callee, returns the generic parameter, type
/// variable (from the opened parameter type) the existential type that needs
/// to be opened (from the argument type), and the adjustments that need to be
/// applied to the existential type after it is opened.
static Optional<
    std::tuple<GenericTypeParamType *, TypeVariableType *, Type,
               OpenedExistentialAdjustments>>
shouldOpenExistentialCallArgument(
    ValueDecl *callee, unsigned paramIdx, Type paramTy, Type argTy,
    Expr *argExpr, ConstraintSystem &cs) {
  if (!callee)
    return None;

  // Only applies to functions and subscripts.
  if (!isa<AbstractFunctionDecl>(callee) && !isa<SubscriptDecl>(callee))
    return None;

  // Special semantics prohibit opening existentials.
  switch (TypeChecker::getDeclTypeCheckingSemantics(callee)) {
  case DeclTypeCheckingSemantics::OpenExistential:
  case DeclTypeCheckingSemantics::TypeOf:
    // type(of:) and _openExistential handle their own opening.
    return None;

  case DeclTypeCheckingSemantics::Normal:
  case DeclTypeCheckingSemantics::WithoutActuallyEscaping:
    break;
  }

  // C++ function templates require specialization, which is not possible with
  // opened existential archetypes, so do not open.
  if (isa_and_nonnull<clang::FunctionTemplateDecl>(callee->getClangDecl()))
    return None;

  // The actual parameter type needs to involve a type variable, otherwise
  // type inference won't be possible.
  if (!paramTy->hasTypeVariable())
    return None;

  // An argument expression that explicitly coerces to an existential
  // disables the implicit opening of the existential unless it's
  // wrapped in parens.
  if (argExpr) {
    if (auto argCast = dyn_cast<ExplicitCastExpr>(
            argExpr->getSemanticsProvidingExpr())) {
      if (auto typeRepr = argCast->getCastTypeRepr()) {
        if (auto toType = cs.getType(typeRepr)) {
          if (!isa<ParenExpr>(argExpr) && toType->isAnyExistentialType())
            return None;
        }
      }
    }
  }

  OpenedExistentialAdjustments adjustments;

  // The argument may be a "var" instead of a "let".
  if (auto lv = argTy->getAs<LValueType>()) {
    argTy = lv->getObjectType();
    adjustments |= OpenedExistentialAdjustmentFlags::LValue;
  }

  // If the argument is inout, strip it off and we can add it back.
  if (auto inOutArg = argTy->getAs<InOutType>()) {
    argTy = inOutArg->getObjectType();
    adjustments |= OpenedExistentialAdjustmentFlags::InOut;
  }

  // The argument type needs to be an existential type or metatype thereof.
  if (!argTy->isAnyExistentialType())
    return None;

  auto param = getParameterAt(callee, paramIdx);
  if (!param)
    return None;

  // If the parameter is non-generic variadic, don't open.
  if (param->isVariadic())
    return None;

  // Look through an inout and optional types on the formal type of the
  // parameter.
  auto formalParamTy = param->getInterfaceType()->getInOutObjectType()
      ->lookThroughSingleOptionalType();

  // If the argument is of an existential metatype, look through the
  // metatype on the parameter.
  if (argTy->is<AnyMetatypeType>()) {
    formalParamTy = formalParamTy->getMetatypeInstanceType();
    paramTy = paramTy->getMetatypeInstanceType();
  }

  // Look through an inout and optional types on the parameter.
  paramTy = paramTy->getInOutObjectType()->lookThroughSingleOptionalType();

  // The parameter type must be a type variable.
  auto paramTypeVar = paramTy->getAs<TypeVariableType>();
  if (!paramTypeVar)
    return None;

  auto genericParam = formalParamTy->getAs<GenericTypeParamType>();
  if (!genericParam)
    return None;

  // Only allow opening the innermost generic parameters.
  auto genericContext = callee->getAsGenericContext();
  if (!genericContext || !genericContext->isGeneric())
    return None;

  auto genericSig = callee->getInnermostDeclContext()
      ->getGenericSignatureOfContext().getCanonicalSignature();
  if (genericParam->getDepth() <
          genericSig.getGenericParams().back()->getDepth())
    return None;

  // If the existential argument conforms to all of protocol requirements on
  // the formal parameter's type, don't open.
  // If all of the conformance requirements on the formal parameter's type
  // are self-conforming, don't open.
  {
    Type existentialObjectType;
    if (auto existentialMetaTy = argTy->getAs<ExistentialMetatypeType>())
      existentialObjectType = existentialMetaTy->getInstanceType();
    else
      existentialObjectType = argTy;
    auto module = cs.DC->getParentModule();
    bool containsNonSelfConformance = false;
    for (auto proto : genericSig->getRequiredProtocols(genericParam)) {
      auto conformance = module->lookupExistentialConformance(
          existentialObjectType, proto);
      if (conformance.isInvalid()) {
        containsNonSelfConformance = true;
        break;
      }
    }

    if (!containsNonSelfConformance)
      return None;
  }

  // Ensure that the formal parameter is only used in covariant positions,
  // because it won't match anywhere else.
  auto referenceInfo = findGenericParameterReferences(
      callee, genericSig, genericParam,
      /*treatNonResultCovarianceAsInvariant=*/false,
      /*skipParamIdx=*/paramIdx);
  if (referenceInfo.selfRef > TypePosition::Covariant ||
      referenceInfo.assocTypeRef > TypePosition::Covariant)
    return None;

  return std::make_tuple(genericParam, paramTypeVar, argTy, adjustments);
}

// Match the argument of a call to the parameter.
static ConstraintSystem::TypeMatchResult matchCallArguments(
    ConstraintSystem &cs, FunctionType *contextualType,
    ArgumentList *argList,
    ArrayRef<AnyFunctionType::Param> args,
    ArrayRef<AnyFunctionType::Param> params, ConstraintKind subKind,
    ConstraintLocatorBuilder locator,
    Optional<TrailingClosureMatching> trailingClosureMatching,
    SmallVectorImpl<std::pair<TypeVariableType *, OpenedArchetypeType *>>
      &openedExistentials) {
  assert(subKind == ConstraintKind::OperatorArgumentConversion ||
         subKind == ConstraintKind::ArgumentConversion);
  auto *loc = cs.getConstraintLocator(locator);
  assert(loc->isLastElement<LocatorPathElt::ApplyArgument>());

  ValueDecl *callee = nullptr;
  bool appliedSelf = false;

  // Resolve the callee for the application.
  auto *calleeLocator = cs.getCalleeLocator(loc);
  if (auto overload = cs.findSelectedOverloadFor(calleeLocator)) {
    callee = overload->choice.getDeclOrNull();
    appliedSelf = hasAppliedSelf(cs, overload->choice);
  }

  ParameterListInfo paramInfo(params, callee, appliedSelf);

  // Make sure that argument list is available.
  assert(argList);

  // Apply labels to arguments.
  SmallVector<AnyFunctionType::Param, 8> argsWithLabels;
  argsWithLabels.append(args.begin(), args.end());
  AnyFunctionType::relabelParams(argsWithLabels, argList);

  // Special case when a single tuple argument if used
  // instead of N distinct arguments e.g.:
  //
  // func foo(_ x: Int, _ y: Int) {}
  // foo((1, 2)) // expected 2 arguments, got a single tuple with 2 elements.
  if (cs.shouldAttemptFixes() && argsWithLabels.size() == 1 &&
      llvm::count_if(indices(params), [&](unsigned paramIdx) {
        return !paramInfo.hasDefaultArgument(paramIdx);
      }) > 1) {
    const auto &arg = argsWithLabels.front();
    auto argTuple = arg.getPlainType()->getRValueType()->getAs<TupleType>();
    // Don't explode a tuple in cases where first parameter is a tuple as
    // well. That is a regular "missing argument case" even if their arity
    // is different e.g.
    //
    // func foo(_: (Int, Int), _: Int) {}
    // foo((1, 2)) // call is missing an argument for parameter #1
    if (argTuple && argTuple->getNumElements() == params.size() &&
        !params.front().getPlainType()->is<TupleType>()) {
      argsWithLabels.pop_back();
      // Let's make sure that labels associated with tuple elements
      // line up with what is expected by argument list.
      SmallVector<SynthesizedArg, 4> synthesizedArgs;
      for (unsigned i = 0, n = argTuple->getNumElements(); i != n; ++i) {
        const auto &elt = argTuple->getElement(i);

        // If tuple doesn't have a label for its first element
        // and parameter does, let's assume parameter's label
        // to aid argument matching. For example:
        //
        // \code
        // func test(val: Int, _: String) {}
        //
        // test(val: (42, "")) // expands into `(val: 42, "")`
        // \endcode
        Identifier label = elt.getName();
        if (i == 0 && !elt.hasName() && params[0].hasLabel()) {
          label = params[0].getLabel();
        }

        AnyFunctionType::Param argument(elt.getType(), label);
        synthesizedArgs.push_back(SynthesizedArg{i, argument});
        argsWithLabels.push_back(argument);
      }

      (void)cs.recordFix(
          AddMissingArguments::create(cs, synthesizedArgs,
                                      cs.getConstraintLocator(locator)),
          /*impact=*/synthesizedArgs.size() * 2);
    }
  }

  // Match up the call arguments to the parameters.
  SmallVector<ParamBinding, 4> parameterBindings;
  TrailingClosureMatching selectedTrailingMatching =
      TrailingClosureMatching::Forward;

  {
    std::unique_ptr<ArgumentFailureTracker> listener;
    if (cs.isForCodeCompletion()) {
      if (auto completionInfo = getCompletionArgInfo(locator.getAnchor(), cs)) {
        listener = std::make_unique<CompletionArgumentTracker>(
            cs, callee, argsWithLabels, params,
            argList->getFirstTrailingClosureIndex(), locator, *completionInfo);
      }
    }
    if (!listener) {
      // We didn't create an argument tracker for code completion. Create a
      // normal one.
      listener = std::make_unique<ArgumentFailureTracker>(
          cs, callee, argsWithLabels, params,
          argList->getFirstTrailingClosureIndex(), locator);
    }
    auto callArgumentMatch = constraints::matchCallArguments(
        argsWithLabels, params, paramInfo,
        argList->getFirstTrailingClosureIndex(), cs.shouldAttemptFixes(),
        *listener, trailingClosureMatching);
    if (!callArgumentMatch)
      return cs.getTypeMatchFailure(locator);

    // If there are different results for both the forward and backward
    // scans, return an ambiguity: the caller will need to build a
    // disjunction.
    if (callArgumentMatch->backwardParameterBindings) {
      return cs.getTypeMatchAmbiguous();
    }

    selectedTrailingMatching = callArgumentMatch->trailingClosureMatching;
    // Record the matching direction and parameter bindings used for this call.
    cs.recordMatchCallArgumentResult(cs.getConstraintLocator(locator),
                                     *callArgumentMatch);

    // If there was a disjunction because both forward and backward were
    // possible, increase the score for forward matches to bias toward the
    // (source-compatible) backward matches. The compiler will produce a
    // warning for such code.
    if (trailingClosureMatching &&
        *trailingClosureMatching == TrailingClosureMatching::Forward)
      cs.increaseScore(SK_ForwardTrailingClosure);

    // Take the parameter bindings we selected.
    parameterBindings = std::move(callArgumentMatch->parameterBindings);

    auto extraArguments = listener->getExtraneousArguments();
    if (!extraArguments.empty()) {
      if (RemoveExtraneousArguments::isMinMaxNameShadowing(cs, locator))
        return cs.getTypeMatchFailure(locator);

      // First let's see whether this is a situation where a single
      // parameter is a tuple, but N distinct arguments were passed in.
      if (AllowTupleSplatForSingleParameter::attempt(
              cs, argsWithLabels, params, parameterBindings, locator)) {
        // Let's produce a generic "extraneous arguments"
        // diagnostic otherwise.
        auto *fix = RemoveExtraneousArguments::create(
            cs, contextualType, extraArguments,
            cs.getConstraintLocator(locator));

        if (cs.recordFix(fix, /*impact=*/extraArguments.size() * 5))
          return cs.getTypeMatchFailure(locator);
      }
    }
  }

  auto isSynthesizedArgument = [](const AnyFunctionType::Param &arg) -> bool {
    if (auto *typeVar = arg.getPlainType()->getAs<TypeVariableType>()) {
      auto *locator = typeVar->getImpl().getLocator();
      return locator->isLastElement<LocatorPathElt::SynthesizedArgument>();
    }

    return false;
  };

  for (unsigned paramIdx = 0, numParams = parameterBindings.size();
       paramIdx != numParams; ++paramIdx){
    // Determine the parameter type.
    const auto &param = params[paramIdx];
    auto paramTy = param.getOldType();

    // Type parameter packs ingest the entire set of argument bindings
    // as a pack type.
    //
    // We pull these out special because variadic parameters ban lots of
    // the more interesting typing constructs called out below like
    // inout and @autoclosure.
    if (paramInfo.isVariadicGenericParameter(paramIdx)) {
      // If generic parameter comes from a variadic type declaration it's
      // possible that it got specialized early and is no longer represented
      // by a pack expansion type. For example, consider expression -
      // `Test<Int>(42)` where `Test<each T>` and the initializer
      // is declared as `init(_: repeat each T)`. Although declaration
      // based information reports parameter at index 0 as variadic generic
      // the call site specializes it to `Int`.
      if (isPackExpansionType(paramTy)) {
        SmallVector<Type, 2> argTypes;
        for (auto argIdx : parameterBindings[paramIdx]) {
          auto argType = argsWithLabels[argIdx].getPlainType();
          argTypes.push_back(argType);
        }

        auto *argPack = PackType::get(cs.getASTContext(), argTypes);
        auto *argPackExpansion = PackExpansionType::get(argPack, argPack);

        auto firstArgIdx =
            argTypes.empty() ? paramIdx : parameterBindings[paramIdx].front();

        cs.addConstraint(
            subKind, argPackExpansion, paramTy,
            locator.withPathElement(LocatorPathElt::ApplyArgToParam(
                firstArgIdx, paramIdx, param.getParameterFlags())));
        continue;
      }
    }

    // If type inference from default arguments is enabled, let's
    // add a constraint from the parameter if necessary, otherwise
    // there is nothing to do but move to the next parameter.
    if (parameterBindings[paramIdx].empty() && callee) {
      // Type inference from default value expressions.
      {
        auto *paramList = getParameterList(callee);
        if (!paramList)
          continue;

        // There is nothing to infer if parameter doesn't have any
        // generic parameters in its type.
        auto *PD = paramList->get(paramIdx);
        if (!PD->getInterfaceType()->hasTypeParameter())
          continue;

        // The type of the default value is going to be determined
        // based on a type deduced for the parameter at this call site.
        if (PD->hasCallerSideDefaultExpr())
          continue;

        auto defaultExprType = PD->getTypeOfDefaultExpr();

        // A caller side default.
        if (!defaultExprType || defaultExprType->hasError())
          continue;

        // If this is just a regular default type that should
        // work for all substitutions of generic parameter,
        // let's continue.
        if (defaultExprType->hasArchetype())
          continue;

        cs.addConstraint(
            ConstraintKind::ArgumentConversion, paramTy, defaultExprType,
            locator.withPathElement(LocatorPathElt::ApplyArgToParam(
                paramIdx, paramIdx, param.getParameterFlags())));
      }

      continue;
    }

    // Compare each of the bound arguments for this parameter.
    for (auto argIdx : parameterBindings[paramIdx]) {
      auto loc = locator.withPathElement(LocatorPathElt::ApplyArgToParam(
          argIdx, paramIdx, param.getParameterFlags()));
      const auto &argument = argsWithLabels[argIdx];
      auto argTy = argument.getOldType();

      bool matchingAutoClosureResult = param.isAutoClosure();
      auto *argExpr = getArgumentExpr(locator.getAnchor(), argIdx);
      if (param.isAutoClosure() && !isSynthesizedArgument(argument)) {
        auto &ctx = cs.getASTContext();

        // If this is a call to a function with a closure argument and the
        // parameter is an autoclosure, let's just increment the score here
        // so situations like below are not ambiguous.
        //    func f<T>(_: () -> T) {}
        //    func f<T>(_: @autoclosure () -> T) {}
        //
        //    f { } // OK
        if (isExpr<ClosureExpr>(argExpr)) {
          cs.increaseScore(SK_FunctionToAutoClosureConversion);
        }

        // If the argument is not marked as @autoclosure or
        // this is Swift version >= 5 where forwarding is not allowed,
        // argument would always be wrapped into an implicit closure
        // at the end, so we can safely match against result type.
        if (ctx.isSwiftVersionAtLeast(5) || !isAutoClosureArgument(argExpr)) {
          // In Swift >= 5 mode there is no @autoclosure forwarding,
          // so let's match result types.
          if (auto *fnType = paramTy->getAs<FunctionType>()) {
            paramTy = fnType->getResult();
          }
        } else {
          // Matching @autoclosure argument to @autoclosure parameter
          // directly would mean introducing a function conversion
          // in Swift <= 4 mode.
          cs.increaseScore(SK_FunctionConversion);
          matchingAutoClosureResult = false;
        }
      }

      // In case solver matched trailing based on the backward scan,
      // let's produce a warning which would suggest to add a label
      // to disambiguate in the future.
      if (selectedTrailingMatching == TrailingClosureMatching::Backward &&
          argIdx == *argList->getFirstTrailingClosureIndex()) {
        cs.recordFix(SpecifyLabelToAssociateTrailingClosure::create(
            cs, cs.getConstraintLocator(loc)));
      }

      // Type-erase any opened existentials from subsequent parameter types
      // unless the argument itself is a generic function, which could handle
      // the opened existentials.
      if (!openedExistentials.empty() && paramTy->hasTypeVariable() &&
          !cs.isArgumentGenericFunction(argTy, argExpr)) {
        for (const auto &opened : openedExistentials) {
          paramTy = typeEraseOpenedExistentialReference(
              paramTy, opened.second->getExistentialType(), opened.first,
              TypePosition::Contravariant);
        }
      }

      // If the argument is an existential type and the parameter is generic,
      // consider opening the existential type.
      if (auto existentialArg = shouldOpenExistentialCallArgument(
              callee, paramIdx, paramTy, argTy, argExpr, cs)) {
        // My kingdom for a decent "if let" in C++.
        GenericTypeParamType *openedGenericParam;
        TypeVariableType *openedTypeVar;
        Type existentialType;
        OpenedExistentialAdjustments adjustments;
        std::tie(openedGenericParam, openedTypeVar, existentialType,
                 adjustments) = *existentialArg;

        OpenedArchetypeType *opened;
        std::tie(argTy, opened) = cs.openExistentialType(
            existentialType, cs.getConstraintLocator(loc));

        if (adjustments.contains(OpenedExistentialAdjustmentFlags::LValue))
          argTy = LValueType::get(argTy);

        if (adjustments.contains(OpenedExistentialAdjustmentFlags::InOut))
          argTy = InOutType::get(argTy);

        openedExistentials.push_back({openedTypeVar, opened});
      }

      auto argLabel = argument.getLabel();
      if (paramInfo.hasExternalPropertyWrapper(paramIdx) ||
          argLabel.hasDollarPrefix()) {
        auto *param = getParameterAt(callee, paramIdx);
        assert(param);
        if (cs.applyPropertyWrapperToParameter(paramTy, argTy,
                                               const_cast<ParamDecl *>(param),
                                               argLabel, subKind, loc)
                .isFailure()) {
          return cs.getTypeMatchFailure(loc);
        }
        continue;
      }

      // If argument comes for declaration it should loose
      // `@autoclosure` flag, because in context it's used
      // as a function type represented by autoclosure.
      //
      // Special case here are synthesized arguments because
      // they mirror parameter flags to ease diagnosis.
      assert(!argsWithLabels[argIdx].isAutoClosure() ||
             isSynthesizedArgument(argument));

      // If parameter is a generic parameter, let's copy its
      // conformance requirements (if any), to the argument
      // be able to filter mismatching choices earlier.
      if (auto *typeVar = paramTy->getAs<TypeVariableType>()) {
        auto *locator = typeVar->getImpl().getLocator();
        if (locator->isForGenericParameter()) {
          auto &CG = cs.getConstraintGraph();

          auto isTransferableConformance = [&typeVar](Constraint *constraint) {
            if (constraint->getKind() != ConstraintKind::ConformsTo)
              return false;

            auto requirementTy = constraint->getFirstType();
            if (!requirementTy->isEqual(typeVar))
              return false;

            return constraint->getSecondType()->is<ProtocolType>();
          };

          for (auto *constraint : CG[typeVar].getConstraints()) {
            if (isTransferableConformance(constraint))
              cs.addConstraint(ConstraintKind::TransitivelyConformsTo, argTy,
                               constraint->getSecondType(),
                               constraint->getLocator());
          }
        }
      }

      // Detect that there is sync -> async mismatch early on for
      // closure argument to avoid re-checking calls if there was
      // an overload choice with synchronous parameter of the same
      // shape e.g.
      //
      // func test(_: () -> Void) -> MyStruct {}
      // func test(_: () async -> Void) -> MyStruct {}
      //
      // test({ ... }).<member>...
      //
      // Synchronous overload is always better in this case so there
      // is no need to re-check follow-up `<member>`s and better
      // to short-circuit this path early.
      if (auto *fnType = paramTy->getAs<FunctionType>()) {
        if (fnType->isAsync()) {
          auto *typeVar = argTy->getAs<TypeVariableType>();
          if (typeVar && typeVar->getImpl().isClosureType()) {
            auto *locator = typeVar->getImpl().getLocator();
            auto *closure = castToExpr<ClosureExpr>(locator->getAnchor());
            if (!cs.getClosureType(closure)->isAsync())
              cs.increaseScore(SK_SyncInAsync);
          }
        }
      }

      if (!argument.isCompileTimeConst() && param.isCompileTimeConst()) {
        auto *locator = cs.getConstraintLocator(loc);
        SourceRange range;
        // simplify locator so the anchor is the exact argument.
        cs.recordFix(NotCompileTimeConst::create(cs, paramTy,
          simplifyLocator(cs, locator, range)));
      }

      cs.addConstraint(
          subKind, argTy, paramTy,
          matchingAutoClosureResult
              ? loc.withPathElement(ConstraintLocator::AutoclosureResult)
              : loc,
          /*isFavored=*/false);
    }
  }

  return cs.getTypeMatchSuccess();
}


ConstraintSystem::TypeMatchResult
ConstraintSystem::matchFunctionResultTypes(Type expectedResult, Type fnResult,
                                           TypeMatchOptions flags,
                                           ConstraintLocatorBuilder locator) {
  // If we have a callee with an IUO return, add a disjunction that can either
  // bind to the result or an unwrapped result.
  auto *calleeLoc = getCalleeLocator(getConstraintLocator(locator));
  auto *calleeResultLoc = getConstraintLocator(
      calleeLoc, ConstraintLocator::FunctionResult);
  auto selected = findSelectedOverloadFor(calleeLoc);

  // If we don't have a direct callee, this might be the second application
  // of a curried function reference, in which case we need to dig into the
  // inner call to find the callee.
  // FIXME: This is a bit of a hack. We should consider rewriting curried
  // applies as regular applies in PreCheckExpr to eliminate the need to special
  // case double applies in the solver.
  bool isSecondApply = false;
  if (!selected) {
    auto anchor = locator.getAnchor();
    if (auto *callExpr = getAsExpr<CallExpr>(anchor)) {
      if (auto *innerCall = getAsExpr<CallExpr>(callExpr->getSemanticFn())) {
        auto *innerCalleeLoc =
            getCalleeLocator(getConstraintLocator(innerCall));
        if (auto innerOverload = findSelectedOverloadFor(innerCalleeLoc)) {
          auto choice = innerOverload->choice;
          if (choice.getFunctionRefKind() == FunctionRefKind::DoubleApply) {
            isSecondApply = true;
            selected.emplace(*innerOverload);
          }
        }
      }
    }
  }
  if (selected) {
    auto choice = selected->choice;

    // Subscripts found through dynamic lookup need special treatment. Unlike
    // other decls found through dynamic lookup, they cannot have an optional
    // applied to their reference, instead it's applied to their result. As
    // such, we may need to unwrap another level of optionality.
    if (choice.getKind() == OverloadChoiceKind::DeclViaDynamic &&
        isa<SubscriptDecl>(choice.getDecl())) {
      // Introduce a type variable to record whether we needed to unwrap the
      // outer optional.
      auto outerTy = createTypeVariable(calleeResultLoc, TVO_CanBindToLValue);
      buildDisjunctionForDynamicLookupResult(outerTy, fnResult,
                                             calleeResultLoc);
      fnResult = outerTy;
    }

    auto iuoKind = choice.getIUOReferenceKind(*this, isSecondApply);
    if (iuoKind == IUOReferenceKind::ReturnValue) {
      buildDisjunctionForImplicitlyUnwrappedOptional(expectedResult, fnResult,
                                                     calleeResultLoc);
      return getTypeMatchSuccess();
    }
  }
  return matchTypes(expectedResult, fnResult, ConstraintKind::Bind, flags,
                    locator);
}

static bool isInPatternMatchingContext(ConstraintLocatorBuilder locator) {
  SmallVector<LocatorPathElt, 4> path;
  (void)locator.getLocatorParts(path);

  auto pathElement = llvm::find_if(path, [](LocatorPathElt &elt) {
    return elt.is<LocatorPathElt::PatternMatch>();
  });
  return pathElement != path.end();
}

namespace {

class TupleMatcher {
  TupleType *tuple1;
  TupleType *tuple2;

public:
  enum class MatchKind : uint8_t {
    Equality,
    Subtype,
    Conversion,
  };

  SmallVector<MatchedPair, 4> pairs;
  bool hasLabelMismatch = false;

  TupleMatcher(TupleType *tuple1, TupleType *tuple2)
      : tuple1(tuple1), tuple2(tuple2) {}

  bool match(MatchKind kind, ConstraintLocatorBuilder locator) {
    // FIXME: TuplePackMatcher should completely replace the non-variadic
    // case too eventually.
    if (containsPackExpansionType(tuple1) ||
        containsPackExpansionType(tuple2)) {
      TuplePackMatcher matcher(tuple1, tuple2, isPackExpansionType);
      if (matcher.match())
        return true;

      pairs = matcher.pairs;
      return false;
    }

    if (tuple1->getNumElements() != tuple2->getNumElements())
      return true;

    switch (kind) {
    case MatchKind::Equality:
      return matchEquality(isInPatternMatchingContext(locator));

    case MatchKind::Subtype:
      return matchSubtype();

    case MatchKind::Conversion:
      return matchConversion();
    }
  }

private:
  bool matchEquality(bool inPatternMatchingContext) {
    for (unsigned i = 0, n = tuple1->getNumElements(); i != n; ++i) {
      const auto &elt1 = tuple1->getElement(i);
      const auto &elt2 = tuple2->getElement(i);

      if (inPatternMatchingContext) {
        if (elt1.hasName() && elt1.getName() != elt2.getName())
          return true;
      } else {
        // If the names don't match, we have a conflict.
        if (elt1.getName() != elt2.getName())
          return true;
      }

      pairs.emplace_back(elt1.getType(), elt2.getType(), i, i);
    }

    return false;
  }

  bool matchSubtype() {
    for (unsigned i = 0, n = tuple1->getNumElements(); i != n; ++i) {
      const auto &elt1 = tuple1->getElement(i);
      const auto &elt2 = tuple2->getElement(i);

      // If the names don't match, we may have a conflict.
      if (elt1.getName() != elt2.getName()) {
        // Make sure that this name isn't used at some other position.
        if (elt2.hasName() && tuple1->getNamedElementId(elt2.getName()) != -1)
          return true;

        // If both elements have names and they mismatch, make a note of it
        // so we can emit a warning.
        if (elt1.hasName() && elt2.hasName())
          hasLabelMismatch = true;
      }

      pairs.emplace_back(elt1.getType(), elt2.getType(), i, i);
    }

    return false;
  }

  bool matchConversion() {
    SmallVector<unsigned, 4> sources;
    if (computeTupleShuffle(tuple1, tuple2, sources))
      return true;

    for (unsigned idx2 = 0, n = sources.size(); idx2 != n; ++idx2) {
      unsigned idx1 = sources[idx2];

      auto lhs = tuple1->getElementType(idx1);
      auto rhs = tuple2->getElementType(idx2);

      pairs.emplace_back(lhs, rhs, idx1, idx2);
    }

    return false;
  }
};

}

ConstraintSystem::TypeMatchResult
ConstraintSystem::matchTupleTypes(TupleType *tuple1, TupleType *tuple2,
                                  ConstraintKind kind, TypeMatchOptions flags,
                                  ConstraintLocatorBuilder locator) {
  using TupleMatchKind = TupleMatcher::MatchKind;

  ConstraintKind subkind;
  TupleMatchKind matchKind;

  switch (kind) {
  case ConstraintKind::Bind:
  case ConstraintKind::Equal: {
    subkind = kind;
    matchKind = TupleMatchKind::Equality;
    break;
  }

  // NOTE: It was probably a mistake that BindToPointerType is handled like
  // Subtype; this was implicit in the old structure of the code due to bogus
  // use of operator<= on enum cases.
  case ConstraintKind::Subtype:
  case ConstraintKind::BindToPointerType: {
    subkind = kind;
    matchKind = TupleMatchKind::Subtype;
    break;
  }

  case ConstraintKind::Conversion:
  case ConstraintKind::ArgumentConversion:
  case ConstraintKind::OperatorArgumentConversion: {
    subkind = ConstraintKind::Conversion;
    matchKind = TupleMatchKind::Conversion;
    break;
  }

  case ConstraintKind::BindParam:
  case ConstraintKind::ApplicableFunction:
  case ConstraintKind::DynamicCallableApplicableFunction:
  case ConstraintKind::BindOverload:
  case ConstraintKind::CheckedCast:
  case ConstraintKind::SubclassOf:
  case ConstraintKind::ConformsTo:
  case ConstraintKind::TransitivelyConformsTo:
  case ConstraintKind::Defaultable:
  case ConstraintKind::Disjunction:
  case ConstraintKind::Conjunction:
  case ConstraintKind::DynamicTypeOf:
  case ConstraintKind::EscapableFunctionOf:
  case ConstraintKind::OpenedExistentialOf:
  case ConstraintKind::KeyPath:
  case ConstraintKind::KeyPathApplication:
  case ConstraintKind::LiteralConformsTo:
  case ConstraintKind::OptionalObject:
  case ConstraintKind::SelfObjectOfProtocol:
  case ConstraintKind::UnresolvedValueMember:
  case ConstraintKind::ValueMember:
  case ConstraintKind::ValueWitness:
  case ConstraintKind::BridgingConversion:
  case ConstraintKind::OneWayEqual:
  case ConstraintKind::OneWayBindParam:
  case ConstraintKind::DefaultClosureType:
  case ConstraintKind::UnresolvedMemberChainBase:
  case ConstraintKind::PropertyWrapper:
  case ConstraintKind::SyntacticElement:
  case ConstraintKind::BindTupleOfFunctionParams:
  case ConstraintKind::PackElementOf:
  case ConstraintKind::ShapeOf:
  case ConstraintKind::ExplicitGenericArguments:
  case ConstraintKind::SameShape:
    llvm_unreachable("Bad constraint kind in matchTupleTypes()");
  }

  TupleMatcher matcher(tuple1, tuple2);

  if (matcher.match(matchKind, locator))
    return getTypeMatchFailure(locator);

  if (matcher.hasLabelMismatch) {
    // If we had a label mismatch, emit a warning. This is something we
    // shouldn't permit, as it's more permissive than what a conversion would
    // allow. Ideally we'd turn this into an error in Swift 6 mode.
    recordFix(AllowTupleLabelMismatch::create(*this, tuple1, tuple2,
                                              getConstraintLocator(locator)));
  }

  TypeMatchOptions subflags = getDefaultDecompositionOptions(flags);

  for (auto pair : matcher.pairs) {
    auto result = matchTypes(pair.lhs, pair.rhs, subkind, subflags,
                             locator.withPathElement(
                                    LocatorPathElt::TupleElement(pair.lhsIdx)));
    if (result.isFailure())
      return result;
  }

  return getTypeMatchSuccess();
}

ConstraintSystem::TypeMatchResult
ConstraintSystem::matchPackTypes(PackType *pack1, PackType *pack2,
                                 ConstraintKind kind, TypeMatchOptions flags,
                                 ConstraintLocatorBuilder locator) {
  TypeMatchOptions subflags = getDefaultDecompositionOptions(flags);

  PackMatcher matcher(pack1->getElementTypes(), pack2->getElementTypes(),
                      getASTContext(), isPackExpansionType);

  if (matcher.match())
    return getTypeMatchFailure(locator);

  for (auto pair : matcher.pairs) {
    auto result = matchTypes(pair.lhs, pair.rhs, kind, subflags,
                             locator.withPathElement(
                                 LocatorPathElt::PackElement(pair.lhsIdx)));
    if (result.isFailure())
      return result;
  }

  return getTypeMatchSuccess();
}

namespace {

/// Collects all unique pack type variables referenced from the pattern type,
/// skipping those captured by nested pack expansion types.
///
/// FIXME: This could probably be a common utility method somewhere.
struct PackTypeVariableCollector: TypeWalker {
  llvm::SetVector<TypeVariableType *> typeVars;

  Action walkToTypePre(Type t) override {
    if (t->is<PackExpansionType>())
      return Action::SkipChildren;

    if (auto *typeVar = t->getAs<TypeVariableType>()) {
      if (typeVar->getImpl().canBindToPack())
        typeVars.insert(typeVar);
    }

    return Action::Continue;
  }
};

}

/// Utility function used when matching a pack expansion type against a
/// pack type.
///
/// Takes a pattern type and an original pack type, and returns an instantiated
/// pack type. The original pack type is then matched against the instantiated
/// pack type.
///
/// As a side effect, it binds each pack type variable occurring in the pattern
/// type to a new pack with the same shape as the original pack, but where the
/// elements are fresh type variables.
///
/// The instantiated pack has the same shape as the original pack, where the
/// ith element is the pattern type with each pack type variable replaced by the
/// ith element of its binding.
///
/// For example, given the pattern Foo<$T0> and the original pack
/// {Foo<Int>, Foo<String>...}, we're going to bind
///
/// $T0 := {$T1, $T2}
///
/// And return the new pack {Foo<$T1>, Foo<$T2>...}.
///
/// The caller will then match the original pack type against the instantiated
/// pack type, which will recover the bindings:
///
/// $T1 := Int
/// $T2 := String
///
static PackType *replaceTypeVariablesWithFreshPacks(ConstraintSystem &cs,
                                                    Type pattern,
                                                    PackType *pack,
                                                    ConstraintLocatorBuilder locator) {
  SmallVector<Type, 2> elts;

  llvm::MapVector<TypeVariableType *, SmallVector<Type, 2>> typeVars;

  PackTypeVariableCollector collector;
  pattern.walk(collector);

  if (collector.typeVars.empty())
    return nullptr;

  auto *loc = cs.getConstraintLocator(locator);

  // For each pack type variable occurring in the pattern type, compute a
  // binding pack type comprised of fresh type variables.
  for (auto *typeVar : collector.typeVars) {
    auto &freshTypeVars = typeVars[typeVar];
    for (unsigned i = 0, e = pack->getNumElements(); i < e; ++i) {
      auto *packExpansionElt = pack->getElementType(i)->getAs<PackExpansionType>();

      // Preserve the pack expansion structure of the original pack. If the ith
      // element was a pack expansion type, create a new pack expansion type
      // wrapping a pack type variable. Otherwise, create a new scalar
      // type variable.
      //
      // FIXME: Other TVO_* flags for type variables?
      auto elementLoc = cs.getConstraintLocator(loc,
          LocatorPathElt::PackElement(freshTypeVars.size()));
      if (packExpansionElt != nullptr) {
        auto *freshTypeVar = cs.createTypeVariable(
            elementLoc,
            TVO_CanBindToPack |
                (typeVar->getImpl().canBindToHole() ? TVO_CanBindToHole : 0));
        freshTypeVars.push_back(PackExpansionType::get(
            freshTypeVar, packExpansionElt->getCountType()));
      } else {
        freshTypeVars.push_back(cs.createTypeVariable(
            elementLoc,
            typeVar->getImpl().canBindToHole() ? TVO_CanBindToHole : 0));
      }
    }
  }

  // For each element of the original pack type, instantiate the pattern type by
  // replacing each pack type variable with the corresponding element of the
  // pack type variable's binding pack.
  for (unsigned i = 0, e = pack->getNumElements(); i < e; ++i) {
    auto *packExpansionElt = pack->getElementType(i)->getAs<PackExpansionType>();

    auto instantiatedPattern = pattern.transformRec([&](Type t)
                                                        -> Optional<Type> {
      if (isPackExpansionType(t))
        return t;

      if (auto *typeVar = t->getAs<TypeVariableType>()) {
        if (typeVar->getImpl().canBindToPack()) {
          auto found = typeVars.find(typeVar);
          assert(found != typeVars.end());

          // The ith element of the binding pack is either a scalar type variable
          // or a pack expansion type wrapping a pack type variable.
          auto projectedType = (found->second)[i];
          if (packExpansionElt != nullptr) {
            projectedType = projectedType->castTo<PackExpansionType>()
                                         ->getPatternType();
            assert(projectedType->castTo<TypeVariableType>()
                                ->getImpl().canBindToPack());
          } else {
            assert(!projectedType->castTo<TypeVariableType>()
                                 ->getImpl().canBindToPack());
          }

          return projectedType;
        }
      }

      return None;
    });

    if (packExpansionElt != nullptr) {
      elts.push_back(PackExpansionType::get(instantiatedPattern,
                                            packExpansionElt->getCountType()));
    } else {
      elts.push_back(instantiatedPattern);
    }
  }

  auto &ctx = cs.getASTContext();

  // Bind each pack type variable occurring in the pattern type to its
  // binding pack that was constructed above.
  for (const auto &pair : typeVars) {
    cs.addConstraint(ConstraintKind::Bind,
                     pair.first, PackType::get(ctx, pair.second), locator);
  }

  // Construct the instantiated pack type.
  return PackType::get(cs.getASTContext(), elts);
}

ConstraintSystem::TypeMatchResult
ConstraintSystem::matchPackExpansionTypes(PackExpansionType *expansion1,
                                          PackExpansionType *expansion2,
                                          ConstraintKind kind, TypeMatchOptions flags,
                                          ConstraintLocatorBuilder locator) {
  // The count types of two pack expansion types must have the same shape.
  addConstraint(ConstraintKind::SameShape, expansion1->getCountType(),
                expansion2->getCountType(),
                locator.withPathElement(ConstraintLocator::PackShape));

  auto pattern1 = expansion1->getPatternType();
  auto pattern2 = expansion2->getPatternType();

  // If both sides are expanded or neither side is, just match them
  // directly.
  if (pattern1->is<PackType>() == pattern2->is<PackType>()) {
    return matchTypes(pattern1, pattern2, kind, flags, locator);

  // If the right hand side is expanded, we have something like
  // Foo<$T0>... vs Pack{Foo<Int>, Foo<String>}...; We're going to
  // bind $T0 to Pack{Int, String}.
  } else if (!pattern1->is<PackType>() && pattern2->is<PackType>()) {
    if (auto *pack2 = pattern2->getAs<PackType>()) {
      if (auto *pack1 = replaceTypeVariablesWithFreshPacks(
             *this, pattern1, pack2, locator)) {
        addConstraint(kind, pack1, pack2, locator);
        return getTypeMatchSuccess();
      }
    }

    return getTypeMatchFailure(locator);

  // If the left hand side is expanded, we have something like
  // Pack{Foo<Int>, Foo<String>}... vs Foo<$T0>...; We're going to
  // bind $T0 to Pack{Int, String}.
  } else {
    assert(pattern1->is<PackType>() && !pattern2->is<PackType>());
    if (auto *pack1 = pattern1->getAs<PackType>()) {
      if (auto *pack2 = replaceTypeVariablesWithFreshPacks(
              *this, pattern2, pack1, locator)) {
        addConstraint(kind, pack1, pack2, locator);
        return getTypeMatchSuccess();
      }
    }

    return getTypeMatchFailure(locator);
  }
}

/// Check where a representation is a subtype of another.
///
/// The subtype relationship is defined as:
/// 1. any representation R is a sub-type of itself.
/// 2. a thin representation is a subtype of any other representation.
/// 3. a thick representation is a subtype of any other thick representation.
///
/// For example, since `@convention(c)` is a thin representation, and
/// `@convention(swift)` is a thick representation,
/// `@convention(c) (A) -> B` is a sub-type of `(A) -> B`.
///
/// NOTE: Unlike typical subtyping relationships, this is not anti-symmetric.
/// For example, @convention(c) and @convention(thin) are subtypes of each other
/// but not equal.
static bool
isSubtypeOf(FunctionTypeRepresentation potentialSubRepr,
            FunctionTypeRepresentation potentialSuperRepr) {
  return (potentialSubRepr == potentialSuperRepr)
       || isThinRepresentation(potentialSubRepr)
       || isThickRepresentation(potentialSuperRepr);
}

/// Returns true if `constraint extInfo1 extInfo2` is satisfied.
static bool matchFunctionRepresentations(FunctionType::ExtInfo einfo1,
                                         FunctionType::ExtInfo einfo2,
                                         ConstraintKind kind,
                                         ConstraintSystemOptions options) {
  auto rep1 = einfo1.getRepresentation();
  auto rep2 = einfo2.getRepresentation();
  bool clangTypeMismatch =
      (options.contains(ConstraintSystemFlags::UseClangFunctionTypes) &&
       (einfo1.getClangTypeInfo() != einfo2.getClangTypeInfo()));
  switch (kind) {
  case ConstraintKind::Bind:
  case ConstraintKind::BindParam:
  case ConstraintKind::BindToPointerType:
  case ConstraintKind::Equal:
    return (rep1 == rep2) && !clangTypeMismatch;

  case ConstraintKind::Subtype: {
    // Breakdown of cases:
    // 1. isSubtypeOf(rep1, rep2) == false (hence rep1 != rep2):
    //    In this case, this function will return false, indicating that we
    //    can't convert. E.g. you can't convert from @convention(swift) to
    //    @convention(c).
    // 2. isSubtypeOf(rep1, rep2) == true and rep1 != rep2:
    //    In this case, this function will return true, indicating that we
    //    can convert, because the Clang type doesn't matter when converting
    //    between different representations. E.g. it is okay to convert from
    //    @convention(c) (regardless of cType) to @convention(swift).
    // 3. isSubtypeOf(rep1, rep2) == true and rep1 == rep2:
    //    In this case, the function returns !clangTypeMismatch, as we forbid
    //    conversions between @convention(c) functions with different cTypes.
    return isSubtypeOf(rep1, rep2) && ((rep1 != rep2) || !clangTypeMismatch);
  }

  // [NOTE: diagnose-swift-to-c-convention-change]: @convention(swift) ->
  // @convention(c) conversions are permitted only in certain cases.
  //
  //   var w = 3; func f() { print(w) }; func g(_ : @convention(c) () -> ()) {}
  //   g(f); // OK
  //   let h = f as @convention(c) () -> (); g(h) // OK
  //   let k = f; g(k) // error
  //   func m() { let x = 0; g({ print(x) }) } // error
  //   func n() { let y = 0; func p() { }; g(p); } // OK
  //   func q() { let z = 0; func r() { print(z) }; g(r); } // error
  //
  // Since checking for disallowed cases requires access to captures,
  // it is simpler to defer diagnosing (to CSApply/SILGen) and return true here.
  case ConstraintKind::Conversion:
  case ConstraintKind::ArgumentConversion:
  case ConstraintKind::OperatorArgumentConversion:
    // For now, forbid conversion if representations match but cTypes differ.
    //
    // let f : @convention(c, cType: "id (*)(void) __attribute__((ns_returns_retained))")
    //           () -> AnyObject = ...
    // let _ : @convention(c, cType: "id (*)(void)")
    //           () -> AnyObject = f // error
    // let g : @convention(c, cType: "void (*)(void *)")
    //           (OpaquePointer?) -> () = ...
    // let _ : @convention(c, cType: "void (*)(MyCtx *)")
    //           (OpaquePointer?) -> () = g // error
    if ((rep1 == rep2) && clangTypeMismatch) {
      return false;
    }
    return true;

  case ConstraintKind::BridgingConversion:
  case ConstraintKind::ApplicableFunction:
  case ConstraintKind::DynamicCallableApplicableFunction:
  case ConstraintKind::BindOverload:
  case ConstraintKind::CheckedCast:
  case ConstraintKind::SubclassOf:
  case ConstraintKind::ConformsTo:
  case ConstraintKind::TransitivelyConformsTo:
  case ConstraintKind::Defaultable:
  case ConstraintKind::Disjunction:
  case ConstraintKind::Conjunction:
  case ConstraintKind::DynamicTypeOf:
  case ConstraintKind::EscapableFunctionOf:
  case ConstraintKind::OpenedExistentialOf:
  case ConstraintKind::KeyPath:
  case ConstraintKind::KeyPathApplication:
  case ConstraintKind::LiteralConformsTo:
  case ConstraintKind::OptionalObject:
  case ConstraintKind::SelfObjectOfProtocol:
  case ConstraintKind::UnresolvedValueMember:
  case ConstraintKind::ValueMember:
  case ConstraintKind::ValueWitness:
  case ConstraintKind::OneWayEqual:
  case ConstraintKind::OneWayBindParam:
  case ConstraintKind::DefaultClosureType:
  case ConstraintKind::UnresolvedMemberChainBase:
  case ConstraintKind::PropertyWrapper:
  case ConstraintKind::SyntacticElement:
  case ConstraintKind::BindTupleOfFunctionParams:
  case ConstraintKind::PackElementOf:
  case ConstraintKind::ShapeOf:
  case ConstraintKind::ExplicitGenericArguments:
  case ConstraintKind::SameShape:
    return true;
  }

  llvm_unreachable("Unhandled ConstraintKind in switch.");
}

/// Check whether given parameter list represents a single tuple
/// or type variable which could be later resolved to tuple.
/// This is useful for SE-0110 related fixes in `matchFunctionTypes`.
static bool isSingleTupleParam(ASTContext &ctx,
                               ArrayRef<AnyFunctionType::Param> params) {
  if (params.size() != 1)
    return false;

  const auto &param = params.front();
  if ((param.isVariadic() || isPackExpansionType(param.getPlainType())) ||
      param.isInOut() || param.hasLabel() || param.isIsolated())
    return false;

  auto paramType = param.getPlainType();

  // Support following case which was allowed until 5:
  //
  // func bar(_: (Int, Int) -> Void) {}
  // let foo: ((Int, Int)?) -> Void = { _ in }
  //
  // bar(foo) // Ok
  if (!ctx.isSwiftVersionAtLeast(5))
    paramType = paramType->lookThroughAllOptionalTypes();

  // Parameter type should either a tuple or something that can become a
  // tuple later on.
  return (paramType->is<TupleType>() || paramType->isTypeVariableOrMember());
}

static ConstraintFix *fixRequirementFailure(ConstraintSystem &cs, Type type1,
                                            Type type2, ASTNode anchor,
                                            ArrayRef<LocatorPathElt> path);

static ConstraintFix *fixRequirementFailure(ConstraintSystem &cs, Type type1,
                                            Type type2,
                                            ConstraintLocatorBuilder locator) {
  SmallVector<LocatorPathElt, 4> path;

  auto anchor = locator.getLocatorParts(path);
  return fixRequirementFailure(cs, type1, type2, anchor, path);
}

static unsigned
assessRequirementFailureImpact(ConstraintSystem &cs, Type requirementType,
                               ConstraintLocatorBuilder locator) {
  assert(requirementType);

  unsigned impact = 1;
  auto anchor = locator.getAnchor();
  if (!anchor)
    return impact;

  // If this requirement is associated with a member reference and it
  // was possible to check it before overload choice is bound, that means
  // types came from the context (most likely Self, or associated type(s))
  // and failing this constraint makes member unrelated/inaccessible, so
  // the impact has to be adjusted accordingly in order for this fix not to
  // interfere with other overload choices.
  //
  // struct S<T> {}
  // extension S where T == AnyObject { func foo() {} }
  //
  // func bar(_ s: S<Int>) { s.foo() }
  //
  // In this case `foo` is only accessible if T == `AnyObject`, which makes
  // fix for same-type requirement higher impact vs. requirement associated
  // with method itself e.g. `func foo<U>() -> U where U : P {}` because
  // `foo` is accessible from any `S` regardless of what `T` is.
  //
  // Don't add this impact with the others, as we want to keep it consistent
  // across requirement failures to present the user with a choice.
  if (isExpr<UnresolvedDotExpr>(anchor) ||
      isExpr<UnresolvedMemberExpr>(anchor)) {
    auto *calleeLoc = cs.getCalleeLocator(cs.getConstraintLocator(locator));
    if (!cs.findSelectedOverloadFor(calleeLoc))
      return 10;
  }
  
  auto resolvedTy = cs.simplifyType(requirementType);

  // Increase the impact of a conformance fix for generic parameters on
  // operators where such conformance failures are not as important as argument
  // mismatches or contextual failures.
  if (auto *ODRE = getAsExpr<OverloadedDeclRefExpr>(anchor)) {
    if (locator.isForRequirement(RequirementKind::Conformance) &&
        resolvedTy->is<ArchetypeType>() && ODRE->isForOperator()) {
      ++impact;
    }
  }

  if (locator.isForRequirement(RequirementKind::Conformance)) {
    // Increase the impact of a conformance fix for a standard library
    // or foundation type, as it's unlikely to be a good suggestion.
    if (resolvedTy->isStdlibType() ||
        getKnownFoundationEntity(resolvedTy->getString())) {
      impact += 2;
    }
    // Also do the same for the builtin compiler types Any and AnyObject, but
    // bump the impact even higher as they cannot conform to protocols at all.
    if (resolvedTy->isAny() || resolvedTy->isAnyObject())
      impact += 4;
  }

  // If this requirement is associated with an overload choice let's
  // tie impact to how many times this requirement type is mentioned.
  if (auto *ODRE = getAsExpr<OverloadedDeclRefExpr>(anchor)) {
    if (auto *typeVar = requirementType->getAs<TypeVariableType>()) {
      unsigned choiceImpact = 0;
      if (auto choice = cs.findSelectedOverloadFor(ODRE)) {
        choice->adjustedOpenedType.visit([&](Type type) {
          if (type->isEqual(typeVar))
            ++choiceImpact;
        });
      }
      // If the type is used multiple times in the signature, increase the
      // impact for every additional use.
      if (choiceImpact > 1)
        impact += choiceImpact - 1;
    }
  }

  // If this requirement is associated with a call that is itself
  // incorrect, let's increase impact to indicate that this failure
  // has a compounding effect on viability of the overload choice it
  // comes from.
  if (locator.endsWith<LocatorPathElt::AnyRequirement>()) {
    if (auto *expr = getAsExpr(anchor)) {
      if (auto *call = getAsExpr<ApplyExpr>(cs.getParentExpr(expr))) {
        if (call->getFn() == expr &&
            llvm::any_of(cs.getFixes(), [&](const auto &fix) {
              return getAsExpr(fix->getAnchor()) == call;
            }))
          impact += 2;
      }
    }
  }

  return impact;
}

/// Attempt to fix missing arguments by introducing type variables
/// and inferring their types from parameters.
static bool fixMissingArguments(ConstraintSystem &cs, ASTNode anchor,
                                SmallVectorImpl<AnyFunctionType::Param> &args,
                                ArrayRef<AnyFunctionType::Param> params,
                                unsigned numMissing,
                                ConstraintLocatorBuilder locator) {
  assert(args.size() < params.size());

  auto &ctx = cs.getASTContext();
  // If there are N parameters but a single closure argument
  // (which might be anonymous), it's most likely used as a
  // tuple e.g. `$0.0`.
  Optional<TypeBase *> argumentTuple;
  if (isSingleTupleParam(ctx, args)) {
    auto argType = args.back().getPlainType();
    // Let's unpack argument tuple into N arguments, this corresponds
    // to something like `foo { (bar: (Int, Int)) in }` where `foo`
    // has a single parameter of type `(Int, Int) -> Void`.
    if (auto *tuple = argType->getAs<TupleType>()) {
      args.pop_back();
      for (const auto &elt : tuple->getElements())
        args.emplace_back(elt.getType(), elt.getName());
    } else if (auto *typeVar = argType->getAs<TypeVariableType>()) {
      auto isParam = [](const Expr *expr) {
        if (auto *DRE = dyn_cast<DeclRefExpr>(expr)) {
          if (auto *decl = DRE->getDecl())
            return isa<ParamDecl>(decl);
        }
        return false;
      };

      // Something like `foo { x in }` or `foo { $0 }`
      if (auto *closure = getAsExpr<ClosureExpr>(anchor)) {
        cs.forEachExpr(closure, [&](Expr *expr) -> Expr * {
          if (auto *UDE = dyn_cast<UnresolvedDotExpr>(expr)) {
            if (!isParam(UDE->getBase()))
              return expr;

            auto name = UDE->getName().getBaseIdentifier();
            unsigned index = 0;
            if (!name.str().getAsInteger(10, index) ||
                llvm::any_of(params, [&](const AnyFunctionType::Param &param) {
                  return param.getLabel() == name;
                })) {
              argumentTuple.emplace(typeVar);
              args.pop_back();
              return nullptr;
            }
          }
          return expr;
        });
      }
    }
  }

  for (unsigned i = args.size(), n = params.size(); i != n; ++i) {
    auto *argLoc = cs.getConstraintLocator(
        anchor, LocatorPathElt::SynthesizedArgument(i));
    args.push_back(params[i].withType(
        cs.createTypeVariable(argLoc, TVO_CanBindToNoEscape)));
  }

  SmallVector<SynthesizedArg, 4> synthesizedArgs;
  synthesizedArgs.reserve(numMissing);
  for (unsigned i = args.size() - numMissing, n = args.size(); i != n; ++i) {
    synthesizedArgs.push_back(SynthesizedArg{i, args[i]});
  }

  // Treat missing anonymous arguments as valid in closures containing the
  // code completion location, since they may have just not been written yet.
  if (cs.isForCodeCompletion()) {
    if (auto *closure = getAsExpr<ClosureExpr>(anchor)) {
      if (cs.containsIDEInspectionTarget(closure) &&
          (closure->hasAnonymousClosureVars() ||
           (args.empty() && closure->getInLoc().isInvalid())))
          return false;
    }
  }

  auto *fix = AddMissingArguments::create(cs, synthesizedArgs,
                                          cs.getConstraintLocator(locator));
  if (cs.recordFix(fix))
    return true;

  // If the argument was a single "tuple", let's bind newly
  // synthesized arguments to it.
  if (argumentTuple) {
    // We can ignore parameter flags here as we're imploding a tuple for a
    // simulated ((X, Y, Z)) -> R to (X, Y, Z) -> R conversion. As such, this is
    // similar to e.g { x, y, z in fn((x, y, z)) }.
    cs.addConstraint(ConstraintKind::Bind, *argumentTuple,
                     FunctionType::composeTuple(
                         ctx, args, ParameterFlagHandling::IgnoreNonEmpty),
                     cs.getConstraintLocator(anchor));
  }

  return false;
}

static bool fixExtraneousArguments(ConstraintSystem &cs,
                                   FunctionType *contextualType,
                                   ArrayRef<AnyFunctionType::Param> args,
                                   int numExtraneous,
                                   ConstraintLocatorBuilder locator) {
  SmallVector<std::pair<unsigned, AnyFunctionType::Param>, 4> extraneous;

  for (unsigned i = args.size() - numExtraneous, n = args.size(); i != n; ++i) {
    extraneous.push_back({i, args[i]});
    if (auto *typeVar = args[i].getPlainType()->getAs<TypeVariableType>()) {
      cs.recordPotentialHole(typeVar);
    }
  }

  return cs.recordFix(
      RemoveExtraneousArguments::create(cs, contextualType, extraneous,
                                        cs.getConstraintLocator(locator)),
      /*impact=*/numExtraneous * 2);
}

bool ConstraintSystem::hasPreconcurrencyCallee(
    ConstraintLocatorBuilder locator) {
  auto calleeLocator = getCalleeLocator(getConstraintLocator(locator));
  auto calleeOverload = findSelectedOverloadFor(calleeLocator);
  if (!calleeOverload || !calleeOverload->choice.isDecl())
    return false;

  return calleeOverload->choice.getDecl()->preconcurrency();
}

/// Determines whether a DeclContext is preconcurrency, using information
/// tracked by the solver to aid in answering that.
static bool isPreconcurrency(ConstraintSystem &cs, DeclContext *dc) {
  if (auto *decl = dc->getAsDecl())
    return decl->preconcurrency();

  if (auto *ce = dyn_cast<ClosureExpr>(dc)) {
    return ClosureIsolatedByPreconcurrency{cs}(ce);
  }

  if (auto *autoClos = dyn_cast<AutoClosureExpr>(dc)) {
    return isPreconcurrency(cs, autoClos->getParent());
  }

  llvm_unreachable("unhandled DeclContext kind in isPreconcurrency");
}

/// A thin wrapper around \c swift::safeToDropGlobalActor that provides the
/// ability to infer the isolation of a closure. Not perfect but mostly works.
static bool okToRemoveGlobalActor(ConstraintSystem &cs,
                                  DeclContext *dc,
                                  Type globalActor, Type ty) {
  auto findGlobalActorForClosure = [&](AbstractClosureExpr *ace) -> ClosureActorIsolation {
    // FIXME: Because the actor isolation checking happens after constraint
    // solving, the closure expression does not yet have its actor isolation
    // set, i.e., the code that would call
    // `AbstractClosureExpr::setActorIsolation` hasn't run yet.
    // So, I expect the existing isolation to always be set to the default.
    // If the assertion below starts tripping, then this ad-hoc inference
    // is no longer needed!
    auto existingIso = ace->getActorIsolation();
    if (existingIso != ClosureActorIsolation()) {
      assert(false && "somebody set the closure's isolation already?");
      return existingIso;
    }

    // Otherwise, do an ad-hoc inference of the closure's isolation.

    // see if the closure's type has isolation.
    if (auto closType = GetClosureType{cs}(ace)) {
      if (auto fnTy = closType->getAs<AnyFunctionType>()) {
        if (auto globActor = fnTy->getGlobalActor()) {
          return ClosureActorIsolation::forGlobalActor(globActor,
                                                       isPreconcurrency(cs, ace));
        }
      }
    }

    // otherwise, check for an explicit annotation
    if (auto *ce = dyn_cast<ClosureExpr>(ace)) {
      if (auto globActor = getExplicitGlobalActor(ce)) {
        return ClosureActorIsolation::forGlobalActor(globActor,
                                                     isPreconcurrency(cs, ce));
      }
    }

    return existingIso;
  };

  return safeToDropGlobalActor(dc, globalActor, ty, findGlobalActorForClosure);
}

ConstraintSystem::TypeMatchResult
ConstraintSystem::matchFunctionTypes(FunctionType *func1, FunctionType *func2,
                                     ConstraintKind kind, TypeMatchOptions flags,
                                     ConstraintLocatorBuilder locator) {
  // A non-throwing function can be a subtype of a throwing function.
  if (func1->isThrowing() != func2->isThrowing()) {
    // Cannot drop 'throws'.
    if (func1->isThrowing() || kind < ConstraintKind::Subtype) {
      if (!shouldAttemptFixes())
        return getTypeMatchFailure(locator);

      auto *fix = DropThrowsAttribute::create(*this, func1, func2,
                                              getConstraintLocator(locator));
      if (recordFix(fix))
        return getTypeMatchFailure(locator);
    }
  }

  // A synchronous function can be a subtype of an 'async' function.
  if (func1->isAsync() != func2->isAsync()) {
    // Cannot drop 'async'.
    if (func1->isAsync() || kind < ConstraintKind::Subtype) {
      if (!shouldAttemptFixes())
        return getTypeMatchFailure(locator);

      auto *fix = DropAsyncAttribute::create(*this, func1, func2,
                                             getConstraintLocator(locator));
      if (recordFix(fix))
        return getTypeMatchFailure(locator);
    }

    bool forClosureInArgumentPosition =
        locator.endsWith<LocatorPathElt::ApplyArgToParam>() &&
        isa<ClosureExpr>(locator.trySimplifyToExpr());

    // Since it's possible to infer `async` from the body of a
    // closure, score for sync -> async mismatch is increased
    // while solver is matching arguments to parameters to
    // indicate than solution with such a mismatch is always
    // worse than one with synchronous functions on both sides.
    if (!forClosureInArgumentPosition)
      increaseScore(SK_SyncInAsync);
  }

  // A @Sendable function can be a subtype of a non-@Sendable function.
  if (func1->isSendable() != func2->isSendable()) {
    // Cannot add '@Sendable'.
    if (func2->isSendable() || kind < ConstraintKind::Subtype) {
      if (AddSendableAttribute::attempt(*this, kind, func1, func2, locator))
        return getTypeMatchFailure(locator);
    }
  }

  // A non-@noescape function type can be a subtype of a @noescape function
  // type.
  if (func1->isNoEscape() != func2->isNoEscape() &&
      (func1->isNoEscape() || kind < ConstraintKind::Subtype)) {
    if (!shouldAttemptFixes())
      return getTypeMatchFailure(locator);

    auto *fix = MarkExplicitlyEscaping::create(*this, func1, func2,
                                               getConstraintLocator(locator));

    if (recordFix(fix))
      return getTypeMatchFailure(locator);
  }

  // A global actor can be added or can match, but cannot be removed.
  if (func1->getGlobalActor() || func2->getGlobalActor()) {
    if (func1->getGlobalActor() && func2->getGlobalActor()) {
      // If both have a global actor, match them.
      const auto subflags = getDefaultDecompositionOptions(flags);
      auto result = matchTypes(
          func1->getGlobalActor(), func2->getGlobalActor(),
          ConstraintKind::Equal, subflags,
          locator.withPathElement(LocatorPathElt::GlobalActorType()));
      if (result == SolutionKind::Error)
        return getTypeMatchFailure(locator);

    } else if (func1->getGlobalActor() && !func2->isAsync()) {
      // Cannot remove a global actor from a synchronous function in mismatched
      // DeclContext.
      //
      // FIXME: the ConstraintSystem's DeclContext is not always precise enough
      // to give an accurate answer. We want the innermost DeclContext that
      // contains the expression associated with the constraint locator.
      // Sometimes the ConstraintSystem only has the enclosing function, and not
      // the inner closure containing the expression. To workaround this,
      // `ActorIsolationChecker::checkFunctionConversion`, has extra checking of
      // function conversions specifically to account for this false positive.
      // This means we may have duplicate diagnostics emitted.
      if (okToRemoveGlobalActor(*this, DC, func1->getGlobalActor(), func2)) {
        // FIXME: this is a bit of a hack to workaround multiple solutions
        // because in these contexts it's valid to both add or remove the actor
        // from these function types. At least with the score increases, we
        // can bias the solver to pick the solution with fewer conversions.
        increaseScore(SK_FunctionConversion);

      } else if (MarkGlobalActorFunction::attempt(*this, kind, func1, func2, locator)) {
        return getTypeMatchFailure(locator);
      }

    } else if (kind < ConstraintKind::Subtype) {
      return getTypeMatchFailure(locator);
    } else {
      // It is possible to convert from a function without a global actor to a
      // similar function type that does have a global actor. But, since there
      // is a function conversion going on here, let's increase the score to
      // avoid ambiguity when solver can also match a global actor matching
      // function type.
      // FIXME: there may be a better way. see https://github.com/apple/swift/pull/62514
      increaseScore(SK_FunctionConversion);
    }
  }

  // To contextual type increase the score to avoid ambiguity when solver can
  // find more than one viable binding different only in representation e.g.
  //   let _: (@convention(block) () -> Void)? = Bool.random() ? nil : {}
  // so same representation should be always favored.
  auto loc = getConstraintLocator(locator);
  if (loc->findLast<LocatorPathElt::ContextualType>() &&
      func1->getRepresentation() != func2->getRepresentation()) {
    increaseScore(SK_FunctionConversion);
  }

  if (!matchFunctionRepresentations(func1->getExtInfo(), func2->getExtInfo(),
                                    kind, Options)) {
    return getTypeMatchFailure(locator);
  }

  // Determine how we match up the input/result types.
  ConstraintKind subKind;
  switch (kind) {
  case ConstraintKind::Bind:
  case ConstraintKind::BindParam:
  case ConstraintKind::BindToPointerType:
  case ConstraintKind::Equal:
    subKind = kind;
    break;

  case ConstraintKind::Subtype:
  case ConstraintKind::Conversion:
  case ConstraintKind::ArgumentConversion:
  case ConstraintKind::OperatorArgumentConversion:
    subKind = ConstraintKind::Subtype;
    break;

  case ConstraintKind::ApplicableFunction:
  case ConstraintKind::DynamicCallableApplicableFunction:
  case ConstraintKind::BindOverload:
  case ConstraintKind::CheckedCast:
  case ConstraintKind::SubclassOf:
  case ConstraintKind::ConformsTo:
  case ConstraintKind::TransitivelyConformsTo:
  case ConstraintKind::Defaultable:
  case ConstraintKind::Disjunction:
  case ConstraintKind::Conjunction:
  case ConstraintKind::DynamicTypeOf:
  case ConstraintKind::EscapableFunctionOf:
  case ConstraintKind::OpenedExistentialOf:
  case ConstraintKind::KeyPath:
  case ConstraintKind::KeyPathApplication:
  case ConstraintKind::LiteralConformsTo:
  case ConstraintKind::OptionalObject:
  case ConstraintKind::SelfObjectOfProtocol:
  case ConstraintKind::UnresolvedValueMember:
  case ConstraintKind::ValueMember:
  case ConstraintKind::ValueWitness:
  case ConstraintKind::BridgingConversion:
  case ConstraintKind::OneWayEqual:
  case ConstraintKind::OneWayBindParam:
  case ConstraintKind::DefaultClosureType:
  case ConstraintKind::UnresolvedMemberChainBase:
  case ConstraintKind::PropertyWrapper:
  case ConstraintKind::SyntacticElement:
  case ConstraintKind::BindTupleOfFunctionParams:
  case ConstraintKind::PackElementOf:
  case ConstraintKind::ShapeOf:
  case ConstraintKind::ExplicitGenericArguments:
  case ConstraintKind::SameShape:
    llvm_unreachable("Not a relational constraint");
  }

  // Input types can be contravariant (or equal).
  auto argumentLocator =
      locator.withPathElement(ConstraintLocator::FunctionArgument);

  TypeMatchOptions subflags = getDefaultDecompositionOptions(flags);

  SmallVector<AnyFunctionType::Param, 8> func1Params;
  func1Params.append(func1->getParams().begin(), func1->getParams().end());

  SmallVector<AnyFunctionType::Param, 8> func2Params;
  func2Params.append(func2->getParams().begin(), func2->getParams().end());

  // Add a very narrow exception to SE-0110 by allowing functions that
  // take multiple arguments to be passed as an argument in places
  // that expect a function that takes a single tuple (of the same
  // arity);
  auto canImplodeParams = [&](ArrayRef<AnyFunctionType::Param> params,
                              const FunctionType *destFn) {
    if (params.size() == 1)
      return false;

    // We do not support imploding into a @differentiable function.
    if (destFn->isDifferentiable())
      return false;

    for (auto &param : params) {
      // We generally cannot handle parameter flags, though we can carve out an
      // exception for ownership flags such as __owned, which we can thunk, and
      // flags that can freely dropped from a function type such as
      // @_nonEphemeral. Note that @noDerivative can also be freely dropped, as
      // we've already ensured that the destination function is not
      // @differentiable.
      auto flags = param.getParameterFlags();
      flags = flags.withOwnershipSpecifier(
          param.isInOut() ? ParamSpecifier::InOut : ParamSpecifier::Default);
      flags = flags.withNonEphemeral(false)
                   .withNoDerivative(false);
      if (!flags.isNone())
        return false;
    }
    return true;
  };

  auto implodeParams = [&](SmallVectorImpl<AnyFunctionType::Param> &params) {
    // Form an imploded tuple type, dropping the parameter flags as although
    // canImplodeParams makes sure we're not dealing with vargs, inout, etc,
    // we may still have e.g ownership flags left over, which we can drop.
    auto input = AnyFunctionType::composeTuple(
        getASTContext(), params, ParameterFlagHandling::IgnoreNonEmpty);
    params.clear();
    // If fixes are disabled let's do an easy thing and implode
    // tuple directly into parameters list.
    if (!shouldAttemptFixes()) {
      params.emplace_back(input);
      return;
    }

    // Synthesize new argument and bind it to tuple formed from existing
    // arguments, this makes it easier to diagnose cases where we attempt
    // a single tuple element formed when no arguments were present.
    auto argLoc = argumentLocator.withPathElement(
        LocatorPathElt::SynthesizedArgument(0));
    auto *typeVar = createTypeVariable(getConstraintLocator(argLoc),
                                       TVO_CanBindToNoEscape);
    params.emplace_back(typeVar);
    assignFixedType(typeVar, input);
  };

  {
    SmallVector<LocatorPathElt, 4> path;
    locator.getLocatorParts(path);

    // Find the last path element, skipping OptionalPayload elements
    // so that we allow this exception in cases of optional injection.
    auto last = std::find_if(
        path.rbegin(), path.rend(), [](LocatorPathElt &elt) -> bool {
          return elt.getKind() != ConstraintLocator::OptionalPayload;
        });

    auto &ctx = getASTContext();
    if (last != path.rend()) {
      if (last->getKind() == ConstraintLocator::ApplyArgToParam) {
        if (isSingleTupleParam(ctx, func2Params) &&
            canImplodeParams(func1Params, /*destFn*/ func2)) {
          implodeParams(func1Params);
          increaseScore(SK_FunctionConversion);
        } else if (!ctx.isSwiftVersionAtLeast(5) &&
                   isSingleTupleParam(ctx, func1Params) &&
                   canImplodeParams(func2Params,  /*destFn*/ func1)) {
          auto *simplified = locator.trySimplifyToExpr();
          // We somehow let tuple unsplatting function conversions
          // through in some cases in Swift 4, so let's let that
          // continue to work, but only for Swift 4.
          if (simplified &&
              (isa<DeclRefExpr>(simplified) ||
               isa<OverloadedDeclRefExpr>(simplified) ||
               isa<UnresolvedDeclRefExpr>(simplified))) {
            implodeParams(func2Params);
            increaseScore(SK_FunctionConversion);
          }
        }
      } else if (last->is<LocatorPathElt::PatternMatch>() &&
                 isa<EnumElementPattern>(
                     last->castTo<LocatorPathElt::PatternMatch>()
                         .getPattern())) {
        // A single paren pattern becomes a labeled tuple pattern
        // e.g. `case .test(let value):` should be able to match
        // `case test(result: Int)`. Note that it also means that:
        // `cast test(result: (String, Int))` would be matched against
        // e.g. `case .test((let x, let y))` but that fails during
        // pattern coercion (behavior consistent with what happens in
        // `TypeCheckPattern`).
        if (func1Params.size() == 1 && !func1Params.front().hasLabel() &&
            func2Params.size() == 1 && func2Params.front().hasLabel()) {
          auto param = func1Params.front();
          auto label = func2Params.front().getLabel();

          auto labeledParam = FunctionType::Param(param.getPlainType(), label,
                                                  param.getParameterFlags());

          func1Params.clear();
          func1Params.push_back(labeledParam);
        }

        // Consider following example:
        //
        // enum E {
        //   case foo((x: Int, y: Int))
        //   case bar(x: Int, y: Int)
        // }
        //
        // func test(e: E) {
        //   if case .foo(let x, let y) = e {}
        //   if case .bar(let tuple) = e {}
        // }
        //
        // Both of `if case` expressions have to be supported:
        //
        // 1. `case .foo(let x, let y) = e` allows a single tuple
        //    parameter to be "destructured" into multiple arguments.
        //
        // 2. `case .bar(let tuple) = e` allows to match multiple
        //    parameters with a single tuple argument.
        if (isSingleTupleParam(ctx, func1Params) &&
            canImplodeParams(func2Params, /*destFn*/ func1)) {
          implodeParams(func2Params);
          increaseScore(SK_FunctionConversion);
        } else if (isSingleTupleParam(ctx, func2Params) &&
                   canImplodeParams(func1Params, /*destFn*/ func2)) {
          implodeParams(func1Params);
          increaseScore(SK_FunctionConversion);
        }
      }
    }

    if (shouldAttemptFixes()) {
      auto *anchor = locator.trySimplifyToExpr();
      if (isa_and_nonnull<ClosureExpr>(anchor) &&
          isSingleTupleParam(ctx, func2Params) &&
          canImplodeParams(func1Params, /*destFn*/ func2)) {
        auto *fix = AllowClosureParamDestructuring::create(
            *this, func2, getConstraintLocator(anchor));
        if (recordFix(fix))
          return getTypeMatchFailure(argumentLocator);

        implodeParams(func1Params);
      }
    }
  }

  // https://github.com/apple/swift/issues/49345
  // Add a super-narrow hack to allow '(()) -> T' to be passed in place
  // of '() -> T'.
  if (getASTContext().isSwiftVersionAtLeast(4) &&
      !getASTContext().isSwiftVersionAtLeast(5)) {
    SmallVector<LocatorPathElt, 4> path;
    locator.getLocatorParts(path);

    // Find the last path element, skipping GenericArgument elements
    // so that we allow this exception in cases of optional types, and
    // skipping OptionalPayload elements so that we allow this
    // exception in cases of optional injection.
    auto last = std::find_if(
        path.rbegin(), path.rend(), [](LocatorPathElt &elt) -> bool {
          return elt.getKind() != ConstraintLocator::GenericArgument &&
                 elt.getKind() != ConstraintLocator::OptionalPayload;
        });

    if (last != path.rend()) {
      if (last->getKind() == ConstraintLocator::ApplyArgToParam) {
        if (isSingleTupleParam(getASTContext(), func1Params) &&
            func1Params[0].getOldType()->isVoid()) {
          if (func2Params.empty()) {
            func2Params.emplace_back(getASTContext().TheEmptyTupleType);
          }
        }
      }
    }
  }

  // FIXME: ParamPackMatcher should completely replace the non-variadic
  // case too eventually.
  if (containsPackExpansionType(func1Params) ||
      containsPackExpansionType(func2Params)) {
    ParamPackMatcher matcher(func1Params, func2Params, getASTContext(),
                             isPackExpansionType);
    if (matcher.match())
      return getTypeMatchFailure(locator);

    for (auto pair : matcher.pairs) {
      auto result = matchTypes(pair.lhs, pair.rhs, subKind, subflags,
                               (func1Params.size() == 1
                                ? argumentLocator
                                : argumentLocator.withPathElement(
                                  LocatorPathElt::TupleElement(pair.lhsIdx))));
      if (result.isFailure())
        return result;
    }
  } else {
    int diff = func1Params.size() - func2Params.size();
    if (diff != 0) {
      if (!shouldAttemptFixes())
        return getTypeMatchFailure(argumentLocator);

      auto *loc = getConstraintLocator(locator);

      // If this is conversion between optional (or IUO) parameter
      // and argument, let's drop the last path element so locator
      // could be simplified down to an argument expression.
      //
      // func foo(_: ((Int, Int) -> Void)?) {}
      // _ = foo { _ in } <- missing second closure parameter.
      if (loc->isLastElement<LocatorPathElt::OptionalPayload>()) {
        auto path = loc->getPath();
        loc = getConstraintLocator(loc->getAnchor(), path.drop_back());
      }

      auto anchor = simplifyLocatorToAnchor(loc);
      if (!anchor)
        return getTypeMatchFailure(argumentLocator);

      // If there are missing arguments, let's add them
      // using parameter as a template.
      if (diff < 0) {
        if (fixMissingArguments(*this, anchor, func1Params, func2Params,
                                abs(diff), loc))
          return getTypeMatchFailure(argumentLocator);
      } else {
        // If there are extraneous arguments, let's remove
        // them from the list.
        if (fixExtraneousArguments(*this, func2, func1Params, diff, loc))
          return getTypeMatchFailure(argumentLocator);

        // Drop all of the extraneous arguments.
        auto numParams = func2Params.size();
        func1Params.erase(func1Params.begin() + numParams, func1Params.end());
      }
    }

    bool hasLabelingFailures = false;
    for (unsigned i : indices(func1Params)) {
      auto func1Param = func1Params[i];
      auto func2Param = func2Params[i];

      // Increase the score if matching an autoclosure parameter to an function
      // type, so we enforce that non-autoclosure overloads are preferred.
      //
      //   func autoclosure(f: () -> Int) { }
      //   func autoclosure(f: @autoclosure () -> Int) { }
      //
      //   let _ = autoclosure as (() -> (Int)) -> () // non-autoclosure preferred
      //
      auto isAutoClosureFunctionMatch = [](AnyFunctionType::Param &param1,
                                           AnyFunctionType::Param &param2) {
        return param1.isAutoClosure() &&
               (!param2.isAutoClosure() &&
                param2.getPlainType()->is<FunctionType>());
      };

      if (isAutoClosureFunctionMatch(func1Param, func2Param) ||
          isAutoClosureFunctionMatch(func2Param, func1Param)) {
        increaseScore(SK_FunctionToAutoClosureConversion);
      }

      // Variadic bit must match.
      if (func1Param.isVariadic() != func2Param.isVariadic()) {
        if (!(shouldAttemptFixes() && func2Param.isVariadic()))
          return getTypeMatchFailure(argumentLocator);

        auto argType =
            getFixedTypeRecursive(func1Param.getPlainType(), /*wantRValue=*/true);
        auto varargsType = func2Param.getPlainType();

        // Delay solving this constraint until argument is resolved.
        if (argType->is<TypeVariableType>()) {
          addUnsolvedConstraint(Constraint::create(
              *this, kind, func1, func2, getConstraintLocator(locator)));
          return getTypeMatchSuccess();
        }

        auto *fix = ExpandArrayIntoVarargs::attempt(
            *this, argType, varargsType,
            argumentLocator.withPathElement(LocatorPathElt::ApplyArgToParam(
                i, i, func2Param.getParameterFlags())));

        if (!fix || recordFix(fix))
          return getTypeMatchFailure(argumentLocator);

        continue;
      }

      // Labels must match.
      //
      // FIXME: We should not end up with labels here at all, but we do
      // from invalid code in diagnostics, and as a result of code completion
      // directly building constraint systems.
      if (func1Param.getLabel() != func2Param.getLabel()) {
        if (!shouldAttemptFixes())
          return getTypeMatchFailure(argumentLocator);

        // If we are allowed to attempt fixes, let's ignore labeling
        // failures, and create a fix to re-label arguments if types
        // line up correctly.
        hasLabelingFailures = true;
      }

      // "isolated" can be added as a subtype relation, but otherwise must match.
      if (func1Param.isIsolated() != func2Param.isIsolated() &&
          !(func2Param.isIsolated() && subKind >= ConstraintKind::Subtype)) {
        return getTypeMatchFailure(argumentLocator);
      }

      // FIXME: We should check value ownership too, but it's not completely
      // trivial because of inout-to-pointer conversions.

      // For equality contravariance doesn't matter, but let's make sure
      // that types are matched in original order because that is important
      // when function types are equated as part of pattern matching.
      auto paramType1 = kind == ConstraintKind::Equal ? func1Param.getOldType()
                                                      : func2Param.getOldType();

      auto paramType2 = kind == ConstraintKind::Equal ? func2Param.getOldType()
                                                      : func1Param.getOldType();

      // Compare the parameter types.
      auto result = matchTypes(paramType1, paramType2, subKind, subflags,
                               (func1Params.size() == 1
                                ? argumentLocator
                                : argumentLocator.withPathElement(
                                      LocatorPathElt::TupleElement(i))));
      if (result.isFailure())
        return result;
    }

    if (hasLabelingFailures && !hasFixFor(loc)) {
      ConstraintFix *fix =
          loc->isLastElement<LocatorPathElt::ApplyArgToParam>()
              ? AllowArgumentMismatch::create(*this, func1, func2, loc)
              : ContextualMismatch::create(*this, func1, func2, loc);

      if (recordFix(fix))
        return getTypeMatchFailure(argumentLocator);
    }
  }

  // Result type can be covariant (or equal).
  return matchTypes(func1->getResult(), func2->getResult(), subKind,
                    subflags,
                    locator.withPathElement(ConstraintLocator::FunctionResult));
}

ConstraintSystem::TypeMatchResult
ConstraintSystem::matchSuperclassTypes(Type type1, Type type2,
                                       TypeMatchOptions flags,
                                       ConstraintLocatorBuilder locator) {
  TypeMatchOptions subflags = getDefaultDecompositionOptions(flags);

  auto classDecl2 = type2->getClassOrBoundGenericClass();
  SmallPtrSet<ClassDecl *, 4> superclasses1;
  for (auto super1 = type1->getSuperclass();
       super1;
       super1 = super1->getSuperclass()) {
    auto superclass1 = super1->getClassOrBoundGenericClass();
    if (superclass1 != classDecl2) {
      // Break if we have circular inheritance.
      if (superclass1 && !superclasses1.insert(superclass1).second)
        break;

      continue;
    }

    return matchTypes(super1, type2, ConstraintKind::Bind,
                      subflags, locator);
  }

  return getTypeMatchFailure(locator);
}

static ConstraintSystem::TypeMatchResult matchDeepTypeArguments(
    ConstraintSystem &cs, ConstraintSystem::TypeMatchOptions subflags,
    ArrayRef<Type> args1, ArrayRef<Type> args2,
    ConstraintLocatorBuilder locator,
    llvm::function_ref<void(unsigned)> recordMismatch = [](unsigned) {}) {
  if (args1.size() != args2.size()) {
    return cs.getTypeMatchFailure(locator);
  }

  auto allMatch = cs.getTypeMatchSuccess();
  for (unsigned i = 0, n = args1.size(); i != n; ++i) {
    auto result = cs.matchTypes(
        args1[i], args2[i], ConstraintKind::Bind, subflags,
        locator.withPathElement(LocatorPathElt::GenericArgument(i)));

    if (result.isFailure()) {
      recordMismatch(i);
      allMatch = result;
    }
  }

  return allMatch;
}

ConstraintSystem::TypeMatchResult
ConstraintSystem::matchDeepEqualityTypes(Type type1, Type type2,
                                         ConstraintLocatorBuilder locator) {
  TypeMatchOptions subflags = TMF_GenerateConstraints;

  // Handle opaque archetypes.
  if (auto arch1 = type1->getAs<ArchetypeType>()) {
    auto arch2 = type2->castTo<ArchetypeType>();
    auto opaque1 = cast<OpaqueTypeArchetypeType>(arch1);
    auto opaque2 = cast<OpaqueTypeArchetypeType>(arch2);
    assert(opaque1->getDecl() == opaque2->getDecl());

    // It's possible to declare a generic requirement like Self == Self.Iterator
    // where both types are going to be opaque.
    if (!opaque1->getCanonicalInterfaceType(arch1->getInterfaceType())
             ->isEqual(
                 opaque2->getCanonicalInterfaceType(arch2->getInterfaceType())))
      return getTypeMatchFailure(locator);

    auto args1 = opaque1->getSubstitutions().getReplacementTypes();
    auto args2 = opaque2->getSubstitutions().getReplacementTypes();

    if (!shouldAttemptFixes()) {
      // Match up the replacement types of the respective substitution maps.
      return matchDeepTypeArguments(*this, subflags, args1, args2, locator);
    }

    unsigned numMismatches = 0;
    auto result =
        matchDeepTypeArguments(*this, subflags, args1, args2, locator,
                               [&numMismatches](unsigned) { ++numMismatches; });

    if (numMismatches > 0) {
      auto anchor = locator.getAnchor();
      // TODO(diagnostics): Only assignments are supported at the moment.
      if (!isExpr<AssignExpr>(anchor))
        return getTypeMatchFailure(locator);

      auto *fix = IgnoreAssignmentDestinationType::create(
          *this, type1, type2, getConstraintLocator(locator));

      if (recordFix(fix, /*impact=*/numMismatches))
        return getTypeMatchFailure(locator);

      return getTypeMatchSuccess();
    }

    return result;
  }

  // Handle existential types.
  if (auto *existential1 = type1->getAs<ExistentialType>()) {
    auto existential2 = type2->castTo<ExistentialType>();

    auto result = matchTypes(existential1->getConstraintType(),
                             existential2->getConstraintType(),
                             ConstraintKind::Bind, subflags,
                             locator.withPathElement(
                               ConstraintLocator::ExistentialConstraintType));

    if (result.isFailure())
      return result;

    return getTypeMatchSuccess();
  }

  // Arguments of parameterized protocol types have to match on the nose.
  if (auto ppt1 = type1->getAs<ParameterizedProtocolType>()) {
    auto ppt2 = type2->castTo<ParameterizedProtocolType>();

    auto result = matchTypes(ppt1->getBaseType(),
                             ppt2->getBaseType(),
                             ConstraintKind::Bind, subflags,
                             locator.withPathElement(
                               ConstraintLocator::ParentType));

    if (result.isFailure())
      return result;

    return matchDeepTypeArguments(*this, subflags,
                                  ppt1->getArgs(),
                                  ppt2->getArgs(),
                                  locator);
  }

  if (type1->isExistentialType()) {
    auto layout1 = type1->getExistentialLayout();
    auto layout2 = type2->getExistentialLayout();

    // Explicit AnyObject and protocols must match exactly.
    if (layout1.hasExplicitAnyObject != layout2.hasExplicitAnyObject)
      return getTypeMatchFailure(locator);

    if (layout1.getProtocols().size() != layout2.getProtocols().size())
      return getTypeMatchFailure(locator);

    for (unsigned i: indices(layout1.getProtocols())) {
      if (layout1.getProtocols()[i] != layout2.getProtocols()[i])
        return getTypeMatchFailure(locator);
    }

    // This is the only interesting case. We might have type variables
    // on either side of the superclass constraint, so make sure we
    // recursively call matchTypes() here.
    if (layout1.explicitSuperclass || layout2.explicitSuperclass) {
      if (!layout1.explicitSuperclass || !layout2.explicitSuperclass)
        return getTypeMatchFailure(locator);

      auto subLocator = locator.withPathElement(
                          ConstraintLocator::ProtocolCompositionSuperclassType);
      auto result = matchTypes(layout1.explicitSuperclass,
                               layout2.explicitSuperclass,
                               ConstraintKind::Bind, subflags,
                               subLocator);
      if (result.isFailure())
        return result;
    }

    return getTypeMatchSuccess();
  }

  // Handle nominal types that are not directly generic.
  if (auto nominal1 = type1->getAs<NominalType>()) {
    auto nominal2 = type2->castTo<NominalType>();

    assert((bool)nominal1->getParent() == (bool)nominal2->getParent() &&
           "Mismatched parents of nominal types");

    if (!nominal1->getParent())
      return getTypeMatchSuccess();

    // Match up the parents, exactly.
    return matchTypes(nominal1->getParent(), nominal2->getParent(),
                      ConstraintKind::Bind, subflags,
                      locator.withPathElement(ConstraintLocator::ParentType));
  }

  auto bound1 = type1->castTo<BoundGenericType>();
  auto bound2 = type2->castTo<BoundGenericType>();

  // Match up the parents, exactly, if there are parents.
  assert((bool)bound1->getParent() == (bool)bound2->getParent() &&
         "Mismatched parents of bound generics");
  if (bound1->getParent()) {
    auto result = matchTypes(bound1->getParent(), bound2->getParent(),
                             ConstraintKind::Bind, subflags,
                             locator.withPathElement(
                                                ConstraintLocator::ParentType));
    if (result.isFailure())
      return result;
  }

  auto args1 = bound1->getGenericArgs();
  auto args2 = bound2->getGenericArgs();

  // Match up the generic arguments, exactly.

  if (shouldAttemptFixes()) {
    auto *baseLoc =
      getConstraintLocator(locator, {LocatorPathElt::GenericType(bound1),
          LocatorPathElt::GenericType(bound2)});

    auto argMatchingFlags =
      subflags | TMF_ApplyingFix | TMF_MatchingGenericArguments;

    // Optionals have a lot of special diagnostics and only one
    // generic argument so if we' re dealing with one, don't produce generic
    // arguments mismatch fixes.
    if (bound1->getDecl()->isOptionalDecl())
      return matchDeepTypeArguments(*this, argMatchingFlags, args1, args2,
                                    baseLoc);

    SmallVector<unsigned, 4> mismatches;
    auto result = matchDeepTypeArguments(
        *this, argMatchingFlags, args1, args2, baseLoc,
        [&mismatches](unsigned position) { mismatches.push_back(position); });

    if (mismatches.empty())
      return result;

    auto *loc = getConstraintLocator(locator);

    auto path = loc->getPath();
    if (!path.empty()) {
      // If we have something like ... -> type req # -> pack element #, we're
      // solving a requirement of the form T : P where T is a type parameter pack
      if (path.back().is<LocatorPathElt::PackElement>())
        path = path.drop_back();

      if (path.back().is<LocatorPathElt::AnyRequirement>()) {
        if (auto *fix = fixRequirementFailure(*this, type1, type2, locator)) {
          if (recordFix(fix))
            return getTypeMatchFailure(locator);

          increaseScore(SK_Fix, mismatches.size());
          return getTypeMatchSuccess();
        }
      }
    }

    unsigned impact = 1;

    if (type1->getAnyPointerElementType() &&
        type2->getAnyPointerElementType()) {
      // If this is a pointer <-> pointer conversion of different kind,
      // there is a dedicated restriction/fix for that in some cases.
      // To accommodate that, let's increase the impact of this fix.
      impact += 2;
    } else {
      // Increase the solution's score for each mismatch this fixes.
      impact += mismatches.size() - 1;
    }

    auto *fix = GenericArgumentsMismatch::create(
        *this, type1, type2, mismatches, loc);

    if (!recordFix(fix, impact))
      return getTypeMatchSuccess();

    return result;
  }
  return matchDeepTypeArguments(*this, subflags, args1, args2, locator);
}

ConstraintSystem::TypeMatchResult
ConstraintSystem::matchExistentialTypes(Type type1, Type type2,
                                        ConstraintKind kind,
                                        TypeMatchOptions flags,
                                        ConstraintLocatorBuilder locator) {
  // If the first type is a type variable or member thereof, there's nothing
  // we can do now.
  if (type1->isTypeVariableOrMember()) {
    if (flags.contains(TMF_GenerateConstraints)) {
      addUnsolvedConstraint(
        Constraint::create(*this, kind, type1, type2,
                           getConstraintLocator(locator)));
      return getTypeMatchSuccess();
    }

    return getTypeMatchAmbiguous();
  }

  // move-only types (and their metatypes) cannot match with existential types.
  if (type1->getMetatypeInstanceType()->isPureMoveOnly()) {
    // tailor error message
    if (shouldAttemptFixes()) {
      auto *fix = MustBeCopyable::create(*this, type1,
                                        getConstraintLocator(locator));
      if (!recordFix(fix))
        return getTypeMatchSuccess();
    }
    return getTypeMatchFailure(locator);
  }

  // FIXME: Feels like a hack.
  if (type1->is<InOutType>())
    return getTypeMatchFailure(locator);

  // FIXME; Feels like a hack...nothing actually "conforms" here, and
  // we need to disallow conversions from types containing @noescape
  // functions to Any.

  // Conformance to 'Any' always holds.
  if (type2->isAny()) {
    if (!type1->isNoEscape())
      return getTypeMatchSuccess();

    if (shouldAttemptFixes()) {
      auto *fix = MarkExplicitlyEscaping::create(*this, type1, type2,
                                                 getConstraintLocator(locator));
      if (!recordFix(fix))
        return getTypeMatchSuccess();
    }

    return getTypeMatchFailure(locator);
  }

  // ConformsTo constraints are generated when opening a generic
  // signature with a RequirementKind::Conformance requirement, so
  // we must handle pack types on the left by splitting up into
  // smaller constraints.
  if (auto *packType = type1->getAs<PackType>()) {
    for (unsigned i = 0, e = packType->getNumElements(); i < e; ++i) {
      addConstraint(kind, packType->getElementType(i), type2,
                    locator.withPathElement(LocatorPathElt::PackElement(i)));
    }

    return getTypeMatchSuccess();
  }

  TypeMatchOptions subflags = getDefaultDecompositionOptions(flags);

  // Handle existential metatypes.
  if (auto meta1 = type1->getAs<MetatypeType>()) {
    ExistentialMetatypeType *meta2;
    if (auto existential = type2->getAs<ExistentialType>()) {
      meta2 = existential->getConstraintType()->getAs<ExistentialMetatypeType>();
    } else {
      meta2 = type2->getAs<ExistentialMetatypeType>();
    }
    if (meta2) {
      return matchExistentialTypes(meta1->getInstanceType(),
                                   meta2->getInstanceType(), kind, subflags,
                                   locator.withPathElement(
                                     ConstraintLocator::InstanceType));
    }
  }

  if (!type2->isExistentialType())
    return getTypeMatchFailure(locator);

  auto layout = type2->getExistentialLayout();

  if (auto layoutConstraint = layout.getLayoutConstraint()) {
    if (layoutConstraint->isClass()) {
      if (kind == ConstraintKind::ConformsTo) {
        if (!type1->satisfiesClassConstraint()) {
          if (shouldAttemptFixes()) {
            if (auto last = locator.last()) {
              // If solver is in diagnostic mode and type1 is a hole, or if this
              // is a superclass requirement, let's consider `AnyObject`
              // conformance solved. The actual superclass requirement
              // will also fail (because type can't satisfy it), and it's
              // more interesting for diagnostics.
              auto req = last->getAs<LocatorPathElt::AnyRequirement>();
              if (!req)
                return getTypeMatchFailure(locator);

              // Superclass constraints are never satisfied by existentials,
              // even those that contain the superclass a la `any C & P`.
              if (!type1->isExistentialType() &&
                  (type1->isPlaceholder() ||
                  req->getRequirementKind() == RequirementKind::Superclass))
                return getTypeMatchSuccess();

              auto *fix = fixRequirementFailure(*this, type1, type2, locator);
              if (fix && !recordFix(fix)) {
                recordFixedRequirement(getConstraintLocator(locator), type2);
                return getTypeMatchSuccess();
              }
            }
          }

          return getTypeMatchFailure(locator);
        }
      } else {
        // Subtype relation to AnyObject also allows class-bound
        // existentials that are not @objc and therefore carry
        // witness tables.
        if (!type1->isClassExistentialType() && !type1->mayHaveSuperclass()) {
          if (shouldAttemptFixes()) {
            llvm::SmallVector<LocatorPathElt, 4> path;
            if (auto anchor = locator.getLocatorParts(path)) {
              // Let's drop `optional` or `generic argument` bits from
              // locator because that helps to diagnose reference equality
              // operators ("===" and "!==") since there is always a
              // `value-to-optional` or `optional-to-optional` conversion
              // associated with them (expected argument is `AnyObject?`).
              if (!path.empty() &&
                  (path.back().is<LocatorPathElt::OptionalPayload>() ||
                   path.back().is<LocatorPathElt::GenericArgument>()))
                path.pop_back();

              auto *fix = AllowNonClassTypeToConvertToAnyObject::create(
                  *this, type1, getConstraintLocator(anchor, path));

              return recordFix(fix) ? getTypeMatchFailure(locator)
                                    : getTypeMatchSuccess();
            }
          }

          return getTypeMatchFailure(locator);
        }
      }

      // Keep going.
    }
  }

  if (layout.explicitSuperclass) {
    auto subKind = std::min(ConstraintKind::Subtype, kind);
    auto result = matchTypes(type1, layout.explicitSuperclass, subKind,
                             subflags, locator);
    if (result.isFailure())
      return result;
  }

  for (auto *protoDecl : layout.getProtocols()) {
    if (auto superclass = protoDecl->getSuperclass()) {
      auto subKind = std::min(ConstraintKind::Subtype, kind);
      auto result = matchTypes(type1, superclass, subKind,
                               subflags, locator);
      if (result.isFailure())
        return result;
    }

    switch (simplifyConformsToConstraint(type1, protoDecl, kind, locator,
                                         subflags)) {
      case SolutionKind::Solved:
      case SolutionKind::Unsolved:
        break;

      case SolutionKind::Error: {
        if (!shouldAttemptFixes())
          return getTypeMatchFailure(locator);

        SmallVector<LocatorPathElt, 4> path;
        auto anchor = locator.getLocatorParts(path);

        // If the path ends at `optional payload` it means that this
        // check is part of an implicit value-to-optional conversion,
        // and it could be safely dropped.
        if (!path.empty() && path.back().is<LocatorPathElt::OptionalPayload>())
          path.pop_back();

        // Determine whether this conformance mismatch is
        // associated with argument to a call, and if so
        // produce a tailored fix.
        if (!path.empty()) {
          auto last = path.back();

          if (last.is<LocatorPathElt::ApplyArgToParam>()) {
            auto proto = protoDecl->getDeclaredInterfaceType();
            // Impact is 2 here because there are two failures
            // 1 - missing conformance and 2 - incorrect argument type.
            //
            // This would make sure that arguments with incorrect
            // conformances are not prioritized over general argument
            // mismatches.
            if (type1->isOptional()) {
              auto unwrappedType = type1->lookThroughAllOptionalTypes();
              auto result = simplifyConformsToConstraint(
                  unwrappedType, protoDecl, kind, locator,
                  subflags | TMF_ApplyingFix);
              if (result == SolutionKind::Solved) {
                auto fix = ForceOptional::create(*this, type1, proto,
                                                 getConstraintLocator(locator));
                if (recordFix(fix))
                  return getTypeMatchFailure(locator);
                break;
              }
            }
            auto fix = AllowArgumentMismatch::create(
                  *this, type1, proto, getConstraintLocator(anchor, path));
            if (recordFix(fix, /*impact=*/2))
              return getTypeMatchFailure(locator);
            break;
          }

          // TODO(diagnostics): If there are any requirement failures associated
          // with result types which are part of a function type conversion,
          // let's record general conversion mismatch in order for it to capture
          // and display complete function types.
          //
          // Once either reacher locators or better diagnostic presentation for
          // nested type failures is available this check could be removed.
          if (last.is<LocatorPathElt::FunctionResult>())
            return getTypeMatchFailure(locator);

          // If instance types didn't line up correctly, let's produce a
          // diagnostic which mentions them together with their metatypes.
          if (last.is<LocatorPathElt::InstanceType>())
            return getTypeMatchFailure(locator);

        } else { // There are no elements in the path
          if (!(isExpr<AssignExpr>(anchor) || isExpr<CoerceExpr>(anchor)))
            return getTypeMatchFailure(locator);
        }

        if (isExpr<CoerceExpr>(anchor)) {
          auto *fix = ContextualMismatch::create(
              *this, type1, type2, getConstraintLocator(anchor, path));
          if (recordFix(fix))
            return getTypeMatchFailure(locator);
          break;
        }

        auto proto = protoDecl->getDeclaredInterfaceType();
        auto *fix = MissingConformance::forContextual(
            *this, type1, proto, getConstraintLocator(anchor, path));

        if (recordFix(fix))
          return getTypeMatchFailure(locator);

        break;
      }
    }
  }

  auto constraintType1 = type1;
  if (auto existential = constraintType1->getAs<ExistentialType>())
    constraintType1 = existential->getConstraintType();

  auto constraintType2 = type2;
  if (auto existential = constraintType2->getAs<ExistentialType>())
    constraintType2 = existential->getConstraintType();

  auto ppt1 = constraintType1->getAs<ParameterizedProtocolType>();
  auto ppt2 = constraintType2->getAs<ParameterizedProtocolType>();

  // With two parameterized protocols, we've already made sure conformance
  // constraints are satisfied. Try to match the arguments!
  if (ppt1 && ppt2) {
    ArrayRef<Type> longerArgs = ppt1->getArgs();
    ArrayRef<Type> shorterArgs = ppt2->getArgs();
    // The more constrained of the two types had better be the first type -
    // otherwise we're forgetting requirements.
    if (longerArgs.size() < shorterArgs.size()) {
      return getTypeMatchFailure(locator);
    }

    // Line up the arguments of the parameterized protocol.
    // FIXME: Extend the locator path to point to the argument
    // inducing the requirement.
    for (const auto &pair : llvm::zip_first(shorterArgs, longerArgs)) {
      auto result = matchTypes(std::get<0>(pair), std::get<1>(pair),
                               ConstraintKind::Bind,
                               subflags, locator);
      if (result.isFailure())
        return result;
    }
  } else if (ppt1 && type2->isExistentialType()) {
    // P<T, U, V, ...> converts to (P & Q & ...) trivially...
    return getTypeMatchSuccess();
  } else if (ppt2 && type1->isExistentialType()) {
    // But (P & Q & ...) does not convert to P<T, U, V, ...>
    return getTypeMatchFailure(locator);
  } else if (ppt1 || ppt2) {
    auto parameterized = constraintType1;
    auto base = constraintType2;
    if (ppt2)
      std::swap(parameterized, base);

    // One of the two is parameterized, and the other is a concrete type.
    // Substitute the base into the requirements of the parameterized type and
    // discharge the requirements of the parameterized protocol.
    // FIXME: Extend the locator path to point to the argument
    // inducing the requirement.
    SmallVector<Requirement, 2> reqs;
    parameterized->castTo<ParameterizedProtocolType>()
                 ->getRequirements(base, reqs);
    for (const auto &req : reqs) {
      assert(req.getKind() == RequirementKind::SameType);
      auto result = matchTypes(req.getFirstType(), req.getSecondType(),
                               ConstraintKind::Bind,
                               subflags, locator);
      if (result.isFailure())
        return result;
    }
  }

  return getTypeMatchSuccess();
}

static bool isStringCompatiblePointerBaseType(ASTContext &ctx,
                                              Type baseType) {
  // Allow strings to be passed to pointer-to-byte or pointer-to-void types.
  if (baseType->isInt8())
    return true;
  if (baseType->isUInt8())
    return true;
  if (baseType->isVoid())
    return true;
  
  return false;
}

/// Determine whether the first type with the given number of optionals
/// is potentially more optional than the second type with its number of
/// optionals.
static bool isPotentiallyMoreOptionalThan(Type type1, Type type2) {
  SmallVector<Type, 2> optionals1;
  Type objType1 = type1->lookThroughAllOptionalTypes(optionals1);
  auto numOptionals1 = optionals1.size();

  SmallVector<Type, 2> optionals2;
  type2->lookThroughAllOptionalTypes(optionals2);
  auto numOptionals2 = optionals2.size();

  if (numOptionals1 <= numOptionals2 && !objType1->isTypeVariableOrMember())
    return false;

  return true;
}

/// Enumerate all of the applicable optional conversion restrictions
static void enumerateOptionalConversionRestrictions(
                    Type type1, Type type2,
                    ConstraintKind kind, ConstraintLocatorBuilder locator,
                    llvm::function_ref<void(ConversionRestrictionKind)> fn) {
  // Optional-to-optional.
  if (type1->getOptionalObjectType() && type2->getOptionalObjectType())
    fn(ConversionRestrictionKind::OptionalToOptional);

  // Inject a value into an optional.
  if (isPotentiallyMoreOptionalThan(type2, type1)) {
    fn(ConversionRestrictionKind::ValueToOptional);
  }
}

/// Determine whether we can bind the given type variable to the given
/// fixed type.
static bool isBindable(TypeVariableType *typeVar, Type type) {
  return !ConstraintSystem::typeVarOccursInType(typeVar, type) &&
         !type->is<DependentMemberType>();
}

ConstraintSystem::TypeMatchResult
ConstraintSystem::matchTypesBindTypeVar(
    TypeVariableType *typeVar, Type origType, ConstraintKind kind,
    TypeMatchOptions flags, ConstraintLocatorBuilder locator,
    llvm::function_ref<TypeMatchResult()> formUnsolvedResult) {
  assert(typeVar->is<TypeVariableType>() && "Expected a type variable!");
  assert(!origType->is<TypeVariableType>() && "Expected a non-type variable!");

  // Simplify the right-hand type and perform the "occurs" check.
  typeVar = getRepresentative(typeVar);
  auto type = simplifyType(origType, flags);
  if (!isBindable(typeVar, type)) {
    if (shouldAttemptFixes()) {
      // If type variable is allowed to be a hole and it can't be bound to
      // a particular (full resolved) type, just ignore this binding
      // instead of re-trying it and failing later.
      if (typeVar->getImpl().canBindToHole() && !type->hasTypeVariable())
        return getTypeMatchSuccess();

      // Just like in cases where both sides are dependent member types
      // with resolved base that can't be simplified to a concrete type
      // let's ignore this mismatch and mark affected type variable as a hole
      // because something else has to be fixed already for this to happen.
      if (type->is<DependentMemberType>() && !type->hasTypeVariable()) {
        // Since the binding couldn't be performed, the type variable is a
        // hole regardless whether it would be bound later to some other
        // type or not. If this is not reflected in constraint system
        // it would let the solver to form a _valid_ solution as if the
        // constraint between the type variable and the unresolved dependent
        // member type never existed.
        increaseScore(SK_Hole);
        recordPotentialHole(typeVar);
        return getTypeMatchSuccess();
      }
    }

    return formUnsolvedResult();
  }

  // Since member lookup doesn't check requirements
  // it might sometimes return types which are not
  // visible in the current context e.g. typealias
  // defined in constrained extension, substitution
  // of which might produce error type for base, so
  // assignment should tread lightly and just fail
  // if it encounters such types.
  if (type->hasError())
    return getTypeMatchFailure(locator);

  // Equal constraints allow mixed LValue/RValue bindings, but
  // if we bind a type to a type variable that can bind to
  // LValues as part of simplifying the Equal constraint we may
  // later block a binding of the opposite "LValue-ness" to the
  // same type variable that happens as part of simplifying
  // another constraint.
  if (kind == ConstraintKind::Equal) {
    if (typeVar->getImpl().canBindToLValue())
      return formUnsolvedResult();

    type = type->getRValueType();
  }

  // Attempt to fix situations where type variable can't be bound
  // to a particular type e.g. `l-value` or `inout`.
  auto fixReferenceMismatch = [&](TypeVariableType *typeVar,
                                  Type type) -> bool {
    if (locator.endsWith<LocatorPathElt::ContextualType>()) {
      auto *fix = IgnoreContextualType::create(*this, typeVar, type,
                                               getConstraintLocator(locator));
      return !recordFix(fix);
    }

    return false;
  };

  // If the left-hand type variable cannot bind to an lvalue,
  // but we still have an lvalue, fail.
  if (!typeVar->getImpl().canBindToLValue() && type->hasLValueType()) {
    if (shouldAttemptFixes() && fixReferenceMismatch(typeVar, type))
      return getTypeMatchSuccess();

    return getTypeMatchFailure(locator);
  }

  // If the left-hand type variable cannot bind to an inout,
  // but we still have an inout, fail.
  if (!typeVar->getImpl().canBindToInOut() && type->is<InOutType>()) {
    if (shouldAttemptFixes() && fixReferenceMismatch(typeVar, type))
      return getTypeMatchSuccess();

    return getTypeMatchFailure(locator);
  }

  // If the left-hand type variable cannot bind to a non-escaping type,
  // but we still have a non-escaping type, fail.
  if (!typeVar->getImpl().canBindToNoEscape() && type->isNoEscape()) {
    if (shouldAttemptFixes()) {
      auto *fix = MarkExplicitlyEscaping::create(*this, typeVar, type,
                                                 getConstraintLocator(locator));
      if (recordFix(fix))
        return getTypeMatchFailure(locator);

      // Allow no-escape function to be bound with recorded fix.
    } else {
      return getTypeMatchFailure(locator);
    }
  }

  if (typeVar->getImpl().isPackExpansion()) {
    if (!flags.contains(TMF_BindingTypeVariable))
      return formUnsolvedResult();

    return resolvePackExpansion(typeVar, origType)
               ? getTypeMatchSuccess()
               : getTypeMatchFailure(locator);
  }

  // If we're attempting to bind a PackType or PackArchetypeType to a type
  // variable that doesn't support it, we have a pack reference outside of a
  // pack expansion expression.
  if (!typeVar->getImpl().canBindToPack() &&
      (type->is<PackArchetypeType>() || type->is<PackType>())) {
    if (!shouldAttemptFixes())
      return getTypeMatchFailure(locator);

    auto *fix = AllowInvalidPackReference::create(
        *this, type, getConstraintLocator(locator));
    if (recordFix(fix))
      return getTypeMatchFailure(locator);

    // Don't allow the invalid pack reference to propagate to other
    // bindings.
    type = PlaceholderType::get(typeVar->getASTContext(), typeVar);
  }

  // Binding to a pack expansion type is always an error in Swift 6 mode.
  // This indicates that a pack expansion expression was used in a context
  // that doesn't support it.
  //
  // In Swift 5 and earlier initializer references are handled in a special
  // way that uses a type variable to represent a type of the parameter
  // list. Such type variables should be allowed to bind to a pack expansion
  // type to support cases where initializer has a single unlabeled variadic
  // generic parameter - `init(_ data: repeat each T)`.
  //
  // See BindTupleOfFunctionParams constraint for more details.
  if (!typeVar->getImpl().isPackExpansion() && type->is<PackExpansionType>()) {
    bool representsParameterList =
        typeVar->getImpl()
            .getLocator()
            ->isLastElement<LocatorPathElt::ApplyArgument>();

    if (!(typeVar->getImpl().canBindToPack() && representsParameterList) ||
        getASTContext().isSwiftVersionAtLeast(6)) {
      if (!shouldAttemptFixes())
        return getTypeMatchFailure(locator);

      auto *fix = AllowInvalidPackExpansion::create(
          *this, getConstraintLocator(locator));
      if (recordFix(fix))
        return getTypeMatchFailure(locator);

      // Don't allow the pack expansion type to propagate to other
      // bindings.
      type = PlaceholderType::get(typeVar->getASTContext(), typeVar);
    }
  }

  // We do not allow keypaths to go through AnyObject. Let's create a fix
  // so this can be diagnosed later.
  if (auto loc = typeVar->getImpl().getLocator()) {
    auto locPath = loc->getPath();

    if (!locPath.empty() &&
        locPath.back().getKind() == ConstraintLocator::KeyPathRoot &&
        type->isAnyObject()) {
      auto *fix = AllowAnyObjectKeyPathRoot::create(
          *this, getConstraintLocator(locator));

      if (recordFix(fix))
        return getTypeMatchFailure(locator);
    }
  }

  // Okay. Bind below.

  // A constraint that binds any pointer to a void pointer is
  // ineffective, since any pointer can be converted to a void pointer.
  if (kind == ConstraintKind::BindToPointerType && type->isVoid()) {
    // Bind type1 to Void only as a last resort.
    addConstraint(ConstraintKind::Defaultable, typeVar, type,
                  getConstraintLocator(locator));
    return getTypeMatchSuccess();
  }

  // When binding a fixed type to a type variable that cannot contain
  // lvalues or noescape types, any type variables within the fixed
  // type cannot contain lvalues or noescape types either.
  if (type->hasTypeVariable()) {
    type.visit([&](Type t) {
      if (auto *tvt = dyn_cast<TypeVariableType>(t.getPointer())) {
        if (!typeVar->getImpl().canBindToLValue()) {
          tvt->getImpl().setCanBindToLValue(getSavedBindings(),
                                            /*enabled=*/false);
        }
        if (!typeVar->getImpl().canBindToNoEscape()) {
          tvt->getImpl().setCanBindToNoEscape(getSavedBindings(),
                                              /*enabled=*/false);
        }
      }
    });
  }

  if (typeVar->getImpl().isClosureType()) {
    return resolveClosure(typeVar, type, locator)
               ? getTypeMatchSuccess()
               : getTypeMatchFailure(locator);
  }

  assignFixedType(typeVar, type, /*updateState=*/true,
                  /*notifyInference=*/!flags.contains(TMF_BindingTypeVariable));

  return getTypeMatchSuccess();
}

static ConstraintFix *fixRequirementFailure(ConstraintSystem &cs, Type type1,
                                            Type type2, ASTNode anchor,
                                            ArrayRef<LocatorPathElt> path) {
  // Can't fix not yet properly resolved types.
  if (type1->isTypeVariableOrMember() || type2->isTypeVariableOrMember())
    return nullptr;

  // If we have something like ... -> type req # -> pack element #, we're
  // solving a requirement of the form T : P where T is a type parameter pack
  if (path.back().is<LocatorPathElt::PackElement>())
    path = path.drop_back();

  auto req = path.back().castTo<LocatorPathElt::AnyRequirement>();
  if (req.isConditionalRequirement()) {
    // path is - ... -> open generic -> type req # -> cond req #,
    // to identify type requirement we only need `open generic -> type req #`
    // part, because that's how fixes for type requirements are recorded.
    auto reqPath = path.drop_back();
    // If underlying conformance requirement has been fixed,
    // then there is no reason to fix up conditional requirements.
    if (cs.hasFixFor(cs.getConstraintLocator(anchor, reqPath)))
      return nullptr;
  }

  auto *reqLoc = cs.getConstraintLocator(anchor, path);

  switch (req.getRequirementKind()) {
  case RequirementKind::SameType:
    return SkipSameTypeRequirement::create(cs, type1, type2, reqLoc);

  case RequirementKind::SameShape:
    return SkipSameShapeRequirement::create(cs, type1, type2, reqLoc);

  case RequirementKind::Superclass:
    return SkipSuperclassRequirement::create(cs, type1, type2, reqLoc);

  case RequirementKind::Layout:
  case RequirementKind::Conformance:
    return MissingConformance::forRequirement(cs, type1, type2, reqLoc);
  }
  llvm_unreachable("covered switch");
}

static ConstraintFix *fixPropertyWrapperFailure(
    ConstraintSystem &cs, Type baseTy, ConstraintLocator *locator,
    llvm::function_ref<bool(SelectedOverload, VarDecl *, Type)> attemptFix,
    Optional<Type> toType = None) {
  // Don't attempt this fix if this is a key path dynamic member
  // lookup which produced no results. Unwrapping or wrapping
  // the base type is not going to produce desired results.
  if (locator->isForKeyPathDynamicMemberLookup())
    return nullptr;

  Expr *baseExpr = nullptr;
  if (auto *anchor = getAsExpr(locator->getAnchor())) {
    if (auto *UDE = dyn_cast<UnresolvedDotExpr>(anchor))
      baseExpr = UDE->getBase();
    else if (auto *SE = dyn_cast<SubscriptExpr>(anchor))
      baseExpr = SE->getBase();
    else if (auto *MRE = dyn_cast<MemberRefExpr>(anchor))
      baseExpr = MRE->getBase();
    else if (auto anchor = simplifyLocatorToAnchor(locator))
      baseExpr = getAsExpr(anchor);
  }

  if (!baseExpr)
    return nullptr;

  auto resolvedOverload = cs.findSelectedOverloadFor(baseExpr);
  if (!resolvedOverload)
    return nullptr;

  enum class Fix : uint8_t {
    ProjectedValue,
    PropertyWrapper,
    WrappedValue,
  };

  auto applyFix = [&](Fix fix, VarDecl *decl, Type type) -> ConstraintFix * {
    if (!decl->hasInterfaceType() || !type)
      return nullptr;

    if (baseTy->isEqual(type))
      return nullptr;

    if (baseTy->is<TypeVariableType>() || type->is<TypeVariableType>())
      return nullptr;

    if (!attemptFix(*resolvedOverload, decl, type))
      return nullptr;

    switch (fix) {
    case Fix::ProjectedValue:
    case Fix::PropertyWrapper:
      return UsePropertyWrapper::create(cs, decl, fix == Fix::ProjectedValue,
                                        baseTy, toType.value_or(type),
                                        locator);

    case Fix::WrappedValue:
      return UseWrappedValue::create(cs, decl, baseTy, toType.value_or(type),
                                     locator);
    }
    llvm_unreachable("Unhandled Fix type in switch");
  };

  if (auto projection =
          cs.getPropertyWrapperProjectionInfo(*resolvedOverload)) {
    if (auto *fix = applyFix(Fix::ProjectedValue, projection->first,
                             projection->second))
      return fix;
  }

  if (auto wrapper = cs.getPropertyWrapperInformation(*resolvedOverload)) {
    if (auto *fix =
            applyFix(Fix::PropertyWrapper, wrapper->first, wrapper->second))
      return fix;
  }

  if (auto wrappedProperty =
          cs.getWrappedPropertyInformation(*resolvedOverload)) {
    if (auto *fix = applyFix(Fix::WrappedValue, wrappedProperty->first,
                             wrappedProperty->second))
      return fix;
  }

  return nullptr;
}

static bool canBridgeThroughCast(ConstraintSystem &cs, Type fromType,
                                 Type toType) {
  // If we have a value of type AnyObject that we're trying to convert to
  // a class, force a downcast.
  // FIXME: Also allow types bridged through Objective-C classes.
  if (fromType->isAnyObject() && toType->getClassOrBoundGenericClass())
    return true;

  auto bridged = TypeChecker::getDynamicBridgedThroughObjCClass(cs.DC,
                                                                fromType, toType);
  if (!bridged)
    return false;

  // Note: don't perform this recovery for NSNumber;
  if (auto classType = bridged->getAs<ClassType>()) {
    SmallString<16> scratch;
    if (classType->getDecl()->isObjC() &&
        classType->getDecl()->getObjCRuntimeName(scratch) == "NSNumber")
      return false;
  }

  return true;
}

static bool
repairViaBridgingCast(ConstraintSystem &cs, Type fromType, Type toType,
                      SmallVectorImpl<RestrictionOrFix> &conversionsOrFixes,
                      ConstraintLocatorBuilder locator) {
  if (fromType->hasTypeVariable() || toType->hasTypeVariable())
    return false;

  auto objectType1 = fromType->getOptionalObjectType();
  auto objectType2 = toType->getOptionalObjectType();

  if (objectType1 && !objectType2) {
    auto *anchor = locator.trySimplifyToExpr();
    if (!anchor)
      return false;

    if (auto overload = cs.findSelectedOverloadFor(anchor)) {
      auto *decl = overload->choice.getDeclOrNull();
      if (decl && decl->isImplicitlyUnwrappedOptional())
        fromType = objectType1;
    }
  }

  if (!canBridgeThroughCast(cs, fromType, toType))
    return false;

  if (!TypeChecker::checkedCastMaySucceed(fromType, toType, cs.DC))
    return false;

  conversionsOrFixes.push_back(ForceDowncast::create(
      cs, fromType, toType, cs.getConstraintLocator(locator)));
  return true;
}

static bool
repairViaOptionalUnwrap(ConstraintSystem &cs, Type fromType, Type toType,
                        ConstraintKind matchKind,
                        SmallVectorImpl<RestrictionOrFix> &conversionsOrFixes,
                        ConstraintLocatorBuilder locator) {
  fromType = fromType->getWithoutSpecifierType();

  if (!fromType->getOptionalObjectType() || toType->is<TypeVariableType>())
    return false;

  // If we have an optional type, try to force-unwrap it.
  // FIXME: Should we also try '?'?
  auto *anchor = locator.trySimplifyToExpr();
  if (!anchor)
    return false;

  // If this is a conversion to a non-optional contextual type e.g.
  // `let _: Bool = try? foo()` and `foo()` produces `Int`
  // we should diagnose it as type mismatch instead of missing unwrap.
  bool possibleContextualMismatch = [&]() {
    if (!locator.endsWith<LocatorPathElt::ContextualType>())
      return false;

    // If the contextual type is optional as well, it's definitely a
    // missing unwrap.
    if (toType->getOptionalObjectType())
      return false;

    // If this is a leading-dot syntax member chain with `?.`
    // notation, it wouldn't be possible to infer the base type
    // without the contextual type, so we have to treat it as
    // a missing unwrap.
    if (auto *OEE = getAsExpr<OptionalEvaluationExpr>(anchor)) {
      if (isExpr<UnresolvedMemberChainResultExpr>(OEE->getSubExpr()))
        return false;
    }

    return true;
  }();

  // `OptionalEvaluationExpr` doesn't add a new level of
  // optionality but it could be hiding concrete types
  // behind itself which we can use to better understand
  // how many levels of optionality have to be unwrapped.
  if (auto *OEE = dyn_cast<OptionalEvaluationExpr>(anchor)) {
    auto *subExpr = OEE->getSubExpr();

    // First, let's check whether it has been determined that
    // it was incorrect to use `?` in this position.
    if (cs.hasFixFor(cs.getConstraintLocator(subExpr), FixKind::RemoveUnwrap))
      return true;

    auto type = cs.getType(subExpr);
    // If the type of sub-expression is optional, type of the
    // `OptionalEvaluationExpr` could be safely ignored because
    // it doesn't add any type information.
    if (type->getOptionalObjectType())
      fromType = type;

    // Don't attempt the fix until sub-expression is resolved
    // if chain is not using leading-dot syntax. This is better
    // than attempting to propagate type information down optional
    // chain which is hard to diagnose.
    if (type->isTypeVariableOrMember() &&
        !isa<UnresolvedMemberChainResultExpr>(subExpr))
      return false;

    // If this is a conversion from optional chain to some
    // other type e.g. contextual type or a parameter type,
    // let's use `Bind` to match object types because
    // object type of the optional chain is a type variable.
    //
    // One exception is contextual conversion - in such cases
    // let's give optional chain a chance to infer its inner type
    // first, that makes it much easier to diagnose contextual
    // mismatch vs. missing optional unwrap.
    if (!possibleContextualMismatch && matchKind >= ConstraintKind::Conversion)
      matchKind = ConstraintKind::Bind;
  }

  if (auto *DRE = dyn_cast<DeclRefExpr>(anchor)) {
    if (DRE->getDecl()->isImplicit()) {
      // The expression that provides the first type is implicit and never
      // spelled out in source code, e.g. $match in an expression pattern.
      // Thus we cannot force unwrap the first type
      return false;
    }
  }

  if (auto *optTryExpr = dyn_cast<OptionalTryExpr>(anchor)) {
    auto subExprType = cs.getType(optTryExpr->getSubExpr());
    const bool isSwift5OrGreater =
        cs.getASTContext().LangOpts.isSwiftVersionAtLeast(5);

    if (subExprType->getOptionalObjectType()) {
      if (isSwift5OrGreater) {
        // For 'try?' expressions, a ForceOptional fix converts 'try?'
        // to 'try!'. If the sub-expression is optional, then a force-unwrap
        // won't change anything in Swift 5+ because 'try?' already avoids
        // adding an additional layer of Optional there.
        return false;
      }
    } else {
      // In cases when sub-expression isn't optional, 'try?'
      // always adds one level of optionality regardless of
      // language mode, so we can safely try to bind its
      // object type to contextual type without risk of
      // causing more optionality mismatches down the road.
      //
      // For contextual conversions let's give `try?` a chance to
      // infer inner type which, if incorrect, should result in
      // contextual conversion failure instead of optional unwrap.
      matchKind = possibleContextualMismatch ? ConstraintKind::Conversion
                                             : ConstraintKind::Bind;
    }
  }

  auto getObjectTypeAndUnwraps = [](Type type) -> std::pair<Type, unsigned> {
    SmallVector<Type, 2> optionals;
    Type objType = type->lookThroughAllOptionalTypes(optionals);
    return std::make_pair(objType, optionals.size());
  };

  Type fromObjectType, toObjectType;
  unsigned fromUnwraps, toUnwraps;

  std::tie(fromObjectType, fromUnwraps) = getObjectTypeAndUnwraps(fromType);
  std::tie(toObjectType, toUnwraps) = getObjectTypeAndUnwraps(toType);

  // Since equality is symmetric and it decays into a `Bind`, eagerly
  // unwrapping optionals from either side might be incorrect since
  // there is not enough information about what is expected e.g.
  // `Int?? equal T0?` just like `T0? equal Int??` allows `T0` to be
  // bound to `Int?` and there is no need to unwrap. Solver has to wait
  // until more information becomes available about what `T0` is expected
  // to be before taking action.
  if (matchKind == ConstraintKind::Equal &&
      (fromObjectType->is<TypeVariableType>() ||
       toObjectType->is<TypeVariableType>())) {
    return false;
  }

  // If `from` is not less optional than `to`, force unwrap is
  // not going to help here. In case of object type of `from`
  // is a type variable, let's assume that it might be optional.
  if (fromUnwraps <= toUnwraps && !fromObjectType->is<TypeVariableType>())
    return false;

  // If the result of optional chaining is converted to
  // an optional contextual type represented by a type
  // variable e.g. `T?`, there can be no optional mismatch
  // because `T` could be bound to an optional of any depth.
  if (isa<OptionalEvaluationExpr>(anchor) && toUnwraps > 0) {
    if (locator.endsWith<LocatorPathElt::ContextualType>() &&
        toObjectType->is<TypeVariableType>())
      return false;
  }

  auto result =
      cs.matchTypes(fromObjectType, toObjectType, matchKind,
                    ConstraintSystem::TypeMatchFlags::TMF_ApplyingFix, locator);
  if (!result.isSuccess())
    return false;

  conversionsOrFixes.push_back(ForceOptional::create(
      cs, fromType, toType, cs.getConstraintLocator(locator)));
  return true;
}

static bool repairArrayLiteralUsedAsDictionary(
    ConstraintSystem &cs, Type arrayType, Type dictType,
    ConstraintKind matchKind,
    SmallVectorImpl<RestrictionOrFix> &conversionsOrFixes,
    ConstraintLocator *loc) {
  if (auto *fix = TreatArrayLiteralAsDictionary::attempt(cs, dictType,
                                                         arrayType, loc)) {
    // Ignore any attempts at promoting the value to an optional as even after
    // stripping off all optionals above the underlying types won't match (array
    // vs dictionary).
    conversionsOrFixes.erase(
        llvm::remove_if(conversionsOrFixes,
                        [&](RestrictionOrFix &E) {
                          if (auto restriction = E.getRestriction())
                            return *restriction == ConversionRestrictionKind::
                                                       ValueToOptional ||
                                   *restriction == ConversionRestrictionKind::
                                                       OptionalToOptional;
                          return false;
                        }),
        conversionsOrFixes.end());

    conversionsOrFixes.push_back(fix);
    return true;
  }
  return false;
}

/// Let's check whether this is an out-of-order argument in binary
/// operator/function with concrete type parameters e.g.
/// `func ^^(x: Int, y: String)` called as `"" ^^ 42` instead of
/// `42 ^^ ""` and repair it by using out-of-order fix on the
/// parent locator.
static bool repairOutOfOrderArgumentsInBinaryFunction(
    ConstraintSystem &cs, SmallVectorImpl<RestrictionOrFix> &conversionsOrFixes,
    ConstraintLocator *locator) {
  if (!locator->isLastElement<LocatorPathElt::ApplyArgToParam>())
    return false;

  auto path = locator->getPath();
  auto *parentLoc =
      cs.getConstraintLocator(locator->getAnchor(), path.drop_back());

  if (cs.hasFixFor(parentLoc, FixKind::MoveOutOfOrderArgument))
    return true;

  auto *calleeLoc = cs.getCalleeLocator(locator);
  if (!calleeLoc)
    return false;

  auto overload = cs.findSelectedOverloadFor(calleeLoc);
  if (!(overload && overload->choice.isDecl()))
    return false;

  auto *fnType = overload->adjustedOpenedType->getAs<FunctionType>();
  if (!(fnType && fnType->getNumParams() == 2))
    return false;

  auto argument = simplifyLocatorToAnchor(locator);
  // Argument could be synthesized.
  if (!argument)
    return false;

  auto currArgIdx =
      locator->castLastElementTo<LocatorPathElt::ApplyArgToParam>().getArgIdx();

  // Argument is extraneous and has been re-ordered to match one
  // of two parameter types.
  if (currArgIdx >= 2)
    return false;

  auto otherArgIdx = currArgIdx == 0 ? 1 : 0;

  auto argType = cs.getType(argument);
  auto paramType = fnType->getParams()[otherArgIdx].getOldType();

  bool isOperatorRef = overload->choice.getDecl()->isOperator();

  // If one of the parameters is `inout`, we can't flip the arguments.
  {
    auto params = fnType->getParams();
    if (params[0].isInOut() != params[1].isInOut())
      return false;
  }

  auto matchArgToParam = [&](Type argType, Type paramType, ASTNode anchor) {
    auto *loc = cs.getConstraintLocator(anchor);
    // If argument (and/or parameter) is a generic type let's not even try this
    // fix because it would be impossible to match given types  without delaying
    // until more context becomes available.
    if (argType->hasTypeVariable() || paramType->hasTypeVariable())
      return cs.getTypeMatchFailure(loc);

    return cs.matchTypes(
        argType->lookThroughAllOptionalTypes(),
        paramType->lookThroughAllOptionalTypes(),
        isOperatorRef ? ConstraintKind::OperatorArgumentConversion
                      : ConstraintKind::ArgumentConversion,
        ConstraintSystem::TypeMatchFlags::TMF_ApplyingFix, loc);
  };

  auto result = matchArgToParam(argType, paramType, argument);
  if (result.isSuccess()) {
    // Let's check whether other argument matches current parameter type,
    // if it does - it's definitely out-of-order arguments issue.
    auto *otherArgLoc = cs.getConstraintLocator(
        parentLoc, LocatorPathElt::ApplyArgToParam(otherArgIdx, otherArgIdx,
                                                   ParameterTypeFlags()));
    auto otherArg = simplifyLocatorToAnchor(otherArgLoc);
    // Argument could be synthesized.
    if (!otherArg)
      return false;

    argType = cs.getType(otherArg);
    paramType = fnType->getParams()[currArgIdx].getOldType();

    result = matchArgToParam(argType, paramType, otherArg);
    if (result.isSuccess()) {
      conversionsOrFixes.push_back(MoveOutOfOrderArgument::create(
          cs, otherArgIdx, currArgIdx, {{0}, {1}}, parentLoc));
      return true;
    }
  }

  return false;
}

/// Attempt to repair typing failures and record fixes if needed.
/// \return true if at least some of the failures has been repaired
/// successfully, which allows type matcher to continue.
bool ConstraintSystem::repairFailures(
    Type lhs, Type rhs, ConstraintKind matchKind, TypeMatchOptions flags,
    SmallVectorImpl<RestrictionOrFix> &conversionsOrFixes,
    ConstraintLocatorBuilder locator) {
  SmallVector<LocatorPathElt, 4> path;
  auto anchor = locator.getLocatorParts(path);

  // If there is a missing explicit call it could be:
  //
  // a). Contextual e.g. `let _: R = foo`
  // b). Argument is a function value passed to parameter
  //     which expects its result type e.g. `foo(bar)`
  // c). Assignment destination type matches return type of
  //     of the function value e.g. `foo = bar` or `foo = .bar`
  auto repairByInsertingExplicitCall = [&](Type srcType, Type dstType) -> bool {
    auto fnType = srcType->getAs<FunctionType>();
    if (!fnType)
      return false;

    // If the locator isn't anchored at an expression, or the expression is
    // implicit, don't try to insert an explicit call into the source code.
    auto *loc = getConstraintLocator(locator);
    auto *anchor = getAsExpr(simplifyLocatorToAnchor(loc));
    if (!anchor || anchor->isImplicit())
      return false;

    if (isArgumentOfPatternMatchingOperator(loc))
      return false;

    // Don't attempt this fix for trailing closures.
    if (auto elt = loc->getLastElementAs<LocatorPathElt::ApplyArgToParam>()) {
      auto argumentList = getArgumentList(loc);
      if (argumentList->isTrailingClosureIndex(elt->getArgIdx()))
        return false;
    }

    // If argument is a function type and all of its parameters have
    // default values, let's see whether error is related to missing
    // explicit call.
    if (fnType->getNumParams() > 0) {
      auto overload = findSelectedOverloadFor(anchor);
      if (!(overload && overload->choice.isDecl()))
        return false;

      const auto &choice = overload->choice;
      ParameterListInfo info(fnType->getParams(), choice.getDecl(),
                             hasAppliedSelf(*this, choice));

      if (llvm::any_of(indices(fnType->getParams()),
                       [&info](const unsigned idx) {
                         return !info.hasDefaultArgument(idx);
                       }))
        return false;
    }

    auto resultType = fnType->getResult();
    // If this is situation like `x = { ... }` where closure results in
    // `Void`, let's not suggest to call the closure, because it's most
    // likely not intended.
    if (auto *assignment = getAsExpr<AssignExpr>(anchor)) {
      if (isa<ClosureExpr>(assignment->getSrc()) && resultType->isVoid())
        return false;
    }

    // If left-hand side is a function type but right-hand
    // side isn't, let's check it would be possible to fix
    // this by forming an explicit call.
    auto convertTo = dstType->lookThroughAllOptionalTypes();
    // Right-hand side can't be - a function, a type variable or dependent
    // member, or `Any` (if function conversion to `Any` didn't succeed there
    // is something else going on e.g. problem with escapiness).
    if (convertTo->is<FunctionType>() || convertTo->isTypeVariableOrMember() ||
        convertTo->isAny())
      return false;

    ConstraintKind matchKind;
    if (resultType->is<TypeVariableType>()) {
      matchKind = ConstraintKind::Equal;
    } else {
      matchKind = ConstraintKind::Conversion;
    }

    auto result = matchTypes(resultType, dstType, matchKind,
                             TypeMatchFlags::TMF_ApplyingFix, locator);

    if (result.isSuccess()) {
      conversionsOrFixes.push_back(
          InsertExplicitCall::create(*this, getConstraintLocator(locator)));
      return true;
    }

    return false;
  };

  auto repairByAnyToAnyObjectCast = [&](Type lhs, Type rhs) -> bool {
    if (!(lhs->isAny() && rhs->isAnyObject()))
      return false;

    conversionsOrFixes.push_back(MissingConformance::forContextual(
        *this, lhs, rhs, getConstraintLocator(locator)));
    return true;
  };

  auto repairByTreatingRValueAsLValue = [&](Type lhs, Type rhs) -> bool {
    if (!lhs->is<LValueType>() &&
        (rhs->is<LValueType>() || rhs->is<InOutType>())) {
      // Conversion from l-value to inout in an operator argument
      // position (which doesn't require explicit `&`) decays into
      // a `Bind` of involved object types, same goes for explicit
      // `&` conversion from l-value to inout type.
      //
      // In case of regular argument conversion although explicit `&`
      // is required we still want to diagnose the problem as one
      // about mutability instead of suggesting to add `&` which wouldn't
      // be correct.
      auto kind = (isExpr<InOutExpr>(anchor) ||
                   (rhs->is<InOutType>() &&
                    (matchKind == ConstraintKind::ArgumentConversion ||
                     matchKind == ConstraintKind::OperatorArgumentConversion)))
                      ? ConstraintKind::Bind
                      : matchKind;

      auto result = matchTypes(lhs, rhs->getWithoutSpecifierType(), kind,
                               TMF_ApplyingFix, locator);

      if (result.isSuccess()) {
        // If left side is a hole, let's not record a fix since hole can
        // assume any type and already represents a problem elsewhere in
        // the expression.
        if (lhs->isPlaceholder())
          return true;

        auto *loc = getConstraintLocator(locator);
        // If this `inout` is in incorrect position, it should be diagnosed
        // by other fixes.
        if (loc->directlyAt<InOutExpr>()) {
          if (!getArgumentLocator(castToExpr(anchor))) {
            conversionsOrFixes.push_back(
                RemoveAddressOf::create(*this, lhs, rhs, loc));
            return true;
          }
        }

        conversionsOrFixes.push_back(TreatRValueAsLValue::create(*this, loc));
        return true;
      }
    }

    return false;
  };

  // Check whether given `value` type matches a `RawValue` type of
  // a given raw representable type.
  auto isValueOfRawRepresentable = [&](Type valueType,
                                       Type rawReprType) -> bool {
    // diagnostic is going to suggest failable initializer anyway.
    if (auto objType = rawReprType->getOptionalObjectType())
      rawReprType = objType;

    // If value is optional diagnostic would suggest using `Optional.map` in
    // combination with `<Type>(rawValue: ...)` initializer.
    if (auto objType = valueType->getOptionalObjectType())
      valueType = objType;

    if (rawReprType->isTypeVariableOrMember())
      return false;

    auto rawValue = isRawRepresentable(*this, rawReprType);
    if (!rawValue)
      return false;

    auto result = matchTypes(valueType, rawValue, ConstraintKind::Conversion,
                             TMF_ApplyingFix, locator);
    return !result.isFailure();
  };

  // Check whether given `rawReprType` does indeed conform to `RawRepresentable`
  // and if so check that given `expectedType` matches its `RawValue` type. If
  // that condition holds add a tailored fix which is going to suggest to
  // explicitly construct a raw representable type from a given value type.
  auto repairByConstructingRawRepresentableType =
      [&](Type expectedType, Type rawReprType) -> bool {
    if (!isValueOfRawRepresentable(expectedType, rawReprType))
      return false;

    conversionsOrFixes.push_back(ExplicitlyConstructRawRepresentable::create(
        *this, rawReprType, expectedType, getConstraintLocator(locator)));
    return true;
  };

  // Check whether given `rawReprType` does indeed conform to `RawRepresentable`
  // and if so check that given `expectedType` matches its `RawValue` type. If
  // that condition holds add a tailored fix which is going to suggest to
  // use `.rawValue` associated with given raw representable type to match
  // given expected type.
  auto repairByUsingRawValueOfRawRepresentableType =
      [&](Type rawReprType, Type expectedType) -> bool {
    if (!isValueOfRawRepresentable(expectedType, rawReprType))
      return false;

    conversionsOrFixes.push_back(UseRawValue::create(
        *this, rawReprType, expectedType, getConstraintLocator(locator)));
    return true;
  };

  auto hasConversionOrRestriction = [&](ConversionRestrictionKind kind) {
    return llvm::any_of(conversionsOrFixes,
                        [kind](const RestrictionOrFix correction) {
                          if (auto restriction = correction.getRestriction())
                            return restriction == kind;
                          return false;
                        });
  };

  // Check whether this is a tuple with a single unlabeled element
  // i.e. `(_: Int)` and return type of that element if so. Note that
  // if the element is pack expansion type the tuple is significant.
  auto isSingleUnlabeledElementTuple = [](Type type) -> Type {
    if (auto *tuple = type->getAs<TupleType>()) {
      if (tuple->getNumElements() == 1 && !tuple->getElement(0).hasName()) {
        auto eltType = tuple->getElement(0).getType();
        return isPackExpansionType(eltType) ? Type() : eltType;
      }
    }
    return Type();
  };

  if (repairArrayLiteralUsedAsDictionary(*this, lhs, rhs, matchKind,
                                         conversionsOrFixes,
                                         getConstraintLocator(locator)))
    return true;

  if (path.empty()) {
    if (!anchor)
      return false;

    // This could be:
    // - `InOutExpr` used with r-value e.g. `foo(&x)` where `x` is a `let`.
    // - `ForceValueExpr` e.g. `foo.bar! = 42` where `bar` or `foo` are
    //   immutable or a subscript e.g. `foo["bar"]! = 42`.
    if (repairByTreatingRValueAsLValue(lhs, rhs))
      return true;

    // If method reference forms a value type of the key path,
    // there is going to be a constraint to match result of the
    // member lookup to the generic parameter `V` of *KeyPath<R, V>
    // type associated with key path expression, which we need to
    // fix-up here unless last component has already a invalid type or
    // instance fix recorded.
    if (auto *kpExpr = getAsExpr<KeyPathExpr>(anchor)) {
      auto i = kpExpr->getComponents().size() - 1;
      auto lastCompLoc = getConstraintLocator(
          locator.withPathElement(LocatorPathElt::KeyPathComponent(i)));
      if (hasFixFor(lastCompLoc, FixKind::AllowTypeOrInstanceMember))
        return true;

      auto lastComponentType = lhs->lookThroughAllOptionalTypes();
      auto keyPathResultType = rhs->lookThroughAllOptionalTypes();

      // Propagate contextual information from/to keypath result type.
      (void)matchTypes(lastComponentType, keyPathResultType, matchKind,
                       TMF_ApplyingFix, getConstraintLocator(locator));

      conversionsOrFixes.push_back(IgnoreContextualType::create(
          *this, lhs, rhs, getConstraintLocator(locator)));
      return true;
    }

    if (auto *ODRE = getAsExpr<OverloadedDeclRefExpr>(anchor)) {
      if (lhs->is<LValueType>()) {
        conversionsOrFixes.push_back(
            TreatRValueAsLValue::create(*this, getConstraintLocator(locator)));
        return true;
      }
    }

    if (auto *OEE = getAsExpr<OptionalEvaluationExpr>(anchor)) {
      // If concrete type of the sub-expression can't be converted to the
      // type associated with optional evaluation result it could only be
      // contextual mismatch where type of the top-level expression
      // comes from contextual type or its parent expression.
      //
      // Because result type of the optional evaluation is supposed to
      // represent the type of its sub-expression with added level of
      // optionality if needed.
      auto contextualTy = simplifyType(rhs)->getOptionalObjectType();
      if (!lhs->getOptionalObjectType() && !lhs->hasTypeVariable() &&
          contextualTy && !contextualTy->isTypeVariableOrMember()) {
        conversionsOrFixes.push_back(IgnoreContextualType::create(
            *this, lhs, rhs, getConstraintLocator(OEE->getSubExpr())));
        return true;
      }
    }

    if (auto *AE = getAsExpr<AssignExpr>(anchor)) {
      if (repairByInsertingExplicitCall(lhs, rhs))
        return true;

      if (auto *inoutExpr = dyn_cast<InOutExpr>(AE->getSrc())) {
        auto *loc = getConstraintLocator(inoutExpr);

        // Remove all of the restrictions because none of them
        // are going to succeed.
        conversionsOrFixes.erase(
            llvm::remove_if(
                conversionsOrFixes,
                [](const auto &entry) { return bool(entry.getRestriction()); }),
            conversionsOrFixes.end());

        if (hasFixFor(loc, FixKind::RemoveAddressOf))
          return true;

        conversionsOrFixes.push_back(
            RemoveAddressOf::create(*this, lhs, rhs, loc));
        return true;
      }

      if (repairByAnyToAnyObjectCast(lhs, rhs))
        return true;

      if (repairViaBridgingCast(*this, lhs, rhs, conversionsOrFixes, locator))
        return true;

      // If destination is `AnyObject` it means that source doesn't conform.
      if (rhs->getWithoutSpecifierType()
              ->lookThroughAllOptionalTypes()
              ->isAnyObject()) {
        conversionsOrFixes.push_back(IgnoreAssignmentDestinationType::create(
            *this, lhs, rhs, getConstraintLocator(locator)));
        return true;
      }

      // If we are trying to assign e.g. `Array<Int>` to `Array<Float>` let's
      // give solver a chance to determine which generic parameters are
      // mismatched and produce a fix for that.
      if (hasConversionOrRestriction(ConversionRestrictionKind::DeepEquality))
        return false;

      // An attempt to assign `Int?` to `String?`.
      if (hasConversionOrRestriction(
              ConversionRestrictionKind::OptionalToOptional)) {
        conversionsOrFixes.push_back(IgnoreAssignmentDestinationType::create(
            *this, lhs, rhs, getConstraintLocator(locator)));
        return true;
      }

      // If the situation has to do with protocol composition types and
      // destination doesn't have one of the conformances e.g. source is
      // `X & Y` but destination is only `Y` or vice versa, there is a
      // tailored "missing conformance" fix for that.
      if (hasConversionOrRestriction(ConversionRestrictionKind::Existential))
        return false;

      if (hasConversionOrRestriction(
              ConversionRestrictionKind::MetatypeToExistentialMetatype) ||
          hasConversionOrRestriction(
              ConversionRestrictionKind::ExistentialMetatypeToMetatype) ||
          hasConversionOrRestriction(ConversionRestrictionKind::Superclass)) {
        conversionsOrFixes.push_back(IgnoreAssignmentDestinationType::create(
            *this, lhs, rhs, getConstraintLocator(locator)));
        return true;
      }

      if (hasConversionOrRestriction(
              ConversionRestrictionKind::ValueToOptional)) {
        lhs = lhs->lookThroughAllOptionalTypes();
        rhs = rhs->lookThroughAllOptionalTypes();

        // If both object types are functions, let's allow the solver to
        // structurally compare them before trying to fix anything.
        if (lhs->is<FunctionType>() && rhs->is<FunctionType>())
          return false;

        // If either object type is a generic, nominal or existential type
        // it means that follow-up to value-to-optional is going to be:
        //
        // 1. "deep equality" check, which is handled by generic argument(s)
        //    or contextual mismatch fix, or
        // 2. "existential" check, which is handled by a missing conformance
        //    fix.
        if ((lhs->is<BoundGenericType>() && rhs->is<BoundGenericType>()) ||
            (lhs->is<NominalType>() && rhs->is<NominalType>()) ||
            rhs->isAnyExistentialType())
          return false;
      }

      auto *destExpr = AE->getDest();
      // Literal expression as well as call/operator application can't be
      // used as an assignment destination because resulting type is immutable.
      if (isa<ApplyExpr>(destExpr) || isa<LiteralExpr>(destExpr)) {
        conversionsOrFixes.push_back(
            TreatRValueAsLValue::create(*this, getConstraintLocator(locator)));
        return true;
      }

      // If destination has a function type, it might either be
      // a property with a function type or a method reference,
      // e.g. `foo.bar = 42` neither can be used if the destination
      // is not l-value.
      auto destType = getType(destExpr);
      auto destTypeVar = destType->getAs<TypeVariableType>();
      bool destIsOrCanBindToLValue =
          destType->is<LValueType>() ||
          (destTypeVar && destTypeVar->getImpl().canBindToLValue());
      if (!destIsOrCanBindToLValue && rhs->is<FunctionType>()) {
        conversionsOrFixes.push_back(
            TreatRValueAsLValue::create(*this, getConstraintLocator(locator)));
        return true;
      }

      if (repairViaOptionalUnwrap(*this, lhs, rhs, matchKind,
                                  conversionsOrFixes, locator))
        return true;

      // `rhs` - is an assignment destination and `lhs` is its source.
      if (repairByConstructingRawRepresentableType(lhs, rhs))
        return true;

      if (repairByUsingRawValueOfRawRepresentableType(lhs, rhs))
        return true;

      // Let's try to match source and destination types one more
      // time to see whether they line up, if they do - the problem is
      // related to immutability, otherwise it's a type mismatch.
      auto result = matchTypes(lhs, rhs, ConstraintKind::Conversion,
                               TMF_ApplyingFix, locator);
      
      auto *loc = getConstraintLocator(locator);
      if (destIsOrCanBindToLValue || result.isFailure()) {
        // Let this assignment failure be diagnosed by the
        // AllowTupleTypeMismatch fix already recorded.
        if (hasFixFor(loc, FixKind::AllowTupleTypeMismatch))
          return true;

        conversionsOrFixes.push_back(
            IgnoreAssignmentDestinationType::create(*this, lhs, rhs, loc));
      } else {
        conversionsOrFixes.push_back(TreatRValueAsLValue::create(*this, loc));
      }

      return true;
    }

    return false;
  }

  auto elt = path.back();
  switch (elt.getKind()) {
  case ConstraintLocator::LValueConversion: {
    // Ignore l-value conversion element since it has already
    // played its role.
    path.pop_back();
    // If this is a contextual mismatch between l-value types e.g.
    // `@lvalue String vs. @lvalue Int`, let's pretend that it's okay.
    if (!path.empty()) {
      if (path.back().is<LocatorPathElt::ContextualType>()) {
        auto *locator = getConstraintLocator(anchor, path.back());
        conversionsOrFixes.push_back(
            IgnoreContextualType::create(*this, lhs, rhs, locator));
        break;
      }

      // If this is a problem with result type of a subscript setter,
      // let's re-attempt to repair without l-value conversion in the
      // locator to fix underlying type mismatch.
      if (path.back().is<LocatorPathElt::FunctionResult>()) {
        return repairFailures(lhs, rhs, matchKind, flags, conversionsOrFixes,
                              getConstraintLocator(anchor, path));
      }

      // If this is a function type param type mismatch in any position,
      // the mismatch we want to report is for the whole structural type.
      auto last = std::find_if(
          path.rbegin(), path.rend(), [](LocatorPathElt &elt) -> bool {
            return elt.is<LocatorPathElt::FunctionArgument>();
          });

      if (last != path.rend())
        break;
    }

    LLVM_FALLTHROUGH;
  }

  case ConstraintLocator::ApplyArgToParam: {
    auto loc = getConstraintLocator(locator);

    // If this type mismatch is associated with a synthesized argument,
    // let's just ignore it because the main problem is the absence of
    // the argument.
    if (auto applyLoc = elt.getAs<LocatorPathElt::ApplyArgToParam>()) {
      if (auto *argumentList = getArgumentList(loc)) {
        // This is either synthesized argument or a default value.
        if (applyLoc->getArgIdx() >= argumentList->size()) {
          auto *calleeLoc = getCalleeLocator(loc);
          auto overload = findSelectedOverloadFor(calleeLoc);
          // If this cannot be a default value matching, let's ignore.
          if (!(overload && overload->choice.isDecl()))
            return true;

          // Ignore decls that don't have meaningful parameter lists - this
          // matches variables and parameters with function types.
          auto *paramList = getParameterList(overload->choice.getDecl());
          if (!paramList)
            return true;

          if (!paramList->get(applyLoc->getParamIdx())->getTypeOfDefaultExpr())
            return true;
        }
      }
    }

    // Don't attempt to fix an argument being passed to a
    // _OptionalNilComparisonType parameter. Such an overload should only take
    // effect when a nil literal is used in valid code, and doesn't offer any
    // useful fixes for invalid code.
    if (auto *nominal = rhs->getAnyNominal()) {
      if (nominal->isStdlibDecl() &&
          nominal->getName() == getASTContext().Id_OptionalNilComparisonType) {
        return false;
      }
    }

    if (isForCodeCompletion()) {
      // If the argument contains the code completion location, the user has not
      // finished typing out this argument yet. Treat the mismatch as valid so
      // we don't penalize this solution.
      if (auto *arg = getAsExpr(simplifyLocatorToAnchor(loc))) {
        // Ignore synthesized args like $match in implicit pattern match
        // operator calls. Their source location is usually the same as the
        // other (explicit) argument's so source range containment alone isn't
        // sufficient.
        bool isSynthesizedArg = arg->isImplicit() && isa<DeclRefExpr>(arg);
        if (!isSynthesizedArg && isForCodeCompletion() &&
            containsIDEInspectionTarget(arg) && !lhs->isVoid() &&
            !lhs->isUninhabited())
          return true;
      }
    }

    if (repairByInsertingExplicitCall(lhs, rhs))
      break;

    bool isPatternMatching = isArgumentOfPatternMatchingOperator(loc);
    // Let's not suggest force downcasts in pattern-matching context.
    if (!isPatternMatching &&
        repairViaBridgingCast(*this, lhs, rhs, conversionsOrFixes, locator))
      break;

    // Argument is a r-value but parameter expects an l-value e.g.
    //
    // func foo(_ x: inout Int) {}
    // let x: Int = 42
    // foo(x) // `x` can't be converted to `inout Int`.
    //
    // This has to happen before checking for optionality mismatch
    // because otherwise `Int? arg conv inout Int` is going to get
    // fixed as 2 fixes - "force unwrap" + r-value -> l-value mismatch.
    if (repairByTreatingRValueAsLValue(lhs, rhs))
      break;

    // If the problem is related to missing unwrap, there is a special
    // fix for that.
    if (lhs->getOptionalObjectType() && !rhs->getOptionalObjectType()) {
      // If this is an attempt to check whether optional conforms to a
      // particular protocol, let's do that before attempting to force
      // unwrap the optional.
      if (hasConversionOrRestriction(ConversionRestrictionKind::Existential))
        break;

      auto result = matchTypes(lhs->getOptionalObjectType(), rhs, matchKind,
                               TMF_ApplyingFix, locator);

      if (result.isSuccess()) {
        conversionsOrFixes.push_back(
            ForceOptional::create(*this, lhs, rhs, loc));
        break;
      }
    }

    // There is no subtyping between object types of inout argument/parameter.
    if (elt.getKind() == ConstraintLocator::LValueConversion) {
      auto result = matchTypes(lhs, rhs, ConstraintKind::Conversion,
                               TMF_ApplyingFix, locator);

      ConstraintFix *fix = nullptr;
      if (result.isFailure()) {
        // If this is a "destination" argument to a mutating operator
        // like `+=`, let's consider it contextual and only attempt
        // to fix type mismatch on the "source" right-hand side of
        // such operators.
        if (isOperatorArgument(loc) &&
            loc->findLast<LocatorPathElt::ApplyArgToParam>()->getArgIdx() == 0)
          break;

        fix = AllowArgumentMismatch::create(*this, lhs, rhs, loc);
      } else {
        fix = AllowInOutConversion::create(*this, lhs, rhs, loc);
      }

      conversionsOrFixes.push_back(fix);
      break;
    }

    if (elt.getKind() != ConstraintLocator::ApplyArgToParam)
      break;

    // If argument in l-value type and parameter is `inout` or a pointer,
    // let's see if it's generic parameter matches and suggest adding explicit
    // `&`.
    if (lhs->is<LValueType>() &&
        (rhs->is<InOutType>() || rhs->getAnyPointerElementType())) {
      auto baseType = rhs->is<InOutType>() ? rhs->getInOutObjectType()
                                           : rhs->getAnyPointerElementType();

      // Let's use `BindToPointer` constraint here to match up base types
      // of implied `inout` argument and `inout` or pointer parameter.
      // This helps us to avoid implicit conversions associated with
      // `ArgumentConversion` constraint.
      auto result = matchTypes(lhs->getRValueType(), baseType,
                               ConstraintKind::BindToPointerType,
                               TypeMatchFlags::TMF_ApplyingFix, locator);

      if (result.isSuccess()) {
        conversionsOrFixes.push_back(AddAddressOf::create(
            *this, lhs, rhs, getConstraintLocator(locator)));
        break;
      }
    }

    // If the argument is inout and the parameter is not inout or a pointer,
    // suggest removing the &.
    if (lhs->is<InOutType>() && !rhs->is<InOutType>()) {
      auto objectType = rhs->lookThroughAllOptionalTypes();
      if (!objectType->getAnyPointerElementType()) {
        auto result = matchTypes(lhs->getInOutObjectType(), rhs,
                                 ConstraintKind::ArgumentConversion,
                                 TypeMatchFlags::TMF_ApplyingFix, locator);

        if (result.isSuccess()) {
          conversionsOrFixes.push_back(RemoveAddressOf::create(
              *this, lhs, rhs, getConstraintLocator(locator)));
          break;
        }
      }
    }

    // If parameter type is `Any` the problem might be related to
    // invalid escapiness of the argument.
    if (rhs->isAny())
      break;

    // If there are any restrictions here we need to wait and let
    // `simplifyRestrictedConstraintImpl` handle them.
    if (llvm::any_of(conversionsOrFixes,
                     [](const RestrictionOrFix &correction) {
                       return bool(correction.getRestriction());
                     }))
      break;

    if (auto *fix = fixPropertyWrapperFailure(
            *this, lhs, loc,
            [&](SelectedOverload overload, VarDecl *decl, Type newBase) {
              // FIXME: There is currently no easy way to avoid attempting
              // fixes, matchTypes do not propagate `TMF_ApplyingFix` flag.
              llvm::SaveAndRestore<ConstraintSystemOptions> options(
                  Options, Options - ConstraintSystemFlags::AllowFixes);

              TypeMatchOptions flags;
              return matchTypes(newBase, rhs, ConstraintKind::Subtype, flags,
                                getConstraintLocator(locator))
                  .isSuccess();
            },
            rhs)) {
      conversionsOrFixes.push_back(fix);
      break;
    }

    // If this is an implicit 'something-to-pointer' conversion
    // it's going to be diagnosed by specialized fix which deals
    // with generic argument mismatches.
    if (matchKind == ConstraintKind::BindToPointerType) {
      if (!rhs->isPlaceholder())
        break;
    }

    // If this is a ~= operator implicitly generated by pattern matching
    // let's not try to fix right-hand side of the operator because it's
    // a correct contextual type.
    if (isPatternMatching &&
        elt.castTo<LocatorPathElt::ApplyArgToParam>().getParamIdx() == 1)
      break;

    if (auto *fix = ExpandArrayIntoVarargs::attempt(*this, lhs, rhs, locator)) {
      conversionsOrFixes.push_back(fix);
      break;
    }

    // If parameter is a collection but argument is not, let's try
    // to try and match collection element type to the argument to
    // produce better diagnostics e.g.:
    //
    // ```
    // func foo<T>(_: [T]) {}
    // foo(1) // expected '[Int]', got 'Int'
    // ```
    if (rhs->isKnownStdlibCollectionType()) {
      std::function<Type(Type)> getArrayOrSetType = [&](Type type) -> Type {
        if (auto eltTy = type->isArrayType())
          return getArrayOrSetType(eltTy);

        if (auto eltTy = isSetType(type))
          return getArrayOrSetType(*eltTy);

        return type;
      };

      // Let's ignore any optional types associated with element e.g. `[T?]`
      auto rhsEltTy = getArrayOrSetType(rhs)->lookThroughAllOptionalTypes();
      (void)matchTypes(lhs, rhsEltTy, ConstraintKind::Equal, TMF_ApplyingFix,
                       locator);
    }

    // If either type has a placeholder, consider this fixed.
    if (lhs->hasPlaceholder() || rhs->hasPlaceholder())
      return true;

    // `lhs` - is an argument and `rhs` is a parameter type.
    if (repairByConstructingRawRepresentableType(lhs, rhs))
      break;

    if (repairByUsingRawValueOfRawRepresentableType(lhs, rhs))
      break;

    if (repairViaOptionalUnwrap(*this, lhs, rhs, matchKind, conversionsOrFixes,
                                locator))
      break;

    {
      auto *calleeLocator = getCalleeLocator(loc);
      if (hasFixFor(calleeLocator, FixKind::AddQualifierToAccessTopLevelName)) {
        if (auto overload = findSelectedOverloadFor(calleeLocator)) {
          if (auto choice = overload->choice.getDeclOrNull()) {
            // If this is an argument of a symmetric function/operator let's
            // not fix any position rather than first because we'd just end
            // up with ambiguity instead of reporting an actual problem with
            // mismatched type since each argument can have district bindings.
            if (auto *AFD = dyn_cast<AbstractFunctionDecl>(choice)) {
              auto *paramList = AFD->getParameters();
              auto firstParamType = paramList->get(0)->getInterfaceType();
              if (elt.castTo<LocatorPathElt::ApplyArgToParam>().getParamIdx() >
                      0 &&
                  llvm::all_of(*paramList, [&](const ParamDecl *param) -> bool {
                    return param->getInterfaceType()->isEqual(firstParamType);
                  }))
                return true;
            }
          }
        }
      }
    }

    if (repairOutOfOrderArgumentsInBinaryFunction(*this, conversionsOrFixes,
                                                  loc))
      return true;

    // There is already a remove extraneous arguments fix recorded for this
    // apply arg to param locator, so let's skip the default argument mismatch.
    if (hasFixFor(loc, FixKind::RemoveExtraneousArguments))
      return true;

    // If parameter is a pack, let's see if we have already recorded
    // either synthesized or extraneous argument fixes.
    if (rhs->is<PackType>()) {
      ArrayRef tmpPath(path);

      // Ignore argument/parameter type conversion mismatch if we already
      // detected a tuple splat issue.
      if (hasFixFor(loc,
                    FixKind::DestructureTupleToMatchPackExpansionParameter))
        return true;

      // Path would end with `ApplyArgument`.
      auto *argsLoc = getConstraintLocator(anchor, tmpPath.drop_back());
      if (hasFixFor(argsLoc, FixKind::RemoveExtraneousArguments) ||
          hasFixFor(argsLoc, FixKind::AddMissingArguments))
        return true;
    }

    // If the argument couldn't be found, this could be a default value
    // type mismatch.
    if (!simplifyLocatorToAnchor(loc)) {
      auto *calleeLocator = getCalleeLocator(loc);
      unsigned paramIdx =
          loc->castLastElementTo<LocatorPathElt::ApplyArgToParam>()
              .getParamIdx();

      if (auto overload = findSelectedOverloadFor(calleeLocator)) {
        if (auto *decl = overload->choice.getDeclOrNull()) {
          if (auto paramList = getParameterList(decl)) {
            if (paramList->get(paramIdx)->getTypeOfDefaultExpr()) {
              conversionsOrFixes.push_back(
                  IgnoreDefaultExprTypeMismatch::create(*this, lhs, rhs, loc));
              break;
            }
          }
        }
      }
    }

    conversionsOrFixes.push_back(
        AllowArgumentMismatch::create(*this, lhs, rhs, loc));
    break;
  }

  case ConstraintLocator::KeyPathRoot: {
    // The root mismatch is from base U? to U or a subtype of U in keypath 
    // application so let's suggest an unwrap the optional fix.
    if (auto unwrapFix = UnwrapOptionalBaseKeyPathApplication::attempt(
            *this, lhs, rhs, getConstraintLocator(locator))) {
      conversionsOrFixes.push_back(unwrapFix);
      break;
    }

    conversionsOrFixes.push_back(AllowKeyPathRootTypeMismatch::create(
        *this, lhs, rhs, getConstraintLocator(locator)));

    break;
  }

  case ConstraintLocator::WrappedValue: {
    conversionsOrFixes.push_back(AllowWrappedValueMismatch::create(
            *this, lhs, rhs, getConstraintLocator(locator)));
    break;
  }

  case ConstraintLocator::FunctionArgument: {
    auto *argLoc = getConstraintLocator(
        locator.withPathElement(LocatorPathElt::SynthesizedArgument(0)));

    // Let's drop the last element which points to a single argument
    // and see if this is a contextual mismatch.
    path.pop_back();
    if (path.empty() ||
        !(path.back().getKind() == ConstraintLocator::ApplyArgToParam ||
          path.back().getKind() == ConstraintLocator::ContextualType))
      return false;

    auto arg = llvm::find_if(getTypeVariables(),
                             [&argLoc](const TypeVariableType *typeVar) {
                               return typeVar->getImpl().getLocator() == argLoc;
                             });

    // What we have here is a form or tuple splat with no arguments
    // demonstrated by following example:
    //
    // func foo<T: P>(_: T, _: (T.Element) -> Int) {}
    // foo { 42 }
    //
    // In cases like this `T.Element` might be resolved to `Void`
    // which means that we have to try a single empty tuple argument
    // as a narrow exception to SE-0110, see `matchFunctionTypes`.
    //
    // But if `T.Element` didn't get resolved to `Void` we'd like
    // to diagnose this as a missing argument which can't be ignored or
    // a tuple is trying to be inferred as a tuple for destructuring but
    // contextual argument does not match(in this case we remove the extra
    // closure arguments).
    if (arg != getTypeVariables().end()) {
      if (auto argToParamElt =
              path.back().getAs<LocatorPathElt::ApplyArgToParam>()) {
        auto loc = getConstraintLocator(anchor, path);
        auto closureAnchor =
            getAsExpr<ClosureExpr>(simplifyLocatorToAnchor(loc));
        if (rhs->is<TupleType>() && closureAnchor &&
            closureAnchor->getParameters()->size() > 1) {
          auto callee = getCalleeLocator(loc);
          if (auto overload = findSelectedOverloadFor(callee)) {
            auto fnType =
                simplifyType(overload->adjustedOpenedType)->castTo<FunctionType>();
            auto paramIdx = argToParamElt->getParamIdx();
            auto paramType = fnType->getParams()[paramIdx].getParameterType();
            if (auto paramFnType = paramType->getAs<FunctionType>()) {
              conversionsOrFixes.push_back(RemoveExtraneousArguments::create(
                  *this, paramFnType, {}, loc));
              break;
            }
          }
        }
      }

      conversionsOrFixes.push_back(AddMissingArguments::create(
          *this, {SynthesizedArg{0, AnyFunctionType::Param(*arg)}},
          getConstraintLocator(anchor, path)));
      break;
    }

    auto *parentLoc = getConstraintLocator(anchor, path);

    if (lhs->is<InOutType>() != rhs->is<InOutType>()) {
      // Since `FunctionArgument` as a last locator element represents
      // a single parameter of the function type involved in a conversion
      // to another function type, see `matchFunctionTypes`. If there is already
      // a fix for the this conversion, we can just ignore individual function
      // argument in-out mismatch failure by considered this fixed.
      if (hasFixFor(parentLoc))
        return true;

      // We want to call matchTypes with the default decomposition options
      // in case there are type variables that we couldn't bind due to the
      // inout attribute mismatch.
      auto result = matchTypes(lhs->getInOutObjectType(),
                               rhs->getInOutObjectType(), matchKind,
                               getDefaultDecompositionOptions(TMF_ApplyingFix),
                               locator);

      if (result.isSuccess()) {
        conversionsOrFixes.push_back(AllowInOutConversion::create(*this, lhs,
            rhs, getConstraintLocator(locator)));
        break;
      }
    }

    // In cases like this `FunctionArgument` as a last locator element
    // represents a single parameter of the function type involved in
    // a conversion to another function type, see `matchFunctionTypes`.
    if (parentLoc->isForContextualType() ||
        parentLoc->isLastElement<LocatorPathElt::ApplyArgToParam>()) {
      // If either type has a placeholder, consider this fixed.
      if (lhs->hasPlaceholder() || rhs->hasPlaceholder())
        return true;

      // If there is a fix associated with contextual conversion or
      // a function type itself, let's ignore argument failure but
      // increase a score.
      if (hasFixFor(parentLoc)) {
        increaseScore(SK_Fix);
        return true;
      }

      // Since there is only one parameter let's give it a chance to diagnose
      // a more specific error in some situations.
      if (hasConversionOrRestriction(ConversionRestrictionKind::DeepEquality) ||
          hasConversionOrRestriction(ConversionRestrictionKind::Existential) ||
          hasConversionOrRestriction(ConversionRestrictionKind::Superclass))
        break;

      conversionsOrFixes.push_back(AllowFunctionTypeMismatch::create(
          *this, lhs, rhs, parentLoc, /*index=*/0));
      break;
    }

    break;
  }

  case ConstraintLocator::TypeParameterRequirement:
  case ConstraintLocator::ConditionalRequirement: {
    // If either type has a placeholder, consider this fixed.
    if (lhs->hasPlaceholder() || rhs->hasPlaceholder())
      return true;

    // If requirement is something like `T == [Int]` let's let
    // type matcher a chance to match generic parameters before
    // recording a fix, because then we'll know exactly how many
    // generic parameters did not match.
    if (hasConversionOrRestriction(ConversionRestrictionKind::DeepEquality))
      break;

    auto *reqLoc = getConstraintLocator(locator);

    if (isFixedRequirement(reqLoc, rhs))
      return true;

    // If this is a requirement on sequence of for-in statement where one
    // of the sides is a completely resolved dependent member, skip it
    // since the issue is with the conformance to `Sequence`, otherwise
    // dependent member would have been substituted.
    if (auto *UDE = getAsExpr<UnresolvedDotExpr>(anchor)) {
      if (UDE->isImplicit() &&
          getContextualTypePurpose(UDE->getBase()) == CTP_ForEachSequence) {
        if ((lhs->is<DependentMemberType>() && !lhs->hasTypeVariable()) ||
            (rhs->is<DependentMemberType>() && !rhs->hasTypeVariable()))
          return true;
      }
    }

    if (auto *fix = fixRequirementFailure(*this, lhs, rhs, anchor, path)) {
      recordFixedRequirement(reqLoc, rhs);
      conversionsOrFixes.push_back(fix);
    }
    break;
  }

  case ConstraintLocator::ClosureBody:
  case ConstraintLocator::ClosureResult: {
    if (repairByInsertingExplicitCall(lhs, rhs))
      break;

    if (repairViaOptionalUnwrap(*this, lhs, rhs, matchKind, conversionsOrFixes,
                                locator))
      return true;

    // If we could record a generic arguments mismatch instead of this fix,
    // don't record a contextual type mismatch here.
    if (hasConversionOrRestriction(ConversionRestrictionKind::DeepEquality))
      break;

    auto *fix = IgnoreContextualType::create(*this, lhs, rhs,
                                             getConstraintLocator(locator));
    conversionsOrFixes.push_back(fix);
    break;
  }

  case ConstraintLocator::ContextualType: {
    // If either type is a placeholder, consider this fixed
    if (lhs->isPlaceholder() || rhs->isPlaceholder())
      return true;

    // If either side is not yet resolved, it's too early for this fix.
    if (lhs->isTypeVariableOrMember() || rhs->isTypeVariableOrMember())
      break;

    // If there is already a fix for contextual failure, let's not
    // record a duplicate one.
    if (hasFixFor(getConstraintLocator(locator)))
      return true;

    auto purpose = getContextualTypePurpose(anchor);
    if (rhs->isVoid() &&
        (purpose == CTP_ReturnStmt || purpose == CTP_ReturnSingleExpr)) {
      conversionsOrFixes.push_back(
          RemoveReturn::create(*this, lhs, getConstraintLocator(locator)));
      return true;
    }

    if (repairByInsertingExplicitCall(lhs, rhs))
      break;

    if (repairByAnyToAnyObjectCast(lhs, rhs))
      break;

    if (repairViaBridgingCast(*this, lhs, rhs, conversionsOrFixes, locator))
      break;

    // If both types are key path, the only differences
    // between them are mutability and/or root, value type mismatch.
    if (isKnownKeyPathType(lhs) && isKnownKeyPathType(rhs)) {
      auto *fix = KeyPathContextualMismatch::create(
          *this, lhs, rhs, getConstraintLocator(locator));
      conversionsOrFixes.push_back(fix);
    }

    if (lhs->is<FunctionType>() && !rhs->is<AnyFunctionType>() &&
        isExpr<ClosureExpr>(anchor)) {
      auto *fix = ContextualMismatch::create(*this, lhs, rhs,
                                             getConstraintLocator(locator));
      conversionsOrFixes.push_back(fix);
    }

    // Solver can unwrap contextual type in an unlabeled one-element tuple
    // while matching type to a tuple that contains one or more pack expansion
    // types (because such tuples can loose their elements under substitution),
    // if that's the case, let's just produce a regular contextual mismatch fix.
    if (auto contextualType = isSingleUnlabeledElementTuple(rhs)) {
      rhs = contextualType;
    }

    if (purpose == CTP_Initialization && lhs->is<TupleType>() &&
        rhs->is<TupleType>()) {
      auto *fix = AllowTupleTypeMismatch::create(*this, lhs, rhs,
                                                 getConstraintLocator(locator));
      conversionsOrFixes.push_back(fix);
      break;
    }

    if (repairViaOptionalUnwrap(*this, lhs, rhs, matchKind, conversionsOrFixes,
                                locator))
      return true;

    // Let's wait until both sides are of the same optionality before
    // attempting `.rawValue` fix.
    if (hasConversionOrRestriction(ConversionRestrictionKind::ValueToOptional))
      break;

    if (repairByUsingRawValueOfRawRepresentableType(lhs, rhs))
      break;

    // If there are any restrictions here we need to wait and let
    // `simplifyRestrictedConstraintImpl` handle them.
    if (llvm::any_of(conversionsOrFixes,
                     [](const RestrictionOrFix &correction) {
                       return bool(correction.getRestriction());
                     }))
      break;

    // `lhs` - is an result type and `rhs` is a contextual type.
    if (repairByConstructingRawRepresentableType(lhs, rhs))
      break;

    conversionsOrFixes.push_back(IgnoreContextualType::create(
        *this, lhs, rhs, getConstraintLocator(locator)));
    break;
  }

  case ConstraintLocator::FunctionResult: {
    if (lhs->isPlaceholder() || rhs->isPlaceholder()) {
      recordAnyTypeVarAsPotentialHole(lhs);
      recordAnyTypeVarAsPotentialHole(rhs);
      return true;
    }

    auto *loc = getConstraintLocator(anchor, {path.begin(), path.end() - 1});
    // If this is a mismatch between contextual type and (trailing)
    // closure with explicitly specified result type let's record it
    // as contextual type mismatch.
    if (loc->isLastElement<LocatorPathElt::ContextualType>() ||
        loc->isLastElement<LocatorPathElt::ApplyArgToParam>()) {
      auto argument = simplifyLocatorToAnchor(loc);
      if (isExpr<ClosureExpr>(argument)) {
        auto *locator =
            getConstraintLocator(argument, ConstraintLocator::ClosureResult);

        if (repairViaOptionalUnwrap(*this, lhs, rhs, matchKind,
                                    conversionsOrFixes, locator))
          return true;

        conversionsOrFixes.push_back(
            IgnoreContextualType::create(*this, lhs, rhs, locator));
        break;
      }
    }

    // Handle function result coerce expression wrong type conversion.
    if (isExpr<CoerceExpr>(anchor)) {
      auto *fix =
          ContextualMismatch::create(*this, lhs, rhs, loc);
      conversionsOrFixes.push_back(fix);
      break;
    }
    LLVM_FALLTHROUGH;
  }

  case ConstraintLocator::Member:
  case ConstraintLocator::DynamicLookupResult: {
    // Most likely this is an attempt to use get-only subscript as mutating,
    // or assign a value of a result of function/member ref e.g. `foo() = 42`
    // or `foo.bar = 42`, or `foo.bar()! = 42`.
    if (repairByTreatingRValueAsLValue(rhs, lhs))
      break;

    // `apply argument` -> `arg/param compare` ->
    // `@autoclosure result` -> `function result`
    if (path.size() > 3) {
      const auto &elt = path[path.size() - 2];
      if (elt.getKind() == ConstraintLocator::AutoclosureResult &&
          repairByInsertingExplicitCall(lhs, rhs))
        return true;
    }
    break;
  }

  case ConstraintLocator::AutoclosureResult: {
    if (repairByInsertingExplicitCall(lhs, rhs))
      return true;

    auto isPointerType = [](Type type) -> bool {
      return bool(
          type->lookThroughAllOptionalTypes()->getAnyPointerElementType());
    };

    // Let's see whether this is an implicit conversion to a pointer type
    // which is invalid in @autoclosure context e.g. from `inout`, Array
    // or String.
    if (!isPointerType(lhs) && isPointerType(rhs)) {
      auto result = matchTypes(
          lhs, rhs, ConstraintKind::ArgumentConversion,
          TypeMatchFlags::TMF_ApplyingFix,
          locator.withPathElement(ConstraintLocator::FunctionArgument));

      if (result.isSuccess())
        conversionsOrFixes.push_back(AllowAutoClosurePointerConversion::create(
            *this, lhs, rhs, getConstraintLocator(locator)));
    }

    // In situations like this:
    //
    // struct S<T> {}
    // func foo(_: @autoclosure () -> S<Int>) {}
    // foo(S<String>())
    //
    // Generic type conversion mismatch is a better fix which is going to
    // point to the generic arguments that did not align properly.
    if (hasConversionOrRestriction(ConversionRestrictionKind::DeepEquality))
      break;

    conversionsOrFixes.push_back(AllowArgumentMismatch::create(
        *this, lhs, rhs, getConstraintLocator(locator)));
    break;
  }

  case ConstraintLocator::TupleElement: {
    if (lhs->isPlaceholder() || rhs->isPlaceholder()) {
      recordAnyTypeVarAsPotentialHole(lhs);
      recordAnyTypeVarAsPotentialHole(rhs);
      return true;
    }

    if (isExpr<ArrayExpr>(anchor) || isExpr<DictionaryExpr>(anchor)) {
      // If we could record a generic arguments mismatch instead of this fix,
      // don't record a ContextualMismatch here.
      if (hasConversionOrRestriction(ConversionRestrictionKind::DeepEquality))
        break;

      // We already have a fix for trying to initialize/assign an array literal
      // to a dictionary type. In this case elements mismatch only add extra
      // verbosity to the diagnostic. So let's skip the fix and only increase
      // the score to focus on suggesting using dictionary literal instead.
      path.pop_back();
      auto loc = getConstraintLocator(anchor, path);
      if (hasFixFor(loc, FixKind::TreatArrayLiteralAsDictionary)) {
        increaseScore(SK_Fix);
        return true;
      }

      conversionsOrFixes.push_back(CollectionElementContextualMismatch::create(
          *this, lhs, rhs, getConstraintLocator(locator)));
      break;
    }

    // Drop the `tuple element` locator element so that all tuple element
    // mismatches within the same tuple type can be coalesced later.
    auto index = elt.getAs<LocatorPathElt::TupleElement>()->getIndex();
    path.pop_back();

    // Drop the tuple type path elements too, but extract each tuple type first.
    if (!path.empty() && path.back().is<LocatorPathElt::TupleType>()) {
      rhs = path.back().getAs<LocatorPathElt::TupleType>()->getType();
      path.pop_back();
      lhs = path.back().getAs<LocatorPathElt::TupleType>()->getType();
      path.pop_back();
    }

    auto *tupleLocator = getConstraintLocator(locator.getAnchor(), path);

    // Let this fail if it's a contextual mismatch with sequence element types,
    // as there's a special fix for that.
    if (tupleLocator->isLastElement<LocatorPathElt::SequenceElementType>())
      break;

    // Generic argument/requirement failures have a more general fix which
    // is attached to a parent type and aggregates all argument failures
    // into a single fix.
    if (tupleLocator->isLastElement<LocatorPathElt::AnyRequirement>() ||
        tupleLocator->isLastElement<LocatorPathElt::GenericArgument>())
      break;

    // If the mismatch is a part of either optional-to-optional or
    // value-to-optional conversions, let's allow fix refer to a complete
    // top level type and not just a part of it.
    if (tupleLocator->findLast<LocatorPathElt::OptionalPayload>())
      break;

    if (tupleLocator->isForContextualType()) {
      if (auto contextualTy = isSingleUnlabeledElementTuple(rhs)) {
        return repairFailures(lhs, contextualTy, matchKind, flags,
                              conversionsOrFixes, tupleLocator);
      }
    }

    ConstraintFix *fix;
    if (tupleLocator->isLastElement<LocatorPathElt::FunctionArgument>()) {
      fix = AllowFunctionTypeMismatch::create(*this, lhs, rhs, tupleLocator, index);
    } else {
      fix = AllowTupleTypeMismatch::create(*this, lhs, rhs, tupleLocator, index);
    }
    conversionsOrFixes.push_back(fix);
    break;
  }

  case ConstraintLocator::PackElement: {
    path.pop_back();

    if (!path.empty() && path.back().is<LocatorPathElt::PackType>())
      path.pop_back();

    if (!path.empty() && path.back().is<LocatorPathElt::PackType>())
      path.pop_back();

    return repairFailures(lhs, rhs, matchKind, flags, conversionsOrFixes,
                          getConstraintLocator(anchor, path));
  }

  case ConstraintLocator::PackShape: {
    auto *shapeLocator = getConstraintLocator(locator);

    // FIXME: If the anchor isn't a pack expansion, this shape requirement
    // came from a same-shape generic requirement, which will fail separately
    // with an applied requirement fix. Currently, pack shapes can themselves be
    // pack types with pack expansions, so matching shape types can recursively
    // add ShapeOf constraints. For now, skip fixing the nested ones to avoid
    // cascading diagnostics.
    if (!isExpr<PackExpansionExpr>(shapeLocator->getAnchor()))
      return true;

    auto *fix = SkipSameShapeRequirement::create(*this, lhs, rhs, shapeLocator);
    conversionsOrFixes.push_back(fix);
    break;
  }

  case ConstraintLocator::SequenceElementType: {
    if (lhs->isPlaceholder() || rhs->isPlaceholder()) {
      recordAnyTypeVarAsPotentialHole(lhs);
      recordAnyTypeVarAsPotentialHole(rhs);
      return true;
    }

    // This is going to be diagnosed as `missing conformance`,
    // so no need to create duplicate fixes.
    if (rhs->isExistentialType())
      break;

    // If the types didn't line up, let's allow right-hand side
    // of the conversion (or pattern match) to have holes. This
    // helps when conversion if between a type and a tuple e.g.
    // `Int` vs. `(_, _)`.
    recordAnyTypeVarAsPotentialHole(rhs);

    conversionsOrFixes.push_back(CollectionElementContextualMismatch::create(
        *this, lhs, rhs, getConstraintLocator(locator)));
    break;
  }

  case ConstraintLocator::SubscriptMember: {
    if (repairByTreatingRValueAsLValue(lhs, rhs))
      break;

    break;
  }

  case ConstraintLocator::Condition: {
    if (repairViaOptionalUnwrap(*this, lhs, rhs, matchKind, conversionsOrFixes,
                                locator))
      return true;

    conversionsOrFixes.push_back(IgnoreContextualType::create(
        *this, lhs, rhs, getConstraintLocator(locator)));
    break;
  }

  case ConstraintLocator::UnresolvedMemberChainResult: {
    // Ignore this mismatch if base or result is already a hole.
    if (lhs->isPlaceholder() || rhs->isPlaceholder())
      return true;

    if (repairViaOptionalUnwrap(*this, lhs, rhs, matchKind, conversionsOrFixes,
                                locator))
      return true;

    if (repairByTreatingRValueAsLValue(lhs, rhs))
      break;

    // If there is a type mismatch here it's contextual e.g.,
    // `let x: E = .foo(42)`, where `.foo` is a member of `E`
    // but produces an incorrect type.
    auto *fix = IgnoreContextualType::create(*this, lhs, rhs,
                                             getConstraintLocator(locator));
    conversionsOrFixes.push_back(fix);
    break;
  }

  case ConstraintLocator::ImplicitlyUnwrappedDisjunctionChoice: {
    // If this is an attempt to use readonly IUO as a destination
    // of an assignment e.g.
    //
    // let x: Int! = 0
    // x = 42 <- `x` can be either `Int?` or `Int` but it can't be an l-value.
    if (lhs->is<LValueType>() && !rhs->is<LValueType>()) {
      auto result = matchTypes(lhs->getWithoutSpecifierType(), rhs, matchKind,
                               TMF_ApplyingFix, locator);

      if (result.isSuccess()) {
        conversionsOrFixes.push_back(
            TreatRValueAsLValue::create(*this, getConstraintLocator(locator)));
      }
    }
    break;
  }

  case ConstraintLocator::InstanceType: {
    if (lhs->hasPlaceholder() || rhs->hasPlaceholder())
      return true;

    break;
  }

  case ConstraintLocator::OptionalPayload: {
    if (repairViaOptionalUnwrap(*this, lhs, rhs, matchKind, conversionsOrFixes,
                                locator))
      return true;

    break;
  }

  case ConstraintLocator::TernaryBranch:
  case ConstraintLocator::SingleValueStmtBranch: {
    recordAnyTypeVarAsPotentialHole(lhs);
    recordAnyTypeVarAsPotentialHole(rhs);

    if (lhs->hasPlaceholder() || rhs->hasPlaceholder())
      return true;

    // If there's a contextual type, let's consider it the source of truth and
    // produce a contextual mismatch instead of  per-branch failure, because
    // it's a better pointer than potential then-to-else type mismatch.
    if (auto contextualType =
            getContextualType(anchor, /*forConstraint=*/false)) {
      auto purpose = getContextualTypePurpose(anchor);
      if (contextualType->isEqual(rhs)) {
        auto *loc = getConstraintLocator(
            anchor, LocatorPathElt::ContextualType(purpose));
        if (hasFixFor(loc, FixKind::IgnoreContextualType))
          return true;

        if (contextualType->isVoid() && purpose == CTP_ReturnStmt) {
          conversionsOrFixes.push_back(RemoveReturn::create(*this, lhs, loc));
          break;
        }

        conversionsOrFixes.push_back(
            IgnoreContextualType::create(*this, lhs, rhs, loc));
        break;
      }
    }

    // If there is no contextual type, this is most likely a contextual type
    // mismatch between the branches.
    conversionsOrFixes.push_back(ContextualMismatch::create(
        *this, lhs, rhs, getConstraintLocator(locator)));
    break;
  }

  case ConstraintLocator::PatternMatch: {
    auto *pattern = elt.castTo<LocatorPathElt::PatternMatch>().getPattern();
    bool isMemberMatch =
        lhs->is<FunctionType>() && isa<EnumElementPattern>(pattern);

    // If member reference couldn't be resolved, let's allow pattern
    // to have holes.
    if (rhs->isPlaceholder() && isMemberMatch) {
      recordAnyTypeVarAsPotentialHole(lhs);
      return true;
    }

    // If either type is a placeholder, consider this fixed.
    if (lhs->isPlaceholder() || rhs->isPlaceholder())
      return true;

    // If member reference didn't match expected pattern,
    // let's consider that a contextual mismatch.
    if (isMemberMatch) {
      recordAnyTypeVarAsPotentialHole(lhs);
      recordAnyTypeVarAsPotentialHole(rhs);
      conversionsOrFixes.push_back(AllowAssociatedValueMismatch::create(
          *this, lhs, rhs, getConstraintLocator(locator)));
    }

    // `weak` declaration with an explicit non-optional type e.g.
    // `weak x: X = ...` where `X` is a class.
    if (auto *TP = dyn_cast<TypedPattern>(pattern)) {
      if (auto *NP = dyn_cast<NamedPattern>(TP->getSubPattern())) {
        auto *var = NP->getDecl();

        auto ROK = ReferenceOwnership::Strong;
        if (auto *OA = var->getAttrs().getAttribute<ReferenceOwnershipAttr>())
          ROK = OA->get();

        if (!rhs->getOptionalObjectType() &&
            optionalityOf(ROK) == ReferenceOwnershipOptionality::Required) {
          conversionsOrFixes.push_back(
              AllowNonOptionalWeak::create(*this, getConstraintLocator(NP)));
          break;
        }
      }
    }

    break;
  }

  case ConstraintLocator::GenericArgument: {
    // If any of the types is a placeholder, consider it fixed.
    if (lhs->isPlaceholder() || rhs->isPlaceholder())
      return true;

    // Ignoring the generic argument because we may have a generic requirement
    // failure e.g. `String bind T.Element`, so let's drop the generic argument
    // path element and recurse in repairFailures to check and potentially
    // record the requirement failure fix.
    auto genericArgElt =
        path.pop_back_val().castTo<LocatorPathElt::GenericArgument>();

    // If we have something like ... -> type req # -> pack element #, we're
    // solving a requirement of the form T : P where T is a type parameter pack
    if (!path.empty() && path.back().is<LocatorPathElt::PackElement>())
      path.pop_back();

    if (!path.empty() && path.back().is<LocatorPathElt::AnyRequirement>()) {
      return repairFailures(lhs, rhs, matchKind, flags, conversionsOrFixes,
                            getConstraintLocator(anchor, path));
    }

    // When the solver sets `TMF_MatchingGenericArguments` it means
    // that it's matching generic argument pairs to identify any mismatches
    // as part of larger matching of two generic types. Letting this
    // fail results in a single fix that aggregates all mismatch locations.
    //
    // Types are not always resolved enough to enable that which means
    // that the comparison should be delayed, which brings us here - a
    // standalone constraint that represents such a match, in such cases
    // we create a fix per mismatch location and coalesce them during
    // diagnostics.
    if (flags.contains(TMF_MatchingGenericArguments))
      break;

    Type fromType;
    Type toType;

    if (path.size() >= 2) {
      if (path[path.size() - 2].is<LocatorPathElt::GenericType>()) {
        fromType = path[path.size() - 2]
                       .castTo<LocatorPathElt::GenericType>()
                       .getType();
      }

      if (path[path.size() - 1].is<LocatorPathElt::GenericType>()) {
        toType = path[path.size() - 1]
                     .castTo<LocatorPathElt::GenericType>()
                     .getType();
      }
    }

    if (!fromType || !toType)
      break;

    // Drop both `GenericType` elements.
    path.pop_back();
    path.pop_back();

    ConstraintFix *fix = nullptr;
    if (!path.empty() && path.back().is<LocatorPathElt::AnyRequirement>()) {
      fix = fixRequirementFailure(*this, fromType, toType, anchor, path);
    } else {
      fix = GenericArgumentsMismatch::create(
          *this, fromType, toType, {genericArgElt.getIndex()},
          getConstraintLocator(anchor, path));
    }

    if (!fix)
      break;

    conversionsOrFixes.push_back(fix);
    return true;
  }

  case ConstraintLocator::ResultBuilderBodyResult: {
    // If result type of the body couldn't be determined
    // there is going to be other fix available to diagnose
    // the underlying issue.
    if (lhs->isPlaceholder())
      return true;

    conversionsOrFixes.push_back(ContextualMismatch::create(
        *this, lhs, rhs, getConstraintLocator(locator)));
    break;
  }
  case ConstraintLocator::GlobalActorType: {
    // Drop global actor element as it servers only to indentify the global
    // actor matching.
    path.pop_back();

    conversionsOrFixes.push_back(AllowGlobalActorMismatch::create(
        *this, lhs, rhs, getConstraintLocator(anchor, path)));
    break;
  }

  case ConstraintLocator::CoercionOperand: {
    auto *coercion = castToExpr<CoerceExpr>(anchor);

    // Coercion from T.Type to T.Protocol.
    if (hasConversionOrRestriction(
            ConversionRestrictionKind::MetatypeToExistentialMetatype))
      return false;

    if (hasConversionOrRestriction(ConversionRestrictionKind::Superclass))
      return false;

    // Let's check whether the sub-expression is an optional type which
    // is possible to unwrap (either by force or `??`) to satisfy the cast,
    // otherwise we'd have to fallback to force downcast.
    if (repairViaOptionalUnwrap(*this, lhs, rhs, matchKind,
                                conversionsOrFixes,
                                getConstraintLocator(coercion->getSubExpr())))
      return true;

    // If the result type of the coercion has an value to optional conversion
    // we can instead suggest the conditional downcast as it is safer in
    // situations like conditional binding.
    auto useConditionalCast =
        llvm::any_of(ConstraintRestrictions, [&](const auto &restriction) {
          Type type1, type2;
          std::tie(type1, type2) = restriction.first;
          auto restrictionKind = restriction.second;

          if (restrictionKind != ConversionRestrictionKind::ValueToOptional)
            return false;

          return rhs->isEqual(type1);
        });

    // Repair a coercion ('as') with a runtime checked cast ('as!' or 'as?').
    if (auto *coerceToCheckCastFix =
            CoerceToCheckedCast::attempt(*this, lhs, rhs, useConditionalCast,
                                         getConstraintLocator(locator))) {
      conversionsOrFixes.push_back(coerceToCheckCastFix);
      return true;
    }

    // If it has a deep equality restriction, defer the diagnostic to
    // GenericMismatch.
    if (hasConversionOrRestriction(ConversionRestrictionKind::DeepEquality) &&
        !hasConversionOrRestriction(
            ConversionRestrictionKind::OptionalToOptional)) {
      return false;
    }

    if (hasConversionOrRestriction(ConversionRestrictionKind::Existential))
      return false;

    auto *fix = ContextualMismatch::create(*this, lhs, rhs,
                                           getConstraintLocator(locator));
    conversionsOrFixes.push_back(fix);
    return true;
  }
  default:
    break;
  }

  return !conversionsOrFixes.empty();
}

static bool isTupleWithUnresolvedPackExpansion(Type type) {
  if (auto *tuple = type->getAs<TupleType>()) {
    return llvm::any_of(tuple->getElements(), [&](const TupleTypeElt &elt) {
      if (auto typeVar = elt.getType()->getAs<TypeVariableType>())
        return typeVar->getImpl().isPackExpansion();
      return false;
    });
  }
  return false;
}

ConstraintSystem::TypeMatchResult
ConstraintSystem::matchTypes(Type type1, Type type2, ConstraintKind kind,
                             TypeMatchOptions flags,
                             ConstraintLocatorBuilder locator) {
  auto origType1 = type1;
  auto origType2 = type2;

  // If we have type variables that have been bound to fixed types, look through
  // to the fixed type.
  type1 = getFixedTypeRecursive(type1, flags, kind == ConstraintKind::Equal);
  type2 = getFixedTypeRecursive(type2, flags, kind == ConstraintKind::Equal);

  auto desugar1 = type1->getDesugaredType();
  auto desugar2 = type2->getDesugaredType();

  // If both sides are dependent members without type variables, it's
  // possible that base type is incorrect e.g. `Foo.Element` where `Foo`
  // is a concrete type substituted for generic parameter,
  // so checking equality here would lead to incorrect behavior,
  // let's defer it until later proper check.
  if (!(desugar1->is<DependentMemberType>() &&
        desugar2->is<DependentMemberType>())) {
    // If the types are obviously equivalent, we're done.
    if (desugar1->isEqual(desugar2) && !isa<InOutType>(desugar2)) {
      return getTypeMatchSuccess();
    }
  }

  // Local function that should be used to produce the return value whenever
  // this function was unable to resolve the constraint. It should be used
  // within \c matchTypes() as
  //
  //   return formUnsolvedResult();
  //
  // along any unsolved path. No other returns should produce
  // SolutionKind::Unsolved or inspect TMF_GenerateConstraints.
  auto formUnsolvedResult = [&] {
    // If we're supposed to generate constraints (i.e., this is a
    // newly-generated constraint), do so now.
    if (flags.contains(TMF_GenerateConstraints)) {
      // Add a new constraint between these types. We consider the current
      // type-matching problem to the "solved" by this addition, because
      // this new constraint will be solved at a later point.
      // Obviously, this must not happen at the top level, or the
      // algorithm would not terminate.
      addUnsolvedConstraint(Constraint::create(*this, kind, type1, type2,
                                               getConstraintLocator(locator)));
      return getTypeMatchSuccess();
    }

    return getTypeMatchAmbiguous();
  };

  auto *typeVar1 = dyn_cast<TypeVariableType>(desugar1);
  auto *typeVar2 = dyn_cast<TypeVariableType>(desugar2);

  // If either (or both) types are type variables, unify the type variables.
  if (typeVar1 || typeVar2) {
    // Handle the easy case of both being type variables, and being
    // identical, first.
    if (typeVar1 && typeVar2) {
      auto rep1 = getRepresentative(typeVar1);
      auto rep2 = getRepresentative(typeVar2);
      if (rep1 == rep2) {
        // We already merged these two types, so this constraint is
        // trivially solved.
        return getTypeMatchSuccess();
      }
    }

    switch (kind) {
    case ConstraintKind::Bind:
    case ConstraintKind::BindToPointerType:
    case ConstraintKind::Equal: {
      if (typeVar1 && typeVar2) {
        auto rep1 = getRepresentative(typeVar1);
        auto rep2 = getRepresentative(typeVar2);

        // Pack expansion variables cannot be merged because
        // they involve other type variables.
        if (rep1->getImpl().isPackExpansion() ||
            rep2->getImpl().isPackExpansion())
          return formUnsolvedResult();

        // If exactly one of the type variables can bind to an lvalue, we
        // can't merge these two type variables.
        if (kind == ConstraintKind::Equal &&
            rep1->getImpl().canBindToLValue()
              != rep2->getImpl().canBindToLValue())
          return formUnsolvedResult();

        // Merge the equivalence classes corresponding to these two variables.
        mergeEquivalenceClasses(rep1, rep2, /*updateWorkList=*/true);
        return getTypeMatchSuccess();
      }

      assert((type1->is<TypeVariableType>() != type2->is<TypeVariableType>()) &&
             "Expected a type variable and a non type variable!");

      auto *typeVar = typeVar1 ? typeVar1 : typeVar2;
      auto type = typeVar1 ? type2 : type1;

      return matchTypesBindTypeVar(typeVar, type, kind, flags, locator,
                                   formUnsolvedResult);
    }

    case ConstraintKind::BindParam: {
      if (typeVar2 && !typeVar1) {
        // Simplify the left-hand type and perform the "occurs" check.
        auto rep2 = getRepresentative(typeVar2);
        type1 = simplifyType(type1, flags);
        if (!isBindable(typeVar2, type1))
          return formUnsolvedResult();

        if (auto *iot = type1->getAs<InOutType>()) {
          if (!rep2->getImpl().canBindToLValue())
            return getTypeMatchFailure(locator);
          assignFixedType(rep2, LValueType::get(iot->getObjectType()));
        } else {
          assignFixedType(rep2, type1);
        }
        return getTypeMatchSuccess();
      } else if (typeVar1 && !typeVar2) {
        // Simplify the right-hand type and perform the "occurs" check.
        auto rep1 = getRepresentative(typeVar1);
        type2 = simplifyType(type2, flags);
        if (!isBindable(rep1, type2))
          return formUnsolvedResult();

        if (auto *lvt = type2->getAs<LValueType>()) {
          if (!rep1->getImpl().canBindToInOut())
            return getTypeMatchFailure(locator);
          assignFixedType(rep1, InOutType::get(lvt->getObjectType()));
        } else {
          assignFixedType(rep1, type2);
        }
        return getTypeMatchSuccess();
      } if (typeVar1 && typeVar2) {
        auto rep1 = getRepresentative(typeVar1);
        auto rep2 = getRepresentative(typeVar2);

        // Pack expansion variables cannot be merged because
        // they involve other type variables.
        if (rep1->getImpl().isPackExpansion() ||
            rep2->getImpl().isPackExpansion())
          return formUnsolvedResult();

        if (!rep1->getImpl().canBindToInOut() ||
            !rep2->getImpl().canBindToLValue()) {
          // Merge the equivalence classes corresponding to these two variables.
          mergeEquivalenceClasses(rep1, rep2, /*updateWorkList=*/true);
          return getTypeMatchSuccess();
        }
      }

      return formUnsolvedResult();
    }

    case ConstraintKind::Subtype:
    case ConstraintKind::Conversion:
    case ConstraintKind::ArgumentConversion:
    case ConstraintKind::OperatorArgumentConversion: {
      if (typeVar1) {
        if (auto *locator = typeVar1->getImpl().getLocator()) {
          // TODO(diagnostics): Only binding here for function types, because
          // doing so for KeyPath types leaves the constraint system in an
          // unexpected state for key path diagnostics should we fail.
          if (locator->isLastElement<LocatorPathElt::KeyPathType>() &&
              type2->is<AnyFunctionType>())
            return matchTypesBindTypeVar(typeVar1, type2, kind, flags, locator,
                                         formUnsolvedResult);
        }

        // Performance optimization: Propagate fully or partially resolved
        // contextual type down into the body of result builder transformed
        // closure by eagerly binding intermediate body result type to the
        // contextual one. This helps to determine when closure body could be
        // solved early.
        //
        // TODO: This could be extended to cover all multi-statement closures.
        //
        // See \c BindingSet::favoredOverConjunction for more details.
        if (!typeVar2 && locator.endsWith<LocatorPathElt::FunctionResult>()) {
          SmallVector<LocatorPathElt> path;
          auto anchor = locator.getLocatorParts(path);

          // Drop `FunctionResult` element.
          path.pop_back();

          ClosureExpr *closure = nullptr;
          {
            // This avoids a new locator allocation.
            SourceRange range;
            ArrayRef<LocatorPathElt> scratchPath(path);
            simplifyLocator(anchor, scratchPath, range);

            if (scratchPath.empty())
              closure = getAsExpr<ClosureExpr>(anchor);
          }

          if (closure && !closure->hasExplicitResultType() &&
              getAppliedResultBuilderTransform(closure)) {
            return matchTypesBindTypeVar(typeVar1, type2, ConstraintKind::Equal,
                                         flags, locator, formUnsolvedResult);
          }
        }
      }

      return formUnsolvedResult();
    }

    case ConstraintKind::ApplicableFunction:
    case ConstraintKind::DynamicCallableApplicableFunction:
    case ConstraintKind::BindOverload:
    case ConstraintKind::BridgingConversion:
    case ConstraintKind::CheckedCast:
    case ConstraintKind::SubclassOf:
    case ConstraintKind::ConformsTo:
    case ConstraintKind::TransitivelyConformsTo:
    case ConstraintKind::Defaultable:
    case ConstraintKind::Disjunction:
    case ConstraintKind::Conjunction:
    case ConstraintKind::DynamicTypeOf:
    case ConstraintKind::EscapableFunctionOf:
    case ConstraintKind::OpenedExistentialOf:
    case ConstraintKind::KeyPath:
    case ConstraintKind::KeyPathApplication:
    case ConstraintKind::LiteralConformsTo:
    case ConstraintKind::OptionalObject:
    case ConstraintKind::SelfObjectOfProtocol:
    case ConstraintKind::UnresolvedValueMember:
    case ConstraintKind::ValueMember:
    case ConstraintKind::ValueWitness:
    case ConstraintKind::OneWayEqual:
    case ConstraintKind::OneWayBindParam:
    case ConstraintKind::DefaultClosureType:
    case ConstraintKind::UnresolvedMemberChainBase:
    case ConstraintKind::PropertyWrapper:
    case ConstraintKind::SyntacticElement:
    case ConstraintKind::BindTupleOfFunctionParams:
    case ConstraintKind::PackElementOf:
    case ConstraintKind::ShapeOf:
    case ConstraintKind::ExplicitGenericArguments:
    case ConstraintKind::SameShape:
      llvm_unreachable("Not a relational constraint");
    }
  }

  // If one of the types is a member type of a type variable type,
  // there's nothing we can do.
  if (desugar1->isTypeVariableOrMember() ||
      desugar2->isTypeVariableOrMember()) {
    return formUnsolvedResult();
  }

  // If the original type on one side consisted of a tuple type with
  // unresolved pack expansion(s), let's make sure that both sides are
  // tuples to enable proper pack matching for situations like:
  //
  // `Int <conversion> (_: $T3)`
  // where `$T3` is pack expansion of pattern type `$T2`
  //
  // `Int` should be wrapped in a one-element tuple to make sure
  // that tuple matcher can form a pack expansion type that would
  // match `$T3` and propagate `Pack{Int}` to `$T2`.
  //
  // This is also important for situations like: `$T2 conv (Int, $T_exp)`
  // becuase expansion could be defaulted to an empty pack which means
  // that under substitution that element would disappear and the type
  // would be just `(Int)`.
  //
  // Notable exception here is `Any` which doesn't require wrapping and
  // would be handled by existental promotion in cases where it's allowed.
  if (isTupleWithUnresolvedPackExpansion(origType1) ||
      isTupleWithUnresolvedPackExpansion(origType2)) {
    if (desugar1->is<TupleType>() != desugar2->is<TupleType>() &&
        (!desugar1->isAny() && !desugar2->isAny())) {
      return matchTypes(
          desugar1->is<TupleType>() ? type1
                                    : TupleType::get({type1}, getASTContext()),
          desugar2->is<TupleType>() ? type2
                                    : TupleType::get({type2}, getASTContext()),
          kind, flags, locator);
    }
  }

  llvm::SmallVector<RestrictionOrFix, 4> conversionsOrFixes;

  // Decompose parallel structure.
  TypeMatchOptions subflags =
    getDefaultDecompositionOptions(flags) - TMF_ApplyingFix;
  if (desugar1->getKind() == desugar2->getKind()) {
    switch (desugar1->getKind()) {
#define SUGARED_TYPE(id, parent) case TypeKind::id:
#define TYPE(id, parent)
#include "swift/AST/TypeNodes.def"
      llvm_unreachable("Type has not been desugared completely");

#define ARTIFICIAL_TYPE(id, parent) case TypeKind::id:
#define TYPE(id, parent)
#include "swift/AST/TypeNodes.def"
      llvm_unreachable("artificial type in constraint");

    case TypeKind::BuiltinTuple:
      llvm_unreachable("BuiltinTupleType in constraint");

    // Note: Mismatched builtin types fall through to the TypeKind::Error
    // case below.
#define BUILTIN_TYPE(id, parent) case TypeKind::id:
#define TYPE(id, parent)
#include "swift/AST/TypeNodes.def"

    case TypeKind::Error:
    case TypeKind::Unresolved:
      return getTypeMatchFailure(locator);

    case TypeKind::Placeholder: {
      // If it's allowed to attempt fixes, let's delegate
      // decision to `repairFailures`, since depending on
      // locator we might either ignore such a mismatch,
      // or record a specialized fix.
      if (shouldAttemptFixes())
        break;

      return getTypeMatchFailure(locator);
    }

    case TypeKind::GenericTypeParam:
      llvm_unreachable("unmapped dependent type in type checker");

    case TypeKind::TypeVariable:
      llvm_unreachable("type variables should have already been handled by now");

    case TypeKind::DependentMember: {
      // If types are identical, let's consider this constraint solved
      // even though they are dependent members, they would be resolved
      // to the same concrete type.
      if (desugar1->isEqual(desugar2))
        return getTypeMatchSuccess();

      if (shouldAttemptFixes()) {
        if (!desugar1->hasTypeVariable() && !desugar2->hasTypeVariable()) {
          auto *loc = getConstraintLocator(locator);

          auto *fix =
              loc->isLastElement<LocatorPathElt::TypeParameterRequirement>()
                  ? fixRequirementFailure(*this, type1, type2, loc->getAnchor(),
                                          loc->getPath())
                  : ContextualMismatch::create(*this, type1, type2, loc);

          if (!fix || recordFix(fix))
            return getTypeMatchFailure(locator);

          return getTypeMatchSuccess();
        }
      }

      // If one of the dependent member types has no type variables,
      // this comparison is effectively illformed, because dependent
      // member couldn't be simplified down to the actual type, and
      // we wouldn't be able to solve this constraint, so let's just fail.
      // This should only happen outside of diagnostic mode, as otherwise the
      // member is replaced by a placeholder in simplifyType.
      if (!desugar1->hasTypeVariable() || !desugar2->hasTypeVariable())
        return getTypeMatchFailure(locator);

      // Nothing we can solve yet, since we need to wait until
      // type variables will get resolved.
      return formUnsolvedResult();
    }

    case TypeKind::Module:
    case TypeKind::PrimaryArchetype:
    case TypeKind::OpenedArchetype:
    case TypeKind::PackArchetype:
    case TypeKind::ElementArchetype: {
      // Give `repairFailures` a chance to fix the problem.
      if (shouldAttemptFixes())
        break;

      // If two module types or archetypes were not already equal, there's
      // nothing more we can do.
      return getTypeMatchFailure(locator);
    }

    case TypeKind::Tuple: {
      // FIXME: TuplePackMatcher doesn't correctly handle matching two
       // abstract contextual tuple types in a generic context.
       if (simplifyType(desugar1)->isEqual(simplifyType(desugar2)))
         return getTypeMatchSuccess();

      // If the tuple has consecutive pack expansions, packs must be
      // resolved before matching.
      auto delayMatching = [](TupleType *tuple) {
        bool afterPack = false;
        for (auto element : tuple->getElements()) {
          if (afterPack && !element.hasName()) {
            SmallPtrSet<TypeVariableType *, 2> typeVars;
            element.getType()->getTypeVariables(typeVars);

            bool hasUnresolvedPack = llvm::any_of(typeVars, [](auto *tv) {
              return tv->getImpl().canBindToPack();
            });

            if (hasUnresolvedPack)
              return true;
          }

          // Delay matching if one of the elements is unresolved pack
          // expansion represented by a type variable.
          if (auto *typeVar = element.getType()->getAs<TypeVariableType>()) {
            if (typeVar->getImpl().isPackExpansion())
              return true;
          }

          afterPack = element.getType()->is<PackExpansionType>();
        }

        return false;
      };


      auto *tuple1 = cast<TupleType>(desugar1);
      auto *tuple2 = cast<TupleType>(desugar2);
      if (delayMatching(tuple1) || delayMatching(tuple2)) {
        return formUnsolvedResult();
      }

      // Closure result is allowed to convert to Void in certain circumstances,
      // let's forego tuple matching because it is guaranteed to fail and jump
      // to `() -> T` to `() -> Void` rule.
      if (locator.endsWith<LocatorPathElt::ClosureBody>()) {
        if (containsPackExpansionType(tuple1) && tuple2->isVoid())
          break;
      }

      // Add each tuple type to the locator before matching the element types.
      // This is useful for diagnostics, because the error message can use the
      // full tuple type for several element mismatches. Use the original types
      // to preserve sugar such as typealiases.
      auto tmpTupleLoc = locator.withPathElement(LocatorPathElt::TupleType(type1));
      auto tupleLoc = tmpTupleLoc.withPathElement(LocatorPathElt::TupleType(type2));
      auto result = matchTupleTypes(cast<TupleType>(desugar1),
                                    cast<TupleType>(desugar2),
                                    kind, subflags, tupleLoc);
      if (result != SolutionKind::Error)
        return result;

      // FIXME: All cases in this switch should go down to the fix logic
      // to give repairFailures() a chance to run, but this breaks stuff
      // right now.
      break;
    }

    case TypeKind::Enum:
    case TypeKind::Struct:
    case TypeKind::Class: {
      auto nominal1 = cast<NominalType>(desugar1);
      auto nominal2 = cast<NominalType>(desugar2);
      if (nominal1->getDecl() == nominal2->getDecl())
        conversionsOrFixes.push_back(ConversionRestrictionKind::DeepEquality);

      // Check for CF <-> ObjectiveC bridging.
      if (isa<ClassType>(desugar1) &&
          kind >= ConstraintKind::Subtype) {
        auto class1 = cast<ClassDecl>(nominal1->getDecl());
        auto class2 = cast<ClassDecl>(nominal2->getDecl());

        // CF -> Objective-C via toll-free bridging.
        if (class1->getForeignClassKind() == ClassDecl::ForeignKind::CFType &&
            class2->getForeignClassKind() != ClassDecl::ForeignKind::CFType &&
            class1->getAttrs().hasAttribute<ObjCBridgedAttr>()) {
          conversionsOrFixes.push_back(
            ConversionRestrictionKind::CFTollFreeBridgeToObjC);
        }

        // Objective-C -> CF via toll-free bridging.
        if (class2->getForeignClassKind() == ClassDecl::ForeignKind::CFType &&
            class1->getForeignClassKind() != ClassDecl::ForeignKind::CFType &&
            class2->getAttrs().hasAttribute<ObjCBridgedAttr>()) {
          conversionsOrFixes.push_back(
            ConversionRestrictionKind::ObjCTollFreeBridgeToCF);
        }
      }

      if (kind >= ConstraintKind::Subtype &&
          nominal1->getDecl() != nominal2->getDecl() &&
          ((nominal1->isCGFloat() || nominal2->isCGFloat()) &&
           (nominal1->isDouble() || nominal2->isDouble()))) {
        ConstraintLocatorBuilder location{locator};
        // Look through all value-to-optional promotions to allow
        // conversions like Double -> CGFloat?? and vice versa.
        // T -> Optional<T>
        if (location.endsWith<LocatorPathElt::OptionalPayload>()) {
          SmallVector<LocatorPathElt, 4> path;
          auto anchor = location.getLocatorParts(path);

          // An attempt at Double/CGFloat conversion through
          // optional chaining. This is not supported at the
          // moment because solution application doesn't know
          // how to map Double to/from CGFloat through optionals.
          if (isExpr<OptionalEvaluationExpr>(anchor)) {
            if (!shouldAttemptFixes())
              return getTypeMatchFailure(locator);

            conversionsOrFixes.push_back(ContextualMismatch::create(
                *this, nominal1, nominal2, getConstraintLocator(locator)));
            break;
          }

          // Drop all of the applied `value-to-optional` promotions.
          path.erase(llvm::remove_if(
                         path,
                         [](const LocatorPathElt &elt) {
                           return elt.is<LocatorPathElt::OptionalPayload>();
                         }),
                     path.end());

          location = getConstraintLocator(anchor, path);
        }

        // Support implicit Double<->CGFloat conversions only for
        // something which could be directly represented in the AST
        // e.g. argument-to-parameter, contextual conversions etc.
        if (!location.trySimplifyToExpr()) {
          return getTypeMatchFailure(locator);
        }

        SmallVector<LocatorPathElt, 4> path;
        auto anchor = location.getLocatorParts(path);

        // Try implicit CGFloat conversion only if:
        // - This is not:
        //     - an explicit call to a CGFloat initializer;
        //     - an explicit coercion;
        //     - a runtime type check (via `is` expression);
        //     - a checked or conditional cast;
        // - This is a first type such conversion is attempted for
        //   for a given path (AST element).

        auto isCGFloatInit = [&](ASTNode location) {
          if (auto *call = getAsExpr<CallExpr>(location)) {
            if (auto *typeExpr = dyn_cast<TypeExpr>(call->getFn())) {
              return getInstanceType(typeExpr)->isCGFloat();
            }
          }
          return false;
        };

        auto isCoercionOrCast = [](ASTNode anchor,
                                   ArrayRef<LocatorPathElt> path) {
          // E.g. contextual conversion from coercion/cast
          // to some other type.
          if (!(path.empty() ||
                path.back().is<LocatorPathElt::CoercionOperand>()))
            return false;

          return isExpr<CoerceExpr>(anchor) || isExpr<IsExpr>(anchor) ||
                 isExpr<ConditionalCheckedCastExpr>(anchor) ||
                 isExpr<ForcedCheckedCastExpr>(anchor);
        };

        if (!isCGFloatInit(anchor) && !isCoercionOrCast(anchor, path) &&
            llvm::none_of(path, [&](const LocatorPathElt &rawElt) {
              if (auto elt =
                      rawElt.getAs<LocatorPathElt::ImplicitConversion>()) {
                auto convKind = elt->getConversionKind();
                return convKind == ConversionRestrictionKind::DoubleToCGFloat ||
                       convKind == ConversionRestrictionKind::CGFloatToDouble;
              }
              return false;
            })) {
          conversionsOrFixes.push_back(
              desugar1->isCGFloat()
                  ? ConversionRestrictionKind::CGFloatToDouble
                  : ConversionRestrictionKind::DoubleToCGFloat);
        }
      }

      break;
    }

    case TypeKind::DynamicSelf:
      // FIXME: Deep equality? What is the rule between two DynamicSelfs?
      break;
       
    case TypeKind::Protocol:
      // Nothing to do here; try existential and user-defined conversions below.
      break;

    case TypeKind::Metatype:
    case TypeKind::ExistentialMetatype: {
      auto meta1 = cast<AnyMetatypeType>(desugar1);
      auto meta2 = cast<AnyMetatypeType>(desugar2);

      // A.Type < B.Type if A < B and both A and B are classes.
      // P.Type < Q.Type if P < Q, both P and Q are protocols, and P.Type
      // and Q.Type are both existential metatypes
      auto subKind = std::min(kind, ConstraintKind::Subtype);
      // If instance types can't have a subtype relationship
      // it means that such types can be simply equated.
      auto instanceType1 = meta1->getInstanceType();
      auto instanceType2 = meta2->getInstanceType();
      if (isa<MetatypeType>(meta1) &&
          !(instanceType1->mayHaveSuperclass() &&
            instanceType2->getClassOrBoundGenericClass())) {
        subKind = ConstraintKind::Bind;
      }

      auto result =
          matchTypes(instanceType1, instanceType2, subKind, subflags,
                     locator.withPathElement(ConstraintLocator::InstanceType));

      // If matching of the instance types resulted in the failure make sure
      // to give `repairFailure` a chance to run to attempt to fix the issue.
      if (shouldAttemptFixes() && result.isFailure())
        break;

      return result;
    }

    case TypeKind::Function: {
      auto func1 = cast<FunctionType>(desugar1);
      auto func2 = cast<FunctionType>(desugar2);

      auto result = matchFunctionTypes(func1, func2, kind, flags, locator);

      if (shouldAttemptFixes() && result.isFailure())
        break;

      return result;
    }

    case TypeKind::GenericFunction:
      llvm_unreachable("Polymorphic function type should have been opened");

    case TypeKind::Existential:
    case TypeKind::ProtocolComposition:
    case TypeKind::ParameterizedProtocol:
      switch (kind) {
      case ConstraintKind::Equal:
      case ConstraintKind::Bind:
      case ConstraintKind::BindParam:
        // If we are matching types for equality, we might still have
        // type variables inside the protocol composition's superclass
        // constraint.
        if (desugar1->getKind() == desugar2->getKind())
          conversionsOrFixes.push_back(ConversionRestrictionKind::DeepEquality);
        break;

      default:
        // Subtype constraints where the RHS is an existential type are
        // handled below.
        break;
      }

      break;

    case TypeKind::LValue:
      if (kind == ConstraintKind::BindParam)
        return getTypeMatchFailure(locator);
      return matchTypes(cast<LValueType>(desugar1)->getObjectType(),
                        cast<LValueType>(desugar2)->getObjectType(),
                        ConstraintKind::Bind, subflags,
                        locator.withPathElement(
                          ConstraintLocator::LValueConversion));
    
    case TypeKind::InOut:
      if (kind == ConstraintKind::BindParam)
        return getTypeMatchFailure(locator);
      
      if (kind == ConstraintKind::OperatorArgumentConversion) {
        conversionsOrFixes.push_back(
            RemoveAddressOf::create(*this, type1, type2,
                                    getConstraintLocator(locator)));
        break;
      }

      return matchTypes(cast<InOutType>(desugar1)->getObjectType(),
                        cast<InOutType>(desugar2)->getObjectType(),
                        ConstraintKind::Bind, subflags,
                  locator.withPathElement(ConstraintLocator::LValueConversion));

    case TypeKind::UnboundGeneric:
      llvm_unreachable("Unbound generic type should have been opened");

    case TypeKind::BoundGenericClass:
    case TypeKind::BoundGenericEnum:
    case TypeKind::BoundGenericStruct: {
      auto bound1 = cast<BoundGenericType>(desugar1);
      auto bound2 = cast<BoundGenericType>(desugar2);
      
      if (bound1->getDecl() == bound2->getDecl())
        conversionsOrFixes.push_back(ConversionRestrictionKind::DeepEquality);
      break;
    }

    // Opaque archetypes are globally bound, so we can match them for deep
    // equality.
    case TypeKind::OpaqueTypeArchetype: {
      auto opaque1 = cast<OpaqueTypeArchetypeType>(desugar1);
      auto opaque2 = cast<OpaqueTypeArchetypeType>(desugar2);
      
      if (opaque1->getDecl() == opaque2->getDecl()) {
        conversionsOrFixes.push_back(ConversionRestrictionKind::DeepEquality);
      }
      break;
    }
    
    case TypeKind::Pack: {
      auto tmpPackLoc = locator.withPathElement(LocatorPathElt::PackType(type1));
      auto packLoc = tmpPackLoc.withPathElement(LocatorPathElt::PackType(type2));

      auto result =
          matchPackTypes(cast<PackType>(desugar1), cast<PackType>(desugar2),
                         kind, subflags, packLoc);

      // Let `repairFailures` attempt to "fix" this.
      if (shouldAttemptFixes() && result.isFailure())
        break;

      return result;
    }
    case TypeKind::PackExpansion: {
      auto expansion1 = cast<PackExpansionType>(desugar1);
      auto expansion2 = cast<PackExpansionType>(desugar2);

      return matchPackExpansionTypes(expansion1, expansion2, kind, subflags,
                                     locator);
    }
    }
  }

  if (kind >= ConstraintKind::Conversion) {
    // An lvalue of type T1 can be converted to a value of type T2 so long as
    // T1 is convertible to T2 (by loading the value).  Note that we cannot get
    // a value of inout type as an lvalue though.
    if (type1->is<LValueType>() && !type2->is<InOutType>()) {
      auto result = matchTypes(type1->getWithoutSpecifierType(), type2, kind,
                               subflags, locator);
      if (result.isSuccess() || !shouldAttemptFixes())
        return result;
    }
  }

  if (kind >= ConstraintKind::Subtype) {
    // Subclass-to-superclass conversion.
    if (type1->mayHaveSuperclass() &&
        type2->getClassOrBoundGenericClass() &&
        type1->getClassOrBoundGenericClass()
          != type2->getClassOrBoundGenericClass()) {
      conversionsOrFixes.push_back(ConversionRestrictionKind::Superclass);
    }

    // Existential-to-superclass conversion.
    if (type1->isClassExistentialType() &&
        type2->getClassOrBoundGenericClass()) {
      conversionsOrFixes.push_back(ConversionRestrictionKind::Superclass);
    }

    // Metatype-to-existential-metatype conversion.
    //
    // Equivalent to a conformance relation on the instance types.
    if (type1->is<MetatypeType>() &&
        type2->is<ExistentialMetatypeType>()) {
      conversionsOrFixes.push_back(
        ConversionRestrictionKind::MetatypeToExistentialMetatype);
    }

    // Existential-metatype-to-superclass-metatype conversion.
    if (type2->is<MetatypeType>()) {
      if (auto *meta1 = type1->getAs<ExistentialMetatypeType>()) {
        if (meta1->getInstanceType()->isClassExistentialType()) {
          conversionsOrFixes.push_back(
            ConversionRestrictionKind::ExistentialMetatypeToMetatype);
        }
      }
    }

    // Concrete value to existential conversion.
    if (!type1->is<LValueType>() &&
        type2->isExistentialType()) {

      // Penalize conversions to Any.
      if (kind >= ConstraintKind::Conversion && type2->isAny())
        increaseScore(ScoreKind::SK_EmptyExistentialConversion);

      conversionsOrFixes.push_back(ConversionRestrictionKind::Existential);
    }

    // T -> AnyHashable.
    if (desugar2->isAnyHashable()) {
      // Don't allow this in operator contexts or we'll end up allowing
      // 'T() == U()' for unrelated T and U that just happen to be Hashable.
      // We can remove this special case when we implement operator hiding.
      if (!type1->is<LValueType>() &&
          kind != ConstraintKind::OperatorArgumentConversion) {
        conversionsOrFixes.push_back(
                              ConversionRestrictionKind::HashableToAnyHashable);
      }
    }

    // Metatype to object conversion.
    //
    // Class and protocol metatypes are interoperable with certain Objective-C
    // runtime classes, but only when ObjC interop is enabled.

    if (getASTContext().LangOpts.EnableObjCInterop) {
      // These conversions are between concrete types that don't need further
      // resolution, so we can consider them immediately solved.
      auto addSolvedRestrictedConstraint
        = [&](ConversionRestrictionKind restriction) -> TypeMatchResult {
          addRestrictedConstraint(ConstraintKind::Subtype, restriction,
                                  type1, type2, locator);
          return getTypeMatchSuccess();
        };

      // Foreign reference types do *not* conform to AnyObject.
      if (type1->isForeignReferenceType() && type2->isAnyObject())
        return getTypeMatchFailure(locator);
      
      if (auto meta1 = type1->getAs<MetatypeType>()) {
        if (meta1->getInstanceType()->mayHaveSuperclass()
            && type2->isAnyObject()) {
          increaseScore(ScoreKind::SK_UserConversion);
          return addSolvedRestrictedConstraint(
                           ConversionRestrictionKind::ClassMetatypeToAnyObject);
        }
        // Single @objc protocol value metatypes can be converted to the ObjC
        // Protocol class type.
        auto isProtocolClassType = [&](Type t) -> bool {
          if (auto classDecl = t->getClassOrBoundGenericClass())
            if (classDecl->getName() == getASTContext().Id_Protocol
                && classDecl->getModuleContext()->getName()
                    == getASTContext().Id_ObjectiveC)
              return true;
          return false;
        };

        auto constraintType = meta1->getInstanceType();
        if (auto existential = constraintType->getAs<ExistentialType>())
          constraintType = existential->getConstraintType();
        
        if (auto protoTy = constraintType->getAs<ProtocolType>()) {
          if (protoTy->getDecl()->isObjC()
              && isProtocolClassType(type2)) {
            increaseScore(ScoreKind::SK_UserConversion);
            return addSolvedRestrictedConstraint(
                    ConversionRestrictionKind::ProtocolMetatypeToProtocolClass);
          }
        }
      }
      if (auto meta1 = type1->getAs<ExistentialMetatypeType>()) {
        // Class-constrained existential metatypes can be converted to AnyObject.
        if (meta1->getInstanceType()->isClassExistentialType()
            && type2->isAnyObject()) {
          increaseScore(ScoreKind::SK_UserConversion);
          return addSolvedRestrictedConstraint(
                     ConversionRestrictionKind::ExistentialMetatypeToAnyObject);
        }
      }
    }

    // Special implicit nominal conversions.
    if (!type1->is<LValueType>() && kind >= ConstraintKind::Subtype) {
      // Array -> Array.
      if (desugar1->isArrayType() && desugar2->isArrayType()) {
        conversionsOrFixes.push_back(ConversionRestrictionKind::ArrayUpcast);
      // Dictionary -> Dictionary.
      } else if (isDictionaryType(desugar1) && isDictionaryType(desugar2)) {
        conversionsOrFixes.push_back(
          ConversionRestrictionKind::DictionaryUpcast);
      // Set -> Set.
      } else if (isSetType(desugar1) && isSetType(desugar2)) {
        conversionsOrFixes.push_back(
          ConversionRestrictionKind::SetUpcast);
      }
    }
  }
  
  if (kind == ConstraintKind::BindToPointerType) {
    if (desugar2->isEqual(getASTContext().TheEmptyTupleType))
      return getTypeMatchSuccess();
  }

  if (kind >= ConstraintKind::Conversion) {
    // It is never legal to form an autoclosure that results in these
    // implicit conversions to pointer types.
    bool isAutoClosureArgument = locator.isForAutoclosureResult();

    // Pointer arguments can be converted from pointer-compatible types.
    if (kind >= ConstraintKind::ArgumentConversion) {
      Type unwrappedType2 = type2;
      bool type2IsOptional = false;
      if (Type unwrapped = type2->getOptionalObjectType()) {
        type2IsOptional = true;
        unwrappedType2 = unwrapped;
      }
      PointerTypeKind pointerKind;
      if (Type pointeeTy =
              unwrappedType2->getAnyPointerElementType(pointerKind)) {
        switch (pointerKind) {
        case PTK_UnsafeRawPointer:
        case PTK_UnsafeMutableRawPointer:
        case PTK_UnsafePointer:
        case PTK_UnsafeMutablePointer:
          // UnsafeMutablePointer can be converted from an inout reference to a
          // scalar or array.
          if (auto inoutType1 = dyn_cast<InOutType>(desugar1)) {
            if (!isAutoClosureArgument) {
              auto inoutBaseType = inoutType1->getInOutObjectType();

              auto baseIsArray =
                  getFixedTypeRecursive(inoutBaseType, /*wantRValue=*/true)
                      ->isArrayType();

              // FIXME: If the base is still a type variable, we can't tell
              // what to do here. Might have to try \c ArrayToPointer and make
              // it more robust.
              if (baseIsArray)
                conversionsOrFixes.push_back(
                    ConversionRestrictionKind::ArrayToPointer);

              // Only try an inout-to-pointer conversion if we know it's not
              // an array being converted to a raw pointer type. Such
              // conversions can only use array-to-pointer.
              if (!baseIsArray || !isRawPointerKind(pointerKind)) {
                conversionsOrFixes.push_back(
                    ConversionRestrictionKind::InoutToPointer);

                // If regular inout-to-pointer conversion doesn't work,
                // let's try C pointer conversion that has special semantics
                // for imported declarations.
                if (isArgumentOfImportedDecl(locator)) {
                  conversionsOrFixes.push_back(
                      ConversionRestrictionKind::InoutToCPointer);
                }
              }
            }
          }

          // Operators cannot use these implicit conversions.
          if (kind == ConstraintKind::ArgumentConversion) {
            // We can potentially convert from an UnsafeMutablePointer
            // of a different type, if we're a void pointer.
            Type unwrappedType1 = type1;
            bool type1IsOptional = false;
            if (Type unwrapped = type1->getOptionalObjectType()) {
              type1IsOptional = true;
              unwrappedType1 = unwrapped;
            }

            // Don't handle normal optional-related conversions here.
            if (unwrappedType1->isEqual(unwrappedType2))
              break;

            PointerTypeKind type1PointerKind;
            bool type1IsPointer{
                unwrappedType1->getAnyPointerElementType(type1PointerKind)};
            bool optionalityMatches = !type1IsOptional || type2IsOptional;
            if (type1IsPointer && optionalityMatches) {
              if (type1PointerKind == PTK_UnsafeMutablePointer) {
                // Favor an UnsafeMutablePointer-to-UnsafeMutablePointer
                // conversion.
                if (type1PointerKind != pointerKind)
                  increaseScore(ScoreKind::SK_ValueToPointerConversion);
                conversionsOrFixes.push_back(
                  ConversionRestrictionKind::PointerToPointer);
              }
              // UnsafeMutableRawPointer -> UnsafeRawPointer
              else if (type1PointerKind == PTK_UnsafeMutableRawPointer &&
                       pointerKind == PTK_UnsafeRawPointer) {
                if (type1PointerKind != pointerKind)
                  increaseScore(ScoreKind::SK_ValueToPointerConversion);
                conversionsOrFixes.push_back(
                  ConversionRestrictionKind::PointerToPointer);              
              }
            }
            // UnsafePointer and UnsafeRawPointer can also be converted from an
            // array or string value, or a UnsafePointer or
            // AutoreleasingUnsafeMutablePointer.
            if (pointerKind == PTK_UnsafePointer
                || pointerKind == PTK_UnsafeRawPointer) {
              if (!isAutoClosureArgument) {
                if (type1->isArrayType()) {
                  conversionsOrFixes.push_back(
                      ConversionRestrictionKind::ArrayToPointer);

                  // If regular array-to-pointer conversion doesn't work,
                  // let's try C pointer conversion that has special semantics
                  // for imported declarations.
                  if (isArgumentOfImportedDecl(locator)) {
                    conversionsOrFixes.push_back(
                        ConversionRestrictionKind::ArrayToCPointer);
                  }
                }

                // The pointer can be converted from a string, if the element
                // type is compatible.
                auto &ctx = getASTContext();
                if (type1->isString()) {
                  auto baseTy = getFixedTypeRecursive(pointeeTy, false);

                  if (baseTy->isTypeVariableOrMember() ||
                      isStringCompatiblePointerBaseType(ctx, baseTy))
                    conversionsOrFixes.push_back(
                        ConversionRestrictionKind::StringToPointer);
                }
              }
              
              if (type1IsPointer && optionalityMatches &&
                  (type1PointerKind == PTK_UnsafePointer ||
                   type1PointerKind == PTK_AutoreleasingUnsafeMutablePointer)) {
                conversionsOrFixes.push_back(
                                   ConversionRestrictionKind::PointerToPointer);
              }
            }

            // If both sides are non-optional pointers, let's check whether
            // this argument supports Swift -> C pointer conversions.
            //
            // Do some light verification before recording restriction to
            // avoid allocating constraints for obviously invalid cases.
            if (type1IsPointer && !type1IsOptional && !type2IsOptional &&
                (shouldAttemptFixes() || isArgumentOfImportedDecl(locator))) {
              // UnsafeRawPointer -> UnsafePointer<[U]Int8>
              if (type1PointerKind == PTK_UnsafeRawPointer &&
                  pointerKind == PTK_UnsafePointer) {
                conversionsOrFixes.push_back(
                    ConversionRestrictionKind::PointerToCPointer);
              }

              // UnsafeMutableRawPointer -> Unsafe[Mutable]Pointer<[U]Int8>
              if (type1PointerKind == PTK_UnsafeMutableRawPointer &&
                  (pointerKind == PTK_UnsafePointer ||
                   pointerKind == PTK_UnsafeMutablePointer)) {
                conversionsOrFixes.push_back(
                    ConversionRestrictionKind::PointerToCPointer);
              }

              // Unsafe[Mutable]Pointer -> Unsafe[Mutable]Pointer
              if (type1PointerKind == PTK_UnsafePointer &&
                  pointerKind == PTK_UnsafePointer) {
                conversionsOrFixes.push_back(
                    ConversionRestrictionKind::PointerToCPointer);
              }

              if (type1PointerKind == PTK_UnsafeMutablePointer &&
                  (pointerKind == PTK_UnsafePointer ||
                   pointerKind == PTK_UnsafeMutablePointer)) {
                conversionsOrFixes.push_back(
                    ConversionRestrictionKind::PointerToCPointer);
              }
            }
          }
          break;

        case PTK_AutoreleasingUnsafeMutablePointer:
          // PTK_AutoreleasingUnsafeMutablePointer can be converted from an
          // inout reference to a scalar.
          if (!isAutoClosureArgument && type1->is<InOutType>()) {
            conversionsOrFixes.push_back(
                                     ConversionRestrictionKind::InoutToPointer);
          }
          break;
        }
      }
    }
  }

  if (kind >= ConstraintKind::OperatorArgumentConversion) {
    // If the RHS is an inout type, the LHS must be an @lvalue type.
    if (auto *lvt = type1->getAs<LValueType>()) {
      if (auto *iot = type2->getAs<InOutType>()) {
        return matchTypes(lvt->getObjectType(), iot->getObjectType(),
                          ConstraintKind::Bind, subflags,
                          locator.withPathElement(
                                  ConstraintLocator::LValueConversion));
      }
    }
  }

  // A value of type T! can be converted to type U if T is convertible
  // to U by force-unwrapping the source value.
  // A value of type T, T?, or T! can be converted to type U? or U! if
  // T is convertible to U.
  if (!type1->is<LValueType>() && kind >= ConstraintKind::Subtype) {
    enumerateOptionalConversionRestrictions(
        type1, type2, kind, locator,
        [&](ConversionRestrictionKind restriction) {
      conversionsOrFixes.push_back(restriction);
    });
  }

  // Allow '() -> T' to '() -> ()' and '() -> Never' to '() -> T' for closure
  // literals and expressions representing an implicit return type of the single
  // expression functions.
  if (auto elt = locator.last()) {
    if (kind >= ConstraintKind::Subtype &&
        (type1->isUninhabited() || type2->isVoid())) {
      // A conversion from closure body type to its signature result type.
      if (auto resultElt = elt->getAs<LocatorPathElt::ClosureBody>()) {
        // If a single statement closure has explicit `return` let's
        // forbid conversion to `Void` and report an error instead to
        // honor user's intent.
        if (type1->isUninhabited() || !resultElt->hasExplicitReturn()) {
          increaseScore(SK_FunctionConversion);
          return getTypeMatchSuccess();
        }
      }

      // Single expression function with implicit `return`.
      if (auto contextualType = elt->getAs<LocatorPathElt::ContextualType>()) {
        if (contextualType->isFor(CTP_ReturnSingleExpr)) {
          increaseScore(SK_FunctionConversion);
          return getTypeMatchSuccess();
        }
      }

      // We also need to propagate this conversion into the branches for single
      // value statements.
      //
      // As with the previous checks, we only allow the Void conversion in
      // an implicit single-expression closure. In the more general case, we
      // only allow the Never conversion.
      auto *loc = getConstraintLocator(locator);
      if (auto branchKind = loc->isForSingleValueStmtBranch()) {
        bool allowConversion = false;
        switch (*branchKind) {
        case SingleValueStmtBranchKind::Regular:
          allowConversion = type1->isUninhabited();
          break;
        case SingleValueStmtBranchKind::InSingleExprClosure:
          allowConversion = true;
          break;
        }
        if (allowConversion) {
          increaseScore(SK_FunctionConversion);
          return getTypeMatchSuccess();
        }
      }
    }
  }

  if (kind == ConstraintKind::BindParam) {
    if (auto *iot = dyn_cast<InOutType>(desugar1)) {
      if (auto *lvt = dyn_cast<LValueType>(desugar2)) {
        return matchTypes(iot->getObjectType(), lvt->getObjectType(),
                          ConstraintKind::Bind, subflags,
                          locator.withPathElement(
                            ConstraintLocator::LValueConversion));
      }
    }
  }

  // Matching types where one side is a pack expansion and the other is not
  // means a pack expansion was used where it isn't supported.
  if (type1->is<PackExpansionType>() != type2->is<PackExpansionType>()) {
    if (!shouldAttemptFixes())
      return getTypeMatchFailure(locator);

    if (type1->isPlaceholder() || type2->isPlaceholder())
      return getTypeMatchSuccess();

    auto *loc = getConstraintLocator(locator);
    if (recordFix(AllowInvalidPackExpansion::create(*this, loc)))
      return getTypeMatchFailure(locator);

    return getTypeMatchSuccess();
  }

  // Attempt fixes iff it's allowed, both types are concrete and
  // we are not in the middle of attempting one already.
  if (shouldAttemptFixes() && !flags.contains(TMF_ApplyingFix)) {
    if (repairFailures(type1, type2, kind, flags, conversionsOrFixes,
                       locator)) {
      if (conversionsOrFixes.empty())
        return getTypeMatchSuccess();
    }
  }

  if (conversionsOrFixes.empty())
    return getTypeMatchFailure(locator);

  // Where there is more than one potential conversion, create a disjunction
  // so that we'll explore all of the options.
  if (conversionsOrFixes.size() > 1) {
    auto fixedLocator = getConstraintLocator(locator);
    SmallVector<Constraint *, 2> constraints;

    for (auto potential : conversionsOrFixes) {
      auto constraintKind = kind;

      if (auto restriction = potential.getRestriction()) {
        // Determine the constraint kind. For a deep equality constraint, only
        // perform equality.
        if (*restriction == ConversionRestrictionKind::DeepEquality)
          constraintKind = ConstraintKind::Bind;

        constraints.push_back(
          Constraint::createRestricted(*this, constraintKind, *restriction,
                                       type1, type2, fixedLocator));

        if (constraints.back()->getKind() == ConstraintKind::Bind)
          constraints.back()->setFavored();

        continue;
      }

      auto fix = *potential.getFix();
      constraints.push_back(
        Constraint::createFixed(*this, constraintKind, fix, type1, type2,
                                fixedLocator));
    }

    // Sort favored constraints first.
    std::sort(constraints.begin(), constraints.end(),
              [&](Constraint *lhs, Constraint *rhs) -> bool {
                if (lhs->isFavored() == rhs->isFavored())
                  return false;

                return lhs->isFavored();
              });

    addDisjunctionConstraint(constraints, fixedLocator);
    return getTypeMatchSuccess();
  }

  // For a single potential conversion, directly recurse, so that we
  // don't allocate a new constraint or constraint locator.

  auto formTypeMatchResult = [&](SolutionKind kind) {
    switch (kind) {
      case SolutionKind::Error:
        return getTypeMatchFailure(locator);

      case SolutionKind::Solved:
        return getTypeMatchSuccess();

      case SolutionKind::Unsolved:
        return getTypeMatchAmbiguous();
    }
    llvm_unreachable("unhandled kind");
  };

  // Handle restrictions.
  if (auto restriction = conversionsOrFixes[0].getRestriction()) {
    return formTypeMatchResult(simplifyRestrictedConstraint(*restriction, type1,
                                                            type2, kind,
                                                            subflags, locator));
  }

  // Handle fixes.
  auto fix = *conversionsOrFixes[0].getFix();
  return formTypeMatchResult(simplifyFixConstraint(fix, type1, type2, kind,
                                                   subflags, locator));
}

ConstraintSystem::SolutionKind
ConstraintSystem::simplifyConstructionConstraint(
    Type valueType, FunctionType *fnType, TypeMatchOptions flags,
    DeclContext *useDC,
    FunctionRefKind functionRefKind, ConstraintLocator *locator) {

  // Desugar the value type.
  auto desugarValueType = valueType->getDesugaredType();

  switch (desugarValueType->getKind()) {
#define SUGARED_TYPE(id, parent) case TypeKind::id:
#define TYPE(id, parent)
#include "swift/AST/TypeNodes.def"
    llvm_unreachable("Type has not been desugared completely");

#define ARTIFICIAL_TYPE(id, parent) case TypeKind::id:
#define TYPE(id, parent)
#include "swift/AST/TypeNodes.def"
      llvm_unreachable("artificial type in constraint");

  case TypeKind::BuiltinTuple:
    llvm_unreachable("BuiltinTupleType in constraint");
    
  case TypeKind::Unresolved:
  case TypeKind::Error:
  case TypeKind::Placeholder:
    return SolutionKind::Error;

  case TypeKind::GenericFunction:
  case TypeKind::GenericTypeParam:
    llvm_unreachable("unmapped dependent type");

  case TypeKind::TypeVariable:
  case TypeKind::DependentMember:
    return SolutionKind::Unsolved;

  case TypeKind::Tuple: {
    // If this is an attempt to construct `Void` with arguments,
    // let's diagnose it.
    if (shouldAttemptFixes()) {
      if (valueType->isVoid() && fnType->getNumParams() > 0) {
        auto contextualType = FunctionType::get({}, fnType->getResult());
        if (fixExtraneousArguments(
                *this, contextualType, fnType->getParams(),
                fnType->getNumParams(),
                getConstraintLocator(locator,
                                     ConstraintLocator::FunctionArgument)))
          return SolutionKind::Error;

        fnType = contextualType;
      }
    }
    SmallVector<AnyFunctionType::Param, 4> args;
    for (auto idx : indices(fnType->getParams())) {
      auto &arg = fnType->getParams()[idx];

      // We can disregard '_const', it's not applicable for tuple construction.
      auto flags = arg.getParameterFlags().withCompileTimeConst(false);

      // We cannot handle inout for tuple construction.
      if (flags.isInOut()) {
        if (!shouldAttemptFixes())
          return SolutionKind::Error;

        auto *argLoc = getConstraintLocator(locator, {
          LocatorPathElt::ApplyArgument(),
          LocatorPathElt::ApplyArgToParam(idx, idx, ParameterTypeFlags())
        });
        auto argTy = arg.getParameterType();
        if (recordFix(RemoveAddressOf::create(*this, argTy, argTy, argLoc)))
          return SolutionKind::Error;

        flags = flags.withInOut(false);
      }
      args.push_back(arg.withFlags(flags));
    }

    // Tuple construction is simply tuple conversion. We should have already
    // handled the parameter flags. If any future parameter flags are added,
    // they should also be verified above.
    Type argType = AnyFunctionType::composeTuple(
        getASTContext(), args, ParameterFlagHandling::AssertEmpty);
    Type resultType = fnType->getResult();

    ConstraintLocatorBuilder builder(locator);
    if (matchTypes(resultType, desugarValueType, ConstraintKind::Bind, flags,
                   builder.withPathElement(ConstraintLocator::ApplyFunction))
            .isFailure())
      return SolutionKind::Error;

    return matchTypes(argType, valueType, ConstraintKind::Conversion,
                      getDefaultDecompositionOptions(flags), locator);
  }

  case TypeKind::Enum:
  case TypeKind::Struct:
  case TypeKind::Class:
  case TypeKind::BoundGenericClass:
  case TypeKind::BoundGenericEnum:
  case TypeKind::BoundGenericStruct:
  case TypeKind::PrimaryArchetype:
  case TypeKind::OpenedArchetype:
  case TypeKind::OpaqueTypeArchetype:
  case TypeKind::PackArchetype:
  case TypeKind::ElementArchetype:
  case TypeKind::DynamicSelf:
  case TypeKind::ProtocolComposition:
  case TypeKind::ParameterizedProtocol:
  case TypeKind::Protocol:
  case TypeKind::Existential:
    // Break out to handle the actual construction below.
    break;

  case TypeKind::UnboundGeneric:
    llvm_unreachable("Unbound generic type should have been opened");

#define BUILTIN_TYPE(id, parent) case TypeKind::id:
#define TYPE(id, parent)
#include "swift/AST/TypeNodes.def"
  case TypeKind::ExistentialMetatype:
  case TypeKind::Metatype:
  case TypeKind::Function:
  case TypeKind::LValue:
  case TypeKind::InOut:
  case TypeKind::Module:
  case TypeKind::Pack:
  case TypeKind::PackExpansion: {
    // If solver is in the diagnostic mode and this is an invalid base,
    // let's give solver a chance to repair it to produce a good diagnostic.
    if (shouldAttemptFixes())
      break;

    return SolutionKind::Error;
  }
  }

  auto fnLocator = getConstraintLocator(locator,
                                        ConstraintLocator::ApplyFunction);
  auto memberTypeLoc =
      getConstraintLocator(fnLocator, LocatorPathElt::ConstructorMemberType(
                                          /*shortFormOrSelfDelegating*/ true));

  auto memberType = createTypeVariable(memberTypeLoc, TVO_CanBindToNoEscape);

  // The constructor will have function type T -> T2, for a fresh type
  // variable T. T2 is the result type provided via the construction
  // constraint itself.
  addValueMemberConstraint(MetatypeType::get(valueType, getASTContext()),
                           DeclNameRef::createConstructor(),
                           memberType,
                           useDC, functionRefKind,
                           /*outerAlternatives=*/{},
                           getConstraintLocator(
                             fnLocator, 
                             ConstraintLocator::ConstructorMember));

  // HACK: Bind the function's parameter list as a tuple to a type variable.
  // This only exists to preserve compatibility with rdar://85263844, as it can
  // affect the prioritization of bindings, which can affect behavior for tuple
  // matching as tuple subtyping is currently a *weaker* constraint than tuple
  // conversion.
  if (!getASTContext().isSwiftVersionAtLeast(6)) {
    auto paramTypeVar = createTypeVariable(
        getConstraintLocator(locator, ConstraintLocator::ApplyArgument),
        TVO_CanBindToLValue | TVO_CanBindToInOut | TVO_CanBindToNoEscape |
            TVO_CanBindToPack);
    addConstraint(ConstraintKind::BindTupleOfFunctionParams, memberType,
                  paramTypeVar, locator);
  }

  addConstraint(ConstraintKind::ApplicableFunction, fnType, memberType,
                fnLocator);

  return SolutionKind::Solved;
}

ConstraintSystem::SolutionKind ConstraintSystem::simplifySubclassOfConstraint(
                                 Type type,
                                 Type classType,
                                 ConstraintLocatorBuilder locator,
                                 TypeMatchOptions flags) {
  if (!classType->getClassOrBoundGenericClass())
    return SolutionKind::Error;

  // Dig out the fixed type to which this type refers.
  type = getFixedTypeRecursive(type, flags, /*wantRValue=*/true);
  if (shouldAttemptFixes() && type->isPlaceholder()) {
    // If the type associated with this subclass check is a "hole" in the
    // constraint system, let's consider this check a success without recording
    // a fix, because it's just a consequence of the other failure, e.g.
    //
    // func foo<T: NSObject>(_: T) {}
    // foo(Foo.bar) <- if `Foo` doesn't have `bar` there is
    //                 no reason to complain the subclass.
    return SolutionKind::Solved;
  }

  auto formUnsolved = [&]() {
    // If we're supposed to generate constraints, do so.
    if (flags.contains(TMF_GenerateConstraints)) {
      auto *subclassOf = Constraint::create(
          *this, ConstraintKind::SubclassOf, type, classType,
          getConstraintLocator(locator));

      addUnsolvedConstraint(subclassOf);
      return SolutionKind::Solved;
    }

    return SolutionKind::Unsolved;
  };

  // If we hit a type variable without a fixed type, we can't
  // solve this yet.
  if (type->isTypeVariableOrMember())
    return formUnsolved();

  // SubclassOf constraints are generated when opening a generic
  // signature with a RequirementKind::Superclass requirement, so
  // we must handle pack types on the left by splitting up into
  // smaller constraints.
  if (auto *packType = type->getAs<PackType>()) {
    for (unsigned i = 0, e = packType->getNumElements(); i < e; ++i) {
      auto eltType = packType->getElementType(i);
      if (auto *packExpansionType = eltType->getAs<PackExpansionType>()) {
        auto patternLoc =
            locator.withPathElement(ConstraintLocator::PackExpansionPattern);
        addConstraint(ConstraintKind::SubclassOf, packExpansionType->getPatternType(),
                      classType, patternLoc);
      } else {
        addConstraint(ConstraintKind::SubclassOf, eltType,
                      classType, locator.withPathElement(LocatorPathElt::PackElement(i)));
      }
    }

    return SolutionKind::Solved;
  }

  // A class-constrained existential like 'C & P' does not satisfy an
  // AnyObject requirement, if 'P' is not self-conforming.
  //
  // While matchSuperclassTypes() will still match here because 'C & P'
  // satisfies a Subtype constraint with 'C', 'C & P' cannot satisfy a
  // superclass requirement in a generic signature, so rule that out here.
  if (type->satisfiesClassConstraint()) {
    // If we have an exact match of class declarations, ensure the
    // generic arguments match.
    if (type->getClassOrBoundGenericClass() ==
        classType->getClassOrBoundGenericClass()) {
      auto result = matchTypes(type, classType, ConstraintKind::Bind,
                               flags, locator);
      if (!result.isFailure())
        return SolutionKind::Solved;

    // Otherwise, ensure the left hand side is a proper subclass of the
    // right hand side.
    } else {
      auto result = matchSuperclassTypes(type, classType, flags, locator);
      if (!result.isFailure())
        return SolutionKind::Solved;
    }
  }

  // Record a fix if we didn't match one of the two cases above.
  if (shouldAttemptFixes()) {
    if (auto *fix = fixRequirementFailure(*this, type, classType, locator)) {
      if (recordFix(fix))
        return SolutionKind::Error;
      return SolutionKind::Solved;
    }
  }

  return SolutionKind::Error;
}

ConstraintSystem::SolutionKind ConstraintSystem::simplifyConformsToConstraint(
                                 Type type,
                                 Type protocol,
                                 ConstraintKind kind,
                                 ConstraintLocatorBuilder locator,
                                 TypeMatchOptions flags) {
  if (auto proto = protocol->getAs<ProtocolType>()) {
    return simplifyConformsToConstraint(type, proto->getDecl(), kind,
                                        locator, flags);
  }

  // Dig out the fixed type to which this type refers.
  type = getFixedTypeRecursive(type, flags, /*wantRValue=*/true);

  return matchExistentialTypes(type, protocol, kind, flags, locator);
}

ConstraintSystem::SolutionKind ConstraintSystem::simplifyConformsToConstraint(
                                 Type type,
                                 ProtocolDecl *protocol,
                                 ConstraintKind kind,
                                 ConstraintLocatorBuilder locator,
                                 TypeMatchOptions flags) {
  const auto rawType = type;
  auto *typeVar = type->getAs<TypeVariableType>();

  // Dig out the fixed type to which this type refers.
  type = getFixedTypeRecursive(type, flags, /*wantRValue=*/true);
  if (shouldAttemptFixes() && type->isPlaceholder()) {
    // If the type associated with this conformance check is a "hole" in the
    // constraint system, let's consider this check a success without recording
    // a fix, because it's just a consequence of the other failure, e.g.
    //
    // func foo<T: BinaryInteger>(_: T) {}
    // foo(Foo.bar) <- if `Foo` doesn't have `bar` there is
    //                 no reason to complain about missing conformance.
    return SolutionKind::Solved;
  }

  auto formUnsolved = [&](bool activate = false) {
    // If we're supposed to generate constraints, do so.
    if (flags.contains(TMF_GenerateConstraints)) {
      auto *conformance = Constraint::create(
          *this, kind, type, protocol->getDeclaredInterfaceType(),
          getConstraintLocator(locator));

      addUnsolvedConstraint(conformance);
      if (activate)
        activateConstraint(conformance);

      return SolutionKind::Solved;
    }

    return SolutionKind::Unsolved;
  };

  // If we hit a type variable without a fixed type, we can't
  // solve this yet.
  if (type->isTypeVariableOrMember())
    return formUnsolved();

  // ConformsTo constraints are generated when opening a generic
  // signature with a RequirementKind::Conformance requirement, so
  // we must handle pack types on the left by splitting up into
  // smaller constraints.
  if (auto *packType = type->getAs<PackType>()) {
    for (unsigned i = 0, e = packType->getNumElements(); i < e; ++i) {
      auto eltType = packType->getElementType(i);
      if (auto *packExpansionType = eltType->getAs<PackExpansionType>()) {
        auto patternLoc =
            locator.withPathElement(ConstraintLocator::PackExpansionPattern);
        addConstraint(ConstraintKind::ConformsTo,
                      packExpansionType->getPatternType(),
                      protocol->getDeclaredInterfaceType(),
                      patternLoc);
      } else {
        addConstraint(ConstraintKind::ConformsTo, eltType,
                      protocol->getDeclaredInterfaceType(),
                      locator.withPathElement(LocatorPathElt::PackElement(i)));
      }
    }

    return SolutionKind::Solved;
  }

  // Copyable is checked structurally, so for better performance, split apart
  // this constraint into individual Copyable constraints on each tuple element.
  if (auto *tupleType = type->getAs<TupleType>()) {
    if (protocol->isSpecificProtocol(KnownProtocolKind::Copyable)) {
      for (unsigned i = 0, e = tupleType->getNumElements(); i < e; ++i) {
        addConstraint(ConstraintKind::ConformsTo,
                      tupleType->getElementType(i),
                      protocol->getDeclaredInterfaceType(),
                      locator.withPathElement(LocatorPathElt::TupleElement(i)));
      }

      return SolutionKind::Solved;
    }
  }

  auto *loc = getConstraintLocator(locator);

  /// Record the given conformance as the result, adding any conditional
  /// requirements if necessary.
  auto recordConformance = [&](ProtocolConformanceRef conformance) {
    if (isConformanceUnavailable(conformance, loc))
      increaseScore(SK_Unavailable);

    // This conformance may be conditional, in which case we need to consider
    // those requirements as constraints too.
    if (conformance.isConcrete()) {
      unsigned index = 0;
      auto *conformanceLoc = getConstraintLocator(
          loc,
          LocatorPathElt::ConformanceRequirement(conformance.getConcrete()));

      for (const auto &req : conformance.getConditionalRequirements()) {
        addConstraint(
            req, getConstraintLocator(conformanceLoc,
                                      LocatorPathElt::ConditionalRequirement(
                                          index++, req.getKind())));
      }
    }

    return SolutionKind::Solved;
  };

  // For purposes of argument type matching, existential types don't need to
  // conform -- they only need to contain the protocol, so check that
  // separately.
  switch (kind) {
  case ConstraintKind::SelfObjectOfProtocol: {
    auto conformance = TypeChecker::containsProtocol(
        type, protocol, DC->getParentModule(),
        /*skipConditionalRequirements=*/true,
        /*allowMissing=*/true);
    if (conformance) {
      return recordConformance(conformance);
    }
  } break;
  case ConstraintKind::ConformsTo:
  case ConstraintKind::LiteralConformsTo: {
    // If existential type is used as a for-in sequence, let's open it
    // and check whether underlying type conforms to `Sequence`.
    if (type->isExistentialType()) {
      if (auto elt = loc->getLastElementAs<LocatorPathElt::ContextualType>()) {
        if (elt->getPurpose() == CTP_ForEachSequence) {
          type = openExistentialType(type, loc).first;
        }
      }
    }

    // Check whether this type conforms to the protocol.
    auto conformance = DC->getParentModule()->lookupConformance(
        type, protocol, /*allowMissing=*/true);
    if (conformance) {
      return recordConformance(conformance);
    }
  } break;

  default:
    llvm_unreachable("bad constraint kind");
  }
  
  if (!shouldAttemptFixes())
    return SolutionKind::Error;

  auto protocolTy = protocol->getDeclaredInterfaceType();

  // If this conformance has been fixed already, let's just consider this done.
  if (isFixedRequirement(loc, protocolTy))
    return SolutionKind::Solved;

  // If this is a generic requirement let's try to record that
  // conformance is missing and consider this a success, which
  // makes it much easier to diagnose problems like that.
  {
    SmallVector<LocatorPathElt, 4> path;
    auto anchor = locator.getLocatorParts(path);

    // If this is a `nil` literal, it would be a contextual failure.
    if (auto *Nil = getAsExpr<NilLiteralExpr>(anchor)) {
      auto *fixLocator = getConstraintLocator(
          getContextualType(Nil, /*forConstraint=*/false)
              ? locator.withPathElement(LocatorPathElt::ContextualType(
                    getContextualTypePurpose(Nil)))
              : locator);

      // Only requirement placed directly on `nil` literal is
      // `ExpressibleByNilLiteral`, so if `nil` is an argument
      // to an application, let's update locator accordingly to
      // diagnose possible ambiguities with multiple mismatched
      // overload choices.
      if (fixLocator->directlyAt<NilLiteralExpr>()) {
        if (auto *loc = getArgumentLocator(castToExpr(fixLocator->getAnchor())))
          fixLocator = loc;
      }

      // Here the roles are reversed - `nil` is something we are trying to
      // convert to `type` by making sure that it conforms to a specific
      // protocol.
      auto *fix =
          ContextualMismatch::create(*this, protocolTy, type, fixLocator);

      return recordFix(fix) ? SolutionKind::Error : SolutionKind::Solved;
    }

    // If there is a missing conformance between source and destination
    // of the assignment, let's ignore current the types and instead use
    // source/destination types directly to make it possible to diagnose
    // protocol compositions.
    if (auto *assignment = getAsExpr<AssignExpr>(anchor)) {
      // If the locator's last element points to the function result,
      // let's check whether there is a problem with function argument
      // as well, and if so, avoid producing a fix here, because
      // contextual mismatch mentions the source/destination
      // types of the assignment.
      if (locator.endsWith<LocatorPathElt::FunctionResult>() &&
          hasFixFor(
              getConstraintLocator(anchor, LocatorPathElt::FunctionArgument())))
        return SolutionKind::Solved;

      auto srcType = getType(assignment->getSrc());
      auto dstType = getType(assignment->getDest());

      auto *fix = IgnoreAssignmentDestinationType::create(
          *this, srcType, dstType, loc);
      return recordFix(fix) ? SolutionKind::Error : SolutionKind::Solved;
    }

    if (path.empty())
      return SolutionKind::Error;

    // If this is a conformance failure related to a contextual type
    // let's record it as a "contextual mismatch" because diagnostic
    // is going to be dependent on other contextual information.
    if (path.back().is<LocatorPathElt::ContextualType>()) {
      auto *fix = ContextualMismatch::create(*this, type, protocolTy, loc);
      return recordFix(fix) ? SolutionKind::Error : SolutionKind::Solved;
    }

    // If we have something like ... -> type req # -> pack element #, we're
    // solving a requirement of the form T : P where T is a type parameter pack
    if (path.back().is<LocatorPathElt::PackElement>())
      path.pop_back();

    // This is similar to `PackElement` but locator points to the requirement
    // associted with pack expansion pattern (i.e. `repeat each T: P`) where
    // the path is something like:
    // `... -> type req # -> pack expansion pattern`.
    if (path.back().is<LocatorPathElt::PackExpansionPattern>())
      path.pop_back();

    if (auto req = path.back().getAs<LocatorPathElt::AnyRequirement>()) {
      // If this is a requirement associated with `Self` which is bound
      // to `Any`, let's consider this "too incorrect" to continue.
      //
      // This helps us to filter out cases like operator overloads where
      // `Self` type comes from e.g. default for collection element -
      // `[1, "hello"].map { $0 + 1 }`. Main problem here is that
      // collection type couldn't be determined without unification to
      // `Any` and `+` failing for all numeric overloads is just a consequence.
      if (typeVar && type->isAny()) {
        if (auto *GP = typeVar->getImpl().getGenericParameter()) {
          if (auto *GPD = GP->getDecl()) {
            auto *DC = GPD->getDeclContext();
            if (DC->isTypeContext() && DC->getSelfInterfaceType()->isEqual(GP))
              return SolutionKind::Error;
          }
        }
      }

      if (auto rawValue = isRawRepresentable(*this, type)) {
        if (!rawValue->isTypeVariableOrMember() &&
            TypeChecker::conformsToProtocol(rawValue, protocol,
                                            DC->getParentModule())) {
          auto *fix = UseRawValue::create(*this, type, protocolTy, loc);
          // Since this is a conformance requirement failure (where the
          // source is most likely an argument), let's increase its impact
          // to disambiguate vs. conversion failure of the same kind.
          return recordFix(fix, /*impact=*/2) ? SolutionKind::Error
                                              : SolutionKind::Solved;
        }
      }

      auto anchor = locator.getAnchor();

      if (isExpr<UnresolvedMemberExpr>(anchor) &&
          req->is<LocatorPathElt::TypeParameterRequirement>()) {
        auto signature = path[path.size() - 2]
                             .castTo<LocatorPathElt::OpenedGeneric>()
                             .getSignature();
        auto requirement = signature.getRequirements()[req->getIndex()];

        auto *memberLoc = getConstraintLocator(anchor, path.front());
        auto overload = findSelectedOverloadFor(memberLoc);

        // To figure out what is going on here we need to wait until
        // member overload is set in the constraint system.
        if (!overload) {
          // If it's not allowed to generate new constraints
          // there is no way to control re-activation, so this
          // check has to fail.
          if (!flags.contains(TMF_GenerateConstraints))
            return SolutionKind::Error;

          return formUnsolved(/*activate=*/true);
        }

        auto *memberRef = overload->choice.getDeclOrNull();
        if (!memberRef)
          return SolutionKind::Error;

        // If this is a `Self` conformance requirement from a static member
        // reference on a protocol metatype, let's produce a tailored diagnostic.
        if (memberRef->isStatic()) {
          if (hasFixFor(memberLoc,
                        FixKind::AllowInvalidStaticMemberRefOnProtocolMetatype))
            return SolutionKind::Solved;

          if (auto *protocolDecl =
                  memberRef->getDeclContext()->getSelfProtocolDecl()) {
            auto selfTy = protocolDecl->getProtocolSelfType();
            if (selfTy->isEqual(requirement.getFirstType())) {
              auto *fix = AllowInvalidStaticMemberRefOnProtocolMetatype::create(
                *this, memberLoc);
              return recordFix(fix) ? SolutionKind::Error : SolutionKind::Solved;
            }
          }
        }
      }

      if (auto *fix =
              fixRequirementFailure(*this, type, protocolTy, anchor, path)) {
        auto impact = assessRequirementFailureImpact(*this, rawType, locator);
        if (!recordFix(fix, impact)) {
          // Record this conformance requirement as "fixed".
          recordFixedRequirement(getConstraintLocator(anchor, path),
                                 protocolTy);
          return SolutionKind::Solved;
        }
      }
    }

    if (loc->isLastElement<LocatorPathElt::MemberRefBase>()) {
      auto *fix = ContextualMismatch::create(*this, protocolTy, type, loc);
      if (!recordFix(fix))
        return SolutionKind::Solved;
    }

    // Conformance constraint that is introduced by an implicit conversion
    // for example to `AnyHashable`.
    if (kind == ConstraintKind::ConformsTo &&
        loc->isLastElement<LocatorPathElt::ApplyArgToParam>()) {
      auto *fix = AllowArgumentMismatch::create(*this, type, protocolTy, loc);
      return recordFix(fix, /*impact=*/2) ? SolutionKind::Error
                                          : SolutionKind::Solved;
    }

    // If this is an implicit Hashable conformance check generated for each
    // index argument of the keypath subscript component, we could just treat
    // it as though it conforms.
    if (loc->isResultOfKeyPathDynamicMemberLookup() ||
        loc->isKeyPathSubscriptComponent()) {
      if (protocol ==
          getASTContext().getProtocol(KnownProtocolKind::Hashable)) {
        auto *fix =
            TreatKeyPathSubscriptIndexAsHashable::create(*this, type, loc);
        if (!recordFix(fix))
          return SolutionKind::Solved;
      }
    }

    // If this is a failure to conform to Copyable, tailor the error message.
    if (protocol->isSpecificProtocol(KnownProtocolKind::Copyable)) {
      auto *fix =
          MustBeCopyable::create(*this, type, getConstraintLocator(locator));
      if (!recordFix(fix))
        return SolutionKind::Solved;
    }
  }

  // There's nothing more we can do; fail.
  return SolutionKind::Error;
}

ConstraintSystem::SolutionKind ConstraintSystem::simplifyTransitivelyConformsTo(
    Type type, Type protocolTy, ConstraintLocatorBuilder locator,
    TypeMatchOptions flags) {
  auto &ctx = getASTContext();

  // Since this is a performance optimization, let's ignore it
  // in diagnostic mode.
  if (shouldAttemptFixes())
    return SolutionKind::Solved;

  auto formUnsolved = [&]() {
    // If we're supposed to generate constraints, do so.
    if (flags.contains(TMF_GenerateConstraints)) {
      auto *conformance =
          Constraint::create(*this, ConstraintKind::TransitivelyConformsTo,
                             type, protocolTy, getConstraintLocator(locator));

      addUnsolvedConstraint(conformance);
      return SolutionKind::Solved;
    }

    return SolutionKind::Unsolved;
  };

  auto resolvedTy = getFixedTypeRecursive(type, /*wantRValue=*/true);
  if (resolvedTy->isTypeVariableOrMember())
    return formUnsolved();

  // If the composition consists of a class + protocol,
  // we can't check conformance of the argument because
  // parameter could pick one of the components.
  if (resolvedTy.findIf(
          [](Type type) { return type->is<ProtocolCompositionType>(); }))
    return SolutionKind::Solved;

  // All bets are off for pointers, there are multiple combinations
  // to check and it doesn't see worth to do that upfront.
  {
    PointerTypeKind pointerKind;
    if (resolvedTy->getAnyPointerElementType(pointerKind))
      return SolutionKind::Solved;
  }

  auto *protocol = protocolTy->castTo<ProtocolType>()->getDecl();

  auto *M = DC->getParentModule();

  // First, let's check whether the type itself conforms,
  // if it does - we are done.
  if (M->lookupConformance(resolvedTy, protocol))
    return SolutionKind::Solved;

  // If the type doesn't conform, let's check whether
  // an Optional or Unsafe{Mutable}Pointer from it would.

  // If the current score is equal to the best score, fail without checking
  // implicit conversions, because an implicit conversion would lead to a
  // worse score anyway.
  if (solverState && solverState->BestScore && CurrentScore == *solverState->BestScore)
    return SolutionKind::Error;

  SmallVector<Type, 4> typesToCheck;

  // T -> Optional<T>
  if (!resolvedTy->getOptionalObjectType())
    typesToCheck.push_back(OptionalType::get(resolvedTy));

  // AnyHashable
  if (auto *anyHashable = ctx.getAnyHashableDecl())
    typesToCheck.push_back(anyHashable->getDeclaredInterfaceType());

  // Rest of the implicit conversions depend on the resolved type.
  {
    auto getPointerFor = [&ctx](PointerTypeKind ptrKind,
                                Optional<Type> elementTy = None) -> Type {
      switch (ptrKind) {
      case PTK_UnsafePointer:
        assert(elementTy);
        return BoundGenericType::get(ctx.getUnsafePointerDecl(),
                                     /*parent=*/Type(), {*elementTy});
      case PTK_UnsafeMutablePointer:
        assert(elementTy);
        return BoundGenericType::get(ctx.getUnsafeMutablePointerDecl(),
                                     /*parent=*/Type(), {*elementTy});

      case PTK_UnsafeRawPointer:
        return ctx.getUnsafeRawPointerDecl()->getDeclaredInterfaceType();

      case PTK_UnsafeMutableRawPointer:
        return ctx.getUnsafeMutableRawPointerDecl()->getDeclaredInterfaceType();

      case PTK_AutoreleasingUnsafeMutablePointer:
        llvm_unreachable("no implicit conversion");
      }
    };

    // String -> UnsafePointer<Void>
    if (auto *string = ctx.getStringDecl()) {
      if (resolvedTy->isEqual(string->getDeclaredInterfaceType())) {
        typesToCheck.push_back(
            getPointerFor(PTK_UnsafePointer, ctx.TheEmptyTupleType));
      }
    }

    // Array<T> -> Unsafe{Raw}Pointer<T>
    if (auto elt = resolvedTy->isArrayType()) {
      typesToCheck.push_back(getPointerFor(PTK_UnsafePointer, elt));
      typesToCheck.push_back(getPointerFor(PTK_UnsafeRawPointer, elt));
    }

    // inout argument -> UnsafePointer<T>, UnsafeMutablePointer<T>,
    //                   UnsafeRawPointer, UnsafeMutableRawPointer.
    if (type->is<InOutType>()) {
      typesToCheck.push_back(getPointerFor(PTK_UnsafePointer, resolvedTy));
      typesToCheck.push_back(getPointerFor(PTK_UnsafeMutablePointer, resolvedTy));
      typesToCheck.push_back(getPointerFor(PTK_UnsafeRawPointer));
      typesToCheck.push_back(getPointerFor(PTK_UnsafeMutableRawPointer));
    }
  }

  return llvm::any_of(typesToCheck,
                      [&](Type type) {
                        return bool(M->lookupConformance(type, protocol));
                      })
             ? SolutionKind::Solved
             : SolutionKind::Error;
}

/// Determine the kind of checked cast to perform from the given type to
/// the given type.
///
/// This routine does not attempt to check whether the cast can actually
/// succeed; that's the caller's responsibility.
static CheckedCastKind getCheckedCastKind(ConstraintSystem *cs,
                                          Type fromType,
                                          Type toType) {
  // Array downcasts are handled specially.
  if (fromType->isArrayType() && toType->isArrayType()) {
    return CheckedCastKind::ArrayDowncast;
  }

  // Dictionary downcasts are handled specially.
  if (cs->isDictionaryType(fromType) && cs->isDictionaryType(toType)) {
    return CheckedCastKind::DictionaryDowncast;
  }

  // Set downcasts are handled specially.
  if (cs->isSetType(fromType) && cs->isSetType(toType)) {
    return CheckedCastKind::SetDowncast;
  }

  return CheckedCastKind::ValueCast;
}

// Optional types always conform to `ExpressibleByNilLiteral`.
static bool isCastToExpressibleByNilLiteral(ConstraintSystem &cs, Type fromType,
                                            Type toType) {
  auto &ctx = cs.getASTContext();
  auto *nilLiteral = TypeChecker::getProtocol(
      ctx, SourceLoc(), KnownProtocolKind::ExpressibleByNilLiteral);
  if (!nilLiteral)
    return false;

  return toType->isEqual(nilLiteral->getDeclaredExistentialType()) &&
         fromType->getOptionalObjectType();
}

static ConstraintFix *maybeWarnAboutExtraneousCast(
    ConstraintSystem &cs, Type origFromType, Type origToType, Type fromType,
    Type toType, SmallVector<Type, 4> fromOptionals,
    SmallVector<Type, 4> toOptionals,
    ConstraintSystem::TypeMatchOptions flags,
    ConstraintLocatorBuilder locator) {
  if (locator.endsWith<LocatorPathElt::GenericArgument>())
    return nullptr;

  // Both types have to be fixed.
  if (fromType->hasTypeVariable() || toType->hasTypeVariable() ||
      fromType->hasPlaceholder() || toType->hasPlaceholder())
    return nullptr;

  SmallVector<LocatorPathElt, 4> path;
  auto anchor = locator.getLocatorParts(path);

  auto *castExpr = getAsExpr<ExplicitCastExpr>(anchor);
  if (!castExpr)
    return nullptr;

  // "from" could be less optional than "to" e.g. `0 as Any?`, so
  // we need to store the difference as a signed integer.
  int extraOptionals = fromOptionals.size() - toOptionals.size();

  // "from" expression could be a type variable wrapped in an optional e.g.
  // Optional<$T0>. So when that is the case we have to add this additional
  // optionality levels to from type.
  const auto subExprType = cs.getType(castExpr->getSubExpr());
  if (subExprType->getOptionalObjectType()) {
    SmallVector<Type, 4> subExprOptionals;
    const auto unwrappedSubExprType =
        subExprType->lookThroughAllOptionalTypes(subExprOptionals);
    if (unwrappedSubExprType->is<TypeVariableType>()) {
      extraOptionals += subExprOptionals.size();
      for (size_t i = 0; i != subExprOptionals.size(); ++i) {
        origFromType = OptionalType::get(origFromType);
      }
    }
  }

  // Removing the optionality from to type when the force cast expr is an IUO.
  const auto *const TR = castExpr->getCastTypeRepr();
  if (isExpr<ForcedCheckedCastExpr>(anchor) && TR &&
      TR->getKind() == TypeReprKind::ImplicitlyUnwrappedOptional) {
    extraOptionals++;
  }

  // In cases of 'try?' where origFromType isn't optional that meaning
  // sub-expression isn't optional, always adds one level of optionality
  // because the result of the expression is always an optional type
  // regardless of language mode.
  auto *sub = castExpr->getSubExpr()->getSemanticsProvidingExpr();
  if (isExpr<OptionalTryExpr>(sub) && !origFromType->getOptionalObjectType()) {
    origFromType = OptionalType::get(fromType);
    extraOptionals++;
  }

  // Except for forced cast expressions, if optionals are more than a single
  // level difference or there is a single level between the types but an extra
  // level of optional is added to subexpr via OptionalEvaluationExpr, we don't
  // need to record any fix.
  if (!isExpr<ForcedCheckedCastExpr>(anchor) &&
      (extraOptionals > 1 ||
       isExpr<OptionalEvaluationExpr>(castExpr->getSubExpr())))
    return nullptr;

  // Always succeed
  if (isCastToExpressibleByNilLiteral(cs, origFromType, toType)) {
    return AllowNoopCheckedCast::create(cs, fromType, toType,
                                        CheckedCastKind::Coercion,
                                        cs.getConstraintLocator(locator));
  }

  // If both original are metatypes we have to use them because most of the
  // logic on how correctly handle metatypes casting is on
  // typeCheckCheckedCast.
  if (origFromType->is<AnyMetatypeType>() &&
      origToType->is<AnyMetatypeType>()) {
    fromType = origFromType;
    toType = origToType;
  }

  auto castKind = TypeChecker::typeCheckCheckedCast(
      fromType, toType, CheckedCastContextKind::None, cs.DC);

  if (castKind == CheckedCastKind::Unresolved) {
    return AllowCheckedCastToUnrelated::attempt(
        cs, origFromType, origToType, castKind,
        cs.getConstraintLocator(locator));
  }

  if (castKind == CheckedCastKind::ValueCast) {
    // https://github.com/apple/swift/issues/44221
    // Special 'is' case diagnostics for CFTypes.
    return AllowNoopExistentialToCFTypeCheckedCast::attempt(
        cs, origFromType, origToType, castKind,
        cs.getConstraintLocator(locator));
  }

  if (!(castKind == CheckedCastKind::Coercion ||
        castKind == CheckedCastKind::BridgingCoercion))
    return nullptr;

  if (auto *fix = AllowUnsupportedRuntimeCheckedCast::attempt(
          cs, fromType, toType, castKind, cs.getConstraintLocator(locator))) {
    return fix;
  }
  if (extraOptionals > 0) {
    // Conditional cast in this case can be an attempt to just unwrap a nil
    // value.
    if (isExpr<ConditionalCheckedCastExpr>(anchor) &&
        castKind == CheckedCastKind::BridgingCoercion) {
      return nullptr;
    }

    return AllowCheckedCastCoercibleOptionalType::create(
        cs, origFromType, origToType, castKind,
        cs.getConstraintLocator(locator));
  } else {
    // No optionals, just a trivial cast that always succeeds.
    return AllowNoopCheckedCast::create(cs, origFromType, origToType, castKind,
                                        cs.getConstraintLocator(locator));
  }
}

ConstraintSystem::SolutionKind
ConstraintSystem::simplifyCheckedCastConstraint(
                    Type fromType, Type toType,
                    TypeMatchOptions flags,
                    ConstraintLocatorBuilder locator) {
  TypeMatchOptions subflags = getDefaultDecompositionOptions(flags);

  /// Form an unresolved result.
  auto formUnsolved = [&] {
    if (flags.contains(TMF_GenerateConstraints)) {
      addUnsolvedConstraint(
        Constraint::create(*this, ConstraintKind::CheckedCast, fromType,
                           toType, getConstraintLocator(locator)));
      return SolutionKind::Solved;
    }

    return SolutionKind::Unsolved;
  };

  Type origFromType =
      getFixedTypeRecursive(fromType, flags, /*wantRValue=*/true);
  Type origToType = getFixedTypeRecursive(toType, flags, /*wantRValue=*/true);

  SmallVector<Type, 4> fromOptionals;
  SmallVector<Type, 4> toOptionals;

  do {
    // Dig out the fixed type this type refers to.
    fromType = getFixedTypeRecursive(fromType, flags, /*wantRValue=*/true);

    // If we hit a type variable without a fixed type, we can't
    // solve this yet.
    if (fromType->isTypeVariableOrMember())
      return formUnsolved();

    // Dig out the fixed type this type refers to.
    toType = getFixedTypeRecursive(toType, flags, /*wantRValue=*/true);

    // If we hit a type variable without a fixed type, we can't
    // solve this yet.
    if (toType->isTypeVariableOrMember())
      return formUnsolved();

    Type origFromType = fromType;
    Type origToType = toType;

    // Peel off optionals metatypes from the types, because we might cast through
    // them.
    toType = toType->lookThroughAllOptionalTypes(toOptionals);
    fromType = fromType->lookThroughAllOptionalTypes(fromOptionals);

    // Peel off metatypes, since if we can cast two types, we can cast their
    // metatypes.
    while (auto toMetatype = toType->getAs<MetatypeType>()) {
      auto fromMetatype = fromType->getAs<MetatypeType>();
      if (!fromMetatype)
        break;
      toType = toMetatype->getInstanceType();
      fromType = fromMetatype->getInstanceType();
    }

    // Peel off a potential layer of existential<->concrete metatype conversion.
    if (auto toMetatype = toType->getAs<AnyMetatypeType>()) {
      if (auto fromMetatype = fromType->getAs<MetatypeType>()) {
        toType = toMetatype->getInstanceType();
        fromType = fromMetatype->getInstanceType();
      }
    }

    // We've decomposed the types further, so adopt the subflags.
    flags = subflags;

    // If nothing changed, we're done.
    if (fromType.getPointer() == origFromType.getPointer() &&
        toType.getPointer() == origToType.getPointer())
      break;
  } while (true);

  auto attemptRecordCastFixIfSolved = [&](SolutionKind result) {
    if (result != SolutionKind::Solved)
      return;

    if (auto *fix = maybeWarnAboutExtraneousCast(
            *this, origFromType, origToType, fromType, toType, fromOptionals,
            toOptionals, flags, locator)) {
      (void)recordFix(fix);
    }
  };

  auto kind = getCheckedCastKind(this, fromType, toType);
  switch (kind) {
  case CheckedCastKind::ArrayDowncast: {
    auto fromBaseType = fromType->isArrayType();
    auto toBaseType = toType->isArrayType();

    auto elementLocator =
        locator.withPathElement(LocatorPathElt::GenericArgument(0));
    auto result = simplifyCheckedCastConstraint(fromBaseType, toBaseType,
                                                subflags, elementLocator);
    attemptRecordCastFixIfSolved(result);
    return result;
  }

  case CheckedCastKind::DictionaryDowncast: {
    Type fromKeyType, fromValueType;
    std::tie(fromKeyType, fromValueType) = *isDictionaryType(fromType);

    Type toKeyType, toValueType;
    std::tie(toKeyType, toValueType) = *isDictionaryType(toType);

    auto keyLocator =
        locator.withPathElement(LocatorPathElt::GenericArgument(0));
    if (simplifyCheckedCastConstraint(fromKeyType, toKeyType, subflags,
                                      keyLocator) == SolutionKind::Error)
      return SolutionKind::Error;

    auto valueLocator =
        locator.withPathElement(LocatorPathElt::GenericArgument(1));
    auto result = simplifyCheckedCastConstraint(fromValueType, toValueType,
                                                subflags, valueLocator);
    attemptRecordCastFixIfSolved(result);
    return result;
  }

  case CheckedCastKind::SetDowncast: {
    auto fromBaseType = *isSetType(fromType);
    auto toBaseType = *isSetType(toType);
    
    auto elementLocator =
        locator.withPathElement(LocatorPathElt::GenericArgument(0));
    auto result = simplifyCheckedCastConstraint(fromBaseType, toBaseType,
                                                subflags, elementLocator);
    attemptRecordCastFixIfSolved(result);
    return result;
  }

  case CheckedCastKind::ValueCast: {
    // If casting among classes, and there are open
    // type variables remaining, introduce a subtype constraint to help resolve
    // them.
    if (fromType->getClassOrBoundGenericClass()
        && toType->getClassOrBoundGenericClass()
        && (fromType->hasTypeVariable() || toType->hasTypeVariable())) {
      addConstraint(ConstraintKind::Subtype, toType, fromType,
                    getConstraintLocator(locator));
    }

    // Attempts to record warning fixes when both types are known by the
    // compiler and we can infer that the runtime checked cast will always
    // succeed or fail.
    if (auto *fix = maybeWarnAboutExtraneousCast(
            *this, origFromType, origToType, fromType, toType, fromOptionals,
            toOptionals, flags, locator)) {
      (void)recordFix(fix);
    }

    return SolutionKind::Solved;
  }

  case CheckedCastKind::Coercion:
  case CheckedCastKind::BridgingCoercion:
  case CheckedCastKind::Unresolved:
    llvm_unreachable("Not a valid result");
  }

  llvm_unreachable("Unhandled CheckedCastKind in switch.");
}

ConstraintSystem::SolutionKind
ConstraintSystem::simplifyOptionalObjectConstraint(
                                           Type first, Type second,
                                           TypeMatchOptions flags,
                                           ConstraintLocatorBuilder locator) {
  // Resolve the optional type.
  Type optLValueTy = getFixedTypeRecursive(first, flags, /*wantRValue=*/false);
  Type optTy = optLValueTy->getRValueType();
  if (optTy.getPointer() != optLValueTy.getPointer())
    optTy = getFixedTypeRecursive(optTy, /*wantRValue=*/false);

  if (optTy->isTypeVariableOrMember()) {
    if (flags.contains(TMF_GenerateConstraints)) {
      addUnsolvedConstraint(
        Constraint::create(*this, ConstraintKind::OptionalObject, optLValueTy,
                           second, getConstraintLocator(locator)));
      return SolutionKind::Solved;
    }

    return SolutionKind::Unsolved;
  }

  if (optTy->isPlaceholder()) {
    if (auto *typeVar = second->getAs<TypeVariableType>())
      recordPotentialHole(typeVar);
    return SolutionKind::Solved;
  }

  Type objectTy = optTy->getOptionalObjectType();
  // If the base type is not optional, let's attempt a fix (if possible)
  // and assume that `!` is just not there.
  if (!objectTy) {
    if (!shouldAttemptFixes())
      return SolutionKind::Error;
    
    // Let's see if we can apply a specific fix here.
    if (optTy->isPlaceholder())
      return SolutionKind::Solved;
    
    auto fnType = optTy->getAs<FunctionType>();
    if (fnType && fnType->getNumParams() == 0) {
      // For function types with no parameters, let's try to
      // offer a "make it a call" fix if possible.
      auto optionalResultType = fnType->getResult()->getOptionalObjectType();
      if (optionalResultType) {
        if (matchTypes(optionalResultType, second, ConstraintKind::Bind,
                       flags | TMF_ApplyingFix, locator)
                .isSuccess()) {
          auto *fix =
              InsertExplicitCall::create(*this, getConstraintLocator(locator));

          return recordFix(fix) ? SolutionKind::Error : SolutionKind::Solved;
        }
      }
    }

    auto *fix =
        RemoveUnwrap::create(*this, optTy, getConstraintLocator(locator));

    if (recordFix(fix))
      return SolutionKind::Error;

    // If the fix was successful let's record
    // "fixed" object type and continue.
    objectTy = optTy;
  }

  // The object type is an lvalue if the optional was.
  if (optLValueTy->is<LValueType>())
    objectTy = LValueType::get(objectTy);

  // Equate it to the other type in the constraint.
  addConstraint(ConstraintKind::Bind, objectTy, second, locator);
  return SolutionKind::Solved;
}

ConstraintSystem::SolutionKind
ConstraintSystem::simplifyBindTupleOfFunctionParamsConstraint(
    Type first, Type second, TypeMatchOptions flags,
    ConstraintLocatorBuilder locator) {
  auto simplified = simplifyType(first);
  auto simplifiedCopy = simplified;

  unsigned unwrapCount = 0;
  if (shouldAttemptFixes()) {
    while (auto objectTy = simplified->getOptionalObjectType()) {
      simplified = objectTy;

      // Track how many times we do this so that we can record a fix for each.
      ++unwrapCount;
    }

    if (simplified->isPlaceholder()) {
      if (auto *typeVar = second->getAs<TypeVariableType>())
        recordPotentialHole(typeVar);
      return SolutionKind::Solved;
    }
  }

  if (simplified->isTypeVariableOrMember()) {
    if (!flags.contains(TMF_GenerateConstraints))
      return SolutionKind::Unsolved;

    addUnsolvedConstraint(
        Constraint::create(*this, ConstraintKind::BindTupleOfFunctionParams,
                           simplified, second, getConstraintLocator(locator)));
    return SolutionKind::Solved;
  }

  auto *funcTy = simplified->getAs<FunctionType>();
  if (!funcTy)
    return SolutionKind::Error;

  auto tupleTy =
      AnyFunctionType::composeTuple(getASTContext(), funcTy->getParams(),
                                    ParameterFlagHandling::IgnoreNonEmpty);

  addConstraint(ConstraintKind::Bind, tupleTy, second,
                locator.withPathElement(ConstraintLocator::FunctionArgument));

  if (unwrapCount > 0) {
    auto *fix = ForceOptional::create(*this, simplifiedCopy, second,
                                      getConstraintLocator(locator));
    if (recordFix(fix, /*impact=*/unwrapCount))
      return SolutionKind::Error;
  }
  return SolutionKind::Solved;
}

ConstraintSystem::SolutionKind
ConstraintSystem::simplifyPackElementOfConstraint(Type first, Type second,
                                                  TypeMatchOptions flags,
                                                  ConstraintLocatorBuilder locator) {
  auto elementType = simplifyType(first, flags);
  auto patternType = simplifyType(second, flags);

  auto formUnsolved = [&]() {
    if (!flags.contains(TMF_GenerateConstraints))
      return SolutionKind::Unsolved;

    addUnsolvedConstraint(
        Constraint::create(*this, ConstraintKind::PackElementOf, first, second,
                           getConstraintLocator(locator)));

    return SolutionKind::Solved;
  };

  // If neither side is fully resolved yet, there is nothing we can do.
  if (elementType->hasTypeVariable() && patternType->hasTypeVariable())
    return formUnsolved();

  if (shouldAttemptFixes()) {
    if (elementType->isPlaceholder() || patternType->isPlaceholder())
      return SolutionKind::Solved;
  }

  // Let's try to resolve element type based on the pattern type.
  if (!patternType->hasTypeVariable()) {
    auto *loc = getConstraintLocator(locator);
    auto shapeClass = patternType->getReducedShape();
    patternType = patternType->mapTypeOutOfContext();
    auto *elementEnv = getPackElementEnvironment(loc, shapeClass);

    // Without an opened element environment, we cannot derive the
    // element binding.
    if (!elementEnv) {
      if (!shouldAttemptFixes())
        return SolutionKind::Error;

      // `each` was applied to a concrete type.
      if (!shapeClass->is<PackArchetypeType>()) {
        if (recordFix(AllowInvalidPackElement::create(*this, patternType, loc)))
          return SolutionKind::Error;
      }

      // Only other posibility is that there is a shape mismatch between
      // elements of the pack expansion pattern which is detected separately.

      recordAnyTypeVarAsPotentialHole(elementType);
      return SolutionKind::Solved;
    }

    auto expectedElementTy =
        elementEnv->mapPackTypeIntoElementContext(patternType);
    assert(!expectedElementTy->is<PackType>());

    addConstraint(ConstraintKind::Equal, elementType, expectedElementTy,
                  locator);
    return SolutionKind::Solved;
  }

  // Otherwise we are inferred or checking pattern type.

  auto *packEnv = DC->getGenericEnvironmentOfContext();

  // Map element archetypes to the pack context to check for equality.
  if (elementType->hasElementArchetype())
    elementType = packEnv->mapElementTypeIntoPackContext(elementType);

  addConstraint(ConstraintKind::Equal, elementType, patternType, locator);
  return SolutionKind::Solved;
}

static bool isForKeyPathSubscript(ConstraintSystem &cs,
                                  ConstraintLocator *locator) {
  if (!locator || !locator->getAnchor())
    return false;

  if (auto *SE = getAsExpr<SubscriptExpr>(locator->getAnchor())) {
    return SE->getArgs()->isUnary() &&
           SE->getArgs()->getLabel(0) == cs.getASTContext().Id_keyPath;
  }
  return false;
}

static bool mayBeForKeyPathSubscriptWithoutLabel(ConstraintSystem &cs,
                                                 ConstraintLocator *locator) {
  if (!locator || !locator->getAnchor())
    return false;

  if (auto *SE = getAsExpr<SubscriptExpr>(locator->getAnchor())) {
    if (auto *unary = SE->getArgs()->getUnlabeledUnaryExpr())
      return isa<KeyPathExpr>(unary) || isa<CodeCompletionExpr>(unary);
  }
  return false;
}

/// Determine whether all of the given candidate overloads
/// found through conditional conformances of a given base type.
/// This is useful to figure out whether it makes sense to
/// perform dynamic member lookup or not.
static bool
allFromConditionalConformances(DeclContext *DC, Type baseTy,
                               ArrayRef<OverloadChoice> candidates) {
  auto *NTD = baseTy->getAnyNominal();
  if (!NTD)
    return false;

  return llvm::all_of(candidates, [&](const OverloadChoice &choice) {
    auto *decl = choice.getDeclOrNull();
    if (!decl)
      return false;

    auto *candidateDC = decl->getDeclContext();

    if (auto *extension = dyn_cast<ExtensionDecl>(candidateDC)) {
      if (extension->isConstrainedExtension())
        return true;
    }

    if (auto *protocol = candidateDC->getSelfProtocolDecl()) {
      auto conformance = DC->getParentModule()->lookupConformance(
          baseTy, protocol);
      if (!conformance.isConcrete())
        return false;

      return !conformance.getConcrete()->getConditionalRequirements().empty();
    }

    return false;
  });
}

// Check whether given key path dynamic member lookup is self-recursive,
// which happens when root type of the key path is the same as base type
// of the member and lookup is attempted on non-existing property e.g.
//
// @dynamicMemberLookup
// struct Recurse<T> {
//   subscript<U>(dynamicMember member: KeyPath<Recurse<T>, U>) -> Int {
//     return 1
//   }
// }
//
// If we going to lookup any no-existent property or member on `Recursive`
// using key path dynamic member lookup it would attempt to lookup such
// member on root type which is also `Recursive` which leads to an infinite
// recursion.
static bool isSelfRecursiveKeyPathDynamicMemberLookup(
    ConstraintSystem &cs, Type keyPathRootTy, ConstraintLocator *locator) {
  // Let's check whether this is a recursive call to keypath
  // dynamic member lookup on the same type.
  if (!locator ||
      !locator->isLastElement<LocatorPathElt::KeyPathDynamicMember>())
    return false;

  auto path = locator->getPath();
  auto *choiceLoc =
      cs.getConstraintLocator(locator->getAnchor(), path.drop_back());

  if (auto overload = cs.findSelectedOverloadFor(choiceLoc)) {
    auto baseTy = overload->choice.getBaseType();

    // If it's `Foo<Int>` vs. `Foo<String>` it doesn't really matter
    // for dynamic lookup because it's going to be performed on `Foo`.
    if (baseTy->is<BoundGenericType>() &&
        keyPathRootTy->is<BoundGenericType>()) {
      auto *baseDecl = baseTy->castTo<BoundGenericType>()->getDecl();
      auto *keyPathRootDecl =
          keyPathRootTy->castTo<BoundGenericType>()->getDecl();
      return baseDecl == keyPathRootDecl;
    }

    // Previous base type could be r-value because that could be
    // a base type of subscript "as written" for which we attempt
    // a dynamic member lookup.
    auto baseTy1 = baseTy->getRValueType();
    // Root type of key path is always wrapped in an l-value
    // before lookup is performed, so we need to unwrap that.
    auto baseTy2 = keyPathRootTy->getRValueType();

    if (baseTy1->isEqual(baseTy2))
      return true;
  }

  return false;
}

/// Given a ValueMember, UnresolvedValueMember, or TypeMember constraint,
/// perform a lookup into the specified base type to find a candidate list.
/// The list returned includes the viable candidates as well as the unviable
/// ones (along with reasons why they aren't viable).
///
/// If includeInaccessibleMembers is set to true, this burns compile time to
/// try to identify and classify inaccessible members that may be being
/// referenced.
MemberLookupResult ConstraintSystem::
performMemberLookup(ConstraintKind constraintKind, DeclNameRef memberName,
                    Type baseTy, FunctionRefKind functionRefKind,
                    ConstraintLocator *memberLocator,
                    bool includeInaccessibleMembers) {
  Type baseObjTy = baseTy->getRValueType();
  Type instanceTy = baseObjTy;

  if (auto baseObjMeta = baseObjTy->getAs<AnyMetatypeType>()) {
    instanceTy = baseObjMeta->getInstanceType();
  }

  if (instanceTy->isTypeVariableOrMember() ||
      instanceTy->is<UnresolvedType>()) {
    MemberLookupResult result;
    result.OverallResult = MemberLookupResult::Unsolved;
    return result;
  }

  // Okay, start building up the result list.
  MemberLookupResult result;
  result.OverallResult = MemberLookupResult::HasResults;

  // Add key path result.
  // If we are including inaccessible members, check for the use of a keypath
  // subscript without a `keyPath:` label. Add it to the result so that it
  // can be caught by the missing argument label checking later.
  if (isForKeyPathSubscript(*this, memberLocator) ||
      (mayBeForKeyPathSubscriptWithoutLabel(*this, memberLocator) &&
       includeInaccessibleMembers)) {
    if (baseTy->isAnyObject()) {
      result.addUnviable(
          OverloadChoice(baseTy, OverloadChoiceKind::KeyPathApplication),
          MemberLookupResult::UR_KeyPathWithAnyObjectRootType);
    } else {
      result.ViableCandidates.push_back(
          OverloadChoice(baseTy, OverloadChoiceKind::KeyPathApplication));
    }
  }

  // If the base type is a tuple type, look for the named or indexed member
  // of the tuple.
  auto &ctx = getASTContext();
  if (auto baseTuple = baseObjTy->getAs<TupleType>()) {
    if (!memberName.isSpecial()) {
      StringRef nameStr = memberName.getBaseIdentifier().str();
      // Accessing `.element` on an abstract tuple materializes a pack.
      if (nameStr == "element" && baseTuple->getNumElements() == 1 &&
          isPackExpansionType(baseTuple->getElementType(0))) {
        auto elementType = baseTuple->getElementType(0);

        if (elementType->is<PackExpansionType>()) {
          result.ViableCandidates.push_back(
            OverloadChoice(baseTy, OverloadChoiceKind::MaterializePack));
        } else {
          assert(elementType->is<TypeVariableType>());
          result.OverallResult = MemberLookupResult::Unsolved;
        }
        return result;
      }

      int fieldIdx = -1;
      // Resolve a number reference into the tuple type.
      unsigned Value = 0;
      if (!nameStr.getAsInteger(10, Value) &&
          Value < baseTuple->getNumElements()) {
        fieldIdx = Value;
      } else {
        fieldIdx = baseTuple->getNamedElementId(memberName.getBaseIdentifier());
      }

      if (fieldIdx != -1) {
        // Add an overload set that selects this field.
        result.ViableCandidates.push_back(OverloadChoice(baseTy, fieldIdx));
        return result;
      }
    }
  }

  if (auto *selfTy = instanceTy->getAs<DynamicSelfType>())
    instanceTy = selfTy->getSelfType();

  if (!instanceTy->mayHaveMembers())
    return result;

  // If we have a simple name, determine whether there are argument
  // labels we can use to restrict the set of lookup results.
  if (baseObjTy->isAnyObject() && memberName.isSimpleName()) {
    // If we're referencing AnyObject and we have argument labels, put
    // the argument labels into the name: we don't want to look for
    // anything else, because the cost of the general search is so
    // high.
    if (auto *args = getArgumentList(memberLocator)) {
      SmallVector<Identifier, 4> scratch;
      memberName.getFullName() = DeclName(ctx, memberName.getBaseName(),
                                          args->getArgumentLabels(scratch));
    }
  }

  DeclNameRef lookupName = memberName;
  if (memberName.isCompoundName()) {
    auto &context = getASTContext();

    // Remove any $ prefixes for lookup
    SmallVector<Identifier, 4> lookupLabels;
    for (auto label : memberName.getArgumentNames()) {
      if (label.hasDollarPrefix()) {
        auto unprefixed = label.str().drop_front();
        lookupLabels.push_back(context.getIdentifier(unprefixed));
      } else {
        lookupLabels.push_back(label);
      }
    }

    DeclName unprefixedName(context, memberName.getBaseName(), lookupLabels);
    lookupName = DeclNameRef(unprefixedName);
  }

  // Look for members within the base.
  LookupResult &lookup = lookupMember(instanceTy, lookupName);

  // If this is true, we're using type construction syntax (Foo()) rather
  // than an explicit call to `init` (Foo.init()).
  bool isImplicitInit = false;
  TypeBase *favoredType = nullptr;
  if (memberName.isSimpleName(DeclBaseName::createConstructor())) {
    SmallVector<LocatorPathElt, 2> parts;
    if (auto anchor = memberLocator->getAnchor()) {
      auto path = memberLocator->getPath();
      if (!path.empty())
        if (path.back().getKind() == ConstraintLocator::ConstructorMember)
          isImplicitInit = true;

      if (auto *applyExpr = getAsExpr<ApplyExpr>(anchor)) {
        if (auto *argExpr = applyExpr->getArgs()->getUnlabeledUnaryExpr()) {
          favoredType = getFavoredType(argExpr);

          if (!favoredType) {
            optimizeConstraints(argExpr);
            favoredType = getFavoredType(argExpr);
          }
        }
      }
    }
  }

  // If we are pattern-matching an enum element and we found any enum elements,
  // ignore anything that isn't an enum element.
  bool onlyAcceptEnumElements = false;
  if (memberLocator &&
      memberLocator->isLastElement<LocatorPathElt::PatternMatch>() &&
      isa<EnumElementPattern>(
          memberLocator->getLastElementAs<LocatorPathElt::PatternMatch>()
            ->getPattern())) {
    for (const auto &result: lookup) {
      if (isa<EnumElementDecl>(result.getValueDecl())) {
        onlyAcceptEnumElements = true;
        break;
      }
    }
  }

  // If the instance type is String bridged to NSString, compute
  // the type we'll look in for bridging.
  Type bridgedType;
  if (baseObjTy->isString()) {
    if (Type classType = ctx.getBridgedToObjC(DC, instanceTy)) {
      bridgedType = classType;
    }
  }

  // Exclude some of the dynamic member choices from results
  // because using such choices would result in a self-recursive reference.
  //
  // This is required because if there are no viable/unviable choices
  // `performMemberLookup` is going to attempt to lookup inaccessible
  // members and results would include dynamic member subscripts which
  // have already been excluded.
  llvm::SmallPtrSet<ValueDecl *, 2> excludedDynamicMembers;

  // Local function that adds the given declaration if it is a
  // reasonable choice.
  auto addChoice = [&](OverloadChoice candidate) {
    auto decl = candidate.getDecl();

    // Reject circular references immediately.
    if (decl->isRecursiveValidation())
      return;

    // If the result is invalid, skip it unless solving for code completion
    // For code completion include the result because we can partially match
    // against function types that only have one parameter with error type.
    if (decl->isInvalid() && !isForCodeCompletion()) {
      result.markErrorAlreadyDiagnosed();
      return;
    }

    // If we only accept enum elements but this isn't one, ignore it.
    if (onlyAcceptEnumElements && !isa<EnumElementDecl>(decl))
      return;

    // Dig out the instance type and figure out what members of the instance type
    // we are going to see.
    auto baseTy = candidate.getBaseType();
    const auto baseObjTy = baseTy->getRValueType();

    bool hasInstanceMembers = false;
    bool hasInstanceMethods = false;
    bool hasStaticMembers = false;
    Type instanceTy = baseObjTy;
    if (baseObjTy->is<ModuleType>()) {
      hasStaticMembers = true;
    } else if (auto baseObjMeta = baseObjTy->getAs<AnyMetatypeType>()) {
      instanceTy = baseObjMeta->getInstanceType();
      if (baseObjMeta->is<ExistentialMetatypeType>()) {
        // An instance of an existential metatype is a concrete type conforming
        // to the existential, say Self. Instance members of the concrete type
        // have type Self -> T -> U, but we don't know what Self is at compile
        // time so we cannot refer to them. Static methods are fine, on the other
        // hand -- we already know that they do not have Self or associated type
        // requirements, since otherwise we would not be able to refer to the
        // existential metatype in the first place.
        hasStaticMembers = true;
      } else if (instanceTy->isExistentialType()) {
        // A protocol metatype has instance methods with type P -> T -> U, but
        // not instance properties or static members, unless result type of a
        // member conforms to this protocol -- the metatype value itself
        // doesn't give us a witness so there's no static method to bind.
        hasInstanceMethods = true;
        hasStaticMembers |=
            memberLocator->isLastElement<LocatorPathElt::UnresolvedMember>();
      } else {
        // Metatypes of nominal types and archetypes have instance methods and
        // static members, but not instance properties.
        // FIXME: partial application of properties
        hasInstanceMethods = true;
        hasStaticMembers = true;
      }

      // If we're at the root of an unevaluated context, we can
      // reference instance members on the metatype.
      if (memberLocator &&
          UnevaluatedRootExprs.count(getAsExpr(memberLocator->getAnchor()))) {
        hasInstanceMembers = true;
      }
    } else {
      // Otherwise, we can access all instance members.
      hasInstanceMembers = true;
      hasInstanceMethods = true;
    }

    // If the invocation's argument expression has a favored type,
    // use that information to determine whether a specific overload for
    // the candidate should be favored.
    if (isa<ConstructorDecl>(decl) && favoredType &&
        result.FavoredChoice == ~0U) {
      auto *ctor = cast<ConstructorDecl>(decl);

      // Only try and favor monomorphic unary initializers.
      if (!ctor->isGenericContext()) {
        if (!ctor->getMethodInterfaceType()->hasError()) {
          // The constructor might have an error type because we don't skip
          // invalid decls for code completion
          auto args = ctor->getMethodInterfaceType()
                          ->castTo<FunctionType>()
                          ->getParams();
          if (args.size() == 1 && !args[0].hasLabel() &&
              args[0].getPlainType()->isEqual(favoredType)) {
            if (!isDeclUnavailable(decl, memberLocator))
              result.FavoredChoice = result.ViableCandidates.size();
          }
        }
      }
    }

    const auto isUnsupportedExistentialMemberAccess = [&] {
      // We may not be able to derive a well defined type for an existential
      // member access if the member's signature references 'Self'.
      if (instanceTy->isExistentialType() &&
          decl->getDeclContext()->getSelfProtocolDecl()) {
        if (!isMemberAvailableOnExistential(instanceTy, decl)) {
          return true;
        }
      }

      return false;
    };

    // See if we have an instance method, instance member or static method,
    // and check if it can be accessed on our base type.

    if (decl->isInstanceMember()) {
      if (baseObjTy->is<AnyMetatypeType>()) {
        // `AnyObject` has special semantics, so let's just let it be.
        // Otherwise adjust base type and reference kind to make it
        // look as if lookup was done on the instance, that helps
        // with diagnostics.
        auto choice = instanceTy->isAnyObject()
                          ? candidate
                          : OverloadChoice(instanceTy, decl,
                                           FunctionRefKind::SingleApply);

        const bool invalidMethodRef = isa<FuncDecl>(decl) && !hasInstanceMethods;
        const bool invalidMemberRef = !isa<FuncDecl>(decl) && !hasInstanceMembers;

        if (invalidMethodRef || invalidMemberRef) {
          // If this is definitely an invalid way to reference a method or member
          // on the metatype, let's stop here.
          result.addUnviable(choice,
                             MemberLookupResult::UR_InstanceMemberOnType);
          return;
        } else if (isUnsupportedExistentialMemberAccess()) {
          // If the member reference itself is legal, but it turns out to be an
          // unsupported existential member access, do not make further
          // assumptions about the correctness of a potential call -- let
          // the unsupported member access error prevail.
          result.addUnviable(candidate,
                             MemberLookupResult::UR_UnavailableInExistential);
          return;
        } else {
          // Otherwise, still add an unviable result to the set, because it
          // could be an invalid call that was supposed to be performed on an
          // instance of the type.
          //
          // New candidate shouldn't affect performance because such
          // choice would only be attempted when solver is in diagnostic mode.
          result.addUnviable(choice,
                             MemberLookupResult::UR_InstanceMemberOnType);

        }
      }

    // If the underlying type of a typealias is fully concrete, it is legal
    // to access the type with a protocol metatype base.
    } else if (instanceTy->isExistentialType() &&
               isa<TypeAliasDecl>(decl) &&
               !cast<TypeAliasDecl>(decl)
                  ->getUnderlyingType()->getCanonicalType()
                    ->hasTypeParameter()) {

      /* We're OK */
    } else if (hasStaticMembers && baseObjTy->is<MetatypeType>() &&
               instanceTy->isExistentialType()) {
      // Static member lookup on protocol metatype in generic context
      // requires `Self` of the protocol to be bound to some concrete
      // type via same-type requirement, otherwise it would be
      // impossible to find a witness for this member.

      if (!isa<ExtensionDecl>(decl->getDeclContext())) {
        result.addUnviable(candidate,
                           MemberLookupResult::UR_TypeMemberOnInstance);
        return;
      }

      // Cannot instantiate a protocol or reference a member on
      // protocol composition type.
      if (isa<ConstructorDecl>(decl) ||
          instanceTy->is<ProtocolCompositionType>()) {
        result.addUnviable(candidate,
                           MemberLookupResult::UR_TypeMemberOnInstance);
        return;
      }

      if (getConcreteReplacementForProtocolSelfType(decl)) {
        result.addViable(candidate);
      } else {
        result.addUnviable(
            candidate,
            MemberLookupResult::UR_InvalidStaticMemberOnProtocolMetatype);
      }

      return;
    } else {
      if (!hasStaticMembers) {
        result.addUnviable(candidate,
                           MemberLookupResult::UR_TypeMemberOnInstance);
        return;
      }
    }

    if (isUnsupportedExistentialMemberAccess()) {
      result.addUnviable(candidate,
                         MemberLookupResult::UR_UnavailableInExistential);
      return;
    }

    // If we have an rvalue base, make sure that the result isn't 'mutating'
    // (only valid on lvalues).
    if (!baseTy->is<AnyMetatypeType>() &&
        !baseTy->is<LValueType>() &&
        decl->isInstanceMember()) {
      if (auto *FD = dyn_cast<FuncDecl>(decl))
        if (FD->isMutating()) {
          result.addUnviable(candidate,
                             MemberLookupResult::UR_MutatingMemberOnRValue);
          return;
        }

      // Subscripts and computed properties are ok on rvalues so long
      // as the getter is nonmutating.
      if (auto storage = dyn_cast<AbstractStorageDecl>(decl)) {
        if (storage->isGetterMutating()) {
          result.addUnviable(candidate,
                             MemberLookupResult::UR_MutatingGetterOnRValue);
          return;
        }
      }
    }

    // Check whether this is overload choice found via keypath
    // based dynamic member lookup. Since it's unknown upfront
    // what kind of declaration lookup is going to find, let's
    // double check here that given keypath is appropriate for it.
    if (memberLocator) {
      using KPDynamicMemberElt = LocatorPathElt::KeyPathDynamicMember;
      if (auto kpElt = memberLocator->getLastElementAs<KPDynamicMemberElt>()) {
        auto *keyPath = kpElt->getKeyPathDecl();
        if (isSelfRecursiveKeyPathDynamicMemberLookup(*this, baseTy,
                                                      memberLocator)) {
          excludedDynamicMembers.insert(candidate.getDecl());
          return;
        }

        if (auto *storage = dyn_cast<AbstractStorageDecl>(decl)) {
          // If this is an attempt to access read-only member via
          // writable key path, let's fail this choice early.
          auto &ctx = getASTContext();
          if (isReadOnlyKeyPathComponent(storage, SourceLoc()) &&
              (keyPath == ctx.getWritableKeyPathDecl() ||
               keyPath == ctx.getReferenceWritableKeyPathDecl())) {
            result.addUnviable(
                candidate,
                MemberLookupResult::UR_WritableKeyPathOnReadOnlyMember);
            return;
          }

          // A nonmutating setter indicates a reference-writable base,
          // on the other hand if setter is mutating there is no point
          // of attempting `ReferenceWritableKeyPath` overload.
          if (storage->isSetterMutating() &&
              keyPath == ctx.getReferenceWritableKeyPathDecl()) {
            result.addUnviable(candidate,
                               MemberLookupResult::
                                   UR_ReferenceWritableKeyPathOnMutatingMember);
            return;
          }
        }
      }
    }

    // Otherwise, we're good, add the candidate to the list.
    result.addViable(candidate);
  };

  // Local function that turns a ValueDecl into a properly configured
  // OverloadChoice.
  auto getOverloadChoice =
      [&](ValueDecl *cand, bool isBridged, bool isUnwrappedOptional,
          bool isFallbackUnwrap = false) -> OverloadChoice {
    // If we're looking into an existential type, check whether this
    // result was found via dynamic lookup.
    if (instanceTy->isAnyObject()) {
      assert(cand->getDeclContext()->isTypeContext() && "Dynamic lookup bug");
      
      // We found this declaration via dynamic lookup, record it as such.
      return OverloadChoice::getDeclViaDynamic(baseTy, cand, functionRefKind);
    }
    
    // If we have a bridged type, we found this declaration via bridging.
    if (isBridged)
      return OverloadChoice::getDeclViaBridge(bridgedType, cand,
                                              functionRefKind);
    
    // If we got the choice by unwrapping an optional type, unwrap the base
    // type.
    if (isUnwrappedOptional) {
      auto ovlBaseTy = MetatypeType::get(baseTy->castTo<MetatypeType>()
                                             ->getInstanceType()
                                             ->getOptionalObjectType());
      return OverloadChoice::getDeclViaUnwrappedOptional(
          ovlBaseTy, cand,
          /*isFallback=*/isFallbackUnwrap, functionRefKind);
    }

    // While looking for subscript choices it's possible to find
    // `subscript(dynamicMember: {Writable}KeyPath)` on types
    // marked as `@dynamicMemberLookup`, let's mark this candidate
    // as representing "dynamic lookup" unless it's a direct call
    // to such subscript (in that case label is expected to match).
    if (auto *subscript = dyn_cast<SubscriptDecl>(cand)) {
      if (memberLocator && instanceTy->hasDynamicMemberLookupAttribute() &&
          isValidKeyPathDynamicMemberLookup(subscript)) {
        auto *args = getArgumentList(memberLocator);

        if (!(args && args->isUnary() &&
              args->getLabel(0) == getASTContext().Id_dynamicMember)) {
          return OverloadChoice::getDynamicMemberLookup(
              baseTy, subscript, ctx.getIdentifier("subscript"),
              /*isKeyPathBased=*/true);
        }
      }
    }

    return OverloadChoice(baseTy, cand, functionRefKind);
  };

  // Add all results from this lookup.
  for (auto result : lookup)
    addChoice(getOverloadChoice(result.getValueDecl(),
                                /*isBridged=*/false,
                                /*isUnwrappedOptional=*/false));

  // Backward compatibility hack. In Swift 4, `init` and init were
  // the same name, so you could write "foo.init" to look up a
  // method or property named `init`.
  if (!ctx.isSwiftVersionAtLeast(5) &&
      memberName.getBaseName() == DeclBaseName::createConstructor() &&
      !isImplicitInit) {
    auto &compatLookup = lookupMember(instanceTy,
                                      DeclNameRef(ctx.getIdentifier("init")));
    for (auto result : compatLookup)
      addChoice(getOverloadChoice(result.getValueDecl(),
                                  /*isBridged=*/false,
                                  /*isUnwrappedOptional=*/false));
  }

  // If the instance type is a bridged to an Objective-C type, perform
  // a lookup into that Objective-C type.
  if (bridgedType) {
    LookupResult &bridgedLookup = lookupMember(bridgedType, memberName);
    ModuleDecl *foundationModule = nullptr;
    for (auto result : bridgedLookup) {
      // Ignore results from the Objective-C "Foundation"
      // module. Those core APIs are explicitly provided by the
      // Foundation module overlay.
      auto module = result.getValueDecl()->getModuleContext();
      if (foundationModule) {
        if (module == foundationModule)
          continue;
      } else if (ClangModuleUnit::hasClangModule(module) &&
                 module->getName().str() == "Foundation") {
        // Cache the foundation module name so we don't need to look
        // for it again.
        foundationModule = module;
        continue;
      }
      
      addChoice(getOverloadChoice(result.getValueDecl(),
                                  /*isBridged=*/true,
                                  /*isUnwrappedOptional=*/false));
    }
  }

  // If we have candidates, and we're doing a member lookup for a pattern
  // match, unwrap optionals and try again to allow implicit creation of
  // optional "some" patterns (spelled "?").
  if (result.ViableCandidates.empty() && result.UnviableCandidates.empty() &&
      memberLocator &&
      memberLocator->isLastElement<LocatorPathElt::PatternMatch>() &&
      instanceTy->getOptionalObjectType() &&
      baseObjTy->is<AnyMetatypeType>()) {
    SmallVector<Type, 2> optionals;
    Type instanceObjectTy = instanceTy->lookThroughAllOptionalTypes(optionals);
    Type metaObjectType = MetatypeType::get(instanceObjectTy);
    auto result = performMemberLookup(
        constraintKind, memberName, metaObjectType,
        functionRefKind, memberLocator, includeInaccessibleMembers);
    result.numImplicitOptionalUnwraps = optionals.size();
    result.actualBaseType = metaObjectType;
    return result;
  }

  // If we're looking into a metatype for an unresolved member lookup, look
  // through optional types.
  //
  // FIXME: Unify with the above code path.
  if (baseObjTy->is<AnyMetatypeType>() &&
      constraintKind == ConstraintKind::UnresolvedValueMember) {
    if (auto objectType = instanceTy->getOptionalObjectType()) {
      // If we don't have a wrapped type yet, we can't look through the optional
      // type.
      if (objectType->getAs<TypeVariableType>() && result.ViableCandidates.empty()) {
        MemberLookupResult result;
        result.OverallResult = MemberLookupResult::Unsolved;
        return result;
      }

      if (objectType->mayHaveMembers()) {
        // If there are viable members directly on `Optional`, let's
        // prioritize them and mark any results found on wrapped type
        // as a fallback results.
        bool isFallback = !result.ViableCandidates.empty();
        LookupResult &optionalLookup = lookupMember(objectType, memberName);
        for (auto result : optionalLookup)
          addChoice(getOverloadChoice(result.getValueDecl(),
                                      /*bridged*/ false,
                                      /*isUnwrappedOptional=*/true,
                                      /*isUnwrapFallback=*/isFallback));
      }
    }
  }

  // If we're about to fail lookup because there are no viable candidates
  // or if all of the candidates come from conditional conformances (which
  // might not be applicable), and we are looking for members in a type with
  // the @dynamicMemberLookup attribute, then we resolve a reference to a
  // `subscript(dynamicMember:)` method and pass the member name as a string
  // parameter.
  if (constraintKind == ConstraintKind::ValueMember &&
      memberName.isSimpleName() && !memberName.isSpecial() &&
      instanceTy->hasDynamicMemberLookupAttribute()) {
    const auto &candidates = result.ViableCandidates;

    if ((candidates.empty() ||
         allFromConditionalConformances(DC, instanceTy, candidates)) &&
        !isSelfRecursiveKeyPathDynamicMemberLookup(*this, baseTy,
                                                   memberLocator)) {
      auto &ctx = getASTContext();

      // Recursively look up `subscript(dynamicMember:)` methods in this type.
      DeclNameRef subscriptName(
          { ctx, DeclBaseName::createSubscript(), { ctx.Id_dynamicMember } });
      auto subscripts = performMemberLookup(
          constraintKind, subscriptName, baseTy, functionRefKind, memberLocator,
          includeInaccessibleMembers);

      // Reflect the candidates found as `DynamicMemberLookup` results.
      auto name = memberName.getBaseIdentifier();
      for (const auto &candidate : subscripts.ViableCandidates) {
        auto *SD = cast<SubscriptDecl>(candidate.getDecl());
        bool isKeyPathBased = isValidKeyPathDynamicMemberLookup(SD);

        if (isValidStringDynamicMemberLookup(SD, DC->getParentModule()) ||
            isKeyPathBased)
          result.addViable(OverloadChoice::getDynamicMemberLookup(
              baseTy, SD, name, isKeyPathBased));
      }

      for (auto index : indices(subscripts.UnviableCandidates)) {
        auto *SD =
            cast<SubscriptDecl>(subscripts.UnviableCandidates[index].getDecl());
        auto choice = OverloadChoice::getDynamicMemberLookup(
            baseTy, SD, name, isValidKeyPathDynamicMemberLookup(SD));
        result.addUnviable(choice, subscripts.UnviableReasons[index]);
      }
    }
  }

  // If we have no viable or unviable candidates, and we're generating,
  // diagnostics, rerun the query with inaccessible members included, so we can
  // include them in the unviable candidates list.
  if (result.ViableCandidates.empty() && result.UnviableCandidates.empty() &&
      includeInaccessibleMembers) {
    NameLookupOptions lookupOptions = defaultMemberLookupOptions;
    
    // Ignore access control so we get candidates that might have been missed
    // before.
    lookupOptions |= NameLookupFlags::IgnoreAccessControl;

    auto lookup =
        TypeChecker::lookupMember(DC, instanceTy, memberName, lookupOptions);
    for (auto entry : lookup) {
      auto *cand = entry.getValueDecl();

      // If the result is invalid, skip it.
      if (cand->isInvalid()) {
        result.markErrorAlreadyDiagnosed();
        return result;
      }

      if (excludedDynamicMembers.count(cand))
        continue;

      result.addUnviable(getOverloadChoice(cand, /*isBridged=*/false,
                                           /*isUnwrappedOptional=*/false),
                         MemberLookupResult::UR_Inaccessible);
    }
  }
  
  return result;
}

/// Determine whether the given type refers to a non-final class (or
/// dynamic self of one).
static bool isNonFinalClass(Type type) {
  if (auto dynamicSelf = type->getAs<DynamicSelfType>())
    type = dynamicSelf->getSelfType();

  if (auto classDecl = type->getClassOrBoundGenericClass())
    return !classDecl->isSemanticallyFinal();

  if (auto archetype = type->getAs<ArchetypeType>())
    if (auto super = archetype->getSuperclass())
      return isNonFinalClass(super);

  return type->isExistentialType();
}

/// Determine whether given constructor reference is valid or does it require
/// any fixes e.g. when base is a protocol metatype.
static ConstraintFix *validateInitializerRef(ConstraintSystem &cs,
                                             ConstructorDecl *init,
                                             ConstraintLocator *locator) {
  auto anchor = locator->getAnchor();
  if (!anchor)
    return nullptr;

  // Avoid checking implicit conversions injected by the compiler.
  if (locator->findFirst<LocatorPathElt::ImplicitConversion>())
    return nullptr;

  auto getType = [&cs](Expr *expr) -> Type {
    return cs.simplifyType(cs.getType(expr))->getRValueType();
  };

  Expr *baseExpr = nullptr;
  Type baseType;

  // Explicit initializer reference e.g. `T.init(...)` or `T.init`.
  if (auto *UDE = getAsExpr<UnresolvedDotExpr>(anchor)) {
    baseExpr = UDE->getBase();
    baseType = getType(baseExpr);
    if (baseType->is<MetatypeType>()) {
      auto instanceType = baseType->getAs<MetatypeType>()
                              ->getInstanceType()
                              ->getWithoutParens();
      if (!cs.isTypeReference(baseExpr) && instanceType->isExistentialType()) {
        return AllowInvalidInitRef::onProtocolMetatype(
            cs, baseType, init, /*isStaticallyDerived=*/true,
            baseExpr->getSourceRange(), locator);
      }
    }
    // Initializer call e.g. `T(...)`
  } else if (auto *CE = getAsExpr<CallExpr>(anchor)) {
    baseExpr = CE->getFn();
    baseType = getType(baseExpr);
    // FIXME: Historically, UnresolvedMemberExprs have allowed implicit
    // construction through a metatype value, but this should probably be
    // illegal.
    if (!isa<UnresolvedMemberExpr>(baseExpr)) {
      // If this is an initializer call without explicit mention
      // of `.init` on metatype value.
      if (auto *AMT = baseType->getAs<AnyMetatypeType>()) {
        auto instanceType = AMT->getInstanceType()->getWithoutParens();
        if (!cs.isTypeReference(baseExpr)) {
          if (baseType->is<MetatypeType>() &&
              instanceType->isAnyExistentialType()) {
            return AllowInvalidInitRef::onProtocolMetatype(
                cs, baseType, init, cs.isStaticallyDerivedMetatype(baseExpr),
                baseExpr->getSourceRange(), locator);
          }

          if (!instanceType->isExistentialType() ||
              instanceType->isAnyExistentialType()) {
            return AllowInvalidInitRef::onNonConstMetatype(cs, baseType, init,
                                                           locator);
          }
        }
      }
    }
    // Initializer reference which requires contextual base type e.g.
    // `.init(...)`. Could also be a nested type or typealias being constructed
    // via implicit member syntax, e.g., `let _: Base = .Nested()` where
    // `Base.Nested: Base`.
  } else if (auto *UME = getAsExpr<UnresolvedMemberExpr>(anchor)) {
    // If we're accessing a nested type to perform the construction implicitly,
    // then the type we're constructing may not actually be the base of the
    // UnresolvedMemberExpr--instead, it will be the type of the nested type
    // member.
    // We need to find type variable which represents contextual base.
    auto *baseLocator = cs.getConstraintLocator(
        UME, locator->isLastElement<LocatorPathElt::ConstructorMember>()
                 ? ConstraintLocator::UnresolvedMember
                 : ConstraintLocator::MemberRefBase);

    // FIXME: Type variables responsible for contextual base could be cached
    // in the constraint system to speed up lookup.
    auto result = llvm::find_if(
        cs.getTypeVariables(), [&baseLocator](const TypeVariableType *typeVar) {
          return typeVar->getImpl().getLocator() == baseLocator;
        });

    assert(result != cs.getTypeVariables().end());
    baseType = cs.simplifyType(*result)->getRValueType();
    // Constraint for member base is formed as '$T.Type[.<member] = ...`
    // which means MetatypeType has to be added after finding a type variable.
    if (baseLocator->isLastElement<LocatorPathElt::MemberRefBase>())
      baseType = MetatypeType::get(baseType);
  } else if (auto *keyPathExpr = getAsExpr<KeyPathExpr>(anchor)) {
    // Key path can't refer to initializers e.g. `\Type.init`
    return AllowInvalidRefInKeyPath::forRef(cs, init, locator);
  }

  if (!baseType)
    return nullptr;

  if (!baseType->is<AnyMetatypeType>()) {
    bool applicable = false;
    // Special case -- in a protocol extension initializer with a class
    // constrained Self type, 'self' has archetype type, and only
    // required initializers can be called.
    if (baseExpr && !baseExpr->isSuperExpr()) {
      auto &ctx = cs.getASTContext();
      if (auto *DRE =
              dyn_cast<DeclRefExpr>(baseExpr->getSemanticsProvidingExpr())) {
        if (DRE->getDecl()->getName() == ctx.Id_self) {
          if (getType(DRE)->is<ArchetypeType>())
            applicable = true;
        }
      }
    }

    if (!applicable)
      return nullptr;
  }

  auto instanceType = baseType->getMetatypeInstanceType();
  bool isStaticallyDerived = true;
  // If this is expression like `.init(...)` where base type is
  // determined by a contextual type.
  if (!baseExpr) {
    isStaticallyDerived = !(instanceType->is<DynamicSelfType>() ||
                            instanceType->is<ArchetypeType>());
  // Otherwise this is something like `T.init(...)`
  } else {
    isStaticallyDerived = cs.isStaticallyDerivedMetatype(baseExpr);
  }

  auto baseRange = baseExpr ? baseExpr->getSourceRange() : SourceRange();
  // FIXME: The "hasClangNode" check here is a complete hack.
  if (isNonFinalClass(instanceType) && !isStaticallyDerived &&
      !init->hasClangNode() &&
      !(init->isRequired() || init->getDeclContext()->getSelfProtocolDecl())) {
    return AllowInvalidInitRef::dynamicOnMetatype(cs, baseType, init, baseRange,
                                                  locator);
    // Constructors cannot be called on a protocol metatype, because there is no
    // metatype to witness it.
  } else if (baseType->is<MetatypeType>() &&
             instanceType->isExistentialType()) {
    return AllowInvalidInitRef::onProtocolMetatype(
        cs, baseType, init, isStaticallyDerived, baseRange, locator);
  }

  return nullptr;
}

static ConstraintFix *
fixMemberRef(ConstraintSystem &cs, Type baseTy,
             DeclNameRef memberName, const OverloadChoice &choice,
             ConstraintLocator *locator,
             Optional<MemberLookupResult::UnviableReason> reason = None) {
  // Not all of the choices handled here are going
  // to refer to a declaration.
  if (auto *decl = choice.getDeclOrNull()) {
    if (auto *CD = dyn_cast<ConstructorDecl>(decl)) {
      if (auto *fix = validateInitializerRef(cs, CD, locator))
        return fix;
    }

    if (locator->isForKeyPathDynamicMemberLookup()) {
      if (auto *fix = AllowInvalidRefInKeyPath::forRef(cs, decl, locator))
        return fix;
    }
  }

  if (reason) {
    switch (*reason) {
    case MemberLookupResult::UR_InstanceMemberOnType:
    case MemberLookupResult::UR_TypeMemberOnInstance: {
      return choice.isDecl()
                 ? AllowTypeOrInstanceMember::create(
                       cs, baseTy, choice.getDecl(), memberName, locator)
                 : nullptr;
    }

    case MemberLookupResult::UR_Inaccessible:
      assert(choice.isDecl());
      return AllowInaccessibleMember::create(cs, baseTy, choice.getDecl(),
                                             memberName, locator);

    case MemberLookupResult::UR_UnavailableInExistential: {
      return choice.isDecl()
                 ? AllowMemberRefOnExistential::create(
                       cs, baseTy, choice.getDecl(), memberName, locator)
                 : nullptr;
    }

    case MemberLookupResult::UR_MutatingMemberOnRValue:
    case MemberLookupResult::UR_MutatingGetterOnRValue: {
      return choice.isDecl()
                 ? AllowMutatingMemberOnRValueBase::create(
                       cs, baseTy, choice.getDecl(), memberName, locator)
                 : nullptr;
    }

    // TODO(diagnostics): Add a new fix that is suggests to
    // add `subscript(dynamicMember: {Writable}KeyPath<T, U>)`
    // overload here, that would help if such subscript has
    // not been provided.
    case MemberLookupResult::UR_WritableKeyPathOnReadOnlyMember:
      return TreatRValueAsLValue::create(cs, cs.getConstraintLocator(locator));
    case MemberLookupResult::UR_ReferenceWritableKeyPathOnMutatingMember:
      break;
    case MemberLookupResult::UR_KeyPathWithAnyObjectRootType:
      return AllowAnyObjectKeyPathRoot::create(cs, locator);

    case MemberLookupResult::UR_InvalidStaticMemberOnProtocolMetatype:
      return AllowInvalidStaticMemberRefOnProtocolMetatype::create(cs, locator);
    }
  }

  return nullptr;
}

/// Convert the given enum element pattern into an expression pattern
/// and synthesize ~= operator application to find the type of the
/// element.
static bool inferEnumMemberThroughTildeEqualsOperator(
    ConstraintSystem &cs, EnumElementPattern *pattern, Type enumTy,
    Type elementTy, ConstraintLocator *locator) {
  if (!pattern->hasUnresolvedOriginalExpr())
    return true;

  auto &ctx = cs.getASTContext();

  // Retrieve a corresponding ExprPattern which we can solve with ~=.
  auto *EP =
      llvm::cantFail(ctx.evaluator(EnumElementExprPatternRequest{pattern}));

  auto target = SyntacticElementTarget::forExprPattern(EP);

  DiagnosticTransaction diagnostics(ctx.Diags);
  {
    if (cs.preCheckTarget(target, /*replaceInvalidRefWithErrors=*/true,
                          /*leaveClosureBodyUnchecked=*/false)) {
      // Skip diagnostics if they are disabled, otherwise it would result in
      // duplicate diagnostics, since this operation is going to be repeated
      // in diagnostic mode.
      if (!cs.shouldAttemptFixes())
        diagnostics.abort();

      return true;
    }
  }
  cs.setType(EP->getMatchVar(), enumTy);
  cs.setType(EP, enumTy);

  if (cs.generateConstraints(target))
    return true;

  // Sub-expression associated with expression pattern is the enum element
  // access which needs to be connected to the provided element type.
  cs.addConstraint(ConstraintKind::Conversion, cs.getType(EP->getSubExpr()),
                   elementTy, cs.getConstraintLocator(EP));

  cs.setTargetFor(pattern, target);
  return false;
}

ConstraintSystem::SolutionKind ConstraintSystem::simplifyMemberConstraint(
    ConstraintKind kind, Type baseTy, DeclNameRef member, Type memberTy,
    DeclContext *useDC, FunctionRefKind functionRefKind,
    ArrayRef<OverloadChoice> outerAlternatives, TypeMatchOptions flags,
    ConstraintLocatorBuilder locatorB) {
  // We'd need to record original base type because it might be a type
  // variable representing another missing member.
  auto origBaseTy = baseTy;
  // Resolve the base type, if we can. If we can't resolve the base type,
  // then we can't solve this constraint.
  baseTy = simplifyType(baseTy, flags);
  Type baseObjTy = baseTy->getRValueType();

  auto locator = getConstraintLocator(locatorB);

  auto formUnsolved = [&](bool activate = false) {
    // If requested, generate a constraint.
    if (flags.contains(TMF_GenerateConstraints)) {
      auto *memberRef = Constraint::createMemberOrOuterDisjunction(
          *this, kind, baseTy, memberTy, member, useDC, functionRefKind,
          outerAlternatives, locator);

      addUnsolvedConstraint(memberRef);

      if (activate)
        activateConstraint(memberRef);

      return SolutionKind::Solved;
    }

    return SolutionKind::Unsolved;
  };

  // If the base type of this member lookup is a "hole" there is no
  // reason to perform a lookup because it wouldn't return any results.
  if (shouldAttemptFixes()) {
    auto markMemberTypeAsPotentialHole = [&](Type memberTy) {
      if (auto *typeVar = memberTy->getAs<TypeVariableType>())
        recordPotentialHole(typeVar);
    };

    // If this is an unresolved member ref e.g. `.foo` and its contextual base
    // type has been determined to be a "hole", let's mark the resulting member
    // type as a potential hole and continue solving.
    if (kind == ConstraintKind::UnresolvedValueMember) {
      // Let's look through all metatypes to find "underlying" type
      // of this lookup.
      Type underlyingType = baseObjTy;
      while (auto *MT = underlyingType->getAs<AnyMetatypeType>()) {
        underlyingType = MT->getInstanceType();
      }

      // Let's delay solving this constraint in diagnostic
      // mode until it's certain that there is no way to
      // find out what the base type is.
      if (underlyingType->isTypeVariableOrMember())
        return formUnsolved();

      // Let's record a fix only if the hole originates either
      // at the result of the chain (that could happen since solving
      // of this constraint is delayed until base could be resolved),
      // or it is certain that base type can't be bound to any other
      // type but a hole.
      auto shouldRecordFixForHole = [&](PlaceholderType *baseType) {
        auto *originator =
            baseType->getOriginator().dyn_cast<TypeVariableType *>();

        if (!originator)
          return false;

        auto *originatorLoc = originator->getImpl().getLocator();

        // It could either be a hole associated directly with the base
        // or a hole which came from result type of the chain.
        if (originatorLoc->isLastElement<
                LocatorPathElt::UnresolvedMemberChainResult>()) {
          auto *UMCR = castToExpr<UnresolvedMemberChainResultExpr>(
              originatorLoc->getAnchor());
          return UMCR->getChainBase() == getAsExpr(locator->getAnchor());
        }

        return originatorLoc == locator;
      };

      if (auto *hole = underlyingType->getAs<PlaceholderType>()) {
        if (shouldRecordFixForHole(hole)) {
          auto *fix = SpecifyBaseTypeForContextualMember::create(*this, member,
                                                                 locator);
          if (recordFix(fix))
            return SolutionKind::Error;
        }

        markMemberTypeAsPotentialHole(memberTy);
        return SolutionKind::Solved;
      }
    } else if ((kind == ConstraintKind::ValueMember ||
                kind == ConstraintKind::ValueWitness) &&
               baseObjTy->getMetatypeInstanceType()->isPlaceholder()) {
      // If base type is a "hole" there is no reason to record any
      // more "member not found" fixes for chained member references.
      markMemberTypeAsPotentialHole(memberTy);
      return SolutionKind::Solved;
    }
  }

  // Special handling of injected references to `makeIterator` and `next`
  // in for-in loops.
  if (auto *expr = getAsExpr(locator->getAnchor())) {
    // `next()` could be wrapped in `await` expression.
    auto memberRef =
        getAsExpr<UnresolvedDotExpr>(expr->getSemanticsProvidingExpr());

    if (memberRef && memberRef->isImplicit() &&
        locator->isLastElement<LocatorPathElt::Member>()) {
      auto &ctx = getASTContext();

      // Cannot simplify this constraint yet since we don't know whether
      // the base type is going to be existential or not.
      if (baseObjTy->isTypeVariableOrMember())
        return formUnsolved();

      // Check whether the given dot expression is a reference
      // to the given name with the given set of argument labels
      // (aka compound name).
      auto isRefTo = [&](UnresolvedDotExpr *UDE, Identifier name,
                         ArrayRef<StringRef> labels) {
        auto refName = UDE->getName().getFullName();
        return refName.isCompoundName(name, labels);
      };

      auto *baseExpr = memberRef->getBase();
      // Handle `makeIterator` reference.
      if (getContextualTypePurpose(baseExpr) == CTP_ForEachSequence &&
          isRefTo(memberRef, ctx.Id_makeIterator, /*lables=*/{})) {
        auto *sequenceProto = cast<ProtocolDecl>(
            getContextualType(baseExpr, /*forConstraint=*/false)
                ->getAnyNominal());
        bool isAsync = sequenceProto ==
                       TypeChecker::getProtocol(
                           ctx, SourceLoc(), KnownProtocolKind::AsyncSequence);

        auto *makeIterator = isAsync ? ctx.getAsyncSequenceMakeAsyncIterator()
                                     : ctx.getSequenceMakeIterator();

        return simplifyValueWitnessConstraint(
            ConstraintKind::ValueWitness, baseTy, makeIterator, memberTy, useDC,
            FunctionRefKind::Compound, flags, locator);
      }

      // Handle `next` reference.
      if (getContextualTypePurpose(baseExpr) == CTP_ForEachSequence &&
          isRefTo(memberRef, ctx.Id_next, /*labels=*/{})) {
        auto *iteratorProto = cast<ProtocolDecl>(
            getContextualType(baseExpr, /*forConstraint=*/false)
                ->getAnyNominal());
        bool isAsync =
            iteratorProto ==
            TypeChecker::getProtocol(ctx, SourceLoc(),
                                     KnownProtocolKind::AsyncIteratorProtocol);

        auto *next =
            isAsync ? ctx.getAsyncIteratorNext() : ctx.getIteratorNext();

        return simplifyValueWitnessConstraint(
            ConstraintKind::ValueWitness, baseTy, next, memberTy, useDC,
            FunctionRefKind::Compound, flags, locator);
      }
    }
  }

  MemberLookupResult result =
      performMemberLookup(kind, member, baseTy, functionRefKind, locator,
                          /*includeInaccessibleMembers*/ shouldAttemptFixes());

  switch (result.OverallResult) {
  case MemberLookupResult::Unsolved:
    return formUnsolved();

  case MemberLookupResult::ErrorAlreadyDiagnosed:
  case MemberLookupResult::HasResults:
    // Keep going!
    break;
  }

  SmallVector<Constraint *, 4> candidates;

  // If we found viable candidates, then we're done!
  if (!result.ViableCandidates.empty()) {
    // If we had to look in a different type, use that.
    if (result.actualBaseType)
      baseTy = result.actualBaseType;

    // If only possible choice to refer to member is via keypath
    // dynamic member dispatch, let's delay solving this constraint
    // until constraint generation phase is complete, because
    // subscript dispatch relies on presence of function application.
    if (result.ViableCandidates.size() == 1) {
      auto &choice = result.ViableCandidates.front();
      if (Phase == ConstraintSystemPhase::ConstraintGeneration &&
          choice.isKeyPathDynamicMemberLookup() &&
          member.getBaseName().isSubscript()) {
        // Let's move this constraint to the active
        // list so it could be picked up right after
        // constraint generation is done.
        return formUnsolved(/*activate=*/true);
      }
    }

    generateConstraints(
        candidates, memberTy, result.ViableCandidates, useDC, locator,
        result.getFavoredIndex(), /*requiresFix=*/false,
        [&](unsigned, const OverloadChoice &choice) {
          return fixMemberRef(*this, baseTy, member, choice, locator);
        });

    if (!outerAlternatives.empty()) {
      // If local scope has a single choice,
      // it should always be preferred.
      if (candidates.size() == 1)
        candidates.front()->setFavored();

      // We *might* include any non-members that we found in outer contexts in
      // some special cases, for backwards compatibility: first, we have to be
      // looking for one of the special names ('min' or 'max'), and second, all
      // of the inner (viable) results need to come from conditional
      // conformances. The second condition is how the problem here was
      // encountered: a type ('Range') was made to conditionally conform to a
      // new protocol ('Sequence'), which introduced some extra methods
      // ('min' and 'max') that shadowed global functions that people regularly
      // called within extensions to that type (usually adding 'clamp').
      bool treatAsViable =
          (member.isSimpleName("min") || member.isSimpleName("max")) &&
          allFromConditionalConformances(DC, baseTy, result.ViableCandidates);

      generateConstraints(
          candidates, memberTy, outerAlternatives, useDC, locator, None,
          /*requiresFix=*/!treatAsViable,
          [&](unsigned, const OverloadChoice &) {
            return treatAsViable ? nullptr
                                 : AddQualifierToAccessTopLevelName::create(
                                       *this, locator);
          });
    }
  }

  if (!result.UnviableCandidates.empty()) {
    // Generate constraints for unavailable choices if they have a fix,
    // and disable them by default, they'd get picked up in the "salvage" mode.
    generateConstraints(
        candidates, memberTy, result.UnviableCandidates, useDC, locator,
        /*favoredChoice=*/None, /*requiresFix=*/true,
        [&](unsigned idx, const OverloadChoice &choice) {
          return fixMemberRef(*this, baseTy, member, choice, locator,
                              result.UnviableReasons[idx]);
        });
  }

  // Attempt to record a warning where the unresolved member could be
  // ambiguous with optional member. e.g.
  //  enum Foo {
  //    case none
  //  }
  //
  //  let _: Foo? = .none // Although base is inferred as Optional.none
  //  it could be also Foo.none.
  if (auto *fix = SpecifyBaseTypeForOptionalUnresolvedMember::attempt(
          *this, kind, baseObjTy, member, functionRefKind, result, locator)) {
    (void)recordFix(fix);
  }

  // If there were no results from a direct enum lookup, let's attempt
  // to resolve this member via ~= operator application.
  if (candidates.empty()) {
    if (auto patternLoc =
            locator->getLastElementAs<LocatorPathElt::PatternMatch>()) {
      if (auto *enumElement =
              dyn_cast<EnumElementPattern>(patternLoc->getPattern())) {
        auto enumType = baseObjTy->getMetatypeInstanceType();

        // Optional base type does not trigger `~=` synthesis, but it tries
        // to find member on both `Optional` and its wrapped type.
        if (!enumType->getOptionalObjectType()) {
          // If the synthesis of ~= resulted in errors (i.e. broken stdlib)
          // that would be diagnosed inline, so let's just fall through and
          // let this situation be diagnosed as a missing member.
          auto hadErrors = inferEnumMemberThroughTildeEqualsOperator(
            *this, enumElement, enumType, memberTy, locator);

          // Let's consider current member constraint solved because it's
          // replaced by a new set of constraints that would resolve member
          // type.
          if (!hadErrors)
            return SolutionKind::Solved;
        }
      }
    }
  }

  if (!candidates.empty()) {
    addOverloadSet(candidates, locator);
    return SolutionKind::Solved;
  }

  // If the lookup found no hits at all (either viable or unviable), diagnose it
  // as such and try to recover in various ways.
  if (shouldAttemptFixes()) {
    auto fixMissingMember = [&](Type baseTy, Type memberTy,
                                ConstraintLocator *locator) -> SolutionKind {
      // Let's check whether there are any generic parameters associated with
      // base type, and record potential holes if so.
      simplifyType(baseTy).transform([&](Type type) -> Type {
        if (auto *typeVar = type->getAs<TypeVariableType>()) {
          if (typeVar->getImpl().hasRepresentativeOrFixed())
            return type;
          recordPotentialHole(typeVar);
        }
        return type;
      });

      auto success = [&]() -> SolutionKind {
        // Record a hole for memberTy to make it possible to form solutions
        // when contextual result type cannot be deduced e.g. `let _ = x.foo`.
        if (auto *memberTypeVar = memberTy->getAs<TypeVariableType>()) {
          if (getFixedType(memberTypeVar)) {
            // If member has been bound before the base and the base was
            // incorrect at that (e.g. fallback to default `Any` type),
            // then we need to re-activate all of the constraints
            // associated with this member reference otherwise some of
            // the constraints could be left unchecked in inactive state.
            // This is especially important for key path expressions because
            // `key path` constraint can't be retired until all components
            // are simplified.
            addTypeVariableConstraintsToWorkList(memberTypeVar);
          } else if (locator->isLastElement<LocatorPathElt::PatternMatch>()) {
            // Let's handle member patterns specifically because they use
            // equality instead of argument application constraint, so allowing
            // them to bind member could mean missing valid hole positions in
            // the pattern.
            recordTypeVariablesAsHoles(memberTypeVar);
          } else {
            recordPotentialHole(memberTypeVar);
          }
        }

        return SolutionKind::Solved;
      };

      bool alreadyDiagnosed = (result.OverallResult ==
                               MemberLookupResult::ErrorAlreadyDiagnosed);
      auto *fix = DefineMemberBasedOnUse::create(*this, baseTy, member,
                                                 alreadyDiagnosed, locator);

      auto instanceTy = baseObjTy->getMetatypeInstanceType();

      auto impact = 2;
      // Impact is higher if the base type is any function type
      // because function types can't have any members other than self
      if (instanceTy->is<AnyFunctionType>()) {
        impact += 10;
      }

      if (instanceTy->isAny() || instanceTy->isAnyObject())
        impact += 5;

      auto *anchorExpr = getAsExpr(locator->getAnchor());
      if (anchorExpr) {
        if (auto *UDE = dyn_cast<UnresolvedDotExpr>(anchorExpr)) {
          // The issue is related to a missing `Sequence` protocol
          // conformance if either `makeIterator` or `next` are missing.
          if (UDE->isImplicit()) {
            // Missing `makeIterator` since the base is sequence expression.
            if (getContextualTypePurpose(UDE->getBase()) == CTP_ForEachSequence)
              return success();

            // Missing `next` where the base is result of `makeIterator`.
            if (auto *base = dyn_cast<DeclRefExpr>(UDE->getBase())) {
              if (auto var = dyn_cast_or_null<VarDecl>(base->getDecl())) {
                if (var->getNameStr().contains("$generator") &&
                    UDE->getName().getBaseIdentifier() == Context.Id_next)
                  return success();
              }
            }
          }
        }

        // Increasing the impact for missing member in any argument position so
        // it doesn't affect situations where there are another fixes involved.
        if (getArgumentLocator(anchorExpr))
          impact += 5;
      }

      if (recordFix(fix, impact))
        return SolutionKind::Error;

      return success();
    };

    if (baseObjTy->getOptionalObjectType()) {
      // If the base type was an optional, look through it.

      // If the base type is optional because we haven't chosen to force an
      // implicit optional, don't try to fix it. The IUO will be forced instead.
      if (auto dotExpr = getAsExpr<UnresolvedDotExpr>(locator->getAnchor())) {
        auto baseExpr = dotExpr->getBase();
        if (auto overload = findSelectedOverloadFor(baseExpr)) {
          auto iuoKind = overload->choice.getIUOReferenceKind(*this);
          if (iuoKind == IUOReferenceKind::Value)
            return SolutionKind::Error;
        }
      }

      // Let's check whether the problem is related to optionality of base
      // type, or there is no member with a given name.
      result =
          performMemberLookup(kind, member, baseObjTy->getOptionalObjectType(),
                              functionRefKind, locator,
                              /*includeInaccessibleMembers*/ true);

      // If unwrapped type still couldn't find anything for a given name,
      // let's fallback to a "not such member" fix.
      if (result.ViableCandidates.empty() && result.UnviableCandidates.empty())
        return fixMissingMember(origBaseTy, memberTy, locator);

      // The result of the member access can either be the expected member type
      // (for '!' or optional members with '?'), or the original member type
      // with one extra level of optionality ('?' with non-optional members).
      auto innerTV = createTypeVariable(locator,
                                        TVO_CanBindToLValue |
                                        TVO_CanBindToNoEscape);
      Type optTy = TypeChecker::getOptionalType(SourceLoc(), innerTV);
      assert(!optTy->hasError());
      SmallVector<Constraint *, 2> optionalities;
      auto nonoptionalResult = Constraint::createFixed(
          *this, ConstraintKind::Bind,
          UnwrapOptionalBase::create(*this, member, baseObjTy, locator),
          memberTy, innerTV, locator);
      auto optionalResult = Constraint::createFixed(
          *this, ConstraintKind::Bind,
          UnwrapOptionalBase::createWithOptionalResult(*this, member,
                                                       baseObjTy, locator),
          optTy, memberTy, locator);
      optionalities.push_back(nonoptionalResult);
      optionalities.push_back(optionalResult);
      addDisjunctionConstraint(optionalities, locator);

      // Look through one level of optional.
      addValueMemberConstraint(baseObjTy->getOptionalObjectType(), member,
                               innerTV, useDC, functionRefKind,
                               outerAlternatives, locator);
      return SolutionKind::Solved;
    }

    auto solveWithNewBaseOrName = [&](Type baseType,
                                      DeclNameRef memberName) -> SolutionKind {
      return simplifyMemberConstraint(kind, baseType, memberName, memberTy,
                                      useDC, functionRefKind, outerAlternatives,
                                      flags | TMF_ApplyingFix, locatorB);
    };

    // If this member reference is a result of a previous fix, let's not allow
    // any more fixes expect when base is optional, because it could also be
    // an IUO which requires a separate fix.
    if (flags.contains(TMF_ApplyingFix))
      return SolutionKind::Error;

    // Check if any property wrappers on the base of the member lookup have
    // matching members that we can fall back to, or if the type wraps any
    // properties that have matching members.
    if (auto *fix = fixPropertyWrapperFailure(
            *this, baseTy, locator,
            [&](SelectedOverload overload, VarDecl *decl, Type newBase) {
              return solveWithNewBaseOrName(newBase, member) ==
                     SolutionKind::Solved;
            })) {
      return recordFix(fix) ? SolutionKind::Error : SolutionKind::Solved;
    }

    // If base is an archetype or metatype of archetype, check for an unintended
    // extra generic parameter.
    if (auto archetype =
            baseTy->getMetatypeInstanceType()->getAs<ArchetypeType>()) {
      if (auto genericTy =
              archetype->mapTypeOutOfContext()->getAs<GenericTypeParamType>()) {
        for (auto param : DC->getGenericSignatureOfContext()
                              .getGenericParams()) {
          // Find a param at the same depth and one index past the type we're
          // dealing with
          if (param->getDepth() != genericTy->getDepth() ||
              param->getIndex() != genericTy->getIndex() + 1)
            continue;
          auto paramDecl = param->getDecl();
          if (!paramDecl)
            continue;

          auto descriptor = UnqualifiedLookupDescriptor{
              DeclNameRef(param->getName()),
              paramDecl->getDeclContext()->getModuleScopeContext(),
              SourceLoc(),
              UnqualifiedLookupFlags::TypeLookup};
          auto lookup = evaluateOrDefault(
              Context.evaluator, UnqualifiedLookupRequest{descriptor}, {});
          for (auto &result : lookup) {
            if (auto proto =
                    dyn_cast_or_null<ProtocolDecl>(result.getValueDecl())) {
              auto result =
                  baseTy->is<MetatypeType>()
                      ? solveWithNewBaseOrName(ExistentialMetatypeType::get(
                                                   proto->getDeclaredInterfaceType()),
                                               member)
                      : solveWithNewBaseOrName(proto->getDeclaredInterfaceType(),
                                               member);
              if (result == SolutionKind::Solved)
                return recordFix(
                           DefineMemberBasedOnUnintendedGenericParam::create(
                               *this, baseTy, member, param->getName(),
                               locator))
                           ? SolutionKind::Error
                           : SolutionKind::Solved;
            }
          }
        }
      }
    }

    if (auto *funcType = baseTy->getAs<FunctionType>()) {
      // We can't really suggest anything useful unless
      // function takes no arguments, otherwise it
      // would make sense to report this a missing member.
      if (funcType->getNumParams() == 0) {
        auto result = solveWithNewBaseOrName(funcType->getResult(), member);
        // If there is indeed a member with given name in result type
        // let's return, otherwise let's fall-through and report
        // this problem as a missing member.
        if (result == SolutionKind::Solved)
          return recordFix(InsertExplicitCall::create(
                     *this, getConstraintLocator(
                                locator, ConstraintLocator::MemberRefBase)))
                     ? SolutionKind::Error
                     : SolutionKind::Solved;
      }
    }

    // Instead of using subscript operator spelled out `subscript` directly.
    if (member.getBaseName() == getTokenText(tok::kw_subscript)) {
      auto result =
          solveWithNewBaseOrName(baseTy, DeclNameRef::createSubscript());
      // Looks like it was indeed meant to be a subscript operator.
      if (result == SolutionKind::Solved)
        return recordFix(UseSubscriptOperator::create(*this, locator))
                   ? SolutionKind::Error
                   : SolutionKind::Solved;
    }

    // FIXME(diagnostics): This is more of a hack than anything.
    // Let's not try to suggest that there is no member related to an
    // obscure underscored type, the real problem would be somewhere
    // else. This helps to diagnose pattern matching cases.
    {
      if (auto *metatype = baseTy->getAs<MetatypeType>()) {
        auto instanceTy = metatype->getInstanceType();
        if (auto *NTD = instanceTy->getAnyNominal()) {
          if (NTD->getName() == getASTContext().Id_OptionalNilComparisonType)
            return SolutionKind::Error;
        }
      }
    }

    result = performMemberLookup(kind, member, baseTy, functionRefKind, locator,
                                 /*includeInaccessibleMembers*/ true);

    // FIXME(diagnostics): If there were no viable results, but there are
    // unviable ones, we'd have to introduce fix for each specific problem.
    if (!result.UnviableCandidates.empty())
      return SolutionKind::Error;

    // Since member with given base and name doesn't exist, let's try to
    // fake its presence based on use, that makes it possible to diagnose
    // problems related to member lookup more precisely.

    return fixMissingMember(origBaseTy, memberTy, locator);
  }
  return SolutionKind::Error;
}

ConstraintSystem::SolutionKind
ConstraintSystem::simplifyValueWitnessConstraint(
    ConstraintKind kind, Type baseType, ValueDecl *requirement, Type memberType,
    DeclContext *useDC, FunctionRefKind functionRefKind,
    TypeMatchOptions flags, ConstraintLocatorBuilder locator) {
  // We'd need to record original base type because it might be a type
  // variable representing another missing member.
  auto origBaseType = baseType;

  auto formUnsolved = [&] {
    // If requested, generate a constraint.
    if (flags.contains(TMF_GenerateConstraints)) {
      auto *witnessConstraint = Constraint::createValueWitness(
          *this, kind, origBaseType, memberType, requirement, useDC,
          functionRefKind, getConstraintLocator(locator));

      addUnsolvedConstraint(witnessConstraint);
      return SolutionKind::Solved;
    }

    return SolutionKind::Unsolved;
  };

  auto fail = [&] {
    // The constraint failed, so mark the member type as a "hole".
    // We cannot do anything further here.
    if (!shouldAttemptFixes())
      return SolutionKind::Error;

    recordAnyTypeVarAsPotentialHole(memberType);

    return SolutionKind::Solved;
  };

  // Resolve the base type, if we can. If we can't resolve the base type,
  // then we can't solve this constraint.
  Type baseObjectType = getFixedTypeRecursive(
      baseType, flags, /*wantRValue=*/true);
  if (baseObjectType->isTypeVariableOrMember()) {
    return formUnsolved();
  }

  // If base type is an existential, let's open it before checking
  // conformance.
  if (baseObjectType->isExistentialType()) {
    baseObjectType =
        OpenedArchetypeType::get(baseObjectType->getCanonicalType(),
                                 useDC->getGenericSignatureOfContext());
  }

  // Check conformance to the protocol. If it doesn't conform, this constraint
  // fails. Don't attempt to fix it.
  // FIXME: Look in the constraint system to see if we've resolved the
  // conformance already?
  auto proto = requirement->getDeclContext()->getSelfProtocolDecl();
  assert(proto && "Value witness constraint for a non-requirement");
  auto conformance = useDC->getParentModule()->lookupConformance(
      baseObjectType, proto);
  if (!conformance)
    return fail();

  // Reference the requirement.
  Type resolvedBaseType = simplifyType(baseType, flags);
  if (resolvedBaseType->isTypeVariableOrMember())
    return formUnsolved();

  auto witness =
      conformance.getWitnessByName(baseObjectType, requirement->getName());
  if (!witness)
    return fail();

  auto choice = OverloadChoice(resolvedBaseType, witness.getDecl(), functionRefKind);
  resolveOverload(getConstraintLocator(locator), memberType, choice,
                  useDC);
  return SolutionKind::Solved;
}

ConstraintSystem::SolutionKind ConstraintSystem::simplifyDefaultableConstraint(
    Type first, Type second, TypeMatchOptions flags,
    ConstraintLocatorBuilder locator) {
  first = getFixedTypeRecursive(first, flags, true);

  if (first->isTypeVariableOrMember()) {
    if (flags.contains(TMF_GenerateConstraints)) {
      addUnsolvedConstraint(
          Constraint::create(*this, ConstraintKind::Defaultable, first, second,
                             getConstraintLocator(locator)));
      return SolutionKind::Solved;
    }

    return SolutionKind::Unsolved;
  }

  // Otherwise, any type is fine.
  return SolutionKind::Solved;
}

ConstraintSystem::SolutionKind
ConstraintSystem::simplifyDefaultClosureTypeConstraint(
    Type closureType, Type inferredType,
    ArrayRef<TypeVariableType *> referencedOuterParameters,
    TypeMatchOptions flags, ConstraintLocatorBuilder locator) {
  closureType = getFixedTypeRecursive(closureType, flags, /*wantRValue=*/true);

  if (closureType->isTypeVariableOrMember()) {
    if (flags.contains(TMF_GenerateConstraints)) {
      addUnsolvedConstraint(Constraint::create(
          *this, ConstraintKind::DefaultClosureType, closureType, inferredType,
          getConstraintLocator(locator), referencedOuterParameters));
      return SolutionKind::Solved;
    }

    return SolutionKind::Unsolved;
  }

  // Otherwise, any type is fine.
  return SolutionKind::Solved;
}

ConstraintSystem::SolutionKind
ConstraintSystem::simplifyPropertyWrapperConstraint(
    Type wrapperType, Type wrappedValueType, TypeMatchOptions flags,
    ConstraintLocatorBuilder locator) {
  wrapperType = getFixedTypeRecursive(wrapperType, flags, /*wantRValue=*/true);
  auto *loc = getConstraintLocator(locator);

  if (wrapperType->isTypeVariableOrMember()) {
    if (flags.contains(TMF_GenerateConstraints)) {
      addUnsolvedConstraint(Constraint::create(
          *this, ConstraintKind::PropertyWrapper, wrapperType, wrappedValueType, loc));

      return SolutionKind::Solved;
    }

    return SolutionKind::Unsolved;
  }

  // If the wrapper type is a hole or a dependent member with no type variables,
  // don't record a fix, because this indicates that there was an error
  // elsewhere in the constraint system.
  if (wrapperType->isPlaceholder() || wrapperType->is<DependentMemberType>())
    return SolutionKind::Solved;

  auto *wrappedVar = getAsDecl<VarDecl>(locator.getAnchor());
  assert(wrappedVar && wrappedVar->hasAttachedPropertyWrapper());

  // The wrapper type must be a property wrapper.
  auto *nominal = wrapperType->getDesugaredType()->getAnyNominal();
  if (!(nominal && nominal->getAttrs().hasAttribute<PropertyWrapperAttr>())) {
    if (shouldAttemptFixes()) {
      auto *fix = AllowInvalidPropertyWrapperType::create(
          *this, wrapperType, getConstraintLocator(locator));
      if (!recordFix(fix))
        return SolutionKind::Solved;
    }

    return SolutionKind::Error;
  }

  auto typeInfo = nominal->getPropertyWrapperTypeInfo();

  // Implicit property wrappers must support projected-value initialization.
  if (wrappedVar->hasImplicitPropertyWrapper() &&
      !(typeInfo.projectedValueVar && typeInfo.hasProjectedValueInit)) {
    if (shouldAttemptFixes()) {
      auto *fix = RemoveProjectedValueArgument::create(
          *this, wrapperType, cast<ParamDecl>(wrappedVar), getConstraintLocator(locator));
      if (!recordFix(fix))
        return SolutionKind::Solved;
    }

    return SolutionKind::Error;
  }

  auto resolvedType = wrapperType->getTypeOfMember(DC->getParentModule(), typeInfo.valueVar);
  if (typeInfo.valueVar->isSettable(nullptr) && typeInfo.valueVar->isSetterAccessibleFrom(DC) &&
      !typeInfo.valueVar->isSetterMutating()) {
    resolvedType = LValueType::get(resolvedType);
  }

  addConstraint(ConstraintKind::Bind, wrappedValueType, resolvedType, locator);

  return SolutionKind::Solved;
}

ConstraintSystem::SolutionKind
ConstraintSystem::simplifyOneWayConstraint(
    ConstraintKind kind,
    Type first, Type second, TypeMatchOptions flags,
    ConstraintLocatorBuilder locator) {
  // Determine whether the second type can be fully simplified. Only then
  // will this constraint be resolved.
  Type secondSimplified = simplifyType(second);
  if (secondSimplified->hasTypeVariable()) {
    if (flags.contains(TMF_GenerateConstraints)) {
      addUnsolvedConstraint(
        Constraint::create(*this, kind, first, second,
                           getConstraintLocator(locator)));
      return SolutionKind::Solved;
    }

    return SolutionKind::Unsolved;
  }

  // Propagate holes through one-way constraints.
  if (secondSimplified->isPlaceholder()) {
    recordAnyTypeVarAsPotentialHole(first);
    return SolutionKind::Solved;
  }

  // Translate this constraint into an equality or bind-parameter constraint,
  // as appropriate.
  if (kind == ConstraintKind::OneWayEqual) {
    return matchTypes(first, secondSimplified, ConstraintKind::Equal, flags,
                      locator);
  }

  assert(kind == ConstraintKind::OneWayBindParam);
  return matchTypes(
      secondSimplified, first, ConstraintKind::BindParam, flags, locator);
}

ConstraintSystem::SolutionKind
ConstraintSystem::simplifyUnresolvedMemberChainBaseConstraint(
    Type first, Type second, TypeMatchOptions flags,
    ConstraintLocatorBuilder locator) {
  auto resultTy = getFixedTypeRecursive(first, flags, /*wantRValue=*/true);
  auto baseTy = getFixedTypeRecursive(second, flags, /*wantRValue=*/true);

  if (baseTy->isTypeVariableOrMember() || resultTy->isTypeVariableOrMember()) {
    if (flags.contains(TMF_GenerateConstraints)) {
      addUnsolvedConstraint(
          Constraint::create(*this, ConstraintKind::UnresolvedMemberChainBase,
                             first, second, getConstraintLocator(locator)));
      return SolutionKind::Solved;
    }

    return SolutionKind::Unsolved;
  }

  if (baseTy->is<ProtocolType>()) {
    auto *baseExpr =
        castToExpr<UnresolvedMemberChainResultExpr>(locator.getAnchor())
            ->getChainBase();
    auto *memberLoc =
        getConstraintLocator(baseExpr, ConstraintLocator::UnresolvedMember);

    if (shouldAttemptFixes() && hasFixFor(memberLoc))
      return SolutionKind::Solved;

    auto *memberRef = findResolvedMemberRef(memberLoc);
    if (memberRef && (memberRef->isStatic() || isa<TypeAliasDecl>(memberRef))) {
      return simplifyConformsToConstraint(
          resultTy, baseTy, ConstraintKind::ConformsTo,
          getConstraintLocator(memberLoc, ConstraintLocator::MemberRefBase),
          flags);
    }
  }

  return matchTypes(baseTy, resultTy, ConstraintKind::Equal, flags, locator);
}

static Type getOpenedResultBuilderTypeFor(ConstraintSystem &cs,
                                            ConstraintLocatorBuilder locator) {
  auto lastElt = locator.last();
  if (!lastElt)
    return Type();

  auto argToParamElt = lastElt->getAs<LocatorPathElt::ApplyArgToParam>();
  if (!argToParamElt)
    return Type();

  auto *calleeLocator = cs.getCalleeLocator(cs.getConstraintLocator(locator));
  auto selectedOverload = cs.findSelectedOverloadFor(calleeLocator);
  if (!(selectedOverload &&
        (selectedOverload->choice.getKind() == OverloadChoiceKind::Decl ||
         selectedOverload->choice.getKind() ==
             OverloadChoiceKind::DeclViaUnwrappedOptional)))
    return Type();

  auto *choice = selectedOverload->choice.getDecl();
  bool skipCurriedSelf = hasAppliedSelf(cs, selectedOverload->choice);

  if (choice->hasCurriedSelf() && !skipCurriedSelf)
    return Type();

  if (!choice->hasParameterList())
    return Type();

  auto *PD = getParameterAt(choice, argToParamElt->getParamIdx());
  assert(PD);

  auto builderType = PD->getResultBuilderType();
  if (!builderType)
    return Type();

  // If the builder type has a type parameter, substitute in the type
  // variables.
  if (builderType->hasTypeParameter()) {
    // Find the opened type for this callee and substitute in the type
    // parameters.
    auto substitutions = cs.getOpenedTypes(calleeLocator);
    if (!substitutions.empty()) {
      OpenedTypeMap replacements(substitutions.begin(), substitutions.end());
      builderType = cs.openType(builderType, replacements, locator);
    }
    assert(!builderType->hasTypeParameter());
  }
  return builderType;
}

bool ConstraintSystem::resolveClosure(TypeVariableType *typeVar,
                                      Type contextualType,
                                      ConstraintLocatorBuilder locator) {
  auto *closureLocator = typeVar->getImpl().getLocator();
  auto *closure = castToExpr<ClosureExpr>(closureLocator->getAnchor());
  auto *inferredClosureType = getClosureType(closure);

  // Note if this closure is isolated by preconcurrency.
  if (hasPreconcurrencyCallee(locator))
    preconcurrencyClosures.insert(closure);

  // Let's look through all optionals associated with contextual
  // type to make it possible to infer parameter/result type of
  // the closure faster e.g.:
  //
  // func test(_: ((Int) -> Void)?) {
  //   ...
  // }
  //
  // test { $0 + ... }
  //
  // In this case dropping optionality from contextual type
  // `((Int) -> Void)?` allows `resolveClosure` to infer type
  // of `$0` directly (via `getContextualParamAt`) instead of
  // having to use type variable inference mechanism.
  contextualType = contextualType->lookThroughAllOptionalTypes();

  auto getContextualParamAt =
      [&contextualType, &inferredClosureType](
          unsigned index) -> Optional<AnyFunctionType::Param> {
    auto *fnType = contextualType->getAs<FunctionType>();
    if (!fnType)
      return None;

    auto numContextualParams = fnType->getNumParams();

    if (numContextualParams == 1) {
      const auto &param = fnType->getParams()[0];
      if (auto *tuple = param.getPlainType()->getAs<TupleType>()) {
        // If arity is the same it's a tuple splat which is allowed
        // for closures (see SE-0110 for more details):
        //
        // func test(_: ((Int, Int)) -> Void) {}
        // test { (arg, _) in
        //   ...
        // }
        if (tuple->getNumElements() == inferredClosureType->getNumParams() &&
            param.getParameterFlags().isNone()) {
          const auto &elt = tuple->getElement(index);
          return AnyFunctionType::Param(elt.getType(), elt.getName());
        }

        return None;
      }
    }

    if (numContextualParams != inferredClosureType->getNumParams() ||
        numContextualParams <= index)
      return None;

    return fnType->getParams()[index];
  };

  // Check whether given contextual parameter type could be
  // used to bind external closure parameter type.
  auto isSuitableContextualType = [](Type contextualTy) {
    // We need to wait until contextual type
    // is fully resolved before binding it.
    if (contextualTy->isTypeVariableOrMember())
      return false;

    // Cannot propagate pack expansion type from context,
    // it has to be handled by type matching logic.
    if (isPackExpansionType(contextualTy))
      return false;

    // If contextual type has an error, let's wait for inference,
    // otherwise contextual would interfere with diagnostics.
    if (contextualTy->hasError())
      return false;

    if (isa<TypeAliasType>(contextualTy.getPointer())) {
      auto underlyingTy = contextualTy->getDesugaredType();
      // FIXME: typealias pointing to an existential type is special
      // because if the typealias has type variables then we'd end up
      // opening existential from a type with unresolved generic
      // parameter(s), which CSApply can't currently simplify while
      // building type-checked AST because `OpenedArchetypeType` doesn't
      // propagate flags. Example is as simple as `{ $0.description }`
      // where `$0` is `Error` that inferred from a (generic) typealias.
      if (underlyingTy->isExistentialType() && contextualTy->hasTypeVariable())
        return false;
    }

    return true;
  };

  // Determine whether a result builder will be applied.
  auto resultBuilderType = getOpenedResultBuilderTypeFor(*this, locator);

  // Determine whether to introduce one-way constraints between the parameter's
  // type as seen in the body of the closure and the external parameter
  // type.
  bool oneWayConstraints =
      getASTContext().LangOpts.hasFeature(Feature::OneWayClosureParameters);

  auto *paramList = closure->getParameters();
  SmallVector<AnyFunctionType::Param, 4> parameters;
  for (unsigned i = 0, n = paramList->size(); i != n; ++i) {
    auto param = inferredClosureType->getParams()[i];
    auto *paramDecl = paramList->get(i);

    // In case of anonymous or name-only parameters, let's infer
    // inout/variadic/isolated flags from context, that helps to propagate
    // type information into the internal type of the parameter and reduces
    // inference solver has to make.
    if (!paramDecl->getTypeRepr()) {
      if (auto contextualParam = getContextualParamAt(i)) {
        auto flags = param.getParameterFlags();

        // Note when a parameter is inferred to be isolated.
        if (contextualParam->isIsolated() && !flags.isIsolated() && paramDecl)
          isolatedParams.insert(paramDecl);

        param =
            param.withFlags(flags.withInOut(contextualParam->isInOut())
                                 .withVariadic(contextualParam->isVariadic())
                                 .withIsolated(contextualParam->isIsolated()));
      }
    }

    if (paramDecl->hasAttachedPropertyWrapper()) {
      Type backingType;
      Type wrappedValueType;

      if (paramDecl->hasImplicitPropertyWrapper()) {
        if (auto contextualType = getContextualParamAt(i)) {
          backingType = contextualType->getPlainType();
        } else {
          // There may not be a contextual parameter type if the contextual
          // type is not a function type or if closure body declares too many
          // parameters.
          auto *paramLoc =
              getConstraintLocator(closure, LocatorPathElt::TupleElement(i));
          backingType = createTypeVariable(paramLoc, TVO_CanBindToHole);
        }

        wrappedValueType = createTypeVariable(getConstraintLocator(paramDecl),
                                              TVO_CanBindToHole | TVO_CanBindToLValue);
      } else {
        auto *wrapperAttr = paramDecl->getOutermostAttachedPropertyWrapper();
        auto wrapperType = paramDecl->getAttachedPropertyWrapperType(0);
        backingType = replaceInferableTypesWithTypeVars(
            wrapperType, getConstraintLocator(wrapperAttr->getTypeRepr()));
        wrappedValueType = computeWrappedValueType(paramDecl, backingType);
      }

      auto *backingVar = paramDecl->getPropertyWrapperBackingProperty();
      setType(backingVar, backingType);

      auto *localWrappedVar = paramDecl->getPropertyWrapperWrappedValueVar();
      setType(localWrappedVar, wrappedValueType);

      if (auto *projection = paramDecl->getPropertyWrapperProjectionVar()) {
        setType(projection, computeProjectedValueType(paramDecl, backingType));
      }

      if (!paramDecl->getName().hasDollarPrefix()) {
        if (generateWrappedPropertyTypeConstraints(paramDecl, backingType,
                                                   param.getParameterType()))
          return false;
      }

      auto result = applyPropertyWrapperToParameter(backingType, param.getParameterType(),
                                                    paramDecl, paramDecl->getName(),
                                                    ConstraintKind::Equal,
                                                    getConstraintLocator(closure));
      if (result.isFailure())
        return false;
    }

    Type internalType;
    if (paramDecl->getTypeRepr()) {
      // Internal type is the type used in the body of the closure,
      // so "external" type translates to it as follows:
      //  - `Int...` -> `[Int]`,
      //  - `inout Int` -> `@lvalue Int`.
      internalType = param.getParameterType();

      // When there are type variables in the type and we have enabled
      // one-way constraints, create a fresh type variable to handle the
      // binding.
      if (oneWayConstraints && internalType->hasTypeVariable()) {
        auto *paramLoc =
            getConstraintLocator(closure, LocatorPathElt::TupleElement(i));
        auto *typeVar = createTypeVariable(paramLoc, TVO_CanBindToLValue |
                                                         TVO_CanBindToNoEscape);
        addConstraint(
            ConstraintKind::OneWayBindParam, typeVar, internalType, paramLoc);
        internalType = typeVar;
      }
    } else {
      auto *paramLoc =
          getConstraintLocator(closure, LocatorPathElt::TupleElement(i));

      auto *typeVar = createTypeVariable(paramLoc, TVO_CanBindToLValue |
                                                       TVO_CanBindToNoEscape);

      // If external parameter is variadic it translates into an array in
      // the body of the closure.
      internalType =
          param.isVariadic() ? VariadicSequenceType::get(typeVar) : Type(typeVar);

      auto externalType = param.getOldType();

      // Performance optimization.
      //
      // If there is a concrete contextual type we could use, let's bind
      // it to the external type right away because internal type has to
      // be equal to that type anyway (through `BindParam` on external type
      // i.e. <internal> bind param <external> conv <concrete contextual>).
      //
      // Note: it's correct to avoid doing this, but it would result
      // in (a lot) more checking since solver would have to re-discover,
      // re-attempt and fail parameter type while solving for overloaded
      // choices in the body.
      if (auto contextualParam = getContextualParamAt(i)) {
        auto paramTy = simplifyType(contextualParam->getOldType());
        if (isSuitableContextualType(paramTy))
          addConstraint(ConstraintKind::Bind, externalType, paramTy, paramLoc);
      }

      if (oneWayConstraints) {
        addConstraint(
            ConstraintKind::OneWayBindParam, typeVar, externalType, paramLoc);
      } else {
        addConstraint(
            ConstraintKind::BindParam, externalType, typeVar, paramLoc);
      }
    }

    setType(paramDecl, internalType);
    parameters.push_back(param);
  }

  // Propagate @Sendable from the contextual type to the closure.
  auto closureExtInfo = inferredClosureType->getExtInfo();
  if (auto contextualFnType = contextualType->getAs<FunctionType>()) {
    if (contextualFnType->isSendable())
      closureExtInfo = closureExtInfo.withConcurrent();
  }

  auto closureType =
      FunctionType::get(parameters, inferredClosureType->getResult(),
                        closureExtInfo);
  assignFixedType(typeVar, closureType, closureLocator);

  // If there is a result builder to apply, do so now.
  if (resultBuilderType) {
    if (auto result = matchResultBuilder(
            closure, resultBuilderType, closureType->getResult(),
            ConstraintKind::Conversion, contextualType, locator)) {
      return result->isSuccess();
    }
  }

  SyntacticElementTarget target(closure, contextualType);
  setTargetFor(closure, target);

  // Generate constraints from the body of this closure.
  return !generateConstraints(AnyFunctionRef{closure}, closure->getBody());
}

bool ConstraintSystem::resolvePackExpansion(TypeVariableType *typeVar,
                                            Type contextualType) {
  auto *locator = typeVar->getImpl().getLocator();

  Type openedExpansionType;
  if (auto expansionElt =
          locator->getLastElementAs<LocatorPathElt::PackExpansionType>()) {
    openedExpansionType = expansionElt->getOpenedType();
  }

  if (!openedExpansionType)
    return false;

  assignFixedType(typeVar, openedExpansionType, locator);

  // We have a fully resolved contextual pack expansion type, let's
  // apply it right away.
  if (!contextualType->isEqual(openedExpansionType)) {
    assert(contextualType->is<PackExpansionType>() &&
           !contextualType->hasTypeVariable());
    auto result = matchTypes(openedExpansionType, contextualType,
                             ConstraintKind::Equal, {}, locator);
    return !result.isFailure();
  }

  return true;
}

ConstraintSystem::SolutionKind
ConstraintSystem::simplifyDynamicTypeOfConstraint(
                                        Type type1, Type type2,
                                        TypeMatchOptions flags,
                                        ConstraintLocatorBuilder locator) {
  TypeMatchOptions subflags = getDefaultDecompositionOptions(flags);

  // Local function to form an unsolved result.
  auto formUnsolved = [&] {
    if (flags.contains(TMF_GenerateConstraints)) {
      addUnsolvedConstraint(
        Constraint::create(*this, ConstraintKind::DynamicTypeOf, type1, type2,
                           getConstraintLocator(locator)));
      return SolutionKind::Solved;
    }

    return SolutionKind::Unsolved;
  };

  // Solve forward.
  type2 = getFixedTypeRecursive(type2, flags, /*wantRValue=*/true);
  if (!type2->isTypeVariableOrMember()) {
    Type dynamicType2;
    if (type2->isAnyExistentialType()) {
      dynamicType2 = ExistentialMetatypeType::get(type2);
    } else {
      dynamicType2 = MetatypeType::get(type2);
    }
    return matchTypes(type1, dynamicType2, ConstraintKind::Bind, subflags,
                      locator);
  }

  // Okay, can't solve forward.  See what we can do backwards.
  type1 = getFixedTypeRecursive(type1, flags, /*wantRValue=*/true);
  if (type1->isTypeVariableOrMember())
    return formUnsolved();

  // If we have an existential metatype, that's good enough to solve
  // the constraint.
  if (auto metatype1 = type1->getAs<ExistentialMetatypeType>())
    return matchTypes(metatype1->getInstanceType(), type2,
                      ConstraintKind::Bind,
                      subflags, locator);

  // If we have a normal metatype, we can't solve backwards unless we
  // know what kind of object it is.
  if (auto metatype1 = type1->getAs<MetatypeType>()) {
    Type instanceType1 = getFixedTypeRecursive(metatype1->getInstanceType(),
                                               true);
    if (instanceType1->isTypeVariableOrMember())
      return formUnsolved();

    return matchTypes(instanceType1, type2, ConstraintKind::Bind, subflags,
                      locator);
  }

  // It's definitely not either kind of metatype, so we can
  // report failure right away.
  return SolutionKind::Error;
}

ConstraintSystem::SolutionKind
ConstraintSystem::simplifyBridgingConstraint(Type type1,
                                             Type type2,
                                             TypeMatchOptions flags,
                                             ConstraintLocatorBuilder locator) {
  TypeMatchOptions subflags = getDefaultDecompositionOptions(flags);

  /// Form an unresolved result.
  auto formUnsolved = [&] {
    if (flags.contains(TMF_GenerateConstraints)) {
      addUnsolvedConstraint(
        Constraint::create(*this, ConstraintKind::BridgingConversion, type1,
                           type2, getConstraintLocator(locator)));
      return SolutionKind::Solved;
    }
    
    return SolutionKind::Unsolved;
  };

  // Local function to look through optional types. It produces the
  // fully-unwrapped type and a count of the total # of optional types that were
  // unwrapped.
  auto unwrapType = [&](Type type) -> std::pair<Type, unsigned> {
    unsigned count = 0;
    while (Type objectType = type->getOptionalObjectType()) {
      ++count;

      TypeMatchOptions unusedOptions;
      type = getFixedTypeRecursive(objectType, unusedOptions, /*wantRValue=*/true);
    }

    return { type, count };
  };

  const auto rawType1 = type1;
  type1 = getFixedTypeRecursive(type1, flags, /*wantRValue=*/true);
  type2 = getFixedTypeRecursive(type2, flags, /*wantRValue=*/true);

  if (type1->isTypeVariableOrMember() || type2->isTypeVariableOrMember())
    return formUnsolved();

  // Move-only types can't be involved in a bridging conversion since a bridged
  // type assumes the ability to copy.
  if (type1->isPureMoveOnly()) {
    return SolutionKind::Error;
  }

  Type unwrappedFromType;
  unsigned numFromOptionals;
  std::tie(unwrappedFromType, numFromOptionals) = unwrapType(type1);

  Type unwrappedToType;
  unsigned numToOptionals;
  std::tie(unwrappedToType, numToOptionals) = unwrapType(type2);

  if (unwrappedFromType->isTypeVariableOrMember() ||
      unwrappedToType->isTypeVariableOrMember())
    return formUnsolved();

  // Update the score.
  increaseScore(SK_UserConversion); // FIXME: Use separate score kind?
  if (worseThanBestSolution()) {
    return SolutionKind::Error;
  }

  // Local function to count the optional injections that will be performed
  // after the bridging conversion.
  auto countOptionalInjections = [&] {
    if (numToOptionals > numFromOptionals)
      increaseScore(SK_ValueToOptional, numToOptionals - numFromOptionals);
  };

  // Anything can be explicitly converted to AnyObject using the universal
  // bridging conversion. This allows both extraneous optionals in the source
  // (because optionals themselves can be boxed for AnyObject) and in the
  // destination (we'll perform the extra injections at the end).
  if (unwrappedToType->isAnyObject()) {
    countOptionalInjections();
    return SolutionKind::Solved;
  }

  // In a previous version of Swift, we could accidently drop the coercion
  // constraint in certain cases. In most cases this led to either miscompiles
  // or crashes later down the pipeline, but for coercions between collections
  // we generated somewhat reasonable code that performed a force cast. To
  // maintain compatibility with that behavior, allow the coercion between
  // two collections, but add a warning fix telling the user to use as! or as?
  // instead. In Swift 6 mode, this becomes an error.
  //
  // We only need to perform this compatibility logic if this is a coercion of
  // something that isn't a collection expr (as collection exprs would have
  // crashed in codegen due to CSApply peepholing them). Additionally, the LHS
  // type must be a (potentially optional) type variable, as only such a
  // constraint could have been previously been left unsolved.
  auto canUseCompatFix = [&]() {
    if (Context.isSwiftVersionAtLeast(6))
      return false;

    if (!rawType1->lookThroughAllOptionalTypes()->isTypeVariableOrMember())
      return false;

    SmallVector<LocatorPathElt, 4> elts;
    auto anchor = locator.getLocatorParts(elts);
    if (elts.empty() || !elts.back().is<LocatorPathElt::CoercionOperand>())
      return false;

    auto *coercion = getAsExpr<CoerceExpr>(anchor);
    if (!coercion)
      return false;

    auto *subject = coercion->getSubExpr();
    while (auto *paren = dyn_cast<ParenExpr>(subject))
      subject = paren->getSubExpr();

    return !isa<CollectionExpr>(subject);
  }();

  // Unless we're allowing the collection compatibility fix, the source cannot
  // be more optional than the destination.
  if (!canUseCompatFix && numFromOptionals > numToOptionals)
    return SolutionKind::Error;

  auto makeCollectionResult = [&](SolutionKind result) -> SolutionKind {
    // If we encountered an error and can use the compatibility fix, do so.
    if (canUseCompatFix) {
      if (numFromOptionals > numToOptionals || result == SolutionKind::Error) {
        auto *loc = getConstraintLocator(locator);
        auto *fix = AllowCoercionToForceCast::create(*this, type1, type2, loc);
        return recordFix(fix) ? SolutionKind::Error : SolutionKind::Solved;
      }
    }
    return result;
  };

  // Bridging the elements of an array.
  if (auto fromElement = unwrappedFromType->isArrayType()) {
    if (auto toElement = unwrappedToType->isArrayType()) {
      countOptionalInjections();
      auto result = simplifyBridgingConstraint(
          fromElement, toElement, subflags,
          locator.withPathElement(LocatorPathElt::GenericArgument(0)));
      return makeCollectionResult(result);
    }
  }

  // Bridging the keys/values of a dictionary.
  if (auto fromKeyValue = isDictionaryType(unwrappedFromType)) {
    if (auto toKeyValue = isDictionaryType(unwrappedToType)) {
      ConstraintFix *compatFix = nullptr;
      if (canUseCompatFix) {
        compatFix = AllowCoercionToForceCast::create(
            *this, type1, type2, getConstraintLocator(locator));
      }
      addExplicitConversionConstraint(fromKeyValue->first, toKeyValue->first,
                                      ForgetChoice,
                                      locator.withPathElement(
                                        LocatorPathElt::GenericArgument(0)),
                                      compatFix);
      addExplicitConversionConstraint(fromKeyValue->second, toKeyValue->second,
                                      ForgetChoice,
                                      locator.withPathElement(
                                        LocatorPathElt::GenericArgument(1)),
                                      compatFix);
      countOptionalInjections();
      return makeCollectionResult(SolutionKind::Solved);
    }
  }

  // Bridging the elements of a set.
  if (auto fromElement = isSetType(unwrappedFromType)) {
    if (auto toElement = isSetType(unwrappedToType)) {
      countOptionalInjections();
      auto result = simplifyBridgingConstraint(
          *fromElement, *toElement, subflags,
          locator.withPathElement(LocatorPathElt::GenericArgument(0)));
      return makeCollectionResult(result);
    }
  }

  // The source cannot be more optional than the destination, because bridging
  // conversions don't allow us to implicitly check for a value in the optional.
  if (numFromOptionals > numToOptionals) {
    return SolutionKind::Error;
  }

  // Explicit bridging from a value type to an Objective-C class type.
  auto &ctx = getASTContext();
  if (unwrappedFromType->isPotentiallyBridgedValueType() &&
      (unwrappedToType->isBridgeableObjectType() ||
       (unwrappedToType->isExistentialType() &&
        !unwrappedToType->isAny()))) {
    countOptionalInjections();
    if (Type classType = ctx.getBridgedToObjC(DC, unwrappedFromType)) {
      return matchTypes(classType, unwrappedToType, ConstraintKind::Conversion,
                        subflags, locator);
    }
  }

  // Bridging from an Objective-C class type to a value type.
  // Note that specifically require a class or class-constrained archetype
  // here, because archetypes cannot be bridged.
  if (unwrappedFromType->mayHaveSuperclass() &&
      unwrappedToType->isPotentiallyBridgedValueType()) {
    Type bridgedValueType;
    if (auto objcClass = ctx.getBridgedToObjC(DC, unwrappedToType,
                                              &bridgedValueType)) {
      // Bridging NSNumber to NSValue is one-way, since there are multiple Swift
      // value types that bridge to those object types. It requires a checked
      // cast to get back.
      if (ctx.isObjCClassWithMultipleSwiftBridgedTypes(objcClass))
        return SolutionKind::Error;

      // If the bridged value type is generic, the generic arguments
      // must either match or be bridged.
      // FIXME: This should be an associated type of the protocol.
      auto &ctx = getASTContext();
      if (auto fromBGT = unwrappedToType->getAs<BoundGenericType>()) {
        if (fromBGT->isArray()) {
          // [AnyObject]
          addConstraint(ConstraintKind::Bind, fromBGT->getGenericArgs()[0],
                        ctx.getAnyObjectType(),
                        getConstraintLocator(locator.withPathElement(
                            LocatorPathElt::GenericArgument(0))));
        } else if (fromBGT->isDictionary()) {
          // [NSObject : AnyObject]
          auto nsObjectType = ctx.getNSObjectType();
          if (!nsObjectType) {
            // Not a bridging case. Should we detect this earlier?
            return SolutionKind::Error;
          }

          addConstraint(ConstraintKind::Bind, fromBGT->getGenericArgs()[0],
                        nsObjectType,
                        getConstraintLocator(
                          locator.withPathElement(
                            LocatorPathElt::GenericArgument(0))));

          addConstraint(ConstraintKind::Bind, fromBGT->getGenericArgs()[1],
                        ctx.getAnyObjectType(),
                        getConstraintLocator(
                          locator.withPathElement(
                            LocatorPathElt::GenericArgument(1))));
        } else if (fromBGT->isSet()) {
          auto nsObjectType = ctx.getNSObjectType();
          if (!nsObjectType) {
            // Not a bridging case. Should we detect this earlier?
            return SolutionKind::Error;
          }
          addConstraint(ConstraintKind::Bind, fromBGT->getGenericArgs()[0],
                        nsObjectType,
                        getConstraintLocator(
                          locator.withPathElement(
                            LocatorPathElt::GenericArgument(0))));
        } else {
          // Nothing special to do; matchTypes will match generic arguments.
        }
      }

      // Make sure we have the bridged value type.
      if (matchTypes(unwrappedToType, bridgedValueType, ConstraintKind::Bind,
                     subflags, locator).isFailure())
        return SolutionKind::Error;

      countOptionalInjections();
      return matchTypes(unwrappedFromType, objcClass, ConstraintKind::Subtype,
                        subflags, locator);
    }
  }

  return SolutionKind::Error;
}

ConstraintSystem::SolutionKind
ConstraintSystem::simplifyEscapableFunctionOfConstraint(
                                        Type type1, Type type2,
                                        TypeMatchOptions flags,
                                        ConstraintLocatorBuilder locator) {
  TypeMatchOptions subflags = getDefaultDecompositionOptions(flags);

  // Local function to form an unsolved result.
  auto formUnsolved = [&] {
    if (flags.contains(TMF_GenerateConstraints)) {
      addUnsolvedConstraint(
        Constraint::create(*this, ConstraintKind::EscapableFunctionOf,
                           type1, type2, getConstraintLocator(locator)));
      return SolutionKind::Solved;
    }

    return SolutionKind::Unsolved;
  };

  type2 = getFixedTypeRecursive(type2, flags, /*wantRValue=*/true);
  if (auto fn2 = type2->getAs<FunctionType>()) {
    // Solve forward by binding the other type variable to the escapable
    // variation of this type.
    auto fn1 = fn2->withExtInfo(fn2->getExtInfo().withNoEscape(false));
    return matchTypes(type1, fn1, ConstraintKind::Bind, subflags, locator);
  }
  if (!type2->isTypeVariableOrMember())
    // We definitely don't have a function, so bail.
    return SolutionKind::Error;
  
  type1 = getFixedTypeRecursive(type1, flags, /*wantRValue=*/true);
  if (auto fn1 = type1->getAs<FunctionType>()) {
    // We should have the escaping end of the relation.
    if (fn1->getExtInfo().isNoEscape())
      return SolutionKind::Error;
    
    // Solve backward by binding the other type variable to the noescape
    // variation of this type.
    auto fn2 = fn1->withExtInfo(fn1->getExtInfo().withNoEscape(true));
    return matchTypes(type2, fn2, ConstraintKind::Bind, subflags, locator);
  }
  if (!type1->isTypeVariableOrMember())
    // We definitely don't have a function, so bail.
    return SolutionKind::Error;

  return formUnsolved();
}

ConstraintSystem::SolutionKind
ConstraintSystem::simplifyOpenedExistentialOfConstraint(
                                        Type type1, Type type2,
                                        TypeMatchOptions flags,
                                        ConstraintLocatorBuilder locator) {
  TypeMatchOptions subflags = getDefaultDecompositionOptions(flags);
  type2 = getFixedTypeRecursive(type2, flags, /*wantRValue=*/true);
  if (type2->isAnyExistentialType()) {
    // We have the existential side. Produce an opened archetype and bind
    // type1 to it.
    Type openedTy = openExistentialType(type2, getConstraintLocator(locator))
        .first;
    return matchTypes(type1, openedTy, ConstraintKind::Bind, subflags, locator);
  }
  if (!type2->isTypeVariableOrMember())
    // We definitely don't have an existential, so bail.
    return SolutionKind::Error;
  
  // If type1 is constrained to anything concrete, the constraint fails.
  // It can only be bound to a type we opened for it.
  type1 = getFixedTypeRecursive(type1, flags, /*wantRValue=*/true);
  if (!type1->isTypeVariableOrMember())
    return SolutionKind::Error;
  
  if (flags.contains(TMF_GenerateConstraints)) {
    addUnsolvedConstraint(
      Constraint::create(*this, ConstraintKind::OpenedExistentialOf,
                         type1, type2, getConstraintLocator(locator)));
    return SolutionKind::Solved;
  }
  return SolutionKind::Unsolved;
}

ConstraintSystem::SolutionKind
ConstraintSystem::simplifyKeyPathConstraint(
    Type keyPathTy,
    Type rootTy,
    Type valueTy,
    ArrayRef<TypeVariableType *> componentTypeVars,
    TypeMatchOptions flags,
    ConstraintLocatorBuilder locator) {
  auto subflags = getDefaultDecompositionOptions(flags);
  // The constraint ought to have been anchored on a KeyPathExpr.
  auto keyPath = castToExpr<KeyPathExpr>(locator.getBaseLocator()->getAnchor());
  keyPathTy = getFixedTypeRecursive(keyPathTy, /*want rvalue*/ true);
  bool definitelyFunctionType = false;
  bool definitelyKeyPathType = false;
  bool resolveAsMultiArgFuncFix = false;

  auto tryMatchRootAndValueFromType = [&](Type type,
                                          bool allowPartial = true) -> bool {
    Type boundRoot = Type(), boundValue = Type();

    if (auto bgt = type->getAs<BoundGenericType>()) {
      definitelyKeyPathType = true;

      // We can get root and value from a concrete key path type.
      if (bgt->isKeyPath() ||
          bgt->isWritableKeyPath() ||
          bgt->isReferenceWritableKeyPath()) {
        boundRoot = bgt->getGenericArgs()[0];
        boundValue = bgt->getGenericArgs()[1];
      } else if (bgt->isPartialKeyPath()) {
        if (!allowPartial)
          return false;

        // We can still get the root from a PartialKeyPath.
        boundRoot = bgt->getGenericArgs()[0];
      }
    }

    if (auto fnTy = type->getAs<FunctionType>()) {
      if (fnTy->getParams().size() != 1) {
        if (!shouldAttemptFixes())
          return false;

        resolveAsMultiArgFuncFix = true;
        auto *fix = AllowMultiArgFuncKeyPathMismatch::create(
            *this, fnTy, locator.getBaseLocator());
        // Pretend the keypath type got resolved and move on.
        return !recordFix(fix);
      }

      definitelyFunctionType = true;

      // Match up the root and value types to the function's param and return
      // types. Note that we're using the type of the parameter as referenced
      // from inside the function body as we'll be transforming the code into:
      // { root in root[keyPath: kp] }.
      boundRoot = fnTy->getParams()[0].getParameterType();
      boundValue = fnTy->getResult();
    }

    if (boundRoot &&
        matchTypes(boundRoot, rootTy, ConstraintKind::Bind, subflags, locator)
            .isFailure())
      return false;

    if (boundValue &&
        matchTypes(boundValue, valueTy, ConstraintKind::Bind, subflags, locator)
            .isFailure())
      return false;

    return true;
  };

  // If we have a hole somewhere in the key path, the solver won't be able to
  // infer the key path type. So let's just assume this is solved.
  if (shouldAttemptFixes()) {
    if (keyPathTy->isPlaceholder())
      return SolutionKind::Solved;

    // If we have a malformed KeyPathExpr e.g. let _: KeyPath<A, C> = \A
    // let's record a AllowKeyPathMissingComponent fix.
    if (keyPath->hasSingleInvalidComponent()) {
      auto *fix = AllowKeyPathWithoutComponents::create(
          *this, getConstraintLocator(locator));
      return recordFix(fix) ? SolutionKind::Error : SolutionKind::Solved;
    }

    // If the root type has been bound to a hole, we cannot infer it.
    if (getFixedTypeRecursive(rootTy, /*wantRValue*/ true)->isPlaceholder())
      return SolutionKind::Solved;

    // If we have e.g a missing member somewhere, a component type variable
    // would either be marked as a potential hole or would have a fix.
    if (llvm::any_of(componentTypeVars, [&](TypeVariableType *tv) {
          auto *locator = tv->getImpl().getLocator();

          // Result type of a component could be bound to a contextual
          // (concrete) type if it's the last component in the chain,
          // so the only way to detect errors is to check for fixes.
          if (locator->isForKeyPathComponentResult()) {
            auto path = locator->getPath();
            auto *componentLoc =
                getConstraintLocator(locator->getAnchor(), path.drop_back());

            if (hasFixFor(componentLoc, FixKind::DefineMemberBasedOnUse) ||
                hasFixFor(componentLoc, FixKind::UnwrapOptionalBase) ||
                hasFixFor(componentLoc,
                          FixKind::UnwrapOptionalBaseWithOptionalResult))
              return true;
          }

          // If something inside of a component is marked as a hole,
          // let's consider while component to be invalid.
          return locator->isInKeyPathComponent() &&
                 tv->getImpl().canBindToHole();
        })) {
      return SolutionKind::Solved;
    }
  }

  // If we're fixed to a bound generic type, trying harvesting context from it.
  // However, we don't want a solution that fixes the expression type to
  // PartialKeyPath; we'd rather that be represented using an upcast conversion.
  if (!tryMatchRootAndValueFromType(keyPathTy, /*allowPartial=*/false))
    return SolutionKind::Error;

  // If the expression has contextual type information, try using that too.
  if (auto contextualTy = getContextualType(keyPath, /*forConstraint=*/false)) {
    if (!tryMatchRootAndValueFromType(contextualTy))
      return SolutionKind::Error;
  }

  // If we fix this keypath as `AllowMultiArgFuncKeyPathMismatch`, just proceed
  if (resolveAsMultiArgFuncFix)
    return SolutionKind::Solved;

  // See if we resolved overloads for all the components involved.
  enum {
    ReadOnly,
    Writable,
    ReferenceWritable
  } capability = Writable;

  bool anyComponentsUnresolved = false;
  bool didOptionalChain = false;

  for (unsigned i : indices(keyPath->getComponents())) {
    auto &component = keyPath->getComponents()[i];
    
    switch (component.getKind()) {
    case KeyPathExpr::Component::Kind::Invalid:
    case KeyPathExpr::Component::Kind::Identity:
      break;

    case KeyPathExpr::Component::Kind::CodeCompletion: {
      anyComponentsUnresolved = true;
      capability = ReadOnly;
      break;
    }
    case KeyPathExpr::Component::Kind::Property:
    case KeyPathExpr::Component::Kind::Subscript:
    case KeyPathExpr::Component::Kind::UnresolvedProperty:
    case KeyPathExpr::Component::Kind::UnresolvedSubscript: {
      auto *componentLoc = getConstraintLocator(
          locator.withPathElement(LocatorPathElt::KeyPathComponent(i)));
      auto *calleeLoc = getCalleeLocator(componentLoc);
      auto overload = findSelectedOverloadFor(calleeLoc);

      // If no choice was made, leave the constraint unsolved. But when
      // generating constraints, we may already have enough information
      // to determine whether the result will be a function type vs BGT KeyPath
      // type, so continue through components to create new constraint at the
      // end.
      if (!overload) {
        if (flags.contains(TMF_GenerateConstraints)) {
          anyComponentsUnresolved = true;
          continue;
        }
        return SolutionKind::Unsolved;
      }

      // tuple elements do not change the capability of the key path
      auto choice = overload->choice;
      if (choice.getKind() == OverloadChoiceKind::TupleIndex) {
        continue;
      }
        
      // Discarded unsupported non-decl member lookups.
      if (!choice.isDecl()) {
        return SolutionKind::Error;
      }

      auto storage = dyn_cast<AbstractStorageDecl>(choice.getDecl());

      if (auto *fix = AllowInvalidRefInKeyPath::forRef(
              *this, choice.getDecl(), calleeLoc)) {
        if (!hasFixFor(calleeLoc, FixKind::AllowTypeOrInstanceMember))
          if (!shouldAttemptFixes() || recordFix(fix))
            return SolutionKind::Error;

        // If this was a method reference let's mark it as read-only.
        if (!storage) {
          capability = ReadOnly;
          continue;
        }
      }

      if (!storage)
        return SolutionKind::Error;

      if (isReadOnlyKeyPathComponent(storage, component.getLoc())) {
        capability = ReadOnly;
        continue;
      }

      // A nonmutating setter indicates a reference-writable base.
      if (!storage->isSetterMutating()) {
        capability = ReferenceWritable;
        continue;
      }

      // Otherwise, the key path maintains its current capability.
      break;
    }
    
    case KeyPathExpr::Component::Kind::OptionalChain:
      didOptionalChain = true;
      break;
    
    case KeyPathExpr::Component::Kind::OptionalForce:
      // Forcing an optional preserves its lvalue-ness.
      break;
    
    case KeyPathExpr::Component::Kind::OptionalWrap:
      // An optional chain should already have been recorded.
      assert(didOptionalChain);
      break;

    case KeyPathExpr::Component::Kind::TupleElement:
      llvm_unreachable("not implemented");
      break;
    case KeyPathExpr::Component::Kind::DictionaryKey:
      llvm_unreachable("DictionaryKey only valid in #keyPath");
      break;
    }
  }

  // Optional chains force the entire key path to be read-only.
  if (didOptionalChain)
    capability = ReadOnly;

  // Resolve the type.
  NominalTypeDecl *kpDecl;
  switch (capability) {
  case ReadOnly:
    kpDecl = getASTContext().getKeyPathDecl();
    break;

  case Writable:
    kpDecl = getASTContext().getWritableKeyPathDecl();
    break;

  case ReferenceWritable:
    kpDecl = getASTContext().getReferenceWritableKeyPathDecl();
    break;
  }
  
  // FIXME: Allow the type to be upcast if the type system has a concrete
  // KeyPath type assigned to the expression already.
  if (auto keyPathBGT = keyPathTy->getAs<BoundGenericType>()) {
    if (keyPathBGT->isKeyPath())
      kpDecl = getASTContext().getKeyPathDecl();
    else if (keyPathBGT->isWritableKeyPath() && capability >= Writable)
      kpDecl = getASTContext().getWritableKeyPathDecl();
  }

  auto loc = locator.getBaseLocator();
  if (definitelyFunctionType) {
    increaseScore(SK_FunctionConversion);
    return SolutionKind::Solved;
  } else if (!anyComponentsUnresolved ||
             (definitelyKeyPathType && capability == ReadOnly)) {
    auto resolvedKPTy =
      BoundGenericType::get(kpDecl, nullptr, {rootTy, valueTy});
    return matchTypes(keyPathTy, resolvedKPTy, ConstraintKind::Bind, subflags,
                      loc);
  } else {
    addUnsolvedConstraint(Constraint::create(*this, ConstraintKind::KeyPath,
                                             keyPathTy, rootTy, valueTy,
                                             locator.getBaseLocator(),
                                             componentTypeVars));
  }
  return SolutionKind::Solved;
}

ConstraintSystem::SolutionKind
ConstraintSystem::simplifyKeyPathApplicationConstraint(
                                        Type keyPathTy,
                                        Type rootTy,
                                        Type valueTy,
                                        TypeMatchOptions flags,
                                        ConstraintLocatorBuilder locator) {
  TypeMatchOptions subflags = getDefaultDecompositionOptions(flags);
  keyPathTy = getFixedTypeRecursive(keyPathTy, flags, /*wantRValue=*/true);
  
  auto unsolved = [&]() -> SolutionKind {
    if (flags.contains(TMF_GenerateConstraints)) {
      addUnsolvedConstraint(Constraint::create(*this,
                    ConstraintKind::KeyPathApplication,
                    keyPathTy, rootTy, valueTy, getConstraintLocator(locator)));
      return SolutionKind::Solved;
    }
    return SolutionKind::Unsolved;
  };

  // When locator points to a KeyPathDynamicMemberLookup, reject the
  // key path application.
  if (locator.endsWith<LocatorPathElt::KeyPathDynamicMember>())
    return SolutionKind::Error;

  if (keyPathTy->isAnyKeyPath()) {
    // Read-only keypath, whose projected value is upcast to `Any?`.
    // The root type can be anything.
    Type resultTy = getASTContext().getAnyExistentialType();
    resultTy = OptionalType::get(resultTy);
    return matchTypes(resultTy, valueTy, ConstraintKind::Bind,
                      subflags, locator);
  }

  if (keyPathTy->isPlaceholder()) {
    if (rootTy->hasTypeVariable()) {
      recordAnyTypeVarAsPotentialHole(rootTy);
    }
    if (valueTy->hasTypeVariable()) {
      recordAnyTypeVarAsPotentialHole(valueTy);
    }
    return SolutionKind::Solved;
  }

  if (auto bgt = keyPathTy->getAs<BoundGenericType>()) {
    // We have the key path type. Match it to the other ends of the constraint.
    auto kpRootTy = bgt->getGenericArgs()[0];
    
    // Try to match the root type.
    rootTy = getFixedTypeRecursive(rootTy, flags, /*wantRValue=*/false);

    auto matchRoot = [&](ConstraintKind kind) -> bool {
      auto rootMatches =
          matchTypes(rootTy, kpRootTy, kind, subflags,
                     locator.withPathElement(LocatorPathElt::KeyPathRoot()));
      switch (rootMatches) {
      case SolutionKind::Error:
        return false;
      case SolutionKind::Solved:
        return true;
      case SolutionKind::Unsolved:
        llvm_unreachable("should have generated constraints");
      }
      llvm_unreachable("unhandled match");
    };

    if (bgt->isPartialKeyPath()) {
      // Read-only keypath, whose projected value is upcast to `Any`.
      auto resultTy = getASTContext().getAnyExistentialType();

      if (!matchRoot(ConstraintKind::Conversion))
        return SolutionKind::Error;

      return matchTypes(resultTy, valueTy,
                        ConstraintKind::Bind, subflags, locator);
    }

    if (bgt->getGenericArgs().size() < 2)
      return SolutionKind::Error;
    auto kpValueTy = bgt->getGenericArgs()[1];

    /// Solve for an rvalue base.
    auto solveRValue = [&]() -> ConstraintSystem::SolutionKind {
      // An rvalue base can be converted to a supertype.
      return matchTypes(kpValueTy, valueTy,
                        ConstraintKind::Bind, subflags, locator);
    };
    /// Solve for a base whose lvalueness is to be determined.
    auto solveUnknown = [&]() -> ConstraintSystem::SolutionKind {
      if (matchTypes(kpValueTy, valueTy, ConstraintKind::Equal, subflags,
                     locator).isFailure())
        return SolutionKind::Error;
      return unsolved();
    };
    /// Solve for an lvalue base.
    auto solveLValue = [&]() -> ConstraintSystem::SolutionKind {
      return matchTypes(LValueType::get(kpValueTy), valueTy,
                        ConstraintKind::Bind, subflags, locator);
    };
  
    if (bgt->isKeyPath()) {
      // Read-only keypath.
      if (!matchRoot(ConstraintKind::Conversion))
        return SolutionKind::Error;

      return solveRValue();
    }
    if (bgt->isWritableKeyPath()) {
      // Writable keypath. The result can be an lvalue if the root was.
      // We can't convert the base without giving up lvalue-ness, though.
      if (!matchRoot(ConstraintKind::Equal))
        return SolutionKind::Error;

      if (rootTy->is<LValueType>())
        return solveLValue();
      if (rootTy->isTypeVariableOrMember())
        // We don't know whether the value is an lvalue yet.
        return solveUnknown();
      return solveRValue();
    }
    if (bgt->isReferenceWritableKeyPath()) {
      if (!matchRoot(ConstraintKind::Conversion))
        return SolutionKind::Error;

      // Reference-writable keypath. The result can always be an lvalue.
      return solveLValue();
    }
    // Otherwise, we don't have a key path type at all.
    return SolutionKind::Error;
  }
  if (!keyPathTy->isTypeVariableOrMember())
    return SolutionKind::Error;
  
  return unsolved();
}

bool ConstraintSystem::simplifyAppliedOverloadsImpl(
    Constraint *disjunction, TypeVariableType *fnTypeVar,
    FunctionType *argFnType, unsigned numOptionalUnwraps,
    ConstraintLocatorBuilder locator) {
  // Don't attempt to filter overloads when solving for code completion
  // because presence of code completion token means that any call
  // could be malformed e.g. missing arguments e.g. `foo([.#^MEMBER^#`
  if (isForCodeCompletion()) {
    bool ArgContainsCCTypeVar = Type(argFnType).findIf(isCodeCompletionTypeVar);
    if (ArgContainsCCTypeVar || isCodeCompletionTypeVar(fnTypeVar)) {
      return false;
    }
  }

  if (shouldAttemptFixes()) {
    auto arguments = argFnType->getParams();
    bool allHoles =
        arguments.size() > 0 &&
        llvm::all_of(arguments, [&](const AnyFunctionType::Param &arg) -> bool {
          auto argType = arg.getPlainType();
          if (argType->isPlaceholder())
            return true;

          if (auto *typeVar = argType->getAs<TypeVariableType>())
            return hasFixFor(typeVar->getImpl().getLocator());

          return false;
        });

    // If this is an operator application and all of the arguments are holes,
    // let's disable all but one overload to make sure holes don't cause
    // performance problems because hole could be bound to any type.
    //
    // Non-operator calls are exempted because they have fewer overloads,
    // and it's possible to filter them based on labels.
    if (allHoles && isOperatorDisjunction(disjunction)) {
      auto choices = disjunction->getNestedConstraints();
      for (auto *choice : choices.slice(1))
        choice->setDisabled();
    }
  }

  /// The common result type amongst all function overloads.
  Type commonResultType;
  auto updateCommonResultType = [&](Type choiceType) {
    auto markFailure = [&] {
      commonResultType = ErrorType::get(getASTContext());
    };

    auto choiceFnType = choiceType->getAs<FunctionType>();
    if (!choiceFnType)
      return markFailure();

    // For now, don't attempt to establish a common result type when there
    // are type parameters.
    Type choiceResultType = choiceFnType->getResult();
    if (choiceResultType->hasTypeParameter())
      return markFailure();

    // If we haven't seen a common result type yet, record what we found.
    if (!commonResultType) {
      commonResultType = choiceResultType;
      return;
    }

    // If we found something different, fail.
    if (!commonResultType->isEqual(choiceResultType))
      return markFailure();
  };

  auto *argList = getArgumentList(getConstraintLocator(locator));

  // If argument list has trailing closures and this is `init` call to
  // a callable type, let's not filter anything since there is a possibility
  // that it needs an implicit `.callAsFunction` to work.
  if (argList && argList->hasAnyTrailingClosures()) {
    if (disjunction->getLocator()
            ->isLastElement<LocatorPathElt::ConstructorMember>()) {
      auto choice = disjunction->getNestedConstraints()[0]->getOverloadChoice();
      if (auto *decl = choice.getDeclOrNull()) {
        auto *dc = decl->getDeclContext();
        if (auto *parent = dc->getSelfNominalTypeDecl()) {
          auto type = parent->getDeclaredInterfaceType();
          if (type->isCallAsFunctionType(DC))
            return false;
        }
      }
    }
  }

  // Consider each of the constraints in the disjunction.
retry_after_fail:
  bool hasUnhandledConstraints = false;
  bool labelMismatch = false;
  auto filterResult =
      filterDisjunction(disjunction, /*restoreOnFail=*/shouldAttemptFixes(),
                         [&](Constraint *constraint) {
        assert(constraint->getKind() == ConstraintKind::BindOverload);

        auto choice = constraint->getOverloadChoice();

        // Determine whether the argument labels we have conflict with those of
        // this overload choice.
        if (argList) {
          auto args = argFnType->getParams();

          SmallVector<FunctionType::Param, 8> argsWithLabels;
          argsWithLabels.append(args.begin(), args.end());
          FunctionType::relabelParams(argsWithLabels, argList);

          auto labelsMatch = [&](MatchCallArgumentListener &listener) {
            if (areConservativelyCompatibleArgumentLabels(
                    *this, choice, argsWithLabels, listener,
                    argList->getFirstTrailingClosureIndex()))
              return true;

            labelMismatch = true;
            return false;
          };

          AllowLabelMismatches listener;

          // This overload has more problems than just missing/invalid labels.
          if (!labelsMatch(listener))
            return false;

          // If overload did match, let's check if it needs to be disabled
          // in "performance" mode because it has missing labels.
          if (listener.hadLabelingIssues()) {
            // In performance mode, let's just disable the choice,
            // this decision could be rolled back for diagnostics.
            if (!shouldAttemptFixes())
              return false;

            // Match expected vs. actual to see whether the only kind
            // of problem here is missing label(s).
            auto onlyMissingLabels =
                [argList](ArrayRef<Identifier> expectedLabels) {
                  if (argList->size() != expectedLabels.size())
                    return false;

                  for (auto i : indices(*argList)) {
                    auto actual = argList->getLabel(i);
                    auto expected = expectedLabels[i];

                    if (actual.compare(expected) != 0 && !actual.empty())
                      return false;
                  }

                  return true;
                };

            auto replacementLabels = listener.getLabelReplacements();
            // Either it's just one argument or all issues are missing labels.
            if (!replacementLabels || onlyMissingLabels(*replacementLabels)) {
              constraint->setDisabled(/*enableForDiagnostics=*/true);
              // Don't include this overload in "common result" computation
              // because it has issues.
              return true;
            }
          }
        }

        // Determine the type that this choice will have.
        Type choiceType = getEffectiveOverloadType(
            constraint->getLocator(), choice, /*allowMembers=*/true,
            constraint->getOverloadUseDC());
        if (!choiceType) {
          hasUnhandledConstraints = true;
          return true;
        }

        // If types of arguments/parameters and result lined up exactly,
        // let's favor this overload choice.
        //
        // Note this check ignores `ExtInfo` on purpose and only compares
        // types, if there are overloads that differ only in effects then
        // all of them are going to be considered and filtered as part of
        // "favored" group after forming a valid partial solution.
        if (auto *choiceFnType = choiceType->getAs<FunctionType>()) {
          if (FunctionType::equalParams(argFnType->getParams(),
                                        choiceFnType->getParams()) &&
              argFnType->getResult()->isEqual(choiceFnType->getResult()))
            constraint->setFavored();
        }

        // Account for any optional unwrapping/binding
        for (unsigned i : range(numOptionalUnwraps)) {
          (void)i;
          if (Type objectType = choiceType->getOptionalObjectType())
            choiceType = objectType;
        }

        // If we have a function type, we can compute a common result type.
        updateCommonResultType(choiceType);
        return true;
      });

  switch (filterResult) {
  case SolutionKind::Error:
    if (labelMismatch && shouldAttemptFixes()) {
      argList = nullptr;
      goto retry_after_fail;
    }
    return true;
  case SolutionKind::Solved:
  case SolutionKind::Unsolved:
    break;
  }

  // If there was a constraint that we couldn't reason about, don't use the
  // results of any common-type computations.
  if (hasUnhandledConstraints)
    return false;

  // If we have a common result type, bind the expected result type to it.
  if (commonResultType && !commonResultType->is<ErrorType>()) {
    if (isDebugMode()) {
      PrintOptions PO;
      PO.PrintTypesForDebugging = true;
      llvm::errs().indent(solverState ? solverState->getCurrentIndent() : 0)
        << "(common result type for $T" << fnTypeVar->getID() << " is "
        << commonResultType.getString(PO)
        << ")\n";
    }

    // Introduction of a `Bind` constraint here could result in the disconnect
    // in the constraint system with unintended consequences because e.g.
    // in case of key path application it could disconnect one of the
    // components like subscript from the rest of the context.
    addConstraint(ConstraintKind::Equal, argFnType->getResult(),
                  commonResultType, locator);
  }
  return false;
}

bool ConstraintSystem::simplifyAppliedOverloads(
    Constraint *disjunction, ConstraintLocatorBuilder locator) {
  auto choices = disjunction->getNestedConstraints();
  assert(choices.size() >= 2);
  assert(choices.front()->getKind() == ConstraintKind::BindOverload);

  // If we've already bound the overload type var, bail.
  auto *typeVar = choices.front()->getFirstType()->getAs<TypeVariableType>();
  if (!typeVar || getFixedType(typeVar))
    return false;

  // Try to find an applicable fn constraint that applies the overload choice.
  auto result = findConstraintThroughOptionals(
      typeVar, OptionalWrappingDirection::Unwrap,
      [&](Constraint *match, TypeVariableType *currentRep) {
        // Check to see if we have an applicable fn with a type var RHS that
        // matches the disjunction.
        if (match->getKind() != ConstraintKind::ApplicableFunction)
          return false;

        auto *rhsTyVar = match->getSecondType()->getAs<TypeVariableType>();
        return rhsTyVar && currentRep == getRepresentative(rhsTyVar);
      });

  if (!result)
    return false;

  auto *applicableFn = result->first;
  auto *fnTypeVar = applicableFn->getSecondType()->castTo<TypeVariableType>();
  auto argFnType = applicableFn->getFirstType()->castTo<FunctionType>();
  AppliedDisjunctions[disjunction->getLocator()] = argFnType;
  return simplifyAppliedOverloadsImpl(disjunction, fnTypeVar, argFnType,
                                      /*numOptionalUnwraps*/ result->second,
                                      applicableFn->getLocator());
}

bool ConstraintSystem::simplifyAppliedOverloads(
    Type fnType, FunctionType *argFnType, ConstraintLocatorBuilder locator) {
  // If we've already bound the function type, bail.
  auto *fnTypeVar = fnType->getAs<TypeVariableType>();
  if (!fnTypeVar || getFixedType(fnTypeVar))
    return false;

  // Try to find a corresponding bind overload disjunction.
  unsigned numOptionalUnwraps = 0;
  auto *disjunction =
      getUnboundBindOverloadDisjunction(fnTypeVar, &numOptionalUnwraps);
  if (!disjunction)
    return false;

  AppliedDisjunctions[disjunction->getLocator()] = argFnType;
  return simplifyAppliedOverloadsImpl(disjunction, fnTypeVar, argFnType,
                                      numOptionalUnwraps, locator);
}

/// Create an implicit dot-member reference expression to be used
/// as a root for injected `.callAsFunction` call.
static UnresolvedDotExpr *
createImplicitRootForCallAsFunction(ConstraintSystem &cs, Type refType,
                                    ArgumentList *arguments,
                                    ConstraintLocator *calleeLocator) {
  auto &ctx = cs.getASTContext();
  auto *baseExpr = castToExpr(calleeLocator->getAnchor());

  SmallVector<Identifier, 2> closureLabelsScratch;
  // Create implicit `.callAsFunction` expression to use as an anchor
  // for new argument list that only has trailing closures in it.
  auto *implicitRef = UnresolvedDotExpr::createImplicit(
      ctx, baseExpr, {ctx.Id_callAsFunction},
      arguments->getArgumentLabels(closureLabelsScratch));

  {
    // Record a type of the new reference in the constraint system.
    cs.setType(implicitRef, refType);
    // Record new `.callAsFunction` in the constraint system.
    cs.recordCallAsFunction(implicitRef, arguments, calleeLocator);
  }

  return implicitRef;
}

ConstraintSystem::SolutionKind
ConstraintSystem::simplifyApplicableFnConstraint(
    Type type1, Type type2,
    Optional<TrailingClosureMatching> trailingClosureMatching,
    TypeMatchOptions flags,
    ConstraintLocatorBuilder locator) {
  auto &ctx = getASTContext();

  // By construction, the left hand side is a type that looks like the
  // following: $T1 -> $T2.
  auto func1 = type1->castTo<FunctionType>();

  // Before stripping lvalue-ness and optional types, save the original second
  // type for handling `func callAsFunction` and `@dynamicCallable`
  // applications. This supports the following cases:
  // - Generating constraints for `mutating func callAsFunction`. The nominal
  //   type (`type2`) should be an lvalue type.
  // - Extending `Optional` itself with `func callAsFunction` or
  //   `@dynamicCallable` functionality. Optional types are stripped below if
  //   `shouldAttemptFixes()` is true.
  auto origLValueType2 =
      getFixedTypeRecursive(type2, flags, /*wantRValue=*/false);
  // Drill down to the concrete type on the right hand side.
  type2 = getFixedTypeRecursive(type2, flags, /*wantRValue=*/true);
  auto desugar2 = type2->getDesugaredType();

  // If a type variable representing "function type" is a hole
  // or it could be bound to some concrete type with a help of
  // a fix, let's propagate holes to the "input" type. Doing so
  // provides more information to upcoming argument and result matching.
  if (shouldAttemptFixes()) {
    if (auto *typeVar = type2->getAs<TypeVariableType>()) {
      auto *locator = typeVar->getImpl().getLocator();
      if (hasFixFor(locator)) {
        recordAnyTypeVarAsPotentialHole(func1);
      }
    }
    Type underlyingType = desugar2;
    while (auto *MT = underlyingType->getAs<AnyMetatypeType>()) {
      underlyingType = MT->getInstanceType();
    }
    underlyingType =
        getFixedTypeRecursive(underlyingType, flags, /*wantRValue=*/true);
    if (underlyingType->isPlaceholder()) {
      recordAnyTypeVarAsPotentialHole(func1);
      return SolutionKind::Solved;
    }
  }

  TypeMatchOptions subflags = getDefaultDecompositionOptions(flags);

  SmallVector<LocatorPathElt, 2> parts;
  auto anchor = locator.getLocatorParts(parts);
  bool isOperator =
      (isExpr<PrefixUnaryExpr>(anchor) || isExpr<PostfixUnaryExpr>(anchor) ||
       isExpr<BinaryExpr>(anchor));

  auto hasInOut = [&]() {
    for (auto param : func1->getParams())
      if (param.isInOut())
        return true;
    return false;
  };

  // Local function to form an unsolved result.
  auto formUnsolved = [&](bool activate = false) {
    if (flags.contains(TMF_GenerateConstraints)) {
      auto *application = Constraint::createApplicableFunction(
          *this, type1, type2, trailingClosureMatching,
          getConstraintLocator(locator));

      addUnsolvedConstraint(application);
      if (activate)
        activateConstraint(application);

      return SolutionKind::Solved;
    }
    
    return SolutionKind::Unsolved;
  };

  // If right-hand side is a type variable, the constraint is unsolved.
  if (desugar2->isTypeVariableOrMember()) {
    return formUnsolved();
  }

  // Strip the 'ApplyFunction' off the locator.
  // FIXME: Perhaps ApplyFunction can go away entirely?
  assert(!parts.empty() && "Nonsensical applicable-function locator");
  assert(parts.back().getKind() == ConstraintLocator::ApplyFunction);
  assert(parts.back().getNewSummaryFlags() == 0);
  parts.pop_back();
  ConstraintLocatorBuilder outerLocator =
    getConstraintLocator(anchor, parts, locator.getSummaryFlags());

  // If the types are obviously equivalent, we're done. This optimization
  // is not valid for operators though, where an inout parameter does not
  // have an explicit inout argument.
  if (type1.getPointer() == desugar2) {
    if (!isOperator || !hasInOut()) {
      recordMatchCallArgumentResult(
          getConstraintLocator(
              outerLocator.withPathElement(ConstraintLocator::ApplyArgument)),
          MatchCallArgumentResult::forArity(func1->getNumParams()));
      return SolutionKind::Solved;
    }
  }

  // Handle applications of types with `callAsFunction` methods.
  // Do this before stripping optional types below, when `shouldAttemptFixes()`
  // is true.
  if (desugar2->isCallAsFunctionType(DC)) {
    auto memberLoc = getConstraintLocator(
        locator.withPathElement(ConstraintLocator::ImplicitCallAsFunction));
    // Add a `callAsFunction` member constraint, binding the member type to a
    // type variable.
    auto memberTy = createTypeVariable(memberLoc, /*options=*/0);
    // TODO: Revisit this if `static func callAsFunction` is to be supported.
    // Static member constraint requires `FunctionRefKind::DoubleApply`.
    addValueMemberConstraint(origLValueType2,
                             DeclNameRef(ctx.Id_callAsFunction),
                             memberTy, DC, FunctionRefKind::SingleApply,
                             /*outerAlternatives*/ {}, memberLoc);
    // Add new applicable function constraint based on the member type
    // variable.
    addConstraint(ConstraintKind::ApplicableFunction, func1, memberTy,
                  locator);
    return SolutionKind::Solved;
  }

  // Record the second type before unwrapping optionals.
  auto origType2 = desugar2;
  unsigned unwrapCount = 0;
  if (shouldAttemptFixes()) {
    // If we have an optional type, try forcing it to see if that
    // helps. Note that we only deal with function and metatype types
    // below, so there is no reason not to attempt to strip these off
    // immediately.
    while (auto objectType2 = desugar2->getOptionalObjectType()) {
      type2 = objectType2;
      desugar2 = type2->getDesugaredType();

      // Track how many times we do this so that we can record a fix for each.
      ++unwrapCount;
    }
  }

  // For a function, bind the output and convert the argument to the input.
  if (auto func2 = dyn_cast<FunctionType>(desugar2)) {
    ConstraintKind subKind = (isOperator
                              ? ConstraintKind::OperatorArgumentConversion
                              : ConstraintKind::ArgumentConversion);

    auto *argumentsLoc = getConstraintLocator(
        outerLocator.withPathElement(ConstraintLocator::ApplyArgument));

    auto *argumentList = getArgumentList(argumentsLoc);
    // The argument type must be convertible to the input type.
    SmallVector<std::pair<TypeVariableType *, OpenedArchetypeType *>, 2>
        openedExistentials;
    auto matchCallResult = ::matchCallArguments(
        *this, func2, argumentList, func1->getParams(), func2->getParams(),
        subKind, argumentsLoc, trailingClosureMatching, openedExistentials);

    switch (matchCallResult) {
    case SolutionKind::Error: {
      auto resultTy = func2->getResult();

      // If this is a call that constructs a callable type with
      // trailing closure(s), closure(s) might not belong to
      // the constructor but rather to implicit `callAsFunction`,
      // there is no way to determine that without trying.
      if (resultTy->isCallAsFunctionType(DC) &&
          argumentList->hasAnyTrailingClosures()) {
        auto *calleeLoc = getCalleeLocator(argumentsLoc);

        bool isInit = false;
        if (auto overload = findSelectedOverloadFor(calleeLoc)) {
          isInit = bool(dyn_cast_or_null<ConstructorDecl>(
              overload->choice.getDeclOrNull()));
        }

        if (!isInit)
          return SolutionKind::Error;

        auto &ctx = getASTContext();
        auto numTrailing = argumentList->getNumTrailingClosures();

        SmallVector<Argument, 4> newArguments(
            argumentList->getNonTrailingArgs());
        SmallVector<Argument, 4> trailingClosures(
            argumentList->getTrailingClosures());

        // Original argument list with all the trailing closures removed.
        auto *newArgumentList = ArgumentList::createParsed(
            ctx, argumentList->getLParenLoc(), newArguments,
            argumentList->getRParenLoc(),
            /*firstTrailingClosureIndex=*/None);

        auto trailingClosureTypes = func1->getParams().take_back(numTrailing);
        // The original result type is going to become a result of
        // implicit `.callAsFunction` instead since `.callAsFunction`
        // is inserted between `.init` and trailing closures.
        auto callAsFunctionResultTy = func1->getResult();

        // The implicit replacement for original result type which
        // represents a callable type produced by `.init` call.
        auto callableType =
            createTypeVariable(getConstraintLocator({}), /*flags=*/0);

        // The original application type with all the trailing closures
        // dropped from it and result replaced to the implicit variable.
        func1 = FunctionType::get(func1->getParams().drop_back(numTrailing),
                                  callableType, func1->getExtInfo());

        auto matchCallResult = ::matchCallArguments(
            *this, func2, newArgumentList, func1->getParams(),
            func2->getParams(), subKind, argumentsLoc, trailingClosureMatching,
            openedExistentials);

        if (matchCallResult != SolutionKind::Solved)
          return SolutionKind::Error;

        auto *implicitCallArgumentList =
            ArgumentList::createImplicit(ctx, trailingClosures,
                                         /*firstTrailingClosureIndex=*/0);

        auto *implicitRef = createImplicitRootForCallAsFunction(
            *this, callAsFunctionResultTy, implicitCallArgumentList, calleeLoc);

        auto callAsFunctionArguments =
            FunctionType::get(trailingClosureTypes, callAsFunctionResultTy,
                              FunctionType::ExtInfo());

        // Form an unsolved constraint to apply trailing closures to a
        // callable type produced by `.init`. This constraint would become
        // active when `callableType` is bound.
        addUnsolvedConstraint(Constraint::create(
            *this, ConstraintKind::ApplicableFunction, callAsFunctionArguments,
            callableType,
            getConstraintLocator(implicitRef,
                                 ConstraintLocator::ApplyFunction)));
        break;
      }

      return SolutionKind::Error;
    }

    case SolutionKind::Unsolved: {
      // Only occurs when there is an ambiguity between forward scanning and
      // backward scanning for the unlabeled trailing closure. Create a
      // disjunction so that we explore both paths, and can diagnose
      // ambiguities later.
      assert(!trailingClosureMatching.has_value());

      auto applyLocator = getConstraintLocator(locator);
      auto forwardConstraint = Constraint::createApplicableFunction(
          *this, type1, type2, TrailingClosureMatching::Forward, applyLocator);
      auto backwardConstraint = Constraint::createApplicableFunction(
          *this, type1, type2, TrailingClosureMatching::Backward,
          applyLocator);
      addDisjunctionConstraint(
          { forwardConstraint, backwardConstraint}, applyLocator);
      break;
    }

    case SolutionKind::Solved:
      // Keep going.
      break;
    }

    // Erase all of the opened existentials.
    Type result2 = func2->getResult();
    if (result2->hasTypeVariable() && !openedExistentials.empty()) {
      for (const auto &opened : openedExistentials) {
        result2 = typeEraseOpenedExistentialReference(
            result2, opened.second->getExistentialType(), opened.first,
            TypePosition::Covariant);
      }

      // If result type has any erased existential types it requires explicit
      // `as` coercion.
      if (AddExplicitExistentialCoercion::isRequired(
              *this, func2->getResult(), openedExistentials, locator)) {

        if (!shouldAttemptFixes())
          return SolutionKind::Error;

        if (recordFix(AddExplicitExistentialCoercion::create(
                *this, result2, getConstraintLocator(locator))))
          return SolutionKind::Error;
      }
    }

    // The result types are equivalent.
    if (matchFunctionResultTypes(
            func1->getResult(), result2, subflags,
            locator.withPathElement(ConstraintLocator::FunctionResult))
            .isFailure())
      return SolutionKind::Error;

    if (unwrapCount == 0)
      return SolutionKind::Solved;

    // Record any fixes we attempted to get to the correct solution.
    auto *fix = ForceOptional::create(*this, origType2, func1,
                                      getConstraintLocator(locator));
    if (recordFix(fix, /*impact=*/unwrapCount))
      return SolutionKind::Error;

    return SolutionKind::Solved;
  }

  // For a metatype, perform a construction.
  if (auto meta2 = dyn_cast<AnyMetatypeType>(desugar2)) {
    auto instance2 = getFixedTypeRecursive(meta2->getInstanceType(), true);
    if (instance2->isTypeVariableOrMember())
      return formUnsolved();

    auto *argumentsLoc = getConstraintLocator(
        outerLocator.withPathElement(ConstraintLocator::ApplyArgument));

    auto *argumentList = getArgumentList(argumentsLoc);
    assert(argumentList);

    // Cannot simplify construction of callable types during constraint
    // generation when trailing closures are present because such calls
    // have special trailing closure matching semantics. It's unclear
    // whether trailing arguments belong to `.init` or implicit
    // `.callAsFunction` in this case.
    //
    // Note that the constraint has to be activate so that solver attempts
    // once constraint generation is done.
    if (getPhase() == ConstraintSystemPhase::ConstraintGeneration &&
        argumentList->hasAnyTrailingClosures() &&
        instance2->isCallAsFunctionType(DC)) {
      return formUnsolved(/*activate=*/true);
    }

    // Construct the instance from the input arguments.
    auto simplified = simplifyConstructionConstraint(instance2, func1, subflags,
                                          /*FIXME?*/ DC,
                                          FunctionRefKind::SingleApply,
                                          getConstraintLocator(outerLocator));

    // Record any fixes we attempted to get to the correct solution.
    if (simplified == SolutionKind::Solved) {
      if (unwrapCount == 0)
        return SolutionKind::Solved;

      auto *fix = ForceOptional::create(*this, origType2, func1,
                                        getConstraintLocator(locator));
      if (recordFix(fix, /*impact=*/unwrapCount))
        return SolutionKind::Error;
    }

    return simplified;
  }

  // Handle applications of @dynamicCallable types.
  auto result = simplifyDynamicCallableApplicableFnConstraint(
      type1, origType2, subflags, locator);

  if (shouldAttemptFixes() && result == SolutionKind::Error) {
    // Skip this fix if the type is not yet resolved or
    // it's a function type/metatype which points to argument mismatches.
    if (desugar2->is<TypeVariableType>() || desugar2->is<FunctionType>() ||
        desugar2->is<AnyMetatypeType>())
      return SolutionKind::Error;

    // If there are any type variables associated with arguments/result
    // they have to be marked as "holes".
    recordAnyTypeVarAsPotentialHole(func1);

    if (desugar2->isPlaceholder())
      return SolutionKind::Solved;

    auto *fix = RemoveInvalidCall::create(*this, getConstraintLocator(locator));
    // Let's make this fix as high impact so if there is a function or member
    // overload with e.g. argument-to-parameter type mismatches it would take
    // a higher priority.
    return recordFix(fix, /*impact=*/10) ? SolutionKind::Error
                                         : SolutionKind::Solved;
  }

  return result;
}

/// Looks up and returns the @dynamicCallable required methods (if they exist)
/// implemented by a type.
static llvm::DenseSet<FuncDecl *>
lookupDynamicCallableMethods(NominalTypeDecl *decl, ConstraintSystem &CS,
                             const ConstraintLocatorBuilder &locator,
                             Identifier argumentName, bool hasKeywordArgs) {
  auto &ctx = CS.getASTContext();

  // The generic arguments don't matter because we only want the member decls,
  // not concrete overload choices (we form those later when adding the overload
  // set). We map into context here to avoid an OverloadChoice assertion for an
  // interface type base.
  // TODO: We really ought to separate out the actual lookup part of
  // `performMemberLookup` from the choice construction. That would allow us to
  // requestify the lookup of dynamicCallable members on a per-decl basis, and
  // map them onto viable and unviable choices onto a given base type.
  auto type = decl->getDeclaredTypeInContext();

  DeclNameRef methodName({ ctx, ctx.Id_dynamicallyCall, { argumentName } });
  auto matches = CS.performMemberLookup(
      ConstraintKind::ValueMember, methodName, type,
      FunctionRefKind::SingleApply, CS.getConstraintLocator(locator),
      /*includeInaccessibleMembers*/ false);
  // Filter valid candidates.
  auto candidates = matches.ViableCandidates;
  auto filter = [&](OverloadChoice choice) {
    auto cand = cast<FuncDecl>(choice.getDecl());
    return !isValidDynamicCallableMethod(cand, CS.DC->getParentModule(),
                                         hasKeywordArgs);
  };
  candidates.erase(
      std::remove_if(candidates.begin(), candidates.end(), filter),
      candidates.end());

  llvm::DenseSet<FuncDecl *> methods;
  for (auto candidate : candidates)
    methods.insert(cast<FuncDecl>(candidate.getDecl()));
  return methods;
}

/// Looks up and returns the @dynamicCallable required methods (if they exist)
/// implemented by a given nominal type decl.
static DynamicCallableMethods
lookupDynamicCallableMethods(NominalTypeDecl *decl, ConstraintSystem &CS,
                             const ConstraintLocatorBuilder &locator) {
  auto it = CS.DynamicCallableCache.find(decl);
  if (it != CS.DynamicCallableCache.end())
    return it->second;

  // The decl must have @dynamicCallable.
  auto &ctx = CS.getASTContext();
  HasDynamicCallableAttributeRequest req(decl);
  if (!evaluateOrDefault(ctx.evaluator, req, false))
    return DynamicCallableMethods();

  DynamicCallableMethods methods;
  methods.argumentsMethods =
    lookupDynamicCallableMethods(decl, CS, locator, ctx.Id_withArguments,
                                 /*hasKeywordArgs*/ false);
  methods.keywordArgumentsMethods =
    lookupDynamicCallableMethods(decl, CS, locator,
                                 ctx.Id_withKeywordArguments,
                                 /*hasKeywordArgs*/ true);
  CS.DynamicCallableCache[decl] = methods;
  return methods;
}

/// Returns the @dynamicCallable required methods (if they exist) implemented
/// by a type.
static DynamicCallableMethods
getDynamicCallableMethods(Type type, ConstraintSystem &CS,
                          const ConstraintLocatorBuilder &locator) {
  SmallVector<NominalTypeDecl *, 4> decls;
  namelookup::tryExtractDirectlyReferencedNominalTypes(type, decls);

  DynamicCallableMethods result;
  for (auto *decl : decls)
    result.addMethods(lookupDynamicCallableMethods(decl, CS, locator));

  return result;
}

// TODO: Refactor/simplify this function.
// - It should perform less duplicate work with its caller
//   `ConstraintSystem::simplifyApplicableFnConstraint`.
// - It should generate a member constraint instead of manually forming an
//   overload set for `func dynamicallyCall` candidates.
// - It should support `mutating func dynamicallyCall`. This should fall out of
//   using member constraints with an lvalue base type.
ConstraintSystem::SolutionKind
ConstraintSystem::simplifyDynamicCallableApplicableFnConstraint(
                                            Type type1,
                                            Type type2,
                                            TypeMatchOptions flags,
                                            ConstraintLocatorBuilder locator) {
  auto &ctx = getASTContext();

  // By construction, the left hand side is a function type: $T1 -> $T2.
  assert(type1->is<FunctionType>());

  // Drill down to the concrete type on the right hand side.
  type2 = getFixedTypeRecursive(type2, flags, /*wantRValue=*/true);
  auto desugar2 = type2->getDesugaredType();

  TypeMatchOptions subflags = getDefaultDecompositionOptions(flags);

  // If the types are obviously equivalent, we're done.
  if (type1.getPointer() == desugar2)
    return SolutionKind::Solved;

  // Local function to form an unsolved result.
  auto formUnsolved = [&] {
    if (flags.contains(TMF_GenerateConstraints)) {
      addUnsolvedConstraint(
        Constraint::create(*this,
          ConstraintKind::DynamicCallableApplicableFunction, type1, type2,
          getConstraintLocator(locator)));
      return SolutionKind::Solved;
    }
    return SolutionKind::Unsolved;
  };

  // If right-hand side is a type variable, the constraint is unsolved.
  if (desugar2->isTypeVariableOrMember())
    return formUnsolved();

  // If right-hand side is a function type, it must be a valid
  // `dynamicallyCall` method type. Bind the output and convert the argument
  // to the input.
  auto func1 = type1->castTo<FunctionType>();
  if (auto func2 = dyn_cast<FunctionType>(desugar2)) {
    // The argument type must be convertible to the input type.
    assert(func1->getParams().size() == 1 && func2->getParams().size() == 1 &&
           "Expected `dynamicallyCall` method with one parameter");
    assert((func2->getParams()[0].getLabel() == ctx.Id_withArguments ||
            func2->getParams()[0].getLabel() == ctx.Id_withKeywordArguments) &&
           "Expected 'dynamicallyCall' method argument label 'withArguments' "
           "or 'withKeywordArguments'");
    if (matchTypes(func1->getParams()[0].getPlainType(),
                   func2->getParams()[0].getPlainType(),
                   ConstraintKind::ArgumentConversion,
                   subflags,
                   locator.withPathElement(
                     ConstraintLocator::ApplyArgument)).isFailure())
      return SolutionKind::Error;

    // The result types are equivalent.
    if (matchFunctionResultTypes(
            func1->getResult(), func2->getResult(), subflags,
            locator.withPathElement(ConstraintLocator::FunctionResult))
            .isFailure())
      return SolutionKind::Error;

    return SolutionKind::Solved;
  }

  // If the right-hand side is not a function type, it must be a valid
  // @dynamicCallable type. Attempt to get valid `dynamicallyCall` methods.
  auto methods = getDynamicCallableMethods(desugar2, *this, locator);
  if (!methods.isValid()) return SolutionKind::Error;

  // Determine whether to call a `withArguments` method or a
  // `withKeywordArguments` method.
  bool useKwargsMethod = methods.argumentsMethods.empty();
  useKwargsMethod |= llvm::any_of(
    func1->getParams(), [](AnyFunctionType::Param p) { return p.hasLabel(); });

  auto candidates = useKwargsMethod ?
    methods.keywordArgumentsMethods :
    methods.argumentsMethods;

  // Create a type variable for the `dynamicallyCall` method.
  auto loc = getConstraintLocator(locator);
  auto tv = createTypeVariable(loc,
                               TVO_CanBindToLValue |
                               TVO_CanBindToNoEscape);

  // Record the 'dynamicallyCall` method overload set.
  SmallVector<OverloadChoice, 4> choices;
  for (auto candidate : candidates) {
    if (candidate->isInvalid()) continue;
    choices.push_back(
      OverloadChoice(type2, candidate, FunctionRefKind::SingleApply));
  }

  if (choices.empty()) {
    if (!shouldAttemptFixes())
      return SolutionKind::Error;

    // TODO(diagnostics): This is not going to be necessary once
    // `@dynamicCallable` uses existing `member` machinery.

    auto argLabel = useKwargsMethod ? ctx.Id_withKeywordArguments
                                    : ctx.Id_withArguments;
    DeclNameRef memberName({ ctx, ctx.Id_dynamicallyCall, {argLabel} });

    auto *fix = DefineMemberBasedOnUse::create(
        *this, desugar2, memberName, /*alreadyDiagnosed=*/false,
        getConstraintLocator(loc, ConstraintLocator::DynamicCallable));

    if (recordFix(fix))
      return SolutionKind::Error;

    recordPotentialHole(tv);
    recordAnyTypeVarAsPotentialHole(func1);

    return SolutionKind::Solved;
  }

  addOverloadSet(tv, choices, DC, loc);

  // Create a type variable for the argument to the `dynamicallyCall` method.
  auto tvParam = createTypeVariable(loc, TVO_CanBindToNoEscape);
  AnyFunctionType *funcType =
    FunctionType::get({ AnyFunctionType::Param(tvParam) }, func1->getResult());
  addConstraint(ConstraintKind::DynamicCallableApplicableFunction,
                funcType, tv, locator);

  // Get argument type for the `dynamicallyCall` method.
  Type argumentType;
  if (!useKwargsMethod) {
    auto arrayLitProto =
      ctx.getProtocol(KnownProtocolKind::ExpressibleByArrayLiteral);
    addConstraint(ConstraintKind::ConformsTo, tvParam,
                  arrayLitProto->getDeclaredInterfaceType(), locator);
    auto elementAssocType = arrayLitProto->getAssociatedType(
        ctx.Id_ArrayLiteralElement);
    argumentType = DependentMemberType::get(tvParam, elementAssocType);
  } else {
    auto dictLitProto =
      ctx.getProtocol(KnownProtocolKind::ExpressibleByDictionaryLiteral);
    addConstraint(ConstraintKind::ConformsTo, tvParam,
                  dictLitProto->getDeclaredInterfaceType(), locator);
    auto valueAssocType = dictLitProto->getAssociatedType(ctx.Id_Value);
    argumentType = DependentMemberType::get(tvParam, valueAssocType);
  }

  // Argument type can default to `Any`.
  addConstraint(ConstraintKind::Defaultable, argumentType,
                ctx.getAnyExistentialType(), locator);

  auto *baseArgLoc = getConstraintLocator(
      loc->getAnchor(),
      {ConstraintLocator::DynamicCallable, ConstraintLocator::ApplyArgument},
      /*summaryFlags=*/0);

  // All dynamic call parameter types must be convertible to the argument type.
  for (auto i : indices(func1->getParams())) {
    auto param = func1->getParams()[i];
    auto paramType = param.getPlainType();

    addConstraint(
        ConstraintKind::ArgumentConversion, paramType, argumentType,
        getConstraintLocator(baseArgLoc, LocatorPathElt::ApplyArgToParam(
                                             i, 0, param.getParameterFlags())));
  }

  return SolutionKind::Solved;
}

static bool hasUnresolvedPackVars(Type type) {
  // We can't compute a reduced shape if the input type still
  // contains type variables that might bind to pack archetypes
  // or pack expansions.
  SmallPtrSet<TypeVariableType *, 2> typeVars;
  type->getTypeVariables(typeVars);
  return llvm::any_of(typeVars, [](const TypeVariableType *typeVar) {
    return typeVar->getImpl().canBindToPack() ||
           typeVar->getImpl().isPackExpansion();
  });
}

ConstraintSystem::SolutionKind ConstraintSystem::simplifyShapeOfConstraint(
    Type shapeTy, Type packTy, TypeMatchOptions flags,
    ConstraintLocatorBuilder locator) {
  // Recursively replace all type variables with fixed bindings if
  // possible.
  packTy = simplifyType(packTy, flags);

  auto formUnsolved = [&]() {
    // If we're supposed to generate constraints, do so.
    if (flags.contains(TMF_GenerateConstraints)) {
      auto *shapeOf = Constraint::create(
          *this, ConstraintKind::ShapeOf, shapeTy, packTy,
          getConstraintLocator(locator));

      addUnsolvedConstraint(shapeOf);
      return SolutionKind::Solved;
    }

    return SolutionKind::Unsolved;
  };

  // Don't try computing the shape of a type variable.
  if (packTy->isTypeVariableOrMember())
    return formUnsolved();

  // We can't compute a reduced shape if the input type still
  // contains type variables that might bind to pack archetypes
  // or pack expansions.
  SmallPtrSet<TypeVariableType *, 2> typeVars;
  packTy->getTypeVariables(typeVars);
  for (auto *typeVar : typeVars) {
    if (typeVar->getImpl().canBindToPack() ||
        typeVar->getImpl().isPackExpansion())
      return formUnsolved();
  }

  if (packTy->hasPlaceholder()) {
    if (!shouldAttemptFixes())
      return SolutionKind::Error;

    recordTypeVariablesAsHoles(shapeTy);
    return SolutionKind::Solved;
  }

  auto shape = packTy->getReducedShape();
  addConstraint(ConstraintKind::Bind, shapeTy, shape, locator);
  return SolutionKind::Solved;
}

ConstraintSystem::SolutionKind ConstraintSystem::simplifySameShapeConstraint(
    Type type1, Type type2, TypeMatchOptions flags,
    ConstraintLocatorBuilder locator) {
  type1 = simplifyType(type1);
  type2 = simplifyType(type2);

  auto formUnsolved = [&]() {
    // If we're supposed to generate constraints, do so.
    if (flags.contains(TMF_GenerateConstraints)) {
      auto *sameShape =
          Constraint::create(*this, ConstraintKind::SameShape, type1, type2,
                             getConstraintLocator(locator));

      addUnsolvedConstraint(sameShape);
      return SolutionKind::Solved;
    }

    return SolutionKind::Unsolved;
  };

  if (hasUnresolvedPackVars(type1) || hasUnresolvedPackVars(type2))
    return formUnsolved();

  auto shape1 = type1->getReducedShape();
  auto shape2 = type2->getReducedShape();

  if (shape1->isEqual(shape2))
    return SolutionKind::Solved;

  if (shouldAttemptFixes()) {
    // If there are placeholders involved shape mismatches are most
    // likely just a symptom of some other issue i.e. type mismatch.
    if (type1->hasPlaceholder() || type2->hasPlaceholder())
      return SolutionKind::Solved;

    auto recordShapeFix = [&](ConstraintFix *fix,
                              unsigned impact) -> SolutionKind {
      return recordFix(fix, impact) ? SolutionKind::Error
                                    : SolutionKind::Solved;
    };

    // Let's check whether we can produce a tailored fix for argument/parameter
    // mismatches.
    if (locator.endsWith<LocatorPathElt::PackShape>()) {
      SmallVector<LocatorPathElt> path;
      auto anchor = locator.getLocatorParts(path);

      // Drop `PackShape`
      path.pop_back();

      // Tailed diagnostics for argument/parameter mismatches - there
      // are either missing or extra arguments.
      if (path.size() > 0 &&
          path[path.size() - 1].is<LocatorPathElt::ApplyArgToParam>()) {
        auto &ctx = getASTContext();

        auto *loc = getConstraintLocator(anchor, path);
        auto argLoc =
            loc->castLastElementTo<LocatorPathElt::ApplyArgToParam>();

        auto argPack = type1->castTo<PackType>();
        auto paramPack = type2->castTo<PackType>();

        // Tailed diagnostic to explode tuples.
        if (argPack->getNumElements() == 1) {
          if (auto *tuple = argPack->getElementType(0)->getAs<TupleType>()) {
            if (tuple->getNumElements() == paramPack->getNumElements()) {
              return recordShapeFix(
                  DestructureTupleToMatchPackExpansionParameter::create(
                      *this, paramPack, loc),
                  /*impact=*/2 * paramPack->getNumElements());
            }
          }
        }

        auto numArgs = shape1->castTo<PackType>()->getNumElements();
        auto numParams = shape2->castTo<PackType>()->getNumElements();

        // Drops `ApplyArgToParam` and left with `ApplyArgument`.
        path.pop_back();

        auto *argListLoc = getConstraintLocator(anchor, path);

        // Missing arguments.
        if (numParams > numArgs) {
          SmallVector<SynthesizedArg> synthesizedArgs;
          for (unsigned i = 0, n = numParams - numArgs; i != n; ++i) {
            auto eltTy = shape2->castTo<PackType>()->getElementType(i);
            synthesizedArgs.push_back(SynthesizedArg{
                argLoc.getParamIdx(), AnyFunctionType::Param(eltTy)});
          }

          return recordShapeFix(
              AddMissingArguments::create(*this, synthesizedArgs, argListLoc),
              /*impact=*/2 * synthesizedArgs.size());
        } else {
          auto argIdx = argLoc.getArgIdx() + numParams;
          SmallVector<std::pair<unsigned, AnyFunctionType::Param>, 4>
              extraneousArgs;

          for (unsigned i = 0, n = numArgs - numParams; i != n; ++i) {
            extraneousArgs.push_back(
                {argIdx + i, AnyFunctionType::Param(ctx.TheEmptyTupleType)});
          }

          auto overload = findSelectedOverloadFor(getCalleeLocator(argListLoc));
          if (!overload)
            return SolutionKind::Error;

          return recordShapeFix(
              RemoveExtraneousArguments::create(
                  *this, overload->openedType->castTo<FunctionType>(),
                  extraneousArgs, argListLoc),
              /*impact=*/2 * extraneousArgs.size());
        }
      }
    }

    unsigned impact = 1;
    if (locator.endsWith<LocatorPathElt::AnyRequirement>())
      impact = assessRequirementFailureImpact(*this, shape1, locator);

    return recordShapeFix(
        SkipSameShapeRequirement::create(*this, type1, type2,
                                         getConstraintLocator(locator)),
        impact);
  }

  return SolutionKind::Error;
}

ConstraintSystem::SolutionKind
ConstraintSystem::simplifyExplicitGenericArgumentsConstraint(
    Type type1, Type type2, TypeMatchOptions flags,
    ConstraintLocatorBuilder locator) {
  auto formUnsolved = [&]() {
    // If we're supposed to generate constraints, do so.
    if (flags.contains(TMF_GenerateConstraints)) {
      auto *explictGenericArgs =
          Constraint::create(*this, ConstraintKind::ExplicitGenericArguments,
                             type1, type2, getConstraintLocator(locator));

      addUnsolvedConstraint(explictGenericArgs);
      return SolutionKind::Solved;
    }

    return SolutionKind::Unsolved;
  };

  // Bail out if we haven't selected an overload yet.
  auto simplifiedBoundType = simplifyType(type1, flags);
  if (simplifiedBoundType->isTypeVariableOrMember())
    return formUnsolved();

  // If the overload hasn't been resolved, we can't simplify this constraint.
  auto overloadLocator = getCalleeLocator(getConstraintLocator(locator));
  auto selectedOverload = findSelectedOverloadFor(overloadLocator);
  if (!selectedOverload)
    return formUnsolved();

  auto overloadChoice = selectedOverload->choice;
  if (!overloadChoice.isDecl()) {
    return SolutionKind::Error;
  }

  auto decl = overloadChoice.getDecl();
  auto genericContext = decl->getAsGenericContext();
  if (!genericContext)
    return SolutionKind::Error;

  auto genericParams = genericContext->getGenericParams();
  if (!genericParams || genericParams->size() == 0) {
    // FIXME: Record an error here that we're ignoring the parameters.
    return SolutionKind::Solved;
  }

  // Map the generic parameters we have over to their opened types.
  SmallVector<Type, 2> openedGenericParams;
  auto genericParamDepth = genericParams->getParams()[0]->getDepth();
  for (const auto &openedType : getOpenedTypes(overloadLocator)) {
    if (openedType.first->getDepth() == genericParamDepth) {
      openedGenericParams.push_back(Type(openedType.second));
    }
  }
  assert(openedGenericParams.size() == genericParams->size());

  // Match the opened generic parameters to the specialized arguments.
  auto specializedArgs = type2->castTo<PackType>()->getElementTypes();
  PackMatcher matcher(openedGenericParams, specializedArgs, getASTContext(),
                      isPackExpansionType);
  if (matcher.match())
    return SolutionKind::Error;

  // Bind the opened generic parameters to the specialization arguments.
  for (const auto &pair : matcher.pairs) {
    addConstraint(
        ConstraintKind::Bind, pair.lhs, pair.rhs,
        getConstraintLocator(
            locator, LocatorPathElt::GenericArgument(pair.lhsIdx)));
  }

  return SolutionKind::Solved;
}

static llvm::PointerIntPair<Type, 3, unsigned>
getBaseTypeForPointer(TypeBase *type) {
  unsigned unwrapCount = 0;
  while (auto objectTy = type->getOptionalObjectType()) {
    type = objectTy.getPointer();
    ++unwrapCount;
  }

  auto pointeeTy = type->getAnyPointerElementType();
  assert(pointeeTy);
  return {pointeeTy, unwrapCount};
}

void ConstraintSystem::addRestrictedConstraint(
                             ConstraintKind kind,
                             ConversionRestrictionKind restriction,
                             Type first, Type second,
                             ConstraintLocatorBuilder locator) {
  (void)simplifyRestrictedConstraint(restriction, first, second, kind,
                                     TMF_GenerateConstraints, locator);
}

/// Given that we have a conversion constraint between two types, and
/// that the given constraint-reduction rule applies between them at
/// the top level, apply it and generate any necessary recursive
/// constraints.
ConstraintSystem::SolutionKind
ConstraintSystem::simplifyRestrictedConstraintImpl(
                                         ConversionRestrictionKind restriction,
                                         Type type1, Type type2,
                                         ConstraintKind matchKind,
                                         TypeMatchOptions flags,
                                         ConstraintLocatorBuilder locator) {
  assert(!type1->isTypeVariableOrMember() && !type2->isTypeVariableOrMember());

  // Add to the score based on context.
  auto addContextualScore = [&] {
    // Okay, we need to perform one or more conversions.  If this
    // conversion will cause a function conversion, score it as worse.
    // This induces conversions to occur within closures instead of
    // outside of them wherever possible.
    if (locator.isFunctionConversion()) {
      increaseScore(SK_FunctionConversion);
    }
  };

  TypeMatchOptions subflags = getDefaultDecompositionOptions(flags);

  auto matchPointerBaseTypes =
      [&](llvm::PointerIntPair<Type, 3, unsigned> baseType1,
          llvm::PointerIntPair<Type, 3, unsigned> baseType2) -> SolutionKind {
    if (restriction != ConversionRestrictionKind::PointerToPointer)
      increaseScore(ScoreKind::SK_ValueToPointerConversion);

    auto result =
        matchTypes(baseType1.getPointer(), baseType2.getPointer(),
                   ConstraintKind::BindToPointerType, subflags, locator);

    if (!(result.isFailure() && shouldAttemptFixes()))
      return result;

    BoundGenericType *ptr1 = nullptr;
    BoundGenericType *ptr2 = nullptr;

    switch (restriction) {
    case ConversionRestrictionKind::ArrayToPointer:
    case ConversionRestrictionKind::InoutToPointer: {
      ptr2 = type2->lookThroughAllOptionalTypes()->castTo<BoundGenericType>();
      ptr1 = BoundGenericType::get(ptr2->getDecl(), ptr2->getParent(),
                                   {baseType1.getPointer()});
      break;
    }

    case ConversionRestrictionKind::PointerToPointer:
      // Original types could be wrapped into a different number of optional.
      ptr1 = type1->lookThroughAllOptionalTypes()->castTo<BoundGenericType>();
      ptr2 = type2->lookThroughAllOptionalTypes()->castTo<BoundGenericType>();
      break;

    default:
      return SolutionKind::Error;
    }

    auto *fix = GenericArgumentsMismatch::create(*this, ptr1, ptr2, {0},
                                                 getConstraintLocator(locator));

    // Treat this as a contextual type mismatch.
    unsigned baseImpact = 2;
    // It's possible to implicitly promote pointer into an optional
    // before matching base types if other side is an optional, so
    // score needs to account for number of such promotions.
    int optionalWraps = baseType2.getInt() - baseType1.getInt();
    return recordFix(fix, baseImpact + abs(optionalWraps))
               ? SolutionKind::Error
               : SolutionKind::Solved;
  };

  auto fixContextualFailure = [&](Type fromType, Type toType,
                                  ConstraintLocatorBuilder locator) -> bool {
    auto *loc = getConstraintLocator(locator);
    // Since this is a contextual type mismatch, let's start from higher
    // impact than regular fix to avoid ambiguities.
    auto impact = 2;
    if (loc->isForAssignment() || loc->isForCoercion() ||
        loc->isForContextualType() ||
        loc->isLastElement<LocatorPathElt::ApplyArgToParam>() ||
        loc->isForOptionalTry()) {
      if (restriction == ConversionRestrictionKind::Superclass) {
        if (auto *fix = CoerceToCheckedCast::attempt(
                *this, fromType, toType, /*useConditionalCast*/ false, loc))
          return !recordFix(fix, impact);
      }
      
      // We already have a fix for this locator indicating a
      // tuple mismatch.
      if (hasFixFor(loc, FixKind::AllowTupleTypeMismatch))
        return true;

      if (restriction == ConversionRestrictionKind::ValueToOptional ||
          restriction == ConversionRestrictionKind::OptionalToOptional)
        ++impact;

      auto *fix =
          loc->isLastElement<LocatorPathElt::ApplyArgToParam>()
              ? AllowArgumentMismatch::create(*this, fromType, toType, loc)
              : ContextualMismatch::create(*this, fromType, toType, loc);
      return !recordFix(fix, impact);
    }

    return false;
  };

  switch (restriction) {
  // for $< in { <, <c, <oc }:
  //   T_i $< U_i ===> (T_i...) $< (U_i...)
  case ConversionRestrictionKind::DeepEquality:
    return matchDeepEqualityTypes(type1, type2, locator);

  case ConversionRestrictionKind::Superclass: {
    addContextualScore();

    auto result = matchSuperclassTypes(type1, type2, subflags, locator);

    if (!(shouldAttemptFixes() && result.isFailure()))
      return result;

    return fixContextualFailure(type1, type2, locator)
               ? getTypeMatchSuccess()
               : getTypeMatchFailure(locator);
  }

  // for $< in { <, <c, <oc }:
  //   T $< U, U : P_i ===> T $< protocol<P_i...>
  case ConversionRestrictionKind::Existential:
    addContextualScore();
    return matchExistentialTypes(type1, type2,
                                 ConstraintKind::SelfObjectOfProtocol,
                                 subflags, locator);

  // for $< in { <, <c, <oc }:
  //   for P protocol, Q protocol,
  //     P : Q ===> T.Protocol $< Q.Type
  //   for P protocol, Q protocol,
  //     P $< Q ===> P.Type $< Q.Type
  case ConversionRestrictionKind::MetatypeToExistentialMetatype: {
    addContextualScore();

    auto instanceTy1 = type1->getMetatypeInstanceType();
    auto instanceTy2 = type2->getMetatypeInstanceType();

    auto result = matchExistentialTypes(
        instanceTy1, instanceTy2, ConstraintKind::ConformsTo, subflags,
        locator.withPathElement(ConstraintLocator::InstanceType));

    if (!(shouldAttemptFixes() && result.isFailure()))
      return result;

    return fixContextualFailure(type1, type2, locator)
               ? getTypeMatchSuccess()
               : getTypeMatchFailure(locator);
  }

  // for $< in { <, <c, <oc }:
  //   for P protocol, C class, D class,
  //     (P & C) : D ===> (P & C).Type $< D.Type
  case ConversionRestrictionKind::ExistentialMetatypeToMetatype: {
    addContextualScore();

    auto instance1 = type1->castTo<ExistentialMetatypeType>()->getInstanceType();
    auto instance2 = type2->castTo<MetatypeType>()->getInstanceType();
    auto superclass1 = instance1->getSuperclass();

    if (!superclass1)
      return SolutionKind::Error;

    auto result =
        matchTypes(superclass1, instance2, ConstraintKind::Subtype, subflags,
                   locator.withPathElement(ConstraintLocator::InstanceType));

    if (!(shouldAttemptFixes() && result.isFailure()))
      return result;

    return fixContextualFailure(type1, type2, locator)
               ? getTypeMatchSuccess()
               : getTypeMatchFailure(locator);
  }
  // for $< in { <, <c, <oc }:
  //   T $< U ===> T $< U?
  case ConversionRestrictionKind::ValueToOptional: {
    addContextualScore();
    increaseScore(SK_ValueToOptional);

    assert(matchKind >= ConstraintKind::Subtype);
    if (auto generic2 = type2->getAs<BoundGenericType>()) {
      if (generic2->getDecl()->isOptionalDecl()) {
        auto result = matchTypes(
            type1, generic2->getGenericArgs()[0], matchKind, subflags,
            locator.withPathElement(ConstraintLocator::OptionalPayload));

        if (!(shouldAttemptFixes() && result.isFailure()))
          return result;
      }
    }

    return shouldAttemptFixes() && fixContextualFailure(type1, type2, locator)
               ? SolutionKind::Solved
               : SolutionKind::Error;
  }

  // for $< in { <, <c, <oc }:
  //   T $< U ===> T? $< U?
  //   T $< U ===> T! $< U!
  //   T $< U ===> T! $< U?
  // also:
  //   T <c U ===> T? <c U!
  case ConversionRestrictionKind::OptionalToOptional: {
    addContextualScore();

    assert(matchKind >= ConstraintKind::Subtype);
    if (auto generic1 = type1->getAs<BoundGenericType>()) {
      if (auto generic2 = type2->getAs<BoundGenericType>()) {
        if (generic1->getDecl()->isOptionalDecl() &&
            generic2->getDecl()->isOptionalDecl()) {
          auto result = matchTypes(
              generic1->getGenericArgs()[0], generic2->getGenericArgs()[0],
              matchKind, subflags,
              locator.withPathElement(LocatorPathElt::GenericArgument(0)));

          if (!(shouldAttemptFixes() && result.isFailure()))
            return result;
        }
      }
    }

    return shouldAttemptFixes() && fixContextualFailure(type1, type2, locator)
               ? SolutionKind::Solved
               : SolutionKind::Error;
  }

  case ConversionRestrictionKind::ClassMetatypeToAnyObject:
  case ConversionRestrictionKind::ExistentialMetatypeToAnyObject:
  case ConversionRestrictionKind::ProtocolMetatypeToProtocolClass: {
    // Nothing more to solve.
    addContextualScore();
    return SolutionKind::Solved;
  }
  
  // T <p U ===> T[] <a UnsafeMutablePointer<U>
  case ConversionRestrictionKind::ArrayToPointer: {
    addContextualScore();
    // Unwrap an inout type.
    auto obj1 = type1->getInOutObjectType();

    obj1 = getFixedTypeRecursive(obj1, false);
    
    auto t2 = type2->getDesugaredType();

    auto baseType1 = getFixedTypeRecursive(obj1->isArrayType(), false);
    auto ptr2 = getBaseTypeForPointer(t2);

    increaseScore(SK_ValueToOptional, ptr2.getInt());

    return matchPointerBaseTypes({baseType1, 0}, ptr2);
  }

  // String ===> UnsafePointer<[U]Int8>
  case ConversionRestrictionKind::StringToPointer: {
    addContextualScore();

    auto ptr2 = getBaseTypeForPointer(type2->getDesugaredType());

    increaseScore(SK_ValueToOptional, ptr2.getInt());

    // The pointer element type must be void or a byte-sized type.
    // TODO: Handle different encodings based on pointer element type, such as
    // UTF16 for [U]Int16 or UTF32 for [U]Int32. For now we only interop with
    // Int8 pointers using UTF8 encoding.
    auto baseType2 = getFixedTypeRecursive(ptr2.getPointer(), false);
    // If we haven't resolved the element type, generate constraints.
    if (baseType2->isTypeVariableOrMember()) {
      if (flags.contains(TMF_GenerateConstraints)) {
        increaseScore(ScoreKind::SK_ValueToPointerConversion);

        auto &ctx = getASTContext();
        auto int8Con = Constraint::create(*this, ConstraintKind::Bind,
                                          baseType2,
                                          ctx.getInt8Type(),
                                          getConstraintLocator(locator));
        auto uint8Con = Constraint::create(*this, ConstraintKind::Bind,
                                           baseType2,
                                           ctx.getUInt8Type(),
                                           getConstraintLocator(locator));
        auto voidCon = Constraint::create(*this, ConstraintKind::Bind,
                                          baseType2, ctx.TheEmptyTupleType,
                                          getConstraintLocator(locator));
        
        Constraint *disjunctionChoices[] = {int8Con, uint8Con, voidCon};
        addDisjunctionConstraint(disjunctionChoices, locator);
        return SolutionKind::Solved;
      }

      return SolutionKind::Unsolved;
    }
    
    if (!isStringCompatiblePointerBaseType(getASTContext(), baseType2)) {
      return SolutionKind::Error;
    }

    increaseScore(ScoreKind::SK_ValueToPointerConversion);
    return SolutionKind::Solved;
  }
      
  // T <p U ===> inout T <a UnsafeMutablePointer<U>
  case ConversionRestrictionKind::InoutToPointer: {
    addContextualScore();

    auto t2 = type2->getDesugaredType();

    auto baseType1 = type1->getInOutObjectType();
    auto ptr2 = getBaseTypeForPointer(t2);

    increaseScore(SK_ValueToOptional, ptr2.getInt());

    return matchPointerBaseTypes({baseType1, 0}, ptr2);
  }
      
  // T <p U ===> UnsafeMutablePointer<T> <a UnsafeMutablePointer<U>
  case ConversionRestrictionKind::PointerToPointer: {
    auto t1 = type1->getDesugaredType();
    auto t2 = type2->getDesugaredType();

    auto ptr1 = getBaseTypeForPointer(t1);
    auto ptr2 = getBaseTypeForPointer(t2);

    return matchPointerBaseTypes(ptr1, ptr2);
  }

  case ConversionRestrictionKind::PointerToCPointer:
    return simplifyPointerToCPointerRestriction(type1, type2, flags, locator);

  case ConversionRestrictionKind::ArrayToCPointer: {
    auto ptr2 = type2->getDesugaredType()->lookThroughAllOptionalTypes();

    PointerTypeKind pointerKind;
    auto cPtr = ptr2->getAnyPointerElementType(pointerKind);

    // If the parameter is a raw pointer or its element type is not a
    // supported (un-)signed integer it implies a regular ArrayToPointer
    // conversion.
    if (isRawPointerKind(pointerKind) ||
        !(cPtr->isInt()   || cPtr->isUInt()   ||
          cPtr->isInt8()  || cPtr->isUInt8()  ||
          cPtr->isInt16() || cPtr->isUInt16() ||
          cPtr->isInt32() || cPtr->isUInt32() ||
          cPtr->isInt64() || cPtr->isUInt64())) {
      return SolutionKind::Error;
    }

    increaseScore(SK_ValueToPointerConversion);

    type1 = getFixedTypeRecursive(type1->getInOutObjectType()->isArrayType(),
                                  /*wantRValue=*/false);
    LLVM_FALLTHROUGH;
  }

  case ConversionRestrictionKind::InoutToCPointer: {
    SmallVector<Type, 2> optionals;

    auto ptr2 =
        type2->getDesugaredType()->lookThroughAllOptionalTypes(optionals);

    increaseScore(SK_ValueToOptional, optionals.size());

    PointerTypeKind pointerKind;
    (void)ptr2->getAnyPointerElementType(pointerKind);

    auto baseType1 = type1->getInOutObjectType();

    Type ptr1;
    // The right-hand size is a raw pointer, so let's use `UnsafeMutablePointer`
    // for the `inout` type.
    if (pointerKind == PTK_UnsafeRawPointer ||
        pointerKind == PTK_UnsafeMutableRawPointer) {
      ptr1 = BoundGenericType::get(Context.getUnsafeMutablePointerDecl(),
                                   /*parent=*/nullptr, {baseType1});
    } else {
      ptr1 = baseType1->wrapInPointer(pointerKind);
    }

    assert(ptr1);

    return simplifyPointerToCPointerRestriction(ptr1, ptr2, flags, locator);
  }

  // T < U or T is bridged to V where V < U ===> Array<T> <c Array<U>
  case ConversionRestrictionKind::ArrayUpcast: {
    Type baseType1 = type1->isArrayType();
    Type baseType2 = type2->isArrayType();

    increaseScore(SK_CollectionUpcastConversion);
    return matchTypes(baseType1,
                      baseType2,
                      matchKind,
                      subflags,
                      locator.withPathElement(
                          LocatorPathElt::GenericArgument(0)));
  }

  // K1 < K2 && V1 < V2 || K1 bridges to K2 && V1 bridges to V2 ===> 
  //   Dictionary<K1, V1> <c Dictionary<K2, V2>
  case ConversionRestrictionKind::DictionaryUpcast: {
    auto t1 = type1->getDesugaredType();    
    Type key1, value1;
    std::tie(key1, value1) = *isDictionaryType(t1);

    auto t2 = type2->getDesugaredType();
    Type key2, value2;
    std::tie(key2, value2) = *isDictionaryType(t2);

    auto subMatchKind = matchKind; // TODO: Restrict this?
    increaseScore(SK_CollectionUpcastConversion);
    // The source key and value types must be subtypes of the destination
    // key and value types, respectively.
    auto result =
        matchTypes(key1, key2, subMatchKind, subflags,
                   locator.withPathElement(LocatorPathElt::GenericArgument(0)));
    if (result.isFailure())
      return result;

    switch (matchTypes(
        value1, value2, subMatchKind, subflags,
        locator.withPathElement(LocatorPathElt::GenericArgument(1)))) {
    case SolutionKind::Solved:
      return result;

    case SolutionKind::Unsolved:
      return SolutionKind::Unsolved;

    case SolutionKind::Error:
      return SolutionKind::Error;
    }
  }

  // T1 < T2 || T1 bridges to T2 ===> Set<T1> <c Set<T2>
  case ConversionRestrictionKind::SetUpcast: {
    Type baseType1 = *isSetType(type1);
    Type baseType2 = *isSetType(type2);

    increaseScore(SK_CollectionUpcastConversion);
    return matchTypes(baseType1,
                      baseType2,
                      matchKind,
                      subflags,
                      locator.withPathElement(LocatorPathElt::GenericArgument(0)));
  }

  // T1 <c T2 && T2 : Hashable ===> T1 <c AnyHashable
  case ConversionRestrictionKind::HashableToAnyHashable: {
    // We never want to do this if the LHS is already AnyHashable.
    type1 = simplifyType(type1);
    if (type1->getRValueType()->lookThroughAllOptionalTypes()->isAnyHashable()) {
      return SolutionKind::Error;
    }

    addContextualScore();
    increaseScore(SK_UserConversion); // FIXME: Use separate score kind?
    if (worseThanBestSolution()) {
      return SolutionKind::Error;
    }

    auto hashableProtocol =
      getASTContext().getProtocol(KnownProtocolKind::Hashable);
    if (!hashableProtocol)
      return SolutionKind::Error;

    auto constraintLocator = getConstraintLocator(locator);
    auto tv = createTypeVariable(constraintLocator,
                                 TVO_PrefersSubtypeBinding |
                                 TVO_CanBindToNoEscape);
    
    addConstraint(ConstraintKind::ConformsTo, tv,
                  hashableProtocol->getDeclaredInterfaceType(),
                  constraintLocator);

    return matchTypes(type1, tv, ConstraintKind::Conversion, subflags,
                      locator);
  }

  // T' < U and T a toll-free-bridged to T' ===> T' <c U
  case ConversionRestrictionKind::CFTollFreeBridgeToObjC: {
    increaseScore(SK_UserConversion); // FIXME: Use separate score kind?
    if (worseThanBestSolution()) {
      return SolutionKind::Error;
    }

    auto nativeClass = type1->getClassOrBoundGenericClass();
    auto bridgedObjCClass
      = nativeClass->getAttrs().getAttribute<ObjCBridgedAttr>()->getObjCClass();

    return matchTypes(bridgedObjCClass->getDeclaredInterfaceType(),
                      type2, ConstraintKind::Subtype, subflags, locator);
  }

  // T < U' and U a toll-free-bridged to U' ===> T <c U
  case ConversionRestrictionKind::ObjCTollFreeBridgeToCF: {
    increaseScore(SK_UserConversion); // FIXME: Use separate score kind?
    if (worseThanBestSolution()) {
      return SolutionKind::Error;
    }

    auto nativeClass = type2->getClassOrBoundGenericClass();
    auto bridgedObjCClass
      = nativeClass->getAttrs().getAttribute<ObjCBridgedAttr>()->getObjCClass();

    return matchTypes(type1,
                      bridgedObjCClass->getDeclaredInterfaceType(),
                      ConstraintKind::Subtype, subflags, locator);
  }

  case ConversionRestrictionKind::DoubleToCGFloat:
  case ConversionRestrictionKind::CGFloatToDouble: {
    // Prefer CGFloat -> Double over other way araund.
    auto impact =
        restriction == ConversionRestrictionKind::CGFloatToDouble ? 1 : 10;

    if (restriction == ConversionRestrictionKind::DoubleToCGFloat) {
      if (auto *anchor = locator.trySimplifyToExpr()) {
        if (auto depth = getExprDepth(anchor))
          impact = (*depth + 1) * impact;
      }
    }

    increaseScore(SK_ImplicitValueConversion, impact);

    if (worseThanBestSolution())
      return SolutionKind::Error;

    auto *conversionLoc =
        getImplicitValueConversionLocator(locator, restriction);

    auto *applicationLoc =
        getConstraintLocator(conversionLoc, ConstraintLocator::ApplyFunction);

    auto *memberLoc = getConstraintLocator(
        applicationLoc, ConstraintLocator::ConstructorMember);

    // Allocate a single argument info to cover all possible
    // Double <-> CGFloat conversion locations.
    auto *argumentsLoc =
        getConstraintLocator(conversionLoc, ConstraintLocator::ApplyArgument);

    if (!ArgumentLists.count(argumentsLoc)) {
      auto *argList = ArgumentList::createImplicit(
          getASTContext(), {Argument(SourceLoc(), Identifier(), nullptr)},
          /*firstTrailingClosureIndex=*/None,
          AllocationArena::ConstraintSolver);
      ArgumentLists.insert({argumentsLoc, argList});
    }

    auto *memberTypeLoc = getConstraintLocator(
        applicationLoc, LocatorPathElt::ConstructorMemberType());

    auto *memberTy = createTypeVariable(memberTypeLoc, TVO_CanBindToNoEscape);

    addValueMemberConstraint(MetatypeType::get(type2, getASTContext()),
                             DeclNameRef(DeclBaseName::createConstructor()),
                             memberTy, DC, FunctionRefKind::DoubleApply,
                             /*outerAlternatives=*/{}, memberLoc);

    addConstraint(ConstraintKind::ApplicableFunction,
                  FunctionType::get({FunctionType::Param(type1)}, type2),
                  memberTy, applicationLoc);

    ImplicitValueConversions.insert(
        {getConstraintLocator(locator), restriction});
    return SolutionKind::Solved;
  }
  }
  
  llvm_unreachable("bad conversion restriction");
}

ConstraintSystem::SolutionKind
ConstraintSystem::simplifyRestrictedConstraint(
                                       ConversionRestrictionKind restriction,
                                       Type type1, Type type2,
                                       ConstraintKind matchKind,
                                       TypeMatchOptions flags,
                                       ConstraintLocatorBuilder locator) {
  switch (simplifyRestrictedConstraintImpl(restriction, type1, type2,
                                           matchKind, flags, locator)) {
  case SolutionKind::Solved: {
    // If we have an application of a non-ephemeral parameter, then record a
    // fix if we have to treat an ephemeral conversion as non-ephemeral. It's
    // important that this is solved as an independent constraint, as the
    // solving of this restriction may be required in order to evaluate it. For
    // example, when solving `foo(&.x)`, we need to first match types for the
    // inout-to-pointer conversion, which then allows us to resolve the overload
    // of `x`, which may or may not produce an ephemeral pointer.
    if (locator.isNonEphemeralParameterApplication()) {
      bool downgradeToWarning =
          !getASTContext().LangOpts.DiagnoseInvalidEphemeralnessAsError;

      auto *fix = TreatEphemeralAsNonEphemeral::create(
          *this, getConstraintLocator(locator), type1, type2, restriction,
          downgradeToWarning);
      addFixConstraint(fix, matchKind, type1, type2, locator);
    }

    ConstraintRestrictions.insert({
        std::make_pair(type1.getPointer(), type2.getPointer()), restriction});
    return SolutionKind::Solved;
  }
  case SolutionKind::Unsolved:
    return SolutionKind::Unsolved;

  case SolutionKind::Error:
    return SolutionKind::Error;
  }

  llvm_unreachable("Unhandled SolutionKind in switch.");
}

ConstraintSystem::SolutionKind
ConstraintSystem::simplifyPointerToCPointerRestriction(
    Type type1, Type type2, TypeMatchOptions flags,
    ConstraintLocatorBuilder locator) {
  bool inCorrectPosition = isArgumentOfImportedDecl(locator);
  if (!inCorrectPosition) {
    // If this is not an imported function, let's not proceed with
    // the conversion, unless in diagnostic mode.
    if (!shouldAttemptFixes())
      return SolutionKind::Error;

    // Let's attempt to convert the types and record a tailored
    // fix if that succeeds.
  }

  auto &ctx = getASTContext();

  PointerTypeKind swiftPtrKind, cPtrKind;

  auto swiftPtr = type1->getAnyPointerElementType(swiftPtrKind);
  auto cPtr = type2->getAnyPointerElementType(cPtrKind);

  assert(swiftPtr);
  assert(cPtr);

  auto markSupported = [&]() -> SolutionKind {
    // Make sure that solutions with implicit pointer conversions
    // are always worse than the ones without them.
    increaseScore(SK_ImplicitValueConversion);

    if (inCorrectPosition)
      return SolutionKind::Solved;

    // If conversion cannot be allowed on account of declaration,
    // let's add a tailored fix.
    auto *fix = AllowSwiftToCPointerConversion::create(
        *this, getConstraintLocator(locator));

    return recordFix(fix) ? SolutionKind::Error : SolutionKind::Solved;
  };

  auto elementLoc = locator.withPathElement(LocatorPathElt::GenericArgument(0));

  if (swiftPtr->isTypeVariableOrMember()) {
    // Inference between the equivalent pointer kinds is
    // handled by regular pointer conversions.
    if (swiftPtrKind == cPtrKind)
      return SolutionKind::Error;

    addConstraint(ConstraintKind::BindToPointerType, swiftPtr, cPtr,
                  elementLoc);
    return markSupported();
  }

  // If pointers have the same element type there is nothing to do.
  if (swiftPtr->isEqual(cPtr))
    return markSupported();

  // Unsafe[Mutable]RawPointer -> Unsafe[Mutable]Pointer<[U]Int8>
  if (swiftPtrKind == PTK_UnsafeRawPointer ||
      swiftPtrKind == PTK_UnsafeMutableRawPointer) {
    // Since it's a C pointer on parameter side it would always
    // be fully resolved.
    if (cPtr->isInt8() || cPtr->isUInt8())
      return markSupported();
  } else {
    // Unsafe[Mutable]Pointer<T> -> Unsafe[Mutable]Pointer<[U]Int8>
    if (cPtr->isInt8() || cPtr->isUInt8()) {
      // <T> can default to the type of C pointer.
      addConstraint(ConstraintKind::Defaultable, swiftPtr, cPtr, elementLoc);
      return markSupported();
    }

    // Unsafe[Mutable]Pointer<Int{8, 16, ...}> <->
    // Unsafe[Mutable]Pointer<UInt{8, 16, ...}>
    if (swiftPtr->isInt() || swiftPtr->isUInt()) {
      addConstraint(ConstraintKind::Equal, cPtr,
                    swiftPtr->isUInt() ? ctx.getIntType() : ctx.getUIntType(),
                    elementLoc);
      return markSupported();
    }

    if (swiftPtr->isInt8() || swiftPtr->isUInt8()) {
      addConstraint(ConstraintKind::Equal, cPtr,
                    swiftPtr->isUInt8() ? ctx.getInt8Type()
                                        : ctx.getUInt8Type(),
                    elementLoc);
      return markSupported();
    }

    if (swiftPtr->isInt16() || swiftPtr->isUInt16()) {
      addConstraint(ConstraintKind::Equal, cPtr,
                    swiftPtr->isUInt16() ? ctx.getInt16Type()
                                         : ctx.getUInt16Type(),
                    elementLoc);
      return markSupported();
    }

    if (swiftPtr->isInt32() || swiftPtr->isUInt32()) {
      addConstraint(ConstraintKind::Equal, cPtr,
                    swiftPtr->isUInt32() ? ctx.getInt32Type()
                                         : ctx.getUInt32Type(),
                    elementLoc);
      return markSupported();
    }

    if (swiftPtr->isInt64() || swiftPtr->isUInt64()) {
      addConstraint(ConstraintKind::Equal, cPtr,
                    swiftPtr->isUInt64() ? ctx.getInt64Type()
                                         : ctx.getUInt64Type(),
                    elementLoc);
      return markSupported();
    }
  }

  // If the conversion is unsupported, let's record a generic argument mismatch.
  if (shouldAttemptFixes() && !inCorrectPosition) {
    auto *fix = AllowArgumentMismatch::create(*this, type1, type2,
                                              getConstraintLocator(locator));
    return recordFix(fix, /*impact=*/2) ? SolutionKind::Error
                                        : SolutionKind::Solved;
  }

  return SolutionKind::Error;
}

static bool isAugmentingFix(ConstraintFix *fix) {
  switch (fix->getKind()) {
    case FixKind::TreatRValueAsLValue:
      return false;
    default:
      return true;
  }
}

bool ConstraintSystem::recordFix(ConstraintFix *fix, unsigned impact) {
  if (isDebugMode()) {
    auto &log = llvm::errs();
    log.indent(solverState ? solverState->getCurrentIndent() : 0)
      << "(attempting fix ";
    fix->print(log);
    log << ")\n";
  }

  if (hasArgumentsIgnoredForCodeCompletion()) {
    // Avoid simplifying the locator if the constraint system didn't ignore any
    // arguments.
    auto argExpr = simplifyLocatorToAnchor(fix->getLocator());
    if (isArgumentIgnoredForCodeCompletion(getAsExpr<Expr>(argExpr))) {
      // The argument was ignored. Don't record any fixes for it.
      return false;
    }
  }

  // Record the fix.

  // If this should affect the solution score, do so.
  if (auto impactScoreKind = fix->impact())
    increaseScore(*impactScoreKind, impact);

  // If we've made the current solution worse than the best solution we've seen
  // already, stop now.
  if (worseThanBestSolution())
    return true;

  if (isAugmentingFix(fix)) {
    Fixes.insert(fix);
    return false;
  }

  auto anchor = fix->getAnchor();
  assert(bool(anchor) && "non-augmenting fix without an anchor?");

  // Only useful to record if no pre-existing fix is associated with
  // current anchor or, in case of anchor being an expression, any of
  // its sub-expressions.
  llvm::SmallDenseSet<ASTNode> anchors;
  for (const auto *fix : Fixes) {
    // Fixes that don't affect the score shouldn't be considered because even
    // if such a fix is recorded at that anchor this should not
    // have any affect in the recording of any other fix.
    if (!fix->impact())
      continue;

    anchors.insert(fix->getAnchor());
  }
  
  bool found = false;
  if (auto *expr = getAsExpr(anchor)) {
    forEachExpr(expr, [&](Expr *subExpr) -> Expr * {
      found |= anchors.count(subExpr);
      return subExpr;
    });
  } else {
    found = anchors.count(anchor);
  }

  if (!found)
    Fixes.insert(fix);

  return false;
}

void ConstraintSystem::recordPotentialHole(TypeVariableType *typeVar) {
  typeVar->getImpl().enableCanBindToHole(getSavedBindings());
}

void ConstraintSystem::recordAnyTypeVarAsPotentialHole(Type type) {
  if (!type->hasTypeVariable())
    return;

  type.visit([&](Type type) {
    if (auto *typeVar = type->getAs<TypeVariableType>()) {
      typeVar->getImpl().enableCanBindToHole(getSavedBindings());
    }
  });
}

void ConstraintSystem::recordTypeVariablesAsHoles(Type type) {
  type.visit([&](Type componentTy) {
    if (auto *typeVar = componentTy->getAs<TypeVariableType>()) {
      // Ignore bound type variables. This can happen if a type variable
      // occurs in multiple positions and/or  if type hasn't been fully
      // simplified before this call.
      if (typeVar->getImpl().hasRepresentativeOrFixed())
        return;

      assignFixedType(typeVar,
                      PlaceholderType::get(getASTContext(), typeVar));
    }
  });
}

void ConstraintSystem::recordMatchCallArgumentResult(
    ConstraintLocator *locator, MatchCallArgumentResult result) {
  assert(locator->isLastElement<LocatorPathElt::ApplyArgument>());
  argumentMatchingChoices.insert({locator, result});
}

void ConstraintSystem::recordCallAsFunction(UnresolvedDotExpr *root,
                                            ArgumentList *arguments,
                                            ConstraintLocator *locator) {
  ImplicitCallAsFunctionRoots.insert({locator, root});

  associateArgumentList(
      getConstraintLocator(root, ConstraintLocator::ApplyArgument), arguments);
}

ConstraintSystem::SolutionKind ConstraintSystem::simplifyFixConstraint(
    ConstraintFix *fix, Type type1, Type type2, ConstraintKind matchKind,
    TypeMatchOptions flags, ConstraintLocatorBuilder locator) {

  // Try with the fix.
  TypeMatchOptions subflags =
    getDefaultDecompositionOptions(flags) | TMF_ApplyingFix;
  switch (fix->getKind()) {
  case FixKind::ForceOptional: {
    SmallVector<Type, 4> unwraps1;
    type1->lookThroughAllOptionalTypes(unwraps1);

    SmallVector<Type, 4> unwraps2;
    type2->lookThroughAllOptionalTypes(unwraps2);

    auto impact = unwraps1.size() != unwraps2.size()
                      ? unwraps1.size() - unwraps2.size()
                      : 1;
    return recordFix(fix, impact) ? SolutionKind::Error : SolutionKind::Solved;
  }

  case FixKind::UnwrapOptionalBase:
  case FixKind::UnwrapOptionalBaseWithOptionalResult: {
    if (recordFix(fix))
      return SolutionKind::Error;

    type1 = simplifyType(type1);
    type2 = simplifyType(type2);

    // Explicitly preserve l-valueness of an unwrapped member type.
    if (!type1->is<LValueType>() && type2->is<LValueType>())
      type1 = LValueType::get(type1);

    // First type already appropriately set.
    return matchTypes(type1, type2, matchKind, subflags, locator);
  }

  case FixKind::ForceDowncast:
    // These work whenever they are suggested.
    if (recordFix(fix))
      return SolutionKind::Error;

    return SolutionKind::Solved;

  case FixKind::AddressOf: {
    // Assume that '&' was applied to the first type, turning an lvalue into
    // an inout.
    auto result = matchTypes(InOutType::get(type1->getRValueType()), type2,
                             matchKind, subflags, locator);
    if (result == SolutionKind::Solved)
      if (recordFix(fix))
        return SolutionKind::Error;

    return result;
  }

  case FixKind::AutoClosureForwarding: {
    if (recordFix(fix))
      return SolutionKind::Error;
    return matchTypes(type1, type2, matchKind, subflags, locator);
  }

  case FixKind::AllowTupleTypeMismatch: {
    if (fix->getAs<AllowTupleTypeMismatch>()->isElementMismatch()) {
      auto *locator = fix->getLocator();
      if (recordFix(fix, /*impact*/locator->isForContextualType() ? 5 : 1))
        return SolutionKind::Error;
      return SolutionKind::Solved;
    }
    auto lhs = type1->castTo<TupleType>();
    auto rhs = type2->castTo<TupleType>();
    // Create a new tuple type the size of the smaller tuple with elements
    // from the larger tuple whenever either side contains a type variable.
    // For example (A, $0, B, $2) and (X, Y, $1) produces: (X, $0, B).
    // This allows us to guarantee that the types will match, and all
    // type variables will get bound to something as long as we default
    // excess types in the larger tuple to Any. In the prior example,
    // when the tuples (X, Y, $1) and (X, $0, B) get matched, $0 is equated
    // to Y, $1 is equated to B, and $2 is defaulted to Any.
    auto lhsLarger = lhs->getNumElements() >= rhs->getNumElements();
    auto isLabelingFailure = lhs->getNumElements() == rhs->getNumElements();
    auto larger = lhsLarger ? lhs : rhs;
    auto smaller = lhsLarger ? rhs : lhs;
    llvm::SmallVector<TupleTypeElt, 4> newTupleTypes;

    // FIXME: For now, if either side contains pack expansion types, consider
    // the fix constraint solved without trying to figure out which tuple
    // elements were part of the pack.
    {
      if (containsPackExpansionType(lhs) ||
          containsPackExpansionType(rhs)) {
        if (recordFix(fix))
          return SolutionKind::Error;
        return SolutionKind::Solved;
      }
    }

    for (unsigned i = 0; i < larger->getNumElements(); ++i) {
      auto largerElt = larger->getElement(i);
      if (i < smaller->getNumElements()) {
        auto smallerElt = smaller->getElement(i);
        if (isLabelingFailure)
          newTupleTypes.push_back(TupleTypeElt(largerElt.getType()));
        else if (largerElt.getType()->isTypeVariableOrMember() ||
            smallerElt.getType()->isTypeVariableOrMember())
          newTupleTypes.push_back(largerElt);
        else
          newTupleTypes.push_back(smallerElt);
      } else {
        if (largerElt.getType()->isTypeVariableOrMember())
          recordAnyTypeVarAsPotentialHole(largerElt.getType());
      }
    }
    auto matchingType =
        TupleType::get(newTupleTypes, getASTContext());
    if (recordFix(fix))
      return SolutionKind::Error;
    return matchTupleTypes(matchingType, smaller, matchKind, subflags, locator);
  }

  case FixKind::AllowFunctionTypeMismatch: {
    if (recordFix(fix, /*impact=*/5))
      return SolutionKind::Error;
    return SolutionKind::Solved;
  }

  case FixKind::TreatEphemeralAsNonEphemeral: {
    auto *theFix = static_cast<TreatEphemeralAsNonEphemeral *>(fix);
    // If we have a non-ephemeral locator for an ephemeral conversion, make a
    // note of the fix.
    auto conversion = theFix->getConversionKind();
    switch (isConversionEphemeral(conversion, locator)) {
    case ConversionEphemeralness::Ephemeral:
      // Record the fix with an impact of zero. This ensures that non-ephemeral
      // diagnostics don't impact solver behavior.
      if (recordFix(fix, /*impact*/ 0))
        return SolutionKind::Error;

      return SolutionKind::Solved;
    case ConversionEphemeralness::Unresolved:
    case ConversionEphemeralness::NonEphemeral:
      // FIXME: The unresolved case should form an unsolved constraint rather
      // than being treated as fully solved. This will require a way to connect
      // the unsolved constraint to the type variable for the unresolved
      // overload such that the fix gets re-activated when the overload is
      // bound.
      return SolutionKind::Solved;
    }
  }

  case FixKind::InsertCall:
  case FixKind::RemoveReturn:
  case FixKind::RemoveAddressOf:
  case FixKind::AddMissingArguments:
  case FixKind::MoveOutOfOrderArgument:
  case FixKind::SkipUnhandledConstructInResultBuilder:
  case FixKind::UsePropertyWrapper:
  case FixKind::UseWrappedValue:
  case FixKind::AllowInvalidPropertyWrapperType:
  case FixKind::RemoveProjectedValueArgument:
  case FixKind::ExpandArrayIntoVarargs:
  case FixKind::UseRawValue:
  case FixKind::SpecifyBaseTypeForContextualMember:
  case FixKind::CoerceToCheckedCast:
  case FixKind::SpecifyObjectLiteralTypeImport:
  case FixKind::AllowKeyPathRootTypeMismatch:
  case FixKind::UnwrapOptionalBaseKeyPathApplication:
  case FixKind::AllowCoercionToForceCast:
  case FixKind::SpecifyKeyPathRootType:
  case FixKind::SpecifyLabelToAssociateTrailingClosure:
  case FixKind::AllowKeyPathWithoutComponents:
  case FixKind::IgnoreInvalidResultBuilderBody:
  case FixKind::IgnoreResultBuilderWithReturnStmts:
  case FixKind::SpecifyContextualTypeForNil:
  case FixKind::AllowRefToInvalidDecl:
  case FixKind::SpecifyBaseTypeForOptionalUnresolvedMember:
  case FixKind::AllowCheckedCastCoercibleOptionalType:
  case FixKind::AllowNoopCheckedCast:
  case FixKind::AllowNoopExistentialToCFTypeCheckedCast:
  case FixKind::AllowUnsupportedRuntimeCheckedCast:
  case FixKind::AllowCheckedCastToUnrelated:
  case FixKind::AllowInvalidStaticMemberRefOnProtocolMetatype:
  case FixKind::AllowWrappedValueMismatch:
  case FixKind::RemoveExtraneousArguments:
  case FixKind::SpecifyTypeForPlaceholder:
  case FixKind::AllowAutoClosurePointerConversion:
  case FixKind::IgnoreKeyPathContextualMismatch:
  case FixKind::NotCompileTimeConst:
  case FixKind::RenameConflictingPatternVariables:
  case FixKind::MustBeCopyable:
  case FixKind::AllowInvalidPackElement:
  case FixKind::AllowInvalidPackReference:
  case FixKind::AllowInvalidPackExpansion:
  case FixKind::MacroMissingPound:
  case FixKind::AllowGlobalActorMismatch:
  case FixKind::AllowAssociatedValueMismatch:
  case FixKind::GenericArgumentsMismatch: {
    return recordFix(fix) ? SolutionKind::Error : SolutionKind::Solved;
  }
  case FixKind::IgnoreInvalidASTNode: {
    return recordFix(fix, 10) ? SolutionKind::Error : SolutionKind::Solved;
  }
  case FixKind::IgnoreUnresolvedPatternVar: {
    return recordFix(fix, 100) ? SolutionKind::Error : SolutionKind::Solved;
  }

  case FixKind::ExplicitlyConstructRawRepresentable: {
    // Let's increase impact of this fix for binary operators because
    // it's possible to get both `.rawValue` and construction fixes for
    // different overloads of a binary operator and `.rawValue` is a
    // better fix because raw representable has a failable constructor.
    return recordFix(fix,
                     /*impact=*/isExpr<BinaryExpr>(locator.getAnchor()) ? 2 : 1)
               ? SolutionKind::Error
               : SolutionKind::Solved;
  }

  case FixKind::TreatRValueAsLValue: {
    unsigned impact = 1;
    // If this is an attempt to use result of a function/subscript call as
    // an l-value, it has to have an increased impact because it's either
    // a function - which is completely incorrect, or it's a get-only
    // subscript, which requires changes to declaration to become mutable.
    impact += (locator.endsWith<LocatorPathElt::FunctionResult>() ||
               locator.endsWith<LocatorPathElt::SubscriptMember>())
                  ? 1
                  : 0;

    return recordFix(fix, impact) ? SolutionKind::Error : SolutionKind::Solved;
  }

  case FixKind::AddConformance:
  case FixKind::SkipSameTypeRequirement:
  case FixKind::SkipSameShapeRequirement:
  case FixKind::SkipSuperclassRequirement: {
    return recordFix(fix, assessRequirementFailureImpact(*this, type1,
                                                         fix->getLocator()))
               ? SolutionKind::Error
               : SolutionKind::Solved;
  }

  case FixKind::AllowArgumentTypeMismatch:
  case FixKind::IgnoreDefaultExprTypeMismatch: {
    auto impact = 2;
    // If there are any other argument mismatches already detected for this
    // call, we increase the score even higher so more argument fixes means
    // less viable is the overload.
    if (llvm::any_of(getFixes(), [&](const ConstraintFix *fix) {
          auto *fixLocator = fix->getLocator();
          return fixLocator->findLast<LocatorPathElt::ApplyArgToParam>()
                     ? fixLocator->getAnchor() == locator.getAnchor()
                     : false;
        }))
      impact += 3;

    // Passing a closure to a parameter that doesn't expect one should
    // be scored lower because there might be an overload that expects
    // a closure but has other issues e.g. wrong number of parameters.
    if (!type2->lookThroughAllOptionalTypes()->is<FunctionType>()) {
      auto argument = simplifyLocatorToAnchor(fix->getLocator());
      if (isExpr<ClosureExpr>(argument)) {
        impact += 2;
      }
    }

    // De-prioritize `Builtin.RawPointer` and `OpaquePointer` parameters
    // because they usually clash with generic parameter mismatches e.g.
    //
    // let ptr: UnsafePointer<String> = ...
    // _ = UnsafePointer<Int>(ups)
    //
    // Here initializer overloads have both `Builtin.RawPointer` and
    // `OpaquePointer` variants, but the actual issue is that generic argument
    // `String` doesn't match `Int`.
    {
      if (type2->is<BuiltinRawPointerType>())
        impact += 1;

      if (type2->getAnyNominal() == getASTContext().getOpaquePointerDecl())
        impact += 1;
    }

    return recordFix(fix, impact) ? SolutionKind::Error : SolutionKind::Solved;
  }

  case FixKind::TreatArrayLiteralAsDictionary: {
    ArrayExpr *AE = getAsExpr<ArrayExpr>(fix->getAnchor());
    assert(AE);

    // If the array was empty, there's nothing to do.
    if (AE->getNumElements() == 0)
      return recordFix(fix) ? SolutionKind::Error : SolutionKind::Solved;

    // For arrays with a single element, match the element type to the
    // dictionary's key type.
    SmallVector<Type, 2> optionals;
    auto dictTy = type2->lookThroughAllOptionalTypes(optionals);

    // If the fix is worse than the best solution, there's no point continuing.
    if (recordFix(fix, optionals.size() + 1))
      return SolutionKind::Error;

    // Extract the dictionary key type.
    ProtocolDecl *dictionaryProto =
        Context.getProtocol(KnownProtocolKind::ExpressibleByDictionaryLiteral);
    auto keyAssocTy = dictionaryProto->getAssociatedType(Context.Id_Key);
    auto valueBaseTy = createTypeVariable(getConstraintLocator(locator),
                                      TVO_CanBindToLValue |
                                        TVO_CanBindToNoEscape |
                                        TVO_CanBindToHole);
    assignFixedType(valueBaseTy, dictTy);
    auto dictionaryKeyTy = DependentMemberType::get(valueBaseTy, keyAssocTy);

    // Extract the array element type.
    auto elemTy = type1->isArrayType();

    ConstraintLocator *elemLoc = getConstraintLocator(AE->getElement(0));
    ConstraintKind kind = isDictionaryType(dictTy)
        ? ConstraintKind::Conversion
        : ConstraintKind::Equal;
    return matchTypes(elemTy, dictionaryKeyTy, kind, subflags, elemLoc);
  }

  case FixKind::ContextualMismatch:
  case FixKind::IgnoreContextualType:
  case FixKind::IgnoreAssignmentDestinationType:
  case FixKind::AllowConversionThroughInOut:
  case FixKind::IgnoreCollectionElementContextualMismatch: {
    auto impact = 1;

    auto locator = fix->getLocator();
    if (auto branchElt =
            locator->getLastElementAs<LocatorPathElt::TernaryBranch>()) {
      // If this is `else` branch of a ternary operator, let's
      // increase its impact to eliminate the chance of ambiguity.
      //
      // Branches are connected through two `subtype` constraints
      // to a common type variable with represents their join, which
      // means that result would attempt a type from each side if
      // one is available and that would result in two fixes - one for
      // each mismatched branch.
      if (branchElt->forElse())
        impact = 10;
    }
    using SingleValueStmtBranch = LocatorPathElt::SingleValueStmtBranch;
    if (auto branchElt = locator->getLastElementAs<SingleValueStmtBranch>()) {
      // Similar to a ternary, except we have N branches. Let's prefer the fix
      // on the first branch, and discount subsequent branches by index.
      if (branchElt->getExprBranchIndex() > 0)
        impact = 9 + branchElt->getExprBranchIndex();
    }
    // Increase impact of invalid conversions to `Any` and `AnyHashable`
    // associated with collection elements (i.e. for-in sequence element)
    // because it means that other side is structurally incompatible.
    if (fix->getKind() == FixKind::IgnoreCollectionElementContextualMismatch) {
      if (type2->isAny() || type2->isAnyHashable())
        ++impact;
    }

    if (recordFix(fix, impact))
      return SolutionKind::Error;

    if (auto *fnType1 = type1->getAs<FunctionType>()) {
      // If this is a contextual mismatch between two
      // function types which we couldn't find a more
      // specific fix for. Let's assume that such types
      // are completely disjoint and adjust impact of
      // the fix accordingly.
      if (auto *fnType2 = type2->getAs<FunctionType>()) {
        increaseScore(SK_Fix, 10);
      } else {
        // If type produced by expression is a function type
        // with result type matching contextual, it should have
        // been diagnosed as "missing explicit call", let's
        // increase the score to make sure that we don't impede that.
        auto result = matchTypes(fnType1->getResult(), type2, matchKind,
                                 TMF_ApplyingFix, locator);
        if (result == SolutionKind::Solved)
          increaseScore(SK_Fix);
      }
    }

    return SolutionKind::Solved;
  }

  case FixKind::AllowNonOptionalWeak: {
    if (recordFix(fix))
      return SolutionKind::Error;

    (void)matchTypes(type1, OptionalType::get(type2),
                     ConstraintKind::Conversion,
                     TypeMatchFlags::TMF_ApplyingFix, locator);

    return SolutionKind::Solved;
  }

  case FixKind::UseSubscriptOperator:
  case FixKind::ExplicitlyEscaping:
  case FixKind::MarkGlobalActorFunction:
  case FixKind::RelabelArguments:
  case FixKind::RemoveCall:
  case FixKind::RemoveUnwrap:
  case FixKind::DefineMemberBasedOnUse:
  case FixKind::AllowMemberRefOnExistential:
  case FixKind::AllowTypeOrInstanceMember:
  case FixKind::AllowInvalidPartialApplication:
  case FixKind::AllowInvalidInitRef:
  case FixKind::AllowClosureParameterDestructuring:
  case FixKind::AllowInaccessibleMember:
  case FixKind::AllowAnyObjectKeyPathRoot:
  case FixKind::AllowMultiArgFuncKeyPathMismatch:
  case FixKind::TreatKeyPathSubscriptIndexAsHashable:
  case FixKind::AllowInvalidRefInKeyPath:
  case FixKind::DefaultGenericArgument:
  case FixKind::AllowMutatingMemberOnRValueBase:
  case FixKind::AllowTupleSplatForSingleParameter:
  case FixKind::AllowNonClassTypeToConvertToAnyObject:
  case FixKind::SpecifyClosureParameterType:
  case FixKind::SpecifyClosureReturnType:
  case FixKind::AddQualifierToAccessTopLevelName:
  case FixKind::AddSendableAttribute:
  case FixKind::DropThrowsAttribute:
  case FixKind::DropAsyncAttribute:
  case FixKind::AllowSwiftToCPointerConversion:
  case FixKind::AllowTupleLabelMismatch:
  case FixKind::AddExplicitExistentialCoercion:
  case FixKind::DestructureTupleToMatchPackExpansionParameter:
  case FixKind::AllowValueExpansionWithoutPackReferences:
  case FixKind::IgnoreMissingEachKeyword:
    llvm_unreachable("handled elsewhere");
  }

  llvm_unreachable("Unhandled FixKind in switch.");
}

ConstraintSystem::SolutionKind
ConstraintSystem::addConstraintImpl(ConstraintKind kind, Type first,
                                    Type second,
                                    ConstraintLocatorBuilder locator,
                                    bool isFavored) {
  assert(first && "Missing first type");
  assert(second && "Missing second type");

  TypeMatchOptions subflags = TMF_GenerateConstraints;
  switch (kind) {
  case ConstraintKind::Equal:
  case ConstraintKind::Bind:
  case ConstraintKind::BindParam:
  case ConstraintKind::BindToPointerType:
  case ConstraintKind::Subtype:
  case ConstraintKind::Conversion:
    return matchTypes(first, second, kind, subflags, locator);

  case ConstraintKind::ArgumentConversion:
  case ConstraintKind::OperatorArgumentConversion:
    return addArgumentConversionConstraintImpl(kind, first, second, locator);

  case ConstraintKind::BridgingConversion:
    return simplifyBridgingConstraint(first, second, subflags, locator);

  case ConstraintKind::ApplicableFunction: {
    // First try to simplify the overload set for the function being applied.
    if (simplifyAppliedOverloads(second, first->castTo<FunctionType>(),
                                 locator)) {
      return SolutionKind::Error;
    }
    return simplifyApplicableFnConstraint(
        first, second, None, subflags, locator);
  }
  case ConstraintKind::DynamicCallableApplicableFunction:
    return simplifyDynamicCallableApplicableFnConstraint(first, second,
                                                         subflags, locator);

  case ConstraintKind::DynamicTypeOf:
    return simplifyDynamicTypeOfConstraint(first, second, subflags, locator);

  case ConstraintKind::EscapableFunctionOf:
    return simplifyEscapableFunctionOfConstraint(first, second,
                                                 subflags, locator);

  case ConstraintKind::OpenedExistentialOf:
    return simplifyOpenedExistentialOfConstraint(first, second,
                                                 subflags, locator);

  case ConstraintKind::SubclassOf:
    return simplifySubclassOfConstraint(first, second, locator, subflags);

  case ConstraintKind::ConformsTo:
  case ConstraintKind::LiteralConformsTo:
  case ConstraintKind::SelfObjectOfProtocol:
    return simplifyConformsToConstraint(first, second, kind, locator,
                                        subflags);

  case ConstraintKind::TransitivelyConformsTo:
    return simplifyTransitivelyConformsTo(first, second, locator,
                                          subflags);

  case ConstraintKind::CheckedCast:
    return simplifyCheckedCastConstraint(first, second, subflags, locator);

  case ConstraintKind::OptionalObject:
    return simplifyOptionalObjectConstraint(first, second, subflags, locator);

  case ConstraintKind::Defaultable:
    return simplifyDefaultableConstraint(first, second, subflags, locator);

  case ConstraintKind::PropertyWrapper:
    return simplifyPropertyWrapperConstraint(first, second, subflags, locator);

  case ConstraintKind::OneWayEqual:
  case ConstraintKind::OneWayBindParam:
    return simplifyOneWayConstraint(kind, first, second, subflags, locator);

  case ConstraintKind::UnresolvedMemberChainBase:
    return simplifyUnresolvedMemberChainBaseConstraint(first, second, subflags,
                                                       locator);

  case ConstraintKind::BindTupleOfFunctionParams:
    return simplifyBindTupleOfFunctionParamsConstraint(first, second, subflags,
                                                       locator);

  case ConstraintKind::PackElementOf:
    return simplifyPackElementOfConstraint(first, second, subflags, locator);

  case ConstraintKind::ShapeOf:
    return simplifyShapeOfConstraint(first, second, subflags, locator);

  case ConstraintKind::SameShape:
    return simplifySameShapeConstraint(first, second, subflags, locator);

  case ConstraintKind::ExplicitGenericArguments:
    return simplifyExplicitGenericArgumentsConstraint(
        first, second, subflags, locator);

  case ConstraintKind::ValueMember:
  case ConstraintKind::UnresolvedValueMember:
  case ConstraintKind::ValueWitness:
  case ConstraintKind::BindOverload:
  case ConstraintKind::Disjunction:
  case ConstraintKind::Conjunction:
  case ConstraintKind::KeyPath:
  case ConstraintKind::KeyPathApplication:
  case ConstraintKind::DefaultClosureType:
  case ConstraintKind::SyntacticElement:
    llvm_unreachable("Use the correct addConstraint()");
  }

  llvm_unreachable("Unhandled ConstraintKind in switch.");
}

ConstraintSystem::SolutionKind
ConstraintSystem::addArgumentConversionConstraintImpl(
    ConstraintKind kind, Type first, Type second,
    ConstraintLocatorBuilder locator) {
  assert(kind == ConstraintKind::ArgumentConversion ||
         kind == ConstraintKind::OperatorArgumentConversion);

  // If we have an unresolved closure argument, form an unsolved argument
  // conversion constraint, making sure to reference the type variables for
  // a result builder if applicable. This ensures we properly connect the
  // closure type variable with any type variables in the result builder, as
  // such type variables will be accessible within the body of the closure when
  // we open it.
  first = getFixedTypeRecursive(first, /*rvalue*/ false);
  if (auto *argTypeVar = first->getAs<TypeVariableType>()) {
    if (argTypeVar->getImpl().isClosureType()) {
      // Extract any type variables present in the parameter's result builder.
      SmallPtrSet<TypeVariableType *, 4> typeVars;
      if (auto builderTy = getOpenedResultBuilderTypeFor(*this, locator))
        builderTy->getTypeVariables(typeVars);

      SmallVector<TypeVariableType *, 4> referencedVars{typeVars.begin(),
                                                        typeVars.end()};

      auto *loc = getConstraintLocator(locator);
      addUnsolvedConstraint(
          Constraint::create(*this, kind, first, second, loc, referencedVars));
      return SolutionKind::Solved;
    }
  }
  return matchTypes(first, second, kind, TMF_GenerateConstraints, locator);
}

void
ConstraintSystem::addKeyPathApplicationRootConstraint(Type root, ConstraintLocatorBuilder locator) {
  // If this is a subscript with a KeyPath expression, add a constraint that
  // connects the subscript's root type to the root type of the KeyPath.
  SmallVector<LocatorPathElt, 4> path;
  auto anchor = locator.getLocatorParts(path);

  auto subscript = getAsExpr<SubscriptExpr>(anchor);
  if (!subscript)
    return;

  assert((path.size() == 1 &&
          path[0].getKind() == ConstraintLocator::SubscriptMember) ||
         (path.size() == 2 &&
          path[1].getKind() == ConstraintLocator::KeyPathDynamicMember));

  // If a keypath subscript is used without the expected `keyPath:` label,
  // continue with type-checking when attempting fixes so that it gets caught
  // by the argument label checking.
  auto *argList = subscript->getArgs();
  auto *unaryArg = argList->getUnaryExpr();
  assert(unaryArg && "Expected KeyPathExpr apply to have single argument");

  auto *keyPathExpr = dyn_cast<KeyPathExpr>(unaryArg);
  if (!keyPathExpr)
    return;

  auto typeVar = getType(keyPathExpr)->getAs<TypeVariableType>();
  if (!typeVar)
    return;

  auto constraints = CG.gatherConstraints(
      typeVar, ConstraintGraph::GatheringKind::EquivalenceClass,
      [&keyPathExpr](Constraint *constraint) -> bool {
        if (constraint->getKind() != ConstraintKind::KeyPath)
          return false;

        auto *locator = constraint->getLocator();
        if (auto KPE = getAsExpr<KeyPathExpr>(locator->getAnchor()))
          return KPE == keyPathExpr;
        return false;
      });

  for (auto constraint : constraints) {
    auto keyPathRootTy = constraint->getSecondType();
    addConstraint(ConstraintKind::Subtype, root->getWithoutSpecifierType(),
                  keyPathRootTy, locator);
  }
}

void
ConstraintSystem::addKeyPathApplicationConstraint(Type keypath,
                                              Type root, Type value,
                                              ConstraintLocatorBuilder locator,
                                              bool isFavored) {
  addKeyPathApplicationRootConstraint(root, locator);
  
  switch (simplifyKeyPathApplicationConstraint(keypath, root, value,
                                               TMF_GenerateConstraints,
                                               locator)) {
  case SolutionKind::Error:
    if (shouldRecordFailedConstraint()) {
      auto c = Constraint::create(*this, ConstraintKind::KeyPathApplication,
                                  keypath, root, value,
                                  getConstraintLocator(locator));
      if (isFavored) c->setFavored();
      recordFailedConstraint(c);
    }
    return;
  
  case SolutionKind::Solved:
    return;
    
  case SolutionKind::Unsolved:
    llvm_unreachable("should have generated constraints");
  }
}

void
ConstraintSystem::addKeyPathConstraint(
    Type keypath,
    Type root, Type value,
    ArrayRef<TypeVariableType *> componentTypeVars,
    ConstraintLocatorBuilder locator,
    bool isFavored) {
  switch (simplifyKeyPathConstraint(keypath, root, value,
                                    componentTypeVars,
                                    TMF_GenerateConstraints,
                                    locator)) {
  case SolutionKind::Error:
    if (shouldRecordFailedConstraint()) {
      auto c = Constraint::create(*this, ConstraintKind::KeyPath,
                                  keypath, root, value,
                                  getConstraintLocator(locator),
                                  componentTypeVars);
      if (isFavored) c->setFavored();
      recordFailedConstraint(c);
    }
    return;
  
  case SolutionKind::Solved:
    return;
    
  case SolutionKind::Unsolved:
    llvm_unreachable("should have generated constraints");
  }
}

void ConstraintSystem::addConstraint(Requirement req,
                                     ConstraintLocatorBuilder locator,
                                     bool isFavored) {
  bool conformsToAnyObject = false;
  Optional<ConstraintKind> kind;
  switch (req.getKind()) {
  case RequirementKind::SameShape: {
    auto type1 = req.getFirstType();
    auto type2 = req.getSecondType();

    addConstraint(ConstraintKind::SameShape, type1, type2, locator);
    return;
  }

  case RequirementKind::Conformance:
    kind = ConstraintKind::ConformsTo;
    break;
  case RequirementKind::Superclass: {
    // FIXME: Should always use ConstraintKind::SubclassOf, but that breaks
    // a couple of diagnostics
    if (auto *typeVar = req.getFirstType()->getAs<TypeVariableType>()) {
      if (typeVar->getImpl().canBindToPack()) {
        kind = ConstraintKind::SubclassOf;
        break;
      }
    }

    conformsToAnyObject = true;
    kind = ConstraintKind::Subtype;
    break;
  }
  case RequirementKind::SameType:
    kind = ConstraintKind::Bind;
    break;
  case RequirementKind::Layout:
    // Only a class constraint can be modeled as a constraint, and only that can
    // appear outside of a @_specialize at the moment anyway.
    if (req.getLayoutConstraint()->isClass()) {
      conformsToAnyObject = true;
      break;
    }
    return;
  }

  auto firstType = req.getFirstType();
  if (kind) {
    addConstraint(*kind, req.getFirstType(), req.getSecondType(), locator,
                  isFavored);
  }

  if (conformsToAnyObject) {
    auto anyObject = getASTContext().getAnyObjectConstraint();
    addConstraint(ConstraintKind::ConformsTo, firstType, anyObject, locator);
  }
}

void ConstraintSystem::addConstraint(ConstraintKind kind, Type first,
                                     Type second,
                                     ConstraintLocatorBuilder locator,
                                     bool isFavored) {
  switch (addConstraintImpl(kind, first, second, locator, isFavored)) {
  case SolutionKind::Error:
    // Add a failing constraint, if needed.
    if (shouldRecordFailedConstraint()) {
      auto c = Constraint::create(*this, kind, first, second,
                                  getConstraintLocator(locator));
      if (isFavored) c->setFavored();
      recordFailedConstraint(c);
    }
    return;

  case SolutionKind::Unsolved:
    llvm_unreachable("should have generated constraints");

  case SolutionKind::Solved:
    return;
  }
}

void ConstraintSystem::addContextualConversionConstraint(
    Expr *expr, Type conversionType, ContextualTypePurpose purpose,
    ConstraintLocator *locator) {
  if (conversionType.isNull())
    return;

  // Determine the type of the constraint.
  auto constraintKind = ConstraintKind::Conversion;
  switch (purpose) {
  case CTP_ReturnStmt:
  case CTP_ReturnSingleExpr:
  case CTP_Initialization: {
    if (conversionType->is<OpaqueTypeArchetypeType>())
      constraintKind = ConstraintKind::Equal;
    // Alternatively, we might have a nested opaque archetype, e.g. `(some P)?`.
    // In that case, we want `ConstraintKind::Conversion`.
    break;
  }
  case CTP_CallArgument:
    constraintKind = ConstraintKind::ArgumentConversion;
    break;

  case CTP_YieldByReference:
    // In a by-reference yield, we expect the contextual type to be an
    // l-value type, so the result must be bound to that.
    constraintKind = ConstraintKind::Bind;
    break;

  case CTP_DiscardStmt:
    // For the 'discard X', we always expect the contextual type to be
    // equal to the type of 'self'.
    constraintKind = ConstraintKind::Equal;
    break;

  case CTP_ForEachSequence:
    // Sequence expression associated with `for-in` loop has to conform
    // to `Sequence` or `AsyncSequence` protocol depending on the context.
    constraintKind = ConstraintKind::ConformsTo;
    break;

  case CTP_ArrayElement:
  case CTP_AssignSource:
  case CTP_CalleeResult:
  case CTP_CannotFail:
  case CTP_Condition:
  case CTP_Unused:
  case CTP_YieldByValue:
  case CTP_CaseStmt:
  case CTP_ThrowStmt:
  case CTP_EnumCaseRawValue:
  case CTP_DefaultParameter:
  case CTP_AutoclosureDefaultParameter:
  case CTP_ClosureResult:
  case CTP_DictionaryKey:
  case CTP_DictionaryValue:
  case CTP_CoerceOperand:
  case CTP_SubscriptAssignSource:
  case CTP_ForEachStmt:
  case CTP_WrappedProperty:
  case CTP_ComposedPropertyWrapper:
  case CTP_ExprPattern:
  case CTP_SingleValueStmtBranch:
    break;
  }

  // Add the constraint.
  auto openedType = openOpaqueType(conversionType, purpose, locator);
  addConstraint(constraintKind, getType(expr), openedType, locator,
                /*isFavored*/ true);
}

void ConstraintSystem::addFixConstraint(ConstraintFix *fix, ConstraintKind kind,
                                        Type first, Type second,
                                        ConstraintLocatorBuilder locator,
                                        bool isFavored) {
  TypeMatchOptions subflags = TMF_GenerateConstraints;
  switch (simplifyFixConstraint(fix, first, second, kind, subflags, locator)) {
  case SolutionKind::Error:
    // Add a failing constraint, if needed.
    if (shouldRecordFailedConstraint()) {
      auto c = Constraint::createFixed(*this, kind, fix, first, second,
                                       getConstraintLocator(locator));
      if (isFavored) c->setFavored();
      recordFailedConstraint(c);
    }
    return;

  case SolutionKind::Unsolved:
    llvm_unreachable("should have generated constraints");

  case SolutionKind::Solved:
    return;
  }
}

void ConstraintSystem::addExplicitConversionConstraint(
    Type fromType, Type toType, RememberChoice_t rememberChoice,
    ConstraintLocatorBuilder locator, ConstraintFix *compatFix) {
  SmallVector<Constraint *, 3> constraints;

  auto locatorPtr = getConstraintLocator(locator);

  // Coercion (the common case).
  Constraint *coerceConstraint =
    Constraint::create(*this, ConstraintKind::Conversion,
                       fromType, toType, locatorPtr);
  coerceConstraint->setFavored();
  constraints.push_back(coerceConstraint);

  // The source type can be explicitly converted to the destination type.
  Constraint *bridgingConstraint =
  Constraint::create(*this, ConstraintKind::BridgingConversion,
                     fromType, toType, locatorPtr);
  constraints.push_back(bridgingConstraint);

  // If we're allowed to use a compatibility fix that emits a warning on
  // failure, add it to the disjunction so that it's recorded on failure.
  if (compatFix) {
    constraints.push_back(
        Constraint::createFixed(*this, ConstraintKind::BridgingConversion,
                                compatFix, fromType, toType, locatorPtr));
  }

  addDisjunctionConstraint(constraints, locator, rememberChoice);
}

ConstraintSystem::SolutionKind
ConstraintSystem::simplifyConstraint(const Constraint &constraint) {
  auto matchKind = constraint.getKind();
  switch (matchKind) {
  case ConstraintKind::Bind:
  case ConstraintKind::Equal:
  case ConstraintKind::BindParam:
  case ConstraintKind::BindToPointerType:
  case ConstraintKind::Subtype:
  case ConstraintKind::Conversion:
  case ConstraintKind::ArgumentConversion:
  case ConstraintKind::OperatorArgumentConversion: {
    // Relational constraints.

    // If there is a fix associated with this constraint, apply it.
    if (auto fix = constraint.getFix()) {
      return simplifyFixConstraint(fix, constraint.getFirstType(),
                                   constraint.getSecondType(), matchKind, None,
                                   constraint.getLocator());
    }

    // If there is a restriction on this constraint, apply it directly rather
    // than going through the general \c matchTypes() machinery.
    if (auto restriction = constraint.getRestriction()) {
      return simplifyRestrictedConstraint(*restriction,
                                          constraint.getFirstType(),
                                          constraint.getSecondType(),
                                          matchKind, None,
                                          constraint.getLocator());
    }

    return matchTypes(constraint.getFirstType(), constraint.getSecondType(),
                      matchKind, None, constraint.getLocator());
  }

  case ConstraintKind::BridgingConversion:
    // If there is a fix associated with this constraint, apply it.
    if (auto fix = constraint.getFix()) {
      return simplifyFixConstraint(fix, constraint.getFirstType(),
                                   constraint.getSecondType(), matchKind, None,
                                   constraint.getLocator());
    }

    return simplifyBridgingConstraint(constraint.getFirstType(),
                                      constraint.getSecondType(),
                                      None, constraint.getLocator());

  case ConstraintKind::ApplicableFunction:
    return simplifyApplicableFnConstraint(
        constraint.getFirstType(), constraint.getSecondType(),
        constraint.getTrailingClosureMatching(), None, constraint.getLocator());

  case ConstraintKind::DynamicCallableApplicableFunction:
    return simplifyDynamicCallableApplicableFnConstraint(
      constraint.getFirstType(), constraint.getSecondType(), None,
      constraint.getLocator());

  case ConstraintKind::DynamicTypeOf:
    return simplifyDynamicTypeOfConstraint(constraint.getFirstType(),
                                           constraint.getSecondType(),
                                           None,
                                           constraint.getLocator());

  case ConstraintKind::EscapableFunctionOf:
    return simplifyEscapableFunctionOfConstraint(constraint.getFirstType(),
                                                 constraint.getSecondType(),
                                                 None,
                                                 constraint.getLocator());

  case ConstraintKind::OpenedExistentialOf:
    return simplifyOpenedExistentialOfConstraint(constraint.getFirstType(),
                                                 constraint.getSecondType(),
                                                 None,
                                                 constraint.getLocator());

  case ConstraintKind::KeyPath:
    return simplifyKeyPathConstraint(
      constraint.getFirstType(), constraint.getSecondType(),
      constraint.getThirdType(), constraint.getTypeVariables(),
      None, constraint.getLocator());

  case ConstraintKind::KeyPathApplication:
    return simplifyKeyPathApplicationConstraint(
      constraint.getFirstType(), constraint.getSecondType(),
      constraint.getThirdType(),
      None, constraint.getLocator());

  case ConstraintKind::BindOverload:
    if (auto *fix = constraint.getFix()) {
      // TODO(diagnostics): Impact should be associated with a fix unless
      // it's a contextual problem, then only solver can decide what the impact
      // would be in each particular situation.
      auto impact =
          fix->getKind() == FixKind::AddQualifierToAccessTopLevelName ? 10 : 1;
      if (recordFix(fix, impact))
        return SolutionKind::Error;
    }

    resolveOverload(constraint.getLocator(), constraint.getFirstType(),
                    constraint.getOverloadChoice(),
                    constraint.getOverloadUseDC());
    return SolutionKind::Solved;

  case ConstraintKind::SubclassOf:
    return simplifySubclassOfConstraint(
             constraint.getFirstType(),
             constraint.getSecondType(),
             constraint.getLocator(),
             /*flags*/ None);

  case ConstraintKind::ConformsTo:
  case ConstraintKind::LiteralConformsTo:
  case ConstraintKind::SelfObjectOfProtocol:
    return simplifyConformsToConstraint(
             constraint.getFirstType(),
             constraint.getSecondType(),
             constraint.getKind(),
             constraint.getLocator(),
             None);

  case ConstraintKind::TransitivelyConformsTo:
    return simplifyTransitivelyConformsTo(
             constraint.getFirstType(),
             constraint.getSecondType(),
             constraint.getLocator(),
             None);

  case ConstraintKind::CheckedCast: {
    auto result = simplifyCheckedCastConstraint(constraint.getFirstType(),
                                                constraint.getSecondType(),
                                                /*flags*/ None,
                                                constraint.getLocator());
    // NOTE: simplifyCheckedCastConstraint() may return Unsolved, e.g. if the
    // subexpression's type is unresolved. Don't record the fix until we
    // successfully simplify the constraint.
    if (result == SolutionKind::Solved) {
      if (auto *fix = constraint.getFix()) {
        if (recordFix(fix)) {
          return SolutionKind::Error;
        }
      }
    }
    return result;
  }

  case ConstraintKind::OptionalObject:
    return simplifyOptionalObjectConstraint(constraint.getFirstType(),
                                            constraint.getSecondType(),
                                            /*flags*/ None,
                                            constraint.getLocator());
      
  case ConstraintKind::ValueMember:
  case ConstraintKind::UnresolvedValueMember:
    return simplifyMemberConstraint(constraint.getKind(),
                                    constraint.getFirstType(),
                                    constraint.getMember(),
                                    constraint.getSecondType(),
                                    constraint.getMemberUseDC(),
                                    constraint.getFunctionRefKind(),
                                    /*outerAlternatives=*/{},
                                    /*flags*/ None, constraint.getLocator());

  case ConstraintKind::ValueWitness:
    return simplifyValueWitnessConstraint(constraint.getKind(),
                                          constraint.getFirstType(),
                                          constraint.getRequirement(),
                                          constraint.getSecondType(),
                                          constraint.getMemberUseDC(),
                                          constraint.getFunctionRefKind(),
                                          /*flags*/ None,
                                          constraint.getLocator());

  case ConstraintKind::Defaultable:
    return simplifyDefaultableConstraint(constraint.getFirstType(),
                                         constraint.getSecondType(),
                                         /*flags*/ None,
                                         constraint.getLocator());

  case ConstraintKind::DefaultClosureType:
    return simplifyDefaultClosureTypeConstraint(constraint.getFirstType(),
                                                constraint.getSecondType(),
                                                constraint.getTypeVariables(),
                                                /*flags*/ None,
                                                constraint.getLocator());

  case ConstraintKind::PropertyWrapper:
    return simplifyPropertyWrapperConstraint(constraint.getFirstType(),
                                             constraint.getSecondType(),
                                             /*flags*/ None,
                                             constraint.getLocator());

  case ConstraintKind::Disjunction:
  case ConstraintKind::Conjunction:
    // See {Dis, Con}junctionStep class in CSStep.cpp for solving
    return SolutionKind::Unsolved;

  case ConstraintKind::OneWayEqual:
  case ConstraintKind::OneWayBindParam:
    return simplifyOneWayConstraint(constraint.getKind(),
                                    constraint.getFirstType(),
                                    constraint.getSecondType(),
                                    /*flags*/ None, constraint.getLocator());

  case ConstraintKind::UnresolvedMemberChainBase:
    return simplifyUnresolvedMemberChainBaseConstraint(
        constraint.getFirstType(), constraint.getSecondType(),
        /*flags=*/None, constraint.getLocator());

  case ConstraintKind::SyntacticElement:
    return simplifySyntacticElementConstraint(
        constraint.getSyntacticElement(), constraint.getElementContext(),
        constraint.isDiscardedElement(),
        /*flags=*/None, constraint.getLocator());

  case ConstraintKind::BindTupleOfFunctionParams:
    return simplifyBindTupleOfFunctionParamsConstraint(
        constraint.getFirstType(), constraint.getSecondType(), /*flags*/ None,
        constraint.getLocator());

  case ConstraintKind::PackElementOf:
    return simplifyPackElementOfConstraint(
        constraint.getFirstType(), constraint.getSecondType(), /*flags*/None,
        constraint.getLocator());

  case ConstraintKind::ShapeOf:
    return simplifyShapeOfConstraint(
        constraint.getFirstType(), constraint.getSecondType(), /*flags*/ None,
        constraint.getLocator());

  case ConstraintKind::SameShape:
    return simplifySameShapeConstraint(constraint.getFirstType(),
                                       constraint.getSecondType(),
                                       /*flags*/ None, constraint.getLocator());

  case ConstraintKind::ExplicitGenericArguments:
    return simplifyExplicitGenericArgumentsConstraint(
        constraint.getFirstType(), constraint.getSecondType(),
        /*flags*/ None, constraint.getLocator());
  }

  llvm_unreachable("Unhandled ConstraintKind in switch.");
}

void ConstraintSystem::simplifyDisjunctionChoice(Constraint *choice) {
  // Simplify this term in the disjunction.
  switch (simplifyConstraint(*choice)) {
  case ConstraintSystem::SolutionKind::Error:
    recordFailedConstraint(choice);
    break;

  case ConstraintSystem::SolutionKind::Solved:
    break;

  case ConstraintSystem::SolutionKind::Unsolved:
    addUnsolvedConstraint(choice);
    break;
  }
}
