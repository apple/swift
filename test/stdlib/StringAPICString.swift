// RUN: %target-run-simple-swift
// REQUIRES: executable_test

//
// Tests for the non-Foundation CString-oriented API of String
//

import StdlibUnittest

#if os(WASI)
let enableCrashTests = false
#else
let enableCrashTests = true
#endif

var CStringTests = TestSuite("CStringTests")

func getNullUTF8() -> UnsafeMutablePointer<UInt8>? {
  return nil
}

func getASCIIUTF8() -> (UnsafeMutablePointer<UInt8>, dealloc: () -> ()) {
  let up = UnsafeMutablePointer<UInt8>.allocate(capacity: 100)
  up[0] = 0x61
  up[1] = 0x62
  up[2] = 0
  return (up, { up.deallocate() })
}

func getNonASCIIUTF8() -> (UnsafeMutablePointer<UInt8>, dealloc: () -> ()) {
  let up = UnsafeMutablePointer<UInt8>.allocate(capacity: 100)
  up[0] = 0xd0
  up[1] = 0xb0
  up[2] = 0xd0
  up[3] = 0xb1
  up[4] = 0
  return (UnsafeMutablePointer(up), { up.deallocate() })
}

func getIllFormedUTF8String1(
) -> (UnsafeMutablePointer<UInt8>, dealloc: () -> ()) {
  let up = UnsafeMutablePointer<UInt8>.allocate(capacity: 100)
  up[0] = 0x41
  up[1] = 0xed
  up[2] = 0xa0
  up[3] = 0x80
  up[4] = 0x41
  up[5] = 0
  return (UnsafeMutablePointer(up), { up.deallocate() })
}

func getIllFormedUTF8String2(
) -> (UnsafeMutablePointer<UInt8>, dealloc: () -> ()) {
  let up = UnsafeMutablePointer<UInt8>.allocate(capacity: 100)
  up[0] = 0x41
  up[0] = 0x41
  up[1] = 0xed
  up[2] = 0xa0
  up[3] = 0x81
  up[4] = 0x41
  up[5] = 0
  return (UnsafeMutablePointer(up), { up.deallocate() })
}

func asCCharArray(_ a: [UInt8]) -> [CChar] {
  return a.map { CChar(bitPattern: $0) }
}

func getUTF8Length(_ cString: UnsafePointer<UInt8>) -> Int {
  var length = 0
  while cString[length] != 0 {
    length += 1
  }
  return length
}

func bindAsCChar(_ utf8: UnsafePointer<UInt8>) -> UnsafePointer<CChar> {
  return UnsafeRawPointer(utf8).bindMemory(to: CChar.self,
    capacity: getUTF8Length(utf8))
}

func expectEqualCString(_ lhs: UnsafePointer<UInt8>,
  _ rhs: UnsafePointer<UInt8>) {

  var index = 0
  while lhs[index] != 0 {
    expectEqual(lhs[index], rhs[index])
    index += 1
  }
  expectEqual(0, rhs[index])
}

func expectEqualCString(_ lhs: UnsafePointer<UInt8>,
  _ rhs: ContiguousArray<UInt8>) {
  rhs.withUnsafeBufferPointer {
    expectEqualCString(lhs, $0.baseAddress!)
  }
}

func expectEqualCString(_ lhs: UnsafePointer<UInt8>,
  _ rhs: ContiguousArray<CChar>) {
  rhs.withUnsafeBufferPointer {
    $0.baseAddress!.withMemoryRebound(
      to: UInt8.self, capacity: rhs.count) {
      expectEqualCString(lhs, $0)
    }
  }
}

CStringTests.test("String.init(validatingUTF8:)") {
  do {
    let (s, dealloc) = getASCIIUTF8()
    expectEqual("ab", String(validatingUTF8: bindAsCChar(s)))
    dealloc()
  }
  do {
    let (s, dealloc) = getNonASCIIUTF8()
    expectEqual("аб", String(validatingUTF8: bindAsCChar(s)))
    dealloc()
  }
  do {
    let (s, dealloc) = getIllFormedUTF8String1()
    expectNil(String(validatingUTF8: bindAsCChar(s)))
    dealloc()
  }
}

CStringTests.test("String(cString:)") {
  do {
    let (s, dealloc) = getASCIIUTF8()
    let result = String(cString: s)
    expectEqual("ab", result)
    let su = bindAsCChar(s)
    expectEqual("ab", String(cString: su))
    dealloc()
  }
  do {
    let (s, dealloc) = getNonASCIIUTF8()
    let result = String(cString: s)
    expectEqual("аб", result)
    let su = bindAsCChar(s)
    expectEqual("аб", String(cString: su))
    dealloc()
  }
  do {
    let (s, dealloc) = getIllFormedUTF8String1()
    let result = String(cString: s)
    expectEqual("\u{41}\u{fffd}\u{fffd}\u{fffd}\u{41}", result)
    let su = bindAsCChar(s)
    expectEqual("\u{41}\u{fffd}\u{fffd}\u{fffd}\u{41}", String(cString: su))
    dealloc()
  }
}

CStringTests.test("String.decodeCString") {
  do {
    let s = getNullUTF8()
    let result = String.decodeCString(s, as: UTF8.self)
    expectNil(result)
  }
  do { // repairing
    let (s, dealloc) = getIllFormedUTF8String1()
    if let (result, repairsMade) = String.decodeCString(
      s, as: UTF8.self, repairingInvalidCodeUnits: true) {
      expectEqual("\u{41}\u{fffd}\u{fffd}\u{fffd}\u{41}", result)
      expectTrue(repairsMade)
    } else {
      expectUnreachable("Expected .some()")
    }
    dealloc()
  }
  do { // non repairing
    let (s, dealloc) = getIllFormedUTF8String1()
    let result = String.decodeCString(
      s, as: UTF8.self, repairingInvalidCodeUnits: false)
    expectNil(result)
    dealloc()
  }
}

CStringTests.test("String.utf8CString") {
  do {
    let (cstr, dealloc) = getASCIIUTF8()
    defer { dealloc() }
    let str = String(cString: cstr)
    expectEqualCString(cstr, str.utf8CString)
  }
  do {
    let (cstr, dealloc) = getNonASCIIUTF8()
    defer { dealloc() }
    let str = String(cString: cstr)
    expectEqualCString(cstr, str.utf8CString)
  }
}

CStringTests.test("String.withCString") {
  do {
    let (cstr, dealloc) = getASCIIUTF8()
    defer { dealloc() }
    let str = String(cString: cstr)
    str.withCString {
      expectEqual(str, String(cString: $0))
    }
  }
  do {
    let (cstr, dealloc) = getASCIIUTF8()
    defer { dealloc() }
    let str = String(cString: cstr)
    str.withCString {
      expectEqual(str, String(cString: $0))
    }
  }
}

CStringTests.test("Substring.withCString") {
  do {
    let (cstr, dealloc) = getASCIIUTF8()
    defer { dealloc() }
    let str = String(cString: cstr).dropFirst()
    str.withCString {
      expectEqual(str, String(cString: $0))
    }
  }
  do {
    let (cstr, dealloc) = getASCIIUTF8()
    defer { dealloc() }
    let str = String(cString: cstr).dropFirst()
    str.withCString {
      expectEqual(str, String(cString: $0))
    }
  }
}

CStringTests.test("String.cString.with.Array.UInt8.input") {
  guard #available(SwiftStdlib 5.7, *) else { return }
  do {
    let (u8p, dealloc) = getASCIIUTF8()
    defer { dealloc() }
    let cstr = UnsafePointer(u8p)
    let buffer = UnsafeBufferPointer(start: cstr, count: getUTF8Length(u8p)+1)
    let str = String(cString: Array(buffer))
    str.withCString {
      $0.withMemoryRebound(to: UInt8.self, capacity: buffer.count) {
        expectEqualCString(u8p, $0)
      }
    }
  }
  guard enableCrashTests else { return }
  // no need to test every case; that is covered in other tests
  expectCrashLater(
    // Workaround for https://github.com/apple/swift/issues/58362 (rdar://91365967)
    // withMessage: "input of String.init(cString:) must be null-terminated"
  )
  _ = String(cString: [] as [UInt8])
  expectUnreachable()
}

CStringTests.test("String.cString.with.Array.CChar.input") {
  guard #available(SwiftStdlib 5.7, *) else { return }
  do {
    let (u8p, dealloc) = getASCIIUTF8()
    defer { dealloc() }
    let buffer = UnsafeBufferPointer(start: u8p, count: getUTF8Length(u8p)+1)
    let str = buffer.withMemoryRebound(to: CChar.self) {
      String(cString: Array($0))
    }
    str.withCString {
      $0.withMemoryRebound(to: UInt8.self, capacity: buffer.count) {
        expectEqualCString(u8p, $0)
      }
    }
  }
  guard enableCrashTests else { return }
  // no need to test every case; that is covered in other tests
  expectCrashLater(
    // Workaround for https://github.com/apple/swift/issues/58362 (rdar://91365967)
    // withMessage: "input of String.init(cString:) must be null-terminated"
  )
  _ = String(cString: [] as [CChar])
  expectUnreachable()
}

CStringTests.test("String.cString.with.String.input") {
  guard #available(SwiftStdlib 5.7, *) else { return }
  let (u8p, dealloc) = getASCIIUTF8()
  defer { dealloc() }
  var str = String(cString: "ab")
  str.withCString {
    $0.withMemoryRebound(to: UInt8.self, capacity: getUTF8Length(u8p)+1) {
      expectEqualCString(u8p, $0)
    }
  }
  str = String(cString: "")
  expectTrue(str.isEmpty)
}

CStringTests.test("String.cString.with.inout.UInt8.conversion") {
  guard #available(SwiftStdlib 5.7, *) else { return }
  var c = UInt8.zero
  var str = String(cString: &c)
  expectTrue(str.isEmpty)
  c = 100
  guard enableCrashTests else { return }
  expectCrashLater(
    // Workaround for https://github.com/apple/swift/issues/58362 (rdar://91365967)
    // withMessage: "input of String.init(cString:) must be null-terminated"
  )
  str = String(cString: &c)
  expectUnreachable()
}

CStringTests.test("String.cString.with.inout.CChar.conversion") {
  guard #available(SwiftStdlib 5.7, *) else { return }
  var c = CChar.zero
  var str = String(cString: &c)
  expectTrue(str.isEmpty)
  c = 100
  guard enableCrashTests else { return }
  expectCrashLater(
    // Workaround for https://github.com/apple/swift/issues/58362 (rdar://91365967)
    // withMessage: "input of String.init(cString:) must be null-terminated"
  )
  str = String(cString: &c)
  expectUnreachable()
}

CStringTests.test("String.validatingUTF8.with.Array.input") {
  guard #available(SwiftStdlib 5.7, *) else { return }
  do {
    let (u8p, dealloc) = getASCIIUTF8()
    defer { dealloc() }
    let buffer = UnsafeBufferPointer(start: u8p, count: getUTF8Length(u8p)+1)
    let str = buffer.withMemoryRebound(to: CChar.self) {
      String(validatingUTF8: Array($0))
    }
    expectNotNil(str)
    str?.withCString {
      $0.withMemoryRebound(to: UInt8.self, capacity: buffer.count) {
        expectEqualCString(u8p, $0)
      }
    }
  }
  guard enableCrashTests else { return }
  // no need to test every case; that is covered in other tests
  expectCrashLater(
    // Workaround for https://github.com/apple/swift/issues/58362 (rdar://91365967)
    // withMessage: "input of String.init(validatingUTF8:) must be null-terminated"
  )
  _ = String(validatingUTF8: [])
  expectUnreachable()
}

CStringTests.test("String.validatingUTF8.with.String.input") {
  guard #available(SwiftStdlib 5.7, *) else { return }
  let (u8p, dealloc) = getASCIIUTF8()
  defer { dealloc() }
  var str = String(validatingUTF8: "ab")
  expectNotNil(str)
  str?.withCString {
    $0.withMemoryRebound(to: UInt8.self, capacity: getUTF8Length(u8p)+1) {
      expectEqualCString(u8p, $0)
    }
  }
  str = String(validatingUTF8: "")
  expectNotNil(str)
  expectEqual(str?.isEmpty, true)
}

CStringTests.test("String.validatingUTF8.with.inout.conversion") {
  guard #available(SwiftStdlib 5.7, *) else { return }
  var c = CChar.zero
  var str = String(validatingUTF8: &c)
  expectNotNil(str)
  expectEqual(str?.isEmpty, true)
  c = 100
  guard enableCrashTests else { return }
  expectCrashLater(
    // Workaround for https://github.com/apple/swift/issues/58362 (rdar://91365967)
    // withMessage: "input of String.init(validatingUTF8:) must be null-terminated"
  )
  str = String(validatingUTF8: &c)
  expectUnreachable()
}

CStringTests.test("String.decodeCString.with.Array.input") {
  guard #available(SwiftStdlib 5.7, *) else { return }
  do {
    let (u8p, dealloc) = getASCIIUTF8()
    defer { dealloc() }
    let buffer = UnsafeBufferPointer(start: u8p, count: getUTF8Length(u8p)+1)
    let result = buffer.withMemoryRebound(to: Unicode.UTF8.CodeUnit.self) {
      String.decodeCString(Array($0), as: Unicode.UTF8.self)
    }
    expectNotNil(result)
    expectEqual(result?.repairsMade, false)
    result?.result.withCString {
      $0.withMemoryRebound(to: UInt8.self, capacity: buffer.count) {
        expectEqualCString(u8p, $0)
      }
    }
  }
  guard enableCrashTests else { return }
  // no need to test every case; that is covered in other tests
  expectCrashLater(
    // Workaround for https://github.com/apple/swift/issues/58362 (rdar://91365967)
    // withMessage: "input of decodeCString(_:as:repairingInvalidCodeUnits:) must be null-terminated"
  )
  _ = String.decodeCString([], as: Unicode.UTF8.self)
  expectUnreachable()
}

CStringTests.test("String.decodeCString.with.String.input") {
  guard #available(SwiftStdlib 5.7, *) else { return }
  let (u8p, dealloc) = getASCIIUTF8()
  defer { dealloc() }
  var result = String.decodeCString(
    "ab", as: Unicode.UTF8.self, repairingInvalidCodeUnits: true
  )
  expectNotNil(result)
  expectEqual(result?.repairsMade, false)
  result?.result.withCString {
    $0.withMemoryRebound(to: UInt8.self, capacity: getUTF8Length(u8p)+1) {
      expectEqualCString(u8p, $0)
    }
  }
  result = String.decodeCString("", as: Unicode.UTF8.self)
  expectNotNil(result)
  expectEqual(result?.repairsMade, false)
  expectEqual(result?.result.isEmpty, true)
}

CStringTests.test("String.decodeCString.with.inout.conversion") {
  guard #available(SwiftStdlib 5.7, *) else { return }
  var c = Unicode.UTF8.CodeUnit.zero
  var result = String.decodeCString(
    &c, as: Unicode.UTF8.self, repairingInvalidCodeUnits: true
  )
  expectNotNil(result)
  expectEqual(result?.result.isEmpty, true)
  expectEqual(result?.repairsMade, false)
  c = 100
  guard enableCrashTests else { return }
  expectCrashLater(
    // Workaround for https://github.com/apple/swift/issues/58362 (rdar://91365967)
    // withMessage: "input of decodeCString(_:as:repairingInvalidCodeUnits:) must be null-terminated"
  )
  result = String.decodeCString(&c, as: Unicode.UTF8.self)
  expectUnreachable()
}

CStringTests.test("String.init.decodingCString.with.Array.input") {
  guard #available(SwiftStdlib 5.7, *) else { return }
  do {
    let (u8p, dealloc) = getASCIIUTF8()
    defer { dealloc() }
    let buffer = UnsafeBufferPointer(start: u8p, count: getUTF8Length(u8p)+1)
    let str = buffer.withMemoryRebound(to: Unicode.UTF8.CodeUnit.self) {
      String(decodingCString: Array($0), as: Unicode.UTF8.self)
    }
    str.withCString {
      $0.withMemoryRebound(to: UInt8.self, capacity: buffer.count) {
        expectEqualCString(u8p, $0)
      }
    }
  }
  guard enableCrashTests else { return }
  // no need to test every case; that is covered in other tests
  expectCrashLater(
    // Workaround for https://github.com/apple/swift/issues/58362 (rdar://91365967)
    // withMessage: "input of decodeCString(_:as:repairingInvalidCodeUnits:) must be null-terminated"
  )
  _ = String(decodingCString: [], as: Unicode.UTF8.self)
  expectUnreachable()
}

CStringTests.test("String.init.decodingCString.with.String.input") {
  guard #available(SwiftStdlib 5.7, *) else { return }
  let (u8p, dealloc) = getASCIIUTF8()
  defer { dealloc() }
  var str = String(decodingCString: "ab", as: Unicode.UTF8.self)
  str.withCString {
    $0.withMemoryRebound(to: UInt8.self, capacity: getUTF8Length(u8p)+1) {
      expectEqualCString(u8p, $0)
    }
  }
  str = String(decodingCString: "", as: Unicode.UTF8.self)
  expectTrue(str.isEmpty)
}

CStringTests.test("String.init.decodingCString.with.inout.conversion") {
  guard #available(SwiftStdlib 5.7, *) else { return }
  var c = Unicode.UTF8.CodeUnit.zero
  var str = String(decodingCString: &c, as: Unicode.UTF8.self)
  expectEqual(str.isEmpty, true)
  c = 100
  guard enableCrashTests else { return }
  expectCrashLater(
    // Workaround for https://github.com/apple/swift/issues/58362 (rdar://91365967)
    // withMessage: "input of String.init(decodingCString:as:) must be null-terminated"
  )
  str = String(decodingCString: &c, as: Unicode.UTF8.self)
  expectUnreachable()
}

runAllTests()

