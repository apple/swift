//===--- InstructionDeleter.h - InstructionDeleter utility ------*- C++ -*-===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2021 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//
///
/// Purpose:
///
/// 1. Avoid adding logic to passes to manually track dead code. This is one of
/// the most common sources of complexity and bugs when passes are composed of
/// multiple layers of utilities.
///
/// 2. Avoid passing InstModCallbacks across APIs. InstModCallbacks should be
/// instantiated very rarely within the implementation of a few utilities. It
/// should never need to passed across the utilities except at the very lowest
/// level. Callbacks are not actually composable. Chaining callbacks, and
/// keeping track of which callbacks are active and valid across code boundaries
/// is highly bug prone and infeasible.
///
/// 3. Fixup OSSA after deletion so SIL remains valid. Allowing OSSA to be
/// invalid across API boundaries creates an intractable problem and makes it
/// impossible to design composable OSSA utilities.
///
/// Strategies for SIL transformation:
///
/// 1. Defer deletion until the end of the pass/sub-pass. This is the simplest
/// strategy. Use it when possible. Use InstructionDeleter::trackIfDead() to
/// record instructions that may be the root of a dead use-def graph after
/// transformation. Use InstructionDeleter::cleanupDeadInstructions().
///
/// 2. For algorithms that delete instructions with side effects or require some
/// instructions and operands to be removed from the instruction list or use
/// list incrementally... Use InstructionDeleter::forceDeleteAndFixLifetimes()
/// This erases instructions even, if they have side-effects, and it leaves OSSA
/// in a valid state. This automatically tracks newly exposed dead code. Use
/// InstructionDeleter::cleanupDeadInstructions() at the end of the pass.
///
/// 3. For algorithms that benefit from incremental dead code elimination
/// because eliminating dead code may expose more optimization incrementally...
/// Use InstructionDeleter::forceDeleteAndFixLifetimes() as in case #2, but
/// simply invoke InstructionDeleter::cleanupDeadInstructions() whenever it is
/// useful to do so.
///
/// Instruction iterator invalidation:
///
/// It is surprisingly difficult for any transform to correctly update
/// instruction iterators during deletion. This is because deleting a single
/// instruction may require deleting other instructions, such as debug info and
/// scope-ending instructions. In OSSA code, deleting an instruction may even
/// cause new instructions to be inserted. This is best handled by acquiring an
/// UpdatingInstructionIterator from the InstructionDeleter. This is usually
/// done via InstructionDeleter::updatingRange(SILBasicBlock *).
/// InstructionDeleter::getIteratorRegistry().makeIterator() offers more
/// control.
///
/// Instruction pointer invalidation:
///
/// For data structures that contain instruction pointers and persist across
/// calls to forceDelete* or cleanupDeadInstructions... There is no need to
/// create an updating iterator. Simply check Instruction::isDeleted() when
/// retrieving a pointer from the data structure.
///
/// Using InstModCallbacks:
///
/// InstructionDeleted is primarily designed to be used with a single 'onDelete'
/// callback, which is invoked consistently just before deleting each
/// instruction. It's usually used to avoid iterator invalidation. The other
/// InstModCallbacks should generally be handled at a higher level, and avoided
/// altogether if possible. The following two are supported for flexibility:
///
/// callbacks.createdNewInst() is invoked incrementally when it fixes lifetimes
/// while deleting a set of instructions, but the SIL may still be invalid
/// relative to the new instruction.
///
/// callbacks.notifyWillBeDeletedFunc() is invoked when a dead instruction is
/// first recognized and was not already passed in by the client. During the
/// callback, the to-be-deleted instruction has valid SIL. It's operands and
/// uses can be inspected and cached. It will be deleted later during
/// cleanupDeadInstructions().
///
/// Note that the forceDelete* APIs only invoke notifyWillBeDeletedFunc() when
/// an operand's definition will become dead after force-deleting the specified
/// instruction. Some clients force-delete related instructions one at a
/// time. It is the client's responsiblity to invoke notifyWillBeDeletedFunc()
/// on those explicitly deleted instructions if needed.
///
//===----------------------------------------------------------------------===//

#ifndef SWIFT_SILOPTIMIZER_UTILS_INSTRUCTIONDELETER_H
#define SWIFT_SILOPTIMIZER_UTILS_INSTRUCTIONDELETER_H

#include "swift/SIL/SILInstruction.h"
#include "swift/SILOptimizer/Utils/InstModCallbacks.h"

namespace swift {

/// A utility for deleting one or more instructions belonging to a function, and
/// cleaning up any dead code resulting from deleting those instructions. Use
/// this utility instead of \p recursivelyDeleteTriviallyDeadInstruction
/// as follows:
///   InstructionDeleter deleter;
///   deleter.deleteIfDead(instruction);
///   deleter.cleanupDeadInstructions();
///
class InstructionDeleter {
  /// A set vector of instructions that are found to be dead. The ordering of
  /// instructions in this set is important as when a dead instruction is
  /// removed, new instructions will be generated to fix the lifetime of the
  /// instruction's operands. This has to be deterministic.
  SmallSetVector<SILInstruction *, 8> deadInstructions;

  /// Callbacks used when adding/deleting instructions.
  InstModCallbacks callbacks;

public:
  InstructionDeleter() : deadInstructions() {}

  InstructionDeleter(InstModCallbacks &&callbacks)
    : deadInstructions(), callbacks(std::move(callbacks)) {}

  InstModCallbacks &getCallbacks() { return callbacks; }

  void setCallbacks(const InstModCallbacks &newCallbacks) {
    callbacks = newCallbacks;
  }

  bool hadCallbackInvocation() const {
    return const_cast<InstructionDeleter *>(this)
        ->getCallbacks()
        .hadCallbackInvocation();
  }

  void resetHadCallbackInvocation() {
    getCallbacks().resetHadCallbackInvocation();
  }

  /// If the instruction \p inst is dead, record it so that it can be cleaned
  /// up.
  ///
  /// Calls callbacks.notifyWillBeDeleted().
  bool trackIfDead(SILInstruction *inst);

  /// Track this instruction as dead even if it has side effects. Used to enable
  /// the deletion of a bunch of instructions at the same time.
  ///
  /// Calls callbacks.notifyWillBeDeleted().
  void forceTrackAsDead(SILInstruction *inst);

  /// If the instruction \p inst is dead, delete it immediately along with its
  /// destroys and scope-ending uses. If any operand definitions will become
  /// dead after deleting this instruction, track them so they can be deleted
  /// later during cleanUpDeadInstructions().
  ///
  /// Calls callbacks.notifyWillBeDeleted().
  bool deleteIfDead(SILInstruction *inst);
  bool deleteIfDead(SILInstruction *inst, bool fixLifetime);

  /// Delete the instruction \p inst, ignoring its side effects. If any operand
  /// definitions will become dead after deleting this instruction, track them
  /// so they can be deleted later during cleanUpDeadInstructions(). This
  /// function will add necessary ownership instructions to fix the lifetimes of
  /// the operands of \p inst to compensate for its deletion.
  ///
  /// \pre the function containing \p inst must be using ownership SIL.
  /// \pre the instruction to be deleted must not have any use other than
  /// incidental uses.
  ///
  /// callbacks.notifyWillBeDeleted will not be called for \p inst but will be
  /// called for any other instructions that become dead as a result.
  void forceDeleteAndFixLifetimes(SILInstruction *inst);

  /// Delete the instruction \p inst and record instructions that may become
  /// dead because of the removal of \c inst. If in ownership SIL, use the
  /// \c forceDeleteAndFixLifetimes function instead, unless under special
  /// circumstances where the client must handle fixing lifetimes of the
  /// operands of the deleted instructions. This function will not fix the
  /// lifetimes of the operands of \c inst once it is deleted. This function
  /// will not clean up dead code resulting from the instruction's removal. To
  /// do so, invoke the method \c cleanupDeadCode of this instance, once the SIL
  /// of the containing function is made consistent.
  ///
  /// \pre the instruction to be deleted must not have any use other than
  /// incidental uses.
  ///
  /// callbacks.notifyWillBeDeleted will not be called for \p inst but will be
  /// called for any other instructions that become dead as a result.
  void forceDelete(SILInstruction *inst);

  /// Recursively delete all of the uses of the instruction before deleting the
  /// instruction itself. Does not fix lifetimes.
  ///
  /// callbacks.notifyWillBeDeleted will not be called for \p inst but will
  /// be called for any other instructions that become dead as a result.
  void forceDeleteWithUsers(SILInstruction *inst) {
    deleteWithUses(inst, /*fixLifetimes*/ false, /*forceDeleteUsers*/ true);
  }

  /// Clean up dead instructions that are tracked by this instance and all
  /// instructions that transitively become dead.
  ///
  /// \pre the function containing dead instructions must be consistent (i.e., no
  /// under or over releases). Note that if \c forceDelete call leaves the
  /// function body in an inconsistent state, it needs to be made consistent
  /// before this method is invoked.
  ///
  /// callbacks.notifyWillBeDeletedFunc will only be called for instructions
  /// that become dead during cleanup but were not already tracked.
  void cleanupDeadInstructions();

  /// Recursively visit users of \p inst and delete instructions that are dead
  /// including \p inst.
  ///
  /// callbacks.notifyWillBeDeletedFunc will be called for any dead
  /// instructions.
  void recursivelyDeleteUsersIfDead(SILInstruction *inst);

  /// Recursively visit users of \p inst and force delete them including \p
  /// inst. Also, destroy the consumed operands of the deleted instructions
  /// whenever necessary.
  ///
  /// callbacks.notifyWillBeDeletedFunc will not be called for \p inst or its
  /// users but will be called for any other instructions that become dead as a
  /// result.
  void recursivelyForceDeleteUsersAndFixLifetimes(SILInstruction *inst);

private:
  void deleteWithUses(SILInstruction *inst, bool fixLifetimes,
                      bool forceDeleteUsers = false);
};

} // namespace swift

#endif // SWIFT_SILOPTIMIZER_UTILS_INSTRUCTIONDELETER_H
