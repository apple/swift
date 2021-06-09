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

template <typename ImplClass> class SILClonerWithScopes;

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
/// due to the restricted problem space. Specificially:
///
/// 1. Size == 0 implies nothing is stored and thus element size is irrelevent.
/// 2. Size == 1 implies we either had a single value instruction or a multiple
/// value instruction, but no matter what instruction we had, we are going to
/// store the results at the same starting location so element size is
/// irrelevent.
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
  return llvm::make_reverse_iterator(end());
}

inline SILInstructionResultArray::reverse_iterator
SILInstructionResultArray::rend() const {
  return llvm::make_reverse_iterator(begin());
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
  /// location's kindAndFlags is stored in the SILNode inline bitfields.
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
  /// It is used only for collecting the compiler statistics.
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
  friend class LibswiftPassInvocation;

  SILInstruction() {
    NumCreatedInstructions++;
  }

  /// This method unlinks 'self' from the containing basic block.
  void removeFromParent();

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
  bool isDeleted() const { return !ParentBB; }

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
    return SILLocation(locationStorage,
                       asSILNode()->Bits.SILInstruction.LocationKindAndFlags);
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
    asSILNode()->Bits.SILInstruction.LocationKindAndFlags =
      loc.kindAndFlags.packedKindAndFlags;
    locationStorage = loc.storage;
  }

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
  /// Predicate used to filter OperandValueRange.
  struct OperandToValue;
  /// Predicate used to filter TransformedOperandValueRange.
  struct OperandToTransformedValue;

public:
  using OperandValueRange =
      OptionalTransformRange<ArrayRef<Operand>, OperandToValue>;
  using TransformedOperandValueRange =
      OptionalTransformRange<ArrayRef<Operand>, OperandToTransformedValue>;

  OperandValueRange
  getOperandValues(bool skipTypeDependentOperands = false) const;
  TransformedOperandValueRange
  getOperandValues(std::function<SILValue(SILValue)> transformFn,
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
  template <typename OpCmp>
  bool isIdenticalTo(const SILInstruction *RHS, OpCmp &&opEqual) const {
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

  /// Returns true if the instruction may have side effects.
  ///
  /// Instructions that store into memory or change retain counts as well as
  /// calls and deallocation instructions are considered to have side effects
  /// that are not visible by merely examining their uses.
  bool mayHaveSideEffects() const;

  /// Returns true if the instruction may write to memory, deinitialize memory,
  /// or have other unknown side effects.
  ///
  /// For details see SILInstruction::MemoryBehavior.
  bool mayWriteToMemory() const {
    MemoryBehavior B = getMemoryBehavior();
    return B == MemoryBehavior::MayWrite ||
      B == MemoryBehavior::MayReadWrite ||
      B == MemoryBehavior::MayHaveSideEffects;
  }

  /// Returns true if the instruction may read from memory, or have other
  /// unknown side effects.
  ///
  /// For details see SILInstruction::MemoryBehavior.
  bool mayReadFromMemory() const {
    MemoryBehavior B = getMemoryBehavior();
    return B == MemoryBehavior::MayRead ||
      B == MemoryBehavior::MayReadWrite ||
      B == MemoryBehavior::MayHaveSideEffects;
  }

  /// Returns true if the instruction may read from memory, write to memory,
  /// deinitialize memory, or have other unknown side effects.
  ///
  /// For details see SILInstruction::MemoryBehavior.
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
    return getKind() == SILInstructionKind::DebugValueInst ||
           getKind() == SILInstructionKind::DebugValueAddrInst;
  }

  /// Returns true if the instruction is a meta instruction which is
  /// relevant for debug information and does not get lowered to a real
  /// instruction.
  bool isMetaInstruction() const;

  /// Verify that all operands of this instruction have compatible ownership
  /// with this instruction.
  void verifyOperandOwnership() const;

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
  // Even if this insttruction is not a NonSingleValueInstruction, but a
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
  const SILInstruction &i;
  bool skipTypeDependentOps;

  OperandToValue(const SILInstruction &i, bool skipTypeDependentOps)
      : i(i), skipTypeDependentOps(skipTypeDependentOps) {}

  Optional<SILValue> operator()(const Operand &use) const {
    if (skipTypeDependentOps && i.isTypeDependentOperand(use))
      return None;
    return use.get();
  }
};

struct SILInstruction::OperandToTransformedValue {
  const SILInstruction &i;
  std::function<SILValue(SILValue)> transformFn;
  bool skipTypeDependentOps;

  OperandToTransformedValue(const SILInstruction &i,
                            std::function<SILValue(SILValue)> transformFn,
                            bool skipTypeDependentOps)
      : i(i), transformFn(transformFn),
        skipTypeDependentOps(skipTypeDependentOps) {}

  Optional<SILValue> operator()(const Operand &use) const {
    if (skipTypeDependentOps && i.isTypeDependentOperand(use))
      return None;
    return transformFn(use.get());
  }
};

inline auto
SILInstruction::getOperandValues(bool skipTypeDependentOperands) const
    -> OperandValueRange {
  return OperandValueRange(getAllOperands(),
                           OperandToValue(*this, skipTypeDependentOperands));
}

inline auto
SILInstruction::getOperandValues(std::function<SILValue(SILValue)> transformFn,
                                 bool skipTypeDependentOperands) const
    -> TransformedOperandValueRange {
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
inline SILInstruction::MemoryBehavior
combineMemoryBehavior(SILInstruction::MemoryBehavior B1,
                  SILInstruction::MemoryBehavior B2) {
  // Basically the combined behavior is the maximum of both operands.
  auto Result = std::max(B1, B2);

  // With one exception: MayRead, MayWrite -> MayReadWrite.
  if (Result == SILInstruction::MemoryBehavior::MayWrite &&
        (B1 == SILInstruction::MemoryBehavior::MayRead ||
         B2 == SILInstruction::MemoryBehavior::MayRead))
    return SILInstruction::MemoryBehavior::MayReadWrite;
  return Result;
}

/// Pretty-print the MemoryBehavior.
llvm::raw_ostream &operator<<(llvm::raw_ostream &OS,
                              SILInstruction::MemoryBehavior B);
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

  /// If this is an instruction which "defines" an opened archetype, it is
  /// returned.
  CanArchetypeType getOpenedArchetype() const;
};

struct SILNodeOffsetChecker {
  static_assert(offsetof(SingleValueInstruction, Bits) ==
                offsetof(NonSingleValueInstruction, Bits),
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
class OwnershipForwardingMixin {
  ValueOwnershipKind ownershipKind;

protected:
  OwnershipForwardingMixin(SILInstructionKind kind,
                           ValueOwnershipKind ownershipKind)
      : ownershipKind(ownershipKind) {
    assert(isa(kind) && "Invalid subclass?!");
  }

public:
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
    ownershipKind = newKind;
  }

  /// Defined inline below due to forward declaration issues.
  static OwnershipForwardingMixin *get(SILInstruction *inst);
  /// Defined inline below due to forward declaration issues.
  static bool isa(SILInstructionKind kind);
  static bool isa(const SILInstruction *inst) { return isa(inst->getKind()); }
  static bool isa(SILNodePointer node) {
    if (auto *i = dyn_cast<const SILInstruction>(node.get()))
      return isa(i);
    return false;
  }
};

/// A single value inst that forwards a static ownership from one (or all) of
/// its operands.
///
/// The ownership kind is set on construction and afterwards must be changed
/// explicitly using setOwnershipKind().
class FirstArgOwnershipForwardingSingleValueInst
    : public SingleValueInstruction,
      public OwnershipForwardingMixin {
protected:
  FirstArgOwnershipForwardingSingleValueInst(SILInstructionKind kind,
                                             SILDebugLocation debugLoc,
                                             SILType ty,
                                             ValueOwnershipKind ownershipKind)
      : SingleValueInstruction(kind, debugLoc, ty),
        OwnershipForwardingMixin(kind, ownershipKind) {
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

/// An ownership forwarding single value that has a preferred operand of owned
/// but if its inputs are all none can have OwnershipKind::None as a result. We
/// assume that we always forward from operand 0.
class OwnedFirstArgForwardingSingleValueInst
    : public FirstArgOwnershipForwardingSingleValueInst {
protected:
  OwnedFirstArgForwardingSingleValueInst(SILInstructionKind kind,
                                         SILDebugLocation debugLoc, SILType ty,
                                         ValueOwnershipKind resultOwnershipKind)
      : FirstArgOwnershipForwardingSingleValueInst(kind, debugLoc, ty,
                                                   resultOwnershipKind) {
    assert(resultOwnershipKind.isCompatibleWith(OwnershipKind::Owned));
    assert(classof(kind) && "classof missing new subclass?!");
  }

public:
  ValueOwnershipKind getPreferredOwnership() const {
    return OwnershipKind::Owned;
  }

  static bool classof(SILNodePointer node) {
    if (auto *i = dyn_cast<SILInstruction>(node.get()))
      return classof(i);
    return false;
  }

  static bool classof(SILInstructionKind kind) {
    switch (kind) {
    case SILInstructionKind::MarkUninitializedInst:
      return true;
    default:
      return false;
    }
  }

  static bool classof(const SILInstruction *inst) {
    return classof(inst->getKind());
  }
};

/// An instruction that forwards guaranteed or none ownership. Assumed to always
/// forward from Operand(0) -> Result(0).
class GuaranteedFirstArgForwardingSingleValueInst
    : public FirstArgOwnershipForwardingSingleValueInst {
protected:
  GuaranteedFirstArgForwardingSingleValueInst(
      SILInstructionKind kind, SILDebugLocation debugLoc, SILType ty,
      ValueOwnershipKind resultOwnershipKind)
      : FirstArgOwnershipForwardingSingleValueInst(kind, debugLoc, ty,
                                                   resultOwnershipKind) {
    assert(resultOwnershipKind.isCompatibleWith(OwnershipKind::Guaranteed));
    assert(classof(kind) && "classof missing new subclass?!");
  }

public:
  ValueOwnershipKind getPreferredOwnership() const {
    return OwnershipKind::Guaranteed;
  }

  static bool classof(SILNodePointer node) {
    if (auto *i = dyn_cast<SILInstruction>(node.get()))
      return classof(i);
    return false;
  }

  static bool classof(SILInstructionKind kind) {
    switch (kind) {
    case SILInstructionKind::TupleExtractInst:
    case SILInstructionKind::StructExtractInst:
    case SILInstructionKind::DifferentiableFunctionExtractInst:
    case SILInstructionKind::LinearFunctionExtractInst:
    case SILInstructionKind::OpenExistentialValueInst:
    case SILInstructionKind::OpenExistentialBoxValueInst:
      return true;
    default:
      return false;
    }
  }

  static bool classof(const SILInstruction *inst) {
    return classof(inst->getKind());
  }
};

inline bool
FirstArgOwnershipForwardingSingleValueInst::classof(SILInstructionKind kind) {
  if (OwnedFirstArgForwardingSingleValueInst::classof(kind))
    return true;
  if (GuaranteedFirstArgForwardingSingleValueInst::classof(kind))
    return true;

  switch (kind) {
  case SILInstructionKind::ObjectInst:
  case SILInstructionKind::EnumInst:
  case SILInstructionKind::UncheckedEnumDataInst:
  case SILInstructionKind::SelectValueInst:
  case SILInstructionKind::OpenExistentialRefInst:
  case SILInstructionKind::InitExistentialRefInst:
  case SILInstructionKind::MarkDependenceInst:
    return true;
  default:
    return false;
  }
}

class AllArgOwnershipForwardingSingleValueInst
    : public SingleValueInstruction,
      public OwnershipForwardingMixin {
protected:
  AllArgOwnershipForwardingSingleValueInst(SILInstructionKind kind,
                                           SILDebugLocation debugLoc,
                                           SILType ty,
                                           ValueOwnershipKind ownershipKind)
      : SingleValueInstruction(kind, debugLoc, ty),
        OwnershipForwardingMixin(kind, ownershipKind) {
    assert(classof(kind) && "classof missing new subclass?!");
  }

public:
  static bool classof(SILNodePointer node) {
    if (auto *i = dyn_cast<SILInstruction>(node.get()))
      return classof(i);
    return false;
  }

  static bool classof(SILInstructionKind kind) {
    switch (kind) {
    case SILInstructionKind::StructInst:
    case SILInstructionKind::TupleInst:
    case SILInstructionKind::LinearFunctionInst:
    case SILInstructionKind::DifferentiableFunctionInst:
      return true;
    default:
      return false;
    }
  }

  static bool classof(const SILInstruction *inst) {
    return classof(inst->getKind());
  }
};

/// A value base result of a multiple value instruction.
///
/// *NOTE* We want this to be a pure abstract class that does not add /any/ size
/// to subclasses.
class MultipleValueInstructionResult : public ValueBase {
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
  /// support subclasses up to 32 bytes in size. We can scavange up to 6 more
  /// bits from ValueBase if this is not large enough.
  MultipleValueInstructionResult(unsigned index, SILType type,
                                 ValueOwnershipKind ownershipKind);

  template <class Inst = MultipleValueInstruction>
  Inst *getParent() const { return cast<Inst>(getParentImpl()); }

  unsigned getIndex() const {
    return Bits.MultipleValueInstructionResult.Index;
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
  friend llvm::TrailingObjects<Derived, Operand, OtherTrailingTypes...>;

  using TrailingObjects =
      llvm::TrailingObjects<Derived, Operand, OtherTrailingTypes...>;

  using TrailingObjects::totalSizeToAlloc;

public:
  template <typename... Args>
  InstructionBaseWithTrailingOperands(ArrayRef<SILValue> Operands,
                                      Args &&...args)
        : InstructionBase<Kind, Base>(std::forward<Args>(args)...) {
    SILNode::Bits.IBWTO.NumOperands = Operands.size();
    TrailingOperandsList::InitOperandsList(getAllOperands().begin(), this,
                                           Operands);
  }

  template <typename... Args>
  InstructionBaseWithTrailingOperands(SILValue Operand0,
                                      ArrayRef<SILValue> Operands,
                                      Args &&...args)
        : InstructionBase<Kind, Base>(std::forward<Args>(args)...) {
    SILNode::Bits.IBWTO.NumOperands = Operands.size() + 1;
    TrailingOperandsList::InitOperandsList(getAllOperands().begin(), this,
                                           Operand0, Operands);
  }

  template <typename... Args>
  InstructionBaseWithTrailingOperands(SILValue Operand0,
                                      SILValue Operand1,
                                      ArrayRef<SILValue> Operands,
                                      Args &&...args)
        : InstructionBase<Kind, Base>(std::forward<Args>(args)...) {
    SILNode::Bits.IBWTO.NumOperands = Operands.size() + 2;
    TrailingOperandsList::InitOperandsList(getAllOperands().begin(), this,
                                           Operand0, Operand1, Operands);
  }

  // Destruct tail allocated objects.
  ~InstructionBaseWithTrailingOperands() {
    Operand *Operands = TrailingObjects::template getTrailingObjects<Operand>();
    auto end = SILNode::Bits.IBWTO.NumOperands;
    for (unsigned i = 0; i < end; ++i) {
      Operands[i].~Operand();
    }
  }

  size_t numTrailingObjects(typename TrailingObjects::template
                            OverloadToken<Operand>) const {
    return SILNode::Bits.IBWTO.NumOperands;
  }

  ArrayRef<Operand> getAllOperands() const {
    return {TrailingObjects::template getTrailingObjects<Operand>(),
            SILNode::Bits.IBWTO.NumOperands};
  }

  MutableArrayRef<Operand> getAllOperands() {
    return {TrailingObjects::template getTrailingObjects<Operand>(),
            SILNode::Bits.IBWTO.NumOperands};
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

  ArrayRef<Operand> getTypeDependentOperands() const {
    return this->getAllOperands().slice(1);
  }

  MutableArrayRef<Operand> getTypeDependentOperands() {
    return this->getAllOperands().slice(1);
  }
};

/// Holds common debug information about local variables and function
/// arguments that are needed by DebugValueInst, DebugValueAddrInst,
/// AllocStackInst, and AllocBoxInst.
struct SILDebugVariable {
  StringRef Name;
  unsigned ArgNo : 16;
  unsigned Constant : 1;

  SILDebugVariable() : ArgNo(0), Constant(false) {}
  SILDebugVariable(bool Constant, uint16_t ArgNo)
      : ArgNo(ArgNo), Constant(Constant) {}
  SILDebugVariable(StringRef Name, bool Constant, unsigned ArgNo)
      : Name(Name), ArgNo(ArgNo), Constant(Constant) {}
  bool operator==(const SILDebugVariable &V) {
    return ArgNo == V.ArgNo && Constant == V.Constant && Name == V.Name;
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
      /// When this is nonzero there is a tail-allocated string storing
      /// variable name present. This typically only happens for
      /// instructions that were created from parsing SIL assembler.
      int_type NameLength : 14;
      /// The source function argument position from left to right
      /// starting with 1 or 0 if this is a local variable.
      int_type ArgNo : 16;
    } Data;
  } Bits;
public:
  TailAllocatedDebugVariable(Optional<SILDebugVariable>, char *buf);
  TailAllocatedDebugVariable(int_type RawValue) { Bits.RawValue = RawValue; }
  int_type getRawValue() const { return Bits.RawValue; }

  unsigned getArgNo() const { return Bits.Data.ArgNo; }
  void setArgNo(unsigned N) { Bits.Data.ArgNo = N; }
  /// Returns the name of the source variable, if it is stored in the
  /// instruction.
  StringRef getName(const char *buf) const;
  bool isLet() const { return Bits.Data.Constant; }

  Optional<SILDebugVariable> get(VarDecl *VD, const char *buf) const {
    if (!Bits.Data.HasValue)
      return None;
    if (VD)
      return SILDebugVariable(VD->getName().empty() ? "" : VD->getName().str(),
                              VD->isLet(), getArgNo());
    else
      return SILDebugVariable(getName(buf), isLet(), getArgNo());
  }
};
static_assert(sizeof(TailAllocatedDebugVariable) == 4,
              "SILNode inline bitfield needs updating");

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
      private llvm::TrailingObjects<AllocStackInst, Operand, char> {
  friend TrailingObjects;
  friend SILBuilder;

  bool dynamicLifetime = false;

  AllocStackInst(SILDebugLocation Loc, SILType elementType,
                 ArrayRef<SILValue> TypeDependentOperands,
                 SILFunction &F,
                 Optional<SILDebugVariable> Var, bool hasDynamicLifetime);

  static AllocStackInst *create(SILDebugLocation Loc, SILType elementType,
                                SILFunction &F,
                                Optional<SILDebugVariable> Var,
                                bool hasDynamicLifetime);

  size_t numTrailingObjects(OverloadToken<Operand>) const {
    return SILNode::Bits.AllocStackInst.NumOperands;
  }

public:
  ~AllocStackInst() {
    Operand *Operands = getTrailingObjects<Operand>();
    size_t end = SILNode::Bits.AllocStackInst.NumOperands;
    for (unsigned i = 0; i < end; ++i) {
      Operands[i].~Operand();
    }
  }

  /// Set to true that this alloc_stack contains a value whose lifetime can not
  /// be ascertained from uses.
  ///
  /// As an example if an alloc_stack is known to be only conditionally
  /// initialized.
  void setDynamicLifetime() { dynamicLifetime = true; }

  /// Returns true if the alloc_stack's initialization can not be ascertained
  /// from uses directly (so should be treated conservatively).
  ///
  /// An example of an alloc_stack with dynamic lifetime is an alloc_stack that
  /// is conditionally initialized.
  bool hasDynamicLifetime() const { return dynamicLifetime; }

  /// Return the debug variable information attached to this instruction.
  Optional<SILDebugVariable> getVarInfo() const {
    auto RawValue = SILNode::Bits.AllocStackInst.VarInfo;
    auto VI = TailAllocatedDebugVariable(RawValue);
    return VI.get(getDecl(), getTrailingObjects<char>());
  };
  void setArgNo(unsigned N) {
    auto RawValue = SILNode::Bits.AllocStackInst.VarInfo;
    auto VI = TailAllocatedDebugVariable(RawValue);
    VI.setArgNo(N);
    SILNode::Bits.AllocStackInst.VarInfo = VI.getRawValue();
  }

  /// getElementType - Get the type of the allocated memory (as opposed to the
  /// type of the instruction itself, which will be an address type).
  SILType getElementType() const {
    return getType().getObjectType();
  }

  ArrayRef<Operand> getAllOperands() const {
    return { getTrailingObjects<Operand>(),
             static_cast<size_t>(SILNode::Bits.AllocStackInst.NumOperands) };
  }

  MutableArrayRef<Operand> getAllOperands() {
    return { getTrailingObjects<Operand>(),
             static_cast<size_t>(SILNode::Bits.AllocStackInst.NumOperands) };
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

/// The base class for AllocRefInst and AllocRefDynamicInst.
///
/// The first NumTailTypes operands are counts for the tail allocated
/// elements, the remaining operands are opened archetype operands.
class AllocRefInstBase : public AllocationInst {
protected:

  AllocRefInstBase(SILInstructionKind Kind,
                   SILDebugLocation DebugLoc,
                   SILType ObjectType,
                   bool objc, bool canBeOnStack,
                   ArrayRef<SILType> ElementTypes);

  SILType *getTypeStorage();
  const SILType *getTypeStorage() const {
    return const_cast<AllocRefInstBase*>(this)->getTypeStorage();
  }

  unsigned getNumTailTypes() const {
    return SILNode::Bits.AllocRefInstBase.NumTailTypes;
  }

public:
  bool canAllocOnStack() const {
    return SILNode::Bits.AllocRefInstBase.OnStack;
  }

  void setStackAllocatable(bool OnStack = true) {
    SILNode::Bits.AllocRefInstBase.OnStack = OnStack;
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
  bool isObjC() const {
    return SILNode::Bits.AllocRefInstBase.ObjC;
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
                      ArrayRef<SILType> ElementTypes,
                      ArrayRef<SILValue> AllOperands)
      : InstructionBaseWithTrailingOperands(AllOperands, DebugLoc, ty, objc,
                                            false, ElementTypes) {
    assert(AllOperands.size() >= ElementTypes.size() + 1);
    std::uninitialized_copy(ElementTypes.begin(), ElementTypes.end(),
                            getTrailingObjects<SILType>());
  }

  static AllocRefDynamicInst *
  create(SILDebugLocation DebugLoc, SILFunction &F,
         SILValue metatypeOperand, SILType ty, bool objc,
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
};

/// AllocValueBufferInst - Allocate memory in a value buffer.
class AllocValueBufferInst final
    : public UnaryInstructionWithTypeDependentOperandsBase<
                                  SILInstructionKind::AllocValueBufferInst,
                                  AllocValueBufferInst,
                                  AllocationInst> {
  friend SILBuilder;

  AllocValueBufferInst(SILDebugLocation DebugLoc, SILType valueType,
                       SILValue operand,
                       ArrayRef<SILValue> TypeDependentOperands);

  static AllocValueBufferInst *
  create(SILDebugLocation DebugLoc, SILType valueType, SILValue operand,
         SILFunction &F);

public:
  SILType getValueType() const { return getType().getObjectType(); }
};

/// This represents the allocation of a heap box for a Swift value of some type.
/// The instruction returns two values.  The first return value is the object
/// pointer with Builtin.NativeObject type.  The second return value
/// is an address pointing to the contained element. The contained
/// element is uninitialized.
class AllocBoxInst final
    : public InstructionBaseWithTrailingOperands<
                                           SILInstructionKind::AllocBoxInst,
                                           AllocBoxInst, AllocationInst, char> {
  friend SILBuilder;

  TailAllocatedDebugVariable VarInfo;

  bool dynamicLifetime = false;

  AllocBoxInst(SILDebugLocation DebugLoc, CanSILBoxType BoxType,
               ArrayRef<SILValue> TypeDependentOperands, SILFunction &F,
               Optional<SILDebugVariable> Var, bool hasDynamicLifetime);

  static AllocBoxInst *create(SILDebugLocation Loc, CanSILBoxType boxType,
                              SILFunction &F,
                              Optional<SILDebugVariable> Var,
                              bool hasDynamicLifetime);

public:
  CanSILBoxType getBoxType() const {
    return getType().castTo<SILBoxType>();
  }

  void setDynamicLifetime() { dynamicLifetime = true; }
  bool hasDynamicLifetime() const { return dynamicLifetime; }

  // Return the type of the memory stored in the alloc_box.
  SILType getAddressType() const;

  /// Return the debug variable information attached to this instruction.
  Optional<SILDebugVariable> getVarInfo() const {
    return VarInfo.get(getDecl(), getTrailingObjects<char>());
  };

  ArrayRef<Operand> getTypeDependentOperands() const {
    return getAllOperands();
  }

  MutableArrayRef<Operand> getTypeDependentOperands() {
    return getAllOperands();
  }
};

/// This represents the allocation of a heap box for an existential container.
/// The instruction returns two values.  The first return value is the owner
/// pointer, which has the existential type.  The second return value
/// is an address pointing to the contained element. The contained
/// value is uninitialized.
class AllocExistentialBoxInst final
    : public InstructionBaseWithTrailingOperands<
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
    : InstructionBaseWithTrailingOperands(TypeDependentOperands, DebugLoc,
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

  ArrayRef<Operand> getTypeDependentOperands() const {
    return getAllOperands();
  }

  MutableArrayRef<Operand> getTypeDependentOperands() {
    return getAllOperands();
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
    if (SubstCallee->getNumAllResults() != 1)
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

  HopToExecutorInst(SILDebugLocation debugLoc, SILValue executor,
                    bool hasOwnership)
      : UnaryInstructionBase(debugLoc, executor) { }

public:
  SILValue getTargetExecutor() const { return getOperand(); }
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
  unsigned NumOperands;
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
              ArrayRef<SILValue> Args,
              SILType Ty);
  
  size_t numTrailingObjects(OverloadToken<Operand>) const {
    return NumOperands;
  }
  
public:
  KeyPathPattern *getPattern() const;
  bool hasPattern() const { return (bool)Pattern; }

  ArrayRef<Operand> getAllOperands() const {
    return const_cast<KeyPathInst*>(this)->getAllOperands();
  }
  MutableArrayRef<Operand> getAllOperands();

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

public:
  enum class Encoding {
    Bytes,
    UTF8,
    /// UTF-8 encoding of an Objective-C selector.
    ObjCSelector,
  };

private:
  StringLiteralInst(SILDebugLocation DebugLoc, StringRef text,
                    Encoding encoding, SILType ty);

  static StringLiteralInst *create(SILDebugLocation DebugLoc, StringRef Text,
                                   Encoding encoding, SILModule &M);

public:
  /// getValue - Return the string data for the literal, in UTF-8.
  StringRef getValue() const {
    return {getTrailingObjects<char>(),
            SILNode::Bits.StringLiteralInst.Length};
  }

  /// getEncoding - Return the desired encoding of the text.
  Encoding getEncoding() const {
    return Encoding(SILNode::Bits.StringLiteralInst.TheEncoding);
  }

  /// getCodeUnitCount - Return encoding-based length of the string
  /// literal in code units.
  uint64_t getCodeUnitCount();

  ArrayRef<Operand> getAllOperands() const { return {}; }
  MutableArrayRef<Operand> getAllOperands() { return {}; }
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
    SILNode::Bits.LoadInst.OwnershipQualifier = unsigned(Q);
  }

public:
  LoadOwnershipQualifier getOwnershipQualifier() const {
    return LoadOwnershipQualifier(
      SILNode::Bits.LoadInst.OwnershipQualifier);
  }
  void setOwnershipQualifier(LoadOwnershipQualifier qualifier) {
    SILNode::Bits.LoadInst.OwnershipQualifier = unsigned(qualifier);
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

  StoreInst(SILDebugLocation DebugLoc, SILValue Src, SILValue Dest,
            StoreOwnershipQualifier Qualifier);

public:
  SILValue getSrc() const { return Operands[Src].get(); }
  SILValue getDest() const { return Operands[Dest].get(); }

  ArrayRef<Operand> getAllOperands() const { return Operands.asArray(); }
  MutableArrayRef<Operand> getAllOperands() { return Operands.asArray(); }

  StoreOwnershipQualifier getOwnershipQualifier() const {
    return StoreOwnershipQualifier(
      SILNode::Bits.StoreInst.OwnershipQualifier);
  }
  void setOwnershipQualifier(StoreOwnershipQualifier qualifier) {
    SILNode::Bits.StoreInst.OwnershipQualifier = unsigned(qualifier);
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

  BeginBorrowInst(SILDebugLocation DebugLoc, SILValue LValue)
      : UnaryInstructionBase(DebugLoc, LValue,
                             LValue->getType().getObjectType()) {}

public:
  using EndBorrowRange =
      decltype(std::declval<ValueBase>().getUsersOfType<EndBorrowInst>());

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

public:
  /// Return the value that this end_borrow is ending the borrow of if we are
  /// borrowing a single value.
  SILValue getSingleOriginalValue() const {
    SILValue v = getOperand();
    if (auto *bbi = dyn_cast<BeginBorrowInst>(v))
      return bbi->getOperand();
    if (auto *lbi = dyn_cast<LoadBorrowInst>(v))
      return lbi->getOperand();
    return SILValue();
  }

  /// Return the set of guaranteed values that have scopes ended by this
  /// end_borrow.
  ///
  /// Discussion: We can only have multiple values associated with an end_borrow
  /// in the case of having Phi arguments with guaranteed inputs. This is
  /// necessary to represent certain conditional operations such as:
  ///
  /// class Klass {
  ///   let k1: Klass
  ///   let k2: Klass
  /// }
  ///
  /// func useKlass(k: Klass) { ... }
  /// var boolValue : Bool { ... }
  ///
  /// func f(k: Klass) {
  ///   useKlass(boolValue ? k.k1 : k.k2)
  /// }
  ///
  /// Today, when we SILGen such code, we copy k.k1 and k.k2 before the Phi when
  /// it could potentially be avoided. So today this just appends
  /// getSingleOriginalValue() to originalValues.
  ///
  /// TODO: Once this changes, this code must be update.
  void getOriginalValues(SmallVectorImpl<SILValue> &originalValues) const {
    SILValue value = getSingleOriginalValue();
    assert(value && "Guaranteed phi arguments are not supported now");
    originalValues.emplace_back(value);
  }
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

  // This enum is encoded.
  Last = Unsafe
};
StringRef getSILAccessEnforcementName(SILAccessEnforcement enforcement);

class EndAccessInst;

/// Begins an access scope. Must be paired with an end_access instruction
/// along every path.
class BeginAccessInst
    : public UnaryInstructionBase<SILInstructionKind::BeginAccessInst,
                                  SingleValueInstruction> {
  friend class SILBuilder;

  BeginAccessInst(SILDebugLocation loc, SILValue lvalue,
                  SILAccessKind accessKind, SILAccessEnforcement enforcement,
                  bool noNestedConflict, bool fromBuiltin)
      : UnaryInstructionBase(loc, lvalue, lvalue->getType()) {
    SILNode::Bits.BeginAccessInst.AccessKind = unsigned(accessKind);
    SILNode::Bits.BeginAccessInst.Enforcement = unsigned(enforcement);
    SILNode::Bits.BeginAccessInst.NoNestedConflict =
      unsigned(noNestedConflict);
    SILNode::Bits.BeginAccessInst.FromBuiltin =
      unsigned(fromBuiltin);

    static_assert(unsigned(SILAccessKind::Last) < (1 << 2),
                  "reserve sufficient bits for serialized SIL");
    static_assert(unsigned(SILAccessEnforcement::Last) < (1 << 2),
                  "reserve sufficient bits for serialized SIL");

    static_assert(unsigned(SILAccessKind::Last) <
                  (1 << SILNode::NumSILAccessKindBits),
                  "SILNode needs updating");
    static_assert(unsigned(SILAccessEnforcement::Last) <
                  (1 << SILNode::NumSILAccessEnforcementBits),
                  "SILNode needs updating");
  }

public:
  SILAccessKind getAccessKind() const {
    return SILAccessKind(SILNode::Bits.BeginAccessInst.AccessKind);
  }
  void setAccessKind(SILAccessKind kind) {
    SILNode::Bits.BeginAccessInst.AccessKind = unsigned(kind);
  }

  SILAccessEnforcement getEnforcement() const {
    return
      SILAccessEnforcement(SILNode::Bits.BeginAccessInst.Enforcement);
  }
  void setEnforcement(SILAccessEnforcement enforcement) {
    SILNode::Bits.BeginAccessInst.Enforcement = unsigned(enforcement);
  }

  /// If hasNoNestedConflict is true, then it is a static guarantee against
  /// inner conflicts. No subsequent access between this point and the
  /// corresponding end_access could cause an enforcement failure. Consequently,
  /// the access will not need to be tracked by the runtime for the duration of
  /// its scope. This access may still conflict with an outer access scope;
  /// therefore may still require dynamic enforcement at a single point.
  bool hasNoNestedConflict() const {
    return SILNode::Bits.BeginAccessInst.NoNestedConflict;
  }
  void setNoNestedConflict(bool noNestedConflict) {
    SILNode::Bits.BeginAccessInst.NoNestedConflict = noNestedConflict;
  }

  /// Return true if this access marker was emitted for a user-controlled
  /// Builtin. Return false if this access marker was auto-generated by the
  /// compiler to enforce formal access that derives from the language.
  bool isFromBuiltin() const {
    return SILNode::Bits.BeginAccessInst.FromBuiltin;
  }
  
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

private:
  EndAccessInst(SILDebugLocation loc, SILValue access, bool aborting = false)
      : UnaryInstructionBase(loc, access) {
    SILNode::Bits.EndAccessInst.Aborting = aborting;
  }

public:
  /// An aborted access is one that did not perform the expected
  /// transition described by the begin_access instruction before it
  /// reached this end_access.
  ///
  /// Only AccessKind::Init and AccessKind::Deinit accesses can be
  /// aborted.
  bool isAborting() const {
    return SILNode::Bits.EndAccessInst.Aborting;
  }
  void setAborting(bool aborting = true) {
    SILNode::Bits.EndAccessInst.Aborting = aborting;
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
    : public InstructionBase<SILInstructionKind::BeginUnpairedAccessInst,
                             NonValueInstruction> {
  friend class SILBuilder;

  FixedOperandList<2> Operands;

  BeginUnpairedAccessInst(SILDebugLocation loc, SILValue addr, SILValue buffer,
                          SILAccessKind accessKind,
                          SILAccessEnforcement enforcement,
                          bool noNestedConflict,
                          bool fromBuiltin)
      : InstructionBase(loc), Operands(this, addr, buffer) {
    SILNode::Bits.BeginUnpairedAccessInst.AccessKind =
      unsigned(accessKind);
    SILNode::Bits.BeginUnpairedAccessInst.Enforcement =
      unsigned(enforcement);
    SILNode::Bits.BeginUnpairedAccessInst.NoNestedConflict =
      unsigned(noNestedConflict);
    SILNode::Bits.BeginUnpairedAccessInst.FromBuiltin =
      unsigned(fromBuiltin);
  }

public:
  SILAccessKind getAccessKind() const {
    return SILAccessKind(
      SILNode::Bits.BeginUnpairedAccessInst.AccessKind);
  }
  void setAccessKind(SILAccessKind kind) {
    SILNode::Bits.BeginUnpairedAccessInst.AccessKind = unsigned(kind);
  }

  SILAccessEnforcement getEnforcement() const {
    return SILAccessEnforcement(
        SILNode::Bits.BeginUnpairedAccessInst.Enforcement);
  }
  void setEnforcement(SILAccessEnforcement enforcement) {
    SILNode::Bits.BeginUnpairedAccessInst.Enforcement
      = unsigned(enforcement);
  }

  /// If hasNoNestedConflict is true, then it is a static guarantee against
  /// inner conflicts. No subsequent access between this point and the
  /// corresponding end_access could cause an enforcement failure. Consequently,
  /// the access will not need to be tracked by the runtime for the duration of
  /// its scope. This access may still conflict with an outer access scope;
  /// therefore may still require dynamic enforcement at a single point.
  bool hasNoNestedConflict() const {
    return SILNode::Bits.BeginUnpairedAccessInst.NoNestedConflict;
  }
  void setNoNestedConflict(bool noNestedConflict) {
    SILNode::Bits.BeginUnpairedAccessInst.NoNestedConflict =
      noNestedConflict;
  }

  /// Return true if this access marker was emitted for a user-controlled
  /// Builtin. Return false if this access marker was auto-generated by the
  /// compiler to enforce formal access that derives from the language.
  bool isFromBuiltin() const {
    return SILNode::Bits.BeginUnpairedAccessInst.FromBuiltin;
  }

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

private:
  EndUnpairedAccessInst(SILDebugLocation loc, SILValue buffer,
                        SILAccessEnforcement enforcement, bool aborting,
                        bool fromBuiltin)
      : UnaryInstructionBase(loc, buffer) {
    SILNode::Bits.EndUnpairedAccessInst.Enforcement
      = unsigned(enforcement);
    SILNode::Bits.EndUnpairedAccessInst.Aborting = aborting;
    SILNode::Bits.EndUnpairedAccessInst.FromBuiltin = fromBuiltin;
  }

public:
  /// An aborted access is one that did not perform the expected
  /// transition described by the begin_access instruction before it
  /// reached this end_access.
  ///
  /// Only AccessKind::Init and AccessKind::Deinit accesses can be
  /// aborted.
  bool isAborting() const {
    return SILNode::Bits.EndUnpairedAccessInst.Aborting;
  }
  void setAborting(bool aborting) {
    SILNode::Bits.EndUnpairedAccessInst.Aborting = aborting;
  }

  SILAccessEnforcement getEnforcement() const {
    return SILAccessEnforcement(
        SILNode::Bits.EndUnpairedAccessInst.Enforcement);
  }
  void setEnforcement(SILAccessEnforcement enforcement) {
    SILNode::Bits.EndUnpairedAccessInst.Enforcement =
        unsigned(enforcement);
  }

  /// Return true if this access marker was emitted for a user-controlled
  /// Builtin. Return false if this access marker was auto-generated by the
  /// compiler to enforce formal access that derives from the language.
  bool isFromBuiltin() const {
    return SILNode::Bits.EndUnpairedAccessInst.FromBuiltin;
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

  AssignInst(SILDebugLocation DebugLoc, SILValue Src, SILValue Dest,
             AssignOwnershipQualifier Qualifier =
             AssignOwnershipQualifier::Unknown);

public:
  AssignOwnershipQualifier getOwnershipQualifier() const {
    return AssignOwnershipQualifier(
      SILNode::Bits.AssignInst.OwnershipQualifier);
  }
  void setOwnershipQualifier(AssignOwnershipQualifier qualifier) {
    SILNode::Bits.AssignInst.OwnershipQualifier = unsigned(qualifier);
  }
};

/// AssignByWrapperInst - Represents an abstract assignment via a wrapper,
/// which may either be an initialization or a store sequence.  This is only
/// valid in Raw SIL.
class AssignByWrapperInst
    : public AssignInstBase<SILInstructionKind::AssignByWrapperInst, 4> {
  friend SILBuilder;

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
  AssignByWrapperInst(SILDebugLocation DebugLoc, SILValue Src, SILValue Dest,
                       SILValue Initializer, SILValue Setter, Mode mode);

public:
  SILValue getInitializer() { return Operands[2].get(); }
  SILValue getSetter() { return  Operands[3].get(); }

  Mode getMode() const {
    return Mode(SILNode::Bits.AssignByWrapperInst.Mode);
  }

  void setMode(Mode mode) {
    SILNode::Bits.AssignByWrapperInst.Mode = unsigned(mode);
  }
};

/// Indicates that a memory location is uninitialized at this point and needs to
/// be initialized by the end of the function and before any escape point for
/// this instruction. This is only valid in Raw SIL.
class MarkUninitializedInst
    : public UnaryInstructionBase<SILInstructionKind::MarkUninitializedInst,
                                  OwnedFirstArgForwardingSingleValueInst> {
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

/// Define the start or update to a symbolic variable value (for loadable
/// types).
class DebugValueInst final
    : public UnaryInstructionBase<SILInstructionKind::DebugValueInst,
                                  NonValueInstruction>,
      private llvm::TrailingObjects<DebugValueInst, char> {
  friend TrailingObjects;
  friend SILBuilder;
  TailAllocatedDebugVariable VarInfo;

  DebugValueInst(SILDebugLocation DebugLoc, SILValue Operand,
                 SILDebugVariable Var, bool poisonRefs);
  static DebugValueInst *create(SILDebugLocation DebugLoc, SILValue Operand,
                                SILModule &M, SILDebugVariable Var,
                                bool poisonRefs);

  size_t numTrailingObjects(OverloadToken<char>) const { return 1; }

public:
  /// Return the underlying variable declaration that this denotes,
  /// or null if we don't have one.
  VarDecl *getDecl() const;
  /// Return the debug variable information attached to this instruction.
  Optional<SILDebugVariable> getVarInfo() const {
    return VarInfo.get(getDecl(), getTrailingObjects<char>());
  }

  /// True if all references within this debug value will be overwritten with a
  /// poison sentinel at this point in the program. This is used in debug builds
  /// when shortening non-trivial value lifetimes to ensure the debugger cannot
  /// inspect invalid memory. These are not generated until OSSA is
  /// lowered. They are not expected to be serialized within the module, and the
  /// debug pipeline is not expected to do any significant code motion after
  /// OSSA lowering. It should not be necessary to model the poison operation as
  /// a side effect, which would violate the rule that debug_values cannot
  /// affect optimization.
  bool poisonRefs() const { return SILNode::Bits.DebugValueInst.PoisonRefs; }

  void setPoisonRefs(bool poisonRefs = true) {
    SILNode::Bits.DebugValueInst.PoisonRefs = poisonRefs;
  }
};

/// Define the start or update to a symbolic variable value (for address-only
/// types) .
class DebugValueAddrInst final
  : public UnaryInstructionBase<SILInstructionKind::DebugValueAddrInst,
                                NonValueInstruction>,
    private llvm::TrailingObjects<DebugValueAddrInst, char> {
  friend TrailingObjects;
  friend SILBuilder;
  TailAllocatedDebugVariable VarInfo;

  DebugValueAddrInst(SILDebugLocation DebugLoc, SILValue Operand,
                     SILDebugVariable Var);
  static DebugValueAddrInst *create(SILDebugLocation DebugLoc,
                                    SILValue Operand, SILModule &M,
                                    SILDebugVariable Var);

public:
  /// Return the underlying variable declaration that this denotes,
  /// or null if we don't have one.
  VarDecl *getDecl() const;
  /// Return the debug variable information attached to this instruction.
  Optional<SILDebugVariable> getVarInfo() const {
    return VarInfo.get(getDecl(), getTrailingObjects<char>());
  };
};


/// An abstract class representing a load from some kind of reference storage.
template <SILInstructionKind K>
class LoadReferenceInstBase
    : public UnaryInstructionBase<K, SingleValueInstruction> {
  static SILType getResultType(SILType operandTy) {
    assert(operandTy.isAddress() && "loading from non-address operand?");
    auto refType = cast<ReferenceStorageType>(operandTy.getASTType());
    return SILType::getPrimitiveObjectType(refType.getReferentType());
  }

protected:
  LoadReferenceInstBase(SILDebugLocation loc, SILValue lvalue, IsTake_t isTake)
    : UnaryInstructionBase<K, SingleValueInstruction>(loc, lvalue,
                                             getResultType(lvalue->getType())) {
    SILNode::Bits.LoadReferenceInstBaseT.IsTake = unsigned(isTake);
  }

public:
  IsTake_t isTake() const {
    return IsTake_t(SILNode::Bits.LoadReferenceInstBaseT.IsTake);
  }
};

/// An abstract class representing a store to some kind of reference storage.
template <SILInstructionKind K>
class StoreReferenceInstBase : public InstructionBase<K, NonValueInstruction> {
  enum { Src, Dest };
  FixedOperandList<2> Operands;
protected:
  StoreReferenceInstBase(SILDebugLocation loc, SILValue src, SILValue dest,
                         IsInitialization_t isInit)
    : InstructionBase<K, NonValueInstruction>(loc),
      Operands(this, src, dest) {
    SILNode::Bits.StoreReferenceInstBaseT.IsInitializationOfDest =
      unsigned(isInit);
  }

public:
  SILValue getSrc() const { return Operands[Src].get(); }
  SILValue getDest() const { return Operands[Dest].get(); }

  IsInitialization_t isInitializationOfDest() const {
    return IsInitialization_t(
      SILNode::Bits.StoreReferenceInstBaseT.IsInitializationOfDest);
  }
  void setIsInitializationOfDest(IsInitialization_t I) {
    SILNode::Bits.StoreReferenceInstBaseT.IsInitializationOfDest =
      (bool)I;
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

  CopyAddrInst(SILDebugLocation DebugLoc, SILValue Src, SILValue Dest,
               IsTake_t isTakeOfSrc, IsInitialization_t isInitializationOfDest);

public:
  SILValue getSrc() const { return Operands[Src].get(); }
  SILValue getDest() const { return Operands[Dest].get(); }

  void setSrc(SILValue V) { Operands[Src].set(V); }
  void setDest(SILValue V) { Operands[Dest].set(V); }

  IsTake_t isTakeOfSrc() const {
    return IsTake_t(SILNode::Bits.CopyAddrInst.IsTakeOfSrc);
  }
  IsInitialization_t isInitializationOfDest() const {
    return IsInitialization_t(
      SILNode::Bits.CopyAddrInst.IsInitializationOfDest);
  }

  void setIsTakeOfSrc(IsTake_t T) {
    SILNode::Bits.CopyAddrInst.IsTakeOfSrc = (bool)T;
  }
  void setIsInitializationOfDest(IsInitialization_t I) {
    SILNode::Bits.CopyAddrInst.IsInitializationOfDest = (bool)I;
  }

  ArrayRef<Operand> getAllOperands() const { return Operands.asArray(); }
  MutableArrayRef<Operand> getAllOperands() { return Operands.asArray(); }
};

/// BindMemoryInst -
/// "bind_memory %0 : $Builtin.RawPointer, %1 : $Builtin.Word to $T"
/// Binds memory at the raw pointer %0 to type $T with enough capacity
/// to hold $1 values.
class BindMemoryInst final :
    public InstructionBaseWithTrailingOperands<
                                          SILInstructionKind::BindMemoryInst,
                                          BindMemoryInst, NonValueInstruction> {
  friend SILBuilder;

  SILType BoundType;

  static BindMemoryInst *create(
    SILDebugLocation Loc, SILValue Base, SILValue Index, SILType BoundType,
    SILFunction &F);

  BindMemoryInst(SILDebugLocation Loc, SILValue Base, SILValue Index,
                 SILType BoundType,
                 ArrayRef<SILValue> TypeDependentOperands)
    : InstructionBaseWithTrailingOperands(Base, Index, TypeDependentOperands,
                                          Loc), BoundType(BoundType) {}

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

//===----------------------------------------------------------------------===//
// Conversion instructions.
//===----------------------------------------------------------------------===//

/// ConversionInst - Abstract class representing instructions that convert
/// values.
///
class ConversionInst : public SingleValueInstruction {
protected:
  ConversionInst(SILInstructionKind Kind, SILDebugLocation DebugLoc, SILType Ty)
      : SingleValueInstruction(Kind, DebugLoc, Ty) {}

public:
  /// All conversion instructions take the converted value, whose reference
  /// identity is expected to be preserved through the conversion chain, as their
  /// first operand. Some instructions may take additional operands that do not
  /// affect the reference identity.
  SILValue getConverted() const { return getOperand(0); }
  
  DEFINE_ABSTRACT_SINGLE_VALUE_INST_BOILERPLATE(ConversionInst)
};

/// A conversion inst that produces a static OwnershipKind set upon the
/// instruction's construction.
class OwnershipForwardingConversionInst : public ConversionInst,
                                          public OwnershipForwardingMixin {
protected:
  OwnershipForwardingConversionInst(SILInstructionKind kind,
                                    SILDebugLocation debugLoc, SILType ty,
                                    ValueOwnershipKind ownershipKind)
      : ConversionInst(kind, debugLoc, ty),
        OwnershipForwardingMixin(kind, ownershipKind) {
    assert(classof(kind) && "classof missing subclass?!");
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
    switch (kind) {
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
};

/// ConvertFunctionInst - Change the type of a function value without
/// affecting how it will codegen.
class ConvertFunctionInst final
    : public UnaryInstructionWithTypeDependentOperandsBase<
          SILInstructionKind::ConvertFunctionInst, ConvertFunctionInst,
          OwnershipForwardingConversionInst> {
  friend SILBuilder;

  ConvertFunctionInst(SILDebugLocation DebugLoc, SILValue Operand,
                      ArrayRef<SILValue> TypeDependentOperands, SILType Ty,
                      bool WithoutActuallyEscaping,
                      ValueOwnershipKind forwardingOwnershipKind)
      : UnaryInstructionWithTypeDependentOperandsBase(DebugLoc, Operand,
                                                      TypeDependentOperands, Ty,
                                                      forwardingOwnershipKind) {
    SILNode::Bits.ConvertFunctionInst.WithoutActuallyEscaping =
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
    return SILNode::Bits.ConvertFunctionInst.WithoutActuallyEscaping;
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
          ConvertEscapeToNoEscapeInst, ConversionInst> {
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

/// ThinFunctionToPointerInst - Convert a thin function pointer to a
/// Builtin.RawPointer.
class ThinFunctionToPointerInst
  : public UnaryInstructionBase<SILInstructionKind::ThinFunctionToPointerInst,
                                ConversionInst>
{
  friend SILBuilder;

  ThinFunctionToPointerInst(SILDebugLocation DebugLoc, SILValue operand,
                            SILType ty)
      : UnaryInstructionBase(DebugLoc, operand, ty) {}
};

/// PointerToThinFunctionInst - Convert a Builtin.RawPointer to a thin
/// function pointer.
class PointerToThinFunctionInst final
    : public UnaryInstructionWithTypeDependentOperandsBase<
          SILInstructionKind::PointerToThinFunctionInst,
          PointerToThinFunctionInst,
          ConversionInst> {
  friend SILBuilder;

  PointerToThinFunctionInst(SILDebugLocation DebugLoc, SILValue operand,
                            ArrayRef<SILValue> TypeDependentOperands,
                            SILType ty)
      : UnaryInstructionWithTypeDependentOperandsBase(
            DebugLoc, operand, TypeDependentOperands, ty) {}

  static PointerToThinFunctionInst *
  create(SILDebugLocation DebugLoc, SILValue Operand, SILType Ty,
         SILFunction &F);
};

/// UpcastInst - Perform a conversion of a class instance to a supertype.
class UpcastInst final : public UnaryInstructionWithTypeDependentOperandsBase<
                             SILInstructionKind::UpcastInst, UpcastInst,
                             OwnershipForwardingConversionInst> {
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
                                ConversionInst>
{
  friend SILBuilder;

  AddressToPointerInst(SILDebugLocation DebugLoc, SILValue Operand, SILType Ty)
      : UnaryInstructionBase(DebugLoc, Operand, Ty) {}
};

/// PointerToAddressInst - Convert a Builtin.RawPointer value to a SIL address.
class PointerToAddressInst
  : public UnaryInstructionBase<SILInstructionKind::PointerToAddressInst,
                                ConversionInst>
{
  friend SILBuilder;

  PointerToAddressInst(SILDebugLocation DebugLoc, SILValue Operand, SILType Ty,
                       bool IsStrict, bool IsInvariant)
    : UnaryInstructionBase(DebugLoc, Operand, Ty) {
    SILNode::Bits.PointerToAddressInst.IsStrict = IsStrict;
    SILNode::Bits.PointerToAddressInst.IsInvariant = IsInvariant;
  }

public:
  /// Whether the returned address adheres to strict aliasing.
  /// If true, then the type of each memory access dependent on
  /// this address must be consistent with the memory's bound type.
  bool isStrict() const {
    return SILNode::Bits.PointerToAddressInst.IsStrict;
  }
  /// Whether the returned address is invariant.
  /// If true, then loading from an address derived from this pointer always
  /// produces the same value.
  bool isInvariant() const {
    return SILNode::Bits.PointerToAddressInst.IsInvariant;
  }
};

/// Convert a heap object reference to a different type without any runtime
/// checks.
class UncheckedRefCastInst final
    : public UnaryInstructionWithTypeDependentOperandsBase<
          SILInstructionKind::UncheckedRefCastInst, UncheckedRefCastInst,
          OwnershipForwardingConversionInst> {
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
                                UncheckedTrivialBitCastInst,
                                ConversionInst>
{
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
                                UncheckedBitwiseCastInst,
                                ConversionInst>
{
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
          OwnershipForwardingConversionInst> {
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
                             OwnershipForwardingConversionInst> {
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
                                  OwnershipForwardingConversionInst> {
  friend SILBuilder;

  BridgeObjectToRefInst(SILDebugLocation DebugLoc, SILValue Operand, SILType Ty,
                        ValueOwnershipKind forwardingOwnershipKind)
      : UnaryInstructionBase(DebugLoc, Operand, Ty, forwardingOwnershipKind) {}
};

/// Sets the BridgeObject to a tagged pointer representation holding its
/// operands
class ValueToBridgeObjectInst
    : public UnaryInstructionBase<SILInstructionKind::ValueToBridgeObjectInst,
                                  ConversionInst> {
  friend SILBuilder;

  ValueToBridgeObjectInst(SILDebugLocation DebugLoc, SILValue Operand,
                          SILType BridgeObjectTy)
      : UnaryInstructionBase(DebugLoc, Operand, BridgeObjectTy) {}
};

/// Retrieve the bit pattern of a BridgeObject.
class BridgeObjectToWordInst
  : public UnaryInstructionBase<SILInstructionKind::BridgeObjectToWordInst,
                                ConversionInst>
{
  friend SILBuilder;

  BridgeObjectToWordInst(SILDebugLocation DebugLoc, SILValue Operand,
                         SILType Ty)
      : UnaryInstructionBase(DebugLoc, Operand, Ty) {}
};

/// RefToRawPointer - Convert a reference type to a Builtin.RawPointer.
class RefToRawPointerInst
  : public UnaryInstructionBase<SILInstructionKind::RefToRawPointerInst,
                                ConversionInst>
{
  friend SILBuilder;

  RefToRawPointerInst(SILDebugLocation DebugLoc, SILValue Operand, SILType Ty)
      : UnaryInstructionBase(DebugLoc, Operand, Ty) {}
};

/// RawPointerToRefInst - Convert a Builtin.RawPointer to a reference type.
class RawPointerToRefInst
  : public UnaryInstructionBase<SILInstructionKind::RawPointerToRefInst,
                                ConversionInst>
{
  friend SILBuilder;

  RawPointerToRefInst(SILDebugLocation DebugLoc, SILValue Operand, SILType Ty)
      : UnaryInstructionBase(DebugLoc, Operand, Ty) {}
};

/// Transparent reference storage to underlying reference type conversion.
/// This does nothing at runtime; it just changes the formal type.
#define LOADABLE_REF_STORAGE(Name, ...) \
class RefTo##Name##Inst \
    : public UnaryInstructionBase<SILInstructionKind::RefTo##Name##Inst, \
                                  ConversionInst> { \
  friend SILBuilder; \
  RefTo##Name##Inst(SILDebugLocation DebugLoc, SILValue Operand, SILType Ty) \
      : UnaryInstructionBase(DebugLoc, Operand, Ty) {} \
}; \
class Name##ToRefInst \
  : public UnaryInstructionBase<SILInstructionKind::Name##ToRefInst, \
                                ConversionInst> { \
  friend SILBuilder; \
  Name##ToRefInst(SILDebugLocation DebugLoc, SILValue Operand, SILType Ty) \
      : UnaryInstructionBase(DebugLoc, Operand, Ty) {} \
};
#include "swift/AST/ReferenceStorage.def"

/// ThinToThickFunctionInst - Given a thin function reference, adds a null
/// context to convert the value to a thick function type.
class ThinToThickFunctionInst final
    : public UnaryInstructionWithTypeDependentOperandsBase<
          SILInstructionKind::ThinToThickFunctionInst, ThinToThickFunctionInst,
          OwnershipForwardingConversionInst> {
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
                                ConversionInst>
{
  friend SILBuilder;

  ThickToObjCMetatypeInst(SILDebugLocation DebugLoc, SILValue Operand,
                          SILType Ty)
      : UnaryInstructionBase(DebugLoc, Operand, Ty) {}
};

/// Given an Objective-C metatype value, produces a thick metatype
/// value.
class ObjCToThickMetatypeInst
  : public UnaryInstructionBase<SILInstructionKind::ObjCToThickMetatypeInst,
                                ConversionInst>
{
  friend SILBuilder;

  ObjCToThickMetatypeInst(SILDebugLocation DebugLoc, SILValue Operand,
                          SILType Ty)
      : UnaryInstructionBase(DebugLoc, Operand, Ty) {}
};

/// Given an Objective-C metatype value, convert it to an AnyObject value.
class ObjCMetatypeToObjectInst
  : public UnaryInstructionBase<SILInstructionKind::ObjCMetatypeToObjectInst,
                                ConversionInst>
{
  friend SILBuilder;

  ObjCMetatypeToObjectInst(SILDebugLocation DebugLoc, SILValue Operand,
                           SILType Ty)
      : UnaryInstructionBase(DebugLoc, Operand, Ty) {}
};

/// Given an Objective-C existential metatype value, convert it to an AnyObject
/// value.
class ObjCExistentialMetatypeToObjectInst
  : public UnaryInstructionBase<SILInstructionKind::ObjCExistentialMetatypeToObjectInst,
                                ConversionInst>
{
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
          UnconditionalCheckedCastInst, OwnershipForwardingConversionInst> {
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

/// Perform an unconditional checked cast that aborts if the cast fails.
/// The result of the checked cast is left in the destination.
class UnconditionalCheckedCastValueInst final
    : public UnaryInstructionWithTypeDependentOperandsBase<
          SILInstructionKind::UnconditionalCheckedCastValueInst,
          UnconditionalCheckedCastValueInst, ConversionInst> {
  CanType SourceFormalTy;
  CanType DestFormalTy;
  friend SILBuilder;

  UnconditionalCheckedCastValueInst(SILDebugLocation DebugLoc,
                                    SILValue Operand, CanType SourceFormalTy,
                                    ArrayRef<SILValue> TypeDependentOperands,
                                    SILType DestLoweredTy, CanType DestFormalTy)
      : UnaryInstructionWithTypeDependentOperandsBase(
            DebugLoc, Operand, TypeDependentOperands,
            DestLoweredTy),
        SourceFormalTy(SourceFormalTy),
        DestFormalTy(DestFormalTy) {}

  static UnconditionalCheckedCastValueInst *
  create(SILDebugLocation DebugLoc,
         SILValue Operand, CanType SourceFormalTy,
         SILType DestLoweredTy, CanType DestFormalTy, SILFunction &F);

public:
  SILType getSourceLoweredType() const { return getOperand()->getType(); }
  CanType getSourceFormalType() const { return SourceFormalTy; }

  SILType getTargetLoweredType() const { return getType(); }
  CanType getTargetFormalType() const { return DestFormalTy; }
};

/// StructInst - Represents a constructed loadable struct.
class StructInst final : public InstructionBaseWithTrailingOperands<
                             SILInstructionKind::StructInst, StructInst,
                             AllArgOwnershipForwardingSingleValueInst> {
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
        if (!Index.hasValue()) {
          Index = i;
          continue;
        }

        // Otherwise, we have two values that are non-trivial. Bail.
        return SILValue();
      }
    }

    // If we did not find an index, return an empty SILValue.
    if (!Index.hasValue())
      return SILValue();

    // Otherwise, return the value associated with index.
    return Ops[Index.getValue()].get();
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
    SILNode::Bits.RefCountingInst.atomicity = bool(Atomicity::Atomic);
  }

public:
  void setAtomicity(Atomicity flag) {
    SILNode::Bits.RefCountingInst.atomicity = bool(flag);
  }
  void setNonAtomic() {
    SILNode::Bits.RefCountingInst.atomicity = bool(Atomicity::NonAtomic);
  }
  void setAtomic() {
    SILNode::Bits.RefCountingInst.atomicity = bool(Atomicity::Atomic);
  }
  Atomicity getAtomicity() const {
    return Atomicity(SILNode::Bits.RefCountingInst.atomicity);
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
/// This instruction can only appear at the end of a gobal variable's
/// static initializer list.
class ObjectInst final : public InstructionBaseWithTrailingOperands<
                             SILInstructionKind::ObjectInst, ObjectInst,
                             FirstArgOwnershipForwardingSingleValueInst> {
  friend SILBuilder;

  /// Because of the storage requirements of ObjectInst, object
  /// creation goes through 'create()'.
  ObjectInst(SILDebugLocation DebugLoc, SILType Ty, ArrayRef<SILValue> Elements,
             unsigned NumBaseElements,
             ValueOwnershipKind forwardingOwnershipKind)
      : InstructionBaseWithTrailingOperands(Elements, DebugLoc, Ty,
                                            forwardingOwnershipKind) {
    SILNode::Bits.ObjectInst.NumBaseElements = NumBaseElements;
  }

  /// Construct an ObjectInst.
  static ObjectInst *create(SILDebugLocation DebugLoc, SILType Ty,
                            ArrayRef<SILValue> Elements,
                            unsigned NumBaseElements, SILModule &M,
                            ValueOwnershipKind forwardingOwnershipKind);

public:
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
    return OperandValueArrayRef(getAllOperands().slice(0,
                              SILNode::Bits.ObjectInst.NumBaseElements));
  }

  /// The elements which initialize the tail allocated elements.
  OperandValueArrayRef getTailElements() const {
    return OperandValueArrayRef(getAllOperands().slice(
                              SILNode::Bits.ObjectInst.NumBaseElements));
  }
};

/// TupleInst - Represents a constructed loadable tuple.
class TupleInst final : public InstructionBaseWithTrailingOperands<
                            SILInstructionKind::TupleInst, TupleInst,
                            AllArgOwnershipForwardingSingleValueInst> {
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
        if (!Index.hasValue()) {
          Index = i;
          continue;
        }

        // Otherwise, we have two values that are non-trivial. Bail.
        return SILValue();
      }
    }

    // If we did not find an index, return an empty SILValue.
    if (!Index.hasValue())
      return SILValue();

    // Otherwise, return the value associated with index.
    return Ops[Index.getValue()].get();
  }
};

/// Represents a loadable enum constructed from one of its
/// elements.
class EnumInst
    : public InstructionBase<SILInstructionKind::EnumInst,
                             FirstArgOwnershipForwardingSingleValueInst> {
  friend SILBuilder;

  Optional<FixedOperandList<1>> OptionalOperand;
  EnumElementDecl *Element;

  EnumInst(SILDebugLocation DebugLoc, SILValue Operand,
           EnumElementDecl *Element, SILType ResultTy,
           ValueOwnershipKind forwardingOwnershipKind)
      : InstructionBase(DebugLoc, ResultTy, forwardingOwnershipKind),
        Element(Element) {
    if (Operand) {
      OptionalOperand.emplace(this, Operand);
    }
  }

public:
  EnumElementDecl *getElement() const { return Element; }

  bool hasOperand() const { return OptionalOperand.hasValue(); }
  SILValue getOperand() const { return OptionalOperand->asValueArray()[0]; }

  Operand &getOperandRef() { return OptionalOperand->asArray()[0]; }

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
                                  FirstArgOwnershipForwardingSingleValueInst> {
  friend SILBuilder;

  EnumElementDecl *Element;

  UncheckedEnumDataInst(SILDebugLocation DebugLoc, SILValue Operand,
                        EnumElementDecl *Element, SILType ResultTy,
                        ValueOwnershipKind forwardingOwnershipKind)
      : UnaryInstructionBase(DebugLoc, Operand, ResultTy,
                             forwardingOwnershipKind),
        Element(Element) {}

public:
  EnumElementDecl *getElement() const { return Element; }

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

  EnumElementDecl *Element;

  InitEnumDataAddrInst(SILDebugLocation DebugLoc, SILValue Operand,
                       EnumElementDecl *Element, SILType ResultTy)
      : UnaryInstructionBase(DebugLoc, Operand, ResultTy), Element(Element) {}

public:
  EnumElementDecl *getElement() const { return Element; }
};

/// InjectEnumAddrInst - Tags an enum as containing a case. The data for
/// that case, if any, must have been written into the enum first.
class InjectEnumAddrInst
  : public UnaryInstructionBase<SILInstructionKind::InjectEnumAddrInst,
                                NonValueInstruction>
{
  friend SILBuilder;

  EnumElementDecl *Element;

  InjectEnumAddrInst(SILDebugLocation DebugLoc, SILValue Operand,
                     EnumElementDecl *Element)
      : UnaryInstructionBase(DebugLoc, Operand), Element(Element) {}

public:
  EnumElementDecl *getElement() const { return Element; }
};

/// Invalidate an enum value and take ownership of its payload data
/// without moving it in memory.
class UncheckedTakeEnumDataAddrInst
  : public UnaryInstructionBase<SILInstructionKind::UncheckedTakeEnumDataAddrInst,
                                SingleValueInstruction>
{
  friend SILBuilder;

  EnumElementDecl *Element;

  UncheckedTakeEnumDataAddrInst(SILDebugLocation DebugLoc, SILValue Operand,
                                EnumElementDecl *Element, SILType ResultTy)
      : UnaryInstructionBase(DebugLoc, Operand, ResultTy), Element(Element) {}

public:
  EnumElementDecl *getElement() const { return Element; }

  EnumDecl *getEnumDecl() const {
    auto *E = getOperand()->getType().getEnumOrBoundGenericEnum();
    assert(E && "Operand of unchecked_take_enum_data_addr must be of enum"
                " type");
    return E;
  }

  unsigned getElementNo() const {
    unsigned i = 0;
    for (EnumElementDecl *E : getEnumDecl()->getAllElements()) {
      if (E == Element)
        return i;
      ++i;
    }
    llvm_unreachable(
        "An unchecked_enum_data_addr's enumdecl should have at least "
        "on element, the element that is being extracted");
  }
};

// Abstract base class of all select instructions like select_enum,
// select_value, etc. The template parameter represents a type of case values
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
    SILNode::Bits.SelectEnumInstBase.HasDefault = defaultValue;
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
    return SILNode::Bits.SelectEnumInstBase.HasDefault;
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
                                              public OwnershipForwardingMixin {
protected:
  OwnershipForwardingSelectEnumInstBase(
      SILInstructionKind kind, SILDebugLocation debugLoc, SILType type,
      bool defaultValue, Optional<ArrayRef<ProfileCounter>> caseCounts,
      ProfileCounter defaultCount, ValueOwnershipKind ownershipKind)
      : SelectEnumInstBase(kind, debugLoc, type, defaultValue, caseCounts,
                           defaultCount),
        OwnershipForwardingMixin(kind, ownershipKind) {
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

/// Select on a value of a builtin integer type.
///
/// There is 'the' operand, followed by pairs of operands for each case,
/// followed by an optional default operand.
class SelectValueInst final
    : public InstructionBaseWithTrailingOperands<
          SILInstructionKind::SelectValueInst, SelectValueInst,
          SelectInstBase<SelectValueInst, SILValue,
                         FirstArgOwnershipForwardingSingleValueInst>> {
  friend SILBuilder;

  SelectValueInst(SILDebugLocation DebugLoc, SILValue Operand, SILType Type,
                  SILValue DefaultResult,
                  ArrayRef<SILValue> CaseValuesAndResults,
                  ValueOwnershipKind forwardingOwnershipKind);

  static SelectValueInst *
  create(SILDebugLocation DebugLoc, SILValue Operand, SILType Type,
         SILValue DefaultValue,
         ArrayRef<std::pair<SILValue, SILValue>> CaseValues, SILModule &M,
         ValueOwnershipKind forwardingOwnershipKind);

public:
  std::pair<SILValue, SILValue>
  getCase(unsigned i) const {
    auto cases = getAllOperands().slice(1);
    return {cases[i*2].get(), cases[i*2+1].get()};
  }

  unsigned getNumCases() const {
    // Ignore the first non-case operand.
    auto count = getAllOperands().size() - 1;
    // This implicitly ignore the optional default operand.
    return count / 2;
  }

  bool hasDefault() const {
    // If the operand count is even, then we have a default value.
    return (getAllOperands().size() & 1) == 0;
  }

  SILValue getDefaultResult() const {
    assert(hasDefault() && "doesn't have a default");
    return getAllOperands().back().get();
  }
};

/// MetatypeInst - Represents the production of an instance of a given metatype
/// named statically.
class MetatypeInst final
    : public InstructionBaseWithTrailingOperands<
                                         SILInstructionKind::MetatypeInst,
                                         MetatypeInst, SingleValueInstruction> {
  friend SILBuilder;

  /// Constructs a MetatypeInst
  MetatypeInst(SILDebugLocation DebugLoc, SILType Metatype,
               ArrayRef<SILValue> TypeDependentOperands)
    : InstructionBaseWithTrailingOperands(TypeDependentOperands, DebugLoc,
                                          Metatype) {}

  static MetatypeInst *create(SILDebugLocation DebugLoc, SILType Metatype,
                              SILFunction *F);

public:
  ArrayRef<Operand> getTypeDependentOperands() const {
    return getAllOperands();
  }

  MutableArrayRef<Operand> getTypeDependentOperands() {
    return getAllOperands();
  }
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
                                  GuaranteedFirstArgForwardingSingleValueInst> {
  friend SILBuilder;

  TupleExtractInst(SILDebugLocation DebugLoc, SILValue Operand,
                   unsigned FieldNo, SILType ResultTy,
                   ValueOwnershipKind forwardingOwnershipKind)
      : UnaryInstructionBase(DebugLoc, Operand, ResultTy,
                             forwardingOwnershipKind) {
    SILNode::Bits.TupleExtractInst.FieldNo = FieldNo;
  }

public:
  unsigned getFieldIndex() const {
    return SILNode::Bits.TupleExtractInst.FieldNo;
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

  TupleElementAddrInst(SILDebugLocation DebugLoc, SILValue Operand,
                       unsigned FieldNo, SILType ResultTy)
      : UnaryInstructionBase(DebugLoc, Operand, ResultTy) {
    SILNode::Bits.TupleElementAddrInst.FieldNo = FieldNo;
  }

public:
  unsigned getFieldIndex() const {
    return SILNode::Bits.TupleElementAddrInst.FieldNo;
  }


  TupleType *getTupleType() const {
    return getOperand()->getType().castTo<TupleType>();
  }
};

/// Get a unique index for a struct or class field in layout order.
///
/// Precondition: \p decl must be a non-resilient struct or class.
///
/// Precondition: \p field must be a stored property declared in \p decl,
///               not in a superclass.
///
/// Postcondition: The returned index is unique across all properties in the
///                object, including properties declared in a superclass.
unsigned getFieldIndex(NominalTypeDecl *decl, VarDecl *property);

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

public:
  template <typename... ArgTys>
  FieldIndexCacheBase(SILInstructionKind kind, SILDebugLocation loc,
                      SILType type, VarDecl *field, ArgTys &&... extraArgs)
      : ParentTy(kind, loc, type, std::forward<ArgTys>(extraArgs)...),
        field(field) {
    SILNode::Bits.FieldIndexCacheBase.FieldIndex = InvalidFieldIndex;
    // This needs to be a concrete class to hold bitfield information. However,
    // it should only be extended by UnaryInstructions.
    assert(ParentTy::getNumOperands() == 1);
  }

  VarDecl *getField() const { return field; }

  unsigned getFieldIndex() const {
    unsigned idx = SILNode::Bits.FieldIndexCacheBase.FieldIndex;
    if (idx != InvalidFieldIndex)
      return idx;

    return const_cast<FieldIndexCacheBase *>(this)->cacheFieldIndex();
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

private:
  unsigned cacheFieldIndex() {
    unsigned index = swift::getFieldIndex(getParentDecl(), getField());
    SILNode::Bits.FieldIndexCacheBase.FieldIndex = index;
    return index;
  }
};

/// Extract a physical, fragile field out of a value of struct type.
class StructExtractInst
    : public UnaryInstructionBase<
          SILInstructionKind::StructExtractInst,
          FieldIndexCacheBase<GuaranteedFirstArgForwardingSingleValueInst>> {
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
    return SILNode::Bits.RefElementAddrInst.Immutable;
  }

  /// Sets the immutable flag.
  void setImmutable(bool immutable = true) {
    SILNode::Bits.RefElementAddrInst.Immutable = immutable;
  }
};

/// RefTailAddrInst - Derive the address of the first element of the first
/// tail-allocated array in a reference type instance.
class RefTailAddrInst
  : public UnaryInstructionBase<SILInstructionKind::RefTailAddrInst,
                                SingleValueInstruction>
{
  friend SILBuilder;

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
    return SILNode::Bits.RefTailAddrInst.Immutable;
  }

  /// Sets the immutable flag.
  void setImmutable(bool immutable = true) {
    SILNode::Bits.RefTailAddrInst.Immutable = immutable;
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
    : public InstructionBaseWithTrailingOperands<
                                          SILInstructionKind::WitnessMethodInst,
                                          WitnessMethodInst, MethodInst> {
  friend SILBuilder;

  CanType LookupType;
  ProtocolConformanceRef Conformance;

  WitnessMethodInst(SILDebugLocation DebugLoc, CanType LookupType,
                    ProtocolConformanceRef Conformance, SILDeclRef Member,
                    SILType Ty, ArrayRef<SILValue> TypeDependentOperands)
      : InstructionBaseWithTrailingOperands(TypeDependentOperands,
                                            DebugLoc, Ty, Member),
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

  ArrayRef<Operand> getTypeDependentOperands() const {
    return getAllOperands();
  }

  MutableArrayRef<Operand> getTypeDependentOperands() {
    return getAllOperands();
  }
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
  OpenedExistentialAccess getAccessKind() const { return ForAccess; }
};

/// Given an opaque value referring to an existential, "opens" the
/// existential by returning a pointer to a fresh archetype T, which also
/// captures the (dynamic) conformances.
class OpenExistentialValueInst
    : public UnaryInstructionBase<SILInstructionKind::OpenExistentialValueInst,
                                  GuaranteedFirstArgForwardingSingleValueInst> {
  friend SILBuilder;

  OpenExistentialValueInst(SILDebugLocation debugLoc, SILValue operand,
                           SILType selfTy,
                           ValueOwnershipKind forwardingOwnershipKind);
};

/// Given a class existential, "opens" the
/// existential by returning a pointer to a fresh archetype T, which also
/// captures the (dynamic) conformances.
class OpenExistentialRefInst
    : public UnaryInstructionBase<SILInstructionKind::OpenExistentialRefInst,
                                  FirstArgOwnershipForwardingSingleValueInst> {
  friend SILBuilder;

  OpenExistentialRefInst(SILDebugLocation DebugLoc, SILValue Operand,
                         SILType Ty,
                         ValueOwnershipKind forwardingOwnershipKind);
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
};

/// Given a boxed existential container, "opens" the existential by returning a
/// fresh archetype T, which also captures the (dynamic) conformances.
class OpenExistentialBoxValueInst
    : public UnaryInstructionBase<
          SILInstructionKind::OpenExistentialBoxValueInst,
          GuaranteedFirstArgForwardingSingleValueInst> {
  friend SILBuilder;

  OpenExistentialBoxValueInst(SILDebugLocation DebugLoc, SILValue operand,
                              SILType ty,
                              ValueOwnershipKind forwardingOwnershipKind);
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
          FirstArgOwnershipForwardingSingleValueInst> {
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
      exType = exMetatype.getInstanceType();
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

  UncheckedOwnershipConversionInst(SILDebugLocation DebugLoc, SILValue operand,
                                   ValueOwnershipKind Kind)
      : UnaryInstructionBase(DebugLoc, operand, operand->getType()) {
    SILNode::Bits.UncheckedOwnershipConversionInst.Kind = Kind;
  }

public:
  ValueOwnershipKind getConversionOwnershipKind() const {
    unsigned kind = SILNode::Bits.UncheckedOwnershipConversionInst.Kind;
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
                             FirstArgOwnershipForwardingSingleValueInst> {
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
  bool poisonRefs() const { return SILNode::Bits.DestroyValueInst.PoisonRefs; }

  void setPoisonRefs(bool poisonRefs = true) {
    SILNode::Bits.DestroyValueInst.PoisonRefs = poisonRefs;
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
    return SILNode::Bits.BeginCOWMutationInst.Native;
  }
  
  void setNative(bool native = true) {
    SILNode::Bits.BeginCOWMutationInst.Native = native;
  }
};

/// Marks the end of the mutation of a reference counted object.
class EndCOWMutationInst
    : public UnaryInstructionBase<SILInstructionKind::EndCOWMutationInst,
                                  SingleValueInstruction>
{
  friend SILBuilder;

  EndCOWMutationInst(SILDebugLocation DebugLoc, SILValue Operand,
                     bool keepUnique)
      : UnaryInstructionBase(DebugLoc, Operand, Operand->getType()) {
    setKeepUnique(keepUnique);
  }

public:
  bool doKeepUnique() const {
    return SILNode::Bits.EndCOWMutationInst.KeepUnique;
  }

  void setKeepUnique(bool keepUnique = true) {
    SILNode::Bits.EndCOWMutationInst.KeepUnique = keepUnique;
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

private:
  DeallocRefInst(SILDebugLocation DebugLoc, SILValue Operand,
                 bool canBeOnStack = false)
      : UnaryInstructionBase(DebugLoc, Operand) {
    SILNode::Bits.DeallocRefInst.OnStack = canBeOnStack;
  }

public:
  bool canAllocOnStack() const {
    return SILNode::Bits.DeallocRefInst.OnStack;
  }

  void setStackAllocatable(bool OnStack) {
    SILNode::Bits.DeallocRefInst.OnStack = OnStack;
  }
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

/// Deallocate memory allocated for an unsafe value buffer.
class DeallocValueBufferInst
    : public UnaryInstructionBase<SILInstructionKind::DeallocValueBufferInst,
                                  DeallocationInst> {
  friend SILBuilder;

  SILType ValueType;

  DeallocValueBufferInst(SILDebugLocation DebugLoc, SILType valueType,
                         SILValue operand)
      : UnaryInstructionBase(DebugLoc, operand), ValueType(valueType) {}

public:
  SILType getValueType() const { return ValueType; }
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

/// Project out the address of the value
/// stored in the given Builtin.UnsafeValueBuffer.
class ProjectValueBufferInst
    : public UnaryInstructionBase<SILInstructionKind::ProjectValueBufferInst,
                                  SingleValueInstruction> {
  friend SILBuilder;

  ProjectValueBufferInst(SILDebugLocation DebugLoc, SILType valueType,
                         SILValue operand)
      : UnaryInstructionBase(DebugLoc, operand, valueType.getAddressType()) {}

public:
  SILType getValueType() const { return getType().getObjectType(); }
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

  DEFINE_ABSTRACT_SINGLE_VALUE_INST_BOILERPLATE(IndexingInst)
};

/// IndexAddrInst - "%2 : $*T = index_addr %0 : $*T, %1 : $Builtin.Word"
/// This takes an address and indexes it, striding by the pointed-
/// to type.  This is used to index into arrays of uniform elements.
class IndexAddrInst
    : public InstructionBase<SILInstructionKind::IndexAddrInst,
                             IndexingInst> {
  friend SILBuilder;

  enum { Base, Index };

  IndexAddrInst(SILDebugLocation DebugLoc, SILValue Operand, SILValue Index)
      : InstructionBase(DebugLoc, Operand->getType(), Operand, Index) {}
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

  /// Returns true if \p BB is a successor of this block.
  bool isSuccessorBlock(SILBasicBlock *BB) const {
    auto Range = getSuccessorBlocks();
    return any_of(Range, [&BB](const SILBasicBlock *SuccBB) -> bool {
      return BB == SuccBB;
    });
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

  DEFINE_ABSTRACT_NON_VALUE_INST_BOILERPLATE(TermInst)

  bool isBranch() const { return !getSuccessors().empty(); }

  /// Returns true if this terminator exits the function.
  bool isFunctionExiting() const;

  /// Returns true if this terminator terminates the program.
  bool isProgramTerminating() const;

  TermKind getTermKind() const { return TermKind(getKind()); }

  /// Returns true if this is a transformation terminator.
  bool isTransformationTerminator() const {
    switch (getTermKind()) {
    case TermKind::UnwindInst:
    case TermKind::UnreachableInst:
    case TermKind::ReturnInst:
    case TermKind::ThrowInst:
    case TermKind::YieldInst:
    case TermKind::TryApplyInst:
    case TermKind::BranchInst:
    case TermKind::CondBranchInst:
    case TermKind::SwitchValueInst:
    case TermKind::SwitchEnumAddrInst:
    case TermKind::DynamicMethodBranchInst:
    case TermKind::CheckedCastAddrBranchInst:
    case TermKind::CheckedCastValueBranchInst:
    case TermKind::AwaitAsyncContinuationInst:
      return false;
    case TermKind::SwitchEnumInst:
    case TermKind::CheckedCastBranchInst:
      return true;
    }
    llvm_unreachable("Covered switch isn't covered.");
  }
};

class OwnershipForwardingTermInst : public TermInst,
                                    public OwnershipForwardingMixin {
protected:
  OwnershipForwardingTermInst(SILInstructionKind kind,
                              SILDebugLocation debugLoc,
                              ValueOwnershipKind ownershipKind)
      : TermInst(kind, debugLoc),
        OwnershipForwardingMixin(kind, ownershipKind) {
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

  /// The number of arguments for the True branch.
  unsigned getNumTrueArgs() const {
    return SILNode::Bits.CondBranchInst.NumTrueArgs;
  }
  /// The number of arguments for the False branch.
  unsigned getNumFalseArgs() const {
    return getAllOperands().size() - NumFixedOpers -
        SILNode::Bits.CondBranchInst.NumTrueArgs;
  }

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
    return SILNode::Bits.SwitchValueInst.HasDefault;
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
    SILNode::Bits.SEIBase.HasDefault = bool(DefaultBB);
    SILNode::Bits.SEIBase.NumCases = CaseBBs.size();
    // Initialize the case and successor arrays.
    auto *cases = getCaseBuf();
    auto *succs = getSuccessorBuf();
    for (unsigned i = 0, size = CaseBBs.size(); i < size; ++i) {
      cases[i] = CaseBBs[i].first;
      if (Counts) {
        ::new (succs + i)
            SILSuccessor(this, CaseBBs[i].second, Counts.getValue()[i]);
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

  unsigned getNumCases() const { return SILNode::Bits.SEIBase.NumCases; }

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

  bool hasDefault() const { return SILNode::Bits.SEIBase.HasDefault; }

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

  SILBasicBlock *getSuccessBB() { return DestBBs[0]; }
  const SILBasicBlock *getSuccessBB() const { return DestBBs[0]; }
  SILBasicBlock *getFailureBB() { return DestBBs[1]; }
  const SILBasicBlock *getFailureBB() const { return DestBBs[1]; }

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
                        ValueOwnershipKind forwardingOwnershipKind)
      : UnaryInstructionWithTypeDependentOperandsBase(
            DebugLoc, Operand, TypeDependentOperands, SuccessBB, FailureBB,
            Target1Count, Target2Count, forwardingOwnershipKind),
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
/// The success branch destination block receives the cast result as a BB
/// argument.
class CheckedCastValueBranchInst final
    : public UnaryInstructionWithTypeDependentOperandsBase<
          SILInstructionKind::CheckedCastValueBranchInst,
          CheckedCastValueBranchInst, CastBranchInstBase<TermInst>> {
  friend SILBuilder;

  CanType SourceFormalTy;
  SILType DestLoweredTy;
  CanType DestFormalTy;

  CheckedCastValueBranchInst(SILDebugLocation DebugLoc, SILValue Operand,
                             CanType SourceFormalTy,
                             ArrayRef<SILValue> TypeDependentOperands,
                             SILType DestLoweredTy, CanType DestFormalTy,
                             SILBasicBlock *SuccessBB, SILBasicBlock *FailureBB)
      : UnaryInstructionWithTypeDependentOperandsBase(
            DebugLoc, Operand, TypeDependentOperands, SuccessBB, FailureBB,
            ProfileCounter(), ProfileCounter()),
        SourceFormalTy(SourceFormalTy), DestLoweredTy(DestLoweredTy),
        DestFormalTy(DestFormalTy) {}

  static CheckedCastValueBranchInst *
  create(SILDebugLocation DebugLoc,
         SILValue Operand, CanType SourceFormalTy,
         SILType DestLoweredTy, CanType DestFormalTy,
         SILBasicBlock *SuccessBB, SILBasicBlock *FailureBB,
         SILFunction &F);

public:
  SILType getSourceLoweredType() const { return getOperand()->getType(); }
  CanType getSourceFormalType() const { return SourceFormalTy; }

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
                                SILInstructionKind::UncheckedAddrCastInst,
                                UncheckedAddrCastInst,
                                ConversionInst>
{
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
          AllArgOwnershipForwardingSingleValueInst> {
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
};

/// LinearFunctionInst - given a function, its derivative and traspose functions,
/// create an `@differentiable(_linear)` function that represents a bundle of these.
class LinearFunctionInst final
    : public InstructionBaseWithTrailingOperands<
          SILInstructionKind::LinearFunctionInst, LinearFunctionInst,
          AllArgOwnershipForwardingSingleValueInst> {
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
};

/// DifferentiableFunctionExtractInst - extracts either the original or
/// derivative function value from a `@differentiable` function.
class DifferentiableFunctionExtractInst
    : public UnaryInstructionBase<
          SILInstructionKind::DifferentiableFunctionExtractInst,
          GuaranteedFirstArgForwardingSingleValueInst> {
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
                                  GuaranteedFirstArgForwardingSingleValueInst> {
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
      Callee = CFI->getConverted();
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
    // Intentionally don't lookup throught dynamic_function_ref and
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

class OwnershipForwardingMultipleValueInstruction
    : public MultipleValueInstruction,
      public OwnershipForwardingMixin {
public:
  OwnershipForwardingMultipleValueInstruction(SILInstructionKind kind,
                                              SILDebugLocation loc,
                                              ValueOwnershipKind ownershipKind)
      : MultipleValueInstruction(kind, loc),
        OwnershipForwardingMixin(kind, ownershipKind) {
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

inline bool OwnershipForwardingMixin::isa(SILInstructionKind kind) {
  return FirstArgOwnershipForwardingSingleValueInst::classof(kind) ||
         AllArgOwnershipForwardingSingleValueInst::classof(kind) ||
         OwnershipForwardingTermInst::classof(kind) ||
         OwnershipForwardingConversionInst::classof(kind) ||
         OwnershipForwardingSelectEnumInstBase::classof(kind) ||
         OwnershipForwardingMultipleValueInstruction::classof(kind);
}

inline OwnershipForwardingMixin *
OwnershipForwardingMixin::get(SILInstruction *inst) {
  // I am purposely performing this cast in this manner rather than reinterpret
  // casting to OwnershipForwardingMixin to ensure that we offset to the
  // appropriate offset inside of inst instead of converting inst's current
  // location to an OwnershipForwardingMixin which would be incorrect.
  if (auto *result = dyn_cast<FirstArgOwnershipForwardingSingleValueInst>(inst))
    return result;
  if (auto *result = dyn_cast<AllArgOwnershipForwardingSingleValueInst>(inst))
    return result;
  if (auto *result = dyn_cast<OwnershipForwardingTermInst>(inst))
    return result;
  if (auto *result = dyn_cast<OwnershipForwardingConversionInst>(inst))
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
  void removeNodeFromList(SILInstruction *I);
  void transferNodesFromList(ilist_traits<SILInstruction> &L2,
                             instr_iterator first, instr_iterator last);

private:
  void createNode(const SILInstruction &);
};

} // end llvm namespace

#endif
