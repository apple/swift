//===--- SILConstants.cpp - SIL constant representation -------------------===//
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

#include "swift/SIL/SILConstants.h"
#include "swift/AST/DiagnosticsSIL.h"
#include "swift/Demangling/Demangle.h"
#include "swift/SIL/SILBuilder.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/Support/TrailingObjects.h"
using namespace swift;

namespace swift {
llvm::cl::opt<unsigned>
    ConstExprLimit("constexpr-limit", llvm::cl::init(3072),
                   llvm::cl::desc("Number of instructions interpreted in a"
                                  " constexpr function"));
}

template <typename... T, typename... U>
static InFlightDiagnostic diagnose(ASTContext &Context, SourceLoc loc,
                                   Diag<T...> diag, U &&... args) {
  // The lifetime of StringRef arguments will be extended as necessary by this
  // utility. The copy happens in onTentativeDiagnosticFlush at the bottom of
  // DiagnosticEngine.cpp, which is called when the destructor of the
  // InFlightDiagnostic returned by diagnose runs.
  return Context.Diags.diagnose(loc, diag, std::forward<U>(args)...);
}

//===----------------------------------------------------------------------===//
// SymbolicValue implementation
//===----------------------------------------------------------------------===//

void SymbolicValue::print(llvm::raw_ostream &os, unsigned indent) const {
  os.indent(indent);
  switch (representationKind) {
  case RK_UninitMemory:
    os << "uninit\n";
    return;
  case RK_Unknown: {
    os << "unknown(" << (int)getUnknownReason().getKind() << "): ";
    getUnknownNode()->dump();
    return;
  }
  case RK_Metatype:
    os << "metatype: ";
    getMetatypeValue()->print(os);
    os << "\n";
    return;
  case RK_Function: {
    auto fn = getFunctionValue();
    os << "fn: " << fn->getName() << ": ";
    os << Demangle::demangleSymbolAsString(fn->getName());
    os << "\n";
    return;
  }
  case RK_Integer:
  case RK_IntegerInline:
    os << "int: " << getIntegerValue() << "\n";
    return;
  case RK_String:
    os << "string: \"" << getStringValue() << "\"\n";
    return;
  case RK_Aggregate: {
    ArrayRef<SymbolicValue> elements = getAggregateMembers();
    switch (elements.size()) {
    case 0:
      os << "agg: 0 elements []\n";
      return;
    case 1:
      os << "agg: 1 elt: ";
      elements[0].print(os, indent + 2);
      return;
    default:
      os << "agg: " << elements.size() << " elements [\n";
      for (auto elt : elements)
        elt.print(os, indent + 2);
      os.indent(indent) << "]\n";
      return;
    }
  }
  case RK_Enum: {
    auto *decl = getEnumValue();
    os << "enum: ";
    decl->print(os);
    return;
  }
  case RK_EnumWithPayload: {
    auto *decl = getEnumValue();
    os << "enum: ";
    decl->print(os);
    os << ", payload: ";
    getEnumPayloadValue().print(os, indent);
    return;
  }
  case RK_DirectAddress:
  case RK_DerivedAddress: {
    SmallVector<unsigned, 4> accessPath;
    SymbolicValueMemoryObject *memObject = getAddressValue(accessPath);
    os << "address[" << memObject->getType() << "] ";
    llvm::interleave(
        accessPath.begin(), accessPath.end(), [&](unsigned idx) { os << idx; },
        [&]() { os << ", "; });
    os << "\n";
    break;
  }
  case RK_ArrayStorage: {
    CanType elementType;
    ArrayRef<SymbolicValue> elements = getStoredElements(elementType);
    os << "elements type: " << elementType << " size: " << elements.size();
    switch (elements.size()) {
    case 0:
      os << " contents []\n";
      return;
    default:
      os << " contents [\n";
      for (auto elt : elements)
        elt.print(os, indent + 2);
      os.indent(indent) << "]\n";
      return;
    }
  }
  case RK_Array: {
    os << getArrayType() << ": \n";
    getStorageOfArray().print(os, indent);
    return;
  }
  case RK_Closure: {
    SymbolicClosure *clo = getClosure();
    SILFunction *target = clo->getTarget();
    std::string targetName = target->getName().str();
    os << "closure: target: " << targetName;
    ArrayRef<SymbolicClosureArgument> args = clo->getCaptures();
    os << " captures [\n";
    for (SymbolicClosureArgument closureArg : args) {
      os.indent(indent + 2) << closureArg.first << "\n";
    }
    os.indent(indent) << "] values: [\n";
    for (SymbolicClosureArgument closureArg : args) {
      llvm::Optional<SymbolicValue> value = closureArg.second;
      if (!value.has_value()) {
        os.indent(indent + 2) << "nil\n";
        continue;
      }
      value->print(os, indent + 2);
    }
    os.indent(indent) << "]\n";
    return;
  }
  }
}

void SymbolicValue::dump() const { print(llvm::errs()); }

/// For constant values, return the classification of this value.  We have
/// multiple forms for efficiency, but provide a simpler interface to clients.
SymbolicValue::Kind SymbolicValue::getKind() const {
  switch (representationKind) {
  case RK_UninitMemory:
    return UninitMemory;
  case RK_Unknown:
    return Unknown;
  case RK_Metatype:
    return Metatype;
  case RK_Function:
    return Function;
  case RK_Aggregate:
    return Aggregate;
  case RK_Enum:
    return Enum;
  case RK_EnumWithPayload:
    return EnumWithPayload;
  case RK_Integer:
  case RK_IntegerInline:
    return Integer;
  case RK_String:
    return String;
  case RK_DirectAddress:
  case RK_DerivedAddress:
    return Address;
  case RK_ArrayStorage:
    return ArrayStorage;
  case RK_Array:
    return Array;
  case RK_Closure:
    return Closure;
  }
  llvm_unreachable("covered switch");
}

/// Clone this SymbolicValue into the specified allocator and return the new
/// version.  This only works for valid constants.
SymbolicValue
SymbolicValue::cloneInto(SymbolicValueAllocator &allocator) const {
  auto thisRK = representationKind;
  switch (thisRK) {
  case RK_UninitMemory:
  case RK_Unknown:
  case RK_Metatype:
  case RK_Function:
    assert(0 && "cloning this representation kind is not supported");
  case RK_Enum:
    // These have trivial inline storage, just return a copy.
    return *this;
  case RK_IntegerInline:
  case RK_Integer:
    return SymbolicValue::getInteger(getIntegerValue(), allocator);
  case RK_String:
    return SymbolicValue::getString(getStringValue(), allocator);
  case RK_Aggregate: {
    auto elts = getAggregateMembers();
    SmallVector<SymbolicValue, 4> results;
    results.reserve(elts.size());
    for (auto elt : elts)
      results.push_back(elt.cloneInto(allocator));
    return getAggregate(results, getAggregateType(), allocator);
  }
  case RK_EnumWithPayload: {
    return getEnumWithPayload(
        getEnumValue(), getEnumPayloadValue().cloneInto(allocator), allocator);
  }
  case RK_DirectAddress:
  case RK_DerivedAddress: {
    SmallVector<unsigned, 4> accessPath;
    auto *memObject = getAddressValue(accessPath);
    auto *newMemObject = SymbolicValueMemoryObject::create(
        memObject->getType(), memObject->getValue().cloneInto(allocator),
        allocator);
    return getAddress(newMemObject, accessPath, allocator);
  }
  case RK_ArrayStorage: {
    CanType elementType;
    ArrayRef<SymbolicValue> oldElements = getStoredElements(elementType);
    SmallVector<SymbolicValue, 4> clonedElements;
    clonedElements.reserve(oldElements.size());
    for (auto elem : oldElements)
      clonedElements.push_back(elem.cloneInto(allocator));
    return getSymbolicArrayStorage(clonedElements, elementType, allocator);
  }
  case RK_Array: {
    SymbolicValue clonedStorage = getStorageOfArray().cloneInto(allocator);
    return getArray(getArrayType(), clonedStorage, allocator);
  }
  case RK_Closure: {
    SymbolicClosure *clo = getClosure();
    ArrayRef<SymbolicClosureArgument> closureArgs = clo->getCaptures();
    return SymbolicValue::makeClosure(clo->getTarget(), closureArgs,
                                      clo->getCallSubstitutionMap(),
                                      clo->getClosureInst(), allocator);
  }
  }
  llvm_unreachable("covered switch");
}

bool SymbolicValue::containsOnlyConstants() const {
  if (!isConstant())
    return false;

  auto thisRK = representationKind;
  switch (thisRK) {
  case RK_UninitMemory:
  case RK_Unknown:
  case RK_Metatype:
  case RK_Function:
  case RK_Enum:
  case RK_IntegerInline:
  case RK_Integer:
  case RK_String:
  case RK_Closure:
    return true;
  case RK_Aggregate: {
    auto elts = getAggregateMembers();
    for (auto elt : elts)
      if (!elt.containsOnlyConstants())
        return false;
    return true;
  }
  case RK_EnumWithPayload: {
    return getEnumPayloadValue().containsOnlyConstants();
  }
  case RK_DirectAddress:
  case RK_DerivedAddress: {
    auto *memObject = getAddressValueMemoryObject();
    return memObject->getValue().containsOnlyConstants();
  }
  case RK_ArrayStorage: {
    CanType elementType;
    ArrayRef<SymbolicValue> elts = getStoredElements(elementType);
    for (auto elt : elts)
      if (!elt.containsOnlyConstants())
        return false;
    return true;
  }
  case RK_Array: {
    return getStorageOfArray().containsOnlyConstants();
  }
  }
  llvm_unreachable("covered switch");
}

//===----------------------------------------------------------------------===//
// SymbolicValueMemoryObject implementation
//===----------------------------------------------------------------------===//

SymbolicValueMemoryObject *
SymbolicValueMemoryObject::create(Type type, SymbolicValue value,
                                  SymbolicValueAllocator &allocator) {
  auto *result = allocator.allocate(sizeof(SymbolicValueMemoryObject),
                                    alignof(SymbolicValueMemoryObject));
  new (result) SymbolicValueMemoryObject(type, value);
  return (SymbolicValueMemoryObject *)result;
}

//===----------------------------------------------------------------------===//
// Integers
//===----------------------------------------------------------------------===//

SymbolicValue SymbolicValue::getInteger(int64_t value, unsigned bitWidth) {
  SymbolicValue result;
  result.representationKind = RK_IntegerInline;
  result.value.integerInline = value;
  result.auxInfo.integerBitwidth = bitWidth;
  return result;
}

SymbolicValue SymbolicValue::getInteger(const APInt &value,
                                        SymbolicValueAllocator &allocator) {
  // In the common case, we can form an inline representation.
  unsigned numWords = value.getNumWords();
  if (numWords == 1)
    return getInteger(value.getRawData()[0], value.getBitWidth());

  // Copy the integers from the APInt into the allocator.
  auto *words = allocator.allocate<uint64_t>(numWords);
  std::uninitialized_copy(value.getRawData(), value.getRawData() + numWords,
                          words);

  SymbolicValue result;
  result.representationKind = RK_Integer;
  result.value.integer = words;
  result.auxInfo.integerBitwidth = value.getBitWidth();
  return result;
}

APInt SymbolicValue::getIntegerValue() const {
  assert(getKind() == Integer);
  if (representationKind == RK_IntegerInline) {
    auto numBits = auxInfo.integerBitwidth;
    return APInt(numBits, value.integerInline);
  }

  assert(representationKind == RK_Integer);
  auto numBits = auxInfo.integerBitwidth;
  auto numWords =
      (numBits + APInt::APINT_BITS_PER_WORD - 1) / APInt::APINT_BITS_PER_WORD;
  return APInt(numBits, {value.integer, numWords});
}

unsigned SymbolicValue::getIntegerValueBitWidth() const {
  assert(getKind() == Integer);
  assert (representationKind == RK_IntegerInline ||
          representationKind == RK_Integer);
  return auxInfo.integerBitwidth;
}

//===----------------------------------------------------------------------===//
// Strings
//===----------------------------------------------------------------------===//

// Returns a SymbolicValue representing a UTF-8 encoded string.
SymbolicValue SymbolicValue::getString(StringRef string,
                                       SymbolicValueAllocator &allocator) {
  // TODO: Could have an inline representation for strings if there was demand,
  // just store a char[8] as the storage.

  auto *resultPtr = allocator.allocate<char>(string.size());
  std::uninitialized_copy(string.begin(), string.end(), resultPtr);

  SymbolicValue result;
  result.representationKind = RK_String;
  result.value.string = resultPtr;
  result.auxInfo.stringNumBytes = string.size();
  return result;
}

// Returns the UTF-8 encoded string underlying a SymbolicValue.
StringRef SymbolicValue::getStringValue() const {
  assert(getKind() == String);

  assert(representationKind == RK_String);
  return StringRef(value.string, auxInfo.stringNumBytes);
}

//===----------------------------------------------------------------------===//
// Aggregates
//===----------------------------------------------------------------------===//

namespace swift {

/// Representation of a constant aggregate namely a struct or a tuple.
struct AggregateSymbolicValue final
    : private llvm::TrailingObjects<AggregateSymbolicValue, SymbolicValue> {
  friend class llvm::TrailingObjects<AggregateSymbolicValue, SymbolicValue>;

  const Type aggregateType;

  const unsigned numElements;

  static AggregateSymbolicValue *create(ArrayRef<SymbolicValue> members,
                                        Type aggregateType,
                                        SymbolicValueAllocator &allocator) {
    auto byteSize =
        AggregateSymbolicValue::totalSizeToAlloc<SymbolicValue>(members.size());
    auto rawMem = allocator.allocate(byteSize, alignof(AggregateSymbolicValue));

    //  Placement initialize the object.
    auto *aggregate =
        ::new (rawMem) AggregateSymbolicValue(aggregateType, members.size());
    std::uninitialized_copy(members.begin(), members.end(),
                            aggregate->getTrailingObjects<SymbolicValue>());
    return aggregate;
  }

  /// Return the type of the aggregate.
  Type getAggregateType() const { return aggregateType; }

  /// Return the symbolic values of members.
  ArrayRef<SymbolicValue> getMemberValues() const {
    return {getTrailingObjects<SymbolicValue>(), numElements};
  }

  // This is used by the llvm::TrailingObjects base class.
  size_t numTrailingObjects(OverloadToken<SymbolicValue>) const {
    return numElements;
  }

private:
  AggregateSymbolicValue() = delete;
  AggregateSymbolicValue(const AggregateSymbolicValue &) = delete;
  AggregateSymbolicValue(Type aggregateType, unsigned numElements)
      : aggregateType(aggregateType), numElements(numElements) {}
};
} // namespace swift

SymbolicValue SymbolicValue::getAggregate(ArrayRef<SymbolicValue> members,
                                          Type aggregateType,
                                          SymbolicValueAllocator &allocator) {
  SymbolicValue result;
  result.representationKind = RK_Aggregate;
  result.value.aggregate =
      AggregateSymbolicValue::create(members, aggregateType, allocator);
  return result;
}

ArrayRef<SymbolicValue> SymbolicValue::getAggregateMembers() const {
  assert(getKind() == Aggregate);
  return value.aggregate->getMemberValues();
}

Type SymbolicValue::getAggregateType() const {
  assert(getKind() == Aggregate);
  return value.aggregate->getAggregateType();
}

//===----------------------------------------------------------------------===//
// Unknown
//===----------------------------------------------------------------------===//

namespace swift {
/// When the value is Unknown, this contains information about the unfoldable
/// part of the computation.
struct alignas(SourceLoc) UnknownSymbolicValue final
    : private llvm::TrailingObjects<UnknownSymbolicValue, SourceLoc> {
  friend class llvm::TrailingObjects<UnknownSymbolicValue, SourceLoc>;

  /// The value that was unfoldable.
  SILNode *node;

  /// A more explanatory reason for the value being unknown.
  UnknownReason reason;

  /// The number of elements in the call stack.
  unsigned callStackSize;

  static UnknownSymbolicValue *create(SILNode *node, UnknownReason reason,
                                      ArrayRef<SourceLoc> elements,
                                      SymbolicValueAllocator &allocator) {
    auto byteSize =
        UnknownSymbolicValue::totalSizeToAlloc<SourceLoc>(elements.size());
    auto *rawMem = allocator.allocate(byteSize, alignof(UnknownSymbolicValue));

    // Placement-new the value inside the memory we just allocated.
    auto value = ::new (rawMem) UnknownSymbolicValue(
        node, reason, static_cast<unsigned>(elements.size()));
    std::uninitialized_copy(elements.begin(), elements.end(),
                            value->getTrailingObjects<SourceLoc>());
    return value;
  }

  ArrayRef<SourceLoc> getCallStack() const {
    return {getTrailingObjects<SourceLoc>(), callStackSize};
  }

  // This is used by the llvm::TrailingObjects base class.
  size_t numTrailingObjects(OverloadToken<SourceLoc>) const {
    return callStackSize;
  }

private:
  UnknownSymbolicValue() = delete;
  UnknownSymbolicValue(const UnknownSymbolicValue &) = delete;
  UnknownSymbolicValue(SILNode *node, UnknownReason reason,
                       unsigned callStackSize)
      : node(node), reason(reason), callStackSize(callStackSize) {}
};
} // namespace swift

SymbolicValue SymbolicValue::getUnknown(SILNode *node, UnknownReason reason,
                                        llvm::ArrayRef<SourceLoc> callStack,
                                        SymbolicValueAllocator &allocator) {
  assert(node && "node must be present");
  SymbolicValue result;
  result.representationKind = RK_Unknown;
  result.value.unknown =
      UnknownSymbolicValue::create(node, reason, callStack, allocator);
  return result;
}

ArrayRef<SourceLoc> SymbolicValue::getUnknownCallStack() const {
  assert(getKind() == Unknown);
  return value.unknown->getCallStack();
}

SILNode *SymbolicValue::getUnknownNode() const {
  assert(getKind() == Unknown);
  return value.unknown->node;
}

UnknownReason SymbolicValue::getUnknownReason() const {
  assert(getKind() == Unknown);
  return value.unknown->reason;
}

//===----------------------------------------------------------------------===//
// Enums
//===----------------------------------------------------------------------===//

namespace swift {

/// This is the representation of a constant enum value with payload.
struct EnumWithPayloadSymbolicValue final {
  /// The enum case.
  EnumElementDecl *enumDecl;
  SymbolicValue payload;

  EnumWithPayloadSymbolicValue(EnumElementDecl *decl, SymbolicValue payload)
      : enumDecl(decl), payload(payload) {}

private:
  EnumWithPayloadSymbolicValue() = delete;
  EnumWithPayloadSymbolicValue(const EnumWithPayloadSymbolicValue &) = delete;
};
} // end namespace swift

/// This returns a constant Symbolic value for the enum case in `decl` with a
/// payload.
SymbolicValue
SymbolicValue::getEnumWithPayload(EnumElementDecl *decl, SymbolicValue payload,
                                  SymbolicValueAllocator &allocator) {
  assert(decl && payload.isConstant());
  auto rawMem = allocator.allocate(sizeof(EnumWithPayloadSymbolicValue),
                                   alignof(EnumWithPayloadSymbolicValue));
  auto enumVal = ::new (rawMem) EnumWithPayloadSymbolicValue(decl, payload);

  SymbolicValue result;
  result.representationKind = RK_EnumWithPayload;
  result.value.enumValWithPayload = enumVal;
  return result;
}

EnumElementDecl *SymbolicValue::getEnumValue() const {
  if (representationKind == RK_Enum)
    return value.enumVal;

  assert(representationKind == RK_EnumWithPayload);
  return value.enumValWithPayload->enumDecl;
}

SymbolicValue SymbolicValue::getEnumPayloadValue() const {
  assert(representationKind == RK_EnumWithPayload);
  return value.enumValWithPayload->payload;
}

//===----------------------------------------------------------------------===//
// Addresses
//===----------------------------------------------------------------------===//

namespace swift {

/// This is the representation of a derived address.  A derived address refers
/// to a memory object along with an access path that drills into it.
struct DerivedAddressValue final
    : private llvm::TrailingObjects<DerivedAddressValue, unsigned> {
  friend class llvm::TrailingObjects<DerivedAddressValue, unsigned>;

  SymbolicValueMemoryObject *memoryObject;

  /// This is the number of indices in the derived address.
  const unsigned numElements;

  static DerivedAddressValue *create(SymbolicValueMemoryObject *memoryObject,
                                     ArrayRef<unsigned> elements,
                                     SymbolicValueAllocator &allocator) {
    auto byteSize =
        DerivedAddressValue::totalSizeToAlloc<unsigned>(elements.size());
    auto *rawMem = allocator.allocate(byteSize, alignof(DerivedAddressValue));

    //  Placement initialize the object.
    auto dav =
        ::new (rawMem) DerivedAddressValue(memoryObject, elements.size());
    std::uninitialized_copy(elements.begin(), elements.end(),
                            dav->getTrailingObjects<unsigned>());
    return dav;
  }

  /// Return the access path for this derived address, which is an array of
  /// indices drilling into the memory object.
  ArrayRef<unsigned> getElements() const {
    return {getTrailingObjects<unsigned>(), numElements};
  }

  // This is used by the llvm::TrailingObjects base class.
  size_t numTrailingObjects(OverloadToken<unsigned>) const {
    return numElements;
  }

private:
  DerivedAddressValue() = delete;
  DerivedAddressValue(const DerivedAddressValue &) = delete;
  DerivedAddressValue(SymbolicValueMemoryObject *memoryObject,
                      unsigned numElements)
      : memoryObject(memoryObject), numElements(numElements) {}
};
} // end namespace swift

/// Return a symbolic value that represents the address of a memory object
/// indexed by a path.
SymbolicValue SymbolicValue::getAddress(SymbolicValueMemoryObject *memoryObject,
                                        ArrayRef<unsigned> indices,
                                        SymbolicValueAllocator &allocator) {
  if (indices.empty())
    return getAddress(memoryObject);

  auto dav = DerivedAddressValue::create(memoryObject, indices, allocator);
  SymbolicValue result;
  result.representationKind = RK_DerivedAddress;
  result.value.derivedAddress = dav;
  return result;
}

/// Return the memory object of this reference along with any access path
/// indices involved.
SymbolicValueMemoryObject *
SymbolicValue::getAddressValue(SmallVectorImpl<unsigned> &accessPath) const {
  assert(getKind() == Address);

  accessPath.clear();
  if (representationKind == RK_DirectAddress)
    return value.directAddress;
  assert(representationKind == RK_DerivedAddress);

  auto *dav = value.derivedAddress;

  // The first entry is the object ID, the rest are indices in the accessPath.
  accessPath.assign(dav->getElements().begin(), dav->getElements().end());
  return dav->memoryObject;
}

/// Return just the memory object for an address value.
SymbolicValueMemoryObject *SymbolicValue::getAddressValueMemoryObject() const {
  if (representationKind == RK_DirectAddress)
    return value.directAddress;
  assert(representationKind == RK_DerivedAddress);
  return value.derivedAddress->memoryObject;
}

//===----------------------------------------------------------------------===//
// Arrays
//===----------------------------------------------------------------------===//

namespace swift {

/// Representation of the internal storage of an array. This is a container for
/// a sequence of symbolic values corresponding to the elements of an array.
struct SymbolicArrayStorage final
    : private llvm::TrailingObjects<SymbolicArrayStorage, SymbolicValue> {
  friend class llvm::TrailingObjects<SymbolicArrayStorage, SymbolicValue>;

  const CanType elementType;

  const unsigned numElements;

  static SymbolicArrayStorage *create(ArrayRef<SymbolicValue> elements,
                                      CanType elementType,
                                      SymbolicValueAllocator &allocator) {
    auto byteSize =
        SymbolicArrayStorage::totalSizeToAlloc<SymbolicValue>(elements.size());
    auto rawMem = allocator.allocate(byteSize, alignof(SymbolicArrayStorage));

    //  Placement initialize the object.
    auto *storage =
        ::new (rawMem) SymbolicArrayStorage(elementType, elements.size());
    std::uninitialized_copy(elements.begin(), elements.end(),
                            storage->getTrailingObjects<SymbolicValue>());
    return storage;
  }

  /// Return the stored elements.
  ArrayRef<SymbolicValue> getElements() const {
    return {getTrailingObjects<SymbolicValue>(), numElements};
  }

  // This is used by the llvm::TrailingObjects base class.
  size_t numTrailingObjects(OverloadToken<SymbolicValue>) const {
    return numElements;
  }

private:
  SymbolicArrayStorage() = delete;
  SymbolicArrayStorage(const SymbolicArrayStorage &) = delete;
  SymbolicArrayStorage(CanType elementType, unsigned numElements)
      : elementType(elementType), numElements(numElements) {}
};
} // namespace swift
// end namespace swift

SymbolicValue
SymbolicValue::getSymbolicArrayStorage(ArrayRef<SymbolicValue> elements,
                                       CanType elementType,
                                       SymbolicValueAllocator &allocator) {
  // TODO: Could compress the empty array representation if there were a reason
  // to.
  auto *arrayStorage =
      SymbolicArrayStorage::create(elements, elementType, allocator);
  SymbolicValue result;
  result.representationKind = RK_ArrayStorage;
  result.value.arrayStorage = arrayStorage;
  return result;
}

ArrayRef<SymbolicValue>
SymbolicValue::getStoredElements(CanType &elementType) const {
  assert(getKind() == ArrayStorage);
  elementType = value.arrayStorage->elementType;
  return value.arrayStorage->getElements();
}

SymbolicValue SymbolicValue::getArray(Type arrayType,
                                      SymbolicValue arrayStorage,
                                      SymbolicValueAllocator &allocator) {
  assert(arrayStorage.getKind() == ArrayStorage);
  SymbolicValue result;
  result.representationKind = RK_Array;
  result.value.array =
      SymbolicValueMemoryObject::create(arrayType, arrayStorage, allocator);
  return result;
}

SymbolicValue
SymbolicValue::getAddressOfArrayElement(SymbolicValueAllocator &allocator,
                                        unsigned index) const {
  assert(getKind() == Array);
  return SymbolicValue::getAddress(value.array, {index}, allocator);
}

SymbolicValue SymbolicValue::getStorageOfArray() const {
  assert(getKind() == Array);
  return value.array->getValue();
}

Type SymbolicValue::getArrayType() const {
  assert(getKind() == Array);
  return value.array->getType();
}

//===----------------------------------------------------------------------===//
// Symbolic Closure
//===----------------------------------------------------------------------===//

SymbolicValue SymbolicValue::makeClosure(SILFunction *target,
                                         ArrayRef<SymbolicClosureArgument> args,
                                         SubstitutionMap substMap,
                                         SingleValueInstruction *closureInst,
                                         SymbolicValueAllocator &allocator) {
  auto clo =
      SymbolicClosure::create(target, args, substMap, closureInst, allocator);
  SymbolicValue result;
  result.representationKind = RK_Closure;
  result.value.closure = clo;
  return result;
}

SymbolicClosure *SymbolicClosure::create(SILFunction *target,
                                         ArrayRef<SymbolicClosureArgument> args,
                                         SubstitutionMap substMap,
                                         SingleValueInstruction *closureInst,
                                         SymbolicValueAllocator &allocator) {
  // Determine whether there are captured arguments without a symbolic value.
  // Consider indirectly captured arguments as well, which can happen with
  // @in_guaranteed convention for captures.
  bool hasNonConstantCapture = false;
  for (SymbolicClosureArgument closureArg : args) {
    if (!closureArg.second) {
      hasNonConstantCapture = true;
      break;
    }
    SymbolicValue closureValue = closureArg.second.value();
    // Is capture non-constant?
    if (!closureValue.isConstant()) {
      hasNonConstantCapture = true;
      break;
    }
    // Is the indirect capture non-constant?
    if (closureValue.getKind() == SymbolicValue::Address &&
        !closureValue.getAddressValueMemoryObject()->getValue().isConstant()) {
      hasNonConstantCapture = true;
      break;
    }
  }

  auto byteSizeOfArgs =
      SymbolicClosure::totalSizeToAlloc<SymbolicClosureArgument>(args.size());
  auto rawMem = allocator.allocate(byteSizeOfArgs, alignof(SymbolicClosure));
  //  Placement initialize the object.
  auto closure = ::new (rawMem) SymbolicClosure(
      target, args.size(), substMap, closureInst, hasNonConstantCapture);
  std::uninitialized_copy(
      args.begin(), args.end(),
      closure->getTrailingObjects<SymbolicClosureArgument>());
  return closure;
}

//===----------------------------------------------------------------------===//
// Higher level code
//===----------------------------------------------------------------------===//

/// The SIL location for operations we process are usually deep in the bowels
/// of inlined code from opaque libraries, which are all implementation details
/// to the user.  As such, walk the inlining location of the specified node to
/// return the first location *outside* opaque libraries.
static SILDebugLocation skipInternalLocations(SILDebugLocation loc) {
  auto ds = loc.getScope();

  if (!ds || loc.getLocation().getSourceLoc().isValid())
    return loc;

  // Zip through inlined call site information that came from the
  // implementation guts of the library.  We want to report the message inside
  // the user's code, not in the guts we inlined through.
  for (; auto ics = ds->InlinedCallSite; ds = ics) {
    // If we found a valid inlined-into location, then we are good.
    if (ds->Loc.getSourceLoc().isValid())
      return SILDebugLocation(ds->Loc, ds);
    if (SILFunction *F = ds->getInlinedFunction()) {
      if (F->getLocation().getSourceLoc().isValid())
        break;
    }
  }

  if (ds->Loc.getSourceLoc().isValid())
    return SILDebugLocation(ds->Loc, ds);

  return loc;
}

/// Dig through single element aggregates, return the ultimate thing inside of
/// it.  This is useful when dealing with integers and floats, because they
/// are often wrapped in single-element struct wrappers.
SymbolicValue SymbolicValue::lookThroughSingleElementAggregates() const {
  auto result = *this;
  while (1) {
    if (result.getKind() != Aggregate)
      return result;
    auto elts = result.getAggregateMembers();
    if (elts.size() != 1)
      return result;
    result = elts[0];
  }
}

bool SymbolicValue::isUnknownDueToUnevaluatedInstructions() {
  auto unknownKind = getUnknownReason().getKind();
  return (unknownKind == UnknownReason::ReturnedByUnevaluatedInstruction ||
          unknownKind == UnknownReason::MutatedByUnevaluatedInstruction);
}

static void getWitnessMethodName(WitnessMethodInst *witnessMethodInst,
                                 SmallVectorImpl<char> &methodName) {
  assert(witnessMethodInst);
  SILDeclRef witnessMember = witnessMethodInst->getMember();
  if (witnessMember.hasDecl()) {
    witnessMember.getDecl()->getName().getString(methodName);
  }
}

/// A helper function to pretty print function names in diagnostics.
static std::string demangleSymbolNameForDiagnostics(StringRef name) {
  return Demangle::demangleSymbolAsString(
      name, Demangle::DemangleOptions::SimplifiedUIDemangleOptions());
}

/// Given that this is an 'Unknown' value, emit diagnostic notes providing
/// context about what the problem is. Specifically, point to interesting
/// source locations and function calls in the call stack.
void SymbolicValue::emitUnknownDiagnosticNotes(SILLocation fallbackLoc) {
  auto unknownNode = getUnknownNode();
  auto unknownReason = getUnknownReason();
  auto errorCallStack = getUnknownCallStack();

  ASTContext &ctx = unknownNode->getModule()->getASTContext();

  // Extract the location of the instruction/construct that triggered the error
  // during interpretation, if available. If the instruction is internal to
  // stdlib and has an invalid location, find the innermost call that has a
  // valid location.
  SourceLoc triggerLoc;
  bool triggerLocSkipsInternalLocs = false;
  if (auto *badInst = dyn_cast<SILInstruction>(unknownNode)) {
    SILDebugLocation debugLoc = badInst->getDebugLocation();
    SourceLoc initialSourceLoc = debugLoc.getLocation().getSourceLoc();
    if (initialSourceLoc.isValid()) {
      triggerLoc = initialSourceLoc;
    } else {
      triggerLocSkipsInternalLocs = true;
      triggerLoc = skipInternalLocations(debugLoc).getLocation().getSourceLoc();
    }
  }

  // Determine the top-level expression where the error happens and use it as
  // the location to emit diagnostics. Specifically, if the call-stack is
  // non-empty, use the first call in the sequence as the error location as the
  // error happens only in the context of this call. Use the fallback loc if
  // the faulty top-level expression location cannot be found.
  auto diagLoc =
      errorCallStack.empty()
          ? (triggerLoc.isValid() ? triggerLoc : fallbackLoc.getSourceLoc())
          : errorCallStack.front();
  if (diagLoc.isInvalid()) {
    return;
  }

  // Emit a note at the trigger location as well if it is different from the
  // top-level expression.
  bool emitTriggerLocInDiag =
      triggerLoc.isValid() ? diagLoc != triggerLoc : false;

  switch (unknownReason.getKind()) {
  case UnknownReason::Default:
    diagnose(ctx, diagLoc, diag::constexpr_unknown_reason_default);
    if (emitTriggerLocInDiag)
      diagnose(ctx, triggerLoc, diag::constexpr_unevaluable_operation,
               triggerLocSkipsInternalLocs);
    return;
  case UnknownReason::TooManyInstructions:
    diagnose(ctx, diagLoc, diag::constexpr_too_many_instructions,
             ConstExprLimit);
    if (emitTriggerLocInDiag)
      diagnose(ctx, triggerLoc, diag::constexpr_limit_exceeding_instruction,
               triggerLocSkipsInternalLocs);
    return;
  case UnknownReason::Loop:
    diagnose(ctx, diagLoc, diag::constexpr_loop_found_note);
    if (emitTriggerLocInDiag)
      diagnose(ctx, triggerLoc, diag::constexpr_loop_instruction,
               triggerLocSkipsInternalLocs);
    return;
  case UnknownReason::Overflow:
    diagnose(ctx, diagLoc, diag::constexpr_overflow);
    if (emitTriggerLocInDiag)
      diagnose(ctx, triggerLoc, diag::constexpr_overflow_operation,
               triggerLocSkipsInternalLocs);
    return;
  case UnknownReason::Trap: {
    diagnose(ctx, diagLoc, diag::constexpr_trap,
             unknownReason.getTrapMessage());
    if (emitTriggerLocInDiag)
      diagnose(ctx, triggerLoc, diag::constexpr_trap_operation,
               triggerLocSkipsInternalLocs);
    return;
  }
  case UnknownReason::InvalidOperandValue:
    diagnose(ctx, diagLoc, diag::constexpr_invalid_operand_seen);
    if (emitTriggerLocInDiag)
      diagnose(ctx, triggerLoc, diag::constexpr_operand_invalid_here,
               triggerLocSkipsInternalLocs);
    return;
  case UnknownReason::NotTopLevelConstant: {
    // For top-level errors, trigger loc is better than diagLoc.
    auto loc = emitTriggerLocInDiag ? triggerLoc : diagLoc;
    diagnose(ctx, loc, diag::constexpr_value_unknown_at_top_level);
    return;
  }
  case UnknownReason::MutipleTopLevelWriters: {
    // For top-level errors, trigger loc is better than diagLoc.
    auto loc = emitTriggerLocInDiag ? triggerLoc : diagLoc;
    diagnose(ctx, loc, diag::constexpr_multiple_writers_found_at_top_level);
    return;
  }
  case UnknownReason::UnsupportedInstruction: {
    // Get the name of the unsupported instruction.
    auto *unsupportedInst = dyn_cast<SILInstruction>(unknownNode);
    assert(unsupportedInst);
    SmallString<4> instName(getSILInstructionName(unsupportedInst->getKind()));
    if (auto *builtinInst = dyn_cast<BuiltinInst>(unsupportedInst)) {
      instName.append(" ");
      instName.append(builtinInst->getName().str());
    }

    diagnose(ctx, diagLoc, diag::constexpr_unsupported_instruction_found,
             instName);
    if (emitTriggerLocInDiag)
      diagnose(ctx, triggerLoc,
               diag::constexpr_unsupported_instruction_found_here,
               triggerLocSkipsInternalLocs);
    return;
  }
  case UnknownReason::CalleeImplementationUnknown: {
    SILFunction *callee = unknownReason.getCalleeWithoutImplmentation();
    std::string demangledCalleeName =
        demangleSymbolNameForDiagnostics(callee->getName());
    diagnose(ctx, diagLoc, diag::constexpr_found_callee_with_no_body,
             demangledCalleeName);
    if (emitTriggerLocInDiag)
      diagnose(ctx, triggerLoc, diag::constexpr_callee_with_no_body,
               triggerLocSkipsInternalLocs);
    return;
  }
  case UnknownReason::CallArgumentUnknown: {
    unsigned argNumber = unknownReason.getArgumentIndex() + 1;
    ApplyInst *call = dyn_cast<ApplyInst>(unknownNode);
    assert(call);
    SILFunction *callee = call->getCalleeFunction();
    assert(callee);
    std::string demangledCalleeName =
        demangleSymbolNameForDiagnostics(callee->getName());
    diagnose(ctx, diagLoc, diag::constexpr_found_call_with_unknown_arg,
             demangledCalleeName,
             (Twine(argNumber) + llvm::getOrdinalSuffix(argNumber)).str());
    if (emitTriggerLocInDiag)
      diagnose(ctx, triggerLoc, diag::constexpr_call_with_unknown_arg,
               triggerLocSkipsInternalLocs);
    return;
  }
  case UnknownReason::UntrackedSILValue:
    diagnose(ctx, diagLoc, diag::constexpr_untracked_sil_value_use_found);
    if (emitTriggerLocInDiag)
      diagnose(ctx, triggerLoc, diag::constexpr_untracked_sil_value_used_here,
               triggerLocSkipsInternalLocs);
    return;
  case UnknownReason::UnknownCastResult: {
    diagnose(ctx, diagLoc, diag::constexpr_unevaluable_cast_found);
    if (emitTriggerLocInDiag)
      diagnose(ctx, triggerLoc, diag::constexpr_unevaluable_cast_used_here,
               triggerLocSkipsInternalLocs);
    return;
  }
  case UnknownReason::UnknownWitnessMethodConformance: {
    SmallString<8> witnessMethodName;
    getWitnessMethodName(dyn_cast<WitnessMethodInst>(unknownNode),
                         witnessMethodName);
    diagnose(ctx, diagLoc, diag::constexpr_unresolvable_witness_call,
             witnessMethodName);
    if (emitTriggerLocInDiag)
      diagnose(ctx, triggerLoc,
               diag::constexpr_witness_call_with_no_conformance,
               triggerLocSkipsInternalLocs);
    return;
  }
  case UnknownReason::NoWitnesTableEntry: {
    SmallString<8> witnessMethodName;
    getWitnessMethodName(dyn_cast<WitnessMethodInst>(unknownNode),
                         witnessMethodName);

    diagnose(ctx, diagLoc, diag::constexpr_unresolvable_witness_call,
             witnessMethodName);
    if (emitTriggerLocInDiag)
      diagnose(ctx, triggerLoc, diag::constexpr_no_witness_table_entry,
               triggerLocSkipsInternalLocs);
    return;
  }
  case UnknownReason::ReturnedByUnevaluatedInstruction:
    diagnose(ctx, diagLoc, diag::constexpr_returned_by_unevaluated_instruction);
    break;
  case UnknownReason::MutatedByUnevaluatedInstruction:
    diagnose(ctx, diagLoc, diag::constexpr_mutated_by_unevaluated_instruction);
    break;
  }
  // TODO: print the call-stack in a controlled way if needed.
}

/// Returns the element of `aggregate` specified by the access path.
///
/// This is a helper for `SymbolicValueMemoryObject::getIndexedElement`. See
/// there for more detailed documentation.
static SymbolicValue getIndexedElement(SymbolicValue aggregate,
                                       ArrayRef<unsigned> accessPath,
                                       Type type) {
  // We're done if we've run out of access path.
  if (accessPath.empty())
    return aggregate;

  // Everything inside uninit memory is uninit memory.
  if (aggregate.getKind() == SymbolicValue::UninitMemory)
    return SymbolicValue::getUninitMemory();

  assert((aggregate.getKind() == SymbolicValue::Aggregate ||
          aggregate.getKind() == SymbolicValue::ArrayStorage) &&
         "the accessPath is invalid for this type");

  unsigned elementNo = accessPath.front();

  SymbolicValue elt;
  Type eltType;

  if (aggregate.getKind() == SymbolicValue::ArrayStorage) {
    CanType arrayEltTy;
    elt = aggregate.getStoredElements(arrayEltTy)[elementNo];
    eltType = arrayEltTy;
  } else {
    elt = aggregate.getAggregateMembers()[elementNo];
    if (auto *decl = type->getStructOrBoundGenericStruct()) {
      eltType = decl->getStoredProperties()[elementNo]->getTypeInContext();
    } else if (auto tuple = type->getAs<TupleType>()) {
      assert(elementNo < tuple->getNumElements() && "invalid index");
      eltType = tuple->getElement(elementNo).getType();
    } else {
      llvm_unreachable("the accessPath is invalid for this type");
    }
  }

  return getIndexedElement(elt, accessPath.drop_front(), eltType);
}

/// Given that this memory object contains an aggregate value like
/// {{1, 2}, 3}, and given an access path like [0,1], return the indexed
/// element, e.g. "2" in this case.
///
/// Returns uninit memory if the access path points at or into uninit memory.
///
/// Precondition: The access path must be valid for this memory object's type.
SymbolicValue
SymbolicValueMemoryObject::getIndexedElement(ArrayRef<unsigned> accessPath) {
  return ::getIndexedElement(value, accessPath, type);
}

/// Returns `aggregate` with the element specified by the access path set to
/// `newElement`.
///
/// This is a helper for `SymbolicValueMemoryObject::setIndexedElement`. See
/// there for more detailed documentation.
static SymbolicValue setIndexedElement(SymbolicValue aggregate,
                                       ArrayRef<unsigned> accessPath,
                                       SymbolicValue newElement, Type type,
                                       SymbolicValueAllocator &allocator) {
  // We're done if we've run out of access path.
  if (accessPath.empty())
    return newElement;

  // Callers are required to ensure unknowns are not passed. However,
  // the recurisve call can pass an unknown as an aggregate.
  if (aggregate.getKind() == SymbolicValue::Unknown)
    return aggregate;

  // If we have an uninit memory, then scalarize it into an aggregate to
  // continue.  This happens when memory objects are initialized piecewise.
  if (aggregate.getKind() == SymbolicValue::UninitMemory) {
    unsigned numMembers;
    // We need to have either a struct or a tuple type.
    if (auto *decl = type->getStructOrBoundGenericStruct()) {
      numMembers = decl->getStoredProperties().size();
    } else if (auto tuple = type->getAs<TupleType>()) {
      numMembers = tuple->getNumElements();
    } else {
      llvm_unreachable("the accessPath is invalid for this type");
    }

    SmallVector<SymbolicValue, 4> newElts(numMembers,
                                          SymbolicValue::getUninitMemory());
    aggregate = SymbolicValue::getAggregate(newElts, type, allocator);
  }

  assert((aggregate.getKind() == SymbolicValue::Aggregate ||
          aggregate.getKind() == SymbolicValue::ArrayStorage) &&
         "the accessPath is invalid for this type");

  unsigned elementNo = accessPath.front();

  ArrayRef<SymbolicValue> oldElts;
  Type eltType;

  if (aggregate.getKind() == SymbolicValue::ArrayStorage) {
    CanType arrayEltTy;
    oldElts = aggregate.getStoredElements(arrayEltTy);
    eltType = arrayEltTy;
  } else {
    oldElts = aggregate.getAggregateMembers();
    if (auto *decl = type->getStructOrBoundGenericStruct()) {
      eltType = decl->getStoredProperties()[elementNo]->getTypeInContext();
    } else if (auto tuple = type->getAs<TupleType>()) {
      assert(elementNo < tuple->getNumElements() && "invalid index");
      eltType = tuple->getElement(elementNo).getType();
    } else {
      llvm_unreachable("the accessPath is invalid for this type");
    }
  }

  // Update the indexed element of the aggregate.
  SmallVector<SymbolicValue, 4> newElts(oldElts.begin(), oldElts.end());
  newElts[elementNo] =
      setIndexedElement(newElts[elementNo], accessPath.drop_front(), newElement,
                        eltType, allocator);

  if (aggregate.getKind() == SymbolicValue::Aggregate) {
    Type aggregateType = aggregate.getAggregateType();
    assert(aggregateType->isEqual(type) &&
           "input type does not match the type of the aggregate");
    return SymbolicValue::getAggregate(newElts, aggregateType, allocator);
  }

  return aggregate = SymbolicValue::getSymbolicArrayStorage(
             newElts, eltType->getCanonicalType(), allocator);
  return aggregate;
}

/// Given that this memory object contains an aggregate value like
/// {{1, 2}, 3}, given an access path like [0,1], and given a new element like
/// "4", set the indexed element to the specified scalar, producing {{1, 4},
/// 3} in this case.
///
/// Precondition: The access path must be valid for this memory object's type.
void SymbolicValueMemoryObject::setIndexedElement(
    ArrayRef<unsigned> accessPath, SymbolicValue newElement,
    SymbolicValueAllocator &allocator) {
  value = ::setIndexedElement(value, accessPath, newElement, type, allocator);
}
