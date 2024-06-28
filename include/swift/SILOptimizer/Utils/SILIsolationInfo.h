//===--- SILIsolationInfo.h -----------------------------------------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2024 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//

#ifndef SWIFT_SILOPTIMIZER_UTILS_SILISOLATIONINFO_H
#define SWIFT_SILOPTIMIZER_UTILS_SILISOLATIONINFO_H

#include "swift/Basic/Defer.h"
#include "swift/Basic/FrozenMultiMap.h"
#include "swift/Basic/ImmutablePointerSet.h"
#include "swift/Basic/LLVM.h"
#include "swift/SIL/SILFunction.h"
#include "swift/SIL/SILInstruction.h"

#include "llvm/ADT/MapVector.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/Support/Debug.h"

#include <algorithm>
#include <variant>

namespace swift {

/// Represents a generalized actor instance reference.
///
/// Used to generalize over cases where we actually have an actor instance
/// associated with a SILValue and other cases where we know that the actor
/// instance is but we don't have a value.
class ActorInstance {
public:
  enum class Kind : uint8_t {
    /// An actor instance where we have an actual SILValue for the actor
    /// instance.
    Value,

    /// An actor instance in an actor accessor init where we do not have direct
    /// access to the self value and instead have access indirectly to the
    /// storage associated with the accessor.
    ActorAccessorInit = 0x1,
  };

  /// Set to (SILValue(), ActorAccessorInit) if we have an ActorAccessorInit. Is
  /// null if we have (SILValue(), Kind::Value).
  llvm::PointerIntPair<SILValue, 1> value;

  ActorInstance(SILValue value, Kind kind)
      : value(value, std::underlying_type<Kind>::type(kind)) {}

  /// We want to look through certain instructions like end_init_ref that have
  /// the appropriate actor type but could disguise the actual underlying value
  /// that we want to represent our actor.
  static SILValue lookThroughInsts(SILValue value);

public:
  ActorInstance() : ActorInstance(SILValue(), Kind::Value) {}

  static ActorInstance getForValue(SILValue value) {
    value = lookThroughInsts(value);
    return ActorInstance(value, Kind::Value);
  }

  static ActorInstance getForActorAccessorInit() {
    return ActorInstance(SILValue(), Kind::ActorAccessorInit);
  }

  explicit operator bool() const { return bool(value.getOpaqueValue()); }

  Kind getKind() const { return Kind(value.getInt()); }

  SILValue getValue() const {
    assert(getKind() == Kind::Value);
    return value.getPointer();
  }

  bool isValue() const { return getKind() == Kind::Value; }

  bool isAccessorInit() const { return getKind() == Kind::ActorAccessorInit; }

  bool operator==(const ActorInstance &other) const {
    // If both are null, return true.
    if (!bool(*this) && !bool(other))
      return true;

    // Otherwise, check if the kinds match.
    if (getKind() != other.getKind())
      return false;

    // Now that we know that the kinds match, perform the kind specific check.
    switch (getKind()) {
    case Kind::Value:
      return getValue() == other.getValue();
    case Kind::ActorAccessorInit:
      return true;
    }
  }

  bool operator!=(const ActorInstance &other) const {
    return !(*this == other);
  }
};

class SILIsolationInfo {
public:
  /// The lattice is:
  ///
  /// Unknown -> Disconnected -> TransferringParameter -> Task -> Actor.
  ///
  /// Unknown means no information. We error when merging on it.
  enum Kind : uint8_t {
    Unknown,
    Disconnected,
    Task,
    Actor,
  };

private:
  /// The actor isolation if this value has one. The default unspecified case
  /// otherwise.
  ActorIsolation actorIsolation;

  /// This is the value that we got isolation from if we were able to find
  /// one. Used for isolation history.
  SILValue isolatedValue;

  /// If set this is the SILValue that represents the actor instance that we
  /// derived isolatedValue from.
  ActorInstance actorInstance;

  unsigned kind : 8;
  unsigned unsafeNonIsolated : 1;

  SILIsolationInfo(SILValue isolatedValue, SILValue actorInstance,
                   ActorIsolation actorIsolation, bool isUnsafeNonIsolated)
      : actorIsolation(actorIsolation), isolatedValue(isolatedValue),
        actorInstance(ActorInstance::getForValue(actorInstance)), kind(Actor),
        unsafeNonIsolated(isUnsafeNonIsolated) {
    assert((!actorInstance ||
            (actorIsolation.getKind() == ActorIsolation::ActorInstance &&
             actorInstance->getType()
                 .getASTType()
                 ->lookThroughAllOptionalTypes()
                 ->getAnyActor())) &&
           "actorInstance must be an actor if it is non-empty");
  }

  SILIsolationInfo(SILValue isolatedValue, ActorInstance actorInstance,
                   ActorIsolation actorIsolation, bool isUnsafeNonIsolated)
      : actorIsolation(actorIsolation), isolatedValue(isolatedValue),
        actorInstance(actorInstance), kind(Actor),
        unsafeNonIsolated(isUnsafeNonIsolated) {
    assert(actorInstance);
    assert(actorIsolation.getKind() == ActorIsolation::ActorInstance);
  }

  SILIsolationInfo(Kind kind, SILValue isolatedValue)
      : actorIsolation(), isolatedValue(isolatedValue), kind(kind),
        unsafeNonIsolated(false) {}

  SILIsolationInfo(Kind kind, bool isUnsafeNonIsolated)
      : actorIsolation(), kind(kind), unsafeNonIsolated(isUnsafeNonIsolated) {}

public:
  SILIsolationInfo()
      : actorIsolation(), kind(Kind::Unknown), unsafeNonIsolated(false) {}

  operator bool() const { return kind != Kind::Unknown; }

  operator Kind() const { return getKind(); }

  Kind getKind() const { return Kind(kind); }

  bool isDisconnected() const { return kind == Kind::Disconnected; }
  bool isActorIsolated() const { return kind == Kind::Actor; }
  bool isTaskIsolated() const { return kind == Kind::Task; }

  bool isUnsafeNonIsolated() const { return unsafeNonIsolated; }

  SILIsolationInfo withUnsafeNonIsolated(bool newValue = true) const {
    assert(*this && "Cannot be unknown");
    auto self = *this;
    self.unsafeNonIsolated = newValue;
    return self;
  }

  void print(llvm::raw_ostream &os) const;

  /// Print a textual representation of the text info that is meant to be
  /// included in other logging output for types that compose with
  /// SILIsolationInfo. As a result, we only print state that can fit on
  /// one line.
  void printForOneLineLogging(llvm::raw_ostream &os) const;

  SWIFT_DEBUG_DUMP {
    print(llvm::dbgs());
    llvm::dbgs() << '\n';
  }

  void printForDiagnostics(llvm::raw_ostream &os) const;

  SWIFT_DEBUG_DUMPER(dumpForDiagnostics()) {
    printForDiagnostics(llvm::dbgs());
    llvm::dbgs() << '\n';
  }

  ActorIsolation getActorIsolation() const {
    assert(kind == Actor);
    return actorIsolation;
  }

  /// If we are actor or task isolated and could find a specific value that
  /// caused the isolation, put it here. Used for isolation history.
  SILValue getIsolatedValue() const {
    assert(kind == Task || kind == Actor);
    return isolatedValue;
  }

  /// Return the specific SILValue for the actor that our isolated value is
  /// isolated to if one exists.
  ActorInstance getActorInstance() const {
    assert(kind == Actor);
    return actorInstance;
  }

  bool hasActorIsolation() const { return kind == Actor; }

  bool hasIsolatedValue() const {
    return (kind == Task || kind == Actor) && bool(isolatedValue);
  }

  static SILIsolationInfo getDisconnected(bool isUnsafeNonIsolated) {
    return {Kind::Disconnected, isUnsafeNonIsolated};
  }

  /// Create an actor isolation for a value that we know is actor isolated to a
  /// specific actor, but we do not know the specific instance yet.
  ///
  /// This can occur when closing over a closure with an isolated parameter or
  /// if we are determining isolation of a function_ref that takes an isolated
  /// parameter. In both cases, we cannot know what the actual isolation is
  /// until we invoke the closure or function.
  ///
  /// TODO: This is just a stub currently until I implement the flow sensitive
  /// part. We just treat all instances the same. There are tests that validate
  /// this behavior.
  static SILIsolationInfo
  getFlowSensitiveActorIsolated(SILValue isolatedValue,
                                ActorIsolation actorIsolation) {
    return {isolatedValue, SILValue(), actorIsolation,
            false /*nonisolated(unsafe)*/};
  }

  /// Only use this as a fallback if we cannot find better information.
  static SILIsolationInfo
  getWithIsolationCrossing(ApplyIsolationCrossing crossing) {
    if (crossing.getCalleeIsolation().isActorIsolated()) {
      // SIL level, just let it through
      return SILIsolationInfo(SILValue(), SILValue(),
                              crossing.getCalleeIsolation(),
                              false /*nonisolated(unsafe)*/);
    }

    return {};
  }

  static SILIsolationInfo getActorInstanceIsolated(SILValue isolatedValue,
                                                   SILValue actorInstance,
                                                   NominalTypeDecl *typeDecl) {
    assert(actorInstance);
    if (!typeDecl->isAnyActor()) {
      assert(!swift::getActorIsolation(typeDecl).isGlobalActor() &&
             "Should have called getGlobalActorIsolated");
      return {};
    }
    return {isolatedValue, actorInstance,
            ActorIsolation::forActorInstanceSelf(typeDecl),
            false /*nonisolated(unsafe)*/};
  }

  static SILIsolationInfo getActorInstanceIsolated(SILValue isolatedValue,
                                                   ActorInstance actorInstance,
                                                   NominalTypeDecl *typeDecl) {
    assert(actorInstance);
    if (!typeDecl->isAnyActor()) {
      assert(!swift::getActorIsolation(typeDecl).isGlobalActor() &&
             "Should have called getGlobalActorIsolated");
      return {};
    }
    return {isolatedValue, actorInstance,
            ActorIsolation::forActorInstanceSelf(typeDecl),
            false /*nonisolated(unsafe)*/};
  }

  /// A special actor instance isolated for partial apply cases where we do not
  /// close over the isolated parameter and thus do not know the actual actor
  /// instance that we are going to use.
  static SILIsolationInfo
  getPartialApplyActorInstanceIsolated(SILValue isolatedValue,
                                       NominalTypeDecl *typeDecl) {
    if (!typeDecl->isAnyActor()) {
      assert(!swift::getActorIsolation(typeDecl).isGlobalActor() &&
             "Should have called getGlobalActorIsolated");
      return {};
    }
    return {isolatedValue, SILValue(),
            ActorIsolation::forActorInstanceSelf(typeDecl),
            false /*nonisolated(unsafe)*/};
  }

  static SILIsolationInfo getGlobalActorIsolated(SILValue value,
                                                 Type globalActorType) {
    return {value, SILValue() /*no actor instance*/,
            ActorIsolation::forGlobalActor(globalActorType),
            false /*nonisolated(unsafe)*/};
  }

  static SILIsolationInfo getGlobalActorIsolated(SILValue value,
                                                 ValueDecl *decl) {
    auto isolation = swift::getActorIsolation(decl);
    if (!isolation.isGlobalActor())
      return {};
    return SILIsolationInfo::getGlobalActorIsolated(value,
                                                    isolation.getGlobalActor());
  }

  static SILIsolationInfo getTaskIsolated(SILValue value) {
    return {Kind::Task, value};
  }

  /// Attempt to infer the isolation region info for \p inst.
  static SILIsolationInfo get(SILInstruction *inst);

  /// Attempt to infer the isolation region info for \p arg.
  static SILIsolationInfo get(SILArgument *arg);

  static SILIsolationInfo get(SILValue value) {
    if (auto *arg = dyn_cast<SILArgument>(value))
      return get(arg);
    if (auto *inst = dyn_cast<SingleValueInstruction>(value))
      return get(inst);
    return {};
  }

  /// A helper that is used to ensure that we treat certain builtin values as
  /// non-Sendable that the AST level otherwise thinks are non-Sendable.
  ///
  /// E.x.: Builtin.RawPointer and Builtin.NativeObject
  ///
  /// TODO: Fix the type checker.
  static bool isNonSendableType(SILType type, SILFunction *fn);

  static bool isNonSendableType(SILValue value) {
    return isNonSendableType(value->getType(), value->getFunction());
  }

  bool hasSameIsolation(ActorIsolation actorIsolation) const;

  /// Returns true if \p this and \p other have the same isolation. It allows
  /// for the isolated values if any to not match.
  ///
  /// This is useful if one has two non-Sendable values projected from the same
  /// actor or global actor isolated value. E.x.: two different ref_element_addr
  /// from the same actor.
  bool hasSameIsolation(const SILIsolationInfo &other) const;

  /// Returns true if this SILIsolationInfo is deeply equal to other. This means
  /// that the isolation and the isolated value match.
  bool isEqual(const SILIsolationInfo &other) const;

  void Profile(llvm::FoldingSetNodeID &id) const;
};

/// A SILIsolationInfo that has gone through merging and represents the dynamic
/// isolation info of a value found by merging its isolation at a region
/// point. This means that nonisolated(unsafe) has been removed. It is used so
/// that in the type system we can distinguish in between isolation info that is
/// static isolation info associated with a value and dynamic isolation info
/// that can just be used for dataflow.
class SILDynamicMergedIsolationInfo {
  SILIsolationInfo innerInfo;

public:
  SILDynamicMergedIsolationInfo() : innerInfo() {}
  SILDynamicMergedIsolationInfo(SILIsolationInfo innerInfo)
      : innerInfo(innerInfo) {}

  /// Returns nullptr only if both this isolation info and \p other are actor
  /// isolated to incompatible actors.
  [[nodiscard]] std::optional<SILDynamicMergedIsolationInfo>
  merge(SILIsolationInfo other) const;

  [[nodiscard]] std::optional<SILDynamicMergedIsolationInfo>
  merge(SILDynamicMergedIsolationInfo other) const {
    return merge(other.getIsolationInfo());
  }

  operator bool() const { return bool(innerInfo); }

  SILIsolationInfo getIsolationInfo() const { return innerInfo; }

  bool isDisconnected() const { return innerInfo.isDisconnected(); }

  bool hasSameIsolation(SILIsolationInfo other) const {
    return innerInfo.hasSameIsolation(other);
  }

  SWIFT_DEBUG_DUMP { innerInfo.dump(); }

  void printForDiagnostics(llvm::raw_ostream &os) const {
    innerInfo.printForDiagnostics(os);
  }

  SWIFT_DEBUG_DUMPER(dumpForDiagnostics()) {
    innerInfo.dumpForDiagnostics();
  }
};

} // namespace swift

#endif
