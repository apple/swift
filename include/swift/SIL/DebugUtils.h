//===--- DebugUtils.h - Utilities for debug-info instructions ---*- C++ -*-===//
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
//
// This file contains utilities to work with debug-info related instructions:
// debug_value, alloc_stack, and alloc_box.
//
// SIL optimizations should deal with debug-info related instructions when
// looking at the uses of a value.
// When performing an analysis, the usual thing is to just ignore all debug-info
// instructions.
// When transforming the SIL, a pass must decide what to do with debug-info
// instructions. Either delete them (if their value is no longer available),
// keep them (if the transformation has no effect on debug-info values) or
// update them.
//
// To ignore debug-info instructions during an analysis, this file provides
// some utility functions, which can be used instead of the relevant member
// functions in ValueBase and SILValue:
//
// V->use_empty()        ->  onlyHaveDebugUses(V)
// V.hasOneUse()         ->  hasOneNonDebugUse(V)
// V.getUses()           ->  getNonDebugUses(V)
// I->eraseFromParent()  ->  eraseFromParentWithDebugInsts(I)
//
//===----------------------------------------------------------------------===//

#ifndef SWIFT_SIL_DEBUGUTILS_H
#define SWIFT_SIL_DEBUGUTILS_H

#include "swift/SIL/SILBasicBlock.h"
#include "swift/SIL/SILInstruction.h"

namespace swift {

class SILInstruction;

/// Deletes all of the debug instructions that use \p value.
inline void deleteAllDebugUses(SILValue value) {
  for (auto ui = value->use_begin(), ue = value->use_end(); ui != ue;) {
    auto *inst = ui->getUser();
    ++ui;
    if (inst->isDebugInstruction()) {
      inst->eraseFromParent();
    }
  }
}

/// Deletes all of the debug uses of any result of \p inst.
inline void deleteAllDebugUses(SILInstruction *inst) {
  for (SILValue v : inst->getResults()) {
    deleteAllDebugUses(v);
  }
}

/// This iterator filters out any debug (or non-debug) instructions from a range
/// of uses, provided by the underlying ValueBaseUseIterator.
/// If \p nonDebugInsts is true, then the iterator provides a view to all non-
/// debug instructions. Otherwise it provides a view ot all debug-instructions.
template <bool nonDebugInsts> class DebugUseIterator
: public std::iterator<std::forward_iterator_tag, Operand *, ptrdiff_t> {
  
  ValueBaseUseIterator BaseIterator;
  
  // Skip any debug or non-debug instructions (depending on the nonDebugInsts
  // template argument).
  void skipInsts() {
    while (true) {
      if (*BaseIterator == nullptr)
        return;
      
      SILInstruction *User = BaseIterator->getUser();
      if (User->isDebugInstruction() != nonDebugInsts)
        return;
      
      BaseIterator++;
    }
  }
  
public:
  
  DebugUseIterator(ValueBaseUseIterator BaseIterator) :
      BaseIterator(BaseIterator) {
    skipInsts();
  }
  
  DebugUseIterator() = default;
  
  Operand *operator*() const { return *BaseIterator; }
  Operand *operator->() const { return *BaseIterator; }
  SILInstruction *getUser() const { return BaseIterator.getUser(); }
  
  DebugUseIterator &operator++() {
    BaseIterator++;
    skipInsts();
    return *this;
  }
  
  DebugUseIterator operator++(int unused) {
    DebugUseIterator Copy = *this;
    ++*this;
    return Copy;
  }
  friend bool operator==(DebugUseIterator lhs,
                         DebugUseIterator rhs) {
    return lhs.BaseIterator == rhs.BaseIterator;
  }
  friend bool operator!=(DebugUseIterator lhs,
                         DebugUseIterator rhs) {
    return !(lhs == rhs);
  }
};

/// Iterator for iteration over debug instructions.
using DUIterator = DebugUseIterator<false>;

/// Iterator for iteration over non-debug instructions.
using NonDUIterator = DebugUseIterator<true>;


/// Returns a range of all debug instructions in the uses of a value (e.g.
/// SILValue or SILInstruction).
inline iterator_range<DUIterator> getDebugUses(SILValue V) {
  return make_range(DUIterator(V->use_begin()), DUIterator(V->use_end()));
}

/// Returns a range of all non-debug instructions in the uses of a value (e.g.
/// SILValue or SILInstruction).
inline iterator_range<NonDUIterator> getNonDebugUses(SILValue V) {
  return make_range(NonDUIterator(V->use_begin()), NonDUIterator(V->use_end()));
}

/// Returns true if a value (e.g. SILInstruction) has no uses except debug
/// instructions.
inline bool onlyHaveDebugUses(SILValue V) {
  auto NonDebugUses = getNonDebugUses(V);
  return NonDebugUses.begin() == NonDebugUses.end();
}

/// Return true if all of the results of the given instruction have no uses
/// except debug instructions.
inline bool onlyHaveDebugUsesOfAllResults(SILInstruction *I) {
  for (auto result : I->getResults()) {
    if (!onlyHaveDebugUses(result))
      return false;
  }
  return true;
}

/// Returns true if a value (e.g. SILInstruction) has exactly one use which is
/// not a debug instruction.
inline bool hasOneNonDebugUse(SILValue V) {
  auto Range = getNonDebugUses(V);
  auto I = Range.begin(), E = Range.end();
  if (I == E) return false;
  return ++I == E;
}

// Returns the use if the value has only one non debug user.
inline SILInstruction *getSingleNonDebugUser(SILValue V) {
  auto Range = getNonDebugUses(V);
  auto I = Range.begin(), E = Range.end();
  if (I == E) return nullptr;
  if (std::next(I) != E)
    return nullptr;
  return I->getUser();
}

/// If \p value has a single debug user, return the operand associated with that
/// use. Otherwise, returns nullptr.
inline Operand *getSingleDebugUse(SILValue value) {
  auto range = getDebugUses(value);
  auto ii = range.begin(), ie = range.end();
  if (ii == ie)
    return nullptr;
  if (std::next(ii) != ie)
    return nullptr;
  return *ii;
}

/// Erases the instruction \p I from it's parent block and deletes it, including
/// all debug instructions which use \p I.
/// Precondition: The instruction may only have debug instructions as uses.
/// If the iterator \p InstIter references any deleted instruction, it is
/// incremented.
///
/// \p callBack will be invoked before each instruction is deleted. \p callBack
/// is not responsible for deleting the instruction because this utility
/// unconditionally deletes the \p I and its debug users.
///
/// Returns an iterator to the next non-deleted instruction after \p I.
inline SILBasicBlock::iterator eraseFromParentWithDebugInsts(
    SILInstruction *I, llvm::function_ref<void(SILInstruction *)> callBack =
                           [](SILInstruction *) {}) {

  auto nextII = std::next(I->getIterator());

  auto results = I->getResults();

  bool foundAny;
  do {
    foundAny = false;
    for (auto result : results) {
      while (!result->use_empty()) {
        foundAny = true;
        auto *User = result->use_begin()->getUser();
        assert(User->isDebugInstruction());
        if (nextII == User->getIterator())
          nextII++;
        callBack(User);
        User->eraseFromParent();
      }
    }
  } while (foundAny);

  I->eraseFromParent();
  return nextII;
}

/// Return true if the def-use graph rooted at \p V contains any non-debug,
/// non-trivial users.
bool hasNonTrivialNonDebugTransitiveUsers(
    PointerUnion<SILInstruction *, SILArgument *> V);

/// A light weight abstraction on top of an instruction that carries within it
/// information about a debug variable. This allows one to write high level code
/// over the set of such instructions with greater correctness by using
/// exhaustive switches, methods, and keeping it light weight by using *, ->
/// operators to access functionality from the underlying instruction when
/// needed.
struct DebugVarCarryingInst {
  enum class Kind {
    Invalid = 0,
    DebugValue,
    AllocStack,
    AllocBox,
  };

  Kind kind;
  SILInstruction *inst;

  DebugVarCarryingInst() : kind(Kind::Invalid), inst(nullptr) {}
  DebugVarCarryingInst(DebugValueInst *dvi)
      : kind(Kind::DebugValue), inst(dvi) {}
  DebugVarCarryingInst(AllocStackInst *asi)
      : kind(Kind::AllocStack), inst(asi) {}
  DebugVarCarryingInst(AllocBoxInst *abi) : kind(Kind::AllocBox), inst(abi) {}
  DebugVarCarryingInst(SILInstruction *newInst)
      : kind(Kind::Invalid), inst(nullptr) {
    switch (newInst->getKind()) {
    default:
      return;
    case SILInstructionKind::DebugValueInst:
      kind = Kind::DebugValue;
      break;
    case SILInstructionKind::AllocStackInst:
      kind = Kind::AllocStack;
      break;
    case SILInstructionKind::AllocBoxInst:
      kind = Kind::AllocBox;
      break;
    }
    inst = newInst;
  }

  /// Enable the composition struct to be used as an instruction easily. We use
  /// a '*' so that in the source it is easily visible to the eye that something
  /// is happening here.
  SILInstruction *operator*() const { return inst; }

  /// Enable one to access the methods of the wrapped instruction using
  /// '->'. This keeps the wrapper light weight.
  SILInstruction *operator->() const { return inst; }

  /// Add support for this struct in `if` statement.
  explicit operator bool() const { return bool(kind); }

  VarDecl *getDecl() const {
    switch (kind) {
    case Kind::Invalid:
      llvm_unreachable("Invalid?!");
    case Kind::DebugValue:
      return cast<DebugValueInst>(inst)->getDecl();
    case Kind::AllocStack:
      return cast<AllocStackInst>(inst)->getDecl();
    case Kind::AllocBox:
      return cast<AllocBoxInst>(inst)->getDecl();
    }
    llvm_unreachable("covered switch");
  }

  Optional<SILDebugVariable> getVarInfo() const {
    switch (kind) {
    case Kind::Invalid:
      llvm_unreachable("Invalid?!");
    case Kind::DebugValue:
      return cast<DebugValueInst>(inst)->getVarInfo();
    case Kind::AllocStack:
      return cast<AllocStackInst>(inst)->getVarInfo();
    case Kind::AllocBox:
      return cast<AllocBoxInst>(inst)->getVarInfo();
    }
    llvm_unreachable("covered switch");
  }

  void setDebugVarScope(const SILDebugScope *NewDS) {
    switch (kind) {
    case Kind::Invalid:
      llvm_unreachable("Invalid?!");
    case Kind::DebugValue:
      cast<DebugValueInst>(inst)->setDebugVarScope(NewDS);
      break;
    case Kind::AllocStack:
      cast<AllocStackInst>(inst)->setDebugVarScope(NewDS);
      break;
    case Kind::AllocBox:
      llvm_unreachable("Not implemented");
    }
  }
};

} // end namespace swift

#endif // SWIFT_SIL_DEBUGUTILS_H
