//===--- SILInstructionWorklist.h -------------------------------*- C++ -*-===//
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
///
/// \file
///
/// When visiting the instructions in a function/basic block, one often modifies
/// the list of instructions to be visited.  That fact introduces complexity in
/// the form of ensuring that no effort is wasted moving items down in the list
/// when an item is removed from it and also in the form of ensuring that no
/// instructions which have been modified/deleted are visited.
///
/// The SILInstructionWorklist manages that complexity.  It is responsible for
/// ensuring that removing an instruction is not unnecessarily expensive and
/// that only valid instructions are removed from the list.
///
/// Additionally, SILInstructionWorklist provides conveniences for simple
/// instruction modifications and ensuring that the appropriate instructions
/// will be visited accordingly.  For example, if provides a method for 
/// replacing an operation which has already been removed with a new instruction
/// determined by a SILInstructionVisitor.
///
//===----------------------------------------------------------------------===//

#ifndef SWIFT_SIL_SILINSTRUCTIONWORKLIST_H
#define SWIFT_SIL_SILINSTRUCTIONWORKLIST_H

#include "swift/Basic/BlotSetVector.h"
#include "swift/SIL/SILInstruction.h"
#include "swift/SIL/InstructionUtils.h"
#include "swift/SIL/SILValue.h"
#include "swift/SILOptimizer/Utils/DebugOptUtils.h"
#include "swift/SILOptimizer/Utils/InstOptUtils.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallVector.h"

namespace swift {

class SILInstructionWorklistBase {
  const char *loggingName;

protected:
  SILInstructionWorklistBase(const char *loggingName)
      : loggingName(loggingName){};
  // Invokes the provided function with the debug stream and the previously
  // specified logging name.
  //
  // Note: Because it contains LLVM_DEBUG, the definition is in the .cpp file.
  //       Consequently, we are relying on LTO to ensure that the calls are
  //       inlined and removed in non-assert builds.
  void withDebugStream(
      std::function<void(llvm::raw_ostream &stream, const char *loggingName)>
          perform);
};

/// Manages a list of instructions awaiting visitation.
template <typename VectorT = std::vector<SILInstruction *>,
          typename MapT = llvm::DenseMap<SILInstruction *, unsigned>>
class SILInstructionWorklist : SILInstructionWorklistBase {
  BlotSetVector<SILInstruction *, VectorT, MapT> worklist;

  void operator=(const SILInstructionWorklist &rhs) = delete;
  SILInstructionWorklist(const SILInstructionWorklist &worklist) = delete;

public:
  SILInstructionWorklist(const char *loggingName = "InstructionWorklist")
      : SILInstructionWorklistBase(loggingName) {}

  /// Returns true if the worklist is empty.
  bool isEmpty() const { return worklist.empty(); }

  /// Returns the number of elements in the worklist.
  unsigned size() const { return worklist.size(); }

  /// Add the specified instruction to the worklist if it isn't already in it.
  void add(SILInstruction *instruction) {
    if (worklist.insert(instruction).second) {
      withDebugStream([&](llvm::raw_ostream &stream, StringRef loggingName) {
        stream << loggingName << ": ADD: " << *instruction << '\n';
      });
    }
  }

  /// If the given SILValue is a SILInstruction add it to the worklist.
  void addValue(SILValue value) {
    if (auto *instruction = value->getDefiningInstruction()) {
      add(instruction);
    }
  }

  /// Add the given list of instructions in reverse order to the worklist. This
  /// routine assumes that the worklist is empty and the given list has no
  /// duplicates.
  void addInitialGroup(ArrayRef<SILInstruction *> list) {
    assert(worklist.empty() && "worklist must be empty to add initial group");
    worklist.reserve(list.size() + 16);
    withDebugStream([&](llvm::raw_ostream &stream, StringRef loggingName) {
      stream << loggingName << ": ADDING: " << list.size()
             << " instrs to worklist\n";
    });
    while (!list.empty()) {
      SILInstruction *instruction = list.back();
      list = list.drop_back();
      worklist.insert(instruction);
    }
  }

  // If instruction is in the worklist, remove it.
  void erase(SILInstruction *instruction) { worklist.erase(instruction); }

  /// Remove the top element from the worklist.
  SILInstruction *pop_back_val() {
    return worklist.pop_back_val().value_or(nullptr);
  }

  /// When an instruction has been simplified, add all of its users to the
  /// worklist, since additional simplifications of its users may have been
  /// exposed.
  void addUsersToWorklist(SILValue value) {
    for (auto *use : value->getUses()) {
      add(use->getUser());
    }
  }

  /// Add operands of \p instruction to the worklist. Meant to be used once it
  /// is certain that \p instruction will be deleted but may have operands that
  /// are still alive. With fewer uses, the operand definition may be
  /// optimizable.
  ///
  /// \p instruction may still have uses because this is called before
  /// InstructionDeleter begins deleting it and some instructions are deleted at
  /// the same time as their uses.
  void addOperandsToWorklist(SILInstruction &instruction) {
    // Make sure that we reprocess all operands now that we reduced their
    // use counts.
    if (instruction.getNumOperands() < 8) {
      for (auto &operand : instruction.getAllOperands()) {
        if (auto *operandInstruction =
                operand.get()->getDefiningInstruction()) {
          withDebugStream([&](llvm::raw_ostream &stream,
                              StringRef loggingName) {
            stream << loggingName << ": add op " << *operandInstruction << '\n'
                   << " from erased inst to worklist\n";
          });
          add(operandInstruction);
        }
      }
    }
  }

  /// When an instruction has been simplified, add all of its users to the
  /// worklist, since additional simplifications of its users may have been
  /// exposed.
  void addUsersOfAllResultsToWorklist(SILInstruction *instruction) {
    for (auto result : instruction->getResults()) {
      addUsersToWorklist(&*result);
    }
  }

  /// Check that the worklist is empty and nuke the backing store if it is
  /// large.
  void resetChecked() {
    assert(worklist.empty() && "Vector empty, but the map is not?");

    // Do an explicit clear, shrinking the storage if needed.
    worklist.clear();
  }

  /// Find usages of \p instruction and replace them with usages of \p result.
  ///
  /// Intended to be called during visitation after \p instruction has been
  /// removed from the worklist.
  ///
  /// \p instruction the instruction whose usages will be replaced. This
  /// instruction may already be deleted to maintain valid SIL. For example, if
  /// it was a block terminator.
  ///
  /// \p result the instruction whose usages will replace \p instruction
  ///
  /// \return whether the instruction was deleted or modified.
  bool replaceInstructionWithInstruction(SILInstruction *instruction,
                                         SILInstruction *result
#ifndef NDEBUG
                                         ,
                                         std::string instructionDescription
#endif
  ) {
    if (result == instruction) {
#ifndef NDEBUG
      withDebugStream([&](llvm::raw_ostream &stream, StringRef loggingName) {
        stream << loggingName << ": Mod = " << instructionDescription << '\n'
               << "  "
               << "  New = " << *instruction << '\n';
      });
#endif

      // If the instruction was modified, it's possible that it is now dead.
      // if so, remove it.
      if (isInstructionTriviallyDead(instruction)) {
        eraseInstFromFunction(*instruction);
      } else {
        add(instruction);
        addUsersOfAllResultsToWorklist(instruction);
      }
      return false;
    }

    assert(&*std::prev(instruction->getIterator()) == result &&
           "Expected new instruction inserted before existing instruction!");

    withDebugStream([&](llvm::raw_ostream &stream, StringRef loggingName) {
      stream << loggingName << ": Old = " << *instruction << '\n'
             << "  "
             << "  New = " << *result << '\n';
    });

    // Everything uses the new instruction now.
    replaceInstUsesPairwiseWith(instruction, result);

    // Push the new instruction and any users onto the worklist.
    add(result);
    addUsersOfAllResultsToWorklist(result);

    eraseInstFromFunction(*instruction);

    return true;
  }

  // Insert the instruction newInstruction before instruction old in old's
  // parent block. Add newInstruction to the worklist.
  SILInstruction *insertNewInstBefore(SILInstruction *newInstruction,
                                      SILInstruction &old) {
    assert(newInstruction && newInstruction->getParent() == nullptr &&
           "newInstruction instruction already inserted into a basic block!");
    SILBasicBlock *block = old.getParent();
    block->insert(&old, newInstruction); // Insert inst
    add(newInstruction);
    return newInstruction;
  }

  // This method is to be used when an instruction is found to be dead or
  // replaceable with another preexisting expression. Here we add all uses of
  // instruction to the worklist, and replace all uses of instruction with the
  // new value.
  void replaceInstUsesWith(SingleValueInstruction &instruction,
                           ValueBase *value) {
    addUsersToWorklist(&instruction); // Add all modified instrs to worklist.

    withDebugStream([&](llvm::raw_ostream &stream, StringRef loggingName) {
      stream << loggingName << ": Replacing " << instruction << '\n'
             << "  "
             << "  with " << *value << '\n';
    });

    instruction.replaceAllUsesWith(value);
  }

  // This method is to be used when a value is found to be dead,
  // replaceable with another preexisting expression. Here we add all
  // uses of oldValue to the worklist, replace all uses of oldValue
  // with newValue.
  void replaceValueUsesWith(SILValue oldValue, SILValue newValue) {
    addUsersToWorklist(oldValue); // Add all modified instrs to worklist.

    withDebugStream([&](llvm::raw_ostream &stream, StringRef loggingName) {
      stream << loggingName << ": Replacing " << oldValue << '\n'
             << "  "
             << "  with " << newValue << '\n';
    });

    oldValue->replaceAllUsesWith(newValue);
  }

  void replaceInstUsesPairwiseWith(SILInstruction *oldI, SILInstruction *newI) {
    withDebugStream([&](llvm::raw_ostream &stream, StringRef loggingName) {
      stream << loggingName << ": Replacing " << *oldI << '\n'
             << "  "
             << "  with " << *newI << '\n';
    });

    auto oldResults = oldI->getResults();
    auto newResults = newI->getResults();
    assert(oldResults.size() == newResults.size());
    for (auto i : indices(oldResults)) {
      // Add all modified instrs to worklist.
      addUsersToWorklist(oldResults[i]);

      oldResults[i]->replaceAllUsesWith(newResults[i]);
    }
  }

  // Some instructions can never be "trivially dead" due to side effects or
  // producing a void value. In those cases, visit methods should use this
  // method to delete the given instruction.
  void eraseInstFromFunction(SILInstruction &instruction,
                             SILBasicBlock::iterator &iterator,
                             bool addOperandsToWorklist = true) {
    // Try to salvage debug info first.
    swift::salvageDebugInfo(&instruction);
    // Then delete old debug users.
    for (auto result : instruction.getResults()) {
      while (!result->use_empty()) {
        auto *user = result->use_begin()->getUser();
        assert(user->isDebugInstruction());
        if (iterator == user->getIterator())
          ++iterator;
        erase(user);
        user->eraseFromParent();
      }
    }
    if (iterator == instruction.getIterator())
      ++iterator;

    eraseSingleInstFromFunction(instruction, addOperandsToWorklist);
  }

  void eraseInstFromFunction(SILInstruction &instruction,
                             bool addOperandsToWorklist = true) {
    SILBasicBlock::iterator nullIter;
    return eraseInstFromFunction(instruction, nullIter, addOperandsToWorklist);
  }

  void eraseSingleInstFromFunction(SILInstruction &instruction,
                                   bool shouldAddOperandsToWorklist) {
    withDebugStream([&](llvm::raw_ostream &stream, StringRef loggingName) {
      stream << loggingName << ": ERASE " << instruction << '\n';
    });

    // If we are asked to add operands to the worklist, do so now.
    if (shouldAddOperandsToWorklist)
      addOperandsToWorklist(instruction);

    erase(&instruction);
    instruction.eraseFromParent();
  }
};

// TODO: This name is somewhat unpleasant.  Once the type is templated over its
//       storage and renamed BlottableWorklist, this type will be
//       SmallBlottableWorklist which is more reasonable.
template <unsigned N>
class SmallSILInstructionWorklist final
    : public SILInstructionWorklist<
          llvm::SmallVector<llvm::Optional<SILInstruction *>, N>,
          // TODO: A DenseMap rather than a SmallDenseMap is used here to avoid
          //       running into an upstream problem with the handling of grow()
          //       when it results in rehashing and tombstone removal:
          //
          //       https://reviews.llvm.org/D56455
          llvm::DenseMap<SILInstruction *, unsigned>> {
public:
  using VectorT = llvm::SmallVector<llvm::Optional<SILInstruction *>, N>;
  using MapT = llvm::DenseMap<SILInstruction *, unsigned>;
  SmallSILInstructionWorklist(const char *loggingName = "InstructionWorklist")
      : SILInstructionWorklist<VectorT, MapT>(loggingName) {}
};

} // end namespace swift

#endif // SWIFT_SIL_SILINSTRUCTIONWORKLIST_H
