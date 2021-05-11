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
    var p1 = UnsafeMutableRawPointer.allocate(
      byteCount: sizeInBytes, alignment: MemoryLayout<Missile>.alignment)
    defer {
      p1.deallocate()
    }
    var ptrM = p1.initializeMemory(as: Missile.self, repeating: Missile(1), count: 1)
    (p1 + MemoryLayout<Missile>.stride).initializeMemory(as: Missile.self, repeating: Missile(2), count: 2)
    expectEqual(1, ptrM[0].number)
    expectEqual(2, ptrM[1].number)
    expectEqual(2, ptrM[2].number)

    var p2 = UnsafeMutableRawPointer.allocate(
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

UnsafeMutableRawPointerExtraTestSuite.test("bindMemory") {
  let sizeInBytes = 3 * MemoryLayout<Int>.stride
  var p1 = UnsafeMutableRawPointer.allocate(
    byteCount: sizeInBytes, alignment: MemoryLayout<Int>.alignment)
  defer {
    p1.deallocate()
  }
  let ptrI = p1.bindMemory(to: Int.self, capacity: 3)
  let bufI = UnsafeMutableBufferPointer(start: ptrI, count: 3)
  bufI.initialize(from: 1...3)
  let ptrU = p1.bindMemory(to: UInt.self, capacity: 3)
  expectEqual(1, ptrU[0])
  expectEqual(2, ptrU[1])
  expectEqual(3, ptrU[2])
  let ptrU2 = p1.assumingMemoryBound(to: UInt.self)
  expectEqual(1, ptrU[0])
}

UnsafeMutableRawPointerExtraTestSuite.test("load/store") {
  let sizeInBytes = 3 * MemoryLayout<Int>.stride
  var p1 = UnsafeMutableRawPointer.allocate(
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

UnsafeMutableRawPointerExtraTestSuite.test("copyMemory") {
  let sizeInBytes = 4 * MemoryLayout<Int>.stride
  var rawPtr = UnsafeMutableRawPointer.allocate(
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

runAllTests()
