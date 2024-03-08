//===--- Random.swift -----------------------------------------------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2018-2023 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//

import SwiftShims

/// A type that provides uniformly distributed random data.
///
/// When you call methods that use random data, such as creating new random
/// values or shuffling a collection, you can pass a `RandomNumberGenerator`
/// type to be used as the source for randomness. When you don't pass a
/// generator, the default `SystemRandomNumberGenerator` type is used.
///
/// When providing new APIs that use randomness, provide a version that accepts
/// a generator conforming to the `RandomNumberGenerator` protocol as well as a
/// version that uses the default system generator. For example, this `Weekday`
/// enumeration provides static methods that return a random day of the week:
///
///     enum Weekday: CaseIterable {
///         case sunday, monday, tuesday, wednesday, thursday, friday, saturday
///
///         static func random<G: RandomNumberGenerator>(using generator: inout G) -> Weekday {
///             return Weekday.allCases.randomElement(using: &generator)!
///         }
///
///         static func random() -> Weekday {
///             var g = SystemRandomNumberGenerator()
///             return Weekday.random(using: &g)
///         }
///     }
///
/// Conforming to the RandomNumberGenerator Protocol
/// ================================================
///
/// A custom `RandomNumberGenerator` type can have different characteristics
/// than the default `SystemRandomNumberGenerator` type. For example, a
/// seedable generator can be used to generate a repeatable sequence of random
/// values for testing purposes.
///
/// To make a custom type conform to the `RandomNumberGenerator` protocol,
/// implement the required `next()` method. Each call to `next()` must produce
/// a uniform and independent random value.
///
/// Types that conform to `RandomNumberGenerator` should specifically document
/// the thread safety and quality of the generator.
public protocol RandomNumberGenerator {
  /// Returns a value from a uniform, independent distribution of binary data.
  ///
  /// Use this method when you need random binary data to generate another
  /// value. If you need an integer value within a specific range, use the
  /// static `random(in:using:)` method on that integer type instead of this
  /// method.
  ///
  /// - Returns: An unsigned 64-bit random value.
  mutating func next() -> UInt64
  
  /// Fills a buffer with uniform random bytes.
  @available(SwiftStdlib 5.9, *)
  mutating func fill(_ buffer: UnsafeMutableRawBufferPointer)
}

extension RandomNumberGenerator {
  
  // An unavailable default implementation of next() prevents types that do
  // not implement the RandomNumberGenerator interface from conforming to the
  // protocol; without this, the default next() method returning a generic
  // unsigned integer will be used, recursing infinitely and probably blowing
  // the stack.
  @available(*, unavailable)
  @_alwaysEmitIntoClient
  public mutating func next() -> UInt64 { fatalError() }
  
  // Widget: fills 0...8 random bytes to given address.
  @_alwaysEmitIntoClient
  @usableFromInline
  internal mutating func fillPartialWord(
    address: UnsafeMutableRawPointer,
    count: Int
  ) {
    _internalInvariant(count >= 0 && count <= 8)
    if count == 0 { return }
    let random: UInt64 = next()
    _withUnprotectedUnsafeBytes(of: random) {
      address.copyMemory(from: $0.baseAddress!, byteCount: count)
    }
  }
  
  // Default implementation of fill is unconditionally available for concrete
  // types (using fill in generic contexts requires an availability check).
  @inlinable
  @backDeployed(before: SwiftStdlib 5.9)
  public mutating func fill(_ buffer: UnsafeMutableRawBufferPointer) {
    if buffer.count == 0 { return }
    // Because buffer is non-empty, baseAddress must be valid, and force-unwrap
    // is safe.
    let base = buffer.baseAddress.unsafelyUnwrapped
    // next() yields 8 bytes, so we will step through the buffer by 8 bytes.
    let step = 8
    // stride(from:to:by:) incurs an overflow check that we know that we
    // can skip, so open-code a while loop instead.
    let roundedCount = buffer.count & -step
    var offset = 0
    while offset < roundedCount {
      fillPartialWord(address: base + offset, count: step)
      offset &+= step
    }
    // Fill the remaining 0 ..< step bytes.
    fillPartialWord(address: base + offset, count: buffer.count &- roundedCount)
  }
  
  /// Returns a value from a uniform, independent distribution of binary data.
  ///
  /// Use this method when you need random binary data to generate another
  /// value. If you need an integer value within a specific range, use the
  /// static `random(in:using:)` method on that integer type instead of this
  /// method.
  ///
  /// - Returns: A random value of `T`. Bits are randomly distributed so that
  ///   every value of `T` is equally likely to be returned.
  @inlinable
  public mutating func next<T: FixedWidthInteger & UnsignedInteger>() -> T {
    return T._random(using: &self)
  }
  
  /// Returns a random value that is less than the given upper bound.
  ///
  /// Use this method when you need random binary data to generate another
  /// value. If you need an integer value within a specific range, use the
  /// static `random(in:using:)` method on that integer type instead of this
  /// method.
  ///
  /// - Parameter upperBound: The upper bound for the randomly generated value.
  ///   Must be non-zero.
  /// - Returns: A random value of `T` in the range `0..<upperBound`. Every
  ///   value in the range `0..<upperBound` is equally likely to be returned.
  @inlinable
  public mutating func next<T: FixedWidthInteger & UnsignedInteger>(
    upperBound: T
  ) -> T {
    _precondition(upperBound != 0, "upperBound cannot be zero.")
    // We use Lemire's "nearly divisionless" method for generating random
    // integers in an interval. For a detailed explanation, see:
    // https://arxiv.org/abs/1805.10941
    var random: T = next()
    var m = random.multipliedFullWidth(by: upperBound)
    if m.low < upperBound {
      let t = (0 &- upperBound) % upperBound
      while m.low < t {
        random = next()
        m = random.multipliedFullWidth(by: upperBound)
      }
    }
    return m.high
  }
}

/// The system's default source of random data.
///
/// When you generate random values, shuffle a collection, or perform another
/// operation that depends on random data, this type is the generator used by
/// default. For example, the two method calls in this example are equivalent:
///
///     let x = Int.random(in: 1...100)
///     var g = SystemRandomNumberGenerator()
///     let y = Int.random(in: 1...100, using: &g)
///
/// `SystemRandomNumberGenerator` is automatically seeded, is safe to use in
/// multiple threads, and uses a cryptographically secure algorithm whenever
/// possible.
///
/// Platform Implementation of `SystemRandomNumberGenerator`
/// ========================================================
///
/// While the system generator is automatically seeded and thread-safe on every
/// platform, the cryptographic quality of the stream of random data produced by
/// the generator may vary. For more detail, see the documentation for the APIs
/// used by each platform.
///
/// - Apple platforms use `arc4random_buf(3)`.
/// - Linux platforms use `getrandom(2)` when available; otherwise, they read
///   from `/dev/urandom`.
/// - Windows uses `BCryptGenRandom`.
@frozen
public struct SystemRandomNumberGenerator: RandomNumberGenerator, Sendable {
  /// Creates a new instance of the system's default random number generator.
  @inlinable
  public init() { }
  
  /// Returns a value from a uniform, independent distribution of binary data.
  ///
  /// - Returns: An unsigned 64-bit random value.
  @inlinable
  public mutating func next() -> UInt64 {
    var random: UInt64 = 0
    _withUnprotectedUnsafeMutablePointer(to: &random) {
      swift_stdlib_random($0, MemoryLayout<UInt64>.size)
    }
    return random
  }
  
  // aEIC is appropriate here, as this is a trivial shim.
  @_alwaysEmitIntoClient
  public mutating func fill(_ buffer: UnsafeMutableRawBufferPointer) {
    if let p = buffer.baseAddress {
      swift_stdlib_random(p, buffer.count)
    }
  }
}
