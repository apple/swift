//===--- Records.h - Swift Type Reflection Records --------------*- C++ -*-===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2016 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See http://swift.org/LICENSE.txt for license information
// See http://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//
//
// Implements the structures of type reflection records.
//
//===----------------------------------------------------------------------===//

#ifndef SWIFT_REFLECTION_RECORDS_H
#define SWIFT_REFLECTION_RECORDS_H

#include "swift/Basic/RelativePointer.h"

namespace swift {
namespace reflection {

// Field records describe the type of a single stored property or case member
// of a class, struct or enum.
class FieldRecordFlags {
  using int_type = uint32_t;
  enum : int_type {
    IsObjC = 0x00000001,
  };
  int_type Data;
public:
  bool isObjC() const {
    return Data & IsObjC;
  }

  void setIsObjC(bool ObjC) {
    if (ObjC)
      Data |= IsObjC;
    else
      Data &= ~IsObjC;
  }

  int_type getRawValue() const {
    return Data;
  }
};

class FieldRecord {
  const FieldRecordFlags Flags;
  const RelativeDirectPointer<const char> MangledTypeName;
  const RelativeDirectPointer<const char> FieldName;

public:
  FieldRecord() = delete;

  bool hasMangledTypeName() const {
    return MangledTypeName;
  }

  std::string getMangledTypeName() const {
    return MangledTypeName.get();
  }

  std::string getFieldName()  const {
    if (FieldName)
      return FieldName.get();
    return "";
  }

  bool isObjC() const {
    return Flags.isObjC();
  }
};

struct FieldRecordIterator {
  const FieldRecord *Cur;
  const FieldRecord * const End;

  FieldRecordIterator(const FieldRecord *Cur, const FieldRecord * const End)
    : Cur(Cur), End(End) {}

  const FieldRecord &operator*() const {
    return *Cur;
  }

  FieldRecordIterator &operator++() {
    ++Cur;
    return *this;
  }

  bool operator==(const FieldRecordIterator &other) const {
    return Cur == other.Cur && End == other.End;
  }

  bool operator!=(const FieldRecordIterator &other) const {
    return !(*this == other);
  }
};

enum class FieldDescriptorKind : uint16_t {
  Struct,
  Class,
  Enum
};

// Field descriptors contain a collection of field records for a single
// class, struct or enum declaration.
class FieldDescriptor {
  const FieldRecord *getFieldRecordBuffer() const {
    return reinterpret_cast<const FieldRecord *>(this + 1);
  }

  const RelativeDirectPointer<const char> MangledTypeName;

public:
  FieldDescriptor() = delete;

  const FieldDescriptorKind Kind;
  const uint16_t FieldRecordSize;
  const uint32_t NumFields;

  using const_iterator = FieldRecordIterator;

  const_iterator begin() const {
    auto Begin = getFieldRecordBuffer();
    auto End = Begin + NumFields;
    return const_iterator { Begin, End };
  }

  const_iterator end() const {
    auto Begin = getFieldRecordBuffer();
    auto End = Begin + NumFields;
    return const_iterator { End, End };
  }

  bool hasMangledTypeName() const {
    return MangledTypeName;
  }

  std::string getMangledTypeName() const {
    return MangledTypeName.get();
  }
};

class FieldDescriptorIterator
  : public std::iterator<std::forward_iterator_tag, FieldDescriptor> {
public:
  const void *Cur;
  const void * const End;
  FieldDescriptorIterator(const void *Cur, const void * const End)
    : Cur(Cur), End(End) {}

  const FieldDescriptor &operator*() const {
    return *reinterpret_cast<const FieldDescriptor *>(Cur);
  }

  FieldDescriptorIterator &operator++() {
    const auto &FR = this->operator*();
    const void *Next = reinterpret_cast<const char *>(Cur)
      + sizeof(FieldDescriptor) + FR.NumFields * FR.FieldRecordSize;
    Cur = Next;
    return *this;
  }

  bool operator==(FieldDescriptorIterator const &other) const {
    return Cur == other.Cur && End == other.End;
  }

  bool operator!=(FieldDescriptorIterator const &other) const {
    return !(*this == other);
  }
};

// Associated type records describe the mapping from an associated
// type to the type witness of a conformance.
class AssociatedTypeRecord {
  const RelativeDirectPointer<const char> Name;
  const RelativeDirectPointer<const char> SubstitutedTypeName;

public:
  std::string getName() const {
    return Name.get();
  }

  std::string getMangledSubstitutedTypeName() const {
    return SubstitutedTypeName.get();
  }
};

struct AssociatedTypeRecordIterator {
  const AssociatedTypeRecord *Cur;
  const AssociatedTypeRecord * const End;

  AssociatedTypeRecordIterator()
    : Cur(nullptr), End(nullptr) {}

  AssociatedTypeRecordIterator(const AssociatedTypeRecord *Cur,
                               const AssociatedTypeRecord * const End)
    : Cur(Cur), End(End) {}

  const AssociatedTypeRecord &operator*() const {
    return *Cur;
  }

  AssociatedTypeRecordIterator &operator++() {
    ++Cur;
    return *this;
  }

  AssociatedTypeRecordIterator
  operator=(const AssociatedTypeRecordIterator &Other) {
    return { Other.Cur, Other.End };
  }

  bool operator==(const AssociatedTypeRecordIterator &other) const {
    return Cur == other.Cur && End == other.End;
  }

  bool operator!=(const AssociatedTypeRecordIterator &other) const {
    return !(*this == other);
  }

  operator bool() const {
    return Cur && End;
  }
};

// An associated type descriptor contains a collection of associated
// type records for a conformance.
struct AssociatedTypeDescriptor {
  const RelativeDirectPointer<const char> ConformingTypeName;
  const RelativeDirectPointer<const char> ProtocolTypeName;
  uint32_t NumAssociatedTypes;
  uint32_t AssociatedTypeRecordSize;

  const AssociatedTypeRecord *getAssociatedTypeRecordBuffer() const {
    return reinterpret_cast<const AssociatedTypeRecord *>(this + 1);
  }

public:
  using const_iterator = AssociatedTypeRecordIterator;

  const_iterator begin() const {
    auto Begin = getAssociatedTypeRecordBuffer();
    auto End = Begin + NumAssociatedTypes;
    return const_iterator { Begin, End };
  }

  const_iterator end() const {
    auto Begin = getAssociatedTypeRecordBuffer();
    auto End = Begin + NumAssociatedTypes;
    return const_iterator { End, End };
  }

  std::string getMangledProtocolTypeName() const {
    return ProtocolTypeName.get();
  }

  std::string getMangledConformingTypeName() const {
    return ConformingTypeName.get();
  }
};

class AssociatedTypeIterator
  : public std::iterator<std::forward_iterator_tag, AssociatedTypeDescriptor> {
public:
  const void *Cur;
  const void * const End;
  AssociatedTypeIterator(const void *Cur, const void * const End)
    : Cur(Cur), End(End) {}

  const AssociatedTypeDescriptor &operator*() const {
    return *reinterpret_cast<const AssociatedTypeDescriptor *>(Cur);
  }

  AssociatedTypeIterator &operator++() {
    const auto &ATR = this->operator*();
    size_t Size = sizeof(AssociatedTypeDescriptor) +
      ATR.NumAssociatedTypes * ATR.AssociatedTypeRecordSize;
    const void *Next = reinterpret_cast<const char *>(Cur) + Size;
    Cur = Next;
    return *this;
  }

  bool operator==(AssociatedTypeIterator const &other) const {
    return Cur == other.Cur && End == other.End;
  }

  bool operator!=(AssociatedTypeIterator const &other) const {
    return !(*this == other);
  }
};

// Builtin type records describe basic layout information about
// any builtin types referenced from the other sections.
class BuiltinTypeDescriptor {
  const RelativeDirectPointer<const char> TypeName;

public:
  uint32_t Size;
  uint32_t Alignment;
  uint32_t Stride;
  uint32_t NumExtraInhabitants;

  bool hasMangledTypeName() const {
    return TypeName;
  }

  std::string getMangledTypeName() const {
    return TypeName.get();
  }
};

class BuiltinTypeDescriptorIterator
  : public std::iterator<std::forward_iterator_tag, BuiltinTypeDescriptor> {
public:
  const void *Cur;
  const void * const End;
  BuiltinTypeDescriptorIterator(const void *Cur, const void * const End)
    : Cur(Cur), End(End) {}

  const BuiltinTypeDescriptor &operator*() const {
    return *reinterpret_cast<const BuiltinTypeDescriptor *>(Cur);
  }

  BuiltinTypeDescriptorIterator &operator++() {
    const void *Next = reinterpret_cast<const char *>(Cur)
      + sizeof(BuiltinTypeDescriptor);
    Cur = Next;
    return *this;
  }

  bool operator==(BuiltinTypeDescriptorIterator const &other) const {
    return Cur == other.Cur && End == other.End;
  }

  bool operator!=(BuiltinTypeDescriptorIterator const &other) const {
    return !(*this == other);
  }
};

/// A key-value pair in a TypeRef -> MetadataSource map.
struct GenericMetadataSource {
  using Key = RelativeDirectPointer<const char>;
  using Value = Key;

  const Key MangledTypeName;
  const Value EncodedMetadataSource;
};

/// Describes the layout of a heap closure.
///
/// For simplicity's sake and other reasons, this shouldn't contain
/// architecture-specifically sized things like direct pointers, uintptr_t, etc.
struct CaptureDescriptor {
public:

  /// The number of captures in the closure and the number of typerefs that
  /// immediately follow this struct.
  const uint32_t NumCaptures;

  /// The number of sources of metadata available in the MetadataSourceMap
  /// directly following the list of capture's typerefs.
  const uint32_t NumMetadataSources;

  /// The number of items in the NecessaryBindings structure at the head of
  /// the closure.
  const uint32_t NumBindings;

  /// Get the key-value pair for the ith generic metadata source.
  const GenericMetadataSource &getGenericMetadataSource(size_t i) const {
    assert(i <= NumMetadataSources &&
           "Generic metadata source index out of range");
    auto Begin = getGenericMetadataSourceBuffer();
    return Begin[i];
  }

  /// Get the typeref (encoded as a mangled type name) of the ith
  /// closure capture.
  const RelativeDirectPointer<const char> &
  getCaptureMangledTypeName(size_t i) const {
    assert(i <= NumCaptures && "Capture index out of range");
    auto Begin = getCaptureTypeRefBuffer();
    return Begin[i];
  }

private:
  const GenericMetadataSource *getGenericMetadataSourceBuffer() const {
    auto BeginTR = reinterpret_cast<const char *>(getCaptureTypeRefBuffer());
    auto EndTR = BeginTR + NumCaptures * sizeof(GenericMetadataSource);
    return reinterpret_cast<const GenericMetadataSource *>(EndTR);
  }

  const RelativeDirectPointer<const char> *getCaptureTypeRefBuffer() const {
    return reinterpret_cast<const RelativeDirectPointer<const char> *>(this+1);
  }
};

} // end namespace reflection
} // end namespace swift

#endif // SWIFT_REFLECTION_RECORDS_H
