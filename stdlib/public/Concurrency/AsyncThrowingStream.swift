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

#if !SWIFT_STDLIB_TASK_TO_THREAD_MODEL_CONCURRENCY
/// An asynchronous sequence generated from an error-throwing closure that
/// calls a continuation to produce new elements.
///
/// `AsyncThrowingStream` conforms to `AsyncSequence`, providing a convenient
/// way to create an asynchronous sequence without manually implementing an
/// asynchronous iterator. In particular, an asynchronous stream is well-suited
/// to adapt callback- or delegation-based APIs to participate with
/// `async`-`await`.
///
/// In contrast to `AsyncStream`, this type can throw an error from the awaited
/// `next()`, which terminates the stream with the thrown error.
///
/// You initialize an `AsyncThrowingStream` with a closure that receives an
/// `AsyncThrowingStream.Continuation`. Produce elements in this closure, then
/// provide them to the stream by calling the continuation's `yield(_:)` method.
/// When there are no further elements to produce, call the continuation's
/// `finish()` method. This causes the sequence iterator to produce a `nil`,
/// which terminates the sequence. If an error occurs, call the continuation's
/// `finish(throwing:)` method, which causes the iterator's `next()` method to
/// throw the error to the awaiting call point. The continuation is `Sendable`,
/// which permits calling it from concurrent contexts external to the iteration
/// of the `AsyncThrowingStream`.
///
/// An arbitrary source of elements can produce elements faster than they are
/// consumed by a caller iterating over them. Because of this, `AsyncThrowingStream`
/// defines a buffering behavior, allowing the stream to buffer a specific
/// number of oldest or newest elements. By default, the buffer limit is
/// `Int.max`, which means it's unbounded.
///
/// ### Adapting Existing Code to Use Streams
///
/// To adapt existing callback code to use `async`-`await`, use the callbacks
/// to provide values to the stream, by using the continuation's `yield(_:)`
/// method.
///
/// Consider a hypothetical `QuakeMonitor` type that provides callers with
/// `Quake` instances every time it detects an earthquake. To receive callbacks,
/// callers set a custom closure as the value of the monitor's
/// `quakeHandler` property, which the monitor calls back as necessary. Callers
/// can also set an `errorHandler` to receive asynchronous error notifications,
/// such as the monitor service suddenly becoming unavailable.
///
///     class QuakeMonitor {
///         var quakeHandler: ((Quake) -> Void)?
///         var errorHandler: ((Error) -> Void)?
///
///         func startMonitoring() {…}
///         func stopMonitoring() {…}
///     }
///
/// To adapt this to use `async`-`await`, extend the `QuakeMonitor` to add a
/// `quakes` property, of type `AsyncThrowingStream<Quake>`. In the getter for
/// this property, return an `AsyncThrowingStream`, whose `build` closure --
/// called at runtime to create the stream -- uses the continuation to
/// perform the following steps:
///
/// 1. Creates a `QuakeMonitor` instance.
/// 2. Sets the monitor's `quakeHandler` property to a closure that receives
/// each `Quake` instance and forwards it to the stream by calling the
/// continuation's `yield(_:)` method.
/// 3. Sets the monitor's `errorHandler` property to a closure that receives
/// any error from the monitor and forwards it to the stream by calling the
/// continuation's `finish(throwing:)` method. This causes the stream's
/// iterator to throw the error and terminate the stream.
/// 4. Sets the continuation's `onTermination` property to a closure that
/// calls `stopMonitoring()` on the monitor.
/// 5. Calls `startMonitoring` on the `QuakeMonitor`.
///
/// ```
/// extension QuakeMonitor {
///
///     static var throwingQuakes: AsyncThrowingStream<Quake, Error> {
///         AsyncThrowingStream { continuation in
///             let monitor = QuakeMonitor()
///             monitor.quakeHandler = { quake in
///                  continuation.yield(quake)
///             }
///             monitor.errorHandler = { error in
///                 continuation.finish(throwing: error)
///             }
///             continuation.onTermination = { @Sendable _ in
///                 monitor.stopMonitoring()
///             }
///             monitor.startMonitoring()
///         }
///     }
/// }
/// ```
///
///
/// Because the stream is an `AsyncSequence`, the call point uses the
/// `for`-`await`-`in` syntax to process each `Quake` instance as produced by the stream:
///
///     do {
///         for try await quake in quakeStream {
///             print("Quake: \(quake.date)")
///         }
///         print("Stream done.")
///     } catch {
///         print("Error: \(error)")
///     }
///
@available(SwiftStdlib 5.1, *)
public struct AsyncThrowingStream<Element, Failure: Error> {
  /// A mechanism to interface between synchronous code and an asynchronous
  /// stream.
  ///
  /// The closure you provide to the `AsyncThrowingStream` in
  /// `init(_:bufferingPolicy:_:)` receives an instance of this type when
  /// invoked. Use this continuation to provide elements to the stream by
  /// calling one of the `yield` methods, then terminate the stream normally by
  /// calling the `finish()` method. You can also use the continuation's
  /// `finish(throwing:)` method to terminate the stream by throwing an error.
  ///
  /// - Note: Unlike other continuations in Swift,
  /// `AsyncThrowingStream.Continuation` supports escaping.
  public struct Continuation: Sendable {
    /// A type that indicates how the stream terminated.
    ///
    /// The `onTermination` closure receives an instance of this type.
    public enum Termination {
      
      /// The stream finished as a result of calling the continuation's
      ///  `finish` method.
      ///
      ///  The associated `Failure` value provides the error that terminated
      ///  the stream. If no error occurred, this value is `nil`.
      case finished(Failure?)
      
      /// The stream finished as a result of cancellation.
      case cancelled
    }
    
    /// A type that indicates the result of yielding a value to a client, by
    /// way of the continuation.
    ///
    /// The various `yield` methods of `AsyncThrowingStream.Continuation` return
    /// this type to indicate the success or failure of yielding an element to
    /// the continuation.
    public enum YieldResult {
      
      /// The stream successfully enqueued the element.
      ///
      /// This value represents the successful enqueueing of an element, whether
      /// the stream buffers the element or delivers it immediately to a pending
      /// call to `next()`. The associated value `remaining` is a hint that
      /// indicates the number of remaining slots in the buffer at the time of
      /// the `yield` call.
      ///
      /// - Note: From a thread safety perspective, `remaining` is a lower bound
      /// on the number of remaining slots. This is because a subsequent call
      /// that uses the `remaining` value could race on the consumption of
      /// values from the stream.
      case enqueued(remaining: Int)
      
      /// The stream didn't enqueue the element because the buffer was full.
      ///
      /// The associated element for this case is the element that the stream
      /// dropped.
      case dropped(Element)
      
      /// The stream didn't enqueue the element because the stream was in a
      /// terminal state.
      ///
      /// This indicates the stream terminated prior to calling `yield`, either
      /// because the stream finished normally or through cancellation, or
      /// it threw an error.
      case terminated
    }
    
    /// A strategy that handles exhaustion of a buffer’s capacity.
    public enum BufferingPolicy {
      /// Continue to add to the buffer, treating its capacity as infinite.
      case unbounded
      
      /// When the buffer is full, discard the newly received element.
      ///
      /// This strategy enforces keeping the specified amount of oldest values.
      case bufferingOldest(Int)
      
      /// When the buffer is full, discard the oldest element in the buffer.
      ///
      /// This strategy enforces keeping the specified amount of newest values.
      case bufferingNewest(Int)
    }

    let storage: _Storage

    /// Resume the task awaiting the next iteration point by having it return
    /// normally from its suspension point with a given element.
    ///
    /// - Parameter value: The value to yield from the continuation.
    /// - Returns: A `YieldResult` that indicates the success or failure of the
    ///   yield operation.
    ///
    /// If nothing is awaiting the next value, the method attempts to buffer the
    /// result's element.
    ///
    /// This can be called more than once and returns to the caller immediately
    /// without blocking for any awaiting consumption from the iteration.
    @discardableResult
    public func yield(_ value: sending Element) -> YieldResult {
      storage.yield(value)
    }

    /// Resume the task awaiting the next iteration point by having it return
    /// nil, which signifies the end of the iteration.
    ///
    /// - Parameter error: The error to throw, or `nil`, to finish normally.
    ///
    /// Calling this function more than once has no effect. After calling
    /// finish, the stream enters a terminal state and doesn't produce any additional
    /// elements.
    public func finish(throwing error: __owned Failure? = nil) {
      storage.finish(throwing: error)
    }

    /// A callback to invoke when canceling iteration of an asynchronous
    /// stream.
    ///
    /// If an `onTermination` callback is set, using task cancellation to
    /// terminate iteration of an `AsyncThrowingStream` results in a call to this
    /// callback.
    ///
    /// Canceling an active iteration invokes the `onTermination` callback
    /// first, and then resumes by yielding `nil` or throwing an error from the
    /// iterator. This means that you can perform needed cleanup in the
    ///  cancellation handler. After reaching a terminal state, the
    ///  `AsyncThrowingStream` disposes of the callback.
    public var onTermination: (@Sendable (Termination) -> Void)? {
      get {
        return storage.getOnTermination()
      }
      nonmutating set {
        storage.setOnTermination(newValue)
      }
    }
  }

  final class _Context {
    let storage: _Storage?
    let produce: () async throws(Failure) -> Element?

    init(storage: _Storage? = nil, produce: @escaping () async throws(Failure) -> Element?) {
      self.storage = storage
      self.produce = produce
    }

    deinit {
      storage?.cancel()
    }
  }

  let context: _Context

  /// Constructs an asynchronous stream for an element type, using the
  /// specified buffering policy and element-producing closure.
  ///
  /// - Parameters:
  ///   - elementType: The type of element the `AsyncThrowingStream`
  ///   produces.
  ///   - limit: The maximum number of elements to
  ///   hold in the buffer. By default, this value is unlimited. Use a
  ///   `Continuation.BufferingPolicy` to buffer a specified number of oldest
  ///   or newest elements.
  ///   - build: A custom closure that yields values to the
  ///   `AsyncThrowingStream`. This closure receives an
  ///   `AsyncThrowingStream.Continuation` instance that it uses to provide
  ///   elements to the stream and terminate the stream when finished.
  ///
  /// The `AsyncStream.Continuation` received by the `build` closure is
  /// appropriate for use in concurrent contexts. It is thread safe to send and
  /// finish; all calls are to the continuation are serialized. However, calling
  /// this from multiple concurrent contexts could result in out-of-order
  /// delivery.
  ///
  /// The following example shows an `AsyncStream` created with this
  /// initializer that produces 100 random numbers on a one-second interval,
  /// calling `yield(_:)` to deliver each element to the awaiting call point.
  /// When the `for` loop exits, the stream finishes by calling the
  /// continuation's `finish()` method. If the random number is divisible by 5
  /// with no remainder, the stream throws a `MyRandomNumberError`.
  ///
  ///     let stream = AsyncThrowingStream<Int, Error>(Int.self,
  ///                                                  bufferingPolicy: .bufferingNewest(5)) { continuation in
  ///         Task.detached {
  ///             for _ in 0..<100 {
  ///                 await Task.sleep(1 * 1_000_000_000)
  ///                 let random = Int.random(in: 1...10)
  ///                 if random % 5 == 0 {
  ///                     continuation.finish(throwing: MyRandomNumberError())
  ///                     return
  ///                 } else {
  ///                     continuation.yield(random)
  ///                 }
  ///             }
  ///             continuation.finish()
  ///         }
  ///     }
  ///
  ///     // Call point:
  ///     do {
  ///         for try await random in stream {
  ///             print(random)
  ///         }
  ///     } catch {
  ///         print(error)
  ///     }
  ///
  public init(
    _ elementType: Element.Type = Element.self,
    bufferingPolicy limit: Continuation.BufferingPolicy = .unbounded,
    _ build: (Continuation) -> Void
  ) where Failure == Error {
    let storage: _Storage = .create(limit: limit)
    context = _Context(storage: storage, produce: storage.next)
    build(Continuation(storage: storage))
  }
  
  /// Constructs an asynchronous throwing stream from a given element-producing
  /// closure.
  ///
  /// - Parameters:
  ///   - produce: A closure that asynchronously produces elements for the
  ///    stream.
  ///
  /// Use this convenience initializer when you have an asynchronous function
  /// that can produce elements for the stream, and don't want to invoke
  /// a continuation manually. This initializer "unfolds" your closure into
  /// a full-blown asynchronous stream. The created stream handles adherence to
  /// the `AsyncSequence` protocol automatically. To terminate the stream with
  /// an error, throw the error from your closure.
  ///
  /// The following example shows an `AsyncThrowingStream` created with this
  /// initializer that produces random numbers on a one-second interval. If the
  /// random number is divisible by 5 with no remainder, the stream throws a
  /// `MyRandomNumberError`.
  ///
  ///     let stream = AsyncThrowingStream<Int, Error> {
  ///         await Task.sleep(1 * 1_000_000_000)
  ///         let random = Int.random(in: 1...10)
  ///         if random % 5 == 0 {
  ///             throw MyRandomNumberError()
  ///         }
  ///         return random
  ///     }
  ///
  ///     // Call point:
  ///     do {
  ///         for try await random in stream {
  ///             print(random)
  ///         }
  ///     } catch {
  ///         print(error)
  ///     }
  ///
  @preconcurrency
  public init(
    unfolding produce: @escaping @Sendable () async throws -> Element?
  ) where Failure == Error {
    let storage: _AsyncStreamCriticalStorage<Optional<() async throws -> Element?>>
      = .create(produce)
    context = _Context {
      return try await withTaskCancellationHandler {
        guard let result = try await storage.value?() else {
          storage.value = nil
          return nil
        }
        return result
      } onCancel: {
        storage.value = nil
      }
    }
  }
}

@available(SwiftStdlib 5.1, *)
extension AsyncThrowingStream: AsyncSequence {
  /// The asynchronous iterator for iterating an asynchronous stream.
  ///
  /// This type is not `Sendable`. Don't use it from multiple
  /// concurrent contexts. It is a programmer error to invoke `next()` from a
  /// concurrent context that contends with another such call, which
  /// results in a call to `fatalError()`.
  public struct Iterator: AsyncIteratorProtocol {
    let context: _Context

    /// The next value from the asynchronous stream.
    ///
    /// When `next()` returns `nil`, this signifies the end of the
    /// `AsyncThrowingStream`.
    ///
    /// It is a programmer error to invoke `next()` from a concurrent context
    /// that contends with another such call, which results in a call to
    ///  `fatalError()`.
    ///
    /// If you cancel the task this iterator is running in while `next()` is
    /// awaiting a value, the `AsyncThrowingStream` terminates. In this case,
    /// `next()` may return `nil` immediately, or else return `nil` on
    /// subsequent calls.
    public mutating func next() async throws -> Element? {
      return try await context.produce()
    }

    /// The next value from the asynchronous stream.
    ///
    /// When `next()` returns `nil`, this signifies the end of the
    /// `AsyncThrowingStream`.
    ///
    /// It is a programmer error to invoke `next()` from a concurrent
    /// context that contends with another such call, which results in a call to
    /// `fatalError()`.
    ///
    /// If you cancel the task this iterator is running in while `next()`
    /// is awaiting a value, the `AsyncThrowingStream` terminates. In this case,
    /// `next()` may return `nil` immediately, or else return `nil` on
    /// subsequent calls.
    @available(SwiftStdlib 6.0, *)
    public mutating func next(isolation actor: isolated (any Actor)?) async throws(Failure) -> Element? {
      return try await context.produce()
    }
  }

  /// Creates the asynchronous iterator that produces elements of this
  /// asynchronous sequence.
  public func makeAsyncIterator() -> Iterator {
    return Iterator(context: context)
  }
}

@available(SwiftStdlib 5.1, *)
extension AsyncThrowingStream.Continuation {
  /// Resume the task awaiting the next iteration point by having it return
  /// normally or throw, based on a given result.
  ///
  /// - Parameter result: A result to yield from the continuation. In the
  ///   `.success(_:)` case, this returns the associated value from the
  ///   iterator's `next()` method. If the result is the `failure(_:)` case,
  ///   this call terminates the stream with the result's error, by calling
  ///   `finish(throwing:)`.
  /// - Returns: A `YieldResult` that indicates the success or failure of the
  ///   yield operation.
  ///
  /// If nothing is awaiting the next value and the result is success, this call
  /// attempts to buffer the result's element.
  ///
  /// If you call this method repeatedly, each call returns immediately, without
  /// blocking for any awaiting consumption from the iteration.
  @discardableResult
  public func yield(
    with result: __shared sending Result<Element, Failure>
  ) -> YieldResult where Failure == Error {
    switch result {
    case .success(let val):
      return storage.yield(val)
    case .failure(let err):
      storage.finish(throwing: err)
      return .terminated
    }
  }

  /// Resume the task awaiting the next iteration point by having it return
  /// normally from its suspension point.
  ///
  /// - Returns: A `YieldResult` that indicates the success or failure of the
  ///   yield operation.
  ///
  /// Use this method with `AsyncThrowingStream` instances whose `Element`
  /// type is `Void`. In this case, the `yield()` call unblocks the
  /// awaiting iteration; there is no value to return.
  ///
  /// If you call this method repeatedly, each call returns immediately,
  /// without blocking for any awaiting consumption from the iteration.
  @discardableResult
  public func yield() -> YieldResult where Element == Void {
    storage.yield(())
  }
}

@available(SwiftStdlib 5.1, *)
extension AsyncThrowingStream {
  /// Initializes a new ``AsyncThrowingStream`` and an ``AsyncThrowingStream/Continuation``.
  ///
  /// - Parameters:
  ///   - elementType: The element type of the stream.
  ///   - failureType: The failure type of the stream.
  ///   - limit: The buffering policy that the stream should use.
  /// - Returns: A tuple containing the stream and its continuation. The continuation should be passed to the
  /// producer while the stream should be passed to the consumer.
  @available(SwiftStdlib 5.1, *)
  @backDeployed(before: SwiftStdlib 5.9)
  public static func makeStream(
      of elementType: Element.Type = Element.self,
      throwing failureType: Failure.Type = Failure.self,
      bufferingPolicy limit: Continuation.BufferingPolicy = .unbounded
  ) -> (stream: AsyncThrowingStream<Element, Failure>, continuation: AsyncThrowingStream<Element, Failure>.Continuation) where Failure == Error {
    var continuation: AsyncThrowingStream<Element, Failure>.Continuation!
    let stream = AsyncThrowingStream<Element, Failure>(bufferingPolicy: limit) { continuation = $0 }
    return (stream: stream, continuation: continuation!)
  }
}

@available(SwiftStdlib 5.1, *)
extension AsyncThrowingStream: @unchecked Sendable where Element: Sendable { }
#else
@available(SwiftStdlib 5.1, *)
@available(*, unavailable, message: "Unavailable in task-to-thread concurrency model")
public struct AsyncThrowingStream<Element, Failure: Error> {
  @available(SwiftStdlib 5.1, *)
  @available(*, unavailable, message: "Unavailable in task-to-thread concurrency model")
  public struct Continuation: Sendable {
    @available(SwiftStdlib 5.1, *)
    @available(*, unavailable, message: "Unavailable in task-to-thread concurrency model")
    public enum Termination {
      case finished(Failure?)
      case cancelled
    }
    @available(SwiftStdlib 5.1, *)
    @available(*, unavailable, message: "Unavailable in task-to-thread concurrency model")
    public enum YieldResult {
      case enqueued(remaining: Int)
      case dropped(Element)
      case terminated
    }
    @available(SwiftStdlib 5.1, *)
    @available(*, unavailable, message: "Unavailable in task-to-thread concurrency model")
    public enum BufferingPolicy {
      case unbounded
      case bufferingOldest(Int)
      case bufferingNewest(Int)
    }
    @discardableResult
    @available(SwiftStdlib 5.1, *)
    @available(*, unavailable, message: "Unavailable in task-to-thread concurrency model")
    public func yield(_ value: sending Element) -> YieldResult {
      fatalError("Unavailable in task-to-thread concurrency model")
    }
    @available(SwiftStdlib 5.1, *)
    @available(*, unavailable, message: "Unavailable in task-to-thread concurrency model")
    public func finish(throwing error: __owned Failure? = nil) {
      fatalError("Unavailable in task-to-thread concurrency model")
    }
    @available(SwiftStdlib 5.1, *)
    @available(*, unavailable, message: "Unavailable in task-to-thread concurrency model")
    public var onTermination: (@Sendable (Termination) -> Void)? {
      get {
        fatalError("Unavailable in task-to-thread concurrency model")
      }
      nonmutating set {
        fatalError("Unavailable in task-to-thread concurrency model")
      }
    }
  }
  @available(SwiftStdlib 5.1, *)
  @available(*, unavailable, message: "Unavailable in task-to-thread concurrency model")
  public init(
    _ elementType: Element.Type = Element.self,
    bufferingPolicy limit: Continuation.BufferingPolicy = .unbounded,
    _ build: (Continuation) -> Void
  ) where Failure == Error {
    fatalError("Unavailable in task-to-thread concurrency model")
  }
  @available(SwiftStdlib 5.1, *)
  @available(*, unavailable, message: "Unavailable in task-to-thread concurrency model")
  public init(
    unfolding produce: @escaping () async throws -> Element?
  ) where Failure == Error {
    fatalError("Unavailable in task-to-thread concurrency model")
  }
}

@available(SwiftStdlib 5.1, *)
@available(*, unavailable, message: "Unavailable in task-to-thread concurrency model")
extension AsyncThrowingStream {
  @available(SwiftStdlib 5.1, *)
  @available(*, unavailable, message: "Unavailable in task-to-thread concurrency model")
  public struct Iterator {
    @available(SwiftStdlib 5.1, *)
    @available(*, unavailable, message: "Unavailable in task-to-thread concurrency model")
    public mutating func next() async throws -> Element? {
      fatalError("Unavailable in task-to-thread concurrency model")
    }
  }
  @available(SwiftStdlib 5.1, *)
  @available(*, unavailable, message: "Unavailable in task-to-thread concurrency model")
  public func makeAsyncIterator() -> Iterator {
    fatalError("Unavailable in task-to-thread concurrency model")
  }
}

@available(SwiftStdlib 5.1, *)
@available(*, unavailable, message: "Unavailable in task-to-thread concurrency model")
extension AsyncThrowingStream.Continuation {
  @discardableResult
  @available(SwiftStdlib 5.1, *)
  @available(*, unavailable, message: "Unavailable in task-to-thread concurrency model")
  public func yield(
    with result: __shared sending Result<Element, Failure>
  ) -> YieldResult where Failure == Error {
    fatalError("Unavailable in task-to-thread concurrency model")
  }
  @discardableResult
  @available(SwiftStdlib 5.1, *)
  @available(*, unavailable, message: "Unavailable in task-to-thread concurrency model")
  public func yield() -> YieldResult where Element == Void {
    fatalError("Unavailable in task-to-thread concurrency model")
  }
}

@available(SwiftStdlib 5.1, *)
@available(*, unavailable, message: "Unavailable in task-to-thread concurrency model")
extension AsyncThrowingStream: @unchecked Sendable where Element: Sendable { }
#endif
