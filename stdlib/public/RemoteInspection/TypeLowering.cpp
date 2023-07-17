//===--- TypeLowering.cpp - Swift Type Lowering for Reflection ------------===//
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
// Implements logic for computing in-memory layouts from TypeRefs loaded from
// reflection metadata.
//
// This has to match up with layout algorithms used in IRGen and the runtime,
// and a bit of SIL type lowering to boot.
//
//===----------------------------------------------------------------------===//

#if SWIFT_ENABLE_REFLECTION

#include "llvm/Support/MathExtras.h"
#include "swift/ABI/Enum.h"
#include "swift/ABI/MetadataValues.h"
#include "swift/RemoteInspection/TypeLowering.h"
#include "swift/RemoteInspection/TypeRef.h"
#include "swift/RemoteInspection/TypeRefBuilder.h"
#include "swift/Basic/Unreachable.h"
#include <iostream>
#include <sstream>
#include <limits>

#ifdef DEBUG_TYPE_LOWERING
  #define DEBUG_LOG(expr) expr;
#else
  #define DEBUG_LOG(expr)
#endif

namespace swift {
namespace reflection {

void TypeInfo::dump() const {
  dump(std::cerr);
}

namespace {

class PrintTypeInfo {
  std::ostream &stream;
  unsigned Indent;

  std::ostream &indent(unsigned Amount) {
    for (unsigned i = 0; i < Amount; ++i)
      stream << " ";
    return stream;
  }

  std::ostream &printHeader(const std::string &name) {
    indent(Indent) << "(" << name;
    return stream;
  }

  std::ostream &printField(const std::string &name, const std::string &value) {
    if (!name.empty())
      stream << " " << name << "=" << value;
    else
      stream << " " << name;
    return stream;
  }

  void printRec(const TypeInfo &TI) {
    stream << "\n";

    Indent += 2;
    print(TI);
    Indent -= 2;
  }

  void printBasic(const TypeInfo &TI) {
    printField("size", std::to_string(TI.getSize()));
    printField("alignment", std::to_string(TI.getAlignment()));
    printField("stride", std::to_string(TI.getStride()));
    printField("num_extra_inhabitants", std::to_string(TI.getNumExtraInhabitants()));
    printField("bitwise_takable", TI.isBitwiseTakable() ? "1" : "0");
  }

  void printFields(const RecordTypeInfo &TI) {
    Indent += 2;
    for (auto Field : TI.getFields()) {
      stream << "\n";
      printHeader("field");
      if (!Field.Name.empty())
        printField("name", Field.Name);
      printField("offset", std::to_string(Field.Offset));
      printRec(Field.TI);
      stream << ")";
    }
    Indent -= 2;
  }

  void printCases(const EnumTypeInfo &TI) {
    Indent += 2;
    int Index = -1;
    for (auto Case : TI.getCases()) {
      Index += 1;
      stream << "\n";
      printHeader("case");
      if (!Case.Name.empty())
        printField("name", Case.Name);
      printField("index", std::to_string(Index));
      if (Case.TR) {
        printField("offset", std::to_string(Case.Offset));
        printRec(Case.TI);
      }
      stream << ")";
    }
    Indent -= 2;
  }

public:
  PrintTypeInfo(std::ostream &stream, unsigned Indent)
      : stream(stream), Indent(Indent) {}

  void print(const TypeInfo &TI) {
    switch (TI.getKind()) {
    case TypeInfoKind::Invalid:
      printHeader("invalid");
      stream << ")";
      return;

    case TypeInfoKind::Builtin:
      printHeader("builtin");
      printBasic(TI);
      stream << ")";
      return;

    case TypeInfoKind::Record: {
      auto &RecordTI = cast<RecordTypeInfo>(TI);
      switch (RecordTI.getRecordKind()) {
      case RecordKind::Invalid:
        printHeader("invalid");
        break;
      case RecordKind::Struct:
        printHeader("struct");
        break;
      case RecordKind::Tuple:
        printHeader("tuple");
        break;
      case RecordKind::ThickFunction:
        printHeader("thick_function");
        break;
      case RecordKind::OpaqueExistential:
        printHeader("opaque_existential");
        break;
      case RecordKind::ClassExistential:
        printHeader("class_existential");
        break;
      case RecordKind::ErrorExistential:
        printHeader("error_existential");
        break;
      case RecordKind::ExistentialMetatype:
        printHeader("existential_metatype");
        break;
      case RecordKind::ClassInstance:
        printHeader("class_instance");
        break;
      case RecordKind::ClosureContext:
        printHeader("closure_context");
        break;
      }
      printBasic(TI);
      printFields(RecordTI);
      stream << ")";
      return;
    }

    case TypeInfoKind::Enum: {
      auto &EnumTI = cast<EnumTypeInfo>(TI);
      switch (EnumTI.getEnumKind()) {
      case EnumKind::NoPayloadEnum:
        printHeader("no_payload_enum");
        break;
      case EnumKind::SinglePayloadEnum:
        printHeader("single_payload_enum");
        break;
      case EnumKind::MultiPayloadEnum:
        printHeader("multi_payload_enum");
        break;
      }
      printBasic(TI);
      printCases(EnumTI);
      stream << ")";
      return;
    }

    case TypeInfoKind::Reference: {
      printHeader("reference");
      auto &ReferenceTI = cast<ReferenceTypeInfo>(TI);
      switch (ReferenceTI.getReferenceKind()) {
      case ReferenceKind::Strong: printField("kind", "strong"); break;
#define REF_STORAGE(Name, name, ...) \
      case ReferenceKind::Name: printField("kind", #name); break;
#include "swift/AST/ReferenceStorage.def"
      }

      switch (ReferenceTI.getReferenceCounting()) {
      case ReferenceCounting::Native:
        printField("refcounting", "native");
        break;
      case ReferenceCounting::Unknown:
        printField("refcounting", "unknown");
        break;
      }

      stream << ")";
      return;
    }
    }

    swift_unreachable("Bad TypeInfo kind");
  }
};

} // end anonymous namespace

void TypeInfo::dump(std::ostream &stream, unsigned Indent) const {
  PrintTypeInfo(stream, Indent).print(*this);
  stream << "\n";
}

BuiltinTypeInfo::BuiltinTypeInfo(TypeRefBuilder &builder,
                                 RemoteRef<BuiltinTypeDescriptor> descriptor)
    : TypeInfo(TypeInfoKind::Builtin,
               descriptor->Size,
               descriptor->getAlignment(),
               descriptor->Stride,
               descriptor->NumExtraInhabitants,
               descriptor->isBitwiseTakable()),
      Name(builder.getTypeRefString(
              builder.readTypeRef(descriptor, descriptor->TypeName)))
{}

bool BuiltinTypeInfo::readExtraInhabitantIndex(
    remote::MemoryReader &reader, remote::RemoteAddress address,
    int *extraInhabitantIndex) const {
  if (getNumExtraInhabitants() == 0) {
    *extraInhabitantIndex = -1;
    return true;
  }
  // If it has extra inhabitants, it could be an integer type with extra
  // inhabitants (a bool) or a pointer.
  // Check if it's an integer first. The mangling of an integer type is
  // type ::= 'Bi' NATURAL '_'
  llvm::StringRef nameRef(Name);
  if (nameRef.startswith("Bi") && nameRef.endswith("_")) {
    // Drop the front "Bi" and "_" end, check that what we're left with is a
    // bool.
    llvm::StringRef naturalRef = nameRef.drop_front(2).drop_back();
    uint8_t natural;
    if (naturalRef.getAsInteger(10, natural))
      return false;

    assert(natural == 1 &&
           "Reading extra inhabitants of integer with more than 1 byte!");
    if (natural != 1)
      return false;

    assert(getSize() == 1 && "Reading extra inhabitants of integer but size of "
                             "type info is different than 1!");
    if (getSize() != 1)
      return false;

    assert(getNumExtraInhabitants() == 254 &&
           "Boolean type info should have 254 extra inhabitants!");
    if (getNumExtraInhabitants() != 254)
      return false;

    uint8_t rawValue;
    if (!reader.readInteger(address, &rawValue))
      return false;

    // The max valid value, for a bool valid values are 0 or 1, so this would
    // be 1.
    auto maxValidValue = 1;
    // If the raw value falls outside the range of valid values, this is an
    // extra inhabitant.
    if (maxValidValue < rawValue)
      *extraInhabitantIndex = rawValue - maxValidValue - 1;
    else
      *extraInhabitantIndex = -1;
    return true;
  } else if (Name == "yyXf") {
    // But there are two different conventions, one for function pointers:
    return reader.readFunctionPointerExtraInhabitantIndex(address,
                                                          extraInhabitantIndex);
  } else {
    // And one for pointers to heap-allocated blocks of memory
    return reader.readHeapObjectExtraInhabitantIndex(address,
                                                     extraInhabitantIndex);
  }
}

bool RecordTypeInfo::readExtraInhabitantIndex(remote::MemoryReader &reader,
                                              remote::RemoteAddress address,
                                              int *extraInhabitantIndex) const {
  *extraInhabitantIndex = -1;

  switch (SubKind) {
  case RecordKind::Invalid:
  case RecordKind::ClosureContext:
    return false;

  case RecordKind::OpaqueExistential:
  case RecordKind::ExistentialMetatype: {
    if (Fields.size() < 1) {
      return false;
    }
    auto metadata = Fields[0];
    auto metadataFieldAddress = address + metadata.Offset;
    return metadata.TI.readExtraInhabitantIndex(
      reader, metadataFieldAddress, extraInhabitantIndex);
  }

  case RecordKind::ThickFunction: {
    if (Fields.size() < 2) {
      return false;
    }
    auto function = Fields[0];
    auto context = Fields[1];
    if (function.Offset != 0) {
      return false;
    }
    auto functionFieldAddress = address;
    return function.TI.readExtraInhabitantIndex(
      reader, functionFieldAddress, extraInhabitantIndex);
  }

  case RecordKind::ClassExistential:
  case RecordKind::ErrorExistential: {
    if (Fields.size() < 1) {
      return true;
    }
    auto first = Fields[0];
    auto firstFieldAddress = address + first.Offset;
    return first.TI.readExtraInhabitantIndex(reader, firstFieldAddress,
                                             extraInhabitantIndex);
  }

  case RecordKind::ClassInstance:
    // This case seems unlikely to ever happen; if we're using XIs with a
    // class, it'll be with a reference, not with the instance itself (i.e.
    // we'll be in the RecordKind::ClassExistential case).
    return false;

  case RecordKind::Tuple:
  case RecordKind::Struct: {
    if (Fields.size() == 0) {
      return true;
    }
    // Tuples and Structs inherit XIs from their most capacious member
    auto mostCapaciousField = std::max_element(
      Fields.begin(), Fields.end(),
      [](const FieldInfo &lhs, const FieldInfo &rhs) {
        return lhs.TI.getNumExtraInhabitants() < rhs.TI.getNumExtraInhabitants();
      });
    auto fieldAddress = remote::RemoteAddress(address.getAddressData()
                                              + mostCapaciousField->Offset);
    return mostCapaciousField->TI.readExtraInhabitantIndex(
      reader, fieldAddress, extraInhabitantIndex);
  }
  }
  return false;
}

class UnsupportedEnumTypeInfo: public EnumTypeInfo {
public:
  UnsupportedEnumTypeInfo(unsigned Size, unsigned Alignment,
                          unsigned Stride, unsigned NumExtraInhabitants,
                          bool BitwiseTakable, EnumKind Kind,
                          const std::vector<FieldInfo> &Cases)
    : EnumTypeInfo(Size, Alignment, Stride, NumExtraInhabitants,
                   BitwiseTakable, Kind, Cases) {}

  bool readExtraInhabitantIndex(remote::MemoryReader &reader,
                       remote::RemoteAddress address,
                       int *index) const override {
    return false;
  }

  bool projectEnumValue(remote::MemoryReader &reader,
                       remote::RemoteAddress address,
                       int *CaseIndex) const override {
    return false;
  }
};

class EmptyEnumTypeInfo: public EnumTypeInfo {
public:
  EmptyEnumTypeInfo(const std::vector<FieldInfo> &Cases)
    : EnumTypeInfo(/*Size*/ 0, /* Alignment*/ 1, /*Stride*/ 1,
                   /*NumExtraInhabitants*/ 0, /*BitwiseTakable*/ true,
                   EnumKind::NoPayloadEnum, Cases) {
    // No cases
    assert(Cases.size() == 0);
  }

  bool readExtraInhabitantIndex(remote::MemoryReader &reader,
                       remote::RemoteAddress address,
                       int *index) const override {
    return false;
  }

  bool projectEnumValue(remote::MemoryReader &reader,
                       remote::RemoteAddress address,
                       int *CaseIndex) const override {
    return false;
  }
};

// Enum with a single non-payload case
class TrivialEnumTypeInfo: public EnumTypeInfo {
public:
  TrivialEnumTypeInfo(EnumKind Kind, const std::vector<FieldInfo> &Cases)
    : EnumTypeInfo(/*Size*/ 0,
                   /* Alignment*/ 1,
                   /*Stride*/ 1,
                   /*NumExtraInhabitants*/ 0,
                   /*BitwiseTakable*/ true,
                   Kind, Cases) {
    // Exactly one case
    assert(Cases.size() == 1);
    // The only case has no payload
    assert(Cases[0].TR == 0);
  }

  bool readExtraInhabitantIndex(remote::MemoryReader &reader,
                       remote::RemoteAddress address,
                       int *index) const override {
    *index = -1;
    return true;
  }

  bool projectEnumValue(remote::MemoryReader &reader,
                       remote::RemoteAddress address,
                       int *CaseIndex) const override {
    *CaseIndex = 0;
    return true;
  }
};

// Enum with 2 or more non-payload cases and no payload cases
class NoPayloadEnumTypeInfo: public EnumTypeInfo {
public:
  NoPayloadEnumTypeInfo(unsigned Size, unsigned Alignment,
                        unsigned Stride, unsigned NumExtraInhabitants,
                        EnumKind Kind,
                        const std::vector<FieldInfo> &Cases)
    : EnumTypeInfo(Size, Alignment, Stride, NumExtraInhabitants,
                   /*BitwiseTakable*/ true,
                   Kind, Cases) {
    // There are at least 2 cases
    // (one case would be trivial, zero is impossible)
    assert(Cases.size() >= 2);
    // No non-empty payloads
    assert(getNumNonEmptyPayloadCases() == 0);
  }

  bool readExtraInhabitantIndex(remote::MemoryReader &reader,
                       remote::RemoteAddress address,
                       int *index) const override {
    uint32_t tag = 0;
    if (!reader.readInteger(address, getSize(), &tag)) {
      return false;
    }
    if (tag < getNumCases()) {
      *index = -1;
    } else {
      *index = tag - getNumCases();
    }
    return true;
  }

  bool projectEnumValue(remote::MemoryReader &reader,
                       remote::RemoteAddress address,
                       int *CaseIndex) const override {
    uint32_t tag = 0;
    if (!reader.readInteger(address, getSize(), &tag)) {
      return false;
    }
    if (tag < getNumCases()) {
      *CaseIndex = tag;
      return true;
    } else {
      return false;
    }
  }
};

// Enum with 1 payload case and zero or more non-payload cases
class SinglePayloadEnumTypeInfo: public EnumTypeInfo {
public:
  SinglePayloadEnumTypeInfo(unsigned Size, unsigned Alignment,
                            unsigned Stride, unsigned NumExtraInhabitants,
                            bool BitwiseTakable,
                            EnumKind Kind,
                            const std::vector<FieldInfo> &Cases)
    : EnumTypeInfo(Size, Alignment, Stride, NumExtraInhabitants,
                   BitwiseTakable, Kind, Cases) {
    // The first case has a payload (possibly empty)
    assert(Cases[0].TR != 0);
    // At most one non-empty payload case
    assert(getNumNonEmptyPayloadCases() <= 1);
  }

  bool readExtraInhabitantIndex(remote::MemoryReader &reader,
                       remote::RemoteAddress address,
                       int *extraInhabitantIndex) const override {
    FieldInfo PayloadCase = getCases()[0];
    if (getSize() < PayloadCase.TI.getSize()) {
      // Single payload enums that use a separate tag don't export any XIs
      // So this is an invalid request.
      return false;
    }

    // Single payload enums inherit XIs from their payload type
    auto NumCases = getNumCases();
    if (NumCases == 1) {
      *extraInhabitantIndex = -1;
      return true;
    } else {
      if (!PayloadCase.TI.readExtraInhabitantIndex(reader, address,
                                                   extraInhabitantIndex)) {
        return false;
      }
      auto NumNonPayloadCases = NumCases - 1;
      if (*extraInhabitantIndex < 0
          || (unsigned long)*extraInhabitantIndex < NumNonPayloadCases) {
        *extraInhabitantIndex = -1;
      } else {
        *extraInhabitantIndex -= NumNonPayloadCases;
      }
      return true;
    }
  }

  // Think of a single-payload enum as being encoded in "pages".
  // The discriminator (tag) tells us which page we're on:
  // * Page 0 is the payload page which can either store
  //   the single payload case (any valid value
  //   for the payload) or any of N non-payload cases
  //   (encoded as XIs for the payload)
  // * Other pages use the payload area to encode non-payload
  //   cases.  The number of cases that can be encoded
  //   on each such page depends only on the size of the
  //   payload area.
  //
  // The above logic generalizes the following important cases:
  // * A payload with XIs will generally have enough to
  //   encode all payload cases.  If so, then it will have
  //   no discriminator allocated, so the discriminator is
  //   always treated as zero.
  // * If the payload has no XIs but is not zero-sized, then
  //   we'll need a page one.  That page will usually be
  //   large enough to encode all non-payload cases.
  // * If the payload is zero-sized, then we only have a
  //   discriminator.  In effect, the single-payload enum
  //   degenerates in this case to a non-payload enum
  //   (except for the subtle distinction that the
  //   single-payload enum doesn't export XIs).

  bool projectEnumValue(remote::MemoryReader &reader,
                       remote::RemoteAddress address,
                       int *CaseIndex) const override {
    auto PayloadCase = getCases()[0];
    auto PayloadSize = PayloadCase.TI.getSize();
    auto DiscriminatorAddress = address + PayloadSize;
    auto DiscriminatorSize = getSize() - PayloadSize;
    unsigned discriminator = 0;
    if (DiscriminatorSize > 0) {
      if (!reader.readInteger(DiscriminatorAddress,
                              DiscriminatorSize,
                              &discriminator)) {
        return false;
      }
    }
    unsigned nonPayloadCasesUsingXIs = PayloadCase.TI.getNumExtraInhabitants();
    int ComputedCase = 0;
    if (discriminator == 0) {
      // This is Page 0, which encodes payload case and some additional cases in Xis
      int XITag;
      if (!PayloadCase.TI.readExtraInhabitantIndex(reader, address, &XITag)) {
        return false;
      }
      ComputedCase = XITag < 0 ? 0 : XITag + 1;
    } else {
      // This is some other page, so the entire payload area is just a case index
      unsigned payloadTag;
      if (!reader.readInteger(address, PayloadSize, &payloadTag)) {
        return false;
      }
      auto casesPerNonPayloadPage =
        PayloadSize >= 4
         ? ValueWitnessFlags::MaxNumExtraInhabitants
         : (1UL << (PayloadSize * 8UL));
      ComputedCase =
        1 + nonPayloadCasesUsingXIs // Cases on page 0
        + (discriminator - 1) * casesPerNonPayloadPage // Cases on other pages
        + payloadTag; // Cases on this page
    }
    if (static_cast<unsigned>(ComputedCase) < getNumCases()) {
      *CaseIndex = ComputedCase;
      return true;
    }
    *CaseIndex = -1;
    return false;
  }
};

// *Tagged* Multi-payload enums use a separate tag value exclusively.
// This may be because it only has one payload (with no XIs) or
// because it's a true MPE but with no "spare bits" in the payload area.
// This includes cases such as:
//
// ```
// // Enums with non-pointer payloads (only pointers carry spare bits)
// enum A {
//   case a(Int)
//   case b(Double)
//   case c((Int8, UInt8))
// }
//
// // Generic enums (compiler doesn't have layout details)
// enum Either<T,U>{
//   case a(T)
//   case b(U)
// }
//
// // Enums where payload is covered by a non-pointer
// enum A {
//   case a(ClassTypeA)
//   case b(ClassTypeB)
//   case c(Int)
// }
//
// // Enums with one non-empty payload but that has no XIs
// // (This is almost but not quite the same as the single-payload
// // case.  Different in that this MPE exposes extra tag values
// // as XIs to an enclosing enum; SPEs don't do that.)
// enum A {
//   case a(Int)
//   case b(Void)
// }
// ```
class TaggedMultiPayloadEnumTypeInfo: public EnumTypeInfo {
  unsigned NumEffectivePayloadCases;
public:
  TaggedMultiPayloadEnumTypeInfo(unsigned Size, unsigned Alignment,
                           unsigned Stride, unsigned NumExtraInhabitants,
                           bool BitwiseTakable,
                           const std::vector<FieldInfo> &Cases,
                           unsigned NumEffectivePayloadCases)
    : EnumTypeInfo(Size, Alignment, Stride, NumExtraInhabitants,
                   BitwiseTakable, EnumKind::MultiPayloadEnum, Cases),
      NumEffectivePayloadCases(NumEffectivePayloadCases) {
    // Definition of "multi-payload enum"
    assert(getCases().size() > 1); // At least 2 cases
    assert(Cases[0].TR != 0); // At least 2 payloads
    // assert(Cases[1].TR != 0);
    // At least one payload is non-empty (otherwise this
    // would get laid out as a non-payload enum)
    assert(getNumNonEmptyPayloadCases() > 0);
    // There's a tag, so the total size must be bigger than any payload
    // assert(getSize() > getPayloadSize());
  }

  bool readExtraInhabitantIndex(remote::MemoryReader &reader,
                       remote::RemoteAddress address,
                       int *extraInhabitantIndex) const override {
    unsigned long PayloadSize = getPayloadSize();
    unsigned PayloadCount = getNumPayloadCases();
    unsigned TagSize = getSize() - PayloadSize;
    unsigned tag = 0;
    if (!reader.readInteger(address + PayloadSize,
                            getSize() - PayloadSize,
                            &tag)) {
      return false;
    }
    if (tag < PayloadCount + 1) {
      *extraInhabitantIndex = -1; // Valid payload, not an XI
    } else {
      // XIs are coded starting from the highest value that fits
      // E.g., for 1-byte tag, tag 255 == XI #0, tag 254 == XI #1, etc.
      unsigned maxTag = (TagSize >= 4) ? ~0U : (1U << (TagSize * 8U)) - 1;
      *extraInhabitantIndex = maxTag - tag;
    }
    return true;
  }

  bool projectEnumValue(remote::MemoryReader &reader,
                        remote::RemoteAddress address,
                        int *CaseIndex) const override {
    unsigned long PayloadSize = getPayloadSize();
    unsigned PayloadCount = NumEffectivePayloadCases;
    unsigned NumCases = getNumCases();
    unsigned TagSize = getSize() - PayloadSize;
    unsigned tag = 0;
    if (!reader.readInteger(address + PayloadSize,
                            getSize() - PayloadSize,
                            &tag)) {
      return false;
    }
    if (tag > ValueWitnessFlags::MaxNumExtraInhabitants) {
      return false;
    } else if (tag < PayloadCount) {
      *CaseIndex = tag;
    } else if (PayloadSize >= 4) {
      unsigned payloadTag = 0;
      if (tag > PayloadCount
          || !reader.readInteger(address, PayloadSize, &payloadTag)
          || PayloadCount + payloadTag >= getNumCases()) {
        return false;
      }
      *CaseIndex = PayloadCount + payloadTag;
    } else {
      unsigned payloadTagCount = (1U << (TagSize * 8U)) - 1;
      unsigned maxValidTag = (NumCases - PayloadCount) / payloadTagCount + PayloadCount;
      unsigned payloadTag = 0;
      if (tag > maxValidTag
          || !reader.readInteger(address, PayloadSize, &payloadTag)) {
        return false;
      }
      unsigned ComputedCase = PayloadCount
        + (tag - PayloadCount) * payloadTagCount + payloadTag;
      if (ComputedCase >= NumCases) {
        return false;
      }
      *CaseIndex = ComputedCase;
    }
    return true;
  }
};

// A variable-length bitmap used to track "spare bits" for general multi-payload
// enums.
class BitMask {
  static constexpr unsigned maxSize = 128 * 1024 * 1024; // 128MB

  unsigned size; // Size of mask in bytes
  uint8_t *mask;
public:
  ~BitMask() {
    free(mask);
  }
  // Construct a bitmask of the appropriate number of bytes
  // initialized to all bits set
  BitMask(unsigned sizeInBytes): size(sizeInBytes) {
    // Gracefully fail by constructing an empty mask if we exceed the size
    // limit.
    if (size > maxSize) {
      size = 0;
      mask = nullptr;
      return;
    }

    mask = (uint8_t *)malloc(size);

    if (!mask) {
      // Malloc might fail if size is large due to some bad data. Assert in
      // asserts builds, and fail gracefully in non-asserts builds by
      // constructing an empty BitMask.
      assert(false && "Failed to allocate BitMask");
      size = 0;
      return;
    }

    memset(mask, 0xff, size);
  }
  // Construct a bitmask of the appropriate number of bytes
  // initialized with bits from the specified buffer
  BitMask(unsigned sizeInBytes, const uint8_t *initialValue,
          unsigned initialValueBytes, unsigned offset)
      : size(sizeInBytes) {
    // Gracefully fail by constructing an empty mask if we exceed the size
    // limit.
    if (size > maxSize) {
      size = 0;
      mask = nullptr;
      return;
    }

    // Bad data could cause the initial value location to be off the end of our
    // size. If initialValueBytes + offset is beyond sizeInBytes (or overflows),
    // assert in asserts builds, and fail gracefully in non-asserts builds by
    // constructing an empty BitMask.
    bool overflowed = false;
    unsigned initialValueEnd =
        llvm::SaturatingAdd(initialValueBytes, offset, &overflowed);
    if (overflowed) {
      assert(false && "initialValueBytes + offset overflowed");
      size = 0;
      mask = nullptr;
      return;
    }
    assert(initialValueEnd <= sizeInBytes);
    if (initialValueEnd > size) {
      assert(false && "initialValueBytes + offset is greater than size");
      size = 0;
      mask = nullptr;
      return;
    }

    mask = (uint8_t *)calloc(1, size);

    if (!mask) {
      // Malloc might fail if size is large due to some bad data. Assert in
      // asserts builds, and fail gracefully in non-asserts builds by
      // constructing an empty BitMask.
      assert(false && "Failed to allocate BitMask");
      size = 0;
      return;
    }

    memcpy(mask + offset, initialValue, initialValueBytes);
  }
  // Move constructor moves ownership and zeros the src
  BitMask(BitMask&& src) noexcept: size(src.size), mask(std::move(src.mask)) {
    src.size = 0;
    src.mask = nullptr;
  }
  // Copy constructor makes a copy of the mask storage
  BitMask(const BitMask& src) noexcept: size(src.size), mask(nullptr) {
    mask = (uint8_t *)malloc(size);
    memcpy(mask, src.mask, size);
  }

  std::string str() const {
    std::ostringstream buff;
    buff << size << ":0x";
    for (unsigned i = 0; i < size; i++) {
      buff << std::hex << ((mask[i] >> 4) & 0x0f) << (mask[i] & 0x0f);
    }
    return buff.str();
  }

  bool operator==(const BitMask& rhs) const {
    // The two masks may be of different sizes.
    // The common prefix must be identical.
    size_t common = std::min(size, rhs.size);
    if (memcmp(mask, rhs.mask, common) != 0)
      return false;
    // The remainder of the longer mask must be
    // all zero bits.
    unsigned mustBeZeroSize = std::max(size, rhs.size) - common;
    uint8_t *mustBeZero;
    if (size < rhs.size) {
      mustBeZero = rhs.mask + size;
    } else if (size > rhs.size) {
      mustBeZero = mask + rhs.size;
    }
    for (unsigned i = 0; i < mustBeZeroSize; ++i) {
      if (mustBeZero[i] != 0) {
        return false;
      }
    }
    return true;
  }

  bool operator!=(const BitMask& rhs) const {
    return !(*this == rhs);
  }

  bool isNonZero() const { return !isZero(); }

  bool isZero() const {
    for (unsigned i = 0; i < size; ++i) {
      if (mask[i] != 0) {
        return false;
      }
    }
    return true;
  }

  void makeZero() {
    memset(mask, 0, size * sizeof(mask[0]));
  }

  void complement() {
    for (unsigned i = 0; i < size; ++i) {
      mask[i] = ~mask[i];
    }
  }

  int countSetBits() const {
    static const int counter[] =
      {0, 1, 1, 2, 1, 2, 2, 3, 1, 2, 2, 3, 2, 3, 3, 4};
    int bits = 0;
    for (unsigned i = 0; i < size; ++i) {
      bits += counter[mask[i] >> 4] + counter[mask[i] & 15];
    }
    return bits;
  }

  int countZeroBits() const {
    static const int counter[] =
      {4, 3, 3, 2, 3, 2, 2, 1, 3, 2, 2, 1, 2, 1, 1, 0};
    int bits = 0;
    for (unsigned i = 0; i < size; ++i) {
      bits += counter[mask[i] >> 4] + counter[mask[i] & 15];
    }
    return bits;
  }

  // Treat the provided value as a mask, `and` it with
  // the part of the mask at the provided byte offset.
  // Bits outside the specified area are unchanged.
  template<typename IntegerType>
  void andMask(IntegerType value, unsigned byteOffset) {
    andMask((void *)&value, sizeof(value), byteOffset);
  }

  // As above, but using the provided bitmask instead
  // of an integer.
  void andMask(BitMask mask, unsigned offset) {
    andMask(mask.mask, mask.size, offset);
  }

  // As above, but using the complement of the
  // provided mask.
  void andNotMask(BitMask mask, unsigned offset) {
    if (offset < size) {
      andNotMask(mask.mask, mask.size, offset);
    }
  }

  // Zero all bits except for the `n` most significant ones.
  // XXX TODO: Big-endian support?
  void keepOnlyMostSignificantBits(unsigned n) {
    unsigned count = 0;
    if (size < 1) {
      return;
    }
    unsigned i = size;
    while (i > 0) {
      i -= 1;
      if (count < n) {
        for (int b = 128; b > 0; b >>= 1) {
          if (count >= n) {
            mask[i] &= ~b;
          } else if ((mask[i] & b) != 0) {
            ++count;
          }
        }
      } else {
        mask[i] = 0;
      }
    }
  }

  unsigned numBits() const {
    return size * 8;
  }

  unsigned numSetBits() const {
    unsigned count = 0;
    for (unsigned i = 0; i < size; ++i) {
      if (mask[i] != 0) {
        for (unsigned b = 1; b < 256; b <<= 1) {
          if ((mask[i] & b) != 0) {
            ++count;
          }
        }
      }
    }
    return count;
  }

  // Read a mask-sized area from the target and collect
  // the masked bits into a single integer.
  template<typename IntegerType>
  bool readMaskedInteger(remote::MemoryReader &reader,
                         remote::RemoteAddress address,
                         IntegerType *dest) const {
    auto data = reader.readBytes(address, size);
    if (!data) {
      return false;
    }
#if defined(__BIG_ENDIAN__)
    assert(false && "Big endian not supported for readMaskedInteger");
#else
    IntegerType result = 0;
    IntegerType resultBit = 1; // Start from least-significant bit
    auto bytes = static_cast<const uint8_t *>(data.get());
    for (unsigned i = 0; i < size; ++i) {
      for (unsigned b = 1; b < 256; b <<= 1) {
        if ((mask[i] & b) != 0) {
          if ((bytes[i] & b) != 0) {
            result |= resultBit;
          }
          resultBit <<= 1;
        }
      }
    }
    *dest = result;
    return true;
#endif
  }

private:
  void andMask(void *maskData, unsigned len, unsigned offset) {
    if (offset < size) {
      unsigned common = std::min(len, size - offset);
      uint8_t *maskBytes = (uint8_t *)maskData;
      for (unsigned i = 0; i < common; ++i) {
        mask[i + offset] &= maskBytes[i];
      }
    }
  }

  void andNotMask(void *maskData, unsigned len, unsigned offset) {
    assert(offset < size);
    if (offset < size) {
      unsigned common = std::min(len, size - offset);
      uint8_t *maskBytes = (uint8_t *)maskData;
      for (unsigned i = 0; i < common; ++i) {
        mask[i + offset] &= ~maskBytes[i];
      }
    }
  }
};

// General multi-payload enum support for enums that do use spare
// bits in the payload.
class MultiPayloadEnumTypeInfo: public EnumTypeInfo {
  BitMask spareBitsMask;
  // "Effective" payload cases includes those with
  // generic payload and non-generic cases that are
  // statically known to have non-zero size.
  // It does not include cases with payloads that are
  // non-generic and zero-sized (these are treated as
  // non-payload cases for many purposes).
  unsigned NumEffectivePayloadCases;
public:
  MultiPayloadEnumTypeInfo(unsigned Size, unsigned Alignment,
                           unsigned Stride, unsigned NumExtraInhabitants,
                           bool BitwiseTakable,
                           const std::vector<FieldInfo> &Cases,
                           BitMask spareBitsMask,
                           unsigned NumEffectivePayloadCases)
    : EnumTypeInfo(Size, Alignment, Stride, NumExtraInhabitants,
                   BitwiseTakable, EnumKind::MultiPayloadEnum, Cases),
      spareBitsMask(spareBitsMask),
      NumEffectivePayloadCases(NumEffectivePayloadCases) {
    assert(Cases[0].TR != 0);
    assert(Cases[1].TR != 0);
    assert(getNumNonEmptyPayloadCases() > 1);
  }

  bool readExtraInhabitantIndex(remote::MemoryReader &reader,
                       remote::RemoteAddress address,
                       int *extraInhabitantIndex) const override {
    unsigned long payloadSize = getPayloadSize();

    // Multi payload enums that use spare bits export unused tag values as XIs.
    uint32_t tag = 0;
    unsigned tagBits = 0;

    // The full tag value is built by combining three sets of bits:
    // Low-order bits: payload tag bits (most-significant spare bits)
    // Middle: spare bits that are not payload tag bits
    // High-order: extra discriminator byte

    auto payloadTagLowBitsMask = getMultiPayloadTagBitsMask();
    auto payloadTagLowBitCount = payloadTagLowBitsMask.countSetBits();
    uint32_t payloadTagLow = 0;
    if (!payloadTagLowBitsMask.readMaskedInteger(reader, address, &payloadTagLow)) {
      return false;
    }

    // Add the payload tag bits to the growing tag...
    tag = payloadTagLow;
    tagBits = payloadTagLowBitCount;

    // Read the other spare bits
    auto otherSpareBitsMask = spareBitsMask; // copy
    otherSpareBitsMask.andNotMask(payloadTagLowBitsMask, 0);
    auto otherSpareBitsCount = otherSpareBitsMask.countSetBits();
    if (otherSpareBitsCount > 0) {
      // Add other spare bits to the growing tag...
      uint32_t otherSpareBits = 0;
      if (!otherSpareBitsMask.readMaskedInteger(reader, address, &otherSpareBits)) {
        return false;
      }
      tag |= otherSpareBits << tagBits;
      tagBits += otherSpareBitsCount;
    }

    // If there is an extra discriminator tag, add those bits to the tag
    auto extraTagSize = getSize() - payloadSize;
    unsigned extraTag = 0;
    if (extraTagSize > 0 && tagBits < 32) {
      auto extraTagAddress = address + payloadSize;
      if (!reader.readInteger(extraTagAddress, extraTagSize,
                              &extraTag)) {
        return false;
      }
    }
    tag |= extraTag << tagBits;
    tagBits += extraTagSize * 8;

    // Check whether this tag is used for valid content
    auto payloadCases = getNumPayloadCases();
    auto nonPayloadCases = getNumCases() - payloadCases;
    uint32_t inhabitedTags;
    if (nonPayloadCases == 0) {
      inhabitedTags = payloadCases;
    } else {
      auto payloadBitsForTags = spareBitsMask.countZeroBits();
      uint32_t nonPayloadTags
        = (nonPayloadCases + (1 << payloadBitsForTags) - 1)
        >> payloadBitsForTags;
      inhabitedTags = payloadCases + nonPayloadTags;
    }

    if (tag < inhabitedTags) {
      *extraInhabitantIndex = -1;
      return true;
    }

    // Transform the tag value into the XI index
    uint32_t maxTag = (tagBits >= 32) ? ~0u : (1UL << tagBits) - 1;
    *extraInhabitantIndex = maxTag - tag;
    return true;
  }

  bool projectEnumValue(remote::MemoryReader &reader,
                        remote::RemoteAddress address,
                        int *CaseIndex) const override {
    unsigned long payloadSize = getPayloadSize();

    // Extra Tag (if any) holds upper bits of case value
    auto extraTagSize = getSize() - payloadSize;
    unsigned extraTag = 0;
    if (extraTagSize > 0) {
      auto extraTagAddress = address + payloadSize;
      if (!reader.readInteger(extraTagAddress, extraTagSize,
                              &extraTag)) {
        return false;
      }
    }

    // The `payloadTagMask` is a subset of the spare bits
    // where we encode the rest of the case value.
    auto payloadTagMask = getMultiPayloadTagBitsMask();
    auto numPayloadTagBits = payloadTagMask.countSetBits();
    uint64_t payloadTag = 0;
    if (!payloadTagMask.readMaskedInteger(reader, address, &payloadTag)) {
      return false;
    }

    // Combine the extra tag and payload tag info:
    int tagValue = 0;
    if (numPayloadTagBits >= 32) {
      tagValue = payloadTag;
    } else {
      tagValue = (extraTag << numPayloadTagBits) | payloadTag;
    }

    // If the above identifies a payload case, we're done
    if (static_cast<unsigned>(tagValue) < NumEffectivePayloadCases) {
      *CaseIndex = tagValue;
      return true;
    }

    // Otherwise, combine with other payload data to select a non-payload case
    auto occupiedBits = spareBitsMask; // Copy
    occupiedBits.complement();

    auto occupiedBitCount = occupiedBits.countSetBits();
    uint64_t payloadValue = 0;
    if (!occupiedBits.readMaskedInteger(reader, address, &payloadValue)) {
      return false;
    }

    int ComputedCase = 0;
    if (occupiedBitCount >= 32) {
      ComputedCase = payloadValue + NumEffectivePayloadCases;
    } else {
      ComputedCase = (((tagValue - NumEffectivePayloadCases) << occupiedBitCount) |  payloadValue) + NumEffectivePayloadCases;
    }

    if (static_cast<unsigned>(ComputedCase) < getNumCases()) {
      *CaseIndex = ComputedCase;
      return true;
    } else {
      *CaseIndex = -1;
      return false;
    }
  }

  // The case value is stored in three pieces:
  // * A separate "discriminator" tag appended to the payload (if necessary)
  // * A "payload tag" that uses (a subset of) the spare bits
  // * The remainder of the payload bits (for non-payload cases)
  // This computes the bits used for the payload tag.
  BitMask getMultiPayloadTagBitsMask() const {
    auto payloadTagValues = NumEffectivePayloadCases - 1;
    if (getNumCases() > NumEffectivePayloadCases) {
      payloadTagValues += 1;
    }
    int payloadTagBits = 0;
    while (payloadTagValues > 0) {
      payloadTagValues >>= 1;
      payloadTagBits += 1;
    }
    BitMask payloadTagBitsMask = spareBitsMask;
    payloadTagBitsMask.keepOnlyMostSignificantBits(payloadTagBits);
    return payloadTagBitsMask;
  }
};

// Recursively populate the spare bit mask for this single type
static bool populateSpareBitsMask(const TypeInfo *TI, BitMask &mask, uint64_t mpePointerSpareBits);

// Recursively populate the spare bit mask for this collection of
// record fields or enum cases.
static bool populateSpareBitsMask(const std::vector<FieldInfo> &Fields, BitMask &mask, uint64_t mpePointerSpareBits) {
  for (auto Field : Fields) {
    if (Field.TR != 0) {
      BitMask submask(Field.TI.getSize());
      if (!populateSpareBitsMask(&Field.TI, submask, mpePointerSpareBits)) {
        return false;
      }
      mask.andMask(submask, Field.Offset);
    }
  }
  return true;
}

// General recursive type walk to combine spare bit info from nested structures.
static bool populateSpareBitsMask(const TypeInfo *TI, BitMask &mask, uint64_t mpePointerSpareBits) {
  switch (TI->getKind()) {
  case TypeInfoKind::Reference: {
    if (TI->getSize() == 8) {
      mask.andMask(mpePointerSpareBits, 0);
    } else /* TI->getSize() == 4 */ {
      uint32_t pointerMask = (uint32_t)mpePointerSpareBits;
      mask.andMask(pointerMask, 0);
    }
    break;
  }
  case TypeInfoKind::Enum: {
    auto EnumTI = reinterpret_cast<const EnumTypeInfo *>(TI);
    // Remove bits used by the payloads
    if (!populateSpareBitsMask(EnumTI->getCases(), mask, mpePointerSpareBits)) {
      return false;
    }
    // TODO: Remove bits needed to discriminate payloads.
    // Until then, return false for any type with an enum in it so we
    // won't claim to support something we don't.
    return false;
    break;
  }
  case TypeInfoKind::Record: {
    auto RecordTI = dyn_cast<RecordTypeInfo>(TI);
    if (!populateSpareBitsMask(RecordTI->getFields(), mask, mpePointerSpareBits)) {
      return false;
    }
    break;
  }
  default: {
    mask.makeZero();
    break;
  }
  }
  return true;
}

/// Utility class for building values that contain witness tables.
class ExistentialTypeInfoBuilder {
  TypeConverter &TC;
  std::vector<const TypeRef *> Protocols;
  const TypeRef *Superclass = nullptr;
  ExistentialTypeRepresentation Representation;
  ReferenceCounting Refcounting;
  bool ObjC;
  unsigned WitnessTableCount;
  bool Invalid;

  bool isSingleError() const {
    // If we changed representation, it means we added a
    // superclass constraint or an AnyObject member.
    if (Representation != ExistentialTypeRepresentation::Opaque)
      return false;

    if (Protocols.size() != 1)
      return false;

    if (Superclass)
      return false;

    for (auto *P : Protocols) {
      if (auto *NTD = dyn_cast<NominalTypeRef>(P))
        if (NTD->isErrorProtocol())
          return true;
    }
    return false;
  }

  void examineProtocols() {
    if (isSingleError()) {
      Representation = ExistentialTypeRepresentation::Error;
      // No extra witness table for protocol<Error>
      return;
    }

    for (auto *P : Protocols) {
      auto *NTD = dyn_cast<NominalTypeRef>(P);
      auto *OP = dyn_cast<ObjCProtocolTypeRef>(P);
      if (!NTD && !OP) {
        DEBUG_LOG(fprintf(stderr, "Bad protocol: "); P->dump())
        Invalid = true;
        continue;
      }

      // Don't look up field info for imported Objective-C protocols.
      if (OP) {
        ObjC = true;
        continue;
      }

      auto FD = TC.getBuilder().getFieldTypeInfo(P);
      if (FD == nullptr) {
        DEBUG_LOG(fprintf(stderr, "No field descriptor: "); P->dump())
        Invalid = true;
        continue;
      }

      switch (FD->Kind) {
        case FieldDescriptorKind::ObjCProtocol:
          // Objective-C protocols do not have any witness tables.
          ObjC = true;
          continue;
        case FieldDescriptorKind::ClassProtocol:
          Representation = ExistentialTypeRepresentation::Class;
          ++WitnessTableCount;

          if (auto *Superclass = TC.getBuilder().lookupSuperclass(P)) {
            // ObjC class info should be available in the metadata, so it's safe
            // to not pass an external provider here. This helps preserving the
            // layering.
            auto *SuperclassTI = TC.getTypeInfo(Superclass, nullptr);
            if (SuperclassTI == nullptr) {
              DEBUG_LOG(fprintf(stderr, "No TypeInfo for superclass: ");
                        Superclass->dump());
              Invalid = true;
              continue;
            }

            if (!isa<ReferenceTypeInfo>(SuperclassTI)) {
              DEBUG_LOG(fprintf(stderr, "Superclass not a reference type: ");
                        SuperclassTI->dump());
              Invalid = true;
              continue;
            }

            if (cast<ReferenceTypeInfo>(SuperclassTI)->getReferenceCounting()
                == ReferenceCounting::Native) {
              Refcounting = ReferenceCounting::Native;
            }
          }

          continue;
        case FieldDescriptorKind::Protocol:
          ++WitnessTableCount;
          continue;
        case FieldDescriptorKind::ObjCClass:
        case FieldDescriptorKind::Struct:
        case FieldDescriptorKind::Enum:
        case FieldDescriptorKind::MultiPayloadEnum:
        case FieldDescriptorKind::Class:
          Invalid = true;
          continue;
      }
    }
  }

public:
  ExistentialTypeInfoBuilder(TypeConverter &TC)
    : TC(TC), Representation(ExistentialTypeRepresentation::Opaque),
      Refcounting(ReferenceCounting::Unknown),
      ObjC(false), WitnessTableCount(0),
      Invalid(false) {}

  void addProtocol(const TypeRef *P) {
    Protocols.push_back(P);
  }

  void addProtocolComposition(const ProtocolCompositionTypeRef *PC) {
    for (auto *T : PC->getProtocols()) {
      addProtocol(T);
    }

    if (PC->hasExplicitAnyObject())
      addAnyObject();

    if (auto *T = PC->getSuperclass()) {
      // Anything else should either be a superclass constraint, or
      // we have an invalid typeref.
      if (!isa<NominalTypeRef>(T) &&
          !isa<BoundGenericTypeRef>(T) &&
          !isa<ObjCClassTypeRef>(T)) {
        DEBUG_LOG(fprintf(stderr, "Bad existential member: "); T->dump())
        Invalid = true;
        return;
      }

      // Don't look up field info for imported Objective-C classes.
      if (isa<ObjCClassTypeRef>(T)) {
        addAnyObject();
        return;
      }

      const auto &FD = TC.getBuilder().getFieldTypeInfo(T);
      if (FD == nullptr) {
        DEBUG_LOG(fprintf(stderr, "No field descriptor: "); T->dump())
        Invalid = true;
        return;
      }

      // We have a valid superclass constraint. It only affects
      // lowering by class-constraining the entire existential.
      switch (FD->Kind) {
      case FieldDescriptorKind::Class:
        Refcounting = ReferenceCounting::Native;
        SWIFT_FALLTHROUGH;

      case FieldDescriptorKind::ObjCClass:
        addAnyObject();
        break;

      default:
        DEBUG_LOG(fprintf(stderr, "Bad existential member: "); T->dump())
        Invalid = true;
        return;
      }
    }
  }

  void addAnyObject() {
    Representation = ExistentialTypeRepresentation::Class;
  }

  void markInvalid() {
    Invalid = true;
  }

  const TypeInfo *build(remote::TypeInfoProvider *ExternalTypeInfo) {
    examineProtocols();

    if (Invalid)
      return nullptr;

    if (ObjC) {
      if (WitnessTableCount > 0) {
        DEBUG_LOG(fprintf(stderr, "@objc existential with witness tables\n"));
        return nullptr;
      }

      return TC.getReferenceTypeInfo(ReferenceKind::Strong,
                                     Refcounting);
    }

    RecordKind Kind;
    switch (Representation) {
    case ExistentialTypeRepresentation::Class:
      Kind = RecordKind::ClassExistential;
      break;
    case ExistentialTypeRepresentation::Opaque:
      Kind = RecordKind::OpaqueExistential;
      break;
    case ExistentialTypeRepresentation::Error:
      Kind = RecordKind::ErrorExistential;
      break;
    }

    RecordTypeInfoBuilder builder(TC, Kind);

    switch (Representation) {
    case ExistentialTypeRepresentation::Class:
      // Class existentials consist of a single retainable pointer
      // followed by witness tables.
      if (Refcounting == ReferenceCounting::Unknown)
        builder.addField("object", TC.getUnknownObjectTypeRef(),
                         ExternalTypeInfo);
      else
        builder.addField("object", TC.getNativeObjectTypeRef(),
                         ExternalTypeInfo);
      break;
    case ExistentialTypeRepresentation::Opaque: {
      auto *TI = TC.getTypeInfo(TC.getRawPointerTypeRef(), ExternalTypeInfo);
      if (TI == nullptr) {
        DEBUG_LOG(fprintf(stderr, "No TypeInfo for RawPointer\n"));
        return nullptr;
      }

      // Non-class existentials consist of a three-word buffer,
      // value metadata, and finally zero or more witness tables.
      // The buffer is always bitwise takable, since non-bitwise
      // takable payloads are stored out of line.
      builder.addField(TI->getSize() * 3,
                       TI->getAlignment(),
                       /*numExtraInhabitants=*/0,
                       /*bitwiseTakable=*/true);
      builder.addField("metadata", TC.getAnyMetatypeTypeRef(), ExternalTypeInfo);
      break;
    }
    case ExistentialTypeRepresentation::Error:
      builder.addField("error", TC.getUnknownObjectTypeRef(), ExternalTypeInfo);
      break;
    }

    for (unsigned i = 0; i < WitnessTableCount; ++i)
      builder.addField("wtable", TC.getRawPointerTypeRef(), ExternalTypeInfo);

    return builder.build();
  }

  const TypeInfo *buildMetatype(remote::TypeInfoProvider *ExternalTypeInfo) {
    examineProtocols();

    if (Invalid)
      return nullptr;

    if (ObjC) {
      if (WitnessTableCount > 0) {
        DEBUG_LOG(fprintf(stderr, "@objc existential with witness tables\n"));
        return nullptr;
      }

      return TC.getAnyMetatypeTypeInfo();
    }

    RecordTypeInfoBuilder builder(TC, RecordKind::ExistentialMetatype);

    builder.addField("metadata", TC.getAnyMetatypeTypeRef(), ExternalTypeInfo);
    for (unsigned i = 0; i < WitnessTableCount; ++i)
      builder.addField("wtable", TC.getRawPointerTypeRef(), ExternalTypeInfo);

    return builder.build();
  }
};

unsigned RecordTypeInfoBuilder::addField(unsigned fieldSize,
                                         unsigned fieldAlignment,
                                         unsigned numExtraInhabitants,
                                         bool bitwiseTakable) {
  assert(fieldAlignment > 0);

  // Align the current size appropriately
  Size = ((Size + fieldAlignment - 1) & ~(fieldAlignment - 1));

  // Record the offset
  unsigned offset = Size;

  // Update the aggregate size
  Size += fieldSize;

  // Update the aggregate alignment
  Alignment = std::max(Alignment, fieldAlignment);

  // The aggregate is bitwise takable if all elements are.
  BitwiseTakable &= bitwiseTakable;

  switch (Kind) {
  // The extra inhabitants of a struct or tuple are the same as the extra
  // inhabitants of the field that has the most.
  // Opaque existentials pick up the extra inhabitants of their type metadata
  // field.
  case RecordKind::Struct:
  case RecordKind::OpaqueExistential:
  case RecordKind::Tuple:
    NumExtraInhabitants = std::max(NumExtraInhabitants, numExtraInhabitants);
    break;

  // For other kinds of records, we only use the extra inhabitants of the
  // first field.
  case RecordKind::ClassExistential:
  case RecordKind::ClassInstance:
  case RecordKind::ClosureContext:
  case RecordKind::ErrorExistential:
  case RecordKind::ExistentialMetatype:
  case RecordKind::Invalid:
  case RecordKind::ThickFunction:
    if (Empty) {
      NumExtraInhabitants = numExtraInhabitants;
    }
    break;
  }
  Empty = false;

  return offset;
}

void RecordTypeInfoBuilder::addField(
    const std::string &Name, const TypeRef *TR,
    remote::TypeInfoProvider *ExternalTypeInfo) {
  const TypeInfo *TI = TC.getTypeInfo(TR, ExternalTypeInfo);
  if (TI == nullptr) {
    DEBUG_LOG(fprintf(stderr, "No TypeInfo for field type: "); TR->dump());
    Invalid = true;
    return;
  }

  unsigned offset = addField(TI->getSize(),
                             TI->getAlignment(),
                             TI->getNumExtraInhabitants(),
                             TI->isBitwiseTakable());
  Fields.push_back({Name, offset, -1, TR, *TI});
}

const RecordTypeInfo *RecordTypeInfoBuilder::build() {
  if (Invalid)
    return nullptr;

  // Calculate the stride
  unsigned Stride = ((Size + Alignment - 1) & ~(Alignment - 1));
  if (Stride == 0)
    Stride = 1;

  return TC.makeTypeInfo<RecordTypeInfo>(
      Size, Alignment, Stride,
      NumExtraInhabitants, BitwiseTakable,
      Kind, Fields);
}

const ReferenceTypeInfo *
TypeConverter::getReferenceTypeInfo(ReferenceKind Kind,
                                    ReferenceCounting Refcounting) {
  auto key = std::make_pair(unsigned(Kind), unsigned(Refcounting));
  auto found = ReferenceCache.find(key);
  if (found != ReferenceCache.end())
    return found->second;

  const TypeRef *TR;
  switch (Refcounting) {
  case ReferenceCounting::Native:
    TR = getNativeObjectTypeRef();
    break;
  case ReferenceCounting::Unknown:
    TR = getUnknownObjectTypeRef();
    break;
  }

  // Unowned and unmanaged references have the same extra inhabitants
  // as the underlying type.
  //
  // Weak references do not have any extra inhabitants.

  auto BuiltinTI = Builder.getBuiltinTypeInfo(TR);
  if (BuiltinTI == nullptr) {
    DEBUG_LOG(fprintf(stderr, "No TypeInfo for reference type: "); TR->dump());
    return nullptr;
  }

  unsigned numExtraInhabitants = BuiltinTI->NumExtraInhabitants;
  bool bitwiseTakable = true;

  switch (Kind) {
  case ReferenceKind::Strong:
    break;
  case ReferenceKind::Weak:
    numExtraInhabitants = 0;
    bitwiseTakable = false;
    break;
  case ReferenceKind::Unowned:
    if (Refcounting == ReferenceCounting::Unknown)
      bitwiseTakable = false;
    break;
  case ReferenceKind::Unmanaged:
    break;
  }

  auto *TI = makeTypeInfo<ReferenceTypeInfo>(BuiltinTI->Size,
                                             BuiltinTI->getAlignment(),
                                             BuiltinTI->Stride,
                                             numExtraInhabitants,
                                             bitwiseTakable,
                                             Kind, Refcounting);
  ReferenceCache[key] = TI;
  return TI;
}

/// Thin functions consist of a function pointer. We do not use
/// Builtin.RawPointer here, since the extra inhabitants differ.
const TypeInfo *
TypeConverter::getThinFunctionTypeInfo() {
  if (ThinFunctionTI != nullptr)
    return ThinFunctionTI;

  auto descriptor = getBuilder().getBuiltinTypeInfo(getThinFunctionTypeRef());
  if (descriptor == nullptr) {
    DEBUG_LOG(fprintf(stderr, "No TypeInfo for function type\n"));
    return nullptr;
  }

  ThinFunctionTI = makeTypeInfo<BuiltinTypeInfo>(getBuilder(), descriptor);

  return ThinFunctionTI;
}

/// Thick functions consist of a function pointer and nullable retainable
/// context pointer. The context is modeled exactly like a native Swift
/// class reference.
const TypeInfo *TypeConverter::getThickFunctionTypeInfo() {
  if (ThickFunctionTI != nullptr)
    return ThickFunctionTI;

  RecordTypeInfoBuilder builder(*this, RecordKind::ThickFunction);
  builder.addField("function", getThinFunctionTypeRef(), nullptr);
  builder.addField("context", getNativeObjectTypeRef(), nullptr);
  ThickFunctionTI = builder.build();

  return ThickFunctionTI;
}

/// Thick metatypes consist of a single pointer, possibly followed
/// by witness tables. We do not use Builtin.RawPointer here, since
/// the extra inhabitants differ.
const TypeInfo *
TypeConverter::getAnyMetatypeTypeInfo() {
  if (AnyMetatypeTI != nullptr)
    return AnyMetatypeTI;

  auto descriptor = getBuilder().getBuiltinTypeInfo(getAnyMetatypeTypeRef());
  if (descriptor == nullptr) {
    DEBUG_LOG(fprintf(stderr, "No TypeInfo for metatype type\n"));
    return nullptr;
  }

  AnyMetatypeTI = makeTypeInfo<BuiltinTypeInfo>(getBuilder(), descriptor);

  return AnyMetatypeTI;
}

const TypeInfo *TypeConverter::getEmptyTypeInfo() {
  if (EmptyTI != nullptr)
    return EmptyTI;

  EmptyTI = makeTypeInfo<BuiltinTypeInfo>();
  return EmptyTI;
}

const TypeRef *TypeConverter::getRawPointerTypeRef() {
  if (RawPointerTR != nullptr)
    return RawPointerTR;

  RawPointerTR = BuiltinTypeRef::create(Builder, "Bp");
  return RawPointerTR;
}

const TypeRef *TypeConverter::getNativeObjectTypeRef() {
  if (NativeObjectTR != nullptr)
    return NativeObjectTR;

  NativeObjectTR = BuiltinTypeRef::create(Builder, "Bo");
  return NativeObjectTR;
}

const TypeRef *TypeConverter::getUnknownObjectTypeRef() {
  if (UnknownObjectTR != nullptr)
    return UnknownObjectTR;

  UnknownObjectTR = BuiltinTypeRef::create(Builder, "BO");
  return UnknownObjectTR;
}

const TypeRef *TypeConverter::getThinFunctionTypeRef() {
  if (ThinFunctionTR != nullptr)
    return ThinFunctionTR;

  ThinFunctionTR = BuiltinTypeRef::create(Builder, "yyXf");
  return ThinFunctionTR;
}

const TypeRef *TypeConverter::getAnyMetatypeTypeRef() {
  if (AnyMetatypeTR != nullptr)
    return AnyMetatypeTR;

  AnyMetatypeTR = BuiltinTypeRef::create(Builder, "ypXp");
  return AnyMetatypeTR;
}

enum class MetatypeRepresentation : unsigned {
  /// Singleton metatype values are empty.
  Thin,

  /// Metatypes containing classes, or where the original unsubstituted
  /// type contains a type parameter, must be represented as pointers
  /// to metadata structures.
  Thick,

  /// Insufficient information to determine which.
  Unknown
};

/// Visitor class to determine if a type has a fixed size.
///
/// Conservative approximation.
class HasFixedSize
  : public TypeRefVisitor<HasFixedSize, bool> {

public:
  HasFixedSize() {}

  using TypeRefVisitor<HasFixedSize, bool>::visit;

  bool visitBuiltinTypeRef(const BuiltinTypeRef *B) {
    return true;
  }

  bool visitNominalTypeRef(const NominalTypeRef *N) {
    return true;
  }

  bool visitBoundGenericTypeRef(const BoundGenericTypeRef *BG) {
    if (BG->isClass())
      return true;
    for (auto Arg : BG->getGenericParams()) {
      if (!visit(Arg))
        return false;
    }
    return true;
  }

  bool visitTupleTypeRef(const TupleTypeRef *T) {
    for (auto Element : T->getElements())
      if (!visit(Element))
        return false;
    return true;
  }

  bool visitFunctionTypeRef(const FunctionTypeRef *F) {
    return true;
  }

  bool
  visitProtocolCompositionTypeRef(const ProtocolCompositionTypeRef *PC) {
    return true;
  }

  bool visitMetatypeTypeRef(const MetatypeTypeRef *M) {
    return true;
  }

  bool
  visitExistentialMetatypeTypeRef(const ExistentialMetatypeTypeRef *EM) {
    return true;
  }

  bool
  visitConstrainedExistentialTypeRef(const ConstrainedExistentialTypeRef *CET) {
    return true;
  }

  bool
  visitSILBoxTypeRef(const SILBoxTypeRef *SB) {
    return true;
  }

  bool visitSILBoxTypeWithLayoutTypeRef(const SILBoxTypeWithLayoutTypeRef *SB) {
    return true;
  }

  bool
  visitForeignClassTypeRef(const ForeignClassTypeRef *F) {
    return true;
  }

  bool visitObjCClassTypeRef(const ObjCClassTypeRef *OC) {
    return true;
  }

  bool visitObjCProtocolTypeRef(const ObjCProtocolTypeRef *OP) {
    return true;
  }

#define REF_STORAGE(Name, ...) \
  bool \
  visit##Name##StorageTypeRef(const Name##StorageTypeRef *US) { \
    return true; \
  }
#include "swift/AST/ReferenceStorage.def"

  bool
  visitGenericTypeParameterTypeRef(const GenericTypeParameterTypeRef *GTP) {
    return false;
  }

  bool
  visitDependentMemberTypeRef(const DependentMemberTypeRef *DM) {
    return false;
  }

  bool visitOpaqueTypeRef(const OpaqueTypeRef *O) {
    return false;
  }

  bool visitOpaqueArchetypeTypeRef(const OpaqueArchetypeTypeRef *O) {
    return false;
  }
};

bool TypeConverter::hasFixedSize(const TypeRef *TR) {
  return HasFixedSize().visit(TR);
}

MetatypeRepresentation combineRepresentations(MetatypeRepresentation rep1,
                                              MetatypeRepresentation rep2) {
  if (rep1 == rep2)
    return rep1;

  if (rep1 == MetatypeRepresentation::Unknown ||
      rep2 == MetatypeRepresentation::Unknown)
    return MetatypeRepresentation::Unknown;

  if (rep1 == MetatypeRepresentation::Thick ||
      rep2 == MetatypeRepresentation::Thick)
    return MetatypeRepresentation::Thick;

  return MetatypeRepresentation::Thin;
}

/// Visitor class to determine if a metatype should use the empty
/// representation.
///
/// This relies on substitution correctly setting wasAbstract() on
/// MetatypeTypeRefs.
class HasSingletonMetatype
  : public TypeRefVisitor<HasSingletonMetatype, MetatypeRepresentation> {

public:
  HasSingletonMetatype() {}

  using TypeRefVisitor<HasSingletonMetatype, MetatypeRepresentation>::visit;

  MetatypeRepresentation visitBuiltinTypeRef(const BuiltinTypeRef *B) {
    return MetatypeRepresentation::Thin;
  }

  MetatypeRepresentation visitNominalTypeRef(const NominalTypeRef *N) {
    if (N->isClass())
      return MetatypeRepresentation::Thick;
    return MetatypeRepresentation::Thin;
  }

  MetatypeRepresentation visitBoundGenericTypeRef(const BoundGenericTypeRef *BG) {
    if (BG->isClass())
      return MetatypeRepresentation::Thick;
    return MetatypeRepresentation::Thin;
  }

  MetatypeRepresentation visitTupleTypeRef(const TupleTypeRef *T) {
    auto result = MetatypeRepresentation::Thin;
    for (auto Element : T->getElements())
      result = combineRepresentations(result, visit(Element));
    return result;
  }

  MetatypeRepresentation visitFunctionTypeRef(const FunctionTypeRef *F) {
    auto result = visit(F->getResult());
    for (const auto &Param : F->getParameters())
      result = combineRepresentations(result, visit(Param.getType()));
    return result;
  }

  MetatypeRepresentation
  visitProtocolCompositionTypeRef(const ProtocolCompositionTypeRef *PC) {
    return MetatypeRepresentation::Thin;
  }

  MetatypeRepresentation
  visitConstrainedExistentialTypeRef(const ConstrainedExistentialTypeRef *CET) {
    return MetatypeRepresentation::Thin;
  }

  MetatypeRepresentation visitMetatypeTypeRef(const MetatypeTypeRef *M) {
    if (M->wasAbstract())
      return MetatypeRepresentation::Thick;
    return visit(M->getInstanceType());
  }

  MetatypeRepresentation
  visitExistentialMetatypeTypeRef(const ExistentialMetatypeTypeRef *EM) {
    return MetatypeRepresentation::Thin;
  }

  MetatypeRepresentation
  visitSILBoxTypeRef(const SILBoxTypeRef *SB) {
    return MetatypeRepresentation::Thin;
  }

  MetatypeRepresentation
  visitSILBoxTypeWithLayoutTypeRef(const SILBoxTypeWithLayoutTypeRef *SB) {
    return MetatypeRepresentation::Thin;
  }

  MetatypeRepresentation
  visitGenericTypeParameterTypeRef(const GenericTypeParameterTypeRef *GTP) {
    DEBUG_LOG(fprintf(stderr, "Unresolved generic TypeRef: "); GTP->dump());
    return MetatypeRepresentation::Unknown;
  }

  MetatypeRepresentation
  visitDependentMemberTypeRef(const DependentMemberTypeRef *DM) {
    DEBUG_LOG(fprintf(stderr, "Unresolved generic TypeRef: "); DM->dump());
    return MetatypeRepresentation::Unknown;
  }

  MetatypeRepresentation
  visitForeignClassTypeRef(const ForeignClassTypeRef *F) {
    return MetatypeRepresentation::Unknown;
  }

  MetatypeRepresentation visitObjCClassTypeRef(const ObjCClassTypeRef *OC) {
    return MetatypeRepresentation::Unknown;
  }

  MetatypeRepresentation visitObjCProtocolTypeRef(const ObjCProtocolTypeRef *OP) {
    return MetatypeRepresentation::Unknown;
  }

#define REF_STORAGE(Name, ...) \
  MetatypeRepresentation \
  visit##Name##StorageTypeRef(const Name##StorageTypeRef *US) { \
    return MetatypeRepresentation::Unknown; \
  }
#include "swift/AST/ReferenceStorage.def"

  MetatypeRepresentation visitOpaqueTypeRef(const OpaqueTypeRef *O) {
    return MetatypeRepresentation::Unknown;
  }

  MetatypeRepresentation visitOpaqueArchetypeTypeRef(const OpaqueArchetypeTypeRef *O) {
    return MetatypeRepresentation::Unknown;
  }
};

class EnumTypeInfoBuilder {
  TypeConverter &TC;
  unsigned Size, Alignment, NumExtraInhabitants;
  bool BitwiseTakable;
  std::vector<FieldInfo> Cases;
  bool Invalid;

  const TypeRef *getCaseTypeRef(FieldTypeInfo Case) {
    // An indirect case is like a payload case with an argument type
    // of Builtin.NativeObject.
    if (Case.Indirect)
      return TC.getNativeObjectTypeRef();

    return Case.TR;
  }

  void addCase(const std::string &Name) {
    // FieldInfo's TI field is a reference, so give it a reference to a value
    // that stays alive forever.
    static TypeInfo emptyTI;
    Cases.push_back({Name, /*offset=*/0, /*value=*/-1, nullptr, emptyTI});
  }

  void addCase(const std::string &Name, const TypeRef *TR,
               const TypeInfo *TI) {
    if (TI == nullptr) {
      DEBUG_LOG(fprintf(stderr, "No TypeInfo for case type: "); TR->dump());
      Invalid = true;
      static TypeInfo emptyTI;
      Cases.push_back({Name, /*offset=*/0, /*value=*/-1, TR, emptyTI});
    } else {
      Size = std::max(Size, TI->getSize());
      Alignment = std::max(Alignment, TI->getAlignment());
      BitwiseTakable &= TI->isBitwiseTakable();
      Cases.push_back({Name, /*offset=*/0, /*value=*/-1, TR, *TI});
    }
  }

public:
  EnumTypeInfoBuilder(TypeConverter &TC)
    : TC(TC), Size(0), Alignment(1), NumExtraInhabitants(0),
      BitwiseTakable(true), Invalid(false) {}

  const TypeInfo *build(const TypeRef *TR, RemoteRef<FieldDescriptor> FD,
                        remote::TypeInfoProvider *ExternalTypeInfo) {
    // Count various categories of cases:
    unsigned NonPayloadCases = 0; // `case a`
    unsigned NonGenericEmptyPayloadCases = 0; // `case a(Void)` or `case b(Never)`
    unsigned NonGenericNonEmptyPayloadCases = 0; // `case a(Int)` or `case d([Int?])`
    unsigned GenericPayloadCases = 0; // `case a(T)` or `case a([String : (Int, T)])`

    // For a single-payload enum, this is the only payload
    const TypeRef *LastPayloadCaseTR = nullptr;

    std::vector<FieldTypeInfo> Fields;
    if (!TC.getBuilder().getFieldTypeRefs(TR, FD, ExternalTypeInfo, Fields)) {
      Invalid = true;
      return nullptr;
    }

    for (auto Case : Fields) {
      if (Case.TR == nullptr) {
        ++NonPayloadCases;
        addCase(Case.Name);
      } else {
        auto *CaseTR = getCaseTypeRef(Case);
        assert(CaseTR != nullptr);
        auto *CaseTI = TC.getTypeInfo(CaseTR, ExternalTypeInfo);
        if (CaseTI == nullptr) {
          // We don't have typeinfo; something is very broken.
          Invalid = true;
          return nullptr;
        } else if (Case.Generic) {
          ++GenericPayloadCases;
          LastPayloadCaseTR = CaseTR;
        } else if (CaseTI->getSize() == 0) {
          ++NonGenericEmptyPayloadCases;
        } else {
          ++NonGenericNonEmptyPayloadCases;
          LastPayloadCaseTR = CaseTR;
        }
        addCase(Case.Name, CaseTR, CaseTI);
      }
    }
    // For determining a layout strategy, cases w/ empty payload are treated the
    // same as cases with no payload, and generic cases are always considered
    // non-empty.
    unsigned EffectiveNoPayloadCases = NonPayloadCases + NonGenericEmptyPayloadCases;
    unsigned EffectivePayloadCases = GenericPayloadCases + NonGenericNonEmptyPayloadCases;

    if (Cases.empty()) {
      return TC.makeTypeInfo<EmptyEnumTypeInfo>(Cases);
    }

    // `Kind` is used when dumping data, so it reflects how the enum was
    // declared in source; the various *TypeInfo classes mentioned below reflect
    // the in-memory layout, which may be different because non-generic cases
    // with zero-sized payloads get treated for layout purposes as non-payload
    // cases.
    EnumKind Kind;
    switch (GenericPayloadCases + NonGenericEmptyPayloadCases + NonGenericNonEmptyPayloadCases) {
    case 0: Kind = EnumKind::NoPayloadEnum; break;
    case 1: Kind = EnumKind::SinglePayloadEnum; break;
    default: Kind = EnumKind::MultiPayloadEnum; break;
    }

    if (Cases.size() == 1) {
      if (EffectivePayloadCases == 0) {
        // Zero-sized enum with only one empty case
        return TC.makeTypeInfo<TrivialEnumTypeInfo>(Kind, Cases);
      } else {
        // Enum that has only one payload case is represented as that case
        return TC.getTypeInfo(LastPayloadCaseTR, ExternalTypeInfo);
      }
    }

    if (EffectivePayloadCases == 0) {
      // Enum with no non-empty payloads.  (It may
      // formally be a single-payload or multi-payload enum,
      // but all the actual payloads have zero size.)

      // Represent it as a 1-, 2-, or 4-byte integer
      unsigned Size, NumExtraInhabitants;
      if (EffectiveNoPayloadCases < 256) {
        Size = 1;
        NumExtraInhabitants = 256 - EffectiveNoPayloadCases;
      } else if (EffectiveNoPayloadCases < 65536) {
        Size = 2;
        NumExtraInhabitants = 65536 - EffectiveNoPayloadCases;
      } else {
        Size = 4;
        NumExtraInhabitants = std::numeric_limits<uint32_t>::max() - EffectiveNoPayloadCases + 1;
      }
      if (NonGenericEmptyPayloadCases > 0) {
        // This enum uses no-payload layout, but the source actually does
        // have payloads (they're just all zero-sized).
        // If this is really a single-payload or multi-payload enum, we
        // formally take extra inhabitants from the first payload, which is
        // zero sized in this case.
        NumExtraInhabitants = 0;
      }
      if (NumExtraInhabitants > ValueWitnessFlags::MaxNumExtraInhabitants) {
        NumExtraInhabitants = ValueWitnessFlags::MaxNumExtraInhabitants;
      }
      return TC.makeTypeInfo<NoPayloadEnumTypeInfo>(
        /* Size */ Size, /* Alignment */ Size, /* Stride */ Size,
        NumExtraInhabitants, Kind, Cases);
    }

    if (EffectivePayloadCases == 1) {
      // SinglePayloadEnumImplStrategy

      // This is a true single-payload enum with
      // a single non-zero-sized payload, or an MPE
      // with a single payload that is not statically empty.
      // It also has at least one non-payload (or empty) case.

      auto *CaseTR = LastPayloadCaseTR;
      auto *CaseTI = TC.getTypeInfo(CaseTR, ExternalTypeInfo);
      if (CaseTR == nullptr || CaseTI == nullptr) {
        return nullptr;
      }
      // Below logic should match the runtime function
      // swift_initEnumMetadataSinglePayload().
      auto PayloadExtraInhabitants = CaseTI->getNumExtraInhabitants();
      if (PayloadExtraInhabitants >= EffectiveNoPayloadCases) {
        // Extra inhabitants can encode all no-payload cases.
        NumExtraInhabitants = PayloadExtraInhabitants - EffectiveNoPayloadCases;
      } else {
        // Not enough extra inhabitants for all cases. We have to add an
        // extra tag field.
        NumExtraInhabitants = 0;
        auto tagCounts = getEnumTagCounts(Size, EffectiveNoPayloadCases,
                                          /*payloadCases=*/1);
        Size += tagCounts.numTagBytes;
        Alignment = std::max(Alignment, tagCounts.numTagBytes);
      }
      unsigned Stride = ((Size + Alignment - 1) & ~(Alignment - 1));
      return TC.makeTypeInfo<SinglePayloadEnumTypeInfo>(
        Size, Alignment, Stride, NumExtraInhabitants, BitwiseTakable, Kind, Cases);

    }

    //
    // Multi-Payload Enum strategies
    //
    // We now know this is a multi-payload enum with at least one non-zero-sized
    // payload case.
    //

    // Do we have a fixed layout?
    // TODO: Test whether a missing FixedDescriptor is actually relevant.
    auto FixedDescriptor = TC.getBuilder().getBuiltinTypeInfo(TR);
    if (!FixedDescriptor || GenericPayloadCases > 0) {
      // This is a "dynamic multi-payload enum".  For example,
      // this occurs with:
      // ```
      // class ClassWithEnum<T> {
      //   enum E {
      //   case t(T)
      //   case u(Int)
      //   }
      //   var e: E?
      // }
      // ```
      auto tagCounts = getEnumTagCounts(Size, EffectiveNoPayloadCases,
                                        EffectivePayloadCases);
      Size += tagCounts.numTagBytes;
      if (tagCounts.numTagBytes >= 4) {
        NumExtraInhabitants = ValueWitnessFlags::MaxNumExtraInhabitants;
      } else {
        NumExtraInhabitants =
          (1 << (tagCounts.numTagBytes * 8)) - tagCounts.numTags;
        if (NumExtraInhabitants > ValueWitnessFlags::MaxNumExtraInhabitants) {
          NumExtraInhabitants = ValueWitnessFlags::MaxNumExtraInhabitants;
        }
      }
      unsigned Stride = ((Size + Alignment - 1) & ~(Alignment - 1));
      if (Stride == 0)
        Stride = 1;
      return TC.makeTypeInfo<TaggedMultiPayloadEnumTypeInfo>(
        Size, Alignment, Stride, NumExtraInhabitants,
        BitwiseTakable, Cases, EffectivePayloadCases);
    }

    // This is a multi-payload enum that:
    //  * Has no generic cases
    //  * Has at least two cases with non-zero payload size
    //  * Has a descriptor stored as BuiltinTypeInfo
    Size = FixedDescriptor->Size;
    Alignment = FixedDescriptor->getAlignment();
    NumExtraInhabitants = FixedDescriptor->NumExtraInhabitants;
    BitwiseTakable = FixedDescriptor->isBitwiseTakable();
    unsigned Stride = ((Size + Alignment - 1) & ~(Alignment - 1));
    if (Stride == 0)
      Stride = 1;
    auto PayloadSize = EnumTypeInfo::getPayloadSizeForCases(Cases);

    // If there's a multi-payload enum descriptor, then we
    // have spare bits information from the compiler.

    // Uncomment the following line to dump the MPE section every time we come through here...
    //TC.getBuilder().dumpMultiPayloadEnumSection(std::cerr); // DEBUG helper

    auto MPEDescriptor = TC.getBuilder().getMultiPayloadEnumInfo(TR);
    if (MPEDescriptor && MPEDescriptor->usesPayloadSpareBits()) {
      auto PayloadSpareBitMaskByteCount = MPEDescriptor->getPayloadSpareBitMaskByteCount();
      auto PayloadSpareBitMaskByteOffset = MPEDescriptor->getPayloadSpareBitMaskByteOffset();
      auto SpareBitMask = MPEDescriptor->getPayloadSpareBits();
      BitMask spareBitsMask(PayloadSize, SpareBitMask,
                            PayloadSpareBitMaskByteCount, PayloadSpareBitMaskByteOffset);
      
      if (spareBitsMask.isZero()) {
        // If there are no spare bits, use the "simple" tag-only implementation.
        return TC.makeTypeInfo<TaggedMultiPayloadEnumTypeInfo>(
          Size, Alignment, Stride, NumExtraInhabitants,
          BitwiseTakable, Cases, EffectivePayloadCases);
      }

#if 0  // TODO: This should be !defined(NDEBUG)
      // DEBUG verification that compiler mask and locally-computed
      // mask are the same (whenever both are available).
      BitMask locallyComputedSpareBitsMask(PayloadSize);
      auto mpePointerSpareBits = TC.getBuilder().getMultiPayloadEnumPointerMask();
      auto locallyComputedSpareBitsMaskIsValid
        = populateSpareBitsMask(Cases, locallyComputedSpareBitsMask, mpePointerSpareBits);
      // If the local computation were always correct, we could:
      // assert(locallyComputedSpareBitsMaskIsValid);
      if (locallyComputedSpareBitsMaskIsValid) {
        // Whenever the compiler and local computation both produce
        // data, they should agree.
        // TODO: Make this true, then change `#if 0` above
        assert(locallyComputedSpareBitsMask == spareBitsMask);
      }
#endif

      // Use compiler-provided spare bit information
      return TC.makeTypeInfo<MultiPayloadEnumTypeInfo>(
        Size, Alignment, Stride, NumExtraInhabitants,
        BitwiseTakable, Cases, spareBitsMask,
        EffectivePayloadCases);
    }

    // Either there was no compiler data or it didn't make sense
    // (existed but claimed to have no mask).
    // Try computing the mask ourselves: This is less robust, but necessary to
    // support images from older compilers.
    BitMask spareBitsMask(PayloadSize);
    auto mpePointerSpareBits = TC.getBuilder().getMultiPayloadEnumPointerMask();
    auto validSpareBitsMask = populateSpareBitsMask(Cases, spareBitsMask, mpePointerSpareBits);
    // For DEBUGGING, disable fallback to local computation to
    // make missing compiler data more obvious:
    // validSpareBitsMask = false;
    if (!validSpareBitsMask) {
      // If we couldn't correctly determine the spare bits mask,
      // return a TI that will always fail when asked for XIs or value.
      return TC.makeTypeInfo<UnsupportedEnumTypeInfo>(
        Size, Alignment, Stride, NumExtraInhabitants,
        BitwiseTakable, EnumKind::MultiPayloadEnum, Cases);
    } else if (spareBitsMask.isZero()) {
      // Simple case that does not use spare bits
      // This is correct as long as our local spare bits calculation
      // above only returns an empty mask when the mask is really empty,
      return TC.makeTypeInfo<TaggedMultiPayloadEnumTypeInfo>(
        Size, Alignment, Stride, NumExtraInhabitants,
        BitwiseTakable, Cases, EffectivePayloadCases);
    } else {
      // General case can mix spare bits and extra discriminator
      // It obviously relies on having an accurate spare bit mask.
      return TC.makeTypeInfo<MultiPayloadEnumTypeInfo>(
        Size, Alignment, Stride, NumExtraInhabitants,
        BitwiseTakable, Cases, spareBitsMask,
        EffectivePayloadCases);
    }
  }
};

class LowerType
  : public TypeRefVisitor<LowerType, const TypeInfo *> {
  TypeConverter &TC;
  remote::TypeInfoProvider *ExternalTypeInfo;

public:
  using TypeRefVisitor<LowerType, const TypeInfo *>::visit;

  LowerType(TypeConverter &TC, remote::TypeInfoProvider *ExternalTypeInfo)
      : TC(TC), ExternalTypeInfo(ExternalTypeInfo) {}

  const TypeInfo *visitBuiltinTypeRef(const BuiltinTypeRef *B) {
    /// The context field of a thick function is a Builtin.NativeObject.
    /// Since we want this to round-trip, lower these as reference
    /// types.
    if (B->getMangledName() == "Bo") {
      return TC.getReferenceTypeInfo(ReferenceKind::Strong,
                                     ReferenceCounting::Native);
    } else if (B->getMangledName() == "BO") {
      return TC.getReferenceTypeInfo(ReferenceKind::Strong,
                                     ReferenceCounting::Unknown);
    }

    /// Otherwise, get the fixed layout information from reflection
    /// metadata.
    auto descriptor = TC.getBuilder().getBuiltinTypeInfo(B);
    if (descriptor == nullptr) {
      DEBUG_LOG(fprintf(stderr, "No TypeInfo for builtin type: "); B->dump());
      return nullptr;
    }
    return TC.makeTypeInfo<BuiltinTypeInfo>(TC.getBuilder(), descriptor);
  }

  const TypeInfo *visitAnyNominalTypeRef(const TypeRef *TR) {
    auto QueryExternalTypeInfoProvider = [&]() -> const TypeInfo * {
      if (ExternalTypeInfo) {
        std::string MangledName;
        if (auto N = dyn_cast<NominalTypeRef>(TR))
          MangledName = N->getMangledName();
        else if (auto BG = dyn_cast<BoundGenericTypeRef>(TR))
          MangledName = BG->getMangledName();
        if (!MangledName.empty())
          if (auto *imported = ExternalTypeInfo->getTypeInfo(MangledName))
            return imported;
      }
      return nullptr;
    };

    auto FD = TC.getBuilder().getFieldTypeInfo(TR);
    if (FD == nullptr || FD->isStruct()) {
      // Maybe this type is opaque -- look for a builtin
      // descriptor to see if we at least know its size
      // and alignment.
      if (auto ImportedTypeDescriptor =
              TC.getBuilder().getBuiltinTypeInfo(TR)) {
        // This might be an external type we treat as opaque (like C structs),
        // the external type info provider might have better type information,
        // so ask it first.
        if (auto External = QueryExternalTypeInfoProvider())
          return External;

        return TC.makeTypeInfo<BuiltinTypeInfo>(TC.getBuilder(),
                                                ImportedTypeDescriptor);
      }

      if (FD == nullptr) {
        // If we still have no type info ask the external provider.
        if (auto External = QueryExternalTypeInfoProvider())
          return External;

        // If the external provider also fails we're out of luck.
        DEBUG_LOG(fprintf(stderr, "No TypeInfo for nominal type: "); TR->dump());
        return nullptr;
      }
    }

    switch (FD->Kind) {
    case FieldDescriptorKind::Class:
      // A value of class type is a single retainable pointer.
      return TC.getReferenceTypeInfo(ReferenceKind::Strong,
                                     ReferenceCounting::Native);
    case FieldDescriptorKind::Struct: {
      // Lower the struct's fields using substitutions from the
      // TypeRef to make field types concrete.
      RecordTypeInfoBuilder builder(TC, RecordKind::Struct);

      std::vector<FieldTypeInfo> Fields;
      if (!TC.getBuilder().getFieldTypeRefs(TR, FD, ExternalTypeInfo, Fields))
        return nullptr;

      for (auto Field : Fields)
        builder.addField(Field.Name, Field.TR, ExternalTypeInfo);
      return builder.build();
    }
    case FieldDescriptorKind::Enum:
    case FieldDescriptorKind::MultiPayloadEnum: {
      EnumTypeInfoBuilder builder(TC);
      return builder.build(TR, FD, ExternalTypeInfo);
    }
    case FieldDescriptorKind::ObjCClass:
      return TC.getReferenceTypeInfo(ReferenceKind::Strong,
                                     ReferenceCounting::Unknown);
    case FieldDescriptorKind::ObjCProtocol:
    case FieldDescriptorKind::ClassProtocol:
    case FieldDescriptorKind::Protocol:
      DEBUG_LOG(fprintf(stderr, "Invalid field descriptor: "); TR->dump());
      return nullptr;
    }

    swift_unreachable("Unhandled FieldDescriptorKind in switch.");
  }

  const TypeInfo *visitNominalTypeRef(const NominalTypeRef *N) {
    return visitAnyNominalTypeRef(N);
  }

  const TypeInfo *visitBoundGenericTypeRef(const BoundGenericTypeRef *BG) {
    return visitAnyNominalTypeRef(BG);
  }

  const TypeInfo *visitTupleTypeRef(const TupleTypeRef *T) {
    RecordTypeInfoBuilder builder(TC, RecordKind::Tuple);
    for (auto Element : T->getElements())
      // The label is not going to be relevant/harmful for looking up type info.
      builder.addField("", Element, ExternalTypeInfo);
    return builder.build();
  }

  const TypeInfo *visitFunctionTypeRef(const FunctionTypeRef *F) {
    switch (F->getFlags().getConvention()) {
    case FunctionMetadataConvention::Swift:
      return TC.getThickFunctionTypeInfo();
    case FunctionMetadataConvention::Block:
      // FIXME: Native convention if blocks are ever supported on Linux?
      return TC.getReferenceTypeInfo(ReferenceKind::Strong,
                                     ReferenceCounting::Unknown);
    case FunctionMetadataConvention::Thin:
    case FunctionMetadataConvention::CFunctionPointer:
      return TC.getTypeInfo(TC.getThinFunctionTypeRef(), ExternalTypeInfo);
    }

    swift_unreachable("Unhandled FunctionMetadataConvention in switch.");
  }

  const TypeInfo *
  visitProtocolCompositionTypeRef(const ProtocolCompositionTypeRef *PC) {
    ExistentialTypeInfoBuilder builder(TC);
    builder.addProtocolComposition(PC);
    return builder.build(ExternalTypeInfo);
  }

  const TypeInfo *
  visitConstrainedExistentialTypeRef(const ConstrainedExistentialTypeRef *CET) {
    return visitProtocolCompositionTypeRef(CET->getBase());
  }

  const TypeInfo *visitMetatypeTypeRef(const MetatypeTypeRef *M) {
    switch (HasSingletonMetatype().visit(M)) {
    case MetatypeRepresentation::Unknown:
      DEBUG_LOG(fprintf(stderr, "Unknown metatype representation: "); M->dump());
      return nullptr;
    case MetatypeRepresentation::Thin:
      return TC.getEmptyTypeInfo();
    case MetatypeRepresentation::Thick:
      return TC.getTypeInfo(TC.getAnyMetatypeTypeRef(), ExternalTypeInfo);
    }

    swift_unreachable("Unhandled MetatypeRepresentation in switch.");
  }

  const TypeInfo *
  visitExistentialMetatypeTypeRef(const ExistentialMetatypeTypeRef *EM) {
    ExistentialTypeInfoBuilder builder(TC);
    auto *TR = EM->getInstanceType();

    if (auto *PC = dyn_cast<ProtocolCompositionTypeRef>(TR)) {
      builder.addProtocolComposition(PC);
    } else {
      DEBUG_LOG(fprintf(stderr, "Invalid existential metatype: "); EM->dump());
      return nullptr;
    }

    return builder.buildMetatype(ExternalTypeInfo);
  }

  const TypeInfo *
  visitGenericTypeParameterTypeRef(const GenericTypeParameterTypeRef *GTP) {
    DEBUG_LOG(fprintf(stderr, "Unresolved generic TypeRef: "); GTP->dump());
    return nullptr;
  }

  const TypeInfo *
  visitDependentMemberTypeRef(const DependentMemberTypeRef *DM) {
    DEBUG_LOG(fprintf(stderr, "Unresolved generic TypeRef: "); DM->dump());
    return nullptr;
  }

  const TypeInfo *visitForeignClassTypeRef(const ForeignClassTypeRef *F) {
    return TC.getReferenceTypeInfo(ReferenceKind::Strong,
                                   ReferenceCounting::Unknown);
  }

  const TypeInfo *visitObjCClassTypeRef(const ObjCClassTypeRef *OC) {
    return TC.getReferenceTypeInfo(ReferenceKind::Strong,
                                   ReferenceCounting::Unknown);
  }

  const TypeInfo *visitObjCProtocolTypeRef(const ObjCProtocolTypeRef *OP) {
    return TC.getReferenceTypeInfo(ReferenceKind::Strong,
                                   ReferenceCounting::Unknown);
  }

  // Apply a storage qualifier, like 'weak', 'unowned' or 'unowned(unsafe)'
  // to a type with reference semantics, such as a class reference or
  // class-bound existential.
  const TypeInfo *
  rebuildStorageTypeInfo(const TypeInfo *TI, ReferenceKind Kind) {
    // If we can't lower the original storage type, give up.
    if (TI == nullptr) {
      DEBUG_LOG(fprintf(stderr, "Invalid reference type"));
      return nullptr;
    }

    // Simple case: Just change the reference kind
    if (auto *ReferenceTI = dyn_cast<ReferenceTypeInfo>(TI))
      return TC.getReferenceTypeInfo(Kind, ReferenceTI->getReferenceCounting());

    if (auto *EnumTI = dyn_cast<EnumTypeInfo>(TI)) {
      if (EnumTI->isOptional() &&
          (Kind == ReferenceKind::Weak || Kind == ReferenceKind::Unowned ||
           Kind == ReferenceKind::Unmanaged)) {
        auto *TI = TC.getTypeInfo(EnumTI->getCases()[0].TR, ExternalTypeInfo);
        return rebuildStorageTypeInfo(TI, Kind);
      }
    }

    if (auto *RecordTI = dyn_cast<RecordTypeInfo>(TI)) {
      auto SubKind = RecordTI->getRecordKind();
      // Class existentials are represented as record types.
      // Destructure the existential and replace the "object"
      // field with the right reference kind.
      if (SubKind == RecordKind::ClassExistential) {
        bool BitwiseTakable = RecordTI->isBitwiseTakable();
        std::vector<FieldInfo> Fields;
        for (auto &Field : RecordTI->getFields()) {
          if (Field.Name == "object") {
            auto *FieldTI = rebuildStorageTypeInfo(&Field.TI, Kind);
            BitwiseTakable &= FieldTI->isBitwiseTakable();
            Fields.push_back({Field.Name, Field.Offset, /*value=*/-1, Field.TR, *FieldTI});
            continue;
          }
          Fields.push_back(Field);
        }

        return TC.makeTypeInfo<RecordTypeInfo>(
            RecordTI->getSize(),
            RecordTI->getAlignment(),
            RecordTI->getStride(),
            RecordTI->getNumExtraInhabitants(),
            BitwiseTakable,
            SubKind, Fields);
      }
    }

    // Anything else -- give up
    DEBUG_LOG(fprintf(stderr, "Invalid reference type"));
    return nullptr;
  }

  const TypeInfo *
  visitAnyStorageTypeRef(const TypeRef *TR, ReferenceKind Kind) {
    return rebuildStorageTypeInfo(TC.getTypeInfo(TR, ExternalTypeInfo), Kind);
  }

#define REF_STORAGE(Name, name, ...) \
  const TypeInfo * \
  visit##Name##StorageTypeRef(const Name##StorageTypeRef *US) { \
    return visitAnyStorageTypeRef(US->getType(), ReferenceKind::Name); \
  }
#include "swift/AST/ReferenceStorage.def"

  const TypeInfo *visitSILBoxTypeRef(const SILBoxTypeRef *SB) {
    return TC.getReferenceTypeInfo(ReferenceKind::Strong,
                                   ReferenceCounting::Native);
  }

  const TypeInfo *
  visitSILBoxTypeWithLayoutTypeRef(const SILBoxTypeWithLayoutTypeRef *SB) {
    return TC.getReferenceTypeInfo(ReferenceKind::Strong,
                                   ReferenceCounting::Native);
  }

  const TypeInfo *visitOpaqueTypeRef(const OpaqueTypeRef *O) {
    DEBUG_LOG(fprintf(stderr, "Can't lower opaque TypeRef"));
    return nullptr;
  }

  const TypeInfo *visitOpaqueArchetypeTypeRef(const OpaqueArchetypeTypeRef *O) {
    // TODO: Provide a hook for the client to try to resolve the opaque archetype
    // with additional information?
    DEBUG_LOG(fprintf(stderr, "Can't lower unresolved opaque archetype TypeRef"));
    return nullptr;
  }
};

const TypeInfo *
TypeConverter::getTypeInfo(const TypeRef *TR,
                           remote::TypeInfoProvider *ExternalTypeInfo) {
  if (!TR) {
    DEBUG_LOG(fprintf(stderr, "null TypeRef"));
    return nullptr;
  }

  auto ExternalTypeInfoId =
      ExternalTypeInfo ? ExternalTypeInfo->getId() : 0;
  // See if we already computed the result
  auto found = Cache.find({TR, ExternalTypeInfoId});
  if (found != Cache.end())
    return found->second;

  // Detect invalid recursive value types (IRGen should not emit
  // them in the first place, but there might be bugs)
  if (!RecursionCheck.insert(TR).second) {
    DEBUG_LOG(fprintf(stderr, "TypeRef recursion detected"));
    return nullptr;
  }

  // Compute the result and cache it
  auto *TI = LowerType(*this, ExternalTypeInfo).visit(TR);
  Cache.insert({{TR, ExternalTypeInfoId}, TI});

  RecursionCheck.erase(TR);

  return TI;
}

const RecordTypeInfo *TypeConverter::getClassInstanceTypeInfo(
    const TypeRef *TR, unsigned start,
    remote::TypeInfoProvider *ExternalTypeInfo) {
  auto FD = getBuilder().getFieldTypeInfo(TR);
  if (FD == nullptr) {
    DEBUG_LOG(fprintf(stderr, "No field descriptor: "); TR->dump());
    return nullptr;
  }

  switch (FD->Kind) {
  case FieldDescriptorKind::Class:
  case FieldDescriptorKind::ObjCClass: {
    // Lower the class's fields using substitutions from the
    // TypeRef to make field types concrete.
    RecordTypeInfoBuilder builder(*this, RecordKind::ClassInstance);

    std::vector<FieldTypeInfo> Fields;
    if (!getBuilder().getFieldTypeRefs(TR, FD, ExternalTypeInfo, Fields))
      return nullptr;

    // Start layout from the given instance start offset. This should
    // be the superclass instance size.
    builder.addField(/*size=*/start,
                     /*alignment=*/1,
                     /*numExtraInhabitants=*/0,
                     /*bitwiseTakable=*/true);

    for (auto Field : Fields)
      builder.addField(Field.Name, Field.TR, ExternalTypeInfo);
    return builder.build();
  }
  case FieldDescriptorKind::Struct:
  case FieldDescriptorKind::Enum:
  case FieldDescriptorKind::MultiPayloadEnum:
  case FieldDescriptorKind::ObjCProtocol:
  case FieldDescriptorKind::ClassProtocol:
  case FieldDescriptorKind::Protocol:
    // Invalid field descriptor.
    DEBUG_LOG(fprintf(stderr, "Invalid field descriptor: "); TR->dump());
    return nullptr;
  }

  swift_unreachable("Unhandled FieldDescriptorKind in switch.");
}

} // namespace reflection
} // namespace swift

#endif
