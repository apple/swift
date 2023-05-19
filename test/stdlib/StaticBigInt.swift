//===----------------------------------------------------------------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2022 - 2023 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//
//
// RUN: %target-run-simple-swift(-parse-as-library)
// REQUIRES: executable_test
// REQUIRES: reflection
// UNSUPPORTED: use_os_stdlib
// END.
//
//===----------------------------------------------------------------------===//

import StdlibUnittest

@available(SwiftStdlib 5.8, *)
@main
final class StaticBigIntTests {

  @available(SwiftStdlib 5.8, *)
  static func main() {
    let testCase = StaticBigIntTests()
    let testSuite = TestSuite("StaticBigIntTests")
    testSuite.test("BinaryRepresentation",     testCase.testBinaryRepresentation)
    testSuite.test("TextualRepresentation",    testCase.testTextualRepresentation)
    testSuite.test("PrefixPlusTypeInference",  testCase.testPrefixPlusTypeInference)
    testSuite.test("PrefixMinusTypeInference", testCase.testPrefixMinusTypeInference)
    testSuite.test("WrapperNegativeValue",     testCase.testWrapperNegativeValue)
    testSuite.test("WrapperPositiveValue",     testCase.testWrapperPositiveValue)
    testSuite.test("WrapperFibonacciSequence", testCase.testWrapperFibonacciSequence)
    runAllTests()
  }
}

//===----------------------------------------------------------------------===//
// MARK: - Binary Representation
//===----------------------------------------------------------------------===//

@available(SwiftStdlib 5.8, *)
extension StaticBigIntTests {

  @available(SwiftStdlib 5.8, *)
  func testBinaryRepresentation() {
    typealias Expected = (signum: Int, bitWidth: Int, _32bit: [UInt], _64bit: [UInt])
    let m = UInt(bitPattern: Int.min)
    let keyValuePairs: KeyValuePairs<StaticBigInt, Expected> = [
      -0x80000000000000000000000000000002: (-1, 129, [~1, ~0, ~0, ~m, ~0], [~1, ~m, ~0]),
      -0x80000000000000000000000000000001: (-1, 129, [~0, ~0, ~0, ~m, ~0], [~0, ~m, ~0]),
      -0x80000000000000000000000000000000: (-1, 128, [ 0,  0,  0,  m, ~0], [ 0,  m, ~0]),
      -0x7FFFFFFFFFFFFFFFFFFFFFFFFFFFFFFF: (-1, 128, [ 1,  0,  0,  m, ~0], [ 1,  m, ~0]),
      -0x4:                                (-1,   3, [~3, ~0, ~0, ~0, ~0], [~3, ~0, ~0]),
      -0x3:                                (-1,   3, [~2, ~0, ~0, ~0, ~0], [~2, ~0, ~0]),
      -0x2:                                (-1,   2, [~1, ~0, ~0, ~0, ~0], [~1, ~0, ~0]),
      -0x1:                                (-1,   1, [~0, ~0, ~0, ~0, ~0], [~0, ~0, ~0]),
       0x0:                                ( 0,   1, [ 0,  0,  0,  0,  0], [ 0,  0,  0]),
       0x1:                                (+1,   2, [ 1,  0,  0,  0,  0], [ 1,  0,  0]),
       0x2:                                (+1,   3, [ 2,  0,  0,  0,  0], [ 2,  0,  0]),
       0x3:                                (+1,   3, [ 3,  0,  0,  0,  0], [ 3,  0,  0]),
       0x7FFFFFFFFFFFFFFFFFFFFFFFFFFFFFFE: (+1, 128, [~1, ~0, ~0, ~m,  0], [~1, ~m,  0]),
       0x7FFFFFFFFFFFFFFFFFFFFFFFFFFFFFFF: (+1, 128, [~0, ~0, ~0, ~m,  0], [~0, ~m,  0]),
       0x80000000000000000000000000000000: (+1, 129, [ 0,  0,  0,  m,  0], [ 0,  m,  0]),
       0x80000000000000000000000000000001: (+1, 129, [ 1,  0,  0,  m,  0], [ 1,  m,  0]),
    ]
    for (actual, expected) in keyValuePairs {
      expectEqual(expected.signum,   actual.signum())
      expectEqual(expected.bitWidth, actual.bitWidth)
#if _pointerBitWidth(_32)
      expectEqual(expected._32bit[0], actual[0])
      expectEqual(expected._32bit[1], actual[1])
      expectEqual(expected._32bit[2], actual[2])
      expectEqual(expected._32bit[3], actual[3])
      expectEqual(expected._32bit[4], actual[4])
#elseif _pointerBitWidth(_64)
      expectEqual(expected._64bit[0], actual[0])
      expectEqual(expected._64bit[1], actual[1])
      expectEqual(expected._64bit[2], actual[2])
#else
#error("Unimplemented")
#endif
    }
  }
}

//===----------------------------------------------------------------------===//
// MARK: - Textual Representation
//===----------------------------------------------------------------------===//

@available(SwiftStdlib 5.8, *)
extension StaticBigIntTests {

  @available(SwiftStdlib 5.8, *)
  func testTextualRepresentation() {
    let keyValuePairs: KeyValuePairs<StaticBigInt, String> = [
      -0x80000000000000000000000000000002: "-0x80000000000000000000000000000002",
      -0x80000000000000000000000000000001: "-0x80000000000000000000000000000001",
      -0x80000000000000000000000000000000: "-0x80000000000000000000000000000000",
      -0x7FFFFFFFFFFFFFFFFFFFFFFFFFFFFFFF: "-0x7FFFFFFFFFFFFFFFFFFFFFFFFFFFFFFF",
      -0x123456789ABCDE:                   "-0x123456789ABCDE",
      -0x123456789ABCD:                    "-0x123456789ABCD",
      -0x123456789ABC:                     "-0x123456789ABC",
      -0x1000:                             "-0x1000",
      -0x100:                              "-0x100",
      -0x10:                               "-0x10",
      -0x4:                                "-0x4",
      -0x3:                                "-0x3",
      -0x2:                                "-0x2",
      -0x1:                                "-0x1",
       0x0:                                "+0x0",
       0x1:                                "+0x1",
       0x2:                                "+0x2",
       0x3:                                "+0x3",
       0x10:                               "+0x10",
       0x100:                              "+0x100",
       0x1000:                             "+0x1000",
       0x123456789ABC:                     "+0x123456789ABC",
       0x123456789ABCD:                    "+0x123456789ABCD",
       0x123456789ABCDE:                   "+0x123456789ABCDE",
       0x7FFFFFFFFFFFFFFFFFFFFFFFFFFFFFFE: "+0x7FFFFFFFFFFFFFFFFFFFFFFFFFFFFFFE",
       0x7FFFFFFFFFFFFFFFFFFFFFFFFFFFFFFF: "+0x7FFFFFFFFFFFFFFFFFFFFFFFFFFFFFFF",
       0x80000000000000000000000000000000: "+0x80000000000000000000000000000000",
       0x80000000000000000000000000000001: "+0x80000000000000000000000000000001",
    ]
    for (actual, expected) in keyValuePairs {
      expectEqual(expected, String(reflecting: actual))
    }
  }
}

//===----------------------------------------------------------------------===//
// MARK: - Type Inference
//===----------------------------------------------------------------------===//

@available(SwiftStdlib 5.8, *)
extension StaticBigIntTests {
  
  func testPrefixPlusTypeInference() {
    let a: Int = 7
    // An earlier version of StaticBigInt contained a prefix + operation,
    // which caused b to be inferred to have type StaticBigInt rather than
    // Int:
    let b = +1
    // and then this would fail to typecheck, because there's no
    // Int + StaticBigInt operation.
    let c = a + b
  }
  
  func testPrefixMinusTypeInference() {
    // This example shouldn't suffer from the same problem as above, because
    // -1 is a literal, not a prefix operator followed by a literal.
    // Nonetheless, let's test it.
    let a: Int = 7
    let b = -1
    let c = a + b
  }
}

//===----------------------------------------------------------------------===//
// MARK: - Wrapper Associated Type
//===----------------------------------------------------------------------===//

@available(SwiftStdlib 5.8, *)
extension StaticBigIntTests {

  @available(SwiftStdlib 5.8, *)
  struct Wrapper: ExpressibleByIntegerLiteral {

    @available(SwiftStdlib 5.8, *)
    let actual: StaticBigInt

    @available(SwiftStdlib 5.8, *)
    init(integerLiteral actual: StaticBigInt) {
      self.actual = actual
    }
  }

  @available(SwiftStdlib 5.8, *)
  func testWrapperNegativeValue() {
    let negative = Wrapper(-0x00112233_44556677_8899AABB_CCDDEEFF)
    expectEqual( -1, negative.actual.signum())
    expectEqual(118, negative.actual.bitWidth)
#if _pointerBitWidth(_32)
    expectEqual(0x33221101, negative.actual[0])
    expectEqual(0x77665544, negative.actual[1])
    expectEqual(0xBBAA9988, negative.actual[2])
    expectEqual(0xFFEEDDCC, negative.actual[3])
    expectEqual(0xFFFFFFFF, negative.actual[4])
    expectEqual(0xFFFFFFFF, negative.actual[.max])
#elseif _pointerBitWidth(_64)
    expectEqual(0x77665544_33221101, negative.actual[0])
    expectEqual(0xFFEEDDCC_BBAA9988, negative.actual[1])
    expectEqual(0xFFFFFFFF_FFFFFFFF, negative.actual[2])
    expectEqual(0xFFFFFFFF_FFFFFFFF, negative.actual[.max])
#else
#error("Unimplemented")
#endif
  }

  @available(SwiftStdlib 5.8, *)
  func testWrapperPositiveValue() {
    let positive = Wrapper(0x00112233_44556677_8899AABB_CCDDEEFF)
    expectEqual( +1, positive.actual.signum())
    expectEqual(118, positive.actual.bitWidth)
#if _pointerBitWidth(_32)
    expectEqual(0xCCDDEEFF, positive.actual[0])
    expectEqual(0x8899AABB, positive.actual[1])
    expectEqual(0x44556677, positive.actual[2])
    expectEqual(0x00112233, positive.actual[3])
    expectEqual(0x00000000, positive.actual[4])
    expectEqual(0x00000000, positive.actual[.max])
#elseif _pointerBitWidth(_64)
    expectEqual(0x8899AABB_CCDDEEFF, positive.actual[0])
    expectEqual(0x00112233_44556677, positive.actual[1])
    expectEqual(0x00000000_00000000, positive.actual[2])
    expectEqual(0x00000000_00000000, positive.actual[.max])
#else
#error("Unimplemented")
#endif
  }

  @available(SwiftStdlib 5.8, *)
  func testWrapperFibonacciSequence() {
#if _pointerBitWidth(_32)
    let wordCount = 48
    let fibonacciSequence = Wrapper(
      0xB11924E1_6D73E55F_43A53F82_29CEA5DD_19D699A5_0FF80C38_09DE8D6D_06197ECB_03C50EA2_02547029_01709E79_00E3D1B0_008CCCC9_005704E7_0035C7E2_00213D05_00148ADD_000CB228_0007D8B5_0004D973_0002FF42_0001DA31_00012511_0000B520_00006FF1_0000452F_00002AC2_00001A6D_00001055_00000A18_0000063D_000003DB_00000262_00000179_000000E9_00000090_00000059_00000037_00000022_00000015_0000000D_00000008_00000005_00000003_00000002_00000001_00000001_00000000
    )
#elseif _pointerBitWidth(_64)
    let wordCount = 94
    let fibonacciSequence = Wrapper(
      0xA94FAD42221F2702_68A3DD8E61ECCFBD_40ABCFB3C0325745_27F80DDAA1BA7878_18B3C1D91E77DECD_0F444C01834299AB_096F75D79B354522_05D4D629E80D5489_039A9FADB327F099_023A367C34E563F0_016069317E428CA9_00D9CD4AB6A2D747_00869BE6C79FB562_00533163EF0321E5_00336A82D89C937D_001FC6E116668E68_0013A3A1C2360515_000C233F54308953_000780626E057BC2_0004A2DCE62B0D91_0002DD8587DA6E31_0001C5575E509F60_0001182E2989CED1_0000AD2934C6D08F_00006B04F4C2FE42_000042244003D24D_000028E0B4BF2BF5_000019438B44A658_00000F9D297A859D_000009A661CA20BB_000005F6C7B064E2_000003AF9A19BBD9_000002472D96A909_000001686C8312D0_000000DEC1139639_00000089AB6F7C97_0000005515A419A2_0000003495CB62F5_000000207FD8B6AD_0000001415F2AC48_0000000C69E60A65_00000007AC0CA1E3_00000004BDD96882_00000002EE333961_00000001CFA62F21_000000011E8D0A40_00000000B11924E1_000000006D73E55F_0000000043A53F82_0000000029CEA5DD_0000000019D699A5_000000000FF80C38_0000000009DE8D6D_0000000006197ECB_0000000003C50EA2_0000000002547029_0000000001709E79_0000000000E3D1B0_00000000008CCCC9_00000000005704E7_000000000035C7E2_0000000000213D05_0000000000148ADD_00000000000CB228_000000000007D8B5_000000000004D973_000000000002FF42_000000000001DA31_0000000000012511_000000000000B520_0000000000006FF1_000000000000452F_0000000000002AC2_0000000000001A6D_0000000000001055_0000000000000A18_000000000000063D_00000000000003DB_0000000000000262_0000000000000179_00000000000000E9_0000000000000090_0000000000000059_0000000000000037_0000000000000022_0000000000000015_000000000000000D_0000000000000008_0000000000000005_0000000000000003_0000000000000002_0000000000000001_0000000000000001_0000000000000000
    )
#else
#error("Unimplemented")
#endif
    expectEqual(
      1 + (wordCount * UInt.bitWidth),
      fibonacciSequence.actual.bitWidth
    )
    for wordIndex in 2..<wordCount {
      expectEqual(
        fibonacciSequence.actual[wordIndex],
        fibonacciSequence.actual[wordIndex - 1] +
        fibonacciSequence.actual[wordIndex - 2]
      )
    }
  }
}
