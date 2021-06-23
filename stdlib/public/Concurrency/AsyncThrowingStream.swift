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

@available(SwiftStdlib 5.5, *)
public struct AsyncThrowingStream<Element, Failure: Error> {
  public struct Continuation: Sendable {
    /// Indication of the type of termination informed to
    /// `onTermination`.
    public enum Termination {
      
      /// The stream was finished via the `finish` method
      case finished(Failure?)
      
      /// The stream was cancelled
      case cancelled
    }
    
    /// A result of yielding values.
    public enum YieldResult {
      
      /// When a value is successfully enqueued, either buffered
      /// or immediately consumed to resume a pending call to next
      /// and a count of remaining slots available in the buffer at
      /// the point in time of yielding. Note: transacting upon the
      /// remaining count is only valid when then calls to yield are
      /// mutually exclusive.
      case enqueued(remaining: Int)
      
      /// Yielding resulted in not buffering an element because the
      /// buffer was full. The element is the dropped value.
      case dropped(Element)
      
      /// Indication that the continuation was yielded when the
      /// stream was already in a terminal state: either by cancel or
      /// by finishing.
      case terminated
    }
    
    /// A strategy that handles exhaustion of a buffer’s capacity.
    public enum BufferingPolicy {
      case unbounded
      
      /// When the buffer is full, discard the newly received element.
      /// This enforces keeping the specified amount of oldest values.
      case bufferingOldest(Int)
      
      /// When the buffer is full, discard the oldest element in the buffer.
      /// This enforces keeping the specified amount of newest values.
      case bufferingNewest(Int)
    }

    let storage: _Storage

    /// Resume the task awaiting the next iteration point by having it return
    /// normally from its suspension point or buffer the value if no awaiting
    /// next iteration is active.
    ///
    /// - Parameter value: The value to yield from the continuation.
    ///
    /// This can be called more than once and returns to the caller immediately
    /// without blocking for any awaiting consumption from the iteration.
    @discardableResult
    public func yield(_ value: __owned Element) -> YieldResult {
      storage.yield(value)
    }

    /// Resume the task awaiting the next iteration point by having it return
    /// nil or throw which signifies the end of the iteration.
    ///
    /// - Parameter error: The error to throw or nil to signify termination.
    ///
    /// Calling this function more than once is idempotent; i.e. finishing more
    /// than once does not alter the state beyond the requirements of
    /// AsyncSequence; which claims that all values past a terminal state are
    /// nil.
    public func finish(throwing error: __owned Failure? = nil) {
      storage.finish(throwing: error)
    }

    /// A callback to invoke when iteration of a AsyncThrowingStream is 
    /// cancelled.
    ///
    /// If an `onTermination` callback is set, when iteration of a AsyncStream 
    /// is cancelled via task cancellation that callback is invoked. The
    /// callback is disposed of after any terminal state is reached.
    ///
    /// Cancelling an active iteration will first invoke the onTermination 
    /// callback and then resume yeilding nil. This means that any cleanup state
    /// can be emitted accordingly in the cancellation handler
    public var onTermination: (@Sendable (Termination) -> Void)? {
      get {
        return storage.getOnTermination()
      }
      nonmutating set {
        storage.setOnTermination(newValue)
      }
    }
  }

  let produce: () async throws -> Element?

  /// Construct a AsyncThrowingStream buffering given an Element type.
  ///
  /// - Parameter elementType: The type the AsyncStream will produce.
  /// - Parameter maxBufferedElements: The maximum number of elements to
  ///   hold in the buffer past any checks for continuations being resumed.
  /// - Parameter build: The work associated with yielding values to the 
  ///   AsyncStream.
  ///
  /// The maximum number of pending elements limited by dropping the oldest
  /// value when a new value comes in if the buffer would excede the limit
  /// placed upon it. By default this limit is unlimited.
  ///
  /// The build closure passes in a Continuation which can be used in
  /// concurrent contexts. It is thread safe to send and finish; all calls
  /// to the continuation are serialized, however calling this from multiple
  /// concurrent contexts could result in out of order delivery.
  public init(
    _ elementType: Element.Type = Element.self,
    bufferingPolicy limit: Continuation.BufferingPolicy = .unbounded,
    _ build: (Continuation) -> Void
  ) where Failure == Error {
    let storage: _Storage = .create(limit: limit)
    self.init(unfolding: storage.next)
    build(Continuation(storage: storage))
  }
  
  public init(
    unfolding produce: @escaping () async throws -> Element?
  ) {
    self.produce = produce
  }
}

@available(SwiftStdlib 5.5, *)
extension AsyncThrowingStream: AsyncSequence {
  /// The asynchronous iterator for iterating a AsyncThrowingStream.
  ///
  /// This type is specificially not Sendable. It is not intended to be used
  /// from multiple concurrent contexts. Any such case that next is invoked
  /// concurrently and contends with another call to next is a programmer error
  /// and will fatalError.
  public struct Iterator: AsyncIteratorProtocol {
    let produce: () async throws -> Element?

    public mutating func next() async throws -> Element? {
      return try await produce()
    }
  }

  /// Construct an iterator.
  public func makeAsyncIterator() -> Iterator {
    return Iterator(produce: produce)
  }
}

@available(SwiftStdlib 5.5, *)
extension AsyncThrowingStream.Continuation {
  /// Resume the task awaiting the next iteration point by having it return
  /// normally from its suspension point or buffer the value if no awaiting
  /// next iteration is active.
  ///
  /// - Parameter result: A result to yield from the continuation.
  ///
  /// This can be called more than once and returns to the caller immediately
  /// without blocking for any awaiting consuption from the iteration.
  @discardableResult
  public func yield(
    with result: Result<Element, Failure>
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
  /// normally from its suspension point or buffer the value if no awaiting
  /// next iteration is active where the `Element` is `Void`.
  ///
  /// This can be called more than once and returns to the caller immediately
  /// without blocking for any awaiting consuption from the iteration.
  @discardableResult
  public func yield() -> YieldResult where Element == Void {
    storage.yield(())
  }
}
