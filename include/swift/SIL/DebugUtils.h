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
#include "swift/SIL/SILBuilder.h"
#include "swift/SIL/SILGlobalVariable.h"
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
template <bool nonDebugInsts> class DebugUseIterator {
public:
  using iterator_category = std::forward_iterator_tag;
  using value_type = Operand*;
  using difference_type = std::ptrdiff_t;
  using pointer = value_type*;
  using reference = value_type&;    

private:
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
/// information about a VarDecl. This allows one to write high level code over
/// the set of such instructions with greater correctness by using exhaustive
/// switches, methods, and keeping it light weight by using *, -> operators to
/// access functionality from the underlying instruction when eneded.
class VarDeclCarryingInst {
public:
  enum class Kind : uint8_t {
    Invalid = 0,
    DebugValue,
    AllocStack,
    AllocBox,
    GlobalAddr,
    RefElementAddr,
  };

protected:
  SILInstruction *inst;
  Kind kind;
  uintptr_t spareBits : (sizeof(uintptr_t) - sizeof(Kind)) * 8;

public:
  VarDeclCarryingInst() : inst(nullptr), kind(Kind::Invalid), spareBits(0) {}
  VarDeclCarryingInst(DebugValueInst *dvi)
      : inst(dvi), kind(Kind::DebugValue), spareBits(0) {}
  VarDeclCarryingInst(AllocStackInst *asi)
      : inst(asi), kind(Kind::AllocStack), spareBits(0) {}
  VarDeclCarryingInst(AllocBoxInst *abi)
      : inst(abi), kind(Kind::AllocBox), spareBits(0) {}
  VarDeclCarryingInst(GlobalAddrInst *gai)
      : inst(gai), kind(Kind::GlobalAddr), spareBits(0) {}
  VarDeclCarryingInst(RefElementAddrInst *reai)
      : inst(reai), kind(Kind::RefElementAddr), spareBits(0) {}
  VarDeclCarryingInst(SILInstruction *newInst)
      : inst(nullptr), kind(Kind::Invalid), spareBits(0) {
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
    case SILInstructionKind::GlobalAddrInst:
      kind = Kind::GlobalAddr;
      break;
    case SILInstructionKind::RefElementAddrInst:
      kind = Kind::RefElementAddr;
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

  unsigned getSpareBits() const { return spareBits; }
  void setSpareBits(unsigned newSpareBits) { spareBits = newSpareBits; }

  bool operator==(const VarDeclCarryingInst &other) const {
    return kind == other.kind && inst == other.inst &&
           spareBits == other.spareBits;
  }

  bool operator!=(const VarDeclCarryingInst &other) const {
    return !(*this == other);
  }

  /// Add support for this struct in `if` statement.
  explicit operator bool() const { return bool(kind); }

  Kind getKind() const { return kind; }

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
    case Kind::GlobalAddr:
      return cast<GlobalAddrInst>(inst)->getReferencedGlobal()->getDecl();
    case Kind::RefElementAddr:
      return cast<RefElementAddrInst>(inst)->getField();
    }
    llvm_unreachable("covered switch");
  }

  /// If \p value is an alloc_stack, alloc_box use that. Otherwise, see if \p
  /// value has a single debug user, return that. Otherwise return the invalid
  /// VarDeclCarryingInst.
  static VarDeclCarryingInst getFromValue(SILValue value);

  StringRef getName() const {
    assert(kind != Kind::Invalid);
    StringRef varName = "unknown";
    if (auto *decl = getDecl()) {
      varName = decl->getBaseName().userFacingName();
    }
    return varName;
  }

  /// Take in \p inst, a potentially invalid VarDeclCarryingInst, and returns a
  /// name for it. If we have an invalid value or don't find var info or a decl,
  /// return "unknown".
  ///
  /// The reason this isn't a method is that in all the other parts of
  /// VarDeclCarryingInst, we use Invalid to signal early error.
  static StringRef getName(VarDeclCarryingInst inst) {
    if (!inst)
      return "unknown";
    return inst.getName();
  }
};

inline VarDeclCarryingInst VarDeclCarryingInst::getFromValue(SILValue value) {
  if (auto *svi = dyn_cast<SingleValueInstruction>(value)) {
    if (auto result = VarDeclCarryingInst(svi)) {
      return result;
    }
  }

  return VarDeclCarryingInst();
}

/// A light weight abstraction on top of an instruction that carries within it
/// information about a debug variable. This allows one to write high level code
/// over the set of such instructions with greater correctness by using
/// exhaustive switches, methods, and keeping it light weight by using *, ->
/// operators to access functionality from the underlying instruction when
/// needed.
struct DebugVarCarryingInst : VarDeclCarryingInst {
#ifdef SET_TO_SUPER_ENUM_KIND
#error "Cannot reuse this macro"
#endif
#define SET_TO_SUPER_ENUM_KIND(X)                                              \
  X = std::underlying_type<VarDeclCarryingInst::Kind>::type(                   \
      VarDeclCarryingInst::Kind::X)
  enum class Kind : uint8_t {
    SET_TO_SUPER_ENUM_KIND(Invalid),
    SET_TO_SUPER_ENUM_KIND(DebugValue),
    SET_TO_SUPER_ENUM_KIND(AllocStack),
    SET_TO_SUPER_ENUM_KIND(AllocBox),
  };
#undef SET_TO_SUPER_ENUM_KIND
  static_assert(
      std::is_same<
          std::underlying_type<VarDeclCarryingInst::Kind>::type,
          std::underlying_type<DebugVarCarryingInst::Kind>::type>::value,
      "DebugVarCarryingInst and VarDeclCarryingInst must have the "
      "same underlying type");

  DebugVarCarryingInst() : VarDeclCarryingInst() {}
  DebugVarCarryingInst(DebugValueInst *dvi) : VarDeclCarryingInst(dvi) {}
  DebugVarCarryingInst(AllocStackInst *asi) : VarDeclCarryingInst(asi) {}
  DebugVarCarryingInst(AllocBoxInst *abi) : VarDeclCarryingInst(abi) {}
  DebugVarCarryingInst(SILInstruction *newInst)
      : VarDeclCarryingInst() {
    switch (newInst->getKind()) {
    default:
      return;
    case SILInstructionKind::DebugValueInst:
      kind = VarDeclCarryingInst::Kind::DebugValue;
      break;
    case SILInstructionKind::AllocStackInst:
      kind = VarDeclCarryingInst::Kind::AllocStack;
      break;
    case SILInstructionKind::AllocBoxInst:
      kind = VarDeclCarryingInst::Kind::AllocBox;
      break;
    }
    inst = newInst;
  }

  Kind getKind() const { return Kind(VarDeclCarryingInst::getKind()); }

  Optional<SILDebugVariable> getVarInfo() const {
    switch (getKind()) {
    case Kind::Invalid:
      llvm_unreachable("Invalid?!");
    case Kind::DebugValue:
      return cast<DebugValueInst>(**this)->getVarInfo();
    case Kind::AllocStack:
      return cast<AllocStackInst>(**this)->getVarInfo();
    case Kind::AllocBox:
      return cast<AllocBoxInst>(**this)->getVarInfo();
    }
    llvm_unreachable("covered switch");
  }

  void setDebugVarScope(const SILDebugScope *NewDS) {
    switch (getKind()) {
    case Kind::Invalid:
      llvm_unreachable("Invalid?!");
    case Kind::DebugValue:
      cast<DebugValueInst>(**this)->setDebugVarScope(NewDS);
      break;
    case Kind::AllocStack:
      cast<AllocStackInst>(**this)->setDebugVarScope(NewDS);
      break;
    case Kind::AllocBox:
      llvm_unreachable("Not implemented");
    }
  }

  void markAsMoved() {
    switch (getKind()) {
    case Kind::Invalid:
      llvm_unreachable("Invalid?!");
    case Kind::DebugValue:
      cast<DebugValueInst>(**this)->setUsesMoveableValueDebugInfo();
      break;
    case Kind::AllocStack:
      cast<AllocStackInst>(**this)->markUsesMoveableValueDebugInfo();
      break;
    case Kind::AllocBox:
      cast<AllocBoxInst>(**this)->setUsesMoveableValueDebugInfo();
      break;
    }
  }

  /// Returns true if this DebugVarCarryingInst was moved.
  bool getWasMoved() const {
    switch (getKind()) {
    case Kind::Invalid:
      llvm_unreachable("Invalid?!");
    case Kind::DebugValue:
      return cast<DebugValueInst>(**this)->getUsesMoveableValueDebugInfo();
    case Kind::AllocStack:
      return cast<AllocStackInst>(**this)->getUsesMoveableValueDebugInfo();
    case Kind::AllocBox:
      return cast<AllocBoxInst>(**this)->getUsesMoveableValueDebugInfo();
    }
  }

  /// If we are attempting to create a "debug_value" clone of this debug var
  /// carrying inst, return the appropriate SILValue to use as the operand of
  /// that debug value.
  ///
  /// For a debug_value, we just return the actual operand, otherwise we return
  /// the pointer address.
  SILValue getOperandForDebugValueClone() const {
    switch (getKind()) {
    case Kind::Invalid:
      llvm_unreachable("Invalid?!");
    case Kind::DebugValue:
      return cast<DebugValueInst>(**this)->getOperand();
    case Kind::AllocStack:
      return cast<AllocStackInst>(**this);
    case Kind::AllocBox:
      return cast<AllocBoxInst>(**this);
    }
  }

  /// If \p value is an alloc_stack, alloc_box use that. Otherwise, see if \p
  /// value has a single debug user, return that. Otherwise return the invalid
  /// DebugVarCarryingInst.
  static DebugVarCarryingInst getFromValue(SILValue value);

  StringRef getName() const {
    assert(getKind() != Kind::Invalid);
    StringRef varName = "unknown";
    if (auto varInfo = getVarInfo()) {
      varName = varInfo->Name;
    } else if (auto *decl = getDecl()) {
      varName = decl->getBaseName().userFacingName();
    }
    return varName;
  }

  /// Take in \p inst, a potentially invalid DebugVarCarryingInst, and returns a
  /// name for it. If we have an invalid value or don't find var info or a decl,
  /// return "unknown".
  ///
  /// The reason this isn't a method is that in all the other parts of
  /// DebugVarCarryingInst, we use Invalid to signal early error.
  static StringRef getName(DebugVarCarryingInst inst) {
    if (!inst)
      return "unknown";
    return inst.getName();
  }
};

static_assert(sizeof(DebugVarCarryingInst) == sizeof(VarDeclCarryingInst) &&
                  alignof(DebugVarCarryingInst) == alignof(VarDeclCarryingInst),
              "Expected debug var carrying inst to have the same "
              "size/alignment/layout as VarDeclCarryingInst!");

/// Attempt to discover a StringRef varName for the value \p value based only
/// off of debug var information. If we fail, we return the name "unknown".
inline StringRef getDebugVarName(SILValue value) {
  auto inst = DebugVarCarryingInst::getFromValue(value);
  return DebugVarCarryingInst::getName(inst);
}

inline StringRef getDiagnosticName(SILValue value) {
  if (auto inst = DebugVarCarryingInst::getFromValue(value))
    return inst.getName();
  if (auto inst = VarDeclCarryingInst::getFromValue(value))
    return inst.getName();
  return "unknown";
}

} // end namespace swift

#endif // SWIFT_SIL_DEBUGUTILS_H
