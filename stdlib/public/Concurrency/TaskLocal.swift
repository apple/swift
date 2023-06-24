//===----------------------------------------------------------------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2020-2021 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//

import Swift
@_implementationOnly import _SwiftConcurrencyShims

/// Property wrapper that defines a task-local value key.
///
/// A task-local value is a value that can be bound and read in the context of a
/// `Task`. It is implicitly carried with the task, and is accessible by any
/// child tasks the task creates (such as TaskGroup or `async let` created tasks).
///
/// ### Task-local declarations
///
/// Task locals must be declared as static properties (or global properties,
/// once property wrappers support these), like this:
///
///     enum TracingExample {
///         @TaskLocal
///         static let traceID: TraceID?
///     }
///
/// ### Default values
/// Task local values of optional types default to `nil`. It is possible to define
/// not-optional task-local values, and an explicit default value must then be
/// defined instead.
///
/// The default value is returned whenever the task-local is read
/// from a context which either: has no task available to read the value from
/// (e.g. a synchronous function, called without any asynchronous function in its call stack),
/// or no value was bound within the scope of the current task or any of its parent tasks.
///
/// ### Reading task-local values
/// Reading task local values is simple and looks the same as-if reading a normal
/// static property:
///
///     guard let traceID = TracingExample.traceID else {
///       print("no trace id")
///       return
///     }
///     print(traceID)
///
/// It is possible to perform task-local value reads from either asynchronous
/// or synchronous functions. Within asynchronous functions, as a "current" task
/// is always guaranteed to exist, this will perform the lookup in the task local context.
///
/// A lookup made from the context of a synchronous function, that is not called
/// from an asynchronous function (!), will immediately return the task-local's
/// default value.
///
/// ### Binding task-local values
/// Task local values cannot be `set` directly and must instead be bound using
/// the scoped `$traceID.withValue() { ... }` operation. The value is only bound
/// for the duration of that scope, and is available to any child tasks which
/// are created within that scope.
///
/// Detached tasks do not inherit task-local values, however tasks created using
/// the `Task { ... }` initializer do inherit task-locals by copying them to the
/// new asynchronous task, even though it is an un-structured task.
///
/// ### Examples
///
///     @TaskLocal
///     static var traceID: TraceID?
///
///     print("traceID: \(traceID)") // traceID: nil
///
///     $traceID.withValue(1234) { // bind the value
///       print("traceID: \(traceID)") // traceID: 1234
///       call() // traceID: 1234
///
///       Task { // unstructured tasks do inherit task locals by copying
///         call() // traceID: 1234
///       }
///
///       Task.detached { // detached tasks do not inherit task-local values
///         call() // traceID: nil
///       }
///     }
///
///     func call() {
///       print("traceID: \(traceID)") // 1234
///     }
///
/// This type must be a `class` so it has a stable identity, that is used as key
/// value for lookups in the task local storage.
@propertyWrapper
@available(SwiftStdlib 5.1, *)
public final class TaskLocal<Value: Sendable>: Sendable, CustomStringConvertible {
  let defaultValue: Value

  public init(wrappedValue defaultValue: Value) {
    self.defaultValue = defaultValue
  }

  @_alwaysEmitIntoClient
  var key: Builtin.RawPointer {
    unsafeBitCast(self, to: Builtin.RawPointer.self)
  }

  /// Gets the value currently bound to this task-local from the current task.
  ///
  /// If no current task is available in the context where this call is made,
  /// or if the task-local has no value bound, this will return the `defaultValue`
  /// of the task local.
  public func get() -> Value {
    guard let rawValue = _taskLocalValueGet(key: key) else {
      return self.defaultValue
    }

    // Take the value; The type should be correct by construction
    let storagePtr =
        rawValue.bindMemory(to: Value.self, capacity: 1)
    return UnsafeMutablePointer<Value>(mutating: storagePtr).pointee
  }

  /// Binds the task-local to the specific value for the duration of the asynchronous operation.
  ///
  /// The value is available throughout the execution of the operation closure,
  /// including any `get` operations performed by child-tasks created during the
  /// execution of the operation closure.
  ///
  /// If the same task-local is bound multiple times, be it in the same task, or
  /// in specific child tasks, the more specific (i.e. "deeper") binding is
  /// returned when the value is read.
  ///
  /// If the value is a reference type, it will be retained for the duration of
  /// the operation closure.
  @inlinable
  @discardableResult
  @_unsafeInheritExecutor
  @backDeployed(before: SwiftStdlib 5.8)
  public func withValue<R>(_ valueDuringOperation: Value, operation: () async throws -> R,
                           file: String = #fileID, line: UInt = #line) async rethrows -> R {
    return try await withValueImpl(valueDuringOperation, operation: operation, file: file, line: line)
  }

  /// Implementation for withValue that consumes valueDuringOperation.
  ///
  /// Because _taskLocalValuePush and _taskLocalValuePop involve calls to
  /// swift_task_alloc/swift_task_dealloc respectively unbeknownst to the
  /// compiler, compiler-emitted calls to swift_task_de/alloc must be avoided
  /// in a function that calls them.
  ///
  /// A copy of valueDuringOperation is required because withValue borrows its
  /// argument but _taskLocalValuePush consumes its.  Because
  /// valueDuringOperation is of generic type, its size is not generally known,
  /// so such a copy entails a stack allocation and a copy to that allocation.
  /// That stack traffic gets lowered to calls to
  /// swift_task_alloc/swift_task_deallloc.
  ///
  /// Split the calls _taskLocalValuePush/Pop from the compiler-emitted calls
  /// to swift_task_de/alloc for the copy as follows:
  /// - withValue contains the compiler-emitted calls swift_task_de/alloc.
  /// - withValueImpl contains the calls to _taskLocalValuePush/Pop
  @inlinable
  @discardableResult
  @_unsafeInheritExecutor
  @backDeployed(before: SwiftStdlib 5.9)
  internal func withValueImpl<R>(_ valueDuringOperation: __owned Value, operation: () async throws -> R,
                                 file: String = #fileID, line: UInt = #line) async rethrows -> R {
    // check if we're not trying to bind a value from an illegal context; this may crash
    _checkIllegalTaskLocalBindingWithinWithTaskGroup(file: file, line: line)

    _taskLocalValuePush(key: key, value: valueDuringOperation)
    defer { _taskLocalValuePop() }

    return try await operation()
  }

  /// Binds the task-local to the specific value for the duration of the
  /// synchronous operation.
  ///
  /// The value is available throughout the execution of the operation closure,
  /// including any `get` operations performed by child-tasks created during the
  /// execution of the operation closure.
  ///
  /// If the same task-local is bound multiple times, be it in the same task, or
  /// in specific child tasks, the "more specific" binding is returned when the
  /// value is read.
  ///
  /// If the value is a reference type, it will be retained for the duration of
  /// the operation closure.
  @inlinable
  @discardableResult
  public func withValue<R>(_ valueDuringOperation: Value, operation: () throws -> R,
                           file: String = #fileID, line: UInt = #line) rethrows -> R {
    // check if we're not trying to bind a value from an illegal context; this may crash
    _checkIllegalTaskLocalBindingWithinWithTaskGroup(file: file, line: line)

    _taskLocalValuePush(key: key, value: valueDuringOperation)
    defer { _taskLocalValuePop() }

    return try operation()
  }

  public var projectedValue: TaskLocal<Value> {
    get {
      self
    }

    @available(*, unavailable, message: "use '$myTaskLocal.withValue(_:do:)' instead")
    set {
      fatalError("Illegal attempt to set a \(Self.self) value, use `withValue(...) { ... }` instead.")
    }
  }

  // This subscript is used to enforce that the property wrapper may only be used
  // on static (or rather, "without enclosing instance") properties.
  // This is done by marking the `_enclosingInstance` as `Never` which informs
  // the type-checker that this property-wrapper never wants to have an enclosing
  // instance (it is impossible to declare a property wrapper inside the `Never`
  // type).
  @available(*, unavailable, message: "property wrappers cannot be instance members")
  public static subscript(
    _enclosingInstance object: Never,
    wrapped wrappedKeyPath: ReferenceWritableKeyPath<Never, Value>,
    storage storageKeyPath: ReferenceWritableKeyPath<Never, TaskLocal<Value>>
  ) -> Value {
    get {
      fatalError("Will never be executed, since enclosing instance is Never")
    }
  }

  public var wrappedValue: Value {
    self.get()
  }

  public var description: String {
    "\(Self.self)(defaultValue: \(self.defaultValue))"
  }

}

// ==== ------------------------------------------------------------------------

@available(SwiftStdlib 5.1, *)
@usableFromInline
@_silgen_name("swift_task_localValuePush")
func _taskLocalValuePush<Value>(
  key: Builtin.RawPointer/*: Key*/,
  value: __owned Value
) // where Key: TaskLocal

@available(SwiftStdlib 5.1, *)
@usableFromInline
@_silgen_name("swift_task_localValuePop")
func _taskLocalValuePop()

@available(SwiftStdlib 5.1, *)
@_silgen_name("swift_task_localValueGet")
func _taskLocalValueGet(
  key: Builtin.RawPointer/*Key*/
) -> UnsafeMutableRawPointer? // where Key: TaskLocal

@available(SwiftStdlib 5.1, *)
@_silgen_name("swift_task_localsCopyTo")
func _taskLocalsCopy(
  to target: Builtin.NativeObject
)

// ==== Checks -----------------------------------------------------------------

@available(SwiftStdlib 5.1, *)
@usableFromInline
func _checkIllegalTaskLocalBindingWithinWithTaskGroup(file: String, line: UInt) {
  if _taskHasTaskGroupStatusRecord() {
    file.withCString { _fileStart in
      _reportIllegalTaskLocalBindingWithinWithTaskGroup(
          _fileStart, file.count, true, line)
    }
  }
}

@available(SwiftStdlib 5.1, *)
@usableFromInline
@_silgen_name("swift_task_reportIllegalTaskLocalBindingWithinWithTaskGroup")
func _reportIllegalTaskLocalBindingWithinWithTaskGroup(
  _ _filenameStart: UnsafePointer<Int8>,
  _ _filenameLength: Int,
  _ _filenameIsASCII: Bool,
  _ _line: UInt)
