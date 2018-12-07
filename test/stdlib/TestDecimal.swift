// Copyright (c) 2014 - 2018 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//
//
// RUN: %empty-directory(%t)
//
// RUN: %target-clang %S/Inputs/FoundationBridge/FoundationBridge.m -c -o %t/FoundationBridgeObjC.o -g
// RUN: %target-build-swift %s -I %S/Inputs/FoundationBridge/ -Xlinker %t/FoundationBridgeObjC.o -o %t/TestDecimal
// RUN: %target-codesign %t/TestDecimal

// RUN: %target-run %t/TestDecimal > %t.txt
// REQUIRES: executable_test
// REQUIRES: objc_interop

import Foundation
import FoundationBridgeObjC

#if FOUNDATION_XCTEST
    import XCTest
    class TestDecimalSuper : XCTestCase { }
#else
    import StdlibUnittest
    class TestDecimalSuper { }
#endif

class TestDecimal : TestDecimalSuper {
    func test_AdditionWithNormalization() {
        
        let biggie = Decimal(65536)
        let smallee = Decimal(65536)
        let answer = biggie/smallee
        expectEqual(Decimal(1),answer)
        
        var one = Decimal(1)
        var addend = Decimal(1)
        var expected = Decimal()
        var result = Decimal()
        
        expected._isNegative = 0;
        expected._isCompact = 0;
        
        // 2 digits -- certain to work
        addend._exponent = -1;
        expectEqual(.noError, NSDecimalAdd(&result, &one, &addend, .plain), "1 + 0.1")
        expected._exponent = -1;
        expected._length = 1;
        expected._mantissa.0 = 11;
        expectEqual(.orderedSame, NSDecimalCompare(&expected, &result), "1.1 == 1 + 0.1")
        
        // 38 digits -- guaranteed by NSDecimal to work
        addend._exponent = -37;
        expectEqual(.noError, NSDecimalAdd(&result, &one, &addend, .plain), "1 + 1e-37")
        expected._exponent = -37;
        expected._length = 8;
        expected._mantissa.0 = 0x0001;
        expected._mantissa.1 = 0x0000;
        expected._mantissa.2 = 0x36a0;
        expected._mantissa.3 = 0x00f4;
        expected._mantissa.4 = 0x46d9;
        expected._mantissa.5 = 0xd5da;
        expected._mantissa.6 = 0xee10;
        expected._mantissa.7 = 0x0785;
        expectEqual(.orderedSame, NSDecimalCompare(&expected, &result), "1 + 1e-37")
        
        // 39 digits -- not guaranteed to work but it happens to, so we make the test work either way
        addend._exponent = -38;
        let error = NSDecimalAdd(&result, &one, &addend, .plain)
        expectTrue(error == .noError || error == .lossOfPrecision, "1 + 1e-38")
        if error == .noError {
            expected._exponent = -38;
            expected._length = 8;
            expected._mantissa.0 = 0x0001;
            expected._mantissa.1 = 0x0000;
            expected._mantissa.2 = 0x2240;
            expected._mantissa.3 = 0x098a;
            expected._mantissa.4 = 0xc47a;
            expected._mantissa.5 = 0x5a86;
            expected._mantissa.6 = 0x4ca8;
            expected._mantissa.7 = 0x4b3b;
            expectEqual(.orderedSame, NSDecimalCompare(&expected, &result), "1 + 1e-38")
        } else {
            expectEqual(.orderedSame, NSDecimalCompare(&one, &result), "1 + 1e-38")
        }
        
        // 40 digits -- doesn't work; need to make sure it's rounding for us
        addend._exponent = -39;
        expectEqual(.lossOfPrecision, NSDecimalAdd(&result, &one, &addend, .plain), "1 + 1e-39")
        expectEqual("1", result.description)
        expectEqual(.orderedSame, NSDecimalCompare(&one, &result), "1 + 1e-39")
    }

    func test_BasicConstruction() {
        let zero = Decimal()
        expectEqual(20, MemoryLayout<Decimal>.size)
        expectEqual(0, zero._exponent)
        expectEqual(0, zero._length)
        expectEqual(0, zero._isNegative)
        expectEqual(0, zero._isCompact)
        expectEqual(0, zero._reserved)
        let (m0, m1, m2, m3, m4, m5, m6, m7) = zero._mantissa
        expectEqual(0, m0)
        expectEqual(0, m1)
        expectEqual(0, m2)
        expectEqual(0, m3)
        expectEqual(0, m4)
        expectEqual(0, m5)
        expectEqual(0, m6)
        expectEqual(0, m7)
        expectEqual(8, NSDecimalMaxSize)
        expectEqual(32767, NSDecimalNoScale)
        expectFalse(zero.isNormal)
        expectTrue(zero.isFinite)
        expectTrue(zero.isZero)
        expectFalse(zero.isSubnormal)
        expectFalse(zero.isInfinite)
        expectFalse(zero.isNaN)
        expectFalse(zero.isSignaling)

        let d1 = Decimal(1234567890123456789 as UInt64)
        expectEqual(d1._exponent, 0)
        expectEqual(d1._length, 4)
    }
    func test_Constants() {
        expectEqual(8, NSDecimalMaxSize)
        expectEqual(32767, NSDecimalNoScale)
        let smallest = Decimal(_exponent: 127, _length: 8, _isNegative: 1, _isCompact: 1, _reserved: 0, _mantissa: (UInt16.max, UInt16.max, UInt16.max, UInt16.max, UInt16.max, UInt16.max, UInt16.max, UInt16.max))
        expectEqual(smallest, Decimal.leastFiniteMagnitude)
        let biggest = Decimal(_exponent: 127, _length: 8, _isNegative: 0, _isCompact: 1, _reserved: 0, _mantissa: (UInt16.max, UInt16.max, UInt16.max, UInt16.max, UInt16.max, UInt16.max, UInt16.max, UInt16.max))
        expectEqual(biggest, Decimal.greatestFiniteMagnitude)
        let leastNormal = Decimal(_exponent: -127, _length: 1, _isNegative: 0, _isCompact: 1, _reserved: 0, _mantissa: (1, 0, 0, 0, 0, 0, 0, 0))
        expectEqual(leastNormal, Decimal.leastNormalMagnitude)
        let leastNonzero = Decimal(_exponent: -127, _length: 1, _isNegative: 0, _isCompact: 1, _reserved: 0, _mantissa: (1, 0, 0, 0, 0, 0, 0, 0))
        expectEqual(leastNonzero, Decimal.leastNonzeroMagnitude)
        let pi = Decimal(_exponent: -38, _length: 8, _isNegative: 0, _isCompact: 1, _reserved: 0, _mantissa: (0x6623, 0x7d57, 0x16e7, 0xad0d, 0xaf52, 0x4641, 0xdfa7, 0xec58))
        expectEqual(pi, Decimal.pi)
        expectEqual(10, Decimal.radix)
        expectTrue(Decimal().isCanonical)
        expectFalse(Decimal().isSignalingNaN)
        expectFalse(Decimal.nan.isSignalingNaN)
        expectTrue(Decimal.nan.isNaN)
        expectEqual(.quietNaN, Decimal.nan.floatingPointClass)
        expectEqual(.positiveZero, Decimal().floatingPointClass)
        expectEqual(.negativeNormal, smallest.floatingPointClass)
        expectEqual(.positiveNormal, biggest.floatingPointClass)
        expectFalse(Double.nan.isFinite)
        expectFalse(Double.nan.isInfinite)
    }

    func test_Description() {
        expectEqual("0", Decimal().description)
        expectEqual("0", Decimal(0).description)
        expectEqual("10", Decimal(_exponent: 1, _length: 1, _isNegative: 0, _isCompact: 1, _reserved: 0, _mantissa: (1, 0, 0, 0, 0, 0, 0, 0)).description)
        expectEqual("10", Decimal(10).description)
        expectEqual("123.458", Decimal(_exponent: -3, _length: 2, _isNegative: 0, _isCompact:1, _reserved: 0, _mantissa: (57922, 1, 0, 0, 0, 0, 0, 0)).description)
        expectEqual("123.458", Decimal(123.458).description)
        expectEqual("123", Decimal(UInt8(123)).description)
        expectEqual("45", Decimal(Int8(45)).description)
        expectEqual("3.14159265358979323846264338327950288419", Decimal.pi.description)
        expectEqual("-30000000000", Decimal(sign: .minus, exponent: 10, significand: Decimal(3)).description)
        expectEqual("300000", Decimal(sign: .plus, exponent: 5, significand: Decimal(3)).description)
        expectEqual("5", Decimal(signOf: Decimal(3), magnitudeOf: Decimal(5)).description)
        expectEqual("-5", Decimal(signOf: Decimal(-3), magnitudeOf: Decimal(5)).description)
        expectEqual("5", Decimal(signOf: Decimal(3), magnitudeOf: Decimal(-5)).description)
        expectEqual("-5", Decimal(signOf: Decimal(-3), magnitudeOf: Decimal(-5)).description)
    }

    func test_ExplicitConstruction() {
        var explicit = Decimal(
            _exponent: 0x17f,
            _length: 0xff,
            _isNegative: 3,
            _isCompact: 4,
            _reserved: UInt32(1<<18 + 1<<17 + 1),
            _mantissa: (6, 7, 8, 9, 10, 11, 12, 13)
        )
        expectEqual(0x7f, explicit._exponent)
        expectEqual(0x7f, explicit.exponent)
        expectEqual(0x0f, explicit._length)
        expectEqual(1, explicit._isNegative)
        expectEqual(FloatingPointSign.minus, explicit.sign)
        expectTrue(explicit.isSignMinus)
        expectEqual(0, explicit._isCompact)
        expectEqual(UInt32(1<<17 + 1), explicit._reserved)
        let (m0, m1, m2, m3, m4, m5, m6, m7) = explicit._mantissa
        expectEqual(6, m0)
        expectEqual(7, m1)
        expectEqual(8, m2)
        expectEqual(9, m3)
        expectEqual(10, m4)
        expectEqual(11, m5)
        expectEqual(12, m6)
        expectEqual(13, m7)
        explicit._isCompact = 5
        explicit._isNegative = 6
        expectEqual(0, explicit._isNegative)
        expectEqual(1, explicit._isCompact)
        expectEqual(FloatingPointSign.plus, explicit.sign)
        expectFalse(explicit.isSignMinus)
        expectTrue(explicit.isNormal)
        
        let significand = explicit.significand
        expectEqual(0, significand._exponent)
        expectEqual(0, significand.exponent)
        expectEqual(0x0f, significand._length)
        expectEqual(0, significand._isNegative)
        expectEqual(1, significand._isCompact)
        expectEqual(0, significand._reserved)
        let (sm0, sm1, sm2, sm3, sm4, sm5, sm6, sm7) = significand._mantissa
        expectEqual(6, sm0)
        expectEqual(7, sm1)
        expectEqual(8, sm2)
        expectEqual(9, sm3)
        expectEqual(10, sm4)
        expectEqual(11, sm5)
        expectEqual(12, sm6)
        expectEqual(13, sm7)
        
        let ulp = explicit.ulp
        expectEqual(0x7f, ulp.exponent)
        expectEqual(8, ulp._length)
        expectEqual(0, ulp._isNegative)
        expectEqual(1, ulp._isCompact)
        expectEqual(0, ulp._reserved)
        expectEqual(1, ulp._mantissa.0)
        expectEqual(0, ulp._mantissa.1)
        expectEqual(0, ulp._mantissa.2)
        expectEqual(0, ulp._mantissa.3)
        expectEqual(0, ulp._mantissa.4)
        expectEqual(0, ulp._mantissa.5)
        expectEqual(0, ulp._mantissa.6)
        expectEqual(0, ulp._mantissa.7)
    }

    func test_Maths() {
        for i in -2...10 {
            for j in 0...5 {
                expectEqual(Decimal(i*j), Decimal(i) * Decimal(j), "\(Decimal(i*j)) == \(i) * \(j)")
                expectEqual(Decimal(i+j), Decimal(i) + Decimal(j), "\(Decimal(i+j)) == \(i)+\(j)")
                expectEqual(Decimal(i-j), Decimal(i) - Decimal(j), "\(Decimal(i-j)) == \(i)-\(j)")
                // Strideable methods
                expectEqual(Decimal(i).advanced(by: Decimal(j)), Decimal(i+j), "\(Decimal(i).advanced(by: Decimal(j))) == \(i) + \(j)")
                expectEqual(Decimal(i).distance(to: Decimal(j)), Decimal(j-i), "\(Decimal(i).distance(to: Decimal(j))) == \(j) - \(i)")
                expectEqual(Decimal(i).advanced(by: Decimal(i).distance(to: Decimal(j))), Decimal(j), "\(Decimal(i).advanced(by: Decimal(i).distance(to: Decimal(j)))) == \(Decimal(j))")
                if j != 0 {
                    let approximation = Decimal(Double(i)/Double(j))
                    let answer = Decimal(i) / Decimal(j)
                    let answerDescription = answer.description
                    let approximationDescription = approximation.description
                    var failed: Bool = false
                    var count = 0
                    let SIG_FIG = 14
                    for (a, b) in zip(answerDescription, approximationDescription) {
                        if a != b {
                            failed = true
                            break
                        }
                        if count == 0 && (a == "-" || a == "0" || a == ".") {
                            continue // don't count these as significant figures
                        }
                        if count >= SIG_FIG {
                            break
                        }
                        count += 1
                    }
                    expectFalse(failed, "\(Decimal(i/j)) == \(i)/\(j)")
                }
            }
        }
    }

    func test_Misc() {
        expectEqual(.minus, Decimal(-5.2).sign)
        expectEqual(.plus, Decimal(5.2).sign)
        var d = Decimal(5.2)
        expectEqual(.plus, d.sign)
        d.negate()
        expectEqual(.minus, d.sign)
        d.negate()
        expectEqual(.plus, d.sign)
        var e = Decimal(0)
        e.negate()
        expectEqual(e, 0)
        expectTrue(Decimal(3.5).isEqual(to: Decimal(3.5)))
        expectTrue(Decimal.nan.isEqual(to: Decimal.nan))
        expectTrue(Decimal(1.28).isLess(than: Decimal(2.24)))
        expectFalse(Decimal(2.28).isLess(than: Decimal(2.24)))
        expectTrue(Decimal(1.28).isTotallyOrdered(belowOrEqualTo: Decimal(2.24)))
        expectFalse(Decimal(2.28).isTotallyOrdered(belowOrEqualTo: Decimal(2.24)))
        expectTrue(Decimal(1.2).isTotallyOrdered(belowOrEqualTo: Decimal(1.2)))
        expectTrue(Decimal.nan.isEqual(to: Decimal.nan))
        expectTrue(Decimal.nan.isLess(than: Decimal(0)))
        expectFalse(Decimal.nan.isLess(than: Decimal.nan))
        expectTrue(Decimal.nan.isLessThanOrEqualTo(Decimal(0)))
        expectTrue(Decimal.nan.isLessThanOrEqualTo(Decimal.nan))
        expectFalse(Decimal.nan.isTotallyOrdered(belowOrEqualTo: Decimal.nan))
        expectFalse(Decimal.nan.isTotallyOrdered(belowOrEqualTo: Decimal(2.3)))
        expectTrue(Decimal(2) < Decimal(3))
        expectTrue(Decimal(3) > Decimal(2))
        expectEqual(Decimal(-9), Decimal(1) - Decimal(10))
        expectEqual(Decimal(3), Decimal(2).nextUp)
        expectEqual(Decimal(2), Decimal(3).nextDown)
        expectEqual(Decimal(-476), Decimal(1024).distance(to: Decimal(1500)))
        expectEqual(Decimal(68040), Decimal(386).advanced(by: Decimal(67654)))
        expectEqual(Decimal(1.234), abs(Decimal(1.234)))
        expectEqual(Decimal(1.234), abs(Decimal(-1.234)))
        var a = Decimal(1234)
        var r = a
        expectEqual(.noError, NSDecimalMultiplyByPowerOf10(&r, &a, 1, .plain))
        expectEqual(Decimal(12340), r)
        a = Decimal(1234)
        expectEqual(.noError, NSDecimalMultiplyByPowerOf10(&r, &a, 2, .plain))
        expectEqual(Decimal(123400), r)
        expectEqual(.overflow, NSDecimalMultiplyByPowerOf10(&r, &a, 128, .plain))
        expectTrue(r.isNaN)
        a = Decimal(1234)
        expectEqual(.noError, NSDecimalMultiplyByPowerOf10(&r, &a, -2, .plain))
        expectEqual(Decimal(12.34), r)
        var ur = r
        expectEqual(.underflow, NSDecimalMultiplyByPowerOf10(&ur, &r, -128, .plain))
        expectTrue(ur.isNaN)
        a = Decimal(1234)
        expectEqual(.noError, NSDecimalPower(&r, &a, 0, .plain))
        expectEqual(Decimal(1), r)
        a = Decimal(8)
        expectEqual(.noError, NSDecimalPower(&r, &a, 2, .plain))
        expectEqual(Decimal(64), r)
        a = Decimal(-2)
        expectEqual(.noError, NSDecimalPower(&r, &a, 3, .plain))
        expectEqual(Decimal(-8), r)
        for i in -2...10 {
            for j in 0...5 {
                var actual = Decimal(i)
                var result = actual
                expectEqual(.noError, NSDecimalPower(&result, &actual, j, .plain))
                let expected = Decimal(pow(Double(i), Double(j)))
                expectEqual(expected, result, "\(result) == \(i)^\(j)")
            }
        }
    }

    func test_MultiplicationOverflow() {
        var multiplicand = Decimal(_exponent: 0, _length: 8, _isNegative: 0, _isCompact: 0, _reserved: 0, _mantissa: ( 0xffff, 0xffff, 0xffff, 0xffff, 0xffff, 0xffff, 0xffff, 0xffff ))
        
        var result = Decimal()
        var multiplier = Decimal(1)
        
        multiplier._mantissa.0 = 2
        
        expectEqual(.noError, NSDecimalMultiply(&result, &multiplicand, &multiplier, .plain), "2 * max mantissa")
        expectEqual(.noError, NSDecimalMultiply(&result, &multiplier, &multiplicand, .plain), "max mantissa * 2")
        
        multiplier._exponent = 0x7f
        expectEqual(.overflow, NSDecimalMultiply(&result, &multiplicand, &multiplier, .plain), "2e127 * max mantissa")
        expectEqual(.overflow, NSDecimalMultiply(&result, &multiplier, &multiplicand, .plain), "max mantissa * 2e127")
    }

    func test_NaNInput() {
        var NaN = Decimal.nan
        var one = Decimal(1)
        var result = Decimal()
        
        expectNotEqual(.noError, NSDecimalAdd(&result, &NaN, &one, .plain))
        expectTrue(NSDecimalIsNotANumber(&result), "NaN + 1")
        expectNotEqual(.noError, NSDecimalAdd(&result, &one, &NaN, .plain))
        expectTrue(NSDecimalIsNotANumber(&result), "1 + NaN")
        
        expectNotEqual(.noError, NSDecimalSubtract(&result, &NaN, &one, .plain))
        expectTrue(NSDecimalIsNotANumber(&result), "NaN - 1")
        expectNotEqual(.noError, NSDecimalSubtract(&result, &one, &NaN, .plain))
        expectTrue(NSDecimalIsNotANumber(&result), "1 - NaN")
        
        expectNotEqual(.noError, NSDecimalMultiply(&result, &NaN, &one, .plain))
        expectTrue(NSDecimalIsNotANumber(&result), "NaN * 1")
        expectNotEqual(.noError, NSDecimalMultiply(&result, &one, &NaN, .plain))
        expectTrue(NSDecimalIsNotANumber(&result), "1 * NaN")
        
        expectNotEqual(.noError, NSDecimalDivide(&result, &NaN, &one, .plain))
        expectTrue(NSDecimalIsNotANumber(&result), "NaN / 1")
        expectNotEqual(.noError, NSDecimalDivide(&result, &one, &NaN, .plain))
        expectTrue(NSDecimalIsNotANumber(&result), "1 / NaN")
        
        expectNotEqual(.noError, NSDecimalPower(&result, &NaN, 0, .plain))
        expectTrue(NSDecimalIsNotANumber(&result), "NaN ^ 0")
        expectNotEqual(.noError, NSDecimalPower(&result, &NaN, 4, .plain))
        expectTrue(NSDecimalIsNotANumber(&result), "NaN ^ 4")
        expectNotEqual(.noError, NSDecimalPower(&result, &NaN, 5, .plain))
        expectTrue(NSDecimalIsNotANumber(&result), "NaN ^ 5")
        
        expectNotEqual(.noError, NSDecimalMultiplyByPowerOf10(&result, &NaN, 0, .plain))
        expectTrue(NSDecimalIsNotANumber(&result), "NaN e0")
        expectNotEqual(.noError, NSDecimalMultiplyByPowerOf10(&result, &NaN, 4, .plain))
        expectTrue(NSDecimalIsNotANumber(&result), "NaN e4")
        expectNotEqual(.noError, NSDecimalMultiplyByPowerOf10(&result, &NaN, 5, .plain))
        expectTrue(NSDecimalIsNotANumber(&result), "NaN e5")
    }

    func test_NegativeAndZeroMultiplication() {
        var one = Decimal(1)
        var zero = Decimal(0)
        var negativeOne = Decimal(-1)
        
        var result = Decimal()
        
        expectEqual(.noError, NSDecimalMultiply(&result, &one, &one, .plain), "1 * 1")
        expectEqual(.orderedSame, NSDecimalCompare(&one, &result), "1 * 1")
        
        expectEqual(.noError, NSDecimalMultiply(&result, &one, &negativeOne, .plain), "1 * -1")
        expectEqual(.orderedSame, NSDecimalCompare(&negativeOne, &result), "1 * -1")
        
        expectEqual(.noError, NSDecimalMultiply(&result, &negativeOne, &one, .plain), "-1 * 1")
        expectEqual(.orderedSame, NSDecimalCompare(&negativeOne, &result), "-1 * 1")
        
        expectEqual(.noError, NSDecimalMultiply(&result, &negativeOne, &negativeOne, .plain), "-1 * -1")
        expectEqual(.orderedSame, NSDecimalCompare(&one, &result), "-1 * -1")
        
        expectEqual(.noError, NSDecimalMultiply(&result, &one, &zero, .plain), "1 * 0")
        expectEqual(.orderedSame, NSDecimalCompare(&zero, &result), "1 * 0")
        expectEqual(0, result._isNegative, "1 * 0")
        
        expectEqual(.noError, NSDecimalMultiply(&result, &zero, &one, .plain), "0 * 1")
        expectEqual(.orderedSame, NSDecimalCompare(&zero, &result), "0 * 1")
        expectEqual(0, result._isNegative, "0 * 1")
        
        expectEqual(.noError, NSDecimalMultiply(&result, &negativeOne, &zero, .plain), "-1 * 0")
        expectEqual(.orderedSame, NSDecimalCompare(&zero, &result), "-1 * 0")
        expectEqual(0, result._isNegative, "-1 * 0")
        
        expectEqual(.noError, NSDecimalMultiply(&result, &zero, &negativeOne, .plain), "0 * -1")
        expectEqual(.orderedSame, NSDecimalCompare(&zero, &result), "0 * -1")
        expectEqual(0, result._isNegative, "0 * -1")
    }

    func test_Normalize() {
        var one = Decimal(1)
        var ten = Decimal(-10)
        expectEqual(.noError, NSDecimalNormalize(&one, &ten, .plain))
        expectEqual(Decimal(1), one)
        expectEqual(Decimal(-10), ten)
        expectEqual(1, one._length)
        expectEqual(1, ten._length)
        one = Decimal(1)
        ten = Decimal(10)
        expectEqual(.noError, NSDecimalNormalize(&one, &ten, .plain))
        expectEqual(Decimal(1), one)
        expectEqual(Decimal(10), ten)
        expectEqual(1, one._length)
        expectEqual(1, ten._length)
    }

    func test_NSDecimal() {
        var nan = Decimal.nan
        expectTrue(NSDecimalIsNotANumber(&nan))
        var zero = Decimal()
        expectFalse(NSDecimalIsNotANumber(&zero))
        var three = Decimal(3)
        var guess = Decimal()
        NSDecimalCopy(&guess, &three)
        expectEqual(three, guess)
        
        var f = Decimal(_exponent: 0, _length: 2, _isNegative: 0, _isCompact: 0, _reserved: 0, _mantissa: (0x0000, 0x0001, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000))
        let before = f.description
        expectEqual(0, f._isCompact)
        NSDecimalCompact(&f)
        expectEqual(1, f._isCompact)
        let after = f.description
        expectEqual(before, after)
    }

    func test_RepeatingDivision()  {
        let repeatingNumerator = Decimal(16)
        let repeatingDenominator = Decimal(9)
        let repeating = repeatingNumerator / repeatingDenominator
        
        let numerator = Decimal(1010)
        var result = numerator / repeating
        
        var expected = Decimal()
        expected._exponent = -35;
        expected._length = 8;
        expected._isNegative = 0;
        expected._isCompact = 1;
        expected._reserved = 0;
        expected._mantissa.0 = 51946;
        expected._mantissa.1 = 3;
        expected._mantissa.2 = 15549;
        expected._mantissa.3 = 55864;
        expected._mantissa.4 = 57984;
        expected._mantissa.5 = 55436;
        expected._mantissa.6 = 45186;
        expected._mantissa.7 = 10941;
        
        expectEqual(.orderedSame, NSDecimalCompare(&expected, &result), "568.12500000000000000000000000000248554: \(expected.description) != \(result.description)");
    }

    func test_Round() {
        let testCases = [
            // expected, start, scale, round
            ( 0, 0.5, 0, Decimal.RoundingMode.down ),
            ( 1, 0.5, 0, Decimal.RoundingMode.up ),
            ( 2, 2.5, 0, Decimal.RoundingMode.bankers ),
            ( 4, 3.5, 0, Decimal.RoundingMode.bankers ),
            ( 5, 5.2, 0, Decimal.RoundingMode.plain ),
            ( 4.5, 4.5, 1, Decimal.RoundingMode.down ),
            ( 5.5, 5.5, 1, Decimal.RoundingMode.up ),
            ( 6.5, 6.5, 1, Decimal.RoundingMode.plain ),
            ( 7.5, 7.5, 1, Decimal.RoundingMode.bankers ),
            
            ( -1, -0.5, 0, Decimal.RoundingMode.down ),
            ( -2, -2.5, 0, Decimal.RoundingMode.up ),
            ( -3, -2.5, 0, Decimal.RoundingMode.bankers ),
            ( -4, -3.5, 0, Decimal.RoundingMode.bankers ),
            ( -5, -5.2, 0, Decimal.RoundingMode.plain ),
            ( -4.5, -4.5, 1, Decimal.RoundingMode.down ),
            ( -5.5, -5.5, 1, Decimal.RoundingMode.up ),
            ( -6.5, -6.5, 1, Decimal.RoundingMode.plain ),
            ( -7.5, -7.5, 1, Decimal.RoundingMode.bankers ),
            ]
        for testCase in testCases {
            let (expected, start, scale, mode) = testCase
            var num = Decimal(start)
            var r = num
            NSDecimalRound(&r, &num, scale, mode)
            expectEqual(Decimal(expected), r)
            let numnum = NSDecimalNumber(decimal:Decimal(start))
            let behavior = NSDecimalNumberHandler(roundingMode: mode, scale: Int16(scale), raiseOnExactness: false, raiseOnOverflow: true, raiseOnUnderflow: true, raiseOnDivideByZero: true)
            let result = numnum.rounding(accordingToBehavior:behavior)
            expectEqual(Double(expected), result.doubleValue)
        }
    }

    func test_ScanDecimal() {
        let testCases = [
            // expected, value
            ( 123.456e78, "123.456e78" ),
            ( -123.456e78, "-123.456e78" ),
            ( 123.456, " 123.456 " ),
            ( 3.14159, " 3.14159e0" ),
            ( 3.14159, " 3.14159e-0" ),
            ( 0.314159, " 3.14159e-1" ),
            ( 3.14159, " 3.14159e+0" ),
            ( 31.4159, " 3.14159e+1" ),
            ( 12.34, " 01234e-02"),
            ]
        for testCase in testCases {
            let (expected, string) = testCase
            let decimal = Decimal(string:string)!
            let aboutOne = Decimal(expected) / decimal
            let approximatelyRight = aboutOne >= Decimal(0.99999) && aboutOne <= Decimal(1.00001)
            expectTrue(approximatelyRight, "\(expected) ~= \(decimal) : \(aboutOne) \(aboutOne >= Decimal(0.99999)) \(aboutOne <= Decimal(1.00001))" )
        }
        guard let ones = Decimal(string:"111111111111111111111111111111111111111") else {
            expectUnreachable("Unable to parse Decimal(string:'111111111111111111111111111111111111111')")
            return
        }
        let num = ones / Decimal(9)
        guard let answer = Decimal(string:"12345679012345679012345679012345679012.3") else {
            expectUnreachable("Unable to parse Decimal(string:'12345679012345679012345679012345679012.3')")
            return
        }
        expectEqual(answer,num,"\(ones) / 9 = \(answer) \(num)")
    }

    func test_SimpleMultiplication() {
        var multiplicand = Decimal()
        multiplicand._isNegative = 0
        multiplicand._isCompact = 0
        multiplicand._length = 1
        multiplicand._exponent = 1
        
        var multiplier = multiplicand
        multiplier._exponent = 2
        
        var expected = multiplicand
        expected._isNegative = 0
        expected._isCompact = 0
        expected._exponent = 3
        expected._length = 1
        
        var result = Decimal()
        
        for i in 1..<UInt8.max {
            multiplicand._mantissa.0 = UInt16(i)
            
            for j in 1..<UInt8.max {
                multiplier._mantissa.0 = UInt16(j)
                expected._mantissa.0 = UInt16(i) * UInt16(j)
                
                expectEqual(.noError, NSDecimalMultiply(&result, &multiplicand, &multiplier, .plain), "\(i) * \(j)")
                expectEqual(.orderedSame, NSDecimalCompare(&expected, &result), "\(expected._mantissa.0) == \(i) * \(j)");
            }
        }
    }

    func test_unconditionallyBridgeFromObjectiveC() {
        expectEqual(Decimal(), Decimal._unconditionallyBridgeFromObjectiveC(nil))
    }
}


#if !FOUNDATION_XCTEST
var DecimalTests = TestSuite("TestDecimal")
DecimalTests.test("test_AdditionWithNormalization") { TestDecimal().test_AdditionWithNormalization() }
DecimalTests.test("test_BasicConstruction") { TestDecimal().test_BasicConstruction() }
DecimalTests.test("test_Constants") { TestDecimal().test_Constants() }
DecimalTests.test("test_Description") { TestDecimal().test_Description() }
DecimalTests.test("test_ExplicitConstruction") { TestDecimal().test_ExplicitConstruction() }
DecimalTests.test("test_Maths") { TestDecimal().test_Maths() }
DecimalTests.test("test_Misc") { TestDecimal().test_Misc() }
DecimalTests.test("test_MultiplicationOverflow") { TestDecimal().test_MultiplicationOverflow() }
DecimalTests.test("test_NaNInput") { TestDecimal().test_NaNInput() }
DecimalTests.test("test_NegativeAndZeroMultiplication") { TestDecimal().test_NegativeAndZeroMultiplication() }
DecimalTests.test("test_Normalize") { TestDecimal().test_Normalize() }
DecimalTests.test("test_NSDecimal") { TestDecimal().test_NSDecimal() }
DecimalTests.test("test_RepeatingDivision") { TestDecimal().test_RepeatingDivision() }
DecimalTests.test("test_Round") { TestDecimal().test_Round() }
DecimalTests.test("test_ScanDecimal") { TestDecimal().test_ScanDecimal() }
DecimalTests.test("test_SimpleMultiplication") { TestDecimal().test_SimpleMultiplication() }
DecimalTests.test("test_unconditionallyBridgeFromObjectiveC") { TestDecimal().test_unconditionallyBridgeFromObjectiveC() }
runAllTests()
#endif
