//===--- ArgumentExplosionTransform.cpp -----------------------------------===//
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
///
/// \file
///
/// This file contains an implementation of the partial dead argument
/// elimination optimization. We do this to attempt to remove non-trivial
/// arguments of callees to eliminate lifetime constraints of a large argument
/// on values in the caller.
///
//===----------------------------------------------------------------------===//

#define DEBUG_TYPE "fso-argument-explosion-transform"
#include "FunctionSignatureOpts.h"
#include "swift/Basic/Assertions.h"
#include "llvm/Support/CommandLine.h"

using namespace swift;

static llvm::cl::opt<bool> FSODisableArgExplosion(
    "sil-fso-disable-arg-explosion",
    llvm::cl::desc("Do not perform argument explosion during FSO. Intended "
                   "only for testing purposes"));

//===----------------------------------------------------------------------===//
//                                  Utility
//===----------------------------------------------------------------------===//

/// Whether the known-to-date upper bound on the live leaf count is high enough
/// so that argument explosion is possible.
static bool
mayExplodeGivenLiveLeafCountUpperBound(unsigned knownLiveLeafCountUpperBound) {
  return knownLiveLeafCountUpperBound > 0;
}

static unsigned maxExplosionSizeWhenSpecializationWillIntroduceThunk(
    bool willSpecializationIntroduceThunk) {
  // 3 is the heuristic max explosion size for a single argument when the
  // specializing the function will introduce a thunk.  If specializing the
  // function may not introduce a thunk, then we rely on the maximum size
  // imposed by shouldExpand.
  return willSpecializationIntroduceThunk ? 3 : UINT_MAX;
}

static bool shouldExplode(unsigned knownLiveLeafCountUpperBound,
                          bool hasKnownDeadLeaves,
                          bool hasKnownDeadNontrivialLeaves,
                          bool willSpecializationIntroduceThunk) {
  unsigned maxExplosionSize =
      maxExplosionSizeWhenSpecializationWillIntroduceThunk(
          /*willSpecializationIntroduceThunk=*/
          willSpecializationIntroduceThunk);
  bool isLiveLeafCountInExplodableRange =
      mayExplodeGivenLiveLeafCountUpperBound(knownLiveLeafCountUpperBound) &&
      (knownLiveLeafCountUpperBound <= maxExplosionSize);
  bool hasKnownDeadRelevantLeaves = willSpecializationIntroduceThunk
                                        ? hasKnownDeadNontrivialLeaves
                                        : hasKnownDeadLeaves;
  return isLiveLeafCountInExplodableRange && hasKnownDeadRelevantLeaves;
}

/// Return true if it's both legal and a good idea to explode this argument.
///
/// Our main interest here is to expose more opportunities for ARC. This means
/// that we are not interested in exploding (and partially DCEing) structs in
/// the following cases:
///
/// 1. Completely dead arguments. This is handled by dead argument elimination.
///
/// 2. Structs with many live leaf nodes. Our heuristic is to explode if there
///    are only 1-3 live leaf nodes for specializations and 1-6 live leaf nodes
///    (in fact, the number specified in shouldExpand). Otherwise again we run
///    into register pressure/spilling issues.
///    TODO: Improve the 1-3 heuristic by having FSO consider the total
///          resultant argument count.  Currently, there is no consideration of
///          that, meaning we could end up with argument exploding even in the
///          case of long argument lists where it isn't beneficial.
///
/// Perform argument exploding if one of the following sets of conditions hold:
///
/// 1. a. The live leaf count is less than or equal to 3.
///    b. There is a dead non-trivial leaf.
/// 2. a. The live leaf count is less than or equal to 6.
///    b. There is a dead trivial leaf.
///    c. Specializing the function will not result in a thunk.
static bool
shouldExplode(FunctionSignatureTransformDescriptor &transformDesc,
              ArgumentDescriptor &argDesc,
              ConsumedArgToEpilogueReleaseMatcher &epilogueReleaseMatcher) {
  // The method is structured as follows:
  //
  // First, do some basic checks and exit early.
  // Then in three steps of increasing complexity, calculate data which could
  // permit the heuristic to decide to explode the argument.  These steps
  // provide information of increasing expense and fidelity.  Checking whether
  // the heuristic allows explosion after each step unnecessary work to be
  // avoided.
  //
  // In a bit more detail:
  //
  // 1) Do some basic checks and exit early, returning false.
  //    - that we can optimize the argument at all
  //    - that the argument has more than a single leaf node
  //    - that the module permits the type to be expanded
  // 2) Gather some basic leaf counts.
  //    - calculate the unmodified (unmodified that is by the results of the
  //      owned-to-guaranteed transformation) live leaf count
  //    - calculate the total list of leaf types to obtain the total leaf count
  // 3) Check whether the heuristic allows the argument to be exploded using
  //    only potentially-trivial leaf counts.  At this point it is certainly not
  //    known that there are dead non-trivial leaves, so exiting early here
  //    is only possible if specializing the function will not result in a
  //    thunk.
  // 4) Gather the counts of non-trivial leaves.
  //    - calculate the count of total non-trivial leaves by filtering the total
  //      list of leaf types from step 2) according to whether leaf is trivial
  //    - calculate an upper bound (upper bound because it doesn't consider the
  //      results of the owned-to-guaranteed transformation) on the count of
  //      live non-trivial leaves
  // 5) Check whether the heuristic allows the argument to be exploded using the
  //    upper bound on live non-trivial leaves.
  // 6) Dial in the upper bounds calculated in steps 2) and 4) by compensating
  //    for the effects of the owned-to-guaranteed transformation.
  // 7) Check whether the heuristic allows the argument to be exploded using the
  //    actual count of live leaves, both trivial and non-trivial.

  // No passes can optimize this argument, so just bail.
  if (!argDesc.canOptimizeLiveArg()) {
    LLVM_DEBUG(llvm::dbgs()
               << "The argument is of a type that cannot be exploded.");
    return false;
  }

  // If the argument is a singleton, it will not be exploded.
  //
  // Explosion makes sense only if some but not all of the leaves are live.
  //
  // Note that ProjectionTree::isSingleton returns true for enums since they are
  // sums and not products and so only have a single top-level node.
  if (argDesc.ProjTree.isSingleton()) {
    LLVM_DEBUG(llvm::dbgs() << "The argument's type is a singleton.");
    return false;
  }

  auto *argument = argDesc.Arg;
  auto &module = argument->getModule();
  auto type = argument->getType().getObjectType();

  // If the global type expansion heuristic does not allow the type to be
  // expanded, it will not be exploded.
  if (!shouldExpand(module, type)) {
    LLVM_DEBUG(llvm::dbgs()
               << "The argument is of a type which should not be expanded.");
    return false;
  }

  bool willSpecializationIntroduceThunk =
      transformDesc.willSpecializationIntroduceThunk();

  unsigned const liveLeafCountUpperBound = argDesc.ProjTree.getLiveLeafCount();

  // If we know already that we may not explode given the upper bound we have
  // established on the live leaf count, exit early.
  //
  // If the argument is completely dead, it will not be exploded.
  //
  // Explosion makes sense only if some but not all of the leaves are live.  The
  // dead argument transformation will try to eliminate the argument.
  if (!mayExplodeGivenLiveLeafCountUpperBound(liveLeafCountUpperBound)) {
    LLVM_DEBUG(llvm::dbgs() << "The argument has no live leaves.");
    return false;
  }

  // To determine whether some but not all of the leaves are used, the total
  // leaf count must be retrieved.
  llvm::SmallVector<SILType, 32> allLeaves;
  argDesc.ProjTree.getAllLeafTypes(allLeaves);
  unsigned const leafCount = allLeaves.size();

  assert(
      liveLeafCountUpperBound <= leafCount &&
      "There should be no more *live* leaves than there are *total* leaves.");

  if (shouldExplode(
          /*knownLifeLeafCount=*/liveLeafCountUpperBound,
          /*hasKnownDeadLeaves=*/liveLeafCountUpperBound < leafCount,
          /*hasKnownDeadNontrivialLeaves=*/false,
          /*willSpecializationIntroduceThunk=*/
          willSpecializationIntroduceThunk)) {
    LLVM_DEBUG(
        llvm::dbgs()
        << "Without considering the liveness of non-trivial leaves, it has "
           "already been determined that there are already fewer ("
        << liveLeafCountUpperBound
        << ") live leaves of the relevant sort (trivial) than total leaves ("
        << leafCount << ") and no more total live leaves ("
        << liveLeafCountUpperBound << ") than the heuristic permits ("
        << maxExplosionSizeWhenSpecializationWillIntroduceThunk(
               /*willSpecializationIntroduceThunk=*/
               willSpecializationIntroduceThunk)
        << ").  Exploding.");
    return true;
  }

  auto *function = argument->getFunction();
  unsigned const nontrivialLeafCount = llvm::count_if(
      allLeaves, [&](SILType type) { return !type.isTrivial(*function); });

  llvm::SmallVector<const ProjectionTreeNode *, 32> liveLeaves;
  argDesc.ProjTree.getLiveLeafNodes(liveLeaves);
  // NOTE: The value obtained here is an upper bound because the
  //       owned-to-guaranteed transformation may eliminate some live
  //       non-trivial leaves, leaving the count lower.
  unsigned const liveNontrivialLeafCountUpperBound =
      llvm::count_if(liveLeaves, [&](const ProjectionTreeNode *leaf) {
        return !leaf->getType().isTrivial(*function);
      });

  assert(liveNontrivialLeafCountUpperBound <= nontrivialLeafCount &&
         "There should be no more *live* non-trivial leaves than there are "
         "*total* non-trivial leaves.");
  assert(nontrivialLeafCount <= leafCount &&
         "There should be no more *non-trivial* leaves than there are *total* "
         "leaves.");

  // If it is known without taking the owned-to-guaranteed transformation into
  // account both that exploding will reduce ARC traffic (because an upper bound
  // for the number of live non-trivial leaves is less than the non-trivial
  // leaf count) and also that the explosion will fit within the heuristic upper
  // bound (because an upper bound for the total live leaf count falls within
  // the limit imposed by the heuristic), then explode now.
  bool shouldExplodeGivenUpperBounds = shouldExplode(
      /*knownLiveLeafCount=*/liveLeafCountUpperBound,
      /*hasKnownDeadLeaves=*/liveLeafCountUpperBound < leafCount,
      /*hasKnownDeadNontrivialLeaves=*/liveNontrivialLeafCountUpperBound <
          nontrivialLeafCount,
      /*willSpecializationIntroduceThunk=*/willSpecializationIntroduceThunk);
  if (shouldExplodeGivenUpperBounds) {
    LLVM_DEBUG(
        llvm::dbgs()
        << "Without considering the expected results of the "
           "owned-to-guaranteed transformation, there are already fewer ("
        << liveNontrivialLeafCountUpperBound
        << ") live non-trivial leaves than total leaves ("
        << nontrivialLeafCount << ") and no more total live leaves ("
        << liveLeafCountUpperBound << ") than the heuristic permits ("
        << maxExplosionSizeWhenSpecializationWillIntroduceThunk(
               /*willSpecializationIntroduceThunk=*/
               willSpecializationIntroduceThunk)
        << ").  Exploding.");
    return true;
  }

  unsigned liveLeafCount = liveLeafCountUpperBound;
  unsigned liveNontrivialLeafCount = liveNontrivialLeafCountUpperBound;

  // The upper bounds that have been established for the live leaf counts are
  // too high to permit us to explode.  That could be because it hasn't been
  // established that any leaves are dead or alternatively that it hasn't been
  // established that there are fewer total live leaves than the limit imposed
  // by the heuristic.  In either case, if some live leaves are eliminated, the
  // number of live leaves may decrease such that exploding will be possible.
  // The results of the owned-to-guaranteed transformation are predicated.  If
  // it is predicted that a leaf will be dead after the owned-to-guaranteed
  // transformation, then the leaf count is decreased.
  //
  // The owned-to-guaranteed will only be applied to the argument if its
  // convention is Direct_Owned.  Additionally, it only applies to non-trivial
  // leaves, which it may kill, so if it is already known that there are no live
  // non-trivial leaves, owned-to-guaranteed will not eliminate anything.
  if (argDesc.hasConvention(SILArgumentConvention::Direct_Owned) &&
      liveNontrivialLeafCountUpperBound > 0) {
    if (auto maybeReleases =
            epilogueReleaseMatcher.getPartiallyPostDomReleaseSet(argument)) {
      auto releases = maybeReleases.value();
      llvm::SmallPtrSet<SILInstruction *, 8> users;
      users.insert(std::begin(releases), std::end(releases));

      for (auto *leaf : liveLeaves) {
        if (llvm::all_of(leaf->getNonProjUsers(), [&](Operand *operand) {
              return users.count(operand->getUser());
            })) {
          // Every non-projection user of the leaf is an epilogue release.  The
          // owned-to-guaranteed transformation will eliminate this usage.  With
          // the expectation of that usage being eliminated, stop considering
          // this leaf to be live for the purposes of deciding whether the
          // argument should be exploded.
          --liveLeafCount;
          --liveNontrivialLeafCount;
        }
      }
    }
  }

  return shouldExplode(
      /*knownLifeLeafCount=*/liveLeafCount,
      /*hasKnownDeadLeaves=*/liveLeafCount < leafCount,
      /*hasKnownDeadNontrivialLeaves=*/liveNontrivialLeafCount <
          nontrivialLeafCount,
      /*willSpecializationIntroduceThunk=*/willSpecializationIntroduceThunk);
}

//===----------------------------------------------------------------------===//
//                          Top Level Implementation
//===----------------------------------------------------------------------===//

bool FunctionSignatureTransform::ArgumentExplosionAnalyzeParameters() {
  // If we are not supposed to perform argument explosion, bail.
  if (FSODisableArgExplosion)
    return false;

  SILFunction *F = TransformDescriptor.OriginalFunction;
  // Did we decide we should optimize any parameter?
  bool SignatureOptimize = false;
  auto Args = F->begin()->getSILFunctionArguments();
  ConsumedArgToEpilogueReleaseMatcher ArgToReturnReleaseMap(
      RCIA->get(F), F, {SILArgumentConvention::Direct_Owned});

  // Analyze the argument information.
  for (unsigned i : indices(Args)) {
    ArgumentDescriptor &A = TransformDescriptor.ArgumentDescList[i];
    // If the argument is dead, there is no point in trying to explode it. The
    // dead argument pass will get it.
    if (A.IsEntirelyDead) {
      continue;
    }

    // Do not optimize argument.
    if (!A.canOptimizeLiveArg()) {
      continue;
    }

    // Explosion of generic parameters is not supported yet.
    if (A.Arg->getType().hasArchetype())
      continue;

    A.ProjTree.computeUsesAndLiveness(A.Arg);
    A.Explode = shouldExplode(TransformDescriptor, A, ArgToReturnReleaseMap);

    // Modified self argument.
    if (A.Explode && Args[i]->isSelf()) {
      TransformDescriptor.shouldModifySelfArgument = true;
    }

    SignatureOptimize |= A.Explode;
  }
  return SignatureOptimize;
}

void FunctionSignatureTransform::ArgumentExplosionFinalizeOptimizedFunction() {
  SILFunction *NewF = TransformDescriptor.OptimizedFunction.get();
  SILBasicBlock *BB = &*NewF->begin();
  SILBuilder Builder(BB->begin());
  Builder.setCurrentDebugScope(BB->getParent()->getDebugScope());
  unsigned TotalArgIndex = 0;
  for (ArgumentDescriptor &AD : TransformDescriptor.ArgumentDescList) {
    // If this argument descriptor was dead and we removed it, just skip it. Do
    // not increment the argument index.
    if (AD.WasErased) {
      continue;
    }

    // Simply continue if do not explode.
    if (!AD.Explode) {
      TransformDescriptor.AIM[TotalArgIndex] = AD.Index;
      ++TotalArgIndex;
      continue;
    }

    assert(!AD.IsEntirelyDead &&
           "Should never see completely dead values here");

    // OK, we need to explode this argument.
    unsigned ArgOffset = ++TotalArgIndex;
    unsigned OldArgIndex = ArgOffset - 1;
    llvm::SmallVector<SILValue, 8> LeafValues;

    // We do this in the same order as leaf types since ProjTree expects that
    // the order of leaf values matches the order of leaf types.
    llvm::SmallVector<const ProjectionTreeNode *, 8> LeafNodes;
    AD.ProjTree.getLiveLeafNodes(LeafNodes);

    for (auto *Node : LeafNodes) {
      auto OwnershipKind = *AD.getTransformedOwnershipKind(Node->getType());
      auto *Argument =
          BB->insertFunctionArgument(ArgOffset, Node->getType(), OwnershipKind,
                                     BB->getArgument(OldArgIndex)->getDecl());
      Argument->copyFlags(AD.Arg);
      LeafValues.push_back(Argument);
      TransformDescriptor.AIM[TotalArgIndex - 1] = AD.Index;
      ++ArgOffset;
      ++TotalArgIndex;
    }

    // Then go through the projection tree constructing aggregates and replacing
    // uses.
    AD.ProjTree.replaceValueUsesWithLeafUses(
        Builder, BB->getParent()->getLocation(), LeafValues);

    // We ignored debugvalue uses when we constructed the new arguments, in
    // order to preserve as much information as possible, we construct a new
    // value for OrigArg from the leaf values and use that in place of the
    // OrigArg.
    SILValue NewOrigArgValue = AD.ProjTree.computeExplodedArgumentValue(
        Builder, BB->getParent()->getLocation(), LeafValues);

    // Replace all uses of the original arg with the new value.
    SILArgument *OrigArg = BB->getArgument(OldArgIndex);
    OrigArg->replaceAllUsesWith(NewOrigArgValue);

    // Now erase the old argument since it does not have any uses. We also
    // decrement ArgOffset since we have one less argument now.
    BB->eraseArgument(OldArgIndex);
    --TotalArgIndex;
  }
}
