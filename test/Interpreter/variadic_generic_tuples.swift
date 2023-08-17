// RUN: %target-run-simple-swift

// REQUIRES: executable_test

import StdlibUnittest

var tuples = TestSuite("VariadicGenericTuples")

// Metadata instantiation for tuples containing pack expansions

func makeTuple<each T>(_: repeat (each T).Type) -> Any.Type {
  return (repeat Array<each T>).self
}

tuples.test("makeTuple") {
  expectEqual("()", _typeName(makeTuple()))

  // Note that we unwrap the one-element tuple!
  expectEqual("Swift.Array<Swift.Int>", _typeName(makeTuple(Int.self)))

  expectEqual("(Swift.Array<Swift.Int>, Swift.Array<Swift.String>)", _typeName(makeTuple(Int.self, String.self)))
  expectEqual("(Swift.Array<Swift.Int>, Swift.Array<Swift.String>, Swift.Array<Swift.Float>)", _typeName(makeTuple(Int.self, String.self, Float.self)))
}

func makeTuple2<each T>(_: repeat (each T).Type) -> Any.Type {
  return (Int, repeat Array<each T>).self
}

tuples.test("makeTuple2") {
  // Note that we unwrap the one-element tuple!
  expectEqual("Swift.Int", _typeName(makeTuple2()))

  expectEqual("(Swift.Int, Swift.Array<Swift.Bool>)", _typeName(makeTuple2(Bool.self)))
  expectEqual("(Swift.Int, Swift.Array<Swift.Bool>, Swift.Array<Swift.Character>)", _typeName(makeTuple2(Bool.self, Character.self)))
  expectEqual("(Swift.Int, Swift.Array<Swift.Bool>, Swift.Array<Swift.Character>, Swift.Array<Swift.Double>)", _typeName(makeTuple2(Bool.self, Character.self, Double.self)))
}

func makeTuple3<each T, each U>(t: repeat (each T).Type, u: repeat (each U).Type) -> Any.Type {
  return (repeat each T, repeat each U).self
}

tuples.test("makeTuple3") {
  expectEqual("()", _typeName(makeTuple3()))

  // Note that we unwrap the one-element tuple!
  expectEqual("Swift.Int", _typeName(makeTuple3(t: Int.self)))
  expectEqual("Swift.Int", _typeName(makeTuple3(u: Int.self)))

  expectEqual("(Swift.Int, Swift.Float)", _typeName(makeTuple3(t: Int.self, u: Float.self)))
}

func makeTuple<each Element>(
  _ element: repeat each Element
) -> (repeat each Element) {
  return (repeat each element)
}

func expandTupleElements<each T: Equatable>(_ value: repeat each T) {
  let values = makeTuple(repeat each value)
  _ = (repeat expectEqual(each value, each values))
}

tuples.test("expandTuple") {
  expandTupleElements(1, "hello", true)
}

func tupleLabelMix<each T, each U>(t: repeat (each T).Type, u: repeat (each U).Type) -> Any.Type {
  return (Float, hello: Int, repeat each T, swift: String, repeat each U, Bool, world: UInt8).self
}

func oneElementLabeledTuple<each T>(t: repeat (each T).Type) -> Any.Type {
  return (label: Int, repeat each T).self
}

tuples.test("labels") {
  expectEqual("(Swift.Float, hello: Swift.Int, swift: Swift.String, Swift.Bool, world: Swift.UInt8)", _typeName(tupleLabelMix()))
  expectEqual("(Swift.Float, hello: Swift.Int, swift: Swift.String, Swift.Double, Swift.Bool, world: Swift.UInt8)", _typeName(tupleLabelMix(u: Double.self)))
  expectEqual("(Swift.Float, hello: Swift.Int, swift: Swift.String, Swift.Double, Swift.Int32, Swift.Bool, world: Swift.UInt8)", _typeName(tupleLabelMix(u: Double.self, Int32.self)))
  expectEqual("(Swift.Float, hello: Swift.Int, Swift.Character, swift: Swift.String, Swift.Double, Swift.Bool, world: Swift.UInt8)", _typeName(tupleLabelMix(t: Character.self, u: Double.self)))
  expectEqual("(Swift.Float, hello: Swift.Int, Swift.Character, Swift.Substring, swift: Swift.String, Swift.Double, Swift.Int32, Swift.Bool, world: Swift.UInt8)", _typeName(tupleLabelMix(t: Character.self, Substring.self, u: Double.self, Int32.self)))

  // FIXME: One-element labeled tuples
  expectEqual("Swift.Int", _typeName(oneElementLabeledTuple()))

  expectEqual("(label: Swift.Int, Swift.String)", _typeName(oneElementLabeledTuple(t: String.self)))
}


runAllTests()
