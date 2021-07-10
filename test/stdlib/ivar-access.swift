// RUN: %target-run-simple-swift
// REQUIRES: executable_test

import StdlibUnittest

let suite = TestSuite("ivar-access")
defer { runAllTests() }

class Foo {
  var value: Int = 100
}

struct S: Equatable {
  var simpleIvar: Int = 1
  var tupleIvar: (Int, Int) = (2, 3)
  var labeledTupleIvar: (x: Int, y: Int) = (4, 5)
  fileprivate(set) var secretlyMutableIvar: Int = 6

  var strongReferenceIvar = Foo()
  weak var weakReferenceIvar: C? = nil

  var closureIvar: () -> Int = { 7 }
  var cFunctionIvar: @convention(c) () -> Int = { 8 }
  var computedProperty: Int {
    get { 9 }
    set { print("Nope") }
  }
  var storedPropertyWithDidSet: Int = 10 {
    didSet { print("It changed") }
  }
  var storedPropertyWithWillSet: Int = 11 {
    willSet { print("It will change") }
  }
  var computedPropertyWithGeneralizedAccessors: Int {
    _read {
      yield 12
    }
    _modify {
      var value = 12
      yield &value
    }
  }

  static func ==(left: S, right: S) -> Bool {
    guard left.simpleIvar == right.simpleIvar else { return false }
    guard left.tupleIvar == right.tupleIvar else { return false }
    guard left.labeledTupleIvar == right.labeledTupleIvar else { return false }
    guard left.secretlyMutableIvar == right.secretlyMutableIvar else { return false }
    guard left.strongReferenceIvar === right.strongReferenceIvar else { return false }
    guard left.weakReferenceIvar === right.weakReferenceIvar else { return false }
    return true
  }
}

class C {
  final var simpleIvar: Int = 1
  final var tupleIvar: (Int, Int) = (2, 3)
  final var labeledTupleIvar: (x: Int, y: Int) = (4, 5)
  final fileprivate(set) var secretlyMutableIvar: Int = 6

  final var strongReferenceIvar = Foo()
  final weak var weakReferenceIvar: C? = nil

  final var structIvar = S()

  final var closureIvar: () -> Int = { 7 }
  final var cFunctionIvar: @convention(c) () -> Int = { 8 }
  final var computedProperty: Int {
    get { 9 }
    set { print("Nope") }
  }
  final var storedPropertyWithDidSet: Int = 10 {
    didSet { print("It changed") }
  }
  final var storedPropertyWithWillSet: Int = 11 {
    willSet { print("It will change") }
  }
  final var computedPropertyWithGeneralizedAccessors: Int {
    _read {
      yield 12
    }
    _modify {
      var value = 12
      yield &value
    }
  }
  var nonFinalIvar: Int = 13
}

suite.test("simpleIvar") {
  guard #available(macOS 9999, iOS 9999, watchOS 9999, tvOS 9999, *) else { return }
  let ref = C()
  defer { withExtendedLifetime(ref) {} }

  let p = MemoryLayout<C>.unsafeAddress(of: \.simpleIvar, in: ref)
  expectNotNil(p)
  expectEqual(p?.pointee, 1)
}

suite.test("tupleIvar") {
  guard #available(macOS 9999, iOS 9999, watchOS 9999, tvOS 9999, *) else { return }
  let ref = C()
  defer { withExtendedLifetime(ref) {} }

  let p = MemoryLayout<C>.unsafeAddress(of: \.tupleIvar, in: ref)
  expectNotNil(p)
  expectEqual(p?.pointee.0, 2)
  expectEqual(p?.pointee.1, 3)

  let p0 = MemoryLayout<C>.unsafeAddress(of: \.tupleIvar.0, in: ref)
  expectNotNil(p0)
  expectEqual(p0?.pointee, 2)

  let p1 = MemoryLayout<C>.unsafeAddress(of: \.tupleIvar.1, in: ref)
  expectNotNil(p1)
  expectEqual(p1?.pointee, 3)
}

suite.test("labeledTupleIvar") {
  guard #available(macOS 9999, iOS 9999, watchOS 9999, tvOS 9999, *) else { return }
  let ref = C()
  defer { withExtendedLifetime(ref) {} }

  let p = MemoryLayout<C>.unsafeAddress(of: \.labeledTupleIvar, in: ref)
  expectNotNil(p)
  expectEqual(p?.pointee.x, 4)
  expectEqual(p?.pointee.y, 5)

  let px = MemoryLayout<C>.unsafeAddress(of: \.labeledTupleIvar.x, in: ref)
  expectNotNil(px)
  expectEqual(px?.pointee, 4)

  let py = MemoryLayout<C>.unsafeAddress(of: \.labeledTupleIvar.y, in: ref)
  expectNotNil(py)
  expectEqual(py?.pointee, 5)
}

suite.test("secretlyMutableIvar") {
  guard #available(macOS 9999, iOS 9999, watchOS 9999, tvOS 9999, *) else { return }
  let ref = C()
  defer { withExtendedLifetime(ref) {} }

  let p = MemoryLayout<C>.unsafeAddress(of: \.secretlyMutableIvar, in: ref)
  expectNotNil(p)
  expectEqual(p?.pointee, 6)
}

suite.test("strongReferenceIvar") {
  guard #available(macOS 9999, iOS 9999, watchOS 9999, tvOS 9999, *) else { return }
  let ref = C()
  defer { withExtendedLifetime(ref) {} }

  let p = MemoryLayout<C>.unsafeAddress(of: \.strongReferenceIvar, in: ref)
  expectNotNil(p)
  expectTrue(p?.pointee === ref.strongReferenceIvar)

  let p1 = MemoryLayout<C>.unsafeAddress(of: \.strongReferenceIvar.value, in: ref)
  expectNil(p1)
}

suite.test("weakReferenceIvar") {
  guard #available(macOS 9999, iOS 9999, watchOS 9999, tvOS 9999, *) else { return }
  let ref = C()
  defer { withExtendedLifetime(ref) {} }
  ref.weakReferenceIvar = ref

  let p = MemoryLayout<C>.unsafeAddress(of: \.weakReferenceIvar, in: ref)
  expectNil(p)
}

suite.test("structIvar") {
  guard #available(macOS 9999, iOS 9999, watchOS 9999, tvOS 9999, *) else { return }
  let ref = C()
  defer { withExtendedLifetime(ref) {} }
  ref.weakReferenceIvar = ref

  let p = MemoryLayout<C>.unsafeAddress(of: \.structIvar, in: ref)
  expectNotNil(p)
  expectTrue(p?.pointee == ref.structIvar)

  let p1 = MemoryLayout<C>.unsafeAddress(of: \.structIvar.simpleIvar, in: ref)
  expectNotNil(p1)
  expectTrue(p1?.pointee == 1)

  let p2 = MemoryLayout<C>.unsafeAddress(of: \.structIvar.tupleIvar.0, in: ref)
  expectNotNil(p2)
  expectTrue(p2?.pointee == 2)

  let p3 = MemoryLayout<C>.unsafeAddress(of: \.structIvar.tupleIvar.1, in: ref)
  expectNotNil(p3)
  expectTrue(p3?.pointee == 3)

  let p4 = MemoryLayout<C>.unsafeAddress(of: \.structIvar.labeledTupleIvar.x, in: ref)
  expectNotNil(p4)
  expectTrue(p4?.pointee == 4)

  let p5 = MemoryLayout<C>.unsafeAddress(of: \.structIvar.labeledTupleIvar.y, in: ref)
  expectNotNil(p5)
  expectTrue(p5?.pointee == 5)

  let p6 = MemoryLayout<C>.unsafeAddress(of: \.structIvar.secretlyMutableIvar, in: ref)
  expectNotNil(p6)
  expectTrue(p6?.pointee == 6)

  let p7 = MemoryLayout<C>.unsafeAddress(of: \.structIvar.strongReferenceIvar, in: ref)
  expectNotNil(p7)
  expectTrue(p7?.pointee === ref.structIvar.strongReferenceIvar)

  expectNil(MemoryLayout<C>.unsafeAddress(of: \.structIvar.strongReferenceIvar.value, in: ref))

  let p8 = MemoryLayout<C>.unsafeAddress(of: \.structIvar.weakReferenceIvar, in: ref)
  expectNil(p8)

  let p9 = MemoryLayout<C>.unsafeAddress(of: \.structIvar.cFunctionIvar, in: ref)
  expectNotNil(p9)
  expectTrue(p9?.pointee() == 8)

  expectNil(MemoryLayout<C>.unsafeAddress(of: \.structIvar.closureIvar, in: ref))
  expectNil(MemoryLayout<C>.unsafeAddress(of: \.structIvar.computedProperty, in: ref))
  expectNil(MemoryLayout<C>.unsafeAddress(of: \.structIvar.storedPropertyWithDidSet, in: ref))
  expectNil(MemoryLayout<C>.unsafeAddress(of: \.structIvar.storedPropertyWithWillSet, in: ref))
  expectNil(MemoryLayout<C>.unsafeAddress(of: \.structIvar.computedPropertyWithGeneralizedAccessors, in: ref))
}

suite.test("closureIvar") {
  guard #available(macOS 9999, iOS 9999, watchOS 9999, tvOS 9999, *) else { return }
  let ref = C()
  defer { withExtendedLifetime(ref) {} }

  let p = MemoryLayout<C>.unsafeAddress(of: \.closureIvar, in: ref)
  expectNil(p) // reabstracted
}

suite.test("cFunctionIvar") {
  guard #available(macOS 9999, iOS 9999, watchOS 9999, tvOS 9999, *) else { return }
  let ref = C()
  defer { withExtendedLifetime(ref) {} }

  let p = MemoryLayout<C>.unsafeAddress(of: \.cFunctionIvar, in: ref)
  expectNotNil(p)
  expectEqual(p?.pointee(), 8)
}

suite.test("computedProperty") {
  guard #available(macOS 9999, iOS 9999, watchOS 9999, tvOS 9999, *) else { return }
  let ref = C()
  defer { withExtendedLifetime(ref) {} }

  let p = MemoryLayout<C>.unsafeAddress(of: \.computedProperty, in: ref)
  expectNil(p)
}

suite.test("storedPropertyWithDidSet") {
  guard #available(macOS 9999, iOS 9999, watchOS 9999, tvOS 9999, *) else { return }
  let ref = C()
  defer { withExtendedLifetime(ref) {} }

  let p = MemoryLayout<C>.unsafeAddress(of: \.storedPropertyWithDidSet, in: ref)
  expectNil(p)
}

suite.test("storedPropertyWithWillSet") {
  guard #available(macOS 9999, iOS 9999, watchOS 9999, tvOS 9999, *) else { return }
  let ref = C()
  defer { withExtendedLifetime(ref) {} }

  let p = MemoryLayout<C>.unsafeAddress(of: \.storedPropertyWithWillSet, in: ref)
  expectNil(p)
}

suite.test("computedPropertyWithGeneralizedAccessors") {
  guard #available(macOS 9999, iOS 9999, watchOS 9999, tvOS 9999, *) else { return }
  let ref = C()
  defer { withExtendedLifetime(ref) {} }

  let p = MemoryLayout<C>.unsafeAddress(of: \.computedPropertyWithGeneralizedAccessors, in: ref)
  expectNil(p)
}

suite.test("nonFinalIvar") {
  guard #available(macOS 9999, iOS 9999, watchOS 9999, tvOS 9999, *) else { return }
  let ref = C()
  defer { withExtendedLifetime(ref) {} }

  let p = MemoryLayout<C>.unsafeAddress(of: \.nonFinalIvar, in: ref)
  expectNil(p)
}
