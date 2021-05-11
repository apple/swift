// RUN: %target-run-simple-swift
// REQUIRES: executable_test

import StdlibUnittest
import Swift


let OptionalTests = TestSuite("Optional")

protocol TestProtocol1 {}

// Check the generic parameter name.
extension Optional where Wrapped : TestProtocol1 {
  var _wrappedIsTestProtocol1: Bool {
    fatalError("not implemented")
  }
}

OptionalTests.test("nil comparison") {
  var x: Int? = nil
  expectFalse(x != nil)

  switch x {
  case .some(let y): expectUnreachable()
  case .none: break
  }

  x = .some(1)
  expectTrue(x != nil)

  do {
    var y1: Int? = .none
    expectTrue(y1 == nil)

    var y2: Int? = .none
    expectTrue(y2 == nil)
  }

  let x1: Int? = nil
  let x2: Int? = .none

  expectTrue(x1 == nil)
  expectTrue(x2 == nil)

  switch x {
    case .some(let y): expectEqual("1", "\(y)")
    case .none: assert(false)
  }

  expectEqual("forced extraction: 1.", "forced extraction: \(x!).")
  expectEqual(
    "forced extraction use: 2.",
    "forced extraction use: \(x!.advanced(by: 1))."
  )
}

func testRelation(_ p: (Int?, Int?) -> Bool) -> [Bool] {
  typealias optPair = (Int?, Int?)
  
  let relationships: [optPair] = [
    (1, 1), (1, 2), (2, 1), (1, .none), (.none, 1), (.none, .none)
  ]

  return relationships.map { p($0, $1) }
}

OptionalTests.test("Equatable") {
  expectEqual([true, false, false, false, false, true], testRelation(==))
  expectEqual([false, true, true, true, true, false], testRelation(!=))
}

OptionalTests.test("Hashable") {
    let o1: Optional<Int> = .some(1010)
    let o2: Optional<Int> = .some(2020)
    let o3: Optional<Int> = .none
    checkHashable([o1, o2, o3], equalityOracle: { $0 == $1 })

    let oo1: Optional<Optional<Int>> = .some(.some(1010))
    let oo2: Optional<Optional<Int>> = .some(.some(2010))
    let oo3: Optional<Optional<Int>> = .some(.none)
    let oo4: Optional<Optional<Int>> = .none
    checkHashable([oo1, oo2, oo3, oo4], equalityOracle: { $0 == $1 })
}

OptionalTests.test("CustomReflectable") {
  // Test with a non-refcountable type.
  do {
    let value: OpaqueValue<Int>? = nil
    var output = ""
    dump(value, to: &output)
    expectEqual("- nil\n", output)
    expectEqual(.optional, Mirror(reflecting: value).displayStyle)
  }
  do {
    let value: OpaqueValue<Int>? = OpaqueValue(1010)
    var output = ""
    dump(value, to: &output)
    let expected =
      "▿ Optional(StdlibUnittest.OpaqueValue<Swift.Int>(value: 1010, identity: 0))\n" +
      "  ▿ some: StdlibUnittest.OpaqueValue<Swift.Int>\n" +
      "    - value: 1010\n" +
      "    - identity: 0\n"
    expectEqual(expected, output)
    expectEqual(.optional, Mirror(reflecting: value).displayStyle)
  }
  // Test with a reference type.
  do {
    let value: LifetimeTracked? = nil
    var output = ""
    dump(value, to: &output)
    expectEqual("- nil\n", output)
    expectEqual(.optional, Mirror(reflecting: value).displayStyle)
  }
  do {
    let value: LifetimeTracked? = LifetimeTracked(1010)
    var output = ""
    dump(value, to: &output)
    let expected =
      "▿ Optional(1010)\n" +
      "  ▿ some: 1010 #0\n" +
      "    - value: 1010\n" +
      "    - identity: 0\n" +
      "    - serialNumber: 1\n"
    expectEqual(expected, output)
    expectEqual(.optional, Mirror(reflecting: value).displayStyle)
  }
}

struct X {}
class C {}
class D {}

class E : Equatable {}
func == (_: E, _: E) -> Bool { return true }

OptionalTests.test("initializers") {
  let _: X? = nil
  let _: X? = X()

  let _: C? = nil
  let _: C? = C()
}

OptionalTests.test("nil comparison") {
  let v0: Int? = nil
  let v1: Int? = 1

  expectFalse(v1 == nil)
  expectTrue(v1 != nil)
  expectTrue(v0 == nil)
  expectFalse(v0 != nil)

  expectFalse(nil == v1)
  expectTrue(nil != v1)
  expectTrue(nil == v0)
  expectFalse(nil != v0)

  let e0: E? = nil
  let e1: E? = E()
  
  expectFalse(e1 == nil)
  expectTrue(e1 != nil)
  expectTrue(e0 == nil)
  expectFalse(e0 != nil)

  expectFalse(nil == e1)
  expectTrue(nil != e1)
  expectTrue(nil == e0)
  expectFalse(nil != e0)

  /*
  // FIXME: <rdar://problem/17489239> Optional<T>() == nil where T: !Equatable
  let _: X? = nil
  let _: X? = X()

  expectFalse(x1 == nil)
  expectTrue(x1 != nil)
  expectTrue(x0 == nil)
  expectFalse(x0 != nil)

  expectFalse(nil == x1)
  expectTrue(nil != x1)
  expectTrue(nil == x0)
  expectFalse(nil != x0)
  */
}

OptionalTests.test("??") {
  var counter = 0
  func nextCounter() -> Int { counter += 1; return counter-1 }
  func nextCounter2() -> Int? { return nextCounter() }

  let a: Int? = 123
  let b: Int? = nil
  let c: Int? = nil
  let d: Int? = 456
  let e: Int? = nil
  let f: Int? = nil

  expectEqual(123, a ?? nextCounter())
  expectEqual(0, b ?? nextCounter())
  expectEqual(1, c ?? nextCounter())
  expectEqual(456, d ?? nextCounter())
  expectEqual(456, e ?? d ?? nextCounter())
  expectEqual(2, f ?? nextCounter())

  expectEqual(Optional(456), c ?? d)
  expectEqual(nil, c ?? e)
  expectEqual(Optional(123), a ?? nextCounter2())
  expectEqual(Optional(3), b ?? nextCounter2())
  expectEqual(Optional(4), c ?? nextCounter2())
  expectEqual(Optional(456), d ?? nextCounter2())
  expectEqual(Optional(456), e ?? d ?? nextCounter2())
  expectEqual(Optional(5), f ?? nextCounter2())
}

OptionalTests.test("flatMap") {
  let half: (Int32) -> Int16? =
    { if $0 % 2 == 0 { return Int16($0 / 2) } else { return .none } }

  expectEqual(2 as Int16, half(4))
  expectNil(half(3))

  expectNil((.none as Int32?).flatMap(half))
  expectEqual(2 as Int16, (4 as Int32?).flatMap(half))
  expectNil((3 as Int32?).flatMap(half))
}

// FIXME: @inline(never) does not inhibit specialization

@inline(never)
@_optimize(none)
func anyToAny<T, U>(_ a: T, _ : U.Type) -> U {
  return a as! U
}

@inline(never)
@_optimize(none)
func anyToAnyIs<T, U>(_ a: T, _ : U.Type) -> Bool {
  return a is U
}

@inline(never)
@_optimize(none)
func anyToAnyIsOptional<T, U>(_ a: T?, _ : U.Type) -> Bool {
  return a is U?
}

@inline(never)
@_optimize(none)
func anyToAnyOrNil<T, U>(_ a: T, _ : U.Type) -> U? {
  return a as? U
}

@inline(never)
@_optimize(none)
func canGenericCast<T, U>(_ a: T, _ ty : U.Type) -> Bool {
  return anyToAnyOrNil(a, ty) != nil
}

protocol TestExistential {}
extension Int : TestExistential {}

OptionalTests.test("Casting Optional") {
  let x = C()
  let sx: C? = x
  let nx: C? = nil
  expectTrue(anyToAny(x, Optional<C>.self)! === x)
  expectTrue(anyToAnyIs(x, Optional<C>.self))
  expectFalse(anyToAnyIs(x, Optional<D>.self))

  expectTrue(anyToAny(sx, C.self) === x)
  expectTrue(anyToAnyIs(sx, C.self))
  expectFalse(anyToAnyIs(sx, D.self))

  expectTrue(anyToAny(sx, Optional<C>.self)! === x)
  expectTrue(anyToAnyIs(sx, Optional<C>.self))
  expectTrue(anyToAnyIsOptional(sx, C.self))
  expectFalse(anyToAnyIsOptional(sx, D.self))

  expectTrue(anyToAny(nx, Optional<C>.self) == nil)
  expectTrue(anyToAnyIs(nx, Optional<C>.self))

  // You can cast a nil of any type to a nil of any other type
  // successfully
  expectTrue(anyToAnyIs(nx, Optional<D>.self))

  expectTrue(anyToAnyIsOptional(nx, C.self))

  expectTrue(anyToAnyOrNil(nx, C.self) == nil)

  let i = Int.max
  let si: Int? = Int.max
  let ni: Int? = nil
  expectEqual(anyToAny(i, Optional<Int>.self)!, Int.max)
  expectEqual(anyToAny(si, Int.self), Int.max)
  expectEqual(anyToAny(si, Optional<Int>.self)!, Int.max)

  expectTrue(anyToAny(ni, Optional<Int>.self) == nil)
  expectTrue(anyToAnyOrNil(ni, Int.self) == nil)

  let ssx: C?? = sx
  expectTrue(anyToAny(ssx, Optional<C>.self)! === x)
  expectTrue(anyToAny(x, Optional<Optional<C>>.self)!! === x)
  expectTrue(anyToAnyOrNil(ni, Int.self) == nil)

  // Test for SR-459: Weakened optionals don't zero.
  var t = LifetimeTracked(0)
  _ = anyToAny(Optional(t), CustomDebugStringConvertible.self)
  expectTrue(anyToAnyIs(Optional(t), CustomDebugStringConvertible.self))

  // Test for SR-912: Runtime exception casting an Any nil to an Optional.
  let oi: Int? = nil
  expectTrue(anyToAny(oi as Any, Optional<Int>.self) == nil)
  expectTrue(anyToAnyIs(oi as Any, Optional<Int>.self))

  // Double-wrapped optional
  expectTrue(anyToAnyIsOptional(oi as Any, Int.self))

  // For good measure test an existential that Optional does not conform to.
  expectTrue(anyToAny(3 as TestExistential, Optional<Int>.self) == 3)

  // Can't do existential + optional wrapping at once for some reason
  expectTrue(anyToAnyIs(3 as TestExistential, Optional<Int>.self))
  expectTrue(anyToAnyIsOptional(3 as TestExistential, Int.self))

  // And a type that is not convertible to its target.
  expectTrue(anyToAny(nx as Any, Optional<Int>.self) == nil)
  expectTrue(anyToAnyIs(nx as Any, Optional<Int>.self))
  expectTrue(anyToAnyIsOptional(nx as Any, Int.self))

  expectTrue(anyToAnyOrNil(sx as Any, Optional<Int>.self) == nil)
  expectFalse(anyToAnyIs(sx as Any, Optional<Int>.self))
  expectFalse(anyToAnyIsOptional(sx as Any, Int.self))

  // OK to convert nil of any type to optional of any other type
  expectTrue(anyToAnyIs(Optional<(String, String)>.none, Optional<Bool>.self))
  expectTrue(anyToAnyIsOptional(Optional<(String, String)>.none, Bool.self))
}

#if !os(WASI)
OptionalTests.test("Casting Optional Traps") {
  let nx: C? = nil
  expectCrash { _blackHole(anyToAny(nx, Int.self)) }
}
OptionalTests.test("Casting Optional Any Traps") {
  let nx: X? = X()
  expectCrash { _blackHole(anyToAny(nx as Any, Optional<Int>.self)) }
}
#endif

class TestNoString {}
class TestString : CustomStringConvertible, CustomDebugStringConvertible {
  var description: String {
    return "AString"
  }
  var debugDescription: String {
    return "XString"
  }
}
class TestStream : TextOutputStreamable {
  func write<Target : TextOutputStream>(to target: inout Target) {
    target.write("AStream")
  }
}

func debugPrintStr<T>(_ a: T) -> String {
  var s = ""
  debugPrint(a, terminator: "", to: &s)
  return s
}
// Optional should not conform to output stream protocols itself, but is
// convertible to them if its wrapped type is.
// Furthermore, printing an Optional should always print the debug
// description regardless of whether the wrapper type conforms to an
// output stream protocol.
OptionalTests.test("Optional TextOutputStream") {
  let optNoString: TestNoString? = TestNoString()
  expectFalse(optNoString is CustomStringConvertible)
  expectFalse(canGenericCast(optNoString, CustomStringConvertible.self))
  expectFalse(optNoString is TextOutputStreamable)
  expectFalse(canGenericCast(optNoString, TextOutputStreamable.self))
  expectTrue(optNoString is CustomDebugStringConvertible)
  expectTrue(canGenericCast(optNoString, CustomDebugStringConvertible.self))
  expectEqual(String(describing: optNoString), "Optional(main.TestNoString)")
  expectEqual(debugPrintStr(optNoString), "Optional(main.TestNoString)")

  let optString: TestString? = TestString()
  expectTrue(optString is CustomStringConvertible)
  expectTrue(canGenericCast(optString, CustomStringConvertible.self))
  expectTrue(optString is CustomDebugStringConvertible)
  expectTrue(canGenericCast(optString, CustomDebugStringConvertible.self))
  expectEqual(String(describing: TestString()), "AString")
  expectEqual(String(describing: optString), "Optional(XString)")
  expectEqual(debugPrintStr(optString), "Optional(XString)")

  let optStream: TestStream? = TestStream()
  expectTrue(optStream is TextOutputStreamable)
  expectTrue(canGenericCast(optStream, TextOutputStreamable.self))
  expectTrue(optStream is CustomDebugStringConvertible)
  expectTrue(canGenericCast(optStream, CustomDebugStringConvertible.self))
  expectEqual(String(describing: TestStream()), "AStream")
  expectEqual(String(describing: optStream), "Optional(AStream)")
  expectEqual(debugPrintStr(optStream), "Optional(AStream)")
}

OptionalTests.test("unsafelyUnwrapped") {
  let nonEmpty: Int? = 3
  expectEqual(3, nonEmpty.unsafelyUnwrapped)
}

#if !os(WASI)
OptionalTests.test("unsafelyUnwrapped nil")
  .xfail(.custom(
    { !_isDebugAssertConfiguration() },
    reason: "assertions are disabled in Release and Unchecked mode"))
  .code {
  let empty: Int? = nil
  expectCrashLater()
  _blackHole(empty.unsafelyUnwrapped)
}
#endif

runAllTests()
