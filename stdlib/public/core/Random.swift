//===--- Random.swift -----------------------------------------------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2018 Apple Inc. and the Swift project authors
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
    // Everyone knows that generating an unbiased random integer in a range
    // 0 ..< upperBound, where upperBound is not a power of two, requires
    // rejection sampling. What if I told you that Big Random Number has
    // lied to us for decades, and we have been played for absolute fools?
    //
    // Previously Swift used Lemire's "nearly divisionless" method
    // (https://arxiv.org/abs/1805.10941) for this operation. We instead
    // now use a novel method that:
    //
    // - never divides
    // - avoids rejection sampling entirely
    // - achieves a theoretically optimal bound on the amount of randomness
    //   consumed to generate a sample
    // - delivers actual performance improvements for most real cases
    //
    // Lemire interprets each word from the random source as a fixed-point
    // number in [0, 1), multiplies by upperBound, and takes the floor. Up
    // to this point, this is the algorithm suggested by Knuth in TAoCP vol 2,
    // and as observed by Knuth, it is slightly biased. Lemire cleverly
    // corrects this bias via rejection sampling, which requires one division
    // in the general case (hence, "nearly divisionless").
    //
    // Our new algorithm takes a different approach. Rather than using
    // rejection sampling, we observe that the bias decreases exponentially
    // in the number of bits used for the computation. In the limit we are
    // interpreting the bitstream from the random source as a uniform real
    // number r in [0, 1) and ⌊r * upperBound⌋ provides an unbiased sample
    // in 0 ..< upperBound. The only challenge, then, is to know when we
    // have computed enough bits of the product to know what the result is.
    //
    // Observe that we can split the random stream at any bit position i,
    // yielding r = r₀ + r₁ with r₀ a fixed-point number in [0,1) and
    // 0 ≤ r₁ < 2⁻ⁱ. Further observe that:
    //
    //    result = ⌊r * upperBound⌋
    //           = ⌊r₀ * upperBound⌋ + ⌊frac(r₀*upperBound) + r₁*upperBound⌋
    //
    // The first term of this expression is Knuth's biased sample, which is
    // computed with just a full-width multiply.
    //
    // If i > log₂(upperBound), both summands in the second term are smaller
    // than 1, so the second term is either 0 or 1. Applying the bound on r₁,
    // we see that if frac(r₀ * upperBound) <= 1 - upperBound * 2⁻ⁱ, the
    // second term is necessarily zero, and the first term is the unbiased
    // result. Happily, this is _also_ a trivial computation on the low-order
    // part of the full-width multiply.
    //
    // If the test fails, we do not reject the sample, throwing away the bits
    // we have already consumed from the random source; instead we increase i
    // by a convenient amount, computing more bits of the product. This is the
    // criticial improvement; while Lemire has a probability of 1/2 to reject
    // for each word consumed in the worst case, we have a probability of
    // terminating of 1/2 for each _bit_ consumed. This reduces the worst-case
    // expected number of random bits required from O(log₂(upperBound)) to
    // log₂(upperBound) + O(1), which is optimal[1].
    //
    // Of more practical interest, this new algorithm opens an intriguing
    // possibility: we can compute just 64 extra bits, and have a probability
    // of 1 - 2⁻⁶⁴ of terminating. This is so close to certainty that we can
    // simply stop without introducing any measurable bias (detecting any
    // difference would require about 2¹²⁸ samples, which is prohibitive).
    // This is a significant performance improvement for slow random
    // generators, since it asymptotically reduces the number of bits
    // required by a factor of two for bignums, while matching or reducing
    // the expected number of bits required for smaller numbers. This is the
    // algorithm implemented below (the formally-uniform method is not
    // much more complex to implement and is only a little bit slower, but
    // there's no reason to do so).
    //
    // More intriguing still, this algorithm can be made unconditional by
    // removing the early out, so that every value computed requires word
    // size + 64 bits from the stream, which breaks the loop-carried
    // dependency for fast generators, unlocking vectorization and
    // parallelization where it was previously impossible. This is an
    // especially powerful advantage when paired with bitstream generators
    // that allow skip-ahead such as newer counter-based generators used
    // in simulations and ML.
    //
    // Note that it is _possible_ to employ Lemire's tighter early-out
    // check that involves a division with this algorithm as well; this
    // is beneficial in some cases when upperBound is a constant and the
    // generator is slow, but we do not think it necessary with the new
    // algorithm and other planned improvements.
    //
    // [1] We can actually achieve log₂(upperBound) + ε for any ε > 0 by
    // generating multiple random samples at once, but that is only of
    // theoretical interest--it is still interesting, however, since I
    // don't think anyone has described how to attain it previously.
    if T.bitWidth < 64 {
      // Ensure T is at least 64 bits wide, as this simplifies things
      // somewhat later on, and doesn't cost us anything with the existing
      // RNG protocol (we might investigate buffered RNGs that let you
      // draw less than 64 bits at a time at some future point).
      return T(truncatingIfNeeded: next(upperBound: UInt64(upperBound)))
    }
    let (result, fraction) = upperBound.multipliedFullWidth(by: next())
    // Optional early out: this is a performance win when the generator is
    // very slow (as with an unbuffered SystemRandomNumberGenerator), but
    // the unpredictable branch and variable consumption from the bitstream
    // is a performance hazard for fast generators. We'll keep it for now,
    // but future changes to allow buffering slow generators should lead
    // to its removal.
    if T(fraction) <= 0 &- upperBound { return result }
    // Compute the product with 64 additional bits of randomness, add that
    // to the fraction, and adjust result upwards if that sum carries out.
    let (_, carry) = T(fraction).addingReportingOverflow(
      multiplyHigh(upperBound, next())
    )
    return result + (carry ? 1 : 0)
  }
  
  /// Computes a*b >> 64.
  ///
  /// Requires T.bitWidth >= 64.
  @_transparent @usableFromInline
  internal func multiplyHigh<T:FixedWidthInteger & UnsignedInteger>(
    _ a: T, _ b: UInt64
  ) -> T {
    // This should eventually be made more efficient for user-defined
    // bignum types, via an explicit nx1 word product operation, but
    // is great for all standard library types, and still generally
    // better than the old implementation for bignums.
    let (phi, plo) = a.multipliedFullWidth(by: T(b))
    // Return the low 64 bits of phi (the bits above are all zero)
    // grafted on to the high bitWidth - 64 bits of plo.
    return phi &<< (T.bitWidth - 64) | T(plo >> 64)
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
#if $TypedThrows
    _withUnprotectedUnsafeMutablePointer(to: &random) {
      swift_stdlib_random($0, MemoryLayout<UInt64>.size)
    }
#else
    __abi_se0413_withUnsafeMutablePointer(to: &random) {
      swift_stdlib_random($0, MemoryLayout<UInt64>.size)
    }
#endif
    return random
  }
}
