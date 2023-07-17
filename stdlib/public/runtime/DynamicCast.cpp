//===--- DynamicCast.cpp - Swift Language Dynamic Casting Support ---------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2020 Apple Inc. and the Swift project authors
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

#include "../CompatibilityOverride/CompatibilityOverride.h"
#include "ErrorObject.h"
#include "Private.h"
#include "SwiftHashableSupport.h"
#include "swift/ABI/MetadataValues.h"
#include "swift/Basic/Lazy.h"
#include "swift/Runtime/Bincompat.h"
#include "swift/Runtime/Casting.h"
#include "swift/Runtime/Config.h"
#include "swift/Runtime/ExistentialContainer.h"
#include "swift/Runtime/HeapObject.h"
#if SWIFT_OBJC_INTEROP
#include "swift/Runtime/ObjCBridge.h"
#include "SwiftObject.h"
#include "SwiftValue.h"
#endif

using namespace swift;
using namespace hashable_support;

//
// The top-level driver code directly handles the most general cases
// (identity casts, _ObjectiveCBridgeable, _SwiftValue boxing) and
// recursively unwraps source and/or destination as appropriate.
// It calls "tryCastToXyz" functions to perform tailored operations
// for a particular destination type.
//
// For each kind of destination, there is a "tryCastToXyz" that
// accepts a source value and attempts to fit it into a destination
// storage location.  This function should assume that:
// * The source and destination types are _not_ identical.
// * The destination is of the expected type.
// * The source is already fully unwrapped.  If the source is an
//   Existential or Optional that you cannot handle directly, do _not_
//   try to unwrap it.  Just return failure and you will get called
//   again with the unwrapped source.
//
// Each such function accepts the following arguments:
// * Destination location and type
// * Source value address and type
// * References to the types that will be used to report failure.
//   The function can update these with specific failing types
//   to improve the reported failure.
// * Bool indicating whether the compiler has asked us to "take" the
//   value instead of copying.
// * Bool indicating whether it's okay to do type checks lazily on later
//   access (this is permitted only for unconditional casts that will
//   abort the program on failure anyway).
//
// The return value is one of the following:
// * Failure.  In this case, the tryCastFunction should do nothing; your
//   caller will either try another strategy or report the failure and
//   do any necessary cleanup.
// * Success via "copy".  You successfully copied the source value.
// * Success via "take".  If "take" was requested and you can do so cheaply,
//   perform the take and return SuccessViaTake.  If "take" is not cheap, you
//   should copy and return SuccessViaCopy.  Top-level code will detect this
//   and take care of destroying the source for you.
//
enum class DynamicCastResult {
  Failure,  /// The cast attempt "failed" (did nothing).
  SuccessViaCopy, /// Cast succeeded, source is still valid.
  SuccessViaTake, /// Cast succeeded, source is invalid
};
static bool isSuccess(DynamicCastResult result) {
  return result != DynamicCastResult::Failure;
}

// All of our `tryCastXyz` functions have the following signature.
typedef DynamicCastResult (tryCastFunctionType)(
  OpaqueValue *destLocation, const Metadata *destType,
  OpaqueValue *srcValue, const Metadata *srcType,
  const Metadata *&destFailureType, const Metadata *&srcFailureType,
  bool takeOnSuccess, bool mayDeferChecks
);

// Forward-declare the main top-level `tryCast()` function
static tryCastFunctionType tryCast;

/// Nominal type descriptor for Swift.AnyHashable
extern "C" const StructDescriptor STRUCT_TYPE_DESCR_SYM(s11AnyHashable);

/// Nominal type descriptor for Swift.__SwiftValue
//extern "C" const StructDescriptor STRUCT_TYPE_DESCR_SYM(s12__SwiftValue);

/// Nominal type descriptor for Swift.Array.
extern "C" const StructDescriptor NOMINAL_TYPE_DESCR_SYM(Sa);

/// Nominal type descriptor for Swift.Dictionary.
extern "C" const StructDescriptor NOMINAL_TYPE_DESCR_SYM(SD);

/// Nominal type descriptor for Swift.Set.
extern "C" const StructDescriptor NOMINAL_TYPE_DESCR_SYM(Sh);

/// Nominal type descriptor for Swift.String.
extern "C" const StructDescriptor NOMINAL_TYPE_DESCR_SYM(SS);

/// This issues a fatal error or warning if the srcValue contains a null object
/// reference.  It is used when the srcType is a non-nullable reference type, in
/// which case it is dangerous to continue with a null reference.  The null
/// reference is returned if we're operating in backwards-compatibility mode, so
/// callers still have to check for null.
static HeapObject * getNonNullSrcObject(OpaqueValue *srcValue,
                                        const Metadata *srcType,
                                        const Metadata *destType) {
  auto object = *reinterpret_cast<HeapObject **>(srcValue);
  if (LLVM_LIKELY(object != nullptr)) {
    return object;
  }

  std::string srcTypeName = nameForMetadata(srcType);
  std::string destTypeName = nameForMetadata(destType);
  const char * const msg = "Found unexpected null pointer value"
                    " while trying to cast value of type '%s' (%p)"
                    " to '%s' (%p)%s\n";
  if (runtime::bincompat::useLegacyPermissiveObjCNullSemanticsInCasting()) {
    // In backwards compatibility mode, this code will warn and return the null
    // reference anyway: If you examine the calls to the function, you'll see
    // that most callers fail the cast in that case, but a few casts (e.g., with
    // Obj-C or CF destination type) sill succeed in that case.  This is
    // dangerous, but necessary for compatibility.
    swift::warning(/* flags = */ 0, msg,
                   srcTypeName.c_str(), srcType,
                   destTypeName.c_str(), destType,
                   ": Continuing with null object, but expect problems later.");
  } else {
    // By default, Swift 5.4 and later issue a fatal error.
    swift::fatalError(/* flags = */ 0, msg,
                      srcTypeName.c_str(), srcType,
                      destTypeName.c_str(), destType,
                      "");
  }
  return object;
}

/******************************************************************************/
/******************************* Bridge Helpers *******************************/
/******************************************************************************/

#define _bridgeAnythingToObjectiveC                                 \
  MANGLE_SYM(s27_bridgeAnythingToObjectiveCyyXlxlF)
SWIFT_CC(swift) SWIFT_RUNTIME_STDLIB_API
HeapObject *_bridgeAnythingToObjectiveC(
  OpaqueValue *src, const Metadata *srcType);

#if SWIFT_OBJC_INTEROP
SWIFT_RUNTIME_EXPORT
id swift_dynamicCastMetatypeToObjectConditional(const Metadata *metatype);
#endif

// protocol _ObjectiveCBridgeable {
struct _ObjectiveCBridgeableWitnessTable : WitnessTable {
  #define _protocolWitnessSignedPointer(n) \
    __ptrauth_swift_protocol_witness_function_pointer(SpecialPointerAuthDiscriminators::n##Discriminator) n

  static_assert(WitnessTableFirstRequirementOffset == 1,
                "Witness table layout changed");

  // associatedtype _ObjectiveCType : class
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

extern "C" const ProtocolDescriptor
PROTOCOL_DESCR_SYM(s21_ObjectiveCBridgeable);

static const _ObjectiveCBridgeableWitnessTable *
findBridgeWitness(const Metadata *T) {
  static const auto bridgeableProtocol
    = &PROTOCOL_DESCR_SYM(s21_ObjectiveCBridgeable);
  auto w = swift_conformsToProtocolCommon(T, bridgeableProtocol);
  return reinterpret_cast<const _ObjectiveCBridgeableWitnessTable *>(w);
}

/// Retrieve the bridged Objective-C type for the given type that
/// conforms to \c _ObjectiveCBridgeable.
MetadataResponse
_getBridgedObjectiveCType(
  MetadataRequest request,
  const Metadata *conformingType,
  const _ObjectiveCBridgeableWitnessTable *wtable)
{
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

/// Dynamic cast from a class type to a value type that conforms to the
/// _ObjectiveCBridgeable, first by dynamic casting the object to the
/// class to which the value type is bridged, and then bridging
/// from that object to the value type via the witness table.
///
/// Caveat: Despite the name, this is also used to bridge pure Swift
/// classes to Swift value types even when Obj-C is not being used.

static DynamicCastResult
_tryCastFromClassToObjCBridgeable(
  OpaqueValue *destLocation, const Metadata *destType,
  OpaqueValue *srcValue, const Metadata *srcType, void *srcObject,
  const Metadata *&destFailureType, const Metadata *&srcFailureType,
  bool takeOnSuccess, bool mayDeferChecks,
  const _ObjectiveCBridgeableWitnessTable *destBridgeWitness,
  const Metadata *targetBridgeClass)
{

  // 2. Allocate a T? to receive the bridge result.

  // The extra byte is for the tag.
  auto targetSize = destType->getValueWitnesses()->getSize() + 1;
  auto targetAlignMask = destType->getValueWitnesses()->getAlignmentMask();

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

  // The extra byte is for the tag on the T?
  const std::size_t inlineValueSize = 3 * sizeof(void*);
  alignas(MaximumAlignment) char inlineBuffer[inlineValueSize + 1];
  void *optDestBuffer;
  if (destType->getValueWitnesses()->getStride() <= inlineValueSize) {
    // Use the inline buffer.
    optDestBuffer = inlineBuffer;
  } else {
    // Allocate a buffer.
    optDestBuffer = swift_slowAlloc(targetSize, targetAlignMask);
    freeBuffer.Buffer = optDestBuffer;
  }

  // Initialize the buffer as an empty optional.
  destType->vw_storeEnumTagSinglePayload((OpaqueValue *)optDestBuffer,
                                           1, 1);

  // 3. Bridge into the T? (Effectively a copy operation.)
  bool success;
  if (mayDeferChecks) {
    destBridgeWitness->forceBridgeFromObjectiveC(
      (HeapObject *)srcObject, (OpaqueValue *)optDestBuffer,
      destType, destType, destBridgeWitness);
    success = true;
  } else {
    success = destBridgeWitness->conditionallyBridgeFromObjectiveC(
      (HeapObject *)srcObject, (OpaqueValue *)optDestBuffer,
      destType, destType, destBridgeWitness);
  }

  // If we succeeded, then take the value from the temp buffer.
  if (success) {
    destType->vw_initializeWithTake(destLocation, (OpaqueValue *)optDestBuffer);
    // Bridge above is effectively a copy, so overall we're a copy.
    return DynamicCastResult::SuccessViaCopy;
  }
  return DynamicCastResult::Failure;
}

static DynamicCastResult
tryCastFromClassToObjCBridgeable(
  OpaqueValue *destLocation, const Metadata *destType,
  OpaqueValue *srcValue, const Metadata *srcType,
  const Metadata *&destFailureType, const Metadata *&srcFailureType,
  bool takeOnSuccess, bool mayDeferChecks)
{
  // We need the _ObjectiveCBridgeable conformance for the target
  auto destBridgeWitness = findBridgeWitness(destType);
  if (destBridgeWitness == nullptr) {
    return DynamicCastResult::Failure;
  }

  // 1. Sanity check whether the source object can cast to the
  // type expected by the target.

  auto targetBridgedClass =
      _getBridgedObjectiveCType(MetadataState::Complete, destType,
                                destBridgeWitness).Value;
  void *srcObject = getNonNullSrcObject(srcValue, srcType, destType);
  // Note: srcObject can be null here in compatibility mode
  if (nullptr == srcObject
      || nullptr == swift_dynamicCastUnknownClass(srcObject, targetBridgedClass)) {
    destFailureType = targetBridgedClass;
    return DynamicCastResult::Failure;
  }

  return _tryCastFromClassToObjCBridgeable(
    destLocation, destType,
    srcValue, srcType, srcObject,
    destFailureType, srcFailureType,
    takeOnSuccess, mayDeferChecks,
    destBridgeWitness, targetBridgedClass);
}

/// Dynamic cast from a value type that conforms to the
/// _ObjectiveCBridgeable protocol to a class type, first by bridging
/// the value to its Objective-C object representation and then by
/// dynamic casting that object to the resulting target type.
///
/// Caveat: Despite the name, this is also used to bridge Swift value types
/// to Swift classes even when Obj-C is not being used.
static DynamicCastResult
tryCastFromObjCBridgeableToClass(
  OpaqueValue *destLocation, const Metadata *destType,
  OpaqueValue *srcValue, const Metadata *srcType,
  const Metadata *&destFailureType, const Metadata *&srcFailureType,
  bool takeOnSuccess, bool mayDeferChecks)
{
  auto srcBridgeWitness = findBridgeWitness(srcType);
  if (srcBridgeWitness == nullptr) {
    return DynamicCastResult::Failure;
  }

  // Bridge the source value to an object.
  auto srcBridgedObject =
    srcBridgeWitness->bridgeToObjectiveC(srcValue, srcType, srcBridgeWitness);

  // Dynamic cast the object to the resulting class type.
  if (auto cast = swift_dynamicCastUnknownClass(srcBridgedObject, destType)) {
    *reinterpret_cast<const void **>(destLocation) = cast;
    return DynamicCastResult::SuccessViaCopy;
  } else {
    // We don't need the object anymore.
    swift_unknownObjectRelease(srcBridgedObject);
    return DynamicCastResult::Failure;
  }
}

/******************************************************************************/
/****************************** SwiftValue Boxing *****************************/
/******************************************************************************/

#if !SWIFT_OBJC_INTEROP // __SwiftValue is a native class
SWIFT_CC(swift) SWIFT_RUNTIME_STDLIB_INTERNAL
bool swift_unboxFromSwiftValueWithType(OpaqueValue *source,
                                       OpaqueValue *result,
                                       const Metadata *destinationType);

SWIFT_CC(swift) SWIFT_RUNTIME_STDLIB_INTERNAL
bool swift_swiftValueConformsTo(const Metadata *, const Metadata *);
#endif

#if SWIFT_OBJC_INTEROP
// Try unwrapping a source holding an Obj-C SwiftValue container and
// recursively casting the contents.
static DynamicCastResult
tryCastUnwrappingObjCSwiftValueSource(
  OpaqueValue *destLocation, const Metadata *destType,
  OpaqueValue *srcValue, const Metadata *srcType,
  const Metadata *&destFailureType, const Metadata *&srcFailureType,
  bool takeOnSuccess, bool mayDeferChecks)
{
    id srcObject;
    memcpy(&srcObject, srcValue, sizeof(id));
    auto srcSwiftValue = getAsSwiftValue(srcObject);

    if (srcSwiftValue == nullptr) {
      return DynamicCastResult::Failure;
    }

    const Metadata *srcInnerType;
    const OpaqueValue *srcInnerValue;
    std::tie(srcInnerType, srcInnerValue)
      = getValueFromSwiftValue(srcSwiftValue);
    // Note: We never `take` the contents from a SwiftValue box as
    // it might have other references.  Instead, let our caller
    // destroy the reference if necessary.
    return tryCast(
      destLocation, destType,
      const_cast<OpaqueValue *>(srcInnerValue), srcInnerType,
      destFailureType, srcFailureType,
      /*takeOnSuccess=*/ false, mayDeferChecks);
}
#else
static DynamicCastResult
tryCastUnwrappingSwiftValueSource(
  OpaqueValue *destLocation, const Metadata *destType,
  OpaqueValue *srcValue, const Metadata *srcType,
  const Metadata *&destFailureType, const Metadata *&srcFailureType,
  bool takeOnSuccess, bool mayDeferChecks)
{
  assert(srcType->getKind() == MetadataKind::Class);

  // unboxFromSwiftValueWithType is really just a recursive casting operation...
  if (swift_unboxFromSwiftValueWithType(srcValue, destLocation, destType)) {
    return DynamicCastResult::SuccessViaCopy;
  } else {
    return DynamicCastResult::Failure;
  }
}
#endif

/******************************************************************************/
/****************************** Class Destination *****************************/
/******************************************************************************/

static DynamicCastResult
tryCastToSwiftClass(
  OpaqueValue *destLocation, const Metadata *destType,
  OpaqueValue *srcValue, const Metadata *srcType,
  const Metadata *&destFailureType, const Metadata *&srcFailureType,
  bool takeOnSuccess, bool mayDeferChecks)
{
  assert(srcType != destType);
  assert(destType->getKind() == MetadataKind::Class);

  auto destClassType = cast<ClassMetadata>(destType);
  switch (srcType->getKind()) {
  case MetadataKind::Class: // Swift class => Swift class
  case MetadataKind::ObjCClassWrapper: { // Obj-C class => Swift class
    void *srcObject = getNonNullSrcObject(srcValue, srcType, destType);
    // Note: srcObject can be null in compatibility mode.
    if (srcObject == nullptr) {
      return DynamicCastResult::Failure;
    }
    if (auto t = swift_dynamicCastClass(srcObject, destClassType)) {
      auto castObject = const_cast<void *>(t);
      *(reinterpret_cast<void **>(destLocation)) = castObject;
      if (takeOnSuccess) {
        return DynamicCastResult::SuccessViaTake;
      } else {
        swift_unknownObjectRetain(castObject);
        return DynamicCastResult::SuccessViaCopy;
      }
    } else {
      srcFailureType = srcType;
      destFailureType = destType;
      return DynamicCastResult::Failure;
    }
  }

  case MetadataKind::ForeignClass: // CF class => Swift class
    // Top-level code will "unwrap" to an Obj-C class and try again.
    return DynamicCastResult::Failure;

  default:
    return DynamicCastResult::Failure;
  }
}

static DynamicCastResult
tryCastToObjectiveCClass(
  OpaqueValue *destLocation, const Metadata *destType,
  OpaqueValue *srcValue, const Metadata *srcType,
  const Metadata *&destFailureType, const Metadata *&srcFailureType,
  bool takeOnSuccess, bool mayDeferChecks)
{
  assert(srcType != destType);
  assert(destType->getKind() == MetadataKind::ObjCClassWrapper);
#if SWIFT_OBJC_INTEROP
  auto destObjCType = cast<ObjCClassWrapperMetadata>(destType);

  switch (srcType->getKind()) {
  case MetadataKind::Class: // Swift class => Obj-C class
  case MetadataKind::ObjCClassWrapper: // Obj-C class => Obj-C class
  case MetadataKind::ForeignClass: { // CF class => Obj-C class
    auto srcObject = getNonNullSrcObject(srcValue, srcType, destType);
    // If object is null, then we're in the compatibility mode.
    // Earlier cast logic always succeeded `as!` casts of nil
    // class references but failed `as?` and `is`
    if (srcObject == nullptr) {
      if (mayDeferChecks) {
        *reinterpret_cast<const void **>(destLocation) = nullptr;
        return DynamicCastResult::SuccessViaCopy;
      } else {
        return DynamicCastResult::Failure;
      }
    }
    auto destObjCClass = destObjCType->Class;
    if (auto resultObject
        = swift_dynamicCastObjCClass(srcObject, destObjCClass)) {
      *reinterpret_cast<const void **>(destLocation) = resultObject;
      if (takeOnSuccess) {
        return DynamicCastResult::SuccessViaTake;
      } else {
        objc_retain((id)const_cast<void *>(resultObject));
        return DynamicCastResult::SuccessViaCopy;
      }
    }
    break;
  }

  default:
    break;
  }
#endif

  return DynamicCastResult::Failure;
}

static DynamicCastResult
tryCastToForeignClass(
  OpaqueValue *destLocation, const Metadata *destType,
  OpaqueValue *srcValue, const Metadata *srcType,
  const Metadata *&destFailureType, const Metadata *&srcFailureType,
  bool takeOnSuccess, bool mayDeferChecks)
{
#if SWIFT_OBJC_INTEROP
  assert(srcType != destType);
  assert(destType->getKind() == MetadataKind::ForeignClass);
  auto destClassType = cast<ForeignClassMetadata>(destType);

  switch (srcType->getKind()) {
  case MetadataKind::Class: // Swift class => CF class
  case MetadataKind::ObjCClassWrapper: // Obj-C class => CF class
  case MetadataKind::ForeignClass: { // CF class => CF class
    auto srcObject = getNonNullSrcObject(srcValue, srcType, destType);
    // If srcObject is null, then we're in compatibility mode.
    // Earlier cast logic always succeeded `as!` casts of nil
    // class references.  Yes, this is very dangerous, which
    // is why we no longer permit it.
    if (srcObject == nullptr) {
      if (mayDeferChecks) {
        *reinterpret_cast<const void **>(destLocation) = nullptr;
        return DynamicCastResult::SuccessViaCopy;
      } else {
        // `as?` and `is` checks always fail on nil sources
        return DynamicCastResult::Failure;
      }
    }
    if (auto resultObject
        = swift_dynamicCastForeignClass(srcObject, destClassType)) {
      *reinterpret_cast<const void **>(destLocation) = resultObject;
      if (takeOnSuccess) {
        return DynamicCastResult::SuccessViaTake;
      } else {
        objc_retain((id)const_cast<void *>(resultObject));
        return DynamicCastResult::SuccessViaCopy;
      }
    }
    break;
  }
  default:
    break;
  }
#endif

  return DynamicCastResult::Failure;
}

/******************************************************************************/
/***************************** Enum Destination *******************************/
/******************************************************************************/

static DynamicCastResult
tryCastToEnum(
  OpaqueValue *destLocation, const Metadata *destType,
  OpaqueValue *srcValue, const Metadata *srcType,
  const Metadata *&destFailureType, const Metadata *&srcFailureType,
  bool takeOnSuccess, bool mayDeferChecks)
{
  assert(srcType != destType);
  // Note: Optional is handled elsewhere
  assert(destType->getKind() == MetadataKind::Enum);

  // Enum has no special cast support at present.

  return DynamicCastResult::Failure;
}

/******************************************************************************/
/**************************** Struct Destination ******************************/
/******************************************************************************/

// internal func _arrayDownCastIndirect<SourceValue, TargetValue>(
//   _ source: UnsafePointer<Array<SourceValue>>,
//   _ target: UnsafeMutablePointer<Array<TargetValue>>)
SWIFT_CC(swift) SWIFT_RUNTIME_STDLIB_INTERNAL
void _swift_arrayDownCastIndirect(OpaqueValue *destination,
                                  OpaqueValue *source,
                                  const Metadata *sourceValueType,
                                  const Metadata *targetValueType);

// internal func _arrayDownCastConditionalIndirect<SourceValue, TargetValue>(
//   _ source: UnsafePointer<Array<SourceValue>>,
//   _ target: UnsafeMutablePointer<Array<TargetValue>>
// ) -> Bool
SWIFT_CC(swift) SWIFT_RUNTIME_STDLIB_INTERNAL
bool _swift_arrayDownCastConditionalIndirect(OpaqueValue *destination,
                                             OpaqueValue *source,
                                             const Metadata *sourceValueType,
                                             const Metadata *targetValueType);

// internal func _setDownCastIndirect<SourceValue, TargetValue>(
//   _ source: UnsafePointer<Set<SourceValue>>,
//   _ target: UnsafeMutablePointer<Set<TargetValue>>)
SWIFT_CC(swift) SWIFT_RUNTIME_STDLIB_INTERNAL
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
SWIFT_CC(swift) SWIFT_RUNTIME_STDLIB_INTERNAL
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
SWIFT_CC(swift) SWIFT_RUNTIME_STDLIB_INTERNAL
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
SWIFT_CC(swift) SWIFT_RUNTIME_STDLIB_INTERNAL
bool _swift_dictionaryDownCastConditionalIndirect(OpaqueValue *destination,
                                        OpaqueValue *source,
                                        const Metadata *sourceKeyType,
                                        const Metadata *sourceValueType,
                                        const Metadata *targetKeyType,
                                        const Metadata *targetValueType,
                                        const void *sourceKeyHashable,
                                        const void *targetKeyHashable);

#if SWIFT_OBJC_INTEROP
// Helper to memoize bridging conformance data for a particular
// Swift struct type.  This is used to speed up the most common
// ObjC->Swift bridging conversions by eliminating repeated
// protocol conformance lookups.

// Currently used only for String, which may be the only
// type used often enough to justify the extra static memory.
struct ObjCBridgeMemo {
#if !NDEBUG
  // Used in assert build to verify that we always get called with
  // the same destType
  const Metadata *destType;
#endif
  const _ObjectiveCBridgeableWitnessTable *destBridgeWitness;
  const Metadata *targetBridgedType;
  Class targetBridgedObjCClass;
  swift_once_t fetchWitnessOnce;

  DynamicCastResult tryBridge(
    OpaqueValue *destLocation, const Metadata *destType,
    OpaqueValue *srcValue, const Metadata *srcType,
    const Metadata *&destFailureType, const Metadata *&srcFailureType,
    bool takeOnSuccess, bool mayDeferChecks)
    {
      struct SetupData {
        const Metadata *destType;
        struct ObjCBridgeMemo *memo;
      } setupData { destType, this };

      swift_once(&fetchWitnessOnce,
                 [](void *data) {
                   struct SetupData *setupData = (struct SetupData *)data;
                   struct ObjCBridgeMemo *memo = setupData->memo;
#if !NDEBUG
                   memo->destType = setupData->destType;
#endif
                   memo->destBridgeWitness = findBridgeWitness(setupData->destType);
                   if (memo->destBridgeWitness == nullptr) {
                     memo->targetBridgedType = nullptr;
                     memo->targetBridgedObjCClass = nullptr;
                   } else {
                     memo->targetBridgedType = _getBridgedObjectiveCType(
                       MetadataState::Complete, setupData->destType, memo->destBridgeWitness).Value;
                     assert(memo->targetBridgedType->getKind() == MetadataKind::ObjCClassWrapper);
                     memo->targetBridgedObjCClass = memo->targetBridgedType->getObjCClassObject();
                     assert(memo->targetBridgedObjCClass != nullptr);
                   }
                 }, (void *)&setupData);

      // Check that this always gets called with the same destType.
      assert((destType == this->destType) && "ObjC casting memo used inconsistently");

      // !! If bridging is not usable, stop here.
      if (targetBridgedObjCClass == nullptr) {
        return DynamicCastResult::Failure;
      }
      // Use the dynamic ISA type of the object always (Note that this
      // also implicitly gives us the ObjC type for a CF object.)
      void *srcObject = getNonNullSrcObject(srcValue, srcType, destType);
      // If srcObject is null, then we're in backwards compatibility mode.
      if (srcObject == nullptr) {
        return DynamicCastResult::Failure;
      }
      Class srcObjCType = object_getClass((id)srcObject);
      // Fail if the ObjC object is not a subclass of the bridge class.
      while (srcObjCType != targetBridgedObjCClass) {
        srcObjCType = class_getSuperclass(srcObjCType);
        if (srcObjCType == nullptr) {
          return DynamicCastResult::Failure;
        }
      }
      // The ObjC object is an acceptable type, so call the bridge function...
      return _tryCastFromClassToObjCBridgeable(
        destLocation, destType, srcValue, srcType, srcObject,
        destFailureType, srcFailureType,
        takeOnSuccess, mayDeferChecks,
        destBridgeWitness, targetBridgedType);
    }
};
#endif

static DynamicCastResult
tryCastToAnyHashable(
  OpaqueValue *destLocation, const Metadata *destType,
  OpaqueValue *srcValue, const Metadata *srcType,
  const Metadata *&destFailureType, const Metadata *&srcFailureType,
  bool takeOnSuccess, bool mayDeferChecks)
{
  assert(srcType != destType);
  assert(destType->getKind() == MetadataKind::Struct);
  assert(cast<StructMetadata>(destType)->Description
         == &STRUCT_TYPE_DESCR_SYM(s11AnyHashable));

  const HashableWitnessTable *hashableConformance = nullptr;
  
  switch (srcType->getKind()) {
  case MetadataKind::ForeignClass: // CF -> String
  case MetadataKind::ObjCClassWrapper: { // Obj-C -> String
#if SWIFT_OBJC_INTEROP
    auto cls = srcType;
    auto nsString = getNSStringMetadata();
    do {
      if (cls == nsString) {
        hashableConformance = getNSStringHashableConformance();
        break;
      }
      cls = _swift_class_getSuperclass(cls);
    } while (cls != nullptr);
    break;
#else
    // If no Obj-C interop, just fall through to the general case.
    break;
#endif
  }
  case MetadataKind::Optional: {
    // FIXME: https://github.com/apple/swift/issues/51550
    // Until the interactions between AnyHashable and Optional is fixed, we
    // avoid directly injecting Optionals.  In particular, this allows casts
    // from [String?:String] to [AnyHashable:Any] to work the way people
    // expect. Otherwise, the resulting dictionary can only be indexed with an
    // explicit Optional<String>, not a plain String.
    // After fixing the issue, we can consider dropping this special
    // case entirely.

    // !!!!  This breaks compatibility with compiler-optimized casts
    // (which just inject) and violates the Casting Spec.  It just preserves
    // the behavior of the older casting code until we can clean things up.
    auto srcInnerType = cast<EnumMetadata>(srcType)->getGenericArgs()[0];
    unsigned sourceEnumCase = srcInnerType->vw_getEnumTagSinglePayload(
      srcValue, /*emptyCases=*/1);
    auto nonNil = (sourceEnumCase == 0);
    if (nonNil) {
      return DynamicCastResult::Failure;  // Our caller will unwrap the optional and try again
    }
    // Else Optional is nil -- the general case below will inject it
    break;
  }
  default:
    break;
  }


  // General case: If it conforms to Hashable, we cast it
  if (hashableConformance == nullptr) {
    hashableConformance = reinterpret_cast<const HashableWitnessTable *>(
      swift_conformsToProtocolCommon(srcType, &HashableProtocolDescriptor)
    );
  }
  if (hashableConformance) {
    _swift_convertToAnyHashableIndirect(srcValue, destLocation,
                                        srcType, hashableConformance);
    return DynamicCastResult::SuccessViaCopy;
  } else {
    return DynamicCastResult::Failure;
  }
}

static DynamicCastResult
tryCastToArray(
  OpaqueValue *destLocation, const Metadata *destType,
  OpaqueValue *srcValue, const Metadata *srcType,
  const Metadata *&destFailureType, const Metadata *&srcFailureType,
  bool takeOnSuccess, bool mayDeferChecks)
{
  assert(srcType != destType);
  assert(destType->getKind() == MetadataKind::Struct);
  assert(cast<StructMetadata>(destType)->Description
         == &NOMINAL_TYPE_DESCR_SYM(Sa));

  switch (srcType->getKind()) {
  case MetadataKind::Struct: { // Struct -> Array
    const auto srcStructType = cast<StructMetadata>(srcType);
    if (srcStructType->Description == &NOMINAL_TYPE_DESCR_SYM(Sa)) { // Array -> Array
      auto sourceArgs = srcType->getGenericArgs();
      auto destArgs = destType->getGenericArgs();
      if (mayDeferChecks) {
        _swift_arrayDownCastIndirect(
          srcValue, destLocation, sourceArgs[0], destArgs[0]);
        return DynamicCastResult::SuccessViaCopy;
      } else {
        auto result = _swift_arrayDownCastConditionalIndirect(
          srcValue, destLocation, sourceArgs[0], destArgs[0]);
        if (result) {
          return DynamicCastResult::SuccessViaCopy;
        }
      }
    }
    break;
  }

  default:
    break;
  }

  return DynamicCastResult::Failure;
}

static DynamicCastResult
tryCastToDictionary(
  OpaqueValue *destLocation, const Metadata *destType,
  OpaqueValue *srcValue, const Metadata *srcType,
  const Metadata *&destFailureType, const Metadata *&srcFailureType,
  bool takeOnSuccess, bool mayDeferChecks)
{
  assert(srcType != destType);
  assert(destType->getKind() == MetadataKind::Struct);
  assert(cast<StructMetadata>(destType)->Description
         == &NOMINAL_TYPE_DESCR_SYM(SD));

  switch (srcType->getKind()) {
  case MetadataKind::Struct: { // Struct -> Dictionary
    const auto srcStructType = cast<StructMetadata>(srcType);
    if (srcStructType->Description == &NOMINAL_TYPE_DESCR_SYM(SD)) { // Dictionary -> Dictionary
      auto sourceArgs = srcType->getGenericArgs();
      auto destArgs = destType->getGenericArgs();
      if (mayDeferChecks) {
        _swift_dictionaryDownCastIndirect(
          srcValue, destLocation, sourceArgs[0], sourceArgs[1],
          destArgs[0], destArgs[1], sourceArgs[2], destArgs[2]);
        return DynamicCastResult::SuccessViaCopy;
      } else {
        auto result = _swift_dictionaryDownCastConditionalIndirect(
          srcValue, destLocation, sourceArgs[0], sourceArgs[1],
          destArgs[0], destArgs[1], sourceArgs[2], destArgs[2]);
        if (result) {
          return DynamicCastResult::SuccessViaCopy;
        }
      }
    }
    break;
  }

  default:
    break;
  }
  return DynamicCastResult::Failure;
}

static DynamicCastResult
tryCastToSet(
  OpaqueValue *destLocation, const Metadata *destType,
  OpaqueValue *srcValue, const Metadata *srcType,
  const Metadata *&destFailureType, const Metadata *&srcFailureType,
  bool takeOnSuccess, bool mayDeferChecks)
{
  assert(srcType != destType);
  assert(destType->getKind() == MetadataKind::Struct);
  assert(cast<StructMetadata>(destType)->Description
         == &NOMINAL_TYPE_DESCR_SYM(Sh));

  switch (srcType->getKind()) {

  case MetadataKind::Struct: { // Struct -> Set
    const auto srcStructType = cast<StructMetadata>(srcType);
    if (srcStructType->Description == &NOMINAL_TYPE_DESCR_SYM(Sh)) { // Set -> Set
      auto sourceArgs = srcType->getGenericArgs();
      auto destArgs = destType->getGenericArgs();
      if (mayDeferChecks) {
        _swift_setDownCastIndirect(srcValue, destLocation,
          sourceArgs[0], destArgs[0], sourceArgs[1], destArgs[1]);
        return DynamicCastResult::SuccessViaCopy;
      } else {
        auto result = _swift_setDownCastConditionalIndirect(
          srcValue, destLocation,
          sourceArgs[0], destArgs[0],
          sourceArgs[1], destArgs[1]);
        if (result) {
          return DynamicCastResult::SuccessViaCopy;
        }
      }
    }
    break;
  }

  default:
    break;
  }
  return DynamicCastResult::Failure;
}

static DynamicCastResult
tryCastToString(
  OpaqueValue *destLocation, const Metadata *destType,
  OpaqueValue *srcValue, const Metadata *srcType,
  const Metadata *&destFailureType, const Metadata *&srcFailureType,
  bool takeOnSuccess, bool mayDeferChecks)
{
  assert(srcType != destType);
  assert(destType->getKind() == MetadataKind::Struct);
  assert(cast<StructMetadata>(destType)->Description
         == &NOMINAL_TYPE_DESCR_SYM(SS));

  switch (srcType->getKind()) {
  case MetadataKind::ForeignClass: // CF -> String
  case MetadataKind::ObjCClassWrapper: { // Obj-C -> String
#if SWIFT_OBJC_INTEROP
    static ObjCBridgeMemo memo;
    return memo.tryBridge(
      destLocation, destType, srcValue, srcType,
      destFailureType, srcFailureType,
      takeOnSuccess, mayDeferChecks);
#else
    SWIFT_FALLTHROUGH;
#endif
  }
  default:
    return DynamicCastResult::Failure;
  }
  return DynamicCastResult::Failure;
}

static DynamicCastResult
tryCastToStruct(
  OpaqueValue *destLocation, const Metadata *destType,
  OpaqueValue *srcValue, const Metadata *srcType,
  const Metadata *&destFailureType, const Metadata *&srcFailureType,
  bool takeOnSuccess, bool mayDeferChecks)
{
  assert(srcType != destType);
  assert(destType->getKind() == MetadataKind::Struct);

  // There is no special cast handling at present for general Struct types.

  // Special logic for AnyHashable, Set, Dictionary, Array, and String
  // is broken out above.  See also selectCasterForDest() for the
  // logic that chooses one of these functions.

  return DynamicCastResult::Failure;
}

/******************************************************************************/
/*************************** Optional Destination *****************************/
/******************************************************************************/

static DynamicCastResult
tryCastToOptional(
  OpaqueValue *destLocation, const Metadata *destType,
  OpaqueValue *srcValue, const Metadata *srcType,
  const Metadata *&destFailureType, const Metadata *&srcFailureType,
  bool takeOnSuccess, bool mayDeferChecks)
{
  assert(srcType != destType);
  assert(destType->getKind() == MetadataKind::Optional);

  // Nothing to do for the basic casting operation.
  // Unwrapping is handled by top-level tryCast with assistance
  // from utility functions below.

  return DynamicCastResult::Failure;
}

// The nil value `T?.none` can be cast to any optional type.
// When the unwrapper sees a source value that is nil, it calls
// tryCastFromNil() to try to set the target optional to nil.
//
// This is complicated by the desire to preserve the nesting
// as far as possible.  For example, we would like:
//   Int?.none => Int??.some(.none)
//   Int??.none => Any????.some(.some(.none))
// Of course, if the target is shallower than the source,
// then we have to just set the outermost optional to nil.

// This helper sets a nested optional to nil at a requested level:
static void
initializeToNilAtDepth(OpaqueValue *destLocation, const Metadata *destType, int depth) {
  assert(destType->getKind() == MetadataKind::Optional);
  auto destInnerType = cast<EnumMetadata>(destType)->getGenericArgs()[0];
  if (depth > 0) {
    initializeToNilAtDepth(destLocation, destInnerType, depth - 1);
    // Set .some at each level as we unwind
    destInnerType->vw_storeEnumTagSinglePayload(
      destLocation, 0, 1);
  } else {
    // Set .none at the lowest level
    destInnerType->vw_storeEnumTagSinglePayload(
      destLocation, 1, 1);
  }
}

static void
copyNilPreservingDepth(OpaqueValue *destLocation, const Metadata *destType, const Metadata *srcType)
{
  assert(srcType->getKind() == MetadataKind::Optional);
  assert(destType->getKind() == MetadataKind::Optional);

  // Measure how deep the source nil is: Is it Int?.none or Int??.none or ...
  auto srcInnerType = cast<EnumMetadata>(srcType)->getGenericArgs()[0];
  int srcDepth = 1;
  while (srcInnerType->getKind() == MetadataKind::Optional) {
    srcInnerType = cast<EnumMetadata>(srcInnerType)->getGenericArgs()[0];
    srcDepth += 1;
  }

  // Measure how deep the destination optional is
  auto destInnerType = cast<EnumMetadata>(destType)->getGenericArgs()[0];
  int destDepth = 1;
  while (destInnerType->getKind() == MetadataKind::Optional) {
    destInnerType = cast<EnumMetadata>(destInnerType)->getGenericArgs()[0];
    destDepth += 1;
  }

  // Recursively set the destination to .some(.some(... .some(.none)))
  auto targetDepth = std::max(destDepth - srcDepth, 0);
  initializeToNilAtDepth(destLocation, destType, targetDepth);
}

// Try unwrapping both source and dest optionals together.
// If the source is nil, then cast that to the destination.
static DynamicCastResult
tryCastUnwrappingOptionalBoth(
  OpaqueValue *destLocation, const Metadata *destType,
  OpaqueValue *srcValue, const Metadata *srcType,
  const Metadata *&destFailureType, const Metadata *&srcFailureType,
  bool takeOnSuccess, bool mayDeferChecks)
{
  assert(destType->getKind() == MetadataKind::Optional);
  assert(srcType->getKind() == MetadataKind::Optional);

  auto srcInnerType = cast<EnumMetadata>(srcType)->getGenericArgs()[0];
  unsigned sourceEnumCase = srcInnerType->vw_getEnumTagSinglePayload(
    srcValue, /*emptyCases=*/1);
  auto sourceIsNil = (sourceEnumCase != 0);
  if (sourceIsNil) {
    if (runtime::bincompat::useLegacyOptionalNilInjectionInCasting()) {
      auto destInnerType = cast<EnumMetadata>(destType)->getGenericArgs()[0];
      // Set .none at the outer level
      destInnerType->vw_storeEnumTagSinglePayload(destLocation, 1, 1);
    } else {
      copyNilPreservingDepth(destLocation, destType, srcType);
    }
    return DynamicCastResult::SuccessViaCopy; // nil was essentially copied to dest
  } else {
    auto destEnumType = cast<EnumMetadata>(destType);
    const Metadata *destInnerType = destEnumType->getGenericArgs()[0];
    auto destInnerLocation = destLocation; // Single-payload enum layout
    auto subcastResult = tryCast(
      destInnerLocation, destInnerType, srcValue, srcInnerType,
      destFailureType, srcFailureType, takeOnSuccess, mayDeferChecks);
    if (isSuccess(subcastResult)) {
      destInnerType->vw_storeEnumTagSinglePayload(
        destLocation, /*case*/ 0, /*emptyCases*/ 1);
    }
    return subcastResult;
  }
  return DynamicCastResult::Failure;
}

// Try unwrapping just the destination optional.
// Note we do this even if both src and dest are optional.
// For example, Int -> Any? requires unwrapping the destination
// in order to inject the Int into the existential.
static DynamicCastResult
tryCastUnwrappingOptionalDestination(
  OpaqueValue *destLocation, const Metadata *destType,
  OpaqueValue *srcValue, const Metadata *srcType,
  const Metadata *&destFailureType, const Metadata *&srcFailureType,
  bool takeOnSuccess, bool mayDeferChecks)
{
  assert(destType->getKind() == MetadataKind::Optional);

  auto destEnumType = cast<EnumMetadata>(destType);
  const Metadata *destInnerType = destEnumType->getGenericArgs()[0];
  auto destInnerLocation = destLocation; // Single-payload enum layout
  auto subcastResult = tryCast(
    destInnerLocation, destInnerType, srcValue, srcType,
    destFailureType, srcFailureType, takeOnSuccess, mayDeferChecks);
  if (isSuccess(subcastResult)) {
    destInnerType->vw_storeEnumTagSinglePayload(
      destLocation, /*case*/ 0, /*emptyCases*/ 1);
  }
  return subcastResult;
}

// Try unwrapping just the source optional.
// Note we do this even if both target and dest are optional.
// For example, this is used when extracting the contents of
// an Optional<Any>.
static DynamicCastResult
tryCastUnwrappingOptionalSource(
  OpaqueValue *destLocation, const Metadata *destType,
  OpaqueValue *srcValue, const Metadata *srcType,
  const Metadata *&destFailureType, const Metadata *&srcFailureType,
  bool takeOnSuccess, bool mayDeferChecks)
{
  assert(srcType->getKind() == MetadataKind::Optional);

  auto srcInnerType = cast<EnumMetadata>(srcType)->getGenericArgs()[0];
  unsigned sourceEnumCase = srcInnerType->vw_getEnumTagSinglePayload(
    srcValue, /*emptyCases=*/1);
  auto nonNil = (sourceEnumCase == 0);
  if (nonNil) {
    // Recurse with unwrapped source
    return tryCast(destLocation, destType, srcValue, srcInnerType,
      destFailureType, srcFailureType, takeOnSuccess, mayDeferChecks);
  }
  return DynamicCastResult::Failure;
}

/******************************************************************************/
/***************************** Tuple Destination ******************************/
/******************************************************************************/

// The only thing that can be legally cast to a tuple is another tuple.
// Most of the logic below is required to handle element-wise casts of
// tuples that are compatible but not of the same type.

static DynamicCastResult
tryCastToTuple(
  OpaqueValue *destLocation, const Metadata *destType,
  OpaqueValue *srcValue, const Metadata *srcType,
  const Metadata *&destFailureType, const Metadata *&srcFailureType,
  bool takeOnSuccess, bool mayDeferChecks)
{
  assert(srcType != destType);
  assert(destType->getKind() == MetadataKind::Tuple);
  const auto destTupleType = cast<TupleTypeMetadata>(destType);

  srcFailureType = srcType;
  destFailureType = destType;

  // We cannot cast non-tuple data to a tuple
  if (srcType->getKind() != MetadataKind::Tuple) {
    return DynamicCastResult::Failure;
  }
  const auto srcTupleType = cast<TupleTypeMetadata>(srcType);

  // Tuples must have same number of elements
  if (srcTupleType->NumElements != destTupleType->NumElements) {
    return DynamicCastResult::Failure;
  }

  // Common labels must match
  const char *srcLabels = srcTupleType->Labels;
  const char *destLabels = destTupleType->Labels;
  while (srcLabels != nullptr
         && destLabels != nullptr
         && srcLabels != destLabels)
  {
    const char *srcSpace = strchr(srcLabels, ' ');
    const char *destSpace = strchr(destLabels, ' ');

    // If we've reached the end of either label string, we're done.
    if (srcSpace == nullptr || destSpace == nullptr) {
      break;
    }

    // If both labels are non-empty, and the labels mismatch, we fail.
    if (srcSpace != srcLabels && destSpace != destLabels) {
      unsigned srcLen = srcSpace - srcLabels;
      unsigned destLen = destSpace - destLabels;
      if (srcLen != destLen ||
          strncmp(srcLabels, destLabels, srcLen) != 0)
        return DynamicCastResult::Failure;
    }

    srcLabels = srcSpace + 1;
    destLabels = destSpace + 1;
  }

  // Compare the element types to see if they all match.
  bool typesMatch = true;
  auto numElements = srcTupleType->NumElements;
  for (unsigned i = 0; typesMatch && i != numElements; ++i) {
    if (srcTupleType->getElement(i).Type != destTupleType->getElement(i).Type) {
      typesMatch = false;
      break;
    }
  }

  if (typesMatch) {
    // The actual element types are identical, so we can use the
    // fast value-witness machinery for the whole tuple.
    if (takeOnSuccess) {
      srcType->vw_initializeWithTake(destLocation, srcValue);
      return DynamicCastResult::SuccessViaTake;
    } else {
      srcType->vw_initializeWithCopy(destLocation, srcValue);
      return DynamicCastResult::SuccessViaCopy;
    }
  } else {
    // Slow path casts each item separately.
    for (unsigned j = 0, n = srcTupleType->NumElements; j != n; ++j) {
      const auto &srcElt = srcTupleType->getElement(j);
      const auto &destElt = destTupleType->getElement(j);
      auto subcastResult = tryCast(destElt.findIn(destLocation), destElt.Type,
                                   srcElt.findIn(srcValue), srcElt.Type,
                                   destFailureType, srcFailureType,
                                   false, mayDeferChecks);
      if (subcastResult == DynamicCastResult::Failure) {
        for (unsigned k = 0; k != j; ++k) {
          const auto &elt = destTupleType->getElement(k);
          elt.Type->vw_destroy(elt.findIn(destLocation));
        }
        return DynamicCastResult::Failure;
      }
    }
    // We succeeded by casting each item.
    return DynamicCastResult::SuccessViaCopy;
  }

}

/******************************************************************************/
/**************************** Function Destination ****************************/
/******************************************************************************/

// The only thing that can be legally cast to a function is another function.

static DynamicCastResult
tryCastToFunction(
  OpaqueValue *destLocation, const Metadata *destType,
  OpaqueValue *srcValue, const Metadata *srcType,
  const Metadata *&destFailureType, const Metadata *&srcFailureType,
  bool takeOnSuccess, bool mayDeferChecks)
{
  assert(srcType != destType);
  assert(destType->getKind() == MetadataKind::Function);
  const auto destFuncType = cast<FunctionTypeMetadata>(destType);

  // Function casts succeed on exact matches, or if the target type is
  // throwier than the source type.
  //
  // TODO: We could also allow ABI-compatible variance, such as casting
  // a dynamic Base -> Derived to Derived -> Base. We wouldn't be able to
  // perform a dynamic cast that required any ABI adjustment without a JIT
  // though.

  if (srcType->getKind() != MetadataKind::Function) {
    return DynamicCastResult::Failure;
  }
  auto srcFuncType = cast<FunctionTypeMetadata>(srcType);

  // Check that argument counts and convention match (but ignore
  // "throws" for now).
  if (srcFuncType->Flags.withThrows(false)
      != destFuncType->Flags.withThrows(false)) {
    return DynamicCastResult::Failure;
  }

  // If the target type can't throw, neither can the source.
  if (srcFuncType->isThrowing() && !destFuncType->isThrowing())
    return DynamicCastResult::Failure;

  // The result and argument types must match.
  if (srcFuncType->ResultType != destFuncType->ResultType)
    return DynamicCastResult::Failure;
  if (srcFuncType->getNumParameters() != destFuncType->getNumParameters())
    return DynamicCastResult::Failure;
  if (srcFuncType->hasParameterFlags() != destFuncType->hasParameterFlags())
    return DynamicCastResult::Failure;
  for (unsigned i = 0, e = srcFuncType->getNumParameters(); i < e; ++i) {
    if (srcFuncType->getParameter(i) != destFuncType->getParameter(i) ||
        srcFuncType->getParameterFlags(i) != destFuncType->getParameterFlags(i))
      return DynamicCastResult::Failure;
  }

  // Everything matches, so we can take/copy the function reference.
  if (takeOnSuccess) {
    srcType->vw_initializeWithTake(destLocation, srcValue);
    return DynamicCastResult::SuccessViaTake;
  } else {
    srcType->vw_initializeWithCopy(destLocation, srcValue);
    return DynamicCastResult::SuccessViaCopy;
  }
}

/******************************************************************************/
/************************** Existential Destination ***************************/
/******************************************************************************/

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
    if (!swift::_conformsToProtocol(value, type, protocol, conformances))
      return false;
    if (conformances != nullptr && protocol.needsWitnessTable()) {
      assert(*conformances != nullptr);
      ++conformances;
    }
  }

  return true;
}

// Cast to unconstrained `Any`
static DynamicCastResult
tryCastToUnconstrainedOpaqueExistential(
  OpaqueValue *destLocation, const Metadata *destType,
  OpaqueValue *srcValue, const Metadata *srcType,
  const Metadata *&destFailureType, const Metadata *&srcFailureType,
  bool takeOnSuccess, bool mayDeferChecks)
{
  assert(srcType != destType);
  assert(destType->getKind() == MetadataKind::Existential);
  assert(cast<ExistentialTypeMetadata>(destType)->getRepresentation()
         == ExistentialTypeRepresentation::Opaque);
  auto destExistential
    = reinterpret_cast<OpaqueExistentialContainer *>(destLocation);

  // Fill in the type and value.
  destExistential->Type = srcType;
  auto *destBox = srcType->allocateBoxForExistentialIn(&destExistential->Buffer);
  if (takeOnSuccess) {
    srcType->vw_initializeWithTake(destBox, srcValue);
    return DynamicCastResult::SuccessViaTake;
  } else {
    srcType->vw_initializeWithCopy(destBox, srcValue);
    return DynamicCastResult::SuccessViaCopy;
  }
}

// Cast to constrained `Any`
static DynamicCastResult
tryCastToConstrainedOpaqueExistential(
  OpaqueValue *destLocation, const Metadata *destType,
  OpaqueValue *srcValue, const Metadata *srcType,
  const Metadata *&destFailureType, const Metadata *&srcFailureType,
  bool takeOnSuccess, bool mayDeferChecks)
{
  assert(srcType != destType);
  assert(destType->getKind() == MetadataKind::Existential);
  auto destExistentialType = cast<ExistentialTypeMetadata>(destType);
  assert(destExistentialType->getRepresentation()
         == ExistentialTypeRepresentation::Opaque);
  auto destExistential
    = reinterpret_cast<OpaqueExistentialContainer *>(destLocation);

  // Check for protocol conformances and fill in the witness tables.
  // TODO (rdar://17033499) If the source is an existential, we should
  // be able to compare the protocol constraints more efficiently than this.
  if (_conformsToProtocols(srcValue, srcType, destExistentialType,
                           destExistential->getWitnessTables())) {
    return tryCastToUnconstrainedOpaqueExistential(
      destLocation, destType, srcValue, srcType,
      destFailureType, srcFailureType, takeOnSuccess, mayDeferChecks);
  } else {
    return DynamicCastResult::Failure;
  }
}

static DynamicCastResult
tryCastToClassExistential(
  OpaqueValue *destLocation, const Metadata *destType,
  OpaqueValue *srcValue, const Metadata *srcType,
  const Metadata *&destFailureType, const Metadata *&srcFailureType,
  bool takeOnSuccess, bool mayDeferChecks)
{
  assert(srcType != destType);
  assert(destType->getKind() == MetadataKind::Existential);
  auto destExistentialType = cast<ExistentialTypeMetadata>(destType);
  assert(destExistentialType->getRepresentation()
         == ExistentialTypeRepresentation::Class);
  auto destExistentialLocation
    = reinterpret_cast<ClassExistentialContainer *>(destLocation);

  MetadataKind srcKind = srcType->getKind();
  switch (srcKind) {

  case MetadataKind::Metatype: {
#if SWIFT_OBJC_INTEROP
    // Get an object reference to the metatype and try fitting that into
    // the existential...
    auto metatypePtr = reinterpret_cast<const Metadata **>(srcValue);
    auto metatype = *metatypePtr;
    if (auto tmp = swift_dynamicCastMetatypeToObjectConditional(metatype)) {
      auto value = reinterpret_cast<OpaqueValue *>(&tmp);
      auto type = reinterpret_cast<const Metadata *>(tmp);
      if (_conformsToProtocols(value, type, destExistentialType,
                               destExistentialLocation->getWitnessTables())) {
        auto object = *(reinterpret_cast<HeapObject **>(value));
        destExistentialLocation->Value = object;
        if (takeOnSuccess) {
          // We copied the pointer without retain, so the source is no
          // longer valid...
          return DynamicCastResult::SuccessViaTake;
        } else {
          swift_unknownObjectRetain(object);
          return DynamicCastResult::SuccessViaCopy;
        }
      } else {
        // We didn't assign to destination, so the source reference
        // is still valid and the reference count is still correct.
      }
    }
#endif
    return DynamicCastResult::Failure;
  }

  case MetadataKind::ObjCClassWrapper:
#if SWIFT_OBJC_INTEROP
    id srcObject;
    memcpy(&srcObject, srcValue, sizeof(id));
    if (!runtime::bincompat::useLegacySwiftValueUnboxingInCasting()) {
      if (getAsSwiftValue(srcObject) != nullptr) {
	// Do not directly cast a `__SwiftValue` box
	// Return failure so our caller will unwrap and try again
	return DynamicCastResult::Failure;
      }
    }
#endif
    SWIFT_FALLTHROUGH;
  case MetadataKind::Class:
  case MetadataKind::ForeignClass: {
    auto srcObject = getNonNullSrcObject(srcValue, srcType, destType);
    // If srcObject is null, then we're in compatibility mode.
    // Earlier cast logic always succeeded `as!` casts of nil
    // class references:
    if (srcObject == nullptr) {
      if (mayDeferChecks) {
        *reinterpret_cast<const void **>(destLocation) = nullptr;
        return DynamicCastResult::SuccessViaCopy;
      } else {
        return DynamicCastResult::Failure;
      }
    }
    if (_conformsToProtocols(srcValue, srcType,
                             destExistentialType,
                             destExistentialLocation->getWitnessTables())) {
      destExistentialLocation->Value = srcObject;
      if (takeOnSuccess) {
        return DynamicCastResult::SuccessViaTake;
      } else {
        swift_unknownObjectRetain(srcObject);
        return DynamicCastResult::SuccessViaCopy;
      }
    }
    return DynamicCastResult::Failure;
  }

  default:
    return DynamicCastResult::Failure;
  }

  return DynamicCastResult::Failure;
}

// SwiftValue boxing is a failsafe that we only want to invoke
// after other approaches have failed.  This is why it's not
// integrated into tryCastToClassExistential() above.
static DynamicCastResult
tryCastToClassExistentialViaSwiftValue(
  OpaqueValue *destLocation, const Metadata *destType,
  OpaqueValue *srcValue, const Metadata *srcType,
  const Metadata *&destFailureType, const Metadata *&srcFailureType,
  bool takeOnSuccess, bool mayDeferChecks)
{
  assert(srcType != destType);
  assert(destType->getKind() == MetadataKind::Existential);
  auto destExistentialType = cast<ExistentialTypeMetadata>(destType);
  assert(destExistentialType->getRepresentation()
         == ExistentialTypeRepresentation::Class);
  auto destExistentialLocation
    = reinterpret_cast<ClassExistentialContainer *>(destLocation);

  switch (srcType->getKind()) {
  case MetadataKind::Class:
  case MetadataKind::ObjCClassWrapper:
  case MetadataKind::ForeignClass:
    // Class references always go directly into
    // class existentials; it makes no sense to wrap them.
    return DynamicCastResult::Failure;

  case MetadataKind::Metatype: {
#if SWIFT_OBJC_INTEROP
    auto metatypePtr = reinterpret_cast<const Metadata **>(srcValue);
    auto metatype = *metatypePtr;
    switch (metatype->getKind()) {
    case MetadataKind::Class:
    case MetadataKind::ObjCClassWrapper:
    case MetadataKind::ForeignClass:
      // Exclude class metatypes on Darwin, since those are object types and can
      // be stored directly.
      return DynamicCastResult::Failure;
    default:
      break;
    }
#endif
    // Non-class metatypes are never objects, and
    // metatypes on non-Darwin are never objects, so
    // fall through to box those.
    SWIFT_FALLTHROUGH;
  }

  default: {
    // We can always box when the destination is a simple
    // (unconstrained) `AnyObject`.
    if (destExistentialType->NumProtocols != 0) {
      // But if there are constraints...
      if (!runtime::bincompat::useLegacyObjCBoxingInCasting()) {
        // ... never box if we're not supporting legacy semantics.
        return DynamicCastResult::Failure;
      }
      // Legacy behavior: We used to permit casts to a constrained (existential)
      // type if the resulting `__SwiftValue` box conformed to the target type.
      // This is no longer supported, since it caused `x is NSCopying` to be
      // true even when x does not in fact implement the requirements of
      // `NSCopying`.
#if SWIFT_OBJC_INTEROP
      if (!findSwiftValueConformances(
            destExistentialType, destExistentialLocation->getWitnessTables())) {
        return DynamicCastResult::Failure;
      }
#else
      if (!swift_swiftValueConformsTo(destType, destType)) {
        return DynamicCastResult::Failure;
      }
#endif
    }

#if SWIFT_OBJC_INTEROP
    auto object = bridgeAnythingToSwiftValueObject(
      srcValue, srcType, takeOnSuccess);
    destExistentialLocation->Value = object;
    if (takeOnSuccess) {
      return DynamicCastResult::SuccessViaTake;
    } else {
      return DynamicCastResult::SuccessViaCopy;
    }
# else
    // Note: Code below works correctly on both Obj-C and non-Obj-C platforms,
    // but the code above is slightly faster on Obj-C platforms.
    auto object = _bridgeAnythingToObjectiveC(srcValue, srcType);
    destExistentialLocation->Value = object;
    return DynamicCastResult::SuccessViaCopy;
#endif
  }
  }
}

static DynamicCastResult
tryCastToErrorExistential(
  OpaqueValue *destLocation, const Metadata *destType,
  OpaqueValue *srcValue, const Metadata *srcType,
  const Metadata *&destFailureType, const Metadata *&srcFailureType,
  bool takeOnSuccess, bool mayDeferChecks)
{
  assert(srcType != destType);
  assert(destType->getKind() == MetadataKind::Existential);
  auto destExistentialType = cast<ExistentialTypeMetadata>(destType);
  assert(destExistentialType->getRepresentation()
         == ExistentialTypeRepresentation::Error);
  auto destBoxAddr = reinterpret_cast<SwiftError **>(destLocation);

  MetadataKind srcKind = srcType->getKind();
  switch (srcKind) {
  case MetadataKind::ForeignClass: // CF object => Error
  case MetadataKind::ObjCClassWrapper: // Obj-C object => Error
  case MetadataKind::Struct: // Struct => Error
  case MetadataKind::Enum: // Enum => Error
  case MetadataKind::Class: {  // Class => Error
    assert(destExistentialType->NumProtocols == 1);
    const WitnessTable *errorWitness;
    if (_conformsToProtocols(
          srcValue, srcType, destExistentialType, &errorWitness)) {
#if SWIFT_OBJC_INTEROP
      // If it already holds an NSError, just use that.
      if (auto embedded = getErrorEmbeddedNSErrorIndirect(
            srcValue, srcType, errorWitness)) {
        *destBoxAddr = reinterpret_cast<SwiftError *>(embedded);
        return DynamicCastResult::SuccessViaCopy;
      }
#endif

      BoxPair destBox = swift_allocError(
        srcType, errorWitness, srcValue, takeOnSuccess);
      *destBoxAddr = reinterpret_cast<SwiftError *>(destBox.object);
      if (takeOnSuccess) {
        return DynamicCastResult::SuccessViaTake;
      } else {
        return DynamicCastResult::SuccessViaCopy;
      }
    }
    return DynamicCastResult::Failure;
  }

  default:
    return DynamicCastResult::Failure;
  }
}

static DynamicCastResult
tryCastUnwrappingExistentialSource(
  OpaqueValue *destLocation, const Metadata *destType,
  OpaqueValue *srcValue, const Metadata *srcType,
  const Metadata *&destFailureType, const Metadata *&srcFailureType,
  bool takeOnSuccess, bool mayDeferChecks)
{
  assert(srcType != destType);
  assert(srcType->getKind() == MetadataKind::Existential);

  auto srcExistentialType = cast<ExistentialTypeMetadata>(srcType);

  // Unpack the existential content
  const Metadata *srcInnerType;
  OpaqueValue *srcInnerValue;
  switch (srcExistentialType->getRepresentation()) {
  case ExistentialTypeRepresentation::Class: {
    auto classContainer
      = reinterpret_cast<ClassExistentialContainer *>(srcValue);
    srcInnerType = swift_getObjectType((HeapObject *)classContainer->Value);
    srcInnerValue = reinterpret_cast<OpaqueValue *>(&classContainer->Value);
    break;
  }
  case ExistentialTypeRepresentation::Opaque: {
    auto opaqueContainer
      = reinterpret_cast<OpaqueExistentialContainer*>(srcValue);
    srcInnerType = opaqueContainer->Type;
    srcInnerValue = srcExistentialType->projectValue(srcValue);
    break;
  }
  case ExistentialTypeRepresentation::Error: {
    const SwiftError *errorBox
      = *reinterpret_cast<const SwiftError * const *>(srcValue);
    auto srcErrorValue
      = errorBox->isPureNSError() ? srcValue : errorBox->getValue();
    srcInnerType = errorBox->getType();
    srcInnerValue = const_cast<OpaqueValue *>(srcErrorValue);
    break;
  }
  }

  srcFailureType = srcInnerType;
  return tryCast(destLocation, destType,
                 srcInnerValue, srcInnerType,
                 destFailureType, srcFailureType,
                 takeOnSuccess && (srcInnerValue == srcValue),
                 mayDeferChecks);
}

static DynamicCastResult tryCastUnwrappingExtendedExistentialSource(
    OpaqueValue *destLocation, const Metadata *destType, OpaqueValue *srcValue,
    const Metadata *srcType, const Metadata *&destFailureType,
    const Metadata *&srcFailureType, bool takeOnSuccess, bool mayDeferChecks) {
  assert(srcType != destType);
  assert(srcType->getKind() == MetadataKind::ExtendedExistential);

  auto srcExistentialType = cast<ExtendedExistentialTypeMetadata>(srcType);

  // Unpack the existential content
  const Metadata *srcInnerType = nullptr;
  OpaqueValue *srcInnerValue = nullptr;
  switch (srcExistentialType->Shape->Flags.getSpecialKind()) {
  case ExtendedExistentialTypeShape::SpecialKind::None: {
    auto opaqueContainer =
        reinterpret_cast<OpaqueExistentialContainer *>(srcValue);
    srcInnerType = opaqueContainer->Type;
    srcInnerValue = const_cast<OpaqueValue *>(opaqueContainer->projectValue());
    break;
  }
  case ExtendedExistentialTypeShape::SpecialKind::Class: {
    auto classContainer =
        reinterpret_cast<ClassExistentialContainer *>(srcValue);
    srcInnerType = swift_getObjectType((HeapObject *)classContainer->Value);
    srcInnerValue = reinterpret_cast<OpaqueValue *>(&classContainer->Value);
    break;
  }
  case ExtendedExistentialTypeShape::SpecialKind::Metatype: {
    auto srcExistentialContainer =
        reinterpret_cast<ExistentialMetatypeContainer *>(srcValue);
    srcInnerType = swift_getMetatypeMetadata(srcExistentialContainer->Value);
    srcInnerValue = reinterpret_cast<OpaqueValue *>(&srcExistentialContainer->Value);
    break;
  }
  case ExtendedExistentialTypeShape::SpecialKind::ExplicitLayout: {
    swift_unreachable("Explicit layout not yet implemented");
    break;
  }
  }

  srcFailureType = srcInnerType;
  return tryCast(destLocation, destType, srcInnerValue, srcInnerType,
                 destFailureType, srcFailureType,
                 takeOnSuccess && (srcInnerValue == srcValue), mayDeferChecks);
}

static DynamicCastResult
tryCastUnwrappingExistentialMetatypeSource(
  OpaqueValue *destLocation, const Metadata *destType,
  OpaqueValue *srcValue, const Metadata *srcType,
  const Metadata *&destFailureType, const Metadata *&srcFailureType,
  bool takeOnSuccess, bool mayDeferChecks)
{
  assert(srcType != destType);
  assert(srcType->getKind() == MetadataKind::ExistentialMetatype);

  auto srcExistentialContainer = reinterpret_cast<ExistentialMetatypeContainer *>(srcValue);
  auto srcInnerValue = reinterpret_cast<OpaqueValue *>(&srcExistentialContainer->Value);
  assert((const void *)srcInnerValue == (const void *)srcValue);
  auto srcInnerValueAsType = srcExistentialContainer->Value;
  const Metadata *srcInnerType = swift_getMetatypeMetadata(srcInnerValueAsType);
  srcFailureType = srcInnerType;
  return tryCast(destLocation, destType,
                 srcInnerValue, srcInnerType,
                 destFailureType, srcFailureType,
                 takeOnSuccess && (srcInnerValue == srcValue),
                 mayDeferChecks);
}


static DynamicCastResult tryCastToExtendedExistential(
    OpaqueValue *destLocation, const Metadata *destType, OpaqueValue *srcValue,
    const Metadata *srcType, const Metadata *&destFailureType,
    const Metadata *&srcFailureType, bool takeOnSuccess, bool mayDeferChecks) {
  assert(srcType != destType);
  assert(destType->getKind() == MetadataKind::ExtendedExistential);

  auto destExistentialType = cast<ExtendedExistentialTypeMetadata>(destType);
  auto *destExistentialShape = destExistentialType->Shape;
  const unsigned shapeArgumentCount =
      destExistentialShape->getGenSigArgumentLayoutSizeInWords();
  const Metadata *selfType = srcType;

  // If we have a type expression to look into, unwrap as much metatype
  // structure as possible so we can reach the type metadata for the 'Self'
  // parameter.
  if (destExistentialShape->Flags.hasTypeExpression()) {
    Demangler dem;
    auto *node = dem.demangleType(destExistentialShape->getTypeExpression()->name.get());
    if (!node)
      return DynamicCastResult::Failure;

    while (node->getKind() == Demangle::Node::Kind::Type &&
           node->getNumChildren() &&
           node->getChild(0)->getKind() == Demangle::Node::Kind::Metatype &&
           node->getChild(0)->getNumChildren()) {
      auto *metatypeMetadata = dyn_cast<MetatypeMetadata>(selfType);
      if (!metatypeMetadata)
        return DynamicCastResult::Failure;

      selfType = metatypeMetadata->InstanceType;
      node = node->getChild(0)->getChild(0);
    }

    // Make sure the thing we've pulled out at the end is a dependent
    // generic parameter.
    if (!(node->getKind() == Demangle::Node::Kind::Type &&
          node->getNumChildren() &&
          node->getChild(0)->getKind() ==
              Demangle::Node::Kind::DependentGenericParamType))
      return DynamicCastResult::Failure;
  }

  llvm::SmallVector<const void *, 8> allGenericArgsVec;
  unsigned witnessesMark = 0;
  {
    // Line up the arguments to the requirement signature.
    auto genArgs = destExistentialType->getGeneralizationArguments();
    allGenericArgsVec.append(genArgs, genArgs + shapeArgumentCount);
    // Tack on the `Self` argument.
    allGenericArgsVec.push_back((const void *)selfType);
    // Mark the point where the generic arguments end.
    // _checkGenericRequirements is going to fill in a set of witness tables
    // after that.
    witnessesMark = allGenericArgsVec.size();

    SubstGenericParametersFromMetadata substitutions(destExistentialShape,
                                                     allGenericArgsVec.data());
    // Verify the requirements in the requirement signature against the
    // arguments from the source value.
    auto error = swift::_checkGenericRequirements(
        destExistentialShape->getRequirementSignature().getRequirements(),
        allGenericArgsVec,
        [&substitutions](unsigned depth, unsigned index) {
          // FIXME: Variadic generics
          return substitutions.getMetadata(depth, index).getMetadata();
        },
        [](const Metadata *type, unsigned index) -> const WitnessTable * {
          swift_unreachable("Resolution of witness tables is not supported");
        });
    if (error)
      return DynamicCastResult::Failure;
  }

  OpaqueValue *destBox = nullptr;
  const WitnessTable **destWitnesses = nullptr;
  switch (destExistentialShape->Flags.getSpecialKind()) {
  case ExtendedExistentialTypeShape::SpecialKind::None: {
    auto destExistential =
        reinterpret_cast<OpaqueExistentialContainer *>(destLocation);

    // Allocate a box and fill in the type information.
    destExistential->Type = srcType;
    destBox = srcType->allocateBoxForExistentialIn(&destExistential->Buffer);
    destWitnesses = destExistential->getWitnessTables();
    break;
  }
  case ExtendedExistentialTypeShape::SpecialKind::Class: {
    auto destExistential =
        reinterpret_cast<ClassExistentialContainer *>(destLocation);
    destBox = reinterpret_cast<OpaqueValue *>(&destExistential->Value);
    destWitnesses = destExistential->getWitnessTables();
    break;
  }
  case ExtendedExistentialTypeShape::SpecialKind::Metatype: {
    auto destExistential =
        reinterpret_cast<ExistentialMetatypeContainer *>(destLocation);
    destBox = reinterpret_cast<OpaqueValue *>(&destExistential->Value);
    destWitnesses = destExistential->getWitnessTables();
    break;
  }
  case ExtendedExistentialTypeShape::SpecialKind::ExplicitLayout:
    swift_unreachable("Witnesses for explicit layout not yet implemented");
  }

  // Fill in the trailing set of witness tables.
  const unsigned numWitnessTables = allGenericArgsVec.size() - witnessesMark;
  assert(numWitnessTables ==
         llvm::count_if(destExistentialShape->getRequirementSignature().getRequirements(),
                        [](const auto &req) -> bool {
                          return req.getKind() ==
                                 GenericRequirementKind::Protocol;
                        }));
  for (unsigned i = 0; i < numWitnessTables; ++i) {
    const auto witness = i + witnessesMark;
    destWitnesses[i] =
        reinterpret_cast<const WitnessTable *>(allGenericArgsVec[witness]);
  }

  if (takeOnSuccess) {
    srcType->vw_initializeWithTake(destBox, srcValue);
    return DynamicCastResult::SuccessViaTake;
  } else {
    srcType->vw_initializeWithCopy(destBox, srcValue);
    return DynamicCastResult::SuccessViaCopy;
  }
}

/******************************************************************************/
/**************************** Opaque Destination ******************************/
/******************************************************************************/

static DynamicCastResult
tryCastToOpaque(
  OpaqueValue *destLocation, const Metadata *destType,
  OpaqueValue *srcValue, const Metadata *srcType,
  const Metadata *&destFailureType, const Metadata *&srcFailureType,
  bool takeOnSuccess, bool mayDeferChecks)
{
  assert(srcType != destType);
  assert(destType->getKind() == MetadataKind::Opaque);

  // There's nothing special we can do here, but we have to have this
  // empty function in order for the general casting logic to run
  // for these types.

  return DynamicCastResult::Failure;
}

/******************************************************************************/
/**************************** Metatype Destination ****************************/
/******************************************************************************/

#if SWIFT_OBJC_INTEROP
/// Check whether an unknown class instance is actually a type/metatype object.
static const Metadata *_getUnknownClassAsMetatype(void *object) {
  // Objective-C class metadata are objects, so an AnyObject (or
  // NSObject) may refer to a class object.

  // Test whether the object's isa is a metaclass, which indicates that
  // the object is a class.

  Class isa = object_getClass((id)object);
  if (class_isMetaClass(isa)) {
    return swift_getObjCClassMetadata((const ClassMetadata *)object);
  }

  return nullptr;
}
#endif

static DynamicCastResult
tryCastToMetatype(
  OpaqueValue *destLocation, const Metadata *destType,
  OpaqueValue *srcValue, const Metadata *srcType,
  const Metadata *&destFailureType, const Metadata *&srcFailureType,
  bool takeOnSuccess, bool mayDeferChecks)
{
  assert(srcType != destType);
  assert(destType->getKind() == MetadataKind::Metatype);

  const MetatypeMetadata *destMetatypeType = cast<MetatypeMetadata>(destType);
  MetadataKind srcKind = srcType->getKind();
  switch (srcKind) {
  case MetadataKind::Metatype: {
    const Metadata *srcMetatype = *(const Metadata * const *) srcValue;
    if (auto result = swift_dynamicCastMetatype(
          srcMetatype, destMetatypeType->InstanceType)) {
      *((const Metadata **) destLocation) = result;
      return DynamicCastResult::SuccessViaCopy;
    }
    return DynamicCastResult::Failure;
  }

  case MetadataKind::Class:
  case MetadataKind::ObjCClassWrapper: {
#if SWIFT_OBJC_INTEROP
    // Some classes are actually metatypes
    void *srcObject = getNonNullSrcObject(srcValue, srcType, destType);
    // If object is null, then we're in compatibility mode.
    // Continuing here at all is dangerous, but that's what the
    // pre-Swift-5.4 casting logic did.
    if (srcObject == nullptr) {
      return DynamicCastResult::Failure;
    }
    if (auto metatype = _getUnknownClassAsMetatype(srcObject)) {
      auto srcInnerValue = reinterpret_cast<OpaqueValue *>(&metatype);
      auto srcInnerType = swift_getMetatypeMetadata(metatype);
      return tryCast(destLocation, destType, srcInnerValue, srcInnerType,
        destFailureType, srcFailureType, takeOnSuccess, mayDeferChecks);
    }
#endif
    return DynamicCastResult::Failure;
  }

  default:
    return DynamicCastResult::Failure;
  }
}

/// Perform a dynamic cast of a metatype to an existential metatype type.
static DynamicCastResult
_dynamicCastMetatypeToExistentialMetatype(
  OpaqueValue *destLocation,  const ExistentialMetatypeMetadata *destType,
  const Metadata *srcMetatype,
  const Metadata *&destFailureType, const Metadata *&srcFailureType,
  bool takeOnSuccess, bool mayDeferChecks)
{
  // The instance type of an existential metatype must be either an
  // existential or an existential metatype.
  auto destMetatype
    = reinterpret_cast<ExistentialMetatypeContainer *>(destLocation);

  // If it's an existential, we need to check for conformances.
  auto targetInstanceType = destType->InstanceType;
  if (auto targetInstanceTypeAsExistential =
        dyn_cast<ExistentialTypeMetadata>(targetInstanceType)) {
    // Check for conformance to all the protocols.
    // TODO: collect the witness tables.
    const WitnessTable **conformance
      = destMetatype ? destMetatype->getWitnessTables() : nullptr;
    if (!_conformsToProtocols(nullptr, srcMetatype,
                              targetInstanceTypeAsExistential,
                              conformance)) {
      return DynamicCastResult::Failure;
    }

    if (destMetatype)
      destMetatype->Value = srcMetatype;
    return DynamicCastResult::SuccessViaCopy;
  }

  // Otherwise, we're casting to SomeProtocol.Type.Type.
  auto targetInstanceTypeAsMetatype =
    cast<ExistentialMetatypeMetadata>(targetInstanceType);

  // If the source type isn't a metatype, the cast fails.
  auto srcMetatypeMetatype = dyn_cast<MetatypeMetadata>(srcMetatype);
  if (!srcMetatypeMetatype) {
    return DynamicCastResult::Failure;
  }

  // The representation of an existential metatype remains consistent
  // arbitrarily deep: a metatype, followed by some protocols.  The
  // protocols are the same at every level, so we can just set the
  // metatype correctly and then recurse, letting the recursive call
  // fill in the conformance information correctly.

  // Proactively set the destination metatype so that we can tail-recur,
  // unless we've already done so.  There's no harm in doing this even if
  // the cast fails.
  if (destLocation)
    *((const Metadata **) destLocation) = srcMetatype;

  // Recurse.
  auto srcInstanceType = srcMetatypeMetatype->InstanceType;
  return _dynamicCastMetatypeToExistentialMetatype(
    nullptr,
    targetInstanceTypeAsMetatype,
    srcInstanceType,
    destFailureType,
    srcFailureType,
    takeOnSuccess, mayDeferChecks);
}

// "ExistentialMetatype" is the metatype for an existential type.
static DynamicCastResult
tryCastToExistentialMetatype(
  OpaqueValue *destLocation, const Metadata *destType,
  OpaqueValue *srcValue, const Metadata *srcType,
  const Metadata *&destFailureType, const Metadata *&srcFailureType,
  bool takeOnSuccess, bool mayDeferChecks)
{
  assert(srcType != destType);
  assert(destType->getKind() == MetadataKind::ExistentialMetatype);

  auto destExistentialMetatypeType
    = cast<ExistentialMetatypeMetadata>(destType);
  MetadataKind srcKind = srcType->getKind();
  switch (srcKind) {
  case MetadataKind::Metatype: { // Metatype => ExistentialMetatype
    const Metadata *srcMetatype = *(const Metadata * const *) srcValue;
    return _dynamicCastMetatypeToExistentialMetatype(
      destLocation,
      destExistentialMetatypeType,
      srcMetatype,
      destFailureType,
      srcFailureType,
      takeOnSuccess, mayDeferChecks);
  }

  case MetadataKind::ObjCClassWrapper: {
    // Some Obj-C classes are actually metatypes
#if SWIFT_OBJC_INTEROP
    void *srcObject = getNonNullSrcObject(srcValue, srcType, destType);
    // If srcObject is null, we're in compatibility mode.
    // Continuing here at al is dangerous, but that's what the
    // pre-Swift-5.4 casting logic did.
    if (srcObject == nullptr) {
      return DynamicCastResult::Failure;
    }
    if (auto metatype = _getUnknownClassAsMetatype(srcObject)) {
      return _dynamicCastMetatypeToExistentialMetatype(
        destLocation,
        destExistentialMetatypeType,
        metatype,
        destFailureType,
        srcFailureType,
        takeOnSuccess, mayDeferChecks);
    }
#endif
    return DynamicCastResult::Failure;
  }

  default:
    return DynamicCastResult::Failure;
  }
}

/******************************************************************************/
/********************************** Dispatch **********************************/
/******************************************************************************/

// A layer of functions that evaluate the source and/or destination types
// in order to invoke a tailored casting operation above.
//
// This layer also deals with general issues of unwrapping box types
// and invoking bridging conversions defined via the _ObjectiveCBridgeable
// protocol.
//
// Most of the caster functions above should be fairly simple:
// * They should deal with a single target type,
// * They should assume the source is fully unwrapped,
// * They should not try to report or cleanup failure,
// * If they can take, they should report the source was destroyed.

// Based on the destination type alone, select a targeted casting function.
// This design avoids some redundant inspection of the destination type
// data, for example, when we unwrap source boxes.
static tryCastFunctionType *selectCasterForDest(const Metadata *destType) {
  auto destKind = destType->getKind();
  switch (destKind) {
  case MetadataKind::Class:
    return tryCastToSwiftClass;
  case MetadataKind::Struct: {
    const auto targetDescriptor = cast<StructMetadata>(destType)->Description;
    if (targetDescriptor == &NOMINAL_TYPE_DESCR_SYM(SS)) {
      return tryCastToString;
    }
    if (targetDescriptor == &STRUCT_TYPE_DESCR_SYM(s11AnyHashable)) {
      return tryCastToAnyHashable;
    }
    if (targetDescriptor == &NOMINAL_TYPE_DESCR_SYM(Sa)) {
      return tryCastToArray;
    }
    if (targetDescriptor == &NOMINAL_TYPE_DESCR_SYM(SD)) {
      return tryCastToDictionary;
    }
    if (targetDescriptor == &NOMINAL_TYPE_DESCR_SYM(Sh)) {
      return tryCastToSet;
    }
    return tryCastToStruct;
  }
  case MetadataKind::Enum:
    return tryCastToEnum;
  case MetadataKind::Optional:
    return tryCastToOptional;
  case MetadataKind::ForeignClass:
    return tryCastToForeignClass;
  case MetadataKind::Opaque:
    return tryCastToOpaque;
  case MetadataKind::Tuple:
    return tryCastToTuple;
  case MetadataKind::Function:
    return tryCastToFunction;
  case MetadataKind::Existential: {
    auto existentialType = cast<ExistentialTypeMetadata>(destType);
    switch (existentialType->getRepresentation()) {
    case ExistentialTypeRepresentation::Opaque:
      if (existentialType->NumProtocols == 0) {
        return tryCastToUnconstrainedOpaqueExistential;  // => Unconstrained Any
      } else {
        return tryCastToConstrainedOpaqueExistential; // => Non-class-constrained protocol
      }
    case ExistentialTypeRepresentation::Class:
      return tryCastToClassExistential; // => AnyObject, with or without protocol constraints
    case ExistentialTypeRepresentation::Error: // => Error existential
      return tryCastToErrorExistential;
    }
    swift_unreachable(
      "Unknown existential type representation in dynamic cast dispatch");
  }
  case MetadataKind::ExtendedExistential:
    return tryCastToExtendedExistential;
  case MetadataKind::Metatype:
    return tryCastToMetatype;
  case MetadataKind::ObjCClassWrapper:
    return tryCastToObjectiveCClass;
  case MetadataKind::ExistentialMetatype:
    return tryCastToExistentialMetatype;
  case MetadataKind::HeapLocalVariable:
  case MetadataKind::HeapGenericLocalVariable:
  case MetadataKind::ErrorObject:
    // These are internal details of runtime-only structures,
    // so will never appear in compiler-generated types.
    // As such, they don't need support here.
    swift_unreachable(
      "Unexpected MetadataKind in dynamic cast dispatch");
    return nullptr;
  default:
    // If you see this message, then there is a new MetadataKind that I didn't
    // know about when I wrote this code.  Please figure out what it is, how to
    // handle it, and add a case for it.
    swift_unreachable(
      "Unknown MetadataKind in dynamic cast dispatch");
  }
}

// This top-level driver provides the general flow for all casting
// operations.  It recursively unwraps source and destination as it
// searches for a suitable conversion.
static DynamicCastResult
tryCast(
  OpaqueValue *destLocation, const Metadata *destType,
  OpaqueValue *srcValue, const Metadata *srcType,
  const Metadata *&destFailureType, const Metadata *&srcFailureType,
  bool takeOnSuccess, bool mayDeferChecks)
{
  destFailureType = destType;
  srcFailureType = srcType;

  ////////////////////////////////////////////////////////////////////////
  //
  // 1. If types match exactly, we can just move/copy the data.
  // (The tryCastToXyz functions never see this trivial case.)
  //
  if (srcType == destType) {
    if (takeOnSuccess) {
      destType->vw_initializeWithTake(destLocation, srcValue);
      return DynamicCastResult::SuccessViaTake;
    } else {
      destType->vw_initializeWithCopy(destLocation, srcValue);
      return DynamicCastResult::SuccessViaCopy;
    }
  }

  auto destKind = destType->getKind();
  auto srcKind = srcType->getKind();

  ////////////////////////////////////////////////////////////////////////
  //
  // 2. Try directly casting the current srcValue to the target type.
  //    (If the dynamic type is different, try that too.)
  //
  auto tryCastToDestType = selectCasterForDest(destType);
  if (tryCastToDestType == nullptr) {
    return DynamicCastResult::Failure;
  }
  auto castResult = tryCastToDestType(destLocation, destType, srcValue,
    srcType, destFailureType, srcFailureType, takeOnSuccess, mayDeferChecks);
  if (isSuccess(castResult)) {
    return castResult;
  }
  if (srcKind == MetadataKind::Class
      || srcKind == MetadataKind::ObjCClassWrapper
      || srcKind == MetadataKind::ForeignClass) {
    auto srcObject = getNonNullSrcObject(srcValue, srcType, destType);
    // If srcObject is null, we're in compatibility mode.
    // But we can't lookup dynamic type for a null class reference, so
    // just skip this in that case.
    if (srcObject != nullptr) {
      auto srcDynamicType = swift_getObjectType(srcObject);
      if (srcDynamicType != srcType) {
        srcFailureType = srcDynamicType;
        auto castResult = tryCastToDestType(
          destLocation, destType, srcValue, srcDynamicType,
          destFailureType, srcFailureType, takeOnSuccess, mayDeferChecks);
        if (isSuccess(castResult)) {
          return castResult;
        }
      }
    }
  }

  ////////////////////////////////////////////////////////////////////////
  //
  // 3. Try recursively unwrapping _source_ boxes, including
  //    existentials, AnyHashable, SwiftValue, and Error.
  //
  switch (srcKind) {

  case MetadataKind::Class: {
#if !SWIFT_OBJC_INTEROP
    // Try unwrapping native __SwiftValue implementation
    auto subcastResult = tryCastUnwrappingSwiftValueSource(
      destLocation, destType, srcValue, srcType,
      destFailureType, srcFailureType, takeOnSuccess, mayDeferChecks);
    if (isSuccess(subcastResult)) {
      return subcastResult;
    }
#endif
    break;
  }

  case MetadataKind::ObjCClassWrapper: {
#if SWIFT_OBJC_INTEROP
    // Try unwrapping Obj-C __SwiftValue implementation
    auto subcastResult = tryCastUnwrappingObjCSwiftValueSource(
      destLocation, destType, srcValue, srcType,
      destFailureType, srcFailureType, takeOnSuccess, mayDeferChecks);
    if (isSuccess(subcastResult)) {
      return subcastResult;
    }
#endif

#if SWIFT_OBJC_INTEROP
    // Try unwrapping Obj-C NSError container
    auto innerFlags = DynamicCastFlags::Default;
    if (tryDynamicCastNSErrorToValue(
          destLocation, srcValue, srcType, destType, innerFlags)) {
      return DynamicCastResult::SuccessViaCopy;
    }
#endif
    break;
  }

  case MetadataKind::Struct: {
    auto srcStructType = cast<StructMetadata>(srcType);
    auto srcStructDescription = srcStructType->getDescription();

    // Try unwrapping AnyHashable container
    if (srcStructDescription == &STRUCT_TYPE_DESCR_SYM(s11AnyHashable)) {
      if (_swift_anyHashableDownCastConditionalIndirect(
            srcValue, destLocation, destType)) {
        return DynamicCastResult::SuccessViaCopy;
      }
    }
    break;
  }

  case MetadataKind::Existential: {
    auto subcastResult = tryCastUnwrappingExistentialSource(
      destLocation, destType, srcValue, srcType,
      destFailureType, srcFailureType, takeOnSuccess, mayDeferChecks);
    if (isSuccess(subcastResult)) {
      return subcastResult;
    }
    break;
  }

  case MetadataKind::ExistentialMetatype: {
    auto subcastResult = tryCastUnwrappingExistentialMetatypeSource(
      destLocation, destType, srcValue, srcType,
      destFailureType, srcFailureType, takeOnSuccess, mayDeferChecks);
    if (isSuccess(subcastResult)) {
      return subcastResult;
    }
    break;
  }

  case MetadataKind::ExtendedExistential: {
    auto subcastResult = tryCastUnwrappingExtendedExistentialSource(
        destLocation, destType, srcValue, srcType, destFailureType,
        srcFailureType, takeOnSuccess, mayDeferChecks);
    if (isSuccess(subcastResult)) {
      return subcastResult;
    }
    break;
  }
  default:
    break;
  }

  ////////////////////////////////////////////////////////////////////////
  //
  // 4. Try recursively unwrapping Optionals.  First try jointly unwrapping
  //    both source and destination, then just destination, then just source.
  //    Note that if both are optional, we try all three of these!
  //    For example, consider casting an Optional<T> to
  //    Optional<CustomDebugStringConvertible>.  If T conforms, we need to
  //    unwrap both.  But if it doesn't, we unwrap just the destination
  //    in order to cast Optional<T> to the protocol directly.
  //
  if (destKind == MetadataKind::Optional) {
    if (srcKind == MetadataKind::Optional) {
      auto subcastResult = tryCastUnwrappingOptionalBoth(
        destLocation, destType, srcValue, srcType,
        destFailureType, srcFailureType, takeOnSuccess, mayDeferChecks);
      if (isSuccess(subcastResult)) {
        return subcastResult;
      }
    }
    auto subcastResult = tryCastUnwrappingOptionalDestination(
      destLocation, destType, srcValue, srcType,
      destFailureType, srcFailureType, takeOnSuccess, mayDeferChecks);
    if (isSuccess(subcastResult)) {
      return subcastResult;
    }
  }

  if (srcKind == MetadataKind::Optional) {
    auto subcastResult = tryCastUnwrappingOptionalSource(
      destLocation, destType, srcValue, srcType,
      destFailureType, srcFailureType, takeOnSuccess, mayDeferChecks);
    if (isSuccess(subcastResult)) {
      return subcastResult;
    }
  }

  ////////////////////////////////////////////////////////////////////////
  //
  // 5. Finally, explore bridging conversions via ObjectiveCBridgeable,
  //    Error, and __SwiftValue boxing.
  //
  switch (destKind) {

  case MetadataKind::Optional: {
    // Optional supports _ObjectiveCBridgeable from an unconstrained AnyObject
    if (srcType->getKind() == MetadataKind::Existential) {
      auto srcExistentialType = cast<ExistentialTypeMetadata>(srcType);
      if ((srcExistentialType->getRepresentation() == ExistentialTypeRepresentation::Class)
          && (srcExistentialType->NumProtocols == 0)
          && (srcExistentialType->getSuperclassConstraint() == nullptr)
          && (srcExistentialType->isClassBounded())) {
        auto toObjCResult = tryCastFromClassToObjCBridgeable(
          destLocation, destType, srcValue, srcType,
          destFailureType, srcFailureType, takeOnSuccess, false);
        if (isSuccess(toObjCResult)) {
          return toObjCResult;
        }
      }
    }

    break;
  }

  case MetadataKind::Existential: {
    // Try general machinery for stuffing values into AnyObject:
    auto destExistentialType = cast<ExistentialTypeMetadata>(destType);
    if (destExistentialType->getRepresentation() == ExistentialTypeRepresentation::Class) {
      // Some types have custom Objective-C bridging support...
      auto subcastResult = tryCastFromObjCBridgeableToClass(
        destLocation, destType, srcValue, srcType,
        destFailureType, srcFailureType, takeOnSuccess, mayDeferChecks);
      if (isSuccess(subcastResult)) {
        return subcastResult;
      }

      // Other types can be boxed into a __SwiftValue container...
      auto swiftValueCastResult = tryCastToClassExistentialViaSwiftValue(
        destLocation, destType, srcValue, srcType,
        destFailureType, srcFailureType, takeOnSuccess, mayDeferChecks);
      if (isSuccess(swiftValueCastResult)) {
        return swiftValueCastResult;
      }
    }
    break;
  }

  case MetadataKind::Class:
  case MetadataKind::ObjCClassWrapper:
  case MetadataKind::ForeignClass: {
    // Try _ObjectiveCBridgeable to bridge _to_ a class type _from_ a
    // struct/enum type.  Note: Despite the name, this is used for both
    // Swift-Swift and Swift-ObjC bridging
    if (srcKind == MetadataKind::Struct || srcKind == MetadataKind::Enum) {
      auto subcastResult = tryCastFromObjCBridgeableToClass(
        destLocation, destType, srcValue, srcType,
        destFailureType, srcFailureType, takeOnSuccess, mayDeferChecks);
      if (isSuccess(subcastResult)) {
        return subcastResult;
      }
    }

#if SWIFT_OBJC_INTEROP
    if (destKind == MetadataKind::ObjCClassWrapper) {
      // If the destination type is an NSError or NSObject, and the source type
      // is an Error, then the cast might succeed by NSError bridging.
      if (auto srcErrorWitness = findErrorWitness(srcType)) {
        if (destType == getNSErrorMetadata()
            || destType == getNSObjectMetadata()) {
          auto flags = DynamicCastFlags::Default;
          auto error = dynamicCastValueToNSError(srcValue, srcType,
                                                 srcErrorWitness, flags);
          *reinterpret_cast<id *>(destLocation) = error;
          return DynamicCastResult::SuccessViaCopy;
        }
      }
    }
#endif

    break;
  }

  case MetadataKind::Struct:
  case MetadataKind::Enum: {
    // Use _ObjectiveCBridgeable to bridge _from_ a class type _to_ a
    // struct/enum type.  Note: Despite the name, this is used for both
    // Swift-Swift and ObjC-Swift bridging
    if (srcKind == MetadataKind::Class
        || srcKind == MetadataKind::ObjCClassWrapper
        || srcKind == MetadataKind::ForeignClass) {
      auto subcastResult = tryCastFromClassToObjCBridgeable(
        destLocation, destType, srcValue, srcType,
        destFailureType, srcFailureType, takeOnSuccess, mayDeferChecks);
      if (isSuccess(subcastResult)) {
        return subcastResult;
      }
    }

    // Note: In theory, if src and dest are both struct/enum types, we could see
    // if the ObjC bridgeable class types matched and then do a two-step
    // conversion from src -> bridge class -> dest.  Such ambitious conversions
    // might cause more harm than good, though.  In particular, it could
    // undermine code that uses a series of `as?` to quickly determine how to
    // handle a particular object.
    break;
  }

  default:
    break;
  }

  return DynamicCastResult::Failure;
}

/******************************************************************************/
/****************************** Main Entrypoint *******************************/
/******************************************************************************/

/// ABI: Perform a dynamic cast to an arbitrary type.
static bool
swift_dynamicCastImpl(OpaqueValue *destLocation,
                      OpaqueValue *srcValue,
                      const Metadata *srcType,
                      const Metadata *destType,
                      DynamicCastFlags flags)
{
  // If the compiler has asked for a "take", we can
  // move pointers without ref-counting overhead.
  bool takeOnSuccess = flags & DynamicCastFlags::TakeOnSuccess;
  // Unconditional casts are allowed to crash the program on failure.
  // We can exploit that for performance: return a partial conversion
  // immediately and do additional checks lazily when the results are
  // actually accessed.
  bool mayDeferChecks = flags & DynamicCastFlags::Unconditional;

  // Attempt the cast...
  const Metadata *destFailureType = destType;
  const Metadata *srcFailureType = srcType;
  auto result = tryCast(
    destLocation, destType,
    srcValue, srcType,
    destFailureType, srcFailureType,
    takeOnSuccess, mayDeferChecks);

  switch (result) {
  case DynamicCastResult::Failure:
    if (flags & DynamicCastFlags::Unconditional) {
      swift_dynamicCastFailure(srcFailureType, destFailureType);
    }
    if (flags & DynamicCastFlags::DestroyOnFailure) {
      srcType->vw_destroy(srcValue);
    }
    return false;
  case DynamicCastResult::SuccessViaCopy:
    if (takeOnSuccess) { // We copied, but compiler asked for take.
      srcType->vw_destroy(srcValue);
    }
    return true;
  case DynamicCastResult::SuccessViaTake:
    return true;
  }
}

#define OVERRIDE_DYNAMICCASTING COMPATIBILITY_OVERRIDE
#include COMPATIBILITY_OVERRIDE_INCLUDE_PATH
