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
// Miscellaneous dynamic cast runtime functions.
// The general-purpose swift_dynamicCast implementation is in DynamicCast.cpp
//
//===----------------------------------------------------------------------===//

#include "swift/Runtime/Casting.h"
#include "../CompatibilityOverride/CompatibilityOverride.h"
#include "ErrorObject.h"
#include "ExistentialMetadataImpl.h"
#include "Private.h"
#include "SwiftHashableSupport.h"
#include "swift/Basic/Lazy.h"
#include "swift/Basic/Unreachable.h"
#include "swift/Demangling/Demangler.h"
#include "swift/Runtime/Config.h"
#include "swift/Runtime/Debug.h"
#include "swift/Runtime/Enum.h"
#include "swift/Runtime/ExistentialContainer.h"
#include "swift/Runtime/HeapObject.h"
#include "swift/Runtime/Metadata.h"
#include "swift/shims/GlobalObjects.h"
#include "swift/shims/RuntimeShims.h"
#include "swift/Threading/Mutex.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/PointerIntPair.h"
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

#if SWIFT_STDLIB_HAS_TYPE_PRINTING

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

std::string MetadataOrPack::nameForMetadata() const {
  if (isNull())
    return "<<nullptr>>";

  if (isMetadata())
    return ::nameForMetadata(getMetadata());

  std::string result = "Pack{";
  MetadataPackPointer pack = getMetadataPack();
  for (size_t i = 0, e = pack.getNumElements(); i < e; ++i) {
    if (i != 0)
      result += ", ";
    result += ::nameForMetadata(pack.getElements()[i]);
  }
  result += "}";

  return result;
}

#else // SWIFT_STDLIB_HAS_TYPE_PRINTING

std::string swift::nameForMetadata(const Metadata *type, bool qualified) {
  return "<<< type printer not available >>>";
}

std::string MetadataOrPack::nameForMetadata() const {
  return "<<< type printer not available >>>";
}

#endif // SWIFT_STDLIB_HAS_TYPE_PRINTING

/// Used as part of cache key for `TypeNameCache`.
enum class TypeNameKind {
  NotQualified,
  Qualified,
  Mangled,
};

using TypeNameCacheKey = llvm::PointerIntPair<const Metadata *, 2, TypeNameKind>;

static LazyMutex TypeNameCacheLock;
static LazyMutex MangledToPrettyFunctionNameCacheLock;

/// Cache containing rendered names for Metadata.
/// Access MUST be protected using `TypeNameCacheLock`.
static Lazy<llvm::DenseMap<TypeNameCacheKey, std::pair<const char *, size_t>>>
  TypeNameCache;

/// Cache containing rendered human-readable names for incoming mangled names.
static Lazy<llvm::DenseMap<llvm::StringRef, std::pair<const char *, size_t>>>
/// Access MUST be protected using `MangledToPrettyFunctionNameCache`.
  MangledToPrettyFunctionNameCache;

TypeNamePair
swift::swift_getTypeName(const Metadata *type, bool qualified) {
  TypeNameCacheKey key = TypeNameCacheKey(type, qualified ? TypeNameKind::Qualified: TypeNameKind::NotQualified);
  auto &cache = TypeNameCache.get();

  // Attempt read-only lookup of cache entry.
  {
    LazyMutex::ScopedLock guard(TypeNameCacheLock);

    auto found = cache.find(key);
    if (found != cache.end()) {
      auto result = found->second;
      return TypeNamePair{result.first, result.second};
    }
  }

  // Read-only lookup failed to find item, we may need to create it.
  {
    LazyMutex::ScopedLock guard(TypeNameCacheLock);

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

/// Return mangled name for the given type.
TypeNamePair
swift::swift_getMangledTypeName(const Metadata *type) {
  TypeNameCacheKey key(type, TypeNameKind::Mangled);
  auto &cache = TypeNameCache.get();

  // Attempt read-only lookup of cache entry.
  {
    LazyMutex::ScopedLock guard(TypeNameCacheLock);

    auto found = cache.find(key);
    if (found != cache.end()) {
      auto result = found->second;
      return TypeNamePair{result.first, result.second};
    }
  }

  // Read-only cache lookup failed, we may need to create it.
  {
    LazyMutex::ScopedLock guard(TypeNameCacheLock);

    // Do lookup again just to make sure it wasn't created by another
    // thread before we acquired the write lock.
    auto found = cache.find(key);
    if (found != cache.end()) {
      auto result = found->second;
      return TypeNamePair{result.first, result.second};
    }

    // Build the mangled name.
    Demangle::Demangler Dem;
    auto demangling = _swift_buildDemanglingForMetadata(type, Dem);

    if (demangling == nullptr) {
      return TypeNamePair{NULL, 0};
    }
    auto mangling = Demangle::mangleNode(demangling);
    if (!mangling.isSuccess())
      return TypeNamePair{NULL, 0};
    std::string name = mangling.result();

    // Copy it to memory we can reference forever.
    auto size = name.size();
    auto result = (char *)malloc(size + 1);
    memcpy(result, name.data(), size);
    result[size] = 0;

    cache.insert({key, {result, size}});

    return TypeNamePair{result, size};
  }
}


TypeNamePair
swift::swift_getFunctionFullNameFromMangledName(
    const char *mangledNameStart, uintptr_t mangledNameLength) {
  llvm::StringRef mangledName(mangledNameStart, mangledNameLength);

  auto &cache = MangledToPrettyFunctionNameCache.get();
  // Attempt read-only lookup of cache entry.
  {
    LazyMutex::ScopedLock guard(MangledToPrettyFunctionNameCacheLock);

    auto found = cache.find(mangledName);
    if (found != cache.end()) {
      auto result = found->second;
      return TypeNamePair{result.first, result.second};
    }
  }

  for (char c : mangledName) {
    if (c >= '\x01' && c <= '\x1F')
      return TypeNamePair{nullptr, 0};
  }

  // Read-only lookup failed, we may need to demangle and cache the entry.
  // We have to copy the string to be able to refer to it "forever":
  auto copy = (char *)malloc(mangledNameLength);
  memcpy(copy, mangledNameStart, mangledNameLength);
  mangledName = StringRef(copy, mangledNameLength);

  std::string demangled;
  StackAllocatedDemangler<1024> Dem;
  NodePointer node = Dem.demangleSymbol(mangledName);
  if (!node) {
    return TypeNamePair{nullptr, 0};
  }

  // Form the demangled string from the node tree.
  node = node->findByKind(Demangle::Node::Kind::Function, /*maxDepth=*/3);
  if (!node || node->getNumChildren() < 3) {
    // we normally expect Class/Identifier/Type, but don't need `Type`
    return TypeNamePair{nullptr, 0};
  }

  // Class identifier:
  auto clazz = node->findByKind(Demangle::Node::Kind::Class, 1);
  if (clazz) {
    if (auto module = clazz->findByKind(Demangle::Node::Kind::Module, 1)) {
      demangled += module->getText();
      demangled += ".";
    }
    if (auto clazzIdent = clazz->findByKind(Demangle::Node::Kind::Identifier, 1)) {
      demangled += clazzIdent->getText();
      demangled += ".";
    }
  }

  // Function identifier:
  NodePointer funcIdent = nullptr; // node == Function
  for (size_t i = 0; i < node->getNumChildren(); ++i) {
    if (node->getChild(i)->getKind() == Demangle::Node::Kind::Identifier) {
      funcIdent = node->getChild(i);
    }
  }

  // We always expect to work with functions here and they must have idents
  if (!funcIdent) {
    return TypeNamePair{nullptr, 0};
  }
  assert(funcIdent->getKind() == Demangle::Node::Kind::Identifier);
  demangled += funcIdent->getText();
  demangled += "(";

  if (auto labelList = node->findByKind(Demangle::Node::Kind::LabelList, /*maxDepth=*/1)) {
    if (labelList->getNumChildren()) {
      size_t paramIdx = 0;
      while (paramIdx < labelList->getNumChildren()) {
        auto labelIdentifier = labelList->getChild(paramIdx++);
        if (labelIdentifier) {
          if (labelIdentifier->getKind() == Demangle::Node::Kind::Identifier) {
            demangled += labelIdentifier->getText();
            demangled += ":";
          } else if (labelIdentifier->getKind() ==
                     Demangle::Node::Kind::FirstElementMarker) {
            demangled += "_:";
          }
        }
      }
    } else if (auto argumentTuple = node->findByKind(
                   Demangle::Node::Kind::ArgumentTuple, /*maxDepth=*/5)) {
      // LabelList was empty.
        //
        // The function has no labels at all, but could have some parameters...
        // we need to check for their count, and render it as e.g. (::) for two
        // anonymous parameters.
        auto params = argumentTuple->getFirstChild();
        if (auto paramsType = params->getFirstChild()) {
          if (paramsType->getKind() != Demangle::Node::Kind::Tuple) {
            // was a single, unnamed, parameter
            demangled += "_:";
          } else {
            // there are a few parameters; find out how many
            while (params && params->getFirstChild() &&
                   params->getFirstChild()->getKind() !=
                       Demangle::Node::Kind::TupleElement) {
              params = params->getFirstChild();
            }
            if (params) {
              for (size_t i = 0; i < params->getNumChildren(); ++i) {
                demangled += "_:";
              }
            }
          }
        }
    }
  }
  demangled += ")";

  // We have to copy the string to be able to refer to it;
  auto size = demangled.size();
  auto result = (char *)malloc(size + 1);
  memcpy(result, demangled.data(), size);
  result[size] = 0; // 0-terminated string

  {
    LazyMutex::ScopedLock guard(MangledToPrettyFunctionNameCacheLock);

    cache.insert({mangledName, {result, size}});
    return TypeNamePair{result, size};
  }
}

/// Report a dynamic cast failure.
// This is noinline to preserve this frame in stack traces.
// We want "dynamicCastFailure" to appear in crash logs even we crash 
// during the diagnostic because some Metadata is invalid.
SWIFT_NORETURN SWIFT_NOINLINE void
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

SWIFT_NORETURN void swift::swift_dynamicCastFailure(const Metadata *sourceType,
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

#if !SWIFT_OBJC_INTEROP // __SwiftValue is a native class
SWIFT_CC(swift) SWIFT_RUNTIME_STDLIB_INTERNAL
bool swift_unboxFromSwiftValueWithType(OpaqueValue *source,
                                       OpaqueValue *result,
                                       const Metadata *destinationType);
/// Nominal type descriptor for Swift.__SwiftValue
extern "C" const ClassDescriptor NOMINAL_TYPE_DESCR_SYM(s12__SwiftValueC);
#endif

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

  auto srcType = _swift_getClassOfAllocated(object);

  if (_dynamicCastClassMetatype(srcType, targetType))
    return object;

#if !SWIFT_OBJC_INTEROP // __SwiftValue is a native class on Linux
  if (srcType->getKind() == MetadataKind::Class
      && targetType->getKind() == MetadataKind::Class) {
    auto srcClassType = cast<ClassMetadata>(srcType);
    auto srcDescr = srcClassType->getDescription();
    if (srcDescr == &NOMINAL_TYPE_DESCR_SYM(s12__SwiftValueC)) {
      auto srcValue = reinterpret_cast<OpaqueValue *>(&object);
      void *result;
      auto destLocation = reinterpret_cast<OpaqueValue *>(&result);
      if (swift_unboxFromSwiftValueWithType(srcValue, destLocation, targetType)) {
        swift_unknownObjectRelease(const_cast<void *>(object));
        return result;
      }
    }
  }
#endif

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
    auto witness = swift_conformsToProtocolCommon(type, protocol.getSwiftProtocol());
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

  case MetadataKind::ForeignReferenceType:
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

  case MetadataKind::ForeignReferenceType:  {
    outValue = value;
    outType = type;
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
    auto targetClassType = static_cast<const ForeignClassMetadata *>(targetType);
    return swift_dynamicCastForeignClass(object, targetClassType);
#else
    return nullptr;
#endif
  }

  // Foreign reference types don't support casting to parent/child types yet
  // (rdar://85881664&85881794).
  case MetadataKind::ForeignReferenceType: {
    return nullptr;
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

  // Foreign reference types don't support casting to parent/child types yet
  // (rdar://85881664&85881794).
  case MetadataKind::ForeignReferenceType: {
    return nullptr;
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
    SWIFT_FALLTHROUGH;
  case MetadataKind::Class:
    // The source value must also be a class; otherwise the cast fails.
    switch (sourceType->getKind()) {
    case MetadataKind::ObjCClassWrapper:
      // Get the actual class object.
      sourceType = static_cast<const ObjCClassWrapperMetadata*>(sourceType)
        ->Class;
      SWIFT_FALLTHROUGH;
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

    // Foreign reference types don't support casting to parent/child types yet
    // (rdar://85881664&85881794).
    case MetadataKind::ForeignReferenceType: {
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
      SWIFT_FALLTHROUGH;
    case MetadataKind::Class:
    case MetadataKind::ForeignClass:
      // Check if the source is a subclass of the target.
      if (swift_dynamicCastForeignClassMetatype(
            (const ClassMetadata*)sourceType,
              (const ClassMetadata*)targetType))
        return origSourceType;
      return nullptr;
    // Foreign reference types don't support casting to parent/child types yet
    // (rdar://85881664&85881794).
    case MetadataKind::ForeignReferenceType:
      return nullptr;
    default:
      return nullptr;
    }
    break;

  default:
    return nullptr;
  }

  swift_unreachable("Unhandled MetadataKind in switch.");
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
    SWIFT_FALLTHROUGH;
  case MetadataKind::Class:
    // The source value must also be a class; otherwise the cast fails.
    switch (sourceType->getKind()) {
    case MetadataKind::ObjCClassWrapper:
      // Get the actual class object.
      sourceType = static_cast<const ObjCClassWrapperMetadata*>(sourceType)
        ->Class;
      SWIFT_FALLTHROUGH;
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
      SWIFT_FALLTHROUGH;
    case MetadataKind::Class:
    case MetadataKind::ForeignClass:
      // Check if the source is a subclass of the target.
      swift_dynamicCastForeignClassMetatypeUnconditional(
                                            (const ClassMetadata*)sourceType,
                                            (const ClassMetadata*)targetType,
                                            file, line, column);
      // If we returned, then the cast succeeded.
      return origSourceType;

    // Foreign reference types don't support casting to parent/child types yet
    // (rdar://85881664&85881794).
    case MetadataKind::ForeignReferenceType:
    default:
      swift_dynamicCastFailure(sourceType, targetType);
    }
    break;

  // Foreign reference types don't support casting to parent/child types yet
  // (rdar://85881664&85881794).
  case MetadataKind::ForeignReferenceType: {
    swift_dynamicCastFailure(sourceType, targetType);
  }

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
/******************************** Existentials ********************************/
/******************************************************************************/

#if SWIFT_OBJC_INTEROP
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
#endif

/******************************************************************************/
/****************************** Main Entrypoint *******************************/
/******************************************************************************/

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
  #define _protocolWitnessSignedPointer(n) \
    __ptrauth_swift_protocol_witness_function_pointer(SpecialPointerAuthDiscriminators::n##Discriminator) n

  static_assert(WitnessTableFirstRequirementOffset == 1,
                "Witness table layout changed");

  void *_ObjectiveCType;

  // func _bridgeToObjectiveC() -> _ObjectiveCType
  SWIFT_CC(swift)
  HeapObject *(*_protocolWitnessSignedPointer(bridgeToObjectiveC))(
                SWIFT_CONTEXT OpaqueValue *self, const Metadata *Self,
                const _ObjectiveCBridgeableWitnessTable *witnessTable);

  // class func _forceBridgeFromObjectiveC(x: _ObjectiveCType,
  //                                       inout result: Self?)
  SWIFT_CC(swift)
  void (*_protocolWitnessSignedPointer(forceBridgeFromObjectiveC))(
         HeapObject *sourceValue,
         OpaqueValue *result,
         SWIFT_CONTEXT const Metadata *self,
         const Metadata *selfType,
         const _ObjectiveCBridgeableWitnessTable *witnessTable);

  // class func _conditionallyBridgeFromObjectiveC(x: _ObjectiveCType,
  //                                              inout result: Self?) -> Bool
  SWIFT_CC(swift)
  bool (*_protocolWitnessSignedPointer(conditionallyBridgeFromObjectiveC))(
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
  const ProtocolConformanceDescriptor *conformance = wtable->getDescription();
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
  // Handle bridgeable types.
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
SWIFT_CC(swift) SWIFT_RUNTIME_STDLIB_API
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

/// Nominal type descriptor for Swift.String.
extern "C" const StructDescriptor NOMINAL_TYPE_DESCR_SYM(SS);

static const _ObjectiveCBridgeableWitnessTable *
swift_conformsToObjectiveCBridgeable(const Metadata *T) {
  return reinterpret_cast<const _ObjectiveCBridgeableWitnessTable *>
    (swift_conformsToProtocolCommon(T, &PROTOCOL_DESCR_SYM(s21_ObjectiveCBridgeable)));
}

static const _ObjectiveCBridgeableWitnessTable *
findBridgeWitness(const Metadata *T) {
  // Special case: Memoize the bridge witness for Swift.String.
  // Swift.String is the most heavily used bridge because of the prevalence of
  // string-keyed dictionaries in Obj-C.  It's worth burning a few words of static
  // storage to avoid repeatedly looking up this conformance.
  if (T->getKind() == MetadataKind::Struct) {
    auto structDescription = cast<StructMetadata>(T)->Description;
    if (structDescription == &NOMINAL_TYPE_DESCR_SYM(SS)) {
      static auto *Swift_String_ObjectiveCBridgeable = swift_conformsToObjectiveCBridgeable(T);
      return Swift_String_ObjectiveCBridgeable;
    }
  }

  auto w = swift_conformsToObjectiveCBridgeable(T);
  if (SWIFT_LIKELY(w))
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
SWIFT_CC(swift) SWIFT_RUNTIME_STDLIB_API
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
SWIFT_CC(swift) SWIFT_RUNTIME_STDLIB_INTERNAL
void
_bridgeNonVerbatimFromObjectiveCToAny(HeapObject *sourceValue,
                                      OpaqueValue *destValue);

// @_silgen_name("_bridgeNonVerbatimBoxedValue")
// func _bridgeNonVerbatimBoxedValue<NativeType>(
//     x: UnsafePointer<NativeType>,
//     inout result: NativeType?
// )
SWIFT_CC(swift) SWIFT_RUNTIME_STDLIB_INTERNAL
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
SWIFT_CC(swift) SWIFT_RUNTIME_STDLIB_API
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
SWIFT_CC(swift) SWIFT_RUNTIME_STDLIB_API
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
SWIFT_CC(swift) SWIFT_RUNTIME_STDLIB_API
bool _isBridgedNonVerbatimToObjectiveC(const Metadata *value,
                                       const Metadata *T) {
  assert(!swift_isClassOrObjCExistentialTypeImpl(T));

  auto bridgeWitness = findBridgeWitness(T);
  return (bool)bridgeWitness;
}

// func _isClassOrObjCExistential<T>(x: T.Type) -> Bool
SWIFT_CC(swift) SWIFT_RUNTIME_STDLIB_API
bool _swift_isClassOrObjCExistentialType(const Metadata *value,
                                                    const Metadata *T) {
  return swift_isClassOrObjCExistentialTypeImpl(T);
}

SWIFT_CC(swift) SWIFT_RUNTIME_STDLIB_API
void _swift_setClassMetadata(const HeapMetadata *newClassMetadata,
                             HeapObject* onObject,
                             const Metadata *T) {
  assert(T == newClassMetadata);
  onObject->metadata = newClassMetadata;
}

SWIFT_CC(swift) SWIFT_RUNTIME_STDLIB_INTERNAL
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
SWIFT_CC(swift) SWIFT_RUNTIME_STDLIB_INTERNAL
bool _swift_isOptional(OpaqueValue *src, const Metadata *type) {
  return swift_isOptionalType(type);
}

SWIFT_CC(swift) SWIFT_RUNTIME_STDLIB_SPI
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

SWIFT_CC(swift) SWIFT_RUNTIME_STDLIB_INTERNAL
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
#include COMPATIBILITY_OVERRIDE_INCLUDE_PATH

