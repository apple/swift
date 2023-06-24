//===--- SILInstruction.h - Instructions for SIL code -----------*- C++ -*-===//
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
// This file defines the high-level SILInstruction class used for SIL code.
//
//===----------------------------------------------------------------------===//

#ifndef SWIFT_SIL_INSTRUCTION_H
#define SWIFT_SIL_INSTRUCTION_H

#include "swift/AST/AutoDiff.h"
#include "swift/AST/Builtins.h"
#include "swift/AST/Decl.h"
#include "swift/AST/GenericSignature.h"
#include "swift/AST/ProtocolConformanceRef.h"
#include "swift/AST/SubstitutionMap.h"
#include "swift/AST/TypeAlignments.h"
#include "swift/Basic/Compiler.h"
#include "swift/Basic/NullablePtr.h"
#include "swift/Basic/OptionSet.h"
#include "swift/Basic/ProfileCounter.h"
#include "swift/Basic/Range.h"
#include "swift/SIL/Consumption.h"
#include "swift/SIL/SILAllocated.h"
#include "swift/SIL/SILArgumentArrayRef.h"
#include "swift/SIL/SILDebugInfoExpression.h"
#include "swift/SIL/SILDebugVariable.h"
#include "swift/SIL/SILDeclRef.h"
#include "swift/SIL/SILFunctionConventions.h"
#include "swift/SIL/SILLocation.h"
#include "swift/SIL/SILSuccessor.h"
#include "swift/SIL/SILValue.h"
#include "swift/SIL/ValueUtils.h"
#include "swift/Strings.h"
#include "llvm/ADT/APFloat.h"
#include "llvm/ADT/APInt.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/ilist.h"
#include "llvm/ADT/ilist_node.h"
#include "llvm/Support/TrailingObjects.h"
#include <array>

namespace llvm {
namespace ilist_detail {

/// The base class of the instruction list in SILBasicBlock.
///
/// We need a custom base class to not clear the prev/next pointers when
/// removing an instruction from the list.
class SILInstructionListBase : public ilist_base<false> {
public:
  /// Remove an instruction from the list.
  ///
  /// In contrast to the default implementation, it does not clear the prev/
  /// next pointers in the node. This is needed to being able to remove
  /// instructions from the list while iterating over the list.
  /// For details see `DeletableInstructionsIterator`.
  template <class T> static void remove(T &N) {
    node_base_type *Prev = N.getPrev();
    node_base_type *Next = N.getNext();
    Next->setPrev(Prev);
    Prev->setNext(Next);
  }

  template <class T> static void insertBefore(T &Next, T &N) {
    insertBeforeImpl(Next, N);
  }

  template <class T> static void transferBefore(T &Next, T &First, T &Last) {
    transferBeforeImpl(Next, First, Last);
  }
};

// This template specialization is needed to replace the default instruction
// list base class with `SILInstructionListBase`.
template <> struct compute_node_options<::swift::SILInstruction> {
  struct type {
    typedef ::swift::SILInstruction value_type;
    typedef value_type *pointer;
    typedef value_type &reference;
    typedef const value_type *const_pointer;
    typedef const value_type &const_reference;

    static const bool enable_sentinel_tracking = false;
    static const bool is_sentinel_tracking_explicit = false;
    typedef void tag;
    typedef ilist_node_base<enable_sentinel_tracking> node_base_type;
    typedef SILInstructionListBase list_base_type;
  };
};

} // end namespace ilist_detail
} // end llvm namespace

namespace swift {

class AllocationInst;
class DeclRefExpr;
class FloatLiteralExpr;
class FuncDecl;
class IntegerLiteralExpr;
class SingleValueInstruction;
class MultipleValueInstruction;
class MultipleValueInstructionResult;
class DestructureTupleInst;
class DestructureStructInst;
class NonValueInstruction;
class SILBasicBlock;
class SILBuilder;
class SILDebugLocation;
class SILDebugScope;
class SILDifferentiabilityWitness;
class SILFunction;
class SILGlobalVariable;
class SILInstructionResultArray;
class SILType;
class SILArgument;
class SILPhiArgument;
class SILUndef;
class Stmt;
class StringLiteralExpr;
class ValueDecl;
class VarDecl;
class FunctionRefBaseInst;
class SILPrintContext;

template <typename ImplClass> class SILClonerWithScopes;

enum class MemoryBehavior {
  None,
  /// The instruction may read memory.
  MayRead,
  /// The instruction may write to memory.
  /// This includes destroying or taking from memory (e.g. destroy_addr,
  /// copy_addr [take], load [take]).
  /// Although, physically, destroying or taking does not modify the memory,
  /// it is important to model it is a write. Optimizations must not assume
  /// that the value stored in memory is still available for loading after
  /// the memory is destroyed or taken.
  MayWrite,
  /// The instruction may read or write memory.
  MayReadWrite,
  /// The instruction may have side effects not captured
  ///        solely by its users. Specifically, it can return,
  ///        release memory, or store. Note, alloc is not considered
  ///        to have side effects because its result/users represent
  ///        its effect.
  MayHaveSideEffects,
};

// An enum class for SILInstructions that enables exhaustive switches over
// instructions.
enum class SILInstructionKind : std::underlying_type<SILNodeKind>::type {
#define INST(ID, PARENT) \
  ID = unsigned(SILNodeKind::ID),
#define INST_RANGE(ID, FIRST, LAST) \
  First_##ID = unsigned(SILNodeKind::First_##ID), \
  Last_##ID = unsigned(SILNodeKind::Last_##ID),
#include "SILNodes.def"
};

/// Return a range which can be used to easily iterate over all
/// SILInstructionKinds.
inline IntRange<SILInstructionKind> allSILInstructionKinds() {
  return IntRange<SILInstructionKind>(
            SILInstructionKind(SILNodeKind::First_SILInstruction),
            SILInstructionKind(unsigned(SILNodeKind::Last_SILInstruction) + 1));
}

/// Map SILInstruction's mnemonic name to its SILInstructionKind.
SILInstructionKind getSILInstructionKind(StringRef InstName);

/// Map SILInstructionKind to a corresponding SILInstruction name.
StringRef getSILInstructionName(SILInstructionKind Kind);

/// A formal SIL reference to a list of values, suitable for use as the result
/// of a SILInstruction.
///
/// *NOTE* Most multiple value instructions will not have many results, so if we
/// want we can cache up to 3 bytes in the lower bits of the value.
///
/// *NOTE* Most of this defined out of line further down in the file to work
/// around forward declaration issues.
///
/// *NOTE* The reason why this does not store the size of the stored element is
/// that just from the number of elements we can infer the size of each element
/// due to the restricted problem space. Specifically:
///
/// 1. Size == 0 implies nothing is stored and thus element size is irrelevant.
/// 2. Size == 1 implies we either had a single value instruction or a multiple
/// value instruction, but no matter what instruction we had, we are going to
/// store the results at the same starting location so element size is
/// irrelevant.
/// 3. Size > 1 implies we must be storing multiple value instruction results
/// implying that the size of each stored element must be
/// sizeof(MultipleValueInstructionResult).
///
/// If we ever allow for subclasses of MultipleValueInstructionResult of
/// different sizes, we will need to store a stride into
/// SILInstructionResultArray. We always assume all results are the same
/// subclass of MultipleValueInstructionResult.
class SILInstructionResultArray {
  friend class MultipleValueInstruction;

  /// Byte pointer to our data. nullptr for empty arrays.
  const uint8_t *Pointer;

  /// The number of stored elements.
  unsigned Size;

public:
  SILInstructionResultArray() : Pointer(nullptr), Size(0) {}
  SILInstructionResultArray(const SingleValueInstruction *SVI);
  SILInstructionResultArray(ArrayRef<MultipleValueInstructionResult> results);

  template <class Result>
  SILInstructionResultArray(ArrayRef<Result> results);

  SILInstructionResultArray(const SILInstructionResultArray &Other) = default;
  SILInstructionResultArray &
  operator=(const SILInstructionResultArray &Other) = default;
  SILInstructionResultArray(SILInstructionResultArray &&Other) = default;
  SILInstructionResultArray &
  operator=(SILInstructionResultArray &&Other) = default;

  SILValue operator[](size_t Index) const;

  bool empty() const { return Size == 0; }

  size_t size() const { return Size; }

  class iterator;

  friend bool operator==(iterator, iterator);
  friend bool operator!=(iterator, iterator);

  iterator begin() const;
  iterator end() const;

  using reverse_iterator = std::reverse_iterator<iterator>;
  reverse_iterator rbegin() const;
  reverse_iterator rend() const;

  using range = iterator_range<iterator>;
  range getValues() const;
  using reverse_range = iterator_range<reverse_iterator>;
  reverse_range getReversedValues() const;

  using type_range = iterator_range<
    llvm::mapped_iterator<iterator, SILType(*)(SILValue), SILType>>;
  type_range getTypes() const;

  bool operator==(const SILInstructionResultArray &rhs);
  bool operator!=(const SILInstructionResultArray &other) {
    return !(*this == other);
  }

  /// Returns true if both this and \p rhs have the same result types.
  ///
  /// *NOTE* This does not imply that the actual return SILValues are the
  /// same. Just that the types are the same.
  bool hasSameTypes(const SILInstructionResultArray &rhs);

private:
  /// Return the first element of the array. Asserts if the array is empty.
  ///
  /// Please do not use this outside of this class. It is only meant to speedup
  /// MultipleValueInstruction::getIndexOfResult(SILValue).
  const ValueBase *front() const;

  /// Return the last element of the array. Asserts if the array is empty.
  ///
  /// Please do not use this outside of this class. It is only meant to speedup
  /// MultipleValueInstruction::getIndexOfResult(SILValue).
  const ValueBase *back() const;
};

class SILInstructionResultArray::iterator {
  /// Our "parent" array.
  ///
  /// This is actually a value type reference into a SILInstruction of some
  /// sort. So we can just have our own copy. This also allows us to not worry
  /// about our underlying array having too short of a lifetime.
  SILInstructionResultArray Parent;

  /// The index into the parent array.
  unsigned Index;

public:
  using difference_type = int;
  using value_type = SILValue;
  using pointer = void;
  using reference = SILValue;
  using iterator_category = std::bidirectional_iterator_tag;

  iterator() = default;
  iterator(const SILInstructionResultArray &Parent, unsigned Index = 0)
      : Parent(Parent), Index(Index) {}

  SILValue operator*() const { return Parent[Index]; }
  SILValue operator->() const { return operator*(); }

  iterator &operator++() {
    ++Index;
    return *this;
  }

  iterator operator++(int) {
    iterator copy = *this;
    ++Index;
    return copy;
  }

  iterator &operator--() {
    --Index;
    return *this;
  }

  iterator operator--(int) {
    iterator copy = *this;
    --Index;
    return copy;
  }

  friend bool operator==(iterator lhs, iterator rhs) {
    assert(lhs.Parent.Pointer == rhs.Parent.Pointer);
    return lhs.Index == rhs.Index;
  }

  friend bool operator!=(iterator lhs, iterator rhs) { return !(lhs == rhs); }
};

inline SILInstructionResultArray::iterator
SILInstructionResultArray::begin() const {
  return iterator(*this, 0);
}

inline SILInstructionResultArray::iterator
SILInstructionResultArray::end() const {
  return iterator(*this, size());
}

inline SILInstructionResultArray::reverse_iterator
SILInstructionResultArray::rbegin() const {
  return std::make_reverse_iterator(end());
}

inline SILInstructionResultArray::reverse_iterator
SILInstructionResultArray::rend() const {
  return std::make_reverse_iterator(begin());
}

inline SILInstructionResultArray::range
SILInstructionResultArray::getValues() const {
  return {begin(), end()};
}

inline SILInstructionResultArray::reverse_range
SILInstructionResultArray::getReversedValues() const {
  return {rbegin(), rend()};
}

/// This is the root class for all instructions that can be used as the
/// contents of a Swift SILBasicBlock.
///
/// Most instructions are defined in terms of two basic kinds of
/// structure: a list of operand values upon which the instruction depends
/// and a list of result values upon which other instructions can depend.
///
/// The operands can be divided into two sets:
///   - the formal operands of the instruction, which reflect its
///     direct value dependencies, and
///   - the type-dependent operands, which reflect dependencies that are
///     not captured by the formal operands; currently, these dependencies
///     only arise due to certain instructions (e.g. open_existential_addr)
///     that bind new archetypes in the local context.
///
/// Conceptually, SILInstruction is a sub-class of SILNode. But implementation-
/// wise, only the two sub-classes of SILInstruction - SingleValueInstruction
/// and NonSingleValueInstruction - inherit from SILNode. Although the
/// SingleValueInstruction's SILNode is embedded into a ValueBase, its relative
/// offset in the class is the same as in NonSingleValueInstruction (see
/// SILNodeOffsetChecker). This makes it possible to cast from a SILInstruction
/// to a SILNode without knowing which SILInstruction sub-class it is.
/// Note that casting a SILInstruction to a SILNode cannot be done implicitly,
/// but only with an LLVM `cast` or with SILInstruction::asSILNode().
class SILInstruction : public llvm::ilist_node<SILInstruction> {
  friend llvm::ilist_traits<SILInstruction>;
  friend llvm::ilist_traits<SILBasicBlock>;
  friend SILBasicBlock;
  friend SILModule;

  /// A backreference to the containing basic block.  This is maintained by
  /// ilist_traits<SILInstruction>.
  SILBasicBlock *ParentBB = nullptr;

  /// This instruction's containing lexical scope for debug info.
  const SILDebugScope *debugScope = nullptr;

  /// This instructions source location for diagnostics and debug info.
  ///
  /// To reduce space, this is only the storage of the SILLocation. The
  /// location's kindAndFlags is stored in `SILNode::locationKindAndFlags`.
  SILLocation::Storage locationStorage;

  void operator=(const SILInstruction &) = delete;
  void operator delete(void *Ptr, size_t) = delete;

  /// Check any special state of instructions that are not represented in the
  /// instructions operands/type.
  bool hasIdenticalState(const SILInstruction *RHS) const;

  /// Update this instruction's SILDebugScope. This function should
  /// never be called directly. Use SILBuilder, SILBuilderWithScope or
  /// SILClonerWithScope instead.
  void setDebugScope(const SILDebugScope *DS);

  /// Total number of created and deleted SILInstructions.
  ///
  /// Ideally, those counters would be inside SILModules to allow mutiple
  /// SILModules (e.g. in different threads).
  static int NumCreatedInstructions;
  static int NumDeletedInstructions;

  // Helper functions used by the ArrayRefViews below.
  static SILValue projectValueBaseAsSILValue(const ValueBase &value) {
    return &value;
  }
  static SILType projectValueBaseType(const ValueBase &value) {
    return value.getType();
  }

  /// An internal method which retrieves the result values of the
  /// instruction as an array of ValueBase objects.
  SILInstructionResultArray getResultsImpl() const;

protected:
  friend class SwiftPassInvocation;

  SILInstruction() {
    NumCreatedInstructions++;
  }

  ~SILInstruction() {
    NumDeletedInstructions++;
  }

public:
  /// Instructions should be allocated using a dedicated instruction allocation
  /// function from the ContextTy.
  template <typename ContextTy>
  void *operator new(size_t Bytes, const ContextTy &C,
                     size_t Alignment = alignof(ValueBase)) {
    return C.allocateInst(Bytes, Alignment);
  }

  /// Returns true if this instruction is removed from its function and
  /// scheduled to be deleted.
  bool isDeleted() const { return asSILNode()->isMarkedAsDeleted(); }

  /// Enumeration representing whether the execution of an instruction can
  /// result in memory being released.
  enum class ReleasingBehavior {
    DoesNotRelease,
    MayRelease,
  };
  
  SILNode *asSILNode();
  const SILNode *asSILNode() const;

  LLVM_ATTRIBUTE_ALWAYS_INLINE
  SILInstructionKind getKind() const;

  SILBasicBlock *getParent() const { return ParentBB; }

  SILFunction *getFunction() const;

  /// Is this instruction part of a static initializer of a SILGlobalVariable?
  bool isStaticInitializerInst() const { return getFunction() == nullptr; }

  SILModule &getModule() const;

  /// This instruction's source location (AST node).
  SILLocation getLoc() const {
    return SILLocation(locationStorage, asSILNode()->locationKindAndFlags);
  }
  const SILDebugScope *getDebugScope() const { return debugScope; }
  SILDebugLocation getDebugLocation() const {
    return SILDebugLocation(getLoc(), debugScope);
  }

  /// Sets the debug location.
  /// Note: Usually it should not be needed to use this function as the location
  /// is already set in when creating an instruction.
  void setDebugLocation(SILDebugLocation debugLoc) {
    debugScope = debugLoc.getScope();
    SILLocation loc = debugLoc.getLocation();
    asSILNode()->locationKindAndFlags = loc.kindAndFlags.packedKindAndFlags;
    locationStorage = loc.storage;
  }

  /// Return the previous instruction, or nullptr if this is the first
  /// instruction in its block.
  SILInstruction *getPreviousInstruction();

  /// Return the next instruction, or nullptr if this is the final
  /// instruction in its block.
  SILInstruction *getNextInstruction();

  /// This method unlinks 'self' from the containing basic block and deletes it.
  void eraseFromParent();

  /// Unlink this instruction from its current basic block and insert the
  /// instruction such that it is the first instruction of \p Block.
  void moveFront(SILBasicBlock *Block);

  /// Unlink this instruction from its current basic block and insert it into
  /// the basic block that Later lives in, right before Later.
  void moveBefore(SILInstruction *Later);

  /// Unlink this instruction from its current basic block and insert it into
  /// the basic block that Earlier lives in, right after Earlier.
  void moveAfter(SILInstruction *Earlier);

  /// Drops all uses that belong to this instruction.
  void dropAllReferences();

  /// Drops all references that aren't represented by operands.
  void dropNonOperandReferences();

  /// Replace all uses of all results of this instruction with undef.
  void replaceAllUsesOfAllResultsWithUndef();

  /// Replace all uses of all results of this instruction
  /// with the parwise-corresponding results of the given instruction.
  void replaceAllUsesPairwiseWith(SILInstruction *other);

  /// Replace all uses of all results of this instruction with the
  /// parwise-corresponding results of the passed in array.
  void
  replaceAllUsesPairwiseWith(const llvm::SmallVectorImpl<SILValue> &NewValues);

  /// Are there uses of any of the results of this instruction?
  bool hasUsesOfAnyResult() const {
    for (auto result : getResults()) {
      if (!result->use_empty())
        return true;
    }
    return false;
  }

  /// Return the array of operands for this instruction.
  ArrayRef<Operand> getAllOperands() const;

  /// Return the array of type dependent operands for this instruction.
  ///
  /// Type dependent operands are hidden operands, i.e. not part of the SIL
  /// syntax (although they are printed as "type-defs" in comments).
  /// Their purpose is to establish a def-use relationship between
  ///   -) an instruction/argument which defines a type, e.g. open_existential
  /// and
  ///   -) this instruction, which uses the type, but doesn't use the defining
  ///      instruction as value-operand, e.g. a type in the substitution list.
  ///
  /// Currently there are two kinds of type dependent operands:
  ///
  /// 1. for opened archetypes:
  ///     %o = open_existential_addr %0 : $*P to $*@opened("UUID") P
  ///     %w = witness_method $@opened("UUID") P, ... // type-defs: %o
  ///
  /// 2. for the dynamic self argument:
  ///     sil @foo : $@convention(method) (@thick X.Type) {
  ///     bb0(%0 : $@thick X.Type):
  ///       %a = apply %f<@dynamic_self X>() ... // type-defs: %0
  ///
  /// The type dependent operands are just there to let optimizations know that
  /// there is a dependency between the instruction/argument which defines the
  /// type and the instruction which uses the type.
  ArrayRef<Operand> getTypeDependentOperands() const;

  /// Return the array of mutable operands for this instruction.
  MutableArrayRef<Operand> getAllOperands();

  /// Return the array of mutable type dependent operands for this instruction.
  MutableArrayRef<Operand> getTypeDependentOperands();

  unsigned getNumOperands() const { return getAllOperands().size(); }

  unsigned getNumTypeDependentOperands() const {
    return getTypeDependentOperands().size();
  }

  unsigned getNumRealOperands() const {
    return getAllOperands().size() - getNumTypeDependentOperands();
  }

  bool isTypeDependentOperand(unsigned i) const {
    return i >= getNumOperands() - getNumTypeDependentOperands();
  }

  bool isTypeDependentOperand(const Operand &Op) const {
    assert(Op.getUser() == this &&
           "Operand does not belong to a SILInstruction");
    return isTypeDependentOperand(Op.getOperandNumber());
  }
      
  /// Returns true if evaluation of this instruction may cause suspension of an
  /// async task.
  bool maySuspend() const;

private:
  /// Functor for Operand::get()
  struct OperandToValue;
  /// Functor for Operand::get()
  struct OperandRefToValue;
  /// Predicate to filter NonTypeDependentOperandValueRange
  struct NonTypeDependentOperandToValue;
  /// Predicate to filter TransformedOperandValueRange.
  struct OperandToTransformedValue;

public:
  using OperandValueRange = TransformRange<ArrayRef<Operand*>, OperandToValue>;
  using OperandRefValueRange =
    TransformRange<ArrayRef<Operand>, OperandRefToValue>;
  using NonTypeDependentOperandValueRange =
    OptionalTransformRange<ArrayRef<Operand>, NonTypeDependentOperandToValue>;
  using TransformedOperandValueRange =
    OptionalTransformRange<ArrayRef<Operand>, OperandToTransformedValue>;

  static OperandValueRange getOperandValues(ArrayRef<Operand*> operands);

  OperandRefValueRange getOperandValues() const;

  NonTypeDependentOperandValueRange getNonTypeDependentOperandValues() const;

  TransformedOperandValueRange
  getOperandValues(std::function<SILValue(const Operand *)> transformFn,
                   bool skipTypeDependentOperands) const;

  SILValue getOperand(unsigned Num) const {
    return getAllOperands()[Num].get();
  }

  /// Return the ith mutable operand of this instruction.
  ///
  /// Equivalent to performing getAllOperands()[index];
  Operand &getOperandRef(unsigned index) { return getAllOperands()[index]; }

  /// Return the ith operand of this instruction.
  ///
  /// Equivalent to performing getAllOperands()[index];
  const Operand &getOperandRef(unsigned index) const {
    return getAllOperands()[index];
  }

  void setOperand(unsigned Num, SILValue V) { getAllOperands()[Num].set(V); }
  void swapOperands(unsigned Num1, unsigned Num2) {
    getAllOperands()[Num1].swap(getAllOperands()[Num2]);
  }

private:
  /// Predicate used to filter OperandTypeRange.
  struct OperandToType;

public:
  using OperandTypeRange =
      OptionalTransformRange<ArrayRef<Operand>, OperandToType>;
  // NOTE: We always skip type dependent operands.
  OperandTypeRange getOperandTypes() const;

  /// Return the list of results produced by this instruction.
  bool hasResults() const { return !getResults().empty(); }
  SILInstructionResultArray getResults() const { return getResultsImpl(); }
  unsigned getNumResults() const { return getResults().size(); }

  SILValue getResult(unsigned index) const { return getResults()[index]; }

  /// Return the types of the results produced by this instruction.
  SILInstructionResultArray::type_range getResultTypes() const {
    return getResultsImpl().getTypes();
  }

  /// Run the given function for each local archetype this instruction
  /// defines, passing the value that should be used to record the
  /// dependency.
  void forEachDefinedLocalArchetype(
      llvm::function_ref<void(CanLocalArchetypeType archetype,
                              SILValue typeDependency)> function) const;
  bool definesLocalArchetypes() const;

  MemoryBehavior getMemoryBehavior() const;
  ReleasingBehavior getReleasingBehavior() const;

  /// Returns true if the instruction may release any object.
  bool mayRelease() const;

  /// Returns true if the instruction may release or may read the reference
  /// count of any object.
  bool mayReleaseOrReadRefCount() const;

  /// Can this instruction abort the program in some manner?
  bool mayTrap() const;

  /// Returns true if the given instruction is completely identical to RHS.
  bool isIdenticalTo(const SILInstruction *RHS) const {
    return isIdenticalTo(RHS,
                         [](const SILValue &Op1, const SILValue &Op2) -> bool {
                           return Op1 == Op2; });
  }
  
  /// Returns true if the given instruction is completely identical to RHS,
  /// using \p opEqual to compare operands.
  ///
  bool
  isIdenticalTo(const SILInstruction *RHS,
                llvm::function_ref<bool(SILValue, SILValue)> opEqual) const {
    // Quick check if both instructions have the same kind, number of operands,
    // and types. This should filter out most cases.
    if (getKind() != RHS->getKind() ||
        getNumOperands() != RHS->getNumOperands()) {
      return false;
    }

    if (!getResults().hasSameTypes(RHS->getResults()))
      return false;

    // Check operands.
    for (unsigned i = 0, e = getNumOperands(); i != e; ++i)
      if (!opEqual(getOperand(i), RHS->getOperand(i)))
        return false;

    // Check any special state of instructions that are not represented in the
    // instructions operands/type.
    return hasIdenticalState(RHS);
  }

  bool isIdenticalTo(const SILInstruction *RHS,
                     llvm::function_ref<bool(const Operand *, const Operand *)>
                         opEqual) const {
    // Quick check if both instructions have the same kind, number of operands,
    // and types. This should filter out most cases.
    if (getKind() != RHS->getKind() ||
        getNumOperands() != RHS->getNumOperands()) {
      return false;
    }

    if (!getResults().hasSameTypes(RHS->getResults()))
      return false;

    // Check operands.
    for (unsigned i = 0, e = getNumOperands(); i != e; ++i)
      if (!opEqual(&getOperandRef(i), &RHS->getOperandRef(i)))
        return false;

    // Check any special state of instructions that are not represented in the
    // instructions operands/type.
    return hasIdenticalState(RHS);
  }

  /// Returns true if the instruction may have side effects.
  ///
  /// Instructions that store into memory or change retain counts as well as
  /// calls and deallocation instructions are considered to have side effects
  /// that are not visible by merely examining their uses.
  bool mayHaveSideEffects() const;

  /// Returns true if the instruction may write to memory, deinitialize memory,
  /// or have other unknown side effects.
  ///
  /// For details see MemoryBehavior.
  bool mayWriteToMemory() const {
    MemoryBehavior B = getMemoryBehavior();
    return B == MemoryBehavior::MayWrite ||
      B == MemoryBehavior::MayReadWrite ||
      B == MemoryBehavior::MayHaveSideEffects;
  }

  /// Returns true if the instruction may read from memory, or have other
  /// unknown side effects.
  ///
  /// For details see MemoryBehavior.
  bool mayReadFromMemory() const {
    MemoryBehavior B = getMemoryBehavior();
    return B == MemoryBehavior::MayRead ||
      B == MemoryBehavior::MayReadWrite ||
      B == MemoryBehavior::MayHaveSideEffects;
  }

  /// Returns true if the instruction may read from memory, write to memory,
  /// deinitialize memory, or have other unknown side effects.
  ///
  /// For details see MemoryBehavior.
  bool mayReadOrWriteMemory() const {
    return getMemoryBehavior() != MemoryBehavior::None;
  }

  /// Return true if the instruction is "pure" in the sense that it may execute
  /// multiple times without affecting behavior. This implies that it can be
  /// trivially cloned at multiple use sites without preserving path
  /// equivalence.
  bool isPure() const {
    return !mayReadOrWriteMemory() && !mayTrap() && !isa<AllocationInst>(this)
           && !isa<TermInst>(this);
  }

  /// Returns true if the result of this instruction is a pointer to stack
  /// allocated memory. In this case there must be an adjacent deallocating
  /// instruction.
  bool isAllocatingStack() const;

  /// Returns true if this is the deallocation of a stack allocating instruction.
  /// The first operand must be the allocating instruction.
  bool isDeallocatingStack() const;

  /// Whether IRGen lowering of this instruction may result in emitting packs of
  /// metadata or witness tables.
  bool mayRequirePackMetadata() const;

  /// Create a new copy of this instruction, which retains all of the operands
  /// and other information of this one.  If an insertion point is specified,
  /// then the new instruction is inserted before the specified point, otherwise
  /// the new instruction is returned without a parent.
  SILInstruction *clone(SILInstruction *InsertPt = nullptr);

  /// Invoke an Instruction's destructor. This dispatches to the appropriate
  /// leaf class destructor for the type of the instruction. This does not
  /// deallocate the instruction.
  static void destroy(SILInstruction *I);

  /// Returns true if the instruction can be duplicated without any special
  /// additional handling. It is important to know this information when
  /// you perform such optimizations like e.g. jump-threading.
  bool isTriviallyDuplicatable() const;

  /// Returns true if the instruction is only relevant for debug
  /// informations and has no other impact on program semantics.
  bool isDebugInstruction() const {
    return getKind() == SILInstructionKind::DebugValueInst;
  }

  /// Returns true if the instruction is a meta instruction which is
  /// relevant for debug information and does not get lowered to a real
  /// instruction.
  bool isMetaInstruction() const;

  /// Verify that all operands of this instruction have compatible ownership
  /// with this instruction.
  void verifyOperandOwnership(SILModuleConventions *silConv = nullptr) const;

  /// Verify that this instruction and its associated debug information follow
  /// all SIL debug info invariants.
  void verifyDebugInfo() const;

  /// Get the number of created SILInstructions.
  static int getNumCreatedInstructions() {
    return NumCreatedInstructions;
  }

  /// Get the number of deleted SILInstructions.
  static int getNumDeletedInstructions() {
    return NumDeletedInstructions;
  }
  
  static void resetInstructionCounts() {
    NumCreatedInstructions = 0;
    NumDeletedInstructions = 0;
  }

  /// Pretty-print the value.
  void dump() const;
  void print(raw_ostream &OS) const;

  /// Pretty-print the value in context, preceded by its operands (if the
  /// value represents the result of an instruction) and followed by its
  /// users.
  void dumpInContext() const;
  void printInContext(raw_ostream &OS) const;

  static bool classof(SILNodePointer node) {
    return node->getKind() >= SILNodeKind::First_SILInstruction &&
           node->getKind() <= SILNodeKind::Last_SILInstruction;
  }
  static bool classof(const SILInstruction *I) { return true; }

  /// This is supportable but usually suggests a logic mistake.
  static bool classof(const ValueBase *) = delete;
  
protected:
  unsigned getCachedFieldIndex(NominalTypeDecl *decl, VarDecl *property);
  unsigned getCachedCaseIndex(EnumElementDecl *enumElement);
};

inline SILNodePointer::SILNodePointer(const SILInstruction *inst) :
  node(inst->asSILNode()) { }

/// The base class for all instructions, which are not SingleValueInstructions:
/// NonValueInstruction and MultipleValueInstruction.
class NonSingleValueInstruction : public SILInstruction, public SILNode {
  friend struct SILNodeOffsetChecker;
public:
  NonSingleValueInstruction(SILInstructionKind kind, SILDebugLocation loc)
      : SILInstruction(), SILNode((SILNodeKind)kind) {
    setDebugLocation(loc);
  }

  using SILInstruction::operator new;
  using SILInstruction::dumpInContext;
  using SILInstruction::print;
  using SILInstruction::printInContext;

  // Redeclare because lldb currently doesn't know about using-declarations
  void dump() const;
  SILFunction *getFunction() const { return SILInstruction::getFunction(); }
  SILModule &getModule() const { return SILInstruction::getModule(); }

  /// Doesn't produce any results.
  SILType getType() const = delete;

  LLVM_ATTRIBUTE_ALWAYS_INLINE
  SILInstructionKind getKind() const {
    return (SILInstructionKind)SILNode::getKind();
  }
  
  static bool classof(const ValueBase *value) = delete;
  static bool classof(SILNodePointer node) {
    return node->getKind() >= SILNodeKind::First_NonSingleValueInstruction &&
           node->getKind() <= SILNodeKind::Last_NonSingleValueInstruction;
  }
  static bool classof(const NonSingleValueInstruction *) { return true; }
};

inline SILNode *SILInstruction::asSILNode() {
  // Even if this instruction is not a NonSingleValueInstruction, but a
  // SingleValueInstruction, the SILNode is at the same offset as in a
  // NonSingleValueInstruction. See the top-level comment of SILInstruction.
  SILNode *node = (NonSingleValueInstruction *)this;
  assert(isa<SingleValueInstruction>(node) ||
         isa<NonSingleValueInstruction>(node));
  return node;
}
inline const SILNode *SILInstruction::asSILNode() const {
  return (const_cast<SILInstruction *>(this))->asSILNode();
}

inline SILNodePointer::SILNodePointer(const NonSingleValueInstruction *nsvi) :
  node(nsvi) { }

inline SILInstructionKind SILInstruction::getKind() const {
  return SILInstructionKind(asSILNode()->getKind());
}

inline SILInstruction *SILNode::castToInstruction() {
  assert(isa<SILInstruction>(this));
  // We use the same trick here as in SILInstruction::asSILNode().
  auto *nsvi = (NonSingleValueInstruction *)this;
  assert((SILNodeKind)nsvi->getKind() == getKind());
  return nsvi;
}

inline SILNode *SILNode::instAsNode(SILInstruction *inst) {
  return inst->asSILNode();
}
inline const SILNode *SILNode::instAsNode(const SILInstruction *inst) {
  return inst->asSILNode();
}


struct SILInstruction::OperandToValue {
  SILValue operator()(const Operand *use) const {
    return use->get();
  }
};

struct SILInstruction::OperandRefToValue {
  SILValue operator()(const Operand &use) const {
    return use.get();
  }
};

struct SILInstruction::NonTypeDependentOperandToValue {
  const SILInstruction &i;

  NonTypeDependentOperandToValue(const SILInstruction &i): i(i) {}

  Optional<SILValue> operator()(const Operand &use) const {
    if (i.isTypeDependentOperand(use))
      return None;
    return use.get();
  }
};

struct SILInstruction::OperandToTransformedValue {
  const SILInstruction &i;
  std::function<SILValue(const Operand *)> transformFn;
  bool skipTypeDependentOps;

  OperandToTransformedValue(
      const SILInstruction &i,
      std::function<SILValue(const Operand *)> transformFn,
      bool skipTypeDependentOps)
      : i(i), transformFn(transformFn),
        skipTypeDependentOps(skipTypeDependentOps) {}

  Optional<SILValue> operator()(const Operand &use) const {
    if (skipTypeDependentOps && i.isTypeDependentOperand(use))
      return None;
    return transformFn(&use);
  }
};

inline SILInstruction::OperandValueRange
SILInstruction::getOperandValues(ArrayRef<Operand*> operands) {
  return OperandValueRange(operands, OperandToValue());
}

inline auto
SILInstruction::getOperandValues() const -> OperandRefValueRange {
  return OperandRefValueRange(getAllOperands(), OperandRefToValue());
}

inline auto
SILInstruction::getNonTypeDependentOperandValues() const
  -> NonTypeDependentOperandValueRange {
  return NonTypeDependentOperandValueRange(getAllOperands(),
                                           NonTypeDependentOperandToValue(*this));
}

inline auto SILInstruction::getOperandValues(
    std::function<SILValue(const Operand *)> transformFn,
    bool skipTypeDependentOperands) const -> TransformedOperandValueRange {
  return TransformedOperandValueRange(
      getAllOperands(),
      OperandToTransformedValue(*this, transformFn, skipTypeDependentOperands));
}

struct SILInstruction::OperandToType {
  const SILInstruction &i;

  OperandToType(const SILInstruction &i) : i(i) {}

  Optional<SILType> operator()(const Operand &use) const {
    if (i.isTypeDependentOperand(use))
      return None;
    return use.get()->getType();
  }
};

inline auto SILInstruction::getOperandTypes() const -> OperandTypeRange {
  return OperandTypeRange(getAllOperands(), OperandToType(*this));
}

inline llvm::raw_ostream &operator<<(llvm::raw_ostream &OS,
                                     const SILInstruction &I) {
  I.print(OS);
  return OS;
}

/// Returns the combined behavior of \p B1 and \p B2.
inline MemoryBehavior
combineMemoryBehavior(MemoryBehavior B1,
                  MemoryBehavior B2) {
  // Basically the combined behavior is the maximum of both operands.
  auto Result = std::max(B1, B2);

  // With one exception: MayRead, MayWrite -> MayReadWrite.
  if (Result == MemoryBehavior::MayWrite &&
        (B1 == MemoryBehavior::MayRead ||
         B2 == MemoryBehavior::MayRead))
    return MemoryBehavior::MayReadWrite;
  return Result;
}

/// Pretty-print the MemoryBehavior.
llvm::raw_ostream &operator<<(llvm::raw_ostream &OS,
                              MemoryBehavior B);
/// Pretty-print the ReleasingBehavior.
llvm::raw_ostream &operator<<(llvm::raw_ostream &OS,
                              SILInstruction::ReleasingBehavior B);

/// An instruction which always produces a single value.
///
/// Because this instruction is both a SILInstruction and a ValueBase,
/// both of which inherit from SILNode, it introduces the need for
/// some care when working with SILNodes.  See the comment on SILNode.
class SingleValueInstruction : public SILInstruction, public ValueBase {
  friend class SILInstruction;
  friend struct SILNodeOffsetChecker;

  SILInstructionResultArray getResultsImpl() const {
    return SILInstructionResultArray(this);
  }
public:
  SingleValueInstruction(SILInstructionKind kind, SILDebugLocation loc,
                         SILType type)
        : SILInstruction(), ValueBase(ValueKind(kind), type) {
    setDebugLocation(loc);
  }

  using SILInstruction::operator new;
  using SILInstruction::dumpInContext;
  using SILInstruction::print;
  using SILInstruction::printInContext;

  // Redeclare because lldb currently doesn't know about using-declarations
  void dump() const;
  SILFunction *getFunction() const { return SILInstruction::getFunction(); }
  SILModule &getModule() const { return SILInstruction::getModule(); }
  SILInstructionKind getKind() const {
    return (SILInstructionKind)ValueBase::getKind();
  }

  void operator delete(void *Ptr, size_t) = delete;

  ValueKind getValueKind() const {
    return ValueBase::getKind();
  }

  SingleValueInstruction *clone(SILInstruction *insertPt = nullptr) {
    return cast<SingleValueInstruction>(SILInstruction::clone(insertPt));
  }

  /// Override this to reflect the more efficient access pattern.
  SILInstructionResultArray getResults() const { return getResultsImpl(); }

  static bool classof(SILNodePointer node) {
    return node->getKind() >= SILNodeKind::First_SingleValueInstruction &&
           node->getKind() <= SILNodeKind::Last_SingleValueInstruction;
  }

  SILInstruction *getPreviousInstruction() {
    return SILInstruction::getPreviousInstruction();
  }

  SILInstruction *getNextInstruction() {
    return SILInstruction::getNextInstruction();
  }
};

struct SILNodeOffsetChecker {
  static_assert(offsetof(SingleValueInstruction, kind) ==
                offsetof(NonSingleValueInstruction, kind),
                "wrong SILNode layout in SILInstruction");
};

inline SILNodePointer::SILNodePointer(const SingleValueInstruction *svi) :
  node(svi) { }

// Resolve SILInstruction vs SILNode ambiguities.
inline llvm::raw_ostream &operator<<(llvm::raw_ostream &OS,
                                     const NonSingleValueInstruction &I) {
  cast<SILInstruction>(&I)->print(OS);
  return OS;
}
inline llvm::raw_ostream &operator<<(llvm::raw_ostream &OS,
                                     const SingleValueInstruction &I) {
  cast<SILInstruction>(&I)->print(OS);
  return OS;
}

#define DEFINE_ABSTRACT_SINGLE_VALUE_INST_BOILERPLATE(ID)                      \
  static bool classof(SILNodePointer node) {                                   \
    return node->getKind() >= SILNodeKind::First_##ID &&                       \
           node->getKind() <= SILNodeKind::Last_##ID;                          \
  }

/// Abstract base class which defines the source and destination operand numbers
/// for copy-like instructions, like store, assign, copy_addr and cast
/// instructions.
class CopyLikeInstruction {
public:
  enum {
    /// The source operand index.
    Src,
    /// The destination operand index.
    Dest
  };
};

/// Abstract base class used for isa checks on instructions to determine if they
/// forward ownership and to verify that the set of ownership instructions and
/// the ownership utilities stay in sync via assertions.
///
/// NOTE: We assume that the constructor for the instruction subclass that
/// initializes the kind field on our object is run before our constructor runs.
class ForwardingInstruction {
  ValueOwnershipKind ownershipKind;
  bool preservesOwnershipFlag;

protected:
  ForwardingInstruction(SILInstructionKind kind,
                        ValueOwnershipKind ownershipKind,
                        bool preservesOwnership = true)
      : ownershipKind(ownershipKind),
        preservesOwnershipFlag(preservesOwnership) {
    assert(isa(kind) && "Invalid subclass?!");
    assert(ownershipKind && "invalid forwarding ownership");
    assert((preservesOwnershipFlag
            || ownershipKind != OwnershipKind::Guaranteed) &&
           "Non directly forwarding instructions can not forward guaranteed "
           "ownership");
  }

public:
  /// A forwarding instruction preserved ownership if it has a
  /// dynamically non-trivial result in which all references are forwarded from
  /// the operand.
  ///
  /// A cast can only forward guaranteed values if it preserves ownership. Such
  /// casts cannot release any references within their operand's value and
  /// cannot retain any references owned by their result.
  bool preservesOwnership() const { return preservesOwnershipFlag; }

  /// Forwarding ownership is determined by the forwarding instruction's
  /// constant ownership attribute. If forwarding ownership is owned, then the
  /// instruction moves an owned operand to its result, ending its lifetime. If
  /// forwarding ownership is guaranteed, then the instruction propagates the
  /// lifetime of its borrows operand through its result.
  ///
  /// The resulting forwarded value's ownership, returned by getOwnershipKind(),
  /// is not identical to the forwarding ownership. It differs when the result
  /// is trivial type. e.g. an owned or guaranteed value can be cast to a
  /// trivial type using owned or guaranteed forwarding.
  ValueOwnershipKind getForwardingOwnershipKind() const {
    return ownershipKind;
  }
  void setForwardingOwnershipKind(ValueOwnershipKind newKind) {
    assert((preservesOwnership() || newKind != OwnershipKind::Guaranteed) &&
           "Non directly forwarding instructions can not forward guaranteed "
           "ownership");
    ownershipKind = newKind;
  }

  static bool canForwardAllOperands(SILInstruction *inst) {
    switch (inst->getKind()) {
    case SILInstructionKind::StructInst:
    case SILInstructionKind::TupleInst:
    case SILInstructionKind::LinearFunctionInst:
    case SILInstructionKind::DifferentiableFunctionInst:
      return true;
    default:
      return false;
    }
  }

  static bool canForwardFirstOperandOnly(SILInstruction *inst) {
    if (!ForwardingInstruction::isa(inst)) {
      return false;
    }
    return !canForwardAllOperands(inst);
  }

  static bool canForwardOwnedCompatibleValuesOnly(SILInstruction *inst) {
    switch (inst->getKind()) {
    case SILInstructionKind::MarkUninitializedInst:
      return true;
    default:
      return false;
    }
  }

  static bool canForwardGuaranteedCompatibleValuesOnly(SILInstruction *inst) {
    switch (inst->getKind()) {
    case SILInstructionKind::TupleExtractInst:
    case SILInstructionKind::StructExtractInst:
    case SILInstructionKind::DifferentiableFunctionExtractInst:
    case SILInstructionKind::LinearFunctionExtractInst:
      return true;
    default:
      return false;
    }
  }

  /// Defined inline below due to forward declaration issues.
  static ForwardingInstruction *get(SILInstruction *inst);
  static bool isa(SILInstructionKind kind);
  static bool isa(const SILInstruction *inst) { return isa(inst->getKind()); }
  static bool isa(SILNodePointer node) {
    if (auto *i = dyn_cast<const SILInstruction>(node.get()))
      return isa(i);
    return false;
  }

  /// Return true if the forwarded value has the same representation. If true,
  /// then the result can be mapped to the same storage without a move or copy.
  ///
  /// \p inst is an ForwardingInstruction
  static bool hasSameRepresentation(SILInstruction *inst);

  /// Return true if the forwarded value is address-only either before or after
  /// forwarding.
  ///
  /// \p inst is an ForwardingInstruction
  static bool isAddressOnly(SILInstruction *inst);
};

/// A single value inst that forwards a static ownership from its first operand.
///
/// The ownership kind is set on construction and afterwards must be changed
/// explicitly using setOwnershipKind().
///
/// TODO: This name is extremely misleading because it may apply to an
/// operation that has no operand at all, like `enum .None`.
class OwnershipForwardingSingleValueInstruction : public SingleValueInstruction,
                                                  public ForwardingInstruction {
protected:
  OwnershipForwardingSingleValueInstruction(SILInstructionKind kind,
                                            SILDebugLocation debugLoc,
                                            SILType ty,
                                            ValueOwnershipKind ownershipKind)
      : SingleValueInstruction(kind, debugLoc, ty),
        ForwardingInstruction(kind, ownershipKind) {
    assert(classof(kind) && "classof missing new subclass?!");
  }

public:
  static bool classof(SILNodePointer node) {
    if (auto *i = dyn_cast<SILInstruction>(node.get()))
      return classof(i);
    return false;
  }

  static bool classof(SILInstructionKind kind);

  static bool classof(const SILInstruction *inst) {
    return classof(inst->getKind());
  }
};

/// A value base result of a multiple value instruction.
///
/// *NOTE* We want this to be a pure abstract class that does not add /any/ size
/// to subclasses.
class MultipleValueInstructionResult : public ValueBase {
  USE_SHARED_UINT8;
  USE_SHARED_UINT32;

  /// Return the parent instruction of this result.
  MultipleValueInstruction *getParentImpl() const;

  /// Set the index of this result.
  void setIndex(unsigned NewIndex);

public:
  /// Create a new multiple value instruction result.
  ///
  /// \arg subclassDeltaOffset This is the delta offset in our parent object's
  /// layout in between the end of the MultipleValueInstruction object and the
  /// end of the specific subclass object.
  ///
  /// *NOTE* subclassDeltaOffset must be use only 5 bits. This gives us to
  /// support subclasses up to 32 bytes in size. We can scavenge up to 6 more
  /// bits from ValueBase if this is not large enough.
  MultipleValueInstructionResult(unsigned index, SILType type,
                                 ValueOwnershipKind ownershipKind);

  template <class Inst = MultipleValueInstruction>
  Inst *getParent() const { return cast<Inst>(getParentImpl()); }

  unsigned getIndex() const {
    return sharedUInt32().MultipleValueInstructionResult.index;
  }

  /// Get the ownership kind assigned to this result by its parent.
  ///
  /// This is stored in the bottom 3 bits of ValueBase's subclass data.
  ValueOwnershipKind getOwnershipKind() const;

  /// Set the ownership kind assigned to this result.
  ///
  /// This is stored in SILNode in the subclass data.
  void setOwnershipKind(ValueOwnershipKind Kind);

  /// Returns true if this is the token result of a begin_apply.
  bool isBeginApplyToken() const;

  static bool classof(const SILInstruction *) { return false; }
  static bool classof(const SILUndef *) = delete;
  static bool classof(const SILArgument *) = delete;
  static bool classof(const MultipleValueInstructionResult *) { return true; }
  static bool classof(SILNodePointer node) {
    return node->getKind() == SILNodeKind::MultipleValueInstructionResult;
  }
};

/// Returns \p val as MultipleValueInstructionResult if \p val is a result of
/// a MultipleValueInstruction \p Inst, or null if this is not the case.
template <class Inst>
MultipleValueInstructionResult *isaResultOf(SILValue val) {
  if (auto *result = dyn_cast<MultipleValueInstructionResult>(val)) {
    if (isa<Inst>(result->getParent()))
      return result;
  }
  return nullptr;
}

/// Returns \p val as MultipleValueInstructionResult if \p val is a result of
/// a MultipleValueInstruction \p Inst.
template <class Inst>
MultipleValueInstructionResult *getAsResultOf(SILValue val) {
  auto *result = cast<MultipleValueInstructionResult>(val);
  assert(result->getParent<Inst>());
  return result;
}

template <class Result>
SILInstructionResultArray::SILInstructionResultArray(ArrayRef<Result> results)
  : SILInstructionResultArray(
        ArrayRef<MultipleValueInstructionResult>(results.data(),
                                                 results.size())) {
  static_assert(sizeof(Result) == sizeof(MultipleValueInstructionResult),
                "MultipleValueInstructionResult subclass has wrong size");
}

/// An instruction that may produce an arbitrary number of values.
class MultipleValueInstruction : public NonSingleValueInstruction {
  friend class SILInstruction;
  friend class SILInstructionResultArray;

protected:
  MultipleValueInstruction(SILInstructionKind kind, SILDebugLocation loc)
      : NonSingleValueInstruction(kind, loc) {}

public:
  void operator delete(void *Ptr, size_t) = delete;

  MultipleValueInstruction *clone(SILInstruction *insertPt = nullptr) {
    return cast<MultipleValueInstruction>(SILInstruction::clone(insertPt));
  }

  SILValue getResult(unsigned Index) const { return getResults()[Index]; }

  /// Return the index of \p Target if it is a result in the given
  /// MultipleValueInstructionResult. Otherwise, returns None.
  Optional<unsigned> getIndexOfResult(SILValue Target) const;

  unsigned getNumResults() const { return getResults().size(); }

  static bool classof(SILNodePointer node) {
    SILNodeKind kind = node->getKind();
    return kind >= SILNodeKind::First_MultipleValueInstruction &&
           kind <= SILNodeKind::Last_MultipleValueInstruction;
  }
};

template <typename...> class InitialTrailingObjects;
template <typename...> class FinalTrailingObjects;

/// A utility mixin class that must be used by /all/ subclasses of
/// MultipleValueInstruction to store their results.
///
/// The exact ordering of trailing types matters quite a lot because
/// it's vital that the fields used by preceding numTrailingObjects
/// implementations be initialized before this base class is (and
/// conversely that this base class be initialized before any of the
/// succeeding numTrailingObjects implementations are called).
template <typename Derived,
          typename Init = InitialTrailingObjects<>,
          typename Final = FinalTrailingObjects<>>
class MultipleValueInstructionTrailingObjects;

template <typename Derived,
          typename... InitialOtherTrailingTypes,
          typename... FinalOtherTrailingTypes>
class MultipleValueInstructionTrailingObjects<Derived,
                      InitialTrailingObjects<InitialOtherTrailingTypes...>,
                      FinalTrailingObjects<FinalOtherTrailingTypes...>>
    : protected llvm::TrailingObjects<Derived,
                                      InitialOtherTrailingTypes...,
                                      MultipleValueInstruction *,
                                      MultipleValueInstructionResult,
                                      FinalOtherTrailingTypes...> {
protected:
  using TrailingObjects =
      llvm::TrailingObjects<Derived,
                            InitialOtherTrailingTypes...,
                            MultipleValueInstruction *,
                            MultipleValueInstructionResult,
                            FinalOtherTrailingTypes...>;
  friend TrailingObjects;

  using TrailingObjects::totalSizeToAlloc;
  using TrailingObjects::getTrailingObjects;

  unsigned NumResults;

  size_t numTrailingObjects(typename TrailingObjects::template OverloadToken<
                            MultipleValueInstruction *>) const {
    return 1;
  }

  size_t numTrailingObjects(typename TrailingObjects::template
                          OverloadToken<MultipleValueInstructionResult>) const {
    return NumResults;
  }

  template <typename... Args>
  MultipleValueInstructionTrailingObjects(
      Derived *Parent, ArrayRef<SILType> Types,
      ArrayRef<ValueOwnershipKind> OwnershipKinds, Args &&... OtherArgs)
      : NumResults(Types.size()) {

    // If we do not have any results, then we do not need to initialize even the
    // parent pointer since we do not have any results that will attempt to get
    // our parent pointer.
    if (!NumResults)
      return;

    auto **ParentPtr = this->TrailingObjects::template
        getTrailingObjects<MultipleValueInstruction *>();
    *ParentPtr = static_cast<MultipleValueInstruction *>(Parent);

    auto *DataPtr = this->TrailingObjects::template
        getTrailingObjects<MultipleValueInstructionResult>();
    for (unsigned i : range(NumResults)) {
      ::new (&DataPtr[i]) MultipleValueInstructionResult(i, Types[i],
                          OwnershipKinds[i], std::forward<Args>(OtherArgs)...);
      assert(DataPtr[i].getParent() == Parent &&
             "Failed to setup parent reference correctly?!");
    }
  }

  // Destruct the Derived Results.
  ~MultipleValueInstructionTrailingObjects() {
    if (!NumResults)
      return;
    auto *DataPtr = this->TrailingObjects::template
        getTrailingObjects<MultipleValueInstructionResult>();
    // We call the MultipleValueInstructionResult destructors to ensure that:
    //
    // 1. If our derived results have any stored data that need to be cleaned
    // up, we clean them up. *NOTE* Today, no results have this property.
    // 2. In ~ValueBase, we validate via an assert that a ValueBase no longer
    // has any uses when it is being destroyed. Rather than re-implement that in
    // result, we get that for free.
    for (unsigned i : range(NumResults))
      DataPtr[i].~MultipleValueInstructionResult();
  }

public:
  ArrayRef<MultipleValueInstructionResult> getAllResultsBuffer() const {
    auto *ptr = this->TrailingObjects::template
        getTrailingObjects<MultipleValueInstructionResult>();
    return { ptr, NumResults };
  }

  MutableArrayRef<MultipleValueInstructionResult> getAllResultsBuffer() {
    auto *ptr = this->TrailingObjects::template
        getTrailingObjects<MultipleValueInstructionResult>();
    return { ptr, NumResults };
  }

  SILInstructionResultArray getAllResults() const {
    // Our results start at element 1 since we stash the pointer to our parent
    // MultipleValueInstruction in the 0 elt slot. This allows all
    // MultipleValueInstructionResult to find their parent
    // MultipleValueInstruction by using pointer arithmetic.
    return SILInstructionResultArray(getAllResultsBuffer());
  };
};

/// A subclass of SILInstruction which does not produce any values.
class NonValueInstruction : public NonSingleValueInstruction {
public:
  NonValueInstruction(SILInstructionKind kind, SILDebugLocation loc)
    : NonSingleValueInstruction(kind, loc) {}

  /// Doesn't produce any results.
  SILType getType() const = delete;
  SILInstructionResultArray getResults() const = delete;

  static bool classof(const ValueBase *value) = delete;
  static bool classof(SILNodePointer node) {
    return node->getKind() >= SILNodeKind::First_NonValueInstruction &&
           node->getKind() <= SILNodeKind::Last_NonValueInstruction;
  }
  static bool classof(const NonValueInstruction *) { return true; }
};
#define DEFINE_ABSTRACT_NON_VALUE_INST_BOILERPLATE(ID)          \
  static bool classof(const ValueBase *value) = delete;         \
  static bool classof(SILNodePointer node) {                    \
    return node->getKind() >= SILNodeKind::First_##ID &&        \
           node->getKind() <= SILNodeKind::Last_##ID;           \
  }

/// A helper class for defining some basic boilerplate.
template <SILInstructionKind Kind, typename InstBase,
          bool IsSingleResult =
              std::is_base_of<SingleValueInstruction, InstBase>::value>
class InstructionBase;

template <SILInstructionKind Kind, typename InstBase>
class InstructionBase<Kind, InstBase, /*HasResult*/ true> : public InstBase {
protected:
  template <typename... As>
  InstructionBase(As &&... args) : InstBase(Kind, std::forward<As>(args)...) {}

public:
  /// Override to statically return the kind.
  static constexpr SILInstructionKind getKind() {
    return Kind;
  }

  static bool classof(SILNodePointer node) {
    return node->getKind() == SILNodeKind(Kind);
  }
};

template <SILInstructionKind Kind, typename InstBase>
class InstructionBase<Kind, InstBase, /*HasResult*/ false> : public InstBase {
protected:
  template <typename... As>
  InstructionBase(As &&... args) : InstBase(Kind, std::forward<As>(args)...) {}

public:
  static constexpr SILInstructionKind getKind() {
    return Kind;
  }

  /// Can never dynamically succeed.
  static bool classof(const ValueBase *value) = delete;

  static bool classof(SILNodePointer node) {
    return node->getKind() == SILNodeKind(Kind);
  }
};

/// A template base class for instructions that take a single SILValue operand.
template<SILInstructionKind Kind, typename Base>
class UnaryInstructionBase : public InstructionBase<Kind, Base> {
  // Space for 1 operand.
  FixedOperandList<1> Operands;

public:
  template <typename... A>
  UnaryInstructionBase(SILDebugLocation loc, SILValue op, A &&... args)
      : InstructionBase<Kind, Base>(loc, std::forward<A>(args)...),
        Operands(this, op) {}

  SILValue getOperand() const { return Operands[0].get(); }
  void setOperand(SILValue V) { Operands[0].set(V); }

  Operand &getOperandRef() { return Operands[0]; }

  const Operand &getOperandRef() const { return Operands[0]; }

  ArrayRef<Operand> getAllOperands() const { return Operands.asArray(); }
  MutableArrayRef<Operand> getAllOperands() { return Operands.asArray(); }

  ArrayRef<Operand> getTypeDependentOperands() const {
    return {};
  }

  MutableArrayRef<Operand> getTypeDependentOperands() {
    return {};
  }
};

/// A template base class for instructions that a variable number of SILValue
/// operands, and has zero or one value results. The operands are tail allocated
/// after the instruction. Further trailing data can be allocated as well if
/// OtherTrailingTypes are provided.
template<SILInstructionKind Kind,
         typename Derived,
         typename Base,
         typename... OtherTrailingTypes>
class InstructionBaseWithTrailingOperands
    : public InstructionBase<Kind, Base>,
      protected llvm::TrailingObjects<Derived, Operand, OtherTrailingTypes...> {

protected:
  TEMPLATE_USE_SHARED_UINT32(Base);

  friend llvm::TrailingObjects<Derived, Operand, OtherTrailingTypes...>;

  using TrailingObjects =
      llvm::TrailingObjects<Derived, Operand, OtherTrailingTypes...>;

  using TrailingObjects::totalSizeToAlloc;

public:
  template <typename... Args>
  InstructionBaseWithTrailingOperands(ArrayRef<SILValue> Operands,
                                      Args &&...args)
        : InstructionBase<Kind, Base>(std::forward<Args>(args)...) {
    sharedUInt32().InstructionBaseWithTrailingOperands.numOperands =
        Operands.size();
    TrailingOperandsList::InitOperandsList(getAllOperands().begin(), this,
                                           Operands);
  }

  template <typename... Args>
  InstructionBaseWithTrailingOperands(SILValue Operand0,
                                      ArrayRef<SILValue> Operands,
                                      Args &&...args)
        : InstructionBase<Kind, Base>(std::forward<Args>(args)...) {
    sharedUInt32().InstructionBaseWithTrailingOperands.numOperands =
        Operands.size() + 1;
    TrailingOperandsList::InitOperandsList(getAllOperands().begin(), this,
                                           Operand0, Operands);
  }

  template <typename... Args>
  InstructionBaseWithTrailingOperands(SILValue Operand0,
                                      SILValue Operand1,
                                      ArrayRef<SILValue> Operands,
                                      Args &&...args)
        : InstructionBase<Kind, Base>(std::forward<Args>(args)...) {
    sharedUInt32().InstructionBaseWithTrailingOperands.numOperands =
        Operands.size() + 2;
    TrailingOperandsList::InitOperandsList(getAllOperands().begin(), this,
                                           Operand0, Operand1, Operands);
  }

  // Destruct tail allocated objects.
  ~InstructionBaseWithTrailingOperands() {
    Operand *Operands = TrailingObjects::template getTrailingObjects<Operand>();
    auto end = sharedUInt32().InstructionBaseWithTrailingOperands.numOperands;
    for (unsigned i = 0; i < end; ++i) {
      Operands[i].~Operand();
    }
  }

  size_t numTrailingObjects(typename TrailingObjects::template
                            OverloadToken<Operand>) const {
    return sharedUInt32().InstructionBaseWithTrailingOperands.numOperands;
  }

  ArrayRef<Operand> getAllOperands() const {
    return {TrailingObjects::template getTrailingObjects<Operand>(),
            sharedUInt32().InstructionBaseWithTrailingOperands.numOperands};
  }

  MutableArrayRef<Operand> getAllOperands() {
    return {TrailingObjects::template getTrailingObjects<Operand>(),
            sharedUInt32().InstructionBaseWithTrailingOperands.numOperands};
  }
};

/// A template base class for instructions that take no operands except
/// for type-dependent operands. The operands are tail allocated after the
/// instruction. Further trailing data can be allocated as well if
/// TRAILING_TYPES are provided.
template<SILInstructionKind Kind,
         typename Derived,
         typename Base,
         typename... OtherTrailingTypes>
class NullaryInstructionWithTypeDependentOperandsBase
    : public InstructionBaseWithTrailingOperands<Kind, Derived, Base,
                                                 OtherTrailingTypes...> {
protected:
  friend InstructionBaseWithTrailingOperands<Kind, Derived, Operand,
                                             OtherTrailingTypes...>;

  using TrailingObjects =
      InstructionBaseWithTrailingOperands<Kind, Derived, Operand,
                                          OtherTrailingTypes...>;

  template <typename... Args>
  NullaryInstructionWithTypeDependentOperandsBase(SILDebugLocation debugLoc,
                                    ArrayRef<SILValue> typeDependentOperands,
                                    Args &&...args)
      : InstructionBaseWithTrailingOperands<Kind, Derived, Base,
                                            OtherTrailingTypes...>(
                                              typeDependentOperands,
                                              debugLoc,
                                              std::forward<Args>(args)...) {}

public:
  unsigned getNumTypeDependentOperands() const {
    return this->getAllOperands().size();
  }

  ArrayRef<Operand> getTypeDependentOperands() const {
    return this->getAllOperands();
  }

  MutableArrayRef<Operand> getTypeDependentOperands() {
    return this->getAllOperands();
  }
};

/// A template base class for instructions that take a single regular SILValue
/// operand, a set of type dependent operands and has no result
/// or a single value result. The operands are tail allocated after the
/// instruction. Further trailing data can be allocated as well if
/// TRAILING_TYPES are provided.
template<SILInstructionKind Kind,
         typename Derived,
         typename Base,
         typename... OtherTrailingTypes>
class UnaryInstructionWithTypeDependentOperandsBase
    : public InstructionBaseWithTrailingOperands<Kind, Derived, Base,
                                                 OtherTrailingTypes...> {
protected:
  friend InstructionBaseWithTrailingOperands<Kind, Derived, Operand,
                                             OtherTrailingTypes...>;

  using TrailingObjects =
      InstructionBaseWithTrailingOperands<Kind, Derived, Operand,
                                          OtherTrailingTypes...>;

public:
  template <typename... Args>
  UnaryInstructionWithTypeDependentOperandsBase(SILDebugLocation debugLoc,
                                       SILValue operand,
                                       ArrayRef<SILValue> typeDependentOperands,
                                       Args &&...args)
      : InstructionBaseWithTrailingOperands<Kind, Derived, Base,
                                            OtherTrailingTypes...>(
                                              operand, typeDependentOperands,
                                              debugLoc,
                                              std::forward<Args>(args)...) {}

  unsigned getNumTypeDependentOperands() const {
    return this->getAllOperands().size() - 1;
  }

  SILValue getOperand() const {
    return this->getAllOperands()[0].get();
  }
  void setOperand(SILValue V) {
    this->getAllOperands()[0].set(V);
  }

  Operand &getOperandRef() {
    return this->getAllOperands()[0];
  }

  const Operand &getOperandRef() const {
    return this->getAllOperands()[0];
  }

  ArrayRef<Operand> getTypeDependentOperands() const {
    return this->getAllOperands().slice(1);
  }

  MutableArrayRef<Operand> getTypeDependentOperands() {
    return this->getAllOperands().slice(1);
  }
};

/// A DebugVariable where storage for the strings has been
/// tail-allocated following the parent SILInstruction.
class TailAllocatedDebugVariable {
  using int_type = uint32_t;
  union {
    int_type RawValue;
    struct {
      /// Whether this is a debug variable at all.
      int_type HasValue : 1;
      /// True if this is a let-binding.
      int_type Constant : 1;
      /// True if this variable is created by compiler
      int_type Implicit : 1;
      /// When this is nonzero there is a tail-allocated string storing
      /// variable name present. This typically only happens for
      /// instructions that were created from parsing SIL assembler.
      int_type NameLength : 13;
      /// The source function argument position from left to right
      /// starting with 1 or 0 if this is a local variable.
      int_type ArgNo : 16;
    } Data;
  } Bits;
public:
  TailAllocatedDebugVariable(Optional<SILDebugVariable>, char *buf,
                             SILType *AuxVarType = nullptr,
                             SILLocation *DeclLoc = nullptr,
                             const SILDebugScope **DeclScope = nullptr,
                             SILDIExprElement *DIExprOps = nullptr);
  TailAllocatedDebugVariable(int_type RawValue) { Bits.RawValue = RawValue; }
  int_type getRawValue() const { return Bits.RawValue; }

  unsigned getArgNo() const { return Bits.Data.ArgNo; }
  void setArgNo(unsigned N) { Bits.Data.ArgNo = N; }
  /// Returns the name of the source variable, if it is stored in the
  /// instruction.
  StringRef getName(const char *buf) const;
  bool isLet() const { return Bits.Data.Constant; }

  bool isImplicit() const { return Bits.Data.Implicit; }
  void setImplicit(bool V = true) { Bits.Data.Implicit = V; }

  Optional<SILDebugVariable>
  get(VarDecl *VD, const char *buf, Optional<SILType> AuxVarType = {},
      Optional<SILLocation> DeclLoc = {},
      const SILDebugScope *DeclScope = nullptr,
      llvm::ArrayRef<SILDIExprElement> DIExprElements = {}) const {
    if (!Bits.Data.HasValue)
      return None;

    StringRef name = getName(buf);
    if (VD && name.empty())
      name = VD->getName().str();
    return SILDebugVariable(name, isLet(), getArgNo(), isImplicit(), AuxVarType,
                            DeclLoc, DeclScope, DIExprElements);
  }
};
static_assert(sizeof(TailAllocatedDebugVariable) == 4,
              "SILNode inline bitfield needs updating");

/// Used for keeping track of advanced / supplement debug variable info
/// stored in trailing objects space inside debug instructions (e.g.
/// debug_value)
class SILDebugVariableSupplement {
protected:
  enum SourceLocKind : unsigned { SLK_Loc = 0b01, SLK_Scope = 0b10 };

  unsigned NumDIExprOperands : 8;

  unsigned HasAuxDebugVariableType : 1;

  unsigned AuxVariableSourceLoc : 2;

  SILDebugVariableSupplement(unsigned NumDIExprOps, bool AuxType, bool AuxLoc,
                             bool AuxScope)
      : NumDIExprOperands(NumDIExprOps), HasAuxDebugVariableType(AuxType),
        AuxVariableSourceLoc((AuxLoc ? SLK_Loc : 0) |
                             (AuxScope ? SLK_Scope : 0)) {}
};

#define SIL_DEBUG_VAR_SUPPLEMENT_TRAILING_OBJS_IMPL()                          \
  inline bool hasAuxDebugLocation() const {                                    \
    return AuxVariableSourceLoc & SLK_Loc;                                     \
  }                                                                            \
  inline bool hasAuxDebugScope() const {                                       \
    return AuxVariableSourceLoc & SLK_Scope;                                   \
  }                                                                            \
                                                                               \
  size_t numTrailingObjects(OverloadToken<SILType>) const {                    \
    return HasAuxDebugVariableType ? 1 : 0;                                    \
  }                                                                            \
                                                                               \
  size_t numTrailingObjects(OverloadToken<SILLocation>) const {                \
    return hasAuxDebugLocation() ? 1 : 0;                                      \
  }                                                                            \
                                                                               \
  size_t numTrailingObjects(OverloadToken<const SILDebugScope *>) const {      \
    return hasAuxDebugScope() ? 1 : 0;                                         \
  }                                                                            \
                                                                               \
  size_t numTrailingObjects(OverloadToken<SILDIExprElement>) const {           \
    return NumDIExprOperands;                                                  \
  }

//===----------------------------------------------------------------------===//
// Allocation Instructions
//===----------------------------------------------------------------------===//

/// Abstract base class for allocation instructions, like alloc_stack, alloc_box
/// and alloc_ref, etc.
class AllocationInst : public SingleValueInstruction {
protected:
  AllocationInst(SILInstructionKind Kind, SILDebugLocation DebugLoc, SILType Ty)
      : SingleValueInstruction(Kind, DebugLoc, Ty) {}

public:
  DEFINE_ABSTRACT_SINGLE_VALUE_INST_BOILERPLATE(AllocationInst)

  /// Return the underlying variable declaration associated with this
  /// allocation, or null if this allocation inst is associated with a temporary
  /// allocation.
  VarDecl *getDecl() const;
};

class DeallocStackInst;

/// AllocStackInst - This represents the allocation of an unboxed (i.e., no
/// reference count) stack memory.  The memory is provided uninitialized.
class AllocStackInst final
    : public InstructionBase<SILInstructionKind::AllocStackInst,
                             AllocationInst>,
      private SILDebugVariableSupplement,
      private llvm::TrailingObjects<AllocStackInst, SILType, SILLocation,
                                    const SILDebugScope *, SILDIExprElement,
                                    Operand, char> {
  friend TrailingObjects;
  friend SILBuilder;

  TailAllocatedDebugVariable VarInfo;
  USE_SHARED_UINT8;
  USE_SHARED_UINT32;

  AllocStackInst(SILDebugLocation Loc, SILType elementType,
                 ArrayRef<SILValue> TypeDependentOperands, SILFunction &F,
                 Optional<SILDebugVariable> Var, bool hasDynamicLifetime,
                 bool isLexical, bool usesMoveableValueDebugInfo);

  static AllocStackInst *create(SILDebugLocation Loc, SILType elementType,
                                SILFunction &F, Optional<SILDebugVariable> Var,
                                bool hasDynamicLifetime, bool isLexical,
                                bool usesMoveableValueDebugInfo);

  SIL_DEBUG_VAR_SUPPLEMENT_TRAILING_OBJS_IMPL()

  size_t numTrailingObjects(OverloadToken<Operand>) const {
    return sharedUInt32().AllocStackInst.numOperands;
  }

public:
  ~AllocStackInst() {
    Operand *Operands = getTrailingObjects<Operand>();
    size_t end = sharedUInt32().AllocStackInst.numOperands;
    for (unsigned i = 0; i < end; ++i) {
      Operands[i].~Operand();
    }
  }

  void markUsesMoveableValueDebugInfo() {
    sharedUInt8().AllocStackInst.usesMoveableValueDebugInfo = true;
  }

  /// Set to true if this alloc_stack's memory location was passed to _move at
  /// any point of the program.
  bool getUsesMoveableValueDebugInfo() const {
    return sharedUInt8().AllocStackInst.usesMoveableValueDebugInfo;
  }

  /// Set to true that this alloc_stack contains a value whose lifetime can not
  /// be ascertained from uses.
  ///
  /// As an example if an alloc_stack is known to be only conditionally
  /// initialized.
  void setDynamicLifetime() { sharedUInt8().AllocStackInst.dynamicLifetime = true; }

  /// Returns true if the alloc_stack's initialization can not be ascertained
  /// from uses directly (so should be treated conservatively).
  ///
  /// An example of an alloc_stack with dynamic lifetime is an alloc_stack that
  /// is conditionally initialized.
  bool hasDynamicLifetime() const { return sharedUInt8().AllocStackInst.dynamicLifetime; }

  /// Whether the alloc_stack instruction corresponds to a source-level VarDecl.
  bool isLexical() const { return sharedUInt8().AllocStackInst.lexical; }

  /// If this is a lexical alloc_stack, eliminate the lexical bit. If this
  /// alloc_stack doesn't have a lexical bit, do not do anything.
  void removeIsLexical() { sharedUInt8().AllocStackInst.lexical = false; }

  /// If this is not a lexical alloc_stack, set the lexical bit. If this
  /// alloc_stack is already lexical, this does nothing.
  void setIsLexical() { sharedUInt8().AllocStackInst.lexical = true; }

  /// Return the debug variable information attached to this instruction.
  Optional<SILDebugVariable> getVarInfo() const {
    // If we used to have debug info attached but our debug info is now
    // invalidated, just bail.
    if (sharedUInt8().AllocStackInst.hasInvalidatedVarInfo) {
      return None;
    }

    Optional<SILType> AuxVarType;
    Optional<SILLocation> VarDeclLoc;
    const SILDebugScope *VarDeclScope = nullptr;
    if (HasAuxDebugVariableType)
      AuxVarType = *getTrailingObjects<SILType>();

    if (hasAuxDebugLocation())
      VarDeclLoc = *getTrailingObjects<SILLocation>();
    if (hasAuxDebugScope())
      VarDeclScope = *getTrailingObjects<const SILDebugScope *>();

    llvm::ArrayRef<SILDIExprElement> DIExprElements(
        getTrailingObjects<SILDIExprElement>(), NumDIExprOperands);

    return VarInfo.get(getDecl(), getTrailingObjects<char>(), AuxVarType,
                       VarDeclLoc, VarDeclScope, DIExprElements);
  }

  /// True if this AllocStack has var info that a pass purposely invalidated.
  ///
  /// NOTE:
  ///
  /// 1. We don't print this state. It is just a way to invalidate the debug
  /// info. When we parse back in whatever we printed, we will parse it without
  /// debug var info since none will be printed.
  ///
  /// 2. Since we do not serialize debug info today, we do not need to serialize
  /// this state.
  ///
  /// TODO: If we begin serializing debug info, we will need to begin
  /// serializing this!
  bool isVarInfoInvalidated() const {
    return sharedUInt8().AllocStackInst.hasInvalidatedVarInfo;
  }

  /// Invalidate the debug info in an alloc_stack. This is useful in cases where
  /// we one is merging alloc_stack and wants to split the debug info on an
  /// alloc_stack into a separate debug_value instruction from the merged
  /// alloc_stack.
  void invalidateVarInfo() {
    sharedUInt8().AllocStackInst.hasInvalidatedVarInfo = true;
  }

  bool isLet() const {
    if (auto varInfo = getVarInfo())
      return varInfo->isLet();
    return false;
  }

  bool isVar() const {
    if (auto varInfo = getVarInfo())
      return varInfo->isVar();
    return false;
  }

  void setArgNo(unsigned N) { VarInfo.setArgNo(N); }

  void setDebugVarScope(const SILDebugScope *NewDS) {
    if (hasAuxDebugScope())
      *getTrailingObjects<const SILDebugScope *>() = NewDS;
  }

  /// getElementType - Get the type of the allocated memory (as opposed to the
  /// type of the instruction itself, which will be an address type).
  SILType getElementType() const {
    return getType().getObjectType();
  }

  ArrayRef<Operand> getAllOperands() const {
    return { getTrailingObjects<Operand>(),
             static_cast<size_t>(sharedUInt32().AllocStackInst.numOperands) };
  }

  MutableArrayRef<Operand> getAllOperands() {
    return { getTrailingObjects<Operand>(),
             static_cast<size_t>(sharedUInt32().AllocStackInst.numOperands) };
  }

  ArrayRef<Operand> getTypeDependentOperands() const {
    return getAllOperands();
  }

  MutableArrayRef<Operand> getTypeDependentOperands() {
    return getAllOperands();
  }

  /// Return a single dealloc_stack user or null.
  DeallocStackInst *getSingleDeallocStack() const;
};

/// AllocPackInst - This represents the allocation of a value pack
/// in stack memory.  The memory is provided uninitialized.
class AllocPackInst final
    : public NullaryInstructionWithTypeDependentOperandsBase<
                  SILInstructionKind::AllocPackInst,
                  AllocPackInst,
                  AllocationInst> {
  friend TrailingObjects;
  friend SILBuilder;

  AllocPackInst(SILDebugLocation loc, SILType resultType,
                ArrayRef<SILValue> typeDependentOperands)
    : NullaryInstructionWithTypeDependentOperandsBase(loc,
                                                      typeDependentOperands,
                                                      resultType) {}

  static AllocPackInst *create(SILDebugLocation loc, SILType packType,
                               SILFunction &F);
public:
  /// Return the allocated pack type.  The result type of the instruction
  /// is an address of this type.
  CanSILPackType getPackType() const {
    return getType().castTo<SILPackType>();
  }
};

/// AllocPackMetadataInst - Marker instruction indicating that the next
///                         instruction might allocate on-stack pack metadata
///                         during IRGen.
///
/// Only valid in lowered SIL.
class AllocPackMetadataInst final
    : public NullaryInstructionWithTypeDependentOperandsBase<
          SILInstructionKind::AllocPackMetadataInst, AllocPackMetadataInst,
          AllocationInst> {
  friend SILBuilder;

  AllocPackMetadataInst(SILDebugLocation loc, SILType elementType)
      : NullaryInstructionWithTypeDependentOperandsBase(
            loc, {}, elementType.getAddressType()) {}

public:
  /// The instruction which may trigger on-stack pack metadata when IRGen
  /// lowering.
  SILInstruction *getIntroducer() { return getNextInstruction(); }
};

/// The base class for AllocRefInst and AllocRefDynamicInst.
///
/// The first NumTailTypes operands are counts for the tail allocated
/// elements, the remaining operands are opened archetype operands.
class AllocRefInstBase : public AllocationInst {
protected:
  USE_SHARED_UINT8;

  AllocRefInstBase(SILInstructionKind Kind,
                   SILDebugLocation DebugLoc,
                   SILType ObjectType,
                   bool objc, bool canBeOnStack,
                   ArrayRef<SILType> ElementTypes);

  SILType *getTypeStorage();
  const SILType *getTypeStorage() const {
    return const_cast<AllocRefInstBase*>(this)->getTypeStorage();
  }

public:
  unsigned getNumTailTypes() const {
    return sharedUInt8().AllocRefInstBase.numTailTypes;
  }

  bool canAllocOnStack() const {
    return sharedUInt8().AllocRefInstBase.onStack;
  }

  void setStackAllocatable(bool OnStack = true) {
    sharedUInt8().AllocRefInstBase.onStack = OnStack;
  }

  ArrayRef<SILType> getTailAllocatedTypes() const {
    return {getTypeStorage(), getNumTailTypes()};
  }

  MutableArrayRef<SILType> getTailAllocatedTypes() {
    return {getTypeStorage(), getNumTailTypes()};
  }
  
  ArrayRef<Operand> getTailAllocatedCounts() const {
    return getAllOperands().slice(0, getNumTailTypes());
  }

  MutableArrayRef<Operand> getTailAllocatedCounts() {
    return getAllOperands().slice(0, getNumTailTypes());
  }

  ArrayRef<Operand> getAllOperands() const;
  MutableArrayRef<Operand> getAllOperands();
  
  /// Whether to use Objective-C's allocation mechanism (+allocWithZone:).
  bool isObjC() const { return sharedUInt8().AllocRefInstBase.objC; }

  static bool classof(SILNodePointer node) {
    if (auto *i = dyn_cast<SILInstruction>(node.get()))
      return classof(i);
    return false;
  }

  static bool classof(const SILInstruction *inst) {
    return classof(inst->getKind());
  }

  static bool classof(SILInstructionKind kind) {
    switch (kind) {
    case SILInstructionKind::AllocRefInst:
    case SILInstructionKind::AllocRefDynamicInst:
      return true;
    default:
      return false;
    }
  }
};

/// AllocRefInst - This represents the primitive allocation of an instance
/// of a reference type. Aside from the reference count, the instance is
/// returned uninitialized.
/// Optionally, the allocated instance contains space for one or more tail-
/// allocated arrays.
class AllocRefInst final
    : public InstructionBaseWithTrailingOperands<
                                               SILInstructionKind::AllocRefInst,
                                               AllocRefInst,
                                               AllocRefInstBase, SILType> {
  friend AllocRefInstBase;
  friend SILBuilder;

  AllocRefInst(SILDebugLocation DebugLoc, SILFunction &F,
               SILType ObjectType,
               bool objc, bool canBeOnStack,
               ArrayRef<SILType> ElementTypes,
               ArrayRef<SILValue> AllOperands)
      : InstructionBaseWithTrailingOperands(AllOperands, DebugLoc, ObjectType,
                        objc, canBeOnStack, ElementTypes) {
    assert(AllOperands.size() >= ElementTypes.size());
    std::uninitialized_copy(ElementTypes.begin(), ElementTypes.end(),
                            getTrailingObjects<SILType>());
  }

  static AllocRefInst *create(SILDebugLocation DebugLoc, SILFunction &F,
                              SILType ObjectType,
                              bool objc, bool canBeOnStack,
                              ArrayRef<SILType> ElementTypes,
                              ArrayRef<SILValue> ElementCountOperands);

public:
  ArrayRef<Operand> getTypeDependentOperands() const {
    return getAllOperands().slice(getNumTailTypes());
  }

  MutableArrayRef<Operand> getTypeDependentOperands() {
    return getAllOperands().slice(getNumTailTypes());
  }
};

/// AllocRefDynamicInst - This represents the primitive allocation of
/// an instance of a reference type whose runtime type is provided by
/// the given metatype value. Aside from the reference count, the
/// instance is returned uninitialized.
/// Optionally, the allocated instance contains space for one or more tail-
/// allocated arrays.
class AllocRefDynamicInst final
    : public InstructionBaseWithTrailingOperands<
                                        SILInstructionKind::AllocRefDynamicInst,
                                        AllocRefDynamicInst,
                                        AllocRefInstBase, SILType> {
  friend AllocRefInstBase;
  friend SILBuilder;

  AllocRefDynamicInst(SILDebugLocation DebugLoc,
                      SILType ty,
                      bool objc,
                      bool canBeOnStack,
                      ArrayRef<SILType> ElementTypes,
                      ArrayRef<SILValue> AllOperands)
      : InstructionBaseWithTrailingOperands(AllOperands, DebugLoc, ty, objc,
                                            canBeOnStack, ElementTypes) {
    assert(AllOperands.size() >= ElementTypes.size() + 1);
    std::uninitialized_copy(ElementTypes.begin(), ElementTypes.end(),
                            getTrailingObjects<SILType>());
  }

  static AllocRefDynamicInst *
  create(SILDebugLocation DebugLoc, SILFunction &F,
         SILValue metatypeOperand, SILType ty, bool objc,
         bool canBeOnStack,
         ArrayRef<SILType> ElementTypes,
         ArrayRef<SILValue> ElementCountOperands);

public:
  SILValue getMetatypeOperand() const {
    return getAllOperands()[getNumTailTypes()].get();
  }

  ArrayRef<Operand> getTypeDependentOperands() const {
    return getAllOperands().slice(getNumTailTypes() + 1);
  }

  MutableArrayRef<Operand> getTypeDependentOperands() {
    return getAllOperands().slice(getNumTailTypes() + 1);
  }
  // Is the deinit and the size of the dynamic type known to be equivalent to
  // the base type (i.e `this->getType()`).
  bool isDynamicTypeDeinitAndSizeKnownEquivalentToBaseType() const;
};

/// This represents the allocation of a heap box for a Swift value of some type.
/// The instruction returns two values.  The first return value is the object
/// pointer with Builtin.NativeObject type.  The second return value
/// is an address pointing to the contained element. The contained
/// element is uninitialized.
class AllocBoxInst final
    : public NullaryInstructionWithTypeDependentOperandsBase<
                                           SILInstructionKind::AllocBoxInst,
                                           AllocBoxInst, AllocationInst, char>
{
  friend SILBuilder;

  TailAllocatedDebugVariable VarInfo;

  USE_SHARED_UINT8;

  AllocBoxInst(SILDebugLocation DebugLoc, CanSILBoxType BoxType,
               ArrayRef<SILValue> TypeDependentOperands, SILFunction &F,
               Optional<SILDebugVariable> Var, bool hasDynamicLifetime,
               bool reflection = false, bool usesMoveableValueDebugInfo = false,
               bool hasPointerEscape = false);

  static AllocBoxInst *create(SILDebugLocation Loc, CanSILBoxType boxType,
                              SILFunction &F, Optional<SILDebugVariable> Var,
                              bool hasDynamicLifetime, bool reflection = false,
                              bool usesMoveableValueDebugInfo = false,
                              bool hasPointerEscape = false);

public:
  CanSILBoxType getBoxType() const {
    return getType().castTo<SILBoxType>();
  }

  void setDynamicLifetime() {
    sharedUInt8().AllocBoxInst.dynamicLifetime = true;
  }

  bool hasDynamicLifetime() const {
    return sharedUInt8().AllocBoxInst.dynamicLifetime;
  }

  void setHasPointerEscape(bool pointerEscape) {
    sharedUInt8().AllocBoxInst.pointerEscape = pointerEscape;
  }

  bool hasPointerEscape() const {
    return sharedUInt8().AllocBoxInst.pointerEscape;
  }

  /// True if the box should be emitted with reflection metadata for its
  /// contents.
  bool emitReflectionMetadata() const {
    return sharedUInt8().AllocBoxInst.reflection;
  }

  // Return the type of the memory stored in the alloc_box.
  SILType getAddressType() const;

  /// Return the debug variable information attached to this instruction.
  Optional<SILDebugVariable> getVarInfo() const {
    return VarInfo.get(getDecl(), getTrailingObjects<char>());
  };

  void setUsesMoveableValueDebugInfo() {
    sharedUInt8().AllocBoxInst.usesMoveableValueDebugInfo = true;
  }

  bool getUsesMoveableValueDebugInfo() const {
    return sharedUInt8().AllocBoxInst.usesMoveableValueDebugInfo;
  }
};

/// This represents the allocation of a heap box for an existential container.
/// The instruction returns two values.  The first return value is the owner
/// pointer, which has the existential type.  The second return value
/// is an address pointing to the contained element. The contained
/// value is uninitialized.
class AllocExistentialBoxInst final
    : public NullaryInstructionWithTypeDependentOperandsBase<
                                   SILInstructionKind::AllocExistentialBoxInst,
                                   AllocExistentialBoxInst, AllocationInst> {
  friend SILBuilder;
  CanType ConcreteType;
  ArrayRef<ProtocolConformanceRef> Conformances;

  AllocExistentialBoxInst(SILDebugLocation DebugLoc, SILType ExistentialType,
                          CanType ConcreteType,
                          ArrayRef<ProtocolConformanceRef> Conformances,
                          ArrayRef<SILValue> TypeDependentOperands,
                          SILFunction *Parent)
    : NullaryInstructionWithTypeDependentOperandsBase(DebugLoc,
                                          TypeDependentOperands,
                                          ExistentialType.getObjectType()),
      ConcreteType(ConcreteType), Conformances(Conformances) {}

  static AllocExistentialBoxInst *
  create(SILDebugLocation DebugLoc, SILType ExistentialType,
         CanType ConcreteType, ArrayRef<ProtocolConformanceRef> Conformances,
         SILFunction *Parent);

public:
  CanType getFormalConcreteType() const { return ConcreteType; }

  SILType getExistentialType() const { return getType(); }

  ArrayRef<ProtocolConformanceRef> getConformances() const {
    return Conformances;
  }
};

/// GenericSpecializationInformation - provides information about a generic
/// specialization. This meta-information is created for each generic
/// specialization, which allows for tracking of dependencies between
/// specialized generic functions and can be used to detect specialization loops
/// during generic specialization.
class GenericSpecializationInformation {
  /// The caller function that triggered this specialization.
  SILFunction *Caller;
  /// The original function that was specialized.
  SILFunction *Parent;
  /// Substitutions used to produce this specialization.
  SubstitutionMap Subs;

  GenericSpecializationInformation(SILFunction *Caller, SILFunction *Parent,
                                   SubstitutionMap Subs);

public:
  static const GenericSpecializationInformation *create(SILFunction *Caller,
                                                        SILFunction *Parent,
                                                        SubstitutionMap Subs);
  static const GenericSpecializationInformation *create(SILInstruction *Inst,
                                                        SILBuilder &B);
  const SILFunction *getCaller() const { return Caller; }
  const SILFunction *getParent() const { return Parent; }
  SubstitutionMap getSubstitutions() const { return Subs; }
};

class PartialApplyInst;

// There's no good reason for the OverloadToken type to be internal
// or protected, and it makes it very difficult to write our CRTP classes
// if it is, so pull it out.  TODO: just fix LLVM.
struct TerribleOverloadTokenHack :
    llvm::trailing_objects_internal::TrailingObjectsBase {
  template <class T>
  using Hack = OverloadToken<T>;
};
template <class T>
using OverloadToken = TerribleOverloadTokenHack::Hack<T>;

enum class ApplyFlags : uint8_t {
  /// This is a call to a 'rethrows' function that is known not to throw.
  DoesNotThrow = 0x1,

  /// This is a call to a 'reasync' function that is known not to 'await'.
  DoesNotAwait = 0x2
};

using ApplyOptions = OptionSet<ApplyFlags>;

/// ApplyInstBase - An abstract class for different kinds of function
/// application.
template <class Impl, class Base,
          bool IsFullApply = !std::is_same<Impl, PartialApplyInst>::value>
class ApplyInstBase;

// The partial specialization for non-full applies.  Note that the
// partial specialization for full applies inherits from this.
template <class Impl, class Base>
class ApplyInstBase<Impl, Base, false> : public Base {
  enum { Callee, NumStaticOperands };

  /// The type of the callee with our substitutions applied.
  SILType SubstCalleeType;

  /// Information about specialization and inlining of this apply.
  /// This is only != nullptr if the apply was inlined. And in this case it
  /// points to the specialization info of the inlined function.
  const GenericSpecializationInformation *SpecializationInfo;

  /// Stores an ApplyOptions.
  unsigned Options: 2;

  /// The number of call arguments as required by the callee.
  unsigned NumCallArguments : 30;

  /// The total number of type-dependent operands.
  unsigned NumTypeDependentOperands;

  /// The substitutions being applied to the callee.
  SubstitutionMap Substitutions;

  Impl &asImpl() { return static_cast<Impl &>(*this); }
  const Impl &asImpl() const { return static_cast<const Impl &>(*this); }

protected:
  template <class... As>
  ApplyInstBase(SILInstructionKind kind, SILDebugLocation DebugLoc, SILValue callee,
                SILType substCalleeType, SubstitutionMap subs,
                ArrayRef<SILValue> args,
                ArrayRef<SILValue> typeDependentOperands,
                const GenericSpecializationInformation *specializationInfo,
                As... baseArgs)
      : Base(kind, DebugLoc, baseArgs...), SubstCalleeType(substCalleeType),
        SpecializationInfo(specializationInfo),
        NumCallArguments(args.size()),
        NumTypeDependentOperands(typeDependentOperands.size()),
        Substitutions(subs) {

    // Initialize the operands.
    auto allOperands = getAllOperands();
    new (&allOperands[Callee]) Operand(this, callee);
    for (size_t i : indices(args)) {
      new (&allOperands[NumStaticOperands + i]) Operand(this, args[i]);
    }
    for (size_t i : indices(typeDependentOperands)) {
      new (&allOperands[NumStaticOperands + args.size() + i])
        Operand(this, typeDependentOperands[i]);
    }
  }

  ~ApplyInstBase() {
    for (auto &operand : getAllOperands())
      operand.~Operand();
  }

  template <class, class...>
  friend class llvm::TrailingObjects;

  unsigned numTrailingObjects(OverloadToken<Operand>) const {
    return getNumAllOperands();
  }

  static size_t getNumAllOperands(ArrayRef<SILValue> args,
                                  ArrayRef<SILValue> typeDependentOperands) {
    return NumStaticOperands + args.size() + typeDependentOperands.size();
  }
  
public:
  void setApplyOptions(ApplyOptions options) {
    Options = unsigned(options.toRaw());
  }
  
  ApplyOptions getApplyOptions() const {
    return ApplyOptions(ApplyFlags(Options));
  }

  bool isNonThrowing() const {
    return getApplyOptions().contains(ApplyFlags::DoesNotThrow);
  }
  
  bool isNonAsync() const {
    return getApplyOptions().contains(ApplyFlags::DoesNotAwait);
  }

  /// The operand number of the first argument.
  static unsigned getArgumentOperandNumber() { return NumStaticOperands; }

  Operand *getCalleeOperand() { return &getAllOperands()[Callee]; }
  const Operand *getCalleeOperand() const { return &getAllOperands()[Callee]; }
  SILValue getCallee() const { return getCalleeOperand()->get(); }

  /// Gets the origin of the callee by looking through function type conversions
  /// until we find a function_ref, partial_apply, or unrecognized value.
  ///
  /// This is defined out of line to work around incomplete definition
  /// issues. It is at the bottom of the file.
  SILValue getCalleeOrigin() const;

  /// Gets the referenced function by looking through partial apply,
  /// convert_function, and thin to thick function until we find a function_ref.
  ///
  /// This is defined out of line to work around incomplete definition
  /// issues. It is at the bottom of the file.
  SILFunction *getCalleeFunction() const;

  bool isCalleeDynamicallyReplaceable() const;

  /// Gets the referenced function if the callee is a function_ref instruction.
  /// Returns null if the callee is dynamic or a (prev_)dynamic_function_ref
  /// instruction.
  SILFunction *getReferencedFunctionOrNull() const {
    if (auto *FRI = dyn_cast<FunctionRefBaseInst>(getCallee()))
      return FRI->getReferencedFunctionOrNull();
    return nullptr;
  }

  /// Return the referenced function if the callee is a function_ref like
  /// instruction.
  ///
  /// WARNING: This not necessarily the function that will be called at runtime.
  /// If the callee is a (prev_)dynamic_function_ref the actual function called
  /// might be different because it could be dynamically replaced at runtime.
  ///
  /// If the client of this API wants to look at the content of the returned SIL
  /// function it should call getReferencedFunctionOrNull() instead.
  SILFunction *getInitiallyReferencedFunction() const {
    if (auto *FRI = dyn_cast<FunctionRefBaseInst>(getCallee()))
      return FRI->getInitiallyReferencedFunction();
    return nullptr;
  }

  /// Get the type of the callee without the applied substitutions.
  CanSILFunctionType getOrigCalleeType() const {
    return getCallee()->getType().template castTo<SILFunctionType>();
  }
  SILFunctionConventions getOrigCalleeConv() const {
    return SILFunctionConventions(getOrigCalleeType(), this->getModule());
  }

  /// Get the type of the callee with the applied substitutions.
  CanSILFunctionType getSubstCalleeType() const {
    return SubstCalleeType.castTo<SILFunctionType>();
  }
  SILType getSubstCalleeSILType() const {
    return SubstCalleeType;
  }
  
  void setSubstCalleeType(CanSILFunctionType t) {
    SubstCalleeType = SILType::getPrimitiveObjectType(t);
  }
  
  SILFunctionConventions getSubstCalleeConv() const {
    return SILFunctionConventions(getSubstCalleeType(), this->getModule());
  }

  bool isCalleeNoReturn() const {
    return getSubstCalleeSILType().isNoReturnFunction(
        this->getModule(), TypeExpansionContext(*this->getFunction()));
  }

  bool isCalleeThin() const {
    auto Rep = getSubstCalleeType()->getRepresentation();
    return Rep == FunctionType::Representation::Thin;
  }

  /// Returns true if the callee function is annotated with
  /// @_semantics("programtermination_point")
  bool isCalleeKnownProgramTerminationPoint() const {
    auto calleeFn = getCalleeFunction();
    if (!calleeFn) return false;
    return calleeFn->hasSemanticsAttr(SEMANTICS_PROGRAMTERMINATION_POINT);
  }

  /// Returns true if the callee function is annotated with
  /// @_semantics("unavailable_code_reached")
  bool isCalleeUnavailableCodeReached() const {
    auto calleeFn = getCalleeFunction();
    if (!calleeFn) return false;
    return calleeFn->hasSemanticsAttr(SEMANTICS_UNAVAILABLE_CODE_REACHED);
  }

  /// True if this application has generic substitutions.
  bool hasSubstitutions() const {
    return Substitutions.hasAnySubstitutableParams();
  }

  /// The substitutions used to bind the generic arguments of this function.
  SubstitutionMap getSubstitutionMap() const { return Substitutions; }

  /// Return the total number of operands of this instruction.
  unsigned getNumAllOperands() const {
    return NumStaticOperands + NumCallArguments + NumTypeDependentOperands;
  }

  /// Return all the operands of this instruction, which are (in order):
  ///   - the callee
  ///   - the formal arguments
  ///   - the type-dependency arguments
  MutableArrayRef<Operand> getAllOperands() {
    return { asImpl().template getTrailingObjects<Operand>(),
             getNumAllOperands() };
  }

  ArrayRef<Operand> getAllOperands() const {
    return { asImpl().template getTrailingObjects<Operand>(),
             getNumAllOperands() };
  }

  /// Check whether the given operand index is a call-argument index
  /// and, if so, return that index.
  Optional<unsigned> getArgumentIndexForOperandIndex(unsigned index) {
    assert(index < getNumAllOperands());
    if (index < NumStaticOperands) return None;
    index -= NumStaticOperands;
    if (index >= NumCallArguments) return None;
    return index;
  }

  /// The arguments passed to this instruction.
  MutableArrayRef<Operand> getArgumentOperands() {
    return getAllOperands().slice(NumStaticOperands, NumCallArguments);
  }

  ArrayRef<Operand> getArgumentOperands() const {
    return getAllOperands().slice(NumStaticOperands, NumCallArguments);
  }

  /// The arguments passed to this instruction.
  OperandValueArrayRef getArguments() const {
    return OperandValueArrayRef(getArgumentOperands());
  }

  /// Returns the number of arguments being passed by this apply.
  /// If this is a partial_apply, it can be less than the number of
  /// parameters.
  unsigned getNumArguments() const { return NumCallArguments; }

  Operand &getArgumentRef(unsigned i) {
    return getArgumentOperands()[i];
  }

  /// Return the ith argument passed to this instruction.
  SILValue getArgument(unsigned i) const { return getArguments()[i]; }

  /// Set the ith argument of this instruction.
  void setArgument(unsigned i, SILValue V) {
    return getArgumentOperands()[i].set(V);
  }

  ArrayRef<Operand> getTypeDependentOperands() const {
    return getAllOperands().slice(NumStaticOperands + NumCallArguments);
  }

  MutableArrayRef<Operand> getTypeDependentOperands() {
    return getAllOperands().slice(NumStaticOperands + NumCallArguments);
  }

  const GenericSpecializationInformation *getSpecializationInfo() const {
    return SpecializationInfo;
  }
};

/// Given the callee operand of an apply or try_apply instruction,
/// does it have the given semantics?
bool doesApplyCalleeHaveSemantics(SILValue callee, StringRef semantics);

/// Predicate used to filter InoutArgumentRange.
struct OperandToInoutArgument {
  ArrayRef<SILParameterInfo> paramInfos;
  OperandValueArrayRef arguments;
  OperandToInoutArgument(ArrayRef<SILParameterInfo> paramInfos,
                         OperandValueArrayRef arguments)
      : paramInfos(paramInfos), arguments(arguments) {
    assert(paramInfos.size() == arguments.size());
  }
  Optional<SILValue> operator()(size_t i) const {
    if (paramInfos[i].isIndirectMutating())
      return arguments[i];
    return None;
  }
};

using InoutArgumentRange =
    OptionalTransformRange<IntRange<size_t>, OperandToInoutArgument>;

/// The partial specialization of ApplyInstBase for full applications.
/// Adds some methods relating to 'self' and to result types that don't
/// make sense for partial applications.
template <class Impl, class Base>
class ApplyInstBase<Impl, Base, true>
  : public ApplyInstBase<Impl, Base, false> {
  using super = ApplyInstBase<Impl, Base, false>;
protected:
  template <class... As>
  ApplyInstBase(As &&...args)
    : ApplyInstBase<Impl, Base, false>(std::forward<As>(args)...) {}

private:
  const Impl &asImpl() const { return static_cast<const Impl &>(*this); }

public:
  using super::getCallee;
  using super::getSubstCalleeType;
  using super::getSubstCalleeConv;
  using super::hasSubstitutions;
  using super::getNumArguments;
  using super::getArgument;
  using super::getArguments;
  using super::getArgumentOperands;

  /// The collection of following routines wrap the representation difference in
  /// between the self substitution being first, but the self parameter of a
  /// function being last.
  ///
  /// The hope is that this will prevent any future bugs from coming up related
  /// to this.
  ///
  /// Self is always the last parameter, but self substitutions are always
  /// first. The reason to add this method is to wrap that dichotomy to reduce
  /// errors.
  ///
  /// FIXME: Could this be standardized? It has and will lead to bugs. IMHO.
  SILValue getSelfArgument() const {
    assert(hasSelfArgument() && "Must have a self argument");
    assert(getNumArguments() && "Should only be called when Callee has "
           "arguments.");
    return getArgument(getNumArguments()-1);
  }

  Operand &getSelfArgumentOperand() {
    assert(hasSelfArgument() && "Must have a self argument");
    assert(getNumArguments() && "Should only be called when Callee has "
           "arguments.");
    return getArgumentOperands()[getNumArguments()-1];
  }

  void setSelfArgument(SILValue V) {
    assert(hasSelfArgument() && "Must have a self argument");
    assert(getNumArguments() && "Should only be called when Callee has "
                                      "arguments.");
    getArgumentOperands()[getNumArguments() - 1].set(V);
  }

  OperandValueArrayRef getArgumentsWithoutSelf() const {
    assert(hasSelfArgument() && "Must have a self argument");
    assert(getNumArguments() && "Should only be called when Callee has "
           "at least a self parameter.");
    ArrayRef<Operand> ops = this->getArgumentOperands();
    ArrayRef<Operand> opsWithoutSelf = ArrayRef<Operand>(&ops[0],
                                                         ops.size()-1);
    return OperandValueArrayRef(opsWithoutSelf);
  }

  Optional<SILResultInfo> getSingleResult() const {
    auto SubstCallee = getSubstCalleeType();
    if (SubstCallee->getNumResults() != 1)
      return None;
    return SubstCallee->getSingleResult();
  }

  bool hasIndirectResults() const {
    return getSubstCalleeConv().hasIndirectSILResults();
  }
  unsigned getNumIndirectResults() const {
    return getSubstCalleeConv().getNumIndirectSILResults();
  }

  bool hasSelfArgument() const {
    return getSubstCalleeType()->hasSelfParam();
  }

  bool hasGuaranteedSelfArgument() const {
    auto C = getSubstCalleeType()->getSelfParameter().getConvention();
    return C == ParameterConvention::Direct_Guaranteed;
  }

  OperandValueArrayRef getIndirectSILResults() const {
    return getArguments().slice(0, getNumIndirectResults());
  }

  OperandValueArrayRef getArgumentsWithoutIndirectResults() const {
    return getArguments().slice(getNumIndirectResults());
  }

  /// Returns all `@inout` and `@inout_aliasable` arguments passed to the
  /// instruction.
  InoutArgumentRange getInoutArguments() const {
    auto &impl = asImpl();
    return InoutArgumentRange(
        indices(getArgumentsWithoutIndirectResults()),
        OperandToInoutArgument(impl.getSubstCalleeConv().getParameters(),
                               impl.getArgumentsWithoutIndirectResults()));
  }

  bool hasSemantics(StringRef semanticsString) const {
    return doesApplyCalleeHaveSemantics(getCallee(), semanticsString);
  }
};

/// ApplyInst - Represents the full application of a function value.
class ApplyInst final
    : public InstructionBase<SILInstructionKind::ApplyInst,
                             ApplyInstBase<ApplyInst, SingleValueInstruction>>,
      public llvm::TrailingObjects<ApplyInst, Operand> {
  friend SILBuilder;

  ApplyInst(SILDebugLocation DebugLoc, SILValue Callee,
            SILType SubstCalleeType, SILType ReturnType,
            SubstitutionMap Substitutions,
            ArrayRef<SILValue> Args,
            ArrayRef<SILValue> TypeDependentOperands,
            ApplyOptions options,
            const GenericSpecializationInformation *SpecializationInfo);

  static ApplyInst *
  create(SILDebugLocation DebugLoc, SILValue Callee,
         SubstitutionMap Substitutions, ArrayRef<SILValue> Args,
         ApplyOptions options,
         Optional<SILModuleConventions> ModuleConventions,
         SILFunction &F,
         const GenericSpecializationInformation *SpecializationInfo);
};

/// PartialApplyInst - Represents the creation of a closure object by partial
/// application of a function value.
class PartialApplyInst final
    : public InstructionBase<SILInstructionKind::PartialApplyInst,
                             ApplyInstBase<PartialApplyInst,
                                           SingleValueInstruction>>,
      public llvm::TrailingObjects<PartialApplyInst, Operand> {
  friend SILBuilder;

public:
  enum OnStackKind {
    NotOnStack, OnStack
  };

private:
  PartialApplyInst(SILDebugLocation DebugLoc, SILValue Callee,
                   SILType SubstCalleeType,
                   SubstitutionMap Substitutions,
                   ArrayRef<SILValue> Args,
                   ArrayRef<SILValue> TypeDependentOperands,
                   SILType ClosureType,
                   const GenericSpecializationInformation *SpecializationInfo);

  static PartialApplyInst *
  create(SILDebugLocation DebugLoc, SILValue Callee, ArrayRef<SILValue> Args,
         SubstitutionMap Substitutions, ParameterConvention CalleeConvention,
         SILFunction &F,
         const GenericSpecializationInformation *SpecializationInfo,
         OnStackKind onStack);

public:
  /// Return the result function type of this partial apply.
  CanSILFunctionType getFunctionType() const {
    return getType().castTo<SILFunctionType>();
  }
  bool hasCalleeGuaranteedContext() const {
    return getType().castTo<SILFunctionType>()->isCalleeGuaranteed();
  }

  OnStackKind isOnStack() const {
    return getFunctionType()->isNoEscape() ? OnStack : NotOnStack;
  }
  
  /// Visit the instructions that end the lifetime of an OSSA on-stack closure.
  bool visitOnStackLifetimeEnds(llvm::function_ref<bool (Operand*)> func) const;
};

class EndApplyInst;
class AbortApplyInst;

/// BeginApplyInst - Represents the beginning of the full application of
/// a yield_once coroutine (up until the coroutine yields a value back).
class BeginApplyInst final
    : public InstructionBase<SILInstructionKind::BeginApplyInst,
                             ApplyInstBase<BeginApplyInst,
                                           MultipleValueInstruction>>,
      public MultipleValueInstructionTrailingObjects<
          BeginApplyInst,
          // These must be earlier trailing objects because their
          // count fields are initialized by an earlier base class.
          InitialTrailingObjects<Operand>> {
  friend SILBuilder;

  template <class, class...>
  friend class llvm::TrailingObjects;
  using InstructionBase::numTrailingObjects;
  using MultipleValueInstructionTrailingObjects::numTrailingObjects;

  friend class ApplyInstBase<BeginApplyInst, MultipleValueInstruction, false>;
  using MultipleValueInstructionTrailingObjects::getTrailingObjects;

  BeginApplyInst(SILDebugLocation debugLoc, SILValue callee,
                 SILType substCalleeType,
                 ArrayRef<SILType> allResultTypes,
                 ArrayRef<ValueOwnershipKind> allResultOwnerships,
                 SubstitutionMap substitutions,
                 ArrayRef<SILValue> args,
                 ArrayRef<SILValue> typeDependentOperands,
                 ApplyOptions options,
                 const GenericSpecializationInformation *specializationInfo);

  static BeginApplyInst *
  create(SILDebugLocation debugLoc, SILValue Callee,
         SubstitutionMap substitutions, ArrayRef<SILValue> args,
         ApplyOptions options, Optional<SILModuleConventions> moduleConventions,
         SILFunction &F,
         const GenericSpecializationInformation *specializationInfo);

public:
  using MultipleValueInstructionTrailingObjects::totalSizeToAlloc;

  MultipleValueInstructionResult *getTokenResult() const {
    return const_cast<MultipleValueInstructionResult *>(
             &getAllResultsBuffer().back());
  }

  SILInstructionResultArray getYieldedValues() const {
    return getAllResultsBuffer().drop_back();
  }

  void getCoroutineEndPoints(
      SmallVectorImpl<EndApplyInst *> &endApplyInsts,
      SmallVectorImpl<AbortApplyInst *> &abortApplyInsts) const;

  void getCoroutineEndPoints(SmallVectorImpl<Operand *> &endApplyInsts,
                             SmallVectorImpl<Operand *> &abortApplyInsts) const;
};

/// AbortApplyInst - Unwind the full application of a yield_once coroutine.
class AbortApplyInst
    : public UnaryInstructionBase<SILInstructionKind::AbortApplyInst,
                                  NonValueInstruction> {
  friend SILBuilder;

  AbortApplyInst(SILDebugLocation debugLoc, SILValue beginApplyToken)
      : UnaryInstructionBase(debugLoc, beginApplyToken) {
    assert(isaResultOf<BeginApplyInst>(beginApplyToken) &&
           isaResultOf<BeginApplyInst>(beginApplyToken)->isBeginApplyToken());
  }

public:
  MultipleValueInstructionResult *getToken() const {
    return getAsResultOf<BeginApplyInst>(getOperand());
  }

  BeginApplyInst *getBeginApply() const {
    return getToken()->getParent<BeginApplyInst>();
  }
};

/// EndApplyInst - Resume the full application of a yield_once coroutine
/// normally.
class EndApplyInst
    : public UnaryInstructionBase<SILInstructionKind::EndApplyInst,
                                  NonValueInstruction> {
  friend SILBuilder;

  EndApplyInst(SILDebugLocation debugLoc, SILValue beginApplyToken)
      : UnaryInstructionBase(debugLoc, beginApplyToken) {
    assert(isaResultOf<BeginApplyInst>(beginApplyToken) &&
           isaResultOf<BeginApplyInst>(beginApplyToken)->isBeginApplyToken());
  }

public:
  MultipleValueInstructionResult *getToken() const {
    return getAsResultOf<BeginApplyInst>(getOperand());
  }

  BeginApplyInst *getBeginApply() const {
    return getToken()->getParent<BeginApplyInst>();
  }
};

//===----------------------------------------------------------------------===//
// Literal instructions.
//===----------------------------------------------------------------------===//

/// Abstract base class for literal instructions.
class LiteralInst : public SingleValueInstruction {
protected:
  LiteralInst(SILInstructionKind Kind, SILDebugLocation DebugLoc, SILType Ty)
      : SingleValueInstruction(Kind, DebugLoc, Ty) {}

public:

  DEFINE_ABSTRACT_SINGLE_VALUE_INST_BOILERPLATE(LiteralInst)
};

class FunctionRefBaseInst : public LiteralInst {
protected:
  SILFunction *f;

  FunctionRefBaseInst(SILInstructionKind Kind, SILDebugLocation DebugLoc,
                      SILFunction *F, TypeExpansionContext context);

public:
  ~FunctionRefBaseInst();

  /// Return the referenced function if this is a function_ref instruction and
  /// therefore a client can rely on the dynamically called function being equal
  /// to the returned value and null otherwise.
  SILFunction *getReferencedFunctionOrNull() const {
    auto kind = getKind();
    if (kind == SILInstructionKind::FunctionRefInst)
      return f;
    assert(kind == SILInstructionKind::DynamicFunctionRefInst ||
           kind == SILInstructionKind::PreviousDynamicFunctionRefInst);
    return nullptr;
  }

  /// Return the initially referenced function.
  ///
  /// WARNING: This not necessarily the function that will be called at runtime.
  /// If the callee is a (prev_)dynamic_function_ref the actual function called
  /// might be different because it could be dynamically replaced at runtime.
  ///
  /// If the client of this API wants to look at the content of the returned SIL
  /// function it should call getReferencedFunctionOrNull() instead.
  SILFunction *getInitiallyReferencedFunction() const { return f; }

  void dropReferencedFunction();

  CanSILFunctionType getFunctionType() const {
    return getType().castTo<SILFunctionType>();
  }
  SILFunctionConventions getConventions() const {
    return SILFunctionConventions(getFunctionType(), getModule());
  }

  ArrayRef<Operand> getAllOperands() const { return {}; }
  MutableArrayRef<Operand> getAllOperands() { return {}; }

  static bool classof(SILNodePointer node) {
    return (node->getKind() == SILNodeKind::FunctionRefInst ||
        node->getKind() == SILNodeKind::DynamicFunctionRefInst ||
        node->getKind() == SILNodeKind::PreviousDynamicFunctionRefInst);
  }
};

/// FunctionRefInst - Represents a reference to a SIL function.
class FunctionRefInst : public FunctionRefBaseInst {
  friend SILBuilder;

  /// Construct a FunctionRefInst.
  ///
  /// \param DebugLoc  The location of the reference.
  /// \param F         The function being referenced.
  /// \param context   The type expansion context of the function reference.
  FunctionRefInst(SILDebugLocation DebugLoc, SILFunction *F,
                  TypeExpansionContext context);

public:
  /// Return the referenced function.
  SILFunction *getReferencedFunction() const { return f; }

  static bool classof(SILNodePointer node) {
    return node->getKind() == SILNodeKind::FunctionRefInst;
  }
};

class DynamicFunctionRefInst : public FunctionRefBaseInst {
  friend SILBuilder;

  /// Construct a DynamicFunctionRefInst.
  ///
  /// \param DebugLoc  The location of the reference.
  /// \param F         The function being referenced.
  /// \param context   The type expansion context of the function reference.
  DynamicFunctionRefInst(SILDebugLocation DebugLoc, SILFunction *F,
                         TypeExpansionContext context);

public:
  static bool classof(SILNodePointer node) {
    return node->getKind() == SILNodeKind::DynamicFunctionRefInst;
  }
};

class PreviousDynamicFunctionRefInst : public FunctionRefBaseInst {
  friend SILBuilder;

  /// Construct a PreviousDynamicFunctionRefInst.
  ///
  /// \param DebugLoc  The location of the reference.
  /// \param F         The function being referenced.
  /// \param context   The type expansion context of the function reference.
  PreviousDynamicFunctionRefInst(SILDebugLocation DebugLoc, SILFunction *F,
                                 TypeExpansionContext context);

public:
  static bool classof(SILNodePointer node) {
    return node->getKind() == SILNodeKind::PreviousDynamicFunctionRefInst;
  }
};

/// Component of a KeyPathInst.
class KeyPathPatternComponent {
public:
  /// Computed property components require an identifier so they can be stably
  /// identified at runtime. This has to correspond to the ABI of the property--
  /// whether a reabstracted stored property, a property dispatched through a
  /// vtable or witness table, or a computed property.
  class ComputedPropertyId {
    friend KeyPathPatternComponent;
  public:
    enum KindType {
      Property, Function, DeclRef,
    };
  private:
  
    union ValueType {
      AbstractStorageDecl *Property;
      SILFunction *Function;
      SILDeclRef DeclRef;
      
      ValueType() : Property(nullptr) {}
      ValueType(AbstractStorageDecl *p) : Property(p) {}
      ValueType(SILFunction *f) : Function(f) {}
      ValueType(SILDeclRef d) : DeclRef(d) {}
    } Value;
  
    KindType Kind;
    
    explicit ComputedPropertyId(ValueType Value, KindType Kind)
      : Value(Value), Kind(Kind)
    {}
    
  public:
    ComputedPropertyId() : Value(), Kind(Property) {}
  
    /*implicit*/ ComputedPropertyId(VarDecl *property)
      : Value{property}, Kind{Property}
    {
    }
    
    /*implicit*/ ComputedPropertyId(SILFunction *function)
      : Value{function}, Kind{Function}
    {}
    
    /*implicit*/ ComputedPropertyId(SILDeclRef declRef)
      : Value{declRef}, Kind{DeclRef}
    {}
    
    KindType getKind() const { return Kind; }
    
    VarDecl *getProperty() const {
      assert(getKind() == Property);
      return cast<VarDecl>(Value.Property);
    }
    
    SILFunction *getFunction() const {
      assert(getKind() == Function);
      return Value.Function;
    }
    
    SILDeclRef getDeclRef() const {
      assert(getKind() == DeclRef);
      return Value.DeclRef;
    }
  };
  
  enum class Kind: unsigned {
    StoredProperty,
    GettableProperty,
    SettableProperty,
    TupleElement,
    OptionalChain,
    OptionalForce,
    OptionalWrap,
  };
  
  // Description of a captured index value and its Hashable conformance for a
  // subscript keypath.
  struct Index {
    unsigned Operand;
    CanType FormalType;
    SILType LoweredType;
    ProtocolConformanceRef Hashable;
  };
  
private:
  enum PackedKind: unsigned {
    PackedStored,
    PackedComputed,
    Unpacked,
  };
  
  static const unsigned KindPackingBits = 2;
  
  static unsigned getPackedKind(Kind k) {
    switch (k) {
    case Kind::StoredProperty:
    case Kind::TupleElement:
      return PackedStored;
    case Kind::GettableProperty:
    case Kind::SettableProperty:
      return PackedComputed;
    case Kind::OptionalChain:
    case Kind::OptionalForce:
    case Kind::OptionalWrap:
      return Unpacked;
    }
  }
  
  // Value is the VarDecl* for StoredProperty, the SILFunction* of the
  // Getter for computed properties, or the Kind for other kinds
  llvm::PointerIntPair<void *, KindPackingBits, unsigned> ValueAndKind;
  llvm::PointerIntPair<SILFunction *, 2,
                       ComputedPropertyId::KindType> SetterAndIdKind;

  // If this component refers to a tuple element then TupleIndex is the
  // 1-based index of the element in the tuple, in order to allow the
  // discrimination of the TupleElement Kind from the StoredProperty Kind
  union {
    unsigned TupleIndex = 0;
    ComputedPropertyId::ValueType IdValue;
  };

  ArrayRef<Index> Indices;
  struct {
    SILFunction *Equal;
    SILFunction *Hash;
  } IndexEquality;
  CanType ComponentType;
  AbstractStorageDecl *ExternalStorage;
  SubstitutionMap ExternalSubstitutions;

  /// Constructor for stored components
  KeyPathPatternComponent(VarDecl *storedProp,
                          CanType ComponentType)
    : ValueAndKind(storedProp, PackedStored),
      ComponentType(ComponentType) {}

  /// Constructor for computed components
  KeyPathPatternComponent(ComputedPropertyId id,
                          SILFunction *getter,
                          SILFunction *setter,
                          ArrayRef<Index> indices,
                          SILFunction *indicesEqual,
                          SILFunction *indicesHash,
                          AbstractStorageDecl *externalStorage,
                          SubstitutionMap externalSubstitutions,
                          CanType ComponentType)
    : ValueAndKind(getter, PackedComputed),
      SetterAndIdKind{setter, id.Kind},
      IdValue{id.Value},
      Indices(indices),
      IndexEquality{indicesEqual, indicesHash},
      ComponentType(ComponentType),
      ExternalStorage(externalStorage),
      ExternalSubstitutions(externalSubstitutions)
  {
  }
  
  /// Constructor for optional components.
  KeyPathPatternComponent(Kind kind, CanType componentType)
    : ValueAndKind((void*)((uintptr_t)kind << KindPackingBits), Unpacked),
      ComponentType(componentType) {
    assert((unsigned)kind >= (unsigned)Kind::OptionalChain
           && "not an optional component");
  }

  /// Constructor for tuple element.
  KeyPathPatternComponent(unsigned tupleIndex, CanType componentType)
    : ValueAndKind((void*)((uintptr_t)Kind::TupleElement << KindPackingBits), PackedStored),
    TupleIndex(tupleIndex + 1),
    ComponentType(componentType)
  {
  }

public:
  KeyPathPatternComponent() : ValueAndKind(nullptr, 0) {}

  bool isNull() const {
    return ValueAndKind.getPointer() == nullptr;
  }

  Kind getKind() const {
    auto packedKind = ValueAndKind.getInt();
    switch ((PackedKind)packedKind) {
    case PackedStored:
      return TupleIndex
        ? Kind::TupleElement : Kind::StoredProperty;
    case PackedComputed:
      return SetterAndIdKind.getPointer()
        ? Kind::SettableProperty : Kind::GettableProperty;
    case Unpacked:
      return (Kind)((uintptr_t)ValueAndKind.getPointer() >> KindPackingBits);
    }
    llvm_unreachable("unhandled kind");
  }

  CanType getComponentType() const {
    return ComponentType;
  }

  VarDecl *getStoredPropertyDecl() const {
    switch (getKind()) {
    case Kind::StoredProperty:
      return static_cast<VarDecl*>(ValueAndKind.getPointer());
    case Kind::GettableProperty:
    case Kind::SettableProperty:
    case Kind::OptionalChain:
    case Kind::OptionalForce:
    case Kind::OptionalWrap:
    case Kind::TupleElement:
      llvm_unreachable("not a stored property");
    }
    llvm_unreachable("unhandled kind");
  }

  ComputedPropertyId getComputedPropertyId() const {
    switch (getKind()) {
    case Kind::StoredProperty:
    case Kind::OptionalChain:
    case Kind::OptionalForce:
    case Kind::OptionalWrap:
    case Kind::TupleElement:
      llvm_unreachable("not a computed property");
    case Kind::GettableProperty:
    case Kind::SettableProperty:
      return ComputedPropertyId(IdValue,
                                SetterAndIdKind.getInt());
    }
    llvm_unreachable("unhandled kind");
  }

  SILFunction *getComputedPropertyGetter() const {
    switch (getKind()) {
    case Kind::StoredProperty:
    case Kind::OptionalChain:
    case Kind::OptionalForce:
    case Kind::OptionalWrap:
    case Kind::TupleElement:
      llvm_unreachable("not a computed property");
    case Kind::GettableProperty:
    case Kind::SettableProperty:
      return static_cast<SILFunction*>(ValueAndKind.getPointer());
    }
    llvm_unreachable("unhandled kind");
  }

  SILFunction *getComputedPropertySetter() const {
    switch (getKind()) {
    case Kind::StoredProperty:
    case Kind::GettableProperty:
    case Kind::OptionalChain:
    case Kind::OptionalForce:
    case Kind::OptionalWrap:
    case Kind::TupleElement:
      llvm_unreachable("not a settable computed property");
    case Kind::SettableProperty:
      return SetterAndIdKind.getPointer();
    }
    llvm_unreachable("unhandled kind");
  }

  ArrayRef<Index> getSubscriptIndices() const {
    switch (getKind()) {
    case Kind::StoredProperty:
    case Kind::OptionalChain:
    case Kind::OptionalForce:
    case Kind::OptionalWrap:
    case Kind::TupleElement:
      return {};
    case Kind::GettableProperty:
    case Kind::SettableProperty:
      return Indices;
    }
    llvm_unreachable("unhandled kind");
  }

  SILFunction *getSubscriptIndexEquals() const {
    switch (getKind()) {
    case Kind::StoredProperty:
    case Kind::OptionalChain:
    case Kind::OptionalForce:
    case Kind::OptionalWrap:
    case Kind::TupleElement:
      llvm_unreachable("not a computed property");
    case Kind::GettableProperty:
    case Kind::SettableProperty:
      return IndexEquality.Equal;
    }
    llvm_unreachable("unhandled kind");
  }
  SILFunction *getSubscriptIndexHash() const {
    switch (getKind()) {
    case Kind::StoredProperty:
    case Kind::OptionalChain:
    case Kind::OptionalForce:
    case Kind::OptionalWrap:
    case Kind::TupleElement:
      llvm_unreachable("not a computed property");
    case Kind::GettableProperty:
    case Kind::SettableProperty:
      return IndexEquality.Hash;
    }
    llvm_unreachable("unhandled kind");
  }

  bool isComputedSettablePropertyMutating() const;
  
  static KeyPathPatternComponent forStoredProperty(VarDecl *property,
                                                   CanType ty) {
    return KeyPathPatternComponent(property, ty);
  }
  
  AbstractStorageDecl *getExternalDecl() const {
    switch (getKind()) {
    case Kind::StoredProperty:
    case Kind::OptionalChain:
    case Kind::OptionalForce:
    case Kind::OptionalWrap:
    case Kind::TupleElement:
      llvm_unreachable("not a computed property");
    case Kind::GettableProperty:
    case Kind::SettableProperty:
      return ExternalStorage;
    }
    llvm_unreachable("unhandled kind");
  }

  SubstitutionMap getExternalSubstitutions() const {
    switch (getKind()) {
    case Kind::StoredProperty:
    case Kind::OptionalChain:
    case Kind::OptionalForce:
    case Kind::OptionalWrap:
    case Kind::TupleElement:
      llvm_unreachable("not a computed property");
    case Kind::GettableProperty:
    case Kind::SettableProperty:
      return ExternalSubstitutions;
    }
    llvm_unreachable("unhandled kind");
  }
    
  unsigned getTupleIndex() const {
    switch (getKind()) {
    case Kind::StoredProperty:
    case Kind::OptionalChain:
    case Kind::OptionalForce:
    case Kind::OptionalWrap:
    case Kind::GettableProperty:
    case Kind::SettableProperty:
      llvm_unreachable("not a tuple element");
    case Kind::TupleElement:
      return TupleIndex - 1;
    }
    llvm_unreachable("unhandled kind");
  }

  static KeyPathPatternComponent
  forComputedGettableProperty(ComputedPropertyId identifier,
                              SILFunction *getter,
                              ArrayRef<Index> indices,
                              SILFunction *indicesEquals,
                              SILFunction *indicesHash,
                              AbstractStorageDecl *externalDecl,
                              SubstitutionMap externalSubs,
                              CanType ty) {
    return KeyPathPatternComponent(identifier,
                                   getter, nullptr, indices,
                                   indicesEquals, indicesHash,
                                   externalDecl, externalSubs,
                                   ty);
  }

  static KeyPathPatternComponent
  forComputedSettableProperty(ComputedPropertyId identifier,
                              SILFunction *getter,
                              SILFunction *setter,
                              ArrayRef<Index> indices,
                              SILFunction *indicesEquals,
                              SILFunction *indicesHash,
                              AbstractStorageDecl *externalDecl,
                              SubstitutionMap externalSubs,
                              CanType ty) {
    return KeyPathPatternComponent(identifier,
                                   getter, setter, indices,
                                   indicesEquals, indicesHash,
                                   externalDecl, externalSubs,
                                   ty);
  }
  
  static KeyPathPatternComponent
  forOptional(Kind kind, CanType ty) {
    switch (kind) {
    case Kind::OptionalChain:
    case Kind::OptionalForce:
      break;
    case Kind::OptionalWrap:
      assert(ty->getOptionalObjectType() &&
             "optional wrap didn't form optional?!");
      break;
    case Kind::StoredProperty:
    case Kind::GettableProperty:
    case Kind::SettableProperty:
    case Kind::TupleElement:
      llvm_unreachable("not an optional kind");
    }
    return KeyPathPatternComponent(kind, ty);
  }
    
  static KeyPathPatternComponent forTupleElement(unsigned tupleIndex,
                                                 CanType ty) {
    return KeyPathPatternComponent(tupleIndex, ty);
  }
  
  void visitReferencedFunctionsAndMethods(
      std::function<void (SILFunction *)> functionCallBack,
      std::function<void (SILDeclRef)> methodCallBack) const;
    
  void incrementRefCounts() const;
  void decrementRefCounts() const;

  void print(SILPrintContext &ctxt) const;

  void Profile(llvm::FoldingSetNodeID &ID);
};

/// An abstract description of a key path pattern.
class KeyPathPattern final
  : public llvm::FoldingSetNode,
    private llvm::TrailingObjects<KeyPathPattern,
                                  KeyPathPatternComponent>
{
  friend TrailingObjects;

  unsigned NumOperands, NumComponents;
  CanGenericSignature Signature;
  CanType RootType, ValueType;
  StringRef ObjCString;
  
  KeyPathPattern(CanGenericSignature signature,
                 CanType rootType,
                 CanType valueType,
                 ArrayRef<KeyPathPatternComponent> components,
                 StringRef ObjCString,
                 unsigned numOperands);
  
  static KeyPathPattern *create(SILModule &M,
                                CanGenericSignature signature,
                                CanType rootType,
                                CanType valueType,
                                ArrayRef<KeyPathPatternComponent> components,
                                StringRef ObjCString,
                                unsigned numOperands);
public:
  CanGenericSignature getGenericSignature() const {
    return Signature;
  }
  
  CanType getRootType() const {
    return RootType;
  }
  
  CanType getValueType() const {
    return ValueType;
  }
  
  unsigned getNumOperands() const {
    return NumOperands;
  }
  
  StringRef getObjCString() const {
    return ObjCString;
  }
  
  ArrayRef<KeyPathPatternComponent> getComponents() const;
  
  void visitReferencedFunctionsAndMethods(
      std::function<void (SILFunction *)> functionCallBack,
      std::function<void (SILDeclRef)> methodCallBack) {
    for (auto &component : getComponents()) {
      component.visitReferencedFunctionsAndMethods(functionCallBack,
                                                   methodCallBack);
    }
  }

  static KeyPathPattern *get(SILModule &M,
                             CanGenericSignature signature,
                             CanType rootType,
                             CanType valueType,
                             ArrayRef<KeyPathPatternComponent> components,
                             StringRef ObjCString);
  
  static void Profile(llvm::FoldingSetNodeID &ID,
                      CanGenericSignature signature,
                      CanType rootType,
                      CanType valueType,
                      ArrayRef<KeyPathPatternComponent> components,
                      StringRef ObjCString);
  
  void Profile(llvm::FoldingSetNodeID &ID) {
    Profile(ID, getGenericSignature(), getRootType(), getValueType(),
            getComponents(), getObjCString());
  }
};

/// Base class for instructions that access the continuation of an async task,
/// in order to set up a suspension.
/// The continuation must be consumed by an AwaitAsyncContinuation instruction locally,
/// and must dynamically be resumed exactly once during the program's ensuing execution.
class GetAsyncContinuationInstBase
    : public SingleValueInstruction
{
protected:
  CanType ResumeType;
  bool Throws;

  GetAsyncContinuationInstBase(SILInstructionKind Kind, SILDebugLocation Loc,
                               SILType ContinuationType, CanType ResumeType,
                               bool Throws)
    : SingleValueInstruction(Kind, Loc, ContinuationType),
      ResumeType(ResumeType), Throws(Throws) {}

public:
  /// Get the type of the value the async task receives on a resume.
  CanType getFormalResumeType() const { return ResumeType; }
  SILType getLoweredResumeType() const;
  
  /// True if the continuation can be used to resume the task by throwing an error.
  bool throws() const { return Throws; }
  
  static bool classof(SILNodePointer node) {
    return node->getKind() >= SILNodeKind::First_GetAsyncContinuationInstBase &&
           node->getKind() <= SILNodeKind::Last_GetAsyncContinuationInstBase;
  }
};

/// Accesses the continuation for an async task, to prepare a primitive suspend operation.
class GetAsyncContinuationInst final
    : public InstructionBase<SILInstructionKind::GetAsyncContinuationInst,
                             GetAsyncContinuationInstBase>
{
  friend SILBuilder;
  
  GetAsyncContinuationInst(SILDebugLocation Loc,
                           SILType ContinuationType, CanType ResumeType,
                           bool Throws)
    : InstructionBase(Loc, ContinuationType, ResumeType, Throws)
  {}
  
public:
  ArrayRef<Operand> getAllOperands() const { return {}; }
  MutableArrayRef<Operand> getAllOperands() { return {}; }
};

/// Accesses the continuation for an async task, to prepare a primitive suspend operation.
/// The continuation must be consumed by an AwaitAsyncContinuation instruction locally,
/// and must dynamically be resumed exactly once during the program's ensuing execution.
///
/// This variation of the instruction additionally takes an operand for the address of the
/// buffer that receives the incoming value when the continuation is resumed.
class GetAsyncContinuationAddrInst final
    : public UnaryInstructionBase<SILInstructionKind::GetAsyncContinuationAddrInst,
                                  GetAsyncContinuationInstBase>
{
  friend SILBuilder;
  GetAsyncContinuationAddrInst(SILDebugLocation Loc,
                               SILValue ResumeBuf,
                               SILType ContinuationType, CanType ResumeType,
                               bool Throws)
    : UnaryInstructionBase(Loc, ResumeBuf, ContinuationType, ResumeType, Throws)
  {}
};

/// Begins a suspension point and enqueues the continuation to the executor
/// which is bound to the operand actor.
class HopToExecutorInst
    : public UnaryInstructionBase<SILInstructionKind::HopToExecutorInst,
                                  NonValueInstruction>
{
  friend SILBuilder;
  USE_SHARED_UINT8;

  HopToExecutorInst(SILDebugLocation debugLoc, SILValue executor,
                    bool hasOwnership, bool isMandatory)
      : UnaryInstructionBase(debugLoc, executor) {
    sharedUInt8().HopToExecutorInst.mandatory = isMandatory;
  }

public:
  SILValue getTargetExecutor() const { return getOperand(); }

  bool isMandatory() const { return sharedUInt8().HopToExecutorInst.mandatory; }
};

/// Extract the ex that the code is executing on the operand executor already.
class ExtractExecutorInst
    : public UnaryInstructionBase<SILInstructionKind::ExtractExecutorInst,
                                  SingleValueInstruction>
{
  friend SILBuilder;

  ExtractExecutorInst(SILDebugLocation debugLoc, SILValue executor,
                      bool hasOwnership, SILType Ty)
      : UnaryInstructionBase(debugLoc, executor, Ty) { }

public:
  SILValue getExpectedExecutor() const { return getOperand(); }
};

/// Instantiates a key path object.
class KeyPathInst final
    : public InstructionBase<SILInstructionKind::KeyPathInst,
                             SingleValueInstruction>,
      private llvm::TrailingObjects<KeyPathInst, Operand> {
  friend SILBuilder;
  friend TrailingObjects;
  
  KeyPathPattern *Pattern;
  unsigned numPatternOperands;
  unsigned numTypeDependentOperands;
  SubstitutionMap Substitutions;
  
  static KeyPathInst *create(SILDebugLocation Loc,
                             KeyPathPattern *Pattern,
                             SubstitutionMap Subs,
                             ArrayRef<SILValue> Args,
                             SILType Ty,
                             SILFunction &F);
  
  KeyPathInst(SILDebugLocation Loc,
              KeyPathPattern *Pattern,
              SubstitutionMap Subs,
              ArrayRef<SILValue> allOperands,
              unsigned numPatternOperands,
              SILType Ty);
  
  size_t numTrailingObjects(OverloadToken<Operand>) const {
    return numPatternOperands + numTypeDependentOperands;
  }
  
public:
  KeyPathPattern *getPattern() const;
  bool hasPattern() const { return (bool)Pattern; }

  ArrayRef<Operand> getAllOperands() const {
    return const_cast<KeyPathInst*>(this)->getAllOperands();
  }
  MutableArrayRef<Operand> getAllOperands();

  ArrayRef<Operand> getPatternOperands() const {
    return getAllOperands().slice(0, numPatternOperands);
  }

  MutableArrayRef<Operand> getPatternOperands() {
    return getAllOperands().slice(0, numPatternOperands);
  }


  ArrayRef<Operand> getTypeDependentOperands() const {
    return getAllOperands().slice(numPatternOperands);
  }

  MutableArrayRef<Operand> getTypeDependentOperands() {
    return getAllOperands().slice(numPatternOperands);
  }

  SubstitutionMap getSubstitutions() const { return Substitutions; }

  void dropReferencedPattern();
  
  ~KeyPathInst();
};

/// Represents an invocation of builtin functionality provided by the code
/// generator.
class BuiltinInst final
    : public InstructionBaseWithTrailingOperands<
                                   SILInstructionKind::BuiltinInst, BuiltinInst,
                                   SingleValueInstruction> {
  friend SILBuilder;

  /// The name of the builtin to invoke.
  Identifier Name;

  /// The substitutions.
  SubstitutionMap Substitutions;

  BuiltinInst(SILDebugLocation DebugLoc, Identifier Name, SILType ReturnType,
              SubstitutionMap Substitutions, ArrayRef<SILValue> Args);

  static BuiltinInst *create(SILDebugLocation DebugLoc, Identifier Name,
                             SILType ReturnType,
                             SubstitutionMap Substitutions,
                             ArrayRef<SILValue> Args, SILModule &M);

public:
  /// Return the name of the builtin operation.
  Identifier getName() const { return Name; }
  void setName(Identifier I) { Name = I; }
  
  /// Looks up the llvm intrinsic ID and type for the builtin function.
  ///
  /// \returns Returns llvm::Intrinsic::not_intrinsic if the function is not an
  /// intrinsic. The particular intrinsic functions which correspond to the
  /// returned value are defined in llvm/Intrinsics.h.
  const IntrinsicInfo &getIntrinsicInfo() const;
  
  /// Looks up the lazily cached identification for the builtin function.
  const BuiltinInfo &getBuiltinInfo() const;

  /// Looks up the llvm intrinsic ID of this builtin. Returns None if
  /// this is not an intrinsic.
  llvm::Optional<llvm::Intrinsic::ID> getIntrinsicID() const {
    auto I = getIntrinsicInfo();
    if (I.ID == llvm::Intrinsic::not_intrinsic)
      return None;
    return I.ID;
  }

  /// Looks up the BuiltinKind of this builtin. Returns None if this is
  /// not a builtin.
  Optional<BuiltinValueKind> getBuiltinKind() const {
    auto I = getBuiltinInfo();
    if (I.ID == BuiltinValueKind::None)
      return None;
    return I.ID;
  }

  /// True if this builtin application has substitutions, which represent type
  /// parameters to the builtin.
  bool hasSubstitutions() const {
    return Substitutions.hasAnySubstitutableParams();
  }

  /// Return the type parameters to the builtin.
  SubstitutionMap getSubstitutions() const { return Substitutions; }

  /// The arguments to the builtin.
  OperandValueArrayRef getArguments() const {
    return OperandValueArrayRef(getAllOperands());
  }
};

/// Increments a given profiler counter for a given PGO function name. This is
/// lowered to the \c llvm.instrprof.increment LLVM intrinsic.
class IncrementProfilerCounterInst final
    : public InstructionBase<SILInstructionKind::IncrementProfilerCounterInst,
                             NonValueInstruction>,
      private llvm::TrailingObjects<IncrementProfilerCounterInst, char> {
  friend TrailingObjects;
  friend SILBuilder;

  unsigned CounterIdx;
  unsigned PGOFuncNameLength;
  unsigned NumCounters;
  uint64_t PGOFuncHash;

  IncrementProfilerCounterInst(SILDebugLocation Loc, unsigned CounterIdx,
                               unsigned PGOFuncNameLength, unsigned NumCounters,
                               uint64_t PGOFuncHash)
      : InstructionBase(Loc), CounterIdx(CounterIdx),
        PGOFuncNameLength(PGOFuncNameLength), NumCounters(NumCounters),
        PGOFuncHash(PGOFuncHash) {}

  static IncrementProfilerCounterInst *
  create(SILDebugLocation Loc, unsigned CounterIdx, StringRef PGOFuncName,
         unsigned NumCounters, uint64_t PGOFuncHash, SILModule &M);

public:
  /// The index of the counter to be incremented.
  unsigned getCounterIndex() const { return CounterIdx; }

  /// The PGO function name for the function in which the counter resides.
  StringRef getPGOFuncName() const {
    return StringRef(getTrailingObjects<char>(), PGOFuncNameLength);
  }

  /// The total number of counters within the function.
  unsigned getNumCounters() const { return NumCounters; }

  /// A hash value for the function used to determine whether the profile is
  /// outdated.
  /// FIXME: This is currently always 0.
  uint64_t getPGOFuncHash() const { return PGOFuncHash; }

  ArrayRef<Operand> getAllOperands() const { return {}; }
  MutableArrayRef<Operand> getAllOperands() { return {}; }
};

/// Initializes a SIL global variable. Only valid once, before any
/// usages of the global via GlobalAddrInst.
class AllocGlobalInst
    : public InstructionBase<SILInstructionKind::AllocGlobalInst,
                             NonValueInstruction> {
  friend SILBuilder;

  SILGlobalVariable *Global;

  AllocGlobalInst(SILDebugLocation DebugLoc, SILGlobalVariable *Global);

public:
  /// Return the referenced global variable.
  SILGlobalVariable *getReferencedGlobal() const { return Global; }
  
  void setReferencedGlobal(SILGlobalVariable *v) { Global = v; }

  ArrayRef<Operand> getAllOperands() const { return {}; }
  MutableArrayRef<Operand> getAllOperands() { return {}; }
};

/// The base class for global_addr and global_value.
class GlobalAccessInst : public LiteralInst {
  SILGlobalVariable *Global;

protected:
  GlobalAccessInst(SILInstructionKind kind, SILDebugLocation loc,
                   SILType ty, SILGlobalVariable *global)
      : LiteralInst(kind, loc, ty), Global(global) { }

public:
  /// Return the referenced global variable.
  SILGlobalVariable *getReferencedGlobal() const { return Global; }
  
  void setReferencedGlobal(SILGlobalVariable *v) { Global = v; }

  ArrayRef<Operand> getAllOperands() const { return {}; }
  MutableArrayRef<Operand> getAllOperands() { return {}; }
};

/// Gives the address of a SIL global variable. Only valid after an
/// AllocGlobalInst.
class GlobalAddrInst
    : public InstructionBase<SILInstructionKind::GlobalAddrInst,
                             GlobalAccessInst> {
  friend SILBuilder;

  GlobalAddrInst(SILDebugLocation DebugLoc, SILGlobalVariable *Global,
                 TypeExpansionContext context);

public:
  // FIXME: This constructor should be private but is currently used
  //        in the SILParser.

  /// Create a placeholder instruction with an unset global reference.
  GlobalAddrInst(SILDebugLocation DebugLoc, SILType Ty)
      : InstructionBase(DebugLoc, Ty, nullptr) {}
};

/// Creates a base address for offset calculations.
class BaseAddrForOffsetInst
    : public InstructionBase<SILInstructionKind::BaseAddrForOffsetInst,
                             LiteralInst> {
  friend SILBuilder;

  BaseAddrForOffsetInst(SILDebugLocation DebugLoc, SILType Ty)
      : InstructionBase(DebugLoc, Ty) {}

public:
  ArrayRef<Operand> getAllOperands() const { return {}; }
  MutableArrayRef<Operand> getAllOperands() { return {}; }
};

/// Gives the value of a global variable.
///
/// The referenced global variable must be a statically initialized object.
/// TODO: in future we might support global variables in general.
class GlobalValueInst
    : public InstructionBase<SILInstructionKind::GlobalValueInst,
                             GlobalAccessInst> {
  friend SILBuilder;

  GlobalValueInst(SILDebugLocation DebugLoc, SILGlobalVariable *Global,
                  TypeExpansionContext context);
};

/// IntegerLiteralInst - Encapsulates an integer constant, as defined originally
/// by an IntegerLiteralExpr.
class IntegerLiteralInst final
    : public InstructionBase<SILInstructionKind::IntegerLiteralInst,
                             LiteralInst>,
      private llvm::TrailingObjects<IntegerLiteralInst, llvm::APInt::WordType> {
  friend TrailingObjects;
  friend SILBuilder;
  USE_SHARED_UINT32;

  IntegerLiteralInst(SILDebugLocation Loc, SILType Ty, const APInt &Value);

  static IntegerLiteralInst *create(IntegerLiteralExpr *E,
                                    SILDebugLocation Loc, SILModule &M);
  static IntegerLiteralInst *create(SILDebugLocation Loc, SILType Ty,
                                    intmax_t Value, SILModule &M);
  static IntegerLiteralInst *create(SILDebugLocation Loc, SILType Ty,
                                    const APInt &Value, SILModule &M);

public:
  /// getValue - Return the APInt for the underlying integer literal.
  APInt getValue() const;

  ArrayRef<Operand> getAllOperands() const { return {}; }
  MutableArrayRef<Operand> getAllOperands() { return {}; }
};

/// FloatLiteralInst - Encapsulates a floating point constant, as defined
/// originally by a FloatLiteralExpr.
class FloatLiteralInst final
    : public InstructionBase<SILInstructionKind::FloatLiteralInst,
                             LiteralInst>,
      private llvm::TrailingObjects<FloatLiteralInst, llvm::APInt::WordType> {
  friend TrailingObjects;
  friend SILBuilder;
  USE_SHARED_UINT32;

  FloatLiteralInst(SILDebugLocation Loc, SILType Ty, const APInt &Bits);

  static FloatLiteralInst *create(FloatLiteralExpr *E, SILDebugLocation Loc,
                                  SILModule &M);
  static FloatLiteralInst *create(SILDebugLocation Loc, SILType Ty,
                                  const APFloat &Value, SILModule &M);

public:
  /// Return the APFloat for the underlying FP literal.
  APFloat getValue() const;

  /// Return the bitcast representation of the FP literal as an APInt.
  APInt getBits() const;

  ArrayRef<Operand> getAllOperands() const { return {}; }
  MutableArrayRef<Operand> getAllOperands() { return {}; }
};

/// StringLiteralInst - Encapsulates a string constant, as defined originally by
/// a StringLiteralExpr.  This produces the address of the string data as a
/// Builtin.RawPointer.
class StringLiteralInst final
    : public InstructionBase<SILInstructionKind::StringLiteralInst,
                             LiteralInst>,
      private llvm::TrailingObjects<StringLiteralInst, char> {
  friend TrailingObjects;
  friend SILBuilder;
  USE_SHARED_UINT8;
  USE_SHARED_UINT32;

public:
  enum class Encoding {
    Bytes = 0,
    UTF8 = 1,
    /// UTF-8 encoding of an Objective-C selector.
    ObjCSelector = 2,
  };

private:
  StringLiteralInst(SILDebugLocation DebugLoc, StringRef text,
                    Encoding encoding, SILType ty);

  static StringLiteralInst *create(SILDebugLocation DebugLoc, StringRef Text,
                                   Encoding encoding, SILModule &M);

public:
  /// getValue - Return the string data for the literal, in UTF-8.
  StringRef getValue() const {
    return {getTrailingObjects<char>(), sharedUInt32().StringLiteralInst.length};
  }

  /// getEncoding - Return the desired encoding of the text.
  Encoding getEncoding() const {
    return Encoding(sharedUInt8().StringLiteralInst.encoding);
  }

  /// getCodeUnitCount - Return encoding-based length of the string
  /// literal in code units.
  uint64_t getCodeUnitCount();

  ArrayRef<Operand> getAllOperands() const { return {}; }
  MutableArrayRef<Operand> getAllOperands() { return {}; }
};

/// HasSymbolInst - Determines whether a weakly-imported declaration is
/// available at runtime. Produces true if each of the underlying symbol
/// addresses associated with a given declaration are non-null, false otherwise.
class HasSymbolInst final : public LiteralInst {
private:
  friend SILBuilder;

  ValueDecl *Decl;

public:
  HasSymbolInst(SILModule &M, SILDebugLocation Loc, ValueDecl *Decl)
      : LiteralInst(SILInstructionKind::HasSymbolInst, Loc,
                    SILType::getBuiltinIntegerType(1, Decl->getASTContext())),
        Decl{Decl} {}

  ValueDecl *getDecl() const { return Decl; }
  void getReferencedFunctions(llvm::SmallVector<SILFunction *, 4> &fns) const;

  ArrayRef<Operand> getAllOperands() const { return {}; }
  MutableArrayRef<Operand> getAllOperands() { return {}; }

  static bool classof(SILNodePointer node) {
    return node->getKind() == SILNodeKind::HasSymbolInst;
  }
};

//===----------------------------------------------------------------------===//
// Memory instructions.
//===----------------------------------------------------------------------===//

/// StringLiteralInst::Encoding hashes to its underlying integer representation.
static inline llvm::hash_code hash_value(StringLiteralInst::Encoding E) {
  return llvm::hash_value(size_t(E));
}

// *NOTE* When serializing, we can only represent up to 4 values here. If more
// qualifiers are added, SIL serialization must be updated.
enum class LoadOwnershipQualifier {
  Unqualified, Take, Copy, Trivial
};
static_assert(2 == SILNode::NumLoadOwnershipQualifierBits, "Size mismatch");

/// LoadInst - Represents a load from a memory location.
class LoadInst
  : public UnaryInstructionBase<SILInstructionKind::LoadInst,
                                SingleValueInstruction>
{
  friend SILBuilder;
  USE_SHARED_UINT8;

  /// Constructs a LoadInst.
  ///
  /// \param DebugLoc The location of the expression that caused the load.
  ///
  /// \param LValue The SILValue representing the lvalue (address) to
  ///        use for the load.
  LoadInst(SILDebugLocation DebugLoc, SILValue LValue,
           LoadOwnershipQualifier Q = LoadOwnershipQualifier::Unqualified)
      : UnaryInstructionBase(DebugLoc, LValue,
                             LValue->getType().getObjectType()) {
    sharedUInt8().LoadInst.ownershipQualifier = uint8_t(Q);
  }

public:
  LoadOwnershipQualifier getOwnershipQualifier() const {
    return LoadOwnershipQualifier(sharedUInt8().LoadInst.ownershipQualifier);
  }
  void setOwnershipQualifier(LoadOwnershipQualifier qualifier) {
    sharedUInt8().LoadInst.ownershipQualifier = uint8_t(qualifier);
  }
};

// *NOTE* When serializing, we can only represent up to 4 values here. If more
// qualifiers are added, SIL serialization must be updated.
enum class StoreOwnershipQualifier {
  Unqualified, Init, Assign, Trivial
};
static_assert(2 == SILNode::NumStoreOwnershipQualifierBits, "Size mismatch");

/// StoreInst - Represents a store from a memory location.
class StoreInst
    : public InstructionBase<SILInstructionKind::StoreInst,
                             NonValueInstruction>,
      public CopyLikeInstruction {
  friend SILBuilder;

private:
  FixedOperandList<2> Operands;
  USE_SHARED_UINT8;

  StoreInst(SILDebugLocation DebugLoc, SILValue Src, SILValue Dest,
            StoreOwnershipQualifier Qualifier);

public:
  SILValue getSrc() const { return Operands[Src].get(); }
  SILValue getDest() const { return Operands[Dest].get(); }

  void setSrc(SILValue V) { Operands[Src].set(V); }
  void setDest(SILValue V) { Operands[Dest].set(V); }

  ArrayRef<Operand> getAllOperands() const { return Operands.asArray(); }
  MutableArrayRef<Operand> getAllOperands() { return Operands.asArray(); }

  StoreOwnershipQualifier getOwnershipQualifier() const {
    return StoreOwnershipQualifier(sharedUInt8().StoreInst.ownershipQualifier);
  }
  void setOwnershipQualifier(StoreOwnershipQualifier qualifier) {
    sharedUInt8().StoreInst.ownershipQualifier = uint8_t(qualifier);
  }
};

class EndBorrowInst;

/// Represents a load of a borrowed value. Must be paired with an end_borrow
/// instruction in its use-def list.
class LoadBorrowInst :
    public UnaryInstructionBase<SILInstructionKind::LoadBorrowInst,
                                SingleValueInstruction> {
  friend class SILBuilder;

public:
  LoadBorrowInst(SILDebugLocation DebugLoc, SILValue LValue)
      : UnaryInstructionBase(DebugLoc, LValue,
                             LValue->getType().getObjectType()) {}

  using EndBorrowRange =
      decltype(std::declval<ValueBase>().getUsersOfType<EndBorrowInst>());

  /// Return a range over all EndBorrow instructions for this BeginBorrow.
  EndBorrowRange getEndBorrows() const;
};

inline auto LoadBorrowInst::getEndBorrows() const -> EndBorrowRange {
  return getUsersOfType<EndBorrowInst>();
}

/// Represents the begin scope of a borrowed value. Must be paired with an
/// end_borrow instruction in its use-def list.
class BeginBorrowInst
    : public UnaryInstructionBase<SILInstructionKind::BeginBorrowInst,
                                  SingleValueInstruction> {
  friend class SILBuilder;

  USE_SHARED_UINT8;

  BeginBorrowInst(SILDebugLocation DebugLoc, SILValue LValue, bool isLexical,
                  bool hasPointerEscape)
      : UnaryInstructionBase(DebugLoc, LValue,
                             LValue->getType().getObjectType()) {
    sharedUInt8().BeginBorrowInst.lexical = isLexical;
    sharedUInt8().BeginBorrowInst.pointerEscape = hasPointerEscape;
  }

public:
  // FIXME: this does not return all instructions that end a local borrow
  // scope. Branches can also end it via a reborrow, so APIs using this are
  // incorrect. Instead, either iterate over all uses and return those with
  // OperandOwnership::EndBorrow or Reborrow.
  using EndBorrowRange =
      decltype(std::declval<ValueBase>().getUsersOfType<EndBorrowInst>());

  /// Whether the borrow scope introduced by this instruction corresponds to a
  /// source-level lexical scope.
  bool isLexical() const { return sharedUInt8().BeginBorrowInst.lexical; }

  /// If this is a lexical borrow, eliminate the lexical bit. If this borrow
  /// doesn't have a lexical bit, do not do anything.
  void removeIsLexical() { sharedUInt8().BeginBorrowInst.lexical = false; }

  bool hasPointerEscape() const {
    return sharedUInt8().BeginBorrowInst.pointerEscape;
  }
  void setHasPointerEscape(bool pointerEscape) {
    sharedUInt8().BeginBorrowInst.pointerEscape = pointerEscape;
  }

  /// Return a range over all EndBorrow instructions for this BeginBorrow.
  EndBorrowRange getEndBorrows() const;

  /// Return the single use of this BeginBorrowInst, not including any
  /// EndBorrowInst uses, or return nullptr if the borrow is dead or has
  /// multiple uses.
  ///
  /// Useful for matching common SILGen patterns that emit one borrow per use,
  /// and simplifying pass logic.
  Operand *getSingleNonEndingUse() const;
};

inline auto BeginBorrowInst::getEndBorrows() const -> EndBorrowRange {
  return getUsersOfType<EndBorrowInst>();
}

/// Represents a store of a borrowed value into an address. Returns the borrowed
/// address. Must be paired with an end_borrow in its use-def list.
class StoreBorrowInst
    : public InstructionBase<SILInstructionKind::StoreBorrowInst,
                             SingleValueInstruction>,
      public CopyLikeInstruction {
  friend class SILBuilder;

private:
  FixedOperandList<2> Operands;
  StoreBorrowInst(SILDebugLocation DebugLoc, SILValue Src, SILValue Dest);

public:
  SILValue getSrc() const { return Operands[Src].get(); }
  SILValue getDest() const { return Operands[Dest].get(); }

  ArrayRef<Operand> getAllOperands() const { return Operands.asArray(); }
  MutableArrayRef<Operand> getAllOperands() { return Operands.asArray(); }
};

/// Represents the end of a borrow scope of a value %val from a
/// value or address %src.
///
/// While %val is "live" in a region then,
///
///   1. If %src is an object, it is undefined behavior for %src to be
///   destroyed. This is enforced by the ownership verifier.
///
///   2. If %src is an address, it is undefined behavior for %src to be
///   destroyed or written to.
class EndBorrowInst
    : public UnaryInstructionBase<SILInstructionKind::EndBorrowInst,
                                  NonValueInstruction> {
  friend class SILBuilder;

  EndBorrowInst(SILDebugLocation debugLoc, SILValue borrowedValue)
      : UnaryInstructionBase(debugLoc, borrowedValue) {}


};

/// Different kinds of access.
enum class SILAccessKind : uint8_t {
  /// An access which takes uninitialized memory and initializes it.
  Init,

  /// An access which reads the value of initialized memory, but doesn't
  /// modify it.
  Read,

  /// An access which changes the value of initialized memory.
  Modify,

  /// An access which takes initialized memory and leaves it uninitialized.
  Deinit,

  // This enum is encoded.
  Last = Deinit,
};
enum { NumSILAccessKindBits = 2 };

StringRef getSILAccessKindName(SILAccessKind kind);

/// Different kinds of exclusivity enforcement for accesses.
enum class SILAccessEnforcement : uint8_t {
  /// The access's enforcement has not yet been determined.
  Unknown,

  /// The access is statically known to not conflict with other accesses.
  Static,

  /// TODO: maybe add InitiallyStatic for when the access is statically
  /// known to not interfere with any accesses when it begins but where
  /// it's possible that other accesses might be started during this access.

  /// The access is not statically known to not conflict with anything
  /// and must be dynamically checked.
  Dynamic,

  /// The access is not statically known to not conflict with anything
  /// but dynamic checking should be suppressed, leaving it undefined
  /// behavior.
  Unsafe,

  /// Access to pointers that are signed via pointer authentication mechanishm.
  /// Such pointers should be authenticated before read and signed before a
  /// write. Optimizer should avoid promoting such accesses to values.
  Signed,

  // This enum is encoded.
  Last = Signed
};
StringRef getSILAccessEnforcementName(SILAccessEnforcement enforcement);

class EndAccessInst;

/// Base class for BeginAccessInst and BeginUnpairedAccessInst.
template<typename Base>
class BeginAccessBase : public Base {
  TEMPLATE_USE_SHARED_UINT8(Base);

protected:
  template <typename... A>
  BeginAccessBase(SILDebugLocation loc,
                  SILAccessKind accessKind, SILAccessEnforcement enforcement,
                  bool noNestedConflict, bool fromBuiltin, A &&... args)
      : Base(loc, std::forward<A>(args)...) {
    sharedUInt8().BeginAccessBase.accessKind = (uint8_t)accessKind;
    sharedUInt8().BeginAccessBase.enforcement = (uint8_t)enforcement;
    sharedUInt8().BeginAccessBase.noNestedConflict = noNestedConflict;
    sharedUInt8().BeginAccessBase.fromBuiltin = fromBuiltin;
  }

public:
  SILAccessKind getAccessKind() const {
    return SILAccessKind(sharedUInt8().BeginAccessBase.accessKind);
  }
  void setAccessKind(SILAccessKind kind) {
    sharedUInt8().BeginAccessBase.accessKind = unsigned(kind);
  }

  SILAccessEnforcement getEnforcement() const {
    return
      SILAccessEnforcement(sharedUInt8().BeginAccessBase.enforcement);
  }
  void setEnforcement(SILAccessEnforcement enforcement) {
    sharedUInt8().BeginAccessBase.enforcement = unsigned(enforcement);
  }

  /// If hasNoNestedConflict is true, then it is a static guarantee against
  /// inner conflicts. No subsequent access between this point and the
  /// corresponding end_access could cause an enforcement failure. Consequently,
  /// the access will not need to be tracked by the runtime for the duration of
  /// its scope. This access may still conflict with an outer access scope;
  /// therefore may still require dynamic enforcement at a single point.
  bool hasNoNestedConflict() const {
    return sharedUInt8().BeginAccessBase.noNestedConflict;
  }
  void setNoNestedConflict(bool noNestedConflict) {
    sharedUInt8().BeginAccessBase.noNestedConflict = noNestedConflict;
  }

  /// Return true if this access marker was emitted for a user-controlled
  /// Builtin. Return false if this access marker was auto-generated by the
  /// compiler to enforce formal access that derives from the language.
  bool isFromBuiltin() const {
    return sharedUInt8().BeginAccessBase.fromBuiltin;
  }
};

/// Begins an access scope. Must be paired with an end_access instruction
/// along every path.
class BeginAccessInst
    : public BeginAccessBase<UnaryInstructionBase<SILInstructionKind::BeginAccessInst,
                                  SingleValueInstruction>> {
  friend class SILBuilder;

  BeginAccessInst(SILDebugLocation loc, SILValue lvalue,
                  SILAccessKind accessKind, SILAccessEnforcement enforcement,
                  bool noNestedConflict, bool fromBuiltin)
      : BeginAccessBase(loc, accessKind, enforcement, noNestedConflict,
        fromBuiltin, lvalue, lvalue->getType()) {

    static_assert(unsigned(SILAccessKind::Last) < (1 << 3),
                  "reserve sufficient bits for serialized SIL");
    static_assert(unsigned(SILAccessEnforcement::Last) < (1 << 3),
                  "reserve sufficient bits for serialized SIL");

    static_assert(unsigned(SILAccessKind::Last) <
                  (1 << SILNode::NumSILAccessKindBits),
                  "SILNode needs updating");
    static_assert(unsigned(SILAccessEnforcement::Last) <
                  (1 << SILNode::NumSILAccessEnforcementBits),
                  "SILNode needs updating");
  }

public:
  SILValue getSource() const {
    return getOperand();
  }

  using EndAccessRange =
      decltype(std::declval<ValueBase>().getUsersOfType<EndAccessInst>());

  /// Find all the associated end_access instructions for this begin_access.
  EndAccessRange getEndAccesses() const;
};

/// Represents the end of an access scope.
class EndAccessInst
    : public UnaryInstructionBase<SILInstructionKind::EndAccessInst,
                                  NonValueInstruction> {
  friend class SILBuilder;
  USE_SHARED_UINT8;

private:
  EndAccessInst(SILDebugLocation loc, SILValue access, bool aborting = false)
      : UnaryInstructionBase(loc, access) {
    sharedUInt8().EndAccessInst.aborting = aborting;
  }

public:
  /// An aborted access is one that did not perform the expected
  /// transition described by the begin_access instruction before it
  /// reached this end_access.
  ///
  /// Only AccessKind::Init and AccessKind::Deinit accesses can be
  /// aborted.
  bool isAborting() const {
    return sharedUInt8().EndAccessInst.aborting;
  }
  void setAborting(bool aborting = true) {
    sharedUInt8().EndAccessInst.aborting = aborting;
  }

  BeginAccessInst *getBeginAccess() const {
    return cast<BeginAccessInst>(getOperand());
  }

  SILValue getSource() const {
    return getBeginAccess()->getSource();
  }
};

inline auto BeginAccessInst::getEndAccesses() const -> EndAccessRange {
  return getUsersOfType<EndAccessInst>();
}

/// Begins an access without requiring a paired end_access.
/// Dynamically, an end_unpaired_access does still need to be called, though.
///
/// This should only be used in materializeForSet, and eventually it should
/// be removed entirely.
class BeginUnpairedAccessInst
    : public BeginAccessBase<InstructionBase<
        SILInstructionKind::BeginUnpairedAccessInst, NonValueInstruction>> {
  friend class SILBuilder;

  FixedOperandList<2> Operands;

  BeginUnpairedAccessInst(SILDebugLocation loc, SILValue addr, SILValue buffer,
                          SILAccessKind accessKind,
                          SILAccessEnforcement enforcement,
                          bool noNestedConflict,
                          bool fromBuiltin)
      : BeginAccessBase(loc, accessKind, enforcement, noNestedConflict,
                        fromBuiltin),
        Operands(this, addr, buffer) {
  }

public:
  SILValue getSource() const {
    return Operands[0].get();
  }

  SILValue getBuffer() const {
    return Operands[1].get();
  }

  ArrayRef<Operand> getAllOperands() const { return Operands.asArray(); }
  MutableArrayRef<Operand> getAllOperands() { return Operands.asArray(); }

  ArrayRef<Operand> getTypeDependentOperands() const {
    return {};
  }

  MutableArrayRef<Operand> getTypeDependentOperands() {
    return {};
  }
};

/// Ends an unpaired access.
class EndUnpairedAccessInst
    : public UnaryInstructionBase<SILInstructionKind::EndUnpairedAccessInst,
                                  NonValueInstruction> {
  friend class SILBuilder;
  USE_SHARED_UINT8;

private:
  EndUnpairedAccessInst(SILDebugLocation loc, SILValue buffer,
                        SILAccessEnforcement enforcement, bool aborting,
                        bool fromBuiltin)
      : UnaryInstructionBase(loc, buffer) {
    sharedUInt8().EndUnpairedAccessInst.enforcement = uint8_t(enforcement);
    sharedUInt8().EndUnpairedAccessInst.aborting = aborting;
    sharedUInt8().EndUnpairedAccessInst.fromBuiltin = fromBuiltin;
  }

public:
  /// An aborted access is one that did not perform the expected
  /// transition described by the begin_access instruction before it
  /// reached this end_access.
  ///
  /// Only AccessKind::Init and AccessKind::Deinit accesses can be
  /// aborted.
  bool isAborting() const {
    return sharedUInt8().EndUnpairedAccessInst.aborting;
  }
  void setAborting(bool aborting) {
    sharedUInt8().EndUnpairedAccessInst.aborting = aborting;
  }

  SILAccessEnforcement getEnforcement() const {
    return SILAccessEnforcement(
        sharedUInt8().EndUnpairedAccessInst.enforcement);
  }
  void setEnforcement(SILAccessEnforcement enforcement) {
    sharedUInt8().EndUnpairedAccessInst.enforcement =
        unsigned(enforcement);
  }

  /// Return true if this access marker was emitted for a user-controlled
  /// Builtin. Return false if this access marker was auto-generated by the
  /// compiler to enforce formal access that derives from the language.
  bool isFromBuiltin() const {
    return sharedUInt8().EndUnpairedAccessInst.fromBuiltin;
  }

  SILValue getBuffer() const {
    return getOperand();
  }
};

// *NOTE* When serializing, we can only represent up to 4 values here. If more
// qualifiers are added, SIL serialization must be updated.
enum class AssignOwnershipQualifier {
  /// Unknown initialization method
  Unknown,

  /// The box contains a fully-initialized value.
  Reassign,

  /// The box contains a class instance that we own, but the instance has
  /// not been initialized and should be freed with a special SIL
  /// instruction made for this purpose.
  Reinit,

  /// The box contains an undefined value that should be ignored.
  Init,
};
static_assert(2 == SILNode::NumAssignOwnershipQualifierBits, "Size mismatch");

template <SILInstructionKind Kind, int NumOps>
class AssignInstBase
    : public InstructionBase<Kind, NonValueInstruction>,
      public CopyLikeInstruction {

protected:
  FixedOperandList<NumOps> Operands;

  template <class... T>
  AssignInstBase(SILDebugLocation DebugLoc, T&&...args) :
      InstructionBase<Kind, NonValueInstruction>(DebugLoc),
      Operands(this, std::forward<T>(args)...) { }

public:
  SILValue getSrc() const { return Operands[Src].get(); }
  SILValue getDest() const { return Operands[Dest].get(); }

  ArrayRef<Operand> getAllOperands() const { return Operands.asArray(); }
  MutableArrayRef<Operand> getAllOperands() { return Operands.asArray(); }
};

/// AssignInst - Represents an abstract assignment to a memory location, which
/// may either be an initialization or a store sequence.  This is only valid in
/// Raw SIL.
class AssignInst
    : public AssignInstBase<SILInstructionKind::AssignInst, 2> {
  friend SILBuilder;
  USE_SHARED_UINT8;

  AssignInst(SILDebugLocation DebugLoc, SILValue Src, SILValue Dest,
             AssignOwnershipQualifier Qualifier =
             AssignOwnershipQualifier::Unknown);

public:
  AssignOwnershipQualifier getOwnershipQualifier() const {
    return AssignOwnershipQualifier(
      sharedUInt8().AssignInst.ownershipQualifier);
  }
  void setOwnershipQualifier(AssignOwnershipQualifier qualifier) {
    sharedUInt8().AssignInst.ownershipQualifier = unsigned(qualifier);
  }
};

/// AssignByWrapperInst - Represents an abstract assignment via a wrapper,
/// which may either be an initialization or a store sequence.  This is only
/// valid in Raw SIL.
class AssignByWrapperInst
    : public AssignInstBase<SILInstructionKind::AssignByWrapperInst, 4> {
  friend SILBuilder;
  USE_SHARED_UINT8;

public:
  enum Mode {
    /// The mode is not decided yet (by DefiniteInitialization).
    Unknown,
    
    /// The initializer is called with Src as argument. The result is stored to
    /// Dest.
    Initialization,
    
    // Like ``Initialization``, except that the destination is "assigned" rather
    // than "initialized". This means that the existing value in the destination
    // is destroyed before the new value is stored.
    Assign,
    
    /// The setter is called with Src as argument. The Dest is not used in this
    /// case.
    AssignWrappedValue
  };

private:
  AssignByWrapperInst(SILDebugLocation DebugLoc,
                      SILValue Src, SILValue Dest, SILValue Initializer,
                      SILValue Setter, Mode mode);

public:
  SILValue getInitializer() { return Operands[2].get(); }
  SILValue getSetter() { return  Operands[3].get(); }

  Mode getMode() const {
    return Mode(sharedUInt8().AssignByWrapperInst.mode);
  }

  void setMode(Mode mode) {
    sharedUInt8().AssignByWrapperInst.mode = uint8_t(mode);
  }
};

/// AssignOrInitInst - Represents an abstract assignment via a init accessor
/// or a setter, which may either be an initialization or a store sequence.
/// This is only valid in Raw SIL.
///
/// Note that this instruction does not inherit from AssignInstBase because
/// there is no physical destination of the assignment. Both the init
/// and the setter are factored into functions.
class AssignOrInitInst
    : public InstructionBase<SILInstructionKind::AssignOrInitInst,
                             NonValueInstruction>,
      public CopyLikeInstruction {
  friend SILBuilder;
  USE_SHARED_UINT8;

  FixedOperandList<3> Operands;

  /// Marks all of the properties in `initializes(...)` list that
  /// have been initialized before this intruction to help Raw SIL
  /// lowering to emit destroys.
  llvm::BitVector Assignments;

public:
  enum Mode {
    /// The mode is not decided yet (by DefiniteInitialization).
    Unknown,

    /// The initializer is called with Src as argument.
    Init,

    /// The setter is called with Src as argument.
    Set
  };

private:
  AssignOrInitInst(SILDebugLocation DebugLoc,
                   SILValue Src, SILValue Initializer,
                   SILValue Setter, Mode mode);

public:
  SILValue getSrc() const { return Operands[0].get(); }
  SILValue getInitializer() const { return Operands[1].get(); }
  SILValue getSetter() { return  Operands[2].get(); }

  Mode getMode() const {
    return Mode(sharedUInt8().AssignOrInitInst.mode);
  }

  void setMode(Mode mode) {
    sharedUInt8().AssignOrInitInst.mode = uint8_t(mode);
  }

  /// Mark a property from `initializes(...)` list as initialized
  /// before this instruction.
  void markAsInitialized(VarDecl *property);
  void markAsInitialized(unsigned propertyIdx);

  /// Check whether a property from `initializes(...)` list with
  /// the given index has already been initialized and requires
  /// destroy before it could be re-initialized.
  bool isPropertyAlreadyInitialized(unsigned propertyIdx);

  unsigned getNumInitializedProperties() const;

  ArrayRef<VarDecl *> getInitializedProperties() const;
  ArrayRef<VarDecl *> getAccessedProperties() const;

  ArrayRef<Operand> getAllOperands() const { return Operands.asArray(); }
  MutableArrayRef<Operand> getAllOperands() { return Operands.asArray(); }

  AccessorDecl *getReferencedInitAccessor() const;
};

/// Indicates that a memory location is uninitialized at this point and needs to
/// be initialized by the end of the function and before any escape point for
/// this instruction. This is only valid in Raw SIL.
class MarkUninitializedInst
    : public UnaryInstructionBase<SILInstructionKind::MarkUninitializedInst,
                                  OwnershipForwardingSingleValueInstruction> {
  friend SILBuilder;

public:
  /// This enum captures what the mark_uninitialized instruction is designating.
  enum Kind {
    /// Var designates the start of a normal variable live range.
    Var,

    /// RootSelf designates "self" in a struct, enum, or root class.
    RootSelf,

    /// CrossModuleRootSelf is the same as "RootSelf", but in a case where
    /// it's not really safe to treat 'self' as root because the original
    /// module might add more stored properties.
    ///
    /// This is only used for Swift 4 compatibility. In Swift 5, cross-module
    /// initializers are always DelegatingSelf.
    CrossModuleRootSelf,

    /// DerivedSelf designates "self" in a derived (non-root) class.
    DerivedSelf,

    /// DerivedSelfOnly designates "self" in a derived (non-root)
    /// class whose stored properties have already been initialized.
    DerivedSelfOnly,

    /// DelegatingSelf designates "self" on a struct, enum, or class
    /// in a delegating constructor (one that calls self.init).
    DelegatingSelf,

    /// DelegatingSelfAllocated designates "self" in a delegating class
    /// initializer where memory has already been allocated.
    DelegatingSelfAllocated,

    /// Out designates an indirectly returned result.
    /// This is the result that has to be checked for initialization.
    Out,
  };
private:
  Kind ThisKind;

  MarkUninitializedInst(SILDebugLocation DebugLoc, SILValue Value, Kind K,
                        ValueOwnershipKind forwardingOwnershipKind)
      : UnaryInstructionBase(DebugLoc, Value, Value->getType(),
                             forwardingOwnershipKind),
        ThisKind(K) {}

public:
  Kind getMarkUninitializedKind() const { return ThisKind; }

  bool isVar() const { return ThisKind == Var; }
  bool isOut() const { return ThisKind == Out; }
  bool isRootSelf() const {
    return ThisKind == RootSelf;
  }
  bool isCrossModuleRootSelf() const {
    return ThisKind == CrossModuleRootSelf;
  }
  bool isDerivedClassSelf() const {
    return ThisKind == DerivedSelf;
  }
  bool isDerivedClassSelfOnly() const {
    return ThisKind == DerivedSelfOnly;
  }
  bool isDelegatingSelf() const {
    return ThisKind == DelegatingSelf;
  }
  bool isDelegatingSelfAllocated() const {
    return ThisKind == DelegatingSelfAllocated;
  }
};

/// MarkFunctionEscape - Represents the escape point of set of variables due to
/// a function definition which uses the variables.  This is only valid in Raw
/// SIL.
class MarkFunctionEscapeInst final
    : public InstructionBaseWithTrailingOperands<
                                  SILInstructionKind::MarkFunctionEscapeInst,
                                  MarkFunctionEscapeInst, NonValueInstruction> {
  friend SILBuilder;

  /// Private constructor.  Because this is variadic, object creation goes
  /// through 'create()'.
  MarkFunctionEscapeInst(SILDebugLocation DebugLoc, ArrayRef<SILValue> Elements)
    : InstructionBaseWithTrailingOperands(Elements, DebugLoc) {}

  /// Construct a MarkFunctionEscapeInst.
  static MarkFunctionEscapeInst *create(SILDebugLocation DebugLoc,
                                        ArrayRef<SILValue> Elements,
                                        SILFunction &F);

public:
  /// The elements referenced by this instruction.
  MutableArrayRef<Operand> getElementOperands() {
    return getAllOperands();
  }

  /// The elements referenced by this instruction.
  OperandValueArrayRef getElements() const {
    return OperandValueArrayRef(getAllOperands());
  }
};

/// This instruction is inserted by Onone optimizations as a replacement for deleted
/// instructions to ensure that it's possible to set a breakpoint on its location.
class DebugStepInst final
    : public InstructionBase<SILInstructionKind::DebugStepInst, NonValueInstruction> {
  friend SILBuilder;

  DebugStepInst(SILDebugLocation debugLoc) : InstructionBase(debugLoc) {}

public:
  ArrayRef<Operand> getAllOperands() const { return {}; }
  MutableArrayRef<Operand> getAllOperands() { return {}; }
};

/// Define the start or update to a symbolic variable value (for loadable
/// types).
class DebugValueInst final
    : public UnaryInstructionBase<SILInstructionKind::DebugValueInst,
                                  NonValueInstruction>,
      private SILDebugVariableSupplement,
      private llvm::TrailingObjects<DebugValueInst, SILType, SILLocation,
                                    const SILDebugScope *, SILDIExprElement,
                                    char> {
  friend TrailingObjects;
  friend SILBuilder;

  TailAllocatedDebugVariable VarInfo;
  USE_SHARED_UINT8;

  DebugValueInst(SILDebugLocation DebugLoc, SILValue Operand,
                 SILDebugVariable Var, bool poisonRefs, bool operandWasMoved,
                 bool trace);
  static DebugValueInst *create(SILDebugLocation DebugLoc, SILValue Operand,
                                SILModule &M, SILDebugVariable Var,
                                bool poisonRefs, bool operandWasMoved,
                                bool trace);
  static DebugValueInst *createAddr(SILDebugLocation DebugLoc, SILValue Operand,
                                    SILModule &M, SILDebugVariable Var,
                                    bool operandWasMoved, bool trace);

  SIL_DEBUG_VAR_SUPPLEMENT_TRAILING_OBJS_IMPL()

  size_t numTrailingObjects(OverloadToken<char>) const { return 1; }

public:
  /// Sets a bool that states this debug_value is supposed to use the
  void setUsesMoveableValueDebugInfo() {
    sharedUInt8().DebugValueInst.usesMoveableValueDebugInfo = true;
  }

  /// True if this debug_value is on an SSA value that was moved.
  ///
  /// IRGen uses this information to determine if we should use llvm.dbg.addr or
  /// llvm.dbg.declare.
  bool getUsesMoveableValueDebugInfo() const {
    return sharedUInt8().DebugValueInst.usesMoveableValueDebugInfo;
  }

  /// Return the underlying variable declaration that this denotes,
  /// or null if we don't have one.
  VarDecl *getDecl() const;

  /// Return the debug variable information attached to this instruction.
  Optional<SILDebugVariable> getVarInfo() const {
    Optional<SILType> AuxVarType;
    Optional<SILLocation> VarDeclLoc;
    const SILDebugScope *VarDeclScope = nullptr;
    if (HasAuxDebugVariableType)
      AuxVarType = *getTrailingObjects<SILType>();

    if (hasAuxDebugLocation())
      VarDeclLoc = *getTrailingObjects<SILLocation>();
    if (hasAuxDebugScope())
      VarDeclScope = *getTrailingObjects<const SILDebugScope *>();

    llvm::ArrayRef<SILDIExprElement> DIExprElements(
        getTrailingObjects<SILDIExprElement>(), NumDIExprOperands);

    return VarInfo.get(getDecl(), getTrailingObjects<char>(), AuxVarType,
                       VarDeclLoc, VarDeclScope, DIExprElements);
  }

  void setDebugVarScope(const SILDebugScope *NewDS) {
    if (hasAuxDebugScope())
      *getTrailingObjects<const SILDebugScope *>() = NewDS;
  }

  /// Whether the SSA value associated with the current debug_value
  /// instruction has an address type.
  bool hasAddrVal() const {
    return getOperand()->getType().isAddress();
  }

  /// An utility to check if \p I is DebugValueInst and
  /// whether it's associated with address type SSA value.
  static DebugValueInst *hasAddrVal(SILInstruction *I) {
    auto *DVI = dyn_cast_or_null<DebugValueInst>(I);
    return DVI && DVI->hasAddrVal()? DVI : nullptr;
  }

  /// Whether the attached di-expression (if there is any) starts
  /// with `op_deref`.
  bool exprStartsWithDeref() const;

  /// True if all references within this debug value will be overwritten with a
  /// poison sentinel at this point in the program. This is used in debug builds
  /// when shortening non-trivial value lifetimes to ensure the debugger cannot
  /// inspect invalid memory. These are not generated until OSSA is
  /// lowered. They are not expected to be serialized within the module, and the
  /// debug pipeline is not expected to do any significant code motion after
  /// OSSA lowering. It should not be necessary to model the poison operation as
  /// a side effect, which would violate the rule that debug_values cannot
  /// affect optimization.
  bool poisonRefs() const { return sharedUInt8().DebugValueInst.poisonRefs; }

  void setPoisonRefs(bool poisonRefs = true) {
    sharedUInt8().DebugValueInst.poisonRefs = poisonRefs;
  }

  bool hasTrace() const { return sharedUInt8().DebugValueInst.trace; }

  void setTrace(bool trace = true) {
    sharedUInt8().DebugValueInst.trace = trace;
  }
};

class TestSpecificationInst final
    : public InstructionBase<SILInstructionKind::TestSpecificationInst,
                             NonValueInstruction>,
      private llvm::TrailingObjects<TestSpecificationInst, char> {
  friend TrailingObjects;
  friend SILBuilder;

  unsigned ArgumentsSpecificationLength;

  TestSpecificationInst(SILDebugLocation Loc,
                        unsigned ArgumentsSpecificationLength)
      : InstructionBase(Loc),
        ArgumentsSpecificationLength(ArgumentsSpecificationLength) {}

  static TestSpecificationInst *
  create(SILDebugLocation Loc, StringRef argumentsSpecification, SILModule &M);

public:
  StringRef getArgumentsSpecification() const {
    return StringRef(getTrailingObjects<char>(), ArgumentsSpecificationLength);
  }

  ArrayRef<Operand> getAllOperands() const { return {}; }
  MutableArrayRef<Operand> getAllOperands() { return {}; }
};

/// An abstract class representing a load from some kind of reference storage.
template <SILInstructionKind K>
class LoadReferenceInstBase
    : public UnaryInstructionBase<K, SingleValueInstruction> {
  TEMPLATE_USE_SHARED_UINT8(SingleValueInstruction);

  static SILType getResultType(SILType operandTy) {
    assert(operandTy.isAddress() && "loading from non-address operand?");
    auto refType = cast<ReferenceStorageType>(operandTy.getASTType());
    return SILType::getPrimitiveObjectType(refType.getReferentType());
  }

protected:
  LoadReferenceInstBase(SILDebugLocation loc, SILValue lvalue, IsTake_t isTake)
    : UnaryInstructionBase<K, SingleValueInstruction>(loc, lvalue,
                                             getResultType(lvalue->getType())) {
    sharedUInt8().LoadReferenceInstBase.isTake = bool(isTake);
  }

public:
  IsTake_t isTake() const {
    return IsTake_t(sharedUInt8().LoadReferenceInstBase.isTake);
  }
};

/// An abstract class representing a store to some kind of reference storage.
template <SILInstructionKind K>
class StoreReferenceInstBase : public InstructionBase<K, NonValueInstruction> {
  enum { Src, Dest };
  FixedOperandList<2> Operands;
  TEMPLATE_USE_SHARED_UINT8(NonValueInstruction);

protected:
  StoreReferenceInstBase(SILDebugLocation loc, SILValue src, SILValue dest,
                         IsInitialization_t isInit)
    : InstructionBase<K, NonValueInstruction>(loc),
      Operands(this, src, dest) {
    sharedUInt8().StoreReferenceInstBase.isInitializationOfDest = bool(isInit);
  }

public:
  SILValue getSrc() const { return Operands[Src].get(); }
  SILValue getDest() const { return Operands[Dest].get(); }

  IsInitialization_t isInitializationOfDest() const {
    return IsInitialization_t(
      sharedUInt8().StoreReferenceInstBase.isInitializationOfDest);
  }
  void setIsInitializationOfDest(IsInitialization_t I) {
    sharedUInt8().StoreReferenceInstBase.isInitializationOfDest = (bool)I;
  }

  ArrayRef<Operand> getAllOperands() const { return Operands.asArray(); }
  MutableArrayRef<Operand> getAllOperands() { return Operands.asArray(); }
};

/// Represents a load from a dynamic reference storage memory location.
/// This is required for address-only scenarios; for loadable references,
/// it's better to use a load and a strong_retain_#name.
///
/// \param loc The location of the expression that caused the load.
/// \param lvalue The SILValue representing the address to
///        use for the load.
#define NEVER_OR_SOMETIMES_LOADABLE_CHECKED_REF_STORAGE(Name, ...) \
class Load##Name##Inst \
    : public LoadReferenceInstBase<SILInstructionKind::Load##Name##Inst> { \
  friend SILBuilder; \
  Load##Name##Inst(SILDebugLocation loc, SILValue lvalue, IsTake_t isTake) \
    : LoadReferenceInstBase(loc, lvalue, isTake) {} \
};
#include "swift/AST/ReferenceStorage.def"

/// Represents a store to a dynamic reference storage memory location.
/// This is only required for address-only scenarios; for loadable
/// references, it's better to use a ref_to_##name and a store.
#define NEVER_OR_SOMETIMES_LOADABLE_CHECKED_REF_STORAGE(Name, ...) \
class Store##Name##Inst \
    : public StoreReferenceInstBase<SILInstructionKind::Store##Name##Inst> { \
  friend SILBuilder; \
  Store##Name##Inst(SILDebugLocation loc, SILValue src, SILValue dest, \
                IsInitialization_t isInit) \
    : StoreReferenceInstBase(loc, src, dest, isInit) {} \
};
#include "swift/AST/ReferenceStorage.def"

/// CopyAddrInst - Represents a copy from one memory location to another. This
/// is similar to:
///   %1 = load %src
///   store %1 to %dest
/// but a copy instruction must be used for address-only types.
class CopyAddrInst
    : public InstructionBase<SILInstructionKind::CopyAddrInst,
                             NonValueInstruction>,
      public CopyLikeInstruction {
  friend SILBuilder;

private:
  FixedOperandList<2> Operands;
  USE_SHARED_UINT8;

  CopyAddrInst(SILDebugLocation DebugLoc, SILValue Src, SILValue Dest,
               IsTake_t isTakeOfSrc, IsInitialization_t isInitializationOfDest);

public:
  SILValue getSrc() const { return Operands[Src].get(); }
  SILValue getDest() const { return Operands[Dest].get(); }

  void setSrc(SILValue V) { Operands[Src].set(V); }
  void setDest(SILValue V) { Operands[Dest].set(V); }

  IsTake_t isTakeOfSrc() const {
    return IsTake_t(sharedUInt8().CopyAddrInst.isTakeOfSrc);
  }
  IsInitialization_t isInitializationOfDest() const {
    return IsInitialization_t(
      sharedUInt8().CopyAddrInst.isInitializationOfDest);
  }

  void setIsTakeOfSrc(IsTake_t T) {
    sharedUInt8().CopyAddrInst.isTakeOfSrc = (bool)T;
  }
  void setIsInitializationOfDest(IsInitialization_t I) {
    sharedUInt8().CopyAddrInst.isInitializationOfDest = (bool)I;
  }

  ArrayRef<Operand> getAllOperands() const { return Operands.asArray(); }
  MutableArrayRef<Operand> getAllOperands() { return Operands.asArray(); }
};

/// ExplicitCopyAddrInst - A copy_addr that should not be optimized and should
/// be viewed
class ExplicitCopyAddrInst
    : public InstructionBase<SILInstructionKind::ExplicitCopyAddrInst,
                             NonValueInstruction>,
      public CopyLikeInstruction {
  friend SILBuilder;

private:
  FixedOperandList<2> Operands;
  USE_SHARED_UINT8;

  ExplicitCopyAddrInst(SILDebugLocation DebugLoc, SILValue Src, SILValue Dest,
                       IsTake_t isTakeOfSrc,
                       IsInitialization_t isInitializationOfDest);

public:
  SILValue getSrc() const { return Operands[Src].get(); }
  SILValue getDest() const { return Operands[Dest].get(); }

  void setSrc(SILValue V) { Operands[Src].set(V); }
  void setDest(SILValue V) { Operands[Dest].set(V); }

  IsTake_t isTakeOfSrc() const {
    return IsTake_t(sharedUInt8().ExplicitCopyAddrInst.isTakeOfSrc);
  }
  IsInitialization_t isInitializationOfDest() const {
    return IsInitialization_t(
        sharedUInt8().ExplicitCopyAddrInst.isInitializationOfDest);
  }

  void setIsTakeOfSrc(IsTake_t T) {
    sharedUInt8().ExplicitCopyAddrInst.isTakeOfSrc = (bool)T;
  }
  void setIsInitializationOfDest(IsInitialization_t I) {
    sharedUInt8().ExplicitCopyAddrInst.isInitializationOfDest = (bool)I;
  }

  ArrayRef<Operand> getAllOperands() const { return Operands.asArray(); }
  MutableArrayRef<Operand> getAllOperands() { return Operands.asArray(); }
};

/// "%token = bind_memory %0 : $Builtin.RawPointer, %1 : $Builtin.Word to $T"
///
/// Binds memory at the raw pointer %0 to type $T with enough capacity
/// to hold %1 values.
///
/// %token is an opaque word representing the previously bound types of this
/// memory region, before binding it to a contiguous region of type $T. This
/// token has no purpose unless it is consumed be a rebind_memory instruction.
///
/// Semantics: changes the type information assocated with a memory region. This
/// affects all memory operations that alias with the given region of memory,
/// regardless of their type or address provenance. For optimizations that query
/// side effects, this is equivalent to writing and immediately reading an
/// unknown value to memory at `%0` of `%1` bytes.
class BindMemoryInst final : public InstructionBaseWithTrailingOperands<
                                 SILInstructionKind::BindMemoryInst,
                                 BindMemoryInst, SingleValueInstruction> {
  friend SILBuilder;

  SILType BoundType;

  static BindMemoryInst *create(
    SILDebugLocation Loc, SILValue Base, SILValue Index, SILType BoundType,
    SILFunction &F);

  BindMemoryInst(SILDebugLocation Loc, SILValue Base, SILValue Index,
                 SILType BoundType, SILType TokenType,
                 ArrayRef<SILValue> TypeDependentOperands)
      : InstructionBaseWithTrailingOperands(Base, Index, TypeDependentOperands,
                                            Loc, TokenType),
        BoundType(BoundType) {}

public:
  enum { BaseOperIdx, IndexOperIdx, NumFixedOpers };

  SILValue getBase() const { return getAllOperands()[BaseOperIdx].get(); }

  SILValue getIndex() const { return getAllOperands()[IndexOperIdx].get(); }

  SILType getBoundType() const { return BoundType; }

  ArrayRef<Operand> getTypeDependentOperands() const {
    return getAllOperands().slice(NumFixedOpers);
  }

  MutableArrayRef<Operand> getTypeDependentOperands() {
    return getAllOperands().slice(NumFixedOpers);
  }
};

/// "%out_token = rebind_memory
///     %0 : $Builtin.RawPointer, %in_token : $Builtin.Word
///
/// Binds memory at the raw pointer %0 to the types abstractly represented by
/// %in_token.
///
/// %in_token is itself the result of a bind_memory or rebind_memory and
/// represents a previously cached set of bound types.
///
/// %out_token represents the previously bound types of this memory region,
/// before binding it to %in_token.
///
/// This has the same semantics as bind_memory except that the size of memory
/// affected must be derived from `%in_token`.
class RebindMemoryInst final : public SingleValueInstruction {
  FixedOperandList<2> Operands;

public:
  enum { BaseOperIdx, InTokenOperIdx };

  RebindMemoryInst(SILDebugLocation Loc, SILValue Base, SILValue InToken,
                   SILType TokenType)
      : SingleValueInstruction(SILInstructionKind::RebindMemoryInst, Loc,
                               TokenType),
        Operands{this, Base, InToken} {}

public:
  SILValue getBase() const { return getAllOperands()[BaseOperIdx].get(); }

  SILValue getInToken() const { return getAllOperands()[InTokenOperIdx].get(); }

  ArrayRef<Operand> getAllOperands() const { return Operands.asArray(); }
  MutableArrayRef<Operand> getAllOperands() { return Operands.asArray(); }
};

/// ConvertFunctionInst - Change the type of a function value without
/// affecting how it will codegen.
class ConvertFunctionInst final
    : public UnaryInstructionWithTypeDependentOperandsBase<
          SILInstructionKind::ConvertFunctionInst, ConvertFunctionInst,
          OwnershipForwardingSingleValueInstruction> {
  friend SILBuilder;
  USE_SHARED_UINT8;

  ConvertFunctionInst(SILDebugLocation DebugLoc, SILValue Operand,
                      ArrayRef<SILValue> TypeDependentOperands, SILType Ty,
                      bool WithoutActuallyEscaping,
                      ValueOwnershipKind forwardingOwnershipKind)
      : UnaryInstructionWithTypeDependentOperandsBase(DebugLoc, Operand,
                                                      TypeDependentOperands, Ty,
                                                      forwardingOwnershipKind) {
    sharedUInt8().ConvertFunctionInst.withoutActuallyEscaping =
        WithoutActuallyEscaping;
    assert((Operand->getType().castTo<SILFunctionType>()->isNoEscape() ==
                Ty.castTo<SILFunctionType>()->isNoEscape() ||
            Ty.castTo<SILFunctionType>()->getRepresentation() !=
                SILFunctionType::Representation::Thick) &&
           "Change of escapeness is not ABI compatible");
  }

  static ConvertFunctionInst *create(
      SILDebugLocation DebugLoc, SILValue Operand, SILType Ty, SILModule &Mod,
      SILFunction *F,
      bool WithoutActuallyEscaping, ValueOwnershipKind forwardingOwnershipKind);

public:
  /// Returns `true` if this converts a non-escaping closure into an escaping
  /// function type. `True` must be returned whenever the closure operand has an
  /// unboxed capture (via @inout_aliasable) *and* the resulting function type
  /// is escaping. (This only happens as a result of
  /// withoutActuallyEscaping()). If `true` is returned, then the resulting
  /// function type must be escaping, but the operand's function type may or may
  /// not be @noescape. Note that a non-escaping closure may have unboxed
  /// captured even though its SIL function type is "escaping".
  bool withoutActuallyEscaping() const {
    return sharedUInt8().ConvertFunctionInst.withoutActuallyEscaping;
  }
            
  /// Returns `true` if the function conversion is between types with the same
  /// argument and return types, as well as all other attributes, after substitution,
  /// such as converting `$<A, B> in (A) -> B for <Int, String>` to `(Int) -> String`.
  bool onlyConvertsSubstitutions() const;
};

/// ConvertEscapeToNoEscapeInst - Change the type of a escaping function value
/// to a trivial function type (@noescape T -> U).
class ConvertEscapeToNoEscapeInst final
    : public UnaryInstructionWithTypeDependentOperandsBase<
          SILInstructionKind::ConvertEscapeToNoEscapeInst,
          ConvertEscapeToNoEscapeInst, SingleValueInstruction> {
  friend SILBuilder;

  bool lifetimeGuaranteed;

  ConvertEscapeToNoEscapeInst(SILDebugLocation DebugLoc, SILValue Operand,
                              ArrayRef<SILValue> TypeDependentOperands,
                              SILType Ty,
                              bool isLifetimeGuaranteed)
      : UnaryInstructionWithTypeDependentOperandsBase(
            DebugLoc, Operand, TypeDependentOperands, Ty),
        lifetimeGuaranteed(isLifetimeGuaranteed) {
    assert(!Operand->getType().castTo<SILFunctionType>()->isNoEscape());
    assert(Ty.castTo<SILFunctionType>()->isNoEscape());
  }

  static ConvertEscapeToNoEscapeInst *
  create(SILDebugLocation DebugLoc, SILValue Operand, SILType Ty,
         SILFunction &F, bool lifetimeGuaranteed);
public:
  /// Return true if we have extended the lifetime of the argument of the
  /// convert_escape_to_no_escape to be over all uses of the trivial type.
  bool isLifetimeGuaranteed() const {
    return lifetimeGuaranteed;
  }

  /// Mark that we have extended the lifetime of the argument of the
  /// convert_escape_to_no_escape to be over all uses of the trivial type.
  ///
  /// NOTE: This is a one way operation.
  void setLifetimeGuaranteed() { lifetimeGuaranteed = true; }
};

/// UpcastInst - Perform a conversion of a class instance to a supertype.
class UpcastInst final : public UnaryInstructionWithTypeDependentOperandsBase<
                             SILInstructionKind::UpcastInst, UpcastInst,
                             OwnershipForwardingSingleValueInstruction> {
  friend SILBuilder;

  UpcastInst(SILDebugLocation DebugLoc, SILValue Operand,
             ArrayRef<SILValue> TypeDependentOperands, SILType Ty,
             ValueOwnershipKind forwardingOwnershipKind)
      : UnaryInstructionWithTypeDependentOperandsBase(DebugLoc, Operand,
                                                      TypeDependentOperands, Ty,
                                                      forwardingOwnershipKind) {
  }

  static UpcastInst *create(SILDebugLocation DebugLoc, SILValue Operand,
                            SILType Ty, SILFunction &F,
                            ValueOwnershipKind forwardingOwnershipKind);
};

/// AddressToPointerInst - Convert a SIL address to a Builtin.RawPointer value.
class AddressToPointerInst
    : public UnaryInstructionBase<SILInstructionKind::AddressToPointerInst,
                                  SingleValueInstruction> {
  friend SILBuilder;
  USE_SHARED_UINT8;

  AddressToPointerInst(SILDebugLocation DebugLoc, SILValue Operand, SILType Ty,
                       bool needsStackProtection)
      : UnaryInstructionBase(DebugLoc, Operand, Ty) {
        sharedUInt8().AddressToPointerInst.needsStackProtection = needsStackProtection;
      }

public:
  bool needsStackProtection() const {
    return sharedUInt8().AddressToPointerInst.needsStackProtection;
  }
};

/// PointerToAddressInst - Convert a Builtin.RawPointer value to a SIL address.
class PointerToAddressInst
    : public UnaryInstructionBase<SILInstructionKind::PointerToAddressInst,
                                  SingleValueInstruction> {
  friend SILBuilder;
  USE_SHARED_UINT8;
  USE_SHARED_UINT32;

  PointerToAddressInst(SILDebugLocation DebugLoc, SILValue Operand, SILType Ty,
                       bool IsStrict, bool IsInvariant,
                       llvm::MaybeAlign Alignment)
      : UnaryInstructionBase(DebugLoc, Operand, Ty) {
    sharedUInt8().PointerToAddressInst.isStrict = IsStrict;
    sharedUInt8().PointerToAddressInst.isInvariant = IsInvariant;
    unsigned encodedAlignment = llvm::encode(Alignment);
    sharedUInt32().PointerToAddressInst.alignment = encodedAlignment;
    assert(sharedUInt32().PointerToAddressInst.alignment == encodedAlignment
           && "pointer_to_address alignment overflow");
  }

public:
  /// Whether the returned address adheres to strict aliasing.
  /// If true, then the type of each memory access dependent on
  /// this address must be consistent with the memory's bound type.
  bool isStrict() const {
    return sharedUInt8().PointerToAddressInst.isStrict;
  }
  /// Whether the returned address is invariant.
  /// If true, then loading from an address derived from this pointer always
  /// produces the same value.
  bool isInvariant() const {
    return sharedUInt8().PointerToAddressInst.isInvariant;
  }

  /// The byte alignment of the address. Since the alignment of types isn't
  /// known until IRGen (TypeInfo::getBestKnownAlignment), in SIL an unknown
  /// alignment indicates the natural in-memory alignment of the element type.
  llvm::MaybeAlign alignment() const {
    return llvm::decodeMaybeAlign(sharedUInt32().PointerToAddressInst.alignment);
  }
};

/// Convert a heap object reference to a different type without any runtime
/// checks.
class UncheckedRefCastInst final
    : public UnaryInstructionWithTypeDependentOperandsBase<
          SILInstructionKind::UncheckedRefCastInst, UncheckedRefCastInst,
          OwnershipForwardingSingleValueInstruction> {
  friend SILBuilder;

  UncheckedRefCastInst(SILDebugLocation DebugLoc, SILValue Operand,
                       ArrayRef<SILValue> TypeDependentOperands, SILType Ty,
                       ValueOwnershipKind forwardingOwnershipKind)
      : UnaryInstructionWithTypeDependentOperandsBase(DebugLoc, Operand,
                                                      TypeDependentOperands, Ty,
                                                      forwardingOwnershipKind) {
  }

  static UncheckedRefCastInst *
  create(SILDebugLocation DebugLoc, SILValue Operand, SILType Ty,
         SILFunction &F, ValueOwnershipKind forwardingOwnershipKind);
};

/// Convert a value's binary representation to a trivial type of the same size.
class UncheckedTrivialBitCastInst final
    : public UnaryInstructionWithTypeDependentOperandsBase<
          SILInstructionKind::UncheckedTrivialBitCastInst,
          UncheckedTrivialBitCastInst, SingleValueInstruction> {
  friend SILBuilder;

  UncheckedTrivialBitCastInst(SILDebugLocation DebugLoc, SILValue Operand,
                              ArrayRef<SILValue> TypeDependentOperands,
                              SILType Ty)
      : UnaryInstructionWithTypeDependentOperandsBase(DebugLoc, Operand,
                                               TypeDependentOperands, Ty) {}

  static UncheckedTrivialBitCastInst *
  create(SILDebugLocation DebugLoc, SILValue Operand, SILType Ty,
         SILFunction &F);
};

/// Bitwise copy a value into another value of the same size or smaller.
class UncheckedBitwiseCastInst final
    : public UnaryInstructionWithTypeDependentOperandsBase<
          SILInstructionKind::UncheckedBitwiseCastInst,
          UncheckedBitwiseCastInst, SingleValueInstruction> {
  friend SILBuilder;

  UncheckedBitwiseCastInst(SILDebugLocation DebugLoc, SILValue Operand,
                           ArrayRef<SILValue> TypeDependentOperands,
                           SILType Ty)
      : UnaryInstructionWithTypeDependentOperandsBase(DebugLoc, Operand,
                                               TypeDependentOperands, Ty) {}
  static UncheckedBitwiseCastInst *
  create(SILDebugLocation DebugLoc, SILValue Operand, SILType Ty,
         SILFunction &F);
};

/// Bitwise copy a value into another value of the same size.
class UncheckedValueCastInst final
    : public UnaryInstructionWithTypeDependentOperandsBase<
          SILInstructionKind::UncheckedValueCastInst, UncheckedValueCastInst,
          OwnershipForwardingSingleValueInstruction> {
  friend SILBuilder;

  UncheckedValueCastInst(SILDebugLocation DebugLoc, SILValue Operand,
                         ArrayRef<SILValue> TypeDependentOperands, SILType Ty,
                         ValueOwnershipKind forwardingOwnershipKind)
      : UnaryInstructionWithTypeDependentOperandsBase(DebugLoc, Operand,
                                                      TypeDependentOperands, Ty,
                                                      forwardingOwnershipKind) {
  }

  static UncheckedValueCastInst *
  create(SILDebugLocation DebugLoc, SILValue Operand, SILType Ty,
         SILFunction &F, ValueOwnershipKind forwardingOwnershipKind);
};

/// Build a Builtin.BridgeObject from a heap object reference by bitwise-or-ing
/// in bits from a word.
class RefToBridgeObjectInst
    : public InstructionBase<SILInstructionKind::RefToBridgeObjectInst,
                             OwnershipForwardingSingleValueInstruction> {
  friend SILBuilder;

  FixedOperandList<2> Operands;
  RefToBridgeObjectInst(SILDebugLocation DebugLoc, SILValue ConvertedValue,
                        SILValue MaskValue, SILType BridgeObjectTy,
                        ValueOwnershipKind forwardingOwnershipKind)
      : InstructionBase(DebugLoc, BridgeObjectTy, forwardingOwnershipKind),
        Operands(this, ConvertedValue, MaskValue) {}

public:
  SILValue getBitsOperand() const { return Operands[1].get(); }
  
  ArrayRef<Operand> getAllOperands() const { return Operands.asArray(); }
  MutableArrayRef<Operand> getAllOperands() { return Operands.asArray(); }
};

/// Extract the heap object reference from a BridgeObject.
class ClassifyBridgeObjectInst
  : public UnaryInstructionBase<SILInstructionKind::ClassifyBridgeObjectInst,
                                SingleValueInstruction>
{
  friend SILBuilder;

  ClassifyBridgeObjectInst(SILDebugLocation DebugLoc, SILValue Operand,
                           SILType Ty)
    : UnaryInstructionBase(DebugLoc, Operand, Ty) {}
};

/// Extract the heap object reference from a BridgeObject.
class BridgeObjectToRefInst
    : public UnaryInstructionBase<SILInstructionKind::BridgeObjectToRefInst,
                                  OwnershipForwardingSingleValueInstruction> {
  friend SILBuilder;

  BridgeObjectToRefInst(SILDebugLocation DebugLoc, SILValue Operand, SILType Ty,
                        ValueOwnershipKind forwardingOwnershipKind)
      : UnaryInstructionBase(DebugLoc, Operand, Ty, forwardingOwnershipKind) {}
};

/// Sets the BridgeObject to a tagged pointer representation holding its
/// operands
class ValueToBridgeObjectInst
    : public UnaryInstructionBase<SILInstructionKind::ValueToBridgeObjectInst,
                                  SingleValueInstruction> {
  friend SILBuilder;

  ValueToBridgeObjectInst(SILDebugLocation DebugLoc, SILValue Operand,
                          SILType BridgeObjectTy)
      : UnaryInstructionBase(DebugLoc, Operand, BridgeObjectTy) {}
};

/// Retrieve the bit pattern of a BridgeObject.
class BridgeObjectToWordInst
    : public UnaryInstructionBase<SILInstructionKind::BridgeObjectToWordInst,
                                  SingleValueInstruction> {
  friend SILBuilder;

  BridgeObjectToWordInst(SILDebugLocation DebugLoc, SILValue Operand,
                         SILType Ty)
      : UnaryInstructionBase(DebugLoc, Operand, Ty) {}
};

/// RefToRawPointer - Convert a reference type to a Builtin.RawPointer.
class RefToRawPointerInst
    : public UnaryInstructionBase<SILInstructionKind::RefToRawPointerInst,
                                  SingleValueInstruction> {
  friend SILBuilder;

  RefToRawPointerInst(SILDebugLocation DebugLoc, SILValue Operand, SILType Ty)
      : UnaryInstructionBase(DebugLoc, Operand, Ty) {}
};

/// RawPointerToRefInst - Convert a Builtin.RawPointer to a reference type.
class RawPointerToRefInst
    : public UnaryInstructionBase<SILInstructionKind::RawPointerToRefInst,
                                  SingleValueInstruction> {
  friend SILBuilder;

  RawPointerToRefInst(SILDebugLocation DebugLoc, SILValue Operand, SILType Ty)
      : UnaryInstructionBase(DebugLoc, Operand, Ty) {}
};

/// Transparent reference storage to underlying reference type conversion.
/// This does nothing at runtime; it just changes the formal type.
#define LOADABLE_REF_STORAGE(Name, ...)                                        \
  class RefTo##Name##Inst                                                      \
      : public UnaryInstructionBase<SILInstructionKind::RefTo##Name##Inst,     \
                                    SingleValueInstruction> {                  \
    friend SILBuilder;                                                         \
    RefTo##Name##Inst(SILDebugLocation DebugLoc, SILValue Operand, SILType Ty) \
        : UnaryInstructionBase(DebugLoc, Operand, Ty) {}                       \
  };                                                                           \
  class Name##ToRefInst                                                        \
      : public UnaryInstructionBase<SILInstructionKind::Name##ToRefInst,       \
                                    SingleValueInstruction> {                  \
    friend SILBuilder;                                                         \
    Name##ToRefInst(SILDebugLocation DebugLoc, SILValue Operand, SILType Ty)   \
        : UnaryInstructionBase(DebugLoc, Operand, Ty) {}                       \
  };
#include "swift/AST/ReferenceStorage.def"

/// ThinToThickFunctionInst - Given a thin function reference, adds a null
/// context to convert the value to a thick function type.
class ThinToThickFunctionInst final
    : public UnaryInstructionWithTypeDependentOperandsBase<
          SILInstructionKind::ThinToThickFunctionInst, ThinToThickFunctionInst,
          OwnershipForwardingSingleValueInstruction> {
  friend SILBuilder;

  ThinToThickFunctionInst(SILDebugLocation DebugLoc, SILValue Operand,
                          ArrayRef<SILValue> TypeDependentOperands, SILType Ty,
                          ValueOwnershipKind forwardingOwnershipKind)
      : UnaryInstructionWithTypeDependentOperandsBase(DebugLoc, Operand,
                                                      TypeDependentOperands, Ty,
                                                      forwardingOwnershipKind) {
  }

  static ThinToThickFunctionInst *
  create(SILDebugLocation DebugLoc, SILValue Operand, SILType Ty,
         SILModule &Mod, SILFunction *F,
         ValueOwnershipKind forwardingOwnershipKind);

public:
  /// Return the callee of the thin_to_thick_function.
  ///
  /// This is not technically necessary, but from a symmetry perspective it
  /// makes sense to follow the lead of partial_apply which also creates
  /// closures.
  SILValue getCallee() const { return getOperand(); }
};

/// Given a thick metatype value, produces an Objective-C metatype
/// value.
class ThickToObjCMetatypeInst
    : public UnaryInstructionBase<SILInstructionKind::ThickToObjCMetatypeInst,
                                  SingleValueInstruction> {
  friend SILBuilder;

  ThickToObjCMetatypeInst(SILDebugLocation DebugLoc, SILValue Operand,
                          SILType Ty)
      : UnaryInstructionBase(DebugLoc, Operand, Ty) {}
};

/// Given an Objective-C metatype value, produces a thick metatype
/// value.
class ObjCToThickMetatypeInst
    : public UnaryInstructionBase<SILInstructionKind::ObjCToThickMetatypeInst,
                                  SingleValueInstruction> {
  friend SILBuilder;

  ObjCToThickMetatypeInst(SILDebugLocation DebugLoc, SILValue Operand,
                          SILType Ty)
      : UnaryInstructionBase(DebugLoc, Operand, Ty) {}
};

/// Given an Objective-C metatype value, convert it to an AnyObject value.
class ObjCMetatypeToObjectInst
    : public UnaryInstructionBase<SILInstructionKind::ObjCMetatypeToObjectInst,
                                  SingleValueInstruction> {
  friend SILBuilder;

  ObjCMetatypeToObjectInst(SILDebugLocation DebugLoc, SILValue Operand,
                           SILType Ty)
      : UnaryInstructionBase(DebugLoc, Operand, Ty) {}
};

/// Given an Objective-C existential metatype value, convert it to an AnyObject
/// value.
class ObjCExistentialMetatypeToObjectInst
    : public UnaryInstructionBase<
          SILInstructionKind::ObjCExistentialMetatypeToObjectInst,
          SingleValueInstruction> {
  friend SILBuilder;

  ObjCExistentialMetatypeToObjectInst(SILDebugLocation DebugLoc,
                                      SILValue Operand, SILType Ty)
      : UnaryInstructionBase(DebugLoc, Operand, Ty) {}
};

/// Return the Objective-C Protocol class instance for a protocol.
class ObjCProtocolInst
    : public InstructionBase<SILInstructionKind::ObjCProtocolInst,
                             SingleValueInstruction> {
  friend SILBuilder;

  ProtocolDecl *Proto;
  ObjCProtocolInst(SILDebugLocation DebugLoc, ProtocolDecl *Proto, SILType Ty)
      : InstructionBase(DebugLoc, Ty),
        Proto(Proto) {}

public:
  ProtocolDecl *getProtocol() const { return Proto; }

  ArrayRef<Operand> getAllOperands() const { return {}; }
  MutableArrayRef<Operand> getAllOperands() { return {}; }
};


/// Perform an unconditional checked cast that aborts if the cast fails.
class UnconditionalCheckedCastInst final
    : public UnaryInstructionWithTypeDependentOperandsBase<
          SILInstructionKind::UnconditionalCheckedCastInst,
          UnconditionalCheckedCastInst,
          OwnershipForwardingSingleValueInstruction> {
  CanType DestFormalTy;
  friend SILBuilder;

  UnconditionalCheckedCastInst(SILDebugLocation DebugLoc, SILValue Operand,
                               ArrayRef<SILValue> TypeDependentOperands,
                               SILType DestLoweredTy, CanType DestFormalTy,
                               ValueOwnershipKind forwardingOwnershipKind)
      : UnaryInstructionWithTypeDependentOperandsBase(
            DebugLoc, Operand, TypeDependentOperands, DestLoweredTy,
            forwardingOwnershipKind),
        DestFormalTy(DestFormalTy) {}

  static UnconditionalCheckedCastInst *
  create(SILDebugLocation DebugLoc, SILValue Operand, SILType DestLoweredTy,
         CanType DestFormalTy, SILFunction &F,
         ValueOwnershipKind forwardingOwnershipKind);

public:
  SILType getSourceLoweredType() const { return getOperand()->getType(); }
  CanType getSourceFormalType() const { return getSourceLoweredType().getASTType(); }

  CanType getTargetFormalType() const { return DestFormalTy; }
  SILType getTargetLoweredType() const { return getType(); }
};

/// StructInst - Represents a constructed loadable struct.
class StructInst final : public InstructionBaseWithTrailingOperands<
                             SILInstructionKind::StructInst, StructInst,
                             OwnershipForwardingSingleValueInstruction> {
  friend SILBuilder;

  /// Because of the storage requirements of StructInst, object
  /// creation goes through 'create()'.
  StructInst(SILDebugLocation DebugLoc, SILType Ty, ArrayRef<SILValue> Elements,
             ValueOwnershipKind forwardingOwnershipKind);

  /// Construct a StructInst.
  static StructInst *create(SILDebugLocation DebugLoc, SILType Ty,
                            ArrayRef<SILValue> Elements, SILModule &M,
                            ValueOwnershipKind forwardingOwnershipKind);

public:
  /// The elements referenced by this StructInst.
  MutableArrayRef<Operand> getElementOperands() {
    return getAllOperands();
  }

  /// The elements referenced by this StructInst.
  OperandValueArrayRef getElements() const {
    return OperandValueArrayRef(getAllOperands());
  }

  SILValue getFieldValue(const VarDecl *V) const {
    return getOperandForField(V)->get();
  }

  /// Return the Operand associated with the given VarDecl.
  const Operand *getOperandForField(const VarDecl *V) const {
    return const_cast<StructInst*>(this)->getOperandForField(V);
  }

  Operand *getOperandForField(const VarDecl *V) {
    // If V is null or is computed, there is no operand associated with it.
    assert(V && V->hasStorage() &&
           "getOperandForField only works with stored fields");

    StructDecl *S = getStructDecl();

    auto Props = S->getStoredProperties();
    for (unsigned I = 0, E = Props.size(); I < E; ++I)
      if (V == Props[I])
        return &getAllOperands()[I];

    // Did not find a matching VarDecl, return nullptr.
    return nullptr;
  }

  /// Search the operands of this struct for a unique non-trivial field. If we
  /// find it, return it. Otherwise return SILValue().
  SILValue getUniqueNonTrivialFieldValue() {
    auto *F = getFunction();
    ArrayRef<Operand> Ops = getAllOperands();

    Optional<unsigned> Index;
    // For each operand...
    for (unsigned i = 0, e = Ops.size(); i != e; ++i) {
      // If the operand is not trivial...
      if (!Ops[i].get()->getType().isTrivial(*F)) {
        // And we have not found an Index yet, set index to i and continue.
        if (!Index.has_value()) {
          Index = i;
          continue;
        }

        // Otherwise, we have two values that are non-trivial. Bail.
        return SILValue();
      }
    }

    // If we did not find an index, return an empty SILValue.
    if (!Index.has_value())
      return SILValue();

    // Otherwise, return the value associated with index.
    return Ops[Index.value()].get();
  }

  StructDecl *getStructDecl() const {
    auto s = getType().getStructOrBoundGenericStruct();
    assert(s && "A struct should always have a StructDecl associated with it");
    return s;
  }
};

/// RefCountingInst - An abstract class of instructions which
/// manipulate the reference count of their object operand.
class RefCountingInst : public NonValueInstruction {
  USE_SHARED_UINT8;
public:
  /// The atomicity of a reference counting operation to be used.
  enum class Atomicity : bool {
    /// Atomic reference counting operations should be used.
    Atomic,
    /// Non-atomic reference counting operations can be used.
    NonAtomic,
  };
protected:
  RefCountingInst(SILInstructionKind Kind, SILDebugLocation DebugLoc)
      : NonValueInstruction(Kind, DebugLoc) {
    sharedUInt8().RefCountingInst.atomicity = bool(Atomicity::Atomic);
  }

public:
  void setAtomicity(Atomicity flag) {
    sharedUInt8().RefCountingInst.atomicity = bool(flag);
  }
  void setNonAtomic() {
    sharedUInt8().RefCountingInst.atomicity = bool(Atomicity::NonAtomic);
  }
  void setAtomic() {
    sharedUInt8().RefCountingInst.atomicity = bool(Atomicity::Atomic);
  }
  Atomicity getAtomicity() const {
    return Atomicity(sharedUInt8().RefCountingInst.atomicity);
  }
  bool isNonAtomic() const { return getAtomicity() == Atomicity::NonAtomic; }
  bool isAtomic() const { return getAtomicity() == Atomicity::Atomic; }

  DEFINE_ABSTRACT_NON_VALUE_INST_BOILERPLATE(RefCountingInst)
};

/// RetainValueInst - Copies a loadable value.
class RetainValueInst
    : public UnaryInstructionBase<SILInstructionKind::RetainValueInst,
                                  RefCountingInst> {
  friend SILBuilder;

  RetainValueInst(SILDebugLocation DebugLoc, SILValue operand,
                  Atomicity atomicity)
      : UnaryInstructionBase(DebugLoc, operand) {
    setAtomicity(atomicity);
  }
};

/// RetainValueAddrInst - Copies a loadable value by address.
class RetainValueAddrInst
    : public UnaryInstructionBase<SILInstructionKind::RetainValueAddrInst,
                                  RefCountingInst> {
  friend SILBuilder;

  RetainValueAddrInst(SILDebugLocation DebugLoc, SILValue operand,
                      Atomicity atomicity)
      : UnaryInstructionBase(DebugLoc, operand) {
    setAtomicity(atomicity);
  }
};

/// ReleaseValueInst - Destroys a loadable value.
class ReleaseValueInst
    : public UnaryInstructionBase<SILInstructionKind::ReleaseValueInst,
                                  RefCountingInst> {
  friend SILBuilder;

  ReleaseValueInst(SILDebugLocation DebugLoc, SILValue operand,
                   Atomicity atomicity)
      : UnaryInstructionBase(DebugLoc, operand) {
    setAtomicity(atomicity);
  }
};

/// ReleaseValueInst - Destroys a loadable value by address.
class ReleaseValueAddrInst
    : public UnaryInstructionBase<SILInstructionKind::ReleaseValueAddrInst,
                                  RefCountingInst> {
  friend SILBuilder;

  ReleaseValueAddrInst(SILDebugLocation DebugLoc, SILValue operand,
                       Atomicity atomicity)
      : UnaryInstructionBase(DebugLoc, operand) {
    setAtomicity(atomicity);
  }
};

/// Copies a loadable value in an unmanaged, unbalanced way. Only meant for use
/// in ownership qualified SIL. Please do not use this EVER unless you are
/// implementing a part of the stdlib called Unmanaged.
class UnmanagedRetainValueInst
    : public UnaryInstructionBase<SILInstructionKind::UnmanagedRetainValueInst,
                                  RefCountingInst> {
  friend SILBuilder;

  UnmanagedRetainValueInst(SILDebugLocation DebugLoc, SILValue operand,
                           Atomicity atomicity)
      : UnaryInstructionBase(DebugLoc, operand) {
    setAtomicity(atomicity);
  }
};

/// Destroys a loadable value in an unmanaged, unbalanced way. Only meant for
/// use in ownership qualified SIL. Please do not use this EVER unless you are
/// implementing a part of the stdlib called Unmanaged.
class UnmanagedReleaseValueInst
    : public UnaryInstructionBase<SILInstructionKind::UnmanagedReleaseValueInst,
                                  RefCountingInst> {
  friend SILBuilder;

  UnmanagedReleaseValueInst(SILDebugLocation DebugLoc, SILValue operand,
                            Atomicity atomicity)
      : UnaryInstructionBase(DebugLoc, operand) {
    setAtomicity(atomicity);
  }
};

/// Transfers ownership of a loadable value to the current autorelease
/// pool. Unmanaged, so it is ignored from an ownership balancing perspective.
class UnmanagedAutoreleaseValueInst
    : public UnaryInstructionBase<SILInstructionKind::UnmanagedAutoreleaseValueInst,
                                  RefCountingInst> {
  friend SILBuilder;

  UnmanagedAutoreleaseValueInst(SILDebugLocation DebugLoc, SILValue operand,
                                Atomicity atomicity)
      : UnaryInstructionBase(DebugLoc, operand) {
    setAtomicity(atomicity);
  }
};

/// Transfers ownership of a loadable value to the current autorelease pool.
class AutoreleaseValueInst
    : public UnaryInstructionBase<SILInstructionKind::AutoreleaseValueInst,
                                  RefCountingInst> {
  friend SILBuilder;

  AutoreleaseValueInst(SILDebugLocation DebugLoc, SILValue operand,
                       Atomicity atomicity)
      : UnaryInstructionBase(DebugLoc, operand) {
    setAtomicity(atomicity);
  }
};

/// SetDeallocatingInst - Sets the operand in deallocating state.
///
/// This is the same operation what's done by a strong_release immediately
/// before it calls the deallocator of the object.
class SetDeallocatingInst
    : public UnaryInstructionBase<SILInstructionKind::SetDeallocatingInst,
                                  RefCountingInst> {
  friend SILBuilder;

  SetDeallocatingInst(SILDebugLocation DebugLoc, SILValue operand,
                      Atomicity atomicity)
      : UnaryInstructionBase(DebugLoc, operand) {
    setAtomicity(atomicity);
  }
};

/// ObjectInst - Represents a object value type.
///
/// This instruction can only appear at the end of a global variable's
/// static initializer list.
class ObjectInst final : public InstructionBaseWithTrailingOperands<
                             SILInstructionKind::ObjectInst, ObjectInst,
                             OwnershipForwardingSingleValueInstruction> {
  friend SILBuilder;

  unsigned numBaseElements;

  /// Because of the storage requirements of ObjectInst, object
  /// creation goes through 'create()'.
  ObjectInst(SILDebugLocation DebugLoc, SILType Ty, ArrayRef<SILValue> Elements,
             unsigned NumBaseElements,
             ValueOwnershipKind forwardingOwnershipKind)
      : InstructionBaseWithTrailingOperands(Elements, DebugLoc, Ty,
                                            forwardingOwnershipKind),
        numBaseElements(NumBaseElements) {}

  /// Construct an ObjectInst.
  static ObjectInst *create(SILDebugLocation DebugLoc, SILType Ty,
                            ArrayRef<SILValue> Elements,
                            unsigned NumBaseElements, SILModule &M,
                            ValueOwnershipKind forwardingOwnershipKind);

public:
  unsigned getNumBaseElements() const { return numBaseElements; }

  /// All elements referenced by this ObjectInst.
  MutableArrayRef<Operand> getElementOperands() {
    return getAllOperands();
  }

  /// All elements referenced by this ObjectInst.
  OperandValueArrayRef getAllElements() const {
    return OperandValueArrayRef(getAllOperands());
  }

  /// The elements which initialize the stored properties of the object itself.
  OperandValueArrayRef getBaseElements() const {
    return OperandValueArrayRef(getAllOperands().slice(0, numBaseElements));
  }

  /// The elements which initialize the tail allocated elements.
  OperandValueArrayRef getTailElements() const {
    return OperandValueArrayRef(getAllOperands().slice(numBaseElements));
  }
};

/// TupleInst - Represents a constructed loadable tuple.
class TupleInst final : public InstructionBaseWithTrailingOperands<
                            SILInstructionKind::TupleInst, TupleInst,
                            OwnershipForwardingSingleValueInstruction> {
  friend SILBuilder;

  /// Because of the storage requirements of TupleInst, object
  /// creation goes through 'create()'.
  TupleInst(SILDebugLocation DebugLoc, SILType Ty, ArrayRef<SILValue> Elems,
            ValueOwnershipKind forwardingOwnershipKind)
      : InstructionBaseWithTrailingOperands(Elems, DebugLoc, Ty,
                                            forwardingOwnershipKind) {}

  /// Construct a TupleInst.
  static TupleInst *create(SILDebugLocation DebugLoc, SILType Ty,
                           ArrayRef<SILValue> Elements, SILModule &M,
                           ValueOwnershipKind forwardingOwnershipKind);

public:
  /// The elements referenced by this TupleInst.
  MutableArrayRef<Operand> getElementOperands() {
    return getAllOperands();
  }

  /// The elements referenced by this TupleInst.
  OperandValueArrayRef getElements() const {
    return OperandValueArrayRef(getAllOperands());
  }

  /// Return the i'th value referenced by this TupleInst.
  SILValue getElement(unsigned i) const {
    return getElements()[i];
  }

  unsigned getElementIndex(Operand *operand) {
    assert(operand->getUser() == this);
    return operand->getOperandNumber();
  }

  TupleType *getTupleType() const {
    return getType().castTo<TupleType>();
  }

  /// Search the operands of this tuple for a unique non-trivial elt. If we find
  /// it, return it. Otherwise return SILValue().
  SILValue getUniqueNonTrivialElt() {
    auto *F = getFunction();
    ArrayRef<Operand> Ops = getAllOperands();

    Optional<unsigned> Index;
    // For each operand...
    for (unsigned i = 0, e = Ops.size(); i != e; ++i) {
      // If the operand is not trivial...
      if (!Ops[i].get()->getType().isTrivial(*F)) {
        // And we have not found an Index yet, set index to i and continue.
        if (!Index.has_value()) {
          Index = i;
          continue;
        }

        // Otherwise, we have two values that are non-trivial. Bail.
        return SILValue();
      }
    }

    // If we did not find an index, return an empty SILValue.
    if (!Index.has_value())
      return SILValue();

    // Otherwise, return the value associated with index.
    return Ops[Index.value()].get();
  }
};

/// Represents a loadable enum constructed from one of its
/// elements.
class EnumInst
    : public InstructionBase<SILInstructionKind::EnumInst,
                             OwnershipForwardingSingleValueInstruction> {
  friend SILBuilder;
  enum : unsigned { InvalidCaseIndex = ~unsigned(0) };

  Optional<FixedOperandList<1>> OptionalOperand;
  EnumElementDecl *Element;
  USE_SHARED_UINT32;

  EnumInst(SILDebugLocation DebugLoc, SILValue Operand,
           EnumElementDecl *Element, SILType ResultTy,
           ValueOwnershipKind forwardingOwnershipKind)
      : InstructionBase(DebugLoc, ResultTy, forwardingOwnershipKind),
        Element(Element) {
    sharedUInt32().EnumInst.caseIndex = InvalidCaseIndex;

    if (Operand) {
      OptionalOperand.emplace(this, Operand);
    }
  }

public:
  EnumElementDecl *getElement() const { return Element; }

  unsigned getCaseIndex() {
    unsigned idx = sharedUInt32().EnumInst.caseIndex;
    if (idx != InvalidCaseIndex)
      return idx;

    unsigned index = getCachedCaseIndex(getElement());
    sharedUInt32().EnumInst.caseIndex = index;
    return index;
  }

  bool hasOperand() const { return OptionalOperand.has_value(); }
  SILValue getOperand() const { return OptionalOperand->asValueArray()[0]; }

  Operand &getOperandRef() { return OptionalOperand->asArray()[0]; }

  const Operand &getOperandRef() const { return OptionalOperand->asArray()[0]; }

  ArrayRef<Operand> getAllOperands() const {
    return OptionalOperand ? OptionalOperand->asArray() : ArrayRef<Operand>{};
  }

  MutableArrayRef<Operand> getAllOperands() {
    return OptionalOperand
      ? OptionalOperand->asArray() : MutableArrayRef<Operand>{};
  }
};

/// Unsafely project the data for an enum case out of an enum without checking
/// the tag.
class UncheckedEnumDataInst
    : public UnaryInstructionBase<SILInstructionKind::UncheckedEnumDataInst,
                                  OwnershipForwardingSingleValueInstruction> {
  friend SILBuilder;
  enum : unsigned { InvalidCaseIndex = ~unsigned(0) };

  EnumElementDecl *Element;
  USE_SHARED_UINT32;

  UncheckedEnumDataInst(SILDebugLocation DebugLoc, SILValue Operand,
                        EnumElementDecl *Element, SILType ResultTy,
                        ValueOwnershipKind forwardingOwnershipKind)
      : UnaryInstructionBase(DebugLoc, Operand, ResultTy,
                             forwardingOwnershipKind),
        Element(Element) {
    sharedUInt32().UncheckedEnumDataInst.caseIndex = InvalidCaseIndex;
  }

public:
  EnumElementDecl *getElement() const { return Element; }

  unsigned getCaseIndex() {
    unsigned idx = sharedUInt32().UncheckedEnumDataInst.caseIndex;
    if (idx != InvalidCaseIndex)
      return idx;

    unsigned index = getCachedCaseIndex(getElement());
    sharedUInt32().UncheckedEnumDataInst.caseIndex = index;
    return index;
  }

  EnumDecl *getEnumDecl() const {
    auto *E = getOperand()->getType().getEnumOrBoundGenericEnum();
    assert(E && "Operand of unchecked_enum_data must be of enum type");
    return E;
  }

  unsigned getElementNo() const {
    unsigned i = 0;
    for (EnumElementDecl *E : getEnumDecl()->getAllElements()) {
      if (E == Element)
        return i;
      ++i;
    }
    llvm_unreachable("An unchecked_enum_data's enumdecl should have at least "
                     "on element, the element that is being extracted");
  }
};

/// Projects the address of the data for a case inside an uninitialized enum in
/// order to initialize the payload for that case.
class InitEnumDataAddrInst
  : public UnaryInstructionBase<SILInstructionKind::InitEnumDataAddrInst,
                                SingleValueInstruction>
{
  friend SILBuilder;
  enum : unsigned { InvalidCaseIndex = ~unsigned(0) };

  EnumElementDecl *Element;
  USE_SHARED_UINT32;

  InitEnumDataAddrInst(SILDebugLocation DebugLoc, SILValue Operand,
                       EnumElementDecl *Element, SILType ResultTy)
      : UnaryInstructionBase(DebugLoc, Operand, ResultTy), Element(Element) {
    sharedUInt32().InitEnumDataAddrInst.caseIndex = InvalidCaseIndex;
  }

public:
  EnumElementDecl *getElement() const { return Element; }

  unsigned getCaseIndex() {
    unsigned idx = sharedUInt32().InitEnumDataAddrInst.caseIndex;
    if (idx != InvalidCaseIndex)
      return idx;

    unsigned index = getCachedCaseIndex(getElement());
    sharedUInt32().InitEnumDataAddrInst.caseIndex = index;
    return index;
  }
};

/// InjectEnumAddrInst - Tags an enum as containing a case. The data for
/// that case, if any, must have been written into the enum first.
class InjectEnumAddrInst
  : public UnaryInstructionBase<SILInstructionKind::InjectEnumAddrInst,
                                NonValueInstruction>
{
  friend SILBuilder;
  enum : unsigned { InvalidCaseIndex = ~unsigned(0) };

  EnumElementDecl *Element;
  USE_SHARED_UINT32;

  InjectEnumAddrInst(SILDebugLocation DebugLoc, SILValue Operand,
                     EnumElementDecl *Element)
      : UnaryInstructionBase(DebugLoc, Operand), Element(Element) {
    sharedUInt32().InjectEnumAddrInst.caseIndex = InvalidCaseIndex;
  }

public:
  EnumElementDecl *getElement() const { return Element; }

  unsigned getCaseIndex() {
    unsigned idx = sharedUInt32().InjectEnumAddrInst.caseIndex;
    if (idx != InvalidCaseIndex)
      return idx;

    unsigned index = getCachedCaseIndex(getElement());
    sharedUInt32().InjectEnumAddrInst.caseIndex = index;
    return index;
  }
};

/// Invalidate an enum value and take ownership of its payload data
/// without moving it in memory.
class UncheckedTakeEnumDataAddrInst
  : public UnaryInstructionBase<SILInstructionKind::UncheckedTakeEnumDataAddrInst,
                                SingleValueInstruction>
{
  friend SILBuilder;
  enum : unsigned { InvalidCaseIndex = ~unsigned(0) };

  EnumElementDecl *Element;
  USE_SHARED_UINT32;

  UncheckedTakeEnumDataAddrInst(SILDebugLocation DebugLoc, SILValue Operand,
                                EnumElementDecl *Element, SILType ResultTy)
      : UnaryInstructionBase(DebugLoc, Operand, ResultTy), Element(Element) {
    sharedUInt32().UncheckedTakeEnumDataAddrInst.caseIndex = InvalidCaseIndex;
  }

public:
  EnumElementDecl *getElement() const { return Element; }

  unsigned getCaseIndex() {
    unsigned idx = sharedUInt32().UncheckedTakeEnumDataAddrInst.caseIndex;
    if (idx != InvalidCaseIndex)
      return idx;

    unsigned index = getCachedCaseIndex(getElement());
    sharedUInt32().UncheckedTakeEnumDataAddrInst.caseIndex = index;
    return index;
  }

  EnumDecl *getEnumDecl() const {
    auto *E = getOperand()->getType().getEnumOrBoundGenericEnum();
    assert(E && "Operand of unchecked_take_enum_data_addr must be of enum"
                " type");
    return E;
  }
};

// Abstract base class of all select instructions like select_enum.
// The template parameter represents a type of case values
// to be compared with the operand of a select instruction.
//
// Subclasses must provide tail allocated storage.
// The first operand is the operand of select_xxx instruction. The rest of
// the operands are the case values and results of a select instruction.
template <class Derived, class T, class Base = SingleValueInstruction>
class SelectInstBase : public Base {
public:
  template <typename... Args>
  SelectInstBase(SILInstructionKind kind, SILDebugLocation Loc, SILType type,
                 Args &&... otherArgs)
      : Base(kind, Loc, type, std::forward<Args>(otherArgs)...) {}

  SILValue getOperand() const { return getAllOperands()[0].get(); }

  ArrayRef<Operand> getAllOperands() const {
    return static_cast<const Derived *>(this)->getAllOperands();
  }
  MutableArrayRef<Operand> getAllOperands() {
    return static_cast<Derived *>(this)->getAllOperands();
  }

  std::pair<T, SILValue> getCase(unsigned i) const {
    return static_cast<const Derived *>(this)->getCase(i);
  }

  unsigned getNumCases() const {
    return static_cast<const Derived *>(this)->getNumCases();
  }

  bool hasDefault() const {
    return static_cast<const Derived *>(this)->hasDefault();
  }

  SILValue getDefaultResult() const {
    return static_cast<const Derived *>(this)->getDefaultResult();
  }
};

/// Common base class for the select_enum and select_enum_addr instructions,
/// which select one of a set of possible results based on the case of an enum.
class SelectEnumInstBase
    : public SelectInstBase<SelectEnumInstBase, EnumElementDecl *> {
  USE_SHARED_UINT8;

  // Tail-allocated after the operands is an array of `NumCases`
  // EnumElementDecl* pointers, referencing the case discriminators for each
  // operand.
  
  EnumElementDecl **getEnumElementDeclStorage();
  EnumElementDecl * const* getEnumElementDeclStorage() const {
    return const_cast<SelectEnumInstBase*>(this)->getEnumElementDeclStorage();
  }

protected:
  SelectEnumInstBase(SILInstructionKind kind, SILDebugLocation debugLoc,
                     SILType type, bool defaultValue,
                     Optional<ArrayRef<ProfileCounter>> CaseCounts,
                     ProfileCounter DefaultCount)
      : SelectInstBase(kind, debugLoc, type) {
    sharedUInt8().SelectEnumInstBase.hasDefault = defaultValue;
  }
  template <typename SELECT_ENUM_INST>
  static SELECT_ENUM_INST *
  createSelectEnum(SILDebugLocation DebugLoc, SILValue Enum, SILType Type,
                   SILValue DefaultValue,
                   ArrayRef<std::pair<EnumElementDecl *, SILValue>> CaseValues,
                   SILModule &M, Optional<ArrayRef<ProfileCounter>> CaseCounts,
                   ProfileCounter DefaultCount,
                   ValueOwnershipKind forwardingOwnershipKind);

public:
  ArrayRef<Operand> getAllOperands() const;
  MutableArrayRef<Operand> getAllOperands();

  SILValue getOperand() const { return getAllOperands()[0].get(); }
  SILValue getEnumOperand() const { return getOperand(); }
  const Operand &getEnumOperandRef() const { return getAllOperands()[0]; }

  std::pair<EnumElementDecl*, SILValue>
  getCase(unsigned i) const {
    return std::make_pair(getEnumElementDeclStorage()[i],
                          getAllOperands()[i+1].get());
  }

  /// Return the value that will be used as the result for the specified enum
  /// case.
  SILValue getCaseResult(EnumElementDecl *D) {
    for (unsigned i = 0, e = getNumCases(); i != e; ++i) {
      auto Entry = getCase(i);
      if (Entry.first == D) return Entry.second;
    }
    // select_enum is required to be fully covered, so return the default if we
    // didn't find anything.
    return getDefaultResult();
  }
  
  /// If the default refers to exactly one case decl, return it.
  NullablePtr<EnumElementDecl> getUniqueCaseForDefault();

  bool hasDefault() const {
    return sharedUInt8().SelectEnumInstBase.hasDefault;
  }

  SILValue getDefaultResult() const {
    assert(hasDefault() && "doesn't have a default");
    return getAllOperands().back().get();
  }

  unsigned getNumCases() const {
    return getAllOperands().size() - 1 - hasDefault();
  }

  /// If there is a single case that returns a literal "true" value (an
  /// "integer_literal $Builtin.Int1, 1" value), return it.
  ///
  /// FIXME: This is used to interoperate with passes that reasoned about the
  /// old enum_is_tag insn. Ideally those passes would become general enough
  /// not to need this.
  NullablePtr<EnumElementDecl> getSingleTrueElement() const;
};

/// A select enum inst that produces a static OwnershipKind.
class OwnershipForwardingSelectEnumInstBase : public SelectEnumInstBase,
                                              public ForwardingInstruction {
protected:
  OwnershipForwardingSelectEnumInstBase(
      SILInstructionKind kind, SILDebugLocation debugLoc, SILType type,
      bool defaultValue, Optional<ArrayRef<ProfileCounter>> caseCounts,
      ProfileCounter defaultCount, ValueOwnershipKind ownershipKind)
      : SelectEnumInstBase(kind, debugLoc, type, defaultValue, caseCounts,
                           defaultCount),
        ForwardingInstruction(kind, ownershipKind) {
    assert(classof(kind) && "classof missing subclass");
  }

public:
  static bool classof(SILNodePointer node) {
    if (auto *i = dyn_cast<SILInstruction>(node.get()))
      return classof(i);
    return false;
  }

  static bool classof(const SILInstruction *i) { return classof(i->getKind()); }

  static bool classof(SILInstructionKind kind) {
    switch (kind) {
    case SILInstructionKind::SelectEnumInst:
      return true;
    default:
      return false;
    }
  }
};

/// Select one of a set of values based on the case of an enum.
class SelectEnumInst final
    : public InstructionBaseWithTrailingOperands<
          SILInstructionKind::SelectEnumInst, SelectEnumInst,
          OwnershipForwardingSelectEnumInstBase, EnumElementDecl *> {
  friend SILBuilder;

private:
  friend SelectEnumInstBase;

  SelectEnumInst(SILDebugLocation DebugLoc, SILValue Operand, SILType Type,
                 bool DefaultValue, ArrayRef<SILValue> CaseValues,
                 ArrayRef<EnumElementDecl *> CaseDecls,
                 Optional<ArrayRef<ProfileCounter>> CaseCounts,
                 ProfileCounter DefaultCount,
                 ValueOwnershipKind forwardingOwnershipKind)
      : InstructionBaseWithTrailingOperands(
            Operand, CaseValues, DebugLoc, Type, bool(DefaultValue), CaseCounts,
            DefaultCount, forwardingOwnershipKind) {
    assert(CaseValues.size() - DefaultValue == CaseDecls.size());
    std::uninitialized_copy(CaseDecls.begin(), CaseDecls.end(),
                            getTrailingObjects<EnumElementDecl *>());
  }
  static SelectEnumInst *
  create(SILDebugLocation DebugLoc, SILValue Operand, SILType Type,
         SILValue DefaultValue,
         ArrayRef<std::pair<EnumElementDecl *, SILValue>> CaseValues,
         SILModule &M, Optional<ArrayRef<ProfileCounter>> CaseCounts,
         ProfileCounter DefaultCount,
         ValueOwnershipKind forwardingOwnershipKind);
};

/// Select one of a set of values based on the case of an enum.
class SelectEnumAddrInst final
    : public InstructionBaseWithTrailingOperands<
          SILInstructionKind::SelectEnumAddrInst, SelectEnumAddrInst,
          SelectEnumInstBase, EnumElementDecl *> {
  friend SILBuilder;
  friend SelectEnumInstBase;

  SelectEnumAddrInst(SILDebugLocation DebugLoc, SILValue Operand, SILType Type,
                     bool DefaultValue, ArrayRef<SILValue> CaseValues,
                     ArrayRef<EnumElementDecl *> CaseDecls,
                     Optional<ArrayRef<ProfileCounter>> CaseCounts,
                     ProfileCounter DefaultCount,
                     ValueOwnershipKind forwardingOwnershipKind)
      : InstructionBaseWithTrailingOperands(Operand, CaseValues, DebugLoc, Type,
                                            bool(DefaultValue), CaseCounts,
                                            DefaultCount) {
    (void)forwardingOwnershipKind;
    assert(CaseValues.size() - DefaultValue == CaseDecls.size());
    std::uninitialized_copy(CaseDecls.begin(), CaseDecls.end(),
                            getTrailingObjects<EnumElementDecl *>());
  }
  static SelectEnumAddrInst *
  create(SILDebugLocation DebugLoc, SILValue Operand, SILType Type,
         SILValue DefaultValue,
         ArrayRef<std::pair<EnumElementDecl *, SILValue>> CaseValues,
         SILModule &M, Optional<ArrayRef<ProfileCounter>> CaseCounts,
         ProfileCounter DefaultCount);
};

/// MetatypeInst - Represents the production of an instance of a given metatype
/// named statically.
class MetatypeInst final
    : public NullaryInstructionWithTypeDependentOperandsBase<
                                         SILInstructionKind::MetatypeInst,
                                         MetatypeInst, SingleValueInstruction> {
  friend SILBuilder;

  /// Constructs a MetatypeInst
  MetatypeInst(SILDebugLocation DebugLoc, SILType Metatype,
               ArrayRef<SILValue> TypeDependentOperands)
    : NullaryInstructionWithTypeDependentOperandsBase(DebugLoc,
                                          TypeDependentOperands, Metatype) {}

  static MetatypeInst *create(SILDebugLocation DebugLoc, SILType Metatype,
                              SILFunction *F);
};

/// Represents loading a dynamic metatype from a value.
class ValueMetatypeInst
  : public UnaryInstructionBase<SILInstructionKind::ValueMetatypeInst,
                                SingleValueInstruction>
{
  friend SILBuilder;

  ValueMetatypeInst(SILDebugLocation DebugLoc, SILType Metatype, SILValue Base)
      : UnaryInstructionBase(DebugLoc, Base, Metatype) {}
};

/// ExistentialMetatype - Represents loading a dynamic metatype from an
/// existential container.
class ExistentialMetatypeInst
  : public UnaryInstructionBase<SILInstructionKind::ExistentialMetatypeInst,
                                SingleValueInstruction>
{
  friend SILBuilder;

  ExistentialMetatypeInst(SILDebugLocation DebugLoc, SILType Metatype,
                          SILValue Base)
      : UnaryInstructionBase(DebugLoc, Base, Metatype) {}
};

/// Extract a numbered element out of a value of tuple type.
class TupleExtractInst
    : public UnaryInstructionBase<SILInstructionKind::TupleExtractInst,
                                  OwnershipForwardingSingleValueInstruction> {
  friend SILBuilder;
  USE_SHARED_UINT32;

  TupleExtractInst(SILDebugLocation DebugLoc, SILValue Operand,
                   unsigned FieldNo, SILType ResultTy,
                   ValueOwnershipKind forwardingOwnershipKind)
      : UnaryInstructionBase(DebugLoc, Operand, ResultTy,
                             forwardingOwnershipKind) {
    sharedUInt32().TupleExtractInst.fieldNo = FieldNo;
  }

public:
  unsigned getFieldIndex() const {
    return sharedUInt32().TupleExtractInst.fieldNo;
  }

  TupleType *getTupleType() const {
    return getOperand()->getType().castTo<TupleType>();
  }

  unsigned getNumTupleElts() const {
    return getTupleType()->getNumElements();
  }

  /// Returns true if this is a trivial result of a tuple that is non-trivial
  /// and represents one RCID.
  bool isTrivialEltOfOneRCIDTuple() const;
  bool isEltOnlyNonTrivialElt() const;
};

/// Derive the address of a numbered element from the address of a tuple.
class TupleElementAddrInst
  : public UnaryInstructionBase<SILInstructionKind::TupleElementAddrInst,
                                SingleValueInstruction>
{
  friend SILBuilder;
  USE_SHARED_UINT32;

  TupleElementAddrInst(SILDebugLocation DebugLoc, SILValue Operand,
                       unsigned FieldNo, SILType ResultTy)
      : UnaryInstructionBase(DebugLoc, Operand, ResultTy) {
    sharedUInt32().TupleElementAddrInst.fieldNo = FieldNo;
  }

public:
  unsigned getFieldIndex() const {
    return sharedUInt32().TupleElementAddrInst.fieldNo;
  }


  TupleType *getTupleType() const {
    return getOperand()->getType().castTo<TupleType>();
  }
};

unsigned getNumFieldsInNominal(NominalTypeDecl *decl);

/// Get the property for a struct or class by its unique index, or nullptr if
/// the index does not match a property declared in this struct or class or
/// one its superclasses.
///
/// Precondition: \p decl must be a non-resilient struct or class.
VarDecl *getIndexedField(NominalTypeDecl *decl, unsigned index);

/// A common base for instructions that require a cached field index.
///
/// "Field" is a term used here to refer to the ordered, accessible stored
/// properties of a class or struct.
///
/// The field's ordinal value is the basis of efficiently comparing and sorting
/// access paths in SIL. For example, whenever a Projection object is created,
/// it stores the field index. Finding the field index initially requires
/// searching the type declaration's array of all stored properties. If this
/// index is not cached, it will cause widespread quadratic complexity in any
/// pass that queries projections, including the SIL verifier.
///
/// FIXME: This cache may not be necessary if the Decl TypeChecker instead
/// caches a field index in the VarDecl itself. This solution would be superior
/// because it would allow constant time lookup of either the VarDecl or the
/// index from a single pointer without referring back to a projection
/// instruction.
template <typename ParentTy>
class FieldIndexCacheBase : public ParentTy {
  enum : unsigned { InvalidFieldIndex = ~unsigned(0) };

  VarDecl *field;
  TEMPLATE_USE_SHARED_UINT32(ParentTy);

public:
  template <typename... ArgTys>
  FieldIndexCacheBase(SILInstructionKind kind, SILDebugLocation loc,
                      SILType type, VarDecl *field, ArgTys &&... extraArgs)
      : ParentTy(kind, loc, type, std::forward<ArgTys>(extraArgs)...),
        field(field) {
    sharedUInt32().FieldIndexCacheBase.fieldIndex = InvalidFieldIndex;
    // This needs to be a concrete class to hold bitfield information. However,
    // it should only be extended by UnaryInstructions.
    assert(ParentTy::getNumOperands() == 1);
  }

  VarDecl *getField() const { return field; }

  unsigned getFieldIndex() {
    unsigned idx = sharedUInt32().FieldIndexCacheBase.fieldIndex;
    if (idx != InvalidFieldIndex)
      return idx;
      
    idx = ParentTy::getCachedFieldIndex(getParentDecl(), getField());
    sharedUInt32().FieldIndexCacheBase.fieldIndex = idx;
    return idx;
  }

  NominalTypeDecl *getParentDecl() const {
    auto s =
        ParentTy::getOperand(0)->getType().getNominalOrBoundGenericNominal();
    assert(s);
    return s;
  }

  static bool classof(SILNodePointer node) {
    SILNodeKind kind = node->getKind();
    return kind == SILNodeKind::StructExtractInst ||
           kind == SILNodeKind::StructElementAddrInst ||
           kind == SILNodeKind::RefElementAddrInst;
  }
};

/// Extract a physical, fragile field out of a value of struct type.
class StructExtractInst
    : public UnaryInstructionBase<
          SILInstructionKind::StructExtractInst,
          FieldIndexCacheBase<OwnershipForwardingSingleValueInstruction>> {
  friend SILBuilder;

  StructExtractInst(SILDebugLocation DebugLoc, SILValue Operand, VarDecl *Field,
                    SILType ResultTy,
                    ValueOwnershipKind forwardingOwnershipKind)
      : UnaryInstructionBase(DebugLoc, Operand, ResultTy, Field,
                             forwardingOwnershipKind) {}

public:
  StructDecl *getStructDecl() const {
    return cast<StructDecl>(getParentDecl());
  }

  /// Returns true if this is a trivial result of a struct that is non-trivial
  /// and represents one RCID.
  bool isTrivialFieldOfOneRCIDStruct() const;

  /// Return true if we are extracting the only non-trivial field of out parent
  /// struct. This implies that a ref count operation on the aggregate is
  /// equivalent to a ref count operation on this field.
  bool isFieldOnlyNonTrivialField() const;
};

/// Derive the address of a physical field from the address of a struct.
class StructElementAddrInst
    : public UnaryInstructionBase<SILInstructionKind::StructElementAddrInst,
                                  FieldIndexCacheBase<SingleValueInstruction>> {
  friend SILBuilder;

  StructElementAddrInst(SILDebugLocation DebugLoc, SILValue Operand,
                        VarDecl *Field, SILType ResultTy)
      : UnaryInstructionBase(DebugLoc, Operand, ResultTy, Field) {}

public:
  StructDecl *getStructDecl() const {
    return cast<StructDecl>(getParentDecl());
  }
};

/// RefElementAddrInst - Derive the address of a named element in a reference
/// type instance.
class RefElementAddrInst
    : public UnaryInstructionBase<SILInstructionKind::RefElementAddrInst,
                                  FieldIndexCacheBase<SingleValueInstruction>> {
  friend SILBuilder;
  USE_SHARED_UINT8;

  RefElementAddrInst(SILDebugLocation DebugLoc, SILValue Operand,
                     VarDecl *Field, SILType ResultTy, bool IsImmutable)
      : UnaryInstructionBase(DebugLoc, Operand, ResultTy, Field) {
    setImmutable(IsImmutable);
  }

public:
  ClassDecl *getClassDecl() const { return cast<ClassDecl>(getParentDecl()); }

  /// Returns true if all loads of the same instance variable from the same
  /// class reference operand are guaranteed to yield the same value.
  bool isImmutable() const {
    return sharedUInt8().RefElementAddrInst.immutable;
  }

  /// Sets the immutable flag.
  void setImmutable(bool immutable = true) {
    sharedUInt8().RefElementAddrInst.immutable = immutable;
  }
};

/// RefTailAddrInst - Derive the address of the first element of the first
/// tail-allocated array in a reference type instance.
class RefTailAddrInst
  : public UnaryInstructionBase<SILInstructionKind::RefTailAddrInst,
                                SingleValueInstruction>
{
  friend SILBuilder;
  USE_SHARED_UINT8;

  RefTailAddrInst(SILDebugLocation DebugLoc, SILValue Operand, SILType ResultTy,
                  bool IsImmutable)
      : UnaryInstructionBase(DebugLoc, Operand, ResultTy) {
    setImmutable(IsImmutable);
  }

public:
  ClassDecl *getClassDecl() const {
    auto s = getOperand()->getType().getClassOrBoundGenericClass();
    assert(s);
    return s;
  }

  SILType getTailType() const { return getType().getObjectType(); }

  /// Returns true if all loads of the same instance variable from the same
  /// class reference operand are guaranteed to yield the same value.
  bool isImmutable() const {
    return sharedUInt8().RefTailAddrInst.immutable;
  }

  /// Sets the immutable flag.
  void setImmutable(bool immutable = true) {
    sharedUInt8().RefTailAddrInst.immutable = immutable;
  }
};

/// MethodInst - Abstract base for instructions that implement dynamic
/// method lookup.
class MethodInst : public SingleValueInstruction {
  SILDeclRef Member;
public:
  MethodInst(SILInstructionKind Kind, SILDebugLocation DebugLoc, SILType Ty,
             SILDeclRef Member)
      : SingleValueInstruction(Kind, DebugLoc, Ty), Member(Member) {
  }

  SILDeclRef getMember() const { return Member; }

  DEFINE_ABSTRACT_SINGLE_VALUE_INST_BOILERPLATE(MethodInst)
};

/// ClassMethodInst - Given the address of a value of class type and a method
/// constant, extracts the implementation of that method for the dynamic
/// instance type of the class.
class ClassMethodInst
    : public UnaryInstructionBase<SILInstructionKind::ClassMethodInst,
                                  MethodInst>
{
  friend SILBuilder;

  ClassMethodInst(SILDebugLocation DebugLoc, SILValue Operand,
                  SILDeclRef Member, SILType Ty)
      : UnaryInstructionBase(DebugLoc, Operand, Ty, Member) {}
};

/// SuperMethodInst - Given the address of a value of class type and a method
/// constant, extracts the implementation of that method for the superclass of
/// the static type of the class.
class SuperMethodInst
  : public UnaryInstructionBase<SILInstructionKind::SuperMethodInst, MethodInst>
{
  friend SILBuilder;

  SuperMethodInst(SILDebugLocation DebugLoc, SILValue Operand,
                  SILDeclRef Member, SILType Ty)
      : UnaryInstructionBase(DebugLoc, Operand, Ty, Member) {}
};

/// ObjCMethodInst - Given the address of a value of class type and a method
/// constant, extracts the implementation of that method for the dynamic
/// instance type of the class.
class ObjCMethodInst final
    : public UnaryInstructionWithTypeDependentOperandsBase<
          SILInstructionKind::ObjCMethodInst,
          ObjCMethodInst,
          MethodInst>
{
  friend SILBuilder;

  ObjCMethodInst(SILDebugLocation DebugLoc, SILValue Operand,
                 ArrayRef<SILValue> TypeDependentOperands,
                 SILDeclRef Member, SILType Ty)
      : UnaryInstructionWithTypeDependentOperandsBase(DebugLoc, Operand,
                               TypeDependentOperands, Ty, Member) {}

  static ObjCMethodInst *
  create(SILDebugLocation DebugLoc, SILValue Operand,
         SILDeclRef Member, SILType Ty, SILFunction *F);
};

/// ObjCSuperMethodInst - Given the address of a value of class type and a method
/// constant, extracts the implementation of that method for the superclass of
/// the static type of the class.
class ObjCSuperMethodInst
  : public UnaryInstructionBase<SILInstructionKind::ObjCSuperMethodInst, MethodInst>
{
  friend SILBuilder;

  ObjCSuperMethodInst(SILDebugLocation DebugLoc, SILValue Operand,
                      SILDeclRef Member, SILType Ty)
      : UnaryInstructionBase(DebugLoc, Operand, Ty, Member) {}
};

/// WitnessMethodInst - Given a type, a protocol conformance,
/// and a protocol method constant, extracts the implementation of that method
/// for the type.
class WitnessMethodInst final
    : public NullaryInstructionWithTypeDependentOperandsBase<
                                          SILInstructionKind::WitnessMethodInst,
                                          WitnessMethodInst, MethodInst> {
  friend SILBuilder;

  CanType LookupType;
  ProtocolConformanceRef Conformance;

  WitnessMethodInst(SILDebugLocation DebugLoc, CanType LookupType,
                    ProtocolConformanceRef Conformance, SILDeclRef Member,
                    SILType Ty, ArrayRef<SILValue> TypeDependentOperands)
      : NullaryInstructionWithTypeDependentOperandsBase(DebugLoc,
                                          TypeDependentOperands, Ty, Member),
        LookupType(LookupType), Conformance(Conformance) {}

  /// Create a witness method call of a protocol requirement, passing in a lookup
  /// type and conformance.
  ///
  /// At runtime, the witness is looked up in the conformance of the lookup type
  /// to the protocol.
  ///
  /// The lookup type is usually an archetype, but it will be concrete if the
  /// witness_method instruction is inside a function body that was specialized.
  ///
  /// The conformance must exactly match the requirement; the caller must handle
  /// the case where the requirement is defined in a base protocol that is
  /// refined by the conforming protocol.
  static WitnessMethodInst *
  create(SILDebugLocation DebugLoc, CanType LookupType,
         ProtocolConformanceRef Conformance, SILDeclRef Member, SILType Ty,
         SILFunction *Parent);

public:
  CanType getLookupType() const { return LookupType; }
  ProtocolDecl *getLookupProtocol() const {
    return getMember().getDecl()->getDeclContext()->getSelfProtocolDecl();
  }

  ProtocolConformanceRef getConformance() const { return Conformance; }
};

/// Access allowed to the opened value by the open_existential_addr instruction.
/// Allowing mutable access to the opened existential requires a boxed
/// existential value's box to be unique.
enum class OpenedExistentialAccess { Immutable, Mutable };

OpenedExistentialAccess getOpenedExistentialAccessFor(AccessKind access);

/// Given the address of an existential, "opens" the
/// existential by returning a pointer to a fresh archetype T, which also
/// captures the (dynamic) conformances.
class OpenExistentialAddrInst
  : public UnaryInstructionBase<SILInstructionKind::OpenExistentialAddrInst,
                                SingleValueInstruction>
{
  friend SILBuilder;
  OpenedExistentialAccess ForAccess;

  OpenExistentialAddrInst(SILDebugLocation DebugLoc, SILValue Operand,
                          SILType SelfTy, OpenedExistentialAccess AccessKind);

public:
  static bool isRead(SILInstruction *inst) {
    auto *open = dyn_cast<OpenExistentialAddrInst>(inst);
    return open && open->getAccessKind() == OpenedExistentialAccess::Immutable;
  }

  OpenedExistentialAccess getAccessKind() const { return ForAccess; }

  CanOpenedArchetypeType getDefinedOpenedArchetype() const {
    const auto archetype = getOpenedArchetypeOf(getType().getASTType());
    assert(archetype && archetype->isRoot() &&
           "Type should be a root opened archetype");
    return archetype;
  }
};

/// Given an opaque value referring to an existential, "opens" the
/// existential by returning a pointer to a fresh archetype T, which also
/// captures the (dynamic) conformances.
class OpenExistentialValueInst
    : public UnaryInstructionBase<SILInstructionKind::OpenExistentialValueInst,
                                  OwnershipForwardingSingleValueInstruction> {
  friend SILBuilder;

  OpenExistentialValueInst(SILDebugLocation debugLoc, SILValue operand,
                           SILType selfTy,
                           ValueOwnershipKind forwardingOwnershipKind);

public:
  CanOpenedArchetypeType getDefinedOpenedArchetype() const {
    const auto archetype = getOpenedArchetypeOf(getType().getASTType());
    assert(archetype && archetype->isRoot() &&
           "Type should be a root opened archetype");
    return archetype;
  }
};

/// Given a class existential, "opens" the
/// existential by returning a pointer to a fresh archetype T, which also
/// captures the (dynamic) conformances.
class OpenExistentialRefInst
    : public UnaryInstructionBase<SILInstructionKind::OpenExistentialRefInst,
                                  OwnershipForwardingSingleValueInstruction> {
  friend SILBuilder;

  OpenExistentialRefInst(SILDebugLocation DebugLoc, SILValue Operand,
                         SILType Ty,
                         ValueOwnershipKind forwardingOwnershipKind);

public:
  CanOpenedArchetypeType getDefinedOpenedArchetype() const {
    const auto archetype = getOpenedArchetypeOf(getType().getASTType());
    assert(archetype && archetype->isRoot() &&
           "Type should be a root opened archetype");
    return archetype;
  }
};

/// Given an existential metatype,
/// "opens" the existential by returning a pointer to a fresh
/// archetype metatype T.Type, which also captures the (dynamic)
/// conformances.
class OpenExistentialMetatypeInst
  : public UnaryInstructionBase<SILInstructionKind::OpenExistentialMetatypeInst,
                                SingleValueInstruction>
{
  friend SILBuilder;

  OpenExistentialMetatypeInst(SILDebugLocation DebugLoc, SILValue operand,
                              SILType ty);

public:
  CanOpenedArchetypeType getDefinedOpenedArchetype() const {
    const auto archetype = getOpenedArchetypeOf(getType().getASTType());
    assert(archetype && archetype->isRoot() &&
           "Type should be a root opened archetype");
    return archetype;
  }
};

/// Given a boxed existential container,
/// "opens" the existential by returning a pointer to a fresh
/// archetype T, which also captures the (dynamic) conformances.
class OpenExistentialBoxInst
  : public UnaryInstructionBase<SILInstructionKind::OpenExistentialBoxInst,
                                SingleValueInstruction>
{
  friend SILBuilder;

  OpenExistentialBoxInst(SILDebugLocation DebugLoc, SILValue operand,
                         SILType ty);

public:
  CanOpenedArchetypeType getDefinedOpenedArchetype() const {
    const auto archetype = getOpenedArchetypeOf(getType().getASTType());
    assert(archetype && archetype->isRoot() &&
           "Type should be a root opened archetype");
    return archetype;
  }
};

/// Given a boxed existential container, "opens" the existential by returning a
/// fresh archetype T, which also captures the (dynamic) conformances.
class OpenExistentialBoxValueInst
    : public UnaryInstructionBase<
          SILInstructionKind::OpenExistentialBoxValueInst,
          OwnershipForwardingSingleValueInstruction> {
  friend SILBuilder;

  OpenExistentialBoxValueInst(SILDebugLocation DebugLoc, SILValue operand,
                              SILType ty,
                              ValueOwnershipKind forwardingOwnershipKind);

public:
  CanOpenedArchetypeType getDefinedOpenedArchetype() const {
    const auto archetype = getOpenedArchetypeOf(getType().getASTType());
    assert(archetype && archetype->isRoot() &&
           "Type should be a root opened archetype");
    return archetype;
  }
};

/// Given an address to an uninitialized buffer of
/// a protocol type, initializes its existential container to contain a concrete
/// value of the given type, and returns the address of the uninitialized
/// concrete value inside the existential container.
class InitExistentialAddrInst final
  : public UnaryInstructionWithTypeDependentOperandsBase<
                                SILInstructionKind::InitExistentialAddrInst,
                                InitExistentialAddrInst,
                                SingleValueInstruction>
{
  friend SILBuilder;

  CanType ConcreteType;
  ArrayRef<ProtocolConformanceRef> Conformances;

  InitExistentialAddrInst(SILDebugLocation DebugLoc, SILValue Existential,
                          ArrayRef<SILValue> TypeDependentOperands,
                          CanType ConcreteType, SILType ConcreteLoweredType,
                          ArrayRef<ProtocolConformanceRef> Conformances)
      : UnaryInstructionWithTypeDependentOperandsBase(DebugLoc, Existential,
                             TypeDependentOperands,
                             ConcreteLoweredType.getAddressType()),
        ConcreteType(ConcreteType), Conformances(Conformances) {}

  static InitExistentialAddrInst *
  create(SILDebugLocation DebugLoc, SILValue Existential, CanType ConcreteType,
         SILType ConcreteLoweredType,
         ArrayRef<ProtocolConformanceRef> Conformances, SILFunction *Parent);

public:
  ArrayRef<ProtocolConformanceRef> getConformances() const {
    return Conformances;
  }
  
  CanType getFormalConcreteType() const {
    return ConcreteType;
  }

  SILType getLoweredConcreteType() const {
    return getType();
  }
};

/// Given an uninitialized buffer of a protocol type,
/// initializes its existential container to contain a concrete
/// value of the given type, and returns the uninitialized
/// concrete value inside the existential container.
class InitExistentialValueInst final
    : public UnaryInstructionWithTypeDependentOperandsBase<
          SILInstructionKind::InitExistentialValueInst, InitExistentialValueInst,
          SingleValueInstruction> {
  friend SILBuilder;

  CanType ConcreteType;
  ArrayRef<ProtocolConformanceRef> Conformances;

  InitExistentialValueInst(SILDebugLocation DebugLoc, SILType ExistentialType,
                            CanType FormalConcreteType, SILValue Instance,
                            ArrayRef<SILValue> TypeDependentOperands,
                            ArrayRef<ProtocolConformanceRef> Conformances)
      : UnaryInstructionWithTypeDependentOperandsBase(
            DebugLoc, Instance, TypeDependentOperands, ExistentialType),
        ConcreteType(FormalConcreteType), Conformances(Conformances) {}

  static InitExistentialValueInst *
  create(SILDebugLocation DebugLoc, SILType ExistentialType,
         CanType ConcreteType, SILValue Instance,
         ArrayRef<ProtocolConformanceRef> Conformances, SILFunction *Parent);

public:
  CanType getFormalConcreteType() const { return ConcreteType; }

  ArrayRef<ProtocolConformanceRef> getConformances() const {
    return Conformances;
  }
};

/// InitExistentialRefInst - Given a class instance reference and a set of
/// conformances, creates a class existential value referencing the
/// class instance.
class InitExistentialRefInst final
    : public UnaryInstructionWithTypeDependentOperandsBase<
          SILInstructionKind::InitExistentialRefInst, InitExistentialRefInst,
          OwnershipForwardingSingleValueInstruction> {
  friend SILBuilder;

  CanType ConcreteType;
  ArrayRef<ProtocolConformanceRef> Conformances;

  InitExistentialRefInst(SILDebugLocation DebugLoc, SILType ExistentialType,
                         CanType FormalConcreteType, SILValue Instance,
                         ArrayRef<SILValue> TypeDependentOperands,
                         ArrayRef<ProtocolConformanceRef> Conformances,
                         ValueOwnershipKind forwardingOwnershipKind)
      : UnaryInstructionWithTypeDependentOperandsBase(
            DebugLoc, Instance, TypeDependentOperands, ExistentialType,
            forwardingOwnershipKind),
        ConcreteType(FormalConcreteType), Conformances(Conformances) {}

  static InitExistentialRefInst *
  create(SILDebugLocation DebugLoc, SILType ExistentialType,
         CanType ConcreteType, SILValue Instance,
         ArrayRef<ProtocolConformanceRef> Conformances, SILFunction *Parent,
         ValueOwnershipKind forwardingOwnershipKind);

public:
  CanType getFormalConcreteType() const {
    return ConcreteType;
  }

  ArrayRef<ProtocolConformanceRef> getConformances() const {
    return Conformances;
  }
};

/// InitExistentialMetatypeInst - Given a metatype reference and a set
/// of conformances, creates an existential metatype value referencing
/// the metatype.
class InitExistentialMetatypeInst final
  : public UnaryInstructionWithTypeDependentOperandsBase<
                                  SILInstructionKind::InitExistentialMetatypeInst,
                                  InitExistentialMetatypeInst,
                                  SingleValueInstruction,
                                  ProtocolConformanceRef>
{
  friend SILBuilder;

  unsigned NumConformances;

  InitExistentialMetatypeInst(SILDebugLocation DebugLoc,
                              SILType existentialMetatypeType,
                              SILValue metatype,
                              ArrayRef<SILValue> TypeDependentOperands,
                              ArrayRef<ProtocolConformanceRef> conformances);

  static InitExistentialMetatypeInst *
  create(SILDebugLocation DebugLoc, SILType existentialMetatypeType,
         SILValue metatype, ArrayRef<ProtocolConformanceRef> conformances,
         SILFunction *parent);

public:
  /// Return the object type which was erased.  That is, if this
  /// instruction erases Decoder<T>.Type.Type to Printable.Type.Type,
  /// this method returns Decoder<T>.
  CanType getFormalErasedObjectType() const {
    auto exType = getType().getASTType();
    auto concreteType = getOperand()->getType().getASTType();
    while (auto exMetatype = dyn_cast<ExistentialMetatypeType>(exType)) {
      exType = exMetatype->getExistentialInstanceType()->getCanonicalType();
      concreteType = cast<MetatypeType>(concreteType).getInstanceType();
    }
    assert(exType.isExistentialType());
    return concreteType;
  }

  ArrayRef<ProtocolConformanceRef> getConformances() const;
};

/// DeinitExistentialAddrInst - Given an address of an existential that has been
/// partially initialized with an InitExistentialAddrInst but whose value buffer
/// has not been initialized, deinitializes the existential and deallocates
/// the value buffer. This should only be used for partially-initialized
/// existentials; a fully-initialized existential can be destroyed with
/// DestroyAddrInst and deallocated with DeallocStackInst.
class DeinitExistentialAddrInst
  : public UnaryInstructionBase<SILInstructionKind::DeinitExistentialAddrInst,
                                NonValueInstruction>
{
  friend SILBuilder;

  DeinitExistentialAddrInst(SILDebugLocation DebugLoc, SILValue Existential)
      : UnaryInstructionBase(DebugLoc, Existential) {}
};

class DeinitExistentialValueInst
    : public UnaryInstructionBase<SILInstructionKind::DeinitExistentialValueInst,
                                  NonValueInstruction> {
  friend SILBuilder;

  DeinitExistentialValueInst(SILDebugLocation DebugLoc, SILValue Existential)
      : UnaryInstructionBase(DebugLoc, Existential) {}
};

/// Compute the length of a pack (as a Builtin.Word).
class PackLengthInst final
    : public NullaryInstructionWithTypeDependentOperandsBase<
                  SILInstructionKind::PackLengthInst,
                  PackLengthInst,
                  SingleValueInstruction> {
  friend TrailingObjects;
  friend SILBuilder;

  CanPackType ThePackType;

  PackLengthInst(SILDebugLocation loc,
                 ArrayRef<SILValue> typeDependentOperands,
                 SILType resultType,
                 CanPackType packType)
    : NullaryInstructionWithTypeDependentOperandsBase(loc,
                                                      typeDependentOperands,
                                                      resultType),
      ThePackType(packType) {}

  static PackLengthInst *create(SILFunction &parent,
                                SILDebugLocation loc,
                                CanPackType packType);
public:
  /// Return the measured pack type.
  CanPackType getPackType() const {
    return ThePackType;
  }
};

/// An abstract class for instructions which producing variadic
/// pack indices.
///
/// All of these instructions produce a Builtin.PackIndex value which
/// can only be used in packs with a specific shape class.  In
/// principle, that shape class could be reflected into the result type,
/// but we actually need more structue than that in order to get the
/// type-safety properties we want.  It therefore makes more sense to
/// enforce structural properties on pack-index derivation than try
/// to go all-in on dependent types.
class AnyPackIndexInst : public SingleValueInstruction {
  CanPackType IndexedPackType;

protected:
  AnyPackIndexInst(SILInstructionKind kind, SILDebugLocation loc,
                   SILType type, CanPackType packType)
      : SingleValueInstruction(kind, loc, type), IndexedPackType(packType) {
    assert(type.isObject() && type.is<BuiltinPackIndexType>());
  }

public:
  /// Return the type that this pack index indexes into.
  CanPackType getIndexedPackType() const { return IndexedPackType; }

  static bool classof(const AnyPackIndexInst *) { return true; }
  static bool classof(SILNodePointer node) {
    return node->getKind() >= SILNodeKind::First_AnyPackIndexInst &&
           node->getKind() <= SILNodeKind::Last_AnyPackIndexInst;
  }
};

/// Produce a dynamic pack index from a Builtin.Int32.
///
/// This instruction has undefined behavior if the value is out of
/// bounds for the given pack (including the "one past the end" value).
class DynamicPackIndexInst final :
    public UnaryInstructionWithTypeDependentOperandsBase<
                                SILInstructionKind::DynamicPackIndexInst,
                                DynamicPackIndexInst,
                                AnyPackIndexInst> {
  friend SILBuilder;
  DynamicPackIndexInst(SILDebugLocation loc,
                       SILValue indexOperand,
                       ArrayRef<SILValue> typeDependentOperands,
                       SILType type, CanPackType packType)
    : UnaryInstructionWithTypeDependentOperandsBase(loc, indexOperand,
                                                    typeDependentOperands,
                                                    type, packType) {}

  static DynamicPackIndexInst *create(SILFunction &parent,
                                      SILDebugLocation loc,
                                      SILValue indexOperand,
                                      CanPackType packType);
};

/// Compute the pack index of an element of a slice of a pack.
class PackPackIndexInst final :
    public UnaryInstructionWithTypeDependentOperandsBase<
                                SILInstructionKind::PackPackIndexInst,
                                PackPackIndexInst,
                                AnyPackIndexInst> {
  unsigned ComponentStartIndex;

  friend SILBuilder;
  PackPackIndexInst(SILDebugLocation loc,
                    unsigned componentStartIndex,
                    SILValue indexWithinComponent,
                    ArrayRef<SILValue> typeDependentOperands,
                    SILType type, CanPackType packType)
    : UnaryInstructionWithTypeDependentOperandsBase(loc,
                                                    indexWithinComponent,
                                                    typeDependentOperands,
                                                    type, packType),
      ComponentStartIndex(componentStartIndex) {}

  static PackPackIndexInst *create(SILFunction &parent,
                                   SILDebugLocation loc,
                                   unsigned componentIndex,
                                   SILValue indexWithinComponent,
                                   CanPackType packType);
public:
  /// Return the instruction which produces the index within the
  /// pack slice.
  AnyPackIndexInst *getSliceIndexOperand() const {
    return cast<AnyPackIndexInst>(getOperand());
  }

  /// Return the structural index of the start of the pack slice.
  unsigned getComponentStartIndex() const {
    return ComponentStartIndex;
  }

  /// Return the structural index of the end of the pack slice.
  unsigned getComponentEndIndex() const {
    return getComponentStartIndex()
         + getSliceIndexOperand()->getIndexedPackType()->getNumElements();
  }
};

/// Compute the pack index of a scalar component of a pack.
class ScalarPackIndexInst final :
    public NullaryInstructionWithTypeDependentOperandsBase<
                                SILInstructionKind::ScalarPackIndexInst,
                                ScalarPackIndexInst,
                                AnyPackIndexInst> {
  unsigned ComponentIndex;

  friend SILBuilder;
  ScalarPackIndexInst(SILDebugLocation loc,
                      unsigned componentIndex,
                      ArrayRef<SILValue> typeDependentOperands,
                      SILType type, CanPackType packType)
    : NullaryInstructionWithTypeDependentOperandsBase(loc,
                                  typeDependentOperands, type, packType),
      ComponentIndex(componentIndex) {}

  static ScalarPackIndexInst *create(SILFunction &parent,
                                     SILDebugLocation loc,
                                     unsigned index,
                                     CanPackType packType);
public:
  /// Return the structural index of the component within the pack.
  unsigned getComponentIndex() const {
    return ComponentIndex;
  }
};

/// Bind archetypes to the given element of one or more type packs.
///
/// The result of this instruction is just for use in recording type
/// dependencies on the bound archetypes.
///
///   %0 = open_pack_element %index
///          of <t_1_0... where t_1_0: Equatable>   // opened signature
///          at <Pack{repeat each T}>,              // contextual subs
///          shape $t_1_0,
///          uuid "01234567-89AB-CDEF-0123-000000000000"
///
/// The %index operand is always a $Builtin.PackIndex and must be
/// the immediate result of one of the pack-indexing instructions.
class OpenPackElementInst final
    : public UnaryInstructionWithTypeDependentOperandsBase<
                                        SILInstructionKind::OpenPackElementInst,
                                        OpenPackElementInst,
                                        SingleValueInstruction> {
  friend SILBuilder;

  /// The opened-element generic environment for this operation.
  ///
  /// In the AST, the opened element generic environment of a
  /// PackExpansionExpr extends the contextual generic environment with
  /// a new, innermost level of parameters representing the opened
  /// elements.  These parameters are not pack parameters, but they are
  /// 1-1 with the expanded pack parameters, and the requirements laid
  /// on them are copied from the requirements on those parameters.
  /// The substitutions in the environment map the contextual generic
  /// parameters to their current archetypes, and only the new element
  /// parameters acquire new archetypes within the environment.
  ///
  /// Parts of this correspondence break down for open_pack_element.
  /// In particular, SIL instructions can be cloned into new contexts,
  /// applying a substitution that can change or even erase the pack
  /// parameters in the contextual environment.  Rather than require
  /// the opened element environment to continue to be an extension
  /// of the contextual environment, SIL allows the two to diverge:
  /// there is no presumed relationship between the generic signature
  /// of the opened environment and that of the contextual environment.
  /// The generic environment should be treated as a source of
  /// information about the expanded packs, the contextual pack
  /// substitutions, and the opened archetype for each pack.
  ///
  /// An alternative representation would be to remove the non-pack
  /// parameters from the opened generic environment, replacing them
  /// in the requirements with references to the contextual archetypes.
  /// However, this would require various algorithms working with
  /// generic signatures and environments to work with a mixture of
  /// archetypes and type parameters, which can introduce problems
  /// when reasoning about certain kinds of generic signatures.
  GenericEnvironment *Env;

  OpenPackElementInst(SILDebugLocation debugLoc,
                      SILValue packIndexOperand,
                      ArrayRef<SILValue> typeDependentOperands,
                      SILType type,
                      GenericEnvironment *env);

  static OpenPackElementInst *
  create(SILFunction &F, SILDebugLocation debugLoc, SILValue index,
         GenericEnvironment *env);

public:
  /// Call the given function for each element archetype that this
  /// instruction opens.
  void forEachDefinedLocalArchetype(
      llvm::function_ref<void(CanLocalArchetypeType, SILValue)> fn) const;

  GenericEnvironment *getOpenedGenericEnvironment() const {
    return Env;
  }

  /// Return a pack type which represents the contextual shape class
  /// of the types this opens.
  CanPackType getOpenedShapeClass() const;

  AnyPackIndexInst *getIndexOperand() const {
    return cast<AnyPackIndexInst>(getOperand());
  }
};

/// Get the value previously stored in a pack by pack_element_set.
class PackElementGetInst final
  : public InstructionBaseWithTrailingOperands<
                           SILInstructionKind::PackElementGetInst,
                           PackElementGetInst, SingleValueInstruction> {
public:
  enum {
    IndexOperand = 0,
    PackOperand = 1
  };

private:
  friend SILBuilder;

  PackElementGetInst(SILDebugLocation debugLoc,
                     ArrayRef<SILValue> allOperands,
                     SILType elementType)
      : InstructionBaseWithTrailingOperands(allOperands, debugLoc,
                                            elementType) {}

  static PackElementGetInst *create(SILFunction &F,
                                    SILDebugLocation debugLoc,
                                    SILValue indexOperand,
                                    SILValue packOperand,
                                    SILType elementType);

public:
  SILValue getIndex() const {
    return getAllOperands()[IndexOperand].get();
  }

  SILValue getPack() const {
    return getAllOperands()[PackOperand].get();
  }

  CanSILPackType getPackType() const {
    return getPack()->getType().castTo<SILPackType>();
  }

  SILType getElementType() const {
    return getType();
  }
};

/// Set the value stored in a pack.
class PackElementSetInst
  : public InstructionBase<SILInstructionKind::PackElementSetInst,
                           NonValueInstruction> {
public:
  enum {
    ValueOperand = 0,
    IndexOperand = 1,
    PackOperand = 2
  };

private:
  friend SILBuilder;

  FixedOperandList<3> Operands;

  PackElementSetInst(SILDebugLocation debugLoc,
                     SILValue valueOperand, SILValue indexOperand,
                     SILValue packOperand)
      : InstructionBase(debugLoc),
        Operands(this, valueOperand, indexOperand, packOperand) {
    assert(packOperand->getType().is<SILPackType>());
  }

public:
  ArrayRef<Operand> getAllOperands() const { return Operands.asArray(); }
  MutableArrayRef<Operand> getAllOperands() { return Operands.asArray(); }

  SILValue getValue() const {
    return getAllOperands()[ValueOperand].get();
  }

  SILValue getIndex() const {
    return getAllOperands()[IndexOperand].get();
  }

  SILValue getPack() const {
    return getAllOperands()[PackOperand].get();
  }

  CanSILPackType getPackType() const {
    return getPack()->getType().castTo<SILPackType>();
  }

  SILType getElementType() const {
    return getValue()->getType();
  }
};

/// Projects a tuple element as appropriate for the given
/// pack element index.  The pack index must index into a pack with
/// the same shape as the tuple element type list.
class TuplePackElementAddrInst final
  : public InstructionBaseWithTrailingOperands<
                           SILInstructionKind::TuplePackElementAddrInst,
                           TuplePackElementAddrInst,
                           SingleValueInstruction> {
public:
  enum {
    IndexOperand = 0,
    TupleOperand = 1
  };

private:
  friend SILBuilder;

  TuplePackElementAddrInst(SILDebugLocation debugLoc,
                           ArrayRef<SILValue> allOperands,
                           SILType elementType)
      : InstructionBaseWithTrailingOperands(allOperands, debugLoc,
                                            elementType) {}

  static TuplePackElementAddrInst *create(SILFunction &F,
                                          SILDebugLocation debugLoc,
                                          SILValue indexOperand,
                                          SILValue tupleOperand,
                                          SILType elementType);

public:
  SILValue getIndex() const {
    return getAllOperands()[IndexOperand].get();
  }

  SILValue getTuple() const {
    return getAllOperands()[TupleOperand].get();
  }

  CanTupleType getTupleType() const {
    return getTuple()->getType().castTo<TupleType>();
  }

  SILType getElementType() const {
    return getType();
  }
};

/// Projects the capture storage address from a @block_storage address.
class ProjectBlockStorageInst
  : public UnaryInstructionBase<SILInstructionKind::ProjectBlockStorageInst,
                                SingleValueInstruction>
{
  friend SILBuilder;

  ProjectBlockStorageInst(SILDebugLocation DebugLoc, SILValue Operand,
                          SILType DestTy)
      : UnaryInstructionBase(DebugLoc, Operand, DestTy) {}
};


/// Initializes a block header, creating a block that
/// invokes a given thin cdecl function.
class InitBlockStorageHeaderInst
    : public InstructionBase<SILInstructionKind::InitBlockStorageHeaderInst,
                             SingleValueInstruction> {
  friend SILBuilder;

  enum { BlockStorage, InvokeFunction };
  SubstitutionMap Substitutions;
  FixedOperandList<2> Operands;
  
  InitBlockStorageHeaderInst(SILDebugLocation DebugLoc, SILValue BlockStorage,
                             SILValue InvokeFunction, SILType BlockType,
                             SubstitutionMap Subs)
      : InstructionBase(DebugLoc, BlockType),
        Substitutions(Subs),
        Operands(this, BlockStorage, InvokeFunction) {
  }
  
  static InitBlockStorageHeaderInst *create(SILFunction &F,
                              SILDebugLocation DebugLoc, SILValue BlockStorage,
                              SILValue InvokeFunction, SILType BlockType,
                              SubstitutionMap Subs);
public:
  /// Get the block storage address to be initialized.
  SILValue getBlockStorage() const { return Operands[BlockStorage].get(); }
  /// Get the invoke function to form the block around.
  SILValue getInvokeFunction() const { return Operands[InvokeFunction].get(); }

  SubstitutionMap getSubstitutions() const { return Substitutions; }

  ArrayRef<Operand> getAllOperands() const { return Operands.asArray(); }
  MutableArrayRef<Operand> getAllOperands() { return Operands.asArray(); }
};

/// StrongRetainInst - Increase the strong reference count of an object.
class StrongRetainInst
  : public UnaryInstructionBase<SILInstructionKind::StrongRetainInst,
                                RefCountingInst>
{
  friend SILBuilder;

  StrongRetainInst(SILDebugLocation DebugLoc, SILValue Operand,
                   Atomicity atomicity)
      : UnaryInstructionBase(DebugLoc, Operand) {
    setAtomicity(atomicity);
  }
};

/// StrongReleaseInst - Decrease the strong reference count of an object.
///
/// An object can be destroyed when its strong reference count is
/// zero.  It can be deallocated when both its strong reference and
/// weak reference counts reach zero.
class StrongReleaseInst
  : public UnaryInstructionBase<SILInstructionKind::StrongReleaseInst,
                                RefCountingInst>
{
  friend SILBuilder;

  StrongReleaseInst(SILDebugLocation DebugLoc, SILValue Operand,
                    Atomicity atomicity)
      : UnaryInstructionBase(DebugLoc, Operand) {
    setAtomicity(atomicity);
  }
};

/// Simple reference storage logic.
///
/// StrongRetain##Name##Inst - Increase the strong reference count of an object
/// and assert that it has not been deallocated.
/// The operand must be of type @name.
///
/// Name##RetainInst - Increase the 'name' reference count of an object.
///
/// Name##ReleaseInst - Decrease the 'name' reference count of an object.
#define ALWAYS_OR_SOMETIMES_LOADABLE_CHECKED_REF_STORAGE(Name, ...) \
class StrongRetain##Name##Inst \
    : public UnaryInstructionBase<SILInstructionKind::StrongRetain##Name##Inst,\
                                  RefCountingInst> { \
  friend SILBuilder; \
  StrongRetain##Name##Inst(SILDebugLocation DebugLoc, SILValue operand, \
                           Atomicity atomicity) \
      : UnaryInstructionBase(DebugLoc, operand) { \
    setAtomicity(atomicity); \
  } \
}; \
class Name##RetainInst \
    : public UnaryInstructionBase<SILInstructionKind::Name##RetainInst, \
                                RefCountingInst> { \
  friend SILBuilder; \
  Name##RetainInst(SILDebugLocation DebugLoc, SILValue Operand, \
                   Atomicity atomicity) \
      : UnaryInstructionBase(DebugLoc, Operand) { \
    setAtomicity(atomicity); \
  } \
}; \
class Name##ReleaseInst \
    : public UnaryInstructionBase<SILInstructionKind::Name##ReleaseInst, \
                                  RefCountingInst> { \
  friend SILBuilder; \
  Name##ReleaseInst(SILDebugLocation DebugLoc, SILValue Operand, \
                    Atomicity atomicity) \
      : UnaryInstructionBase(DebugLoc, Operand) { \
    setAtomicity(atomicity); \
  } \
};
#include "swift/AST/ReferenceStorage.def"

/// FixLifetimeInst - An artificial use of a value for the purposes of ARC or
/// RVO optimizations.
class FixLifetimeInst :
  public UnaryInstructionBase<SILInstructionKind::FixLifetimeInst,
                              NonValueInstruction>
{
  friend SILBuilder;

  FixLifetimeInst(SILDebugLocation DebugLoc, SILValue Operand)
      : UnaryInstructionBase(DebugLoc, Operand) {}
};

/// EndLifetimeInst - An artificial end lifetime use of a value for the purpose
/// of working around verification problems.
///
/// Specifically, the signature of destroying deinit takes self at +0 and
/// returns self at +1. This is an issue since a deallocating deinit takes in
/// self at +1. Previously, we could rely on the deallocating bit being set in
/// the object header to allow SILGen to statically balance the +1 from the
/// deallocating deinit. This is because deallocating values used to be
/// immortal. The runtime now asserts if we release a deallocating value,
/// meaning such an approach does not work. This instruction acts as a "fake"
/// lifetime ending use allowing for static verification of deallocating
/// destroyers, without an actual release being emitted (avoiding the runtime
/// assert).
class EndLifetimeInst
    : public UnaryInstructionBase<SILInstructionKind::EndLifetimeInst,
                                  NonValueInstruction> {
  friend SILBuilder;

  EndLifetimeInst(SILDebugLocation DebugLoc, SILValue Operand)
      : UnaryInstructionBase(DebugLoc, Operand) {}
};

/// An unsafe conversion in between ownership kinds.
///
/// This is used today in destructors where due to Objective-C legacy
/// constraints, we need to be able to convert a guaranteed parameter to an owned
/// parameter.
class UncheckedOwnershipConversionInst
    : public UnaryInstructionBase<SILInstructionKind::UncheckedOwnershipConversionInst,
                                  SingleValueInstruction> {
  friend SILBuilder;
  USE_SHARED_UINT8;

  UncheckedOwnershipConversionInst(SILDebugLocation DebugLoc, SILValue operand,
                                   ValueOwnershipKind Kind)
      : UnaryInstructionBase(DebugLoc, operand, operand->getType()) {
    sharedUInt8().UncheckedOwnershipConversionInst.valueOwnershipKind = Kind;
  }

public:
  ValueOwnershipKind getConversionOwnershipKind() const {
    uint8_t kind = sharedUInt8().UncheckedOwnershipConversionInst.valueOwnershipKind;
    return ValueOwnershipKind(kind);
  }
};

/// Indicates that the validity of the first operand ("the value") depends on
/// the value of the second operand ("the base").  Operations that would destroy
/// the base must not be moved before any instructions which depend on the
/// result of this instruction, exactly as if the address had been obviously
/// derived from that operand (e.g. using ``ref_element_addr``). The result is
/// always equal to the first operand and thus forwards ownership through the
/// first operand. This is a "regular" use of the second operand (i.e. the
/// second operand must be live at the use point).
///
/// Example:
///
///   %base = ...
///   %value = ... @trivial value ...
///   %value_dependent_on_base = mark_dependence %value on %base
///   ...
///   use(%value_dependent_on_base)     (1)
///   ...
///   destroy_value %base               (2)
///
/// (2) can never move before (1). In English this is a way for the compiler
/// writer to say to the optimizer: 'This subset of uses of "value" (the uses of
/// result) have a dependence on "base" being alive. Do not allow for things
/// that /may/ destroy base to be moved earlier than any of these uses of
/// "value"'.
class MarkDependenceInst
    : public InstructionBase<SILInstructionKind::MarkDependenceInst,
                             OwnershipForwardingSingleValueInstruction> {
  friend SILBuilder;

  FixedOperandList<2> Operands;

  MarkDependenceInst(SILDebugLocation DebugLoc, SILValue value, SILValue base,
                     ValueOwnershipKind forwardingOwnershipKind)
      : InstructionBase(DebugLoc, value->getType(), forwardingOwnershipKind),
        Operands{this, value, base} {}

public:
  enum { Value, Base };

  SILValue getValue() const { return Operands[Value].get(); }

  SILValue getBase() const { return Operands[Base].get(); }

  void setValue(SILValue newVal) {
    Operands[Value].set(newVal);
  }
  void setBase(SILValue newVal) {
    Operands[Base].set(newVal);
  }
  
  ArrayRef<Operand> getAllOperands() const { return Operands.asArray(); }
  MutableArrayRef<Operand> getAllOperands() { return Operands.asArray(); }
};

/// Promote an Objective-C block that is on the stack to the heap, or simply
/// retain a block that is already on the heap.
class CopyBlockInst
    : public UnaryInstructionBase<SILInstructionKind::CopyBlockInst,
                                  SingleValueInstruction>
{
  friend SILBuilder;

  CopyBlockInst(SILDebugLocation DebugLoc, SILValue operand)
      : UnaryInstructionBase(DebugLoc, operand, operand->getType()) {}
};

class CopyBlockWithoutEscapingInst
    : public InstructionBase<SILInstructionKind::CopyBlockWithoutEscapingInst,
                             SingleValueInstruction> {
  friend SILBuilder;

  FixedOperandList<2> Operands;

  CopyBlockWithoutEscapingInst(SILDebugLocation DebugLoc, SILValue block,
                               SILValue closure)
      : InstructionBase(DebugLoc, block->getType()), Operands{this, block,
                                                              closure} {}

public:
  enum { Block, Closure };

  SILValue getBlock() const { return Operands[Block].get(); }
  SILValue getClosure() const { return Operands[Closure].get(); }

  void setBlock(SILValue block) {
    Operands[Block].set(block);
  }
  void setClosure(SILValue closure) {
    Operands[Closure].set(closure);
  }

  ArrayRef<Operand> getAllOperands() const { return Operands.asArray(); }
  MutableArrayRef<Operand> getAllOperands() { return Operands.asArray(); }
};

class CopyValueInst
    : public UnaryInstructionBase<SILInstructionKind::CopyValueInst,
                                  SingleValueInstruction> {
  friend class SILBuilder;

  CopyValueInst(SILDebugLocation DebugLoc, SILValue operand)
      : UnaryInstructionBase(DebugLoc, operand, operand->getType()) {}
};

class ExplicitCopyValueInst
    : public UnaryInstructionBase<SILInstructionKind::ExplicitCopyValueInst,
                                  SingleValueInstruction> {
  friend class SILBuilder;

  ExplicitCopyValueInst(SILDebugLocation DebugLoc, SILValue operand)
      : UnaryInstructionBase(DebugLoc, operand, operand->getType()) {}
};

#define UNCHECKED_REF_STORAGE(Name, ...)                                       \
  class StrongCopy##Name##ValueInst                                            \
      : public UnaryInstructionBase<                                           \
            SILInstructionKind::StrongCopy##Name##ValueInst,                   \
            SingleValueInstruction> {                                          \
    friend class SILBuilder;                                                   \
    StrongCopy##Name##ValueInst(SILDebugLocation DebugLoc, SILValue operand,   \
                                SILType type)                                  \
        : UnaryInstructionBase(DebugLoc, operand, type) {}                     \
  };

#define ALWAYS_OR_SOMETIMES_LOADABLE_CHECKED_REF_STORAGE(Name, ...)            \
  class StrongCopy##Name##ValueInst                                            \
      : public UnaryInstructionBase<                                           \
            SILInstructionKind::StrongCopy##Name##ValueInst,                   \
            SingleValueInstruction> {                                          \
    friend class SILBuilder;                                                   \
    StrongCopy##Name##ValueInst(SILDebugLocation DebugLoc, SILValue operand,   \
                                SILType type)                                  \
        : UnaryInstructionBase(DebugLoc, operand, type) {}                     \
  };
#include "swift/AST/ReferenceStorage.def"

class DestroyValueInst
    : public UnaryInstructionBase<SILInstructionKind::DestroyValueInst,
                                  NonValueInstruction> {
  friend class SILBuilder;
  USE_SHARED_UINT8;

  DestroyValueInst(SILDebugLocation DebugLoc, SILValue operand, bool poisonRefs)
      : UnaryInstructionBase(DebugLoc, operand) {
    setPoisonRefs(poisonRefs);
  }

public:
  /// If true, then all references within the destroyed value will be
  /// overwritten with a sentinel. This is used in debug builds when shortening
  /// non-trivial value lifetimes to ensure the debugger cannot inspect invalid
  /// memory. These semantics are part of the destroy_value instruction to
  /// avoid representing use-after-destroy in OSSA form and so that OSSA
  /// transformations keep the poison operation associated with the destroy
  /// point. After OSSA, these are lowered to 'debug_values [poison]'
  /// instructions, after which the Onone pipeline should avoid code motion.
  bool poisonRefs() const { return sharedUInt8().DestroyValueInst.poisonRefs; }

  void setPoisonRefs(bool poisonRefs = true) {
    sharedUInt8().DestroyValueInst.poisonRefs = poisonRefs;
  }
  
  /// If the value being destroyed is a stack allocation of a nonescaping
  /// closure, then return the PartialApplyInst that allocated the closure.
  PartialApplyInst *getNonescapingClosureAllocation() const;
};

class MoveValueInst
    : public UnaryInstructionBase<SILInstructionKind::MoveValueInst,
                                  SingleValueInstruction> {
  friend class SILBuilder;

  USE_SHARED_UINT8;

  MoveValueInst(SILDebugLocation DebugLoc, SILValue operand, bool isLexical,
                bool hasPointerEscape)
      : UnaryInstructionBase(DebugLoc, operand, operand->getType()) {
    sharedUInt8().MoveValueInst.lexical = isLexical;
    sharedUInt8().MoveValueInst.pointerEscape = hasPointerEscape;
  }

public:
  /// If set to true, we should emit the kill diagnostic for this move_value. If
  /// set to false, we shouldn't emit such a diagnostic. This is a short term
  /// addition until we get MoveOnly wrapper types into the SIL type system.
  bool getAllowDiagnostics() const {
    return sharedUInt8().MoveValueInst.allowDiagnostics;
  }
  void setAllowsDiagnostics(bool newValue) {
    sharedUInt8().MoveValueInst.allowDiagnostics = newValue;
  }

  bool isLexical() const { return sharedUInt8().MoveValueInst.lexical; }
  void removeIsLexical() { sharedUInt8().MoveValueInst.lexical = false; }

  bool hasPointerEscape() const {
    return sharedUInt8().MoveValueInst.pointerEscape;
  }
  void setHasPointerEscape(bool pointerEscape) {
    sharedUInt8().MoveValueInst.pointerEscape = pointerEscape;
  }
};

class DropDeinitInst
    : public UnaryInstructionBase<SILInstructionKind::DropDeinitInst,
                                  SingleValueInstruction> {
  friend class SILBuilder;

  DropDeinitInst(SILDebugLocation DebugLoc, SILValue operand)
      : UnaryInstructionBase(DebugLoc, operand, operand->getType()) {}
};

/// Equivalent to a copy_addr to [init] except that it is used for diagnostics
/// and should not be pattern matched. During the diagnostic passes, the "move
/// function" checker for addresses always converts this to a copy_addr [init]
/// (if we emitted a diagnostic and proved we could not emit a move here) or a
/// copy_addr [take][init] if we can. So this should never occur in canonical
/// SIL.
class MarkUnresolvedMoveAddrInst
    : public InstructionBase<SILInstructionKind::MarkUnresolvedMoveAddrInst,
                             NonValueInstruction>,
      public CopyLikeInstruction {
  friend class SILBuilder;

  FixedOperandList<2> Operands;

  MarkUnresolvedMoveAddrInst(SILDebugLocation DebugLoc, SILValue srcAddr,
                             SILValue takeAddr)
      : InstructionBase(DebugLoc), Operands(this, srcAddr, takeAddr) {}

public:
  SILValue getSrc() const { return Operands[Src].get(); }
  SILValue getDest() const { return Operands[Dest].get(); }

  void setSrc(SILValue V) { Operands[Src].set(V); }
  void setDest(SILValue V) { Operands[Dest].set(V); }

  ArrayRef<Operand> getAllOperands() const { return Operands.asArray(); }
  MutableArrayRef<Operand> getAllOperands() { return Operands.asArray(); }
};

/// This is a marker instruction that has no effect that is consumed by a
/// diagnostic based semantic checker. Example: no implicit copy. Only legal in
/// Raw SIL so that we can guarantee canonical SIL has had all SSA based
/// checking by the checkers that rely upon this instruction.
class MarkMustCheckInst
    : public UnaryInstructionBase<SILInstructionKind::MarkMustCheckInst,
                                  OwnershipForwardingSingleValueInstruction> {
  friend class SILBuilder;

public:
  enum class CheckKind : unsigned {
    Invalid = 0,

    /// A signal to the move only checker to perform checking that allows for
    /// this value to be consumed along its boundary (in the case of let/var
    /// semantics) and also written over in the case of var semantics. NOTE: Of
    /// course this still implies the value cannot be copied and can be consumed
    /// only once along all program paths.
    ConsumableAndAssignable,

    /// A signal to the move only checker to perform no consume or assign
    /// checking. This forces the result of this instruction owned value to
    /// never be consumed (for let/var semantics) or assigned over (for var
    /// semantics). Of course, we still allow for non-consuming uses.
    NoConsumeOrAssign,

    /// A signal to the move checker that the given value cannot be consumed,
    /// but is allowed to be assigned over. This is used for situations like
    /// global_addr/ref_element_addr/closure escape where we do not want to
    /// allow for the user to take the value (leaving the memory in an
    /// uninitialized state), but we are ok with the user assigning a new value,
    /// completely assigning over the value at once.
    AssignableButNotConsumable,

    /// A signal to the move checker that the given value cannot be consumed or
    /// assigned, but is allowed to be initialized. This is used for situations
    /// like class initializers.
    InitableButNotConsumable,
  };

private:
  CheckKind kind;

  MarkMustCheckInst(SILDebugLocation DebugLoc, SILValue operand,
                    CheckKind checkKind)
      : UnaryInstructionBase(DebugLoc, operand, operand->getType(),
                             operand->getOwnershipKind()),
        kind(checkKind) {
    assert(operand->getType().isMoveOnly() &&
           "mark_must_check can only take a move only typed value");
  }

public:
  CheckKind getCheckKind() const { return kind; }

  void setCheckKind(CheckKind newKind) { kind = newKind; }

  bool hasMoveCheckerKind() const {
    switch (kind) {
    case CheckKind::Invalid:
      return false;
    case CheckKind::ConsumableAndAssignable:
    case CheckKind::NoConsumeOrAssign:
    case CheckKind::AssignableButNotConsumable:
    case CheckKind::InitableButNotConsumable:
      return true;
    }
  }
};

/// A marker instruction that states a given alloc_box or alloc_stack is a
/// reference binding that must be transformed.
class MarkUnresolvedReferenceBindingInst
    : public UnaryInstructionBase<
          SILInstructionKind::MarkUnresolvedReferenceBindingInst,
          OwnershipForwardingSingleValueInstruction> {
  friend class SILBuilder;

public:
  enum class Kind : unsigned {
    Invalid = 0,

    InOut = 1,
  };

private:
  Kind kind;

  MarkUnresolvedReferenceBindingInst(SILDebugLocation debugLoc,
                                     SILValue operand, Kind kind)
      : UnaryInstructionBase(debugLoc, operand, operand->getType(),
                             operand->getOwnershipKind()),
        kind(kind) {}

public:
  Kind getKind() const { return kind; }
};

/// Convert from a non-trivial copyable type to an `@moveOnly` wrapper type.
///
/// IMPORTANT: Unlike other forwarding instructions, the ownership of
/// copyable_to_moveonly is not decided by the operand passed in on
/// construction. Instead in SILBuilder one must select the specific type of
/// ownership one wishes by using the following APIs:
///
/// * SILBuilder::createOwnedCopyableToMoveOnlyWrapperValueInst
/// * SILBuilder::createGuaranteedCopyableToMoveOnlyWrapperInst
///
/// The reason why this instruction was designed in this manner is that a
/// frontend chooses the ownership form of this instruction based off of the
/// semantic place that the value is used. Specifically:
///
/// 1. When creating a moveOnly wrapped value for an owned argument or a value,
/// we use the owned variant.
///
/// 2. When creating a moveOnly wrapped value from a guaranteed argument, we use
/// the guaranteed variant.
class CopyableToMoveOnlyWrapperValueInst
    : public UnaryInstructionBase<
          SILInstructionKind::CopyableToMoveOnlyWrapperValueInst,
          OwnershipForwardingSingleValueInstruction> {
public:
  enum InitialKind {
    Guaranteed,
    Owned,
  };

private:
  friend class SILBuilder;

  InitialKind initialKind;

  CopyableToMoveOnlyWrapperValueInst(SILDebugLocation DebugLoc,
                                     SILValue operand, InitialKind kind)
      : UnaryInstructionBase(
            DebugLoc, operand, operand->getType().addingMoveOnlyWrapper(),
            kind == InitialKind::Guaranteed ? OwnershipKind::Guaranteed
                                            : OwnershipKind::Owned),
        initialKind(kind) {
    assert(!operand->getType().isMoveOnly() &&
           "Cannot be moveonly or moveonly wrapped");
  }

public:
  InitialKind getInitialKind() const { return initialKind; }

  bool hasGuaranteedInitialKind() const {
    return getInitialKind() == InitialKind::Guaranteed;
  }

  bool hasOwnedInitialKind() const {
    return getInitialKind() == InitialKind::Owned;
  }
};

/// Convert from an @moveOnly wrapper type to the underlying copyable type. Can
/// be either owned or guaranteed.
///
/// IMPORTANT: Unlike other forwarding instructions, the ownership of moveonly
/// to copyable is not forwarded from the operand. Instead in SILBuilder one
/// must select the specific type of ownership one wishes by using the following
/// APIs:
///
/// * SILBuilder::createOwnedMoveOnlyWrapperToCopyableValueInst
/// * SILBuilder::createGuaranteedMoveOnlyWrapperToCopyableValueInst
///
/// The reason why this instruction was designed in this manner is that a
/// frontend chooses the ownership form of this instruction based off of the
/// semantic place that the value is used. As an example:
///
/// 1. When calling a function semantically with guaranteed ownership, the
///    frontend would use the "guaranteed variant".
///
/// 2. When returning a value or assigning into another binding, a frontend
///    would want to use the owned variant so that the move only checker will
///    enforce the end of the moved value's lifetime.
///
/// NOTE: With time, we are going to eliminate the guaranteed form of this
/// instruction in favor of a function conversion instruction.
class MoveOnlyWrapperToCopyableValueInst
    : public UnaryInstructionBase<
          SILInstructionKind::MoveOnlyWrapperToCopyableValueInst,
          OwnershipForwardingSingleValueInstruction> {
public:
  enum InitialKind {
    Guaranteed,
    Owned,
  };

private:
  friend class SILBuilder;

  InitialKind initialKind;

  MoveOnlyWrapperToCopyableValueInst(const SILFunction &fn,
                                     SILDebugLocation DebugLoc,
                                     SILValue operand, InitialKind kind)
      : UnaryInstructionBase(
            DebugLoc, operand, operand->getType().removingMoveOnlyWrapper(),
            kind == InitialKind::Guaranteed ? OwnershipKind::Guaranteed
                                            : OwnershipKind::Owned),
        initialKind(kind) {
    assert(operand->getType().isMoveOnlyWrapped() &&
           "Expected moveonlywrapped argument!");
  }

public:
  InitialKind getInitialKind() const { return initialKind; }

  bool hasGuaranteedInitialKind() const {
    return getInitialKind() == InitialKind::Guaranteed;
  }

  bool hasOwnedInitialKind() const {
    return getInitialKind() == InitialKind::Owned;
  }
};

/// Convert a ${ @moveOnly T } to $T. This is a forwarding instruction that acts
/// similarly to an object cast like upcast, unlike
/// MoveOnlyWrapperToCopyableValue which provides artificial semantics injected
/// by SILGen.
class MoveOnlyWrapperToCopyableBoxInst
    : public UnaryInstructionBase<
          SILInstructionKind::MoveOnlyWrapperToCopyableBoxInst,
          OwnershipForwardingSingleValueInstruction> {
  friend class SILBuilder;

  MoveOnlyWrapperToCopyableBoxInst(SILDebugLocation DebugLoc, SILValue operand,
                                   ValueOwnershipKind forwardingOwnershipKind)
      : UnaryInstructionBase(
            DebugLoc, operand,
            operand->getType().removingMoveOnlyWrapperToBoxedType(
                operand->getFunction()),
            forwardingOwnershipKind) {
    assert(
        operand->getType().isBoxedMoveOnlyWrappedType(operand->getFunction()) &&
        "Expected moveonlywrapped argument!");
  }
};

class CopyableToMoveOnlyWrapperAddrInst
    : public UnaryInstructionBase<
          SILInstructionKind::CopyableToMoveOnlyWrapperAddrInst,
          SingleValueInstruction> {
  friend class SILBuilder;

  CopyableToMoveOnlyWrapperAddrInst(SILDebugLocation DebugLoc, SILValue operand)
      : UnaryInstructionBase(DebugLoc, operand,
                             operand->getType().addingMoveOnlyWrapper()) {
    assert(!operand->getType().isMoveOnly() && "Expected copyable argument");
  }
};

class MoveOnlyWrapperToCopyableAddrInst
    : public UnaryInstructionBase<
          SILInstructionKind::MoveOnlyWrapperToCopyableAddrInst,
          SingleValueInstruction> {
  friend class SILBuilder;

  MoveOnlyWrapperToCopyableAddrInst(SILDebugLocation DebugLoc, SILValue operand)
      : UnaryInstructionBase(DebugLoc, operand,
                             operand->getType().removingMoveOnlyWrapper()) {
    assert(operand->getType().isMoveOnlyWrapped() &&
           "Expected moveonlywrapped argument");
  }
};

/// Given an object reference, return true iff it is non-nil and refers
/// to a native swift object with strong reference count of 1.
class IsUniqueInst
    : public UnaryInstructionBase<SILInstructionKind::IsUniqueInst,
                                  SingleValueInstruction>
{
  friend SILBuilder;

  IsUniqueInst(SILDebugLocation DebugLoc, SILValue Operand, SILType BoolTy)
      : UnaryInstructionBase(DebugLoc, Operand, BoolTy) {}
};

/// Performs a uniqueness check of the operand for the purpose of modifying
/// a copy-on-write object.
///
/// Returns two results: the first result is an Int1 which is the result of the
/// uniqueness check. The second result is the class reference operand, which
/// can be used for mutation.
class BeginCOWMutationInst final
    : public UnaryInstructionBase<SILInstructionKind::BeginCOWMutationInst,
                                  MultipleValueInstruction>,
      public MultipleValueInstructionTrailingObjects<BeginCOWMutationInst>
{
  friend SILBuilder;
  friend TrailingObjects;
  USE_SHARED_UINT8;

  BeginCOWMutationInst(SILDebugLocation loc, SILValue operand,
                       ArrayRef<SILType> resultTypes,
                       ArrayRef<ValueOwnershipKind> resultOwnerships,
                       bool isNative);

  static BeginCOWMutationInst *
  create(SILDebugLocation loc, SILValue operand, SILType BoolTy, SILFunction &F,
         bool isNative);

public:
  using MultipleValueInstructionTrailingObjects::totalSizeToAlloc;

  /// Returns the result of the uniqueness check.
  SILValue getUniquenessResult() const {
    return &getAllResultsBuffer()[0];
  }

  /// Returns the class reference which can be used for mutation.
  SILValue getBufferResult() const {
    return &getAllResultsBuffer()[1];
  }

  bool isNative() const {
    return sharedUInt8().BeginCOWMutationInst.native;
  }
  
  void setNative(bool native = true) {
    sharedUInt8().BeginCOWMutationInst.native = native;
  }
};

/// Marks the end of the mutation of a reference counted object.
class EndCOWMutationInst
    : public UnaryInstructionBase<SILInstructionKind::EndCOWMutationInst,
                                  SingleValueInstruction>
{
  friend SILBuilder;
  USE_SHARED_UINT8;

  EndCOWMutationInst(SILDebugLocation DebugLoc, SILValue Operand,
                     bool keepUnique)
      : UnaryInstructionBase(DebugLoc, Operand, Operand->getType()) {
    setKeepUnique(keepUnique);
  }

public:
  bool doKeepUnique() const {
    return sharedUInt8().EndCOWMutationInst.keepUnique;
  }

  void setKeepUnique(bool keepUnique = true) {
    sharedUInt8().EndCOWMutationInst.keepUnique = keepUnique;
  }
};

/// Given an escaping closure return true iff it has a non-nil context and the
/// context has a strong reference count greater than 1.
class IsEscapingClosureInst
    : public UnaryInstructionBase<SILInstructionKind::IsEscapingClosureInst,
                                  SingleValueInstruction> {
  friend SILBuilder;

  unsigned VerificationType;

  IsEscapingClosureInst(SILDebugLocation DebugLoc, SILValue Operand,
                        SILType BoolTy, unsigned VerificationType)
      : UnaryInstructionBase(DebugLoc, Operand, BoolTy),
        VerificationType(VerificationType) {}

public:
  enum { WithoutActuallyEscaping, ObjCEscaping };

  unsigned getVerificationType() const { return VerificationType; }
};

//===----------------------------------------------------------------------===//
// DeallocationInsts
//===----------------------------------------------------------------------===//

/// DeallocationInst - An abstract parent class for Dealloc{Stack, Box, Ref}.
class DeallocationInst : public NonValueInstruction {
protected:
  DeallocationInst(SILInstructionKind Kind, SILDebugLocation DebugLoc)
      : NonValueInstruction(Kind, DebugLoc) {}

public:
  DEFINE_ABSTRACT_NON_VALUE_INST_BOILERPLATE(DeallocationInst)
};

/// DeallocStackInst - Deallocate stack memory allocated by alloc_stack.
class DeallocStackInst :
    public UnaryInstructionBase<SILInstructionKind::DeallocStackInst,
                                DeallocationInst> {
  friend SILBuilder;

  DeallocStackInst(SILDebugLocation DebugLoc, SILValue operand)
      : UnaryInstructionBase(DebugLoc, operand) {}
};

/// DeallocPackInst - Deallocate stack memory allocated by alloc_pack.
class DeallocPackInst :
    public UnaryInstructionBase<SILInstructionKind::DeallocPackInst,
                                DeallocationInst> {
  friend SILBuilder;

  DeallocPackInst(SILDebugLocation debugLoc, SILValue operand)
      : UnaryInstructionBase(debugLoc, operand) {}
};

/// DeallocPackMetadataInst - Deallocate stack memory allocated on behalf of the
///                           operand by IRGen.
///
/// Only valid in lowered SIL.
class DeallocPackMetadataInst final
    : public UnaryInstructionBase<SILInstructionKind::DeallocPackMetadataInst,
                                  DeallocationInst> {
  friend SILBuilder;

  DeallocPackMetadataInst(SILDebugLocation debugLoc, SILValue alloc)
      : UnaryInstructionBase(debugLoc, alloc) {}

public:
  AllocPackMetadataInst *getAllocation() {
    return cast<AllocPackMetadataInst>(getOperand().getDefiningInstruction());
  }
  /// The instruction which may trigger on-stack pack metadata when IRGen
  /// lowering.
  SILInstruction *getIntroducer() { return getAllocation()->getIntroducer(); }
};

/// Like DeallocStackInst, but for `alloc_ref [stack]`.
class DeallocStackRefInst
    : public UnaryInstructionBase<SILInstructionKind::DeallocStackRefInst,
                                  DeallocationInst> {
  friend SILBuilder;

  DeallocStackRefInst(SILDebugLocation DebugLoc, SILValue Operand)
      : UnaryInstructionBase(DebugLoc, Operand) {}
public:
  AllocRefInstBase *getAllocRef() {
    return cast<AllocRefInstBase>(getOperand());
  }
};

/// Deallocate memory for a reference type instance from a destructor or
/// failure path of a constructor.
///
/// This does not destroy the referenced instance; it must be destroyed
/// first.
///
/// It is undefined behavior if the type of the operand does not match the
/// most derived type of the allocated instance.
class DeallocRefInst :
  public UnaryInstructionBase<SILInstructionKind::DeallocRefInst,
                              DeallocationInst> {
  friend SILBuilder;

  DeallocRefInst(SILDebugLocation DebugLoc, SILValue Operand)
      : UnaryInstructionBase(DebugLoc, Operand) { }
};

/// Deallocate memory for a reference type instance from a failure path of a
/// constructor.
///
/// The instance is assumed to have been partially initialized, with the
/// initialized portion being all instance variables in classes that are more
/// derived than the given metatype.
///
/// The metatype value can either be the static self type (in a designated
/// initializer) or a dynamic self type (in a convenience initializer).
class DeallocPartialRefInst
    : public InstructionBase<SILInstructionKind::DeallocPartialRefInst,
                             DeallocationInst> {
  friend SILBuilder;

private:
  FixedOperandList<2> Operands;

  DeallocPartialRefInst(SILDebugLocation DebugLoc, SILValue Operand,
                        SILValue Metatype)
      : InstructionBase(DebugLoc),
        Operands(this, Operand, Metatype) {}

public:
  ArrayRef<Operand> getAllOperands() const { return Operands.asArray(); }
  MutableArrayRef<Operand> getAllOperands() { return Operands.asArray(); }
  
  SILValue getInstance() const { return getOperand(0); }
  SILValue getMetatype() const { return getOperand(1); }
};

/// Deallocate memory allocated for a boxed value created by an AllocBoxInst.
/// It is undefined behavior if the type of the boxed type does not match the
/// type the box was allocated for.
///
/// This does not destroy the boxed value instance; it must either be
/// uninitialized or have been manually destroyed.
class DeallocBoxInst
    : public UnaryInstructionBase<SILInstructionKind::DeallocBoxInst,
                                  DeallocationInst>
{
  friend SILBuilder;

  DeallocBoxInst(SILDebugLocation DebugLoc, SILValue operand)
      : UnaryInstructionBase(DebugLoc, operand) {}
};

/// Deallocate memory allocated for a boxed existential container created by
/// AllocExistentialBox. It is undefined behavior if the given concrete type
/// does not match the concrete type for which the box was allocated.
///
/// This does not destroy the boxed value instance; it must either be
/// uninitialized or have been manually destroyed.
class DeallocExistentialBoxInst
    : public UnaryInstructionBase<SILInstructionKind::DeallocExistentialBoxInst,
                                  DeallocationInst>
{
  friend SILBuilder;

  CanType ConcreteType;

  DeallocExistentialBoxInst(SILDebugLocation DebugLoc, CanType concreteType,
                            SILValue operand)
      : UnaryInstructionBase(DebugLoc, operand), ConcreteType(concreteType) {}

public:
  CanType getConcreteType() const { return ConcreteType; }
};

/// Destroy the value at a memory location according to
/// its SIL type. This is similar to:
///   %1 = load %operand
///   release_value %1
/// but a destroy instruction can be used for types that cannot be loaded,
/// such as resilient value types.
class DestroyAddrInst
    : public UnaryInstructionBase<SILInstructionKind::DestroyAddrInst,
                                  NonValueInstruction>
{
  friend SILBuilder;

  DestroyAddrInst(SILDebugLocation DebugLoc, SILValue Operand)
      : UnaryInstructionBase(DebugLoc, Operand) {}
};

/// Project out the address of the value in a box.
class ProjectBoxInst
    : public UnaryInstructionBase<SILInstructionKind::ProjectBoxInst,
                                  SingleValueInstruction> {
  friend SILBuilder;

  unsigned Index;

  ProjectBoxInst(SILDebugLocation DebugLoc,
                 SILValue operand,
                 unsigned fieldIndex,
                 SILType fieldTy)
      : UnaryInstructionBase(DebugLoc, operand, fieldTy.getAddressType()),
        Index(fieldIndex) {}


public:
  unsigned getFieldIndex() const { return Index; }
};

/// Project out the address of the value in an existential box.
class ProjectExistentialBoxInst
    : public UnaryInstructionBase<SILInstructionKind::ProjectExistentialBoxInst,
                                  SingleValueInstruction> {
  friend SILBuilder;
  
  ProjectExistentialBoxInst(SILDebugLocation DebugLoc, SILType valueType,
                            SILValue operand)
      : UnaryInstructionBase(DebugLoc, operand, valueType.getAddressType()) {}
};

//===----------------------------------------------------------------------===//
// Runtime failure
//===----------------------------------------------------------------------===//

/// Trigger a runtime failure if the given Int1 value is true.
///
/// Optionally cond_fail has a static failure message, which is displayed in the debugger in case the failure
/// is triggered.
class CondFailInst final
    : public UnaryInstructionBase<SILInstructionKind::CondFailInst,
                                  NonValueInstruction>,
      private llvm::TrailingObjects<CondFailInst, char>
{
  friend TrailingObjects;
  friend SILBuilder;

  unsigned MessageSize;

  CondFailInst(SILDebugLocation DebugLoc, SILValue Operand, StringRef Message);

  static CondFailInst *create(SILDebugLocation DebugLoc, SILValue Operand,
                              StringRef Message, SILModule &M);

public:
  StringRef getMessage() const {
    return {getTrailingObjects<char>(), MessageSize};
  }
};

//===----------------------------------------------------------------------===//
// Pointer/address indexing instructions
//===----------------------------------------------------------------------===//

/// Abstract base class for indexing instructions.
class IndexingInst : public SingleValueInstruction {
  enum { Base, Index };
  FixedOperandList<2> Operands;
public:
  IndexingInst(SILInstructionKind Kind, SILDebugLocation DebugLoc,
               SILType ResultTy, SILValue Operand, SILValue Index)
      : SingleValueInstruction(Kind, DebugLoc, ResultTy),
        Operands{this, Operand, Index} {}

  SILValue getBase() const { return Operands[Base].get(); }
  SILValue getIndex() const { return Operands[Index].get(); }

  ArrayRef<Operand> getAllOperands() const { return Operands.asArray(); }
  MutableArrayRef<Operand> getAllOperands() { return Operands.asArray(); }

  const Operand &getBaseOperandRef() const { return getAllOperands()[Base]; }

  DEFINE_ABSTRACT_SINGLE_VALUE_INST_BOILERPLATE(IndexingInst)
};

/// IndexAddrInst - "%2 : $*T = index_addr %0 : $*T, %1 : $Builtin.Word"
/// This takes an address and indexes it, striding by the pointed-
/// to type.  This is used to index into arrays of uniform elements.
class IndexAddrInst
    : public InstructionBase<SILInstructionKind::IndexAddrInst,
                             IndexingInst> {
  friend SILBuilder;
  USE_SHARED_UINT8;

  enum { Base, Index };

  IndexAddrInst(SILDebugLocation DebugLoc, SILValue Operand, SILValue Index,
                bool needsStackProtection)
      : InstructionBase(DebugLoc, Operand->getType(), Operand, Index) {
    sharedUInt8().IndexAddrInst.needsStackProtection = needsStackProtection;
  }

public:
  bool needsStackProtection() const {
    return sharedUInt8().IndexAddrInst.needsStackProtection;
  }
};

/// TailAddrInst - like IndexingInst, but aligns-up the resulting address to a
/// tail-allocated element type.
class TailAddrInst
    : public InstructionBase<SILInstructionKind::TailAddrInst,
                             IndexingInst> {
  friend SILBuilder;

  TailAddrInst(SILDebugLocation DebugLoc, SILValue Operand, SILValue Count,
               SILType ResultTy)
      : InstructionBase(DebugLoc, ResultTy, Operand, Count) {}

public:
  SILType getTailType() const { return getType().getObjectType(); }
};

/// IndexRawPointerInst
/// %2 : $Builtin.RawPointer \
///   = index_raw_pointer %0 : $Builtin.RawPointer, %1 : $Builtin.Word
/// This takes an address and indexes it, striding by the pointed-
/// to type.  This is used to index into arrays of uniform elements.
class IndexRawPointerInst
    : public InstructionBase<SILInstructionKind::IndexRawPointerInst,
                             IndexingInst> {
  friend SILBuilder;

  enum { Base, Index };

  IndexRawPointerInst(SILDebugLocation DebugLoc, SILValue Operand,
                      SILValue Index)
      : InstructionBase(DebugLoc, Operand->getType(), Operand, Index) {
  }
};

//===----------------------------------------------------------------------===//
// Instructions representing terminators
//===----------------------------------------------------------------------===//

enum class TermKind {
#define TERMINATOR(Id, TextualName, Parent, MemBehavior, MayRelease) \
  Id = unsigned(SILInstructionKind::Id),
#include "SILNodes.def"
};

/// This class defines a "terminating instruction" for a SILBasicBlock.
class TermInst : public NonValueInstruction {
protected:
  TermInst(SILInstructionKind K, SILDebugLocation DebugLoc)
      : NonValueInstruction(K, DebugLoc) {}

public:
  using ConstSuccessorListTy = ArrayRef<SILSuccessor>;
  using SuccessorListTy = MutableArrayRef<SILSuccessor>;

  /// The successor basic blocks of this terminator.
  SuccessorListTy getSuccessors();
  ConstSuccessorListTy getSuccessors() const {
    return const_cast<TermInst*>(this)->getSuccessors();
  }

  using const_succ_iterator = ConstSuccessorListTy::const_iterator;
  using succ_iterator = SuccessorListTy::iterator;

  bool succ_empty() const { return getSuccessors().empty(); }
  succ_iterator succ_begin() { return getSuccessors().begin(); }
  succ_iterator succ_end() { return getSuccessors().end(); }
  const_succ_iterator succ_begin() const { return getSuccessors().begin(); }
  const_succ_iterator succ_end() const { return getSuccessors().end(); }

  unsigned getNumSuccessors() const { return getSuccessors().size(); }

  using succblock_iterator =
      TransformIterator<SILSuccessor *,
                        SILBasicBlock *(*)(const SILSuccessor &)>;
  using const_succblock_iterator = TransformIterator<
      const SILSuccessor *,
      const SILBasicBlock *(*)(const SILSuccessor &)>;
  succblock_iterator succblock_begin() {
    return succblock_iterator(getSuccessors().begin(),
                              [](const SILSuccessor &succ) -> SILBasicBlock * {
      return succ.getBB();
    });
  }
  succblock_iterator succblock_end() {
    return succblock_iterator(getSuccessors().end(),
                              [](const SILSuccessor &succ) -> SILBasicBlock * {
      return succ.getBB();
    });
  }
  const_succblock_iterator succblock_begin() const {
    return const_succblock_iterator(
        getSuccessors().begin(),
        [](const SILSuccessor &succ) -> const SILBasicBlock * {
      return succ.getBB();
    });
  }
  const_succblock_iterator succblock_end() const {
    return const_succblock_iterator(
        getSuccessors().end(),
        [](const SILSuccessor &succ) -> const SILBasicBlock * {
      return succ.getBB();
    });
  }

  SILBasicBlock *getSingleSuccessorBlock() {
    if (succ_empty() || std::next(succ_begin()) != succ_end())
      return nullptr;
    return *succ_begin();
  }

  const SILBasicBlock *getSingleSuccessorBlock() const {
    return const_cast<TermInst *>(this)->getSingleSuccessorBlock();
  }

  using SuccessorBlockArgumentListTy =
      TransformRange<ConstSuccessorListTy, function_ref<ArrayRef<SILArgument *>(
                                               const SILSuccessor &)>>;

  /// Return the range of Argument arrays for each successor of this
  /// block.
  SuccessorBlockArgumentListTy getSuccessorBlockArgumentLists() const;

  using SuccessorBlockListTy =
      TransformRange<SuccessorListTy,
                     SILBasicBlock *(*)(const SILSuccessor &)>;
  using ConstSuccessorBlockListTy =
      TransformRange<ConstSuccessorListTy,
                     const SILBasicBlock *(*)(const SILSuccessor &)>;

  /// Return the range of SILBasicBlocks that are successors of this block.
  SuccessorBlockListTy getSuccessorBlocks() {
    return SuccessorBlockListTy(getSuccessors(),
                                [](const SILSuccessor &succ) -> SILBasicBlock* {
      return succ.getBB();
    });
  }

  /// Return the range of SILBasicBlocks that are successors of this block.
  ConstSuccessorBlockListTy getSuccessorBlocks() const {
    return ConstSuccessorBlockListTy(
        getSuccessors(),
        [](const SILSuccessor &succ) -> const SILBasicBlock * {
      return succ.getBB();
    });
  }

  void replaceBranchTarget(SILBasicBlock *oldDest, SILBasicBlock *newDest);

  DEFINE_ABSTRACT_NON_VALUE_INST_BOILERPLATE(TermInst)

  bool isBranch() const { return !getSuccessors().empty(); }

  /// Returns true if this terminator exits the function.
  bool isFunctionExiting() const;

  /// Returns true if this terminator terminates the program.
  bool isProgramTerminating() const;

  TermKind getTermKind() const { return TermKind(getKind()); }

  /// Returns true if this terminator may have a result, represented as a block
  /// argument in any of its successor blocks.
  ///
  /// Phis (whose operands originate from BranchInst terminators) are not
  /// terminator results.
  ///
  /// CondBr might produce block arguments for legacy reasons. This is gradually
  /// being deprecated. For now, they are considered phis. In OSSA, these "phis"
  /// must be trivial and critical edges cannot be present.
  bool mayHaveTerminatorResult() const {
    switch (getTermKind()) {
    case TermKind::UnwindInst:
    case TermKind::UnreachableInst:
    case TermKind::ReturnInst:
    case TermKind::ThrowInst:
    case TermKind::YieldInst:
    case TermKind::CondBranchInst:
    case TermKind::BranchInst:
    case TermKind::SwitchEnumAddrInst:
    case TermKind::CheckedCastAddrBranchInst:
      return false;
    case TermKind::CheckedCastBranchInst:
    case TermKind::SwitchEnumInst:
    case TermKind::SwitchValueInst:
    case TermKind::TryApplyInst:
    case TermKind::AwaitAsyncContinuationInst:
    case TermKind::DynamicMethodBranchInst:
      return true;
    }
    llvm_unreachable("Covered switch isn't covered.");
  }

  /// Returns an Operand reference if this terminator forwards ownership of a
  /// single operand to a single result for at least one successor
  /// block. Otherwise returns nullptr.
  ///
  /// By convention, terminators can forward ownership of at most one operand to
  /// at most one result. The operand value might not be directly forwarded. For
  /// example, a switch forwards ownership of the enum type into ownership of
  /// the payload.
  ///
  /// Postcondition: if the result is non-null, then each successor has zero or
  /// one block arguments which represents the forwaded result.
  const Operand *forwardedOperand() const;

  Operand *forwardedOperand() {
    return const_cast<Operand *>(
      static_cast<const TermInst *>(this)->forwardedOperand());
  }
};

// Forwards the first operand to a result in each successor block.
class OwnershipForwardingTermInst : public TermInst,
                                    public ForwardingInstruction {
protected:
  OwnershipForwardingTermInst(SILInstructionKind kind,
                              SILDebugLocation debugLoc,
                              ValueOwnershipKind ownershipKind,
                              bool preservesOwnership = true)
      : TermInst(kind, debugLoc),
        ForwardingInstruction(kind, ownershipKind, preservesOwnership) {
    assert(classof(kind));
  }

public:
  static bool classof(SILNodePointer node) {
    if (auto *i = dyn_cast<SILInstruction>(node.get()))
      return classof(i);
    return false;
  }

  static bool classof(const SILInstruction *inst) {
    return classof(inst->getKind());
  }

  static bool classof(SILInstructionKind kind) {
    return kind == SILInstructionKind::SwitchEnumInst ||
           kind == SILInstructionKind::CheckedCastBranchInst;
  }

  SILValue getOperand() const { return getAllOperands()[0].get(); }

  Operand &getOperandRef() { return getAllOperands()[0]; }

  const Operand &getOperandRef() const { return getAllOperands()[0]; }

  /// Create a result for this terminator on the given successor block.
  SILPhiArgument *createResult(SILBasicBlock *succ, SILType resultTy);
};

/// UnreachableInst - Position in the code which would be undefined to reach.
/// These are always implicitly generated, e.g. when falling off the end of a
/// function or after a no-return function call.
class UnreachableInst
    : public InstructionBase<SILInstructionKind::UnreachableInst,
                             TermInst> {
  friend SILBuilder;

  UnreachableInst(SILDebugLocation DebugLoc)
      : InstructionBase(DebugLoc) {}

public:
  SuccessorListTy getSuccessors() {
    // No Successors.
    return SuccessorListTy();
  }

  ArrayRef<Operand> getAllOperands() const { return {}; }
  MutableArrayRef<Operand> getAllOperands() { return {}; }
};

/// ReturnInst - Representation of a ReturnStmt.
class ReturnInst
  : public UnaryInstructionBase<SILInstructionKind::ReturnInst, TermInst>
{
  friend SILBuilder;

  /// We store the ownership kind in the return inst, but we do not consider the
  /// underlying return inst to be forwarding. This is because its ownership is
  /// tied to the function signature and thus should be static.
  ValueOwnershipKind ownershipKind;

  /// Constructs a ReturnInst representing a return.
  ///
  /// \param func The function we are returning from. Used to compute the
  ///             preferred ownership kind.
  /// \param debugLoc The backing AST location.
  /// \param returnValue The value to be returned.
  ReturnInst(SILFunction &func, SILDebugLocation debugLoc,
             SILValue returnValue);

public:
  /// Return the ownership kind for this instruction if we had any direct
  /// results.
  ValueOwnershipKind getOwnershipKind() const { return ownershipKind; }

  SuccessorListTy getSuccessors() {
    // No Successors.
    return SuccessorListTy();
  }
};

/// ThrowInst - Throw a typed error (which, in our system, is
/// essentially just a funny kind of return).
class ThrowInst
  : public UnaryInstructionBase<SILInstructionKind::ThrowInst, TermInst>
{
  friend SILBuilder;

  /// Constructs a ThrowInst representing a throw out of the current
  /// function.
  ///
  /// \param DebugLoc The location of the throw.
  /// \param errorValue The value to be thrown.
  ThrowInst(SILDebugLocation DebugLoc, SILValue errorValue)
      : UnaryInstructionBase(DebugLoc, errorValue) {}

public:
  SuccessorListTy getSuccessors() {
    // No successors.
    return SuccessorListTy();
  }
};

/// UnwindInst - Continue unwinding out of this function.  Currently this is
/// only used in coroutines as the eventual terminator of the unwind edge
/// out of a 'yield'.
class UnwindInst
  : public InstructionBase<SILInstructionKind::UnwindInst,
                           TermInst> {
  friend SILBuilder;

  UnwindInst(SILDebugLocation loc)
    : InstructionBase(loc) {}

public:
  SuccessorListTy getSuccessors() {
    // No successors.
    return SuccessorListTy();
  }

  ArrayRef<Operand> getAllOperands() const { return {}; }
  MutableArrayRef<Operand> getAllOperands() { return {}; }
};

/// Suspend execution of an async task until
/// essentially just a funny kind of return).
class AwaitAsyncContinuationInst final
  : public UnaryInstructionBase<SILInstructionKind::AwaitAsyncContinuationInst,
                                TermInst>
{
  friend SILBuilder;
  
  std::array<SILSuccessor, 2> Successors;
  
  AwaitAsyncContinuationInst(SILDebugLocation Loc, SILValue Continuation,
                             SILBasicBlock *resumeBB,
                             SILBasicBlock *errorBBOrNull)
    : UnaryInstructionBase(Loc, Continuation),
      Successors{{{this}, {this}}}
  {
    Successors[0] = resumeBB;
    if (errorBBOrNull)
      Successors[1] = errorBBOrNull;
  }
  
public:
  /// Returns the basic block to which control is transferred when the task is
  /// resumed normally.
  ///
  /// This basic block should take an argument of the continuation's resume type,
  /// unless the continuation is formed by a \c GetAsyncContinuationAddrInst
  /// that binds a specific memory location to receive the resume value.
  SILBasicBlock *getResumeBB() const { return Successors[0].getBB(); }
  
  /// Returns the basic block to which control is transferred when the task is
  /// resumed in an error state, or `nullptr` if the continuation does not support
  /// failure.
  ///
  /// This basic block should take an argument of Error type.
  SILBasicBlock *getErrorBB() const {
    return Successors[1].getBB();
  }
  
  SuccessorListTy getSuccessors() {
    if (getErrorBB())
      return Successors;
    return SuccessorListTy(Successors.data(), 1);
  }
};

/// YieldInst - Yield control temporarily to the caller of this coroutine.
///
/// This is a terminator because the caller can abort the coroutine,
/// e.g. if an error is thrown and an unwind is provoked.
class YieldInst final
  : public InstructionBaseWithTrailingOperands<SILInstructionKind::YieldInst,
                                               YieldInst, TermInst> {
  friend SILBuilder;

  std::array<SILSuccessor, 2> DestBBs;

  YieldInst(SILDebugLocation loc, ArrayRef<SILValue> yieldedValues,
            SILBasicBlock *normalBB, SILBasicBlock *unwindBB)
    : InstructionBaseWithTrailingOperands(yieldedValues, loc),
      DestBBs{{{this, normalBB}, {this, unwindBB}}} {}

  static YieldInst *create(SILDebugLocation loc,
                           ArrayRef<SILValue> yieldedValues,
                           SILBasicBlock *normalBB, SILBasicBlock *unwindBB,
                           SILFunction &F);

public:
  /// Return the normal resume destination of the yield, which is where the
  /// coroutine resumes when the caller is ready to continue normally.
  ///
  /// This must be the unique predecessor edge of the given block.
  ///
  /// Control flow along every path from this block must either loop or
  /// eventually terminate in a 'return', 'throw', or 'unreachable'
  /// instruction.  In a yield_many coroutine, control is permitted to
  /// first reach a 'yield' instruction; this is prohibited in a
  /// yield_once coroutine.
  SILBasicBlock *getResumeBB() const { return DestBBs[0]; }

  /// Return the 'unwind' destination of the yield, which is where the
  /// coroutine resumes when the caller is unconditionally aborting the
  /// coroutine.
  ///
  /// This must be the unique predecessor edge of the given block.
  ///
  /// Control flow along every path from this block must either loop or
  /// eventually terminate in an 'unwind' or 'unreachable' instruction.
  /// It is not permitted to reach a 'yield' instruction.
  SILBasicBlock *getUnwindBB() const { return DestBBs[1]; }

  OperandValueArrayRef getYieldedValues() const {
    return OperandValueArrayRef(getAllOperands());
  }

  SuccessorListTy getSuccessors() {
    return DestBBs;
  }

  SILYieldInfo getYieldInfoForOperand(const Operand &op) const;

  SILArgumentConvention
  getArgumentConventionForOperand(const Operand &op) const;
};

/// BranchInst - An unconditional branch.
class BranchInst final
    : public InstructionBaseWithTrailingOperands<SILInstructionKind::BranchInst,
                                                 BranchInst, TermInst> {
  friend SILBuilder;

  SILSuccessor DestBB;

  BranchInst(SILDebugLocation DebugLoc, SILBasicBlock *DestBB,
             ArrayRef<SILValue> Args)
    : InstructionBaseWithTrailingOperands(Args, DebugLoc),
      DestBB(this, DestBB) {}

  /// Construct a BranchInst that will branch to the specified block.
  /// The destination block must take no parameters.
  static BranchInst *create(SILDebugLocation DebugLoc, SILBasicBlock *DestBB,
                            SILFunction &F);

  /// Construct a BranchInst that will branch to the specified block with
  /// the given parameters.
  static BranchInst *create(SILDebugLocation DebugLoc, SILBasicBlock *DestBB,
                            ArrayRef<SILValue> Args, SILFunction &F);

public:
  /// returns jump target for the branch.
  SILBasicBlock *getDestBB() const { return DestBB; }

  /// The arguments for the destination BB.
  OperandValueArrayRef getArgs() const {
    return OperandValueArrayRef(getAllOperands());
  }

  SuccessorListTy getSuccessors() {
    return SuccessorListTy(&DestBB, 1);
  }

  unsigned getNumArgs() const { return getAllOperands().size(); }
  SILValue getArg(unsigned i) const { return getAllOperands()[i].get(); }

  /// Return the SILPhiArgument for the given operand.
  const SILPhiArgument *getArgForOperand(const Operand *oper) const {
    auto *self = const_cast<BranchInst *>(this);
    return self->getArgForOperand(oper);
  }

  /// Return the SILPhiArgument for the given operand.
  ///
  /// See SILArgument.cpp.
  SILPhiArgument *getArgForOperand(const Operand *oper);
};

/// A conditional branch.
class CondBranchInst final
    : public InstructionBaseWithTrailingOperands<
                                             SILInstructionKind::CondBranchInst,
                                             CondBranchInst,
                                             TermInst> {
  friend SILBuilder;

public:
  enum {
    /// The operand index of the condition value used for the branch.
    ConditionIdx,
    NumFixedOpers,
  };
  enum {
    // Map branch targets to block successor indices.
    TrueIdx,
    FalseIdx
  };
private:
  std::array<SILSuccessor, 2> DestBBs;
  unsigned numTrueArguments;

  CondBranchInst(SILDebugLocation DebugLoc, SILValue Condition,
                 SILBasicBlock *TrueBB, SILBasicBlock *FalseBB,
                 ArrayRef<SILValue> Args, unsigned NumTrue, unsigned NumFalse,
                 ProfileCounter TrueBBCount, ProfileCounter FalseBBCount);

  /// Construct a CondBranchInst that will branch to TrueBB or FalseBB based on
  /// the Condition value. Both blocks must not take any arguments.
  static CondBranchInst *create(SILDebugLocation DebugLoc, SILValue Condition,
                                SILBasicBlock *TrueBB, SILBasicBlock *FalseBB,
                                ProfileCounter TrueBBCount,
                                ProfileCounter FalseBBCount, SILFunction &F);

  /// Construct a CondBranchInst that will either branch to TrueBB and pass
  /// TrueArgs or branch to FalseBB and pass FalseArgs based on the Condition
  /// value.
  static CondBranchInst *
  create(SILDebugLocation DebugLoc, SILValue Condition, SILBasicBlock *TrueBB,
         ArrayRef<SILValue> TrueArgs, SILBasicBlock *FalseBB,
         ArrayRef<SILValue> FalseArgs, ProfileCounter TrueBBCount,
         ProfileCounter FalseBBCount, SILFunction &F);

public:
  const Operand *getConditionOperand() const {
    return &getAllOperands()[ConditionIdx];
  }
  SILValue getCondition() const { return getConditionOperand()->get(); }
  void setCondition(SILValue newCondition) {
    getAllOperands()[ConditionIdx].set(newCondition);
  }

  SuccessorListTy getSuccessors() {
    return DestBBs;
  }

  SILBasicBlock *getTrueBB() { return DestBBs[0]; }
  const SILBasicBlock *getTrueBB() const { return DestBBs[0]; }
  SILBasicBlock *getFalseBB() { return DestBBs[1]; }
  const SILBasicBlock *getFalseBB() const { return DestBBs[1]; }

  /// The number of times the True branch was executed.
  ProfileCounter getTrueBBCount() const { return DestBBs[0].getCount(); }
  /// The number of times the False branch was executed.
  ProfileCounter getFalseBBCount() const { return DestBBs[1].getCount(); }

  /// The number of arguments for the True branch.
  unsigned getNumTrueArgs() const { return numTrueArguments; }

  /// The number of arguments for the False branch.
  unsigned getNumFalseArgs() const {
    return getAllOperands().size() - NumFixedOpers - numTrueArguments;
  }

  /// Get the arguments to the true BB.
  OperandValueArrayRef getTrueArgs() const {
    return OperandValueArrayRef(getTrueOperands());
  }
  /// Get the arguments to the false BB.
  OperandValueArrayRef getFalseArgs() const {
    return OperandValueArrayRef(getFalseOperands());
  }

  /// Get the operands to the true BB.
  ArrayRef<Operand> getTrueOperands() const {
    return getAllOperands().slice(NumFixedOpers, getNumTrueArgs());
  }
  MutableArrayRef<Operand> getTrueOperands() {
    return getAllOperands().slice(NumFixedOpers, getNumTrueArgs());
  }

  /// Get the operands to the false BB.
  ArrayRef<Operand> getFalseOperands() const {
    // The remaining arguments are 'false' operands.
    return getAllOperands().slice(NumFixedOpers + getNumTrueArgs());
  }
  MutableArrayRef<Operand> getFalseOperands() {
    // The remaining arguments are 'false' operands.
    return getAllOperands().slice(NumFixedOpers + getNumTrueArgs());
  }

  /// Returns true if \p op is mapped to the condition operand of the cond_br.
  bool isConditionOperand(Operand *op) const {
    return getConditionOperand() == op;
  }

  bool isConditionOperandIndex(unsigned OpIndex) const {
    assert(OpIndex < getNumOperands() &&
           "OpIndex must be an index for an actual operand");
    return OpIndex == ConditionIdx;
  }

  /// Is \p OpIndex an operand associated with the true case?
  bool isTrueOperandIndex(unsigned OpIndex) const {
    assert(OpIndex < getNumOperands() &&
           "OpIndex must be an index for an actual operand");
    if (getNumTrueArgs() == 0)
      return false;

    auto Operands = getTrueOperands();
    return Operands.front().getOperandNumber() <= OpIndex &&
           OpIndex <= Operands.back().getOperandNumber();
  }

  /// Is \p OpIndex an operand associated with the false case?
  bool isFalseOperandIndex(unsigned OpIndex) const {
    assert(OpIndex < getNumOperands() &&
           "OpIndex must be an index for an actual operand");
    if (getNumFalseArgs() == 0)
      return false;

    auto Operands = getFalseOperands();
    return Operands.front().getOperandNumber() <= OpIndex &&
           OpIndex <= Operands.back().getOperandNumber();
  }

  /// Returns the operand on the cond_br terminator associated with the value
  /// that will be passed to DestBB in A.
  Operand *getOperandForDestBB(const SILBasicBlock *DestBB,
                               const SILArgument *A) const;

  /// Returns the operand on the cond_br terminator associated with the value
  /// that will be passed as the \p Index argument to DestBB.
  Operand *getOperandForDestBB(const SILBasicBlock *DestBB,
                               unsigned ArgIndex) const;

  /// Returns the argument on the cond_br terminator that will be passed to
  /// DestBB in A.
  SILValue getArgForDestBB(const SILBasicBlock *DestBB,
                           const SILArgument *A) const {
    if (auto *op = getOperandForDestBB(DestBB, A)) {
      return op->get();
    }
    return SILValue();
  }

  /// Returns the argument on the cond_br terminator that will be passed as the
  /// \p Index argument to DestBB.
  SILValue getArgForDestBB(const SILBasicBlock *DestBB,
                           unsigned ArgIndex) const {
    if (auto *op = getOperandForDestBB(DestBB, ArgIndex)) {
      return op->get();
    }
    return SILValue();
  }

  /// Return the SILPhiArgument from either the true or false destination for
  /// the given operand.
  ///
  /// Returns nullptr for an operand with no block argument
  /// (i.e the branch condition).
  ///
  /// See SILArgument.cpp.
  const SILPhiArgument *getArgForOperand(const Operand *oper) const;

  void swapSuccessors();
};

/// A switch on a value of a builtin type.
class SwitchValueInst final
    : public InstructionBaseWithTrailingOperands<
                                      SILInstructionKind::SwitchValueInst,
                                      SwitchValueInst, TermInst, SILSuccessor> {
  friend SILBuilder;
  USE_SHARED_UINT8;

  SwitchValueInst(SILDebugLocation DebugLoc, SILValue Operand,
                  SILBasicBlock *DefaultBB, ArrayRef<SILValue> Cases,
                  ArrayRef<SILBasicBlock *> BBs);

  // Tail-allocated after the SwitchValueInst record are:
  // - `NumCases` SILValue values, containing
  //   the SILValue references for each case
  // - `NumCases + HasDefault` SILSuccessor records, referencing the
  //   destinations for each case, ending with the default destination if
  //   present.

  OperandValueArrayRef getCaseBuf() const {
    return OperandValueArrayRef(getAllOperands().slice(1));
  }

  SILSuccessor *getSuccessorBuf() {
    return getTrailingObjects<SILSuccessor>();
  }
  const SILSuccessor *getSuccessorBuf() const {
    return getTrailingObjects<SILSuccessor>();
  }

  static SwitchValueInst *
  create(SILDebugLocation DebugLoc, SILValue Operand, SILBasicBlock *DefaultBB,
         ArrayRef<std::pair<SILValue, SILBasicBlock *>> CaseBBs,
         SILFunction &F);

public:
  /// Clean up tail-allocated successor records for the switch cases.
  ~SwitchValueInst();

  SILValue getOperand() const { return getAllOperands()[0].get(); }

  SuccessorListTy getSuccessors() {
    return MutableArrayRef<SILSuccessor>{getSuccessorBuf(),
                           static_cast<size_t>(getNumCases() + hasDefault())};
  }

  unsigned getNumCases() const {
    return getAllOperands().size() - 1;
  }
  std::pair<SILValue, SILBasicBlock*>
  getCase(unsigned i) const {
    assert(i < getNumCases() && "case out of bounds");
    return {getCaseBuf()[i], getSuccessorBuf()[i]};
  }

  bool hasDefault() const {
    return sharedUInt8().SwitchValueInst.hasDefault;
  }
  SILBasicBlock *getDefaultBB() const {
    assert(hasDefault() && "doesn't have a default");
    return getSuccessorBuf()[getNumCases()];
  }

  Optional<unsigned> getUniqueCaseForDestination(SILBasicBlock *bb) const {
    for (unsigned i = 0; i < getNumCases(); ++i) {
      if (getCase(i).second == bb) {
        return i + 1;
      }
    }
    return None;
  }
};

/// Common implementation for the switch_enum and switch_enum_addr instructions.
template <typename BaseTy>
class SwitchEnumInstBase : public BaseTy {
  FixedOperandList<1> Operands;
  TEMPLATE_USE_SHARED_UINT8(BaseTy);
  TEMPLATE_USE_SHARED_UINT32(BaseTy);

  // Tail-allocated after the SwitchEnumInst record are:
  // - an array of `NumCases` EnumElementDecl* pointers, referencing the case
  //   discriminators
  // - `NumCases + HasDefault` SILSuccessor records, referencing the
  //   destinations for each case, ending with the default destination if
  //   present.
  // FIXME: This should use llvm::TrailingObjects, but it has subclasses
  // (which are empty, of course).

  EnumElementDecl **getCaseBuf() {
    return reinterpret_cast<EnumElementDecl**>(this + 1);

  }
  EnumElementDecl * const* getCaseBuf() const {
    return reinterpret_cast<EnumElementDecl* const*>(this + 1);

  }

  SILSuccessor *getSuccessorBuf() {
    return reinterpret_cast<SILSuccessor*>(getCaseBuf() + getNumCases());
  }
  const SILSuccessor *getSuccessorBuf() const {
    return reinterpret_cast<const SILSuccessor*>(getCaseBuf() + getNumCases());
  }

protected:
  template <typename... Rest>
  SwitchEnumInstBase(
      SILInstructionKind Kind, SILDebugLocation DebugLoc, SILValue Operand,
      SILBasicBlock *DefaultBB,
      ArrayRef<std::pair<EnumElementDecl *, SILBasicBlock *>> CaseBBs,
      Optional<ArrayRef<ProfileCounter>> Counts, ProfileCounter DefaultCount,
      Rest &&... rest)
      : BaseTy(Kind, DebugLoc, std::forward<Rest>(rest)...),
        Operands(this, Operand) {
    sharedUInt8().SwitchEnumInstBase.hasDefault = bool(DefaultBB);
    sharedUInt32().SwitchEnumInstBase.numCases = CaseBBs.size();
    // Initialize the case and successor arrays.
    auto *cases = getCaseBuf();
    auto *succs = getSuccessorBuf();
    for (unsigned i = 0, size = CaseBBs.size(); i < size; ++i) {
      cases[i] = CaseBBs[i].first;
      if (Counts) {
        ::new (succs + i)
            SILSuccessor(this, CaseBBs[i].second, Counts.value()[i]);
      } else {
        ::new (succs + i) SILSuccessor(this, CaseBBs[i].second);
      }
    }

    if (hasDefault()) {
      ::new (succs + getNumCases()) SILSuccessor(this, DefaultBB, DefaultCount);
    }
  }

  template <typename SWITCH_ENUM_INST, typename... RestTys>
  static SWITCH_ENUM_INST *createSwitchEnum(
      SILDebugLocation DebugLoc, SILValue Operand, SILBasicBlock *DefaultBB,
      ArrayRef<std::pair<EnumElementDecl *, SILBasicBlock *>> CaseBBs,
      SILFunction &F, Optional<ArrayRef<ProfileCounter>> Counts,
      ProfileCounter DefaultCount, RestTys &&... restArgs);

public:
  /// Clean up tail-allocated successor records for the switch cases.
  ~SwitchEnumInstBase() {
    // Destroy the successor records to keep the CFG up to date.
    auto *succs = getSuccessorBuf();
    for (unsigned i = 0, end = getNumCases() + hasDefault(); i < end; ++i) {
      succs[i].~SILSuccessor();
    }
  }

  SILValue getOperand() const { return Operands[0].get(); }

  ArrayRef<Operand> getAllOperands() const { return Operands.asArray(); }
  MutableArrayRef<Operand> getAllOperands() { return Operands.asArray(); }

  TermInst::SuccessorListTy getSuccessors() {
    return MutableArrayRef<SILSuccessor>{getSuccessorBuf(),
                           static_cast<size_t>(getNumCases() + hasDefault())};
  }

  unsigned getNumCases() const {
    return sharedUInt32().SwitchEnumInstBase.numCases;
  }

  std::pair<EnumElementDecl*, SILBasicBlock*>
  getCase(unsigned i) const {
    assert(i < getNumCases() && "case out of bounds");
    return {getCaseBuf()[i], getSuccessorBuf()[i].getBB()};
  }

  ProfileCounter getCaseCount(unsigned i) const {
    assert(i < getNumCases() && "case out of bounds");
    return getSuccessorBuf()[i].getCount();
  }

  // Swap the cases at indices \p i and \p j.
  void swapCase(unsigned i, unsigned j) {
    assert(i < getNumCases() && "First index is out of bounds?!");
    assert(j < getNumCases() && "Second index is out of bounds?!");

    auto *succs = getSuccessorBuf();

    // First grab our destination blocks.
    SILBasicBlock *iBlock = succs[i].getBB();
    SILBasicBlock *jBlock = succs[j].getBB();

    // Then destroy the sil successors and reinitialize them with the new things
    // that they are pointing at.
    succs[i].~SILSuccessor();
    ::new (succs + i) SILSuccessor(this, jBlock);
    succs[j].~SILSuccessor();
    ::new (succs + j) SILSuccessor(this, iBlock);

    // Now swap our cases.
    auto *cases = getCaseBuf();
    std::swap(cases[i], cases[j]);
  }

  /// Return the block that will be branched to on the specified enum
  /// case.
  SILBasicBlock *getCaseDestination(EnumElementDecl *D) {
    for (unsigned i = 0, e = getNumCases(); i != e; ++i) {
      auto Entry = getCase(i);
      if (Entry.first == D) return Entry.second;
    }
    // switch_enum is required to be fully covered, so return the default if we
    // didn't find anything.
    return getDefaultBB();
  }

  /// If the default refers to exactly one case decl, return it.
  NullablePtr<EnumElementDecl> getUniqueCaseForDefault() {
    auto enumValue = getOperand();
    SILType enumType = enumValue->getType();

    auto *f = SILInstruction::getFunction();
    if (!enumType.isEffectivelyExhaustiveEnumType(f))
      return nullptr;

    EnumDecl *decl = enumType.getEnumOrBoundGenericEnum();
    assert(decl && "switch_enum operand is not an enum");

    SmallPtrSet<EnumElementDecl *, 4> unswitchedElts;
    for (auto elt : decl->getAllElements())
      unswitchedElts.insert(elt);

    for (unsigned i = 0, e = getNumCases(); i != e; ++i) {
      auto Entry = getCase(i);
      unswitchedElts.erase(Entry.first);
    }

    if (unswitchedElts.size() == 1)
      return *unswitchedElts.begin();

    return nullptr;
  }

  /// If the given block only has one enum element decl matched to it,
  /// return it.
  NullablePtr<EnumElementDecl>
  getUniqueCaseForDestination(SILBasicBlock *block) {
    SILValue value = getOperand();
    SILType enumType = value->getType();
    EnumDecl *decl = enumType.getEnumOrBoundGenericEnum();
    assert(decl && "switch_enum operand is not an enum");
    (void)decl;

    EnumElementDecl *eltDecl = nullptr;
    for (unsigned i : range(getNumCases())) {
      auto entry = getCase(i);
      if (entry.second == block) {
        if (eltDecl != nullptr)
          return nullptr;
        eltDecl = entry.first;
      }
    }
    if (!eltDecl && hasDefault() && getDefaultBB() == block) {
      return getUniqueCaseForDefault();
    }
    return eltDecl;
  }

  bool hasDefault() const { return sharedUInt8().SwitchEnumInstBase.hasDefault; }

  SILBasicBlock *getDefaultBB() const {
    assert(hasDefault() && "doesn't have a default");
    return getSuccessorBuf()[getNumCases()];
  }

  NullablePtr<SILBasicBlock> getDefaultBBOrNull() const {
    if (!hasDefault())
      return nullptr;
    return getDefaultBB();
  }

  ProfileCounter getDefaultCount() const {
    assert(hasDefault() && "doesn't have a default");
    return getSuccessorBuf()[getNumCases()].getCount();
  }

  static bool classof(SILNodePointer node) {
    return node->getKind() >= SILNodeKind::SwitchEnumInst &&
           node->getKind() <= SILNodeKind::SwitchEnumAddrInst;
  }
};

/// A switch on a loadable enum's discriminator. The data for each case is
/// passed into the corresponding destination block as an argument.
class SwitchEnumInst
    : public InstructionBase<SILInstructionKind::SwitchEnumInst,
                             SwitchEnumInstBase<OwnershipForwardingTermInst>> {
  friend SILBuilder;

private:
  friend SwitchEnumInstBase;

  SwitchEnumInst(
      SILDebugLocation DebugLoc, SILValue Operand, SILBasicBlock *DefaultBB,
      ArrayRef<std::pair<EnumElementDecl *, SILBasicBlock *>> CaseBBs,
      Optional<ArrayRef<ProfileCounter>> CaseCounts,
      ProfileCounter DefaultCount, ValueOwnershipKind forwardingOwnershipKind)
      : InstructionBase(DebugLoc, Operand, DefaultBB, CaseBBs, CaseCounts,
                        DefaultCount, forwardingOwnershipKind) {}
  static SwitchEnumInst *
  create(SILDebugLocation DebugLoc, SILValue Operand, SILBasicBlock *DefaultBB,
         ArrayRef<std::pair<EnumElementDecl *, SILBasicBlock *>> CaseBBs,
         SILFunction &F, Optional<ArrayRef<ProfileCounter>> CaseCounts,
         ProfileCounter DefaultCount,
         ValueOwnershipKind forwardingOwnershipKind);

public:
  /// Create the default result for a partially built switch_enum.
  /// Returns nullptr if no default argument is needed.
  SILPhiArgument *createDefaultResult();

  /// Create the .some result for an optional switch_enum.
  SILPhiArgument *createOptionalSomeResult();
};
/// A switch on an enum's discriminator in memory.
class SwitchEnumAddrInst
    : public InstructionBase<SILInstructionKind::SwitchEnumAddrInst,
                             SwitchEnumInstBase<TermInst>> {
  friend SILBuilder;

private:
  friend SwitchEnumInstBase;

  SwitchEnumAddrInst(
      SILDebugLocation DebugLoc, SILValue Operand, SILBasicBlock *DefaultBB,
      ArrayRef<std::pair<EnumElementDecl *, SILBasicBlock *>> CaseBBs,
      Optional<ArrayRef<ProfileCounter>> CaseCounts,
      ProfileCounter DefaultCount)
      : InstructionBase(DebugLoc, Operand, DefaultBB, CaseBBs, CaseCounts,
                        DefaultCount) {}
  static SwitchEnumAddrInst *
  create(SILDebugLocation DebugLoc, SILValue Operand, SILBasicBlock *DefaultBB,
         ArrayRef<std::pair<EnumElementDecl *, SILBasicBlock *>> CaseBBs,
         SILFunction &F, Optional<ArrayRef<ProfileCounter>> CaseCounts,
         ProfileCounter DefaultCount);
};

/// Branch on the existence of an Objective-C method in the dynamic type of
/// an object.
///
/// If the method exists, branches to the first BB, providing it with the
/// method reference; otherwise, branches to the second BB.
class DynamicMethodBranchInst
    : public InstructionBase<SILInstructionKind::DynamicMethodBranchInst,
                             TermInst> {
  friend SILBuilder;

  SILDeclRef Member;

  std::array<SILSuccessor, 2> DestBBs;

  /// The operand.
  FixedOperandList<1> Operands;

  DynamicMethodBranchInst(SILDebugLocation DebugLoc, SILValue Operand,
                          SILDeclRef Member, SILBasicBlock *HasMethodBB,
                          SILBasicBlock *NoMethodBB);

  /// Construct a DynamicMethodBranchInst that will branch to \c HasMethodBB or
  /// \c NoMethodBB based on the ability of the object operand to respond to
  /// a message with the same selector as the member.
  static DynamicMethodBranchInst *
  create(SILDebugLocation DebugLoc, SILValue Operand, SILDeclRef Member,
         SILBasicBlock *HasMethodBB, SILBasicBlock *NoMethodBB, SILFunction &F);

public:
  SILValue getOperand() const { return Operands[0].get(); }

  SILDeclRef getMember() const { return Member; }

  SuccessorListTy getSuccessors() {
    return DestBBs;
  }

  SILBasicBlock *getHasMethodBB() { return DestBBs[0]; }
  const SILBasicBlock *getHasMethodBB() const { return DestBBs[0]; }
  SILBasicBlock *getNoMethodBB() { return DestBBs[1]; }
  const SILBasicBlock *getNoMethodBB() const { return DestBBs[1]; }

  ArrayRef<Operand> getAllOperands() const { return Operands.asArray(); }
  MutableArrayRef<Operand> getAllOperands() { return Operands.asArray(); }
};

/// The base class for cast instructions which are terminators.
template <typename BaseTy> class CastBranchInstBase : public BaseTy {
  std::array<SILSuccessor, 2> DestBBs;

public:
  template <typename... ArgTys>
  CastBranchInstBase(SILInstructionKind K, SILDebugLocation DebugLoc,
                     SILBasicBlock *SuccessBB, SILBasicBlock *FailureBB,
                     ProfileCounter Target1Count, ProfileCounter Target2Count,
                     ArgTys &&... args)
      : BaseTy(K, DebugLoc, std::forward<ArgTys>(args)...),
        DestBBs{{{this, SuccessBB, Target1Count},
                 {this, FailureBB, Target2Count}}} {}

  TermInst::SuccessorListTy getSuccessors() { return DestBBs; }

  // Enumerate the successor indices
  enum SuccessorPath { SuccessIdx = 0, FailIdx = 1};

  SILBasicBlock *getSuccessBB() { return DestBBs[SuccessIdx]; }
  const SILBasicBlock *getSuccessBB() const { return DestBBs[SuccessIdx]; }
  SILBasicBlock *getFailureBB() { return DestBBs[FailIdx]; }
  const SILBasicBlock *getFailureBB() const { return DestBBs[FailIdx]; }

  /// The number of times the True branch was executed
  ProfileCounter getTrueBBCount() const { return DestBBs[0].getCount(); }
  /// The number of times the False branch was executed
  ProfileCounter getFalseBBCount() const { return DestBBs[1].getCount(); }
};

/// The base class for cast instructions which are terminators and have a
/// CastConsumptionKind.
class CastBranchWithConsumptionKindBase : public CastBranchInstBase<TermInst> {
  CastConsumptionKind ConsumptionKind;

public:

  CastBranchWithConsumptionKindBase(SILInstructionKind K, SILDebugLocation DebugLoc,
                     CastConsumptionKind consumptionKind,
                     SILBasicBlock *SuccessBB, SILBasicBlock *FailureBB,
                     ProfileCounter Target1Count = ProfileCounter(),
                     ProfileCounter Target2Count = ProfileCounter()) :
    CastBranchInstBase(K, DebugLoc, SuccessBB, FailureBB,
                       Target1Count, Target2Count),
    ConsumptionKind(consumptionKind) {}

  CastConsumptionKind getConsumptionKind() const { return ConsumptionKind; }
};

/// Helper base class for AddrCastInstBase.
///
/// Ideally, the types would just be a member of AddrCastInstBase. But because
/// of tail-allocated operands, they need to be in a base class of
/// InstructionBaseWithTrailingOperands.
template<typename Base>
class TypesForAddrCasts : public Base {
  CanType SourceType;
  CanType TargetType;

public:
  template <typename... Args>
  TypesForAddrCasts(SILInstructionKind K, SILDebugLocation debugLoc,
                    CanType SourceType, CanType TargetType,
                    Args &&...args)
      : Base(K, debugLoc, std::forward<Args>(args)...),
        SourceType(SourceType), TargetType(TargetType) {}

  CanType getSourceFormalType() const { return SourceType; }
  CanType getTargetFormalType() const { return TargetType; }
};

/// Base class for cast instructions with address-type operands.
template<SILInstructionKind Kind,
         typename Derived,
         typename Base>
class AddrCastInstBase
    : public InstructionBaseWithTrailingOperands<Kind, Derived,
                                                 TypesForAddrCasts<Base>>,
      public CopyLikeInstruction {
protected:
  friend InstructionBaseWithTrailingOperands<Kind, Derived, Operand>;

  using TrailingObjects =
      InstructionBaseWithTrailingOperands<Kind, Derived, Operand>;

public:
  template <typename... Args>
  AddrCastInstBase(SILDebugLocation debugLoc,
                                       SILValue src, CanType srcType,
                                       SILValue dest, CanType targetType,
                                       ArrayRef<SILValue> typeDependentOperands,
                                       Args &&...args)
      : InstructionBaseWithTrailingOperands<Kind, Derived, TypesForAddrCasts<Base>> (
                                              src, dest, typeDependentOperands,
                                              debugLoc, srcType, targetType,
                                              std::forward<Args>(args)...) {}

  unsigned getNumTypeDependentOperands() const {
    return this->getAllOperands().size() - 2;
  }

  ArrayRef<Operand> getTypeDependentOperands() const {
    return this->getAllOperands().slice(2);
  }

  MutableArrayRef<Operand> getTypeDependentOperands() {
    return this->getAllOperands().slice(2);
  }

  SILValue getSrc() const { return this->getAllOperands()[Src].get(); }
  SILValue getDest() const { return this->getAllOperands()[Dest].get(); }

  SILType getSourceLoweredType() const { return getSrc()->getType(); }
  SILType getTargetLoweredType() const { return getDest()->getType(); }
};

/// Perform a checked cast operation and branch on whether the cast succeeds.
/// The success branch destination block receives the cast result as a BB
/// argument.
class CheckedCastBranchInst final
    : public UnaryInstructionWithTypeDependentOperandsBase<
          SILInstructionKind::CheckedCastBranchInst, CheckedCastBranchInst,
          CastBranchInstBase<OwnershipForwardingTermInst>> {
  friend SILBuilder;

  SILType DestLoweredTy;
  CanType DestFormalTy;
  bool IsExact;

  CheckedCastBranchInst(SILDebugLocation DebugLoc, bool IsExact,
                        SILValue Operand,
                        ArrayRef<SILValue> TypeDependentOperands,
                        SILType DestLoweredTy, CanType DestFormalTy,
                        SILBasicBlock *SuccessBB, SILBasicBlock *FailureBB,
                        ProfileCounter Target1Count,
                        ProfileCounter Target2Count,
                        ValueOwnershipKind forwardingOwnershipKind,
                        bool preservesOwnership)
      : UnaryInstructionWithTypeDependentOperandsBase(
            DebugLoc, Operand, TypeDependentOperands, SuccessBB, FailureBB,
            Target1Count, Target2Count, forwardingOwnershipKind,
            preservesOwnership),
        DestLoweredTy(DestLoweredTy), DestFormalTy(DestFormalTy),
        IsExact(IsExact) {}

  static CheckedCastBranchInst *
  create(SILDebugLocation DebugLoc, bool IsExact, SILValue Operand,
         SILType DestLoweredTy, CanType DestFormalTy, SILBasicBlock *SuccessBB,
         SILBasicBlock *FailureBB, SILFunction &F,
         ProfileCounter Target1Count, ProfileCounter Target2Count,
         ValueOwnershipKind forwardingOwnershipKind);

public:
  bool isExact() const { return IsExact; }

  SILType getSourceLoweredType() const { return getOperand()->getType(); }
  CanType getSourceFormalType() const { return getSourceLoweredType().getASTType(); }

  SILType getTargetLoweredType() const { return DestLoweredTy; }
  CanType getTargetFormalType() const { return DestFormalTy; }
};

/// Perform a checked cast operation and branch on whether the cast succeeds.
/// The result of the checked cast is left in the destination address.
class CheckedCastAddrBranchInst final
    : public AddrCastInstBase<
              SILInstructionKind::CheckedCastAddrBranchInst,
              CheckedCastAddrBranchInst, CastBranchWithConsumptionKindBase> {
  friend SILBuilder;

  CheckedCastAddrBranchInst(SILDebugLocation DebugLoc,
                            CastConsumptionKind consumptionKind, SILValue src,
                            CanType srcType, SILValue dest, CanType targetType,
                            ArrayRef<SILValue> TypeDependentOperands,
                            SILBasicBlock *successBB, SILBasicBlock *failureBB,
                            ProfileCounter Target1Count,
                            ProfileCounter Target2Count);

  static CheckedCastAddrBranchInst *
  create(SILDebugLocation DebugLoc, CastConsumptionKind consumptionKind,
         SILValue src, CanType srcType, SILValue dest, CanType targetType,
         SILBasicBlock *successBB, SILBasicBlock *failureBB,
         ProfileCounter Target1Count, ProfileCounter Target2Count,
         SILFunction &F);
};

/// Converts a heap object reference to a different type without any runtime
/// checks. This is a variant of UncheckedRefCast that works on address types,
/// thus encapsulates an implicit load and take of the reference followed by a
/// store and initialization of a new reference.
class UncheckedRefCastAddrInst final
    : public AddrCastInstBase<
               SILInstructionKind::UncheckedRefCastAddrInst,
               UncheckedRefCastAddrInst, NonValueInstruction> {
public:
  UncheckedRefCastAddrInst(SILDebugLocation Loc, SILValue src, CanType srcType,
                           SILValue dest, CanType targetType,
                           ArrayRef<SILValue> TypeDependentOperands);
  
  static UncheckedRefCastAddrInst *
  create(SILDebugLocation Loc, SILValue src, CanType srcType,
         SILValue dest, CanType targetType, SILFunction &F);
};

class UncheckedAddrCastInst final
    : public UnaryInstructionWithTypeDependentOperandsBase<
          SILInstructionKind::UncheckedAddrCastInst, UncheckedAddrCastInst,
          SingleValueInstruction> {
  friend SILBuilder;

  UncheckedAddrCastInst(SILDebugLocation DebugLoc, SILValue Operand,
                        ArrayRef<SILValue> TypeDependentOperands, SILType Ty)
      : UnaryInstructionWithTypeDependentOperandsBase(DebugLoc, Operand,
                                               TypeDependentOperands, Ty) {}
  static UncheckedAddrCastInst *
  create(SILDebugLocation DebugLoc, SILValue Operand, SILType Ty,
         SILFunction &F);
};

/// Perform an unconditional checked cast that aborts if the cast fails.
/// The result of the checked cast is left in the destination address.
class UnconditionalCheckedCastAddrInst final
    : public AddrCastInstBase<
               SILInstructionKind::UnconditionalCheckedCastAddrInst,
               UnconditionalCheckedCastAddrInst, NonValueInstruction> {
  friend SILBuilder;

  UnconditionalCheckedCastAddrInst(SILDebugLocation Loc,
                                   SILValue src, CanType sourceType,
                                   SILValue dest, CanType targetType,
                                   ArrayRef<SILValue> TypeDependentOperands);

  static UnconditionalCheckedCastAddrInst *
  create(SILDebugLocation DebugLoc, SILValue src, CanType sourceType,
         SILValue dest, CanType targetType,
         SILFunction &F);
};

/// A private abstract class to store the destinations of a TryApplyInst.
class TryApplyInstBase : public TermInst {
public:
  enum {
    // Map branch targets to block successor indices.
    NormalIdx,
    ErrorIdx
  };
private:
  std::array<SILSuccessor, 2> DestBBs;

protected:
  TryApplyInstBase(SILInstructionKind valueKind, SILDebugLocation Loc,
                   SILBasicBlock *normalBB, SILBasicBlock *errorBB);

public:
  SuccessorListTy getSuccessors() {
    return DestBBs;
  }

  bool isNormalSuccessorRef(SILSuccessor *successor) const {
    assert(successor == &DestBBs[0] || successor == &DestBBs[1]);
    return successor == &DestBBs[0];
  }
  bool isErrorSuccessorRef(SILSuccessor *successor) const {
    assert(successor == &DestBBs[0] || successor == &DestBBs[1]);
    return successor == &DestBBs[1];
  }  

  SILBasicBlock *getNormalBB() { return DestBBs[NormalIdx]; }
  const SILBasicBlock *getNormalBB() const { return DestBBs[NormalIdx]; }
  SILBasicBlock *getErrorBB() { return DestBBs[ErrorIdx]; }
  const SILBasicBlock *getErrorBB() const { return DestBBs[ErrorIdx]; }
};

/// TryApplyInst - Represents the full application of a function that
/// can produce an error.
class TryApplyInst final
    : public InstructionBase<SILInstructionKind::TryApplyInst,
                             ApplyInstBase<TryApplyInst, TryApplyInstBase>>,
      public llvm::TrailingObjects<TryApplyInst, Operand> {
  friend SILBuilder;

  TryApplyInst(SILDebugLocation DebugLoc, SILValue callee,
               SILType substCalleeType, SubstitutionMap substitutions,
               ArrayRef<SILValue> args,
               ArrayRef<SILValue> TypeDependentOperands,
               SILBasicBlock *normalBB, SILBasicBlock *errorBB,
               ApplyOptions options,
               const GenericSpecializationInformation *SpecializationInfo);

  static TryApplyInst *
  create(SILDebugLocation DebugLoc, SILValue callee,
         SubstitutionMap substitutions, ArrayRef<SILValue> args,
         SILBasicBlock *normalBB, SILBasicBlock *errorBB,
         ApplyOptions options, SILFunction &F,
         const GenericSpecializationInformation *SpecializationInfo);


};

/// DifferentiableFunctionInst - creates a `@differentiable` function-typed
/// value from an original function operand and derivative function operands
/// (optional). The differentiation transform canonicalizes
/// `differentiable_function` instructions, filling in derivative function
/// operands if missing.
class DifferentiableFunctionInst final
    : public InstructionBaseWithTrailingOperands<
          SILInstructionKind::DifferentiableFunctionInst,
          DifferentiableFunctionInst,
          OwnershipForwardingSingleValueInstruction> {
private:
  friend SILBuilder;
  /// Differentiability parameter indices.
  IndexSubset *ParameterIndices;
  /// Differentiability result indices.
  IndexSubset *ResultIndices;
  /// Indicates whether derivative function operands (JVP/VJP) exist.
  bool HasDerivativeFunctions;

  DifferentiableFunctionInst(SILDebugLocation DebugLoc,
                             IndexSubset *ParameterIndices,
                             IndexSubset *ResultIndices,
                             SILValue OriginalFunction,
                             ArrayRef<SILValue> DerivativeFunctions,
                             ValueOwnershipKind forwardingOwnershipKind);

  static SILType getDifferentiableFunctionType(SILValue OriginalFunction,
                                               IndexSubset *ParameterIndices,
                                               IndexSubset *ResultIndices);

  static ValueOwnershipKind
  getMergedOwnershipKind(SILValue OriginalFunction,
                         ArrayRef<SILValue> DerivativeFunctions);

public:
  static DifferentiableFunctionInst *
  create(SILModule &Module, SILDebugLocation Loc, IndexSubset *ParameterIndices,
         IndexSubset *ResultIndices, SILValue OriginalFunction,
         Optional<std::pair<SILValue, SILValue>> VJPAndJVPFunctions,
         ValueOwnershipKind forwardingOwnershipKind);

  /// Returns the original function operand.
  SILValue getOriginalFunction() const { return getOperand(0); }

  /// Returns differentiability parameter indices.
  IndexSubset *getParameterIndices() const { return ParameterIndices; }

  /// Returns differentiability result indices.
  IndexSubset *getResultIndices() const { return ResultIndices; }

  /// Returns true if derivative functions (JVP/VJP) exist.
  bool hasDerivativeFunctions() const { return HasDerivativeFunctions; }

  /// Returns the derivative function operands if they exist.
  /// Otherwise, return `None`.
  Optional<std::pair<SILValue, SILValue>>
  getOptionalDerivativeFunctionPair() const {
    if (!HasDerivativeFunctions)
      return None;
    return std::make_pair(getOperand(1), getOperand(2));
  }

  ArrayRef<Operand> getDerivativeFunctionArray() const {
    return getAllOperands().drop_front();
  }

  /// Returns the JVP function operand.
  SILValue getJVPFunction() const {
    assert(HasDerivativeFunctions);
    return getOperand(1);
  }

  /// Returns the VJP function operand.
  SILValue getVJPFunction() const {
    assert(HasDerivativeFunctions);
    return getOperand(2);
  }

  /// Returns the derivative function operand (JVP or VJP) with the given kind.
  SILValue getDerivativeFunction(AutoDiffDerivativeFunctionKind kind) const {
    switch (kind) {
    case AutoDiffDerivativeFunctionKind::JVP:
      return getJVPFunction();
    case AutoDiffDerivativeFunctionKind::VJP:
      return getVJPFunction();
    }
    llvm_unreachable("invalid derivative kind");
  }

  
  /// Returns true iff the operand corresponding to the given extractee kind
  /// exists.
  bool hasExtractee(NormalDifferentiableFunctionTypeComponent extractee) const {
    switch (extractee) {
    case NormalDifferentiableFunctionTypeComponent::Original:
      return true;
    case NormalDifferentiableFunctionTypeComponent::JVP:
    case NormalDifferentiableFunctionTypeComponent::VJP:
      return hasDerivativeFunctions();
    }
    llvm_unreachable("invalid extractee kind");
  }

  /// Returns the operand corresponding to the given extractee kind.
  SILValue
  getExtractee(NormalDifferentiableFunctionTypeComponent extractee) const {
    switch (extractee) {
    case NormalDifferentiableFunctionTypeComponent::Original:
      return getOriginalFunction();
    case NormalDifferentiableFunctionTypeComponent::JVP:
      return getJVPFunction();
    case NormalDifferentiableFunctionTypeComponent::VJP:
      return getVJPFunction();
    }
    llvm_unreachable("invalid extractee kind");
  }
};

/// LinearFunctionInst - given a function, its derivative and transpose functions,
/// create an `@differentiable(_linear)` function that represents a bundle of these.
class LinearFunctionInst final
    : public InstructionBaseWithTrailingOperands<
          SILInstructionKind::LinearFunctionInst, LinearFunctionInst,
          OwnershipForwardingSingleValueInstruction> {
private:
  friend SILBuilder;
  /// Parameters to differentiate with respect to.
  IndexSubset *ParameterIndices;
  /// Indicates whether a transpose function exists.
  bool HasTransposeFunction;

  static SILType getLinearFunctionType(
      SILValue OriginalFunction, IndexSubset *ParameterIndices);

public:
  LinearFunctionInst(SILDebugLocation Loc, IndexSubset *ParameterIndices,
                     SILValue OriginalFunction,
                     Optional<SILValue> TransposeFunction,
                     ValueOwnershipKind forwardingOwnershipKind);

  static LinearFunctionInst *create(SILModule &Module, SILDebugLocation Loc,
                                    IndexSubset *ParameterIndices,
                                    SILValue OriginalFunction,
                                    Optional<SILValue> TransposeFunction,
                                    ValueOwnershipKind forwardingOwnershipKind);

  IndexSubset *getParameterIndices() const { return ParameterIndices; }
  bool hasTransposeFunction() const { return HasTransposeFunction; }
  SILValue getOriginalFunction() const { return getOperand(0); }
  Optional<SILValue> getOptionalTransposeFunction() const {
    return HasTransposeFunction ? Optional<SILValue>(getOperand(1)) : None;
  }
  SILValue getTransposeFunction() const {
    assert(HasTransposeFunction);
    return getOperand(1);
  }

  
  /// Returns true iff the operand corresponding to the given extractee kind
  /// exists.
  bool hasExtractee(LinearDifferentiableFunctionTypeComponent extractee) const {
    switch (extractee) {
    case LinearDifferentiableFunctionTypeComponent::Original:
      return true;
    case LinearDifferentiableFunctionTypeComponent::Transpose:
      return hasTransposeFunction();
    }
    llvm_unreachable("invalid extractee kind");
  }

  /// Returns the operand corresponding to the given extractee kind.
  SILValue
  getExtractee(LinearDifferentiableFunctionTypeComponent extractee) const {
    switch (extractee) {
    case LinearDifferentiableFunctionTypeComponent::Original:
      return getOriginalFunction();
    case LinearDifferentiableFunctionTypeComponent::Transpose:
      return getTransposeFunction();
    }
    llvm_unreachable("invalid extractee kind");
  }
};

/// DifferentiableFunctionExtractInst - extracts either the original or
/// derivative function value from a `@differentiable` function.
class DifferentiableFunctionExtractInst
    : public UnaryInstructionBase<
          SILInstructionKind::DifferentiableFunctionExtractInst,
          OwnershipForwardingSingleValueInstruction> {
private:
  /// The extractee.
  NormalDifferentiableFunctionTypeComponent Extractee;
  /// True if the instruction has an explicit extractee type.
  bool HasExplicitExtracteeType;

  static SILType
  getExtracteeType(SILValue function,
                   NormalDifferentiableFunctionTypeComponent extractee,
                   SILModule &module);

public:
  /// Note: explicit extractee type is used to avoid inconsistent typing in:
  /// - Canonical SIL, due to generic specialization.
  /// - Lowered SIL, due to LoadableByAddress.
  /// - Raw SIL, due to deserialization of canonical/lowered SIL functions.
  /// See `TypeSubstCloner::visitDifferentiableFunctionExtractInst` for an
  /// explanation of how explicit extractee type is used.
  explicit DifferentiableFunctionExtractInst(
      SILModule &module, SILDebugLocation debugLoc,
      NormalDifferentiableFunctionTypeComponent extractee, SILValue function,
      ValueOwnershipKind forwardingOwnershipKind,
      Optional<SILType> extracteeType = None);

  NormalDifferentiableFunctionTypeComponent getExtractee() const {
    return Extractee;
  }

  AutoDiffDerivativeFunctionKind getDerivativeFunctionKind() const {
    auto kind = Extractee.getAsDerivativeFunctionKind();
    assert(kind);
    return *kind;
  }

  bool hasExplicitExtracteeType() const { return HasExplicitExtracteeType; }
};

/// LinearFunctionExtractInst - given an `@differentiable(_linear)` function
/// representing a bundle of the original function and the transpose function,
/// extract the specified function.
class LinearFunctionExtractInst
    : public UnaryInstructionBase<SILInstructionKind::LinearFunctionExtractInst,
                                  OwnershipForwardingSingleValueInstruction> {
private:
  /// The extractee.
  LinearDifferentiableFunctionTypeComponent extractee;

  static SILType
  getExtracteeType(SILValue function,
                   LinearDifferentiableFunctionTypeComponent extractee,
                   SILModule &module);

public:
  explicit LinearFunctionExtractInst(
      SILModule &module, SILDebugLocation debugLoc,
      LinearDifferentiableFunctionTypeComponent extractee, SILValue theFunction,
      ValueOwnershipKind forwardingOwnershipKind);

  LinearDifferentiableFunctionTypeComponent getExtractee() const {
    return extractee;
  }
};

inline bool
OwnershipForwardingSingleValueInstruction::classof(SILInstructionKind kind) {
  switch (kind) {
  case SILInstructionKind::ObjectInst:
  case SILInstructionKind::EnumInst:
  case SILInstructionKind::UncheckedEnumDataInst:
  case SILInstructionKind::OpenExistentialRefInst:
  case SILInstructionKind::InitExistentialRefInst:
  case SILInstructionKind::MarkDependenceInst:
  case SILInstructionKind::MoveOnlyWrapperToCopyableValueInst:
  case SILInstructionKind::MoveOnlyWrapperToCopyableBoxInst:
  case SILInstructionKind::CopyableToMoveOnlyWrapperValueInst:
  case SILInstructionKind::MarkUninitializedInst:
  case SILInstructionKind::TupleExtractInst:
  case SILInstructionKind::StructExtractInst:
  case SILInstructionKind::DifferentiableFunctionExtractInst:
  case SILInstructionKind::LinearFunctionExtractInst:
  case SILInstructionKind::OpenExistentialValueInst:
  case SILInstructionKind::OpenExistentialBoxValueInst:
  case SILInstructionKind::StructInst:
  case SILInstructionKind::TupleInst:
  case SILInstructionKind::LinearFunctionInst:
  case SILInstructionKind::DifferentiableFunctionInst:
  case SILInstructionKind::MarkMustCheckInst:
  case SILInstructionKind::MarkUnresolvedReferenceBindingInst:
  case SILInstructionKind::ConvertFunctionInst:
  case SILInstructionKind::UpcastInst:
  case SILInstructionKind::UncheckedRefCastInst:
  case SILInstructionKind::UncheckedValueCastInst:
  case SILInstructionKind::RefToBridgeObjectInst:
  case SILInstructionKind::BridgeObjectToRefInst:
  case SILInstructionKind::ThinToThickFunctionInst:
  case SILInstructionKind::UnconditionalCheckedCastInst:
    return true;
  default:
    return false;
  }
}

/// DifferentiabilityWitnessFunctionInst - Looks up a differentiability witness
/// function for a given original function.
class DifferentiabilityWitnessFunctionInst
    : public InstructionBase<
          SILInstructionKind::DifferentiabilityWitnessFunctionInst,
          SingleValueInstruction> {
private:
  friend SILBuilder;
  /// The differentiability witness function kind.
  DifferentiabilityWitnessFunctionKind witnessKind;
  /// The referenced SIL differentiability witness.
  SILDifferentiabilityWitness *witness;
  /// Whether the instruction has an explicit function type.
  bool hasExplicitFunctionType;

  static SILType getDifferentiabilityWitnessType(
      SILModule &module, DifferentiabilityWitnessFunctionKind witnessKind,
      SILDifferentiabilityWitness *witness);

public:
  /// Note: explicit function type may be specified only in lowered SIL.
  DifferentiabilityWitnessFunctionInst(
      SILModule &module, SILDebugLocation loc,
      DifferentiabilityWitnessFunctionKind witnessKind,
      SILDifferentiabilityWitness *witness, Optional<SILType> FunctionType);

  DifferentiabilityWitnessFunctionKind getWitnessKind() const {
    return witnessKind;
  }
  SILDifferentiabilityWitness *getWitness() const { return witness; }
  bool getHasExplicitFunctionType() const { return hasExplicitFunctionType; }

  ArrayRef<Operand> getAllOperands() const { return {}; }
  MutableArrayRef<Operand> getAllOperands() { return {}; }
};

// This is defined out of line to work around the fact that this depends on
// PartialApplyInst being defined, but PartialApplyInst is a subclass of
// ApplyInstBase, so we can not place ApplyInstBase after it.
template <class Impl, class Base>
SILValue ApplyInstBase<Impl, Base, false>::getCalleeOrigin() const {
  SILValue Callee = getCallee();
  while (true) {
    if (auto *TTTFI = dyn_cast<ThinToThickFunctionInst>(Callee)) {
      Callee = TTTFI->getCallee();
      continue;
    }
    if (auto *CFI = dyn_cast<ConvertFunctionInst>(Callee)) {
      Callee = CFI->getOperand();
      continue;
    }
    if (auto *CETN = dyn_cast<ConvertEscapeToNoEscapeInst>(Callee)) {
      Callee = CETN->getOperand();
      continue;
    }
    // convert_escape_to_noescape's are within a borrow scope.
    if (auto *beginBorrow = dyn_cast<BeginBorrowInst>(Callee)) {
      Callee = beginBorrow->getOperand();
      continue;
    }
    if (auto *copy = dyn_cast<CopyValueInst>(Callee)) {
      Callee = copy->getOperand();
      continue;
    }
    return Callee;
  }
}

template <class Impl, class Base>
bool ApplyInstBase<Impl, Base, false>::isCalleeDynamicallyReplaceable() const {
  SILValue Callee = getCalleeOrigin();

  while (true) {
    if (isa<FunctionRefInst>(Callee))
      return false;

    if (isa<DynamicFunctionRefInst>(Callee))
      return true;
    if (isa<PreviousDynamicFunctionRefInst>(Callee))
      return true;

    if (auto *PAI = dyn_cast<PartialApplyInst>(Callee)) {
      Callee = PAI->getCalleeOrigin();
      continue;
    }
    return false;
  }
}

template <class Impl, class Base>
SILFunction *ApplyInstBase<Impl, Base, false>::getCalleeFunction() const {
  SILValue Callee = getCalleeOrigin();

  while (true) {
    // Intentionally don't lookup through dynamic_function_ref and
    // previous_dynamic_function_ref as the target of those functions is not
    // statically known.
    if (auto *FRI = dyn_cast<FunctionRefInst>(Callee))
      return FRI->getReferencedFunctionOrNull();

    if (auto *PAI = dyn_cast<PartialApplyInst>(Callee)) {
      Callee = PAI->getCalleeOrigin();
      continue;
    }
    return nullptr;
  }
}

/// The first operand is the ownership equivalent source.
class OwnershipForwardingMultipleValueInstruction
    : public MultipleValueInstruction,
      public ForwardingInstruction {
public:
  OwnershipForwardingMultipleValueInstruction(SILInstructionKind kind,
                                              SILDebugLocation loc,
                                              ValueOwnershipKind ownershipKind)
      : MultipleValueInstruction(kind, loc),
        ForwardingInstruction(kind, ownershipKind) {
    assert(classof(kind) && "Missing subclass from classof?!");
  }

  static bool classof(SILNodePointer node) {
    if (auto *i = dyn_cast<SILInstruction>(node.get()))
      return classof(i);
    return false;
  }

  static bool classof(const SILInstruction *i) { return classof(i->getKind()); }

  static bool classof(SILInstructionKind kind) {
    switch (kind) {
    case SILInstructionKind::DestructureTupleInst:
    case SILInstructionKind::DestructureStructInst:
      return true;
    default:
      return false;
    }
  }
};

/// Instruction that takes in a struct value and splits the struct into the
/// struct's fields.
class DestructureStructInst final
    : public UnaryInstructionBase<SILInstructionKind::DestructureStructInst,
                                  OwnershipForwardingMultipleValueInstruction>,
      public MultipleValueInstructionTrailingObjects<DestructureStructInst> {
  friend TrailingObjects;

  DestructureStructInst(SILModule &M, SILDebugLocation Loc, SILValue Operand,
                        ArrayRef<SILType> Types,
                        ArrayRef<ValueOwnershipKind> OwnershipKinds,
                        ValueOwnershipKind forwardingOwnershipKind)
      : UnaryInstructionBase(Loc, Operand, forwardingOwnershipKind),
        MultipleValueInstructionTrailingObjects(this, Types, OwnershipKinds) {}

public:
  static DestructureStructInst *
  create(const SILFunction &F, SILDebugLocation Loc, SILValue Operand,
         ValueOwnershipKind forwardingOwnershipKind);

  StructDecl *getStructDecl() const {
    return getOperand()->getType().getStructOrBoundGenericStruct();
  }

  static bool classof(SILNodePointer node) {
    return node->getKind() == SILNodeKind::DestructureStructInst;
  }
};

/// Instruction that takes in a tuple value and splits the tuple into the
/// tuples's elements.
class DestructureTupleInst final
    : public UnaryInstructionBase<SILInstructionKind::DestructureTupleInst,
                                  OwnershipForwardingMultipleValueInstruction>,
      public MultipleValueInstructionTrailingObjects<DestructureTupleInst> {
  friend TrailingObjects;

  DestructureTupleInst(SILModule &M, SILDebugLocation Loc, SILValue Operand,
                       ArrayRef<SILType> Types,
                       ArrayRef<ValueOwnershipKind> OwnershipKinds,
                       ValueOwnershipKind forwardingOwnershipKind)
      : UnaryInstructionBase(Loc, Operand, forwardingOwnershipKind),
        MultipleValueInstructionTrailingObjects(this, Types, OwnershipKinds) {}

public:
  static DestructureTupleInst *
  create(const SILFunction &F, SILDebugLocation Loc, SILValue Operand,
         ValueOwnershipKind forwardingOwnershipKind);
  static bool classof(SILNodePointer node) {
    return node->getKind() == SILNodeKind::DestructureTupleInst;
  }
};

inline SILType *AllocRefInstBase::getTypeStorage() {
  // If the size of the subclasses are equal, then all of this compiles away.
  if (auto I = dyn_cast<AllocRefInst>(this))
    return I->getTrailingObjects<SILType>();
  if (auto I = dyn_cast<AllocRefDynamicInst>(this))
    return I->getTrailingObjects<SILType>();
  llvm_unreachable("Unhandled AllocRefInstBase subclass");
}

inline ArrayRef<Operand> AllocRefInstBase::getAllOperands() const {
  // If the size of the subclasses are equal, then all of this compiles away.
  if (auto I = dyn_cast<AllocRefInst>(this))
    return I->getAllOperands();
  if (auto I = dyn_cast<AllocRefDynamicInst>(this))
    return I->getAllOperands();
  llvm_unreachable("Unhandled AllocRefInstBase subclass");
}

inline MutableArrayRef<Operand> AllocRefInstBase::getAllOperands() {
  // If the size of the subclasses are equal, then all of this compiles away.
  if (auto I = dyn_cast<AllocRefInst>(this))
    return I->getAllOperands();
  if (auto I = dyn_cast<AllocRefDynamicInst>(this))
    return I->getAllOperands();
  llvm_unreachable("Unhandled AllocRefInstBase subclass");
}

inline ArrayRef<Operand> SelectEnumInstBase::getAllOperands() const {
  // If the size of the subclasses are equal, then all of this compiles away.
  if (auto I = dyn_cast<SelectEnumInst>(this))
    return I->getAllOperands();
  if (auto I = dyn_cast<SelectEnumAddrInst>(this))
    return I->getAllOperands();
  llvm_unreachable("Unhandled SelectEnumInstBase subclass");
}

inline MutableArrayRef<Operand> SelectEnumInstBase::getAllOperands() {
  // If the size of the subclasses are equal, then all of this compiles away.
  if (auto I = dyn_cast<SelectEnumInst>(this))
    return I->getAllOperands();
  if (auto I = dyn_cast<SelectEnumAddrInst>(this))
    return I->getAllOperands();
  llvm_unreachable("Unhandled SelectEnumInstBase subclass");
}

inline EnumElementDecl **SelectEnumInstBase::getEnumElementDeclStorage() {
  // If the size of the subclasses are equal, then all of this compiles away.
  if (auto I = dyn_cast<SelectEnumInst>(this))
    return I->getTrailingObjects<EnumElementDecl*>();
  if (auto I = dyn_cast<SelectEnumAddrInst>(this))
    return I->getTrailingObjects<EnumElementDecl*>();
  llvm_unreachable("Unhandled SelectEnumInstBase subclass");
}

inline void SILSuccessor::pred_iterator::cacheBasicBlock() {
  if (Cur != nullptr) {
    Block = Cur->ContainingInst->getParent();
    assert(Block != nullptr);
  } else {
    Block = nullptr;
  }
}

// Declared in SILValue.h
inline bool Operand::isTypeDependent() const {
  return getUser()->isTypeDependentOperand(*this);
}

inline bool ForwardingInstruction::isa(SILInstructionKind kind) {
  return OwnershipForwardingSingleValueInstruction::classof(kind) ||
         OwnershipForwardingTermInst::classof(kind) ||
         OwnershipForwardingSelectEnumInstBase::classof(kind) ||
         OwnershipForwardingMultipleValueInstruction::classof(kind);
}

inline ForwardingInstruction *ForwardingInstruction::get(SILInstruction *inst) {
  // I am purposely performing this cast in this manner rather than reinterpret
  // casting to ForwardingInstruction to ensure that we offset to the
  // appropriate offset inside of inst instead of converting inst's current
  // location to an ForwardingInstruction which would be incorrect.
  if (auto *result = dyn_cast<OwnershipForwardingSingleValueInstruction>(inst))
    return result;
  if (auto *result = dyn_cast<OwnershipForwardingTermInst>(inst))
    return result;
  if (auto *result = dyn_cast<OwnershipForwardingSelectEnumInstBase>(inst))
    return result;
  if (auto *result =
          dyn_cast<OwnershipForwardingMultipleValueInstruction>(inst))
    return result;
  return nullptr;
}

inline bool MultipleValueInstructionResult::isBeginApplyToken() const {
  return getParent<BeginApplyInst>()->getTokenResult() == this;
}


} // end swift namespace

//===----------------------------------------------------------------------===//
// ilist_traits for SILInstruction
//===----------------------------------------------------------------------===//

namespace llvm {

template <>
struct ilist_traits<::swift::SILInstruction> :
  public ilist_node_traits<::swift::SILInstruction> {
  using SILInstruction = ::swift::SILInstruction;

private:
  swift::SILBasicBlock *getContainingBlock();

  using instr_iterator = simple_ilist<SILInstruction>::iterator;

public:
  static void deleteNode(SILInstruction *V) {
    SILInstruction::destroy(V);
  }

  void addNodeToList(SILInstruction *I);
  void transferNodesFromList(ilist_traits<SILInstruction> &L2,
                             instr_iterator first, instr_iterator last);

private:
  void createNode(const SILInstruction &);
};

template <>
struct DenseMapInfo<swift::SILDebugVariable> {
  using KeyTy = swift::SILDebugVariable;
  static inline KeyTy getEmptyKey() {
    return KeyTy(KeyTy::IsDenseMapSingleton::IsEmpty);
  }
  static inline KeyTy getTombstoneKey() {
    return KeyTy(KeyTy::IsDenseMapSingleton::IsTombstone);
  }
  static unsigned getHashValue(const KeyTy &Val) { return hash_value(Val); }
  static bool isEqual(const KeyTy &LHS, const KeyTy &RHS) { return LHS == RHS; }
};

} // end llvm namespace

#endif
