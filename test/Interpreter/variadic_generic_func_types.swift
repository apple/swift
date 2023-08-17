// RUN: %target-run-simple-swift

// REQUIRES: executable_test

import StdlibUnittest

var funcs = TestSuite("VariadicGenericFuncTypes")

func makeFunctionType1<each T>(_: repeat (each T).Type) -> Any.Type {
  return ((repeat each T) -> ()).self
}

func makeFunctionType2<each T>(_: repeat (each T).Type) -> Any.Type {
  return ((Character, repeat each T, Bool) -> ()).self
}

func makeFunctionType3<each T>(_: repeat (each T).Type) -> Any.Type {
  return ((inout Character, repeat each T, inout Bool) -> ()).self
}

funcs.test("makeFunctionType1") {
  expectEqual("() -> ()", _typeName(makeFunctionType1()))
  expectEqual("(Swift.Int) -> ()", _typeName(makeFunctionType1(Int.self)))
  expectEqual("(Swift.Int, Swift.String) -> ()", _typeName(makeFunctionType1(Int.self, String.self)))
  expectEqual("(Swift.Int, Swift.Float, Swift.String) -> ()", _typeName(makeFunctionType1(Int.self, Float.self, String.self)))
}

funcs.test("makeFunctionType2") {
  expectEqual("(Swift.Character, Swift.Bool) -> ()", _typeName(makeFunctionType2()))
  expectEqual("(Swift.Character, Swift.Int, Swift.Bool) -> ()", _typeName(makeFunctionType2(Int.self)))
  expectEqual("(Swift.Character, Swift.Int, Swift.String, Swift.Bool) -> ()", _typeName(makeFunctionType2(Int.self, String.self)))
  expectEqual("(Swift.Character, Swift.Int, Swift.Float, Swift.String, Swift.Bool) -> ()", _typeName(makeFunctionType2(Int.self, Float.self, String.self)))
}

funcs.test("makeFunctionType3") {
  expectEqual("(inout Swift.Character, inout Swift.Bool) -> ()", _typeName(makeFunctionType3()))
  expectEqual("(inout Swift.Character, Swift.Int, inout Swift.Bool) -> ()", _typeName(makeFunctionType3(Int.self)))
  expectEqual("(inout Swift.Character, Swift.Int, Swift.String, inout Swift.Bool) -> ()", _typeName(makeFunctionType3(Int.self, String.self)))
  expectEqual("(inout Swift.Character, Swift.Int, Swift.Float, Swift.String, inout Swift.Bool) -> ()", _typeName(makeFunctionType3(Int.self, Float.self, String.self)))
}

runAllTests()
