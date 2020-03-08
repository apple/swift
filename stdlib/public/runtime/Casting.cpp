//===--- Casting.cpp - Swift Language Dynamic Casting Support -------------===//
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
// Implementations of the dynamic cast runtime functions.
//
//===----------------------------------------------------------------------===//

#include "swift/Runtime/Casting.h"
#include "../SwiftShims/RuntimeShims.h"
#include "CompatibilityOverride.h"
#include "ErrorObject.h"
#include "ExistentialMetadataImpl.h"
#include "Private.h"
#include "SwiftHashableSupport.h"
#include "swift/Basic/LLVM.h"
#include "swift/Basic/Lazy.h"
#include "swift/Demangling/Demangler.h"
#include "swift/Runtime/Config.h"
#include "swift/Runtime/Debug.h"
#include "swift/Runtime/Enum.h"
#include "swift/Runtime/ExistentialContainer.h"
#include "swift/Runtime/HeapObject.h"
#include "swift/Runtime/Metadata.h"
#if defined(__wasi__)
# define SWIFT_CASTING_SUPPORTS_MUTEX 0
#else
# define SWIFT_CASTING_SUPPORTS_MUTEX 1
# include "swift/Runtime/Mutex.h"
#endif
#include "swift/Runtime/Unreachable.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/PointerIntPair.h"
#include "llvm/Support/Compiler.h"
#if SWIFT_OBJC_INTEROP
#include "swift/Runtime/ObjCBridge.h"
#include "SwiftObject.h"
#include "SwiftValue.h"
#endif

#include <cstddef>
#include <cstring>
#include <type_traits>

#if defined(__GLIBCXX__) && __GLIBCXX__ < 20160726
#include <stddef.h>
namespace std { using ::max_align_t; }
#endif

using namespace swift;
using namespace swift::hashable_support;
using namespace metadataimpl;

#if SWIFT_OBJC_INTEROP
#include <objc/NSObject.h>
#include <objc/runtime.h>
#include <objc/message.h>
#include <objc/objc.h>

// Aliases for Objective-C runtime entry points.
static const char *class_getName(const ClassMetadata* type) {
  return class_getName(
    reinterpret_cast<Class>(const_cast<ClassMetadata*>(type)));
}

// Aliases for Swift runtime entry points for Objective-C types.
extern "C" const void *swift_dynamicCastObjCProtocolConditional(
                         const void *object,
                         size_t numProtocols,
                         Protocol * const *protocols);
#endif

// Build a user-comprehensible name for a type.
static void _buildNameForMetadata(const Metadata *type,
                                  bool qualified,
                                  std::string &result) {
#if SWIFT_OBJC_INTEROP
  if (type->getKind() == MetadataKind::Class) {
    auto classType = static_cast<const ClassMetadata *>(type);
    // Look through artificial subclasses.
    while (classType->isTypeMetadata() && classType->isArtificialSubclass())
      classType = classType->Superclass;

    // Ask the Objective-C runtime to name ObjC classes.
    if (!classType->isTypeMetadata()) {
      result += class_getName(classType);
      return;
    }
  } else if (type->getKind() == MetadataKind::ObjCClassWrapper) {
    auto objcWrapper = static_cast<const ObjCClassWrapperMetadata *>(type);
    const char *className = class_getName(class_const_cast(objcWrapper->Class));
    result = className;
    return;
  }
#endif

  // Use the remangler to generate a mangled name from the type metadata.
  
  Demangle::Demangler Dem;
  auto demangling = _swift_buildDemanglingForMetadata(type, Dem);
  if (demangling == nullptr) {
    result = "<<< invalid type >>>";
    return;
  }

  Demangle::DemangleOptions options;
  options.QualifyEntities = qualified;
  if (!qualified)
    options.ShowPrivateDiscriminators = false;
  result = Demangle::nodeToString(demangling, options);
}

/// Return a user-comprehensible name for the given type.
std::string swift::nameForMetadata(const Metadata *type,
                                   bool qualified) {
  std::string result;
  _buildNameForMetadata(type, qualified, result);
  return result;
}

TypeNamePair
swift::swift_getTypeName(const Metadata *type, bool qualified) {
  using Key = llvm::PointerIntPair<const Metadata *, 1, bool>;

  #if SWIFT_CASTING_SUPPORTS_MUTEX
  static StaticReadWriteLock TypeNameCacheLock;
  #endif
  static Lazy<llvm::DenseMap<Key, std::pair<const char *, size_t>>>
    TypeNameCache;
  
  Key key(type, qualified);
  auto &cache = TypeNameCache.get();

  // Attempt read-only lookup of cache entry.
  {
    #if SWIFT_CASTING_SUPPORTS_MUTEX
    StaticScopedReadLock guard(TypeNameCacheLock);
    #endif

    auto found = cache.find(key);
    if (found != cache.end()) {
      auto result = found->second;
      return TypeNamePair{result.first, result.second};
    }
  }

  // Read-only lookup failed to find item, we may need to create it.
  {
    #if SWIFT_CASTING_SUPPORTS_MUTEX
    StaticScopedWriteLock guard(TypeNameCacheLock);
    #endif

    // Do lookup again just to make sure it wasn't created by another
    // thread before we acquired the write lock.
    auto found = cache.find(key);
    if (found != cache.end()) {
      auto result = found->second;
      return TypeNamePair{result.first, result.second};
    }

    // Build the metadata name.
    auto name = nameForMetadata(type, qualified);
    // Copy it to memory we can reference forever.
    auto size = name.size();
    auto result = (char *)malloc(size + 1);
    memcpy(result, name.data(), size);
    result[size] = 0;

    cache.insert({key, {result, size}});
    return TypeNamePair{result, size};
  }
}

/// Report a dynamic cast failure.
// This is noinline to preserve this frame in stack traces.
// We want "dynamicCastFailure" to appear in crash logs even we crash 
// during the diagnostic because some Metadata is invalid.
LLVM_ATTRIBUTE_NORETURN
LLVM_ATTRIBUTE_NOINLINE
void 
swift::swift_dynamicCastFailure(const void *sourceType, const char *sourceName, 
                                const void *targetType, const char *targetName, 
                                const char *message) {
  swift::fatalError(/* flags = */ 0,
                    "Could not cast value of type '%s' (%p) to '%s' (%p)%s%s\n",
                    sourceName, sourceType, 
                    targetName, targetType, 
                    message ? ": " : ".", 
                    message ? message : "");
}

LLVM_ATTRIBUTE_NORETURN
void 
swift::swift_dynamicCastFailure(const Metadata *sourceType,
                                const Metadata *targetType, 
                                const char *message) {
  std::string sourceName = nameForMetadata(sourceType);
  std::string targetName = nameForMetadata(targetType);

  swift_dynamicCastFailure(sourceType, sourceName.c_str(), 
                           targetType, targetName.c_str(), message);
}

// Objective-C bridging helpers.
namespace {
  struct _ObjectiveCBridgeableWitnessTable;
}
static const _ObjectiveCBridgeableWitnessTable *
findBridgeWitness(const Metadata *T);

static bool _dynamicCastValueToClassViaObjCBridgeable(
              OpaqueValue *dest,
              OpaqueValue *src,
              const Metadata *srcType,
              const Metadata *targetType,
              const _ObjectiveCBridgeableWitnessTable *srcBridgeWitness,
              DynamicCastFlags flags);

static bool _dynamicCastValueToClassExistentialViaObjCBridgeable(
              OpaqueValue *dest,
              OpaqueValue *src,
              const Metadata *srcType,
              const ExistentialTypeMetadata *targetType,
              const _ObjectiveCBridgeableWitnessTable *srcBridgeWitness,
              DynamicCastFlags flags);

static bool _dynamicCastClassToValueViaObjCBridgeable(
               OpaqueValue *dest,
               OpaqueValue *src,
               const Metadata *srcType,
               const Metadata *targetType,
               const _ObjectiveCBridgeableWitnessTable *targetBridgeWitness,
               DynamicCastFlags flags);

/// A convenient method for failing out of a dynamic cast.
static bool _fail(OpaqueValue *srcValue, const Metadata *srcType,
                  const Metadata *targetType, DynamicCastFlags flags,
                  const Metadata *srcDynamicType = nullptr) {
  if (flags & DynamicCastFlags::Unconditional) {
    const Metadata *srcTypeToReport =
        srcDynamicType ? srcDynamicType
                       : srcType;
    swift_dynamicCastFailure(srcTypeToReport, targetType);
  }
  if (flags & DynamicCastFlags::DestroyOnFailure)
    srcType->vw_destroy(srcValue);
  return false;
}

/// A convenient method for succeeding at a dynamic cast.
static bool _succeed(OpaqueValue *dest, OpaqueValue *src,
                     const Metadata *srcType, DynamicCastFlags flags) {
  if (flags & DynamicCastFlags::TakeOnSuccess) {
    srcType->vw_initializeWithTake(dest, src);
  } else {
    srcType->vw_initializeWithCopy(dest, src);
  }
  return true;
}

/// Dynamically cast a class metatype to a Swift class metatype.
static const ClassMetadata *
_dynamicCastClassMetatype(const ClassMetadata *sourceType,
                          const ClassMetadata *targetType) {
  do {
    if (sourceType == targetType) {
      return sourceType;
    }
    sourceType = sourceType->Superclass;
  } while (sourceType);
  
  return nullptr;
}

/// Dynamically cast a class instance to a Swift class type.
static const void *swift_dynamicCastClassImpl(const void *object,
                                              const ClassMetadata *targetType) {
#if SWIFT_OBJC_INTEROP
  assert(!targetType->isPureObjC());

  // Swift native classes never have a tagged-pointer representation.
  if (isObjCTaggedPointerOrNull(object)) {
    return nullptr;
  }
#endif

  auto isa = _swift_getClassOfAllocated(object);

  if (_dynamicCastClassMetatype(isa, targetType))
    return object;
  return nullptr;
}

/// Dynamically cast a class object to a Swift class type.
static const void *
swift_dynamicCastClassUnconditionalImpl(const void *object,
                                        const ClassMetadata *targetType,
                                        const char *file, unsigned line, unsigned column) {
  auto value = swift_dynamicCastClass(object, targetType);
  if (value) return value;

  swift_dynamicCastFailure(_swift_getClass(object), targetType);
}

#if SWIFT_OBJC_INTEROP
static bool _unknownClassConformsToObjCProtocol(const OpaqueValue *value,
                                                Protocol *protocol) {
  const void *object
    = *reinterpret_cast<const void * const *>(value);
  return swift_dynamicCastObjCProtocolConditional(object, 1, &protocol);
}
#endif

bool swift::_conformsToProtocol(const OpaqueValue *value,
                                const Metadata *type,
                                ProtocolDescriptorRef protocol,
                                const WitnessTable **conformance) {
  // Look up the witness table for protocols that need them.
  if (protocol.needsWitnessTable()) {
    auto witness = swift_conformsToProtocol(type, protocol.getSwiftProtocol());
    if (!witness)
      return false;
    if (conformance)
      *conformance = witness;
    return true;
  }

  // For Objective-C protocols, check whether we have a class that
  // conforms to the given protocol.
  switch (type->getKind()) {
  case MetadataKind::Class:
#if SWIFT_OBJC_INTEROP
    if (value) {
      return _unknownClassConformsToObjCProtocol(value,
                                                 protocol.getObjCProtocol());
    } else {
      return classConformsToObjCProtocol(type, protocol);
    }
#endif
    return false;

  case MetadataKind::ObjCClassWrapper: {
#if SWIFT_OBJC_INTEROP
    if (value) {
      return _unknownClassConformsToObjCProtocol(value,
                                                 protocol.getObjCProtocol());
    } else {
      auto wrapper = cast<ObjCClassWrapperMetadata>(type);
      return classConformsToObjCProtocol(wrapper->Class, protocol);
    }
#endif
    return false;
  }

  case MetadataKind::ForeignClass:
#if SWIFT_OBJC_INTEROP
    if (value)
      return _unknownClassConformsToObjCProtocol(value,
                                                 protocol.getObjCProtocol());
    return false;
#else
   return false;
#endif

  
  case MetadataKind::Existential: {
#if SWIFT_OBJC_INTEROP
    // If all protocols are @objc and at least one of them conforms to the
    // protocol, succeed.
    auto existential = cast<ExistentialTypeMetadata>(type);
    if (!existential->isObjC())
      return false;
    for (auto existentialProto : existential->getProtocols()) {
      if (protocol_conformsToProtocol(existentialProto.getObjCProtocol(),
                                      protocol.getObjCProtocol()))
        return true;
    }
#endif

    return false;
  }

  case MetadataKind::ExistentialMetatype:
  default:
    return false;
  }

  return false;
}

/// Check whether a type conforms to the given protocols, filling in a
/// list of conformances.
static bool _conformsToProtocols(const OpaqueValue *value,
                                 const Metadata *type,
                                 const ExistentialTypeMetadata *existentialType,
                                 const WitnessTable **conformances) {
  if (auto *superclass = existentialType->getSuperclassConstraint()) {
    if (!swift_dynamicCastMetatype(type, superclass))
      return false;
  }

  if (existentialType->isClassBounded()) {
    if (!Metadata::isAnyKindOfClass(type->getKind()))
      return false;
  }

  for (auto protocol : existentialType->getProtocols()) {
    if (!_conformsToProtocol(value, type, protocol, conformances))
      return false;
    if (conformances != nullptr && protocol.needsWitnessTable()) {
      assert(*conformances != nullptr);
      ++conformances;
    }
  }
  
  return true;
}

static bool shouldDeallocateSource(bool castSucceeded, DynamicCastFlags flags) {
  return (castSucceeded && (flags & DynamicCastFlags::TakeOnSuccess)) ||
        (!castSucceeded && (flags & DynamicCastFlags::DestroyOnFailure));
}

static bool
isAnyObjectExistentialType(const ExistentialTypeMetadata *targetType) {
  unsigned numProtos =  targetType->NumProtocols;
  return (numProtos == 0 &&
          targetType->getSuperclassConstraint() == nullptr &&
          targetType->isClassBounded());
}

/// Given a possibly-existential value, find its dynamic type and the
/// address of its storage.
static void
findDynamicValueAndType(OpaqueValue *value, const Metadata *type,
                        OpaqueValue *&outValue, const Metadata *&outType,
                        bool &inoutCanTake,
                        bool isTargetTypeAnyObject,
                        bool isTargetExistentialMetatype) {
  switch (type->getKind()) {
  case MetadataKind::Class:
  case MetadataKind::ObjCClassWrapper:
  case MetadataKind::ForeignClass: {
    // TODO: avoid unnecessary repeat lookup of
    // ObjCClassWrapper/ForeignClass when the type matches.
    outValue = value;
    outType = swift_getObjectType(*reinterpret_cast<HeapObject**>(value));
    return;
  }

  case MetadataKind::Existential: {
    auto existentialType = cast<ExistentialTypeMetadata>(type);
    inoutCanTake &= existentialType->mayTakeValue(value);

    // We can't drill through existential containers unless the result is an
    // existential metatype.
    if (!isTargetExistentialMetatype) {
      outValue = value;
      outType = type;
      return;
    }
    
    switch (existentialType->getRepresentation()) {
    case ExistentialTypeRepresentation::Class: {
      // Class existentials can't recursively contain existential containers,
      // so we can fast-path by not bothering to recur.
      auto existential =
        reinterpret_cast<ClassExistentialContainer*>(value);
      outValue = (OpaqueValue*) &existential->Value;
      outType = swift_getObjectType((HeapObject*) existential->Value);
      return;
    }
    
    case ExistentialTypeRepresentation::Opaque:
    case ExistentialTypeRepresentation::Error: {
      const Metadata *innerType = existentialType->getDynamicType(value);

      // Short cut class in existential as AnyObject casts.
      if (isTargetTypeAnyObject  &&
          innerType->getKind() == MetadataKind::Class) {
        // inline value buffer storage.
        outValue = value;
        outType = 0;
        inoutCanTake = true;
        return;
      }
      OpaqueValue *innerValue
        = existentialType->projectValue(value);

      return findDynamicValueAndType(innerValue, innerType,
                                     outValue, outType, inoutCanTake, false,
                                     isTargetExistentialMetatype);
    }
    }
  }
    
  case MetadataKind::Metatype:
  case MetadataKind::ExistentialMetatype: {
    auto storedType = *(const Metadata **) value;
    outValue = value;
    outType = swift_getMetatypeMetadata(storedType);
    return;
  }

  // Non-polymorphic types.
  default:
    outValue = value;
    outType = type;
    return;
  }
}

extern "C" const Metadata *
swift::swift_getDynamicType(OpaqueValue *value, const Metadata *self,
                            bool existentialMetatype) {
  OpaqueValue *outValue;
  const Metadata *outType;
  bool canTake = false;
  findDynamicValueAndType(value, self, outValue, outType, canTake,
                          /*isAnyObject*/ false,
                          existentialMetatype);
  return outType;
}


/// Given a possibly-existential value, deallocate any buffer in its storage.
static void deallocateDynamicValue(OpaqueValue *value, const Metadata *type) {
  switch (type->getKind()) {
  case MetadataKind::Existential: {
    auto existentialType = cast<ExistentialTypeMetadata>(type);
    
    switch (existentialType->getRepresentation()) {
    case ExistentialTypeRepresentation::Class:
      // Nothing to clean up.
      break;
      
    case ExistentialTypeRepresentation::Error:
      // TODO: We could clean up from a reclaimed uniquely-referenced error box.
      break;
      
    case ExistentialTypeRepresentation::Opaque:
      swift::fatalError(
          0 /* flags */,
          "Attempting to move out of a copy-on-write existential");
      break;
    }
    return;
  }

  // None of the rest of these require deallocation.
  default:
    return;
  }
}

#if SWIFT_OBJC_INTEROP
SWIFT_RUNTIME_EXPORT
id
swift_dynamicCastMetatypeToObjectConditional(const Metadata *metatype) {
  switch (metatype->getKind()) {
  case MetadataKind::Class:
  case MetadataKind::ObjCClassWrapper:
    // Swift classes are objects in and of themselves.
    // ObjC class wrappers get unwrapped.
    return (id)metatype->getObjCClassObject();
  
  // Other kinds of metadata don't cast to AnyObject.
  default:
    return nullptr;
  }
}

SWIFT_RUNTIME_EXPORT
id
swift_dynamicCastMetatypeToObjectUnconditional(const Metadata *metatype,
                                               const char *file, unsigned line, unsigned column) {
  switch (metatype->getKind()) {
  case MetadataKind::Class:
  case MetadataKind::ObjCClassWrapper:
    // Swift classes are objects in and of themselves.
    // ObjC class wrappers get unwrapped.
    return (id)metatype->getObjCClassObject();
  
  // Other kinds of metadata don't cast to AnyObject.
  default: {
    std::string sourceName = nameForMetadata(metatype);
    swift_dynamicCastFailure(metatype, sourceName.c_str(),
                             nullptr, "AnyObject",
                         "only class metatypes can be converted to AnyObject");
  }
  }
}

#endif

/******************************************************************************/
/******************************** AnyHashable *********************************/
/******************************************************************************/

/// Nominal type descriptor for Swift.AnyHashable.
extern "C" const StructDescriptor STRUCT_TYPE_DESCR_SYM(s11AnyHashable);

static bool isAnyHashableType(const StructMetadata *type) {
  return type->getDescription() == &STRUCT_TYPE_DESCR_SYM(s11AnyHashable);
}

static bool isAnyHashableType(const Metadata *type) {
  if (auto structType = dyn_cast<StructMetadata>(type)) {
    return isAnyHashableType(structType);
  }
  return false;
}

/// Perform a dynamic cast from a nominal type to AnyHashable.
static bool _dynamicCastToAnyHashable(OpaqueValue *destination,
                                      OpaqueValue *source,
                                      const Metadata *sourceType,
                                      const Metadata *targetType,
                                      DynamicCastFlags flags) {
  // Look for a conformance to Hashable.
  auto hashableConformance = reinterpret_cast<const HashableWitnessTable *>(
      swift_conformsToProtocol(sourceType, &HashableProtocolDescriptor));

  // If we don't find one, the cast fails.
  if (!hashableConformance) {
    return _fail(source, sourceType, targetType, flags);
  }

  // If we do find one, the cast succeeds.

  // Initialize the destination.
  _swift_convertToAnyHashableIndirect(source, destination,
                                      sourceType, hashableConformance);

  // Destroy the value if requested.
  if (flags & DynamicCastFlags::TakeOnSuccess) {
    sourceType->vw_destroy(source);
  }

  // The cast succeeded.
  return true;
}

/// Perform a dynamic cast to an arbitrary type from AnyHashable.
static bool _dynamicCastFromAnyHashable(OpaqueValue *destination,
                                        OpaqueValue *source,
                                        const Metadata *sourceType,
                                        const Metadata *targetType,
                                        DynamicCastFlags flags) {
  // Perform a conditional, non-consuming cast into the destination.
  if (_swift_anyHashableDownCastConditionalIndirect(source, destination,
                                                    targetType)) {
    // If that succeeded, and we were supposed to claim the source,
    // destroy it now.
    if (flags & DynamicCastFlags::TakeOnSuccess) {
      sourceType->vw_destroy(source);
    }

    return true;
  }

  return _fail(source, sourceType, targetType, flags);
}

/******************************************************************************/
/******************************** Existentials ********************************/
/******************************************************************************/

#if !SWIFT_OBJC_INTEROP // __SwiftValue is a native class
SWIFT_CC SWIFT_RUNTIME_STDLIB_INTERNAL
bool swift_swiftValueConformsTo(const Metadata *, const Metadata *);

#define _bridgeAnythingToObjectiveC                                 \
  MANGLE_SYM(s27_bridgeAnythingToObjectiveCyyXlxlF)
SWIFT_CC SWIFT_RUNTIME_STDLIB_API
HeapObject *_bridgeAnythingToObjectiveC(OpaqueValue *src, const Metadata *srcType);
#endif

/// Perform a dynamic cast to an existential type.
static bool _dynamicCastToExistential(OpaqueValue *dest,
                                      OpaqueValue *src,
                                      const Metadata *srcType,
                                      const ExistentialTypeMetadata *targetType,
                                      DynamicCastFlags flags) {
#if SWIFT_OBJC_INTEROP
  // This variable's lifetime needs to be for the whole function, but is
  // only valid with Objective-C interop enabled.
  id tmp;
#endif
  // Find the actual type of the source.
  OpaqueValue *srcDynamicValue;
  const Metadata *srcDynamicType;
  bool canConsumeDynamicValue = true;

  // Are we casting to AnyObject? In this case we can fast-path casts from
  // classes.
  bool isTargetTypeAnyObject =
      targetType->getKind() == MetadataKind::Existential &&
      isAnyObjectExistentialType(targetType);

  // We don't care what the target type of a cast from class to AnyObject is.
  // srcDynamicType will be set to a nullptr in this case to save a lookup.
  findDynamicValueAndType(src, srcType, srcDynamicValue, srcDynamicType,
                          canConsumeDynamicValue, isTargetTypeAnyObject,
                          /*isExistentialMetatype*/ true);

  // Recursive casts on the dynamic value should not destroy the source
  // if findDynamicValueAndType doesn't allow it.
  DynamicCastFlags dynamicFlags = flags;
  if (!canConsumeDynamicValue) {
    dynamicFlags = dynamicFlags - (DynamicCastFlags::TakeOnSuccess |
                                   DynamicCastFlags::DestroyOnFailure);
  }

  // Given that we performed a cast on srcDynamicValue and obeyed
  // dynamicFlags there, clean up after src.
  auto maybeDeallocateSource = [&](bool success) {
    // If the flags don't say to destroy src, we're done.
    if (!shouldDeallocateSource(success, flags)) return;

    // If findDynamicValueAndType didn't give us a consumable interior
    // value, then src is still completely intact.
    if (!canConsumeDynamicValue) {
      srcType->vw_destroy(src);

    // Otherwise, if we destroyed an interior value, we need to clean
    // up any leftover buffers it may have been contained in.
    } else if (src != srcDynamicValue) {
      deallocateDynamicValue(src, srcType);
    }
  };

  // A function to call if the dynamic type does not conform to the
  // protocols.  Typically fails, but if the source type is AnyHashable,
  // tries to unwrap that.
  auto fallbackForNonDirectConformance = [&] {
    // As a fallback, if the source type is AnyHashable, perform a cast
    // on its interior value.
    if (isAnyHashableType(srcDynamicType)) {
      bool success =
        _dynamicCastFromAnyHashable(dest, srcDynamicValue, srcDynamicType,
                                    targetType, dynamicFlags);
      maybeDeallocateSource(success);
      return success;
    }

    return _fail(src, srcType, targetType, flags, srcDynamicType);
  };

  // The representation of an existential is different for some protocols.
  switch (targetType->getRepresentation()) {
  case ExistentialTypeRepresentation::Class: {
    auto destExistential =
      reinterpret_cast<ClassExistentialContainer*>(dest);
    MetadataKind kind =
        srcDynamicType ? srcDynamicType->getKind() : MetadataKind::Class;

    // A fallback to use if we don't have a more specialized approach
    // for a non-class type.
    auto fallbackForNonClass = [&] {
#if SWIFT_OBJC_INTEROP
      // If the destination type is a set of protocols that SwiftValue
      // implements, we're fine.
      if (findSwiftValueConformances(targetType,
                                     destExistential->getWitnessTables())) {
        bool consumeValue = dynamicFlags & DynamicCastFlags::TakeOnSuccess;
        destExistential->Value =
          bridgeAnythingToSwiftValueObject(srcDynamicValue, srcDynamicType,
                                           consumeValue);
        maybeDeallocateSource(true);
        return true;
      }
#else // !SWIFT_OBJC_INTEROP -- __SwiftValue is a native class
      bool isMetatype = kind == MetadataKind::ExistentialMetatype ||
                        kind == MetadataKind::Metatype;
      if (!isMetatype && (isTargetTypeAnyObject ||
                          swift_swiftValueConformsTo(targetType, targetType))) {
        auto object = _bridgeAnythingToObjectiveC(src, srcType);
        swift_retain(object);
        destExistential->Value = object;
        maybeDeallocateSource(true);
        return true;
      }
#endif

      return _fail(src, srcType, targetType, flags);
    };

    // If the source type is a value type, it cannot possibly conform
    // to a class-bounded protocol. 
    switch (kind) {
    case MetadataKind::ExistentialMetatype:
    case MetadataKind::Metatype: {
#if SWIFT_OBJC_INTEROP
      // Class metadata can be used as an object when ObjC interop is available.
      auto metatypePtr = reinterpret_cast<const Metadata **>(src);
      auto metatype = *metatypePtr;
      tmp = swift_dynamicCastMetatypeToObjectConditional(metatype);
      // If the cast succeeded, use the result value as the class instance
      // below.
      if (tmp) {
        srcDynamicValue = reinterpret_cast<OpaqueValue*>(&tmp);
        srcDynamicType = reinterpret_cast<const Metadata*>(tmp);
        break;
      }
#endif
      // Otherwise, metatypes aren't class objects.
      return fallbackForNonClass();
    }
    
    case MetadataKind::Class:
    case MetadataKind::ObjCClassWrapper:
    case MetadataKind::ForeignClass:
    case MetadataKind::Existential:
      // Handle the class cases below.  Note that opaque existentials
      // shouldn't get here because we should have drilled into them above.
      break;

    case MetadataKind::Struct:
      // If the source type is AnyHashable, cast from that.
      if (isAnyHashableType(cast<StructMetadata>(srcDynamicType))) {
        bool result =
          _dynamicCastFromAnyHashable(dest, srcDynamicValue, srcDynamicType,
                                      targetType, dynamicFlags);
        maybeDeallocateSource(result);
        return result;
      }
      LLVM_FALLTHROUGH;

    case MetadataKind::Enum:
    case MetadataKind::Optional:
      // If the source type is bridged to Objective-C, try to bridge.
      if (auto srcBridgeWitness = findBridgeWitness(srcDynamicType)) {
        bool success = _dynamicCastValueToClassExistentialViaObjCBridgeable(
                         dest,
                         srcDynamicValue,
                         srcDynamicType,
                         targetType,
                         srcBridgeWitness,
                         dynamicFlags);
        maybeDeallocateSource(success);
        return success;
      }
      LLVM_FALLTHROUGH;

    default:
      return fallbackForNonClass();
    }

    // Check for protocol conformances and fill in the witness tables. If
    // srcDynamicType equals nullptr we have a cast from an existential
    // container with a class instance to AnyObject. In this case no check is
    // necessary.
    if (srcDynamicType && !_conformsToProtocols(srcDynamicValue, srcDynamicType,
                              targetType,
                              destExistential->getWitnessTables()))
      return fallbackForNonDirectConformance();

    auto object = *(reinterpret_cast<HeapObject**>(srcDynamicValue));
    destExistential->Value = object;
    if (!canConsumeDynamicValue || !(flags & DynamicCastFlags::TakeOnSuccess)) {
      swift_unknownObjectRetain(object);
    }
    maybeDeallocateSource(true);
    return true;
  }
  case ExistentialTypeRepresentation::Opaque: {
    auto destExistential =
      reinterpret_cast<OpaqueExistentialContainer*>(dest);

    // Check for protocol conformances and fill in the witness tables.
    if (!_conformsToProtocols(srcDynamicValue, srcDynamicType,
                              targetType,
                              destExistential->getWitnessTables()))
      return fallbackForNonDirectConformance();

    // Fill in the type and value.
    destExistential->Type = srcDynamicType;
    if (canConsumeDynamicValue && (flags & DynamicCastFlags::TakeOnSuccess)) {
      auto *value = srcDynamicType->allocateBoxForExistentialIn(&destExistential->Buffer);
      srcDynamicType->vw_initializeWithTake(value, srcDynamicValue);
    } else {
      auto *value = srcDynamicType->allocateBoxForExistentialIn(&destExistential->Buffer);
      srcDynamicType->vw_initializeWithCopy(value, srcDynamicValue);
    }
    maybeDeallocateSource(true);
    return true;
  }
  case ExistentialTypeRepresentation::Error: {
    auto destBoxAddr =
      reinterpret_cast<SwiftError**>(dest);
    // Check for the Error protocol conformance, which should be the only
    // one we need.
    assert(targetType->NumProtocols == 1);
    const WitnessTable *errorWitness;
    if (!_conformsToProtocols(srcDynamicValue, srcDynamicType,
                              targetType,
                              &errorWitness))
      return fallbackForNonDirectConformance();

#if SWIFT_OBJC_INTEROP
    // Check whether there is an embedded NSError. If so, use that for our Error
    // representation.
    if (auto embedded = getErrorEmbeddedNSErrorIndirect(srcDynamicValue,
                                                        srcDynamicType,
                                                        errorWitness)) {
      *destBoxAddr = reinterpret_cast<SwiftError*>(embedded);
      if (shouldDeallocateSource(true, flags)) {
        srcType->vw_destroy(src);
      }
      return true;
    }
#endif

    bool isTake =
      (canConsumeDynamicValue && (flags & DynamicCastFlags::TakeOnSuccess));
    BoxPair destBox = swift_allocError(srcDynamicType, errorWitness,
                                       srcDynamicValue, isTake);
    *destBoxAddr = reinterpret_cast<SwiftError*>(destBox.object);
    maybeDeallocateSource(true);
    return true;
  }
  }

  swift_runtime_unreachable("Unhandled ExistentialTypeRepresentation in switch.");
}

/******************************************************************************/
/********************************** Classes ***********************************/
/******************************************************************************/

static const void *
_dynamicCastUnknownClassToExistential(const void *object,
                                    const ExistentialTypeMetadata *targetType) {
  // FIXME: check superclass constraint here.

  for (auto protocol : targetType->getProtocols()) {
    switch (protocol.getDispatchStrategy()) {
    case ProtocolDispatchStrategy::Swift:
      // If the target existential requires witness tables, we can't do this cast.
      // The result type would not have a single-refcounted-pointer rep.
      return nullptr;
    case ProtocolDispatchStrategy::ObjC:
#if SWIFT_OBJC_INTEROP
      if (!objectConformsToObjCProtocol(object, protocol))
        return nullptr;
      break;
#else
      assert(false && "ObjC interop disabled?!");
      return nullptr;
#endif
    }
  }
  
  return object;
}

/// Perform a dynamic class of some sort of class instance to some
/// sort of class type.
static const void *
swift_dynamicCastUnknownClassImpl(const void *object,
                                  const Metadata *targetType) {
  switch (targetType->getKind()) {
  case MetadataKind::Class: {
    auto targetClassType = static_cast<const ClassMetadata *>(targetType);
    return swift_dynamicCastClass(object, targetClassType);
  }

  case MetadataKind::ObjCClassWrapper: {
#if SWIFT_OBJC_INTEROP
    auto targetClassType
      = static_cast<const ObjCClassWrapperMetadata *>(targetType)->Class;
    return swift_dynamicCastObjCClass(object, targetClassType);
#else
    return nullptr;
#endif
  }

  case MetadataKind::ForeignClass: {
#if SWIFT_OBJC_INTEROP
    auto targetClassType = static_cast<const ForeignClassMetadata*>(targetType);
    return swift_dynamicCastForeignClass(object, targetClassType);
#else
    return nullptr;
#endif
  }

  case MetadataKind::Existential: {
    return _dynamicCastUnknownClassToExistential(object,
                      static_cast<const ExistentialTypeMetadata *>(targetType));
  }
  default:
    return nullptr;
  }
}

/// Perform a dynamic class of some sort of class instance to some
/// sort of class type.
static const void *
swift_dynamicCastUnknownClassUnconditionalImpl(const void *object,
                                               const Metadata *targetType,
                                               const char *file, unsigned line, unsigned column) {
  switch (targetType->getKind()) {
  case MetadataKind::Class: {
    auto targetClassType = static_cast<const ClassMetadata *>(targetType);
    return swift_dynamicCastClassUnconditional(object, targetClassType, file, line, column);
  }

  case MetadataKind::ObjCClassWrapper: {
#if SWIFT_OBJC_INTEROP
    auto targetClassType
      = static_cast<const ObjCClassWrapperMetadata *>(targetType)->Class;
    return swift_dynamicCastObjCClassUnconditional(object, targetClassType, file, line, column);
#else
    swift_dynamicCastFailure(_swift_getClass(object), targetType);
#endif
  }

  case MetadataKind::ForeignClass: {
#if SWIFT_OBJC_INTEROP
    auto targetClassType = static_cast<const ForeignClassMetadata*>(targetType);
    return swift_dynamicCastForeignClassUnconditional(object, targetClassType, file, line, column);
#else
    swift_dynamicCastFailure(_swift_getClass(object), targetType);
#endif
  }

  case MetadataKind::Existential: {
    // We can cast to ObjC existentials. Non-ObjC existentials don't have
    // a single-refcounted-pointer representation.
    if (auto result = _dynamicCastUnknownClassToExistential(object,
                     static_cast<const ExistentialTypeMetadata *>(targetType)))
      return result;
    
    swift_dynamicCastFailure(_swift_getClass(object), targetType);
  }

  default:
    swift_dynamicCastFailure(_swift_getClass(object), targetType);
  }
}

/******************************************************************************/
/********************************* Metatypes **********************************/
/******************************************************************************/

static const Metadata *
swift_dynamicCastMetatypeImpl(const Metadata *sourceType,
                              const Metadata *targetType) {
  auto origSourceType = sourceType;

  // Identical types always succeed
  if (sourceType == targetType)
    return origSourceType;

  switch (targetType->getKind()) {
  case MetadataKind::ObjCClassWrapper:
    // Get the actual class object.
    targetType = static_cast<const ObjCClassWrapperMetadata*>(targetType)
      ->Class;
    LLVM_FALLTHROUGH;
  case MetadataKind::Class:
    // The source value must also be a class; otherwise the cast fails.
    switch (sourceType->getKind()) {
    case MetadataKind::ObjCClassWrapper:
      // Get the actual class object.
      sourceType = static_cast<const ObjCClassWrapperMetadata*>(sourceType)
        ->Class;
      LLVM_FALLTHROUGH;
    case MetadataKind::Class: {
      // Check if the source is a subclass of the target.
#if SWIFT_OBJC_INTEROP
      // We go through ObjC lookup to deal with potential runtime magic in ObjC
      // land.
      if (swift_dynamicCastObjCClassMetatype((const ClassMetadata*)sourceType,
                                             (const ClassMetadata*)targetType))
        return origSourceType;
#else
      if (_dynamicCastClassMetatype((const ClassMetadata*)sourceType,
                                    (const ClassMetadata*)targetType))
        return origSourceType;
#endif
      return nullptr;
    }
    case MetadataKind::ForeignClass: {
      // Check if the source is a subclass of the target.
      if (swift_dynamicCastForeignClassMetatype(
            (const ClassMetadata*)sourceType,
              (const ClassMetadata*)targetType))
        return origSourceType;
      return nullptr;
    }

    default:
      return nullptr;
    }
    break;
      
  case MetadataKind::ForeignClass:
    switch (sourceType->getKind()) {
    case MetadataKind::ObjCClassWrapper:
      // Get the actual class object.
      sourceType = static_cast<const ObjCClassWrapperMetadata*>(sourceType)
        ->Class;
      LLVM_FALLTHROUGH;
    case MetadataKind::Class:
    case MetadataKind::ForeignClass:
      // Check if the source is a subclass of the target.
      if (swift_dynamicCastForeignClassMetatype(
            (const ClassMetadata*)sourceType,
              (const ClassMetadata*)targetType))
        return origSourceType;
      return nullptr;
    default:
      return nullptr;
    }
    break;

  case MetadataKind::Existential: {
    auto targetTypeAsExistential = static_cast<const ExistentialTypeMetadata *>(targetType);
    if (_conformsToProtocols(nullptr, sourceType, targetTypeAsExistential, nullptr))
      return origSourceType;
    return nullptr;
  }

  default:
    return nullptr;
  }

  swift_runtime_unreachable("Unhandled MetadataKind in switch.");
}

static const Metadata *
swift_dynamicCastMetatypeUnconditionalImpl(const Metadata *sourceType,
                                           const Metadata *targetType,
                                           const char *file, unsigned line, unsigned column) {
  auto origSourceType = sourceType;

  // Identical types always succeed
  if (sourceType == targetType)
    return origSourceType;

  switch (targetType->getKind()) {
  case MetadataKind::ObjCClassWrapper:
    // Get the actual class object.
    targetType = static_cast<const ObjCClassWrapperMetadata*>(targetType)
      ->Class;
    LLVM_FALLTHROUGH;
  case MetadataKind::Class:
    // The source value must also be a class; otherwise the cast fails.
    switch (sourceType->getKind()) {
    case MetadataKind::ObjCClassWrapper:
      // Get the actual class object.
      sourceType = static_cast<const ObjCClassWrapperMetadata*>(sourceType)
        ->Class;
      LLVM_FALLTHROUGH;
    case MetadataKind::Class: {
      // Check if the source is a subclass of the target.
#if SWIFT_OBJC_INTEROP
      // We go through ObjC lookup to deal with potential runtime magic in ObjC
      // land.
      swift_dynamicCastObjCClassMetatypeUnconditional(
                                            (const ClassMetadata*)sourceType,
                                            (const ClassMetadata*)targetType,
                                            file, line, column);
#else
      if (!_dynamicCastClassMetatype((const ClassMetadata*)sourceType,
                                     (const ClassMetadata*)targetType))
        swift_dynamicCastFailure(sourceType, targetType);
#endif
      // If we returned, then the cast succeeded.
      return origSourceType;
    }
    case MetadataKind::ForeignClass: {
      // Check if the source is a subclass of the target.
      swift_dynamicCastForeignClassMetatypeUnconditional(
                                            (const ClassMetadata*)sourceType,
                                            (const ClassMetadata*)targetType,
                                            file, line, column);
      // If we returned, then the cast succeeded.
      return origSourceType;
    }
    default:
      swift_dynamicCastFailure(sourceType, targetType);
    }
    break;
    
  case MetadataKind::ForeignClass:
    // The source value must also be a class; otherwise the cast fails.
    switch (sourceType->getKind()) {
    case MetadataKind::ObjCClassWrapper:
      // Get the actual class object.
      sourceType = static_cast<const ObjCClassWrapperMetadata*>(sourceType)
        ->Class;
      LLVM_FALLTHROUGH;
    case MetadataKind::Class:
    case MetadataKind::ForeignClass:
      // Check if the source is a subclass of the target.
      swift_dynamicCastForeignClassMetatypeUnconditional(
                                            (const ClassMetadata*)sourceType,
                                            (const ClassMetadata*)targetType,
                                            file, line, column);
      // If we returned, then the cast succeeded.
      return origSourceType;
    default:
      swift_dynamicCastFailure(sourceType, targetType);
    }
    break;

  case MetadataKind::Existential: {
    auto targetTypeAsExistential = static_cast<const ExistentialTypeMetadata *>(targetType);
    if (_conformsToProtocols(nullptr, sourceType, targetTypeAsExistential, nullptr))
      return origSourceType;
    swift_dynamicCastFailure(sourceType, targetType);
  }

  default:
    swift_dynamicCastFailure(sourceType, targetType);
  }
}

/******************************************************************************/
/********************************** Classes ***********************************/
/******************************************************************************/

/// Do a dynamic cast to the target class.
static void *_dynamicCastUnknownClass(void *object,
                                      const Metadata *targetType,
                                      // TODO: carry through source location info
                                      bool unconditional) {
  // The unconditional path avoids some failure logic.
  if (unconditional) {
    return const_cast<void*>(
          swift_dynamicCastUnknownClassUnconditional(object, targetType, nullptr, 0, 0));
  }

  return const_cast<void*>(swift_dynamicCastUnknownClass(object, targetType));
}

static bool _dynamicCastUnknownClassIndirect(OpaqueValue *dest,
                                             void *object,
                                             const Metadata *targetType,
                                             DynamicCastFlags flags) {
    void **destSlot = reinterpret_cast<void **>(dest);

  // The unconditional path avoids some failure logic.
  if (flags & DynamicCastFlags::Unconditional) {
    void *result = const_cast<void*>(
          swift_dynamicCastUnknownClassUnconditional(object, targetType, nullptr, 0, 0));
    *destSlot = result;

    if (!(flags & DynamicCastFlags::TakeOnSuccess)) {
      swift_unknownObjectRetain(result);
    }
    return true;
  }

  // Okay, we're doing a conditional cast.
  void *result =
    const_cast<void*>(swift_dynamicCastUnknownClass(object, targetType));

  // If the cast failed, destroy the input and return false.
  if (!result) {
    if (flags & DynamicCastFlags::DestroyOnFailure) {
      swift_unknownObjectRelease(object);
    }
    return false;
  }

  // Otherwise, store to the destination and return true.
  *destSlot = result;
  if (!(flags & DynamicCastFlags::TakeOnSuccess)) {
    swift_unknownObjectRetain(result);
  }
  return true;
}

/******************************************************************************/
/******************************** Existentials ********************************/
/******************************************************************************/

/// Perform a dynamic cast from an existential type to some kind of
/// class type.
static bool _dynamicCastToUnknownClassFromExistential(OpaqueValue *dest,
                                                      OpaqueValue *src,
                                        const ExistentialTypeMetadata *srcType,
                                        const Metadata *targetType,
                                        DynamicCastFlags flags) {
  switch (srcType->getRepresentation()) {
  case ExistentialTypeRepresentation::Class: {
    auto classContainer =
      reinterpret_cast<ClassExistentialContainer*>(src);
    void *obj = classContainer->Value;
    return _dynamicCastUnknownClassIndirect(dest, obj, targetType, flags);
  }
  case ExistentialTypeRepresentation::Opaque: {
    auto opaqueContainer =
      reinterpret_cast<OpaqueExistentialContainer*>(src);
    auto srcCapturedType = opaqueContainer->Type;
    OpaqueValue *srcValue = srcType->projectValue(src);
    // Can't 'take' out of a non-unique box. So we will force a copy.
    auto subFlags = flags;
    if (src != srcValue)
      subFlags = subFlags - (DynamicCastFlags::DestroyOnFailure
                             | DynamicCastFlags::TakeOnSuccess);
    bool result = swift_dynamicCast(dest, srcValue, srcCapturedType, targetType,
                                    subFlags);
    // We were supposed to 'take' the source but we have copied. Reconcile by
    // destroying the source.
    if (src != srcValue)
      if (shouldDeallocateSource(result, flags))
        srcType->vw_destroy(src);
    return result;
  }
  case ExistentialTypeRepresentation::Error: {
    const SwiftError *errorBox =
      *reinterpret_cast<const SwiftError * const *>(src);
    auto srcCapturedType = errorBox->getType();
    const OpaqueValue *srcValue;
    // A bridged NSError is itself the value.
    if (errorBox->isPureNSError())
      srcValue = src;
    else
      srcValue = errorBox->getValue();
    
    // We can't take or destroy the value out of the box since it might be
    // shared.
    auto subFlags = flags - (DynamicCastFlags::TakeOnSuccess
                             | DynamicCastFlags::DestroyOnFailure);
    bool result = swift_dynamicCast(dest,
                                    const_cast<OpaqueValue*>(srcValue),
                                    srcCapturedType, targetType,
                                    subFlags);
    if (shouldDeallocateSource(result, flags))
      srcType->vw_destroy(src);
    return result;
  }
  }

  swift_runtime_unreachable(
      "Unhandled ExistentialTypeRepresentation in switch.");
}

static void unwrapExistential(OpaqueValue *src,
                              const ExistentialTypeMetadata *srcType,
                              OpaqueValue *&srcValue,
                              const Metadata *&srcCapturedType,
                              bool &isOutOfLine,
                              bool &canTake) {
  switch (srcType->getRepresentation()) {
    case ExistentialTypeRepresentation::Class: {
      auto classContainer =
        reinterpret_cast<ClassExistentialContainer*>(src);
      srcValue = (OpaqueValue*) &classContainer->Value;
      void *obj = classContainer->Value;
      srcCapturedType = swift_getObjectType(reinterpret_cast<HeapObject*>(obj));
      isOutOfLine = false;
      canTake = true;
      break;
    }
    case ExistentialTypeRepresentation::Opaque: {
      auto opaqueContainer = reinterpret_cast<OpaqueExistentialContainer*>(src);
      srcCapturedType = opaqueContainer->Type;
      srcValue = srcType->projectValue(src);
      // Can't take out of possibly shared existential boxes.
      canTake = (src == srcValue);
      assert(canTake == srcCapturedType->getValueWitnesses()->isValueInline() &&
             "Only inline storage is take-able");
      isOutOfLine = (src != srcValue);
      break;
    }
    case ExistentialTypeRepresentation::Error: {
      const SwiftError *errorBox
      = *reinterpret_cast<const SwiftError * const *>(src);
      
      srcCapturedType = errorBox->getType();
      // A bridged NSError is itself the value.
      if (errorBox->isPureNSError())
        srcValue = src;
      else
        srcValue = const_cast<OpaqueValue*>(errorBox->getValue());
      
      // The value is out-of-line, but we can't take it, since it may be shared.
      isOutOfLine = true;
      canTake = false;
      break;
    }
  }
}

/// Perform a dynamic cast from an existential type to a
/// non-existential type.
static bool _dynamicCastFromExistential(OpaqueValue *dest,
                                        OpaqueValue *src,
                                        const ExistentialTypeMetadata *srcType,
                                        const Metadata *targetType,
                                        DynamicCastFlags flags) {
  OpaqueValue *srcValue;
  const Metadata *srcCapturedType;
  bool isOutOfLine;
  bool canTake;

  unwrapExistential(src, srcType,
                    srcValue, srcCapturedType, isOutOfLine, canTake);
  
  auto subFlags = flags;
  if (!canTake)
    subFlags = subFlags - (DynamicCastFlags::DestroyOnFailure
                           | DynamicCastFlags::TakeOnSuccess);

  bool result = swift_dynamicCast(dest, srcValue, srcCapturedType,
                                  targetType, subFlags);

  if (!canTake) {
    // swift_dynamicCast performed no memory management.
    // Destroy the value if requested.
    if (shouldDeallocateSource(result, flags))
      srcType->vw_destroy(src);
  } else {
    assert(!isOutOfLine &&
           "Should only see inline representations of existentials");
  }

  return result;
}

/******************************************************************************/
/********************************* Metatypes **********************************/
/******************************************************************************/

/// Perform a dynamic cast of a metatype to a metatype.
///
/// Note that the check is whether 'metatype' is an *instance of*
/// 'targetType', not a *subtype of it*.
static bool _dynamicCastMetatypeToMetatype(OpaqueValue *dest,
                                           const Metadata *metatype,
                                           const MetatypeMetadata *targetType,
                                           DynamicCastFlags flags) {
  const Metadata *result;
  if (flags & DynamicCastFlags::Unconditional) {
    result = swift_dynamicCastMetatypeUnconditional(metatype,
                                                    targetType->InstanceType,
                                                    nullptr, 0, 0);
  } else {
    result = swift_dynamicCastMetatype(metatype, targetType->InstanceType);
    if (!result) return false;
  }

  *((const Metadata **) dest) = result;
  return true;
}

/// Check whether an unknown class instance is actually a class object.
static const Metadata *_getUnknownClassAsMetatype(void *object) {
#if SWIFT_OBJC_INTEROP
  // Objective-C class metadata are objects, so an AnyObject (or NSObject)
  // may refer to a class object.

  // Test whether the object's isa is a metaclass, which indicates that the
  // object is a class.

  Class isa = object_getClass((id)object);
  if (class_isMetaClass(isa)) {
    return swift_getObjCClassMetadata((const ClassMetadata *)object);
  }
#endif

  // Class values are currently never metatypes in the native runtime.
  return nullptr;
}

/// Perform a dynamic cast of a class value to a metatype type.
static bool _dynamicCastUnknownClassToMetatype(OpaqueValue *dest,
                                               void *object,
                                             const MetatypeMetadata *targetType,
                                               DynamicCastFlags flags) {
  if (auto metatype = _getUnknownClassAsMetatype(object))
    return _dynamicCastMetatypeToMetatype(dest, metatype, targetType, flags);

  if (flags & DynamicCastFlags::Unconditional)
    swift_dynamicCastFailure(_swift_getClass(object), targetType);
  if (flags & DynamicCastFlags::DestroyOnFailure)
    swift_unknownObjectRelease((HeapObject*) object);
  return false;
}

/// Perform a dynamic cast to a metatype type.
static bool _dynamicCastToMetatype(OpaqueValue *dest,
                                   OpaqueValue *src,
                                   const Metadata *srcType,
                                   const MetatypeMetadata *targetType,
                                   DynamicCastFlags flags) {

  switch (srcType->getKind()) {
  case MetadataKind::Metatype: {
    const Metadata *srcMetatype = *(const Metadata * const *) src;
    return _dynamicCastMetatypeToMetatype(dest, srcMetatype,
                                          targetType, flags);
  }

  case MetadataKind::ExistentialMetatype: {
    const Metadata *srcMetatype = *(const Metadata * const *) src;
    return _dynamicCastMetatypeToMetatype(dest, srcMetatype,
                                          targetType, flags);
  }

  case MetadataKind::Existential: {
    auto srcExistentialType = cast<ExistentialTypeMetadata>(srcType);
    switch (srcExistentialType->getRepresentation()) {
    case ExistentialTypeRepresentation::Class: {
      auto srcExistential = (ClassExistentialContainer*) src;
      return _dynamicCastUnknownClassToMetatype(dest,
                                                srcExistential->Value,
                                                targetType, flags);
    }
    case ExistentialTypeRepresentation::Opaque: {
      auto srcExistential = (OpaqueExistentialContainer*) src;
      auto srcValueType = srcExistential->Type;
      // Can't 'take' out of a non-unique box. So we will force a copy.
      auto subFlags = flags;
      auto srcValue = srcExistentialType->projectValue(src);
      if (src != srcValue)
        subFlags = subFlags - (DynamicCastFlags::DestroyOnFailure |
                               DynamicCastFlags::TakeOnSuccess);
      bool result = _dynamicCastToMetatype(dest, srcValue, srcValueType,
                                           targetType, subFlags);
      // We were supposed to 'take' the source but we have copied. Reconcile by
      // destroying the source.
      if (src != srcValue)
        if (shouldDeallocateSource(result, flags))
          srcType->vw_destroy(src);
      return result;
    }
    case ExistentialTypeRepresentation::Error: {
      const SwiftError *srcBox
        = *reinterpret_cast<const SwiftError * const *>(src);
      
      auto srcValueType = srcBox->getType();
      const OpaqueValue *srcValue;
      if (srcBox->isPureNSError())
        srcValue = src;
      else
        srcValue = srcBox->getValue();
      
      // Can't take from a box since the value may be shared.
      auto subFlags = flags - (DynamicCastFlags::TakeOnSuccess
                               | DynamicCastFlags::DestroyOnFailure);
      bool result = _dynamicCastToMetatype(dest,
                                           const_cast<OpaqueValue*>(srcValue),
                                           srcValueType,
                                           targetType, subFlags);
      if (shouldDeallocateSource(result, flags))
        srcType->vw_destroy(src);
      return result;
    }
    }
  }

  case MetadataKind::Class:
  case MetadataKind::ObjCClassWrapper:
  case MetadataKind::ForeignClass: {
    void *object = *reinterpret_cast<void**>(src);
    return _dynamicCastUnknownClassToMetatype(dest, object, targetType, flags);
  }

  case MetadataKind::Struct: // AnyHashable, if metatypes implement Hashable
  default:
    return _fail(src, srcType, targetType, flags);
  }
}

/// Perform a dynamic cast of a metatype to an existential metatype type.
static bool _dynamicCastMetatypeToExistentialMetatype(OpaqueValue *dest,
                                 const Metadata *srcMetatype,
                                 const ExistentialMetatypeMetadata *targetType,
                                 DynamicCastFlags flags,
                                 bool writeDestMetatype = true) {
  // The instance type of an existential metatype must be either an
  // existential or an existential metatype.
  auto destMetatype = reinterpret_cast<ExistentialMetatypeContainer*>(dest);

  // If it's an existential, we need to check for conformances.
  auto targetInstanceType = targetType->InstanceType;
  if (auto targetInstanceTypeAsExistential =
        dyn_cast<ExistentialTypeMetadata>(targetInstanceType)) {
    // Check for conformance to all the protocols.
    // TODO: collect the witness tables.
    const WitnessTable **conformance
      = writeDestMetatype ? destMetatype->getWitnessTables() : nullptr;
    if (!_conformsToProtocols(nullptr, srcMetatype,
                              targetInstanceTypeAsExistential,
                              conformance)) {
      if (flags & DynamicCastFlags::Unconditional)
        swift_dynamicCastFailure(srcMetatype, targetType);
      return false;
    }

    if (writeDestMetatype)
      destMetatype->Value = srcMetatype;
    return true;
  }

  // Otherwise, we're casting to SomeProtocol.Type.Type.
  auto targetInstanceTypeAsMetatype =
    cast<ExistentialMetatypeMetadata>(targetInstanceType);

  // If the source type isn't a metatype, the cast fails.
  auto srcMetatypeMetatype = dyn_cast<MetatypeMetadata>(srcMetatype);
  if (!srcMetatypeMetatype) {
    if (flags & DynamicCastFlags::Unconditional)
      swift_dynamicCastFailure(srcMetatype, targetType);
    return false;
  }

  // The representation of an existential metatype remains consistent
  // arbitrarily deep: a metatype, followed by some protocols.  The
  // protocols are the same at every level, so we can just set the
  // metatype correctly and then recurse, letting the recursive call
  // fill in the conformance information correctly.

  // Proactively set the destination metatype so that we can tail-recur,
  // unless we've already done so.  There's no harm in doing this even if
  // the cast fails.
  if (writeDestMetatype)
    *((const Metadata **) dest) = srcMetatype;  

  // Recurse.
  auto srcInstanceType = srcMetatypeMetatype->InstanceType;
  return _dynamicCastMetatypeToExistentialMetatype(dest, srcInstanceType,
                                             targetInstanceTypeAsMetatype,
                                                   flags,
                                                   /*overwrite*/ false);
}

/// Perform a dynamic cast of a class value to an existential metatype type.
static bool _dynamicCastUnknownClassToExistentialMetatype(OpaqueValue *dest,
                                                          void *object,
                                const ExistentialMetatypeMetadata *targetType,
                                                       DynamicCastFlags flags) {
  if (auto metatype = _getUnknownClassAsMetatype(object))
    return _dynamicCastMetatypeToExistentialMetatype(dest, metatype,
                                                     targetType, flags);
  
  // Class values are currently never metatypes (?).
  if (flags & DynamicCastFlags::Unconditional)
    swift_dynamicCastFailure(_swift_getClass(object), targetType);
  if (flags & DynamicCastFlags::DestroyOnFailure)
    swift_unknownObjectRelease((HeapObject*) object);
  return false;
}

/// Perform a dynamic cast to an existential metatype type.
static bool _dynamicCastToExistentialMetatype(OpaqueValue *dest,
                                              OpaqueValue *src,
                                              const Metadata *srcType,
                           const ExistentialMetatypeMetadata *targetType,
                                              DynamicCastFlags flags) {
  
  switch (srcType->getKind()) {
  case MetadataKind::Metatype: {
    const Metadata *srcMetatype = *(const Metadata * const *) src;
    return _dynamicCastMetatypeToExistentialMetatype(dest, srcMetatype,
                                                     targetType, flags);
  }

  // TODO: take advantage of protocol conformances already known.
  case MetadataKind::ExistentialMetatype: {
    const Metadata *srcMetatype = *(const Metadata * const *) src;
    return _dynamicCastMetatypeToExistentialMetatype(dest, srcMetatype,
                                                     targetType, flags);
  }

  case MetadataKind::Existential: {
    auto srcExistentialType = cast<ExistentialTypeMetadata>(srcType);
    switch (srcExistentialType->getRepresentation()) {
    case ExistentialTypeRepresentation::Class: {
      auto srcExistential = (ClassExistentialContainer*) src;
      return _dynamicCastUnknownClassToExistentialMetatype(dest,
                                                srcExistential->Value,
                                                targetType, flags);
    }
    case ExistentialTypeRepresentation::Opaque: {
      auto srcExistential = (OpaqueExistentialContainer*) src;
      auto srcValueType = srcExistential->Type;
      auto subFlags = flags;
      auto srcValue = srcExistentialType->projectValue(src);
      // Can't 'take' out of a non-unique box. So we will force a copy.
      if (src != srcValue)
        subFlags = subFlags - (DynamicCastFlags::DestroyOnFailure |
                               DynamicCastFlags::TakeOnSuccess);
      bool result = _dynamicCastToExistentialMetatype(
          dest, srcValue, srcValueType, targetType, subFlags);
      // We were supposed to 'take' the source but we have copied. Reconcile by
      // destroying the source.
      if (src != srcValue)
        if (shouldDeallocateSource(result, flags))
          srcType->vw_destroy(src);
      return result;
    }
    case ExistentialTypeRepresentation::Error: {
      const SwiftError *srcBox
        = *reinterpret_cast<const SwiftError * const *>(src);
      
      auto srcValueType = srcBox->getType();
      const OpaqueValue *srcValue;
      if (srcBox->isPureNSError())
        srcValue = src;
      else
        srcValue = srcBox->getValue();
      
      // Can't take from a box since the value may be shared.
      auto subFlags = flags - (DynamicCastFlags::TakeOnSuccess
                               | DynamicCastFlags::DestroyOnFailure);
      bool result = _dynamicCastToExistentialMetatype(dest,
                                           const_cast<OpaqueValue*>(srcValue),
                                           srcValueType,
                                           targetType, subFlags);
      if (shouldDeallocateSource(result, flags))
        srcType->vw_destroy(src);
      return result;
    }
    }
  }

  case MetadataKind::Class:
  case MetadataKind::ObjCClassWrapper:
  case MetadataKind::ForeignClass: {
    void *object = *reinterpret_cast<void**>(src);
    return _dynamicCastUnknownClassToExistentialMetatype(dest, object,
                                                         targetType, flags);
  }

  case MetadataKind::Struct:  // AnyHashable, if metatypes implement Hashable
  default:
    return _fail(src, srcType, targetType, flags);
  }
}

/******************************************************************************/
/********************************* Functions **********************************/
/******************************************************************************/

static bool _dynamicCastToFunction(OpaqueValue *dest,
                                   OpaqueValue *src,
                                   const Metadata *srcType,
                                   const FunctionTypeMetadata *targetType,
                                   DynamicCastFlags flags) {
  // Function casts succeed on exact matches, or if the target type is
  // throwier than the source type.
  //
  // TODO: We could also allow ABI-compatible variance, such as casting
  // a dynamic Base -> Derived to Derived -> Base. We wouldn't be able to
  // perform a dynamic cast that required any ABI adjustment without a JIT
  // though.

  // Check for an exact type match first.
  if (srcType == targetType) {
    return _succeed(dest, src, srcType, flags);
  }

  switch (srcType->getKind()) {
  case MetadataKind::Function: {
    auto srcFn = static_cast<const FunctionTypeMetadata *>(srcType);
    auto targetFn = static_cast<const FunctionTypeMetadata *>(targetType);
    
    // Check that argument counts and convention match. "throws" can vary.
    if (srcFn->Flags.withThrows(false) != targetFn->Flags.withThrows(false))
      return _fail(src, srcType, targetType, flags);
    
    // If the target type can't throw, neither can the source.
    if (srcFn->throws() && !targetFn->throws())
      return _fail(src, srcType, targetType, flags);
    
    // The result and argument types must match.
    if (srcFn->ResultType != targetFn->ResultType)
      return _fail(src, srcType, targetType, flags);
    if (srcFn->getNumParameters() != targetFn->getNumParameters())
      return _fail(src, srcType, targetType, flags);

    if (srcFn->hasParameterFlags() != targetFn->hasParameterFlags())
      return _fail(src, srcType, targetType, flags);

    for (unsigned i = 0, e = srcFn->getNumParameters(); i < e; ++i) {
      if (srcFn->getParameter(i) != targetFn->getParameter(i) ||
          srcFn->getParameterFlags(i) != targetFn->getParameterFlags(i))
        return _fail(src, srcType, targetType, flags);
    }

    return _succeed(dest, src, srcType, flags);
  }
  
  case MetadataKind::Existential:
    return _dynamicCastFromExistential(dest, src,
                         static_cast<const ExistentialTypeMetadata*>(srcType),
                         targetType, flags);
    
  default:
    return _fail(src, srcType, targetType, flags);
  }
}

/******************************************************************************/
/********************************* Optionals **********************************/
/******************************************************************************/

namespace {

struct OptionalCastResult {
  bool success;
  const Metadata* payloadType;
};

} // end anonymous namespace

/// Handle optional unwrapping of the cast source.
/// \returns {true, nullptr} if the cast succeeds without unwrapping.
/// \returns {false, nullptr} if the cast fails before unwrapping.
/// \returns {false, payloadType} if the cast should be attempted using an
/// equivalent payloadType.
static OptionalCastResult
checkDynamicCastFromOptional(OpaqueValue *dest,
                             OpaqueValue *src,
                             const Metadata *srcType,
                             const Metadata *targetType,
                             DynamicCastFlags flags) {
  if (srcType->getKind() != MetadataKind::Optional)
    return {false, srcType};

  // Check if the target is an existential that Optional always conforms to.
  if (targetType->getKind() == MetadataKind::Existential) {
    // Attempt a conditional cast without destroying on failure.
    DynamicCastFlags checkCastFlags
      = flags - (DynamicCastFlags::Unconditional
                 | DynamicCastFlags::DestroyOnFailure);
    assert((checkCastFlags - DynamicCastFlags::TakeOnSuccess)
           == DynamicCastFlags::Default && "Unhandled DynamicCastFlag");
    if (_dynamicCastToExistential(dest, src, srcType,
                                  cast<ExistentialTypeMetadata>(targetType),
                                  checkCastFlags)) {
      return {true, nullptr};
    }
  }
  const Metadata *payloadType =
    cast<EnumMetadata>(srcType)->getGenericArgs()[0];
  unsigned enumCase =
    payloadType->vw_getEnumTagSinglePayload(src, /*emptyCases=*/1);
  if (enumCase != 0) {
    // Allow Optional<T>.none -> Optional<U>.none
    if (targetType->getKind() != MetadataKind::Optional) {
      _fail(src, srcType, targetType, flags);
      return {false, nullptr};
    }

    // Get the destination payload type
    const Metadata *targetPayloadType =
      cast<EnumMetadata>(targetType)->getGenericArgs()[0];

    // Inject the .none tag
    targetPayloadType->vw_storeEnumTagSinglePayload(dest, enumCase, 
                                                    /*emptyCases=*/1);

    // We don't have to destroy the source, because it was nil.
    return {true, nullptr};
  }
  // .Some
  // Single payload enums are guaranteed layout compatible with their
  // payload. Only the source's payload needs to be taken or destroyed.
  return checkDynamicCastFromOptional(dest, src, payloadType, targetType, flags);
}

/******************************************************************************/
/**************************** Bridging __SwiftValue ****************************/
/******************************************************************************/

#if !SWIFT_OBJC_INTEROP // __SwiftValue is a native class
SWIFT_CC SWIFT_RUNTIME_STDLIB_INTERNAL
bool swift_unboxFromSwiftValueWithType(OpaqueValue *source,
                                       OpaqueValue *result,
                                       const Metadata *destinationType);
#endif

/// Try to unbox a __SwiftValue box to perform a dynamic cast.
static bool tryDynamicCastBoxedSwiftValue(OpaqueValue *dest,
                                          OpaqueValue *src,
                                          const Metadata *srcType,
                                          const Metadata *targetType,
                                          DynamicCastFlags flags) {
  // These flag combinations are not handled here.
  assert(!(flags & DynamicCastFlags::Unconditional));
  assert(!(flags & DynamicCastFlags::DestroyOnFailure));

  auto originalSrc = src;
  auto originalSrcType = srcType;

  // Swift type should be AnyObject or a class type.
  while (true) {
    if (srcType->isAnyClass()) {
      break;
    }
    auto existentialType = dyn_cast<ExistentialTypeMetadata>(srcType);
    if (!existentialType)
      return false;
    switch (existentialType->getRepresentation()) {
    case ExistentialTypeRepresentation::Class: {
      // If it's a Class object, it must be `AnyObject`
      if (isAnyObjectExistentialType(existentialType)) {
        goto validated;
      }
      return false;
    }
    case ExistentialTypeRepresentation::Opaque: {
      // If it's an opaque existential, unwrap it and check again
      auto opaqueContainer = reinterpret_cast<OpaqueExistentialContainer*>(src);
      srcType = opaqueContainer->Type;
      src = existentialType->projectValue(src);
      break;
    }
    default: {
      return false;
    }
    }
  }
  validated:
  
#if !SWIFT_OBJC_INTEROP // __SwiftValue is a native class:
  if (swift_unboxFromSwiftValueWithType(src, dest, targetType)) {
    // Release the source if we need to.
    if (flags & DynamicCastFlags::TakeOnSuccess)
      originalSrcType->vw_destroy(originalSrc);
    return true;
  }
#endif
  
#if SWIFT_OBJC_INTEROP // __SwiftValue is an ObjC class:
  id srcObject;
  memcpy(&srcObject, src, sizeof(id));
  
  // Do we have a __SwiftValue?
  __SwiftValue *srcSwiftValue = getAsSwiftValue(srcObject);
  if (!srcSwiftValue)
    return false;
  
  // If so, extract the boxed value and try to cast it.
  const Metadata *boxedType;
  const OpaqueValue *boxedValue;
  std::tie(boxedType, boxedValue)
    = getValueFromSwiftValue(srcSwiftValue);
  
  // We can't touch the value from the box because it may be
  // multiply-referenced.
  // TODO: Check for uniqueness and consume if box is unique?
  
  // Does the boxed type exactly match the target type we're looking for?
  if (boxedType == targetType) {
    targetType->vw_initializeWithCopy(dest,
                                      const_cast<OpaqueValue*>(boxedValue));
    // Release the box if we need to.
    if (flags & DynamicCastFlags::TakeOnSuccess)
      originalSrcType->vw_destroy(originalSrc);
    return true;
  }
  
  // Maybe we can cast the boxed value to our destination type somehow.
  auto innerFlags = flags - DynamicCastFlags::TakeOnSuccess
                          - DynamicCastFlags::DestroyOnFailure
                          - DynamicCastFlags::Unconditional;
  if (swift_dynamicCast(dest, const_cast<OpaqueValue*>(boxedValue),
                        boxedType, targetType, innerFlags)) {
    // Release the box if we need to.
    if (flags & DynamicCastFlags::TakeOnSuccess)
      originalSrcType->vw_destroy(originalSrc);
    return true;
  }
#endif
  
  return false;
}

/******************************************************************************/
/******************************** Collections *********************************/
/******************************************************************************/

/// Nominal type descriptor for Swift.Array.
extern "C" const StructDescriptor NOMINAL_TYPE_DESCR_SYM(Sa);

/// Nominal type descriptor for Swift.Dictionary.
extern "C" const StructDescriptor NOMINAL_TYPE_DESCR_SYM(SD);

/// Nominal type descriptor for Swift.Set.
extern "C" const StructDescriptor NOMINAL_TYPE_DESCR_SYM(Sh);

// internal func _arrayDownCastIndirect<SourceValue, TargetValue>(
//   _ source: UnsafePointer<Array<SourceValue>>,
//   _ target: UnsafeMutablePointer<Array<TargetValue>>)
SWIFT_CC SWIFT_RUNTIME_STDLIB_INTERNAL
void _swift_arrayDownCastIndirect(OpaqueValue *destination,
                                  OpaqueValue *source,
                                  const Metadata *sourceValueType,
                                  const Metadata *targetValueType);

// internal func _arrayDownCastConditionalIndirect<SourceValue, TargetValue>(
//   _ source: UnsafePointer<Array<SourceValue>>,
//   _ target: UnsafeMutablePointer<Array<TargetValue>>
// ) -> Bool
SWIFT_CC SWIFT_RUNTIME_STDLIB_INTERNAL
bool _swift_arrayDownCastConditionalIndirect(OpaqueValue *destination,
                                             OpaqueValue *source,
                                             const Metadata *sourceValueType,
                                             const Metadata *targetValueType);

// internal func _setDownCastIndirect<SourceValue, TargetValue>(
//   _ source: UnsafePointer<Set<SourceValue>>,
//   _ target: UnsafeMutablePointer<Set<TargetValue>>)
SWIFT_CC SWIFT_RUNTIME_STDLIB_INTERNAL
void _swift_setDownCastIndirect(OpaqueValue *destination,
                                OpaqueValue *source,
                                const Metadata *sourceValueType,
                                const Metadata *targetValueType,
                                const void *sourceValueHashable,
                                const void *targetValueHashable);

// internal func _setDownCastConditionalIndirect<SourceValue, TargetValue>(
//   _ source: UnsafePointer<Set<SourceValue>>,
//   _ target: UnsafeMutablePointer<Set<TargetValue>>
// ) -> Bool
SWIFT_CC SWIFT_RUNTIME_STDLIB_INTERNAL
bool _swift_setDownCastConditionalIndirect(OpaqueValue *destination,
                                       OpaqueValue *source,
                                       const Metadata *sourceValueType,
                                       const Metadata *targetValueType,
                                       const void *sourceValueHashable,
                                       const void *targetValueHashable);

// internal func _dictionaryDownCastIndirect<SourceKey, SourceValue,
//                                           TargetKey, TargetValue>(
//   _ source: UnsafePointer<Dictionary<SourceKey, SourceValue>>,
//   _ target: UnsafeMutablePointer<Dictionary<TargetKey, TargetValue>>)
SWIFT_CC SWIFT_RUNTIME_STDLIB_INTERNAL
void _swift_dictionaryDownCastIndirect(OpaqueValue *destination,
                                       OpaqueValue *source,
                                       const Metadata *sourceKeyType,
                                       const Metadata *sourceValueType,
                                       const Metadata *targetKeyType,
                                       const Metadata *targetValueType,
                                       const void *sourceKeyHashable,
                                       const void *targetKeyHashable);

// internal func _dictionaryDownCastConditionalIndirect<SourceKey, SourceValue,
//                                                      TargetKey, TargetValue>(
//   _ source: UnsafePointer<Dictionary<SourceKey, SourceValue>>,
//   _ target: UnsafeMutablePointer<Dictionary<TargetKey, TargetValue>>
// ) -> Bool
SWIFT_CC SWIFT_RUNTIME_STDLIB_INTERNAL
bool _swift_dictionaryDownCastConditionalIndirect(OpaqueValue *destination,
                                        OpaqueValue *source,
                                        const Metadata *sourceKeyType,
                                        const Metadata *sourceValueType,
                                        const Metadata *targetKeyType,
                                        const Metadata *targetValueType,
                                        const void *sourceKeyHashable,
                                        const void *targetKeyHashable);

static bool _dynamicCastStructToStruct(OpaqueValue *destination,
                                       OpaqueValue *source,
                                       const StructMetadata *sourceType,
                                       const StructMetadata *targetType,
                                       DynamicCastFlags flags) {
  if (sourceType == targetType)
    return _succeed(destination, source, sourceType, flags);

  // The two types have to be instantiations of the same type.
  const auto descriptor = sourceType->Description;
  const auto targetDescriptor = targetType->Description;
  if (descriptor != targetDescriptor) {
    if (descriptor == &STRUCT_TYPE_DESCR_SYM(s11AnyHashable)) {
      return _dynamicCastFromAnyHashable(destination, source,
                                         sourceType, targetType, flags);
    } else if (targetDescriptor == &STRUCT_TYPE_DESCR_SYM(s11AnyHashable)) {
      return _dynamicCastToAnyHashable(destination, source,
                                       sourceType, targetType, flags);
    } else {
      return _fail(source, sourceType, targetType, flags);
    }
  }

  auto sourceArgs = sourceType->getGenericArgs();
  auto targetArgs = targetType->getGenericArgs();

  bool result;

  // Arrays.
  if (descriptor == &NOMINAL_TYPE_DESCR_SYM(Sa)) {
    if (flags & DynamicCastFlags::Unconditional) {
      _swift_arrayDownCastIndirect(source, destination,
                                   sourceArgs[0], targetArgs[0]);
      result = true;
    } else {
      result =
        _swift_arrayDownCastConditionalIndirect(source, destination,
                                                sourceArgs[0], targetArgs[0]);
    }

  // Dictionaries.
  } else if (descriptor == &NOMINAL_TYPE_DESCR_SYM(SD)) {
    if (flags & DynamicCastFlags::Unconditional) {
      _swift_dictionaryDownCastIndirect(source, destination,
                                        sourceArgs[0], sourceArgs[1],
                                        targetArgs[0], targetArgs[1],
                                        sourceArgs[2], targetArgs[2]);
      result = true;
    } else {
      result =
        _swift_dictionaryDownCastConditionalIndirect(source, destination,
                                        sourceArgs[0], sourceArgs[1],
                                        targetArgs[0], targetArgs[1],
                                        sourceArgs[2], targetArgs[2]);
    }

  // Sets.
  } else if (descriptor == &NOMINAL_TYPE_DESCR_SYM(Sh)) {
    if (flags & DynamicCastFlags::Unconditional) {
      _swift_setDownCastIndirect(source, destination,
                                 sourceArgs[0], targetArgs[0],
                                 sourceArgs[1], targetArgs[1]);
      result = true;
    } else {
      result =
        _swift_setDownCastConditionalIndirect(source, destination,
                                              sourceArgs[0], targetArgs[0],
                                              sourceArgs[1], targetArgs[1]);
    }

  // Other struct types don't support dynamic covariance for now.
  } else {
    return _fail(source, sourceType, targetType, flags);
  }

  // The intrinsics above never consume the source value.
  bool shouldDestroySource =
    result ? flags & DynamicCastFlags::TakeOnSuccess
           : flags & DynamicCastFlags::DestroyOnFailure;
  if (shouldDestroySource) {
    sourceType->vw_destroy(source);
  }

  return result;
}

static bool _dynamicCastTupleToTuple(OpaqueValue *destination,
                                     OpaqueValue *source,
                                     const TupleTypeMetadata *sourceType,
                                     const TupleTypeMetadata *targetType,
                                     DynamicCastFlags flags) {
  assert(sourceType != targetType &&
         "Caller should handle exact tuple matches");


  // Simple case: number of elements mismatches.
  if (sourceType->NumElements != targetType->NumElements)
    return _fail(source, sourceType, targetType, flags);

  // Check that the elements line up.
  const char *sourceLabels = sourceType->Labels;
  const char *targetLabels = targetType->Labels;
  bool anyTypeMismatches = false;
  for (unsigned i = 0, n = sourceType->NumElements; i != n; ++i) {
    // Check the label, if there is one.
    if (sourceLabels && targetLabels && sourceLabels != targetLabels) {
      const char *sourceSpace = strchr(sourceLabels, ' ');
      const char *targetSpace = strchr(targetLabels, ' ');

      // If both have labels, and the labels mismatch, we fail.
      if (sourceSpace && sourceSpace != sourceLabels &&
          targetSpace && targetSpace != targetLabels) {
        unsigned sourceLen = sourceSpace - sourceLabels;
        unsigned targetLen = targetSpace - targetLabels;
        if (sourceLen != targetLen ||
            strncmp(sourceLabels, targetLabels, sourceLen) != 0)
          return _fail(source, sourceType, targetType, flags);
      }

      sourceLabels = sourceSpace ? sourceSpace + 1 : nullptr;
      targetLabels = targetSpace ? targetSpace + 1 : nullptr;
    }

    // If the types don't match exactly, make a note of it. We'll try to
    // convert them in a second pass.
    if (sourceType->getElement(i).Type != targetType->getElement(i).Type)
      anyTypeMismatches = true;
  }

  // If there were no type mismatches, the only difference was in the argument
  // labels. We can directly map from the source to the destination type.
  if (!anyTypeMismatches)
    return _succeed(destination, source, targetType, flags);

  // Determine how the individual elements will get casted.
  // If both success and failure will destroy the source, then
  // we can be destructively cast each element. Otherwise, we'll have to
  // manually handle them.
  const DynamicCastFlags alwaysDestroySourceFlags =
    DynamicCastFlags::TakeOnSuccess | DynamicCastFlags::DestroyOnFailure;
  bool alwaysDestroysSource =
    (static_cast<DynamicCastFlags>(flags & alwaysDestroySourceFlags)
       == alwaysDestroySourceFlags);
  DynamicCastFlags elementFlags = flags;
  if (!alwaysDestroysSource)
    elementFlags = elementFlags - alwaysDestroySourceFlags;

  // Local function to destroy the elements in the range [start, end).
  auto destroyRange = [](const TupleTypeMetadata *type, OpaqueValue *value,
                        unsigned start, unsigned end) {
    assert(start <= end && "invalid range in destroyRange");
    for (unsigned i = start; i != end; ++i) {
      const auto &elt = type->getElement(i);
      elt.Type->vw_destroy(elt.findIn(value));
    }
  };

  // Cast each element.
  for (unsigned i = 0, n = sourceType->NumElements; i != n; ++i) {
    // Cast the element. If it succeeds, keep going.
    const auto &sourceElt = sourceType->getElement(i);
    const auto &targetElt = targetType->getElement(i);
    if (swift_dynamicCast(targetElt.findIn(destination),
                          sourceElt.findIn(source),
                          sourceElt.Type, targetElt.Type, elementFlags))
      continue;

    // Casting failed, so clean up.

    // Destroy all of the elements that got casted into the destination buffer.
    destroyRange(targetType, destination, 0, i);

    // If we're supposed to destroy on failure, destroy any elements from the
    // source buffer that haven't been destroyed/taken from yet.
    if (flags & DynamicCastFlags::DestroyOnFailure)
      destroyRange(sourceType, source, alwaysDestroysSource ? i : 0, n);

    // If an unconditional cast failed, complain.
    if (flags & DynamicCastFlags::Unconditional)
      swift_dynamicCastFailure(sourceType, targetType);
    return false;
  }

  // Casting succeeded.

  // If we were supposed to take on success from the source buffer but couldn't
  // before, destroy the source buffer now.
  if (!alwaysDestroysSource && (flags & DynamicCastFlags::TakeOnSuccess))
    destroyRange(sourceType, source, 0, sourceType->NumElements);

  return true;
}

/******************************************************************************/
/****************************** Main Entrypoint *******************************/
/******************************************************************************/

/// Perform a dynamic cast to an arbitrary type.

static bool swift_dynamicCastImpl(OpaqueValue *dest, OpaqueValue *src,
                                  const Metadata *srcType,
                                  const Metadata *targetType,
                                  DynamicCastFlags flags) {
  auto unwrapResult = checkDynamicCastFromOptional(dest, src, srcType,
                                                   targetType, flags);
  srcType = unwrapResult.payloadType;
  if (!srcType)
    return unwrapResult.success;

  // A class or AnyObject reference may point to a __SwiftValue box.
  {
    auto innerFlags = flags - DynamicCastFlags::Unconditional
                            - DynamicCastFlags::DestroyOnFailure;
    if (tryDynamicCastBoxedSwiftValue(dest, src, srcType,
                                      targetType, innerFlags)) {
      // TakeOnSuccess was handled inside tryDynamicCastBoxedSwiftValue().
      return true;
    } else {
      // Couldn't cast boxed value to targetType.
      // Fall through and try to cast the __SwiftValue box itself to targetType.
      // (for example, casting __SwiftValue to NSObject will be successful)
    }
  }

  switch (targetType->getKind()) {
  // Handle wrapping an Optional target.
  case MetadataKind::Optional: {
    // If the source is an existential, attempt to cast it first without
    // unwrapping the target. This handles an optional source wrapped within an
    // existential that Optional conforms to (Any).
    if (auto srcExistentialType = dyn_cast<ExistentialTypeMetadata>(srcType)) {
      // If coming from AnyObject, we may want to bridge.
      if (isAnyObjectExistentialType(srcExistentialType)) {
        if (auto targetBridgeWitness = findBridgeWitness(targetType)) {
          return _dynamicCastClassToValueViaObjCBridgeable(dest, src, srcType,
                                                           targetType,
                                                           targetBridgeWitness,
                                                           flags);
        }
      }
      return _dynamicCastFromExistential(dest, src, srcExistentialType,
                                         targetType, flags);
    }
    // Recursively cast into the layout compatible payload area.
    const Metadata *payloadType =
      cast<EnumMetadata>(targetType)->getGenericArgs()[0];
    if (swift_dynamicCast(dest, src, srcType, payloadType, flags)) {
      payloadType->vw_storeEnumTagSinglePayload(dest, /*case*/ 0,
                                                /*emptyCases*/ 1);
      return true;
    }
    return false;
  }

  // Casts to class type.
  case MetadataKind::Class:
  case MetadataKind::ObjCClassWrapper:
  case MetadataKind::ForeignClass:
    switch (srcType->getKind()) {
    case MetadataKind::Class:
    case MetadataKind::ObjCClassWrapper:
    case MetadataKind::ForeignClass: {
      // Do a dynamic cast on the instance pointer.
      void *object = *reinterpret_cast<void * const *>(src);
      return _dynamicCastUnknownClassIndirect(dest, object,
                                              targetType, flags);
    }

    case MetadataKind::Existential: {
      auto srcExistentialType = cast<ExistentialTypeMetadata>(srcType);
      return _dynamicCastToUnknownClassFromExistential(dest, src,
                                                       srcExistentialType,
                                                       targetType, flags);
    }

    case MetadataKind::Struct:
      // If the source type is AnyHashable, cast from that.
      if (isAnyHashableType(cast<StructMetadata>(srcType))) {
        return _dynamicCastFromAnyHashable(dest, src, srcType,
                                           targetType, flags);
      }
      LLVM_FALLTHROUGH;

    case MetadataKind::Enum:
    case MetadataKind::Optional: {
      // If the source type is bridged to Objective-C, try to bridge.
      if (auto srcBridgeWitness = findBridgeWitness(srcType)) {
        return _dynamicCastValueToClassViaObjCBridgeable(dest, src, srcType,
                                                         targetType,
                                                         srcBridgeWitness,
                                                         flags);
      }

#if SWIFT_OBJC_INTEROP
      // If the destination type is an NSError or NSObject, and the source type
      // is an Error, then the cast can succeed by NSError bridging.
      if (targetType == getNSErrorMetadata() ||
          targetType == getNSObjectMetadata()) {
        if (auto srcErrorWitness = findErrorWitness(srcType)) {
          auto error = dynamicCastValueToNSError(src, srcType,
                                                 srcErrorWitness, flags);
          *reinterpret_cast<id *>(dest) = error;
          return true;
        }
      }
#endif

      return _fail(src, srcType, targetType, flags);
    }

    default:
      return _fail(src, srcType, targetType, flags);
    }
    break;

  case MetadataKind::Existential:
    return _dynamicCastToExistential(dest, src, srcType,
                                     cast<ExistentialTypeMetadata>(targetType),
                                     flags);

  case MetadataKind::Metatype:
    return _dynamicCastToMetatype(dest, src, srcType,
                                  cast<MetatypeMetadata>(targetType),
                                  flags);
    
  case MetadataKind::ExistentialMetatype:
    return _dynamicCastToExistentialMetatype(dest, src, srcType,
                                 cast<ExistentialMetatypeMetadata>(targetType),
                                             flags);

  // Function types.
  case MetadataKind::Function: {
    return _dynamicCastToFunction(dest, src, srcType,
                                  cast<FunctionTypeMetadata>(targetType),
                                  flags);
  }

  case MetadataKind::Struct:
  case MetadataKind::Enum:
    switch (srcType->getKind()) {
    case MetadataKind::Class:
    case MetadataKind::ObjCClassWrapper:
    case MetadataKind::ForeignClass: {
      // Casts to AnyHashable.
      if (isAnyHashableType(targetType)) {
        return _dynamicCastToAnyHashable(dest, src, srcType, targetType, flags);
      }

      // If the target type is bridged to Objective-C, try to bridge.
      if (auto targetBridgeWitness = findBridgeWitness(targetType)) {
        return _dynamicCastClassToValueViaObjCBridgeable(dest, src, srcType,
                                                         targetType,
                                                         targetBridgeWitness,
                                                         flags);
      }
      
#if SWIFT_OBJC_INTEROP
      // If the source is an NSError, and the target is a bridgeable
      // Error, try to bridge.
      auto innerFlags = flags - DynamicCastFlags::Unconditional
                              - DynamicCastFlags::DestroyOnFailure;
      if (tryDynamicCastNSErrorToValue(dest, src, srcType, targetType,
                                       innerFlags)) {
        return true;
      }
#endif

      break;
    }

    case MetadataKind::Struct:
      // Collection and AnyHashable casts.
      if (targetType->getKind() == MetadataKind::Struct) {
        return _dynamicCastStructToStruct(dest, src,
                                          cast<StructMetadata>(srcType),
                                          cast<StructMetadata>(targetType),
                                          flags);
      } else if (isAnyHashableType(srcType)) {
        // AnyHashable casts for enums.
        return _dynamicCastFromAnyHashable(dest, src, srcType, targetType,
                                           flags);
      }
      break;

    case MetadataKind::Optional:
    case MetadataKind::Enum:
      // Casts to AnyHashable.
      if (isAnyHashableType(targetType)) {
        return _dynamicCastToAnyHashable(dest, src, srcType, targetType, flags);
      }
      break;

    default:
      break;
    }

    LLVM_FALLTHROUGH;

  // The non-polymorphic types.
  default:
    // If there's an exact type match, we're done.
    if (srcType == targetType)
      return _succeed(dest, src, srcType, flags);

    // If we have an existential, look at its dynamic type.
    if (auto srcExistentialType = dyn_cast<ExistentialTypeMetadata>(srcType)) {
      return _dynamicCastFromExistential(dest, src, srcExistentialType,
                                         targetType, flags);
    }

    // If both are tuple types, allow the cast to add/remove labels.
    if (srcType->getKind() == MetadataKind::Tuple &&
        targetType->getKind() == MetadataKind::Tuple) {
      return _dynamicCastTupleToTuple(dest, src,
                                      cast<TupleTypeMetadata>(srcType),
                                      cast<TupleTypeMetadata>(targetType),
                                      flags);
    }

    // Otherwise, we have a failure.
    return _fail(src, srcType, targetType, flags);
  }
}

static inline bool swift_isClassOrObjCExistentialTypeImpl(const Metadata *T) {
  auto kind = T->getKind();
  // Classes.
  if (Metadata::isAnyKindOfClass(kind))
    return true;
#if SWIFT_OBJC_INTEROP
  // ObjC existentials.
  if (kind == MetadataKind::Existential &&
      static_cast<const ExistentialTypeMetadata *>(T)->isObjC())
    return true;
  
  // Blocks are ObjC objects.
  if (kind == MetadataKind::Function) {
    auto fT = static_cast<const FunctionTypeMetadata *>(T);
    return fT->getConvention() == FunctionMetadataConvention::Block;
  }
#endif

  return false;
}

/******************************************************************************/
/********************************** Bridging **********************************/
/******************************************************************************/

//===----------------------------------------------------------------------===//
// Bridging to and from Objective-C
//===----------------------------------------------------------------------===//

namespace {

// protocol _ObjectiveCBridgeable {
struct _ObjectiveCBridgeableWitnessTable : WitnessTable {
  static_assert(WitnessTableFirstRequirementOffset == 1,
                "Witness table layout changed");

  // associatedtype _ObjectiveCType : class
  void *_ObjectiveCType;

  // func _bridgeToObjectiveC() -> _ObjectiveCType
  SWIFT_CC
  HeapObject *(*bridgeToObjectiveC)(
                SWIFT_CONTEXT OpaqueValue *self, const Metadata *Self,
                const _ObjectiveCBridgeableWitnessTable *witnessTable);

  // class func _forceBridgeFromObjectiveC(x: _ObjectiveCType,
  //                                       inout result: Self?)
  SWIFT_CC
  void (*forceBridgeFromObjectiveC)(
         HeapObject *sourceValue,
         OpaqueValue *result,
         SWIFT_CONTEXT const Metadata *self,
         const Metadata *selfType,
         const _ObjectiveCBridgeableWitnessTable *witnessTable);

  // class func _conditionallyBridgeFromObjectiveC(x: _ObjectiveCType,
  //                                              inout result: Self?) -> Bool
  SWIFT_CC
  bool (*conditionallyBridgeFromObjectiveC)(
         HeapObject *sourceValue,
         OpaqueValue *result,
         SWIFT_CONTEXT const Metadata *self,
         const Metadata *selfType,
         const _ObjectiveCBridgeableWitnessTable *witnessTable);
};
// }

/// Retrieve the bridged Objective-C type for the given type that
/// conforms to \c _ObjectiveCBridgeable.
MetadataResponse _getBridgedObjectiveCType(
                             MetadataRequest request,
                             const Metadata *conformingType,
                             const _ObjectiveCBridgeableWitnessTable *wtable) {
  // FIXME: Can we directly reference the descriptor somehow?
  const ProtocolConformanceDescriptor *conformance = wtable->Description;
  const ProtocolDescriptor *protocol = conformance->getProtocol();
  auto assocTypeRequirement = protocol->getRequirements().begin();
  assert(assocTypeRequirement->Flags.getKind() ==
         ProtocolRequirementFlags::Kind::AssociatedTypeAccessFunction);
  auto mutableWTable = (WitnessTable *)wtable;
  return swift_getAssociatedTypeWitness(
                                      request, mutableWTable, conformingType,
                                      protocol->getRequirementBaseDescriptor(),
                                      assocTypeRequirement);
}
  
} // unnamed namespace

extern "C" const ProtocolDescriptor PROTOCOL_DESCR_SYM(s21_ObjectiveCBridgeable);

/// Dynamic cast from a value type that conforms to the _ObjectiveCBridgeable
/// protocol to a class type, first by bridging the value to its Objective-C
/// object representation and then by dynamic casting that object to the
/// resulting target type.
static bool _dynamicCastValueToClassViaObjCBridgeable(
               OpaqueValue *dest,
               OpaqueValue *src,
               const Metadata *srcType,
               const Metadata *targetType,
               const _ObjectiveCBridgeableWitnessTable *srcBridgeWitness,
               DynamicCastFlags flags) {
  // Bridge the source value to an object.
  auto srcBridgedObject =
    srcBridgeWitness->bridgeToObjectiveC(src, srcType, srcBridgeWitness);

  // Dynamic cast the object to the resulting class type.
  bool success;
  if (auto cast = _dynamicCastUnknownClass(srcBridgedObject, targetType,
                               flags & DynamicCastFlags::Unconditional)) {
    *reinterpret_cast<void **>(dest) = cast;
    success = true;
  } else {
    // We don't need the object anymore.
    swift_unknownObjectRelease(srcBridgedObject);
    success = false;
  }

  // Clean up the source if we're supposed to.
  if (shouldDeallocateSource(success, flags)) {
    srcType->vw_destroy(src);
  }

  // We're done.
  return success;
}

/// Dynamic cast from a value type that conforms to the
/// _ObjectiveCBridgeable protocol to a class-bounded existential,
/// first by bridging the value to its Objective-C object
/// representation and then by dynamic-casting that object to the
/// resulting target type.
static bool _dynamicCastValueToClassExistentialViaObjCBridgeable(
              OpaqueValue *dest,
              OpaqueValue *src,
              const Metadata *srcType,
              const ExistentialTypeMetadata *targetType,
              const _ObjectiveCBridgeableWitnessTable *srcBridgeWitness,
              DynamicCastFlags flags) {
  // Bridge the source value to an object.
  auto srcBridgedObject =
    srcBridgeWitness->bridgeToObjectiveC(src, srcType, srcBridgeWitness);

  // Try to cast the object to the destination existential.
  DynamicCastFlags subFlags = DynamicCastFlags::TakeOnSuccess
                            | DynamicCastFlags::DestroyOnFailure;
  if (flags & DynamicCastFlags::Unconditional)
    subFlags |= DynamicCastFlags::Unconditional;
  bool success = _dynamicCastToExistential(
                   dest, 
                   (OpaqueValue *)&srcBridgedObject,
                   swift_getObjectType(srcBridgedObject),
                   targetType,
                   subFlags);

  // Clean up the source if we're supposed to.
  if (shouldDeallocateSource(success, flags)) {
    srcType->vw_destroy(src);
  }

  // We're done.
  return success;
}

/// Dynamic cast from a class type to a value type that conforms to the
/// _ObjectiveCBridgeable, first by dynamic casting the object to the
/// Objective-C class to which the value type is bridged, and then bridging
/// from that object to the value type via the witness table.
static bool _dynamicCastClassToValueViaObjCBridgeable(
               OpaqueValue *dest,
               OpaqueValue *src,
               const Metadata *srcType,
               const Metadata *targetType,
               const _ObjectiveCBridgeableWitnessTable *targetBridgeWitness,
               DynamicCastFlags flags) {
  // Determine the class type to which the target value type is bridged.
  auto targetBridgedClass =
      _getBridgedObjectiveCType(MetadataState::Complete, targetType,
                                targetBridgeWitness).Value;

  // Dynamic cast the source object to the class type to which the target value
  // type is bridged. If we succeed, we can bridge from there; if we fail,
  // there's nothing more to do.
  void *srcObject = *reinterpret_cast<void * const *>(src);
  if (!_dynamicCastUnknownClass(srcObject,
                                targetBridgedClass,
                                flags & DynamicCastFlags::Unconditional)) {
    return _fail(src, srcType, targetType, flags);
  }

  // TODO: Avoid the retain here in +0 mode if the input is never consumed.
  bool alwaysConsumeSrc = (flags & DynamicCastFlags::TakeOnSuccess) &&
                          (flags & DynamicCastFlags::DestroyOnFailure);
  if (!alwaysConsumeSrc) {
    swift_unknownObjectRetain(srcObject);
  }

  // The extra byte is for the tag.
  auto targetSize = targetType->getValueWitnesses()->getSize() + 1;
  auto targetAlignMask = targetType->getValueWitnesses()->getAlignmentMask();

  // Object that frees a buffer when it goes out of scope.
  struct FreeBuffer {
    void *Buffer = nullptr;
    size_t size, alignMask;
    FreeBuffer(size_t size, size_t alignMask) :
      size(size), alignMask(alignMask) {}

    ~FreeBuffer() {
      if (Buffer)
        swift_slowDealloc(Buffer, size, alignMask);
    }
  } freeBuffer{targetSize, targetAlignMask};

  // Allocate a buffer to store the T? returned by bridging.
  // The extra byte is for the tag.
  const std::size_t inlineValueSize = 3 * sizeof(void*);
  alignas(std::max_align_t) char inlineBuffer[inlineValueSize + 1];
  void *optDestBuffer;
  if (targetType->getValueWitnesses()->getStride() <= inlineValueSize) {
    // Use the inline buffer.
    optDestBuffer = inlineBuffer;
  } else {
    // Allocate a buffer.
    optDestBuffer = swift_slowAlloc(targetSize, targetAlignMask);
    freeBuffer.Buffer = optDestBuffer;
  }

  // Initialize the buffer as an empty optional.
  targetType->vw_storeEnumTagSinglePayload((OpaqueValue *)optDestBuffer,
                                           1, 1);

  // Perform the bridging operation.
  bool success;
  if (flags & DynamicCastFlags::Unconditional) {
    // For an unconditional dynamic cast, use forceBridgeFromObjectiveC.
    targetBridgeWitness->forceBridgeFromObjectiveC(
      (HeapObject *)srcObject, (OpaqueValue *)optDestBuffer,
      targetType, targetType, targetBridgeWitness);
    success = true;
  } else {
    // For a conditional dynamic cast, use conditionallyBridgeFromObjectiveC.
    success = targetBridgeWitness->conditionallyBridgeFromObjectiveC(
                (HeapObject *)srcObject, (OpaqueValue *)optDestBuffer,
                targetType, targetType, targetBridgeWitness);
  }
  swift_unknownObjectRelease(srcObject);

  // If we succeeded, take from the optional buffer into the
  // destination buffer.
  if (success) {
    targetType->vw_initializeWithTake(dest, (OpaqueValue *)optDestBuffer);
  }

  if (!alwaysConsumeSrc && shouldDeallocateSource(success, flags)) {
    swift_unknownObjectRelease(srcObject);
  }

  return success;
}

#if !SWIFT_OBJC_INTEROP // __SwiftValue is a native class:



#endif // !_SWIFT_OBJC_INTEROP

#if SWIFT_OBJC_INTEROP
static id bridgeAnythingNonVerbatimToObjectiveC(OpaqueValue *src,
                                                const Metadata *srcType,
                                                bool consume) {
  // We can always bridge objects verbatim.
  if (srcType->isAnyClass()) {
    id result;
    memcpy(&result, src, sizeof(id));
    if (!consume)
      swift_unknownObjectRetain(result);
    return result;
  }
  
  // Dig through existential types.
  if (auto srcExistentialTy = dyn_cast<ExistentialTypeMetadata>(srcType)) {
    OpaqueValue *srcInnerValue;
    const Metadata *srcInnerType;
    bool isOutOfLine;
    bool canTake;
    
    unwrapExistential(src, srcExistentialTy,
                      srcInnerValue, srcInnerType, isOutOfLine, canTake);
    auto result = bridgeAnythingNonVerbatimToObjectiveC(srcInnerValue,
                                                        srcInnerType,
                                                        consume && canTake);
    // Clean up the existential, or its remains after taking the value from
    // it.
    if (consume) {
      if (canTake) {
        if (isOutOfLine) {
          // Copy-on-write existentials share boxed and can't be 'take'n out of
          // without a uniqueness check (which we currently don't do).
          swift::fatalError(
              0 /* flags */,
              "Attempting to move out of a copy-on-write existential");
        }
      } else {
        // We didn't take the value, so clean up the existential value.
        srcType->vw_destroy(src);
      }
    }
    return result;
  }
  
  // Handle metatypes.
  if (isa<ExistentialMetatypeMetadata>(srcType)
      || isa<MetatypeMetadata>(srcType)) {
    const Metadata *srcMetatypeValue;
    memcpy(&srcMetatypeValue, src, sizeof(srcMetatypeValue));
    
    // Class metatypes bridge to their class object.
    if (isa<ClassMetadata>(srcMetatypeValue)
        || isa<ObjCClassWrapperMetadata>(srcMetatypeValue)) {
      return (id)srcMetatypeValue->getObjCClassObject();
    
    // ObjC protocols bridge to their Protocol object.
    } else if (auto existential
               = dyn_cast<ExistentialTypeMetadata>(srcMetatypeValue)) {
      if (existential->isObjC() && existential->NumProtocols == 1) {
        // Though they're statically-allocated globals, Protocol inherits
        // NSObject's default refcounting behavior so must be retained.
        auto protocolObj = existential->getProtocols()[0].getObjCProtocol();
        return objc_retain(protocolObj);
      }
    }
  // Handle bridgable types.
  } else if (auto srcBridgeWitness = findBridgeWitness(srcType)) {
    // Bridge the source value to an object.
    auto srcBridgedObject =
      srcBridgeWitness->bridgeToObjectiveC(src, srcType, srcBridgeWitness);

    // Consume if the source object was passed in +1.
    if (consume)
      srcType->vw_destroy(src);

    return (id)srcBridgedObject;
  // Handle Errors.
  } else if (auto srcErrorWitness = findErrorWitness(srcType)) {
    // Bridge the source value to an NSError.
    auto flags = consume ? DynamicCastFlags::TakeOnSuccess
                         : DynamicCastFlags::Default;
    return dynamicCastValueToNSError(src, srcType, srcErrorWitness, flags);
  }

  // Fall back to boxing.
  return (id)bridgeAnythingToSwiftValueObject(src, srcType, consume);
}

/// public
/// func _bridgeAnythingNonVerbatimToObjectiveC<T>(_ x: __owned T) -> AnyObject
///
/// Called by inlined stdlib code.
#define _bridgeAnythingNonVerbatimToObjectiveC                                 \
  MANGLE_SYM(s38_bridgeAnythingNonVerbatimToObjectiveCyyXlxnlF)
SWIFT_CC SWIFT_RUNTIME_STDLIB_API
id _bridgeAnythingNonVerbatimToObjectiveC(OpaqueValue *src,
                                          const Metadata *srcType) {
  bool shouldConsume = true;
  return bridgeAnythingNonVerbatimToObjectiveC(src, srcType,
                                               /*consume*/shouldConsume);
}
#endif

//===--- Bridging helpers for the Swift stdlib ----------------------------===//
// Functions that must discover and possibly use an arbitrary type's
// conformance to a given protocol.  See ../core/BridgeObjectiveC.swift for
// documentation.
//===----------------------------------------------------------------------===//

#if SWIFT_OBJC_INTEROP
#define BRIDGING_CONFORMANCE_SYM \
  MANGLE_SYM(s19_BridgeableMetatypeVs21_ObjectiveCBridgeablesWP)

extern "C" const _ObjectiveCBridgeableWitnessTable BRIDGING_CONFORMANCE_SYM;
#endif

static const _ObjectiveCBridgeableWitnessTable *
findBridgeWitness(const Metadata *T) {
  auto w = swift_conformsToProtocol(T,
                                &PROTOCOL_DESCR_SYM(s21_ObjectiveCBridgeable));
  if (LLVM_LIKELY(w))
    return reinterpret_cast<const _ObjectiveCBridgeableWitnessTable *>(w);
  // Class and ObjC existential metatypes can be bridged, but metatypes can't
  // directly conform to protocols yet. Use a stand-in conformance for a type
  // that looks like a metatype value if the metatype can be bridged.
  switch (T->getKind()) {
  case MetadataKind::Metatype: {
#if SWIFT_OBJC_INTEROP
    auto metaTy = static_cast<const MetatypeMetadata *>(T);
    if (metaTy->InstanceType->isAnyClass())
      return &BRIDGING_CONFORMANCE_SYM;
#endif
    break;
  }
  case MetadataKind::ExistentialMetatype: {
#if SWIFT_OBJC_INTEROP
    auto existentialMetaTy =
      static_cast<const ExistentialMetatypeMetadata *>(T);
    if (existentialMetaTy->isObjC())
      return &BRIDGING_CONFORMANCE_SYM;
#endif
    break;
  }

  default:
    break;
  }
  return nullptr;
}

// public func _getBridgedNonVerbatimObjectiveCType<T>(_: T.Type) -> Any.Type?
// Called by inlined stdlib code.
#define _getBridgedNonVerbatimObjectiveCType \
  MANGLE_SYM(s36_getBridgedNonVerbatimObjectiveCTypeyypXpSgxmlF)
SWIFT_CC SWIFT_RUNTIME_STDLIB_API
const Metadata *_getBridgedNonVerbatimObjectiveCType(
  const Metadata *value, const Metadata *T
) {
  // Classes and Objective-C existentials bridge verbatim.
  assert(!swift_isClassOrObjCExistentialTypeImpl(T));

  // Check if the type conforms to _BridgedToObjectiveC, in which case
  // we'll extract its associated type.
  if (const auto *bridgeWitness = findBridgeWitness(T)) {
    return _getBridgedObjectiveCType(MetadataState::Complete, T,
                                     bridgeWitness).Value;
  }
  
  return nullptr;
}

#if SWIFT_OBJC_INTEROP

// @_silgen_name("_bridgeNonVerbatimFromObjectiveCToAny")
// func _bridgeNonVerbatimFromObjectiveCToAny(
//     x: AnyObject,
//     inout result: Any?
// )
SWIFT_CC SWIFT_RUNTIME_STDLIB_INTERNAL
void
_bridgeNonVerbatimFromObjectiveCToAny(HeapObject *sourceValue,
                                      OpaqueValue *destValue);

// @_silgen_name("_bridgeNonVerbatimBoxedValue")
// func _bridgeNonVerbatimBoxedValue<NativeType>(
//     x: UnsafePointer<NativeType>,
//     inout result: NativeType?
// )
SWIFT_CC SWIFT_RUNTIME_STDLIB_INTERNAL
void
_bridgeNonVerbatimBoxedValue(const OpaqueValue *sourceValue,
                             OpaqueValue *destValue,
                             const Metadata *nativeType);

// Try bridging by conversion to Any or boxing if applicable.
static bool tryBridgeNonVerbatimFromObjectiveCUniversal(
  HeapObject *sourceValue,
  const Metadata *nativeType,
  OpaqueValue *destValue
) {
  // If the type is the Any type, we can bridge by "upcasting" the object
  // to Any.
  if (auto nativeExistential = dyn_cast<ExistentialTypeMetadata>(nativeType)) {
    if (nativeExistential->NumProtocols == 0 &&
        !nativeExistential->isClassBounded()) {
      _bridgeNonVerbatimFromObjectiveCToAny(sourceValue, destValue);
      return true;
    }
  }
  // Check if the value is a box containing a value of the desired type.
  if (auto srcBox = getAsSwiftValue((id)sourceValue)) {
    const Metadata *sourceType;
    const OpaqueValue *sourceBoxedValue;
    
    std::tie(sourceType, sourceBoxedValue) = getValueFromSwiftValue(srcBox);
    if (sourceType == nativeType) {
      _bridgeNonVerbatimBoxedValue(sourceBoxedValue, destValue, nativeType);
      return true;
    }
  }
  // Try to bridge NSError to Error.
  if (tryDynamicCastNSErrorObjectToValue(sourceValue, destValue, nativeType,
                                         DynamicCastFlags::Default)) {
    return true;
  }

  
  return false;
}

// func _bridgeNonVerbatimFromObjectiveC<T>(
//     _ x: AnyObject,
//     _ nativeType: T.Type
//     _ inout result: T?
// )
// Called by inlined stdlib code.
#define _bridgeNonVerbatimFromObjectiveC \
  MANGLE_SYM(s32_bridgeNonVerbatimFromObjectiveCyyyXl_xmxSgztlF)
SWIFT_CC SWIFT_RUNTIME_STDLIB_API
void
_bridgeNonVerbatimFromObjectiveC(
  HeapObject *sourceValue,
  const Metadata *nativeType,
  OpaqueValue *destValue,
  const Metadata *nativeType_
) {
  
  if (tryBridgeNonVerbatimFromObjectiveCUniversal(sourceValue, nativeType,
                                                  destValue))
    return;

  // Check if the type conforms to _BridgedToObjectiveC.
  if (const auto *bridgeWitness = findBridgeWitness(nativeType)) {
    // Check if sourceValue has the _ObjectiveCType type required by the
    // protocol.
    const Metadata *objectiveCType =
        _getBridgedObjectiveCType(MetadataState::Complete, nativeType,
                                  bridgeWitness).Value;
      
    auto sourceValueAsObjectiveCType =
        const_cast<void*>(swift_dynamicCastUnknownClass(sourceValue,
                                                        objectiveCType));
      
    if (!sourceValueAsObjectiveCType) {
      swift::swift_dynamicCastFailure(_swift_getClass(sourceValue),
                                      objectiveCType);
    }

    // The type matches.  _forceBridgeFromObjectiveC returns `Self`, so
    // we can just return it directly.
    bridgeWitness->forceBridgeFromObjectiveC(
      static_cast<HeapObject*>(sourceValueAsObjectiveCType),
      destValue, nativeType, nativeType, bridgeWitness);
    return;
  }
  
  // Fail.
  swift::crash("value type is not bridged to Objective-C");
}

/// func _bridgeNonVerbatimFromObjectiveCConditional<T>(
///   _ x: AnyObject, _ nativeType: T.Type, _ result: inout T?) -> Bool
/// Called by inlined stdlib code.
#define _bridgeNonVerbatimFromObjectiveCConditional \
  MANGLE_SYM(s43_bridgeNonVerbatimFromObjectiveCConditionalySbyXl_xmxSgztlF)
SWIFT_CC SWIFT_RUNTIME_STDLIB_API
bool
_bridgeNonVerbatimFromObjectiveCConditional(
  HeapObject *sourceValue,
  const Metadata *nativeType,
  OpaqueValue *destValue,
  const Metadata *nativeType_
) {
  if (tryBridgeNonVerbatimFromObjectiveCUniversal(sourceValue, nativeType,
                                                  destValue))
    return true;

  // Local function that releases the source and returns false.
  auto fail = [&] () -> bool {
    return false;
  };
  
  // Check if the type conforms to _BridgedToObjectiveC.
  const auto *bridgeWitness = findBridgeWitness(nativeType);
  if (!bridgeWitness)
    return fail();

  // Dig out the Objective-C class type through which the native type
  // is bridged.
  const Metadata *objectiveCType =
    _getBridgedObjectiveCType(MetadataState::Complete, nativeType,
                              bridgeWitness).Value;
        
  // Check whether we can downcast the source value to the Objective-C
  // type.
  auto sourceValueAsObjectiveCType =
    const_cast<void*>(swift_dynamicCastUnknownClass(sourceValue, 
                                                    objectiveCType));
  if (!sourceValueAsObjectiveCType)
    return fail();

  // If the type also conforms to _ConditionallyBridgedToObjectiveC,
  // use conditional bridging.
  return bridgeWitness->conditionallyBridgeFromObjectiveC(
    static_cast<HeapObject*>(sourceValueAsObjectiveCType),
    destValue, nativeType, nativeType, bridgeWitness);
}

#endif // SWIFT_OBJC_INTEROP

// func _isBridgedNonVerbatimToObjectiveC<T>(_: T.Type) -> Bool
// Called by inlined stdlib code.
#define _isBridgedNonVerbatimToObjectiveC \
  MANGLE_SYM(s33_isBridgedNonVerbatimToObjectiveCySbxmlF)
SWIFT_CC SWIFT_RUNTIME_STDLIB_API
bool _isBridgedNonVerbatimToObjectiveC(const Metadata *value,
                                       const Metadata *T) {
  assert(!swift_isClassOrObjCExistentialTypeImpl(T));

  auto bridgeWitness = findBridgeWitness(T);
  return (bool)bridgeWitness;
}

// func _isClassOrObjCExistential<T>(x: T.Type) -> Bool
SWIFT_CC SWIFT_RUNTIME_STDLIB_API
bool _swift_isClassOrObjCExistentialType(const Metadata *value,
                                                    const Metadata *T) {
  return swift_isClassOrObjCExistentialTypeImpl(T);
}

SWIFT_CC SWIFT_RUNTIME_STDLIB_INTERNAL
const Metadata *swift::_swift_class_getSuperclass(const Metadata *theClass) {
  if (const ClassMetadata *classType = theClass->getClassObject()) {
    if (classHasSuperclass(classType))
      return getMetadataForClass(classType->Superclass);
  }

  if (const ForeignClassMetadata *foreignClassType
        = dyn_cast<ForeignClassMetadata>(theClass)) {
    if (const Metadata *superclass = foreignClassType->Superclass)
      return superclass;
  }

  return nullptr;
}

// Called by compiler-generated cast code.
SWIFT_RUNTIME_STDLIB_API
bool swift_isClassType(const Metadata *type) {
  return Metadata::isAnyKindOfClass(type->getKind());
}

// Called by compiler-generated code.
SWIFT_RUNTIME_STDLIB_API
bool swift_isOptionalType(const Metadata *type) {
  return type->getKind() == MetadataKind::Optional;
}

#if !SWIFT_OBJC_INTEROP
SWIFT_CC SWIFT_RUNTIME_STDLIB_INTERNAL
bool _swift_isOptional(OpaqueValue *src, const Metadata *type) {
  return swift_isOptionalType(type);
}

SWIFT_CC SWIFT_RUNTIME_STDLIB_SPI
HeapObject *_swift_extractDynamicValue(OpaqueValue *value, const Metadata *self) {
  OpaqueValue *outValue;
  const Metadata *outType;
  bool canTake = false;
  
  findDynamicValueAndType(value, self, outValue, outType, canTake,
                          /*isAnyObject*/ true,
                          /*isExistentialMetatype*/ true);

  if (!outType || (outType != self && outType->isAnyClass())) {
    HeapObject *object = *(reinterpret_cast<HeapObject**>(outValue));
    swift_retain(object);
    return object;
  }
  
  return nullptr;
}

SWIFT_CC SWIFT_RUNTIME_STDLIB_INTERNAL
HeapObject *_swift_bridgeToObjectiveCUsingProtocolIfPossible(
  OpaqueValue *src, const Metadata *srcType) {
  assert(!swift_isClassOrObjCExistentialTypeImpl(srcType));
  
  OpaqueValue *outValue;
  const Metadata *outType;
  bool canTake = false;
  
  findDynamicValueAndType(src, srcType, outValue, outType, canTake,
                          /*isAnyObject*/ false,
                          /*isExistentialMetatype*/ true);
  
  auto bridgeWitness = findBridgeWitness(outType);
  if (bridgeWitness) {
    auto bridgedObject =
      bridgeWitness->bridgeToObjectiveC(outValue, outType, bridgeWitness);
    return bridgedObject;
  } else {
    return nullptr;
  }
}
#endif

#define OVERRIDE_CASTING COMPATIBILITY_OVERRIDE
#include "CompatibilityOverride.def"

