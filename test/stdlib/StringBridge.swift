// RUN: %empty-directory(%t)
// RUN: cp %s %t/main.swift
// RUN: %target-build-swift -Xfrontend -disable-access-control -module-name a %t/main.swift %S/../Inputs/SmallStringTestUtilities.swift -o %t.out -O
// RUN: %target-codesign %t.out
// RUN: %target-run %t.out

// REQUIRES: executable_test

// REQUIRES: objc_interop

import Foundation
import StdlibUnittest

var StringBridgeTests = TestSuite("StringBridgeTests")

extension String {
  init(fromCocoa s: String) {
    self = (s as NSString) as String
  }


}

StringBridgeTests.test("Tagged NSString") {
  guard #available(macOS 10.13, iOS 11.0, tvOS 11.0, *) else { return }
#if _pointerBitWidth(_64)
  // Bridge tagged strings as small
  expectSmall((("0123456" as NSString) as String))
  expectSmall((("012345678" as NSString) as String))
  expectSmall((("aaaaaaaaaaa" as NSString) as String))
  expectSmall((("bbbbbbbbb" as NSString) as String))

  // Bridge non-tagged as non-small even if they fit, for fear of losing
  // associated information
  let bigAs = ("aaaaaaaaaaaa" as NSString) as String
  let bigBs = ("bbbbbbbbbb" as NSString) as String
  let bigQs = ("????????" as NSString) as String
  expectCocoa(bigAs)
  expectCocoa(bigBs)
  expectCocoa(bigQs)

#if false // FIXME: Re-enable (https://github.com/apple/swift/issues/50136)
  let littleAsNSString = ("aa" as NSString)
  var littleAs = littleAsNSString as String

  // But become small when appended to
  expectSmall(bigAs + "c")
  expectSmall(bigBs + "c")
  expectSmall("a\(bigAs)")
  expectSmall("a\(bigBs)")
  expectSmall(littleAs + bigQs)
  expectSmall(bigQs + littleAs)
  expectSmall("\(littleAs)bigQs\(littleAs)")
#endif // false

#endif // not 32bit
}

StringBridgeTests.test("Bridging") {
  // Test bridging retains small string form
  func bridge(_ small: _SmallString) -> String {
    return String(_StringGuts(small))._bridgeToObjectiveCImpl() as! String
  }
  func runTestSmall(_ input: String) throws {
    // Constructed through CF
    guard let fromCocoaSmall = _SmallString(
      _cocoaString: input as NSString
    ) else {
        throw "Didn't fit"
    }
    verifySmallString(fromCocoaSmall, input)
    verifySmallString(fromCocoaSmall, bridge(fromCocoaSmall))
  }

  // Pass tests

  #if _pointerBitWidth(_64)
  if #available(macOS 10.15, iOS 13, *) {
    expectDoesNotThrow({ try runTestSmall("abc") })
    expectDoesNotThrow({ try runTestSmall("defghijk") })
    expectDoesNotThrow({ try runTestSmall("aaaaaaaaaaa") })
  } else if #available(macOS 10.10, iOS 9, *) {
    // FIXME: tests temporarily disabled on these OS versions
    // due to failing in CI. rdar://problem/54875979
  } else {
    // OS X 10.9, iOS 7/8 did not have tagged strings
    expectThrows("Didn't fit", { try runTestSmall("abc") })
    expectThrows("Didn't fit", { try runTestSmall("defghijk") })
    expectThrows("Didn't fit", { try runTestSmall("aaaaaaaaaaa") })
  }
  #endif

  // Fail tests
  //
  expectThrows("Didn't fit", { try runTestSmall("\u{0}") })
  expectThrows("Didn't fit", { try runTestSmall("0123456789abcde") })
  expectThrows("Didn't fit", { try runTestSmall("👨‍👦abcd") })
  expectThrows("Didn't fit", { try runTestSmall("👨‍👦") })
  expectThrows("Didn't fit", { try runTestSmall("👨‍👩‍👦") })
  expectThrows("Didn't fit", { try runTestSmall("👨‍👦abcde") })
}

func returnOne<T>(_ t: T) -> Int { return 1 }
StringBridgeTests.test("Character from NSString") {
  guard #available(macOS 10.15, iOS 13, watchOS 6, tvOS 13, *) else { return }

  // NOTE: Using hard-coded literals to directly construct NSStrings
  let ns1 = "A" as NSString
  let ns2 = "A\u{301}" as NSString
  let ns3 = "𓁹͇͈͉͍͎͊͋͌ͧͨͩͪͫͬͭͮ͏̛͓͔͕͖͙͚̗̘̙̜̹̺̻̼͐͑͒͗͛ͣͤͥͦ̽̾̿̀́͂̓̈́͆ͧͨͩͪͫͬͭͮ͘̚͜͟͢͝͞͠͡ͅ" as NSString

  let c1 = Character(ns1 as String)
  let c2 = Character(ns2 as String)
  let c3 = Character(ns3 as String)

  expectEqual("A", String(c1))
  expectNotNil(String(c1).utf8.withContiguousStorageIfAvailable(returnOne))

  expectEqual("A\u{301}", String(c2))
  expectNotNil(String(c2).utf8.withContiguousStorageIfAvailable(returnOne))
  expectNil((ns2 as String).utf8.withContiguousStorageIfAvailable(returnOne))

  expectEqual("𓁹͇͈͉͍͎͊͋͌ͧͨͩͪͫͬͭͮ͏̛͓͔͕͖͙͚̗̘̙̜̹̺̻̼͐͑͒͗͛ͣͤͥͦ̽̾̿̀́͂̓̈́͆ͧͨͩͪͫͬͭͮ͘̚͜͟͢͝͞͠͡ͅ", String(c3))
  expectNotNil(String(c3).utf8.withContiguousStorageIfAvailable(returnOne))
  expectNil((ns3 as String).utf8.withContiguousStorageIfAvailable(returnOne))
}


runAllTests()
