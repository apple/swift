// RUN: %target-run-simple-swift(-Xfrontend -disable-availability-checking)

// REQUIRES: executable_test

// UNSUPPORTED: use_os_stdlib
// UNSUPPORTED: back_deployment_runtime

import StdlibUnittest

var conformances = TestSuite("VariadicGenericConformances")

protocol P {
  static func foobar() -> [String]
}

struct G<each T> {}

extension G: P where repeat each T: P {
  static func foobar() -> [String] {
    var result: [String] = []
    repeat result += (each T).foobar()
    return result
  }
}

extension Int: P {
  static func foobar() -> [String] {
    return ["Int"]
  }
}

extension String: P {
  static func foobar() -> [String] {
    return ["String"]
  }
}

func callFoobar<T: P>(_: T) -> [String] {
  return T.foobar()
}

conformances.test("conditional") {
  expectEqual([], callFoobar(G< >()))
  expectEqual(["Int"], callFoobar(G<Int>()))
  expectEqual(["Int", "String"], callFoobar(G<Int, String>()))
}

func cast<T, U>(_ value: T, to: U.Type) -> Bool {
  return value is U
}

conformances.test("cast") {
  expectEqual(true, cast(G< >(), to: (any P).self))
  expectEqual(true, cast(G<Int>(), to: (any P).self))
  expectEqual(true, cast(G<Int, String>(), to: (any P).self))

  expectEqual(false, cast(G<Bool>(), to: (any P).self))
  expectEqual(false, cast(G<Int, Bool>(), to: (any P).self))
  expectEqual(false, cast(G<Bool, Int>(), to: (any P).self))
}

struct Outer<each U> {
  struct Inner<each V> {}
}

extension Outer.Inner: P where repeat (repeat (each U, each V)): Any {
  static func foobar() -> [String] {
    return ["hello"]
  }
}

conformances.test("shape") {
  expectEqual(true, cast(Outer< >.Inner< >(), to: (any P).self))
  expectEqual(true, cast(Outer<Int>.Inner<Bool>(), to: (any P).self))
  expectEqual(true, cast(Outer<Int, String>.Inner<Bool, Float>(), to: (any P).self))

  expectEqual(false, cast(Outer<Bool>.Inner< >(), to: (any P).self))
  expectEqual(false, cast(Outer<Int, Bool>.Inner<String, Float, Character>(), to: (any P).self))
}

runAllTests()