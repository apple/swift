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


// Macros are disabled when Swift is built without swift-syntax.
#if $Macros && hasAttribute(attached)

/// Macro that introduces a ``TaskLocal-class`` binding.
///
/// For information about task-local bindings, see ``TaskLocal-class``.
///
/// - SeeAlso: ``TaskLocal-class``
@available(SwiftStdlib 5.1, *)
@attached(accessor)
@attached(peer, names: prefixed(`$`))
public macro TaskLocal() =
  #externalMacro(module: "SwiftMacros", type: "TaskLocalMacro")

#endif

/// Wrapper type that defines a task-local value key.
///
/// A task-local value is a value that can be bound and read in the context of a
/// ``Task``. It is implicitly carried with the task, and is accessible by any
/// child tasks it creates (such as TaskGroup or `async let` created tasks).
///
/// ### Task-local declarations
///
/// Task locals must be declared as static properties or global properties, like this:
///
///     enum Example {
///         @TaskLocal
///         static var traceID: TraceID?
///     }
///
///     // Global task local properties are supported since Swift 6.0:
///     @TaskLocal
///     var contextualNumber: Int = 12
///
/// ### Default values
/// Reading a task local value when no value was bound to it results in returning
/// its default value. For a task local declared as optional (such as e.g. `TraceID?`),
/// this defaults to nil, however a different default value may be defined at declaration
/// site of the task local, like this:
///
///     enum Example { 
///         @TaskLocal
///         static var traceID: TraceID = TraceID.default
///     }
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
///     guard let traceID = Example.traceID else {
///       print("no trace id")
///       return
///     }
///     print(traceID)
///
/// It is possible to perform task-local value reads from either asynchronous
/// or synchronous functions. 
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
/// ### Using task local values outside of tasks
/// It is possible to bind and read task local values outside of tasks.
///
/// This comes in handy within synchronous functions which are not guaranteed 
/// to be called from within a task. When binding a task-local value from 
/// outside of a task, the runtime will set a thread-local in which the same 
/// storage mechanism as used within tasks will be used. This means that you 
/// can reliably bind and read task local values without having to worry
/// about the specific calling context, e.g.:
///
///     func enter() {
///         Example.$traceID.withValue("1234") {
///             read() // always "1234", regardless if enter() was called from inside a task or not:
///     }
///    
///     func read() -> String {
///         if let value = Self.traceID {
///             "\(value)" 
///         } else { 
///             "<no value>"
///         }
///     }
///
///     // 1) Call `enter` from non-Task code
///     //    e.g. synchronous main() or non-Task thread (e.g. a plain pthread)
///     enter()
///
///     // 2) Call 'enter' from Task
///     Task { 
///         enter()
///     }
///
/// In either cases listed above, the binding and reading of the task-local value works as expected.
///
/// ### Examples
///
///
///     enum Example {
///         @TaskLocal
///         static var traceID: TraceID?
///     }
///     
///     func read() -> String {
///         if let value = Self.traceID {
///             "\(value)" 
///         } else { 
///             "<no value>"
///         }
///     }
///
///     await Example.$traceID.withValue(1234) { // bind the value
///       print("traceID: \(Example.traceID)") // traceID: 1234
///       read() // traceID: 1234
///
///       async let id = read() // async let child task, traceID: 1234
///
///       await withTaskGroup(of: String.self) { group in 
///           group.addTask { read() } // task group child task, traceID: 1234
///           return await group.next()!
///       }
///
///       Task { // unstructured tasks do inherit task locals by copying
///         read() // traceID: 1234
///       }
///
///       Task.detached { // detached tasks do not inherit task-local values
///         read() // traceID: nil
///       }
///     }
///
/// - SeeAlso: ``TaskLocal-macro``
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
  @available(SwiftStdlib 5.1, *)
  @backDeployed(before: SwiftStdlib 6.0)
  public func withValue<R>(_ valueDuringOperation: Value,
                           operation: () async throws -> R,
                           isolation: isolated (any Actor)? = #isolation,
                           file: String = #fileID, line: UInt = #line) async rethrows -> R {
    return try await withValueImpl(
      valueDuringOperation,
      operation: operation,
      isolation: isolation,
      file: file, line: line)
  }

  @usableFromInline
  @discardableResult
  @_unsafeInheritExecutor // ABI compatibility with Swift 5.1
  @available(SwiftStdlib 5.1, *)
  @_silgen_name("$ss9TaskLocalC9withValue_9operation4file4lineqd__x_qd__yYaKXESSSutYaKlF")
  internal func __abi_withValue<R>(_ valueDuringOperation: Value,
                                   operation: () async throws -> R,
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
  @available(SwiftStdlib 5.1, *)
  @backDeployed(before: SwiftStdlib 6.0)
  internal func withValueImpl<R>(_ valueDuringOperation: __owned Value,
                                 operation: () async throws -> R,
                                 isolation: isolated (any Actor)?,
                                 file: String = #fileID, line: UInt = #line) async rethrows -> R {
    _taskLocalValuePush(key: key, value: consume valueDuringOperation)
    defer { _taskLocalValuePop() }

    return try await operation()
  }

  @inlinable
  @discardableResult
  @_unsafeInheritExecutor // internal for backwards compatibility; though may be able to be removed safely?
  @available(SwiftStdlib 5.1, *)
  @backDeployed(before: SwiftStdlib 5.9)
  internal func withValueImpl<R>(_ valueDuringOperation: __owned Value,
                                 operation: () async throws -> R,
                                 file: String = #fileID, line: UInt = #line) async rethrows -> R {
    _taskLocalValuePush(key: key, value: consume valueDuringOperation)
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

@usableFromInline
@available(SwiftStdlib 5.1, *)
@available(*, deprecated, message: "The situation diagnosed by this is not handled gracefully rather than by crashing")
func _checkIllegalTaskLocalBindingWithinWithTaskGroup(file: String, line: UInt) {
  if _taskHasTaskGroupStatusRecord() {
    file.withCString { _fileStart in
      _reportIllegalTaskLocalBindingWithinWithTaskGroup(
          _fileStart, file.count, true, line)
    }
  }
}

@usableFromInline
@available(SwiftStdlib 5.1, *)
@available(*, deprecated, message: "The situation diagnosed by this is not handled gracefully rather than by crashing")
@_silgen_name("swift_task_reportIllegalTaskLocalBindingWithinWithTaskGroup")
func _reportIllegalTaskLocalBindingWithinWithTaskGroup(
  _ _filenameStart: UnsafePointer<Int8>,
  _ _filenameLength: Int,
  _ _filenameIsASCII: Bool,
  _ _line: UInt)
