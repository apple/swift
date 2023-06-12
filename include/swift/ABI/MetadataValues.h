//===--- MetadataValues.h - Compiler/runtime ABI Metadata -------*- C++ -*-===//
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
// This header is shared between the runtime and the compiler and
// includes target-independent information which can be usefully shared
// between them.
//
//===----------------------------------------------------------------------===//

#ifndef SWIFT_ABI_METADATAVALUES_H
#define SWIFT_ABI_METADATAVALUES_H

#include "swift/ABI/KeyPath.h"
#include "swift/ABI/ProtocolDispatchStrategy.h"
#include "swift/AST/Ownership.h"
#include "swift/Basic/Debug.h"
#include "swift/Basic/LLVM.h"
#include "swift/Basic/FlagSet.h"
#include "llvm/ADT/ArrayRef.h"

#include <stdlib.h>
#include <stdint.h>

namespace swift {

enum {
  /// The number of words (pointers) in a value buffer.
  NumWords_ValueBuffer = 3,

  /// The number of words in a metadata completion context.
  NumWords_MetadataCompletionContext = 4,

  /// The number of words in a yield-once coroutine buffer.
  NumWords_YieldOnceBuffer = 4,

  /// The number of words in a yield-many coroutine buffer.
  NumWords_YieldManyBuffer = 8,

  /// The number of words (in addition to the heap-object header)
  /// in a default actor.
  NumWords_DefaultActor = 12,

  /// The number of words (in addition to the heap-object header)
  /// in a non-default distributed actor.
  NumWords_NonDefaultDistributedActor = 12,

  /// The number of words in a task.
  NumWords_AsyncTask = 24,

  /// The number of words in a task group.
  NumWords_TaskGroup = 32,

  /// The number of words in an AsyncLet (flags + child task context & allocation)
  NumWords_AsyncLet = 80, // 640 bytes ought to be enough for anyone

  /// The size of a unique hash.
  NumBytes_UniqueHash = 16,

  /// The maximum number of generic parameters that can be
  /// implicitly declared, for generic signatures that support that.
  MaxNumImplicitGenericParamDescriptors = 64,
};

struct InProcess;
template <typename Runtime> struct TargetMetadata;
using Metadata = TargetMetadata<InProcess>;

/// Non-type metadata kinds have this bit set.
const unsigned MetadataKindIsNonType = 0x400;

/// Non-heap metadata kinds have this bit set.
const unsigned MetadataKindIsNonHeap = 0x200;

// The above two flags are negative because the "class" kind has to be zero,
// and class metadata is both type and heap metadata.

/// Runtime-private metadata has this bit set. The compiler must not statically
/// generate metadata objects with these kinds, and external tools should not
/// rely on the stability of these values or the precise binary layout of
/// their associated data structures.
const unsigned MetadataKindIsRuntimePrivate = 0x100;

/// Kinds of Swift metadata records.  Some of these are types, some
/// aren't.
enum class MetadataKind : uint32_t {
#define METADATAKIND(name, value) name = value,
#define ABSTRACTMETADATAKIND(name, start, end)                                 \
  name##_Start = start, name##_End = end,
#include "MetadataKind.def"

  /// The largest possible non-isa-pointer metadata kind value.
  ///
  /// This is included in the enumeration to prevent against attempts to
  /// exhaustively match metadata kinds. Future Swift runtimes or compilers
  /// may introduce new metadata kinds, so for forward compatibility, the
  /// runtime must tolerate metadata with unknown kinds.
  /// This specific value is not mapped to a valid metadata kind at this time,
  /// however.
  LastEnumerated = 0x7FF,
};

const unsigned LastEnumeratedMetadataKind =
  (unsigned)MetadataKind::LastEnumerated;

inline bool isHeapMetadataKind(MetadataKind k) {
  return !((uint32_t)k & MetadataKindIsNonHeap);
}
inline bool isTypeMetadataKind(MetadataKind k) {
  return !((uint32_t)k & MetadataKindIsNonType);
}
inline bool isRuntimePrivateMetadataKind(MetadataKind k) {
  return (uint32_t)k & MetadataKindIsRuntimePrivate;
}

/// Try to translate the 'isa' value of a type/heap metadata into a value
/// of the MetadataKind enum.
inline MetadataKind getEnumeratedMetadataKind(uint64_t kind) {
  if (kind > LastEnumeratedMetadataKind)
    return MetadataKind::Class;
  return MetadataKind(kind);
}

StringRef getStringForMetadataKind(MetadataKind kind);

/// Kinds of Swift nominal type descriptor records.
enum class NominalTypeKind : uint32_t {
#define NOMINALTYPEMETADATAKIND(name, value) name = value,
#include "MetadataKind.def"
};

/// The maximum supported type alignment.
const size_t MaximumAlignment = 16;

/// The alignment of a DefaultActor.
const size_t Alignment_DefaultActor = MaximumAlignment;
const size_t Alignment_NonDefaultDistributedActor = MaximumAlignment;

/// The alignment of a TaskGroup.
const size_t Alignment_TaskGroup = MaximumAlignment;

/// The alignment of an AsyncLet.
const size_t Alignment_AsyncLet = MaximumAlignment;

/// Flags stored in the value-witness table.
template <typename int_type>
class TargetValueWitnessFlags {
public:
  // The polarity of these bits is chosen so that, when doing struct layout, the
  // flags of the field types can be mostly bitwise-or'ed together to derive the
  // flags for the struct. (The "non-inline" and "has-extra-inhabitants" bits
  // still require additional fixup.)
  enum : uint32_t {
    AlignmentMask =       0x000000FF,
    // unused             0x0000FF00,
    IsNonPOD =            0x00010000,
    IsNonInline =         0x00020000,
    // unused             0x00040000,
    HasSpareBits =        0x00080000,
    IsNonBitwiseTakable = 0x00100000,
    HasEnumWitnesses =    0x00200000,
    Incomplete =          0x00400000,
    IsNonCopyable =       0x00800000,
    // unused             0xFF000000,
  };

  static constexpr const uint32_t MaxNumExtraInhabitants = 0x7FFFFFFF;

private:
  uint32_t Data;

  explicit constexpr TargetValueWitnessFlags(uint32_t data) : Data(data) {}

public:
  constexpr TargetValueWitnessFlags() : Data(0) {}

  /// The required alignment of the first byte of an object of this
  /// type, expressed as a mask of the low bits that must not be set
  /// in the pointer.
  ///
  /// This representation can be easily converted to the 'alignof'
  /// result by merely adding 1, but it is more directly useful for
  /// performing dynamic structure layouts, and it grants an
  /// additional bit of precision in a compact field without needing
  /// to switch to an exponent representation.
  ///
  /// For example, if the type needs to be 8-byte aligned, the
  /// appropriate alignment mask should be 0x7.
  size_t getAlignmentMask() const {
    return (Data & AlignmentMask);
  }
  constexpr TargetValueWitnessFlags withAlignmentMask(size_t alignMask) const {
    return TargetValueWitnessFlags((Data & ~AlignmentMask) | alignMask);
  }

  size_t getAlignment() const { return getAlignmentMask() + 1; }
  constexpr TargetValueWitnessFlags withAlignment(size_t alignment) const {
    return withAlignmentMask(alignment - 1);
  }

  /// True if the type requires out-of-line allocation of its storage.
  /// This can be the case because the value requires more storage or if it is
  /// not bitwise takable.
  bool isInlineStorage() const { return !(Data & IsNonInline); }
  constexpr TargetValueWitnessFlags withInlineStorage(bool isInline) const {
    return TargetValueWitnessFlags((Data & ~IsNonInline) |
                                   (isInline ? 0 : IsNonInline));
  }

  /// True if values of this type can be copied with memcpy (if it's copyable)
  /// and destroyed with a no-op.
  bool isPOD() const { return !(Data & IsNonPOD); }
  constexpr TargetValueWitnessFlags withPOD(bool isPOD) const {
    return TargetValueWitnessFlags((Data & ~IsNonPOD) |
                                   (isPOD ? 0 : IsNonPOD));
  }

  /// True if values of this type can be taken with memcpy. Unlike C++ 'move',
  /// 'take' is a destructive operation that invalidates the source object, so
  /// most types can be taken with a simple bitwise copy. Only types with side
  /// table references, like @weak references, or types with opaque value
  /// semantics, like imported C++ types, are not bitwise-takable.
  bool isBitwiseTakable() const { return !(Data & IsNonBitwiseTakable); }
  constexpr TargetValueWitnessFlags withBitwiseTakable(bool isBT) const {
    return TargetValueWitnessFlags((Data & ~IsNonBitwiseTakable) |
                                   (isBT ? 0 : IsNonBitwiseTakable));
  }
  
  /// True if values of this type can be copied.
  bool isCopyable() const { return !(Data & IsNonCopyable); }
  constexpr TargetValueWitnessFlags withCopyable(bool isCopyable) const {
    return TargetValueWitnessFlags((Data & ~IsNonCopyable) |
                                   (isCopyable ? 0 : IsNonCopyable));
  }

  /// True if this type's binary representation is that of an enum, and the
  /// enum value witness table entries are available in this type's value
  /// witness table.
  bool hasEnumWitnesses() const { return Data & HasEnumWitnesses; }
  constexpr TargetValueWitnessFlags
  withEnumWitnesses(bool hasEnumWitnesses) const {
    return TargetValueWitnessFlags((Data & ~HasEnumWitnesses) |
                                   (hasEnumWitnesses ? HasEnumWitnesses : 0));
  }

  /// True if the type with this value-witness table is incomplete,
  /// meaning that its external layout (size, etc.) is meaningless
  /// pending completion of the metadata layout.
  bool isIncomplete() const { return Data & Incomplete; }
  constexpr TargetValueWitnessFlags
  withIncomplete(bool isIncomplete) const {
    return TargetValueWitnessFlags((Data & ~Incomplete) |
                                   (isIncomplete ? Incomplete : 0));
  }

  constexpr uint32_t getOpaqueValue() const {
    return Data;
  }
};
using ValueWitnessFlags = TargetValueWitnessFlags<size_t>;

/// Flags for dynamic-cast operations.
enum class DynamicCastFlags : size_t {
  /// All flags clear.
  Default = 0x0,

  /// True if the cast is not permitted to fail.
  Unconditional = 0x1,

  /// True if the cast should 'take' the source value on success;
  /// false if the value should be copied.
  TakeOnSuccess = 0x2,

  /// True if the cast should destroy the source value on failure;
  /// false if the value should be left in place.
  DestroyOnFailure = 0x4,
};
inline bool operator&(DynamicCastFlags a, DynamicCastFlags b) {
  return (size_t(a) & size_t(b)) != 0;
}
inline DynamicCastFlags operator|(DynamicCastFlags a, DynamicCastFlags b) {
  return DynamicCastFlags(size_t(a) | size_t(b));
}
inline DynamicCastFlags operator-(DynamicCastFlags a, DynamicCastFlags b) {
  return DynamicCastFlags(size_t(a) & ~size_t(b));
}
inline DynamicCastFlags &operator|=(DynamicCastFlags &a, DynamicCastFlags b) {
  return a = (a | b);
}

/// Swift class flags.
/// These flags are valid only when isTypeMetadata().
/// When !isTypeMetadata() these flags will collide with other Swift ABIs.
enum class ClassFlags : uint32_t {
  /// Is this a Swift class from the Darwin pre-stable ABI?
  /// This bit is clear in stable ABI Swift classes.
  /// The Objective-C runtime also reads this bit.
  IsSwiftPreStableABI = 0x1,

  /// Does this class use Swift refcounting?
  UsesSwiftRefcounting = 0x2,

  /// Has this class a custom name, specified with the @objc attribute?
  HasCustomObjCName = 0x4,

  /// Whether this metadata is a specialization of a generic metadata pattern
  /// which was created during compilation.
  IsStaticSpecialization = 0x8,

  /// Whether this metadata is a specialization of a generic metadata pattern
  /// which was created during compilation and made to be canonical by
  /// modifying the metadata accessor.
  IsCanonicalStaticSpecialization = 0x10,
};
inline bool operator&(ClassFlags a, ClassFlags b) {
  return (uint32_t(a) & uint32_t(b)) != 0;
}
inline ClassFlags operator|(ClassFlags a, ClassFlags b) {
  return ClassFlags(uint32_t(a) | uint32_t(b));
}
inline ClassFlags &operator|=(ClassFlags &a, ClassFlags b) {
  return a = (a | b);
}

/// Flags that go in a MethodDescriptor structure.
class MethodDescriptorFlags {
public:
  typedef uint32_t int_type;
  enum class Kind {
    Method,
    Init,
    Getter,
    Setter,
    ModifyCoroutine,
    ReadCoroutine,
  };

private:
  enum : int_type {
    KindMask = 0x0F,                // 16 kinds should be enough for anybody
    IsInstanceMask = 0x10,
    IsDynamicMask = 0x20,
    IsAsyncMask = 0x40,
    ExtraDiscriminatorShift = 16,
    ExtraDiscriminatorMask = 0xFFFF0000,
  };

  int_type Value;

public:
  MethodDescriptorFlags(Kind kind) : Value(unsigned(kind)) {}

  MethodDescriptorFlags withIsInstance(bool isInstance) const {
    auto copy = *this;
    if (isInstance) {
      copy.Value |= IsInstanceMask;
    } else {
      copy.Value &= ~IsInstanceMask;
    }
    return copy;
  }

  MethodDescriptorFlags withIsDynamic(bool isDynamic) const {
    auto copy = *this;
    if (isDynamic)
      copy.Value |= IsDynamicMask;
    else
      copy.Value &= ~IsDynamicMask;
    return copy;
  }

  MethodDescriptorFlags withIsAsync(bool isAsync) const {
    auto copy = *this;
    if (isAsync)
      copy.Value |= IsAsyncMask;
    else
      copy.Value &= ~IsAsyncMask;
    return copy;
  }

  MethodDescriptorFlags withExtraDiscriminator(uint16_t value) const {
    auto copy = *this;
    copy.Value = (copy.Value & ~ExtraDiscriminatorMask)
               | (int_type(value) << ExtraDiscriminatorShift);
    return copy;
  }

  Kind getKind() const { return Kind(Value & KindMask); }

  /// Is the method marked 'dynamic'?
  bool isDynamic() const { return Value & IsDynamicMask; }

  /// Is the method an instance member?
  ///
  /// Note that 'init' is not considered an instance member.
  bool isInstance() const { return Value & IsInstanceMask; }

  bool isAsync() const { return Value & IsAsyncMask; }

  uint16_t getExtraDiscriminator() const {
    return (Value >> ExtraDiscriminatorShift);
  }

  int_type getIntValue() const { return Value; }
};

enum : unsigned {
  /// Number of words reserved in generic metadata patterns.
  NumGenericMetadataPrivateDataWords = 16,
};

/// Kinds of type metadata/protocol conformance records.
enum class TypeReferenceKind : unsigned {
  /// The conformance is for a nominal type referenced directly;
  /// getTypeDescriptor() points to the type context descriptor.
  DirectTypeDescriptor = 0x00,

  /// The conformance is for a nominal type referenced indirectly;
  /// getTypeDescriptor() points to the type context descriptor.
  IndirectTypeDescriptor = 0x01,

  /// The conformance is for an Objective-C class that should be looked up
  /// by class name.
  DirectObjCClassName = 0x02,

  /// The conformance is for an Objective-C class that has no nominal type
  /// descriptor.
  /// getIndirectObjCClass() points to a variable that contains the pointer to
  /// the class object, which then requires a runtime call to get metadata.
  ///
  /// On platforms without Objective-C interoperability, this case is
  /// unused.
  IndirectObjCClass = 0x03,

  // We only reserve three bits for this in the various places we store it.

  First_Kind = DirectTypeDescriptor,
  Last_Kind = IndirectObjCClass,
};

/// Flag that indicates whether an existential type is class-constrained or not.
enum class ProtocolClassConstraint : bool {
  /// The protocol is class-constrained, so only class types can conform to it.
  ///
  /// This must be 0 for ABI compatibility with Objective-C protocol_t records.
  Class = false,
  /// Any type can conform to the protocol.
  Any = true,
};

/// Identifiers for protocols with special meaning to the Swift runtime.
enum class SpecialProtocol: uint8_t {
  /// Not a special protocol.
  ///
  /// This must be 0 for ABI compatibility with Objective-C protocol_t records.
  None = 0,
  /// The Error protocol.
  Error = 1,
};

/// Flags for protocol descriptors.
class ProtocolDescriptorFlags {
  typedef uint32_t int_type;
  enum : int_type {
    IsSwift           =   1U <<  0U,
    ClassConstraint   =   1U <<  1U,

    DispatchStrategyMask  = 0xFU << 2U,
    DispatchStrategyShift = 2,

    SpecialProtocolMask  = 0x000003C0U,
    SpecialProtocolShift = 6,

    IsResilient       =   1U <<  10U,

    /// Reserved by the ObjC runtime.
    _ObjCReserved        = 0xFFFF0000U,
  };

  int_type Data;

  constexpr ProtocolDescriptorFlags(int_type Data) : Data(Data) {}
public:
  constexpr ProtocolDescriptorFlags() : Data(0) {}
  constexpr ProtocolDescriptorFlags withSwift(bool s) const {
    return ProtocolDescriptorFlags((Data & ~IsSwift) | (s ? IsSwift : 0));
  }
  constexpr ProtocolDescriptorFlags withClassConstraint(
                                              ProtocolClassConstraint c) const {
    return ProtocolDescriptorFlags((Data & ~ClassConstraint)
                                     | (bool(c) ? ClassConstraint : 0));
  }
  constexpr ProtocolDescriptorFlags withDispatchStrategy(
                                             ProtocolDispatchStrategy s) const {
    return ProtocolDescriptorFlags((Data & ~DispatchStrategyMask)
                                     | (int_type(s) << DispatchStrategyShift));
  }
  constexpr ProtocolDescriptorFlags
  withSpecialProtocol(SpecialProtocol sp) const {
    return ProtocolDescriptorFlags((Data & ~SpecialProtocolMask)
                                     | (int_type(sp) << SpecialProtocolShift));
  }
  constexpr ProtocolDescriptorFlags withResilient(bool s) const {
    return ProtocolDescriptorFlags((Data & ~IsResilient) | (s ? IsResilient : 0));
  }

  /// Was the protocol defined in Swift 1 or 2?
  bool isSwift() const { return Data & IsSwift; }

  /// Is the protocol class-constrained?
  ProtocolClassConstraint getClassConstraint() const {
    return ProtocolClassConstraint(bool(Data & ClassConstraint));
  }

  /// What dispatch strategy does this protocol use?
  ProtocolDispatchStrategy getDispatchStrategy() const {
    return ProtocolDispatchStrategy((Data & DispatchStrategyMask)
                                      >> DispatchStrategyShift);
  }

  /// Does the protocol require a witness table for method dispatch?
  bool needsWitnessTable() const {
    return swift::protocolRequiresWitnessTable(getDispatchStrategy());
  }

  /// Return the identifier if this is a special runtime-known protocol.
  SpecialProtocol getSpecialProtocol() const {
    return SpecialProtocol(uint8_t((Data & SpecialProtocolMask)
                                 >> SpecialProtocolShift));
  }

  /// Can new requirements with default witnesses be added resiliently?
  bool isResilient() const { return Data & IsResilient; }

  int_type getIntValue() const {
    return Data;
  }

#ifndef NDEBUG
  SWIFT_DEBUG_DUMP;
#endif
};

/// Flags that go in a ProtocolRequirement structure.
class ProtocolRequirementFlags {
public:
  typedef uint32_t int_type;
  enum class Kind {
    BaseProtocol,
    Method,
    Init,
    Getter,
    Setter,
    ReadCoroutine,
    ModifyCoroutine,
    AssociatedTypeAccessFunction,
    AssociatedConformanceAccessFunction,
  };

private:
  enum : int_type {
    KindMask = 0x0F,                // 16 kinds should be enough for anybody
    IsInstanceMask = 0x10,
    IsAsyncMask = 0x20,
    ExtraDiscriminatorShift = 16,
    ExtraDiscriminatorMask = 0xFFFF0000,
  };

  int_type Value;

public:
  ProtocolRequirementFlags(Kind kind) : Value(unsigned(kind)) {}

  ProtocolRequirementFlags withIsInstance(bool isInstance) const {
    auto copy = *this;
    if (isInstance) {
      copy.Value |= IsInstanceMask;
    } else {
      copy.Value &= ~IsInstanceMask;
    }
    return copy;
  }

  ProtocolRequirementFlags withIsAsync(bool isAsync) const {
    auto copy = *this;
    if (isAsync)
      copy.Value |= IsAsyncMask;
    else
      copy.Value &= ~IsAsyncMask;
    return copy;
  }

  ProtocolRequirementFlags withExtraDiscriminator(uint16_t value) const {
    auto copy = *this;
    copy.Value = (copy.Value & ~ExtraDiscriminatorMask)
               | (int_type(value) << ExtraDiscriminatorShift);
    return copy;
  }

  Kind getKind() const { return Kind(Value & KindMask); }

  /// Is the method an instance member?
  ///
  /// Note that 'init' is not considered an instance member.
  bool isInstance() const { return Value & IsInstanceMask; }

  bool isAsync() const { return Value & IsAsyncMask; }

  bool isSignedWithAddress() const {
    return getKind() != Kind::BaseProtocol;
  }

  uint16_t getExtraDiscriminator() const {
    return (Value >> ExtraDiscriminatorShift);
  }

  int_type getIntValue() const { return Value; }

  /// Is the method implementation is represented as a native function pointer?
  bool isFunctionImpl() const {
    switch (getKind()) {
    case ProtocolRequirementFlags::Kind::Method:
    case ProtocolRequirementFlags::Kind::Init:
    case ProtocolRequirementFlags::Kind::Getter:
    case ProtocolRequirementFlags::Kind::Setter:
    case ProtocolRequirementFlags::Kind::ReadCoroutine:
    case ProtocolRequirementFlags::Kind::ModifyCoroutine:
      return !isAsync();
    default:
      return false;
    }
  }

  enum : uintptr_t {
    /// Bit used to indicate that an associated type witness is a pointer to
    /// a mangled name (vs. a pointer to metadata).
    AssociatedTypeMangledNameBit = 0x01,
  };

  enum : uint8_t {
    /// Prefix byte used to identify an associated type whose mangled name
    /// is relative to the protocol's context rather than the conforming
    /// type's context.
    AssociatedTypeInProtocolContextByte = 0xFF
  };
};

/// Flags that go in a TargetConformanceDescriptor structure.
class ConformanceFlags {
public:
  typedef uint32_t int_type;

private:
  enum : int_type {
    UnusedLowBits = 0x07,      // historical conformance kind

    TypeMetadataKindMask = 0x7 << 3, // 8 type reference kinds
    TypeMetadataKindShift = 3,

    IsRetroactiveMask = 0x01 << 6,
    IsSynthesizedNonUniqueMask = 0x01 << 7,

    NumConditionalRequirementsMask = 0xFF << 8,
    NumConditionalRequirementsShift = 8,

    HasResilientWitnessesMask = 0x01 << 16,
    HasGenericWitnessTableMask = 0x01 << 17,
  };

  int_type Value;

public:
  ConformanceFlags(int_type value = 0) : Value(value) {}

  ConformanceFlags withTypeReferenceKind(TypeReferenceKind kind) const {
    return ConformanceFlags((Value & ~TypeMetadataKindMask)
                            | (int_type(kind) << TypeMetadataKindShift));
  }

  ConformanceFlags withIsRetroactive(bool isRetroactive) const {
    return ConformanceFlags((Value & ~IsRetroactiveMask)
                            | (isRetroactive? IsRetroactiveMask : 0));
  }

  ConformanceFlags withIsSynthesizedNonUnique(
                                          bool isSynthesizedNonUnique) const {
    return ConformanceFlags(
                  (Value & ~IsSynthesizedNonUniqueMask)
                  | (isSynthesizedNonUnique ? IsSynthesizedNonUniqueMask : 0));
  }

  ConformanceFlags withNumConditionalRequirements(unsigned n) const {
    return ConformanceFlags((Value & ~NumConditionalRequirementsMask)
                            | (n << NumConditionalRequirementsShift));
  }

  ConformanceFlags withHasResilientWitnesses(bool hasResilientWitnesses) const {
    return ConformanceFlags((Value & ~HasResilientWitnessesMask)
                            | (hasResilientWitnesses? HasResilientWitnessesMask
                                                    : 0));
  }

  ConformanceFlags withHasGenericWitnessTable(
                                           bool hasGenericWitnessTable) const {
    return ConformanceFlags((Value & ~HasGenericWitnessTableMask)
                            | (hasGenericWitnessTable
                                 ? HasGenericWitnessTableMask
                                 : 0));
  }

  /// Retrieve the type reference kind kind.
  TypeReferenceKind getTypeReferenceKind() const {
    return TypeReferenceKind(
                      (Value & TypeMetadataKindMask) >> TypeMetadataKindShift);
  }

  /// Is the conformance "retroactive"?
  ///
  /// A conformance is retroactive when it occurs in a module that is
  /// neither the module in which the protocol is defined nor the module
  /// in which the conforming type is defined. With retroactive conformance,
  /// it is possible to detect a conflict at run time.
  bool isRetroactive() const { return Value & IsRetroactiveMask; }

  /// Is the conformance synthesized in a non-unique manner?
  ///
  /// The Swift compiler will synthesize conformances on behalf of some
  /// imported entities (e.g., C typedefs with the swift_wrapper attribute).
  /// Such conformances are retroactive by nature, but the presence of multiple
  /// such conformances is not a conflict because all synthesized conformances
  /// will be equivalent.
  bool isSynthesizedNonUnique() const {
    return Value & IsSynthesizedNonUniqueMask;
  }

  /// Retrieve the # of conditional requirements.
  unsigned getNumConditionalRequirements() const {
    return (Value & NumConditionalRequirementsMask)
              >> NumConditionalRequirementsShift;
  }

  /// Whether this conformance has any resilient witnesses.
  bool hasResilientWitnesses() const {
    return Value & HasResilientWitnessesMask;
  }

  /// Whether this conformance has a generic witness table that may need to
  /// be instantiated.
  bool hasGenericWitnessTable() const {
    return Value & HasGenericWitnessTableMask;
  }

  int_type getIntValue() const { return Value; }
};

/// Flags in an existential type metadata record.
class ExistentialTypeFlags {
public:
  typedef uint32_t int_type;

private:
  enum : int_type {
    NumWitnessTablesMask  = 0x00FFFFFFU,
    ClassConstraintMask   = 0x80000000U, // Warning: Set if NOT class-constrained!
    HasSuperclassMask     = 0x40000000U,
    SpecialProtocolMask   = 0x3F000000U,
    SpecialProtocolShift  = 24U,
  };
  int_type Data;

public:
  constexpr ExistentialTypeFlags(int_type Data) : Data(Data) {}
  constexpr ExistentialTypeFlags() : Data(0) {}
  constexpr ExistentialTypeFlags withNumWitnessTables(unsigned numTables) const {
    return ExistentialTypeFlags((Data & ~NumWitnessTablesMask) | numTables);
  }
  constexpr ExistentialTypeFlags
  withClassConstraint(ProtocolClassConstraint c) const {
    return ExistentialTypeFlags((Data & ~ClassConstraintMask)
                                  | (bool(c) ? ClassConstraintMask : 0));
  }
  constexpr ExistentialTypeFlags
  withHasSuperclass(bool hasSuperclass) const {
    return ExistentialTypeFlags((Data & ~HasSuperclassMask)
                                  | (hasSuperclass ? HasSuperclassMask : 0));
  }
  constexpr ExistentialTypeFlags
  withSpecialProtocol(SpecialProtocol sp) const {
    return ExistentialTypeFlags((Data & ~SpecialProtocolMask)
                                  | (int_type(sp) << SpecialProtocolShift));
  }

  unsigned getNumWitnessTables() const {
    return Data & NumWitnessTablesMask;
  }

  ProtocolClassConstraint getClassConstraint() const {
    return ProtocolClassConstraint(bool(Data & ClassConstraintMask));
  }

  bool hasSuperclassConstraint() const {
    return bool(Data & HasSuperclassMask);
  }

  /// Return whether this existential type represents an uncomposed special
  /// protocol.
  SpecialProtocol getSpecialProtocol() const {
    return SpecialProtocol(uint8_t((Data & SpecialProtocolMask)
                                     >> SpecialProtocolShift));
  }

  int_type getIntValue() const {
    return Data;
  }
};

/// Flags in an extended existential shape.
class ExtendedExistentialTypeShapeFlags {
public:
  typedef uint32_t int_type;

  /// Special cases for the representation.
  enum class SpecialKind {
    None = 0,

    /// The existential has a class constraint.
    /// The inline storage is sizeof(void*) / alignof(void*),
    /// the value is always stored inline, the value is reference-
    /// counted (using unknown reference counting), and the
    /// type metadata for the requirement generic parameters are
    /// not stored in the existential container because they can
    /// be recovered from the instance type of the class.
    Class = 1,

    /// The existential has a metatype constraint.
    /// The inline storage is sizeof(void*) / alignof(void*),
    /// the value is always stored inline, the value is a Metadata*,
    /// and the type metadata for the requirement generic parameters
    /// are not stored in the existential container because they can
    /// be recovered from the stored metatype.
    Metatype = 2,

    /// The inline value storage has a non-storage layout.  The shape
    /// must include a value witness table.  Type metadata for the
    /// requirement generic parameters are still stored in the existential
    /// container.
    ExplicitLayout = 3,

    // 255 is the maximum
  };

private:
  enum : int_type {
    SpecialKindMask             = 0x000000FFU,
    SpecialKindShift            = 0,
    HasGeneralizationSignature  = 0x00000100U,
    HasTypeExpression           = 0x00000200U,
    HasSuggestedValueWitnesses  = 0x00000400U,
    HasImplicitReqSigParams     = 0x00000800U,
    HasImplicitGenSigParams     = 0x00001000U,
    HasTypePacks                = 0x00002000U,
  };
  int_type Data;

public:
  constexpr ExtendedExistentialTypeShapeFlags() : Data(0) {}
  constexpr ExtendedExistentialTypeShapeFlags(int_type Data) : Data(Data) {}
  constexpr ExtendedExistentialTypeShapeFlags
  withSpecialKind(SpecialKind kind) const {
    return ExtendedExistentialTypeShapeFlags(
      (Data & ~SpecialKindMask) | (int_type(kind) << SpecialKindShift));
  }
  constexpr ExtendedExistentialTypeShapeFlags
  withHasTypeExpression(bool hasTypeExpression) const {
    return ExtendedExistentialTypeShapeFlags(
      hasTypeExpression ? (Data | HasTypeExpression)
                        : (Data & ~HasTypeExpression));
  }
  constexpr ExtendedExistentialTypeShapeFlags
  withGeneralizationSignature(bool hasGeneralization) const {
    return ExtendedExistentialTypeShapeFlags(
      hasGeneralization ? (Data | HasGeneralizationSignature)
                        : (Data & ~HasGeneralizationSignature));
  }
  constexpr ExtendedExistentialTypeShapeFlags
  withSuggestedValueWitnesses(bool hasSuggestedVWT) const {
    return ExtendedExistentialTypeShapeFlags(
      hasSuggestedVWT ? (Data | HasSuggestedValueWitnesses)
                      : (Data & ~HasSuggestedValueWitnesses));
  }
  constexpr ExtendedExistentialTypeShapeFlags
  withImplicitReqSigParams(bool implicit) const {
    return ExtendedExistentialTypeShapeFlags(
      implicit ? (Data | HasImplicitReqSigParams)
               : (Data & ~HasImplicitReqSigParams));
  }
  constexpr ExtendedExistentialTypeShapeFlags
  withImplicitGenSigParams(bool implicit) const {
    return ExtendedExistentialTypeShapeFlags(
      implicit ? (Data | HasImplicitGenSigParams)
               : (Data & ~HasImplicitGenSigParams));
  }
  constexpr ExtendedExistentialTypeShapeFlags
  withTypePacks(bool hasTypePacks) const {
    return ExtendedExistentialTypeShapeFlags(
      hasTypePacks ? (Data | HasTypePacks)
                   : (Data & ~HasTypePacks));
  }

  /// Is this a special kind of existential?
  SpecialKind getSpecialKind() const {
    return SpecialKind((Data & SpecialKindMask) >> SpecialKindShift);
  }
  bool isOpaque() const { return getSpecialKind() == SpecialKind::None; }
  bool isClassConstrained() const {
    return getSpecialKind() == SpecialKind::Class;
  }
  bool isMetatypeConstrained() const {
    return getSpecialKind() == SpecialKind::Metatype;
  }

  bool hasGeneralizationSignature() const {
    return Data & HasGeneralizationSignature;
  }

  bool hasTypeExpression() const {
    return Data & HasTypeExpression;
  }

  bool hasSuggestedValueWitnesses() const {
    return Data & HasSuggestedValueWitnesses;
  }

  /// The parameters of the requirement signature are not stored
  /// explicitly in the shape.
  ///
  /// In order to enable this, there must be no more than
  /// MaxNumImplicitGenericParamDescriptors generic parameters, and
  /// they must match GenericParamDescriptor::implicit().
  bool hasImplicitReqSigParams() const {
    return Data & HasImplicitReqSigParams;
  }

  /// The parameters of the generalization signature are not stored
  /// explicitly in the shape.
  ///
  /// In order to enable this, there must be no more than
  /// MaxNumImplicitGenericParamDescriptors generic parameters, and
  /// they must match GenericParamDescriptor::implicit().
  bool hasImplicitGenSigParams() const {
    return Data & HasImplicitGenSigParams;
  }

  /// Whether the generic context has type parameter packs. This
  /// occurs when the existential has a superclass requirement
  /// whose class declaration has a type parameter pack, eg
  /// `any P & C<...>` with `class C<each T> {}`.
  bool hasTypePacks() const {
    return Data & HasTypePacks;
  }

  int_type getIntValue() const {
    return Data;
  }
};

/// Convention values for function type metadata.
enum class FunctionMetadataConvention: uint8_t {
  Swift = 0,
  Block = 1,
  Thin = 2,
  CFunctionPointer = 3,
};

/// Differentiability kind for function type metadata.
/// Duplicates `DifferentiabilityKind` in AST/AutoDiff.h.
template <typename int_type>
struct TargetFunctionMetadataDifferentiabilityKind {
  enum Value : int_type {
    NonDifferentiable = 0,
    Forward           = 1,
    Reverse           = 2,
    Normal            = 3,
    Linear            = 4,
  } Value;

  constexpr TargetFunctionMetadataDifferentiabilityKind(
      enum Value value = NonDifferentiable) : Value(value) {}

  int_type getIntValue() const {
    return (int_type)Value;
  }

  bool isDifferentiable() const {
    return Value != NonDifferentiable;
  }
};
using FunctionMetadataDifferentiabilityKind =
    TargetFunctionMetadataDifferentiabilityKind<size_t>;

/// Flags in a function type metadata record.
template <typename int_type>
class TargetFunctionTypeFlags {
  // If we were ever to run out of space for function flags (8 bits)
  // one of the flag bits could be used to identify that the rest of
  // the flags is going to be stored somewhere else in the metadata.
  enum : int_type {
    NumParametersMask      = 0x0000FFFFU,
    ConventionMask         = 0x00FF0000U,
    ConventionShift        = 16U,
    ThrowsMask             = 0x01000000U,
    ParamFlagsMask         = 0x02000000U,
    EscapingMask           = 0x04000000U,
    DifferentiableMask     = 0x08000000U,
    GlobalActorMask        = 0x10000000U,
    AsyncMask              = 0x20000000U,
    SendableMask           = 0x40000000U,
    // NOTE: The next bit will need to introduce a separate flags word.
  };
  int_type Data;

  constexpr TargetFunctionTypeFlags(int_type Data) : Data(Data) {}
public:
  constexpr TargetFunctionTypeFlags() : Data(0) {}

  constexpr TargetFunctionTypeFlags
  withNumParameters(unsigned numParams) const {
    return TargetFunctionTypeFlags((Data & ~NumParametersMask) | numParams);
  }

  constexpr TargetFunctionTypeFlags<int_type>
  withConvention(FunctionMetadataConvention c) const {
    return TargetFunctionTypeFlags((Data & ~ConventionMask)
                             | (int_type(c) << ConventionShift));
  }

  constexpr TargetFunctionTypeFlags<int_type>
  withAsync(bool async) const {
    return TargetFunctionTypeFlags<int_type>((Data & ~AsyncMask) |
                                             (async ? AsyncMask : 0));
  }

  constexpr TargetFunctionTypeFlags<int_type>
  withThrows(bool throws) const {
    return TargetFunctionTypeFlags<int_type>((Data & ~ThrowsMask) |
                                             (throws ? ThrowsMask : 0));
  }

  constexpr TargetFunctionTypeFlags<int_type>
  withDifferentiable(bool differentiable) const {
    return TargetFunctionTypeFlags<int_type>((Data & ~DifferentiableMask) |
                                     (differentiable ? DifferentiableMask : 0));
  }

  constexpr TargetFunctionTypeFlags<int_type>
  withParameterFlags(bool hasFlags) const {
    return TargetFunctionTypeFlags<int_type>((Data & ~ParamFlagsMask) |
                                             (hasFlags ? ParamFlagsMask : 0));
  }

  constexpr TargetFunctionTypeFlags<int_type>
  withEscaping(bool isEscaping) const {
    return TargetFunctionTypeFlags<int_type>((Data & ~EscapingMask) |
                                             (isEscaping ? EscapingMask : 0));
  }

  constexpr TargetFunctionTypeFlags<int_type>
  withConcurrent(bool isSendable) const {
    return TargetFunctionTypeFlags<int_type>(
        (Data & ~SendableMask) |
        (isSendable ? SendableMask : 0));
  }

  constexpr TargetFunctionTypeFlags<int_type>
  withGlobalActor(bool globalActor) const {
    return TargetFunctionTypeFlags<int_type>(
        (Data & ~GlobalActorMask) | (globalActor ? GlobalActorMask : 0));
  }

  unsigned getNumParameters() const { return Data & NumParametersMask; }

  FunctionMetadataConvention getConvention() const {
    return FunctionMetadataConvention((Data&ConventionMask) >> ConventionShift);
  }

  bool isAsync() const { return bool(Data & AsyncMask); }

  bool isThrowing() const { return bool(Data & ThrowsMask); }

  bool isEscaping() const {
    return bool (Data & EscapingMask);
  }

  bool isSendable() const {
    return bool (Data & SendableMask);
  }

  bool hasParameterFlags() const { return bool(Data & ParamFlagsMask); }

  bool isDifferentiable() const {
    return bool (Data & DifferentiableMask);
  }

  bool hasGlobalActor() const {
    return bool (Data & GlobalActorMask);
  }

  int_type getIntValue() const {
    return Data;
  }

  static TargetFunctionTypeFlags<int_type> fromIntValue(int_type Data) {
    return TargetFunctionTypeFlags(Data);
  }

  bool operator==(TargetFunctionTypeFlags<int_type> other) const {
    return Data == other.Data;
  }
  bool operator!=(TargetFunctionTypeFlags<int_type> other) const {
    return Data != other.Data;
  }
};
using FunctionTypeFlags = TargetFunctionTypeFlags<size_t>;

template <typename int_type>
class TargetParameterTypeFlags {
  enum : int_type {
    ValueOwnershipMask    = 0x7F,
    VariadicMask          = 0x80,
    AutoClosureMask       = 0x100,
    NoDerivativeMask      = 0x200,
    IsolatedMask          = 0x400,
  };
  int_type Data;

  constexpr TargetParameterTypeFlags(int_type Data) : Data(Data) {}

public:
  constexpr TargetParameterTypeFlags() : Data(0) {}

  constexpr TargetParameterTypeFlags<int_type>
  withValueOwnership(ValueOwnership ownership) const {
    return TargetParameterTypeFlags<int_type>((Data & ~ValueOwnershipMask) |
                                              (int_type)ownership);
  }

  constexpr TargetParameterTypeFlags<int_type>
  withVariadic(bool isVariadic) const {
    return TargetParameterTypeFlags<int_type>((Data & ~VariadicMask) |
                                              (isVariadic ? VariadicMask : 0));
  }

  constexpr TargetParameterTypeFlags<int_type>
  withAutoClosure(bool isAutoClosure) const {
    return TargetParameterTypeFlags<int_type>(
        (Data & ~AutoClosureMask) | (isAutoClosure ? AutoClosureMask : 0));
  }

  constexpr TargetParameterTypeFlags<int_type>
  withNoDerivative(bool isNoDerivative) const {
    return TargetParameterTypeFlags<int_type>(
        (Data & ~NoDerivativeMask) | (isNoDerivative ? NoDerivativeMask : 0));
  }

  constexpr TargetParameterTypeFlags<int_type>
  withIsolated(bool isIsolated) const {
    return TargetParameterTypeFlags<int_type>(
        (Data & ~IsolatedMask) | (isIsolated ? IsolatedMask : 0));
  }

  bool isNone() const { return Data == 0; }
  bool isVariadic() const { return Data & VariadicMask; }
  bool isAutoClosure() const { return Data & AutoClosureMask; }
  bool isNoDerivative() const { return Data & NoDerivativeMask; }
  bool isIsolated() const { return Data & IsolatedMask; }

  ValueOwnership getValueOwnership() const {
    return (ValueOwnership)(Data & ValueOwnershipMask);
  }

  int_type getIntValue() const { return Data; }

  static TargetParameterTypeFlags<int_type> fromIntValue(int_type Data) {
    return TargetParameterTypeFlags(Data);
  }

  bool operator==(TargetParameterTypeFlags<int_type> other) const {
    return Data == other.Data;
  }
  bool operator!=(TargetParameterTypeFlags<int_type> other) const {
    return Data != other.Data;
  }
};
using ParameterFlags = TargetParameterTypeFlags<uint32_t>;

template <typename int_type>
class TargetTupleTypeFlags {
  enum : int_type {
    NumElementsMask = 0x0000FFFFU,
    NonConstantLabelsMask = 0x00010000U,
  };
  int_type Data;

public:
  constexpr TargetTupleTypeFlags() : Data(0) {}
  constexpr TargetTupleTypeFlags(int_type Data) : Data(Data) {}

  constexpr TargetTupleTypeFlags
  withNumElements(unsigned numElements) const {
    return TargetTupleTypeFlags((Data & ~NumElementsMask) | numElements);
  }

  constexpr TargetTupleTypeFlags<int_type> withNonConstantLabels(
                                             bool hasNonConstantLabels) const {
    return TargetTupleTypeFlags<int_type>(
                        (Data & ~NonConstantLabelsMask) |
                          (hasNonConstantLabels ? NonConstantLabelsMask : 0));
  }

  unsigned getNumElements() const { return Data & NumElementsMask; }

  bool hasNonConstantLabels() const { return Data & NonConstantLabelsMask; }

  int_type getIntValue() const { return Data; }

  static TargetTupleTypeFlags<int_type> fromIntValue(int_type Data) {
    return TargetTupleTypeFlags(Data);
  }

  bool operator==(TargetTupleTypeFlags<int_type> other) const {
    return Data == other.Data;
  }
  bool operator!=(TargetTupleTypeFlags<int_type> other) const {
    return Data != other.Data;
  }
};
using TupleTypeFlags = TargetTupleTypeFlags<size_t>;

/// Flags for exclusivity-checking operations.
enum class ExclusivityFlags : uintptr_t {
  Read             = 0x0,
  Modify           = 0x1,
  // ActionMask can grow without breaking the ABI because the runtime controls
  // how these flags are encoded in the "value buffer". However, any additional
  // actions must be compatible with the original behavior for the old, smaller
  // ActionMask (older runtimes will continue to treat them as either a simple
  // Read or Modify).
  ActionMask       = 0x1,

  // The runtime should track this access to check against subsequent accesses.
  Tracking         = 0x20
};
static inline ExclusivityFlags operator|(ExclusivityFlags lhs,
                                         ExclusivityFlags rhs) {
  return ExclusivityFlags(uintptr_t(lhs) | uintptr_t(rhs));
}
static inline ExclusivityFlags &operator|=(ExclusivityFlags &lhs,
                                           ExclusivityFlags rhs) {
  return (lhs = (lhs | rhs));
}
static inline ExclusivityFlags getAccessAction(ExclusivityFlags flags) {
  return ExclusivityFlags(uintptr_t(flags)
                        & uintptr_t(ExclusivityFlags::ActionMask));
}
static inline bool isTracking(ExclusivityFlags flags) {
  return uintptr_t(flags) & uintptr_t(ExclusivityFlags::Tracking);
}

/// Flags for struct layout.
enum class StructLayoutFlags : uintptr_t {
  /// Reserve space for 256 layout algorithms.
  AlgorithmMask     = 0xff,

  /// The ABI baseline algorithm, i.e. the algorithm implemented in Swift 5.
  Swift5Algorithm   = 0x00,

  /// Is the value-witness table mutable in place, or does layout need to
  /// clone it?
  IsVWTMutable      = 0x100,
};
static inline StructLayoutFlags operator|(StructLayoutFlags lhs,
                                          StructLayoutFlags rhs) {
  return StructLayoutFlags(uintptr_t(lhs) | uintptr_t(rhs));
}
static inline StructLayoutFlags &operator|=(StructLayoutFlags &lhs,
                                            StructLayoutFlags rhs) {
  return (lhs = (lhs | rhs));
}
static inline StructLayoutFlags getLayoutAlgorithm(StructLayoutFlags flags) {
  return StructLayoutFlags(uintptr_t(flags)
                             & uintptr_t(StructLayoutFlags::AlgorithmMask));
}
static inline bool isValueWitnessTableMutable(StructLayoutFlags flags) {
  return uintptr_t(flags) & uintptr_t(StructLayoutFlags::IsVWTMutable);
}

/// Flags for class layout.
enum class ClassLayoutFlags : uintptr_t {
  /// Reserve space for 256 layout algorithms.
  AlgorithmMask     = 0xff,

  /// The ABI baseline algorithm, i.e. the algorithm implemented in Swift 5.
  Swift5Algorithm   = 0x00,

  /// If true, the vtable for this class and all of its superclasses was emitted
  /// statically in the class metadata. If false, the superclass vtable is
  /// copied from superclass metadata, and the immediate class vtable is
  /// initialized from the type context descriptor.
  HasStaticVTable   = 0x100,
};
static inline ClassLayoutFlags operator|(ClassLayoutFlags lhs,
                                         ClassLayoutFlags rhs) {
  return ClassLayoutFlags(uintptr_t(lhs) | uintptr_t(rhs));
}
static inline ClassLayoutFlags &operator|=(ClassLayoutFlags &lhs,
                                           ClassLayoutFlags rhs) {
  return (lhs = (lhs | rhs));
}
static inline ClassLayoutFlags getLayoutAlgorithm(ClassLayoutFlags flags) {
  return ClassLayoutFlags(uintptr_t(flags)
                             & uintptr_t(ClassLayoutFlags::AlgorithmMask));
}
static inline bool hasStaticVTable(ClassLayoutFlags flags) {
  return uintptr_t(flags) & uintptr_t(ClassLayoutFlags::HasStaticVTable);
}

/// Flags for enum layout.
enum class EnumLayoutFlags : uintptr_t {
  /// Reserve space for 256 layout algorithms.
  AlgorithmMask     = 0xff,

  /// The ABI baseline algorithm, i.e. the algorithm implemented in Swift 5.
  Swift5Algorithm   = 0x00,

  /// Is the value-witness table mutable in place, or does layout need to
  /// clone it?
  IsVWTMutable      = 0x100,
};
static inline EnumLayoutFlags operator|(EnumLayoutFlags lhs,
                                        EnumLayoutFlags rhs) {
  return EnumLayoutFlags(uintptr_t(lhs) | uintptr_t(rhs));
}
static inline EnumLayoutFlags &operator|=(EnumLayoutFlags &lhs,
                                          EnumLayoutFlags rhs) {
  return (lhs = (lhs | rhs));
}
static inline EnumLayoutFlags getLayoutAlgorithm(EnumLayoutFlags flags) {
  return EnumLayoutFlags(uintptr_t(flags)
                           & uintptr_t(EnumLayoutFlags::AlgorithmMask));
}
static inline bool isValueWitnessTableMutable(EnumLayoutFlags flags) {
  return uintptr_t(flags) & uintptr_t(EnumLayoutFlags::IsVWTMutable);
}

namespace SpecialPointerAuthDiscriminators {
  // All of these values are the stable string hash of the corresponding
  // variable name:
  //   (computeStableStringHash % 65535 + 1)

  /// HeapMetadataHeader::destroy
  const uint16_t HeapDestructor = 0xbbbf;

  /// Type descriptor data pointers.
  const uint16_t TypeDescriptor = 0xae86;

  /// Runtime function variables exported by the runtime.
  const uint16_t RuntimeFunctionEntry = 0x625b;

  /// Protocol conformance descriptors.
  const uint16_t ProtocolConformanceDescriptor = 0xc6eb;

  /// Pointer to value witness table stored in type metadata.
  ///
  /// Computed with ptrauth_string_discriminator("value_witness_table_t").
  const uint16_t ValueWitnessTable = 0x2e3f;

  /// Extended existential type shapes.
  const uint16_t ExtendedExistentialTypeShape = 0x5a3d; // = 23101
  const uint16_t NonUniqueExtendedExistentialTypeShape = 0xe798; // = 59288

  /// Value witness functions.
  const uint16_t InitializeBufferWithCopyOfBuffer = 0xda4a;
  const uint16_t Destroy = 0x04f8;
  const uint16_t InitializeWithCopy = 0xe3ba;
  const uint16_t AssignWithCopy = 0x8751;
  const uint16_t InitializeWithTake = 0x48d8;
  const uint16_t AssignWithTake = 0xefda;
  const uint16_t DestroyArray = 0x2398;
  const uint16_t InitializeArrayWithCopy = 0xa05c;
  const uint16_t InitializeArrayWithTakeFrontToBack = 0x1c3e;
  const uint16_t InitializeArrayWithTakeBackToFront = 0x8dd3;
  const uint16_t StoreExtraInhabitant = 0x79c5;
  const uint16_t GetExtraInhabitantIndex = 0x2ca8;
  const uint16_t GetEnumTag = 0xa3b5;
  const uint16_t DestructiveProjectEnumData = 0x041d;
  const uint16_t DestructiveInjectEnumTag = 0xb2e4;
  const uint16_t GetEnumTagSinglePayload = 0x60f0;
  const uint16_t StoreEnumTagSinglePayload = 0xa0d1;

  /// KeyPath metadata functions.
  const uint16_t KeyPathDestroy = _SwiftKeyPath_ptrauth_ArgumentDestroy;
  const uint16_t KeyPathCopy = _SwiftKeyPath_ptrauth_ArgumentCopy;
  const uint16_t KeyPathEquals = _SwiftKeyPath_ptrauth_ArgumentEquals;
  const uint16_t KeyPathHash = _SwiftKeyPath_ptrauth_ArgumentHash;
  const uint16_t KeyPathGetter = _SwiftKeyPath_ptrauth_Getter;
  const uint16_t KeyPathNonmutatingSetter = _SwiftKeyPath_ptrauth_NonmutatingSetter;
  const uint16_t KeyPathMutatingSetter = _SwiftKeyPath_ptrauth_MutatingSetter;
  const uint16_t KeyPathGetLayout = _SwiftKeyPath_ptrauth_ArgumentLayout;
  const uint16_t KeyPathInitializer = _SwiftKeyPath_ptrauth_ArgumentInit;
  const uint16_t KeyPathMetadataAccessor = _SwiftKeyPath_ptrauth_MetadataAccessor;

  /// ObjC bridging entry points.
  const uint16_t ObjectiveCTypeDiscriminator = 0x31c3; // = 12739
  const uint16_t bridgeToObjectiveCDiscriminator = 0xbca0; // = 48288
  const uint16_t forceBridgeFromObjectiveCDiscriminator = 0x22fb; // = 8955
  const uint16_t conditionallyBridgeFromObjectiveCDiscriminator = 0x9a9b; // = 39579

  /// Dynamic replacement pointers.
  const uint16_t DynamicReplacementScope = 0x48F0; // = 18672
  const uint16_t DynamicReplacementKey = 0x2C7D; // = 11389

  /// Resume functions for yield-once coroutines that yield a single
  /// opaque borrowed/inout value.  These aren't actually hard-coded, but
  /// they're important enough to be worth writing in one place.
  const uint16_t OpaqueReadResumeFunction = 56769;
  const uint16_t OpaqueModifyResumeFunction = 3909;

  /// ObjC class pointers.
  const uint16_t ObjCISA = 0x6AE1;
  const uint16_t ObjCSuperclass = 0xB5AB;

  /// Resilient class stub initializer callback
  const uint16_t ResilientClassStubInitCallback = 0xC671;

  /// Jobs, tasks, and continuations.
  const uint16_t JobInvokeFunction = 0xcc64; // = 52324
  const uint16_t TaskResumeFunction = 0x2c42; // = 11330
  const uint16_t TaskResumeContext = 0x753a; // = 30010
  const uint16_t AsyncRunAndBlockFunction = 0x0f08; // 3848
  const uint16_t AsyncContextParent = 0xbda2; // = 48546
  const uint16_t AsyncContextResume = 0xd707; // = 55047
  const uint16_t AsyncContextYield = 0xe207; // = 57863
  const uint16_t CancellationNotificationFunction = 0x1933; // = 6451
  const uint16_t EscalationNotificationFunction = 0x5be4; // = 23524
  const uint16_t AsyncThinNullaryFunction = 0x0f08; // = 3848
  const uint16_t AsyncFutureFunction = 0x720f; // = 29199

  /// Swift async context parameter stored in the extended frame info.
  const uint16_t SwiftAsyncContextExtendedFrameEntry = 0xc31a; // = 49946

  // C type TaskContinuationFunction* descriminator.
  const uint16_t ClangTypeTaskContinuationFunction = 0x2abe; // = 10942

  /// Dispatch integration.
  const uint16_t DispatchInvokeFunction = 0xf493; // = 62611

  /// Functions accessible at runtime (i.e. distributed method accessors).
  const uint16_t AccessibleFunctionRecord = 0x438c; // = 17292

  /// C type GetExtraInhabitantTag function descriminator
  const uint16_t GetExtraInhabitantTagFunction = 0x392e; // = 14638

  /// C type StoreExtraInhabitantTag function descriminator
  const uint16_t StoreExtraInhabitantTagFunction = 0x9bf6; // = 39926

  // Relative protocol witness table descriminator
  const uint16_t RelativeProtocolWitnessTable = 0xb830; // = 47152

  const uint16_t TypeLayoutString = 0x8b65; // = 35685
}

/// The number of arguments that will be passed directly to a generic
/// nominal type access function. The remaining arguments (if any) will be
/// passed as an array. That array has enough storage for all of the arguments,
/// but only fills in the elements not passed directly. The callee may
/// mutate the array to fill in the direct arguments.
constexpr unsigned NumDirectGenericTypeMetadataAccessFunctionArgs = 3;

/// The offset (in pointers) to the first requirement in a witness table.
constexpr unsigned WitnessTableFirstRequirementOffset = 1;

/// Kinds of context descriptor.
enum class ContextDescriptorKind : uint8_t {
  /// This context descriptor represents a module.
  Module = 0,

  /// This context descriptor represents an extension.
  Extension = 1,

  /// This context descriptor represents an anonymous possibly-generic context
  /// such as a function body.
  Anonymous = 2,

  /// This context descriptor represents a protocol context.
  Protocol = 3,

  /// This context descriptor represents an opaque type alias.
  OpaqueType = 4,

  /// First kind that represents a type of any sort.
  Type_First = 16,

  /// This context descriptor represents a class.
  Class = Type_First,

  /// This context descriptor represents a struct.
  Struct = Type_First + 1,

  /// This context descriptor represents an enum.
  Enum = Type_First + 2,

  /// Last kind that represents a type of any sort.
  Type_Last = 31,
};

/// Common flags stored in the first 32-bit word of any context descriptor.
struct ContextDescriptorFlags {
private:
  uint32_t Value;

  explicit constexpr ContextDescriptorFlags(uint32_t Value)
    : Value(Value) {}
public:
  constexpr ContextDescriptorFlags() : Value(0) {}
  constexpr ContextDescriptorFlags(ContextDescriptorKind kind,
                                   bool isGeneric,
                                   bool isUnique,
                                   uint8_t version,
                                   uint16_t kindSpecificFlags)
    : ContextDescriptorFlags(ContextDescriptorFlags()
                               .withKind(kind)
                               .withGeneric(isGeneric)
                               .withUnique(isUnique)
                               .withVersion(version)
                               .withKindSpecificFlags(kindSpecificFlags))
  {}

  /// The kind of context this descriptor describes.
  constexpr ContextDescriptorKind getKind() const {
    return ContextDescriptorKind(Value & 0x1Fu);
  }

  /// Whether the context being described is generic.
  constexpr bool isGeneric() const {
    return (Value & 0x80u) != 0;
  }

  /// Whether this is a unique record describing the referenced context.
  constexpr bool isUnique() const {
    return (Value & 0x40u) != 0;
  }

  /// The format version of the descriptor. Higher version numbers may have
  /// additional fields that aren't present in older versions.
  constexpr uint8_t getVersion() const {
    return (Value >> 8u) & 0xFFu;
  }

  /// The most significant two bytes of the flags word, which can have
  /// kind-specific meaning.
  constexpr uint16_t getKindSpecificFlags() const {
    return (Value >> 16u) & 0xFFFFu;
  }

  constexpr ContextDescriptorFlags withKind(ContextDescriptorKind kind) const {
    return assert((uint8_t(kind) & 0x1F) == uint8_t(kind)),
      ContextDescriptorFlags((Value & 0xFFFFFFE0u) | uint8_t(kind));
  }

  constexpr ContextDescriptorFlags withGeneric(bool isGeneric) const {
    return ContextDescriptorFlags((Value & 0xFFFFFF7Fu)
                                  | (isGeneric ? 0x80u : 0));
  }

  constexpr ContextDescriptorFlags withUnique(bool isUnique) const {
    return ContextDescriptorFlags((Value & 0xFFFFFFBFu)
                                  | (isUnique ? 0x40u : 0));
  }

  constexpr ContextDescriptorFlags withVersion(uint8_t version) const {
    return ContextDescriptorFlags((Value & 0xFFFF00FFu) | (version << 8u));
  }

  constexpr ContextDescriptorFlags
  withKindSpecificFlags(uint16_t flags) const {
    return ContextDescriptorFlags((Value & 0xFFFFu) | (flags << 16u));
  }

  constexpr uint32_t getIntValue() const {
    return Value;
  }
};

/// Flags for nominal type context descriptors. These values are used as the
/// kindSpecificFlags of the ContextDescriptorFlags for the type.
class TypeContextDescriptorFlags : public FlagSet<uint16_t> {
  enum {
    // All of these values are bit offsets or widths.
    // Generic flags build upwards from 0.
    // Type-specific flags build downwards from 15.

    /// Whether there's something unusual about how the metadata is
    /// initialized.
    ///
    /// Meaningful for all type-descriptor kinds.
    MetadataInitialization = 0,
    MetadataInitialization_width = 2,

    /// Set if the type has extended import information.
    ///
    /// If true, a sequence of strings follow the null terminator in the
    /// descriptor, terminated by an empty string (i.e. by two null
    /// terminators in a row).  See TypeImportInfo for the details of
    /// these strings and the order in which they appear.
    ///
    /// Meaningful for all type-descriptor kinds.
    HasImportInfo = 2,

    /// Set if the type descriptor has a pointer to a list of canonical
    /// prespecializations.
    HasCanonicalMetadataPrespecializations = 3,

    /// Set if the metadata contains a pointer to a layout string
    HasLayoutString = 4,

    // Type-specific flags:

    /// Set if the class is an actor.
    ///
    /// Only meaningful for class descriptors.
    Class_IsActor = 7,

    /// Set if the class is a default actor class.  Note that this is
    /// based on the best knowledge available to the class; actor
    /// classes with resilient superclassess might be default actors
    /// without knowing it.
    ///
    /// Only meaningful for class descriptors.
    Class_IsDefaultActor = 8,

    /// The kind of reference that this class makes to its resilient superclass
    /// descriptor.  A TypeReferenceKind.
    ///
    /// Only meaningful for class descriptors.
    Class_ResilientSuperclassReferenceKind = 9,
    Class_ResilientSuperclassReferenceKind_width = 3,

    /// Whether the immediate class members in this metadata are allocated
    /// at negative offsets.  For now, we don't use this.
    Class_AreImmediateMembersNegative = 12,

    /// Set if the context descriptor is for a class with resilient ancestry.
    ///
    /// Only meaningful for class descriptors.
    Class_HasResilientSuperclass = 13,

    /// Set if the context descriptor includes metadata for dynamically
    /// installing method overrides at metadata instantiation time.
    Class_HasOverrideTable = 14,

    /// Set if the context descriptor includes metadata for dynamically
    /// constructing a class's vtables at metadata instantiation time.
    ///
    /// Only meaningful for class descriptors.
    Class_HasVTable = 15,
  };

public:
  explicit TypeContextDescriptorFlags(uint16_t bits) : FlagSet(bits) {}
  constexpr TypeContextDescriptorFlags() {}

  enum MetadataInitializationKind {
    /// There are either no special rules for initializing the metadata
    /// or the metadata is generic.  (Genericity is set in the
    /// non-kind-specific descriptor flags.)
    NoMetadataInitialization = 0,

    /// The type requires non-trivial singleton initialization using the
    /// "in-place" code pattern.
    SingletonMetadataInitialization = 1,

    /// The type requires non-trivial singleton initialization using the
    /// "foreign" code pattern.
    ForeignMetadataInitialization = 2,

    // We only have two bits here, so if you add a third special kind,
    // include more flag bits in its out-of-line storage.
  };

  FLAGSET_DEFINE_FIELD_ACCESSORS(MetadataInitialization,
                                 MetadataInitialization_width,
                                 MetadataInitializationKind,
                                 getMetadataInitialization,
                                 setMetadataInitialization)

  bool hasSingletonMetadataInitialization() const {
    return getMetadataInitialization() == SingletonMetadataInitialization;
  }

  bool hasForeignMetadataInitialization() const {
    return getMetadataInitialization() == ForeignMetadataInitialization;
  }

  FLAGSET_DEFINE_FLAG_ACCESSORS(HasImportInfo, hasImportInfo, setHasImportInfo)

  FLAGSET_DEFINE_FLAG_ACCESSORS(HasCanonicalMetadataPrespecializations, hasCanonicalMetadataPrespecializations, setHasCanonicalMetadataPrespecializations)

  FLAGSET_DEFINE_FLAG_ACCESSORS(HasLayoutString,
                                hasLayoutString,
                                setHasLayoutString)

  FLAGSET_DEFINE_FLAG_ACCESSORS(Class_HasVTable,
                                class_hasVTable,
                                class_setHasVTable)
  FLAGSET_DEFINE_FLAG_ACCESSORS(Class_HasOverrideTable,
                                class_hasOverrideTable,
                                class_setHasOverrideTable)
  FLAGSET_DEFINE_FLAG_ACCESSORS(Class_HasResilientSuperclass,
                                class_hasResilientSuperclass,
                                class_setHasResilientSuperclass)
  FLAGSET_DEFINE_FLAG_ACCESSORS(Class_AreImmediateMembersNegative,
                                class_areImmediateMembersNegative,
                                class_setAreImmediateMembersNegative)
  FLAGSET_DEFINE_FLAG_ACCESSORS(Class_IsDefaultActor,
                                class_isDefaultActor,
                                class_setIsDefaultActor)
  FLAGSET_DEFINE_FLAG_ACCESSORS(Class_IsActor,
                                class_isActor,
                                class_setIsActor)

  FLAGSET_DEFINE_FIELD_ACCESSORS(Class_ResilientSuperclassReferenceKind,
                                 Class_ResilientSuperclassReferenceKind_width,
                                 TypeReferenceKind,
                                 class_getResilientSuperclassReferenceKind,
                                 class_setResilientSuperclassReferenceKind)
};

/// Extra flags for resilient classes, since we need more than 16 bits of
/// flags there.
class ExtraClassDescriptorFlags : public FlagSet<uint32_t> {
  enum {
    /// Set if the context descriptor includes a pointer to an Objective-C
    /// resilient class stub structure. See the description of
    /// TargetObjCResilientClassStubInfo in Metadata.h for details.
    ///
    /// Only meaningful for class descriptors when Objective-C interop is
    /// enabled.
    HasObjCResilientClassStub = 0,
  };

public:
  explicit ExtraClassDescriptorFlags(uint32_t bits) : FlagSet(bits) {}
  constexpr ExtraClassDescriptorFlags() {}

  FLAGSET_DEFINE_FLAG_ACCESSORS(HasObjCResilientClassStub,
                                hasObjCResilientClassStub,
                                setObjCResilientClassStub)
};

/// Flags for protocol context descriptors. These values are used as the
/// kindSpecificFlags of the ContextDescriptorFlags for the protocol.
class ProtocolContextDescriptorFlags : public FlagSet<uint16_t> {
  enum {
    /// Whether this protocol is class-constrained.
    HasClassConstraint = 0,
    HasClassConstraint_width = 1,

    /// Whether this protocol is resilient.
    IsResilient = 1,

    /// Special protocol value.
    SpecialProtocolKind = 2,
    SpecialProtocolKind_width = 6,
  };

public:
  explicit ProtocolContextDescriptorFlags(uint16_t bits) : FlagSet(bits) {}
  constexpr ProtocolContextDescriptorFlags() {}

  FLAGSET_DEFINE_FLAG_ACCESSORS(IsResilient, isResilient, setIsResilient)

  FLAGSET_DEFINE_FIELD_ACCESSORS(HasClassConstraint,
                                 HasClassConstraint_width,
                                 ProtocolClassConstraint,
                                 getClassConstraint,
                                 setClassConstraint)

  FLAGSET_DEFINE_FIELD_ACCESSORS(SpecialProtocolKind,
                                 SpecialProtocolKind_width,
                                 SpecialProtocol,
                                 getSpecialProtocol,
                                 setSpecialProtocol)
};

/// Flags for anonymous type context descriptors. These values are used as the
/// kindSpecificFlags of the ContextDescriptorFlags for the anonymous context.
class AnonymousContextDescriptorFlags : public FlagSet<uint16_t> {
  enum {
    /// Whether this anonymous context descriptor is followed by its
    /// mangled name, which can be used to match the descriptor at runtime.
    HasMangledName = 0,
  };

public:
  explicit AnonymousContextDescriptorFlags(uint16_t bits) : FlagSet(bits) {}
  constexpr AnonymousContextDescriptorFlags() {}

  FLAGSET_DEFINE_FLAG_ACCESSORS(HasMangledName, hasMangledName,
                                setHasMangledName)
};

class GenericContextDescriptorFlags {
  uint16_t Value;

public:
  constexpr GenericContextDescriptorFlags() : Value(0) {}

  explicit constexpr GenericContextDescriptorFlags(uint16_t value)
    : Value(value) {}

  constexpr GenericContextDescriptorFlags(bool hasTypePacks)
    : GenericContextDescriptorFlags(
        GenericContextDescriptorFlags((uint16_t)0)
          .withHasTypePacks(hasTypePacks)) {}

  /// Whether this generic context has at least one type parameter
  /// pack, in which case the generic context will have a trailing
  /// GenericPackShapeHeader.
  constexpr bool hasTypePacks() const {
    return (Value & 0x1) != 0;
  }

  constexpr GenericContextDescriptorFlags
  withHasTypePacks(bool hasTypePacks) const {
    return GenericContextDescriptorFlags((uint16_t)(
      (Value & ~0x1) | (hasTypePacks ? 0x1 : 0)));
  }

  constexpr uint16_t getIntValue() const {
    return Value;
  }
};

enum class GenericParamKind : uint8_t {
  /// A type parameter.
  Type = 0,

  /// A type parameter pack.
  TypePack = 1,

  Max = 0x3F,
};

class GenericParamDescriptor {
  /// Don't set 0x40 for compatibility with pre-Swift 5.8 runtimes
  uint8_t Value;

  explicit constexpr GenericParamDescriptor(uint8_t Value)
    : Value(Value) {}
public:
  constexpr GenericParamDescriptor(GenericParamKind kind,
                                   bool hasKeyArgument)
    : GenericParamDescriptor(GenericParamDescriptor(0)
                         .withKind(kind)
                         .withKeyArgument(hasKeyArgument))
  {}

  constexpr bool hasKeyArgument() const {
    return (Value & 0x80u) != 0;
  }

  constexpr GenericParamKind getKind() const {
    return GenericParamKind(Value & 0x3Fu);
  }

  constexpr GenericParamDescriptor
  withKeyArgument(bool hasKeyArgument) const {
    return GenericParamDescriptor((Value & 0x7Fu)
      | (hasKeyArgument ? 0x80u : 0));
  }

  constexpr GenericParamDescriptor withKind(GenericParamKind kind) const {
    return assert((uint8_t(kind) & 0x3Fu) == uint8_t(kind)),
      GenericParamDescriptor((Value & 0xC0u) | uint8_t(kind));
  }

  constexpr uint8_t getIntValue() const {
    return Value;
  }

  friend bool operator==(GenericParamDescriptor lhs,
                         GenericParamDescriptor rhs) {
    return lhs.getIntValue() == rhs.getIntValue();
  }
  friend bool operator!=(GenericParamDescriptor lhs,
                         GenericParamDescriptor rhs) {
    return !(lhs == rhs);
  }

  /// The default parameter descriptor for an implicit parameter.
  static constexpr GenericParamDescriptor implicit() {
    return GenericParamDescriptor(GenericParamKind::Type,
                                  /*key argument*/ true);
  }
};

/// Can the given generic parameter array be implicit, for places in
/// the ABI which support that?
inline bool canGenericParamsBeImplicit(
                            llvm::ArrayRef<GenericParamDescriptor> params) {
  // If there are more parameters than the maximum, they cannot be implicit.
  if (params.size() > MaxNumImplicitGenericParamDescriptors)
    return false;

  // If any parameter is not the implicit pattern, they cannot be implicit.
  for (auto param : params)
    if (param != GenericParamDescriptor::implicit())
      return false;

  return true;
}

enum class GenericRequirementKind : uint8_t {
  /// A protocol requirement.
  Protocol = 0,
  /// A same-type requirement.
  SameType = 1,
  /// A base class requirement.
  BaseClass = 2,
  /// A "same-conformance" requirement, implied by a same-type or base-class
  /// constraint that binds a parameter with protocol requirements.
  SameConformance = 3,
  /// A same-shape requirement between generic parameter packs.
  SameShape = 4,
  /// A layout requirement.
  Layout = 0x1F,
};

class GenericRequirementFlags {
  /// Don't set 0x40 for compatibility with pre-Swift 5.8 runtimes
  uint32_t Value;

  explicit constexpr GenericRequirementFlags(uint32_t Value)
    : Value(Value) {}
public:
  constexpr GenericRequirementFlags(GenericRequirementKind kind,
                                    bool hasKeyArgument,
                                    bool isPackRequirement)
    : GenericRequirementFlags(GenericRequirementFlags(0)
                         .withKind(kind)
                         .withKeyArgument(hasKeyArgument)
                         .withPackRequirement(isPackRequirement))
  {}

  constexpr bool hasKeyArgument() const {
    return (Value & 0x80u) != 0;
  }

  /// If this is true, the subject type of the requirement is a pack.
  /// When the requirement is a conformance requirement, the corresponding
  /// entry in the generic arguments array becomes a TargetWitnessTablePack.
  constexpr bool isPackRequirement() const {
    return (Value & 0x20u) != 0;
  }

  constexpr GenericRequirementKind getKind() const {
    return GenericRequirementKind(Value & 0x1Fu);
  }

  constexpr GenericRequirementFlags
  withKeyArgument(bool hasKeyArgument) const {
    return GenericRequirementFlags((Value & 0x7Fu)
      | (hasKeyArgument ? 0x80u : 0));
  }

  constexpr GenericRequirementFlags
  withPackRequirement(bool isPackRequirement) const {
    return GenericRequirementFlags((Value & 0xBFu)
      | (isPackRequirement ? 0x20u : 0));
  }

  constexpr GenericRequirementFlags
  withKind(GenericRequirementKind kind) const {
    return assert((uint8_t(kind) & 0x1Fu) == uint8_t(kind)),
      GenericRequirementFlags((Value & 0xE0u) | uint8_t(kind));
  }

  constexpr uint32_t getIntValue() const {
    return Value;
  }
};

enum class GenericRequirementLayoutKind : uint32_t {
  // A class constraint.
  Class = 0,
};

enum class GenericPackKind : uint16_t {
  Metadata = 0,
  WitnessTable = 1
};

class GenericEnvironmentFlags {
  uint32_t Value;

  enum : uint32_t {
    NumGenericParameterLevelsMask = 0xFFF,
    NumGenericRequirementsShift = 12,
    NumGenericRequirementsMask = 0xFFFF << NumGenericRequirementsShift,
  };

  constexpr explicit GenericEnvironmentFlags(uint32_t value) : Value(value) { }

public:
  constexpr GenericEnvironmentFlags() : Value(0) { }

  constexpr GenericEnvironmentFlags
  withNumGenericParameterLevels(uint16_t numGenericParameterLevels) const {
    return GenericEnvironmentFlags((Value &~ NumGenericParameterLevelsMask)
                                   | numGenericParameterLevels);
  }

  constexpr GenericEnvironmentFlags
  withNumGenericRequirements(uint16_t numGenericRequirements) const {
    return GenericEnvironmentFlags((Value &~ NumGenericRequirementsMask)
             | (numGenericRequirements << NumGenericRequirementsShift));
  }

  constexpr unsigned getNumGenericParameterLevels() const {
    return Value & NumGenericParameterLevelsMask;
  }

  constexpr unsigned getNumGenericRequirements() const {
    return (Value & NumGenericRequirementsMask) >> NumGenericRequirementsShift;
  }

  constexpr uint32_t getIntValue() const {
    return Value;
  }
};

/// Flags used by generic metadata patterns.
class GenericMetadataPatternFlags : public FlagSet<uint32_t> {
  enum {
    // All of these values are bit offsets or widths.
    // General flags build up from 0.
    // Kind-specific flags build down from 31.

    /// Does this pattern have an extra-data pattern?
    HasExtraDataPattern = 0,

    /// Do instances of this pattern have a bitset of flags that occur at the
    /// end of the metadata, after the extra data if there is any?
    HasTrailingFlags = 1,

    // Class-specific flags.

    /// Does this pattern have an immediate-members pattern?
    Class_HasImmediateMembersPattern = 31,

    // Value-specific flags.

    /// For value metadata: the metadata kind of the type.
    Value_MetadataKind = 21,
    Value_MetadataKind_width = 11,
  };

public:
  explicit GenericMetadataPatternFlags(uint32_t bits) : FlagSet(bits) {}
  constexpr GenericMetadataPatternFlags() {}

  FLAGSET_DEFINE_FLAG_ACCESSORS(Class_HasImmediateMembersPattern,
                                class_hasImmediateMembersPattern,
                                class_setHasImmediateMembersPattern)

  FLAGSET_DEFINE_FLAG_ACCESSORS(HasExtraDataPattern,
                                hasExtraDataPattern,
                                setHasExtraDataPattern)

  FLAGSET_DEFINE_FLAG_ACCESSORS(HasTrailingFlags,
                                hasTrailingFlags,
                                setHasTrailingFlags)

  FLAGSET_DEFINE_FIELD_ACCESSORS(Value_MetadataKind,
                                 Value_MetadataKind_width,
                                 MetadataKind,
                                 value_getMetadataKind,
                                 value_setMetadataKind)
};

/// The public state of a metadata.
enum class MetadataState : size_t {
  // The values of this enum are set up to give us some future flexibility
  // in adding states.  The compiler emits unsigned comparisons against
  // these values, so adding states that aren't totally ordered with at
  // least the existing values will pose a problem; but we also use a
  // gradually-shrinking bitset in case it's useful to track states as
  // separate capabilities.  Specific values have been chosen so that a
  // MetadataRequest of 0 represents a blocking complete request, which
  // is the most likely request from ordinary code.  The total size of a
  // state is kept to 8 bits so that a full request, even with additional
  // flags, can be materialized as a single immediate on common ISAs, and
  // so that the state can be extracted with a byte truncation.
  // The spacing between states reflects guesswork about where new
  // states/capabilities are most likely to be added.

  /// The metadata is fully complete.  By definition, this is the
  /// end-state of all metadata.  Generally, metadata is expected to be
  /// complete before it can be passed to arbitrary code, e.g. as
  /// a generic argument to a function or as a metatype value.
  ///
  /// In addition to the requirements of NonTransitiveComplete, certain
  /// transitive completeness guarantees must hold.  Most importantly,
  /// complete nominal type metadata transitively guarantee the completion
  /// of their stored generic type arguments and superclass metadata.
  Complete = 0x00,

  /// The metadata is fully complete except for any transitive completeness
  /// guarantees.
  ///
  /// In addition to the requirements of LayoutComplete, metadata in this
  /// state must be prepared for all basic type operations.  This includes:
  ///
  ///   - any sort of internal layout necessary to allocate and work
  ///     with concrete values of the type, such as the instance layout
  ///     of a class;
  ///
  ///   - any sort of external dynamic registration that might be required
  ///     for the type, such as the realization of a class by the Objective-C
  ///     runtime; and
  ///
  ///   - the initialization of any other information kept in the metadata
  ///     object, such as a class's v-table.
  NonTransitiveComplete = 0x01,

  /// The metadata is ready for the layout of other types that store values
  /// of this type.
  ///
  /// In addition to the requirements of Abstract, metadata in this state
  /// must have a valid value witness table, meaning that its size,
  /// alignment, and basic type properties (such as POD-ness) have been
  /// computed.
  LayoutComplete = 0x3F,

  /// The metadata has its basic identity established.  It is possible to
  /// determine what formal type it corresponds to.  Among other things, it
  /// is possible to use the runtime mangling facilities with the type.
  ///
  /// For example, a metadata for a generic struct has a metadata kind,
  /// a type descriptor, and all of its type arguments.  However, it does not
  /// necessarily have a meaningful value-witness table.
  ///
  /// References to other types that are not part of the type's basic identity
  /// may not yet have been established.  Most crucially, this includes the
  /// superclass pointer.
  Abstract = 0xFF,
};

/// Something that can be static_asserted in all the places where we do
/// comparisons on metadata states.
constexpr const bool MetadataStateIsReverseOrdered = true;

/// Return true if the first metadata state is at least as advanced as the
/// second.
inline bool isAtLeast(MetadataState lhs, MetadataState rhs) {
  static_assert(MetadataStateIsReverseOrdered,
                "relying on the ordering of MetadataState here");
  return size_t(lhs) <= size_t(rhs);
}

/// Kinds of requests for metadata.
class MetadataRequest : public FlagSet<size_t> {
  using IntType = size_t;
  using super = FlagSet<IntType>;

public:
  enum : IntType {
    State_bit = 0,
    State_width = 8,

    /// A blocking request will not return until the runtime is able to produce
    /// metadata with the given kind.  A non-blocking request will return
    /// "immediately", producing an abstract metadata and a flag saying that
    /// the operation failed.
    ///
    /// An abstract request will never be non-zero.
    NonBlocking_bit = 8,
  };

  MetadataRequest(MetadataState state, bool isNonBlocking = false) {
    setState(state);
    setIsNonBlocking(isNonBlocking);
  }
  explicit MetadataRequest(IntType bits) : super(bits) {}
  constexpr MetadataRequest() {}

  FLAGSET_DEFINE_EQUALITY(MetadataRequest)

  FLAGSET_DEFINE_FIELD_ACCESSORS(State_bit,
                                 State_width,
                                 MetadataState,
                                 getState,
                                 setState)

  FLAGSET_DEFINE_FLAG_ACCESSORS(NonBlocking_bit,
                                isNonBlocking,
                                setIsNonBlocking)
  bool isBlocking() const { return !isNonBlocking(); }

  /// Is this request satisfied by a metadata that's in the given state?
  bool isSatisfiedBy(MetadataState state) const {
    return isAtLeast(state, getState());
  }
};

struct MetadataTrailingFlags : public FlagSet<uint64_t> {
  enum {
    /// Whether this metadata is a specialization of a generic metadata pattern
    /// which was created during compilation.
    IsStaticSpecialization = 0,

    /// Whether this metadata is a specialization of a generic metadata pattern
    /// which was created during compilation and made to be canonical by
    /// modifying the metadata accessor.
    IsCanonicalStaticSpecialization = 1,
  };

  explicit MetadataTrailingFlags(uint64_t bits) : FlagSet(bits) {}
  constexpr MetadataTrailingFlags() {}

  FLAGSET_DEFINE_FLAG_ACCESSORS(IsStaticSpecialization,
                                isStaticSpecialization,
                                setIsStaticSpecialization)

  FLAGSET_DEFINE_FLAG_ACCESSORS(IsCanonicalStaticSpecialization,
                                isCanonicalStaticSpecialization,
                                setIsCanonicalStaticSpecialization)
};

/// Flags for Builtin.IntegerLiteral values.
class IntegerLiteralFlags {
public:
  enum : size_t {
    IsNegativeFlag = 0x1,

    // Save some space for other flags.

    BitWidthShift = 8,
  };

private:
  size_t Data;

  explicit IntegerLiteralFlags(size_t data) : Data(data) {}

public:
  constexpr IntegerLiteralFlags(size_t bitWidth, bool isNegative)
    : Data((bitWidth << BitWidthShift)
           | (isNegative ? IsNegativeFlag : 0)) {}

  /// Return true if the value is negative.
  bool isNegative() const { return Data & IsNegativeFlag; }

  /// Return the minimum number of bits necessary to store the value in
  /// two's complement, including a leading sign bit.
  unsigned getBitWidth() const { return Data >> BitWidthShift; }

  size_t getOpaqueValue() const { return Data; }
  static IntegerLiteralFlags getFromOpaqueValue(size_t value) {
    return IntegerLiteralFlags(value);
  }
};

/// Kinds of schedulable job.s
enum class JobKind : size_t {
  // There are 256 possible job kinds.

  /// An AsyncTask.
  Task = 0,

  /// Job kinds >= 192 are private to the implementation.
  First_Reserved = 192,

  DefaultActorInline = First_Reserved,
  DefaultActorSeparate,
  DefaultActorOverride,
  NullaryContinuation
};

/// The priority of a job.  Higher priorities are larger values.
enum class JobPriority : size_t {
  // This is modelled off of Dispatch.QoS, and the values are directly
  // stolen from there.
  UserInteractive = 0x21, /* UI */
  UserInitiated   = 0x19, /* IN */
  Default         = 0x15, /* DEF */
  Utility         = 0x11, /* UT */
  Background      = 0x09, /* BG */
  Unspecified     = 0x00, /* UN */
};

/// A tri-valued comparator which orders higher priorities first.
inline int descendingPriorityOrder(JobPriority lhs,
                                   JobPriority rhs) {
  return (lhs == rhs ? 0 : lhs > rhs ? -1 : 1);
}

inline JobPriority withUserInteractivePriorityDowngrade(JobPriority priority) {
  return (priority == JobPriority::UserInteractive) ? JobPriority::UserInitiated
                                                    : priority;
}

/// Flags for task creation.
class TaskCreateFlags : public FlagSet<size_t> {
public:
  enum {
    // Priority that user specified while creating the task
    RequestedPriority = 0,
    RequestedPriority_width = 8,

    Task_IsChildTask                              = 8,
    // Should only be set in task-to-thread model where Task.runInline is
    // available
    Task_IsInlineTask                             = 9,
    Task_CopyTaskLocals                           = 10,
    Task_InheritContext                           = 11,
    Task_EnqueueJob                               = 12,
    Task_AddPendingGroupTaskUnconditionally       = 13,
    Task_IsDiscardingTask                         = 14,
  };

  explicit constexpr TaskCreateFlags(size_t bits) : FlagSet(bits) {}
  constexpr TaskCreateFlags() {}

  FLAGSET_DEFINE_FIELD_ACCESSORS(RequestedPriority, RequestedPriority_width,
                                 JobPriority, getRequestedPriority,
                                 setRequestedPriority)
  FLAGSET_DEFINE_FLAG_ACCESSORS(Task_IsChildTask,
                                isChildTask,
                                setIsChildTask)
  FLAGSET_DEFINE_FLAG_ACCESSORS(Task_IsInlineTask,
                                isInlineTask,
                                setIsInlineTask)
  FLAGSET_DEFINE_FLAG_ACCESSORS(Task_CopyTaskLocals,
                                copyTaskLocals,
                                setCopyTaskLocals)
  FLAGSET_DEFINE_FLAG_ACCESSORS(Task_InheritContext,
                                inheritContext,
                                setInheritContext)
  FLAGSET_DEFINE_FLAG_ACCESSORS(Task_EnqueueJob,
                                enqueueJob,
                                setEnqueueJob)
  FLAGSET_DEFINE_FLAG_ACCESSORS(Task_AddPendingGroupTaskUnconditionally,
                                addPendingGroupTaskUnconditionally,
                                setAddPendingGroupTaskUnconditionally)
  FLAGSET_DEFINE_FLAG_ACCESSORS(Task_IsDiscardingTask,
                                isDiscardingTask,
                                setIsDiscardingTask)
};

/// Flags for schedulable jobs.
class JobFlags : public FlagSet<uint32_t> {
public:
  enum {
    Kind           = 0,
    Kind_width     = 8,

    Priority       = 8,
    Priority_width = 8,

    // 8 bits reserved for more generic job flags.

    // Kind-specific flags.

    Task_IsChildTask           = 24,
    Task_IsFuture              = 25,
    Task_IsGroupChildTask      = 26,
    // 27 is currently unused
    Task_IsAsyncLetTask        = 28,
  };

  explicit JobFlags(uint32_t bits) : FlagSet(bits) {}
  JobFlags(JobKind kind) { setKind(kind); }
  JobFlags(JobKind kind, JobPriority priority) {
    setKind(kind);
    setPriority(priority);
  }
  constexpr JobFlags() {}

  FLAGSET_DEFINE_FIELD_ACCESSORS(Kind, Kind_width, JobKind,
                                 getKind, setKind)

  FLAGSET_DEFINE_FIELD_ACCESSORS(Priority, Priority_width, JobPriority,
                                 getPriority, setPriority)

  bool isAsyncTask() const {
    return getKind() == JobKind::Task;
  }

  FLAGSET_DEFINE_FLAG_ACCESSORS(Task_IsChildTask,
                                task_isChildTask,
                                task_setIsChildTask)
  FLAGSET_DEFINE_FLAG_ACCESSORS(Task_IsFuture,
                                task_isFuture,
                                task_setIsFuture)
  FLAGSET_DEFINE_FLAG_ACCESSORS(Task_IsGroupChildTask,
                                task_isGroupChildTask,
                                task_setIsGroupChildTask)
  FLAGSET_DEFINE_FLAG_ACCESSORS(Task_IsAsyncLetTask,
                                task_isAsyncLetTask,
                                task_setIsAsyncLetTask)
};

/// Kinds of task status record.
enum class TaskStatusRecordKind : uint8_t {
  /// A TaskDependencyStatusRecord which tracks what the current task is
  /// dependent on.
  TaskDependency = 0,

  /// A ChildTaskStatusRecord, which represents the potential for
  /// active child tasks.
  ChildTask = 1,

  /// A TaskGroupTaskStatusRecord, which represents a task group
  /// and child tasks spawned within it.
  TaskGroup = 2,

  /// A CancellationNotificationStatusRecord, which represents the
  /// need to call a custom function when the task is cancelled.
  CancellationNotification = 3,

  /// An EscalationNotificationStatusRecord, which represents the
  /// need to call a custom function when the task's priority is
  /// escalated.
  EscalationNotification = 4,

  // Kinds >= 192 are private to the implementation.
  First_Reserved = 192,
  Private_RecordLock = 192
};

/// Kinds of option records that can be passed to creating asynchronous tasks.
enum class TaskOptionRecordKind : uint8_t {
  /// Request a task to be kicked off, or resumed, on a specific executor.
  Executor  = 0,
  /// Request a child task to be part of a specific task group.
  TaskGroup = 1,
  /// DEPRECATED. AsyncLetWithBuffer is used instead.
  /// Request a child task for an 'async let'.
  AsyncLet  = 2,
  /// Request a child task for an 'async let'.
  AsyncLetWithBuffer = 3,
  /// Request a child task for swift_task_run_inline.
  RunInline = UINT8_MAX,
};

/// Flags for TaskGroup.
class TaskGroupFlags : public FlagSet<uint32_t> {
public:
  enum {
    // 8 bits are reserved for future use
    /// Request the TaskGroup to immediately release completed tasks,
    /// and not store their results. This also effectively disables `next()`.
    TaskGroup_DiscardResults = 8,
  };

  explicit TaskGroupFlags(uint32_t bits) : FlagSet(bits) {}
  constexpr TaskGroupFlags() {}

  FLAGSET_DEFINE_FLAG_ACCESSORS(TaskGroup_DiscardResults,
                                isDiscardResults,
                                setIsDiscardResults)
};

/// Flags for cancellation records.
class TaskStatusRecordFlags : public FlagSet<size_t> {
public:
  enum {
    Kind           = 0,
    Kind_width     = 8,
  };

  explicit TaskStatusRecordFlags(size_t bits) : FlagSet(bits) {}
  constexpr TaskStatusRecordFlags() {}
  TaskStatusRecordFlags(TaskStatusRecordKind kind) {
    setKind(kind);
  }

  FLAGSET_DEFINE_FIELD_ACCESSORS(Kind, Kind_width, TaskStatusRecordKind,
                                 getKind, setKind)
};

/// Flags for task option records.
class TaskOptionRecordFlags : public FlagSet<size_t> {
public:
  enum {
    Kind           = 0,
    Kind_width     = 8,
  };

  explicit TaskOptionRecordFlags(size_t bits) : FlagSet(bits) {}
  constexpr TaskOptionRecordFlags() {}
  TaskOptionRecordFlags(TaskOptionRecordKind kind) {
    setKind(kind);
  }

  FLAGSET_DEFINE_FIELD_ACCESSORS(Kind, Kind_width, TaskOptionRecordKind,
                                 getKind, setKind)
};

/// Flags passed to swift_continuation_init.
class AsyncContinuationFlags : public FlagSet<size_t> {
public:
  enum {
    CanThrow            = 0,
    HasExecutorOverride = 1,
    IsPreawaited        = 2,
    IsExecutorSwitchForced = 3,
  };

  explicit AsyncContinuationFlags(size_t bits) : FlagSet(bits) {}
  constexpr AsyncContinuationFlags() {}

  /// Whether the continuation is permitted to throw.
  FLAGSET_DEFINE_FLAG_ACCESSORS(CanThrow, canThrow, setCanThrow)

  /// Whether the continuation should be resumed on a different
  /// executor than the current one.  swift_continuation_init
  /// will not initialize ResumeToExecutor if this is set.
  FLAGSET_DEFINE_FLAG_ACCESSORS(HasExecutorOverride,
                                hasExecutorOverride,
                                setHasExecutorOverride)

  /// Whether the switch to the target executor should be forced
  /// by swift_continuation_await.  If this is not set, and
  /// swift_continuation_await finds that the continuation has
  /// already been resumed, then execution will continue on the
  /// current executor.  This has no effect in combination with
  /// pre-awaiting.
  ///
  /// Setting this flag when you know statically that you're
  /// already on the right executor is suboptimal.  In particular,
  /// there's no good reason to set this if you're not also using
  /// an executor override.
  FLAGSET_DEFINE_FLAG_ACCESSORS(IsExecutorSwitchForced,
                                isExecutorSwitchForced,
                                setIsExecutorSwitchForced)

  /// Whether the continuation is "pre-awaited".  If so, it should
  /// be set up in the already-awaited state, and so resumptions
  /// will immediately schedule the continuation to begin
  /// asynchronously.  The continuation must not be subsequently
  /// awaited if this is set.  The task is immediately treated as
  /// suspended.
  FLAGSET_DEFINE_FLAG_ACCESSORS(IsPreawaited,
                                isPreawaited,
                                setIsPreawaited)
};

/// Status values for a continuation.  Note that the "not yet"s in
/// the description below aren't quite right because the system
/// does not actually promise to update the status before scheduling
/// the task.  This is because the continuation context is immediately
/// invalidated once the task starts running again, so the window in
/// which we can usefully protect against (say) double-resumption may
/// be very small.
enum class ContinuationStatus : size_t {
  /// The continuation has not yet been awaited or resumed.
  Pending = 0,

  /// The continuation has already been awaited, but not yet resumed.
  Awaited = 1,

  /// The continuation has already been resumed, but not yet awaited.
  Resumed = 2
};

/// Flags that go in a TargetAccessibleFunction structure.
class AccessibleFunctionFlags : public FlagSet<uint32_t> {
public:
  enum {
    /// Whether this is a "distributed" actor function.
    Distributed = 0,
  };

  explicit AccessibleFunctionFlags(uint32_t bits) : FlagSet(bits) {}
  constexpr AccessibleFunctionFlags() {}

  /// Whether the this is a "distributed" actor function.
  FLAGSET_DEFINE_FLAG_ACCESSORS(Distributed, isDistributed, setDistributed)
};

} // end namespace swift

#endif // SWIFT_ABI_METADATAVALUES_H
