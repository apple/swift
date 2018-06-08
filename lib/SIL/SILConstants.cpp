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

// SWIFT_ENABLE_TENSORFLOW

#include "swift/SIL/SILConstants.h"
#include "swift/SIL/SILBuilder.h"
#include "swift/Demangling/Demangle.h"
#include "swift/AST/DiagnosticsSIL.h"
#include "llvm/Support/TrailingObjects.h"
using namespace swift;

template<typename...T, typename...U>
static InFlightDiagnostic
diagnose(ASTContext &Context, SourceLoc loc, Diag<T...> diag, U &&...args) {
  return Context.Diags.diagnose(loc, diag, std::forward<U>(args)...);
}

//===----------------------------------------------------------------------===//
// SymbolicValue implementation
//===----------------------------------------------------------------------===//

void SymbolicValue::print(llvm::raw_ostream &os, unsigned indent) const {
  os.indent(indent);
  switch (representationKind) {
  case RK_UninitMemory: os << "uninit\n"; return;
  case RK_Unknown: {
    std::pair<SILNode *, UnknownReason> unknown = getUnknownValue();
    switch (unknown.second) {
    case UnknownReason::Default: os << "unknown: "; break;
    case UnknownReason::TooManyInstructions: os << "unknown(toobig): "; break;
    }
    unknown.first->dump();
    return;
  }
  case RK_Metatype:
    os << "metatype: ";
    getMetatypeValue()->print(os);
    os << "\n";
    return;
  case RK_Function:
    os << "fn: " << getFunctionValue()->getName() << ": ";
    os << Demangle::demangleSymbolAsString(getFunctionValue()->getName());
    os << "\n";
    return;
  case RK_Inst:
    os << "inst: ";
    value.inst->dump();
    return;
  case RK_Integer:
    os << "int: " << getIntegerValue() << "\n";
    return;
  case RK_Float:
    os << "float: ";
    getFloatValue().print(os);
    os << "\n";
    return;
  case RK_Address: {
    os << "address indices = [";
    interleave(getAddressIndices(), [&](unsigned idx) { os << idx; },
               [&]() { os << ", "; });
    os << "]:  " << getAddressBase();
    return;
  }
  case RK_Aggregate: {
    ArrayRef<SymbolicValue> elements = getAggregateValue();
    os << "agg: " << elements.size() << " element" << "s"[elements.size() == 1]
       << " [\n";
    for (auto elt : elements)
      elt.print(os, indent+2);
    os.indent(indent) << "]\n";
    return;
  }
  }
}

void SymbolicValue::dump() const {
  print(llvm::errs());
}

/// For constant values, return the classification of this value.  We have
/// multiple forms for efficiency, but provide a simpler interface to clients.
SymbolicValue::Kind SymbolicValue::getKind() const {
  switch (representationKind) {
  case RK_UninitMemory: return UninitMemory;
  case RK_Unknown:      return Unknown;
  case RK_Metatype:     return Metatype;
  case RK_Function:     return Function;
  case RK_Address:      return Address;
  case RK_Aggregate:    return Aggregate;
  case RK_Integer:      return Integer;
  case RK_Float:        return Float;
  case RK_Inst:
    auto *inst = value.inst;
    if (isa<IntegerLiteralInst>(inst))
      return Integer;
    if (isa<FloatLiteralInst>(inst))
      return Float;
    assert(isa<StringLiteralInst>(inst) && "Unknown ConstantInst kind");
    return String;
  }
}


//===----------------------------------------------------------------------===//
// Integers
//===----------------------------------------------------------------------===//

namespace swift {
/// This is a representation of an integer value, stored as a trailing array
/// of words.  Elements of this value are bump pointer allocated.
struct alignas(uint64_t) APIntSymbolicValue final
  : private llvm::TrailingObjects<APIntSymbolicValue, uint64_t> {
    friend class llvm::TrailingObjects<APIntSymbolicValue, uint64_t>;

  /// The number of words in the trailing array and # bits of the value.
  const unsigned numWords, numBits;

  static APIntSymbolicValue *create(unsigned numBits,
                                    ArrayRef<uint64_t> elements,
                                    llvm::BumpPtrAllocator &allocator) {
    auto byteSize =
      APIntSymbolicValue::totalSizeToAlloc<uint64_t>(elements.size());
    auto rawMem = allocator.Allocate(byteSize, alignof(APIntSymbolicValue));

    //  Placement initialize the APIntSymbolicValue.
    auto ilv = ::new (rawMem) APIntSymbolicValue(numBits, elements.size());
    std::uninitialized_copy(elements.begin(), elements.end(),
                            ilv->getTrailingObjects<uint64_t>());
    return ilv;
  }

  APInt getValue() const {
    return APInt(numBits, { getTrailingObjects<uint64_t>(), numWords });
  }

  // This is used by the llvm::TrailingObjects base class.
  size_t numTrailingObjects(OverloadToken<uint64_t>) const {
    return numWords;
  }
private:
  APIntSymbolicValue() = delete;
  APIntSymbolicValue(const APIntSymbolicValue &) = delete;
  APIntSymbolicValue(unsigned numBits, unsigned numWords)
    : numWords(numWords), numBits(numBits) {}
};
} // end namespace swift


SymbolicValue SymbolicValue::getInteger(const APInt &value,
                                        llvm::BumpPtrAllocator &allocator) {
  // TODO: Could store these inline in the union in the common case.
  auto intValue =
    APIntSymbolicValue::create(value.getBitWidth(),
                               { value.getRawData(), value.getNumWords()},
                               allocator);
  assert(intValue && "aggregate value must be present");
  SymbolicValue result;
  result.representationKind = RK_Integer;
  result.value.integer = intValue;
  return result;
}

APInt SymbolicValue::getIntegerValue() const {
  assert(getKind() == Integer);
  if (representationKind == RK_Integer)
    return value.integer->getValue();

  assert(representationKind == RK_Inst);
  // TODO: Will eventually support the bump-pointer allocated folded int value.
  return cast<IntegerLiteralInst>(value.inst)->getValue();
}

//===----------------------------------------------------------------------===//
// Floats
//===----------------------------------------------------------------------===//

namespace swift {
/// This is a representation of an integer value, stored as a trailing array
/// of words.  Elements of this value are bump pointer allocated.
struct alignas(uint64_t) APFloatSymbolicValue final
  : private llvm::TrailingObjects<APFloatSymbolicValue, uint64_t> {
    friend class llvm::TrailingObjects<APFloatSymbolicValue, uint64_t>;

  const llvm::fltSemantics &semantics;

  static APFloatSymbolicValue *create(const llvm::fltSemantics &semantics,
                                     ArrayRef<uint64_t> elements,
                                     llvm::BumpPtrAllocator &allocator) {
    assert((APFloat::getSizeInBits(semantics)+63)/64 == elements.size());

    auto byteSize =
      APFloatSymbolicValue::totalSizeToAlloc<uint64_t>(elements.size());
    auto rawMem = allocator.Allocate(byteSize, alignof(APFloatSymbolicValue));

    //  Placement initialize the APFloatSymbolicValue.
    auto ilv = ::new (rawMem) APFloatSymbolicValue(semantics);
    std::uninitialized_copy(elements.begin(), elements.end(),
                            ilv->getTrailingObjects<uint64_t>());
    return ilv;
  }

  APFloat getValue() const {
    auto val = APInt(APFloat::getSizeInBits(semantics),
                     { getTrailingObjects<uint64_t>(),
                       numTrailingObjects(OverloadToken<uint64_t>())
                     });
    return APFloat(semantics, val);
  }

  // This is used by the llvm::TrailingObjects base class.
  size_t numTrailingObjects(OverloadToken<uint64_t>) const {
    return (APFloat::getSizeInBits(semantics)+63)/64;
  }
private:
  APFloatSymbolicValue() = delete;
  APFloatSymbolicValue(const APFloatSymbolicValue &) = delete;
  APFloatSymbolicValue(const llvm::fltSemantics &semantics)
    : semantics(semantics) {}
};
} // end namespace swift


SymbolicValue SymbolicValue::getFloat(const APFloat &value,
                                      llvm::BumpPtrAllocator &allocator) {
  APInt val = value.bitcastToAPInt();

  // TODO: Could store these inline in the union in the common case.
  auto fpValue =
    APFloatSymbolicValue::create(value.getSemantics(),
                                 { val.getRawData(), val.getNumWords()},
                                 allocator);
  assert(fpValue && "aggregate value must be present");
  SymbolicValue result;
  result.representationKind = RK_Float;
  result.value.float_ = fpValue;
  return result;
}


APFloat SymbolicValue::getFloatValue() const {
  assert(getKind() == Float);

  if (representationKind == RK_Float)
    return value.float_->getValue();

  assert(representationKind == RK_Inst);
  return cast<FloatLiteralInst>(value.inst)->getValue();
}

//===----------------------------------------------------------------------===//
// Addresses
//===----------------------------------------------------------------------===//

namespace swift {
/// This is a representation of an address value, stored as a base pointer plus
/// trailing array of indices.  Elements of this value are bump pointer
/// allocated.
struct alignas(SILValue) AddressSymbolicValue final
  : private llvm::TrailingObjects<AddressSymbolicValue, unsigned> {
    friend class llvm::TrailingObjects<AddressSymbolicValue, unsigned>;

  /// The number of words in the trailing array and # bits of the value.
  const SILValue base;
  const unsigned numIndices;

  static AddressSymbolicValue *create(SILValue base, ArrayRef<unsigned> indices,
                                      llvm::BumpPtrAllocator &allocator) {
    auto byteSize =
      AddressSymbolicValue::totalSizeToAlloc<unsigned>(indices.size());
    auto rawMem = allocator.Allocate(byteSize, alignof(AddressSymbolicValue));

    //  Placement initialize the AddressSymbolicValue.
    auto alv = ::new (rawMem) AddressSymbolicValue(base, indices.size());
    std::uninitialized_copy(indices.begin(), indices.end(),
                            alv->getTrailingObjects<unsigned>());
    return alv;
  }

  ArrayRef<unsigned> getIndices() const {
    return { getTrailingObjects<unsigned>(), numIndices };
  }

  // This is used by the llvm::TrailingObjects base class.
  size_t numTrailingObjects(OverloadToken<unsigned>) const {
    return numIndices;
  }
private:
  AddressSymbolicValue() = delete;
  AddressSymbolicValue(const AddressSymbolicValue &) = delete;
  AddressSymbolicValue(SILValue base, unsigned numIndices)
    : base(base), numIndices(numIndices) {}
};
} // end namespace swift


SymbolicValue
SymbolicValue::getAddress(SILValue base, ArrayRef<unsigned> indices,
                          llvm::BumpPtrAllocator &allocator) {
  auto alv = AddressSymbolicValue::create(base, indices, allocator);
  assert(alv && "aggregate value must be present");
  SymbolicValue result;
  result.representationKind = RK_Address;
  result.value.address = alv;
  return result;
}

SILValue SymbolicValue::getAddressBase() const {
  assert(representationKind == RK_Address);
  return value.address->base;
}

ArrayRef<unsigned> SymbolicValue::getAddressIndices() const {
  assert(representationKind == RK_Address);
  return value.address->getIndices();
}


//===----------------------------------------------------------------------===//
// Aggregates
//===----------------------------------------------------------------------===//

namespace swift {

/// This is the representation of a constant aggregate value.  It maintains
/// the elements as a trailing array of SymbolicValue's.  Note that single
/// element structs do not use this (as a performance optimization to reduce
/// allocations).
struct alignas(SymbolicValue) AggregateSymbolicValue final
: private llvm::TrailingObjects<AggregateSymbolicValue, SymbolicValue> {
  friend class llvm::TrailingObjects<AggregateSymbolicValue, SymbolicValue>;

  /// This is the number of elements in the aggregate.
  const unsigned numElements;

  static AggregateSymbolicValue *create(ArrayRef<SymbolicValue> elements,
                                       llvm::BumpPtrAllocator &allocator) {
    auto byteSize =
      AggregateSymbolicValue::totalSizeToAlloc<SymbolicValue>(elements.size());
    auto rawMem = allocator.Allocate(byteSize, alignof(AggregateSymbolicValue));

    //  Placement initialize the AggregateSymbolicValue.
    auto alv = ::new (rawMem) AggregateSymbolicValue(elements.size());
    std::uninitialized_copy(elements.begin(), elements.end(),
                            alv->getTrailingObjects<SymbolicValue>());
    return alv;
  }

  /// Return the element constants for this aggregate constant.  These are
  /// known to all be constants.
  ArrayRef<SymbolicValue> getElements() const {
    return { getTrailingObjects<SymbolicValue>(), numElements };
  }

  // This is used by the llvm::TrailingObjects base class.
  size_t numTrailingObjects(OverloadToken<SymbolicValue>) const {
    return numElements;
  }
private:
  AggregateSymbolicValue() = delete;
  AggregateSymbolicValue(const AggregateSymbolicValue &) = delete;
  AggregateSymbolicValue(unsigned numElements) : numElements(numElements) {}
};
} // end namespace swift


/// This returns a constant Symbolic value with the specified elements in it.
/// This assumes that the elements lifetime has been managed for this.
SymbolicValue SymbolicValue::getAggregate(ArrayRef<SymbolicValue> elements,
                                          llvm::BumpPtrAllocator &allocator) {
  auto aggregate = AggregateSymbolicValue::create(elements, allocator);
  assert(aggregate && "aggregate value must be present");
  SymbolicValue result;
  result.representationKind = RK_Aggregate;
  result.value.aggregate = aggregate;
  return result;
}

ArrayRef<SymbolicValue> SymbolicValue::getAggregateValue() const {
  assert(getKind() == Aggregate);
  return value.aggregate->getElements();
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
  // implementation guts of the tensor library.  We want to report the
  // message inside the user's code, not in the guts we inlined through.
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

/// Given that this is an 'Unknown' value, emit diagnostic notes providing
/// context about what the problem is.
void SymbolicValue::emitUnknownDiagnosticNotes() {
  std::pair<SILNode *, UnknownReason> unknown = getUnknownValue();
  auto badInst = dyn_cast<SILInstruction>(unknown.first);
  if (!badInst) return;

  std::string error;
  switch (unknown.second) {
  case UnknownReason::Default:
    error = "could not fold operation";
    break;
  case UnknownReason::TooManyInstructions:
    // TODO: Should pop up a level of the stack trace.
    error = "expression is too large to evaluate at compile-time";
    break;
  }

  auto &module = badInst->getModule();

  auto loc = skipInternalLocations(badInst->getDebugLocation()).getLocation();
  if (loc.isNull()) return;

  diagnose(module.getASTContext(), loc.getSourceLoc(),
           diag::tf_op_misuse_note, error)
    .highlight(loc.getSourceRange());
}


