//===--- SILBasicBlock.h - Basic blocks for SIL -----------------*- C++ -*-===//
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
// This file defines the high-level BasicBlocks used for Swift SIL code.
//
//===----------------------------------------------------------------------===//

#ifndef SWIFT_SIL_BASICBLOCK_H
#define SWIFT_SIL_BASICBLOCK_H

#include "swift/Basic/Compiler.h"
#include "swift/Basic/Range.h"
#include "swift/Basic/SwiftObjectHeader.h"
#include "swift/SIL/SILArgumentArrayRef.h"
#include "swift/SIL/SILInstruction.h"
#include "swift/SIL/SILArgument.h"
#include "llvm/ADT/TinyPtrVector.h"

namespace swift {

class SILFunction;
class SILArgument;
class SILPrintContext;

/// Instruction iterator which allows to "delete" instructions while iterating
/// over the instruction list.
///
/// Iteration with this iterator allows to delete the current, the next or any
/// instruction in the list while iterating.
/// This works because instruction deletion is deferred (for details see
/// `SILModule::scheduledForDeletion`) and removing an instruction from the list
/// keeps the prev/next pointers (see `SILInstructionListBase`).
template <typename IteratorBase>
class DeletableInstructionsIterator {
  using Self = DeletableInstructionsIterator<IteratorBase>;

  IteratorBase base;
  IteratorBase end;

public:
  using value_type = typename IteratorBase::value_type;
  using difference_type = ptrdiff_t;
  using pointer = value_type *;
  using iterator_category = std::forward_iterator_tag;

  DeletableInstructionsIterator(IteratorBase base, IteratorBase end)
  : base(base), end(end) {}

  value_type &operator*() const { return *base; }

  SILInstruction *operator->() const { return base.operator->(); }

  Self &operator++() {
    // If the current instruction is "deleted" (which means: removed from the
    // list), it's prev/next pointers still point to the next instruction which
    // is still in the list - or "deleted", too.
    ++base;
    // Skip over all deleted instructions. Eventually we reach an instruction
    // is still in the list (= not "deleted") or the end iterator.
    while (base != end && base->isDeleted()) {
      ++base;
    }
    return *this;
  }

  bool operator==(const Self &rhs) const { return base == rhs.base; }
  bool operator!=(const Self &rhs) const { return !(*this == rhs); }
};

class SILBasicBlock :
public llvm::ilist_node<SILBasicBlock>, public SILAllocated<SILBasicBlock>,
public SwiftObjectHeader {
  friend class SILSuccessor;
  friend class SILFunction;
  friend class SILGlobalVariable;
  template <typename, unsigned> friend class BasicBlockData;
  template <class, class> friend class SILBitfield;

  static SwiftMetatype registeredMetatype;
  
  using CustomBitsType = uint32_t;
  
public:
  using InstListType = llvm::iplist<SILInstruction>;
private:
  /// A backreference to the containing SILFunction.
  SILFunction *Parent;

  /// PredList - This is a list of all of the terminator operands that are
  /// branching to this block, forming the predecessor list.  This is
  /// automatically managed by the SILSuccessor class.
  SILSuccessor *PredList = nullptr;

  /// This is the list of basic block arguments for this block.
  /// A TinyPtrVector is the right choice, because ~98% of blocks have 0 or 1
  /// arguments.
  TinyPtrVector<SILArgument *> ArgumentList;

  /// The ordered set of instructions in the SILBasicBlock.
  InstListType InstList;

  /// Used by BasicBlockData to index the Data vector.
  ///
  /// A value of -1 means that the index is not initialized yet.
  int index = -1;

  /// Custom bits managed by BasicBlockBitfield.
  CustomBitsType customBits = 0;
  
  /// The BasicBlockBitfield ID of the last initialized bitfield in customBits.
  /// Example:
  ///
  ///                   Last initialized field:
  ///           lastInitializedBitfieldID == C.bitfieldID
  ///                              |
  ///                              V
  /// customBits:  <unused> EE DDD C BB AAA
  ///              31         ...         0
  ///
  /// -> AAA, BB and C are initialized,
  ///    DD and EEE are uninitialized
  ///
  /// See also: SILBitfield::bitfieldID, SILFunction::currentBitfieldID.
  int64_t lastInitializedBitfieldID = 0;

  // Used by `BasicBlockBitfield`.
  unsigned getCustomBits() const { return customBits; }
  // Used by `BasicBlockBitfield`.
  void setCustomBits(unsigned value) { customBits = value; }

  // Used by `BasicBlockBitfield`.
  enum { numCustomBits = std::numeric_limits<CustomBitsType>::digits };

  friend struct llvm::ilist_traits<SILBasicBlock>;

  SILBasicBlock();
  SILBasicBlock(SILFunction *parent);

  void operator=(const SILBasicBlock &) = delete;
  void operator delete(void *Ptr, size_t) = delete;

public:
  static void registerBridgedMetatype(SwiftMetatype metatype) {
    registeredMetatype = metatype;
  }

  ~SILBasicBlock();

  bool isMarkedAsDeleted() const { return lastInitializedBitfieldID < 0; }

  /// Gets the ID (= index in the function's block list) of the block.
  ///
  /// Returns -1 if the block is not contained in a function.
  /// Warning: This function is slow. Therefore it should only be used for
  ///          debug output.
  int getDebugID() const;

  void setDebugName(llvm::StringRef name);
  Optional<llvm::StringRef> getDebugName() const;

  SILFunction *getParent() { return Parent; }
  SILFunction *getFunction() { return getParent(); }
  const SILFunction *getParent() const { return Parent; }

  SILModule &getModule() const;

  /// This method unlinks 'self' from the containing SILFunction and deletes it.
  void eraseFromParent();

  /// Remove all instructions of a SILGlobalVariable's static initializer block.
  void clearStaticInitializerBlock(SILModule &module);

  //===--------------------------------------------------------------------===//
  // SILInstruction List Inspection and Manipulation
  //===--------------------------------------------------------------------===//

  using iterator = InstListType::iterator;
  using const_iterator = InstListType::const_iterator;
  using reverse_iterator = InstListType::reverse_iterator;
  using const_reverse_iterator = InstListType::const_reverse_iterator;

  void insert(iterator InsertPt, SILInstruction *I);
  void insert(SILInstruction *InsertPt, SILInstruction *I) {
    insert(InsertPt->getIterator(), I);
  }

  void push_back(SILInstruction *I);
  void push_front(SILInstruction *I);
  void erase(SILInstruction *I);
  void erase(SILInstruction *I, SILModule &module);
  static void moveInstruction(SILInstruction *inst, SILInstruction *beforeInst);
  void moveInstructionToFront(SILInstruction *inst);

  void eraseAllInstructions(SILModule &module);

  SILInstruction &back() { return InstList.back(); }
  const SILInstruction &back() const {
    return const_cast<SILBasicBlock *>(this)->back();
  }

  SILInstruction &front() { return InstList.front(); }
  const SILInstruction &front() const {
    return const_cast<SILBasicBlock *>(this)->front();
  }

  /// Transfer the instructions from Other to the end of this block.
  void spliceAtEnd(SILBasicBlock *Other) {
    InstList.splice(end(), Other->InstList);
  }

  void spliceAtBegin(SILBasicBlock *Other) {
    InstList.splice(begin(), Other->InstList);
  }

  bool empty() const { return InstList.empty(); }
  iterator begin() { return InstList.begin(); }
  iterator end() { return InstList.end(); }
  const_iterator begin() const { return InstList.begin(); }
  const_iterator end() const { return InstList.end(); }
  reverse_iterator rbegin() { return InstList.rbegin(); }
  reverse_iterator rend() { return InstList.rend(); }
  const_reverse_iterator rbegin() const { return InstList.rbegin(); }
  const_reverse_iterator rend() const { return InstList.rend(); }

  /// Allows deleting instructions while iterating over all instructions of the
  /// block.
  ///
  /// For details see `DeletableInstructionsIterator`.
  llvm::iterator_range<DeletableInstructionsIterator<iterator>>
  deletableInstructions() { return {{begin(), end()}, {end(), end()}}; }

  /// Allows deleting instructions while iterating over all instructions of the
  /// block in reverse order.
  ///
  /// For details see `DeletableInstructionsIterator`.
  llvm::iterator_range<DeletableInstructionsIterator<reverse_iterator>>
  reverseDeletableInstructions() { return {{rbegin(), rend()}, {rend(), rend()}}; }

  TermInst *getTerminator() {
    assert(!InstList.empty() && "Can't get successors for malformed block");
    return cast<TermInst>(&InstList.back());
  }

  const TermInst *getTerminator() const {
    return const_cast<SILBasicBlock *>(this)->getTerminator();
  }

  /// Splits a basic block into two at the specified instruction.
  ///
  /// Note that all the instructions BEFORE the specified iterator
  /// stay as part of the original basic block. The old basic block is left
  /// without a terminator.
  SILBasicBlock *split(iterator I);

  /// Moves the instruction to the iterator in this basic block.
  void moveTo(SILBasicBlock::iterator To, SILInstruction *I);

  //===--------------------------------------------------------------------===//
  // SILBasicBlock Argument List Inspection and Manipulation
  //===--------------------------------------------------------------------===//

  using arg_iterator = TinyPtrVector<SILArgument *>::iterator;
  using const_arg_iterator = TinyPtrVector<SILArgument *>::const_iterator;

  bool args_empty() const { return ArgumentList.empty(); }
  size_t args_size() const { return ArgumentList.size(); }
  arg_iterator args_begin() { return ArgumentList.begin(); }
  arg_iterator args_end() { return ArgumentList.end(); }
  const_arg_iterator args_begin() const { return ArgumentList.begin(); }
  const_arg_iterator args_end() const { return ArgumentList.end(); }

  /// Iterator over the PHI arguments of a basic block.
  /// Defines an implicit cast operator on the iterator, so that this iterator
  /// can be used in the SSAUpdaterImpl.
  template <typename PHIArgT = SILPhiArgument,
            typename IteratorT = arg_iterator>
  class phi_iterator_impl {
  private:
    IteratorT It;

  public:
    explicit phi_iterator_impl(IteratorT A) : It(A) {}
    phi_iterator_impl &operator++() { ++It; return *this; }

    operator PHIArgT *() { return cast<PHIArgT>(*It); }
    bool operator==(const phi_iterator_impl& x) const { return It == x.It; }
    bool operator!=(const phi_iterator_impl& x) const { return !operator==(x); }
  };
  typedef phi_iterator_impl<> phi_iterator;
  typedef phi_iterator_impl<const SILPhiArgument,
                            SILBasicBlock::const_arg_iterator>
      const_phi_iterator;

  inline iterator_range<phi_iterator> phis() {
    return make_range(phi_iterator(args_begin()), phi_iterator(args_end()));
  }
  inline iterator_range<const_phi_iterator> phis() const {
    return make_range(const_phi_iterator(args_begin()),
                      const_phi_iterator(args_end()));
  }

  ArrayRef<SILArgument *> getArguments() const { return ArgumentList; }

  /// Returns a transform array ref that performs llvm::cast<NAME>
  /// each argument and then returns the downcasted value.
#define ARGUMENT(NAME, PARENT) NAME##ArrayRef get##NAME##s() const;
#include "swift/SIL/SILNodes.def"

  unsigned getNumArguments() const { return ArgumentList.size(); }
  const SILArgument *getArgument(unsigned i) const { return ArgumentList[i]; }
  SILArgument *getArgument(unsigned i) { return ArgumentList[i]; }

  void cloneArgumentList(SILBasicBlock *Other);

  void moveArgumentList(SILBasicBlock *from);

  /// Erase a specific argument from the arg list.
  void eraseArgument(int Index);

  /// Allocate a new argument of type \p Ty and append it to the argument
  /// list. Optionally you can pass in a value decl parameter.
  SILFunctionArgument *createFunctionArgument(SILType Ty,
                                              const ValueDecl *D = nullptr,
                                              bool disableEntryBlockVerification = false);

  SILFunctionArgument *insertFunctionArgument(unsigned AtArgPos, SILType Ty,
                                              ValueOwnershipKind OwnershipKind,
                                              const ValueDecl *D = nullptr);

  /// Replace the \p{i}th Function arg with a new Function arg with SILType \p
  /// Ty and ValueDecl \p D.
  SILFunctionArgument *replaceFunctionArgument(unsigned i, SILType Ty,
                                               ValueOwnershipKind Kind,
                                               const ValueDecl *D = nullptr);

  /// Replace the \p{i}th BB arg with a new BBArg with SILType \p Ty and
  /// ValueDecl \p D.
  ///
  /// NOTE: This assumes that the current argument in position \p i has had its
  /// uses eliminated. To replace/replace all uses with, use
  /// replacePhiArgumentAndRAUW.
  SILPhiArgument *replacePhiArgument(unsigned i, SILType type,
                                     ValueOwnershipKind kind,
                                     const ValueDecl *decl = nullptr,
                                     bool isReborrow = false,
                                     bool isEscaping = false);

  /// Replace phi argument \p i and RAUW all uses.
  SILPhiArgument *replacePhiArgumentAndReplaceAllUses(
      unsigned i, SILType type, ValueOwnershipKind kind,
      const ValueDecl *decl = nullptr, bool isReborrow = false,
      bool isEscaping = false);

  /// Allocate a new argument of type \p Ty and append it to the argument
  /// list. Optionally you can pass in a value decl parameter, reborrow flag and
  /// escaping flag.
  SILPhiArgument *createPhiArgument(SILType Ty, ValueOwnershipKind Kind,
                                    const ValueDecl *D = nullptr,
                                    bool isReborrow = false,
                                    bool isEscaping = false);

  /// Insert a new SILPhiArgument with type \p Ty and \p Decl at position \p
  /// AtArgPos.
  SILPhiArgument *insertPhiArgument(unsigned AtArgPos, SILType Ty,
                                    ValueOwnershipKind Kind,
                                    const ValueDecl *D = nullptr,
                                    bool isReborrow = false,
                                    bool isEscaping = false);

  /// Remove all block arguments.
  void dropAllArguments();

  //===--------------------------------------------------------------------===//
  // Successors
  //===--------------------------------------------------------------------===//

  using SuccessorListTy = TermInst::SuccessorListTy;
  using ConstSuccessorListTy = TermInst::ConstSuccessorListTy;
  
  /// The successors of a SILBasicBlock are defined either explicitly as
  /// a single successor as the branch targets of the terminator instruction.
  ConstSuccessorListTy getSuccessors() const {
    return getTerminator()->getSuccessors();
  }
  SuccessorListTy getSuccessors() {
    return getTerminator()->getSuccessors();
  }

  using const_succ_iterator = TermInst::const_succ_iterator;
  using succ_iterator = TermInst::succ_iterator;

  bool succ_empty() const { return getTerminator()->succ_empty(); }
  succ_iterator succ_begin() { return getTerminator()->succ_begin(); }
  succ_iterator succ_end() { return getTerminator()->succ_end(); }
  const_succ_iterator succ_begin() const {
    return getTerminator()->succ_begin();
  }
  const_succ_iterator succ_end() const { return getTerminator()->succ_end(); }

  using succblock_iterator = TermInst::succblock_iterator;
  using const_succblock_iterator = TermInst::const_succblock_iterator;

  succblock_iterator succblock_begin() {
    return getTerminator()->succblock_begin();
  }
  succblock_iterator succblock_end() {
    return getTerminator()->succblock_end();
  }
  const_succblock_iterator succblock_begin() const {
    return getTerminator()->succblock_begin();
  }
  const_succblock_iterator succblock_end() const {
    return getTerminator()->succblock_end();
  }
  
  unsigned getNumSuccessors() const {
    return getTerminator()->getNumSuccessors();
  }

  SILBasicBlock *getSingleSuccessorBlock() {
    return getTerminator()->getSingleSuccessorBlock();
  }

  const SILBasicBlock *getSingleSuccessorBlock() const {
    return getTerminator()->getSingleSuccessorBlock();
  }

  using SuccessorBlockListTy = TermInst::SuccessorBlockListTy;
  using ConstSuccessorBlockListTy = TermInst::ConstSuccessorBlockListTy;

  /// Return the range of SILBasicBlocks that are successors of this block.
  SuccessorBlockListTy getSuccessorBlocks() {
    return getTerminator()->getSuccessorBlocks();
  }

  /// Return the range of SILBasicBlocks that are successors of this block.
  ConstSuccessorBlockListTy getSuccessorBlocks() const {
    return getTerminator()->getSuccessorBlocks();
  }

  //===--------------------------------------------------------------------===//
  // Predecessors
  //===--------------------------------------------------------------------===//

  using pred_iterator = SILSuccessor::pred_iterator;

  bool pred_empty() const { return PredList == nullptr; }
  pred_iterator pred_begin() const { return pred_iterator(PredList); }
  pred_iterator pred_end() const { return pred_iterator(); }

  iterator_range<pred_iterator> getPredecessorBlocks() const {
    return {pred_begin(), pred_end()};
  }

  SILBasicBlock *getSinglePredecessorBlock() {
    if (pred_empty() || std::next(pred_begin()) != pred_end())
      return nullptr;
    return *pred_begin();
  }

  const SILBasicBlock *getSinglePredecessorBlock() const {
    return const_cast<SILBasicBlock *>(this)->getSinglePredecessorBlock();
  }

  //===--------------------------------------------------------------------===//
  // Utility
  //===--------------------------------------------------------------------===//

  /// Returns true if this BB is the entry BB of its parent.
  bool isEntry() const;

  /// Returns true if this block ends in an unreachable or an apply of a
  /// no-return apply or builtin.
  bool isNoReturn() const;

  /// Returns true if this block only contains a branch instruction.
  bool isTrampoline() const;

  /// Returns true if it is legal to hoist instructions into this block.
  ///
  /// Used by llvm::LoopInfo.
  bool isLegalToHoistInto() const;

  /// Returns the debug scope of the first non-meta instructions in the
  /// basic block. SILBuilderWithScope uses this to correctly set up
  /// the debug scope for newly created instructions.
  const SILDebugScope *getScopeOfFirstNonMetaInstruction();

  /// Whether the block has any phi arguments.
  ///
  /// Note that a block could have an argument and still return false.  The
  /// argument must also satisfy SILPhiArgument::isPhi.
  bool hasPhi() const;

  //===--------------------------------------------------------------------===//
  // Debugging
  //===--------------------------------------------------------------------===//

  /// Pretty-print the SILBasicBlock.
  void dump() const;

  /// Pretty-print the SILBasicBlock with the designated stream.
  void print(llvm::raw_ostream &OS) const;

  /// Pretty-print the SILBasicBlock with the designated context.
  void print(SILPrintContext &Ctx) const;

  void printAsOperand(raw_ostream &OS, bool PrintType = true);

  /// getSublistAccess() - returns pointer to member of instruction list
  static InstListType SILBasicBlock::*getSublistAccess() {
    return &SILBasicBlock::InstList;
  }

  /// Drops all uses that belong to this basic block.
  void dropAllReferences() {
    dropAllArguments();
    for (SILInstruction &I : *this)
      I.dropAllReferences();
  }

private:
  friend class SILArgument;

  /// BBArgument's ctor adds it to the argument list of this block.
  void insertArgument(arg_iterator Iter, SILArgument *Arg) {
    ArgumentList.insert(Iter, Arg);
  }
};

inline llvm::raw_ostream &operator<<(llvm::raw_ostream &OS,
                                     const SILBasicBlock &BB) {
  BB.print(OS);
  return OS;
}
} // end swift namespace

namespace llvm {

//===----------------------------------------------------------------------===//
// ilist_traits for SILBasicBlock
//===----------------------------------------------------------------------===//

template <>
struct ilist_traits<::swift::SILBasicBlock>
  : ilist_node_traits<::swift::SILBasicBlock> {
  using SelfTy = ilist_traits<::swift::SILBasicBlock>;
  using SILBasicBlock = ::swift::SILBasicBlock;
  using SILFunction = ::swift::SILFunction;
  using FunctionPtrTy = ::swift::NullablePtr<SILFunction>;

private:
  friend class ::swift::SILFunction;

  SILFunction *Parent;
  using block_iterator = simple_ilist<SILBasicBlock>::iterator;

public:
  static void deleteNode(SILBasicBlock *BB) { BB->~SILBasicBlock(); }

  void transferNodesFromList(ilist_traits<SILBasicBlock> &SrcTraits,
                             block_iterator First, block_iterator Last);
private:
  static void createNode(const SILBasicBlock &);
};

} // end llvm namespace

//===----------------------------------------------------------------------===//
//                           PhiOperand & PhiValue
//===----------------------------------------------------------------------===//

namespace swift {

/// Represent a phi argument without storing pointers to branches or their
/// operands which are invalidated by adding new, unrelated phi values. Because
/// this only stores a block pointer, it remains valid as long as the CFG is
/// immutable and the index of the phi value does not change.
///
/// Note: this should not be confused with SILPhiArgument which should be
/// renamed to SILPhiValue and only used for actual phis.
///
/// Warning: This is invalid for CondBranchInst arguments. Clients assume that
/// any instructions inserted at the phi argument is post-dominated by that phi
/// argument. This warning can be removed once the SILVerifier fully prohibits
/// CondBranchInst arguments at all SIL stages.
struct PhiOperand {
  SILBasicBlock *predBlock = nullptr;
  unsigned argIndex = 0;

  PhiOperand() = default;

  // This if \p operand is a CondBrInst operand, then this constructs and
  // invalid PhiOperand. The abstraction only works for non-trivial OSSA values.
  PhiOperand(Operand *operand) {
    auto *branch = dyn_cast<BranchInst>(operand->getUser());
    if (!branch)
      return;

    predBlock = branch->getParent();
    argIndex = operand->getOperandNumber();
  }

  explicit operator bool() const { return predBlock != nullptr; }

  bool operator==(PhiOperand other) const {
    return predBlock == other.predBlock && argIndex == other.argIndex;
  }

  bool operator!=(PhiOperand other) const { return !(*this == other); }

  BranchInst *getBranch() const {
    return cast<BranchInst>(predBlock->getTerminator());
  }

  Operand *getOperand() const {
    return &getBranch()->getAllOperands()[argIndex];
  }

  SILPhiArgument *getValue() const {
    return
      cast<SILPhiArgument>(getBranch()->getDestBB()->getArgument(argIndex));
  }

  SILValue getSource() const {
    return getOperand()->get();
  }

  operator Operand *() const { return getOperand(); }
  Operand *operator*() const { return getOperand(); }
  Operand *operator->() const { return getOperand(); }
};

/// Represent a phi value without referencing the SILValue, which is invalidated
/// by adding new, unrelated phi values. Because this only stores a block
/// pointer, it remains valid as long as the CFG is immutable and the index of
/// the phi value does not change.
struct PhiValue {
  SILBasicBlock *phiBlock = nullptr;
  unsigned argIndex = 0;

  PhiValue() = default;

  PhiValue(SILValue value) {
    if (auto *blockArg = SILArgument::asPhi(value)) {
      phiBlock = blockArg->getParent();
      argIndex = blockArg->getIndex();
    }
  }

  explicit operator bool() const { return phiBlock != nullptr; }

  bool operator==(PhiValue other) const {
    return phiBlock == other.phiBlock && argIndex == other.argIndex;
  }

  bool operator!=(PhiValue other) const { return !(*this == other); }

  SILPhiArgument *getValue() const {
    return cast<SILPhiArgument>(phiBlock->getArgument(argIndex));
  }

  Operand *getOperand(SILBasicBlock *predecessor) {
    auto *term = predecessor->getTerminator();
    if (auto *branch = dyn_cast<BranchInst>(term)) {
      return &branch->getAllOperands()[argIndex];
    }
    // TODO: Support CondBr for legacy reasons
    return cast<CondBranchInst>(term)->getOperandForDestBB(phiBlock, argIndex);
  }

  operator SILValue() const { return getValue(); }
  SILValue operator*() const { return getValue(); }
  SILValue operator->() const { return getValue(); }
};

} // namespace swift

namespace llvm {

template <> struct DenseMapInfo<swift::PhiOperand> {
  static swift::PhiOperand getEmptyKey() { return swift::PhiOperand(); }
  static swift::PhiOperand getTombstoneKey() {
    swift::PhiOperand phiOper;
    phiOper.predBlock =
        llvm::DenseMapInfo<swift::SILBasicBlock *>::getTombstoneKey();
    return phiOper;
  }
  static unsigned getHashValue(swift::PhiOperand phiOper) {
    return llvm::hash_combine(phiOper.predBlock, phiOper.argIndex);
  }
  static bool isEqual(swift::PhiOperand lhs, swift::PhiOperand rhs) {
    return lhs == rhs;
  }
};

template <> struct DenseMapInfo<swift::PhiValue> {
  static swift::PhiValue getEmptyKey() { return swift::PhiValue(); }
  static swift::PhiValue getTombstoneKey() {
    swift::PhiValue phiValue;
    phiValue.phiBlock =
        llvm::DenseMapInfo<swift::SILBasicBlock *>::getTombstoneKey();
    return phiValue;
  }
  static unsigned getHashValue(swift::PhiValue phiValue) {
    return llvm::hash_combine(phiValue.phiBlock, phiValue.argIndex);
  }
  static bool isEqual(swift::PhiValue lhs, swift::PhiValue rhs) {
    return lhs == rhs;
  }
};

} // end namespace llvm

//===----------------------------------------------------------------------===//
// Inline SILInstruction implementations
//===----------------------------------------------------------------------===//

namespace swift {

inline SILFunction *SILInstruction::getFunction() const {
  return getParent()->getParent();
}

inline SILInstruction *SILInstruction::getPreviousInstruction() {
  auto pos = getIterator();
  return pos == getParent()->begin() ? nullptr : &*std::prev(pos);
}

inline SILInstruction *SILInstruction::getNextInstruction() {
  auto nextPos = std::next(getIterator());
  return nextPos == getParent()->end() ? nullptr : &*nextPos;
}

} // end swift namespace

#endif
