// RUN: %target-typecheck-verify-swift -disable-availability-checking -enable-experimental-feature ImplicitSome

// REQUIRES: asserts

protocol Foo { }

var x: any Foo

protocol SelfAsType {
  var x: Self { get } // expected-note{{protocol requires property 'x' with type 'U'; add a stub for conformance}}
}

struct U : SelfAsType { // expected-error{{type 'U' does not conform to protocol 'SelfAsType'}}
  var x: any SelfAsType { self } // expected-note {{candidate has non-matching type 'any SelfAsType'}}
}

protocol HasSelfRequirements {
  func foo(_ x: Self)

  func returnsOwnProtocol() -> any HasSelfRequirements
}

protocol Bar {
  init()

  func bar() -> any Bar
}

func useBarAsType(_ x: any Bar) {}

protocol Pub : Bar { }

func refinementErasure(_ p: any Pub) {
  useBarAsType(p)
}

struct Struct1<T> { }

typealias T1 = Pub & Bar
typealias T2 = any Pub & Bar

protocol HasAssoc {
  associatedtype Assoc
  func foo()
}

do {
  enum MyError: Error {
    case bad(Any)
  }

  func checkIt(_ js: Any) throws {
    switch js {
    case let dbl as any HasAssoc:
      throw MyError.bad(dbl)

    default:
      fatalError("wrong")
    }
  }
}

func testHasAssoc(_ x: Any, _: any HasAssoc) {
  if let p = x as? any HasAssoc {
    p.foo()
  }

  struct ConformingType : HasAssoc {
    typealias Assoc = Int
    func foo() {}

    func method() -> any HasAssoc {}
  }
}

var b: any HasAssoc

protocol P {}
typealias MoreHasAssoc = HasAssoc & P
func testHasMoreAssoc(_ x: Any) {
  if let p = x as? any MoreHasAssoc {
    p.foo()
  }
}

typealias X = Struct1<any Pub & Bar>
_ = Struct1<any Pub & Bar>.self

typealias AliasWhere<T> = T
where T: HasAssoc, T.Assoc == any HasAssoc

struct StructWhere<T>
where T: HasAssoc,
      T.Assoc == any HasAssoc {}

protocol ProtocolWhere where T == any HasAssoc {
  associatedtype T

  associatedtype U: HasAssoc
    where U.Assoc == any HasAssoc
}

extension HasAssoc where Assoc == any HasAssoc {}

func FunctionWhere<T>(_: T)
where T : HasAssoc,
      T.Assoc == any HasAssoc {}

struct SubscriptWhere {
  subscript<T>(_: T) -> Int
  where T : HasAssoc,
        T.Assoc == any HasAssoc {
    get {}
    set {}
  }
}

struct OuterGeneric<T> {
  func contextuallyGenericMethod() where T == any HasAssoc {}
}

func testInvalidAny() {
  struct S: HasAssoc {
    typealias Assoc = Int
    func foo() {}
  }
  let _: any S = S() // expected-error{{'any' has no effect on concrete type 'S'}}

  func generic<T: HasAssoc>(t: T) {
    let _: any T = t // expected-error{{'any' has no effect on type parameter 'T'}}
    let _: any T.Assoc // expected-error {{'any' has no effect on type parameter 'T.Assoc'}}
  }

  let _: any ((S) -> Void) = generic // expected-error{{'any' has no effect on concrete type '(S) -> Void'}}
}

func anyAny() {
  let _: any Any
  let _: any AnyObject
}

protocol P1 {}
protocol P2 {}
protocol P3 {}
do {
  // Test that we don't accidentally misparse an 'any' type as a 'some' type
  // and vice versa.
  let _: P1 & any P2 // expected-error {{'any' should appear at the beginning of a composition}} {{15-19=}} {{10-10=any }}
  let _: any P1 & any P2 // expected-error {{'any' should appear at the beginning of a composition}} {{19-23=}}
  let _: any P1 & P2 & any P3 // expected-error {{'any' should appear at the beginning of a composition}} {{24-28=}}
  let _: any P1 & some P2 // expected-error {{'some' should appear at the beginning of a composition}} {{19-24=}}
  let _: some P1 & any P2
  // expected-error@-1 {{'some' type can only be declared on a single property declaration}}
  // expected-error@-2 {{'any' should appear at the beginning of a composition}} {{20-24=}}
}

struct ConcreteComposition: P1, P2 {}

func testMetatypes() {
  let _: any P1.Type = ConcreteComposition.self
  let _: any (P1 & P2).Type = ConcreteComposition.self
}

func generic<T: any P1>(_ t: T) {} // expected-error {{type 'T' constrained to non-protocol, non-class type 'any P1'}}

public protocol MyError {}

extension MyError {
  static func ~=(lhs: any Error, rhs: Self) -> Bool {
    return true
  }
}

struct Wrapper {
  typealias E = Error
}

func typealiasMemberReferences(metatype: Wrapper.Type) {
  let _: any Wrapper.E.Protocol = metatype.E.self // expected-error{{'any' has no effect on concrete type '(any Wrapper.E).Type' (aka '(any Error).Type')}}
  let _: (any Wrapper.E).Type = metatype.E.self
}

func testAnyTypeExpr() {
  let _: (any P).Type = (any P).self

  func test(_: (any P).Type) {}
  test((any P).self)

  // expected-error@+2 {{expected member name or initializer call after type name}}
  // expected-note@+1 {{use '.self' to reference the type object}}
  let invalid = any P
  test(invalid)

  // Make sure 'any' followed by an identifier
  // on the next line isn't parsed as a type.
  func doSomething() {}

  let any = 10
  let _ = any
  doSomething()
}

func hasInvalidExistential(_: any DoesNotExistIHope) {}
// expected-error@-1 {{cannot find type 'DoesNotExistIHope' in scope}}

protocol Input {
  associatedtype A
}
protocol Output {
  associatedtype A
}

// ImplicitSome feature always expects explicit 'any' and plain protocols now imply 'some'
// Verify existential_requires_any error is no longer produced
typealias OpaqueFunction = (Input) -> Output
func testOpaqueFunctionAlias(fn: OpaqueFunction) {}

typealias ExistentialFunction = (any Input) -> any Output
func testFunctionAlias(fn: ExistentialFunction) {}

typealias Constraint = Input
func testConstraintAlias(x: Constraint) {}

typealias Existential = any Input
func testExistentialAlias(x: Existential, y: any Constraint) {}

// Reject explicit existential types in inheritance clauses
protocol Empty {}

struct S : any Empty {} // expected-error {{inheritance from non-protocol type 'any Empty'}}
class C : any Empty {} // expected-error {{inheritance from non-protocol, non-class type 'any Empty'}}

// FIXME: Diagnostics are not great in the enum case because we confuse this with a raw type

enum E : any Empty { // expected-error {{raw type 'any Empty' is not expressible by a string, integer, or floating-point literal}}
// expected-error@-1 {{'E' declares raw type 'any Empty', but does not conform to RawRepresentable and conformance could not be synthesized}}
// expected-error@-2 {{RawRepresentable conformance cannot be synthesized because raw type 'any Empty' is not Equatable}}
  case hack
}

enum EE : Equatable, any Empty { // expected-error {{raw type 'any Empty' is not expressible by a string, integer, or floating-point literal}}
// expected-error@-1 {{'EE' declares raw type 'any Empty', but does not conform to RawRepresentable and conformance could not be synthesized}}
// expected-error@-2 {{RawRepresentable conformance cannot be synthesized because raw type 'any Empty' is not Equatable}}
// expected-error@-3 {{raw type 'any Empty' must appear first in the enum inheritance clause}}
  case hack
}

func testAnyFixIt() {
  struct ConformingType : HasAssoc {
    typealias Assoc = Int
    func foo() {}

    func method() -> any HasAssoc {}
  }

  let _: any HasAssoc = ConformingType()
  let _: Optional<any HasAssoc> = nil
  let _: any HasAssoc.Type = ConformingType.self
  let _: (any HasAssoc.Type) = ConformingType.self
  let _: ((any HasAssoc.Type)) = ConformingType.self
  let _: (any HasAssoc).Protocol = (any HasAssoc).self
  let _: (any HasAssoc)? = ConformingType()
  let _: (any HasAssoc.Type)? = ConformingType.self
  let _: (any HasAssoc).Protocol? = (any HasAssoc).self

  // expected-error@+1 {{optional 'any' type must be written '(any HasAssoc)?'}}{{10-23=(any HasAssoc)?}}
  let _: any HasAssoc? = nil
  // expected-error@+1 {{optional 'any' type must be written '(any HasAssoc.Type)?'}}{{10-28=(any HasAssoc.Type)?}}
  let _: any HasAssoc.Type? = nil
}

func testNestedMetatype() {
  let _: (any P.Type).Type = (any P.Type).self
  let _: (any (P.Type)).Type = (any P.Type).self
  let _: ((any (P.Type))).Type = (any P.Type).self
}

func testEnumAssociatedValue() {
  enum E {
    case c1((any HasAssoc) -> Void)
  }
}

// https://github.com/apple/swift/issues/58920
typealias Iterator = any IteratorProtocol
var example: any Iterator = 5 // expected-error{{redundant 'any' in type 'any Iterator' (aka 'any any IteratorProtocol')}} {{14-18=}}
// expected-error@-1{{value of type 'Int' does not conform to specified type 'IteratorProtocol'}}
var example1: any (any IteratorProtocol) = 5 // expected-error{{redundant 'any' in type 'any (any IteratorProtocol)'}} {{15-19=}}
// expected-error@-1{{value of type 'Int' does not conform to specified type 'IteratorProtocol'}}

protocol PP {}
struct A : PP {}
let _: any PP = A() // Ok
let _: any (any PP) = A() // expected-error{{redundant 'any' in type 'any (any PP)'}} {{8-12=}}
