//===- LexicalDestroyFolding.cpp - Fold destroys into final owned applies -===//
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
/// After ShrinkBorrowScope and CanonicalizeOSSALifetime both run, when a final
/// use of the extended simple lifetime of a begin_borrow [lexical] is as an
/// owned argument, we will have the following pattern:
///
/// %result = apply %fn(..., %copy, ...) : $... (..., @owned Ty, ...)
/// end_borrow %lifetime : $Ty
/// destroy_value %dvi
///
/// where %lifetime is the result of our begin_borrow [lexical]:
///
/// %lifetime = begin_borrow [lexical] %borrowee : $Ty
///
/// and %copy is a (transitive) copy of it
///
/// %copy = copy_value (copy_value (... %lifetime))
///
/// At that point, we want to fold the destroy_value, apply, and borrow into
///
/// end_borrow %lifetime : $Ty
/// apply %fn(..., %move, ...) : $... (..., @owned Ty, ...)
///
/// where %move is a the result of a new instruction added above our scope:
///
/// %move = move_value [lexical] %borrowee : $Ty
/// %lifetime = begin_borrow %move : $Ty
///
/// We only want to do this when it would mean allowing the callee to end a
/// lifetime of a value that the caller owned--if we can transfer ownership from
/// the caller to the callee.  In order to transfer ownership, the caller must
/// first own the value, so we are only interested in borrows of owned values:
///
/// %borrowee : @owned $Ty
/// %lifetime = begin_borrow [lexical] %borrowee
///
/// At the other end, we can only transfer ownership if there are no more
/// interesting uses of the owned value after the apply.  Specifically, we
/// require that the instruction after the end_borrow be a destroy_value of the
/// borrowee.
///
/// The simplest example:
///
/// %copy = copy_value %lifetime : $Ty
/// %result = apply %fn(..., %copy, ...) : $... (..., @owned Ty, ...)
/// end_borrow %lifetime : $Ty
/// destroy_value %borrowee : $Ty
///
/// Taken together, we get the simplest example of this transformation:
///
/// INPUT:
///
///     %borrowee : @owned
///     %lifetime = begin_borrow [lexical] %owned
///     %copy = copy_value %lifetime
///     apply %fn(%copy) : $... (@owned)
///     end_borrow %lifetime
///     destroy_value %borrowee
///
/// OUTPUT:
///
///     %borrowee : @owned
///     %move = move_value [lexical] %borrowee
///     %lifetime = begin_borrow [lexical] %move
///     end_borrow %lifetime
///     apply %fn(%move) : $... (@owned)
///
/// TODO: Handle partial_apply, try_apply, and begin_apply.
//===----------------------------------------------------------------------===//

#include "swift/SIL/BasicBlockDatastructures.h"
#include "swift/SIL/BasicBlockUtils.h"
#include "swift/SIL/OwnershipUtils.h"
#include "swift/SIL/PrunedLiveness.h"
#include "swift/SIL/SILBasicBlock.h"
#include "swift/SIL/SILBuilder.h"
#include "swift/SIL/SILInstruction.h"
#include "swift/SIL/SILNode.h"
#include "swift/SIL/Test.h"
#include "swift/SILOptimizer/Analysis/Reachability.h"
#include "swift/SILOptimizer/PassManager/Transforms.h"
#include "swift/SILOptimizer/Utils/CanonicalizeBorrowScope.h"
#include "swift/SILOptimizer/Utils/CanonicalizeOSSALifetime.h"
#include "swift/SILOptimizer/Utils/InstructionDeleter.h"
#include "swift/SILOptimizer/Utils/SILSSAUpdater.h"
#include "llvm/ADT/SmallVector.h"

#define DEBUG_TYPE "copy-propagation"

using namespace swift;

//===----------------------------------------------------------------------===//
//                       MARK: LexicalDestroyFolding
//===----------------------------------------------------------------------===//

namespace LexicalDestroyFolding {

/// The environment within which to fold.
struct Context final {
  /// The instruction that begins the borrow scope.
  BeginBorrowInst *const introducer;

  /// The function containing the introducer.
  ///
  /// introducer->getFunction()
  SILFunction *const function;

  /// BorrowedValue(introducer)
  BorrowedValue const borrowedValue;

  /// The value whose lifetime is guaranteed by the lexical borrow scope.
  ///
  /// introducer->getOperand()
  SILValue const borrowee;

  DominanceInfo &dominanceTree;

  InstructionDeleter &deleter;

public:
  Context(BeginBorrowInst *introducer, DominanceInfo &dominanceTree,
          InstructionDeleter &deleter)
      : introducer(introducer), function(introducer->getFunction()),
        borrowedValue(BorrowedValue(introducer)),
        borrowee(introducer->getOperand()), dominanceTree(dominanceTree),
        deleter(deleter) {
    assert(introducer->isLexical());
    assert(introducer->getOperand()->getOwnershipKind() ==
           OwnershipKind::Owned);
    assert(borrowedValue.isLocalScope());
  }
};

/// Fold within the specified context.
MoveValueInst *run(Context &);

/// The consuming use pattern we are trying to match and transform.
struct Match final {
  ApplyInst *ai = nullptr;
  EndBorrowInst *ebi = nullptr;
  DestroyValueInst *dvi = nullptr;

  /// Whether the match is a candidate for folding.
  ///
  /// A partial match--which has both the end_borrow and the destroy value
  /// but no apply--cannot itself be folded but is not an obstruction to
  /// folding other candidates.
  bool isFullMatch() {
    assert(ebi != nullptr);
    assert(dvi != nullptr);
    return ai != nullptr;
  }
};

/// A sequence of instructions under consideration for folding
struct Candidate final {
  /// The instruction sequence itself.
  Match match;
  /// Whether the candidate could indeed be folded as determined by
  /// isViableMatch.
  bool viable;
  /// The indices of the arguments of the apply to rewrite as determined by
  /// rewritableArgumentIndicesForApply.
  SmallVector<int, 2> argumentIndices;
};

/// The degree to which a match is a candidate for folding.
enum class MatchViability {
  /// This match can be folded, supposing any can.
  Viable,
  /// This match cannot be folded, even if others can.
  Nonviable,
  /// Neither this match nor any other can be folded.
  Illegal
};

struct Candidates final {
  /// The sequences of scope ending instructions that are under consideration
  /// for folding.
  llvm::SmallVector<Candidate, 4> vector;
};

/// Quickly filter scope ends of %lifetime that MIGHT BE foldable.
class FindCandidates final {

  Context const &context;

public:
  FindCandidates(Context const &context) : context(context) {}

  /// Find among the scope ending instructions of %lifetime any that match the
  /// expected instruction sequence pattern.
  ///
  /// Uses definesMatchingInstructionSequence to determine whether an
  /// instruction is a candidate for folding.
  ///
  /// Includes both full matches which could potentially be folded and partial
  /// matches which need to be verified later aren't illegal in isViableMatch.
  ///
  /// returns true if any full matches are found
  ///         false otherwise
  bool run(Candidates &);

private:
  /// Fast check for whether the given instruction is a candidate for folding.
  ///
  /// Tries to find patterns like
  ///
  ///     apply
  ///     end_borrow
  ///     ...           // instructions which CanonicalizeOSSALifetime will not
  ///                   // hoist destroys over
  ///     destroy_value %borrowee
  ///
  /// If None is returned, we can't do the transformation at any end_borrow.
  /// If a Match WITHOUT an apply is returned, we can't do the transformation
  /// on the provided instruction but we still might be able to do it on the
  /// other scope ending instructions.
  /// If a Match WITH an apply is returned, we might be able to transform this
  /// instruction, so more expensive checks are in order.
  llvm::Optional<Match>
  definesMatchingInstructionSequence(SILInstruction *) const;

  /// Whether the specified instruction is or might be the beginning of a
  /// sequence of "inconsequential" instructions the last of which destroys
  /// %borrowee.
  ///
  /// CanonicalizeOSSALifetime will put destroy_value instructions after every
  /// final non-consuming use of %borrowee.  So it will put a
  /// destroy_value after an
  ///     end_borrow %lifetime
  /// if there are no subsequent uses of %borrowee.  However, if there
  /// is already one or more other instructions whose opcodes satisfy
  /// CanonicalizeOSSALifetime::ignoredByDestroyHoisting just after
  ///     end_borrow %lifetime,
  /// it won't hoist the
  ///     destroy_value %borrowee
  /// over them.
  ///
  /// Consequently, it's not good enough just to look at the instruction
  /// immediately following
  ///     end_borrow %lifetime
  /// Instead, we need to look over the sequence of them.
  ///
  /// To be certain that the sequence is valid, we need to check that none of
  /// the instructions between the end_borrow and the destroy_value are users of
  /// %borrowee.  That requires having a set of uses to check for membership
  /// in, though, so that is postponed until isViableMatch.
  ///
  /// returns the destroy_value instruction in the sequence that destroys
  ///         %borrowee or nullptr if there isn't one
  DestroyValueInst *findNextBorroweeDestroy(SILInstruction *) const;
};

/// How %lifetime is used.
struct IntroducerUsage final {
  /// The operands that are uses of introducer.
  SmallPtrSet<Operand *, 16> uses;
  /// The instructions that are users of the simple extended borrow scope.
  SmallPtrSet<SILInstruction *, 16> users;
};

/// Identifies all the simple extended users of %lifetime.
///
/// returns true if none of the uses of %lifetime escaped
///         false otherwise
bool findIntroducerUsage(Context const &, IntroducerUsage &);

/// How %borrowee is used.
struct BorroweeUsage final {
  /// The operands that are uses of the borrowee.
  SmallVector<Operand *, 16> uses;
  /// The instructions that are users of the borrowee.
  ///
  /// A set of the users of uses for fast membership checking.
  SmallPtrSet<SILInstruction *, 16> users;
};

/// Find all uses of %borrowee that are dominated by introducer.
///
/// We are only interested in those dominated by introducer because those are
/// the uses all of which must be "outside" the liveness boundary of
/// %lifetime.  In detail, PrunedLiveness::isWithinBoundary relies on
/// clients to know that instructions are after the start of liveness.  We
/// determine this via the dominance tree.
bool findBorroweeUsage(Context const &, BorroweeUsage &);

/// Sift scope ends of %lifetime for those that CAN be folded.
class FilterCandidates final {

  Context const &context;
  IntroducerUsage const &introducerUsage;
  BorroweeUsage const &borroweeUsage;

public:
  FilterCandidates(Context const &context,
                   IntroducerUsage const &introducerUsage,
                   BorroweeUsage const &borroweeUsage)
      : context(context), introducerUsage(introducerUsage),
        borroweeUsage(borroweeUsage){};

  /// Determines whether each candidate is viable for folding.
  ///
  /// returns true if any candidates were viable
  ///         false otherwise
  bool run(Candidates &candidates);

private:
  /// Slow check, dependent on finding users, that a Match found by
  /// definesMatchingInstructionSequence can be folded.
  MatchViability isViableMatch(Match &, SmallVectorImpl<int> &) const;

  /// Find the arguments in the specified apply that could be rewritten.
  bool rewritableArgumentIndicesForApply(ApplySite,
                                         SmallVectorImpl<int> &indices) const;

  /// Whether the specified value is %lifetime or its iterated copy_value.
  ///
  /// In other words, it has to be a simple extended def of %lifetime.
  bool isSimpleExtendedIntroducerDef(SILValue value) const;
};

/// Whether there are any uses of the borrowee within the borrow scope.
///
/// If there are, we can't fold the apply.  Specifically, we can't introduce
/// a move_value [lexical] %borrowee because that value still needs to be used
/// in those locations.
///
/// For example, given the following SIL
///     %borrowee : @owned
///     %lifetime = begin_borrow [lexical] %borrowee
///     apply %take_guaranteed(%borrowee)
///     %copy = copy_value %lifetime
///     apply %take_owned(%copy)
///     end_borrow %lifetime
///     destroy_value %borrowee
/// we can't rewrite like
///     %borrowee : @owned
///     %move = move_value [lexical] %borrowee
///     %lifetime = begin_borrow [lexical] %move
///     apply %take_guaranteed(??????)
///     apply %take_owned(%move)
/// because there is no appropriate value to pass to %take_guaranteed.
/// Specifically, it's not legal to use %move there because that would make
/// the instruction a user of the lexical scope which it was not before.
bool borroweeHasUsesWithinBorrowScope(Context const &, BorroweeUsage const &);

/// Rewrite the appropriate scope ends of %lifetime.
class Rewriter final {

  Context &context;
  Candidates const &candidates;

  // The move_value [lexical] instruction that was added during the run.
  //
  // Defined during createMove.
  MoveValueInst *mvi = nullptr;

public:
  Rewriter(Context &context, Candidates const &candidates)
      : context(context), candidates(candidates){};

  /// Make all changes required to fold the viable candidates.
  ///
  /// Specifically:
  /// - create a single move_value [lexical]
  ///       %move = move_value [lexical] %borrowee
  ///       %lifetime = begin_borrow [lexical] %move
  /// - transform all the candidates from
  ///       apply %fn(..., %copy, ...)
  ///       end_borrow %lifetime
  ///       ...
  ///       destroy_value %borrowee
  ///   to
  ///       end_borrow %lifetime
  ///       apply %fn(..., %move, ...)
  ///       ...
  /// - update SSA now that a second def has been introduced for
  ///   %borrowee
  MoveValueInst *run();

private:
  /// Add the new move_value [lexical] above the begin_borrow [lexical].
  ///
  /// At most one will be created per run of LexicalDestroyFolding.
  ///
  /// returns false if the move_value [lexical] instruction was added already
  ///         true if the move_value [lexical] instruction was added just now
  bool createMove();

  /// Combine the matched instruction sequence
  ///
  ///   apply %fn(%copy)
  ///   end_borrow %lifetime
  ///   destroy_value %instance
  ///
  /// into
  ///
  ///   end_borrow %lifetime
  ///   apply %fn(%move)
  ///
  /// This is done in three steps:
  /// (1) rewrite the apply (and delete the copy_value that is fed to it if
  ///     possible)
  /// (2) hoist the end_borrow
  /// (3) delete the destroy_value
  void fold(Match, ArrayRef<int> rewritableArgumentIndices);
};

//===----------------------------------------------------------------------===//
//                             MARK: Driver
//===----------------------------------------------------------------------===//

/// Perform any possible folding.
///
/// Returns whether any change was made.
MoveValueInst *run(Context &context) {
  Candidates candidates;

  // Do a cheap search for scope ending instructions that could potentially be
  // candidates for folding.
  if (!FindCandidates(context).run(candidates))
    return nullptr;

  // At least one full match was found and more expensive checks on the matches
  // are in order.

  BorroweeUsage borroweeUsage;
  if (!findBorroweeUsage(context, borroweeUsage))
    return nullptr;
  IntroducerUsage introducerUsage;
  if (!findIntroducerUsage(context, introducerUsage))
    return nullptr;

  // Now, filter the candidates using those values.
  if (!FilterCandidates(context, introducerUsage, borroweeUsage)
           .run(candidates))
    return nullptr;

  // Finally, check that %borrowee has no uses within %lifetime's
  // borrow scope.
  if (borroweeHasUsesWithinBorrowScope(context, borroweeUsage))
    return nullptr;

  // It is safe to rewrite the viable candidates.  Do so.
  return Rewriter(context, candidates).run();
}

//===----------------------------------------------------------------------===//
//                             MARK: Rewriting
//===----------------------------------------------------------------------===//

MoveValueInst *Rewriter::run() {
  bool foldedAny = false;
  (void)foldedAny;
  auto size = candidates.vector.size();
  for (unsigned index = 0; index < size; ++index) {
    auto candidate = candidates.vector[index];
    createMove();
    if (!candidate.viable) {
      // Nonviable candidates still end with the pattern
      //
      //     end_borrow %lifetime
      //     ...
      //     destroy_value %borrowee
      //
      // Now that the new move_value [lexical] dominates all candidates, the
      // every candidate's destroy_value %borrowee is dominated by it, so every
      // one is dominated by another consuming use which is illegal.  Rewrite
      // each such destroy_value to be a destroy_value of the move.
      candidate.match.dvi->setOperand(mvi);
      continue;
    }

    fold(candidate.match, candidate.argumentIndices);
#ifndef NDEBUG
    foldedAny = true;
#endif
  }
  assert(foldedAny && "rewriting without anything to rewrite!?");
  return mvi;
}

bool Rewriter::createMove() {
  // We only will create a single MoveValueInst.
  if (mvi)
    return false;

  auto introducerBuilder = SILBuilderWithScope(context.introducer);
  mvi = introducerBuilder.createMoveValue(
      RegularLocation::getAutoGeneratedLocation(context.introducer->getLoc()),
      context.borrowee,
      /*isLexical=*/true);
  context.introducer->setOperand(mvi);
  return true;
}

void Rewriter::fold(Match candidate, ArrayRef<int> rewritableArgumentIndices) {
  // First, rewrite the apply in terms of the move_value.
  unsigned argumentNumber = 0;
  for (auto index : rewritableArgumentIndices) {
    auto argument = candidate.ai->getArgument(index);
    auto *cvi = cast<CopyValueInst>(argument);
    if (argumentNumber == 0) {
      candidate.ai->setArgument(index, mvi);
      if (!context.deleter.deleteIfDead(cvi)) {
        // We can't delete the copy_value because it has other users.  Instead,
        // add a compensating destroy just before the apply.
        auto applyBuilder = SILBuilderWithScope(candidate.ai);
        applyBuilder.createDestroyValue(
            RegularLocation::getAutoGeneratedLocation(candidate.ai->getLoc()),
            cvi);
      }
    } else {
      cvi->setOperand(mvi);
    }
    ++argumentNumber;
  }

  // At this point, we have something along the lines of
  //
  //   %move = move_value [lexical] %borrowee
  //   %lifetime = begin_borrow [lexical] %move
  //   ...
  //   apply %fn(%move)
  //   end_borrow %lifetime
  //
  // This isn't valid, though, because the apply consumes the %move but
  // the borrow scope guarantees it until the subsequent end_borrow.
  //
  // Fix this by hoisting the end_borrow above the apply.
  auto applyBuilder = SILBuilderWithScope(candidate.ai);
  applyBuilder.createEndBorrow(
      RegularLocation::getAutoGeneratedLocation(candidate.ai->getLoc()),
      context.introducer);
  context.deleter.forceDelete(candidate.ebi);

  // We have introduced a consuming use of %borrowee--the move_value-- and we
  // just rewrote the apply to consume it.  Delete the old destroy_value.
  context.deleter.forceDelete(candidate.dvi);
}

//===----------------------------------------------------------------------===//
//                             MARK: Lookups
//===----------------------------------------------------------------------===//

bool FindCandidates::run(Candidates &candidates) {
  llvm::SmallVector<SILInstruction *, 16> scopeEndingInsts;
  context.borrowedValue.getLocalScopeEndingInstructions(scopeEndingInsts);

  bool foundAnyFull = false;

  for (auto *instruction : scopeEndingInsts) {
    if (auto match = definesMatchingInstructionSequence(instruction)) {
      assert(match->ebi->getOperand() == context.introducer);
      assert(match->dvi->getOperand() == context.borrowee);
      candidates.vector.push_back({*match, false, {}});

      foundAnyFull = foundAnyFull || match->isFullMatch();
    } else {
      // The instruction doesn't define even a partial match.  Either the scope
      // ending instruction isn't an end_borrow or the subsequent instruction
      // isn't a destroy_value.  We can't fold any applies.
      return false;
    }
  }
  return foundAnyFull;
}

bool findIntroducerUsage(Context const &context, IntroducerUsage &usage) {
  SmallVector<Operand *, 16> useVector;
  if (!findExtendedUsesOfSimpleBorrowedValue(context.borrowedValue,
                                             &useVector)) {
    // If the value produced by begin_borrow escapes, don't shrink the borrow
    // scope over the apply.
    return false;
  }
  for (auto *use : useVector) {
    usage.uses.insert(use);
    usage.users.insert(use->getUser());
  }
  return true;
}

bool FilterCandidates::run(Candidates &candidates) {
  bool anyViable = false;

  // We have some end_borrows that might be candidates for folding.
  for (unsigned index = 0, count = candidates.vector.size(); index < count;
       ++index) {
    auto &candidate = candidates.vector[index];
    SmallVector<int, 2> rewritableArgumentIndices;
    auto viability = isViableMatch(candidate.match, candidate.argumentIndices);
    switch (viability) {
    case MatchViability::Viable:
      candidate.viable = true;
      anyViable = true;
      break;
    case MatchViability::Nonviable:
      break;
    case MatchViability::Illegal:
      return false;
      break;
    }
  }

  return anyViable;
}

bool findBorroweeUsage(Context const &context, BorroweeUsage &usage) {
  auto recordUse = [&](Operand *use) {
    // Ignore uses that aren't dominated by the introducer.  PrunedLiveness
    // relies on us doing this check.
    if (!context.dominanceTree.dominates(context.introducer, use->getUser()))
      return;
    usage.uses.push_back(use);
    usage.users.insert(use->getUser());
  };
  for (auto *use : context.borrowee->getUses()) {
    auto *user = use->getUser();
    if (user == context.introducer)
      continue;
    switch (use->getOperandOwnership()) {
    case OperandOwnership::PointerEscape:
      return false;
    case OperandOwnership::Borrow:
      if (!BorrowingOperand(use).visitScopeEndingUses([&](Operand *end) {
        if (end->getOperandOwnership() == OperandOwnership::Reborrow) {
          return false;
        }
        recordUse(end);
        return true;
      })) {
        return false;
      }
      break;
    default:
      break;
    }
    recordUse(use);
  }
  return true;
}

bool borroweeHasUsesWithinBorrowScope(Context const &context,
                                      BorroweeUsage const &usage) {
  MultiDefPrunedLiveness liveness(context.function);
  context.borrowedValue.computeTransitiveLiveness(liveness);
  DeadEndBlocks deadEndBlocks(context.function);
  return !liveness.areUsesOutsideBoundary(usage.uses, &deadEndBlocks);
}

//===----------------------------------------------------------------------===//
//                             MARK: Predicates
//===----------------------------------------------------------------------===//

llvm::Optional<Match>
FindCandidates::definesMatchingInstructionSequence(SILInstruction *inst) const {
  // Look specifically for
  //
  //   apply
  //   end_borrow // inst
  //   ...
  //   destroy_value %borrowee
  auto *ebi = dyn_cast<EndBorrowInst>(inst);
  if (!ebi)
    return llvm::None;
  auto *dvi = findNextBorroweeDestroy(ebi->getNextInstruction());
  if (!dvi)
    return llvm::None;
  auto *ai = dyn_cast_or_null<ApplyInst>(ebi->getPreviousInstruction());
  if (!ai)
    return {{nullptr, ebi, dvi}};

  return {{ai, ebi, dvi}};
}

DestroyValueInst *
FindCandidates::findNextBorroweeDestroy(SILInstruction *from) const {
  for (auto *inst = from; inst; inst = inst->getNextInstruction()) {
    if (!CanonicalizeOSSALifetime::ignoredByDestroyHoisting(inst->getKind())) {
      // This is not an instruction that CanonicalizeOSSALifetime would not
      // hoist a destroy above.  In other words, CanonicalizeOSSALifetime would
      // have hoisted
      //     destroy_value %borrowee
      // over this instruction if it could have.  Stop looking.
      return nullptr;
    }
    if (auto *dvi = dyn_cast<DestroyValueInst>(inst)) {
      if (dvi->getOperand() == context.borrowee) {
        return dvi;
      }
    }
  }
  return nullptr;
}

MatchViability FilterCandidates::isViableMatch(
    Match &candidate, SmallVectorImpl<int> &rewritableArgumentIndices) const {
  for (SILInstruction *inst = candidate.ebi; inst != candidate.dvi;
       inst = inst->getNextInstruction()) {
    if (borroweeUsage.users.contains(inst)) {
      // In the sequence of instructions
      //     end_borrow %lifetime
      //     ...
      //     destroy_value %borrowee
      // we have found a user of %borrowee.  That existing means not
      // only that this candidate is not viable but that NONE of the candidates
      // are viable and we need to bail out completely.
      return MatchViability::Illegal;
    }
  }

  // Now that we've checked that this partial match isn't illegal, go ahead and
  // discard it.
  if (!candidate.isFullMatch())
    return MatchViability::Nonviable;

  // If the apply isn't a user of the extended simple value, then this
  // transformation can't be done: the callee can't end the lexical lifetime
  // of a value it doesn't see.  Presumably, this apply is a deinit barrier.
  if (!introducerUsage.users.contains(candidate.ai))
    return MatchViability::Nonviable;

  // We aren't able to rewrite every simple extended use of %lifetime
  // in the apply.
  if (!rewritableArgumentIndicesForApply(candidate.ai,
                                         rewritableArgumentIndices))
    return MatchViability::Nonviable;

  return MatchViability::Viable;
}

bool FilterCandidates::isSimpleExtendedIntroducerDef(SILValue value) const {
  while (true) {
    auto *instruction = value.getDefiningInstruction();
    if (!instruction)
      return false;
    if (instruction == context.introducer)
      return true;
    if (auto *cvi = dyn_cast<CopyValueInst>(instruction)) {
      value = cvi->getOperand();
      continue;
    }
    return false;
  }
}

bool FilterCandidates::rewritableArgumentIndicesForApply(
    ApplySite apply, SmallVectorImpl<int> &indices) const {
  for (auto &operand : apply->getAllOperands()) {
    if (introducerUsage.uses.contains(&operand)) {
      if (apply.isArgumentOperand(operand)) {
        auto convention = apply.getArgumentConvention(operand);
        if (isSimpleExtendedIntroducerDef(operand.get()) &&
            convention.isOwnedConvention()) {
          indices.push_back(apply.getCalleeArgIndex(operand));
        } else {
          // This argument is a use of %lifetime but not an owned use that we
          // can rewrite.
          return false;
        }
      } else {
        // If %lifetime is used in some non-argument position, e.g. as the
        // callee, we can't fold.
        return false;
      }
    }
  }
  return true;
}

} // namespace LexicalDestroyFolding

//===----------------------------------------------------------------------===//
//                             MARK: Entry Point
//===----------------------------------------------------------------------===//

/// The entry point.
MoveValueInst *
swift::foldDestroysOfCopiedLexicalBorrow(BeginBorrowInst *bbi,
                                         DominanceInfo &dominanceTree,
                                         InstructionDeleter &deleter) {
  if (!bbi->isLexical())
    return nullptr;
  if (bbi->getOperand()->getOwnershipKind() != OwnershipKind::Owned)
    return nullptr;
  if (!dominanceTree.isReachableFromEntry(bbi->getParentBlock()))
    return nullptr;

  auto context = LexicalDestroyFolding::Context(bbi, dominanceTree, deleter);
  return LexicalDestroyFolding::run(context);
}

namespace swift::test {
// Arguments:
// - the lexical borrow to fold
// Dumps:
// - the function
static FunctionTest LexicalDestroyFoldingTest(
    "lexical-destroy-folding", [](auto &function, auto &arguments, auto &test) {
      auto *dominanceAnalysis = test.template getAnalysis<DominanceAnalysis>();
      DominanceInfo *domTree = dominanceAnalysis->get(&function);
      auto value = arguments.takeValue();
      auto *bbi = cast<BeginBorrowInst>(value);
      InstructionDeleter deleter;
      foldDestroysOfCopiedLexicalBorrow(bbi, *domTree, deleter);
      function.dump();
    });
} // end namespace swift::test
