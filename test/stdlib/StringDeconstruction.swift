// RUN: %target-run-simple-swift
// REQUIRES: executable_test
// UNSUPPORTED: freestanding

import StdlibUnittest
defer { runAllTests() }

var StringDeconstructTests = TestSuite("StringDeconstructTests")

enum ExpectedDeconstruction {
  case scratchIfAvailable
  case interiorPointer
  case extraAllocation
}

func expectDeconstruct(
  _ str: String,
  _ expectDeconstruct: ExpectedDeconstruction,
  stackTrace: SourceLocStack = SourceLocStack(),
  showFrame: Bool = true,
  file: String = #file, line: UInt = #line
) {
  var stackTrace = stackTrace.pushIf(showFrame, file: file, line: line)
  let expectBytes = Array(str.utf8)

  _ = Array<UInt8>(unsafeUninitializedCapacity: 16) {
    buffer, initializedCount in
    // Deconstruct with a provided scratch space

    // WS == with scratch, N == nil
    let scratch = UnsafeMutableRawBufferPointer(buffer)
    let (ownerWS, ptrWS, lengthWS, usedScratchWS, allocatedMemoryWS)
      : (AnyObject?, UnsafePointer<UInt8>, Int, Bool, Bool)
      = str._deconstructUTF8(scratch: scratch)
    let (ownerN, ptrN, lengthN, usedScratchN, allocatedMemoryN)
      : (AnyObject?, UnsafePointer<UInt8>, Int, Bool, Bool)
      = str._deconstructUTF8(scratch: nil)

    let rawBytesWS = UnsafeRawBufferPointer(start: ptrWS, count: lengthWS)
    let rawBytesN = UnsafeRawBufferPointer(start: ptrN, count: lengthN)

    expectEqualSequence(expectBytes, rawBytesWS, stackTrace: stackTrace)
    expectEqualSequence(rawBytesWS, rawBytesN, stackTrace: stackTrace)

    switch expectDeconstruct {
      case .scratchIfAvailable:
        expectNil(ownerWS, stackTrace: stackTrace)
        expectNotNil(ownerN, stackTrace: stackTrace)

        expectEqual(scratch.baseAddress, rawBytesWS.baseAddress,
          stackTrace: stackTrace)
        expectNotEqual(scratch.baseAddress, rawBytesN.baseAddress,
          stackTrace: stackTrace)

        expectTrue(lengthWS < scratch.count, stackTrace: stackTrace)
        expectTrue(lengthN < scratch.count, stackTrace: stackTrace)

        expectTrue(usedScratchWS, stackTrace: stackTrace)
        expectFalse(usedScratchN, stackTrace: stackTrace)

        expectFalse(allocatedMemoryWS, stackTrace: stackTrace)
        expectTrue(allocatedMemoryN, stackTrace: stackTrace)

      case .interiorPointer:
        // TODO: owner == (immortal ? nil : StringObject.largeAddress)
        expectTrue(str.isContiguousUTF8, stackTrace: stackTrace)
        var copy = str
        copy.withUTF8 {
          expectEqual($0.baseAddress, ptrWS, stackTrace: stackTrace)
          expectEqual($0.baseAddress, ptrN, stackTrace: stackTrace)
          expectEqual($0.count, lengthWS, stackTrace: stackTrace)
          expectEqual($0.count, lengthN, stackTrace: stackTrace)
        }

        expectFalse(usedScratchWS, stackTrace: stackTrace)
        expectFalse(usedScratchN, stackTrace: stackTrace)
        expectFalse(allocatedMemoryWS, stackTrace: stackTrace)
        expectFalse(allocatedMemoryN, stackTrace: stackTrace)
      case .extraAllocation:
        expectFalse(str.isContiguousUTF8, stackTrace: stackTrace)
        expectNotNil(ownerWS, stackTrace: stackTrace)
        expectNotNil(ownerN, stackTrace: stackTrace)
        expectFalse(usedScratchWS, stackTrace: stackTrace)
        expectFalse(usedScratchN, stackTrace: stackTrace)
        expectTrue(allocatedMemoryWS, stackTrace: stackTrace)
        expectTrue(allocatedMemoryN, stackTrace: stackTrace)
    }
  }
}

@inline(never)
func id<T>(_ a: T) -> T { a }

StringDeconstructTests.test("deconstruct") {
  let smallASCII = "abcd"

#if _pointerBitWidth(_32)
  let smallUTF8 = "ジッパ"
#else
  let smallUTF8 = "ジッパー"
#endif

  let large = "the quick fox jumped over the lazy brown dog"

  var largeMortal = large
  largeMortal.append(id("🧟‍♀️"))
  largeMortal.append(id(largeMortal.last!))

  expectDeconstruct(smallASCII, .scratchIfAvailable)
  expectDeconstruct(smallUTF8, .scratchIfAvailable)
  expectDeconstruct(large, .interiorPointer)
  expectDeconstruct(largeMortal, .interiorPointer)
}

#if _runtime(_ObjC)
import Foundation
StringDeconstructTests.test("deconstruct cocoa") {
  let smallCocoa: NSString = "aaa"
  let largeASCIICocoa: NSString = "the quick fox jumped over the lazy brown dog"
  let largeCocoa: NSString = "the quick 🧟‍♀️ ate the slow 🧠"

#if _pointerBitWidth(_32)
  expectDeconstruct(smallCocoa as String, .interiorPointer)
#elseif _pointerBitWidth(_64)
  expectDeconstruct(smallCocoa as String, .scratchIfAvailable)
#else
#error("Unknown platform")
#endif

  expectDeconstruct(largeASCIICocoa as String, .interiorPointer)
  expectDeconstruct(largeCocoa as String, .extraAllocation)
}
#endif

