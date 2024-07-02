//===--- Executor.h - ABI structures for executors --------------*- C++ -*-===//
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
// Swift ABI describing executors.
//
//===----------------------------------------------------------------------===//

#ifndef SWIFT_ABI_EXECUTOR_H
#define SWIFT_ABI_EXECUTOR_H

#include <inttypes.h>
#include "swift/ABI/Actor.h"
#include "swift/ABI/HeapObject.h"
#include "swift/Runtime/Casting.h"

namespace swift {
class AsyncContext;
class AsyncTask;
class DefaultActor;
class Job;
class SerialExecutorWitnessTable;
class TaskExecutorWitnessTable;

/// An unmanaged reference to a serial executor.
///
/// This type corresponds to the type Optional<Builtin.Executor> in
/// Swift.  The representation of nil in Optional<Builtin.Executor>
/// aligns with what this type calls the generic executor, so the
/// notional subtype of this type which is never generic corresponds
/// to the type Builtin.Executor.
///
/// An executor reference is divided into two pieces:
///
/// - The identity, which is just a (potentially ObjC) object
///   reference; when this is null, the reference is generic.
///   Equality of executor references is based solely on equality
///   of identity.
///
/// - The implementation, which is an optional reference to a
///   witness table for the SerialExecutor protocol.  When this
///   is null, but the identity is non-null, the reference is to
///   a default actor.  The low bits of the implementation pointer
///   are reserved for the use of marking interesting properties
///   about the executor's implementation.  The runtime masks these
///   bits off before accessing the witness table, so setting them
///   in the future should back-deploy as long as the witness table
///   reference is still present.
class SerialExecutorRef {
  HeapObject *Identity; // Not necessarily Swift reference-countable
  uintptr_t Implementation;

  // We future-proof the ABI here by masking the low bits off the
  // implementation pointer before using it as a witness table.
  //
  // We have 3 bits for future use remaining here.
  enum: uintptr_t {
    WitnessTableMask = ~uintptr_t(alignof(void*) - 1)
  };

  /// The kind is stored in the free bits in the `Implementation` witness table reference.
  enum class ExecutorKind : uintptr_t {
    /// Ordinary executor.
    ///
    /// Note that the "Generic" executor is also implicitly "Ordinary".
    /// To check if an executor is Generic, explicitly check this by calling `isGeneric`.
    Ordinary = 0b00,
    /// Executor that may need to participate in complex "same context" checks,
    /// by invoking `isSameExclusiveExecutionContext` when comparing execution contexts.
    ComplexEquality = 0b01,
  };

  static_assert(static_cast<uintptr_t>(ExecutorKind::Ordinary) == 0);

  constexpr SerialExecutorRef(HeapObject *identity, uintptr_t implementation)
    : Identity(identity), Implementation(implementation) {}

public:

  /// A generic execution environment.  When running in a generic
  /// environment, it's presumed to be okay to switch synchronously
  /// to an actor.  As an executor request, this represents a request
  /// to drop whatever the current actor is.
  constexpr static SerialExecutorRef generic() {
    return SerialExecutorRef(nullptr, 0);
  }

  /// Given a pointer to a default actor, return an executor reference
  /// for it.
  static SerialExecutorRef forDefaultActor(DefaultActor *actor) {
    assert(actor);
    return SerialExecutorRef(actor, 0);
  }

  /// Given a pointer to a serial executor and its SerialExecutor
  /// conformance, return an executor reference for it.
  static SerialExecutorRef forOrdinary(HeapObject *identity,
                           const SerialExecutorWitnessTable *witnessTable) {
    assert(identity);
    assert(witnessTable);
    auto wtable = reinterpret_cast<uintptr_t>(witnessTable) |
        static_cast<uintptr_t>(ExecutorKind::Ordinary);
    return SerialExecutorRef(identity, wtable);
  }

  static SerialExecutorRef forComplexEquality(HeapObject *identity,
                                        const SerialExecutorWitnessTable *witnessTable) {
    assert(identity);
    assert(witnessTable);
    auto wtable = reinterpret_cast<uintptr_t>(witnessTable) |
                  static_cast<uintptr_t>(ExecutorKind::ComplexEquality);
    return SerialExecutorRef(identity, wtable);
  }

  HeapObject *getIdentity() const {
    return Identity;
  }

  const char* getIdentityDebugName() const {
    return isMainExecutor() ? " (MainActorExecutor)"
           : isGeneric()    ? " (GenericExecutor)"
                            : "";
  }

  /// Is this the generic executor reference?
  bool isGeneric() const {
    return Identity == 0;
  }

  ExecutorKind getExecutorKind() const {
    return static_cast<ExecutorKind>(Implementation & ~WitnessTableMask);
  }

  /// Is this an ordinary executor reference?
  /// These executor references are the default kind, and have no special treatment elsewhere in the system.
  bool isOrdinary() const {
    return getExecutorKind() == ExecutorKind::Ordinary;
  }

  /// Is this an `complex-equality` executor reference?
  /// These executor references should implement `isSameExclusiveExecutionContext` which will be invoked
  /// when two executors are compared for being the same exclusive execution context.
  bool isComplexEquality() const {
    return getExecutorKind() == ExecutorKind::ComplexEquality;
  }

  /// Is this a default-actor executor reference?
  bool isDefaultActor() const {
    return !isGeneric() && Implementation == 0;
  }
  DefaultActor *getDefaultActor() const {
    assert(isDefaultActor());
    return reinterpret_cast<DefaultActor*>(Identity);
  }

  bool hasSerialExecutorWitnessTable() const {
    return !isGeneric() && !isDefaultActor();
  }

  const SerialExecutorWitnessTable *getSerialExecutorWitnessTable() const {
    assert(hasSerialExecutorWitnessTable());
    auto table = Implementation & WitnessTableMask;
    return reinterpret_cast<const SerialExecutorWitnessTable*>(table);
  }

  /// Do we have to do any work to start running as the requested
  /// executor?
  bool mustSwitchToRun(SerialExecutorRef newExecutor) const {
    return Identity != newExecutor.Identity;
  }

  /// Is this executor the main executor?
  bool isMainExecutor() const;

  /// Get the raw value of the Implementation field, for tracing.
  uintptr_t getRawImplementation() const {
    return Implementation & WitnessTableMask;
  }

  bool operator==(SerialExecutorRef other) const {
    return Identity == other.Identity;
  }
  bool operator!=(SerialExecutorRef other) const {
    return !(*this == other);
  }
};

class TaskExecutorRef {
  HeapObject *Identity; // Not necessarily Swift reference-countable
  uintptr_t Implementation;

  // We future-proof the ABI here by masking the low bits off the
  // implementation pointer before using it as a witness table.
  //
  // We have 3 bits for future use remaining here.
  enum: uintptr_t {
    WitnessTableMask = ~uintptr_t(alignof(void*) - 1)
  };

  /// The kind is stored in the free bits in the `Implementation` witness table reference.
  enum class TaskExecutorKind : uintptr_t {
    /// Ordinary executor.
    Ordinary = 0b00,
  };

  static_assert(static_cast<uintptr_t>(TaskExecutorKind::Ordinary) == 0);

  constexpr TaskExecutorRef(HeapObject *identity, uintptr_t implementation)
    : Identity(identity), Implementation(implementation) {}

public:

  // Only public for CompatibilityOverrideConcurrency stubs.
  // Prefer `TaskExecutorRef::undefined` instead.
  explicit TaskExecutorRef() : Identity(nullptr), Implementation(0) {
    assert(isUndefined());
  }

  constexpr static TaskExecutorRef undefined() {
    return TaskExecutorRef(nullptr, 0);
  }

  /// Given a pointer to a serial executor and its TaskExecutor
  /// conformance, return an executor reference for it.
  static TaskExecutorRef forOrdinary(HeapObject *identity,
                           const SerialExecutorWitnessTable *witnessTable) {
    assert(identity);
    assert(witnessTable);
    auto wtable = reinterpret_cast<uintptr_t>(witnessTable) |
        static_cast<uintptr_t>(TaskExecutorKind::Ordinary);
    return TaskExecutorRef(identity, wtable);
  }

  /// If the job is an 'AsyncTask' return its task executor preference,
  /// otherwise return 'undefined', meaning "no preference".
  static TaskExecutorRef fromTaskExecutorPreference(Job *job);

  HeapObject *getIdentity() const {
    return Identity;
  }

  /// Is this the generic executor reference?
  bool isUndefined() const {
    return Identity == 0;
  }

  bool isDefined() const { return !isUndefined(); }

  TaskExecutorKind getExecutorKind() const {
    return static_cast<TaskExecutorKind>(Implementation & ~WitnessTableMask);
  }

  /// Is this an ordinary executor reference?
  /// These executor references are the default kind, and have no special treatment elsewhere in the system.
  bool isOrdinary() const {
    return getExecutorKind() == TaskExecutorKind::Ordinary;
  }

  const TaskExecutorWitnessTable *getTaskExecutorWitnessTable() const {
    assert(!isUndefined());
    auto table = Implementation & WitnessTableMask;
    return reinterpret_cast<const TaskExecutorWitnessTable*>(table);
  }

  /// Get the raw value of the Implementation field, for tracing.
  uintptr_t getRawImplementation() const {
    return Implementation & WitnessTableMask;
  }

  bool operator==(TaskExecutorRef other) const {
    return Identity == other.Identity;
  }
  bool operator!=(TaskExecutorRef other) const {
    return !(*this == other);
  }
};

using JobInvokeFunction =
  SWIFT_CC(swiftasync)
  void (Job *);

using TaskContinuationFunction =
  SWIFT_CC(swiftasync)
  void (SWIFT_ASYNC_CONTEXT AsyncContext *);

using ThrowingTaskFutureWaitContinuationFunction =
  SWIFT_CC(swiftasync)
  void (SWIFT_ASYNC_CONTEXT AsyncContext *, SWIFT_CONTEXT void *);


template <class AsyncSignature>
class AsyncFunctionPointer;
template <class AsyncSignature>
struct AsyncFunctionTypeImpl;
template <class AsyncSignature>
struct AsyncContinuationTypeImpl;

/// The abstract signature for an asynchronous function.
template <class Sig, bool HasErrorResult>
struct AsyncSignature;

template <class DirectResultTy, class... ArgTys, bool HasErrorResult>
struct AsyncSignature<DirectResultTy(ArgTys...), HasErrorResult> {
  bool hasDirectResult = !std::is_same<DirectResultTy, void>::value;
  using DirectResultType = DirectResultTy;

  bool hasErrorResult = HasErrorResult;

  using FunctionPointer = AsyncFunctionPointer<AsyncSignature>;
  using FunctionType = typename AsyncFunctionTypeImpl<AsyncSignature>::type;
  using ContinuationType = typename AsyncContinuationTypeImpl<AsyncSignature>::type;
};

/// A signature for a thin async function that takes no arguments
/// and returns no results.
using ThinNullaryAsyncSignature =
  AsyncSignature<void(), false>;

/// A signature for a thick async function that takes no formal
/// arguments and returns no results.
using ThickNullaryAsyncSignature =
  AsyncSignature<void(HeapObject*), false>;

template <class Signature>
struct AsyncFunctionTypeImpl;

template <class DirectResultTy, class... ArgTys, bool HasErrorResult>
struct AsyncFunctionTypeImpl<
    AsyncSignature<DirectResultTy(ArgTys...), HasErrorResult>> {

  using type = SWIFT_CC(swiftasync) void(SWIFT_ASYNC_CONTEXT AsyncContext *,
                                         ArgTys...);
};

template <class Signature>
struct AsyncContinuationTypeImpl;

template <class DirectResultTy, class... ArgTys>
struct AsyncContinuationTypeImpl<
  AsyncSignature<DirectResultTy(ArgTys...), /*throws=*/true>> {

  using type = SWIFT_CC(swiftasync) void(SWIFT_ASYNC_CONTEXT AsyncContext *,
                                         DirectResultTy,
                                         SWIFT_CONTEXT void *);
};

template <class DirectResultTy, class... ArgTys>
struct AsyncContinuationTypeImpl<
  AsyncSignature<DirectResultTy(ArgTys...), /*throws=*/false>> {

  using type = SWIFT_CC(swiftasync) void(SWIFT_ASYNC_CONTEXT AsyncContext *,
                                         DirectResultTy);
};

template <class... ArgTys>
struct AsyncContinuationTypeImpl<
  AsyncSignature<void(ArgTys...), /*throws=*/true>> {

  using type = SWIFT_CC(swiftasync) void(SWIFT_ASYNC_CONTEXT AsyncContext *,
                                         SWIFT_CONTEXT SwiftError *);
};

template <class... ArgTys>
struct AsyncContinuationTypeImpl<
  AsyncSignature<void(ArgTys...), /*throws=*/false>> {

  using type = SWIFT_CC(swiftasync) void(SWIFT_ASYNC_CONTEXT AsyncContext *);
};

template <class Fn>
using AsyncFunctionType = typename AsyncFunctionTypeImpl<Fn>::type;

template <class Fn>
using AsyncContinuationType = typename AsyncContinuationTypeImpl<Fn>::type;

/// A "function pointer" for an async function.
///
/// Eventually, this will always be signed with the data key
/// using a type-specific discriminator.
template <class AsyncSignature>
class AsyncFunctionPointer {
public:
  /// The function to run.
  TargetCompactFunctionPointer<InProcess, AsyncFunctionType<AsyncSignature>,
                        /*nullable*/ false,
                        int32_t> Function;

  /// The expected size of the context.
  uint32_t ExpectedContextSize;
};

}

#endif
