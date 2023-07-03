//===--- SILConstants.h - SIL constant representation -----------*- C++ -*-===//
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
// This defines an interface to represent SIL level structured constants in a
// memory efficient way.
//
//===----------------------------------------------------------------------===//

#ifndef SWIFT_SIL_CONSTANTS_H
#define SWIFT_SIL_CONSTANTS_H

#include "swift/AST/SubstitutionMap.h"
#include "swift/SIL/SILInstruction.h"
#include "swift/SIL/SILValue.h"
#include "llvm/Support/CommandLine.h"

namespace swift {
class SingleValueInstruction;
class SILValue;
class SILBuilder;
class SerializedSILLoader;

struct AggregateSymbolicValue;
struct SymbolicArrayStorage;
struct DerivedAddressValue;
struct EnumWithPayloadSymbolicValue;
struct SymbolicValueMemoryObject;
struct UnknownSymbolicValue;
struct SymbolicClosure;

extern llvm::cl::opt<unsigned> ConstExprLimit;

/// An abstract class that exposes functions for allocating symbolic values.
/// The implementors of this class have to determine where to allocate them and
/// and manage the lifetime of the allocated symbolic values.
class SymbolicValueAllocator {
public:
  virtual ~SymbolicValueAllocator() {}

  /// Allocate raw bytes.
  /// \param byteSize number of bytes to allocate.
  /// \param alignment alignment for the allocated bytes.
  virtual void *allocate(unsigned long byteSize, unsigned alignment) = 0;

  /// Allocate storage for a given number of elements of a specific type
  /// provided as a template parameter. Precondition: \c T must have an
  /// accessible zero argument constructor.
  /// \param numElts number of elements of the type to allocate.
  template <typename T> T *allocate(unsigned numElts) {
    T *res = (T *)allocate(sizeof(T) * numElts, alignof(T));
    for (unsigned i = 0; i != numElts; ++i)
      new (res + i) T();
    return res;
  }
};

/// A class that allocates symbolic values in a local bump allocator. The
/// lifetime of the bump allocator is same as the lifetime of \c this object.
class SymbolicValueBumpAllocator : public SymbolicValueAllocator {
private:
  llvm::BumpPtrAllocator bumpAllocator;

public:
  SymbolicValueBumpAllocator() {}
  ~SymbolicValueBumpAllocator() {}

  void *allocate(unsigned long byteSize, unsigned alignment) override {
    return bumpAllocator.Allocate(byteSize, alignment);
  }
};

/// When we fail to constant fold a value, this captures a reason why,
/// allowing the caller to produce a specific diagnostic.  The "Unknown"
/// SymbolicValue representation also includes a pointer to the SILNode in
/// question that was problematic.
class UnknownReason {
public:
  enum UnknownKind {
    // TODO: Eliminate the default kind, by making classifications for each
    // failure mode.
    Default,

    /// The constant expression was too big.  This is reported on a random
    /// instruction within the constexpr that triggered the issue.
    TooManyInstructions,

    /// A control flow loop was found.
    Loop,

    /// Integer overflow detected.
    Overflow,

    /// Trap detected. Traps will a message as a payload.
    Trap,

    /// An operation was applied over operands whose symbolic values were
    /// constants but were not valid for the operation.
    InvalidOperandValue,

    /// Encountered an instruction not supported by the interpreter.
    UnsupportedInstruction,

    /// Encountered a function call whose arguments are not constants.
    CallArgumentUnknown,

    /// Encountered a function call where the body of the called function is
    /// not available.
    CalleeImplementationUnknown,

    /// Attempted to load from/store into a SIL value that was not tracked by
    /// the interpreter.
    UntrackedSILValue,

    /// Encountered a checked cast operation whose result cannot be evaluated
    /// to a constant.
    UnknownCastResult,

    /// Attempted to find a concrete protocol conformance for a witness method
    /// and failed.
    UnknownWitnessMethodConformance,

    /// Attempted to determine the SIL function of a witness method  and failed.
    NoWitnesTableEntry,

    /// The value of a top-level variable cannot be determined to be a constant.
    /// This is only relevant in the backward evaluation mode, which is used by
    /// #assert.
    NotTopLevelConstant,

    /// A top-level value has multiple writers. This is only relevant in the
    /// non-flow-sensitive evaluation mode,  which is used by #assert.
    MutipleTopLevelWriters,

    /// Indicates the return value of an instruction that was not evaluated
    /// during interpretation.
    ReturnedByUnevaluatedInstruction,

    /// Indicates that the value was possibly modified by an instruction
    /// that was not evaluated during the interpretation.
    MutatedByUnevaluatedInstruction,
  };

private:
  UnknownKind kind;

  // Auxiliary information for different unknown kinds.
  union {
    SILFunction *function;   // For CalleeImplementationUnknown
    const char *trapMessage; // For Trap.
    unsigned argumentIndex;  // For CallArgumentUnknown
  } payload;

public:
  UnknownKind getKind() { return kind; }

  static bool isUnknownKindWithPayload(UnknownKind kind) {
    switch (kind) {
    case UnknownKind::CalleeImplementationUnknown:
    case UnknownKind::Trap:
    case UnknownKind::CallArgumentUnknown:
      return true;
    default:
      return false;
    }
  }

  static UnknownReason create(UnknownKind kind) {
    assert(!isUnknownKindWithPayload(kind));
    UnknownReason reason;
    reason.kind = kind;
    return reason;
  }

  static UnknownReason createCalleeImplementationUnknown(SILFunction *callee) {
    assert(callee);
    UnknownReason reason;
    reason.kind = UnknownKind::CalleeImplementationUnknown;
    reason.payload.function = callee;
    return reason;
  }

  SILFunction *getCalleeWithoutImplmentation() {
    assert(kind == UnknownKind::CalleeImplementationUnknown);
    return payload.function;
  }

  static UnknownReason createTrap(StringRef message,
                                  SymbolicValueAllocator &allocator) {
    // Copy and null terminate the string.
    size_t size = message.size();
    char *messagePtr = allocator.allocate<char>(size + 1);
    std::uninitialized_copy(message.begin(), message.end(), messagePtr);
    messagePtr[size] = '\0';

    UnknownReason reason;
    reason.kind = UnknownKind::Trap;
    reason.payload.trapMessage = messagePtr;
    return reason;
  }

  const char *getTrapMessage() {
    assert(kind == UnknownKind::Trap);
    return payload.trapMessage;
  }

  static UnknownReason createCallArgumentUnknown(unsigned argIndex) {
    UnknownReason reason;
    reason.kind = UnknownKind::CallArgumentUnknown;
    reason.payload.argumentIndex = argIndex;
    return reason;
  }

  unsigned getArgumentIndex() {
    assert(kind == UnknownKind::CallArgumentUnknown);
    return payload.argumentIndex;
  }
};

/// This is the symbolic value tracked for each SILValue in a scope.  We
/// support multiple representational forms for the constant node in order to
/// avoid pointless memory bloat + copying.  This is intended to be a
/// light-weight POD type we can put in hash tables and pass around by-value.
///
/// Internally, this value has multiple ways to represent the same sorts of
/// symbolic values (e.g. to save memory).  It provides a simpler public
/// interface though.
class SymbolicValue {
private:
  enum RepresentationKind {
    /// This value is an alloc stack that has not (yet) been initialized
    /// by flow-sensitive analysis.
    RK_UninitMemory,

    /// This symbolic value cannot be determined, carries multiple values
    /// (i.e., varies dynamically at the top level), or is of some type that
    /// we cannot analyze and propagate (e.g. NSObject).
    ///
    RK_Unknown,

    /// This value is known to be a metatype reference.  The type is stored
    /// in the "metatype" member.
    RK_Metatype,

    /// This value is known to be a function reference, e.g. through
    /// function_ref directly, or a devirtualized method reference.
    RK_Function,

    /// This value is represented with a bump-pointer allocated APInt.
    RK_Integer,

    /// This value is represented with an inline integer representation.
    RK_IntegerInline,

    /// This value is represented with a bump-pointer allocated char array
    /// representing a UTF-8 encoded string.
    RK_String,

    /// This value is a struct or tuple of constants.  This is tracked by the
    /// "aggregate" member of the value union.
    RK_Aggregate,

    /// This value is an enum with no payload.
    RK_Enum,

    /// This value is an enum with a payload.
    RK_EnumWithPayload,

    /// This represents the address of a memory object.
    RK_DirectAddress,

    /// This represents an index *into* a memory object.
    RK_DerivedAddress,

    /// This represents the internal storage of an array.
    RK_ArrayStorage,

    /// This represents an array.
    RK_Array,

    /// This represents a closure.
    RK_Closure,
  };

  union {
    /// When the value is Unknown, this contains information about the
    /// unfoldable part of the computation.
    UnknownSymbolicValue *unknown;

    /// This is always a SILType with an object category.  This is the value
    /// of the underlying instance type, not the MetatypeType.
    TypeBase *metatype;

    SILFunction *function;

    /// When this SymbolicValue is of "Integer" kind, this pointer stores
    /// the words of the APInt value it holds.
    uint64_t *integer;

    /// This holds the bits of an integer for an inline representation.
    uint64_t integerInline;

    /// When this SymbolicValue is of "String" kind, this pointer stores
    /// information about the StringRef value it holds.
    const char *string;

    /// When this SymbolicValue is of "Aggregate" kind, this pointer stores
    /// information about the aggregate elements, its type and count.
    const AggregateSymbolicValue *aggregate;

    /// When this SymbolicValue is of "Enum" kind, this pointer stores
    /// information about the enum case type.
    EnumElementDecl *enumVal;

    /// When this SymbolicValue is of "EnumWithPayload" kind, this pointer
    /// stores information about the enum case type and its payload.
    EnumWithPayloadSymbolicValue *enumValWithPayload;

    /// When the representationKind is "DirectAddress", this pointer is the
    /// memory object referenced.
    SymbolicValueMemoryObject *directAddress;

    /// When this SymbolicValue is of "DerivedAddress" kind, this pointer stores
    /// information about the memory object and access path of the access.
    DerivedAddressValue *derivedAddress;

    // The following two fields are for representing an Array.
    //
    // In Swift, an array is a non-trivial struct that stores a reference to an
    // internal storage: _ContiguousArrayStorage. Though arrays have value
    // semantics in Swift, it is not the case in SIL. In SIL, an array can be
    // mutated by taking the address of the internal storage i.e., through a
    // shared, mutable pointer to the internal storage of the array. In fact,
    // this is how an array initialization is lowered in SIL. Therefore, the
    // symbolic representation of an array is an addressable "memory cell"
    // (i.e., a SymbolicValueMemoryObject) containing the array storage. The
    // array storage is modeled by the type: SymbolicArrayStorage. This
    // representation of the array enables obtaining the address of the internal
    // storage and modifying the array through that address. Array operations
    // such as `append` that mutate an array must clone the internal storage of
    // the array, following the semantics of the Swift implementation of those
    // operations.

    /// Representation of array storage (RK_ArrayStorage). SymbolicArrayStorage
    /// is a container for a sequence of symbolic values.
    SymbolicArrayStorage *arrayStorage;

    /// When this symbolic value is of an "Array" kind, this stores a memory
    /// object that contains a SymbolicArrayStorage value.
    SymbolicValueMemoryObject *array;

    /// When this symbolic value is of "Closure" kind, store a pointer to the
    /// symbolic representation of the closure.
    SymbolicClosure *closure;
  } value;

  RepresentationKind representationKind : 8;

  union {
    /// This is the number of bits in an RK_Integer or RK_IntegerInline
    /// representation, which makes the number of entries in the list derivable.
    unsigned integerBitwidth;

    /// This is the number of bytes for an RK_String representation.
    unsigned stringNumBytes;
  } auxInfo;

public:
  /// This enum is used to indicate the sort of value held by a SymbolicValue
  /// independent of its concrete representation.  This is the public
  /// interface to SymbolicValue.
  enum Kind {
    /// This is a value that isn't a constant.
    Unknown,

    /// This is a known metatype value.
    Metatype,

    /// This is a function, represented as a SILFunction.
    Function,

    /// This is an integer constant.
    Integer,

    /// String values may have SIL type of Builtin.RawPointer or Builtin.Word
    /// type.
    String,

    /// This can be an array, struct, tuple, etc.
    Aggregate,

    /// This is an enum without payload.
    Enum,

    /// This is an enum with payload (formally known as "associated value").
    EnumWithPayload,

    /// This value represents the address of, or into, a memory object.
    Address,

    /// This represents an internal array storage.
    ArrayStorage,

    /// This represents an array value.
    Array,

    /// This represents a closure.
    Closure,

    /// These values are generally only seen internally to the system, external
    /// clients shouldn't have to deal with them.
    UninitMemory
  };

  /// For constant values, return the type classification of this value.
  Kind getKind() const;

  /// Return true if this represents a constant value.
  bool isConstant() const {
    auto kind = getKind();
    return kind != Unknown && kind != UninitMemory;
  }

  static SymbolicValue getUnknown(SILNode *node, UnknownReason reason,
                                  llvm::ArrayRef<SourceLoc> callStack,
                                  SymbolicValueAllocator &allocator);

  /// Return true if this represents an unknown result.
  bool isUnknown() const { return getKind() == Unknown; }

  /// Return the call stack for an unknown result.
  ArrayRef<SourceLoc> getUnknownCallStack() const;

  /// Return the node that triggered an unknown result.
  SILNode *getUnknownNode() const;

  /// Return the reason an unknown result was generated.
  UnknownReason getUnknownReason() const;

  static SymbolicValue getUninitMemory() {
    SymbolicValue result;
    result.representationKind = RK_UninitMemory;
    return result;
  }

  static SymbolicValue getMetatype(CanType type) {
    SymbolicValue result;
    result.representationKind = RK_Metatype;
    result.value.metatype = type.getPointer();
    return result;
  }

  CanType getMetatypeValue() const {
    assert(representationKind == RK_Metatype);
    return CanType(value.metatype);
  }

  static SymbolicValue getFunction(SILFunction *fn) {
    assert(fn && "Function cannot be null");
    SymbolicValue result;
    result.representationKind = RK_Function;
    result.value.function = fn;
    return result;
  }

  SILFunction *getFunctionValue() const {
    assert(getKind() == Function);
    return value.function;
  }

  static SymbolicValue getInteger(int64_t value, unsigned bitWidth);
  static SymbolicValue getInteger(const APInt &value,
                                  SymbolicValueAllocator &allocator);

  APInt getIntegerValue() const;
  unsigned getIntegerValueBitWidth() const;

  /// Returns a SymbolicValue representing a UTF-8 encoded string.
  static SymbolicValue getString(StringRef string,
                                 SymbolicValueAllocator &allocator);

  /// Returns the UTF-8 encoded string underlying a SymbolicValue.
  StringRef getStringValue() const;

  /// This returns an aggregate value with the specified elements in it.  This
  /// copies the member values into the specified Allocator.
  static SymbolicValue getAggregate(ArrayRef<SymbolicValue> members,
                                    Type aggregateType,
                                    SymbolicValueAllocator &allocator);

  ArrayRef<SymbolicValue> getAggregateMembers() const;

  /// Return the type of this aggregate symbolic value.
  Type getAggregateType() const;

  /// This returns a constant Symbolic value for the enum case in `decl`, which
  /// must not have an associated value.
  static SymbolicValue getEnum(EnumElementDecl *decl) {
    assert(decl);
    SymbolicValue result;
    result.representationKind = RK_Enum;
    result.value.enumVal = decl;
    return result;
  }

  /// `payload` must be a constant.
  static SymbolicValue getEnumWithPayload(EnumElementDecl *decl,
                                          SymbolicValue payload,
                                          SymbolicValueAllocator &allocator);

  EnumElementDecl *getEnumValue() const;

  SymbolicValue getEnumPayloadValue() const;

  /// Return a symbolic value that represents the address of a memory object.
  static SymbolicValue getAddress(SymbolicValueMemoryObject *memoryObject) {
    SymbolicValue result;
    result.representationKind = RK_DirectAddress;
    result.value.directAddress = memoryObject;
    return result;
  }

  /// Return a symbolic value that represents the address of a memory object
  /// indexed by a path.
  static SymbolicValue getAddress(SymbolicValueMemoryObject *memoryObject,
                                  ArrayRef<unsigned> indices,
                                  SymbolicValueAllocator &allocator);

  /// Return the memory object of this reference along with any access path
  /// indices involved.
  SymbolicValueMemoryObject *
  getAddressValue(SmallVectorImpl<unsigned> &accessPath) const;

  /// Return just the memory object for an address value.
  SymbolicValueMemoryObject *getAddressValueMemoryObject() const;

  /// Create a symbolic array storage containing \c elements.
  static SymbolicValue
  getSymbolicArrayStorage(ArrayRef<SymbolicValue> elements, CanType elementType,
                          SymbolicValueAllocator &allocator);

  /// Create a symbolic array using the given symbolic array storage, which
  /// contains the array elements.
  static SymbolicValue getArray(Type arrayType, SymbolicValue arrayStorage,
                                SymbolicValueAllocator &allocator);

  /// Return the elements stored in this SymbolicValue of "ArrayStorage" kind.
  ArrayRef<SymbolicValue> getStoredElements(CanType &elementType) const;

  /// Return the symbolic value representing the internal storage of this array.
  SymbolicValue getStorageOfArray() const;

  /// Return the symbolic value representing the address of the element of this
  /// array at the given \c index. The return value is a derived address whose
  /// base is the memory object \c value.array (which contains the array
  /// storage) and whose accesspath is \c index.
  SymbolicValue getAddressOfArrayElement(SymbolicValueAllocator &allocator,
                                         unsigned index) const;

  /// Return the type of this array symbolic value.
  Type getArrayType() const;

  /// Create and return a symbolic value that represents a closure.
  /// \param target SILFunction corresponding the target of the closure.
  /// \param capturedArguments an array consisting of SILValues of captured
  /// arguments along with their symbolic values when available.
  /// \param allocator the allocator to use for storing the contents of this
  /// symbolic value.
  static SymbolicValue
  makeClosure(SILFunction *target,
              ArrayRef<std::pair<SILValue, llvm::Optional<SymbolicValue>>>
                  capturedArguments,
              SubstitutionMap substMap, SingleValueInstruction *closureInst,
              SymbolicValueAllocator &allocator);

  SymbolicClosure *getClosure() const {
    assert(getKind() == Closure);
    return value.closure;
  }

  //===--------------------------------------------------------------------===//
  // Helpers

  /// Dig through single element aggregates, return the ultimate thing inside of
  /// it.  This is useful when dealing with integers and floats, because they
  /// are often wrapped in single-element struct wrappers.
  SymbolicValue lookThroughSingleElementAggregates() const;

  /// Given that this is an 'Unknown' value, emit diagnostic notes providing
  /// context about what the problem is.  If there is no location for some
  /// reason, we fall back to using the specified location.
  void emitUnknownDiagnosticNotes(SILLocation fallbackLoc);

  bool isUnknownDueToUnevaluatedInstructions();

  /// Clone this SymbolicValue into the specified Allocator and return the new
  /// version. This only works for valid constants.
  SymbolicValue cloneInto(SymbolicValueAllocator &allocator) const;

  /// Check that all nested SymbolicValues are constant. Symbolic values such as arrays,
  /// aggregates and pointers can contain non-constant symbolic values, when instructions
  /// are skipped during evaluation.
  bool containsOnlyConstants() const;

  void print(llvm::raw_ostream &os, unsigned indent = 0) const;
  void dump() const;
};

static_assert(sizeof(SymbolicValue) == 2 * sizeof(uint64_t),
              "SymbolicValue should stay small");
static_assert(std::is_trivial<SymbolicValue>::value,
              "SymbolicValue should stay trivial");

inline llvm::raw_ostream &operator<<(llvm::raw_ostream &os, SymbolicValue val) {
  val.print(os);
  return os;
}

/// This is a representation of a memory object referred to by an address.
/// Memory objects may be mutated over their lifetime, but their overall type
/// remains the same.
struct SymbolicValueMemoryObject {
  Type getType() const { return type; }

  SymbolicValue getValue() const { return value; }
  void setValue(SymbolicValue newValue) {
    assert((newValue.getKind() != SymbolicValue::Aggregate ||
            newValue.getAggregateType()->isEqual(type)) &&
           "Memory object type does not match the type of the symbolic value");
    value = newValue;
  }

  /// Create a new memory object whose overall type is as specified.
  static SymbolicValueMemoryObject *create(Type type, SymbolicValue value,
                                           SymbolicValueAllocator &allocator);

  /// Given that this memory object contains an aggregate value like
  /// {{1, 2}, 3}, and given an access path like [0,1], return the indexed
  /// element, e.g. "2" in this case.
  ///
  /// Returns uninit memory if the access path points at or into uninit memory.
  ///
  /// Precondition: The access path must be valid for this memory object's type.
  SymbolicValue getIndexedElement(ArrayRef<unsigned> accessPath);

  /// Given that this memory object contains an aggregate value like
  /// {{1, 2}, 3}, given an access path like [0,1], and given a new element like
  /// "4", set the indexed element to the specified scalar, producing {{1, 4},
  /// 3} in this case.
  ///
  /// Precondition: The access path must be valid for this memory object's type.
  void setIndexedElement(ArrayRef<unsigned> accessPath,
                         SymbolicValue newElement,
                         SymbolicValueAllocator &allocator);

private:
  const Type type;
  SymbolicValue value;

  SymbolicValueMemoryObject(Type type, SymbolicValue value)
      : type(type), value(value) {}
  SymbolicValueMemoryObject(const SymbolicValueMemoryObject &) = delete;
  void operator=(const SymbolicValueMemoryObject &) = delete;
};

using SymbolicClosureArgument =
    std::pair<SILValue, llvm::Optional<SymbolicValue>>;

/// Representation of a symbolic closure. A symbolic closure consists of a
/// SILFunction and an array of SIL values, corresponding to the captured
/// arguments, and (optional) symbolic values representing the constant values
/// of the captured arguments. The symbolic values are optional as it is not
/// necessary for every captured argument to be a constant, which enables
/// representing closures whose captured arguments are not compile-time
/// constants.
struct SymbolicClosure final
  : private llvm::TrailingObjects<SymbolicClosure, SymbolicClosureArgument> {

  friend class llvm::TrailingObjects<SymbolicClosure, SymbolicClosureArgument>;

private:

  SILFunction *target;

  // The number of SIL values captured by the closure.
  unsigned numCaptures;

  // True iff there exists a captured argument whose constant value is not
  // known.
  bool hasNonConstantCaptures = true;

  // A substitution map that partially maps the generic parameters of the
  // applied function to the generic arguments of passed to the call.
  SubstitutionMap substitutionMap;

  // The closure instruction such as partial apply that resulted in this
  // symbolic value. This is tracked to obtain SILType and other SIL-level
  // information of the symbolic closure.
  SingleValueInstruction *closureInst;

  SymbolicClosure() = delete;
  SymbolicClosure(const SymbolicClosure &) = delete;
  SymbolicClosure(SILFunction *callee, unsigned numArguments,
                  SubstitutionMap substMap, SingleValueInstruction *inst,
                  bool nonConstantCaptures)
      : target(callee), numCaptures(numArguments),
        hasNonConstantCaptures(nonConstantCaptures), substitutionMap(substMap),
        closureInst(inst) {}

public:
  static SymbolicClosure *create(SILFunction *callee,
                                 ArrayRef<SymbolicClosureArgument> args,
                                 SubstitutionMap substMap,
                                 SingleValueInstruction *closureInst,
                                 SymbolicValueAllocator &allocator);

  ArrayRef<SymbolicClosureArgument> getCaptures() const {
    return {getTrailingObjects<SymbolicClosureArgument>(), numCaptures};
  }

  // This is used by the llvm::TrailingObjects base class.
  size_t numTrailingObjects(OverloadToken<SymbolicClosureArgument>) const {
    return numCaptures;
  }

  SILFunction *getTarget() {
    return target;
  }

  SingleValueInstruction *getClosureInst() { return closureInst; }

  SILType getClosureType() { return closureInst->getType(); }

  SubstitutionMap getCallSubstitutionMap() { return substitutionMap; }

  bool hasOnlyConstantCaptures() { return !hasNonConstantCaptures; }
};

} // end namespace swift

#endif
