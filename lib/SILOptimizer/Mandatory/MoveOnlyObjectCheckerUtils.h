//===--- MoveOnlyObjectCheckerUtils.h -------------------------------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2022 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//
///
/// This is for shared code in between the move only object checker and the move
/// only address checker. This is needed since the move only address checker
/// uses the move only object checker to check values loaded from allocations
/// that it is analyzing.
///
//===----------------------------------------------------------------------===//

#ifndef SWIFT_SILOPTIMIZER_MANDATORY_MOVEONLYOBJECTCHECKERUTILS_H
#define SWIFT_SILOPTIMIZER_MANDATORY_MOVEONLYOBJECTCHECKERUTILS_H

#include "swift/SILOptimizer/Utils/CanonicalizeOSSALifetime.h"
#include "llvm/Support/Compiler.h"

#include "MoveOnlyBorrowToDestructureUtils.h"

namespace swift {
namespace siloptimizer {

class DiagnosticEmitter;

/// Wrapper around CanonicalizeOSSALifetime that we use to specialize its
/// interface for our purposes.
struct OSSACanonicalizer {
  /// A per mark must check, vector of uses that copy propagation says need a
  /// copy and thus are not final consuming uses.
  SmallVector<SILInstruction *, 32> consumingUsesNeedingCopy;

  /// A per mark must check, vector of consuming uses that copy propagation says
  /// are actual last uses.
  SmallVector<SILInstruction *, 32> consumingBoundaryUsers;

  /// A list of non-consuming boundary uses.
  SmallVector<SILInstruction *, 32> nonConsumingBoundaryUsers;

  CanonicalizeOSSALifetime canonicalizer;

  OSSACanonicalizer(SILFunction *fn, DominanceInfo *domTree,
                    InstructionDeleter &deleter)
      : canonicalizer(false /*pruneDebugMode*/,
                      !fn->shouldOptimize() /*maximizeLifetime*/, fn,
                      nullptr /*accessBlockAnalysis*/, domTree,
                      nullptr /*calleeAnalysis*/, deleter) {}

  void clear() {
    consumingUsesNeedingCopy.clear();
    consumingBoundaryUsers.clear();
    nonConsumingBoundaryUsers.clear();
  }

  struct LivenessState {
    OSSACanonicalizer &parent;
    CanonicalizeOSSALifetime::LivenessState canonicalizerState;

    LivenessState(OSSACanonicalizer &parent, SILValue def)
        : parent(parent), canonicalizerState(parent.canonicalizer, def) {}

    ~LivenessState() { parent.clear(); }
  };

  /// MARK: The following APIs require an active on-stack instance of
  /// LivenessState:
  ///
  ///     OSSACanonicalizer::LivenessState livenessState(canonicalizer, value);

  /// Perform computeLiveness, computeBoundaryData, and rewriteLifetimes in one
  /// fell swoop.
  bool canonicalize();

  bool computeLiveness() { return canonicalizer.computeLiveness(); }

  void computeBoundaryData(SILValue value);

  void rewriteLifetimes() { canonicalizer.rewriteLifetimes(); }

  void findOriginalBoundary(PrunedLivenessBoundary &resultingFoundBoundary) {
    canonicalizer.findOriginalBoundary(resultingFoundBoundary);
  }

  bool foundAnyConsumingUses() const {
    return consumingUsesNeedingCopy.size() || consumingBoundaryUsers.size();
  }

  bool foundConsumingUseRequiringCopy() const {
    return consumingUsesNeedingCopy.size();
  }

  bool foundFinalConsumingUses() const { return consumingBoundaryUsers.size(); }

  /// Helper method for hasPartialApplyConsumingUse and
  /// hasNonPartialApplyConsumingUse.
  static bool isPartialApplyUser(SILInstruction *user) {
    // If our user is an owned moveonlywrapper to copyable value inst, search
    // through it for a partial_apply user.
    if (auto *mtci = dyn_cast<MoveOnlyWrapperToCopyableValueInst>(user)) {
      if (mtci->hasOwnedInitialKind()) {
        return llvm::any_of(mtci->getUses(), [](Operand *use) {
          return isa<PartialApplyInst>(use->getUser());
        });
      }
    }
    return isa<PartialApplyInst>(user);
  }

  static bool isNotPartialApplyUser(SILInstruction *user) {
    // If our user is an owned moveonlywrapper to copyable value inst, search
    // through it for a partial_apply user.
    if (auto *mtci = dyn_cast<MoveOnlyWrapperToCopyableValueInst>(user)) {
      if (mtci->hasOwnedInitialKind()) {
        return llvm::any_of(mtci->getUses(), [](Operand *use) {
          return !isa<PartialApplyInst>(use->getUser());
        });
      }
    }
    return !isa<PartialApplyInst>(user);
  }

  bool hasPartialApplyConsumingUse() const {
    auto test = OSSACanonicalizer::isPartialApplyUser;
    return llvm::any_of(consumingUsesNeedingCopy, test) ||
           llvm::any_of(consumingBoundaryUsers, test);
  }

  bool hasNonPartialApplyConsumingUse() const {
    auto test = OSSACanonicalizer::isNotPartialApplyUser;
    return llvm::any_of(consumingUsesNeedingCopy, test) ||
           llvm::any_of(consumingBoundaryUsers, test);
  }

  struct DropDeinitFilter {
    bool operator()(SILInstruction *inst) const {
      return isa<DropDeinitInst>(inst);
    }
  };
  using DropDeinitIter =
      llvm::filter_iterator<SILInstruction *const *, DropDeinitFilter>;
  using DropDeinitRange = iterator_range<DropDeinitIter>;

  /// Returns a range of final uses of the mark_must_check that are drop_deinit
  DropDeinitRange getDropDeinitUses() const {
    return llvm::make_filter_range(consumingBoundaryUsers, DropDeinitFilter());
  }
};

/// Search for candidate object mark_must_checks. If we find one that does not
/// fit a pattern that we understand, emit an error diagnostic telling the
/// programmer that the move checker did not know how to recognize this code
/// pattern.
///
/// \returns true if we deleted a mark_must_check inst that we didn't recognize
/// after emitting the diagnostic.
///
/// To check if an error was emitted call \p
/// diagnosticEmitter.getDiagnosticCount().
///
/// NOTE: This is the routine used by the move only object checker to find mark
/// must checks to process.
bool searchForCandidateObjectMarkMustChecks(
    SILFunction *fn,
    SmallSetVector<MarkMustCheckInst *, 32> &moveIntroducersToProcess,
    DiagnosticEmitter &diagnosticEmitter);

struct MoveOnlyObjectChecker {
  DiagnosticEmitter &diagnosticEmitter;
  DominanceInfo *domTree;
  PostOrderAnalysis *poa;
  borrowtodestructure::IntervalMapAllocator &allocator;

  /// Returns true if we changed the IR in any way. Check with \p
  /// diagnosticEmitter to see if we emitted any diagnostics.
  bool check(llvm::SmallSetVector<MarkMustCheckInst *, 32> &instsToCheck);
};

} // namespace siloptimizer
} // namespace swift

#endif
