// RUN: %target-run-simple-swift(-Xfrontend -disable-concrete-type-metadata-mangled-name-accessors)
// RUN: %target-run-simple-swift

// REQUIRES: executable_test

import StdlibUnittest

var types = TestSuite("VariadicGenericCaptures")

func hasMetadataPack<each T>(_: repeat each T) -> () -> Any.Type {
  return { return (repeat each T).self }
}

types.test("Metadata") {
  expectEqual(Void.self, hasMetadataPack()())
  expectEqual((Int, String, Bool).self, hasMetadataPack(1, "hi", false)())
}

func hasWitnessTablePack<each T: Sequence>(_: repeat each T) -> () -> Any.Type {
  return { return (repeat (each T).Element).self }
}

types.test("WitnessTable") {
  expectEqual(Void.self, hasWitnessTablePack()())
  expectEqual((Int, String, Bool).self, hasWitnessTablePack([1], ["hi"], [false])())
}

func hasWitnessTablePack2<each T: Sequence>(_: repeat each T) -> () -> Any.Type where repeat (each T).Element: Sequence {
  return { return (repeat (each T).Element.Element).self }
}

types.test("WitnessTable2") {
  expectEqual(Void.self, hasWitnessTablePack2()())
  expectEqual((Int, String, Bool).self, hasWitnessTablePack2([[1]], [["hi"]], [[false]])())
}

// Test lifetimes of captured packs
func lifetimeTest1() -> () -> Any.Type {
  return hasMetadataPack("hello", Set<Int>())
}

func lifetimeTest2() -> () -> Any.Type {
  return hasMetadataPack(3, 1.0)
}

types.test("Lifetime") {
  let fn1 = lifetimeTest1()
  let fn2 = lifetimeTest2()
  expectEqual((String, Set<Int>).self, fn1())
  expectEqual((Int, Double).self, fn2())
}

// Test captured parameter packs
func testEscapingCapture<each T: Hashable>(_ t: repeat each T) -> () -> [AnyHashable] {
  return {
    var result = [AnyHashable]()
    repeat result.append(each t)
    return result
  }
}

func callNonEscaping(_ fn: () -> [AnyHashable]) -> [AnyHashable] {
  return fn()
}

func testNonEscapingCapture<each T: Hashable>(_ t: repeat each T) -> [AnyHashable] {
  return callNonEscaping {
    var result = [AnyHashable]()
    repeat result.append(each t)
    return result
  }
}

types.test("CapturedValue") {
  let fn1 = testEscapingCapture(1, "hi")
  let fn2 = testEscapingCapture(5.0, false)

  expectEqual([1, "hi"], fn1())
  expectEqual([5.0, false], fn2())

  expectEqual(["bye", 3.0], testNonEscapingCapture("bye", 3.0))
  expectEqual([true, 7], testNonEscapingCapture(true, 7))
}

runAllTests()
