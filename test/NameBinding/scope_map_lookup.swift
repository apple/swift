// RUN: %target-parse-verify-swift %s -enable-astscope-lookup

// Name binding in default arguments

// FIXME: Semantic analysis should produce an error here, because 'x'
// is not actually available.
func functionParamScopes(x: Int, y: Int = x) -> Int {
  return x + y
}

// Name binding in instance methods.
class C1 {
	var x = 0

  var hashValue: Int {
    return x
  }
}

// Protocols involving 'Self'.
protocol P1 {
  associatedtype A = Self
}

// Extensions.
protocol P2 {
}

extension P2 {
  func getSelf() -> Self {
    return self
  }
}

// Lazy properties
class LazyProperties {
  init() {
    lazy var localvar = 42  // expected-error {{lazy is only valid for members of a struct or class}} {{5-10=}}
    localvar += 1
    _ = localvar
  }

  var value: Int = 17

  lazy var prop1: Int = value

  lazy var prop2: Int = { value + 1 }()

  lazy var prop3: Int = { [weak self] in self.value + 1 }()

  lazy var prop4: Int = self.value

  lazy var prop5: Int = { self.value + 1 }()
}

// Protocol extensions.
// Extending via a superclass constraint.
class Superclass {
  func foo() { }
  static func bar() { }

  typealias Foo = Int
}

protocol PConstrained4 { }

extension PConstrained4 where Self : Superclass {
  final func testFoo() -> Foo {
    foo()
    self.foo()

    return Foo(5)
  }

  final static func testBar() {
    bar()
    self.bar()
  }
}
