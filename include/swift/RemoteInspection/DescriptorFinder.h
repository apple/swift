//===--------------- DescriptorFinder.h -------------------------*- C++ -*-===//
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

#ifndef SWIFT_REFLECTION_DESCRIPTOR_FINDER_H
#define SWIFT_REFLECTION_DESCRIPTOR_FINDER_H

#include "swift/Demangling/Demangle.h"
#include "swift/RemoteInspection/Records.h"
#include "llvm/ADT/StringRef.h"

namespace swift {
namespace reflection {

class TypeRef;

/// An abstract interface for a builtin type descriptor.
struct BuiltinTypeDescriptorBase {
  const uint32_t Size;
  const uint32_t Alignment;
  const uint32_t Stride;
  const uint32_t NumExtraInhabitants;
  const bool IsBitwiseTakable;

  BuiltinTypeDescriptorBase(uint32_t Size, uint32_t Alignment, uint32_t Stride,
                            uint32_t NumExtraInhabitants, bool IsBitwiseTakable)
      : Size(Size), Alignment(Alignment), Stride(Stride),
        NumExtraInhabitants(NumExtraInhabitants),
        IsBitwiseTakable(IsBitwiseTakable) {}

  virtual ~BuiltinTypeDescriptorBase(){};

  virtual llvm::StringRef getMangledTypeName() = 0;
};

/// An abstract interface for a field record.
struct FieldRecordBase {
  const bool IsIndirectCase;
  const bool IsVar;
  const bool HasMangledTypeName;

  FieldRecordBase(bool IsIndirectCase, bool IsVar,
                       bool HasMangledTypeName)
      : IsIndirectCase(IsIndirectCase), IsVar(IsVar),
        HasMangledTypeName(HasMangledTypeName) {}

  virtual ~FieldRecordBase(){};

  virtual llvm::StringRef getFieldName() = 0;
  virtual Demangle::Node *getDemangledTypeName() = 0;
};

/// An abstract interface for a field descriptor.
struct FieldDescriptorBase {
  const FieldDescriptorKind Kind;
  const bool HasSuperClass;

  FieldDescriptorBase(FieldDescriptorKind Kind, bool HasSuperClass)
      : Kind(Kind), HasSuperClass(HasSuperClass) {}

  bool isEnum() const {
    return (Kind == FieldDescriptorKind::Enum ||
            Kind == FieldDescriptorKind::MultiPayloadEnum);
  }

  bool isClass() const {
    return (Kind == FieldDescriptorKind::Class ||
            Kind == FieldDescriptorKind::ObjCClass);
  }

  bool isProtocol() const {
    return (Kind == FieldDescriptorKind::Protocol ||
            Kind == FieldDescriptorKind::ClassProtocol ||
            Kind == FieldDescriptorKind::ObjCProtocol);
  }

  bool isStruct() const {
    return Kind == FieldDescriptorKind::Struct;
  }

  virtual ~FieldDescriptorBase(){};

  virtual Demangle::Node *demangleSuperclass() = 0;
  virtual std::vector<std::unique_ptr<FieldRecordBase>>
  getFieldRecords() = 0;
};

struct MultiPayloadEnumDescriptorBase {
  virtual ~MultiPayloadEnumDescriptorBase(){};

  virtual llvm::StringRef getMangledTypeName() = 0;

  virtual uint32_t getContentsSizeInWords() const = 0;

  virtual size_t getSizeInBytes() const = 0;

  virtual uint32_t getFlags() const = 0;

  virtual bool usesPayloadSpareBits() const = 0;

  virtual uint32_t getPayloadSpareBitMaskByteOffset() const = 0;

  virtual uint32_t getPayloadSpareBitMaskByteCount() const = 0;

  virtual const uint8_t *getPayloadSpareBits() const = 0;

};
/// Interface for finding type descriptors. Implementors may provide descriptors
/// that live inside or outside reflection metadata.
struct DescriptorFinder {
  virtual ~DescriptorFinder(){};

  virtual std::unique_ptr<BuiltinTypeDescriptorBase>
  getBuiltinTypeDescriptor(const TypeRef *TR) = 0;


  virtual std::unique_ptr<FieldDescriptorBase>
  getFieldDescriptor(const TypeRef *TR) = 0;

  virtual std::unique_ptr<MultiPayloadEnumDescriptorBase>
  getMultiPayloadEnumDescriptor(const TypeRef *TR) { abort(); };
};

} // namespace reflection
} // namespace swift
#endif
