// RUN: %target-swift-frontend -disable-availability-checking -typecheck -verify %s

protocol P {
  func paul()
  mutating func priscilla()
}
protocol Q { func quinn() }
extension Int: P, Q { func paul() {}; mutating func priscilla() {}; func quinn() {} }
extension String: P, Q { func paul() {}; mutating func priscilla() {}; func quinn() {} }
extension Array: P, Q { func paul() {}; mutating func priscilla() {}; func quinn() {} }

class C {}
class D: C, P, Q { func paul() {}; func priscilla() {}; func quinn() {} }

let property: some P = 1
let deflessLet: some P // expected-error{{has no initializer}} {{educational-notes=opaque-type-inference}}
var deflessVar: some P // expected-error{{has no initializer}}

struct GenericProperty<T: P> {
  var x: T
  var property: some P {
    return x
  }
}

let (bim, bam): some P = (1, 2) // expected-error{{'some' type can only be declared on a single property declaration}}
var computedProperty: some P {
  get { return 1 }
  set { _ = newValue + 1 } // expected-error{{cannot convert value of type 'some P' to expected argument type 'Int'}}
}
struct SubscriptTest {
  subscript(_ x: Int) -> some P {
    return x
  }
}

func bar() -> some P {
  return 1
}
func bas() -> some P & Q {
  return 1
}
func zim() -> some C {
  return D()
}
func zang() -> some C & P & Q {
  return D()
}
func zung() -> some AnyObject {
  return D()
}
func zoop() -> some Any {
  return D()
}
func zup() -> some Any & P {
  return D()
}
func zip() -> some AnyObject & P {
  return D()
}
func zorp() -> some Any & C & P {
  return D()
}
func zlop() -> some C & AnyObject & P {
  return D()
}

// Don't allow opaque types to propagate by inference into other global decls'
// types
struct Test {
  let inferredOpaque = bar() // expected-error{{inferred type}}
  let inferredOpaqueStructural = Optional(bar()) // expected-error{{inferred type}}
  let inferredOpaqueStructural2 = (bar(), bas()) // expected-error{{inferred type}}
}

let zingle = {() -> some P in 1 } // expected-error{{'some' types are only implemented}}

// Invalid positions

typealias Foo = some P // expected-error{{'some' types are only implemented}}

func blibble(blobble: some P) {} // expected-error{{'some' types are only implemented}}

let blubble: () -> some P = { 1 } // expected-error{{'some' types are only implemented}}

func blib() -> P & some Q { return 1 } // expected-error{{'some' should appear at the beginning}}
func blab() -> (P, some Q) { return (1, 2) } // expected-error{{'some' types are only implemented}}
func blob() -> (some P) -> P { return { $0 } } // expected-error{{'some' types are only implemented}}
func blorb<T: some P>(_: T) { } // expected-error{{'some' types are only implemented}}
func blub<T>() -> T where T == some P { return 1 } // expected-error{{'some' types are only implemented}} expected-error{{cannot convert}}

protocol OP: some P {} // expected-error{{'some' types are only implemented}}

func foo() -> some P {
  let x = (some P).self // expected-error*{{}}
  return 1
}

// Invalid constraints

let zug: some Int = 1 // FIXME expected-error{{must specify only}}
let zwang: some () = () // FIXME expected-error{{must specify only}}
let zwoggle: some (() -> ()) = {} // FIXME expected-error{{must specify only}}

// Type-checking of expressions of opaque type

func alice() -> some P { return 1 }
func bob() -> some P { return 1 }

func grace<T: P>(_ x: T) -> some P { return x }

func typeIdentity() {
  do {
    var a = alice()
    a = alice()
    a = bob() // expected-error{{}}
    a = grace(1) // expected-error{{}}
    a = grace("two") // expected-error{{}}
  }

  do {
    var af = alice
    af = alice
    af = bob // expected-error{{}}
    af = grace // expected-error{{generic parameter 'T' could not be inferred}}
    // expected-error@-1 {{cannot assign value of type '(T) -> some P' to type '() -> some P'}}
  }

  do {
    var b = bob()
    b = alice() // expected-error{{}}
    b = bob()
    b = grace(1) // expected-error{{}}
    b = grace("two") // expected-error{{}}
  }

  do {
    var gi = grace(1)
    gi = alice() // expected-error{{}}
    gi = bob() // expected-error{{}}
    gi = grace(2)
    gi = grace("three") // expected-error{{}}
  }

  do {
    var gs = grace("one")
    gs = alice() // expected-error{{}}
    gs = bob() // expected-error{{}}
    gs = grace(2) // expected-error{{}}
    gs = grace("three")
  }

  // The opaque type should conform to its constraining protocols
  do {
    let gs = grace("one")
    var ggs = grace(gs)
    ggs = grace(gs)
  }

  // The opaque type should expose the members implied by its protocol
  // constraints
  do {
    var a = alice()
    a.paul()
    a.priscilla()
  }
}

func recursion(x: Int) -> some P {
  if x == 0 {
    return 0
  }
  return recursion(x: x - 1)
}

func noReturnStmts() -> some P {} // expected-error {{function declares an opaque return type, but has no return statements in its body from which to infer an underlying type}} {{educational-notes=opaque-type-inference}}

func returnUninhabited() -> some P { // expected-note {{opaque return type declared here}}
    fatalError() // expected-error{{return type of global function 'returnUninhabited()' requires that 'Never' conform to 'P'}}
}

func mismatchedReturnTypes(_ x: Bool, _ y: Int, _ z: String) -> some P { // expected-error{{do not have matching underlying types}} {{educational-notes=opaque-type-inference}}
  if x {
    return y // expected-note{{underlying type 'Int'}}
  } else {
    return z // expected-note{{underlying type 'String'}}
  }
}

var mismatchedReturnTypesProperty: some P { // expected-error{{do not have matching underlying types}}
  if true {
    return 0 // expected-note{{underlying type 'Int'}}
  } else {
    return "" // expected-note{{underlying type 'String'}}
  }
}

struct MismatchedReturnTypesSubscript {
  subscript(x: Bool, y: Int, z: String) -> some P { // expected-error{{do not have matching underlying types}}
    if x {
      return y // expected-note{{underlying type 'Int'}}
    } else {
      return z // expected-note{{underlying type 'String'}}
    }
  }
}

func jan() -> some P {
  return [marcia(), marcia(), marcia()]
}
func marcia() -> some P {
  return [marcia(), marcia(), marcia()] // expected-error{{defines the opaque type in terms of itself}} {{educational-notes=opaque-type-inference}}
}

protocol R {
  associatedtype S: P, Q // expected-note*{{}}

  func r_out() -> S
  func r_in(_: S)
}

extension Int: R {
  func r_out() -> String {
    return ""
  }
  func r_in(_: String) {}
}

func candace() -> some R {
  return 0
}
func doug() -> some R {
  return 0
}

func gary<T: R>(_ x: T) -> some R {
  return x
}

func sameType<T>(_: T, _: T) {}

func associatedTypeIdentity() {
  let c = candace()
  let d = doug()

  var cr = c.r_out()
  cr = candace().r_out()
  cr = doug().r_out() // expected-error{{}}

  var dr = d.r_out()
  dr = candace().r_out() // expected-error{{}}
  dr = doug().r_out()

  c.r_in(cr)
  c.r_in(c.r_out())
  c.r_in(dr) // expected-error{{}}
  c.r_in(d.r_out()) // expected-error{{}}

  d.r_in(cr) // expected-error{{}}
  d.r_in(c.r_out()) // expected-error{{}}
  d.r_in(dr)
  d.r_in(d.r_out())

  cr.paul()
  cr.priscilla()
  cr.quinn()
  dr.paul()
  dr.priscilla()
  dr.quinn()

  sameType(cr, c.r_out())
  sameType(dr, d.r_out())
  sameType(cr, dr) // expected-error {{conflicting arguments to generic parameter 'T' ('(some R).S' (associated type of protocol 'R') vs. '(some R).S' (associated type of protocol 'R'))}}
  sameType(gary(candace()).r_out(), gary(candace()).r_out())
  sameType(gary(doug()).r_out(), gary(doug()).r_out())
  // TODO(diagnostics): This is not great but the problem comes from the way solver discovers and attempts bindings, if we could detect that
  // `(some R).S` from first reference to `gary()` in incosistent with the second one based on the parent type of `S` it would be much easier to diagnose.
  sameType(gary(doug()).r_out(), gary(candace()).r_out())
  // expected-error@-1:3  {{conflicting arguments to generic parameter 'T' ('(some R).S' (associated type of protocol 'R') vs. '(some R).S' (associated type of protocol 'R'))}}
  // expected-error@-2:12 {{conflicting arguments to generic parameter 'T' ('some R' (result type of 'doug') vs. 'some R' (result type of 'candace'))}}
  // expected-error@-3:34 {{conflicting arguments to generic parameter 'T' ('some R' (result type of 'doug') vs. 'some R' (result type of 'candace'))}}
}

func redeclaration() -> some P { return 0 } // expected-note 2{{previously declared}}
func redeclaration() -> some P { return 0 } // expected-error{{redeclaration}}
func redeclaration() -> some Q { return 0 } // expected-error{{redeclaration}}
func redeclaration() -> P { return 0 }
func redeclaration() -> Any { return 0 }

var redeclaredProp: some P { return 0 } // expected-note 3{{previously declared}}
var redeclaredProp: some P { return 0 } // expected-error{{redeclaration}}
var redeclaredProp: some Q { return 0 } // expected-error{{redeclaration}}
var redeclaredProp: P { return 0 } // expected-error{{redeclaration}}

struct RedeclarationTest {
  func redeclaration() -> some P { return 0 } // expected-note 2{{previously declared}}
  func redeclaration() -> some P { return 0 } // expected-error{{redeclaration}}
  func redeclaration() -> some Q { return 0 } // expected-error{{redeclaration}}
  func redeclaration() -> P { return 0 }

  var redeclaredProp: some P { return 0 } // expected-note 3{{previously declared}}
  var redeclaredProp: some P { return 0 } // expected-error{{redeclaration}}
  var redeclaredProp: some Q { return 0 } // expected-error{{redeclaration}}
  var redeclaredProp: P { return 0 } // expected-error{{redeclaration}}

  subscript(redeclared _: Int) -> some P { return 0 } // expected-note 2{{previously declared}}
  subscript(redeclared _: Int) -> some P { return 0 } // expected-error{{redeclaration}}
  subscript(redeclared _: Int) -> some Q { return 0 } // expected-error{{redeclaration}}
  subscript(redeclared _: Int) -> P { return 0 }
}

func diagnose_requirement_failures() {
  struct S {
    var foo: some P { return S() } // expected-note {{declared here}}
    // expected-error@-1 {{return type of property 'foo' requires that 'S' conform to 'P'}}

    subscript(_: Int) -> some P { // expected-note {{declared here}}
      return S()
      // expected-error@-1 {{return type of subscript 'subscript(_:)' requires that 'S' conform to 'P'}}
    }

    func bar() -> some P { // expected-note {{declared here}}
      return S()
      // expected-error@-1 {{return type of instance method 'bar()' requires that 'S' conform to 'P'}}
    }

    static func baz(x: String) -> some P { // expected-note {{declared here}}
      return S()
      // expected-error@-1 {{return type of static method 'baz(x:)' requires that 'S' conform to 'P'}}
    }
  }

  func fn() -> some P { // expected-note {{declared here}}
    return S()
    // expected-error@-1 {{return type of local function 'fn()' requires that 'S' conform to 'P'}}
  }
}

func global_function_with_requirement_failure() -> some P { // expected-note {{declared here}}
  return 42 as Double
  // expected-error@-1 {{return type of global function 'global_function_with_requirement_failure()' requires that 'Double' conform to 'P'}}
}

func recursive_func_is_invalid_opaque() {
  func rec(x: Int) -> some P {
    // expected-error@-1 {{function declares an opaque return type, but has no return statements in its body from which to infer an underlying type}}
    if x == 0 {
      return rec(x: 0)
    }
    return rec(x: x - 1)
  }
}

func closure() -> some P {
  _ = {
    return "test"
  }
  return 42
}

protocol HasAssocType {
  associatedtype Assoc

  func assoc() -> Assoc
}

struct GenericWithOpaqueAssoc<T>: HasAssocType {
  func assoc() -> some Any { return 0 }
}

struct OtherGeneric<X, Y, Z> {
  var x: GenericWithOpaqueAssoc<X>.Assoc
  var y: GenericWithOpaqueAssoc<Y>.Assoc
  var z: GenericWithOpaqueAssoc<Z>.Assoc
}

protocol P_51641323 {
  associatedtype T

  var foo: Self.T { get }
}

func rdar_51641323() {
  struct Foo: P_51641323 {
    var foo: some P_51641323 { // expected-note {{required by opaque return type of property 'foo'}}
      {} // expected-error {{type '() -> ()' cannot conform to 'P_51641323'}} expected-note {{only concrete types such as structs, enums and classes can conform to protocols}}
    }
  }
}

// Protocol requirements cannot have opaque return types
protocol OpaqueProtocolRequirement {
  // expected-error@+1 {{cannot be the return type of a protocol requirement}}{{3-3=associatedtype <#AssocType#>\n}}{{20-26=<#AssocType#>}}
  func method() -> some P

  // expected-error@+1 {{cannot be the return type of a protocol requirement}}{{3-3=associatedtype <#AssocType#>\n}}{{13-19=<#AssocType#>}}
  var prop: some P { get }

  // expected-error@+1 {{cannot be the return type of a protocol requirement}}{{3-3=associatedtype <#AssocType#>\n}}{{18-24=<#AssocType#>}}
  subscript() -> some P { get }
}

func testCoercionDiagnostics() {
  var opaque = foo()
  opaque = bar() // expected-error {{cannot assign value of type 'some P' (result of 'bar()') to type 'some P' (result of 'foo()')}} {{none}}
  opaque = () // expected-error {{cannot assign value of type '()' to type 'some P'}} {{none}}
  opaque = computedProperty // expected-error {{cannot assign value of type 'some P' (type of 'computedProperty') to type 'some P' (result of 'foo()')}} {{none}}
  opaque = SubscriptTest()[0] // expected-error {{cannot assign value of type 'some P' (result of 'SubscriptTest.subscript(_:)') to type 'some P' (result of 'foo()')}} {{none}}

  var opaqueOpt: Optional = opaque
  opaqueOpt = bar() // expected-error {{cannot assign value of type 'some P' (result of 'bar()') to type 'some P' (result of 'foo()')}} {{none}}
  opaqueOpt = () // expected-error {{cannot assign value of type '()' to type 'some P'}} {{none}}
}

var globalVar: some P = 17
let globalLet: some P = 38

struct Foo {
  static var staticVar: some P = 17
  static let staticLet: some P = 38

  var instanceVar: some P = 17
  let instanceLet: some P = 38
}

protocol P_52528543 {
  init()

  associatedtype A: Q_52528543

  var a: A { get }
}

protocol Q_52528543 {
  associatedtype B // expected-note 2 {{associated type 'B'}}

  var b: B { get }
}

extension P_52528543 {
  func frob(a_b: A.B) -> some P_52528543 { return self }
}

func foo<T: P_52528543>(x: T) -> some P_52528543 {
  return x
    .frob(a_b: x.a.b)
    .frob(a_b: x.a.b) // expected-error {{cannot convert}}
}

struct GenericFoo<T: P_52528543, U: P_52528543> {
  let x: some P_52528543 = T()
  let y: some P_52528543 = U()

  mutating func bump() {
    var xab = f_52528543(x: x)
    xab = f_52528543(x: y) // expected-error{{cannot assign}}
  }
}

func f_52528543<T: P_52528543>(x: T) -> T.A.B { return x.a.b }

func opaque_52528543<T: P_52528543>(x: T) -> some P_52528543 { return x }

func invoke_52528543<T: P_52528543, U: P_52528543>(x: T, y: U) {
  let x2 = opaque_52528543(x: x)
  let y2 = opaque_52528543(x: y)
  var xab = f_52528543(x: x2)
  xab = f_52528543(x: y2) // expected-error{{cannot assign}}
}

protocol Proto {}

struct I : Proto {}

dynamic func foo<S>(_ s: S) -> some Proto {
  return I()
}

@_dynamicReplacement(for: foo)
func foo_repl<S>(_ s: S) -> some Proto {
 return   I()
}

protocol SomeProtocolA {}
protocol SomeProtocolB {}
struct SomeStructC: SomeProtocolA, SomeProtocolB {}
let someProperty: SomeProtocolA & some SomeProtocolB = SomeStructC() // expected-error {{'some' should appear at the beginning of a composition}}{{35-40=}}{{19-19=some }}

// An opaque result type on a protocol extension member effectively
// contains an invariant reference to 'Self', and therefore cannot
// be referenced on an existential type.

protocol OpaqueProtocol {}
extension OpaqueProtocol {
  var asSome: some OpaqueProtocol { return self }
  func getAsSome() -> some OpaqueProtocol { return self }
  subscript(_: Int) -> some OpaqueProtocol { return self }
}

func takesOpaqueProtocol(existential: OpaqueProtocol) {
  // this is not allowed:
  _ = existential.asSome // expected-error{{member 'asSome' cannot be used on value of protocol type 'OpaqueProtocol'; use a generic constraint instead}}
  _ = existential.getAsSome() // expected-error{{member 'getAsSome' cannot be used on value of protocol type 'OpaqueProtocol'; use a generic constraint instead}}
  _ = existential[0] // expected-error{{member 'subscript' cannot be used on value of protocol type 'OpaqueProtocol'; use a generic constraint instead}}
}

func takesOpaqueProtocol<T : OpaqueProtocol>(generic: T) {
  // these are all OK:
  _ = generic.asSome
  _ = generic.getAsSome()
  _ = generic[0]
}

func opaquePlaceholderFunc() -> some _ { 1 } // expected-error {{placeholder type not allowed here}}
var opaquePlaceholderVar: some _ = 1 // expected-error {{placeholder type not allowed here}}
