// RUN: %target-run-simple-swiftgyb
// REQUIRES: executable_test

import StdlibUnittest

var UnsafeMutableRawPointerExtraTestSuite =
  TestSuite("UnsafeMutableRawPointerExtra")

class Missile {
  static var missilesLaunched = 0
  let number: Int
  init(_ number: Int) { self.number = number }
  deinit { Missile.missilesLaunched += 1 }
}

UnsafeMutableRawPointerExtraTestSuite.test("initializeMemory") {
  Missile.missilesLaunched = 0
  do {
    let sizeInBytes = 3 * MemoryLayout<Missile>.stride
    let p1 = UnsafeMutableRawPointer.allocate(
      byteCount: sizeInBytes, alignment: MemoryLayout<Missile>.alignment)
    defer {
      p1.deallocate()
    }
    var ptrM = p1.initializeMemory(as: Missile.self, repeating: Missile(1), count: 1)
    (p1 + MemoryLayout<Missile>.stride).initializeMemory(as: Missile.self, repeating: Missile(2), count: 2)
    expectEqual(1, ptrM[0].number)
    expectEqual(2, ptrM[1].number)
    expectEqual(2, ptrM[2].number)

    let p2 = UnsafeMutableRawPointer.allocate(
      byteCount: sizeInBytes, alignment: MemoryLayout<Missile>.alignment)
    defer {
      p2.deallocate()
    }
    let ptrM2 = p2.moveInitializeMemory(as: Missile.self, from: ptrM, count: 3)
    defer {
      ptrM2.deinitialize(count: 3)
    }
    // p1 is now deinitialized.
    expectEqual(1, ptrM2[0].number)
    expectEqual(2, ptrM2[1].number)
    expectEqual(2, ptrM2[2].number)

    ptrM = p1.initializeMemory(
      as: Missile.self, from: (1...3).map(Missile.init))
    defer {
      ptrM.deinitialize(count: 3)
    }
    expectEqual(1, ptrM[0].number)
    expectEqual(2, ptrM[1].number)
    expectEqual(3, ptrM[2].number)
  }
  expectEqual(5, Missile.missilesLaunched)
}

UnsafeMutableRawPointerExtraTestSuite.test("initializeMemorySingleElement")
.skip(.custom({
   if #available(SwiftStdlib 5.8, *) { return false } else { return true }
 }, reason: "Requires standard library from Swift 5.8"))
.code {
  Missile.missilesLaunched = 0
  let p1 = UnsafeMutableRawPointer.allocate(
    byteCount: MemoryLayout<Missile>.stride, alignment: MemoryLayout<Missile>.alignment
  )
  defer { p1.deallocate() }
  var p2 = p1.initializeMemory(as: Missile.self, to: Missile(1))
  expectEqual(1, p2.pointee.number)
  expectEqual(p1, p2)
  p2.deinitialize()
  expectEqual(Missile.missilesLaunched, 1)
}

UnsafeMutableRawPointerExtraTestSuite.test("bindMemory") {
  let sizeInBytes = 3 * MemoryLayout<Int>.stride
  let p1 = UnsafeMutableRawPointer.allocate(
    byteCount: sizeInBytes, alignment: MemoryLayout<Int>.alignment)
  defer {
    p1.deallocate()
  }
  let ptrI = p1.bindMemory(to: Int.self, capacity: 3)
  let bufI = UnsafeMutableBufferPointer(start: ptrI, count: 3)
  _ = bufI.initialize(from: 1...3)
  let ptrU = p1.bindMemory(to: UInt.self, capacity: 3)
  expectEqual(1, ptrU[0])
  expectEqual(2, ptrU[1])
  expectEqual(3, ptrU[2])
  let ptrU2 = p1.assumingMemoryBound(to: UInt.self)
  expectEqual(1, ptrU2[0])
}

UnsafeMutableRawPointerExtraTestSuite.test("load/store") {
  let sizeInBytes = 3 * MemoryLayout<Int>.stride
  let p1 = UnsafeMutableRawPointer.allocate(
    byteCount: sizeInBytes, alignment: MemoryLayout<Int>.alignment)
  defer {
    p1.deallocate()
  }
  let ptrI = p1.initializeMemory(as: Int.self, from: 1...3)
  defer {
    ptrI.deinitialize(count: 3)
  }
  expectEqual(1, p1.load(as: Int.self))
  expectEqual(2, p1.load(fromByteOffset: MemoryLayout<Int>.stride, as: Int.self))
  expectEqual(3, p1.load(fromByteOffset: 2*MemoryLayout<Int>.stride, as: Int.self))
  p1.storeBytes(of: 4, as: Int.self)
  p1.storeBytes(of: 5, toByteOffset: MemoryLayout<Int>.stride, as: Int.self)
  p1.storeBytes(of: 6, toByteOffset: 2 * MemoryLayout<Int>.stride, as: Int.self)
  expectEqual(4, p1.load(as: Int.self))
  expectEqual(5, p1.load(fromByteOffset: MemoryLayout<Int>.stride, as: Int.self))
  expectEqual(6, p1.load(fromByteOffset: 2 * MemoryLayout<Int>.stride, as: Int.self))
}

UnsafeMutableRawPointerExtraTestSuite.test("load.unaligned")
.skip(.custom({
  if #available(SwiftStdlib 5.7, *) { return false }
  return true
}, reason: "Requires Swift 5.7's stdlib"))
.code {
  guard #available(SwiftStdlib 5.7, *) else { return }
  var data: [UInt8] = [0x0, 0x0, 0x0, 0xff, 0xff, 0xff, 0xff, 0x0]
  var result: UInt32
  result = data.withUnsafeBytes {
    $0.loadUnaligned(fromByteOffset: 3, as: UInt32.self)
  }
  expectEqual(result, 0xffff_ffff)
  result = data.withUnsafeMutableBytes {
    $0[0] = 0
    return $0.loadUnaligned(fromByteOffset: 1, as: UInt32.self)
  }
  expectEqual(result, 0xffff_0000)
}

#if !os(WASI)
UnsafeMutableRawPointerExtraTestSuite.test("load.invalid")
.skip(.custom({ !_isDebugAssertConfiguration() },
              reason: "This tests a debug precondition.."))
.code {
  let data: [UInt8] = [0x0, 0x0, 0x0, 0xff, 0xff, 0xff, 0xff, 0x0]
  expectCrashLater()
  _ = data.withUnsafeBytes {
    $0.load(fromByteOffset: 3, as: UInt32.self)
  }
  expectUnreachable()
}

UnsafeMutableRawPointerExtraTestSuite.test("load.invalid.mutable")
.skip(.custom({ !_isDebugAssertConfiguration() },
              reason: "This tests a debug precondition.."))
.code {
  var data: [UInt8] = [0x0, 0x0, 0x0, 0xff, 0xff, 0xff, 0xff, 0x0]
  expectCrashLater()
  _ = data.withUnsafeMutableBytes {
    $0.load(fromByteOffset: 1, as: UInt32.self)
  }
  expectUnreachable()
}
#endif

UnsafeMutableRawPointerExtraTestSuite.test("store.unaligned")
.skip(.custom({
  if #available(SwiftStdlib 5.7, *) { return false }
  return true
}, reason: "Requires Swift 5.7's stdlib"))
.code {
  let count = MemoryLayout<UInt>.stride * 2
  let p1 = UnsafeMutableRawPointer.allocate(
    byteCount: count,
    alignment: MemoryLayout<UInt>.alignment
  )
  defer { p1.deallocate() }
  Array(repeating: UInt8.zero, count: count).withUnsafeBufferPointer {
    p1.copyBytes(from: UnsafeRawPointer($0.baseAddress!), count: $0.count)
  }
  let value = UInt.max
  let offset = 3
  p1.storeBytes(of: value, toByteOffset: offset, as: UInt.self)
  expectEqual(p1.load(fromByteOffset: offset-1, as: UInt8.self),
              0)
  expectEqual(p1.load(fromByteOffset: offset, as: UInt8.self),
              .max)
  let storedLength = MemoryLayout<UInt>.size
  expectEqual(p1.load(fromByteOffset: offset-1+storedLength, as: UInt8.self),
              .max)
  expectEqual(p1.load(fromByteOffset: offset+storedLength, as: UInt8.self),
              0)
}

#if !os(WASI)
UnsafeMutableRawPointerExtraTestSuite.test("store.invalid")
.skip(.custom({ !_isDebugAssertConfiguration() },
              reason: "This tests a debug precondition.."))
.skip(.custom({
  if #available(SwiftStdlib 5.7, *) { return false }
  return true
}, reason: "Requires Swift 5.7's stdlib"))
.code {
  Missile.missilesLaunched = 0
  let m = Missile(0)
  let p1 = UnsafeMutableRawPointer.allocate(
    byteCount: MemoryLayout<Missile>.size,
    alignment: MemoryLayout<Missile>.alignment
  )
  defer { p1.deallocate() }
  expectCrashLater()
  p1.storeBytes(of: m, as: Missile.self)
  expectUnreachable()
}
#endif

UnsafeMutableRawPointerExtraTestSuite.test("copyMemory") {
  let sizeInBytes = 4 * MemoryLayout<Int>.stride
  let rawPtr = UnsafeMutableRawPointer.allocate(
    byteCount: sizeInBytes, alignment: MemoryLayout<Int>.alignment)
  defer {
    rawPtr.deallocate()
  }
  let ptrI = rawPtr.initializeMemory(as: Int.self, repeating: 42, count: 4)
  defer {
    ptrI.deinitialize(count: 4)
  }
  let roPtr = UnsafeRawPointer(rawPtr)
  // Right overlap
  ptrI[0] = 1
  ptrI[1] = 2
  (rawPtr + MemoryLayout<Int>.stride).copyMemory(
    from: roPtr, byteCount: 2 * MemoryLayout<Int>.stride)
  expectEqual(1, ptrI[1])
  expectEqual(2, ptrI[2])

  // Left overlap
  ptrI[1] = 2
  ptrI[2] = 3
  rawPtr.copyMemory(
    from: roPtr + MemoryLayout<Int>.stride, byteCount: 2 * MemoryLayout<Int>.stride)
  expectEqual(2, ptrI[0])
  expectEqual(3, ptrI[1])

  // Disjoint:
  ptrI[2] = 2
  ptrI[3] = 3
  rawPtr.copyMemory(
    from: roPtr + 2 * MemoryLayout<Int>.stride, byteCount: 2 * MemoryLayout<Int>.stride)
  expectEqual(2, ptrI[0])
  expectEqual(3, ptrI[1])

  // Backwards
  ptrI[0] = 0
  ptrI[1] = 1
  (rawPtr + 2 * MemoryLayout<Int>.stride).copyMemory(
    from: roPtr, byteCount: 2 * MemoryLayout<Int>.stride)
  expectEqual(0, ptrI[2])
  expectEqual(1, ptrI[3])
}

// --------------------------------------------
// Test bulk initialization from UnsafePointer.

enum Check {
  case LeftOverlap
  case RightOverlap
  case Disjoint
}

func checkRawPointerCorrectness(_ check: Check,
  _ f: (UnsafeMutableRawPointer) -> (_ as: Int.Type, _ from: UnsafeMutablePointer<Int>, _ count: Int) -> UnsafeMutablePointer<Int>) {
  let ptr = UnsafeMutablePointer<Int>.allocate(capacity: 4)
  switch check {
  case .RightOverlap:
    ptr.initialize(to: 1)
    (ptr + 1).initialize(to: 2)
    _ = f(UnsafeMutableRawPointer(ptr + 1))(Int.self, ptr, 2)
    expectEqual(1, ptr[1])
    expectEqual(2, ptr[2])
  case .LeftOverlap:
    (ptr + 1).initialize(to: 2)
    (ptr + 2).initialize(to: 3)
    _ = f(UnsafeMutableRawPointer(ptr))(Int.self, ptr + 1, 2)
    expectEqual(2, ptr[0])
    expectEqual(3, ptr[1])
  case .Disjoint:
    (ptr + 2).initialize(to: 2)
    (ptr + 3).initialize(to: 3)
    _ = f(UnsafeMutableRawPointer(ptr))(Int.self, ptr + 2, 2)
    expectEqual(2, ptr[0])
    expectEqual(3, ptr[1])
    // backwards
    let ptr2 = UnsafeMutablePointer<Int>.allocate(capacity: 4)
    ptr2.initialize(to: 0)
    (ptr2 + 1).initialize(to: 1)
    _ = f(UnsafeMutableRawPointer(ptr2 + 2))(Int.self, ptr2, 2)
    expectEqual(0, ptr2[2])
    expectEqual(1, ptr2[3])
  }
}

func checkPtr(
  _ f: @escaping ((UnsafeMutableRawPointer)
    -> (_ as: Int.Type, _ from: UnsafeMutablePointer<Int>, _ count: Int)
    -> UnsafeMutablePointer<Int>)
) -> (Check) -> Void {
  return { checkRawPointerCorrectness($0, f) }
}

func checkPtr(
  _ f: @escaping ((UnsafeMutableRawPointer)
    -> (_ as: Int.Type, _ from: UnsafePointer<Int>, _ count: Int)
    -> UnsafeMutablePointer<Int>)
) -> (Check) -> Void {
  return {
    checkRawPointerCorrectness($0) { destPtr in 
      return { f(destPtr)($0, UnsafeMutablePointer($1), $2) }
    }
  }
}

#if !os(WASI)
// Trap tests aren't available on WASI.
UnsafeMutableRawPointerExtraTestSuite.test("initializeMemory:as:from:count:") {
  let check = checkPtr(UnsafeMutableRawPointer.initializeMemory(as:from:count:))
  check(Check.Disjoint)
  // This check relies on _debugPrecondition() so will only trigger in
  // -Onone mode.
  if _isDebugAssertConfiguration() {
    expectCrashLater()
    check(Check.LeftOverlap)
  }
}

UnsafeMutableRawPointerExtraTestSuite.test("initializeMemory:as:from:count:.Right") {
  let check = checkPtr(UnsafeMutableRawPointer.initializeMemory(as:from:count:))
  // This check relies on _debugPrecondition() so will only trigger in
  // -Onone mode.
  if _isDebugAssertConfiguration() {
    expectCrashLater()
    check(Check.RightOverlap)
  }
}
#endif

UnsafeMutableRawPointerExtraTestSuite.test("moveInitialize:from:") {
  let check =
    checkPtr(UnsafeMutableRawPointer.moveInitializeMemory(as:from:count:))
  check(Check.LeftOverlap)
  check(Check.Disjoint)
  check(Check.RightOverlap)
}

UnsafeMutableRawPointerExtraTestSuite.test("withMemoryRebound") {
  // test withMemoryRebound behaviour, post SE-0333.
  let allocated = UnsafeMutableRawPointer.allocate(byteCount: 32,  alignment: 8)
  defer { allocated.deallocate() }
  allocated.withMemoryRebound(to: Int.self, capacity: 4) {
    // Make sure the closure argument is a `UnsafeMutablePointer<Int>`
    let ptrT: UnsafeMutablePointer<Int> = $0
    // and that the pointee type is `Int`.
    expectType(Int.self, &ptrT.pointee)
  }
  let nonmutable = UnsafeRawPointer(allocated)
  nonmutable.withMemoryRebound(to: UInt.self, capacity: 4) {
    // Make sure the closure argument is a `UnsafePointer<UInt>`
    let ptrT: UnsafePointer<UInt> = $0
    // and that the element type is `Int`.
    let mutablePtrT = UnsafeMutablePointer(mutating: ptrT)
    expectType(UInt.self, &mutablePtrT.pointee)
  }
}

UnsafeMutableRawPointerExtraTestSuite.test("realignment-functions") {
  var m = UnsafeMutableRawPointer(bitPattern: 1)!
  expectEqual(m.alignedUp(for: Int64.self), .init(bitPattern: 8)!)
  expectEqual(m.alignedUp(for: Int32.self), .init(bitPattern: 4)!)
  expectEqual(m.alignedUp(toMultipleOf: 8), .init(bitPattern: 8)!)
  expectEqual(m.alignedUp(toMultipleOf: 16), .init(bitPattern: 16)!)

  m = .init(bitPattern: 13)!
  expectEqual(m.alignedDown(for: Int64.self), .init(bitPattern: 8)!)
  expectEqual(m.alignedDown(for: Int32.self), .init(bitPattern: 12)!)
  expectEqual(m.alignedDown(toMultipleOf: 8), .init(bitPattern: 8)!)
  expectEqual(m.alignedDown(toMultipleOf: 4), .init(bitPattern: 12)!)

  var p = UnsafeRawPointer(bitPattern: 1)!
  expectEqual(p.alignedUp(for: Int64.self), .init(bitPattern: 8)!)
  expectEqual(p.alignedUp(for: Int32.self), .init(bitPattern: 4)!)
  expectEqual(p.alignedUp(toMultipleOf: 8), .init(bitPattern: 8)!)
  expectEqual(p.alignedUp(toMultipleOf: 16), .init(bitPattern: 16)!)

  p = .init(bitPattern: 13)!
  expectEqual(p.alignedDown(for: Int64.self), .init(bitPattern: 8)!)
  expectEqual(p.alignedDown(for: Int32.self), .init(bitPattern: 12)!)
  expectEqual(p.alignedDown(toMultipleOf: 8), .init(bitPattern: 8)!)
  expectEqual(p.alignedDown(toMultipleOf: 4), .init(bitPattern: 12)!)
}

UnsafeMutableRawPointerExtraTestSuite.test("pointer-comparisons") {
  let a = UnsafeMutableRawPointer(bitPattern: 0x8000)!
  let b = UnsafeRawPointer(bitPattern: 0x9000)!

  expectTrue(a.assumingMemoryBound(to: Int.self) == UnsafeRawPointer(a))
  expectTrue(b.assumingMemoryBound(to: UInt.self) >= b)
  expectTrue(a <= a.assumingMemoryBound(to: Double.self))

  expectTrue(a.assumingMemoryBound(to: Int.self) != b)
  expectTrue(b.assumingMemoryBound(to: UInt.self) > UnsafeMutableRawPointer(a))
  expectTrue(a < b.assumingMemoryBound(to: Double.self))
}

runAllTests()
