//===--- ARCAnalysis.h - SIL ARC Analysis -----------------------*- C++ -*-===//
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

#ifndef SWIFT_SILOPTIMIZER_ANALYSIS_ARCANALYSIS_H
#define SWIFT_SILOPTIMIZER_ANALYSIS_ARCANALYSIS_H

#include "swift/Basic/LLVM.h"
#include "swift/SIL/SILArgument.h"
#include "swift/SIL/SILBasicBlock.h"
#include "swift/SIL/SILValue.h"
#include "swift/SILOptimizer/Analysis/AliasAnalysis.h"
#include "swift/SILOptimizer/Analysis/PostOrderAnalysis.h"
#include "swift/SILOptimizer/Analysis/RCIdentityAnalysis.h"
#include "llvm/ADT/MapVector.h"
#include "llvm/ADT/TinyPtrVector.h"

namespace swift {

class SILInstruction;
class AliasAnalysis;
class PostOrderAnalysis;
class RCIdentityAnalysis;
class RCIdentityFunctionInfo;
class LoopRegionFunctionInfo;
class SILLoopInfo;
class SILFunction;

} // end namespace swift

namespace swift {

/// Return true if this is a retain instruction.
bool isRetainInstruction(SILInstruction *II);

/// Return true if this is a release instruction.
bool isReleaseInstruction(SILInstruction *II);

/// \returns True if the user \p User decrements the ref count of \p Ptr.
bool mayDecrementRefCount(SILInstruction *User, SILValue Ptr,
                          AliasAnalysis *AA);

/// \returns True if the \p User might use the pointer \p Ptr in a manner that
/// requires \p Ptr to be alive before Inst or the release of Ptr may use memory
/// accessed by \p User.
bool mayHaveSymmetricInterference(SILInstruction *User, SILValue Ptr,
                                 AliasAnalysis *AA);

/// \returns True if the \p User must use the pointer \p Ptr in a manner that
/// requires \p Ptr to be alive before Inst.
bool mustUseValue(SILInstruction *User, SILValue Ptr, AliasAnalysis *AA);

/// Returns true if User must use Ptr in a guaranteed way.
///
/// This means that assuming that everything is conservative, we can ignore the
/// ref count effects of User on Ptr since we will only remove things over
/// guaranteed parameters if we are known safe in both directions.
bool mustGuaranteedUseValue(SILInstruction *User, SILValue Ptr,
                            AliasAnalysis *AA);

/// Returns true if \p Inst can never conservatively decrement reference counts.
bool canNeverDecrementRefCounts(SILInstruction *Inst);

/// Returns true if \p Inst may access any indirect object either via an address
/// or reference.
///
/// If false is returned and \p Inst has an address or reference type operand,
/// then \p Inst only operates on the value of the address itself, not the
/// memory. i.e. it does not dereference the address.
bool canUseObject(SILInstruction *Inst);

/// \returns true if the user \p User may use \p Ptr in a manner that requires
/// Ptr's life to be guaranteed to exist at this point.
///
/// TODO: Better name.
bool mayGuaranteedUseValue(SILInstruction *User, SILValue Ptr,
                           AliasAnalysis *AA);

/// If \p Op has arc uses in the instruction range [Start, End), return the
/// first such instruction. Otherwise return None. We assume that
/// Start and End are both in the same basic block.
llvm::Optional<SILBasicBlock::iterator>
valueHasARCUsesInInstructionRange(SILValue Op, SILBasicBlock::iterator Start,
                                  SILBasicBlock::iterator End,
                                  AliasAnalysis *AA);

/// If \p Op has arc uses in the instruction range [Start, End), return the last
/// use of such instruction. Otherwise return None. We assume that Start and End
/// are both in the same basic block.
llvm::Optional<SILBasicBlock::iterator>
valueHasARCUsesInReverseInstructionRange(SILValue Op,
                                         SILBasicBlock::iterator Start,
                                         SILBasicBlock::iterator End,
                                         AliasAnalysis *AA);

/// If \p Op has instructions in the instruction range (Start, End] which may
/// decrement it, return the first such instruction. Returns None
/// if no such instruction exists. We assume that Start and End are both in the
/// same basic block.
llvm::Optional<SILBasicBlock::iterator>
valueHasARCDecrementOrCheckInInstructionRange(SILValue Op,
                                              SILBasicBlock::iterator Start,
                                              SILBasicBlock::iterator End,
                                              AliasAnalysis *AA);

/// A class that attempts to match owned return value and corresponding
/// epilogue retains for a specific function.
///
/// If we can not find the retain in the return block, we will try to find
/// in the predecessors. 
///
/// The search stop when we encounter an instruction that may decrement
/// the returned value, as we do not want to create a lifetime gap once the
/// retain is moved.
class ConsumedResultToEpilogueRetainMatcher {
public:
  /// The state on how retains are found in a basic block.
  enum class FindRetainKind { 
    None,      ///< Did not find a retain.
    Found,     ///< Found a retain.
    Recursion, ///< Found a retain and its due to self-recursion.
    Blocked    ///< Found a blocking instructions, i.e. MayDecrement.
  };

  using RetainKindValue = std::pair<FindRetainKind, SILInstruction *>;

private:
  SILFunction *F;
  RCIdentityFunctionInfo *RCFI;
  AliasAnalysis *AA;

  // We use a list of instructions for now so that we can keep the same interface
  // and handle exploded retain_value later.
  TinyPtrVector<SILInstruction *> EpilogueRetainInsts;

public:
  /// Finds matching releases in the return block of the function \p F.
  ConsumedResultToEpilogueRetainMatcher(RCIdentityFunctionInfo *RCFI,
                                        AliasAnalysis *AA,
                                        SILFunction *F);

  /// Finds matching releases in the provided block \p BB.
  void findMatchingRetains(SILBasicBlock *BB);

  ArrayRef<SILInstruction *> getEpilogueRetains() const {
    return EpilogueRetainInsts;
  }

  /// Recompute the mapping from argument to consumed arg.
  void recompute();

  using iterator = decltype(EpilogueRetainInsts)::iterator;
  using const_iterator = decltype(EpilogueRetainInsts)::const_iterator;
  iterator begin() { return EpilogueRetainInsts.begin(); }
  iterator end() { return EpilogueRetainInsts.end(); }
  const_iterator begin() const { return EpilogueRetainInsts.begin(); }
  const_iterator end() const { return EpilogueRetainInsts.end(); }

  using reverse_iterator = decltype(EpilogueRetainInsts)::reverse_iterator;
  using const_reverse_iterator = decltype(EpilogueRetainInsts)::const_reverse_iterator;
  reverse_iterator rbegin() { return EpilogueRetainInsts.rbegin(); }
  reverse_iterator rend() { return EpilogueRetainInsts.rend(); }
  const_reverse_iterator rbegin() const { return EpilogueRetainInsts.rbegin(); }
  const_reverse_iterator rend() const { return EpilogueRetainInsts.rend(); }

  unsigned size() const { return EpilogueRetainInsts.size(); }

  iterator_range<iterator> getRange() { return swift::make_range(begin(), end()); }

private:
  /// Return true if all the successors of the EpilogueRetainInsts do not have
  /// a retain.
  bool
  isTransitiveSuccessorsRetainFree(const llvm::DenseSet<SILBasicBlock *> &BBs);

  /// Finds matching releases in the provided block \p BB.
  RetainKindValue findMatchingRetainsInBasicBlock(SILBasicBlock *BB,
                                                  SILValue V);
};

/// A class that attempts to match owned arguments and corresponding epilogue
/// releases for a specific function.
///
/// Only try to find the epilogue release in the return block.
class ConsumedArgToEpilogueReleaseMatcher {
public:
  enum class ExitKind { Return, Throw };

private:
  SILFunction *F;
  RCIdentityFunctionInfo *RCFI;
  ExitKind Kind;
  ArrayRef<SILArgumentConvention> ArgumentConventions;

  class ArgumentState {
    /// The list of releases associated with this argument.
    TinyPtrVector<SILInstruction *> releases;

    /// If this is set to true, then we know that we were able to find
    /// a set of releases.
    bool jointPostDominatingReleaseSet;

  public:
    ArgumentState(ArrayRef<SILInstruction *> releases)
        : releases(releases), jointPostDominatingReleaseSet(false) {}

    void addRelease(SILInstruction *release) { releases.push_back(release); }
    void setHasJointPostDominatingReleaseSet() {
      jointPostDominatingReleaseSet = true;
    }

    bool foundSomeButNotAllReleases() const {
      return releases.size() && !jointPostDominatingReleaseSet;
    }

    /// If we were able to find a set of releases for this argument that joint
    /// post-dominate the argument, return our release set.
    llvm::Optional<ArrayRef<SILInstruction *>> getFullyPostDomReleases() const {
      if (releases.empty() || foundSomeButNotAllReleases())
        return llvm::None;
      return ArrayRef<SILInstruction *>(releases);
    }

    /// If we were able to find a set of releases for this argument, but those
    /// releases do not joint post-dominate the argument, return our release
    /// set.
    ///
    /// *NOTE* This returns none if we did not find any releases.
    llvm::Optional<ArrayRef<SILInstruction *>>
    getPartiallyPostDomReleases() const {
      if (releases.empty() || !foundSomeButNotAllReleases())
        return llvm::None;
      return ArrayRef<SILInstruction *>(releases);
    }
  };
  llvm::SmallMapVector<SILArgument *, ArgumentState, 8> ArgInstMap;

  /// Eventually this will be used in place of HasBlock.
  SILBasicBlock *ProcessedBlock;

public:
  /// Finds matching releases in the return block of the function \p F.
  ConsumedArgToEpilogueReleaseMatcher(
      RCIdentityFunctionInfo *RCFI,
      SILFunction *F,
      ArrayRef<SILArgumentConvention> ArgumentConventions =
          {SILArgumentConvention::Direct_Owned},
      ExitKind Kind = ExitKind::Return);

  /// Finds matching releases in the provided block \p BB.
  void findMatchingReleases(SILBasicBlock *BB);

  bool hasBlock() const { return ProcessedBlock != nullptr; }

  bool isEpilogueRelease(SILInstruction *i) const {
    // This is not a release instruction in the epilogue block.
    if (i->getParent() != ProcessedBlock)
      return false;

    using PairTy = const std::pair<SILArgument *, ArgumentState>;
    return llvm::any_of(ArgInstMap, [&i](PairTy &p) {
      auto completeList = p.second.getFullyPostDomReleases();
      // If we did not have a complete post dominating release set, then we do
      // not want to treat any releases from p as epilogue releases.
      if (!completeList)
        return false;

      // Then make sure that we either found an epilogue release or found
      // exploded epilogue releases. We rely on our callers to split up exploded
      // parameters.
      return completeList->size() == 1 && *completeList->begin() == i;
    });
  }

  /// Return true if we've found some epilogue releases for the argument
  /// but not all.
  bool hasSomeReleasesForArgument(SILArgument *arg) const {
    auto iter = ArgInstMap.find(arg);
    if (iter == ArgInstMap.end())
      return false;
    return iter->second.foundSomeButNotAllReleases();
  }

  bool isSingleRelease(SILArgument *arg) const {
    auto iter = ArgInstMap.find(arg);
    assert(iter != ArgInstMap.end() &&
           "Failed to get release list for argument");

    // If we do not have a fully post dominating release set bail.
    auto completeList = iter->second.getFullyPostDomReleases();
    if (!completeList)
      return false;

    return completeList->size() == 1;
  }

  SILInstruction *getSingleReleaseForArgument(SILArgument *arg) const {
    auto iter = ArgInstMap.find(arg);
    if (iter == ArgInstMap.end())
      return nullptr;
    if (!isSingleRelease(arg))
      return nullptr;
    auto completeList = iter->second.getFullyPostDomReleases();
    if (!completeList)
      return nullptr;
    return *completeList->begin();
  }

  SILInstruction *getSingleReleaseForArgument(SILValue value) const {
    auto *arg = dyn_cast<SILArgument>(value);
    if (!arg)
      return nullptr;
    return getSingleReleaseForArgument(arg);
  }

  ArrayRef<SILInstruction *> getReleasesForArgument(SILArgument *arg) const {
    auto iter = ArgInstMap.find(arg);
    if (iter == ArgInstMap.end())
      return {};
    auto completeList = iter->second.getFullyPostDomReleases();
    if (!completeList)
      return {};
    return completeList.value();
  }

  llvm::Optional<ArrayRef<SILInstruction *>>
  getPartiallyPostDomReleaseSet(SILArgument *arg) const {
    auto iter = ArgInstMap.find(arg);
    if (iter == ArgInstMap.end())
      return llvm::None;
    auto partialList = iter->second.getPartiallyPostDomReleases();
    if (!partialList)
      return llvm::None;
    return partialList;
  }

  ArrayRef<SILInstruction *> getReleasesForArgument(SILValue value) const {
    auto *arg = dyn_cast<SILArgument>(value);
    if (!arg)
      return {};
    return getReleasesForArgument(arg);
  }

  /// Recompute the mapping from argument to consumed arg.
  void recompute();

  bool isSingleReleaseMatchedToArgument(SILInstruction *inst) {
    using PairTy = const std::pair<SILArgument *, ArgumentState>;
    return count_if(ArgInstMap, [&inst](PairTy &p) {
      auto completeList = p.second.getFullyPostDomReleases();
      if (!completeList || completeList->size() > 1)
        return false;
      return *completeList->begin() == inst;
    });
  }

private:
  /// Return true if we have seen releases to part or all of \p Derived in
  /// \p Insts.
  ///
  /// NOTE: This function relies on projections to analyze the relation
  /// between the releases values in \p Insts and \p Derived, it also bails
  /// out and return true if projection path can not be formed between Base
  /// and any one the released values.
  bool isRedundantRelease(ArrayRef<SILInstruction *> Insts, SILValue Base,
                          SILValue Derived);

  /// Return true if we have a release instruction for all the reference
  /// semantics part of \p Argument.
  bool releaseArgument(ArrayRef<SILInstruction *> Insts, SILValue Argument);

  /// Walk the basic block and find all the releases that match to function
  /// arguments.
  void collectMatchingReleases(SILBasicBlock *BB);

  /// Walk the function and find all the destroy_addr instructions that match
  /// to function arguments.
  void collectMatchingDestroyAddresses(SILBasicBlock *BB);

  /// For every argument in the function, check to see whether all epilogue
  /// releases are found. Clear all releases for the argument if not all
  /// epilogue releases are found.
  void processMatchingReleases();
};

/// Match a call to a trap BB with no ARC relevant side effects.
bool isARCInertTrapBB(const SILBasicBlock *BB);

} // end namespace swift

#endif
