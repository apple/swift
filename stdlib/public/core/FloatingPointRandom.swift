//===----------------------------------------------------------------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2020 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//

// This file implements methods to generate random floating-point numbers, with
// probability proportional to the distance between each representable value
// and the next.
//
// The behavior is as if choosing a real number in a range, and rounding down
// to the next representible value. For closed ranges, we extend to a half-open
// range bounded by upperBound.nextUp (treating infinity as one ulp beyond
// greatestFiniteMagnitude).
//
// This is analogous to the methods for generating random integers in a range,
// which also behave as if generating a uniform real number in the half-open
// interval and rounding down to the next integer. And for closed ranges, by
// extending to the half-open interval bounded by the next-larger integer.
//
// Terminology note: "raw binade" as used in this file refers to the set of
// all floating-point numbers that share the same sign and raw exponent.

extension BinaryFloatingPoint where RawSignificand: FixedWidthInteger {
  /// Returns a random value within the specified range, using the given
  /// generator as a source for randomness.
  ///
  /// Use this method to generate a floating-point value within a specific
  /// range when you are using a custom random number generator. This example
  /// creates three new values in the range `10.0 ..< 20.0`.
  ///
  ///     for _ in 1...3 {
  ///         print(Double.random(in: 10.0 ..< 20.0, using: &myGenerator))
  ///     }
  ///     // Prints "18.1900709259179"
  ///     // Prints "14.2286325689993"
  ///     // Prints "13.1485686260762"
  ///
  /// The `random(in:using:)` static method chooses a random value from a
  /// continuous uniform distribution in `range`, and then converts that value
  /// to the nearest representable value in this type. Depending on the size
  /// and span of `range`, some concrete values may be represented more
  /// frequently than others.
  ///
  /// - Note: The algorithm used to create random values may change in a future
  ///   version of Swift. If you're passing a generator that results in the
  ///   same sequence of floating-point values each time you run your program,
  ///   that sequence may change when your program is compiled using a
  ///   different version of Swift.
  ///
  /// - Parameters:
  ///   - range: The range in which to create a random value.
  ///     `range` must be finite and non-empty.
  ///   - generator: The random number generator to use when creating the
  ///     new random value.
  /// - Returns: A random value within the bounds of `range`.
  @inlinable
  public static func random<T: RandomNumberGenerator>(
    in range: Range<Self>,
    using generator: inout T
  ) -> Self {
    _precondition(
      range.upperBound.isFinite,
      "There is no uniform distribution on an infinite range"
    )
    return _uniformRandomRoundedDown(in: range, using: &generator)
  }
  
  /// Returns a random value within the specified range.
  ///
  /// Use this method to generate a floating-point value within a specific
  /// range. This example creates three new values in the range
  /// `10.0 ..< 20.0`.
  ///
  ///     for _ in 1...3 {
  ///         print(Double.random(in: 10.0 ..< 20.0))
  ///     }
  ///     // Prints "18.1900709259179"
  ///     // Prints "14.2286325689993"
  ///     // Prints "13.1485686260762"
  ///
  /// The `random()` static method chooses a random value from a continuous
  /// uniform distribution in `range`, and then converts that value to the
  /// nearest representable value in this type. Depending on the size and span
  /// of `range`, some concrete values may be represented more frequently than
  /// others.
  ///
  /// This method is equivalent to calling `random(in:using:)`, passing in the
  /// system's default random generator.
  ///
  /// - Parameter range: The range in which to create a random value.
  ///   `range` must be finite and non-empty.
  /// - Returns: A random value within the bounds of `range`.
  @inlinable
  public static func random(in range: Range<Self>) -> Self {
    var g = SystemRandomNumberGenerator()
    return random(in: range, using: &g)
  }
  
  /// Returns a random value within the specified range, using the given
  /// generator as a source for randomness.
  ///
  /// Use this method to generate a floating-point value within a specific
  /// range when you are using a custom random number generator. This example
  /// creates three new values in the range `10.0 ... 20.0`.
  ///
  ///     for _ in 1...3 {
  ///         print(Double.random(in: 10.0 ... 20.0, using: &myGenerator))
  ///     }
  ///     // Prints "18.1900709259179"
  ///     // Prints "14.2286325689993"
  ///     // Prints "13.1485686260762"
  ///
  /// The `random(in:using:)` static method chooses a random value from a
  /// continuous uniform distribution in `range`, and then converts that value
  /// to the nearest representable value in this type. Depending on the size
  /// and span of `range`, some concrete values may be represented more
  /// frequently than others.
  ///
  /// - Note: The algorithm used to create random values may change in a future
  ///   version of Swift. If you're passing a generator that results in the
  ///   same sequence of floating-point values each time you run your program,
  ///   that sequence may change when your program is compiled using a
  ///   different version of Swift.
  ///
  /// - Parameters:
  ///   - range: The range in which to create a random value. Must be finite.
  ///   - generator: The random number generator to use when creating the
  ///     new random value.
  /// - Returns: A random value within the bounds of `range`.
  @inlinable
  public static func random<T: RandomNumberGenerator>(
    in range: ClosedRange<Self>,
    using generator: inout T
  ) -> Self {
    _precondition(
      range.upperBound.isFinite,
      "There is no uniform distribution on an infinite range"
    )
    let extendedRange = range.lowerBound ..< range.upperBound.nextUp
    return _uniformRandomRoundedDown(in: extendedRange, using: &generator)
  }
  
  /// Returns a random value within the specified range.
  ///
  /// Use this method to generate a floating-point value within a specific
  /// range. This example creates three new values in the range
  /// `10.0 ... 20.0`.
  ///
  ///     for _ in 1...3 {
  ///         print(Double.random(in: 10.0 ... 20.0))
  ///     }
  ///     // Prints "18.1900709259179"
  ///     // Prints "14.2286325689993"
  ///     // Prints "13.1485686260762"
  ///
  /// The `random()` static method chooses a random value from a continuous
  /// uniform distribution in `range`, and then converts that value to the
  /// nearest representable value in this type. Depending on the size and span
  /// of `range`, some concrete values may be represented more frequently than
  /// others.
  ///
  /// This method is equivalent to calling `random(in:using:)`, passing in the
  /// system's default random generator.
  ///
  /// - Parameter range: The range in which to create a random value. Must be finite.
  /// - Returns: A random value within the bounds of `range`.
  @inlinable
  public static func random(in range: ClosedRange<Self>) -> Self {
    var g = SystemRandomNumberGenerator()
    return random(in: range, using: &g)
  }
  
  // MARK: Implementation details
  
  // Generate a random floating-point value in the specified range, as if a real
  // number were chosen uniformly at random from that range then rounded down to
  // the nearest representable value. An upper bound of infinity is treated as
  // one ulp beyond `greatestFiniteMagnitude`.
  //
  // The general approach is:
  //
  // i) Expand the range so its new bounds are symmetric about 0, and their
  //    significand bits are 0.
  //
  // ii) Divide the expanded range into 2^60 equal-size subintervals, which are
  //     labeled (in order) with integers.
  //
  // iii) Find the first and last sections that overlap the original range,
  //      call them M and N.
  //
  // iv) Choose an integer uniformly at random between M and N (inclusive).
  //
  // v) Pick a floating-point value uniformly at random from that section.
  //
  //    a. In most sections, all floating-point values have a single raw
  //       exponent, so only a significand needs to be generated.
  //
  //    b. The exceptions are sections with 0 as a bound, where first an
  //       exponent is chosen logarithmically, then a significand uniformly.
  //
  // vi) If the resulting value is contained in the original range, return it.
  //     Otherwise, continue from step iv.
  //
  // This strategy is augmented with special handling for very small ranges, to
  // avoid the scenario where only 1 or 2 sections overlap the range, and most
  // of the values in those sections fall outside the range.
  //
  // To implement the above algorithm, it is necessary to convert back and forth
  // between floating-point values and their corresponding section numbers. For
  // this purpose we utilize the symmetry between floating-point exponents and
  // integer binary logarithms.
  //
  // All integers that occupy a given number of bits, represent sections within
  // a single raw binade. Each raw binade is twice as wide as the previous, and
  // using one more bit produces twice as many integers, so the length of each
  // section stays constant across binades.
  //
  // The exception is when the raw exponent is 0, which is handled by using
  // integers below some minimum bit-length. For example, the integer 0
  // represents a section which begins at 0 and has the same length as all
  // other sections. Similarly, negative integers represent sections of the
  // same length extending below zero.
  @_alwaysEmitIntoClient
  internal static func _uniformRandomRoundedDown<R: RandomNumberGenerator>(
    in range: Range<Self>,
    using generator: inout R
  ) -> Self {
    _precondition(
      !range.isEmpty,
      "Can't get random value with an empty range"
    )
    _precondition(
      range.lowerBound.isFinite,
      "There is no uniform distribution on an infinite range"
    )
    
    // Fast path
    //
    // Simple ranges bounded by the start of a raw binade and either 0 or the
    // negative of the first bound. We expect these ranges will be the most
    // common in practice, as they include 0..<1 and -1..<1.
    let (a, b) = (range.lowerBound, range.upperBound)
    
    if (a.significandBitPattern == 0) && (b.significandBitPattern == 0) {
      let (aExp, bExp) = (a.exponentBitPattern, b.exponentBitPattern)
      
      if aExp == 0 {
        return _randomUpToExponent(bExp, using: &generator)
      } else if a == -b {
        return _randomUpToExponent(bExp, allowNegative: true, using: &generator)
      } else if bExp == 0 {
        return -_randomUpToExponent(aExp, using: &generator).nextUp
      }
    }
    
    // Small range
    //
    // Ranges that cross up to one raw binade boundary are handled here to
    // ensure the `while true` loop in the general case usually succeeds.
    //
    // This only needs to be done when it is possible for more than one
    // representable number in the second-largest raw binade of the range to
    // fall in a single section.
    if significandBitCount > _sectionBitCount &- 3  {
      if let x = _smallRangeUniformRandom(in: range, using: &generator) {
        return x
      }
    }
    
    // General case
    //
    // Expand the range to be centered at 0, with bounds having all significand
    // bits equal to 0. Divide it into 2^60 equal sections, and find which
    // sections intersect the original range.
    let (sections, e) = _sectionsAndExponent(range)
    
    while true {
      let n = Int64.random(in: sections, using: &generator)
      let x = _uniformRandomInSection(n, maxExponent: e, using: &generator)
      if range.contains(x) { return x }
    }
  }
  
  // MARK: General case
  
  // Convert a range of Self into a range of Int64 section numbers and the
  // corresponding maximum exponent.
  @_alwaysEmitIntoClient
  internal static func _sectionsAndExponent(
    _ range: Range<Self>
  ) -> (sections: ClosedRange<Int64>, maxExponent: RawExponent) {
    let (a, b) = (range.lowerBound, range.upperBound)
    
    let m = maximumMagnitude(a, b)
    var e = m.exponentBitPattern
    if m.significandBitPattern != 0 { e += 1 }
    
    let (low, _) = a._sectionNumber(maxExponent: e)
    let (h, isLowerBound) = b._sectionNumber(maxExponent: e)
    let high = isLowerBound ? h &- 1 : h
    return (low...high, e)
  }
  
  // Find which section a floating-point value is in. First subtract its raw
  // exponent from the maximum allowed, to obtain the number of leading zeros
  // in the section number. Then shift its significand bits (including the
  // implicit bit) to that position.
  @_alwaysEmitIntoClient
  internal func _sectionNumber(
    maxExponent eMax: RawExponent
  ) -> (section: Int64, isLowerBound: Bool) {
    let (e, s) = (exponentBitPattern, significandBitPattern)
    _internalInvariant((eMax != 0) && (e <= eMax))
    
    if self == 0 { return (section: 0, isLowerBound: true) }
    
    var n: UInt64
    let isLowerBound: Bool
    
    let w = Self._sectionBitCount - 1  // Subtract 1 to account for implicit bit
    let z = eMax - max(1, e)           // Leading zeros before implicit bit
    
    if z < w {
      // We will need (w - z) significand bits.
      let bitsNeeded = w &- Int(truncatingIfNeeded: z)
      let shift = bitsNeeded &- Self.significandBitCount
      
      if shift < 0 {
        // It is okay to use `&>>` here because `-shift` is less than
        // `Self.significandBitCount`. We know this because (z < w) implies
        // (bitsNeeded >= 1), so (shift >= 1 - Self.significandBitCount).
        let usableBits = s &>> -shift
        isLowerBound = s == (usableBits &<< -shift)
        n = UInt64(truncatingIfNeeded: usableBits)
      } else {
        // It is okay to use `&<<` here because `shift` is less than
        // `UInt64.bitWidth`. We know this because:
        // shift <= bitsNeeded <= w < _sectionBitCount <= UInt64.bitWidth
        n = UInt64(truncatingIfNeeded: s) &<< shift
        isLowerBound = true
      }
      
      if e != 0 {
        // As above, `&<<` is okay because (bitsNeeded < UInt64.bitWidth).
        n |= (1 &<< bitsNeeded)
      }
    } else if (z == w) && (e != 0) {
      (n, isLowerBound) = (1, s == 0)
    } else {
      (n, isLowerBound) = (0, false)
    }
    
    if self < 0 {
      n = isLowerBound ? (0 &- n) : ~n
    }
    return (Int64(bitPattern: n), isLowerBound)
  }
  
  // Choose a random number in a single section.
  //
  // The number of leading zeros in the section number indicates the number of
  // raw binades below maxExponent.
  //
  // Its remaining bits form the implicit bit and the significand of the result.
  // If there are not enough bits in the section number to fill the significand,
  // the low bits are chosen uniformly at random.
  //
  // Section 0 may span multiple raw binades, and is handled specially. Negative
  // sections are nearly mirrors of the positive, but off by one.
  @_alwaysEmitIntoClient
  internal static func _uniformRandomInSection<R: RandomNumberGenerator>(
    _ section: Int64,
    maxExponent eMax: RawExponent,
    using generator: inout R
  ) -> Self {
    _internalInvariant(eMax != 0)
    
    let k = (section < 0) ? ~section : section
    let n = UInt64(bitPattern: k)
    let x: Self
    
    if (n == 0) && (eMax >= _sectionBitCount) {
      // Section 0 spanning at least one full raw binade
      let e = eMax - RawExponent(truncatingIfNeeded: _sectionBitCount &- 1)
      x = _randomUpToExponent(e, using: &generator)
    } else {
      // Each other section fits in a single raw binade
      let z = n.leadingZeroBitCount &- (UInt64.bitWidth - _sectionBitCount)
      _internalInvariant(z >= 0)
      
      let isNormal = z < eMax
      let e = isNormal ? eMax - RawExponent(truncatingIfNeeded: z) : 0
      let unusedBitCount = isNormal ? z &+ 1 : Int(truncatingIfNeeded: eMax)
      let availableBitCount = _sectionBitCount &- unusedBitCount
      let shift = significandBitCount &- availableBitCount
      
      var s: RawSignificand
      
      if shift <= 0 {
        s = RawSignificand(truncatingIfNeeded: n >> -shift)
      } else {
        s = generator.next()
        s &= (1 << shift) &- 1
        s |= RawSignificand(truncatingIfNeeded: n) << shift
      }
      
      s &= _significandMask
      x = Self(sign: .plus, exponentBitPattern: e, significandBitPattern: s)
    }
    
    return (section < 0) ? -x.nextUp : x
  }
  
  // MARK: Fast path
  
  // Choose a uniformly random representable number with raw exponent less than
  // eMax. If allowNegative is true, then make it negative half the time.
  @_alwaysEmitIntoClient
  internal static func _randomUpToExponent<R: RandomNumberGenerator>(
    _ eMax: RawExponent,
    allowNegative: Bool = false,
    using generator: inout R
  ) -> Self {
    _internalInvariant(eMax != 0)
    
    let e: RawExponent
    var bits: UInt64
    var bitCount: Int
    
    if (exponentBitCount < Int.bitWidth) || (eMax <= Int.max) {
      // This branch is purely for optimizing speed
      var i = Int(truncatingIfNeeded: eMax)
      (i, bits, bitCount) = _randomExponent(upperBound: i, using: &generator)
      e = RawExponent(truncatingIfNeeded: i)
    } else {
      (e, bits, bitCount) = _randomExponent(upperBound: eMax, using: &generator)
    }
    
    var s: RawSignificand
    
    if bitCount < significandBitCount {
      s = generator.next()
      
      if bitCount == 0 {
        bits = UInt64(truncatingIfNeeded: s >> significandBitCount)
        bitCount = RawSignificand.bitWidth &- significandBitCount
      }
    } else {
      s = RawSignificand(truncatingIfNeeded: bits)
      bits >>= significandBitCount
      bitCount &-= significandBitCount
    }
    
    var isNegative = false
    
    if allowNegative {
      if bitCount == 0 {
        isNegative = Bool.random(using: &generator)
      } else {
        isNegative = (bits & 1) == 0
      }
    }
    
    s &= _significandMask
    let x = Self(sign: .plus, exponentBitPattern: e, significandBitPattern: s)
    return isNegative ? -x.nextUp : x
  }
  
  // Choose a raw exponent less than upperBound, with probability proportional
  // to the width of the raw binade with that raw exponent. Also return any
  // additional random bits that were left over, and a count of how many.
  //
  // This function is generic over T because it is faster for Int, but also
  // needs to work for RawExponent.
  @_alwaysEmitIntoClient
  internal static func _randomExponent<T, R>(
    upperBound: T,
    using generator: inout R
  ) -> (e: T, bits: UInt64, bitCount: Int)
    where R: RandomNumberGenerator, T: BinaryInteger
  {
    _internalInvariant(upperBound > 0)
    if upperBound == 1 { return (0, 0, 0) }
    
    var e = upperBound - 1
    var bits: UInt64
    var z: Int
    
    // Each raw binade (except raw exponent 0) is the same width as all those
    // below it. So with 50% probability stop where we are, and otherwise
    // reduce the exponent. Repeat until we stop or reach 0.
    repeat {
      bits = generator.next()
      z = bits.leadingZeroBitCount
      if e <= z {
        // Enough "continues" to reach raw exponent zero.
        // The rest of the bits are still random.
        return (0, bits, UInt64.bitWidth &- Int(truncatingIfNeeded: e))
      }
      e -= T(truncatingIfNeeded: z)
    } while bits == 0
    
    // All the bits after the first "stop" are still random.
    return (e, bits, UInt64.bitWidth &- 1 &- z)
  }
  
  // MARK: Small range
  
  // If the range is very small and either fits within one section or spans from
  // the end of a section to the beginning of the next, then for a type with a
  // large significand, the general-case algorithm would spin many times before
  // landing within the range.
  //
  // In order to prevent this, we handle small ranges separately. If the size of
  // the range, divided by the smallest ulp in it, fits in significandBitCount
  // bits, this function returns a random value from the range. Otherwise, for
  // larger ranges, it returns nil.
  @_alwaysEmitIntoClient
  internal static func _smallRangeUniformRandom<R: RandomNumberGenerator>(
    in range: Range<Self>,
    using generator: inout R
  ) -> Self? {
    let (a, b) = (range.lowerBound, range.upperBound)
    let aExp = a.exponentBitPattern
    let bExp = b.exponentBitPattern
    
    if a.sign == b.sign {
      let sign = a.sign
      let eSpan = (sign == .plus) ? (bExp - aExp) : (aExp - bExp)
      if eSpan > 1 { return nil }
      
      let aSig = a.significandBitPattern
      let bSig = b.significandBitPattern
      let (low, high) = (sign == .plus) ? (aSig, bSig) : (bSig, aSig)
      let x: Self
      
      if eSpan == 0 {
        // Single raw binade
        let s = RawSignificand.random(in: low..<high, using: &generator)
        x = Self(sign: sign, exponentBitPattern: aExp, significandBitPattern: s)
        
      } else {
        // Adjacent raw binades
        let eBase = (sign == .plus) ? aExp : bExp
        let isHigh: Bool
        let s: RawSignificand
        
        if (eBase == 0) && (high <= low) {
          // One subnormal
          let span = high &+ (_significandMask &- low)
          let r = RawSignificand.random(in: 0...span, using: &generator)
          isHigh = r < high
          s = isHigh ? r : low &+ (r &- high)
          
        } else if high <= (low &>> 1) {
          // Both normal
          let h2 = high &<< 1
          let span = h2 &+ (_significandMask &- low)
          let r = RawSignificand.random(in: 0...span, using: &generator)
          isHigh = r < h2
          s = isHigh ? (r &>> 1) : low &+ (r &- h2)
          
        } else {
          // Large range
          return nil
        }
        
        let e = isHigh ? eBase + 1 : eBase
        x = Self(sign: sign, exponentBitPattern: e, significandBitPattern: s)
      }
      
      return (sign == .plus) ? x : x.nextDown
      
    } else if (aExp == 0) && (bExp == 0) {
      // Subnormal opposite signs
      let bSig = b.significandBitPattern
      let span = a.significandBitPattern &+ bSig
      if span < bSig { return nil }
      
      let r = RawSignificand.random(in: 0 ..< span, using: &generator)
      let sign: FloatingPointSign = (r < bSig) ? .plus : .minus
      let s = (r < bSig) ? r : r &- bSig &+ 1
      return Self(sign: sign, exponentBitPattern: 0, significandBitPattern: s)
      
    } else {
      // Large range
      return nil
    }
  }
  
  // MARK: Helpers
  
  // If section numbers used 64 bits, then for ranges like `-1.0...1.0`, the
  // `Int64.random(in:using:)` call in the general case would need to call
  // `next()` twice on average. Each bit smaller than that halves the
  // probability of a second `next()` call.
  //
  // The tradeoff is wider sections, which means an increased probability of
  // landing in a section which spans more than one representable value and
  // thus requires a second random integer.
  //
  // We optimize for `Double` by using 60 bits. This gives worst-case ranges
  // like `-1.0...64.0` a 3% chance of needing a second random integer.
  @_transparent
  @_alwaysEmitIntoClient
  internal static var _sectionBitCount: Int { UInt64.bitWidth - 4 }
  
  @_transparent
  @_alwaysEmitIntoClient
  internal static var _significandMask: RawSignificand {
    // We use `<<` in case significandBitCount == RawSignificand.bitwidth
    return (1 << significandBitCount) &- 1
  }
}
