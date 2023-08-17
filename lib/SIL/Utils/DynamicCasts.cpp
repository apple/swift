//===--- DynamicCasts.cpp - Utilities for dynamic casts -------------------===//
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

#include "swift/SIL/DynamicCasts.h"
#include "swift/AST/Module.h"
#include "swift/AST/ProtocolConformance.h"
#include "swift/AST/Types.h"
#include "swift/SIL/SILArgument.h"
#include "swift/SIL/SILBuilder.h"
#include "swift/SIL/TypeLowering.h"

using namespace swift;
using namespace Lowering;

static unsigned getAnyMetatypeDepth(CanType type) {
  unsigned depth = 0;
  while (auto metatype = dyn_cast<AnyMetatypeType>(type)) {
    type = metatype.getInstanceType();
    ++depth;
  }
  return depth;
}

static bool
mayBridgeToObjectiveC(ModuleDecl *M, CanType T) {
  // FIXME: Disable when we don't support Objective-C interoperability?
  return true;
}

static bool
mustBridgeToSwiftValueBox(ModuleDecl *M, CanType T) {
  // If the target type is either an unknown dynamic type, or statically
  // known to bridge, the cast may succeed.
  if (T->hasArchetype())
    return false;

  if (T->isAnyExistentialType())
    return false;

  // getBridgedToObjC() might return a null-type for some types
  // whose bridging implementation is allowed to live elsewhere. Exclude this
  // case here.
  if (auto N = T->getAnyNominal())
    if (M->getASTContext().isTypeBridgedInExternalModule(N))
      return false;

  return !M->getASTContext().getBridgedToObjC(M, T);
}

static bool canClassOrSuperclassesHaveUnknownSubclasses(ClassDecl *CD,
                                                      bool isWholeModuleOpts) {
  while (CD) {
    // Open classes can always have unknown subclasses.
    if (CD->getEffectiveAccess() == AccessLevel::Open)
      return true;

    // Internal and public classes may have unknown subclasses if we are not in
    // whole-module-optimization mode.
    if (CD->getEffectiveAccess() >= AccessLevel::Internal &&
        !isWholeModuleOpts)
      return true;

    if (!CD->hasSuperclass())
      break;

    CD = CD->getSuperclassDecl();
  }

  return false;
}

static CanType unwrapExistential(CanType e) {
  assert(e.isExistentialType());

  if (auto et = dyn_cast<ExistentialType>(e))
    return et.getConstraintType();

  return e;
}

/// Try to classify a conversion from non-existential type
/// into an existential type by performing a static check
/// of protocol conformances if it is possible.
static DynamicCastFeasibility
classifyDynamicCastToProtocol(ModuleDecl *M, CanType source, CanType target,
                              bool isWholeModuleOpts) {
  assert(target.isExistentialType() &&
         "target should be an existential type");

  if (source == target)
    return DynamicCastFeasibility::WillSucceed;

  auto *TargetProtocol = cast_or_null<ProtocolDecl>(target.getAnyNominal());
  if (!TargetProtocol)
    return DynamicCastFeasibility::MaySucceed;

  // If the target is a parameterized protocol type, conformsToProtocol
  // is insufficient to prove the feasibility of the cast as it does not
  // check the additional requirements.
  // FIXME: This is a weak predicate that doesn't take into account
  // class compositions - since any C & P<T> doesn't work yet anyways.
  if (isa<ParameterizedProtocolType>(unwrapExistential(target)))
    return DynamicCastFeasibility::MaySucceed;

  // If conformsToProtocol returns a valid conformance, then all requirements
  // were proven by the type checker.
  if (M->conformsToProtocol(source, TargetProtocol))
    return DynamicCastFeasibility::WillSucceed;

  auto *SourceNominalTy = source.getAnyNominal();
  if (!SourceNominalTy)
    return DynamicCastFeasibility::MaySucceed;

  // Protocol types may conform to their own protocols (or other protocols)
  // in the future.
  if (source->isAnyExistentialType()) {
    return DynamicCastFeasibility::MaySucceed;
  }

  // If it is a class and it can be proven that this class and its
  // superclasses cannot have unknown subclasses, then it is safe to proceed.
  if (auto *CD = source.getClassOrBoundGenericClass()) {
    if (canClassOrSuperclassesHaveUnknownSubclasses(CD, isWholeModuleOpts))
      return DynamicCastFeasibility::MaySucceed;
    // Derived types may conform to the protocol.
    if (!CD->isFinal()) {
      // TODO: If it is a private type or internal type and we
      // can prove that there are no derived types conforming to a
      // protocol, then we can still return WillFail.
      return DynamicCastFeasibility::MaySucceed;
    }
  }

  // The WillFail conditions below assume any possible conformance on the
  // nominal source type has been ruled out. The prior conformsToProtocol query
  // identified any definite conformance. Now check if there is already a known
  // conditional conformance on the nominal type with requirements that were
  // not proven.
  //
  // TODO: The TypeChecker can easily prove that some requirements cannot be
  // met. Returning WillFail in those cases would be more optimal. To do that,
  // the conformsToProtocol interface needs to be reformulated as a query, and
  // the implementation, including checkGenericArguments, needs to be taught to
  // recognize that types with archetypes may potentially succeed.
  if (auto conformance = M->lookupConformance(source, TargetProtocol)) {
    assert(!conformance.getConditionalRequirements().empty());
    return DynamicCastFeasibility::MaySucceed;
  }

  // If the source type is file-private or target protocol is file-private,
  // then conformances cannot be changed at run-time, because only this
  // file could have implemented them, but no conformances were found.
  // Therefore it is safe to make a negative decision at compile-time.
  if (SourceNominalTy->getEffectiveAccess() <= AccessLevel::FilePrivate ||
      TargetProtocol->getEffectiveAccess() <= AccessLevel::FilePrivate) {
    // This cast is always false. Replace it with a branch to the
    // failure block.
    return DynamicCastFeasibility::WillFail;
  }

  // AnyHashable is a special case: although it's a struct, there maybe another
  // type conforming to it and to the TargetProtocol at the same time.
  if (source->isAnyHashable())
    return DynamicCastFeasibility::MaySucceed;

  // If we are in a whole-module compilation and
  // if the source type is internal or target protocol is internal,
  // then conformances cannot be changed at run-time, because only this
  // module could have implemented them, but no conformances were found.
  // Therefore it is safe to make a negative decision at compile-time.
  if (isWholeModuleOpts &&
      (SourceNominalTy->getEffectiveAccess() <= AccessLevel::Internal ||
       TargetProtocol->getEffectiveAccess() <= AccessLevel::Internal)) {
    return DynamicCastFeasibility::WillFail;
  }

  return DynamicCastFeasibility::MaySucceed;
}

static DynamicCastFeasibility
classifyDynamicCastFromProtocol(ModuleDecl *M, CanType source, CanType target,
                                bool isWholeModuleOpts) {
  assert(source.isExistentialType() &&
         "source should be an existential type");

  if (source == target)
    return DynamicCastFeasibility::WillSucceed;

  // Casts from class existential into a non-class can never succeed.
  if (source->isClassExistentialType() &&
      !target.isAnyExistentialType() &&
      !target.getClassOrBoundGenericClass() &&
      !isa<ArchetypeType>(target) &&
      !mayBridgeToObjectiveC(M, target)) {
    assert((target.getEnumOrBoundGenericEnum() ||
            target.getStructOrBoundGenericStruct() ||
            isa<TupleType>(target) ||
            isa<SILFunctionType>(target) ||
            isa<FunctionType>(target) ||
            isa<MetatypeType>(target)) &&
           "Target should be an enum, struct, tuple, metatype or function type");
    return DynamicCastFeasibility::WillFail;
  }

  // TODO: maybe prove that certain conformances are impossible?

  return DynamicCastFeasibility::MaySucceed;
}

/// Returns the existential type associated with the Hashable
/// protocol, if it can be found.
static CanType getHashableExistentialType(ModuleDecl *M) {
  auto hashable =
    M->getASTContext().getProtocol(KnownProtocolKind::Hashable);
  if (!hashable) return CanType();
  return hashable->getDeclaredInterfaceType()->getCanonicalType();
}

// Returns true if casting \p sourceFormalType to \p targetFormalType preserves
// ownership.
//
// Casting preserves ownership when all references from the source value are
// forwarded into the result value (without unbalanced retains or releases).
//
// When both the source and target types of checked-cast preserve ownership,
// then the cast is compatible with guaranteed ownership. A guaranteed
// compatible cast cannot release any references within its operand's value
// and cannot retain any references owned by its result.
//
// A type's ownership might not be preserved by a dynamic cast if it is either
//   (A) a potentially bridged value
// or
//   (B) potentially wrapped in a transparent type, which is equivalent to
//       isPotentiallyAnyObject()
//
// Given:
//   let source: sourceType
//   let dest = source as! targetType
//
// Ownership conversion happens when
//
//   (A) one type is a bridged value and the other is an object:
//
//      (A1) Boxing: <trivial> as! Object instantiates references in Object
//           Presumably, Object's type must be class-bound, but this is not
//           currently checked.
//
//      (A2) Unboxing: Object as! <trivial> destroys references in Object
//           Object may be any type that can hold an object, including
//           non-class-bound archetypes and existentials.
//
//   (B) one type is transparently wrapped in __SwiftValue, while the other is
//       unwrapped. Given:
//
//     class C : Hashable {}
//     let a = AnyHashable(C())
//
//     (B1) When the substituted source type is AnyHashable and the
//          substituted destination type is AnyObject, the cast
//          instantiates an owned __SwiftValue:
//
//          // instantiates __SwiftValue
//          let b = a as! AnyObject
//        or
//          let b = a as! T where T.self == AnyObject.self
//
//     (B2) When the substituted source type is Any or AnyObject, and the
//          substituted destination type is not Any or AnyObject, the cast
//          releases the owned __SwiftValue:
//
//          let c = b as! C // releases __SwiftValue
//
// After unwrapping Optional, the type may fall into one of
// the following categories that are relevant for cast ownership:
//
// Class-bound types (hasReferenceSemantics() && !isPotentiallyAnyObject())
// - includes classes, class-bound existentials other than AnyObject,
//   class-bound archetypes with a superclass or protocol constraint,
//   objc types, blocks, Builtin.NativeObject, etc.
// - excludes any type that are potentially AnyObject after substitution
// - the value is a single reference
// - the single reference is "known unwrapped". It never transparently wraps the
//   underlying dynamically typed value in another type, such as __SwiftValue
// - casting directly forwards the reference
//
// Potentially bridged values:
// - includes struct, enum, non-class archetype, non-class existential,
//   and non-objc-metatype
// - these types are potentially trivial after substitution. If so, then they
//   convert to a reference when casting to AnyObject or certain classes
//
// Any and AnyObject existentials:
// - although called existentials, their type is a protocol composition
// - these do not include existentials with constraints
// - these are very special types, unlike normal existentials...
// - the immediately erased value may itself be an existential
//   (an AnyObject existential can be wrapped within an Any existential!)
// - the underlying dynamically typed value may be transparently wrapped in
//   __SwiftValue
//
// These type categories are disjoint, except that a non-class archetype is both
// potentially bridged and potentially Any or AnyObject after substitution.
//
// TODO: In the future, when the runtime stops wrapping nontrivial types inside
// __SwiftValue, cases (B1) and (B2) above will no longer apply. At that time,
// expand ownership preserving cast types to AnyObject. Then remove the
// isPotentiallyAnyObject() check.
bool swift::doesCastPreserveOwnershipForTypes(SILModule &module,
                                              CanType sourceType,
                                              CanType targetType) {
  if (!canIRGenUseScalarCheckedCastInstructions(module, sourceType, targetType))
    return false;

  // (B2) unwrapping
  if (sourceType->isPotentiallyAnyObject())
    return false;

  // (B1) wrapping
  if (targetType->isPotentiallyAnyObject()) {
    // A class type cannot be wrapped in __SwiftValue, so casting
    // from a class to AnyObject preserves ownership.
    return
      sourceType->mayHaveSuperclass() || sourceType->isClassExistentialType();
  }
  return true;
}

bool SILDynamicCastInst::isRCIdentityPreserving() const {
  // Casts which cast from a trivial type, like a metatype, to something which
  // is retainable (or vice versa), like an AnyObject, are not RC identity
  // preserving.
  // On some platforms such casts dynamically allocate a ref-counted box for the
  // metatype. Naturally that is the place where a new rc-identity begins.
  // Therefore such a cast is introducing a new rc identical object.
  //
  // If RCIdentityAnalysis would look through such a cast, ARC optimizations
  // would get confused and might eliminate a retain of such an object
  // completely.
  SILFunction &f = *getFunction();
  if (getSourceLoweredType().isTrivial(f)
      && getTargetLoweredType().isTrivial(f)) {
    return true;
  }
  return doesCastPreserveOwnershipForTypes(f.getModule(), getSourceFormalType(),
                                           getTargetFormalType());
}

/// Check if a given type conforms to _BridgedToObjectiveC protocol.
bool swift::isObjectiveCBridgeable(ModuleDecl *M, CanType Ty) {
  // Retrieve the _BridgedToObjectiveC protocol.
  auto bridgedProto =
      M->getASTContext().getProtocol(KnownProtocolKind::ObjectiveCBridgeable);

  if (bridgedProto) {
    // Find the conformance of the value type to _BridgedToObjectiveC.
    // Check whether the type conforms to _BridgedToObjectiveC.
    return (bool)M->lookupConformance(Ty, bridgedProto);
  }
  return false;
}

/// Check if a given type conforms to _Error protocol.
bool swift::isError(ModuleDecl *M, CanType Ty) {
  // Retrieve the Error protocol.
  auto errorTypeProto =
      M->getASTContext().getProtocol(KnownProtocolKind::Error);

  if (errorTypeProto) {
    // Find the conformance of the value type to Error.
    // Check whether the type conforms to Error.
    return (bool)M->lookupConformance(Ty, errorTypeProto);
  }
  return false;
}

/// Given that a type is not statically known to be an optional type, check
/// whether it might dynamically be able to store an optional.
static bool canDynamicallyStoreOptional(CanType type) {
  assert(!type.getOptionalObjectType());
  return type->canDynamicallyBeOptionalType(/* includeExistential */ true);
}
  
/// Given two class types, check whether there's a hierarchy relationship
/// between them.
static DynamicCastFeasibility
classifyClassHierarchyCast(CanType source, CanType target) {
  // Upcast: if the target type statically matches a type in the
  // source type's hierarchy, this is a static upcast and the cast
  // will always succeed.
  if (target->isExactSuperclassOf(source))
    return DynamicCastFeasibility::WillSucceed;

  // Upcast: if the target type might dynamically match a type in the
  // source type's hierarchy, this might be an upcast, in which
  // case the cast might succeed.
  if (target->isBindableToSuperclassOf(source))
    return DynamicCastFeasibility::MaySucceed;

  // Downcast: if the source type might dynamically match a type in the
  // target type's hierarchy, this might be a downcast, in which case
  // the cast might succeed.  Note that this also covers the case where
  // the source type statically matches a type in the target type's
  // hierarchy; since it's a downcast, the cast still at best might succeed.
  if (source->isBindableToSuperclassOf(target))
    return DynamicCastFeasibility::MaySucceed;

  // Otherwise, the classes are unrelated and the cast will fail (at least
  // on these grounds).
  return DynamicCastFeasibility::WillFail;
}

CanType swift::getNSBridgedClassOfCFClass(ModuleDecl *M, CanType type) {
  if (auto classDecl = type->getClassOrBoundGenericClass()) {
    if (classDecl->getForeignClassKind() == ClassDecl::ForeignKind::CFType) {
      if (auto bridgedAttr =
            classDecl->getAttrs().getAttribute<ObjCBridgedAttr>()) {
        auto bridgedClass = bridgedAttr->getObjCClass();
        // TODO: this should handle generic classes properly.
        if (!bridgedClass->isGenericContext()) {
          return bridgedClass->getDeclaredInterfaceType()->getCanonicalType();
        }
      }
    }
  }
  return CanType();
}

static bool isCFBridgingConversion(ModuleDecl *M,
                                   CanType sourceFormalType,
                                   CanType targetFormalType) {
  if (auto bridgedTarget =
        getNSBridgedClassOfCFClass(M, targetFormalType)) {
    return bridgedTarget->isExactSuperclassOf(sourceFormalType);
  }
  if (auto bridgedSource =
        getNSBridgedClassOfCFClass(M, sourceFormalType)) {
    return targetFormalType->isExactSuperclassOf(bridgedSource);
  }
  
  return false;
}

/// Try to classify the dynamic-cast relationship between two types.
DynamicCastFeasibility
swift::classifyDynamicCast(ModuleDecl *M,
                           CanType source,
                           CanType target,
                           bool isSourceTypeExact,
                           bool isWholeModuleOpts) {
  if (source == target) return DynamicCastFeasibility::WillSucceed;

  // Return a conservative answer for opaque archetypes for now.
  if (source->hasOpaqueArchetype() || target->hasOpaqueArchetype())
    return DynamicCastFeasibility::MaySucceed;

  auto sourceObject = source.getOptionalObjectType();
  auto targetObject = target.getOptionalObjectType();

  // A common level of optionality doesn't affect the feasibility,
  // except that we can't fold things to failure because nil inhabits
  // both types.
  if (sourceObject && targetObject) {
    return atWorst(classifyDynamicCast(M, sourceObject, targetObject),
                   DynamicCastFeasibility::MaySucceed);

  // Casting to a more optional type follows the same rule unless we
  // know that the source cannot dynamically be an optional value,
  // in which case we'll always just cast and inject into an optional.
  } else if (targetObject) {
    auto result = classifyDynamicCast(M, source, targetObject,
                                      /* isSourceTypeExact */ false,
                                      isWholeModuleOpts);
    if (canDynamicallyStoreOptional(source))
      result = atWorst(result, DynamicCastFeasibility::MaySucceed);
    return result;

  // Casting to a less-optional type can always fail.
  } else if (sourceObject) {
    auto result = atBest(classifyDynamicCast(M, sourceObject, target,
                                             /* isSourceTypeExact */ false,
                                             isWholeModuleOpts),
                         DynamicCastFeasibility::MaySucceed);
    if (target.isExistentialType()) {
      result = atWorst(result, classifyDynamicCastToProtocol(
                                   M, source, target, isWholeModuleOpts));
    }
    return result;
  }
  assert(!sourceObject && !targetObject);

  // Assume that casts to or from existential types or involving
  // dependent types can always succeed.  This is over-conservative.
  if (source->hasArchetype() || source.isExistentialType() ||
      target->hasArchetype() || target.isExistentialType()) {

    // Check conversions from non-protocol types into protocol types.
    if (!source.isExistentialType() &&
        target.isExistentialType())
      return classifyDynamicCastToProtocol(M, source, target,
                                           isWholeModuleOpts);

    // Check conversions from protocol types to non-protocol types.
    if (source.isExistentialType() &&
        !target.isExistentialType())
      return classifyDynamicCastFromProtocol(M, source, target,
                                             isWholeModuleOpts);

    return DynamicCastFeasibility::MaySucceed;
  }

  // Casts from AnyHashable.
  if (auto sourceStruct = dyn_cast<StructType>(source)) {
    if (sourceStruct->isAnyHashable()) {
      if (auto hashable = getHashableExistentialType(M)) {
        // Succeeds if Hashable can be cast to the target type.
        return classifyDynamicCastFromProtocol(M, hashable, target,
                                               isWholeModuleOpts);
      }
    }
  }

  // Casts to AnyHashable.
  if (auto targetStruct = dyn_cast<StructType>(target)) {
    if (targetStruct->isAnyHashable()) {
      // Succeeds if the source type can be dynamically cast to Hashable.
      // Hashable is not actually a legal existential type right now, but
      // the check doesn't care about that.
      if (auto hashable = getHashableExistentialType(M)) {
        return classifyDynamicCastToProtocol(M, source, hashable,
                                             isWholeModuleOpts);
      }
    }
  }

  // Metatype casts.
  if (auto sourceMetatype = dyn_cast<AnyMetatypeType>(source)) {
    auto targetMetatype = dyn_cast<AnyMetatypeType>(target);
    if (!targetMetatype) return DynamicCastFeasibility::WillFail;

    source = sourceMetatype.getInstanceType();
    target = targetMetatype.getInstanceType();

    if (source == target &&
        targetMetatype.isAnyExistentialType() ==
            sourceMetatype.isAnyExistentialType())
      return DynamicCastFeasibility::WillSucceed;

    // If the source and target are the same existential type, but the source is
    // P.Protocol and the dest is P.Type, then we need to consider whether the
    // protocol is self-conforming.
    // The only cases where a protocol self-conforms are objc protocols, but
    // we're going to expect P.Type to hold a class object. And this case
    // doesn't matter since for a self-conforming protocol type there can't be
    // any type-level methods.
    // Thus we consider this kind of cast to always fail. The only exception
    // from this rule is when the target is Any.Type, because *.Protocol
    // can always be casted to Any.Type.
    if (source->isAnyExistentialType() && isa<MetatypeType>(sourceMetatype) &&
        isa<ExistentialMetatypeType>(targetMetatype)) {
      return target->isAny() ? DynamicCastFeasibility::WillSucceed
                             : DynamicCastFeasibility::WillFail;
    }

    if (targetMetatype.isAnyExistentialType() && target->isExistentialType()) {
      auto Feasibility =
          classifyDynamicCastToProtocol(M, source, target, isWholeModuleOpts);
      // Cast from existential metatype to existential metatype may still
      // succeed, even if we cannot prove anything statically.
      if (Feasibility != DynamicCastFeasibility::WillFail ||
          !sourceMetatype.isAnyExistentialType())
        return Feasibility;
    }

    // If isSourceTypeExact is true, we know we are casting the result of a
    // MetatypeInst instruction.
    if (isSourceTypeExact) {
      // If source or target are existentials, then it can be cast
      // successfully only into itself.
      if ((target.isAnyExistentialType() || source.isAnyExistentialType()) &&
          target != source)
        return DynamicCastFeasibility::WillFail;
    }

    // Casts from class existential metatype into a concrete non-class metatype
    // can never succeed.
    if (source->isClassExistentialType() &&
        !target.isAnyExistentialType() &&
        !target.getClassOrBoundGenericClass())
      return DynamicCastFeasibility::WillFail;

    // TODO: prove that some conversions to existential metatype will
    // obviously succeed/fail.
    // TODO: prove that some conversions from class existential metatype
    // to a concrete non-class metatype will obviously fail.
    // TODO: class metatype to/from AnyObject
    // TODO: protocol concrete metatype to/from ObjCProtocol
    if (isa<ExistentialMetatypeType>(sourceMetatype) ||
        isa<ExistentialMetatypeType>(targetMetatype))
      return (getAnyMetatypeDepth(source) == getAnyMetatypeDepth(target)
              ? DynamicCastFeasibility::MaySucceed
              : DynamicCastFeasibility::WillFail);

    // If both metatypes are class metatypes, check if classes can be
    // cast.
    if (source.getClassOrBoundGenericClass() &&
        target.getClassOrBoundGenericClass())
      return classifyClassHierarchyCast(source, target);

    // Different structs cannot be cast to each other.
    if (source.getStructOrBoundGenericStruct() &&
        target.getStructOrBoundGenericStruct() &&
        source != target)
      return DynamicCastFeasibility::WillFail;

    // Different enums cannot be cast to each other.
    if (source.getEnumOrBoundGenericEnum() &&
        target.getEnumOrBoundGenericEnum() &&
        source != target)
      return DynamicCastFeasibility::WillFail;

    // If we don't know any better, assume that the cast may succeed.
    return DynamicCastFeasibility::MaySucceed;
  }

  // Function casts.
  if (auto sourceFunction = dyn_cast<FunctionType>(source)) {
    if (auto targetFunction = dyn_cast<FunctionType>(target)) {
      // A function cast can succeed if the function types can be identical,
      // or if the target type is throwier than the original.

      // An async function cannot be cast to a non-async function and
      // vice-versa.
      if (sourceFunction->isAsync() != targetFunction->isAsync())
        return DynamicCastFeasibility::WillFail;

      // A non-throwing source function can be cast to a throwing target type,
      // but not vice versa.
      if (sourceFunction->isThrowing() && !targetFunction->isThrowing())
        return DynamicCastFeasibility::WillFail;
      
      // The cast can't change the representation at runtime.
      if (targetFunction->getRepresentation()
            != sourceFunction->getRepresentation())
        return DynamicCastFeasibility::WillFail;

      if (AnyFunctionType::equalParams(sourceFunction.getParams(),
                                       targetFunction.getParams()) &&
          sourceFunction.getResult() == targetFunction.getResult())
        return DynamicCastFeasibility::WillSucceed;

      // Be conservative about function type relationships we may add in
      // the future.
      return DynamicCastFeasibility::MaySucceed;
    }
  }

  // Tuple casts.
  if (auto sourceTuple = dyn_cast<TupleType>(source)) {
    if (auto targetTuple = dyn_cast<TupleType>(target)) {
      // # of elements must coincide.
      if (sourceTuple->getNumElements() != targetTuple->getNumElements())
        return DynamicCastFeasibility::WillFail;

      DynamicCastFeasibility result = DynamicCastFeasibility::WillSucceed;
      for (unsigned i : range(sourceTuple->getNumElements())) {
        const auto &sourceElt = sourceTuple->getElement(i);
        const auto &targetElt = targetTuple->getElement(i);

        // If both have names and the names mismatch, the cast will fail.
        if (sourceElt.hasName() && targetElt.hasName() &&
            sourceElt.getName() != targetElt.getName())
          return DynamicCastFeasibility::WillFail;

        // Combine the result of prior elements with this element type.
        result = std::max(result,
                          classifyDynamicCast(M,
                            sourceElt.getType()->getCanonicalType(),
                            targetElt.getType()->getCanonicalType(),
                            isSourceTypeExact,
                            isWholeModuleOpts));

        // If this element failed, we're done.
        if (result == DynamicCastFeasibility::WillFail)
          break;
      }

      return result;
    }
  }

  // Class casts.
  auto sourceClass = source.getClassOrBoundGenericClass();
  auto targetClass = target.getClassOrBoundGenericClass();
  if (sourceClass) {
    if (targetClass) {
      // Imported Objective-C generics don't check the generic parameters, which
      // are lost at runtime.
      if (sourceClass->isTypeErasedGenericClass()) {
      
        if (sourceClass == targetClass)
          return DynamicCastFeasibility::WillSucceed;
        
        if (targetClass->isTypeErasedGenericClass()) {
          // If both classes are ObjC generics, the cast may succeed if the
          // classes are related, irrespective of their generic parameters.

          if (sourceClass->isSuperclassOf(targetClass))
            return DynamicCastFeasibility::MaySucceed;
          
          if (targetClass->isSuperclassOf(sourceClass))
            return DynamicCastFeasibility::WillSucceed;

          return DynamicCastFeasibility::WillFail;
        }
      }

      // Try a hierarchy cast.  If that isn't failure, we can report it.
      auto hierarchyResult = classifyClassHierarchyCast(source, target);
      if (hierarchyResult != DynamicCastFeasibility::WillFail)
        return hierarchyResult;

      // As a backup, consider whether either type is a CF class type
      // with an NS bridged equivalent.
      CanType bridgedSource = getNSBridgedClassOfCFClass(M, source);
      CanType bridgedTarget = getNSBridgedClassOfCFClass(M, target);

      // If neither type qualifies, we're done.
      if (!bridgedSource && !bridgedTarget)
        return DynamicCastFeasibility::WillFail;

      // Otherwise, map over to the bridged types and try to answer the
      // question there.
      if (bridgedSource) source = bridgedSource;
      if (bridgedTarget) target = bridgedTarget;
      return classifyDynamicCast(M, source, target, false, isWholeModuleOpts);
    }

    // Casts from a class into a non-class can never succeed if the target must
    // be bridged to a SwiftValueBox. You would need an AnyObject source for
    // that.
    if (!target.isAnyExistentialType() &&
        !target.getClassOrBoundGenericClass() &&
        !isa<ArchetypeType>(target) &&
        mustBridgeToSwiftValueBox(M, target)) {
      assert((target.getEnumOrBoundGenericEnum() ||
              target.getStructOrBoundGenericStruct() ||
              isa<TupleType>(target) ||
              isa<SILFunctionType>(target) ||
              isa<FunctionType>(target) ||
              isa<MetatypeType>(target)) &&
             "Target should be an enum, struct, tuple, metatype or function type");
      return DynamicCastFeasibility::WillFail;
    }


    // In the Objective-C runtime, class metatypes are also class instances.
    // The cast may succeed if the target type can be inhabited by a class
    // metatype.
    // TODO: Narrow this to the sourceClass being exactly NSObject.
    if (M->getASTContext().LangOpts.EnableObjCInterop) {
      if (auto targetMeta = dyn_cast<MetatypeType>(target)) {
        if (isa<ArchetypeType>(targetMeta.getInstanceType())
            || targetMeta.getInstanceType()->mayHaveSuperclass())
          return DynamicCastFeasibility::MaySucceed;
      } else if (isa<ExistentialMetatypeType>(target)) {
        return DynamicCastFeasibility::MaySucceed;
      }
    }
  }

  // If the source is not existential, an archetype, or (under the ObjC runtime)
  // a class, and the destination is a metatype, there is no way the cast can
  // succeed.
  if (target->is<AnyMetatypeType>()) return DynamicCastFeasibility::WillFail;

  // FIXME: Be more careful with bridging conversions from
  // NSArray, NSDictionary and NSSet as they may fail?

  // We know that a cast from Int -> class foobar will fail.
  if (targetClass &&
      !source.isAnyExistentialType() &&
      !source.getClassOrBoundGenericClass() &&
      !isa<ArchetypeType>(source) &&
      mustBridgeToSwiftValueBox(M, source)) {
      assert((source.getEnumOrBoundGenericEnum() ||
              source.getStructOrBoundGenericStruct() ||
              isa<TupleType>(source) ||
              isa<SILFunctionType>(source) ||
              isa<FunctionType>(source) ||
              isa<MetatypeType>(source)) &&
             "Source should be an enum, struct, tuple, metatype or function type");
    return DynamicCastFeasibility::WillFail;
  }

  // Check if there might be a bridging conversion.
  if (source->isBridgeableObjectType() && mayBridgeToObjectiveC(M, target)) {
    // Try to get the ObjC type which is bridged to target type.
    assert(!target.isAnyExistentialType());
    // ObjC-to-Swift casts may fail. And in most cases it is impossible to
    // statically predict the outcome. So, let's be conservative here.
    return DynamicCastFeasibility::MaySucceed;
  }
  
  if (target->isBridgeableObjectType() && mayBridgeToObjectiveC(M, source)) {
    // Try to get the ObjC type which is bridged to source type.
    assert(!source.isAnyExistentialType());
    if (Type ObjCTy = M->getASTContext().getBridgedToObjC(M, source)) {
      // If the bridged ObjC type is known, check if
      // this type can be cast into target type.
      return classifyDynamicCast(M,
          ObjCTy->getCanonicalType(),
          target,
          /* isSourceTypeExact */ false, isWholeModuleOpts);
    }
    return DynamicCastFeasibility::MaySucceed;
  }

  // Check if it is a cast between bridged error types.
  if (isError(M, source) && isError(M, target)) {
    // TODO: Cast to NSError succeeds always.
    return DynamicCastFeasibility::MaySucceed;
  }

  // Check for a viable collection cast.
  if (auto sourceStruct = dyn_cast<BoundGenericStructType>(source)) {
    if (auto targetStruct = dyn_cast<BoundGenericStructType>(target)) {
      // Both types have to be the same kind of collection.
      if (sourceStruct->getDecl() == targetStruct->getDecl()) {
        auto sourceArgs = sourceStruct.getGenericArgs();
        auto targetArgs = targetStruct.getGenericArgs();

        // Note that we can never say that a collection cast is impossible:
        // a cast can always succeed on an empty collection.

        // Arrays and sets.
        if (sourceStruct->isArray() || sourceStruct->isSet()) {
          auto valueFeasibility =
            classifyDynamicCast(M, sourceArgs[0], targetArgs[0]);
          return atWorst(valueFeasibility,
                         DynamicCastFeasibility::MaySucceed);

        // Dictionaries.
        } else if (sourceStruct->isDictionary()) {
          auto keyFeasibility =
            classifyDynamicCast(M, sourceArgs[0], targetArgs[0]);
          auto valueFeasibility =
            classifyDynamicCast(M, sourceArgs[1], targetArgs[1]);
          return atWorst(atBest(keyFeasibility, valueFeasibility),
                         DynamicCastFeasibility::MaySucceed);
        }
      }
    }
  }

  return DynamicCastFeasibility::WillFail;
}

static unsigned getOptionalDepth(CanType type) {
  unsigned depth = 0;
  while (CanType objectType = type.getOptionalObjectType()) {
    ++depth;
    type = objectType;
  }
  return depth;
}

namespace {
  struct Source {
    SILValue Value;
    CanType FormalType;

    bool isAddress() const { return Value->getType().isAddress(); }

    SILType getSILType() const { return Value->getType(); }

    Source() = default;
    Source(SILValue value, CanType formalType)
      : Value(value), FormalType(formalType) {}
  };

  struct Target {
    SILValue Address;
    SILType LoweredType;
    CanType FormalType;

    bool isAddress() const { return (bool) Address; }

    Source asAddressSource() const {
      assert(isAddress());
      return { Address, FormalType };
    }
    Source asScalarSource(SILValue value) const {
      assert(!isAddress());
      assert(!value->getType().isAddress());
      return { value, FormalType };
    }
    SILType getSILType() const {
      if (isAddress())
        return Address->getType();
      else
        return LoweredType;
    }

    Target() = default;
    Target(SILValue address, CanType formalType)
      : Address(address), LoweredType(address->getType()),
        FormalType(formalType) {
      assert(LoweredType.isAddress());
    }
    Target(SILType loweredType, CanType formalType)
      : Address(), LoweredType(loweredType), FormalType(formalType) {
      assert(!loweredType.isAddress());
    }
  };

  class CastEmitter {
    SILBuilder &B;
    SILModule &M;
    ASTContext &Ctx;
    SILLocation Loc;
    ModuleDecl *SwiftModule;
  public:
    CastEmitter(SILBuilder &B, ModuleDecl *swiftModule, SILLocation loc)
      : B(B), M(B.getModule()), Ctx(M.getASTContext()), Loc(loc),
        SwiftModule(swiftModule) {}

    Source emitTopLevel(Source source, Target target) {
      unsigned sourceOptDepth = getOptionalDepth(source.FormalType);
      unsigned targetOptDepth = getOptionalDepth(target.FormalType);      

      assert(sourceOptDepth <= targetOptDepth);
      return emitAndInjectIntoOptionals(source, target,
                                        targetOptDepth - sourceOptDepth);
    }

  private:
    const TypeLowering &getTypeLowering(SILType type) {
      return B.getFunction().getTypeLowering(type);
    }

    SILValue getOwnedScalar(Source source, const TypeLowering &srcTL) {
      assert(!source.isAddress());
      auto value = source.Value;
      if (value->getOwnershipKind() == OwnershipKind::Guaranteed)
        value = B.emitCopyValueOperation(Loc, value);
      return value;
    }

    Source emitSameType(Source source, Target target) {
      assert(source.FormalType == target.FormalType ||
             source.getSILType() == target.getSILType());

      auto &srcTL = getTypeLowering(source.Value->getType());

      // The destination always wants a +1 value, so make the source
      // +1 if it's a scalar.
      if (!source.isAddress()) {
        source.Value = getOwnedScalar(source, srcTL);
      }

      // If we've got a scalar and want a scalar, the source is
      // exactly right.
      if (!target.isAddress() && !source.isAddress())
        return source;

      // If the destination wants a non-address value, load
      if (!target.isAddress()) {
        SILValue value = srcTL.emitLoadOfCopy(B, Loc, source.Value, IsTake);
        return target.asScalarSource(value);
      }

      if (source.isAddress()) {
        srcTL.emitCopyInto(B, Loc, source.Value, target.Address,
                           IsTake, IsInitialization);
      } else {
        srcTL.emitStoreOfCopy(B, Loc, source.Value, target.Address,
                              IsInitialization);
      }
      return target.asAddressSource();
    }

    Source emit(Source source, Target target) {
      if (source.FormalType == target.FormalType ||
          source.getSILType() == target.getSILType())
        return emitSameType(source, target);

      // Handle subtype conversions involving optionals.
      if (auto sourceObjectType = source.FormalType.getOptionalObjectType()) {
        return emitOptionalToOptional(source, sourceObjectType, target);
      }
      assert(!target.FormalType.getOptionalObjectType());

      // The only other things we return WillSucceed for currently is
      // an upcast or CF/NS toll-free-bridging conversion.
      // FIXME: Upcasts between existential metatypes are not handled yet.
      // We should generate for it:
      // %openedSrcMetatype = open_existential srcMetatype
      // init_existential dstMetatype, %openedSrcMetatype
      auto &srcTL = getTypeLowering(source.Value->getType());
      SILValue value;
      if (source.isAddress()) {
        value = srcTL.emitLoadOfCopy(B, Loc, source.Value, IsTake);
      } else {
        // May have any valid ownership.
        value = source.Value;
      }
      auto targetFormalTy = target.FormalType;
      auto targetLoweredTy =
          SILType::getPrimitiveObjectType(target.LoweredType.getASTType());
      if (isCFBridgingConversion(SwiftModule,
                                 source.FormalType,
                                 targetFormalTy)) {
        value = B.createUncheckedRefCast(Loc, value, targetLoweredTy);
      } else {
        value = B.createUpcast(Loc, value, targetLoweredTy);
      }
      // If the target is an address, then scalar must be Owned. Otherwise, it
      // may be Guaranteed.
      assert(value->getType() == target.LoweredType.getObjectType());
      if (!target.isAddress())
        return target.asScalarSource(value);

      auto &targetTL = getTypeLowering(target.LoweredType);
      targetTL.emitStoreOfCopy(B, Loc, value, target.Address, IsInitialization);
      return target.asAddressSource();
    }

    Source emitAndInjectIntoOptionals(Source source, Target target,
                                      unsigned depth) {
      if (depth == 0)
        return emit(source, target);

      // Recurse.
      EmitSomeState state;
      Target objectTarget = prepareForEmitSome(target, state);
      Source objectSource =
        emitAndInjectIntoOptionals(source, objectTarget, depth - 1);
      return emitSome(objectSource, target, state);
    }

    Source emitOptionalToOptional(Source source,
                                  CanType sourceObjectType,
                                  Target target) {
      // Switch on the incoming value.
      SILBasicBlock *contBB = B.splitBlockForFallthrough();
      SILBasicBlock *noneBB = B.splitBlockForFallthrough();
      SILBasicBlock *someBB = B.splitBlockForFallthrough();

      // Emit the switch.
      std::pair<EnumElementDecl*, SILBasicBlock*> cases[] = {
        { Ctx.getOptionalSomeDecl(), someBB },
        { Ctx.getOptionalNoneDecl(), noneBB },
      };
      if (source.isAddress()) {
        B.createSwitchEnumAddr(Loc, source.Value, /*default*/ nullptr, cases);
      } else {
        auto *switchEnum =
            B.createSwitchEnum(Loc, source.Value, /*default*/ nullptr, cases);
        switchEnum->createOptionalSomeResult();
      }

      // Create the Some block, which recurses.
      B.setInsertionPoint(someBB);
      {
        auto sourceSomeDecl = Ctx.getOptionalSomeDecl();

        SILType loweredSourceObjectType =
            source.Value->getType().getEnumElementType(
                sourceSomeDecl, M, B.getTypeExpansionContext());

        // Form the target for the optional object.
        EmitSomeState state;
        Target objectTarget = prepareForEmitSome(target, state);

        // Form the source value.
        AllocStackInst *sourceTemp = nullptr;
        Source objectSource;
        if (source.isAddress()) {
          // TODO: add an instruction for non-destructively getting a
          // specific element's data.
          SILValue sourceAddr = source.Value;
          sourceAddr = B.createUncheckedTakeEnumDataAddr(Loc, sourceAddr,
                                    sourceSomeDecl, loweredSourceObjectType);
          objectSource = Source(sourceAddr, sourceObjectType);
        } else {
          objectSource = Source(someBB->getArgument(0), sourceObjectType);
        }

        Source resultObject = emit(objectSource, objectTarget);

        // Deallocate the source temporary if we needed one.
        if (sourceTemp) {
          B.createDeallocStack(Loc, sourceTemp);
        }

        Source result = emitSome(resultObject, target, state);
        assert(result.isAddress() == target.isAddress());
        if (target.isAddress()) {
          B.createBranch(Loc, contBB);
        } else {
          auto &resultTL = getTypeLowering(result.Value->getType());
          SILValue resultVal = getOwnedScalar(source, resultTL);
          B.createBranch(Loc, contBB, {resultVal});
        }
      }

      // Create the None block.
      B.setInsertionPoint(noneBB);
      {
        Source result = emitNone(target);
        assert(result.isAddress() == target.isAddress());
        if (target.isAddress()) {
          B.createBranch(Loc, contBB);
        } else {
          B.createBranch(Loc, contBB, { result.Value });
        }
      }

      // Continuation block.
      B.setInsertionPoint(contBB);
      if (target.isAddress()) {
        return target.asAddressSource();
      } else {
        SILValue result =
            contBB->createPhiArgument(target.LoweredType, OwnershipKind::Owned);
        return target.asScalarSource(result);
      }
    }

    struct EmitSomeState {
      EnumElementDecl *SomeDecl;
    };

    Target prepareForEmitSome(Target target, EmitSomeState &state) {
      auto objectType = target.FormalType.getOptionalObjectType();
      assert(objectType && "emitting Some into non-optional type");

      auto someDecl = Ctx.getOptionalSomeDecl();
      state.SomeDecl = someDecl;

      SILType loweredObjectType = target.LoweredType.getEnumElementType(
          someDecl, M, B.getTypeExpansionContext());

      if (target.isAddress()) {
        SILValue objectAddr =
          B.createInitEnumDataAddr(Loc, target.Address, someDecl,
                                   loweredObjectType);
        return { objectAddr, objectType };
      } else {
        return { loweredObjectType, objectType };
      }
    }

    // May return an Owned or Guaranteed result. If source is has ownership
    // None, then the result may still be Guaranteed for nontrivial types.
    Source emitSome(Source source, Target target, EmitSomeState &state) {
      // If our target is an address, prepareForEmitSome should have set this
      // up so that we emitted directly into 
      if (target.isAddress()) {
        B.createInjectEnumAddr(Loc, target.Address, state.SomeDecl);
        return target.asAddressSource();
      } else {
        auto someEnum =
            B.createEnum(Loc, source.Value, state.SomeDecl, target.LoweredType);
        return target.asScalarSource(someEnum);
      }
    }

    Source emitNone(Target target) {
      auto noneDecl = Ctx.getOptionalNoneDecl();
      
      if (target.isAddress()) {
        B.createInjectEnumAddr(Loc, target.Address, noneDecl);
        return target.asAddressSource();
      } else {
        SILValue res = B.createEnum(Loc, nullptr, noneDecl, target.LoweredType);
        return target.asScalarSource(res);
      }
    }
  };
} // end anonymous namespace

SILValue
swift::emitSuccessfulScalarUnconditionalCast(SILBuilder &B, SILLocation loc,
                                             SILDynamicCastInst dynamicCast) {
  return emitSuccessfulScalarUnconditionalCast(
      B, B.getModule().getSwiftModule(), loc, dynamicCast.getSource(),
      dynamicCast.getTargetLoweredType(), dynamicCast.getSourceFormalType(),
      dynamicCast.getTargetFormalType(), dynamicCast.getInstruction());
}

/// Emit an unconditional scalar cast that's known to succeed.
SILValue
swift::emitSuccessfulScalarUnconditionalCast(SILBuilder &B, ModuleDecl *M,
                                             SILLocation loc, SILValue value,
                                             SILType targetLoweredType,
                                             CanType sourceFormalType,
                                             CanType targetFormalType,
                                             SILInstruction *existingCast) {
  assert(classifyDynamicCast(M, sourceFormalType, targetFormalType)
           == DynamicCastFeasibility::WillSucceed);

  // Casts to/from existential types cannot be further improved.
  if (sourceFormalType.isAnyExistentialType() ||
      targetFormalType.isAnyExistentialType()) {
    if (existingCast)
      // Indicate that the existing cast cannot be further improved.
      return SILValue();

    llvm_unreachable("Casts to/from existentials are not supported yet");
  }

  // Fast path changes that don't change the type.
  if (sourceFormalType == targetFormalType)
    return value;

  Source source(value, sourceFormalType);
  Target target(targetLoweredType, targetFormalType);
  Source result = CastEmitter(B, M, loc).emitTopLevel(source, target);
  assert(!result.isAddress());
  assert(result.Value->getType() == targetLoweredType);
  return result.Value;
}

bool swift::emitSuccessfulIndirectUnconditionalCast(
    SILBuilder &B, SILLocation loc, SILDynamicCastInst dynamicCast) {
  return emitSuccessfulIndirectUnconditionalCast(
      B, B.getModule().getSwiftModule(), loc, dynamicCast.getSource(),
      dynamicCast.getSourceFormalType(), dynamicCast.getDest(),
      dynamicCast.getTargetFormalType(), dynamicCast.getInstruction());
}

bool swift::emitSuccessfulIndirectUnconditionalCast(
    SILBuilder &B, ModuleDecl *M, SILLocation loc, SILValue src,
    CanType sourceFormalType, SILValue dest, CanType targetFormalType,
    SILInstruction *existingCast) {
  assert(classifyDynamicCast(M, sourceFormalType, targetFormalType)
           == DynamicCastFeasibility::WillSucceed);

  assert(src->getType().isAddress());
  assert(dest->getType().isAddress());

  // Casts between the same types can be always handled here.
  // Casts from non-existentials into existentials and
  // vice-versa cannot be improved yet.
  // Casts between a value type and a class cannot be optimized.
  // Therefore generate a simple unconditional_checked_cast_aadr.

  if (src->getType() != dest->getType())
  if (src->getType().isAnyExistentialType() !=
      dest->getType().isAnyExistentialType() ||
      !(src->getType().getClassOrBoundGenericClass() &&
       dest->getType().getClassOrBoundGenericClass())) {

    // If there is an existing cast with the same arguments,
    // indicate we cannot improve it.
    if (existingCast) {
      auto *UCCAI = dyn_cast<UnconditionalCheckedCastAddrInst>(existingCast);
      if (UCCAI && UCCAI->getSrc() == src && UCCAI->getDest() == dest
          && UCCAI->getSourceFormalType() == sourceFormalType
          && UCCAI->getTargetFormalType() == targetFormalType) {
        // Indicate that the existing cast cannot be further improved.
        return false;
      }
    }

    B.createUnconditionalCheckedCastAddr(loc,
                                         src, sourceFormalType,
                                         dest, targetFormalType);
    return true;
  }

  Source source(src, sourceFormalType);
  Target target(dest, targetFormalType);
  Source result = CastEmitter(B, M, loc).emitTopLevel(source, target);
  assert(result.isAddress());
  assert(result.Value == dest);
  (void) result;
  return true;
}

/// Can the given cast be performed by the scalar checked-cast
/// instructions at the current SIL stage?
///
/// Always returns true for !useLoweredAddresses. Scalar casts are always
/// valid for owned values. If the operand is +1, the case will always destroy
/// or forward it. The result is always either +1 or trivial. The cast never
/// hides a copy. doesCastPreserveOwnershipForTypes determines whether the
/// scalar cast is also compatible with guaranteed values.
bool swift::canSILUseScalarCheckedCastInstructions(SILModule &M,
                                                   CanType sourceFormalType,
                                                   CanType targetFormalType) {
  if (!M.useLoweredAddresses())
    return true;

  return canIRGenUseScalarCheckedCastInstructions(M, sourceFormalType,
                                                  targetFormalType);
}

/// Can the given cast be performed by the scalar checked-cast
/// instructions?
bool swift::canIRGenUseScalarCheckedCastInstructions(SILModule &M,
                                                     CanType sourceFormalType,
                                                     CanType targetFormalType) {
  // If the cast involves any kind of generalized existential we
  // need to use the indirect-cast path to handle checking the extra
  // constraints there as the scalar path does not (yet) know how to do it.
  if (sourceFormalType->hasParameterizedExistential() ||
      targetFormalType->hasParameterizedExistential()) {
    return false;
  }

  // Look through one level of optionality on the source.
  auto objectType = sourceFormalType;
  if (auto type = objectType.getOptionalObjectType())
    objectType = type;

  // Casting to NSError needs to go through the indirect-cast case,
  // since it may conform to Error and require Error-to-NSError
  // bridging, unless we can statically see that the source type inherits
  // NSError.
  
  // A class-constrained archetype may be bound to NSError, unless it has a
  // non-NSError superclass constraint. Casts to archetypes thus must always be
  // indirect.
  if (auto archetype = targetFormalType->getAs<ArchetypeType>()) {
    // Only ever permit this if the source type is a reference type.
    if (!objectType.isAnyClassReferenceType())
      return false;
    
    auto super = archetype->getSuperclass();
    if (super.isNull())
      return false;

    // A base class constraint that isn't NSError rules out the archetype being
    // bound to NSError.
    if (M.getASTContext().LangOpts.EnableObjCInterop) {
      if (auto nserror = M.Types.getNSErrorType())
         return !super->isEqual(nserror);
    }
    
    // If NSError wasn't loaded, any base class constraint must not be NSError.
    return true;
  }
  
  if (M.getASTContext().LangOpts.EnableObjCInterop
      && targetFormalType == M.Types.getNSErrorType()) {
    // If we statically know the source is an NSError subclass, then the cast
    // can go through the scalar path (and it's trivially true so can be
    // killed).
    return targetFormalType->isExactSuperclassOf(objectType);
  }
  
  // Three supported cases:
  // - metatype to metatype
  // - metatype to object
  // - object to object
  if ((objectType.isAnyClassReferenceType() || isa<AnyMetatypeType>(objectType))
      && targetFormalType.isAnyClassReferenceType())
    return true;

  if (isa<AnyMetatypeType>(objectType) && isa<AnyMetatypeType>(targetFormalType))
    return true;
  
  // Otherwise, we need to use the general indirect-cast functions.
  return false;
}

/// Carry out the operations required for an indirect conditional cast
/// using a scalar cast operation.
void swift::emitIndirectConditionalCastWithScalar(
    SILBuilder &B, ModuleDecl *M, SILLocation loc,
    CastConsumptionKind consumption,
    SILValue srcAddr, CanType sourceFormalType,
    SILValue destAddr, CanType targetFormalType,
    SILBasicBlock *indirectSuccBB, SILBasicBlock *indirectFailBB,
    ProfileCounter TrueCount, ProfileCounter FalseCount) {
  assert(canSILUseScalarCheckedCastInstructions(B.getModule(),
                                                sourceFormalType,
                                                targetFormalType));

  // Create our successor and fail blocks.
  SILBasicBlock *scalarFailBB = B.splitBlockForFallthrough();
  SILBasicBlock *scalarSuccBB = B.splitBlockForFallthrough();

  // Always take; this works under an assumption that retaining the result is
  // equivalent to retaining the source. That means that these casts would not
  // be appropriate for bridging-like conversions.
  //
  // Our plan is:
  //
  // 1. If the original cast was a take_always cast, then we take from our
  // memory location in the caller, store the value into dest in the success
  // block, and perform a destroy of our default argument in the failure block.
  //
  // 2. If the original cast was copy_on_success, then with ownership we borrow,
  // copy in the success path and store back into the source slot after copying.
  //
  // 3. If the original cast was take_on_success, then on success we place the
  // casted value into dest and on failure, store the original value back into
  // src.
  SILType targetLoweredType = destAddr->getType().getObjectType();
  // Inline constructor
  auto srcValue = ([&]() -> SILValue {
    if (consumption == CastConsumptionKind::CopyOnSuccess)
      return B.emitLoadBorrowOperation(loc, srcAddr);
    return B.emitLoadValueOperation(loc, srcAddr, LoadOwnershipQualifier::Take);
  })();

  auto *ccb = B.createCheckedCastBranch(
      loc, /*exact*/ false, srcValue, sourceFormalType, targetLoweredType,
      targetFormalType, scalarSuccBB, scalarFailBB, TrueCount, FalseCount);

  // Emit the success block.
  B.setInsertionPoint(scalarSuccBB); {
    SILValue succValue = scalarSuccBB->createPhiArgument(
        targetLoweredType, ccb->getForwardingOwnershipKind());

    switch (consumption) {
    // On success, we take with both take_always and take_on_success.
    case CastConsumptionKind::TakeAlways:
    case CastConsumptionKind::TakeOnSuccess:
      break;
    case CastConsumptionKind::CopyOnSuccess: {
      succValue = B.emitCopyValueOperation(loc, succValue);
      B.emitEndBorrowOperation(loc, srcValue);
      break;
    }
    case CastConsumptionKind::BorrowAlways:
      llvm_unreachable("should never see a borrow_always here");
    }

    // And then store the succValue into dest.
    B.emitStoreValueOperation(loc, succValue, destAddr,
                              StoreOwnershipQualifier::Init);
    B.createBranch(loc, indirectSuccBB);
  }

  // Emit the failure block.
  B.setInsertionPoint(scalarFailBB);
  {
    SILValue failValue = srcValue;

    // If we have ownership, we need to create something for the default
    // argument. Otherwise, we just use the input argument to the
    // checked_cast_br.
    if (B.hasOwnership()) {
      failValue = scalarFailBB->createPhiArgument(srcValue->getType(),
                                                  srcValue->getOwnershipKind());
    }

    switch (consumption) {
    case CastConsumptionKind::TakeAlways:
      // We need to destroy the fail value if we have take_always.
      B.emitDestroyValueOperation(loc, failValue);
      break;
    case CastConsumptionKind::TakeOnSuccess:
      // If we have take_on_success, since we failed, just store the value back
      // into the src location that we originally took from.
      B.emitStoreValueOperation(loc, failValue, srcAddr,
                                StoreOwnershipQualifier::Init);
      break;
    case CastConsumptionKind::CopyOnSuccess:
      B.emitEndBorrowOperation(loc, srcValue);
      break;
    case CastConsumptionKind::BorrowAlways:
      llvm_unreachable("borrow_on_success should never appear here");
    }

    B.createBranch(loc, indirectFailBB);
  }
}
