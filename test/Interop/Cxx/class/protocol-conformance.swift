// RUN: %target-run-simple-swift(-I %S/Inputs/ -Xfrontend -enable-cxx-interop)

// REQUIRES: executable_test

import StdlibUnittest
import ProtocolConformance

protocol TestMethods {
  init(_: DummyStruct)
  mutating func test1()
  mutating func test2(_: Int32)
  mutating func test3(_: Int32, _: UInt32) -> CChar
}

protocol CanReturn42 {
  mutating func return42() -> Int32
}

extension CanReturn42 {
  mutating func return42() -> Int32 { 0 }
}

protocol DefaultInitializable {
  init()
}

extension NonTrivial : TestMethods { }
extension NonTrivial : DefaultInitializable { }

extension Trivial : TestMethods { }
extension Trivial : DefaultInitializable { }

extension Trivial : CanReturn42 { }
extension ConformsToProtocol : CanReturn42 { }

@inline(never)
@_optimize(none)
func tryReturn42(_ _p: CanReturn42) -> Int32 {
  var p = _p
  return p.return42()
}

@inline(never)
@_optimize(none)
func makeIt<T : DefaultInitializable>(_ _: T.Type) -> T { T() }

@inline(never)
@_optimize(none)
func makeItWithDummy<T : TestMethods>(_ _: T.Type) -> T { T(DummyStruct()) }

@inline(never)
@_optimize(none)
func callTestMethods(on _subject: TestMethods) -> CChar {
  var subject = _subject
  subject.test1()
  subject.test2(0 as Int32)
  return subject.test3(0 as Int32, 1 as UInt32)
}

struct Holder<T : DefaultInitializable & TestMethods> {
  var value: DefaultInitializable & TestMethods = T()
}

@inline(never)
@_optimize(none)
func callTestMethods<T : TestMethods>(on subject: Holder<T>) -> CChar {
  callTestMethods(on: subject.value)
}

var ExtendedTypes = TestSuite("Extended C++ Types")

ExtendedTypes.test("(Don't) Use default impl") {
  let noMethod = Trivial()
  expectEqual(0, tryReturn42(noMethod))

  let hasMethod = ConformsToProtocol()
  expectEqual(42, tryReturn42(hasMethod))
}

ExtendedTypes.test("Constrained generic") {
  var trivial = makeIt(Trivial.self)
  var result = callTestMethods(on: trivial)
  expectEqual(42, result)
  
  let nonTrivial = makeIt(NonTrivial.self)
  result = callTestMethods(on: nonTrivial)
  expectEqual(42, result)
  
  trivial = makeItWithDummy(Trivial.self)
  result = callTestMethods(on: trivial)
  expectEqual(42, result)
}

ExtendedTypes.test("Generic struct") {
  var hold = Holder<Trivial>()
  var result = callTestMethods(on: hold)
  expectEqual(42, result)
  
  hold.value = makeItWithDummy(NonTrivial.self)
  result = callTestMethods(on: hold)
  expectEqual(42, result)
}

runAllTests()
