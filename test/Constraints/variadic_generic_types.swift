// RUN: %target-typecheck-verify-swift -disable-availability-checking

// Parsing an UnresolvedSpecializeExpr containing a PackExpansionType
struct G<each T> {}

func f<each T>(_: repeat each T) {
  _ = G< >.self
  _ = G<Int>.self
  _ = G<Int, String>.self
  _ = G<repeat each T>.self
  _ = G<Int, repeat Array<each T>>.self
}

// Forming PackExpansionTypeReprs in simplifyTypeExpr()
func g<each T>(_: repeat each T) {
  _ = (repeat each T).self
  _ = (Int, repeat each T).self
  _ = ((repeat each T) -> ()).self
  _ = ((Int, repeat Array<each T>) -> ()).self

  _ = (repeat each Int).self
  // expected-error@-1 {{pack expansion 'Int' must contain at least one pack reference}}
  // expected-error@-2 {{'each' cannot be applied to non-pack type 'Int'}}{{15-19=}}
}

struct MissingMemberError<each T> {
  init() {
    self.doesNotExist = 1
    // expected-error@-1 {{value of type 'MissingMemberError<repeat each T>' has no member 'doesNotExist'}}
  }
}

// https://github.com/apple/swift/issues/66095
do {
  struct Test<each S> {
    init(_ s: repeat each S) {}
  }

  func test1<each T>(_ v: repeat each T) -> Test<repeat each T> {
    return Test(repeat each v) // Ok
  }

  func test2<each T>(_ v: repeat each T) -> Test<repeat each T> {
    return Test<repeat each T>(repeat each v) // Ok
  }

  func test3<each T>(_ v: repeat each T) -> Test<String, repeat each T, Int> {
    return Test("a", repeat each v, 42) // Ok
  }

  func test4<each T>(_ v: repeat each T) -> Test<repeat each T, String, Int> {
    return Test<repeat each T, String, Int>(repeat each v, "a", 42) // Ok
  }

  func test5<each T>(_ v: repeat each T) -> Test<String, Int, repeat each T> {
    return Test<String, Int, repeat each T>("a", 42, repeat each v) // Ok
  }
}
// rdar://107479662 - variadic tuple of Sendable elements does not conform to Sendable
do {
  struct Test<each T> : Sendable {
    let prop: (repeat TestProperty<each T>)
  }

  struct TestProperty<T> : Sendable {
  }
}
