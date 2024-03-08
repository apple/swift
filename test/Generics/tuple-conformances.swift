// RUN: %target-typecheck-verify-swift -enable-experimental-feature TupleConformances

// XFAIL: noncopyable_generics

// Because of -enable-experimental-feature TupleConformances
// REQUIRES: asserts

extension () {
  // expected-error@-1 {{tuple extension must be written as extension of '(repeat each Element)'}}
  // expected-error@-2 {{tuple extension must declare conformance to exactly one protocol}}

  struct Nested {}
  // expected-error@-1 {{type 'Nested' cannot be nested in tuple extension}}
}

typealias BadTuple<each Horse> = (repeat each Horse, Int)
extension BadTuple {}
// expected-error@-1 {{tuple extension must be written as extension of '(repeat each Horse)'}}
// expected-error@-2 {{tuple extension must declare conformance to exactly one protocol}}

typealias Tuple<each Element> = (repeat each Element)

protocol Q {}

class C {}

extension Tuple: Q where repeat each Element: Collection, repeat (each Element).Element == (each Element).Index, repeat (each Element).Indices: AnyObject, repeat (each Element).SubSequence: C {}
// expected-error@-1 {{tuple extension must require that 'each Element' conforms to 'Q'}}
// expected-error@-2 {{tuple extension cannot require that 'each Element' conforms to 'Collection'}}
// expected-error@-3 {{tuple extension cannot require that '(each Element).Element' is the same type as '(each Element).Index'}}
// expected-error@-4 {{tuple extension cannot require that '(each Element).Indices' conforms to 'AnyObject'}}
// expected-error@-5 {{tuple extension cannot require that '(each Element).SubSequence' subclasses 'C'}}

protocol Base1 {}
protocol Derived1: Base1 {}

extension Tuple: Derived1 where repeat each Element: Derived1 {}
// expected-error@-1 {{conditional conformance of type '(repeat each Element)' to protocol 'Derived1' does not imply conformance to inherited protocol 'Base1'}}
// expected-note@-2 {{did you mean to explicitly state the conformance like 'extension Tuple: Base1 where ...'?}}
// expected-error@-3 {{tuple extension must declare conformance to exactly one protocol}}

protocol Base2 {}
protocol Derived2: Base2 {}

extension Tuple: Derived2 {}
// expected-error@-1 {{tuple extension must declare conformance to exactly one protocol}} // FIXME: crappy error

////

protocol P2 {}

typealias FancyTuple1<each Cat: P2> = (repeat each Cat)
extension FancyTuple1: P2 {}

protocol P3 {}

typealias FancyTuple2<each Cat: C> = (repeat each Cat)
extension FancyTuple2: P3 {}
// expected-error@-1 {{tuple extension cannot require that 'each Cat' subclasses 'C'}}
// expected-error@-2 {{tuple extension must require that 'each Cat' conforms to 'P3'}}

////

protocol P {
  associatedtype A
  associatedtype B

  func f()
}

extension Tuple: P where repeat each Element: P {
  typealias A = (repeat (each Element).A)
  typealias B = (repeat (each Element).B)
  func f() {}
}

extension Int: P {
  typealias A = Int
  typealias B = String
  func f() {}
}

func returnsPA<T: P>(_: T) -> T.A.Type {}
func returnsPB<T: P>(_: T) -> T.B.Type {}

func same<T>(_: T, _: T) {}

func useConformance() {
  same(returnsPA((1, 2, 3)), (Int, Int, Int).self)
  same(returnsPB((1, 2, 3)), (String, String, String).self)

  (1, 2, 3).f()
}

////

extension Tuple: Equatable where repeat each Element: Equatable {
  // FIXME: Hack
  @_disfavoredOverload
  public static func ==(lhs: Self, rhs: Self) -> Bool {
    for (l, r) in repeat (each lhs, each rhs) {
      if l != r { return false }
    }
    return true
  }
}

extension Tuple: Hashable where repeat each Element: Hashable {
  public func hash(into hasher: inout Hasher) {
    repeat (each self).hash(into: &hasher)
  }
}

extension Tuple: Comparable where repeat each Element: Comparable {
  // FIXME: Hack
  @_disfavoredOverload
  public static func <(lhs: Self, rhs: Self) -> Bool {
    for (l, r) in repeat (each lhs, each rhs) {
      if l > r { return false }
      if l < r { return true }
    }
    return false
  }
}

