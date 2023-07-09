// RUN: %target-run-simple-swift(-Xfrontend -disable-availability-checking)

// REQUIRES: executable_test

// UNSUPPORTED: use_os_stdlib
// UNSUPPORTED: back_deployment_runtime

import StdlibUnittest

var conformances = TestSuite("VariadicGenericConformances")

protocol P {
  associatedtype A
  associatedtype B
  associatedtype C
}

struct H<T> {}
struct G<each T> {}

struct TupleWitnesses<each T: Sequence>: P {
  typealias A = (Bool, repeat each T)
  typealias B = (repeat each T.Element, x: Bool)
  typealias C = (x: Bool, repeat H<each T.Element>)
}

struct SingletonTupleWitnesses<each T>: P {
  typealias A = (repeat each T)
  typealias B = (repeat each T, Int)
  typealias C = (Int, repeat each T)
}

struct FunctionWitnesses<each T: Sequence>: P {
  typealias A = (Bool, repeat each T) -> ()
  typealias B = (repeat each T.Element, Bool) -> ()
  typealias C = (Bool, repeat H<each T.Element>) -> ()
}

struct NominalWitnesses<each T: Sequence>: P {
  typealias A = G<Bool, repeat each T>
  typealias B = G<repeat each T.Element, Bool>
  typealias C = G<Bool, repeat H<each T.Element>>
}

func getA<T: P>(_: T.Type) -> Any.Type {
  return T.A.self
}

func getB<T: P>(_: T.Type) -> Any.Type {
  return T.B.self
}

func getC<T: P>(_: T.Type) -> Any.Type {
  return T.C.self
}

conformances.test("tupleWitnesses") {
  let g = TupleWitnesses<Array<Int>, Set<String>>.self
  expectEqual((Bool, Array<Int>, Set<String>).self, getA(g))
  expectEqual((Int, String, x: Bool).self, getB(g))
  expectEqual((x: Bool, H<Int>, H<String>).self, getC(g))
}

conformances.test("singletonTupleWitnesses") {
  let g1 = SingletonTupleWitnesses<Bool>.self
  expectEqual(Bool.self, getA(g1))

  let g2 = SingletonTupleWitnesses< >.self
  expectEqual(Int.self, getB(g2))
  expectEqual(Int.self, getC(g2))
}

conformances.test("functionWitnesses") {
  let g = FunctionWitnesses<Array<Int>, Set<String>>.self
  expectEqual(((Bool, Array<Int>, Set<String>) -> ()).self, getA(g))
  expectEqual(((Int, String, Bool) -> ()).self, getB(g))
  expectEqual(((Bool, H<Int>, H<String>) -> ()).self, getC(g))
}

conformances.test("nominalWitnesses") {
  let g = NominalWitnesses<Array<Int>, Set<String>>.self
  expectEqual(G<Bool, Array<Int>, Set<String>>.self, getA(g))
  expectEqual(G<Int, String, Bool>.self, getB(g))
  expectEqual(G<Bool, H<Int>, H<String>>.self, getC(g))
}

runAllTests()
