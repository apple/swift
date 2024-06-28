// RUN: not %target-swift-frontend %s -c  -swift-version 6 -module-name _Concurrency

// READ THIS: This test is only supposed to be making sure that we do not crash
// when we fail to match a witness that doesn't match AsyncIteratorProtocol.next
// b/c of sending. It should fail... but not crash.

@available(SwiftStdlib 5.1, *)
public protocol AsyncIteratorProtocol<Element, Failure> {
  associatedtype Element

  /// The type of failure produced by iteration.
  @available(SwiftStdlib 6.0, *)
  associatedtype Failure: Error = any Error

  /// Asynchronously advances to the next element and returns it, or ends the
  /// sequence if there is no next element.
  ///
  /// - Returns: The next element, if it exists, or `nil` to signal the end of
  ///   the sequence.
  mutating func next() async throws -> sending Element?

  /// Asynchronously advances to the next element and returns it, or ends the
  /// sequence if there is no next element.
  ///
  /// - Returns: The next element, if it exists, or `nil` to signal the end of
  ///   the sequence.
  @available(SwiftStdlib 6.0, *)
  mutating func next(isolation actor: isolated (any Actor)?) async throws(Failure) -> sending Element?
}

@available(SwiftStdlib 5.1, *)
extension AsyncIteratorProtocol {
  /// Default implementation of `next(isolation:)` in terms of `next()`, which
  /// is required to maintain backward compatibility with existing async
  /// iterators.
  @available(SwiftStdlib 6.0, *)
  @inlinable
  public mutating func next(isolation actor: isolated (any Actor)?) async throws(Failure) -> sending Element? {
    do {
      return try await next()
    } catch {
      throw error as! Failure
    }
  }
}

@available(SwiftStdlib 5.1, *)
extension AsyncIteratorProtocol {
  /// Default implementation of `next()` in terms of `next(isolation:)`, which
  /// is required to maintain backward compatibility with existing async
  /// iterators.
  @available(SwiftStdlib 6.0, *)
  @inlinable
  public mutating func next() async throws(Failure) -> sending Element? {
#if $OptionalIsolatedParameters
    return try await next(isolation: nil)
#else
    fatalError("unsupported compiler")
#endif
  }
}

public struct FakeMapSequence<T> : AsyncIteratorProtocol {
  typealias Element = T

  @available(SwiftStdlib 6.0, *)
  @inlinable
  public mutating func next(isolation actor: isolated (any Actor)?) async throws(Failure) -> Element? {
    fatalError()
  }

  @inlinable
  public mutating func next() async throws -> Element? {
    fatalError()
  }
}

