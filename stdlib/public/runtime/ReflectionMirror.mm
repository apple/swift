//===----------------------------------------------------------------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2018 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//

#include "swift/Basic/Lazy.h"
#include "swift/Runtime/Reflection.h"
#include "swift/Runtime/Casting.h"
#include "swift/Runtime/Config.h"
#include "swift/Runtime/HeapObject.h"
#include "swift/Runtime/Metadata.h"
#include "swift/Runtime/Enum.h"
#include "swift/Runtime/Unreachable.h"
#include "swift/Demangling/Demangle.h"
#include "swift/Runtime/Debug.h"
#include "swift/Runtime/Portability.h"
#include "Private.h"
#include "WeakReference.h"
#include <cassert>
#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <new>
#include <string>
#include <tuple>

#if SWIFT_OBJC_INTEROP
#include "swift/Runtime/ObjCBridge.h"
#include "SwiftObject.h"
#include <Foundation/Foundation.h>
#include <objc/objc.h>
#include <objc/runtime.h>
#endif

#if defined(_WIN32)
#include <stdarg.h>

namespace {
int asprintf(char **strp, const char *fmt, ...) {
  va_list argp0, argp1;

  va_start(argp0, fmt);
  va_copy(argp1, argp0);

  int length = _vscprintf(fmt, argp0);

  *strp = reinterpret_cast<char *>(malloc(length + 1));
  if (*strp == nullptr)
    return -1;

  length = _vsnprintf(*strp, length, fmt, argp1);

  va_end(argp0);
  va_end(argp1);

  return length;
}

char *strndup(const char *s, size_t n) {
  size_t length = std::min(strlen(s), n);

  char *buffer = reinterpret_cast<char *>(malloc(length + 1));
  if (buffer == nullptr)
    return buffer;

  strncpy(buffer, s, length);
  buffer[length] = '\0';
  return buffer;
}
}
#endif

using namespace swift;

#if SWIFT_OBJC_INTEROP
// Declare the debugQuickLookObject selector.
@interface DeclareSelectors
- (id)debugQuickLookObject;
@end
#endif

namespace {

class FieldType {
  const Metadata *type;
  bool indirect;
  TypeReferenceOwnership referenceOwnership;
public:

  constexpr FieldType() : type(nullptr), indirect(false), referenceOwnership() { }
  constexpr FieldType(const Metadata *T) : type(T), indirect(false), referenceOwnership() { }

  static constexpr FieldType untypedEnumCase(bool indirect) {
    FieldType type{};
    type.indirect = indirect;
    return type;
  }
  const Metadata *getType() const { return type; }
  const TypeReferenceOwnership getReferenceOwnership() const { return referenceOwnership; }
  bool isIndirect() const { return indirect; }
  void setIndirect(bool value) { indirect = value; }
  void setReferenceOwnership(TypeReferenceOwnership newOwnership) {
    referenceOwnership = newOwnership;
  }
};

/// The layout of Any.
using Any = OpaqueExistentialContainer;

// Swift assumes Any is returned in memory.
// Use AnyReturn to guarantee that even on architectures
// where Any would be returned in registers.
struct AnyReturn {
  Any any;
  AnyReturn(Any a) : any(a) { }
  operator Any() { return any; }
  ~AnyReturn() { }
};

static std::tuple<const Metadata *, OpaqueValue *>
unwrapExistential(const Metadata *T, OpaqueValue *Value) {
  // If the value is an existential container, look through it to reflect the
  // contained value.
  // TODO: Should look through existential metatypes too, but it doesn't
  // really matter yet since we don't have any special mirror behavior for
  // concrete metatypes yet.
  while (T->getKind() == MetadataKind::Existential) {
    auto *existential
      = static_cast<const ExistentialTypeMetadata *>(T);

    // Unwrap the existential container.
    T = existential->getDynamicType(Value);
    Value = existential->projectValue(Value);

    // Existential containers can end up nested in some cases due to generic
    // abstraction barriers.  Repeat in case we have a nested existential.
  }
  return std::make_tuple(T, Value);
}

static void copyWeakFieldContents(OpaqueValue *destContainer, const Metadata *type, OpaqueValue *fieldData) {
  assert(type->getKind() == MetadataKind::Optional);
  auto *srcContainer = reinterpret_cast<WeakClassExistentialContainer*>(fieldData);
  auto *destClassContainer = reinterpret_cast<ClassExistentialContainer*>(destContainer);
  destClassContainer->Value = swift_unknownObjectWeakLoadStrong(&srcContainer->Value);
  auto witnessTablesSize = type->vw_size() - sizeof(WeakClassExistentialContainer);
  memcpy(destClassContainer->getWitnessTables(), srcContainer->getWitnessTables(), witnessTablesSize);
}

static void copyUnownedFieldContents(OpaqueValue *destContainer, const Metadata *type, OpaqueValue *fieldData) {
  auto *srcContainer = reinterpret_cast<UnownedClassExistentialContainer*>(fieldData);
  auto *destClassContainer = reinterpret_cast<ClassExistentialContainer*>(destContainer);
  destClassContainer->Value = swift_unknownObjectUnownedLoadStrong(&srcContainer->Value);
  auto witnessTablesSize = type->vw_size() - sizeof(UnownedClassExistentialContainer);
  memcpy(destClassContainer->getWitnessTables(), srcContainer->getWitnessTables(), witnessTablesSize);
}

static void copyUnmanagedFieldContents(OpaqueValue *destContainer, const Metadata *type, OpaqueValue *fieldData) {
  // Also known as "unowned(unsafe)".
  // This is simpler than the unowned/weak cases because unmanaged
  // references are fundamentally the same as strong ones, so we
  // can use the regular strong reference support that already
  // knows how to handle existentials and Obj-C references.
  type->vw_initializeWithCopy(destContainer, fieldData);
}

static AnyReturn copyFieldContents(OpaqueValue *fieldData,
                                        const FieldType fieldType) {
  Any outValue;
  auto *type = fieldType.getType();
  outValue.Type = type;
  auto ownership = fieldType.getReferenceOwnership();
  auto *destContainer = type->allocateBoxForExistentialIn(&outValue.Buffer);

  if (ownership.isStrong()) {
    type->vw_initializeWithCopy(destContainer, fieldData);
  }

  // Generate a conditional clause for every known ownership type.
  // If this causes errors, it's because someone added a new ownership type
  // to ReferenceStorage.def and missed some related updates.
#define REF_STORAGE(Name, ...) \
  else if (ownership.is##Name()) { \
    copy##Name##FieldContents(destContainer, type, fieldData); \
  }
#include "swift/AST/ReferenceStorage.def"

  else {
    // The field was declared with a reference type we don't understand.
    warning(0, "Value with unrecognized reference type is reflected as ()");
    // Clean up the buffer allocated above
    type->deallocateBoxForExistentialIn(&outValue.Buffer);
    // Return an existential containing Void
    outValue.Type = &METADATA_SYM(EMPTY_TUPLE_MANGLING);
  }

  return AnyReturn(outValue);
}


// Abstract base class for reflection implementations.
struct ReflectionMirrorImpl {
  const Metadata *type;
  OpaqueValue *value;
  
  virtual char displayStyle() = 0;
  virtual intptr_t count() = 0;
  virtual AnyReturn subscript(intptr_t index, const char **outName,
                              void (**outFreeFunc)(const char *)) = 0;
  virtual const char *enumCaseName() { return nullptr; }

#if SWIFT_OBJC_INTEROP
  virtual id quickLookObject() { return nil; }
#endif
  
  virtual ~ReflectionMirrorImpl() {}
};


// Implementation for tuples.
struct TupleImpl : ReflectionMirrorImpl {
  char displayStyle() {
    return 't';
  }
  
  intptr_t count() {
    auto *Tuple = static_cast<const TupleTypeMetadata *>(type);
    return Tuple->NumElements;
  }
  
  AnyReturn subscript(intptr_t i, const char **outName,
                      void (**outFreeFunc)(const char *)) {
    auto *Tuple = static_cast<const TupleTypeMetadata *>(type);

    if (i < 0 || (size_t)i > Tuple->NumElements)
      swift::crash("Swift mirror subscript bounds check failure");

    // Determine whether there is a label.
    bool hasLabel = false;
    if (const char *labels = Tuple->Labels) {
      const char *space = strchr(labels, ' ');
      for (intptr_t j = 0; j != i && space; ++j) {
        labels = space + 1;
        space = strchr(labels, ' ');
      }

      // If we have a label, create it.
      if (labels && space && labels != space) {
        *outName = strndup(labels, space - labels);
        hasLabel = true;
      }
    }

    if (!hasLabel) {
      // The name is the stringized element number '.0'.
      char *str;
      asprintf(&str, ".%" PRIdPTR, i);
      *outName = str;
    }
    
    *outFreeFunc = [](const char *str) { free(const_cast<char *>(str)); };

    // Get the nth element.
    auto &elt = Tuple->getElement(i);
    auto *bytes = reinterpret_cast<const char *>(value);
    auto *eltData = reinterpret_cast<const OpaqueValue *>(bytes + elt.Offset);

    Any result;

    result.Type = elt.Type;
    auto *opaqueValueAddr = result.Type->allocateBoxForExistentialIn(&result.Buffer);
    result.Type->vw_initializeWithCopy(opaqueValueAddr,
                                       const_cast<OpaqueValue *>(eltData));

    return AnyReturn(result);
  }
};
  
struct swift_closure {
  void *fptr;
  HeapObject *context;
};
SWIFT_RUNTIME_STDLIB_API SWIFT_CC(swift) swift_closure
MANGLE_SYM(s20_playgroundPrintHookySScSgvg)();

static bool _shouldReportMissingReflectionMetadataWarnings() {
  // Missing metadata warnings noise up playground sessions and aren't really
  // actionable in playground contexts. If we're running in a playground,
  // suppress warnings.
  //
  // Guesstimate whether we're in a playground by looking at the
  // _playgroundPrintHook variable in the standard library, which is set during
  // playground execution.
  auto hook = MANGLE_SYM(s20_playgroundPrintHookySScSgvg)();
  if (hook.fptr) {
    swift_release(hook.context);
    return false;
  } else {
    return true;
  }
}

/// Raise a warning about reflection metadata that could not be found
/// at runtime. This is usually mostly harmless, but it's good to alert
/// users that it happens.
static void
missing_reflection_metadata_warning(const char *fmt, ...) {
  bool shouldWarn =
    SWIFT_LAZY_CONSTANT(_shouldReportMissingReflectionMetadataWarnings());
  
  if (!shouldWarn)
    return;
  
  va_list args;
  va_start(args, fmt);
  
  warningv(0, fmt, args);
}

static std::pair<StringRef /*name*/, FieldType /*fieldInfo*/>
getFieldAt(const Metadata *base, unsigned index) {
  using namespace reflection;
  
  // If we failed to find the field descriptor metadata for the type, fall
  // back to returning an empty tuple as a standin.
  auto failedToFindMetadata = [&]() -> std::pair<StringRef, FieldType> {
    auto typeName = swift_getTypeName(base, /*qualified*/ true);
    missing_reflection_metadata_warning(
      "warning: the Swift runtime found no field metadata for "
      "type '%*s' that claims to be reflectable. Its fields will show up as "
      "'unknown' in Mirrors\n",
      (int)typeName.length, typeName.data);
    return {"unknown", FieldType(&METADATA_SYM(EMPTY_TUPLE_MANGLING))};
  };

  auto *baseDesc = base->getTypeContextDescriptor();
  if (!baseDesc)
    return failedToFindMetadata();

  auto *fields = baseDesc->Fields.get();
  if (!fields)
    return failedToFindMetadata();
  
  auto &field = fields->getFields()[index];
  // Bounds are always valid as the offset is constant.
  auto name = field.getFieldName();

  // Enum cases don't always have types.
  if (!field.hasMangledTypeName())
    return {name, FieldType::untypedEnumCase(field.isIndirectCase())};

  auto typeName = field.getMangledTypeName();

  SubstGenericParametersFromMetadata substitutions(base);
  auto typeInfo = swift_getTypeByMangledName(MetadataState::Complete,
   typeName,
   substitutions.getGenericArgs(),
   [&substitutions](unsigned depth, unsigned index) {
     return substitutions.getMetadata(depth, index);
   },
   [&substitutions](const Metadata *type, unsigned index) {
     return substitutions.getWitnessTable(type, index);
   });

  // If demangling the type failed, pretend it's an empty type instead with
  // a log message.
  if (!typeInfo.getMetadata()) {
    typeInfo = TypeInfo({&METADATA_SYM(EMPTY_TUPLE_MANGLING),
                         MetadataState::Complete}, {});
    missing_reflection_metadata_warning(
      "warning: the Swift runtime was unable to demangle the type "
      "of field '%*s'. the mangled type name is '%*s'. this field will "
      "show up as an empty tuple in Mirrors\n",
      (int)name.size(), name.data(),
      (int)typeName.size(), typeName.data());
  }

  auto fieldType = FieldType(typeInfo.getMetadata());
  fieldType.setIndirect(field.isIndirectCase());
  fieldType.setReferenceOwnership(typeInfo.getReferenceOwnership());
  return {name, fieldType};
}

// Implementation for structs.
struct StructImpl : ReflectionMirrorImpl {
  bool isReflectable() {
    const auto *Struct = static_cast<const StructMetadata *>(type);
    const auto &Description = Struct->getDescription();
    return Description->isReflectable();
  }

  char displayStyle() {
    return 's';
  }
  
  intptr_t count() {
    if (!isReflectable()) {
      return 0;
    }

    auto *Struct = static_cast<const StructMetadata *>(type);
    return Struct->getDescription()->NumFields;
  }
  
  AnyReturn subscript(intptr_t i, const char **outName,
                      void (**outFreeFunc)(const char *)) {
    auto *Struct = static_cast<const StructMetadata *>(type);

    if (i < 0 || (size_t)i > Struct->getDescription()->NumFields)
      swift::crash("Swift mirror subscript bounds check failure");

    // Load the offset from its respective vector.
    auto fieldOffset = Struct->getFieldOffsets()[i];

    StringRef name;
    FieldType fieldInfo;
    std::tie(name, fieldInfo) = getFieldAt(type, i);
    assert(!fieldInfo.isIndirect() && "indirect struct fields not implemented");
    
    *outName = name.data();
    *outFreeFunc = nullptr;
    
    auto *bytes = reinterpret_cast<char*>(value);
    auto *fieldData = reinterpret_cast<OpaqueValue *>(bytes + fieldOffset);
    
    return copyFieldContents(fieldData, fieldInfo);
  }
};


// Implementation for enums.
struct EnumImpl : ReflectionMirrorImpl {
  bool isReflectable() {
    const auto *Enum = static_cast<const EnumMetadata *>(type);
    const auto &Description = Enum->getDescription();
    return Description->isReflectable();
  }
  
  const char *getInfo(unsigned *tagPtr = nullptr,
                      const Metadata **payloadTypePtr = nullptr,
                      bool *indirectPtr = nullptr) {
    // 'tag' is in the range [0..NumElements-1].
    unsigned tag = type->vw_getEnumTag(value);

    StringRef name;
    FieldType info;
    std::tie(name, info) = getFieldAt(type, tag);
    const Metadata *payloadType = info.getType();
    bool indirect = info.isIndirect();

    if (tagPtr)
      *tagPtr = tag;
    if (payloadTypePtr)
      *payloadTypePtr = payloadType;
    if (indirectPtr)
      *indirectPtr = indirect;
    
    return name.data();
  }

  char displayStyle() {
    return 'e';
  }
  
  intptr_t count() {
    if (!isReflectable()) {
      return 0;
    }
    
    const Metadata *payloadType;
    getInfo(nullptr, &payloadType, nullptr);
    return (payloadType != nullptr) ? 1 : 0;
  }

  AnyReturn subscript(intptr_t i, const char **outName,
                      void (**outFreeFunc)(const char *)) {
    unsigned tag;
    const Metadata *payloadType;
    bool indirect;

    auto *caseName = getInfo(&tag, &payloadType, &indirect);

    // Copy the enum itself so that we can project the data without destroying
    // the original.
    Any enumCopy;
    auto *enumCopyContainer
      = type->allocateBoxForExistentialIn(&enumCopy.Buffer);
    type->vw_initializeWithCopy(enumCopyContainer,
                                const_cast<OpaqueValue *>(value));

    // Copy the enum payload into a box
    const Metadata *boxType = (indirect ? &METADATA_SYM(Bo).base : payloadType);
    BoxPair pair = swift_allocBox(boxType);
    type->vw_destructiveProjectEnumData(enumCopyContainer);
    boxType->vw_initializeWithTake(pair.buffer, enumCopyContainer);
    type->deallocateBoxForExistentialIn(&enumCopy.Buffer);
    
    value = pair.buffer;

    // If the payload is indirect, we need to jump through the box to get it.
    if (indirect) {
      const HeapObject *owner = *reinterpret_cast<HeapObject * const *>(value);
      value = swift_projectBox(const_cast<HeapObject *>(owner));
    }
    
    *outName = caseName;
    *outFreeFunc = nullptr;
    
    Any result;

    result.Type = payloadType;
    auto *opaqueValueAddr = result.Type->allocateBoxForExistentialIn(&result.Buffer);
    result.Type->vw_initializeWithCopy(opaqueValueAddr,
                                       const_cast<OpaqueValue *>(value));

    swift_release(pair.object);
    return AnyReturn(result);
  }
  
  const char *enumCaseName() {
    if (!isReflectable()) {
      return nullptr;
    }
    
    return getInfo();
  }
};


// Implementation for classes.
struct ClassImpl : ReflectionMirrorImpl {
  bool isReflectable() {
    const auto *Class = static_cast<const ClassMetadata *>(type);
    const auto &Description = Class->getDescription();
    return Description->isReflectable();
  }

  char displayStyle() {
    return 'c';
  }
  
  intptr_t count() {
    if (!isReflectable())
      return 0;

    auto *Clas = static_cast<const ClassMetadata*>(type);
    auto count = Clas->getDescription()->NumFields;

    return count;
  }
  
  AnyReturn subscript(intptr_t i, const char **outName,
                      void (**outFreeFunc)(const char *)) {
    auto *Clas = static_cast<const ClassMetadata*>(type);

    if (i < 0 || (size_t)i > Clas->getDescription()->NumFields)
      swift::crash("Swift mirror subscript bounds check failure");

    // FIXME: If the class has ObjC heritage, get the field offset using the ObjC
    // metadata, because we don't update the field offsets in the face of
    // resilient base classes.
    uintptr_t fieldOffset;
    if (usesNativeSwiftReferenceCounting(Clas)) {
      fieldOffset = Clas->getFieldOffsets()[i];
    } else {
  #if SWIFT_OBJC_INTEROP
      Ivar *ivars = class_copyIvarList((Class)Clas, nullptr);
      fieldOffset = ivar_getOffset(ivars[i]);
      free(ivars);
  #else
      swift::crash("Object appears to be Objective-C, but no runtime.");
  #endif
    }

    StringRef name;
    FieldType fieldInfo;
    std::tie(name, fieldInfo) = getFieldAt(type, i);
    assert(!fieldInfo.isIndirect() && "class indirect properties not implemented");
    
    auto *bytes = *reinterpret_cast<char * const *>(value);
    auto *fieldData = reinterpret_cast<OpaqueValue *>(bytes + fieldOffset);

    *outName = name.data();
    *outFreeFunc = nullptr;
  
    return copyFieldContents(fieldData, fieldInfo);
  }

#if SWIFT_OBJC_INTEROP
  id quickLookObject() {
    id object = [*reinterpret_cast<const id *>(value) retain];
    if ([object respondsToSelector:@selector(debugQuickLookObject)]) {
      id quickLookObject = [object debugQuickLookObject];
      [quickLookObject retain];
      [object release];
      return quickLookObject;
    }

    return object;
  }
#endif
};


#if SWIFT_OBJC_INTEROP
// Implementation for ObjC classes.
struct ObjCClassImpl : ClassImpl {
  intptr_t count() {
    // ObjC makes no guarantees about the state of ivars, so we can't safely
    // introspect them in the general case.
    return 0;
  }
  
  AnyReturn subscript(intptr_t i, const char **outName,
                      void (**outFreeFunc)(const char *)) {
    swift::crash("Cannot get children of Objective-C objects.");
  }
};
#endif


// Implementation for metatypes.
struct MetatypeImpl : ReflectionMirrorImpl {
  char displayStyle() {
    return '\0';
  }
  
  intptr_t count() {
    return 0;
  }
  
  AnyReturn subscript(intptr_t i, const char **outName,
                    void (**outFreeFunc)(const char *)) {
    swift::crash("Metatypes have no children.");
  }
};


// Implementation for opaque types.
struct OpaqueImpl : ReflectionMirrorImpl {
  char displayStyle() {
    return '\0';
  }
  
  intptr_t count() {
    return 0;
  }
  
  AnyReturn subscript(intptr_t i, const char **outName,
                    void (**outFreeFunc)(const char *)) {
    swift::crash("Opaque types have no children.");
  }
};


template<typename F>
auto call(OpaqueValue *passedValue, const Metadata *T, const Metadata *passedType,
          const F &f) -> decltype(f(nullptr))
{
  const Metadata *type;
  OpaqueValue *value;
  std::tie(type, value) = unwrapExistential(T, passedValue);
  
  if (passedType != nullptr) {
    type = passedType;
  }
  
  auto call = [&](ReflectionMirrorImpl *impl) {
    impl->type = type;
    impl->value = value;
    auto result = f(impl);
    return result;
  };
  
  auto callClass = [&] {
    if (passedType == nullptr) {
      // Get the runtime type of the object.
      const void *obj = *reinterpret_cast<const void * const *>(value);
      auto isa = _swift_getClass(obj);

      // Look through artificial subclasses.
      while (isa->isTypeMetadata() && isa->isArtificialSubclass()) {
        isa = isa->Superclass;
      }
      passedType = isa;
    }

  #if SWIFT_OBJC_INTEROP
    // If this is a pure ObjC class, reflect it using ObjC's runtime facilities.
    // ForeignClass (e.g. CF classes) manifests as a NULL class object.
    auto *classObject = passedType->getClassObject();
    if (classObject == nullptr || !classObject->isTypeMetadata()) {
      ObjCClassImpl impl;
      return call(&impl);
    }
  #endif

    // Otherwise, use the native Swift facilities.
    ClassImpl impl;
    return call(&impl);
  };
  
  switch (type->getKind()) {
    case MetadataKind::Tuple: {
      TupleImpl impl;
      return call(&impl);
    }

    case MetadataKind::Struct: {
      StructImpl impl;
      return call(&impl);
    }
    

    case MetadataKind::Enum:
    case MetadataKind::Optional: {
      EnumImpl impl;
      return call(&impl);
    }
      
    case MetadataKind::ObjCClassWrapper:
    case MetadataKind::ForeignClass:
    case MetadataKind::Class: {
      return callClass();
    }

    case MetadataKind::Metatype:
    case MetadataKind::ExistentialMetatype: {
      MetatypeImpl impl;
      return call(&impl);
    }

    case MetadataKind::Opaque: {
#if SWIFT_OBJC_INTEROP
      // If this is the AnyObject type, use the dynamic type of the
      // object reference.
      if (type == &METADATA_SYM(BO).base) {
        return callClass();
      }
#endif
      // If this is the Builtin.NativeObject type, and the heap object is a
      // class instance, use the dynamic type of the object reference.
      if (type == &METADATA_SYM(Bo).base) {
        const HeapObject *obj
          = *reinterpret_cast<const HeapObject * const*>(value);
        if (obj->metadata->getKind() == MetadataKind::Class) {
          return callClass();
        }
      }
      SWIFT_FALLTHROUGH;
    }

    /// TODO: Implement specialized mirror witnesses for all kinds.
    default:
      break;

    // Types can't have these kinds.
    case MetadataKind::HeapLocalVariable:
    case MetadataKind::HeapGenericLocalVariable:
    case MetadataKind::ErrorObject:
      swift::crash("Swift mirror lookup failure");
    }

    // If we have an unknown kind of type, or a type without special handling,
    // treat it as opaque.
    OpaqueImpl impl;
    return call(&impl);
}

} // end anonymous namespace


// func _getNormalizedType<T>(_: T, type: Any.Type) -> Any.Type
SWIFT_CC(swift) SWIFT_RUNTIME_STDLIB_API
const Metadata *swift_reflectionMirror_normalizedType(OpaqueValue *value,
                                                      const Metadata *type,
                                                      const Metadata *T) {
  return call(value, T, type, [](ReflectionMirrorImpl *impl) { return impl->type; });
}

// func _getChildCount<T>(_: T, type: Any.Type) -> Int
SWIFT_CC(swift) SWIFT_RUNTIME_STDLIB_API
intptr_t swift_reflectionMirror_count(OpaqueValue *value,
                                      const Metadata *type,
                                      const Metadata *T) {
  return call(value, T, type, [](ReflectionMirrorImpl *impl) {
    return impl->count();
  });
}

// We intentionally use a non-POD return type with this entry point to give
// it an indirect return ABI for compatibility with Swift.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wreturn-type-c-linkage"
// func _getChild<T>(
//   of: T,
//   type: Any.Type,
//   index: Int,
//   outName: UnsafeMutablePointer<UnsafePointer<CChar>?>,
//   outFreeFunc: UnsafeMutablePointer<NameFreeFunc?>
// ) -> Any
SWIFT_CC(swift) SWIFT_RUNTIME_STDLIB_API
AnyReturn swift_reflectionMirror_subscript(OpaqueValue *value, const Metadata *type,
                                           intptr_t index,
                                           const char **outName,
                                           void (**outFreeFunc)(const char *),
                                           const Metadata *T) {
  return call(value, T, type, [&](ReflectionMirrorImpl *impl) {
    return impl->subscript(index, outName, outFreeFunc);
  });
}
#pragma clang diagnostic pop

// func _getDisplayStyle<T>(_: T) -> CChar
SWIFT_CC(swift) SWIFT_RUNTIME_STDLIB_API
char swift_reflectionMirror_displayStyle(OpaqueValue *value, const Metadata *T) {
  return call(value, T, nullptr, [](ReflectionMirrorImpl *impl) { return impl->displayStyle(); });
}

// func _getEnumCaseName<T>(_ value: T) -> UnsafePointer<CChar>?
SWIFT_CC(swift) SWIFT_RUNTIME_STDLIB_API
const char *swift_EnumCaseName(OpaqueValue *value, const Metadata *T) {
  return call(value, T, nullptr, [](ReflectionMirrorImpl *impl) { return impl->enumCaseName(); });
}

// func _opaqueSummary(_ metadata: Any.Type) -> UnsafePointer<CChar>?
SWIFT_CC(swift) SWIFT_RUNTIME_STDLIB_API
const char *swift_OpaqueSummary(const Metadata *T) {
  switch (T->getKind()) {
    case MetadataKind::Class:
    case MetadataKind::Struct:
    case MetadataKind::Enum:
    case MetadataKind::Optional:
    case MetadataKind::Metatype:
      return nullptr;
    case MetadataKind::Opaque:
      return "(Opaque Value)";
    case MetadataKind::Tuple:
      return "(Tuple)";
    case MetadataKind::Function:
      return "(Function)";
    case MetadataKind::Existential:
      return "(Existential)";
    case MetadataKind::ObjCClassWrapper:
      return "(Objective-C Class Wrapper)";
    case MetadataKind::ExistentialMetatype:
      return "(Existential Metatype)";
    case MetadataKind::ForeignClass:
      return "(Foreign Class)";
    case MetadataKind::HeapLocalVariable:
      return "(Heap Local Variable)";
    case MetadataKind::HeapGenericLocalVariable:
      return "(Heap Generic Local Variable)";
    case MetadataKind::ErrorObject:
      return "(ErrorType Object)";
    default:
      return "(Unknown)";
  }
}

#if SWIFT_OBJC_INTEROP
// func _getQuickLookObject<T>(_: T) -> AnyObject?
SWIFT_CC(swift) SWIFT_RUNTIME_STDLIB_API
id swift_reflectionMirror_quickLookObject(OpaqueValue *value, const Metadata *T) {
  return call(value, T, nullptr, [](ReflectionMirrorImpl *impl) { return impl->quickLookObject(); });
}
#endif
