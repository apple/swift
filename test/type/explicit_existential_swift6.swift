// RUN: %target-typecheck-verify-swift -enable-upcoming-feature ExistentialAny
// RUN: %target-typecheck-verify-swift -enable-upcoming-feature ExistentialAny -enable-experimental-feature NoncopyableGenerics

protocol HasSelfRequirements {
  func foo(_ x: Self)

  func returnsOwnProtocol() -> any HasSelfRequirements
}
protocol Bar {
  init()

  func bar() -> any Bar
}

class Bistro{
    convenience init(_: Bar){ self.init()} // expected-error{{use of protocol 'Bar' as a type must be written 'any Bar'}}{{25-28=any Bar}}
    class func returnBar() -> Bar {} // expected-error {{use of protocol 'Bar' as a type must be written 'any Bar'}}{{31-34=any Bar}}
}

func useBarAsType(_ x: any Bar) {}

protocol Pub : Bar { }

func refinementErasure(_ p: any Pub) {
  useBarAsType(p)
}

typealias Compo = HasSelfRequirements & Bar

struct CompoAssocType {
  typealias Compo = HasSelfRequirements & Bar
}

func useAsRequirement<T: HasSelfRequirements>(_ x: T) { }
func useCompoAsRequirement<T: HasSelfRequirements & Bar>(_ x: T) { }
func useCompoAliasAsRequirement<T: Compo>(_ x: T) { }
func useNestedCompoAliasAsRequirement<T: CompoAssocType.Compo>(_ x: T) { }

func useAsWhereRequirement<T>(_ x: T) where T: HasSelfRequirements {}
func useCompoAsWhereRequirement<T>(_ x: T) where T: HasSelfRequirements & Bar {}
func useCompoAliasAsWhereRequirement<T>(_ x: T) where T: Compo {}
func useNestedCompoAliasAsWhereRequirement<T>(_ x: T) where T: CompoAssocType.Compo {}

func useAsType(_: any HasSelfRequirements,
               _: any HasSelfRequirements & Bar,
               _: any Compo,
               _: any CompoAssocType.Compo) { }

struct TypeRequirement<T: HasSelfRequirements> {}
struct CompoTypeRequirement<T: HasSelfRequirements & Bar> {}
struct CompoAliasTypeRequirement<T: Compo> {}
struct NestedCompoAliasTypeRequirement<T: CompoAssocType.Compo> {}

struct CompoTypeWhereRequirement<T> where T: HasSelfRequirements & Bar {}
struct CompoAliasTypeWhereRequirement<T> where T: Compo {}
struct NestedCompoAliasTypeWhereRequirement<T> where T: CompoAssocType.Compo {}

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
    func existentialArray() ->  [any HasAssoc] {}
    func existentialcSequence() ->  any Sequence<HasAssoc> {}
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

protocol Collection<T> {
  associatedtype T
}

struct TestParameterizedProtocol<T> : Collection {
  typealias T = T

  let x : Collection<T> // expected-error {{use of protocol 'Collection<T>' as a type must be written 'any Collection<T>'}}
}

func acceptAny(_: Collection<Int>) {}
// expected-error@-1 {{use of protocol 'Collection<Int>' as a type must be written 'any Collection<Int>'}}
func returnsAny() -> Collection<Int> {}
// expected-error@-1 {{use of protocol 'Collection<Int>' as a type must be written 'any Collection<Int>'}}

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

protocol RawRepresentable {
  associatedtype RawValue
  var rawValue: RawValue { get }
}

enum E1: RawRepresentable {
  typealias RawValue = P1

  var rawValue: P1 { // expected-error {{use of protocol 'P1' as a type must be written 'any P1'}}{{17-19=any P1}}
    return ConcreteComposition()
  }
}

enum E2: RawRepresentable {
  typealias RawValue = any P1

  var rawValue: any P1 {
    return ConcreteComposition()
  }
}

public protocol MyError {}

extension MyError {
  static func ~=(lhs: any Error, rhs: Self) -> Bool {
    return true
  }
}

func testAnyTypeExpr() {
  let _: (any P).Type = (any P).self
  let _: (any P1 & P2).Type = (any P1 & P2).self

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
protocol InputB {
  associatedtype B
}

protocol Output {
  associatedtype A
}

// expected-error@+2{{use of protocol 'Input' as a type must be written 'any Input'}}{{30-35=any Input}}
// expected-error@+1{{use of protocol 'Output' as a type must be written 'any Output'}}{{40-46=any Output}}
typealias InvalidFunction = (Input) -> Output
func testInvalidFunctionAlias(fn: InvalidFunction) {}

typealias ExistentialFunction = (any Input) -> any Output
func testFunctionAlias(fn: ExistentialFunction) {}

typealias Constraint = Input
typealias ConstraintB = Input & InputB

//expected-error@+2{{use of 'Constraint' (aka 'Input') as a type must be written 'any Constraint' (aka 'any Input')}}
//expected-error@+1 {{use of 'ConstraintB' (aka 'Input & InputB') as a type must be written 'any ConstraintB' (aka 'any Input & InputB')}}
func testConstraintAlias(x: Constraint, y: ConstraintB) {}

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

// Protocols from a serialized module (the standard library).
do {
  // expected-error@+1 {{use of protocol 'Decodable' as a type must be written 'any Decodable'}}
  let _: Decodable
  // expected-error@+1 {{use of 'Codable' (aka 'Decodable & Encodable') as a type must be written 'any Codable' (aka 'any Decodable & Encodable')}}
  let _: Codable
}

func testAnyFixIt() {
  struct ConformingType : HasAssoc {
    typealias Assoc = Int
    func foo() {}

    func method() -> any HasAssoc {}
  }

  // expected-error@+1 {{use of protocol 'HasAssoc' as a type must be written 'any HasAssoc'}}{{10-18=any HasAssoc}}
  let _: HasAssoc = ConformingType()
  // expected-error@+1 {{use of protocol 'HasAssoc' as a type must be written 'any HasAssoc'}}{{19-27=any HasAssoc}}
  let _: Optional<HasAssoc> = nil
  // expected-error@+1 {{use of protocol 'HasAssoc' as a type must be written 'any HasAssoc'}}{{10-23=any HasAssoc.Type}}
  let _: HasAssoc.Type = ConformingType.self
  // expected-error@+1 {{use of protocol 'HasAssoc' as a type must be written 'any HasAssoc'}}{{10-25=any (HasAssoc).Type}}
  let _: (HasAssoc).Type = ConformingType.self
  // expected-error@+1 {{use of protocol 'HasAssoc' as a type must be written 'any HasAssoc'}}{{10-27=any ((HasAssoc)).Type}}
  let _: ((HasAssoc)).Type = ConformingType.self
  // expected-error@+2 {{use of protocol 'HasAssoc' as a type must be written 'any HasAssoc'}}{{10-18=(any HasAssoc)}}
  // expected-error@+1 {{use of protocol 'HasAssoc' as a type must be written 'any HasAssoc'}}{{30-38=(any HasAssoc)}}
  let _: HasAssoc.Protocol = HasAssoc.self
  do {
    struct Wrapper {
      typealias HasAssocAlias = HasAssoc
    }
    let wrapperMeta: Wrapper.Type
    // FIXME: What is the correct fix-it for the initializer?
    //
    // expected-error@+2:20 {{use of 'Wrapper.HasAssocAlias' (aka 'HasAssoc') as a type must be written 'any Wrapper.HasAssocAlias' (aka 'any HasAssoc')}}{{12-33=(any Wrapper.HasAssocAlias)}}
    // expected-error@+1:57 {{use of 'Wrapper.HasAssocAlias' (aka 'HasAssoc') as a type must be written 'any Wrapper.HasAssocAlias' (aka 'any HasAssoc')}}{{57-70=(any HasAssocAlias)}}
    let _: Wrapper.HasAssocAlias.Protocol = wrapperMeta.HasAssocAlias.self
  }
  // expected-error@+1 {{use of protocol 'HasAssoc' as a type must be written 'any HasAssoc'}}{{11-19=any HasAssoc}}
  let _: (HasAssoc).Protocol = (any HasAssoc).self
  // expected-error@+1 {{use of protocol 'HasAssoc' as a type must be written 'any HasAssoc'}}{{10-18=(any HasAssoc)}}
  let _: HasAssoc? = ConformingType()
  // expected-error@+1 {{use of protocol 'HasAssoc' as a type must be written 'any HasAssoc'}}{{10-23=(any HasAssoc.Type)}}
  let _: HasAssoc.Type? = ConformingType.self
  // expected-error@+1 {{use of protocol 'HasAssoc' as a type must be written 'any HasAssoc'}}{{10-18=(any HasAssoc)}}
  let _: HasAssoc.Protocol? = (any HasAssoc).self

  // expected-error@+1 {{optional 'any' type must be written '(any HasAssoc)?'}}{{10-23=(any HasAssoc)?}}
  let _: any HasAssoc? = nil
  // expected-error@+1 {{optional 'any' type must be written '(any HasAssoc.Type)?'}}{{10-28=(any HasAssoc.Type)?}}
  let _: any HasAssoc.Type? = nil

  do {
    struct Outer<T> {
      struct Inner<U> {}
    }

    // expected-error@+2:18 {{must be written 'any HasAssoc'}}
    // expected-error@+1:34 {{must be written 'any HasAssoc'}}
    let _: Outer<HasAssoc>.Inner<HasAssoc>
  }
}

func testNestedMetatype() {
  let _: (any P.Type).Type = (any P.Type).self
  let _: (any (P.Type)).Type = (any P.Type).self
  let _: ((any (P.Type))).Type = (any P.Type).self
}

func testEnumAssociatedValue() {
  enum E {
    case c1((any HasAssoc) -> Void)
    // expected-error@+1 {{use of protocol 'HasAssoc' as a type must be written 'any HasAssoc'}}
    case c2((HasAssoc) -> Void)
    // expected-error@+1 {{use of protocol 'P' as a type must be written 'any P'}}
    case c3((P) -> Void)
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

// coverage for rdar://123332844
let x: Any.Type = AnyObject.self
let y: Any.Type = Any.self

typealias Objectlike = AnyObject
func f(_ x: Objectlike) {}

typealias Copy = Copyable
func h(_ z1: Copy, // expected-error {{use of 'Copy' (aka 'Copyable') as a type must be written 'any Copy' (aka 'any Copyable')}}
       _ z2: Copyable) {} // expected-error {{use of protocol 'Copyable' as a type must be written 'any Copyable'}}
