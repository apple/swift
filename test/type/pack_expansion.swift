// RUN: %target-typecheck-verify-swift -disable-availability-checking

func f1<each T>() -> repeat each T {}
// expected-error@-1 {{pack expansion 'repeat each T' can only appear in a function parameter list, tuple element, or generic argument list}}

func f2<each T>() -> (repeat each T) {}
// okay

struct G<each T> {}

func f3<each T>() -> G<repeat each T> {}

protocol P<T> {
  associatedtype T
}

func f4<each T>() -> any P<repeat each T> {}

typealias T1<each T> = repeat each T
// expected-error@-1 {{pack expansion 'repeat each T' can only appear in a function parameter list, tuple element, or generic argument list}}

typealias T2<each T> = (repeat each T)

func f4<each T>() -> repeat () -> each T {}
// expected-error@-1 {{pack expansion 'repeat () -> each T' can only appear in a function parameter list, tuple element, or generic argument list}}

func f5<each T>() -> () -> (repeat each T) {}

func f6<each T>() -> (repeat each T) -> () {}

enum E<each T> { // expected-error {{enums cannot declare a type pack}}
  case f1(_: repeat each T)

  case f2(_: G<repeat each T>)

  var x: repeat each T { fatalError() }
  // expected-error@-1 {{pack expansion 'repeat each T' can only appear in a function parameter list, tuple element, or generic argument list}}

  var x: (repeat each T) { fatalError() }

  subscript(_: repeat each T) -> Int { fatalError() }

  subscript() -> repeat each T { fatalError() }
  // expected-error@-1 {{pack expansion 'repeat each T' can only appear in a function parameter list, tuple element, or generic argument list}}

  subscript() -> (repeat each T) { fatalError() }
}

func withWhereClause<each T>(_ x: repeat each T) where repeat each T: P {}

struct Outer<each T> {
  struct Bad<each U> {
    typealias Value = (repeat (each T, each U)) // expected-error {{pack expansion 'repeat (each T, each U)' requires that 'each T' and 'each U' have the same shape}}
  }

  struct Good<each U> where (repeat (each T, each U)): Any {
    typealias Value = (repeat (each T, each U))
  }

  struct AlsoGood<each U> {
    typealias Value = (repeat (each T, E<repeat each U>))
  }
}

func packRef<each T>(_: repeat each T) where repeat each T: P {}

func packMemberRef<each T>(_: repeat each T.T) where repeat each T: P {}

// expected-error@+1 {{'each' cannot be applied to non-pack type 'Int'}}{{31-35=}}
func invalidPackRefEachInt(_: each Int) {}

// expected-error@+1 {{pack expansion 'Int' must contain at least one pack reference}}
func invalidPackRefRepeatInt(_: repeat Int) {}

// expected-error@+1 {{pack reference 'T' requires expansion using keyword 'repeat'}}
func packRefOutsideExpansion<each T>(_: each T) {}

// expected-error@+1 {{pack reference 'T' requires expansion using keyword 'repeat'}}
func packRefOutsideExpansion<each T>(_: Array<each T>) {}

// expected-error@+1 {{pack reference 'T' requires expansion using keyword 'repeat'}}
func packRefOutsideExpansion<each T>(_: Array<(each T) -> ()>) {}

// expected-error@+1 {{pack reference 'T' requires expansion using keyword 'repeat'}}
func packRefOutsideExpansion<each T>(_: (each T)) {}

// expected-error@+1 {{pack reference 'T' requires expansion using keyword 'repeat'}}
func packRefOutsideExpansion<each T>(_: each T.Type) {}

// expected-error@+1 {{pack reference 'T' requires expansion using keyword 'repeat'}}
func packRefOutsideExpansionRequirement1<each T>(_: repeat each T) where each T: P {}

// expected-error@+1 {{pack reference 'T' requires expansion using keyword 'repeat'}}
func packRefOutsideExpansionRequirement2<each T>(_: repeat each T) where G<each T>: P {}

// coverage to ensure a 'repeat each' type is considered Copyable
func golden<Z>(_ z: Z) {}
func hour<each T>(_ t: repeat each T)  {
  _ = (repeat golden(each t))
}
