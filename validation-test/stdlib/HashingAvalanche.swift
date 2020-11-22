// RUN: %target-build-swift -Xfrontend -disable-access-control -module-name a %s -o %t.out -O
// RUN: %target-codesign %t.out
// RUN: %target-run %t.out
// REQUIRES: executable_test

import SwiftPrivate
import StdlibUnittest


var HashingTestSuite = TestSuite("Hashing")

func avalancheTest<Input: FixedWidthInteger & UnsignedInteger>(
  for type: Input.Type,
  _ hashUnderTest: @escaping (Input) -> Int,
  _ pValue: Double
) {
  typealias Output = Int
  let testsInBatch = 100000
  let testData = (0 ..< testsInBatch).map { _ in Input.random(in: .min ... .max) }
  let testDataHashed = testData.map { hashUnderTest($0) }

  for inputBit in 0..<Input.bitWidth {
    // Using an array here makes the test too slow.
    let bitFlips = UnsafeMutablePointer<Int>.allocate(capacity: Output.bitWidth)
    bitFlips.initialize(repeating: 0, count: Output.bitWidth)
    for i in testData.indices {
      let inputA = testData[i]
      let outputA = testDataHashed[i]
      let inputB = inputA ^ (1 << UInt64(inputBit))
      let outputB = hashUnderTest(inputB)
      var delta = outputA ^ outputB
      for outputBit in 0..<Output.bitWidth {
        if delta & 1 == 1 {
          bitFlips[outputBit] += 1
        }
        delta = delta >> 1
      }
    }
    for outputBit in 0..<Output.bitWidth {
      expectTrue(
        chiSquaredUniform2(testsInBatch, bitFlips[outputBit], pValue),
        "inputBit: \(inputBit), outputBit: \(outputBit)")
    }
    bitFlips.deallocate()
  }
}

// White-box testing: assume that the other N-bit to N-bit mixing functions
// just dispatch to these.  (Avalanche test is relatively expensive.)
HashingTestSuite.test("Hasher.combine(UInt64)/avalanche") {
  avalancheTest(for: UInt64.self, _hashValue(for:), 0.02)
}

HashingTestSuite.test("Hasher.combine(UInt32)/avalanche") {
  avalancheTest(for: UInt32.self, _hashValue(for:), 0.02)
}

runAllTests()
