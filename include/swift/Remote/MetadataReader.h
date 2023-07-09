//===--- MetadataReader.h - Abstract access to remote metadata --*- C++ -*-===//
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
//  This file defines operations for reading metadata from a remote process.
//
//===----------------------------------------------------------------------===//

#ifndef SWIFT_REMOTE_METADATAREADER_H
#define SWIFT_REMOTE_METADATAREADER_H

#include "swift/Runtime/Metadata.h"
#include "swift/Remote/MemoryReader.h"
#include "swift/Demangling/Demangler.h"
#include "swift/Demangling/TypeDecoder.h"
#include "swift/Basic/Defer.h"
#include "swift/Basic/ExternalUnion.h"
#include "swift/Basic/Range.h"
#include "swift/Basic/LLVM.h"
#include "swift/ABI/TypeIdentity.h"
#include "swift/Runtime/ExistentialContainer.h"
#include "swift/Runtime/HeapObject.h"
#include "swift/Basic/Unreachable.h"

#include <type_traits>
#include <vector>
#include <unordered_map>

#include <inttypes.h>

namespace swift {
namespace remote {

template <typename BuiltType>
using FunctionParam = swift::Demangle::FunctionParam<BuiltType>;

template <typename BuilderType>
using TypeDecoder = swift::Demangle::TypeDecoder<BuilderType>;

/// The kind of mangled name to read.
enum class MangledNameKind {
  Type,
  Symbol,
};

/// A pointer to the local buffer of an object that also remembers the
/// address at which it was stored remotely.
template <typename T>
class RemoteRef {
private:
  uint64_t Address;
  const T *LocalBuffer;

public:
  RemoteRef()
    : Address(0), LocalBuffer(nullptr) {}

  /*implicit*/
  RemoteRef(std::nullptr_t _) : RemoteRef() {}

  template<typename StoredPointer>
  explicit RemoteRef(StoredPointer address, const T *localBuffer)
    : Address((uint64_t)address), LocalBuffer(localBuffer) {}

  // <rdar://99715218> Some versions of clang++ sometimes fail to generate the
  // copy constructor for this type correctly - add a workaround
  RemoteRef(const RemoteRef &other)
    : Address(other.Address), LocalBuffer(other.LocalBuffer) {}

  RemoteRef& operator=(const RemoteRef &other) {
    Address = other.Address;
    LocalBuffer = other.LocalBuffer;
    return *this;
  }

  uint64_t getAddressData() const {
    return Address;
  }

  const T *getLocalBuffer() const {
    return LocalBuffer;
  }

  explicit operator bool() const {
    return LocalBuffer != nullptr;
  }
  
  const T *operator->() const {
    assert(LocalBuffer);
    return LocalBuffer;
  }
  
  bool operator==(RemoteRef<T> other) const {
    return Address == other.Address;
  }
  
  bool operator!=(RemoteRef<T> other) const {
    return !operator==(other);
  }
  
  /// Project a reference for a field. The field must be projected from the same
  /// LocalBuffer pointer as this RemoteRef.
  template<typename U>
  RemoteRef<U> getField(U &field) const {
    auto offset = (intptr_t)&field - (intptr_t)LocalBuffer;
    return RemoteRef<U>((uint64_t)(Address + (int64_t)offset), &field);
  }
  
  /// Resolve the remote address of a relative offset stored at the remote address.
  uint64_t resolveRelativeAddressData() const {
    int32_t offset;
    memcpy(&offset, LocalBuffer, sizeof(int32_t));
    if (offset == 0)
      return 0;
    return Address + (int64_t)offset;
  }
  
  template<typename U>
  uint64_t resolveRelativeFieldData(U &field) const {
    return getField(field).resolveRelativeAddressData();
  }
  
  RemoteRef atByteOffset(int64_t Offset) const {
    return RemoteRef(Address + Offset,
                     (const T *)((intptr_t)LocalBuffer + Offset));
  }
};

/// A structure, designed for use with std::unique_ptr, which destroys
/// a pointer by calling free on it (and not trying to call a destructor).
struct delete_with_free {
  void operator()(const void *memory) {
    free(const_cast<void*>(memory));
  }
};

/// A structure representing an opened existential type.
struct RemoteExistential {
  /// The payload's concrete type metadata.
  RemoteAddress MetadataAddress;

  /// The address of the payload value.
  RemoteAddress PayloadAddress;

  /// True if this is an NSError instance transparently bridged to an Error
  /// existential.
  bool IsBridgedError;

  RemoteExistential(RemoteAddress MetadataAddress,
                    RemoteAddress PayloadAddress,
                    bool IsBridgedError=false)
    : MetadataAddress(MetadataAddress),
      PayloadAddress(PayloadAddress),
      IsBridgedError(IsBridgedError) {}
};

/// A generic reader of metadata.
///
/// BuilderType must implement a particular interface which is currently
/// too fluid to allow useful documentation; consult the actual
/// implementations.  The chief thing is that it provides several member
/// types which should obey the following constraints:
///   - T() yields a value which is false when contextually converted to bool
///   - a false value signals that an error occurred when building a value
template <typename Runtime, typename BuilderType>
class MetadataReader {
public:
  using BuiltType = typename BuilderType::BuiltType;
  using BuiltTypeDecl = typename BuilderType::BuiltTypeDecl;
  using BuiltProtocolDecl = typename BuilderType::BuiltProtocolDecl;
  using BuiltRequirement = typename BuilderType::BuiltRequirement;
  using BuiltSubstitution = typename BuilderType::BuiltSubstitution;
  using BuiltSubstitutionMap = typename BuilderType::BuiltSubstitutionMap;
  using BuiltGenericSignature = typename BuilderType::BuiltGenericSignature;
  using StoredPointer = typename Runtime::StoredPointer;
  using StoredSignedPointer = typename Runtime::StoredSignedPointer;
  using StoredSize = typename Runtime::StoredSize;
  using TargetClassMetadata = TargetClassMetadataType<Runtime>;

  static const int defaultTypeRecursionLimit = 50;

private:
  /// The maximum number of bytes to read when reading metadata. Anything larger
  /// will automatically return failure. This prevents us from reading absurd
  /// amounts of data when we encounter corrupt values for sizes/counts.
  static const uint64_t MaxMetadataSize = 1048576; // 1MB

  /// A cache of built types, keyed by the address of the type.
  std::unordered_map<StoredPointer, BuiltType> TypeCache;

  using MetadataRef = RemoteRef<const TargetMetadata<Runtime>>;
  using OwnedMetadataRef = MemoryReader::ReadBytesResult;

  /// A cache of read type metadata, keyed by the address of the metadata.
  std::unordered_map<StoredPointer, OwnedMetadataRef>
    MetadataCache;

  using ContextDescriptorRef =
      RemoteRef<const TargetContextDescriptor<Runtime>>;
  using OwnedContextDescriptorRef = MemoryReader::ReadBytesResult;

  using ShapeRef =
      RemoteRef<const TargetExtendedExistentialTypeShape<Runtime>>;
  using OwnedShapeRef = MemoryReader::ReadBytesResult;

  /// A reference to a context descriptor that may be in an unloaded image.
  class ParentContextDescriptorRef {
    bool IsResolved;
    using Payloads = ExternalUnionMembers<std::string, ContextDescriptorRef>;
    static typename Payloads::Index getPayloadIndex(bool IsResolved) {
      return IsResolved ? Payloads::template indexOf<ContextDescriptorRef>()
                        : Payloads::template indexOf<std::string>();
    }
    
    ExternalUnion<bool, Payloads, getPayloadIndex> Payload;
    
  public:
    explicit ParentContextDescriptorRef(StringRef Symbol)
      : IsResolved(false)
    {
      Payload.template emplace<std::string>(IsResolved, Symbol);
    }
    
    explicit ParentContextDescriptorRef(ContextDescriptorRef Resolved)
      : IsResolved(true)
    {
      Payload.template emplace<ContextDescriptorRef>(IsResolved, Resolved);
    }
    
    ParentContextDescriptorRef()
      : ParentContextDescriptorRef(ContextDescriptorRef())
    {}
    
    ParentContextDescriptorRef(const ParentContextDescriptorRef &o)
      : IsResolved(o.IsResolved)
    {
      Payload.copyConstruct(IsResolved, o.Payload);
    }
    
    ParentContextDescriptorRef(ParentContextDescriptorRef &&o)
      : IsResolved(o.IsResolved)
    {
      Payload.moveConstruct(IsResolved, std::move(o.Payload));
    }
    
    ~ParentContextDescriptorRef() {
      Payload.destruct(IsResolved);
    }

    ParentContextDescriptorRef &operator=(const ParentContextDescriptorRef &o) {
      Payload.copyAssign(IsResolved, o.IsResolved, o.Payload);
      IsResolved = o.isResolved();
      return *this;
    }
    ParentContextDescriptorRef &operator=(ParentContextDescriptorRef &&o) {
      Payload.moveAssign(IsResolved, o.IsResolved, std::move(o.Payload));
      IsResolved = o.isResolved();
      return *this;
    }

    bool isResolved() const { return IsResolved; }
    
    StringRef getSymbol() const {
      return Payload.template get<std::string>(IsResolved);
    }
    
    ContextDescriptorRef getResolved() const {
      return Payload.template get<ContextDescriptorRef>(IsResolved);
    }

    explicit operator bool() const {
      return !isResolved() || getResolved();
    }
  };

  /// A cache of read nominal type descriptors, keyed by the address of the
  /// nominal type descriptor.
  std::unordered_map<StoredPointer, OwnedContextDescriptorRef>
    ContextDescriptorCache;

  using OwnedProtocolDescriptorRef =
    std::unique_ptr<const TargetProtocolDescriptor<Runtime>, delete_with_free>;
  /// A cache of read extended existential shape metadata, keyed by the
  /// address of the shape metadata.
  std::unordered_map<StoredPointer, OwnedShapeRef>
    ShapeCache;

  enum class IsaEncodingKind {
    /// We haven't checked yet.
    Unknown,

    /// There was an error trying to find out the isa encoding.
    Error,

    /// There's no special isa encoding.
    None,

    /// There's an unconditional mask to apply to the isa pointer.
    ///   - IsaMask stores the mask.
    Masked,

    /// Isa pointers are indexed.  If applying a mask yields a magic value,
    /// applying a different mask and shifting yields an index into a global
    /// array of class pointers.  Otherwise, the isa pointer is just a raw
    /// class pointer.
    ///  - IsaIndexMask stores the index mask.
    ///  - IsaIndexShift stores the index shift.
    ///  - IsaMagicMask stores the magic value mask.
    ///  - IsaMagicValue stores the magic value.
    ///  - IndexedClassesPointer stores the pointer to the start of the
    ///    indexed classes array; this is constant throughout the program.
    ///  - IndexedClassesCountPointer stores a pointer to the number
    ///    of elements in the indexed classes array.
    Indexed
  };

  IsaEncodingKind IsaEncoding = IsaEncodingKind::Unknown;
  union {
    StoredPointer IsaMask;
    StoredPointer IsaIndexMask;
  };
  StoredPointer IsaIndexShift;
  StoredPointer IsaMagicMask;
  StoredPointer IsaMagicValue;
  StoredPointer IndexedClassesPointer;
  StoredPointer IndexedClassesCountPointer;
  StoredPointer LastIndexedClassesCount = 0;

  enum class TaggedPointerEncodingKind {
    /// We haven't checked yet.
    Unknown,

    /// There was an error trying to find out the tagged pointer encoding.
    Error,

    /// The "extended" encoding.
    ///
    /// 1 bit:   is-tagged
    /// 3 bits:  class index (for objc_debug_taggedpointer_classes[])
    /// 60 bits: payload
    ///
    /// Class index 0b111 represents 256 additional classes:
    ///
    /// 1 bit:   is-tagged
    /// 3 bits:  0b111
    /// 8 bits:  extended class index (for objc_debug_taggedpointer_ext_classes[])
    /// 54 bits: payload
    Extended
  };
  TaggedPointerEncodingKind TaggedPointerEncoding =
      TaggedPointerEncodingKind::Unknown;
  StoredPointer TaggedPointerMask;
  StoredPointer TaggedPointerSlotShift;
  StoredPointer TaggedPointerSlotMask;
  StoredPointer TaggedPointerClasses;
  StoredPointer TaggedPointerExtendedMask;
  StoredPointer TaggedPointerExtendedSlotShift;
  StoredPointer TaggedPointerExtendedSlotMask;
  StoredPointer TaggedPointerExtendedClasses;
  StoredPointer TaggedPointerObfuscator;

  Demangle::NodeFactory Factory;

  Demangle::NodeFactory &getNodeFactory() { return Factory; }

public:
  BuilderType Builder;

  BuilderType &getBuilder() {
    return this->Builder;
  }

  std::shared_ptr<MemoryReader> Reader;

  StoredPointer PtrAuthMask;

  StoredPointer stripSignedPointer(StoredSignedPointer P) {
    return P.SignedValue & PtrAuthMask;
  }

  RemoteAbsolutePointer stripSignedPointer(const RemoteAbsolutePointer &P) {
    if (P.isResolved()) {
      return RemoteAbsolutePointer("", 
        P.getResolvedAddress().getAddressData() & PtrAuthMask);
    }
    return P;
  }

  StoredPointer queryPtrAuthMask() {
    StoredPointer QueryResult;
    if (Reader->queryDataLayout(DataLayoutQueryType::DLQ_GetPtrAuthMask,
                                nullptr, &QueryResult)) {
      return QueryResult;
    }
    return ~StoredPointer(0);
  }

  template <class... T>
  MetadataReader(std::shared_ptr<MemoryReader> reader, T &&... args)
    : Builder(std::forward<T>(args)...),
      Reader(std::move(reader)),
      PtrAuthMask(queryPtrAuthMask()) {
  }

  MetadataReader(const MetadataReader &other) = delete;
  MetadataReader &operator=(const MetadataReader &other) = delete;

  /// Clear all of the caches in this reader.
  void clear() {
    TypeCache.clear();
    MetadataCache.clear();
    ContextDescriptorCache.clear();
  }

  /// Demangle a mangled name that was read from the given remote address.
  Demangle::NodePointer demangle(RemoteRef<char> mangledName,
                                 MangledNameKind kind,
                                 Demangler &dem,
                                 bool useOpaqueTypeSymbolicReferences = false) {
    // Symbolic reference resolver for the demangle operation below.
    auto symbolicReferenceResolver = [&](SymbolicReferenceKind kind,
                                         Directness directness,
                                         int32_t offset,
                                         const void *base) ->
        swift::Demangle::NodePointer {
      // Resolve the reference to a remote address.
      auto offsetInMangledName =
            (const char *)base - mangledName.getLocalBuffer();
      auto remoteAddress =
        mangledName.getAddressData() + offsetInMangledName + offset;

      RemoteAbsolutePointer resolved;
      if (directness == Directness::Indirect) {
        if (auto indirectAddress = readPointer(remoteAddress)) {
          resolved = stripSignedPointer(*indirectAddress);
        } else {
          return nullptr;
        }
      } else {
        resolved = Reader->getSymbol(RemoteAddress(remoteAddress));
      }

      switch (kind) {
      case Demangle::SymbolicReferenceKind::Context: {
        auto context = readContextDescriptor(resolved);
        if (!context)
          return nullptr;
        // Try to preserve a reference to an OpaqueTypeDescriptor
        // symbolically, since we'd like to read out and resolve the type ref
        // to the underlying type if available.
        if (useOpaqueTypeSymbolicReferences
           && context.isResolved()
           && context.getResolved()->getKind() == ContextDescriptorKind::OpaqueType){
          return dem.createNode(
                            Node::Kind::OpaqueTypeDescriptorSymbolicReference,
                            context.getResolved().getAddressData());
        }

        return buildContextMangling(context, dem);
      }
      case Demangle::SymbolicReferenceKind::AccessorFunctionReference: {
        // The symbolic reference points at a resolver function, but we can't
        // execute code in the target process to resolve it from here.
        return nullptr;
      }
      case Demangle::SymbolicReferenceKind::UniqueExtendedExistentialTypeShape: {
        // The symbolic reference points at a unique extended
        // existential type shape.
        return dem.createNode(
                          Node::Kind::UniqueExtendedExistentialTypeShapeSymbolicReference,
                          resolved.getResolvedAddress().getAddressData());
      }
      case Demangle::SymbolicReferenceKind::NonUniqueExtendedExistentialTypeShape: {
        // The symbolic reference points at a non-unique extended
        // existential type shape.
        return dem.createNode(
                          Node::Kind::NonUniqueExtendedExistentialTypeShapeSymbolicReference,
                          resolved.getResolvedAddress().getAddressData());
      }
      }

      return nullptr;
    };
    
    auto mangledNameStr =
      Demangle::makeSymbolicMangledNameStringRef(mangledName.getLocalBuffer());

    swift::Demangle::NodePointer result;
    switch (kind) {
    case MangledNameKind::Type:
      result = dem.demangleType(mangledNameStr, symbolicReferenceResolver);
      break;

    case MangledNameKind::Symbol:
      result = dem.demangleSymbol(mangledNameStr, symbolicReferenceResolver);
      break;
    }

    return result;
  }

  /// Demangle a mangled name from a potentially temporary std::string. The
  /// demangler may produce pointers into the string data, so this copies the
  /// string into the demangler's allocation first.
  Demangle::NodePointer demangle(uint64_t remoteAddress,
                                 const std::string &mangledName,
                                 MangledNameKind kind,
                                 Demangler &dem) {
    StringRef mangledNameCopy = dem.copyString(mangledName);
    return demangle(RemoteRef<char>(remoteAddress, mangledNameCopy.data()),
                    kind, dem);
  }

  /// Given a demangle tree, attempt to turn it into a type.
  TypeLookupErrorOr<typename BuilderType::BuiltType>
  decodeMangledType(NodePointer Node) {
    return swift::Demangle::decodeMangledType(Builder, Node);
  }

  /// Get the remote process's swift_isaMask.
  llvm::Optional<StoredPointer> readIsaMask() {
    auto encoding = getIsaEncoding();
    if (encoding != IsaEncodingKind::Masked) {
      // Still return success if there's no isa encoding at all.
      if (encoding == IsaEncodingKind::None)
        return 0;
      else
        return llvm::None;
    }

    return IsaMask;
  }

  /// Given a remote pointer to metadata, attempt to discover its MetadataKind.
  llvm::Optional<MetadataKind>
  readKindFromMetadata(StoredPointer MetadataAddress) {
    auto meta = readMetadata(MetadataAddress);
    if (!meta)
      return llvm::None;

    return meta->getKind();
  }

  /// Given a remote pointer to class metadata, attempt to read its superclass.
  StoredPointer
  readSuperClassFromClassMetadata(StoredPointer MetadataAddress) {
    auto meta = readMetadata(MetadataAddress);
    if (!meta || meta->getKind() != MetadataKind::Class)
      return StoredPointer();

    auto classMeta = cast<TargetClassMetadata>(meta);
    return stripSignedPointer(classMeta->Superclass);
  }

  /// Given a remote pointer to class metadata, attempt to discover its class
  /// instance size and whether fields should use the resilient layout strategy.
  llvm::Optional<unsigned> readInstanceStartAndAlignmentFromClassMetadata(
      StoredPointer MetadataAddress) {
    auto meta = readMetadata(MetadataAddress);
    if (!meta || meta->getKind() != MetadataKind::Class)
      return llvm::None;

    if (Runtime::ObjCInterop) {
      // The following algorithm only works on the non-fragile Apple runtime.

      // Grab the RO-data pointer.  This part is not ABI.
      StoredPointer roDataPtr = readObjCRODataPtr(MetadataAddress);
      if (!roDataPtr)
        return llvm::None;

      // Get the address of the InstanceStart field.
      auto address = roDataPtr + sizeof(uint32_t) * 1;

      unsigned start;
      if (!Reader->readInteger(RemoteAddress(address), &start))
        return llvm::None;

      return start;
    } else {
      // All swift class instances start with an isa pointer,
      // followed by the retain counts (which are the size of a long long).
      size_t isaAndRetainCountSize = sizeof(StoredSize) + sizeof(long long);
      size_t start = isaAndRetainCountSize;

      auto classMeta = cast<TargetClassMetadata>(meta);
      while (stripSignedPointer(classMeta->Superclass)) {
        classMeta = cast<TargetClassMetadata>(
            readMetadata(stripSignedPointer(classMeta->Superclass)));

        // Subtract the size contribution of the isa and retain counts from
        // the super class.
        start += classMeta->InstanceSize - isaAndRetainCountSize;
      }
      return start;
    }
  }

  /// Given a pointer to the metadata, attempt to read the value
  /// witness table. Note that it's not safe to access any non-mandatory
  /// members of the value witness table, like extra inhabitants or enum members.
  llvm::Optional<TargetValueWitnessTable<Runtime>>
  readValueWitnessTable(StoredPointer MetadataAddress) {
    // The value witness table pointer is at offset -1 from the metadata
    // pointer, that is, the pointer-sized word immediately before the
    // pointer's referenced address.
    TargetValueWitnessTable<Runtime> VWT;
    auto ValueWitnessTableAddrAddr = MetadataAddress - sizeof(StoredPointer);
    StoredPointer ValueWitnessTableAddr;
    if (!Reader->readInteger(RemoteAddress(ValueWitnessTableAddrAddr),
                             &ValueWitnessTableAddr))
      return llvm::None;
    if (!Reader->readBytes(RemoteAddress(ValueWitnessTableAddr),
                           (uint8_t *)&VWT, sizeof(VWT)))
      return llvm::None;
    return VWT;
  }

  /// Given a pointer to a known-error existential, attempt to discover the
  /// pointer to its metadata address, its value address, and whether this
  /// is a toll-free-bridged NSError or an actual Error existential wrapper
  /// around a native Swift value.
  llvm::Optional<RemoteExistential>
  readMetadataAndValueErrorExistential(RemoteAddress ExistentialAddress) {
    // An pointer to an error existential is always an heap object.
    auto MetadataAddress =
        readMetadataFromInstance(ExistentialAddress.getAddressData());
    if (!MetadataAddress)
      return llvm::None;

    bool isObjC = false;
    bool isBridged = false;

    auto Meta = readMetadata(*MetadataAddress);
    if (!Meta)
      return llvm::None;

    if (auto ClassMeta = dyn_cast<TargetClassMetadata>(Meta)) {
      if (ClassMeta->isPureObjC()) {
        // If we can determine the Objective-C class name, this is probably an
        // error existential with NSError-compatible layout.
        std::string ObjCClassName;
        if (readObjCClassName(*MetadataAddress, ObjCClassName)) {
          if (ObjCClassName == "__SwiftNativeNSError")
            isObjC = true;
          else
            isBridged = true;
        }
      } else {
        isBridged = true;
      }
    }

    if (isBridged) {
      // NSError instances don't need to be unwrapped.
      return RemoteExistential(RemoteAddress(*MetadataAddress),
                               ExistentialAddress,
                               isBridged);
    }

    // In addition to the isa pointer and two 32-bit reference counts, if the
    // error existential is layout-compatible with NSError, we also need to
    // skip over its three word-sized fields: the error code, the domain,
    // and userInfo.
    StoredPointer InstanceMetadataAddressAddress =
        ExistentialAddress.getAddressData() +
        (isObjC ? 5 : 2) * sizeof(StoredPointer);

    // We need to get the instance's alignment info so we can get the exact
    // offset of the start of its data in the class.
    auto InstanceMetadataAddress =
        readMetadataFromInstance(InstanceMetadataAddressAddress);
    if (!InstanceMetadataAddress)
      return llvm::None;

    // Read the value witness table.
    auto VWT = readValueWitnessTable(*InstanceMetadataAddress);
    if (!VWT)
      return llvm::None;

    // Now we need to skip over the instance metadata pointer and instance's
    // conformance pointer for Swift.Error.
    StoredPointer InstanceAddress =
        InstanceMetadataAddressAddress + 2 * sizeof(StoredPointer);

    // When built with Objective-C interop, the runtime also stores a conformance
    // to Hashable and the base type introducing the Hashable conformance.
    if (isObjC)
      InstanceAddress += 2 * sizeof(StoredPointer);

    // Round up to alignment, and we have the start address of the
    // instance payload.
    auto AlignmentMask = VWT->getAlignmentMask();
    InstanceAddress = (InstanceAddress + AlignmentMask) & ~AlignmentMask;

    return RemoteExistential(
        RemoteAddress(*InstanceMetadataAddress),
        RemoteAddress(InstanceAddress),
        isBridged);
  }

  /// Given a known-opaque existential, attempt to discover the pointer to its
  /// metadata address and its value.
  llvm::Optional<RemoteExistential>
  readMetadataAndValueOpaqueExistential(RemoteAddress ExistentialAddress) {
    // OpaqueExistentialContainer is the layout of an opaque existential.
    // `Type` is the pointer to the metadata.
    TargetOpaqueExistentialContainer<Runtime> Container;
    if (!Reader->readBytes(RemoteAddress(ExistentialAddress),
                           (uint8_t *)&Container, sizeof(Container)))
      return llvm::None;
    auto MetadataAddress = static_cast<StoredPointer>(Container.Type);
    auto Metadata = readMetadata(MetadataAddress);
    if (!Metadata)
      return llvm::None;

    auto VWT = readValueWitnessTable(MetadataAddress);
    if (!VWT)
      return llvm::None;

    // Inline representation (the value fits in the existential container).
    // So, the value starts at the first word of the container.
    if (VWT->isValueInline())
      return RemoteExistential(RemoteAddress(MetadataAddress),
                               ExistentialAddress);

    // Non-inline (box'ed) representation.
    // The first word of the container stores the address to the box.
    StoredPointer BoxAddress;
    if (!Reader->readInteger(ExistentialAddress, &BoxAddress))
      return llvm::None;

    auto AlignmentMask = VWT->getAlignmentMask();
    auto Offset = (sizeof(HeapObject) + AlignmentMask) & ~AlignmentMask;
    auto StartOfValue = BoxAddress + Offset;
    return RemoteExistential(RemoteAddress(MetadataAddress),
                             RemoteAddress(StartOfValue));
  }

  /// Given a known-opaque existential, discover if its value is inlined in
  /// the existential container.
  llvm::Optional<bool>
  isValueInlinedInExistentialContainer(RemoteAddress ExistentialAddress) {
    // OpaqueExistentialContainer is the layout of an opaque existential.
    // `Type` is the pointer to the metadata.
    TargetOpaqueExistentialContainer<Runtime> Container;
    if (!Reader->readBytes(RemoteAddress(ExistentialAddress),
                           (uint8_t *)&Container, sizeof(Container)))
      return llvm::None;
    auto MetadataAddress = static_cast<StoredPointer>(Container.Type);
    auto Metadata = readMetadata(MetadataAddress);
    if (!Metadata)
      return llvm::None;

    auto VWT = readValueWitnessTable(MetadataAddress);
    if (!VWT)
      return llvm::None;

    return VWT->isValueInline();
  }

  /// Read a protocol from a reference to said protocol.
  template<typename Resolver>
  typename Resolver::Result readProtocol(
      const TargetProtocolDescriptorRef<Runtime> &ProtocolAddress,
      Demangler &dem,
      Resolver resolver) {
#if SWIFT_OBJC_INTEROP
    if (Runtime::ObjCInterop) {
      // Check whether we have an Objective-C protocol.
      if (ProtocolAddress.isObjC()) {
        auto Name = readObjCProtocolName(ProtocolAddress.getObjCProtocol());
        StringRef NameStr(Name);

        // If this is a Swift-defined protocol, demangle it.
        if (NameStr.startswith("_TtP")) {
          auto Demangled = dem.demangleSymbol(NameStr);
          if (!Demangled)
            return resolver.failure();

          // FIXME: This appears in _swift_buildDemanglingForMetadata().
          while (Demangled->getKind() == Node::Kind::Global ||
                 Demangled->getKind() == Node::Kind::TypeMangling ||
                 Demangled->getKind() == Node::Kind::Type ||
                 Demangled->getKind() == Node::Kind::ProtocolList ||
                 Demangled->getKind() == Node::Kind::TypeList ||
                 Demangled->getKind() == Node::Kind::Type) {
            if (Demangled->getNumChildren() != 1)
              return resolver.failure();
            Demangled = Demangled->getFirstChild();
          }

          return resolver.swiftProtocol(Demangled);
        }

        // Otherwise, this is an imported protocol.
        return resolver.objcProtocol(NameStr);
      }
    }
#endif

    // Swift-native protocol.
    auto Demangled =
      readDemanglingForContextDescriptor(
        stripSignedPointer({ProtocolAddress.getSwiftProtocol()}), dem);
    if (!Demangled)
      return resolver.failure();

    return resolver.swiftProtocol(Demangled);
  }

  /// Given a remote pointer to metadata, attempt to turn it into a type.
  BuiltType
  readTypeFromMetadata(StoredPointer MetadataAddress,
                       bool skipArtificialSubclasses = false,
                       int recursion_limit = defaultTypeRecursionLimit) {
    auto Cached = TypeCache.find(MetadataAddress);
    if (Cached != TypeCache.end())
      return Cached->second;

    if (recursion_limit <= 0) {
      return nullptr;
    }

    // readTypeFromMetadata calls out to various other functions which can call
    // back to readTypeFromMetadata. We only want to bump the recursion limit
    // down here, not in the other functions, so that we're only counting
    // recursive calls to readTypeFromMetadata. This decrement is the only place
    // where we'll subtract 1.
    recursion_limit--;

    // If we see garbage data in the process of building a BuiltType, and get
    // the same metadata address again, we will hit an infinite loop.
    // Insert a negative result into the cache now so that, if we recur with
    // the same address, we will return the negative result with the check
    // just above.
    TypeCache.insert({MetadataAddress, BuiltType()});

    auto Meta = readMetadata(MetadataAddress);
    if (!Meta) return BuiltType();

    switch (Meta->getKind()) {
    case MetadataKind::Class:
      return readNominalTypeFromClassMetadata(Meta, recursion_limit,
                                              skipArtificialSubclasses);
    case MetadataKind::Struct:
    case MetadataKind::Enum:
    case MetadataKind::Optional:
      return readNominalTypeFromMetadata(Meta, recursion_limit);
    case MetadataKind::Tuple: {
      auto tupleMeta = cast<TargetTupleTypeMetadata<Runtime>>(Meta);

      std::vector<BuiltType> elementTypes;
      elementTypes.reserve(tupleMeta->NumElements);

      for (unsigned i = 0, n = tupleMeta->NumElements; i != n; ++i) {
        auto &element = tupleMeta->getElement(i);
        if (auto elementType =
                readTypeFromMetadata(element.Type, false, recursion_limit))
          elementTypes.push_back(elementType);
        else
          return BuiltType();
      }

      // Read the labels string.
      std::string labelStr;
      if (tupleMeta->Labels &&
          !Reader->readString(RemoteAddress(tupleMeta->Labels), labelStr))
        return BuiltType();

      std::vector<llvm::StringRef> labels;
      std::string::size_type end, start = 0;
      while (true) {
        end = labelStr.find(' ', start);
        if (end == std::string::npos)
          break;
        labels.push_back(llvm::StringRef(labelStr.data() + start, end - start));
        start = end + 1;
      }
      // Pad the vector with empty labels.
      for (unsigned i = labels.size(); i < elementTypes.size(); ++i)
        labels.push_back(StringRef());

      auto BuiltTuple =
          Builder.createTupleType(elementTypes, labels);
      TypeCache[MetadataAddress] = BuiltTuple;
      return BuiltTuple;
    }
    case MetadataKind::Function: {
      auto Function = cast<TargetFunctionTypeMetadata<Runtime>>(Meta);

      std::vector<FunctionParam<BuiltType>> Parameters;
      for (unsigned i = 0, n = Function->getNumParameters(); i != n; ++i) {
        auto ParamTypeRef = readTypeFromMetadata(Function->getParameter(i),
                                                 false, recursion_limit);
        if (!ParamTypeRef)
          return BuiltType();

        FunctionParam<BuiltType> Param;
        Param.setType(ParamTypeRef);
        Param.setFlags(Function->getParameterFlags(i));
        Parameters.push_back(std::move(Param));
      }

      auto Result =
          readTypeFromMetadata(Function->ResultType, false, recursion_limit);
      if (!Result)
        return BuiltType();

      auto flags = FunctionTypeFlags()
                       .withConvention(Function->getConvention())
                       .withAsync(Function->isAsync())
                       .withThrows(Function->isThrowing())
                       .withParameterFlags(Function->hasParameterFlags())
                       .withEscaping(Function->isEscaping())
                       .withDifferentiable(Function->isDifferentiable());

      BuiltType globalActor = BuiltType();
      if (Function->hasGlobalActor()) {
        globalActor = readTypeFromMetadata(Function->getGlobalActor(), false,
                                           recursion_limit);
        if (globalActor)
          flags = flags.withGlobalActor(true);
      }

      FunctionMetadataDifferentiabilityKind diffKind;
      switch (Function->getDifferentiabilityKind().Value) {
      #define CASE(X) \
        case TargetFunctionMetadataDifferentiabilityKind< \
            typename Runtime::StoredSize>::X: \
          diffKind = FunctionMetadataDifferentiabilityKind::X; \
          break;
      CASE(NonDifferentiable)
      CASE(Forward)
      CASE(Reverse)
      CASE(Normal)
      CASE(Linear)
      #undef CASE
      }

      auto BuiltFunction = Builder.createFunctionType(
          Parameters, Result, flags, diffKind, globalActor);
      TypeCache[MetadataAddress] = BuiltFunction;
      return BuiltFunction;
    }
    case MetadataKind::Existential: {
      auto Exist = cast<TargetExistentialTypeMetadata<Runtime>>(Meta);

      bool HasExplicitAnyObject = false;
      if (Exist->isClassBounded())
        HasExplicitAnyObject = true;

      BuiltType SuperclassType = BuiltType();
      if (Exist->Flags.hasSuperclassConstraint()) {
        // The superclass is stored after the list of protocols.
        SuperclassType = readTypeFromMetadata(Exist->getSuperclassConstraint(),
                                              false, recursion_limit);
        if (!SuperclassType) return BuiltType();

        HasExplicitAnyObject = true;
      }

      /// Resolver to turn a protocol reference into a protocol declaration.
      struct ProtocolResolver {
        using Result = BuiltProtocolDecl;

        BuilderType &builder;

        BuiltProtocolDecl failure() const {
          return BuiltProtocolDecl();
        }

        BuiltProtocolDecl swiftProtocol(Demangle::Node *node) {
          return builder.createProtocolDecl(node);
        }

#if SWIFT_OBJC_INTEROP
        BuiltProtocolDecl objcProtocol(StringRef name) {
          return builder.createObjCProtocolDecl(name.str());
        }
#endif
      } resolver{Builder};

      Demangler dem;
      std::vector<BuiltProtocolDecl> Protocols;
      for (auto ProtocolAddress : Exist->getProtocols()) {
        if (auto Protocol = readProtocol(ProtocolAddress, dem, resolver))
          Protocols.push_back(Protocol);
        else
          return BuiltType();
      }
      auto BuiltExist = Builder.createProtocolCompositionType(
        Protocols, SuperclassType, HasExplicitAnyObject);
      TypeCache[MetadataAddress] = BuiltExist;
      return BuiltExist;
    }
    case MetadataKind::ExtendedExistential: {
      auto Exist = cast<TargetExtendedExistentialTypeMetadata<Runtime>>(Meta);

      // Read the shape for this existential.
      StoredPointer shapeAddress = stripSignedPointer(Exist->Shape);
      ShapeRef Shape = readShape(shapeAddress);
      if (!Shape)
        return BuiltType();

      const unsigned shapeArgumentCount
          = Shape->getGenSigArgumentLayoutSizeInWords();
      // Pull out the arguments to the generalization signature.
      assert(Shape->hasGeneralizationSignature());
      std::vector<BuiltType> builtArgs;
      for (unsigned i = 0; i < shapeArgumentCount; ++i) {
        auto remoteArg = Exist->getGeneralizationArguments()[i];
        auto builtArg = readTypeFromMetadata(remoteArg, false, recursion_limit);
        if (!builtArg)
          return BuiltType();
        builtArgs.push_back(builtArg);
      }

      // Pull out the existential type from the mangled type name.
      Demangler dem;
      auto mangledExistentialAddr =
          resolveRelativeField(Shape, Shape->ExistentialType);
      auto node = readMangledName(RemoteAddress(mangledExistentialAddr),
                                  MangledNameKind::Type, dem);
      if (!node)
        return BuiltType();

      BuiltType builtProto = decodeMangledType(node).getType();
      if (!builtProto)
        return BuiltType();

      // Build up a substitution map for the generalized signature.
      BuiltGenericSignature sig =
          decodeRuntimeGenericSignature(Shape,
                                        Shape->getGeneralizationSignature())
              .getType();
      if (!sig)
        return BuiltType();

      BuiltSubstitutionMap subst =
          Builder.createSubstitutionMap(sig, builtArgs);
      if (subst.empty())
        return BuiltType();

      builtProto = Builder.subst(builtProto, subst);
      if (!builtProto)
        return BuiltType();

      // Read the type expression to build up any remaining layers of
      // existential metatype.
      if (Shape->Flags.hasTypeExpression()) {
        Demangler dem;

        // Read the mangled name.
        auto mangledContextName = Shape->getTypeExpression();
        auto mangledNameAddress =
            resolveRelativeField(Shape, mangledContextName->name);
        auto node = readMangledName(RemoteAddress(mangledNameAddress),
                                    MangledNameKind::Type, dem);
        if (!node)
          return BuiltType();

        while (node->getKind() == Demangle::Node::Kind::Type &&
               node->getNumChildren() &&
               node->getChild(0)->getKind() == Demangle::Node::Kind::Metatype &&
               node->getChild(0)->getNumChildren()) {
          builtProto = Builder.createExistentialMetatypeType(builtProto);
          node = node->getChild(0)->getChild(0);
        }
      }

      TypeCache[MetadataAddress] = builtProto;
      return builtProto;
    }

    case MetadataKind::Metatype: {
      auto Metatype = cast<TargetMetatypeMetadata<Runtime>>(Meta);
      auto Instance =
          readTypeFromMetadata(Metatype->InstanceType, false, recursion_limit);
      if (!Instance) return BuiltType();
      auto BuiltMetatype = Builder.createMetatypeType(Instance);
      TypeCache[MetadataAddress] = BuiltMetatype;
      return BuiltMetatype;
    }
    case MetadataKind::ObjCClassWrapper: {
      auto objcWrapper = cast<TargetObjCClassWrapperMetadata<Runtime>>(Meta);
      auto classAddress = objcWrapper->Class;

      std::string className;
      if (!readObjCClassName(classAddress, className))
        return BuiltType();

      auto BuiltObjCClass = Builder.createObjCClassType(std::move(className));
      TypeCache[MetadataAddress] = BuiltObjCClass;
      return BuiltObjCClass;
    }
    case MetadataKind::ExistentialMetatype: {
      auto Exist = cast<TargetExistentialMetatypeMetadata<Runtime>>(Meta);
      auto Instance =
          readTypeFromMetadata(Exist->InstanceType, false, recursion_limit);
      if (!Instance) return BuiltType();
      auto BuiltExist = Builder.createExistentialMetatypeType(Instance);
      TypeCache[MetadataAddress] = BuiltExist;
      return BuiltExist;
    }
    case MetadataKind::ForeignReferenceType:
    case MetadataKind::ForeignClass: {
      auto descriptorAddr = readAddressOfNominalTypeDescriptor(Meta);
      if (!descriptorAddr)
        return BuiltType();
      auto descriptor = readContextDescriptor(descriptorAddr);
      if (!descriptor)
        return BuiltType();

      // Build the demangling tree from the context tree.
      Demangler dem;
      auto node = buildContextMangling(descriptor, dem);
      if (!node || node->getKind() != Node::Kind::Type)
        return BuiltType();

      auto mangling = Demangle::mangleNode(node);
      if (!mangling.isSuccess())
        return BuiltType();
      auto name = mangling.result();

      auto BuiltForeign = Builder.createForeignClassType(std::move(name));
      TypeCache[MetadataAddress] = BuiltForeign;
      return BuiltForeign;
    }
    case MetadataKind::HeapLocalVariable:
    case MetadataKind::HeapGenericLocalVariable:
    case MetadataKind::ErrorObject:
      // Treat these all as Builtin.NativeObject for type lowering purposes.
      return Builder.createBuiltinType("Builtin.NativeObject", "Bo");
    case MetadataKind::Opaque:
    default: {
      auto BuiltOpaque = Builder.getOpaqueType();
      TypeCache[MetadataAddress] = BuiltOpaque;
      return BuiltOpaque;
    }
    }

    swift_unreachable("Unhandled MetadataKind in switch");
  }

  TypeLookupErrorOr<typename BuilderType::BuiltGenericSignature>
  decodeRuntimeGenericSignature(ShapeRef contextRef,
                                const RuntimeGenericSignature<Runtime> &Sig) {
    std::vector<BuiltType> params;
    for (unsigned sigIdx : indices(Sig.getParams())) {
      auto param =
          Builder.createGenericTypeParameterType(/*depth*/ 0, /*index*/ sigIdx);
      if (!param)
        return TypeLookupError("Failed to read generic parameter type in "
                               "runtime generic signature.");
      params.push_back(param);
    }

    std::vector<BuiltRequirement> reqs;
    for (auto &req : Sig.getRequirements()) {
      if (!req.hasKnownKind()) {
        return TypeLookupError("unknown kind");
      }

      Demangler ldem;
      auto lhsTypeNode = ldem.demangleType(req.getParam());
      if (!lhsTypeNode) {
        return TypeLookupError("Failed to read subject type in requirement of "
                               "runtime generic signature.");
      }

      BuiltType subjectType = decodeMangledType(lhsTypeNode).getType();
      if (!subjectType)
        return TypeLookupError("Failed to read subject type in requirement of "
                               "runtime generic signature.");

      switch (req.Flags.getKind()) {
      case GenericRequirementKind::SameType: {
        Demangler rdem;
        auto demangledConstraint =
            demangle(RemoteRef<char>(req.getMangledTypeName().data(),
                                     req.getMangledTypeName().data()),
                     MangledNameKind::Type, rdem);
        auto constraintType = decodeMangledType(demangledConstraint);
        if (auto *error = constraintType.getError()) {
          return *error;
        }

        reqs.push_back(BuiltRequirement(RequirementKind::SameType, subjectType,
                                        constraintType.getType()));
        break;
      }
      case GenericRequirementKind::Protocol: {
        /// Resolver to turn a protocol reference into an existential type.
        struct ProtocolReferenceResolver {
          using Result = BuiltType;

          BuilderType &builder;

          BuiltType failure() const { return BuiltType(); }

          BuiltType swiftProtocol(Demangle::Node *node) {
            auto decl = builder.createProtocolDecl(node);
            if (!decl)
              return failure();
            return builder.createProtocolTypeFromDecl(decl);
          }

#if SWIFT_OBJC_INTEROP
          BuiltType objcProtocol(StringRef name) {
            auto decl = builder.createObjCProtocolDecl(name.str());
            if (!decl)
              return failure();
            return builder.createProtocolTypeFromDecl(decl);
          }
#endif
        } resolver{Builder};

        Demangler dem;
        auto protocolAddress =
            resolveRelativeIndirectProtocol(contextRef, req.Protocol);
        auto protocol = readProtocol(protocolAddress, dem, resolver);
        if (!protocol) {
          return TypeLookupError("Failed to read protocol type in conformance "
                                 "requirement of runtime generic signature.");
        }

        reqs.push_back(BuiltRequirement(RequirementKind::Conformance,
                                        subjectType, protocol));
        break;
      }
      case GenericRequirementKind::BaseClass: {
        Demangler rdem;
        auto demangledConstraint =
            demangle(RemoteRef<char>(req.getMangledTypeName().data(),
                                     req.getMangledTypeName().data()),
                     MangledNameKind::Type, rdem);
        auto constraintType = decodeMangledType(demangledConstraint);
        if (auto *error = constraintType.getError()) {
          return *error;
        }

        reqs.push_back(BuiltRequirement(RequirementKind::Superclass,
                                        subjectType, constraintType.getType()));
        break;
      }
      case GenericRequirementKind::SameConformance:
        return TypeLookupError("Unexpected same conformance requirement in "
                               "runtime generic signature");
      case GenericRequirementKind::Layout:
        return TypeLookupError(
            "Unexpected layout requirement in runtime generic signature");
      case GenericRequirementKind::SameShape:
        return TypeLookupError(
            "Unexpected same-shape requirement in runtime generic signature");
      }
    }

    return Builder.createGenericSignature(params, reqs);
  }

  TypeLookupErrorOr<typename BuilderType::BuiltType>
  readTypeFromMangledName(const char *MangledTypeName, size_t Length) {
    Demangle::Demangler Dem;
    Demangle::NodePointer Demangled =
      Dem.demangleSymbol(StringRef(MangledTypeName, Length));
    return decodeMangledType(Demangled);
  }

  /// Given the address of a context descriptor, attempt to read it, or
  /// represent it symbolically.
  ParentContextDescriptorRef
  readContextDescriptor(const RemoteAbsolutePointer &address) {
    // Map an unresolved pointer to an unresolved context ref.
    if (!address.isResolved()) {
      // We can only handle references to a symbol without an offset currently.
      if (address.getOffset() != 0) {
        return ParentContextDescriptorRef();
      }
      return ParentContextDescriptorRef(address.getSymbol());
    }
    
    return ParentContextDescriptorRef(
          readContextDescriptor(address.getResolvedAddress().getAddressData()));
  }

  ShapeRef
  readShape(StoredPointer address) {
    if (address == 0)
      return nullptr;

    auto cached = ShapeCache.find(address);
    if (cached != ShapeCache.end())
      return ShapeRef(address,
        reinterpret_cast<const TargetExtendedExistentialTypeShape<Runtime> *>(
            cached->second.get()));

    ExtendedExistentialTypeShapeFlags flags;
    if (!Reader->readBytes(RemoteAddress(address), (uint8_t*)&flags,
                           sizeof(flags)))
      return nullptr;

    // Read the size of the requirement signature.
    uint64_t reqSigGenericSize = 0;
    uint64_t genericHeaderSize = sizeof(GenericContextDescriptorHeader);
    {
      GenericContextDescriptorHeader header;
      auto headerAddr = address + sizeof(flags);

      if (!Reader->readBytes(RemoteAddress(headerAddr),
                             (uint8_t*)&header, sizeof(header)))
        return nullptr;

      reqSigGenericSize = reqSigGenericSize
        + (header.NumParams + 3u & ~3u)
        + header.NumRequirements
          * sizeof(TargetGenericRequirementDescriptor<Runtime>);
    }
    uint64_t typeExprSize = flags.hasTypeExpression() ? sizeof(StoredPointer) : 0;
    uint64_t suggestedVWSize = flags.hasSuggestedValueWitnesses() ? sizeof(StoredPointer) : 0;

    uint64_t size = sizeof(ExtendedExistentialTypeShapeFlags) +
                    sizeof(TargetRelativeDirectPointer<Runtime, const char,
                                                       /*nullable*/ false>) +
                    genericHeaderSize + typeExprSize + suggestedVWSize +
                    reqSigGenericSize;
    if (size > MaxMetadataSize)
      return nullptr;
    auto readResult = Reader->readBytes(RemoteAddress(address), size);
    if (!readResult)
      return nullptr;

    auto descriptor =
        reinterpret_cast<const TargetExtendedExistentialTypeShape<Runtime> *>(
            readResult.get());

    ShapeCache.insert(
        std::make_pair(address, std::move(readResult)));
    return ShapeRef(address, descriptor);
  }

  /// Given the address of a context descriptor, attempt to read it.
  ContextDescriptorRef
  readContextDescriptor(StoredPointer address) {
    if (address == 0)
      return nullptr;

    auto cached = ContextDescriptorCache.find(address);
    if (cached != ContextDescriptorCache.end())
      return ContextDescriptorRef(
          address, reinterpret_cast<const TargetContextDescriptor<Runtime> *>(
                       cached->second.get()));

    // Read the flags to figure out how much space we should read.
    ContextDescriptorFlags flags;
    if (!Reader->readBytes(RemoteAddress(address), (uint8_t*)&flags,
                           sizeof(flags)))
      return nullptr;
    
    TypeContextDescriptorFlags typeFlags(flags.getKindSpecificFlags());
    uint64_t baseSize = 0;
    uint64_t genericHeaderSize = sizeof(GenericContextDescriptorHeader);
    uint64_t metadataInitSize = 0;
    bool hasVTable = false;

    auto readMetadataInitSize = [&]() -> unsigned {
      switch (typeFlags.getMetadataInitialization()) {
      case TypeContextDescriptorFlags::NoMetadataInitialization:
        return 0;
      case TypeContextDescriptorFlags::SingletonMetadataInitialization:
        // FIXME: classes
        return sizeof(TargetSingletonMetadataInitialization<Runtime>);
      case TypeContextDescriptorFlags::ForeignMetadataInitialization:
        return sizeof(TargetForeignMetadataInitialization<Runtime>);
      }
      return 0;
    };

    switch (auto kind = flags.getKind()) {
    case ContextDescriptorKind::Module:
      baseSize = sizeof(TargetModuleContextDescriptor<Runtime>);
      break;
    // TODO: Should we include trailing generic arguments in this load?
    case ContextDescriptorKind::Extension:
      baseSize = sizeof(TargetExtensionContextDescriptor<Runtime>);
      break;
    case ContextDescriptorKind::Anonymous:
      baseSize = sizeof(TargetAnonymousContextDescriptor<Runtime>);
      if (AnonymousContextDescriptorFlags(flags.getKindSpecificFlags())
            .hasMangledName()) {
        metadataInitSize = sizeof(TargetMangledContextName<Runtime>);
      }
      break;
    case ContextDescriptorKind::Class:
      baseSize = sizeof(TargetClassDescriptor<Runtime>);
      genericHeaderSize = sizeof(TypeGenericContextDescriptorHeader);
      hasVTable = typeFlags.class_hasVTable();
      metadataInitSize = readMetadataInitSize();
      break;
    case ContextDescriptorKind::Enum:
      baseSize = sizeof(TargetEnumDescriptor<Runtime>);
      genericHeaderSize = sizeof(TypeGenericContextDescriptorHeader);
      metadataInitSize = readMetadataInitSize();
      break;
    case ContextDescriptorKind::Struct:
      baseSize = sizeof(TargetStructDescriptor<Runtime>);
      genericHeaderSize = sizeof(TypeGenericContextDescriptorHeader);
      metadataInitSize = readMetadataInitSize();
      break;
    case ContextDescriptorKind::Protocol:
      baseSize = sizeof(TargetProtocolDescriptor<Runtime>);
      break;
    case ContextDescriptorKind::OpaqueType:
      baseSize = sizeof(TargetOpaqueTypeDescriptor<Runtime>);
      metadataInitSize =
        sizeof(typename Runtime::template RelativeDirectPointer<const char>)
          * flags.getKindSpecificFlags();
      break;
    default:
      // We don't know about this kind of context.
      return nullptr;
    }

    // Determine the full size of the descriptor. This is reimplementing a fair
    // bit of TrailingObjects but for out-of-process; maybe there's a way to
    // factor the layout stuff out...
    uint64_t genericsSize = 0;
    if (flags.isGeneric()) {
      GenericContextDescriptorHeader header;
      auto headerAddr = address
        + baseSize
        + genericHeaderSize
        - sizeof(header);
      
      if (!Reader->readBytes(RemoteAddress(headerAddr),
                             (uint8_t*)&header, sizeof(header)))
        return nullptr;
      
      genericsSize = genericHeaderSize
        + (header.NumParams + 3u & ~3u)
        + header.NumRequirements
          * sizeof(TargetGenericRequirementDescriptor<Runtime>);
    }

    uint64_t vtableSize = 0;
    if (hasVTable) {
      TargetVTableDescriptorHeader<Runtime> header;
      auto headerAddr = address
        + baseSize
        + genericsSize
        + metadataInitSize;
      
      if (!Reader->readBytes(RemoteAddress(headerAddr),
                             (uint8_t*)&header, sizeof(header)))
        return nullptr;

      vtableSize = sizeof(header)
        + header.VTableSize * sizeof(TargetMethodDescriptor<Runtime>);
    }

    uint64_t size = baseSize + genericsSize + metadataInitSize + vtableSize;
    if (size > MaxMetadataSize)
      return nullptr;
    auto readResult = Reader->readBytes(RemoteAddress(address), size);
    if (!readResult)
      return nullptr;

    auto descriptor =
        reinterpret_cast<const TargetContextDescriptor<Runtime> *>(
            readResult.get());

    ContextDescriptorCache.insert(
        std::make_pair(address, std::move(readResult)));
    return ContextDescriptorRef(address, descriptor);
  }
  
  /// Demangle the entity represented by a symbolic reference to a given symbol name.
  Demangle::NodePointer
  buildContextManglingForSymbol(StringRef symbol, Demangler &dem) {
    auto demangledSymbol = dem.demangleSymbol(symbol);
    if (demangledSymbol->getKind() == Demangle::Node::Kind::Global) {
      demangledSymbol = demangledSymbol->getChild(0);
    }
    
    switch (demangledSymbol->getKind()) {
    // Pointers to nominal type or protocol descriptors would demangle to
    // the type they represent.
    case Demangle::Node::Kind::NominalTypeDescriptor:
    case Demangle::Node::Kind::ProtocolDescriptor:
      demangledSymbol = demangledSymbol->getChild(0);
      assert(demangledSymbol->getKind() == Demangle::Node::Kind::Type);
      break;
    // Pointers to opaque type descriptors demangle to the name of the opaque
    // type declaration.
    case Demangle::Node::Kind::OpaqueTypeDescriptor:
      demangledSymbol = demangledSymbol->getChild(0);
      break;
      // We don't handle pointers to other symbols yet.
    default:
      return nullptr;
    }
  
    return demangledSymbol;
  }

  Demangle::NodePointer buildContextManglingForSymbol(const std::string &symbol,
                                                      Demangler &dem) {
    return buildContextManglingForSymbol(dem.copyString(symbol), dem);
  }

  /// Given a read context descriptor, attempt to build a demangling tree
  /// for it.
  Demangle::NodePointer
  buildContextMangling(const ParentContextDescriptorRef &descriptor,
                       Demangler &dem) {
    if (descriptor.isResolved()) {
      return buildContextMangling(descriptor.getResolved(), dem);
    }

    // Try to demangle the symbol name to figure out what context it would
    // point to.
    return buildContextManglingForSymbol(descriptor.getSymbol(), dem);
  }

  /// Given a read context descriptor, attempt to build a demangling tree
  /// for it.
  Demangle::NodePointer
  buildContextMangling(ContextDescriptorRef descriptor,
                       Demangler &dem) {
    auto demangling = buildContextDescriptorMangling(descriptor, dem, 50);
    if (!demangling)
      return nullptr;

    Demangle::NodePointer top;
    // References to type nodes behave as types in the mangling.
    if (isa<TargetTypeContextDescriptor<Runtime>>(descriptor.getLocalBuffer()) ||
        isa<TargetProtocolDescriptor<Runtime>>(descriptor.getLocalBuffer())) {
      top = dem.createNode(Node::Kind::Type);
      top->addChild(demangling, dem);
    } else {
      top = demangling;
    }
    
    return top;
  }

  /// Read a context descriptor from the given address and build a mangling
  /// tree representing it.
  Demangle::NodePointer
  readDemanglingForContextDescriptor(StoredPointer contextAddress,
                                     Demangler &Dem) {
    auto context = readContextDescriptor(contextAddress);
    if (!context)
      return nullptr;
    return buildContextMangling(context, Dem);
  }
  
  /// Read the mangled underlying type from an opaque type descriptor.
  Demangle::NodePointer
  readUnderlyingTypeManglingForOpaqueTypeDescriptor(StoredPointer contextAddr,
                                                    unsigned ordinal,
                                                    Demangler &Dem) {
    auto context = readContextDescriptor(contextAddr);
    if (!context)
      return nullptr;
    if (context->getKind() != ContextDescriptorKind::OpaqueType)
      return nullptr;
    
    auto opaqueType =
      reinterpret_cast<const TargetOpaqueTypeDescriptor<Runtime> *>(
                                                      context.getLocalBuffer());

    if (ordinal >= opaqueType->getNumUnderlyingTypeArguments())
      return nullptr;
    
    auto nameAddr = resolveRelativeField(context,
                     opaqueType->getUnderlyingTypeArgumentMangledName(ordinal));
    
    return readMangledName(RemoteAddress(nameAddr),
                                MangledNameKind::Type, Dem);
  }

  TypeLookupErrorOr<typename BuilderType::BuiltType>
  readUnderlyingTypeForOpaqueTypeDescriptor(StoredPointer contextAddr,
                                            unsigned ordinal) {
    Demangle::Demangler Dem;
    auto node = readUnderlyingTypeManglingForOpaqueTypeDescriptor(contextAddr,
                                                                  ordinal, Dem);
    if (!node)
      return TypeLookupError("Failed to read type mangling for descriptor.");
    return decodeMangledType(node);
  }

  bool isTaggedPointer(StoredPointer objectAddress) {
    if (getTaggedPointerEncoding() != TaggedPointerEncodingKind::Extended)
      return false;
  
    return (objectAddress ^ TaggedPointerObfuscator) & TaggedPointerMask;
  }

  /// Read the isa pointer of an Object-C tagged pointer value.
  llvm::Optional<StoredPointer>
  readMetadataFromTaggedPointer(StoredPointer objectAddress) {
    auto readArrayElement =
        [&](StoredPointer base,
            StoredPointer tag) -> llvm::Optional<StoredPointer> {
      StoredPointer addr = base + tag * sizeof(StoredPointer);
      StoredPointer isa;
      if (!Reader->readInteger(RemoteAddress(addr), &isa))
        return llvm::None;
      return isa;
    };

    // Extended pointers have a tag of 0b111, using 8 additional bits
    // to specify the class.
    if (TaggedPointerExtendedMask != 0 &&
        (((objectAddress ^ TaggedPointerObfuscator) & TaggedPointerExtendedMask)
           == TaggedPointerExtendedMask)) {
      auto tag = ((objectAddress >> TaggedPointerExtendedSlotShift) &
                  TaggedPointerExtendedSlotMask);
      return readArrayElement(TaggedPointerExtendedClasses, tag);
    }

    // Basic tagged pointers use a 3 bit tag to specify the class.
    auto tag = ((objectAddress >> TaggedPointerSlotShift) &
                TaggedPointerSlotMask);
    return readArrayElement(TaggedPointerClasses, tag);
  }

  /// Read the isa pointer of a class or closure context instance and apply
  /// the isa mask.
  llvm::Optional<StoredPointer>
  readMetadataFromInstance(StoredPointer objectAddress) {
    if (isTaggedPointer(objectAddress))
      return readMetadataFromTaggedPointer(objectAddress);

    StoredPointer isa;
    if (!Reader->readInteger(RemoteAddress(objectAddress), &isa))
      return llvm::None;

    switch (getIsaEncoding()) {
    case IsaEncodingKind::Unknown:
    case IsaEncodingKind::Error:
      return llvm::None;

    case IsaEncodingKind::None:
      return isa;

    case IsaEncodingKind::Masked:
      return isa & IsaMask;

    case IsaEncodingKind::Indexed: {
      // If applying the magic mask doesn't give us the magic value,
      // it's not an indexed isa.
      if ((isa & IsaMagicMask) != IsaMagicValue)
        return isa;

      // Extract the index.
      auto classIndex = (isa & IsaIndexMask) >> IsaIndexShift;

      // 0 is never a valid index.
      if (classIndex == 0) {
        return llvm::None;

        // If the index is out of range, it's an error; but check for an
        // update first.  (This will also trigger the first time because
        // we initialize LastIndexedClassesCount to 0).
      } else if (classIndex >= LastIndexedClassesCount) {
        StoredPointer count;
        if (!Reader->readInteger(RemoteAddress(IndexedClassesCountPointer),
                                 &count)) {
          return llvm::None;
        }

        LastIndexedClassesCount = count;
        if (classIndex >= count) {
          return llvm::None;
        }
      }

      // Find the address of the appropriate array element.
      RemoteAddress eltPointer =
        RemoteAddress(IndexedClassesPointer
                        + classIndex * sizeof(StoredPointer));
      StoredPointer metadataPointer;
      if (!Reader->readInteger(eltPointer, &metadataPointer)) {
        return llvm::None;
      }

      return metadataPointer;
    }
    }

    swift_unreachable("Unhandled IsaEncodingKind in switch.");
  }

  /// Read the offset of the generic parameters of a class from the nominal
  /// type descriptor. If the class has a resilient superclass, we also
  /// have to read the superclass size and add that to the offset.
  ///
  /// The offset is in units of words, from the start of the class's
  /// metadata.
  llvm::Optional<int32_t>
  readGenericArgsOffset(MetadataRef metadata, ContextDescriptorRef descriptor) {
    switch (descriptor->getKind()) {
    case ContextDescriptorKind::Class: {
      auto type = cast<TargetClassDescriptor<Runtime>>(descriptor);

      if (!type->hasResilientSuperclass())
        return type->getNonResilientGenericArgumentOffset();

      auto bounds = readMetadataBoundsOfSuperclass(descriptor);
      if (!bounds)
        return llvm::None;

      bounds->adjustForSubclass(type->areImmediateMembersNegative(),
                                type->NumImmediateMembers);

      return bounds->ImmediateMembersOffset / sizeof(StoredPointer);
    }

    case ContextDescriptorKind::Enum: {
      auto type = cast<TargetEnumDescriptor<Runtime>>(descriptor);
      return type->getGenericArgumentOffset();
    }

    case ContextDescriptorKind::Struct: {
      auto type = cast<TargetStructDescriptor<Runtime>>(descriptor);
      return type->getGenericArgumentOffset();
    }

    default:
      return llvm::None;
    }
  }

  using ClassMetadataBounds = TargetClassMetadataBounds<Runtime>;

  // This follows computeMetadataBoundsForSuperclass.
  llvm::Optional<ClassMetadataBounds>
  readMetadataBoundsOfSuperclass(ContextDescriptorRef subclassRef) {
    auto subclass = cast<TargetClassDescriptor<Runtime>>(subclassRef);
    if (!subclass->hasResilientSuperclass())
      return ClassMetadataBounds::forSwiftRootClass();

    auto rawSuperclass =
      resolveRelativeField(subclassRef, subclass->getResilientSuperclass());
    if (!rawSuperclass) {
      return ClassMetadataBounds::forSwiftRootClass();
    }

    return forTypeReference<ClassMetadataBounds>(
        subclass->getResilientSuperclassReferenceKind(), rawSuperclass,
        [&](ContextDescriptorRef superclass)
            -> llvm::Optional<ClassMetadataBounds> {
          if (!isa<TargetClassDescriptor<Runtime>>(superclass))
            return llvm::None;
          return readMetadataBoundsOfSuperclass(superclass);
        },
        [&](MetadataRef metadata) -> llvm::Optional<ClassMetadataBounds> {
          auto cls = dyn_cast<TargetClassMetadata>(metadata);
          if (!cls)
            return llvm::None;

          return cls->getClassBoundsAsSwiftSuperclass();
        },
        [](StoredPointer objcClassName) -> llvm::Optional<ClassMetadataBounds> {
          // We have no ability to look up an ObjC class by name.
          // FIXME: add a query for this; clients may have a way to do it.
          return llvm::None;
        });
  }

  template <class Result, class DescriptorFn, class MetadataFn,
            class ClassNameFn>
  llvm::Optional<Result> forTypeReference(TypeReferenceKind refKind,
                                          StoredPointer ref,
                                          const DescriptorFn &descriptorFn,
                                          const MetadataFn &metadataFn,
                                          const ClassNameFn &classNameFn) {
    switch (refKind) {
    case TypeReferenceKind::IndirectTypeDescriptor: {
      StoredPointer descriptorAddress = 0;
      if (!Reader->readInteger(RemoteAddress(ref), &descriptorAddress))
        return llvm::None;

      ref = descriptorAddress;
      LLVM_FALLTHROUGH;
    }

    case TypeReferenceKind::DirectTypeDescriptor: {
      auto descriptor = readContextDescriptor(ref);
      if (!descriptor)
        return llvm::None;

      return descriptorFn(descriptor);
    }

    case TypeReferenceKind::DirectObjCClassName:
      return classNameFn(ref);

    case TypeReferenceKind::IndirectObjCClass: {
      StoredPointer classRef = 0;
      if (!Reader->readInteger(RemoteAddress(ref), &classRef))
        return llvm::None;

      auto metadata = readMetadata(classRef);
      if (!metadata)
        return llvm::None;

      return metadataFn(metadata);
    }
    }

    return llvm::None;
  }

  /// Read a single generic type argument from a bound generic type
  /// metadata.
  llvm::Optional<StoredPointer>
  readGenericArgFromMetadata(StoredPointer metadata, unsigned index) {
    auto Meta = readMetadata(metadata);
    if (!Meta)
      return llvm::None;

    auto descriptorAddress = readAddressOfNominalTypeDescriptor(Meta);
    if (!descriptorAddress)
      return llvm::None;

    // Read the nominal type descriptor.
    auto descriptor = readContextDescriptor(descriptorAddress);
    if (!descriptor)
      return llvm::None;

    auto generics = descriptor->getGenericContext();
    if (!generics)
      return llvm::None;

    auto offsetToGenericArgs = readGenericArgsOffset(Meta, descriptor);
    if (!offsetToGenericArgs)
      return llvm::None;

    auto addressOfGenericArgAddress =
      (getAddress(Meta) +
       *offsetToGenericArgs * sizeof(StoredPointer) +
       index * sizeof(StoredPointer));

    if (index >= generics->getGenericContextHeader().getNumArguments())
      return llvm::None;

    StoredPointer genericArgAddress;
    if (!Reader->readInteger(RemoteAddress(addressOfGenericArgAddress),
                             &genericArgAddress))
      return llvm::None;

    return genericArgAddress;
  }

  /// Given the address of a nominal type descriptor, attempt to resolve
  /// its nominal type declaration.
  BuiltTypeDecl readNominalTypeFromDescriptor(StoredPointer address) {
    auto descriptor = readContextDescriptor(address);
    if (!descriptor)
      return BuiltTypeDecl();

    return buildNominalTypeDecl(descriptor);
  }

  /// Try to read the offset of a tuple element from a tuple metadata.
  bool readTupleElementOffset(StoredPointer metadataAddress, unsigned eltIndex,
                              StoredSize *offset) {
    // Read the metadata.
    auto metadata = readMetadata(metadataAddress);
    if (!metadata)
      return false;

    // Ensure that the metadata actually is tuple metadata.
    auto tupleMetadata = dyn_cast<TargetTupleTypeMetadata<Runtime>>(metadata);
    if (!tupleMetadata)
      return false;

    // Ensure that the element is in-bounds.
    if (eltIndex >= tupleMetadata->NumElements)
      return false;

    // Read the offset.
    const auto &element = tupleMetadata->getElement(eltIndex);
    *offset = element.Offset;
    return true;
  }

  /// Given a remote pointer to class metadata, attempt to read its superclass.
  llvm::Optional<StoredPointer>
  readOffsetToFirstCaptureFromMetadata(StoredPointer MetadataAddress) {
    auto meta = readMetadata(MetadataAddress);
    if (!meta || meta->getKind() != MetadataKind::HeapLocalVariable)
      return llvm::None;

    auto heapMeta = cast<TargetHeapLocalVariableMetadata<Runtime>>(meta);
    return heapMeta->OffsetToFirstCapture;
  }

  llvm::Optional<RemoteAbsolutePointer> readPointer(StoredPointer address) {
    return Reader->readPointer(RemoteAddress(address), sizeof(StoredPointer));
  }

  llvm::Optional<StoredPointer>
  readResolvedPointerValue(StoredPointer address) {
    if (auto pointer = readPointer(address)) {
      if (!pointer->isResolved())
        return llvm::None;
      return (StoredPointer)pointer->getResolvedAddress().getAddressData();
    }
    return llvm::None;
  }

  template<typename T, typename U>
  RemoteAbsolutePointer resolvePointerField(RemoteRef<T> base,
                                            const U &field) {
    auto pointerRef = base.getField(field);
    return Reader->resolvePointer(RemoteAddress(getAddress(pointerRef)),
                                  *pointerRef.getLocalBuffer());
  }

  /// Given a remote pointer to class metadata, attempt to read its superclass.
  llvm::Optional<RemoteAbsolutePointer>
  readCaptureDescriptorFromMetadata(StoredPointer MetadataAddress) {
    auto meta = readMetadata(MetadataAddress);
    if (!meta || meta->getKind() != MetadataKind::HeapLocalVariable)
      return llvm::None;

    auto heapMeta = cast<TargetHeapLocalVariableMetadata<Runtime>>(meta);
    return resolvePointerField(meta, heapMeta->CaptureDescription);
  }

protected:
  template<typename Base>
  StoredPointer getAddress(RemoteRef<Base> base) {
    return (StoredPointer)base.getAddressData();
  }
  
  template<typename Base, typename Field>
  StoredPointer resolveRelativeField(
                            RemoteRef<Base> base, const Field &field) {
    return (StoredPointer)base.resolveRelativeFieldData(field);
  }

  template <typename Base, typename Field>
  llvm::Optional<RemoteAbsolutePointer>
  resolveRelativeIndirectableField(RemoteRef<Base> base, const Field &field) {
    auto fieldRef = base.getField(field);
    int32_t offset;
    memcpy(&offset, fieldRef.getLocalBuffer(), sizeof(int32_t));
    
    if (offset == 0)
      return llvm::Optional<RemoteAbsolutePointer>(nullptr);
    bool indirect = offset & 1;
    offset &= ~1u;
    
    using SignedPointer = typename std::make_signed<StoredPointer>::type;
    
    StoredPointer resultAddress = getAddress(fieldRef) + (SignedPointer)offset;
    
    // Low bit set in the offset indicates that the offset leads to the absolute
    // address in memory.
    if (indirect) {
      if (auto ptr = readPointer(resultAddress)) {
        return stripSignedPointer(*ptr);
      }
      return llvm::None;
    }
    
    return RemoteAbsolutePointer("", resultAddress);
  }

  /// Given a pointer to an Objective-C class, try to read its class name.
  bool readObjCClassName(StoredPointer classAddress, std::string &className) {
    // The following algorithm only works on the non-fragile Apple runtime.

    // Grab the RO-data pointer.  This part is not ABI.
    StoredPointer roDataPtr = readObjCRODataPtr(classAddress);
    if (!roDataPtr) return false;

    // This is ABI.
    static constexpr auto OffsetToName =
    roundUpToAlignment(size_t(12), sizeof(StoredPointer))
      + sizeof(StoredPointer);

    // Read the name pointer.
    StoredPointer namePtr;
    if (!Reader->readInteger(RemoteAddress(roDataPtr + OffsetToName), &namePtr))
      return false;

    // If the name pointer is null, treat that as an error.
    if (!namePtr)
      return false;

    return Reader->readString(RemoteAddress(namePtr), className);
  }

  MetadataRef readMetadata(StoredPointer address) {
    auto cached = MetadataCache.find(address);
    if (cached != MetadataCache.end())
      return MetadataRef(address,
                         reinterpret_cast<const TargetMetadata<Runtime> *>(
                             cached->second.get()));

    StoredPointer KindValue = 0;
    if (!Reader->readInteger(RemoteAddress(address), &KindValue))
      return nullptr;

    switch (getEnumeratedMetadataKind(KindValue)) {
      case MetadataKind::Class:

        return _readMetadata<TargetClassMetadataType>(address);

      case MetadataKind::Enum:
        return _readMetadata<TargetEnumMetadata>(address);
      case MetadataKind::ErrorObject:
        return _readMetadata<TargetEnumMetadata>(address);
      case MetadataKind::Existential: {
        StoredPointer flagsAddress = address +
          sizeof(StoredPointer);

        ExistentialTypeFlags::int_type flagsData;
        if (!Reader->readInteger(RemoteAddress(flagsAddress),
                                 &flagsData))
          return nullptr;

        ExistentialTypeFlags flags(flagsData);

        StoredPointer numProtocolsAddress = flagsAddress + sizeof(flagsData);
        uint32_t numProtocols;
        if (!Reader->readInteger(RemoteAddress(numProtocolsAddress),
                                 &numProtocols))
          return nullptr;

        // Make sure the number of protocols is reasonable
        if (numProtocols >= 256)
          return nullptr;

        auto totalSize = sizeof(TargetExistentialTypeMetadata<Runtime>)
          + numProtocols *
          sizeof(ConstTargetMetadataPointer<Runtime, TargetProtocolDescriptor>);

        if (flags.hasSuperclassConstraint())
          totalSize += sizeof(StoredPointer);

        return _readMetadata(address, totalSize);
      }
      case MetadataKind::ExistentialMetatype:
        return _readMetadata<TargetExistentialMetatypeMetadata>(address);
      case MetadataKind::ExtendedExistential: {
        // We need to read the shape in order to figure out how large
        // the generalization arguments are.
        StoredPointer shapeAddress = address + sizeof(StoredPointer);
        StoredSignedPointer signedShapePtr;
        if (!Reader->readInteger(RemoteAddress(shapeAddress), &signedShapePtr))
          return nullptr;
        auto shapePtr = stripSignedPointer(signedShapePtr);

        auto shape = readShape(shapePtr);
        if (!shape)
          return nullptr;

        auto totalSize =
            sizeof(TargetExtendedExistentialTypeMetadata<Runtime>)
          + shape->getGeneralizationSignature().getArgumentLayoutSizeInWords()
              * sizeof(StoredPointer);
        return _readMetadata(address, totalSize);
      }
      case MetadataKind::ForeignClass:
        return _readMetadata<TargetForeignClassMetadata>(address);
      case MetadataKind::ForeignReferenceType:
        return _readMetadata<TargetForeignReferenceTypeMetadata>(address);
      case MetadataKind::Function: {
        StoredSize flagsValue;
        auto flagsAddr =
            address + TargetFunctionTypeMetadata<Runtime>::OffsetToFlags;
        if (!Reader->readInteger(RemoteAddress(flagsAddr), &flagsValue))
          return nullptr;

        auto flags =
            TargetFunctionTypeFlags<StoredSize>::fromIntValue(flagsValue);

        auto totalSize =
            sizeof(TargetFunctionTypeMetadata<Runtime>) +
            flags.getNumParameters() * sizeof(FunctionTypeMetadata::Parameter);

        if (flags.hasParameterFlags())
          totalSize += flags.getNumParameters() * sizeof(uint32_t);

        if (flags.isDifferentiable())
          totalSize = roundUpToAlignment(totalSize, sizeof(StoredPointer)) +
              sizeof(TargetFunctionMetadataDifferentiabilityKind<
                  typename Runtime::StoredSize>);

        return _readMetadata(address,
                             roundUpToAlignment(totalSize, sizeof(StoredPointer)));
      }
      case MetadataKind::HeapGenericLocalVariable:
        return _readMetadata<TargetGenericBoxHeapMetadata>(address);
      case MetadataKind::HeapLocalVariable:
        return _readMetadata<TargetHeapLocalVariableMetadata>(address);
      case MetadataKind::Metatype:
        return _readMetadata<TargetMetatypeMetadata>(address);
      case MetadataKind::ObjCClassWrapper:
        return _readMetadata<TargetObjCClassWrapperMetadata>(address);
      case MetadataKind::Optional:
        return _readMetadata<TargetEnumMetadata>(address);
      case MetadataKind::Struct:
        return _readMetadata<TargetStructMetadata>(address);
      case MetadataKind::Tuple: {
        auto numElementsAddress = address +
          TargetTupleTypeMetadata<Runtime>::getOffsetToNumElements();
        StoredSize numElements;
        if (!Reader->readInteger(RemoteAddress(numElementsAddress),
                                 &numElements))
          return nullptr;
        auto totalSize = sizeof(TargetTupleTypeMetadata<Runtime>) +
                         numElements * sizeof(TupleTypeMetadata::Element);

        // Make sure the number of elements is reasonable
        if (numElements >= 256)
          return nullptr;

        return _readMetadata(address, totalSize);
      }
      case MetadataKind::Opaque:
      default:
        return _readMetadata<TargetOpaqueMetadata>(address);
    }

    // We can fall out here if the value wasn't actually a valid
    // MetadataKind.
    return nullptr;
  }

  StoredPointer
  readAddressOfNominalTypeDescriptor(MetadataRef &metadata,
                                     bool skipArtificialSubclasses = false) {
    switch (metadata->getKind()) {
    case MetadataKind::Class: {
      auto classMeta = cast<TargetClassMetadata>(metadata);
      while (true) {
        if (!classMeta->isTypeMetadata())
          return 0;

        StoredSignedPointer descriptorAddressSigned = classMeta->getDescriptionAsSignedPointer();
        StoredPointer descriptorAddress = stripSignedPointer(descriptorAddressSigned);

        // If this class has a null descriptor, it's artificial,
        // and we need to skip it upon request.  Otherwise, we're done.
        if (descriptorAddress || !skipArtificialSubclasses)
          return static_cast<StoredPointer>(descriptorAddress);

        auto superclassMetadataAddress =
            stripSignedPointer(classMeta->Superclass);
        if (!superclassMetadataAddress)
          return 0;

        auto superMeta = readMetadata(superclassMetadataAddress);
        if (!superMeta)
          return 0;

        auto superclassMeta = dyn_cast<TargetClassMetadata>(superMeta);
        if (!superclassMeta)
          return 0;

        classMeta = superclassMeta;
        metadata = superMeta;
      }
    }

    case MetadataKind::Struct:
    case MetadataKind::Optional:
    case MetadataKind::Enum: {
      auto valueMeta = cast<TargetValueMetadata<Runtime>>(metadata);
      StoredSignedPointer descriptorAddressSigned = valueMeta->getDescriptionAsSignedPointer();
      StoredPointer descriptorAddress = stripSignedPointer(descriptorAddressSigned);
      return descriptorAddress;
    }
        
    case MetadataKind::ForeignClass: {
      auto foreignMeta = cast<TargetForeignClassMetadata<Runtime>>(metadata);
      StoredSignedPointer descriptorAddressSigned = foreignMeta->getDescriptionAsSignedPointer();
      StoredPointer descriptorAddress = stripSignedPointer(descriptorAddressSigned);
      return descriptorAddress;
    }

    case MetadataKind::ForeignReferenceType: {
      auto foreignMeta = cast<TargetForeignReferenceTypeMetadata<Runtime>>(metadata);
      StoredSignedPointer descriptorAddressSigned = foreignMeta->getDescriptionAsSignedPointer();
      StoredPointer descriptorAddress = stripSignedPointer(descriptorAddressSigned);
      return descriptorAddress;
    }

    default:
      return 0;
    }
  }

private:
  template <template <class R> class M>
  MetadataRef _readMetadata(StoredPointer address) {
    return _readMetadata(address, sizeof(M<Runtime>));
  }

  MetadataRef _readMetadata(StoredPointer address, size_t sizeAfter) {
    if (sizeAfter > MaxMetadataSize)
      return nullptr;
    auto readResult = Reader->readBytes(RemoteAddress(address), sizeAfter);
    if (!readResult)
      return nullptr;

    auto metadata =
        reinterpret_cast<const TargetMetadata<Runtime> *>(readResult.get());
    MetadataCache.insert(std::make_pair(address, std::move(readResult)));
    return MetadataRef(address, metadata);
  }

  /// Returns Optional(ParentContextDescriptorRef()) if there's no parent descriptor.
  /// Returns None if there was an error reading the parent descriptor.
  llvm::Optional<ParentContextDescriptorRef>
  readParentContextDescriptor(ContextDescriptorRef base) {
    auto parentAddress = resolveRelativeIndirectableField(base, base->Parent);
    if (!parentAddress)
      return llvm::None;
    if (!parentAddress->isResolved()) {
      // Currently we can only handle references directly to a symbol without
      // an offset.
      if (parentAddress->getOffset() != 0) {
        return llvm::None;
      }
      return ParentContextDescriptorRef(parentAddress->getSymbol());
    }
    auto addr = parentAddress->getResolvedAddress();
    if (!addr)
      return ParentContextDescriptorRef();
    if (auto parentDescriptor = readContextDescriptor(addr.getAddressData()))
      return ParentContextDescriptorRef(parentDescriptor);
    return llvm::None;
  }

  static bool isCImportedContext(Demangle::NodePointer node) {
    do {
      if (node->getKind() == Demangle::Node::Kind::Module) {
        return isCImportedModuleName(node->getText());
      }

      // Continue to the parent.
      node = node->getChild(0);
    } while (node);
    return false;
  }

  /// Read the name from a module, type, or protocol context descriptor.
  llvm::Optional<std::string> readContextDescriptorName(
      ContextDescriptorRef descriptor,
      llvm::Optional<TypeImportInfo<std::string>> &importInfo) {
    std::string name;
    auto context = descriptor.getLocalBuffer();

    // Read the name of a protocol.
    if (auto protoBuffer =
            dyn_cast<TargetProtocolDescriptor<Runtime>>(context)) {
      auto nameAddress = resolveRelativeField(descriptor, protoBuffer->Name);
      if (Reader->readString(RemoteAddress(nameAddress), name))
        return name;

      return llvm::None;
    }

    // Read the name of a module.
    if (auto moduleBuffer =
            dyn_cast<TargetModuleContextDescriptor<Runtime>>(context)) {
      auto nameAddress = resolveRelativeField(descriptor, moduleBuffer->Name);
      if (Reader->readString(RemoteAddress(nameAddress), name))
        return name;

      return llvm::None;
    }

    // Only type contexts remain.
    auto typeBuffer = dyn_cast<TargetTypeContextDescriptor<Runtime>>(context);
    if (!typeBuffer)
      return llvm::None;

    auto nameAddress = resolveRelativeField(descriptor, typeBuffer->Name);
    if (!Reader->readString(RemoteAddress(nameAddress), name))
      return llvm::None;

    // Read the TypeImportInfo if present.
    if (typeBuffer->getTypeContextDescriptorFlags().hasImportInfo()) {
      importInfo.emplace();
      nameAddress += name.size() + 1;

      while (true) {
        // Read the next string.
        std::string temp;
        if (!Reader->readString(RemoteAddress(nameAddress), temp))
          return llvm::None;

        // If we read an empty string, we're done.
        if (temp.empty())
          break;

        // Advance past the string.
        nameAddress += temp.size() + 1;

        // Collect the import information.  Ignore anything we don't
        // understand.
        importInfo->collect</*asserting*/false>(std::move(temp));
      }

      // Ignore the original if we have an ABI name override.
      if (!importInfo->ABIName.empty())
        name = std::move(importInfo->ABIName);
    }

    return name;
  }

  /// Clone the given demangle node into the demangler \c dem.
  static Demangle::NodePointer cloneDemangleNode(Demangle::NodePointer node,
                                                 Demangler &dem) {
    if (!node)
      return nullptr;

    Demangle::NodePointer newNode;
    if (node->hasText())
      newNode = dem.createNode(node->getKind(), node->getText());
    else if (node->hasIndex())
      newNode = dem.createNode(node->getKind(), node->getIndex());
    else
      newNode = dem.createNode(node->getKind());

    for (auto child : *node) {
      newNode->addChild(cloneDemangleNode(child, dem), dem);
    }
    return newNode;
  }
  
  /// Read a mangled name at the given remote address and return the
  /// demangle tree.
  Demangle::NodePointer readMangledName(RemoteAddress address,
                                        MangledNameKind kind,
                                        Demangler &dem) {
    // Read chunks of the mangled name, for which the string can be
    // prematurely terminated by a symbolic reference.
    std::string mangledName;
    RemoteAddress currentAddress = address;
    unsigned index = 0;
    while (true) {
      // Read the next chunk.
      std::string chunk;
      if (!Reader->readString(currentAddress, chunk))
        return nullptr;

      // Move the address forward and add the next chunk.
      currentAddress =
        RemoteAddress(currentAddress.getAddressData() + chunk.size() + 1);
      mangledName += std::move(chunk);

      // Scan through the mangled name to skip over symbolic references.
      unsigned end = mangledName.size();
      while (index < end) {
        char c = mangledName[index];

        // Figure out how far to step in the string.
        unsigned step = 1;
        if (c >= '\x01' && c <= '\x17') {
          step += sizeof(uint32_t);
        }
        else if (c >= '\x18' && c <= '\x1F') {
          step += sizeof(typename Runtime::StoredPointer);
        }

        // If we would be stepping past the end, break out.
        if (index + step > end)
          break;

        index += step;
      }

      // If we didn't make it to the end, the '\0' is significant. Add it to the
      // mangled name and
      if (index < end) {
        mangledName.push_back('\0');
        continue;
      }

      // We're done.
      break;
    }
    return demangle(address.getAddressData(), mangledName, kind, dem);
  }

  /// Read and demangle the name of an anonymous context.
  Demangle::NodePointer demangleAnonymousContextName(
      ContextDescriptorRef contextRef,
      Demangler &dem) {
    auto anonymousBuffer = cast<TargetAnonymousContextDescriptor<Runtime>>(
        contextRef.getLocalBuffer());

    if (!anonymousBuffer->hasMangledName())
      return nullptr;

    // Read the mangled name.
    auto mangledContextName = anonymousBuffer->getMangledContextName();
    auto mangledNameAddress = resolveRelativeField(contextRef,
                                                   mangledContextName->name);
    return readMangledName(RemoteAddress(mangledNameAddress),
                           MangledNameKind::Symbol,
                           dem);
  }

  /// If we have a context whose parent context is an anonymous context
  /// that provides the local/private name for the current context,
  /// produce a mangled node describing the name of \c context.
  Demangle::NodePointer adoptAnonymousContextName(
      ContextDescriptorRef contextRef,
      llvm::Optional<ParentContextDescriptorRef> &parentContextRef,
      Demangler &dem, Demangle::NodePointer &outerNode) {
    outerNode = nullptr;

    // Bail if there is no parent, or if the parent is in another image.
    // (Anonymous contexts should always be emitted in the same image as their
    // children.)
    if (!parentContextRef
        || !*parentContextRef
        || !parentContextRef->isResolved())
      return nullptr;
    
    auto parentContextLocalRef = parentContextRef->getResolved();

    auto context = contextRef.getLocalBuffer();
    auto typeContext = dyn_cast<TargetTypeContextDescriptor<Runtime>>(context);
    auto protoContext = dyn_cast<TargetProtocolDescriptor<Runtime>>(context);
    if (!typeContext && !protoContext)
      return nullptr;

    auto anonymousParent = dyn_cast_or_null<TargetAnonymousContextDescriptor<Runtime>>(
            parentContextLocalRef.getLocalBuffer());
    if (!anonymousParent)
      return nullptr;

    auto mangledNode = demangleAnonymousContextName(parentContextLocalRef, dem);
    if (!mangledNode)
      return nullptr;

    if (mangledNode->getKind() == Demangle::Node::Kind::Global)
      mangledNode = mangledNode->getFirstChild();

    if (mangledNode->getNumChildren() < 2)
      return nullptr;

    // Dig out the name of the entity.
    swift::Demangle::NodePointer nameChild = mangledNode->getChild(1);
    if ((nameChild->getKind() != Node::Kind::PrivateDeclName &&
         nameChild->getKind() != Node::Kind::LocalDeclName) ||
        nameChild->getNumChildren() < 2)
      return nullptr;

    // Make sure we have an identifier where we expect it.
    auto identifierNode = nameChild->getChild(1);
    if (identifierNode->getKind() != Node::Kind::Identifier ||
        !identifierNode->hasText())
      return nullptr;

    // Read the name of the current context.
    llvm::Optional<TypeImportInfo<std::string>> importInfo;
    auto contextName = readContextDescriptorName(contextRef, importInfo);
    if (!contextName)
      return nullptr;

    // Make sure the name of the current context matches the one in the mangled
    // name of the parent anonymous context.
    // FIXME: Use the ABI name here.
    if (*contextName != identifierNode->getText())
      return nullptr;

    // We have a match. Update the parent context to skip the anonymous
    // context entirely.
    parentContextRef = readParentContextDescriptor(parentContextLocalRef);

    // The outer node is the first child.
    outerNode = mangledNode->getChild(0);

    // Return the name.
    return nameChild;
  }

  /// Resolve a relative target protocol descriptor pointer, which uses
  /// the lowest bit to indicate an indirect vs. direct relative reference and
  /// the second lowest bit to indicate whether it is an Objective-C protocol.
  template<typename Base>
  StoredPointer resolveRelativeIndirectProtocol(
      RemoteRef<Base> descriptor,
      const RelativeTargetProtocolDescriptorPointer<Runtime> &protocol) {
    // Map the offset from within our local buffer to the remote address.
    auto distance = (intptr_t)&protocol - (intptr_t)descriptor.getLocalBuffer();
    StoredPointer targetAddress(getAddress(descriptor) + distance);

    // Read the relative offset.
    int32_t relative;
    if (!Reader->readInteger(RemoteAddress(targetAddress), &relative))
      return StoredPointer();

    // Collect and mask off the 'indirect' and 'isObjC' bits.
    bool indirect = relative & 1;
    relative &= ~1u;
    bool isObjC = relative & 2;
    relative &= ~2u;

    using SignedPointer = typename std::make_signed<StoredPointer>::type;
    auto signext = (SignedPointer)(int32_t)relative;

    StoredPointer resultAddress = targetAddress + signext;

    // Low bit set in the offset indicates that the offset leads to the absolute
    // address in memory.
    if (indirect) {
      if (!Reader->readBytes(RemoteAddress(resultAddress),
                             (uint8_t *)&resultAddress,
                             sizeof(StoredPointer)))
        return StoredPointer();
    }

    // Add back the Objective-C bit.
    if (isObjC)
      resultAddress |= 0x1;

    return resultAddress;
  }

  Demangle::NodePointer
  buildContextDescriptorMangling(const ParentContextDescriptorRef &descriptor,
                                 Demangler &dem, int recursion_limit) {
    if (recursion_limit <= 0) {
      return nullptr;
    }

    if (descriptor.isResolved()) {
      return buildContextDescriptorMangling(descriptor.getResolved(), dem, recursion_limit);
    }
    
    // Try to demangle the symbol name to figure out what context it would
    // point to.
    auto demangledSymbol = buildContextManglingForSymbol(descriptor.getSymbol(),
                                                         dem);
    if (!demangledSymbol)
      return nullptr;
    // Look through Type notes since we're building up a mangling here.
    if (demangledSymbol->getKind() == Demangle::Node::Kind::Type){
      demangledSymbol = demangledSymbol->getChild(0);
    }
    return demangledSymbol;
  }

  Demangle::NodePointer
  buildContextDescriptorMangling(ContextDescriptorRef descriptor,
                                 Demangler &dem, int recursion_limit) {
    if (recursion_limit <= 0) {
      return nullptr;
    }

    // Read the parent descriptor.
    auto parentDescriptorResult = readParentContextDescriptor(descriptor);

    // If the parent is an anonymous context that provides a complete
    // name for this node, note that.
    Demangle::NodePointer demangledParentNode = nullptr;
    auto nameNode = adoptAnonymousContextName(
        descriptor, parentDescriptorResult, dem, demangledParentNode);

    // If there was a problem reading the parent descriptor, we're done.
    if (!parentDescriptorResult) return nullptr;

    // Try to produce a mangle-tree for the parent.
    Demangle::NodePointer parentDemangling = nullptr;
    if (auto parentDescriptor = *parentDescriptorResult) {
      parentDemangling =
        buildContextDescriptorMangling(parentDescriptor, dem, recursion_limit - 1);
      if (!parentDemangling && !demangledParentNode)
        return nullptr;
    }

    // If we have a better parent node produced from an enclosing nominal
    // context, use that.
    if (demangledParentNode &&
        (!parentDemangling ||
         parentDemangling->getKind() == Node::Kind::AnonymousContext)) {
      parentDemangling = demangledParentNode;
    }

    Demangle::Node::Kind nodeKind;
    llvm::Optional<TypeImportInfo<std::string>> importInfo;

    auto getContextName = [&]() -> bool {
      if (nameNode)
        return true;

      if (auto name = readContextDescriptorName(descriptor, importInfo)) {
        nameNode = dem.createNode(Node::Kind::Identifier, std::move(*name));
        return true;
      }

      return false;
    };

    switch (auto contextKind = descriptor->getKind()) {
    case ContextDescriptorKind::Class:
      if (!getContextName())
        return nullptr;
      nodeKind = Demangle::Node::Kind::Class;
      break;
    case ContextDescriptorKind::Struct:
      if (!getContextName())
        return nullptr;
      nodeKind = Demangle::Node::Kind::Structure;
      break;
    case ContextDescriptorKind::Enum:
      if (!getContextName())
        return nullptr;
      nodeKind = Demangle::Node::Kind::Enum;
      break;
    case ContextDescriptorKind::Protocol: {
      if (!getContextName())
        return nullptr;

      nodeKind = Demangle::Node::Kind::Protocol;
      break;
    }
    case ContextDescriptorKind::Extension: {
      // There should always be a parent describing where the extension
      // lives.
      if (!parentDemangling) {
        return nullptr;
      }

      auto extensionBuffer =
        reinterpret_cast<const TargetExtensionContextDescriptor<Runtime> *>(
          descriptor.getLocalBuffer());

      auto extendedContextAddress =
        resolveRelativeField(descriptor, extensionBuffer->ExtendedContext);

      auto demangledExtendedContext =
        readMangledName(RemoteAddress(extendedContextAddress),
                        MangledNameKind::Type,
                        dem);
      if (!demangledExtendedContext)
        return nullptr;

      auto demangling = dem.createNode(Node::Kind::Extension);
      demangling->addChild(parentDemangling, dem);
      demangling->addChild(demangledExtendedContext, dem);

      /// Resolver to turn a protocol reference into a demangling.
      struct ProtocolResolver {
        using Result = Demangle::Node *;

        Demangler &dem;

        Result failure() const {
          return nullptr;
        }

        Result swiftProtocol(Demangle::Node *node) {
          return node;
        }

#if SWIFT_OBJC_INTEROP
        Result objcProtocol(StringRef name) {
          // FIXME: Unify this with the runtime's Demangle.cpp
          auto module = dem.createNode(Node::Kind::Module,
                                       MANGLING_MODULE_OBJC);
          auto node = dem.createNode(Node::Kind::Protocol);
          node->addChild(module, dem);
          node->addChild(dem.createNode(Node::Kind::Identifier, name),
                         dem);
          return node;
        }
#endif
      } protocolResolver{dem};

      // If there are generic requirements, form the generic signature.
      auto requirements = extensionBuffer->getGenericRequirements();
      if (!requirements.empty()) {
        auto signatureNode =
          dem.createNode(Node::Kind::DependentGenericSignature);
        bool failed = false;
        for (const auto &req : requirements) {
          if (failed)
            break;

          // Demangle the subject.
          auto subjectAddress = resolveRelativeField(descriptor, req.Param);
          swift::Demangle::NodePointer subject =
              readMangledName(RemoteAddress(subjectAddress),
                              MangledNameKind::Type, dem);
          if (!subject) {
            failed = true;
            break;
          }

          switch (req.Flags.getKind()) {
          case GenericRequirementKind::Protocol: {
            auto protocolAddress =
              resolveRelativeIndirectProtocol(descriptor, req.Protocol);
            auto protocol = readProtocol(protocolAddress, dem,
                                         protocolResolver);
            if (!protocol) {
              failed = true;
              break;
            }

            auto reqNode =
              dem.createNode(
                Node::Kind::DependentGenericConformanceRequirement);
            reqNode->addChild(subject, dem);
            reqNode->addChild(protocol, dem);
            signatureNode->addChild(reqNode, dem);
            break;
          }

          case GenericRequirementKind::SameType:
          case GenericRequirementKind::BaseClass: {
            // Demangle the right-hand type.
            auto typeAddress = resolveRelativeField(descriptor, req.Type);
            swift::Demangle::NodePointer type =
                readMangledName(RemoteAddress(typeAddress),
                                MangledNameKind::Type, dem);
            if (!type) {
              failed = true;
              break;
            }

            Node::Kind nodeKind;
            if (req.Flags.getKind() == GenericRequirementKind::SameType)
              nodeKind = Node::Kind::DependentGenericSameTypeRequirement;
            else
              nodeKind = Node::Kind::DependentGenericConformanceRequirement;

            auto reqNode = dem.createNode(nodeKind);
            reqNode->addChild(subject, dem);
            reqNode->addChild(type, dem);
            signatureNode->addChild(reqNode, dem);
            break;
          }

          case GenericRequirementKind::SameConformance:
            // Do nothing
            break;

          case GenericRequirementKind::Layout: {
            // Map the offset from within our local buffer to the remote
            // address.
            auto distance =
              (intptr_t)&req.Layout - (intptr_t)descriptor.getLocalBuffer();
            StoredPointer targetAddress(getAddress(descriptor) + distance);

            GenericRequirementLayoutKind kind;
            if (!Reader->readBytes(RemoteAddress(targetAddress),
                                   (uint8_t *)&kind, sizeof(kind))) {
              failed = true;
              break;
            }

            if (kind == GenericRequirementLayoutKind::Class) {
              auto reqNode =
                dem.createNode(Node::Kind::DependentGenericLayoutRequirement);
              reqNode->addChild(subject, dem);
              auto idNode = dem.createNode(Node::Kind::Identifier, "C");
              reqNode->addChild(idNode, dem);
              signatureNode->addChild(reqNode, dem);
            } else {
              failed = true;
            }
            break;
          }

          case GenericRequirementKind::SameShape:
            llvm_unreachable("Implement me");
          }
        }

        if (!failed) {
          demangling->addChild(signatureNode, dem);
        }
      }
      return demangling;
    }

    case ContextDescriptorKind::Anonymous: {
      // Use the remote address to identify the anonymous context.
      char addressBuf[18];
      snprintf(addressBuf, sizeof(addressBuf), "$%" PRIx64,
               (uint64_t)descriptor.getAddressData());
      auto anonNode = dem.createNode(Node::Kind::AnonymousContext);
      CharVector addressStr;
      addressStr.append(addressBuf, dem);
      auto name = dem.createNode(Node::Kind::Identifier, addressStr);
      anonNode->addChild(name, dem);
      if (parentDemangling)
        anonNode->addChild(parentDemangling, dem);
      
      return anonNode;
    }

    case ContextDescriptorKind::Module: {
      // Modules shouldn't have a parent.
      if (parentDemangling) {
        return nullptr;
      }

      nodeKind = Demangle::Node::Kind::Module;
      auto moduleBuffer =
        reinterpret_cast<const TargetModuleContextDescriptor<Runtime> *>
          (descriptor.getLocalBuffer());
      auto nameAddress
        = resolveRelativeField(descriptor, moduleBuffer->Name);
      std::string moduleName;
      if (!Reader->readString(RemoteAddress(nameAddress), moduleName))
        return nullptr;

      // The form of module contexts is a little different from other
      // contexts; just create the node directly here and return.
      return dem.createNode(nodeKind, std::move(moduleName));
    }
        
    case ContextDescriptorKind::OpaqueType: {
      // The opaque type may have a named anonymous context for us to map
      // back to its defining decl.
      if (!parentDescriptorResult
          || !*parentDescriptorResult
          || !parentDescriptorResult->isResolved())
        return nullptr;

      if (parentDemangling->getKind() == Node::Kind::AnonymousContext) {
        auto mangledNode =
        demangleAnonymousContextName(parentDescriptorResult->getResolved(), dem);
        if (!mangledNode)
          return nullptr;
        if (mangledNode->getKind() == Node::Kind::Global)
          mangledNode = mangledNode->getChild(0);

        auto opaqueNode = dem.createNode(Node::Kind::OpaqueReturnTypeOf);
        opaqueNode->addChild(mangledNode, dem);
        return opaqueNode;
      } else if (parentDemangling->getKind() == Node::Kind::Module) {
        auto opaqueNode = dem.createNode(Node::Kind::OpaqueReturnTypeOf);
        opaqueNode->addChild(parentDemangling, dem);
        return opaqueNode;
      } else {
        return nullptr;
      }
    }
    
    default:
      // Not a kind of context we know about.
      return nullptr;
    }

    // The root context should be a module context, which we handled directly
    // in the switch, so if we got here without a parent, we're ill-formed.
    if (!parentDemangling) {
      return nullptr;
    }

    // Override various aspects of the mangling for imported types.
    if (importInfo && isCImportedContext(parentDemangling)) {
      if (importInfo->SymbolNamespace == TypeImportSymbolNamespace::CTypedef)
        nodeKind = Demangle::Node::Kind::TypeAlias;

      // As a special case, use the struct mangling for C-imported value
      // types that don't have a special namespace and aren't a related
      // entity.
      //
      // This should be kept in sync with the AST mangler and with
      // _isCImportedTagType in the runtime.
      else if (importInfo->SymbolNamespace.empty() &&
               importInfo->RelatedEntityName.empty() &&
               nodeKind == Demangle::Node::Kind::Enum)
        nodeKind = Demangle::Node::Kind::Structure;
    }

    // Use private declaration names for anonymous context references.
    if (parentDemangling->getKind() == Node::Kind::AnonymousContext
        && nameNode->getKind() == Node::Kind::Identifier) {
      if (parentDemangling->getNumChildren() < 2)
        return nullptr;

      auto privateDeclName =
        dem.createNode(Node::Kind::PrivateDeclName);
      privateDeclName->addChild(parentDemangling->getChild(0), dem);
      privateDeclName->addChild(nameNode, dem);

      nameNode = privateDeclName;
      parentDemangling = parentDemangling->getChild(1);
    }

    if (importInfo && !importInfo->RelatedEntityName.empty()) {
      auto kindNode = dem.createNode(Node::Kind::Identifier,
                                 std::move(importInfo->RelatedEntityName));
      auto relatedNode = dem.createNode(Node::Kind::RelatedEntityDeclName);
      relatedNode->addChild(kindNode, dem);
      relatedNode->addChild(nameNode, dem);
      nameNode = relatedNode;
    }

    auto demangling = dem.createNode(nodeKind);
    demangling->addChild(parentDemangling, dem);
    demangling->addChild(nameNode, dem);
    return demangling;
  }

  /// Given a read nominal type descriptor, attempt to build a
  /// nominal type decl from it.
  template <
      typename T = BuilderType,
      typename std::enable_if_t<
          !(std::is_same<
                const bool,
                decltype(T::needsToPrecomputeParentGenericContextShapes)>::
                value &&
            T::needsToPrecomputeParentGenericContextShapes),
          bool> = true>
  BuiltTypeDecl buildNominalTypeDecl(ContextDescriptorRef descriptor) {
    // Build the demangling tree from the context tree.
    Demangler dem;
    auto node = buildContextMangling(descriptor, dem);
    if (!node || node->getKind() != Node::Kind::Type)
      return BuiltTypeDecl();
    bool typeAlias = false;
    BuiltTypeDecl decl = Builder.createTypeDecl(node, typeAlias);
    return decl;
  }

  template <typename T = BuilderType,
            typename std::enable_if_t<
                std::is_same<
                    const bool,
                    decltype(T::needsToPrecomputeParentGenericContextShapes)>::
                        value &&
                    T::needsToPrecomputeParentGenericContextShapes,
                bool> = true>
  BuiltTypeDecl buildNominalTypeDecl(ContextDescriptorRef descriptor) {
    // Build the demangling tree from the context tree.
    Demangler dem;
    auto node = buildContextMangling(descriptor, dem);
    if (!node || node->getKind() != Node::Kind::Type)
      return BuiltTypeDecl();
    std::vector<size_t> paramsPerLevel;
    size_t runningCount = 0;
    std::function<void(ContextDescriptorRef current, size_t &)> countLevels =
        [&](ContextDescriptorRef current, size_t &runningCount) {
          if (auto parentContextRef = readParentContextDescriptor(current))
            if (parentContextRef->isResolved())
              if (auto parentContext = parentContextRef->getResolved())
                countLevels(parentContext, runningCount);

          auto genericContext = current->getGenericContext();
          // Only consider generic contexts of type class, enum or struct.
          // There are other context types that can be generic, but they should
          // not affect the generic shape.
          if (genericContext &&
              (current->getKind() == ContextDescriptorKind::Class ||
               current->getKind() == ContextDescriptorKind::Enum ||
               current->getKind() == ContextDescriptorKind::Struct)) {
            auto contextHeader = genericContext->getGenericContextHeader();
            paramsPerLevel.emplace_back(contextHeader.NumParams - runningCount);
            runningCount += paramsPerLevel.back();
          }
        };
    countLevels(descriptor, runningCount);
    BuiltTypeDecl decl = Builder.createTypeDecl(node, paramsPerLevel);
    return decl;
  }

#if SWIFT_OBJC_INTEROP
  std::string readObjCProtocolName(StoredPointer Address) {
    auto Size = sizeof(TargetObjCProtocolPrefix<Runtime>);
    auto Buffer = (uint8_t *)malloc(Size);
    if (!Buffer)
      return std::string();
    SWIFT_DEFER {
      free(Buffer);
    };

    if (!Reader->readBytes(RemoteAddress(Address), Buffer, Size))
      return std::string();

    auto ProtocolDescriptor
      = reinterpret_cast<TargetObjCProtocolPrefix<Runtime> *>(Buffer);
    std::string Name;
    if (!Reader->readString(RemoteAddress(ProtocolDescriptor->Name), Name))
      return std::string();

    return Name;
  }
#endif

  // TODO: We need to be able to produce protocol conformances for each
  // substitution type as well in order to accurately rebuild bound generic
  // types or types in protocol-constrained inner contexts.
  std::vector<BuiltType> getGenericSubst(MetadataRef metadata,
                                         ContextDescriptorRef descriptor,
                                         int recursion_limit) {
    auto generics = descriptor->getGenericContext();
    if (!generics)
      return {};
    
    auto numGenericArgs =
      generics->getGenericContextHeader().getNumArguments();
    
    auto offsetToGenericArgs = readGenericArgsOffset(metadata, descriptor);
    if (!offsetToGenericArgs)
      return {};

    auto genericArgsAddr = getAddress(metadata)
      + sizeof(StoredPointer) * *offsetToGenericArgs;

    std::vector<BuiltType> builtSubsts;
    for (auto param : generics->getGenericParams()) {
      switch (param.getKind()) {
      case GenericParamKind::Type:
        // The type should have a key argument unless it's been same-typed
        // to another type.
        if (param.hasKeyArgument()) {
          if (numGenericArgs == 0)
            return {};
          --numGenericArgs;
          
          StoredPointer arg;
          if (!Reader->readBytes(RemoteAddress(genericArgsAddr),
                                 (uint8_t*)&arg, sizeof(arg))) {
            return {};
          }
          genericArgsAddr += sizeof(StoredPointer);

          auto builtArg = readTypeFromMetadata(arg, false, recursion_limit);
          if (!builtArg)
            return {};
          builtSubsts.push_back(builtArg);
        } else {
          // TODO: If the key argument has been concretized by a same-type
          // constraint, that should be reflected in the built nominal type
          // decl's generic constraints. This isn't handled correctly yet.
          return {};
        }
        break;
        
      case GenericParamKind::TypePack:
        assert(false && "Packs not supported here yet");

      default:
        // We don't know about this kind of parameter.
        return {};
      }
    }
    return builtSubsts;
  }

  BuiltType
  readNominalTypeFromMetadata(MetadataRef origMetadata,
                              int recursion_limit = defaultTypeRecursionLimit,
                              bool skipArtificialSubclasses = false) {
    auto metadata = origMetadata;
    auto descriptorAddress =
      readAddressOfNominalTypeDescriptor(metadata,
                                         skipArtificialSubclasses);
    if (!descriptorAddress)
      return BuiltType();

    // If we've skipped an artificial subclasses, check the cache at
    // the superclass.  (This also protects against recursion.)
    if (skipArtificialSubclasses && metadata != origMetadata) {
      auto it = TypeCache.find(getAddress(metadata));
      if (it != TypeCache.end())
        return it->second;
    }

    // Read the nominal type descriptor.
    ContextDescriptorRef descriptor = readContextDescriptor(descriptorAddress);
    if (!descriptor)
      return BuiltType();

    // From that, attempt to resolve a nominal type.
    BuiltTypeDecl typeDecl = buildNominalTypeDecl(descriptor);
    if (!typeDecl)
      return BuiltType();

    // Build the nominal type.
    BuiltType nominal;
    if (descriptor->isGeneric()) {
      // Resolve the generic arguments.
      auto builtGenerics =
          getGenericSubst(metadata, descriptor, recursion_limit);
      if (builtGenerics.empty())
        return BuiltType();
      nominal = Builder.createBoundGenericType(typeDecl, builtGenerics);
    } else {
      nominal = Builder.createNominalType(typeDecl);
    }

    if (!nominal)
      return BuiltType();
    
    TypeCache[getAddress(metadata)] = nominal;

    // If we've skipped an artificial subclass, remove the
    // recursion-protection entry we made for it.
    if (skipArtificialSubclasses && metadata != origMetadata) {
      TypeCache.erase(getAddress(origMetadata));
    }

    return nominal;
  }

  BuiltType
  readNominalTypeFromClassMetadata(MetadataRef origMetadata,
                                   int recursion_limit,
                                   bool skipArtificialSubclasses = false) {
    auto classMeta = cast<TargetClassMetadata>(origMetadata);
    if (classMeta->isTypeMetadata())
      return readNominalTypeFromMetadata(origMetadata, recursion_limit,
                                         skipArtificialSubclasses);

    std::string className;
    auto origMetadataPtr = getAddress(origMetadata);
    if (!readObjCClassName(origMetadataPtr, className))
      return BuiltType();

    BuiltType BuiltObjCClass = Builder.createObjCClassType(std::move(className));
    if (!BuiltObjCClass) {
      // Try the superclass.
      if (!stripSignedPointer(classMeta->Superclass))
        return BuiltType();

      BuiltObjCClass =
          readTypeFromMetadata(stripSignedPointer(classMeta->Superclass),
                               skipArtificialSubclasses, recursion_limit);
    }

    TypeCache[origMetadataPtr] = BuiltObjCClass;
    return BuiltObjCClass;
  }

  using TargetClassMetadataObjCInterop =
      swift::TargetClassMetadata<Runtime,
                                 TargetAnyClassMetadataObjCInterop<Runtime>>;

  /// Given that the remote process is running the non-fragile Apple runtime,
  /// grab the ro-data from a class pointer.
  StoredPointer readObjCRODataPtr(StoredPointer classAddress) {
    // WARNING: the following algorithm works on current modern Apple
    // runtimes but is not actually ABI.  But it is pretty reliable.

#if SWIFT_OBJC_INTEROP
    StoredPointer dataPtr;
    if (!Reader->readInteger(
            RemoteAddress(classAddress +
                          TargetClassMetadataObjCInterop::offsetToData()),
            &dataPtr))
      return StoredPointer();

    // Apply the data-pointer mask.
    // These values have been stolen from the runtime source.
    static constexpr uint64_t DataPtrMask =
      (Runtime::PointerSize == 8 ? 0x00007ffffffffff8ULL : 0xfffffffcULL);
    dataPtr &= StoredPointer(DataPtrMask);
    if (!dataPtr)
      return StoredPointer();

    // Read the flags, which is a 32-bit header on both formats.
    uint32_t flags;
    if (!Reader->readInteger(RemoteAddress(dataPtr), &flags))
      return StoredPointer();

    // If the type is not realized, this is the RO-data.
    static constexpr uint32_t RO_REALIZED = 0x80000000U;
    if (!(flags & RO_REALIZED))
      return dataPtr;

    // Otherwise, it's the RW-data; read the RO-data pointer from a
    // well-known position within the RW-data.
    StoredSignedPointer signedDataPtr;
    static constexpr uint32_t OffsetToROPtr = 8;
    if (!Reader->readInteger(RemoteAddress(dataPtr + OffsetToROPtr), &signedDataPtr))
      return StoredPointer();
    dataPtr = stripSignedPointer(signedDataPtr);

    // Newer Objective-C runtimes implement a size optimization where the RO
    // field is a tagged union. If the low-bit is set, then the pointer needs
    // to be dereferenced once more to yield the real class_ro_t pointer.
    if (dataPtr & 1) {
      if (!Reader->readInteger(RemoteAddress(dataPtr^1), &signedDataPtr))
        return StoredPointer();
      dataPtr = stripSignedPointer(signedDataPtr);
    }

    return dataPtr;
#else
    return StoredPointer();
#endif
  }

  IsaEncodingKind getIsaEncoding() {
    if (IsaEncoding != IsaEncodingKind::Unknown)
      return IsaEncoding;

    auto finish = [&](IsaEncodingKind result) -> IsaEncodingKind {
      IsaEncoding = result;
      return result;
    };

    /// Look up the given global symbol and bind 'varname' to its
    /// address if its exists.
#   define tryFindSymbol(varname, symbolName)                \
      auto varname = Reader->getSymbolAddress(symbolName);   \
      if (!varname)                                          \
        return finish(IsaEncodingKind::Error)
    /// Read from the given pointer into 'dest'.
#   define tryReadSymbol(varname, dest) do {                 \
      if (!Reader->readInteger(varname, &dest))              \
        return finish(IsaEncodingKind::Error);               \
    } while (0)
    /// Read from the given global symbol into 'dest'.
#   define tryFindAndReadSymbol(dest, symbolName) do {       \
      tryFindSymbol(_address, symbolName);                    \
      tryReadSymbol(_address, dest);                          \
    } while (0)

    // Check for the magic-mask symbol that indicates that the ObjC
    // runtime is using indexed ISAs.
    if (auto magicMaskAddress =
          Reader->getSymbolAddress("objc_debug_indexed_isa_magic_mask")) {
      tryReadSymbol(magicMaskAddress, IsaMagicMask);
      if (IsaMagicMask != 0) {
        tryFindAndReadSymbol(IsaMagicValue,
                             "objc_debug_indexed_isa_magic_value");
        tryFindAndReadSymbol(IsaIndexMask,
                             "objc_debug_indexed_isa_index_mask");
        tryFindAndReadSymbol(IsaIndexShift,
                             "objc_debug_indexed_isa_index_shift");
        tryFindSymbol(indexedClasses, "objc_indexed_classes");
        IndexedClassesPointer = indexedClasses.getAddressData();
        tryFindSymbol(indexedClassesCount, "objc_indexed_classes_count");
        IndexedClassesCountPointer = indexedClassesCount.getAddressData();

        return finish(IsaEncodingKind::Indexed);
      }
    }

    // Check for the ISA mask symbol.  This has to come second because
    // the standard library will define this even if the ObjC runtime
    // doesn't use it.
    if (auto maskAddress = Reader->getSymbolAddress("swift_isaMask")) {
      tryReadSymbol(maskAddress, IsaMask);
      if (IsaMask != 0) {
        return finish(IsaEncodingKind::Masked);
      }
    }

#   undef tryFindSymbol
#   undef tryReadSymbol
#   undef tryFindAndReadSymbol

    return finish(IsaEncodingKind::None);
  }

  TaggedPointerEncodingKind getTaggedPointerEncoding() {
    if (TaggedPointerEncoding != TaggedPointerEncodingKind::Unknown)
      return TaggedPointerEncoding;

    auto finish = [&](TaggedPointerEncodingKind result)
        -> TaggedPointerEncodingKind {
      TaggedPointerEncoding = result;
      return result;
    };

    /// Look up the given global symbol and bind 'varname' to its
    /// address if its exists.
#   define tryFindSymbol(varname, symbolName)                \
      auto varname = Reader->getSymbolAddress(symbolName);   \
      if (!varname)                                          \
        return finish(TaggedPointerEncodingKind::Error)
    /// Read from the given pointer into 'dest'.
#   define tryReadSymbol(varname, dest) do {                 \
      if (!Reader->readInteger(varname, &dest))              \
        return finish(TaggedPointerEncodingKind::Error);     \
    } while (0)
    /// Read from the given global symbol into 'dest'.
#   define tryFindAndReadSymbol(dest, symbolName) do {       \
      tryFindSymbol(_address, symbolName);                   \
      tryReadSymbol(_address, dest);                         \
    } while (0)
#   define tryFindAndReadSymbolWithDefault(dest, symbolName, default) do { \
      dest = default;                                                      \
      auto _address = Reader->getSymbolAddress(symbolName);                \
      if (_address)                                                        \
        tryReadSymbol(_address, dest);                                     \
    } while (0)

    tryFindAndReadSymbol(TaggedPointerMask,
                         "objc_debug_taggedpointer_mask");
    tryFindAndReadSymbol(TaggedPointerSlotShift,
                         "objc_debug_taggedpointer_slot_shift");
    tryFindAndReadSymbol(TaggedPointerSlotMask,
                         "objc_debug_taggedpointer_slot_mask");
    tryFindSymbol(TaggedPointerClassesAddr,
                  "objc_debug_taggedpointer_classes");
    if (!TaggedPointerClassesAddr)
      finish(TaggedPointerEncodingKind::Error);
    TaggedPointerClasses = TaggedPointerClassesAddr.getAddressData();
    
    // Extended tagged pointers don't exist on older OSes. Handle those
    // by setting the variables to zero.
    tryFindAndReadSymbolWithDefault(TaggedPointerExtendedMask,
                                    "objc_debug_taggedpointer_ext_mask",
                                    0);
    tryFindAndReadSymbolWithDefault(TaggedPointerExtendedSlotShift,
                                    "objc_debug_taggedpointer_ext_slot_shift",
                                    0);
    tryFindAndReadSymbolWithDefault(TaggedPointerExtendedSlotMask,
                                    "objc_debug_taggedpointer_ext_slot_mask",
                                    0);
    auto TaggedPointerExtendedClassesAddr =
      Reader->getSymbolAddress("objc_debug_taggedpointer_ext_classes");
    if (TaggedPointerExtendedClassesAddr)
      TaggedPointerExtendedClasses =
          TaggedPointerExtendedClassesAddr.getAddressData();

    // The tagged pointer obfuscator is not present on older OSes, in
    // which case we can treat it as zero.
    tryFindAndReadSymbolWithDefault(TaggedPointerObfuscator,
                                    "objc_debug_taggedpointer_obfuscator",
                                    0);
    
#   undef tryFindSymbol
#   undef tryReadSymbol
#   undef tryFindAndReadSymbol
#   undef tryFindAndReadSymbolWithDefault

    return finish(TaggedPointerEncodingKind::Extended);
  }

  template <class T>
  static constexpr T roundUpToAlignment(T offset, T alignment) {
    return (offset + alignment - 1) & ~(alignment - 1);
  }
};

} // end namespace remote
} // end namespace swift

namespace llvm {
  template<typename T>
  struct simplify_type<swift::remote::RemoteRef<T>> {
    using SimpleType = const T *;
    static SimpleType
    getSimplifiedValue(swift::remote::RemoteRef<T> value) {
      return value.getLocalBuffer();
    }
  };
}

#endif // SWIFT_REFLECTION_READER_H
