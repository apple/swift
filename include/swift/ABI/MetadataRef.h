//===--- MetadataRef.h - ABI for references to metadata ---------*- C++ -*-===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2022 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//
//
// This file describes runtime metadata structures for references to
// other metadata.
//
//===----------------------------------------------------------------------===//

#ifndef SWIFT_ABI_METADATAREF_H
#define SWIFT_ABI_METADATAREF_H

#include "swift/ABI/TargetLayout.h"
#include "swift/ABI/MetadataValues.h"

#if SWIFT_OBJC_INTEROP
#include <objc/runtime.h>
#endif

namespace swift {

template <typename Runtime>
struct TargetAnyClassMetadata;
template <typename Runtime>
struct TargetAnyClassMetadataObjCInterop;
template <typename Runtime, typename TargetAnyClassMetadataVariant>
struct TargetClassMetadata;
template <typename Runtime>
struct swift_ptrauth_struct_context_descriptor(ContextDescriptor)
    TargetContextDescriptor;
template <typename Runtime>
struct swift_ptrauth_struct_context_descriptor(ProtocolDescriptor)
    TargetProtocolDescriptor;

namespace detail {
template <typename Runtime, bool ObjCInterop = Runtime::ObjCInterop>
struct TargetAnyClassMetadataTypeImpl;

template <typename Runtime>
struct TargetAnyClassMetadataTypeImpl<Runtime, /*ObjCInterop*/ true> {
  using type = TargetAnyClassMetadataObjCInterop<Runtime>;
};

template <typename Runtime>
struct TargetAnyClassMetadataTypeImpl<Runtime, /*ObjCInterop*/ false> {
  using type = TargetAnyClassMetadata<Runtime>;
};
}

/// A convenience typedef for the correct target-parameterized
/// AnyClassMetadata class.
template <typename Runtime>
using TargetAnyClassMetadataType =
  typename detail::TargetAnyClassMetadataTypeImpl<Runtime>::type;

/// A convenience typedef for the correct target-parameterized
/// ClassMetadata class.
template <typename Runtime>
using TargetClassMetadataType =
  TargetClassMetadata<Runtime, TargetAnyClassMetadataType<Runtime>>;

/// A convenience typedef for a ClassMetadata class that's forced to
/// support ObjC interop even if that's not the default for the target.
template <typename Runtime>
using TargetClassMetadataObjCInterop =
  TargetClassMetadata<Runtime, TargetAnyClassMetadataObjCInterop<Runtime>>;

/// A convenience typedef for a target-parameterized pointer to a
/// target-parameterized type.
template <typename Runtime, template <typename> class Pointee>
using TargetMetadataPointer
  = typename Runtime::template Pointer<Pointee<Runtime>>;
  
/// A convenience typedef for a target-parameterized const pointer
/// to a target-parameterized type.
template <typename Runtime, template <typename> class Pointee>
using ConstTargetMetadataPointer
  = typename Runtime::template Pointer<const Pointee<Runtime>>;

/// A signed pointer to a type context descriptor, using the standard
/// signing schema.
template <typename Runtime,
          template<typename> class Context = TargetContextDescriptor>
using TargetSignedContextPointer
  = TargetSignedPointer<Runtime,
                        Context<Runtime> * __ptrauth_swift_type_descriptor>;

/// A relative pointer to a type context descriptor.
template <typename Runtime,
          template<typename> class Context = TargetContextDescriptor>
using TargetRelativeContextPointer =
  RelativeIndirectablePointer<const Context<Runtime>,
                              /*nullable*/ true, int32_t,
                              TargetSignedContextPointer<Runtime, Context>>;
using RelativeContextPointer = TargetRelativeContextPointer<InProcess>;

/// A relative-indirectable pointer to a type context descriptor, with
/// the low bits used to store a small additional discriminator.
template <typename Runtime, typename IntTy,
          template<typename _Runtime> class Context = TargetContextDescriptor>
using RelativeContextPointerIntPair =
  RelativeIndirectablePointerIntPair<const Context<Runtime>, IntTy,
                              /*nullable*/ true, int32_t,
                              TargetSignedContextPointer<Runtime, Context>>;

/// Layout of a small prefix of an Objective-C protocol, used only to
/// directly extract the name of the protocol.
template <typename Runtime>
struct TargetObjCProtocolPrefix {
  /// Unused by the Swift runtime.
  TargetPointer<Runtime, const void> _ObjC_Isa;

  /// The mangled name of the protocol.
  TargetPointer<Runtime, const char> Name;
};

/// A reference to a protocol within the runtime, which may be either
/// a Swift protocol or (when Objective-C interoperability is enabled) an
/// Objective-C protocol.
///
/// This type always contains a single target pointer, whose lowest bit is
/// used to distinguish between a Swift protocol referent and an Objective-C
/// protocol referent.
template <typename Runtime>
class TargetProtocolDescriptorRef {
  using StoredPointer = typename Runtime::StoredPointer;
  using ProtocolDescriptorPointer =
    ConstTargetMetadataPointer<Runtime, TargetProtocolDescriptor>;

  enum : StoredPointer {
    // The bit used to indicate whether this is an Objective-C protocol.
    IsObjCBit = 0x1U,
  };

  /// A direct pointer to a protocol descriptor for either an Objective-C
  /// protocol (if the low bit is set) or a Swift protocol (if the low bit
  /// is clear).
  StoredPointer storage;

public:
  constexpr TargetProtocolDescriptorRef(StoredPointer storage)
    : storage(storage) { }

  constexpr TargetProtocolDescriptorRef() : storage() { }

  TargetProtocolDescriptorRef(
                        ProtocolDescriptorPointer protocol,
                        ProtocolDispatchStrategy dispatchStrategy) {
    if (Runtime::ObjCInterop) {
      storage =
          reinterpret_cast<StoredPointer>(protocol) |
          (dispatchStrategy == ProtocolDispatchStrategy::ObjC ? IsObjCBit : 0);
    } else {
      assert(dispatchStrategy == ProtocolDispatchStrategy::Swift);
      storage = reinterpret_cast<StoredPointer>(protocol);
    }
  }

  const static TargetProtocolDescriptorRef forSwift(
                                          ProtocolDescriptorPointer protocol) {
    return TargetProtocolDescriptorRef{
        reinterpret_cast<StoredPointer>(protocol)};
  }

#if SWIFT_OBJC_INTEROP
  constexpr static TargetProtocolDescriptorRef forObjC(Protocol *objcProtocol) {
    return TargetProtocolDescriptorRef{
        reinterpret_cast<StoredPointer>(objcProtocol) | IsObjCBit};
  }
#endif

  explicit constexpr operator bool() const {
    return storage != 0;
  }

  /// The name of the protocol.
  TargetPointer<Runtime, const char> getName() const {
#if SWIFT_OBJC_INTEROP
    if (isObjC()) {
      return reinterpret_cast<TargetObjCProtocolPrefix<Runtime> *>(
          getObjCProtocol())->Name;
    }
#endif

    return getSwiftProtocol()->Name;
  }

  /// Determine what kind of protocol this is, Swift or Objective-C.
  ProtocolDispatchStrategy getDispatchStrategy() const {
    if (isObjC()) {
      return ProtocolDispatchStrategy::ObjC;
    }

    return ProtocolDispatchStrategy::Swift;
  }

  /// Determine whether this protocol has a 'class' constraint.
  ProtocolClassConstraint getClassConstraint() const {
    if (isObjC()) {
      return ProtocolClassConstraint::Class;
    }

    return getSwiftProtocol()->getProtocolContextDescriptorFlags()
        .getClassConstraint();
  }

  /// Determine whether this protocol needs a witness table.
  bool needsWitnessTable() const {
    if (isObjC()) {
      return false;
    }

    return true;
  }

  SpecialProtocol getSpecialProtocol() const {
    if (isObjC()) {
      return SpecialProtocol::None;
    }

    return getSwiftProtocol()->getProtocolContextDescriptorFlags()
        .getSpecialProtocol();
  }

  /// Retrieve the Swift protocol descriptor.
  ProtocolDescriptorPointer getSwiftProtocol() const {
    assert(!isObjC());

    // NOTE: we explicitly use a C-style cast here because cl objects to the
    // reinterpret_cast from a uintptr_t type to an unsigned type which the
    // Pointer type may be depending on the instantiation.  Using the C-style
    // cast gives us a single path irrespective of the template type parameters.
    return (ProtocolDescriptorPointer)(storage & ~IsObjCBit);
  }

  /// Retrieve the raw stored pointer and discriminator bit.
  constexpr StoredPointer getRawData() const {
    return storage;
  }

  /// Whether this references an Objective-C protocol.
  bool isObjC() const {
    if (Runtime::ObjCInterop)
      return (storage & IsObjCBit) != 0;
    else
      return false;
  }

#if SWIFT_OBJC_INTEROP
  /// Retrieve the Objective-C protocol.
  TargetPointer<Runtime, Protocol> getObjCProtocol() const {
    assert(isObjC());
    return reinterpret_cast<TargetPointer<Runtime, Protocol> >(
                                                         storage & ~IsObjCBit);
  }
#endif
};

using ProtocolDescriptorRef = TargetProtocolDescriptorRef<InProcess>;

/// A relative pointer to a protocol descriptor, which provides the relative-
/// pointer equivalent to \c TargetProtocolDescriptorRef.
template <typename Runtime>
class RelativeTargetProtocolDescriptorPointer {
  union {
    /// Relative pointer to a Swift protocol descriptor.
    /// The \c bool value will be false to indicate that the protocol
    /// is a Swift protocol, or true to indicate that this references
    /// an Objective-C protocol.
    RelativeContextPointerIntPair<Runtime, bool, TargetProtocolDescriptor>
      swiftPointer;
#if SWIFT_OBJC_INTEROP    
    /// Relative pointer to an ObjC protocol descriptor.
    /// The \c bool value will be false to indicate that the protocol
    /// is a Swift protocol, or true to indicate that this references
    /// an Objective-C protocol.
    RelativeIndirectablePointerIntPair<Protocol, bool> objcPointer;
#endif
  };

  bool isObjC() const {
#if SWIFT_OBJC_INTEROP
    if (Runtime::ObjCInterop)
      return objcPointer.getInt();
#endif
    return false;
  }

public:
  /// Retrieve a reference to the protocol.
  TargetProtocolDescriptorRef<Runtime> getProtocol() const {
#if SWIFT_OBJC_INTEROP
    if (isObjC()) {
      return TargetProtocolDescriptorRef<Runtime>::forObjC(
          const_cast<Protocol *>(objcPointer.getPointer()));
    }
#endif

    return TargetProtocolDescriptorRef<Runtime>::forSwift(
        reinterpret_cast<
            ConstTargetMetadataPointer<Runtime, TargetProtocolDescriptor>>(
            swiftPointer.getPointer()));
  }

  /// Retrieve a reference to the protocol.
  int32_t getUnresolvedProtocolAddress() const {
#if SWIFT_OBJC_INTEROP
    if (isObjC()) {
      return objcPointer.getUnresolvedOffset();
    }
#endif
    return swiftPointer.getUnresolvedOffset();
  }

  operator TargetProtocolDescriptorRef<Runtime>() const {
    return getProtocol();
  }
};

/// A reference to a type.
template <typename Runtime>
struct TargetTypeReference {
  union {
    /// A direct reference to a TypeContextDescriptor or ProtocolDescriptor.
    RelativeDirectPointer<TargetContextDescriptor<Runtime>>
      DirectTypeDescriptor;

    /// An indirect reference to a TypeContextDescriptor or ProtocolDescriptor.
    RelativeDirectPointer<
        TargetSignedPointer<Runtime, TargetContextDescriptor<Runtime> * __ptrauth_swift_type_descriptor>>
      IndirectTypeDescriptor;

    /// An indirect reference to an Objective-C class.
    RelativeDirectPointer<
        ConstTargetMetadataPointer<Runtime, TargetClassMetadataType>>
      IndirectObjCClass;

    /// A direct reference to an Objective-C class name.
    RelativeDirectPointer<const char>
      DirectObjCClassName;
  };

  const TargetContextDescriptor<Runtime> *
  getTypeDescriptor(TypeReferenceKind kind) const {
    switch (kind) {
    case TypeReferenceKind::DirectTypeDescriptor:
      return DirectTypeDescriptor;

    case TypeReferenceKind::IndirectTypeDescriptor:
      return *IndirectTypeDescriptor;

    case TypeReferenceKind::DirectObjCClassName:
    case TypeReferenceKind::IndirectObjCClass:
      return nullptr;
    }

    return nullptr;
  }

  /// If this type reference is one of the kinds that supports ObjC
  /// references,
  const TargetClassMetadataObjCInterop<Runtime> *
  getObjCClass(TypeReferenceKind kind) const;

  const TargetClassMetadataObjCInterop<Runtime> * const *
  getIndirectObjCClass(TypeReferenceKind kind) const {
    assert(kind == TypeReferenceKind::IndirectObjCClass);
    return IndirectObjCClass.get();
  }

  const char *getDirectObjCClassName(TypeReferenceKind kind) const {
    assert(kind == TypeReferenceKind::DirectObjCClassName);
    return DirectObjCClassName.get();
  }
};
using TypeReference = TargetTypeReference<InProcess>;

} // end namespace swift

#endif
