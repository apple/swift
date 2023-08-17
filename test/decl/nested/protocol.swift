// RUN: %target-typecheck-verify-swift -parse-as-library

// Protocols cannot be nested inside other types, and types cannot
// be nested inside protocols

struct OuterGeneric<D> {
  protocol InnerProtocol { // expected-error{{protocol 'InnerProtocol' cannot be nested inside another declaration}}
    associatedtype Rooster
    func flip(_ r: Rooster)
    func flop(_ t: D) // expected-error{{cannot find type 'D' in scope}}
  }
}

class OuterGenericClass<T> {
  protocol InnerProtocol { // expected-error{{protocol 'InnerProtocol' cannot be nested inside another declaration}}
    associatedtype Rooster
    func flip(_ r: Rooster)
    func flop(_ t: T) // expected-error{{cannot find type 'T' in scope}}
  }
}

protocol OuterProtocol {
  associatedtype Hen
  protocol InnerProtocol { // expected-error{{protocol 'InnerProtocol' cannot be nested inside another declaration}}
    associatedtype Rooster
    func flip(_ r: Rooster)
    func flop(_ h: Hen) // expected-error{{cannot find type 'Hen' in scope}}
  }
}

struct ConformsToOuterProtocol : OuterProtocol {
  typealias Hen = Int

  func f() { let _ = InnerProtocol.self } // expected-error {{use of protocol 'InnerProtocol' as a type must be written 'any InnerProtocol'}}
}

protocol Racoon {
  associatedtype Stripes
  class Claw<T> { // expected-error{{type 'Claw' cannot be nested in protocol 'Racoon'}}
    func mangle(_ s: Stripes) {}
    // expected-error@-1 {{cannot find type 'Stripes' in scope}}
  }
  struct Fang<T> { // expected-error{{type 'Fang' cannot be nested in protocol 'Racoon'}}
    func gnaw(_ s: Stripes) {}
    // expected-error@-1 {{cannot find type 'Stripes' in scope}}
  }
  enum Fur { // expected-error{{type 'Fur' cannot be nested in protocol 'Racoon'}}
    case Stripes
  }
}

enum SillyRawEnum : SillyProtocol.InnerClass {} // expected-error {{an enum with no cases cannot declare a raw type}}
// expected-error@-1 {{reference to generic type 'SillyProtocol.InnerClass' requires arguments in <...>}}

protocol SillyProtocol {
  class InnerClass<T> {} // expected-error {{type 'InnerClass' cannot be nested in protocol 'SillyProtocol'}}
  // expected-note@-1 {{generic type 'InnerClass' declared here}}
}

// N.B. Redeclaration checks don't see this case because `protocol A` is invalid.
enum OuterEnum {
  protocol C {} // expected-error{{protocol 'C' cannot be nested inside another declaration}}
  case C(C)
}

class OuterClass {
  protocol InnerProtocol : OuterClass { }
  // expected-error@-1{{protocol 'InnerProtocol' cannot be nested inside another declaration}}
}

// 'InnerProtocol' does not inherit the generic parameters of
// 'OtherGenericClass', so the occurrence of 'OtherGenericClass'
// in 'InnerProtocol' is not "in context" with implicitly
// inferred generic arguments <T, U>.
class OtherGenericClass<T, U> { // expected-note {{generic type 'OtherGenericClass' declared here}}
  protocol InnerProtocol : OtherGenericClass { }
  // expected-error@-1{{protocol 'InnerProtocol' cannot be nested inside another declaration}}
  // expected-error@-2{{reference to generic type 'OtherGenericClass' requires arguments in <...>}}
}

protocol SelfDotTest {
  func f(_: Self.Class)
  class Class {}
  // expected-error@-1{{type 'Class' cannot be nested in protocol 'SelfDotTest'}}
}

struct Outer {
  typealias E = NestedValidation.T
  protocol NestedValidation { // expected-error {{protocol 'NestedValidation' cannot be nested inside another declaration}}
    typealias T = A.B
    class A { // expected-error {{type 'A' cannot be nested in protocol 'NestedValidation'}}
      typealias B = Int
    }
  }
}

struct OuterForUFI {
  @usableFromInline
  protocol Inner { // expected-error {{protocol 'Inner' cannot be nested inside another declaration}}
    func req()
  }
}

extension OuterForUFI.Inner {
  public func extMethod() {} // The 'public' puts this in a special path.
}

func testLookup(_ x: OuterForUFI.Inner) {
  x.req()
  x.extMethod()
}

func testLookup<T: OuterForUFI.Inner>(_ x: T) {
  x.req()
  x.extMethod()
}

// rdar://problem/113103854

protocol Q {
  associatedtype A
}

struct OuterNonGeneric {
  protocol P: Q where Self.A == Int {}
  // expected-error@-1 {{protocol 'P' cannot be nested inside another declaration}}
}

func usesProtoWithWhereClause<T: OuterNonGeneric.P>(_: T) {}