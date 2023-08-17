// RUN: %target-run-simple-swift
// REQUIRES: executable_test

// rdar://91405760
// UNSUPPORTED: use_os_stdlib, back_deployment_runtime

import StdlibUnittest
defer { runAllTests() }

var StringCreateTests = TestSuite("StringCreateTests")

enum SimpleString: String, CaseIterable {
  case smallASCII = "abcdefg"
  case smallUnicode = "abéÏ𓀀"
  case largeASCII = "012345678901234567890"
  case largeUnicode = "abéÏ012345678901234567890𓀀"
  case emoji = "😀😃🤢🤮👩🏿‍🎤🧛🏻‍♂️🧛🏻‍♂️👩‍👩‍👦‍👦"
  case empty = ""
}

extension String {
  var utf32: [UInt32] { return unicodeScalars.map { $0.value } }
}

StringCreateTests.test("String(decoding:as:)") {
  func validateDecodingAs(_ str: String) {
    // Non-contiguous (maybe) storage
    expectEqual(str, String(decoding: str.utf8, as: UTF8.self))
    expectEqual(str, String(decoding: str.utf16, as: UTF16.self))
    expectEqual(str, String(decoding: str.utf32, as: UTF32.self))

    // Contiguous storage
    expectEqual(str, String(decoding: Array(str.utf8), as: UTF8.self))
    expectEqual(str, String(decoding: Array(str.utf16), as: UTF16.self))
    expectEqual(str, String(decoding: Array(str.utf32), as: UTF32.self))

  }

  for simpleString in SimpleString.allCases {
    validateDecodingAs(simpleString.rawValue)
  }

  // https://github.com/apple/swift/issues/52275
  // Corner-case: UBP with null pointer.
  expectEqual(
    "", String(decoding: UnsafeBufferPointer(_empty: ()), as: UTF8.self))
  expectEqual(
    "", String(decoding: UnsafeBufferPointer(_empty: ()), as: UTF16.self))
  expectEqual(
    "", String(decoding: UnsafeBufferPointer(_empty: ()), as: UTF32.self))
}

if #available(SwiftStdlib 5.3, *) {
  StringCreateTests.test("String(unsafeUninitializedCapacity:initializingUTF8With:)") {
    for simpleString in SimpleString.allCases {
      let expected = simpleString.rawValue
      let expectedUTF8 = expected.utf8
      let actual = String(unsafeUninitializedCapacity: expectedUTF8.count) {
        _ = $0.initialize(from: expectedUTF8)
        return expectedUTF8.count
      }
      expectEqual(expected, actual)
    }
        
    let validUTF8: [UInt8] = [0x43, 0x61, 0x66, 0xC3, 0xA9]
    let invalidUTF8: [UInt8] = [0x43, 0x61, 0x66, 0xC3]
    let longerValidUTF8: [UInt8] = [0x21, 0x21, 0x21, 0x21, 0x21, 0x21, 0x21, 0x21, 0x21, 0x21, 0x43, 0x61, 0x66, 0xC3, 0xA9]
    let longerInvalidUTF8: [UInt8] = [0x21, 0x21, 0x43, 0x61, 0x66, 0xC3, 0x43, 0x61, 0x66, 0xC3, 0x43, 0x61, 0x66, 0xC3]
    
    func test(bufferSize: Int, input: [UInt8], expected: String) {
      let strs = (0..<100).map { _ in
        String(unsafeUninitializedCapacity: bufferSize) { buffer in
          if #available(SwiftStdlib 5.10, *) {
            expectEqual(bufferSize, buffer.count)
          }
          _ = buffer.initialize(from: input)
          return input.count
        }
      }
      for str in strs {
        expectEqual(expected, str)
      }
    }
    
    test(bufferSize: validUTF8.count, input: validUTF8, expected: "Café")
    test(bufferSize: invalidUTF8.count, input: invalidUTF8, expected: "Caf�")
    // Force non-smol strings by using a larger capacity
    test(bufferSize: 16, input: validUTF8, expected: "Café")
    test(bufferSize: 16, input: invalidUTF8, expected: "Caf�")
    test(bufferSize: longerValidUTF8.count, input: longerValidUTF8, expected: "!!!!!!!!!!Café")
    test(bufferSize: longerInvalidUTF8.count, input: longerInvalidUTF8, expected: "!!Caf�Caf�Caf�")
    test(bufferSize: 16, input: longerValidUTF8, expected: "!!!!!!!!!!Café")
    test(bufferSize: 16, input: longerInvalidUTF8, expected: "!!Caf�Caf�Caf�")
    let empty = String(unsafeUninitializedCapacity: 16) { _ in
      // Can't initialize the buffer (e.g. the capacity is too small).
      return 0
    }
    expectTrue(empty.isEmpty)
  }
}

if #available(SwiftStdlib 5.3, *) {
  StringCreateTests.test("Small string unsafeUninitializedCapacity") {
    let str1 = "42"
    let str2 = String(42)
    expectEqual(str1, str2)

    let str3 = String(unsafeUninitializedCapacity: 2) {
      $0[0] = UInt8(ascii: "4")
      $0[1] = UInt8(ascii: "2")
      return 2
    }
    expectEqual(str1, str3)

    // Write into uninitialized space.
    let str4 = String(unsafeUninitializedCapacity: 3) {
      $0[0] = UInt8(ascii: "4")
      $0[1] = UInt8(ascii: "2")
      $0[2] = UInt8(ascii: "X")
      // Deinitialize memory used for scratch space before returning.
      ($0.baseAddress! + 2).deinitialize(count: 1)
      return 2
    }
    expectEqual(str1, str4)

    let str5 = String(unsafeUninitializedCapacity: 3) {
      $0[1] = UInt8(ascii: "4")
      $0[2] = UInt8(ascii: "2")
      // Move the initialized UTF-8 code units to the start of the buffer,
      // leaving the non-overlapping source memory uninitialized.
      $0.baseAddress!.moveInitialize(from: $0.baseAddress! + 1, count: 2)
      return 2
    }
    expectEqual(str1, str5)

    // Ensure reasonable behavior despite a deliberate API contract violation.
    let str6 = String(unsafeUninitializedCapacity: 3) {
      $0[0] = UInt8(ascii: "4")
      $0[1] = UInt8(ascii: "2")
      $0[2] = UInt8(ascii: "X")
      return 2
    }
    expectEqual(str1, str6)
  }
}
