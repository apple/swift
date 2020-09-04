// RUN: %target-typecheck-verify-swift -swift-version 4
// not ready: dont_run: %target-typecheck-verify-swift -enable-astscope-lookup -swift-version 4

// Types in expression contexts must be followed by a member access or
// constructor call.

struct Foo {
  struct Bar {
    init() {}
    static var prop: Int = 0
    static func meth() {}
    func instMeth() {}
  }
  init() {}
  static var prop: Int = 0
  static func meth() {}
  func instMeth() {}
}

protocol Zim {
  associatedtype Zang

  init()
  // TODO class var prop: Int { get }
  static func meth() {} // expected-error{{protocol methods must not have bodies}}
  func instMeth() {} // expected-error{{protocol methods must not have bodies}}
}

protocol Bad {
  init() {} // expected-error{{protocol initializers must not have bodies}}
}

struct Gen<T> {
  struct Bar {
    init() {}
    static var prop: Int { return 0 }
    static func meth() {}
    func instMeth() {}
  }

  init() {}
  static var prop: Int { return 0 }
  static func meth() {}
  func instMeth() {}
}

func unqualifiedType() {
  _ = Foo.self
  _ = Foo.self
  _ = Foo()
  _ = Foo.prop
  _ = Foo.meth
  let _ : () = Foo.meth()
  _ = Foo.instMeth

  _ = Foo // expected-error{{expected member name or constructor call after type name}} expected-note{{add arguments}} {{10-10=()}} expected-note{{use '.self'}} {{10-10=.self}}
  _ = Foo.dynamicType // expected-error {{type 'Foo' has no member 'dynamicType'}}
                      // expected-error@-1 {{'.dynamicType' is deprecated. Use 'type(of: ...)' instead}} {{7-7=type(of: }} {{10-22=)}}

  _ = Bad // expected-error{{expected member name or constructor call after type name}}
  // expected-note@-1{{use '.self' to reference the type object}}{{10-10=.self}}
}

func qualifiedType() {
  _ = Foo.Bar.self
  let _ : Foo.Bar.Type = Foo.Bar.self
  let _ : Foo.Protocol = Foo.self // expected-error{{cannot use 'Protocol' with non-protocol type 'Foo'}}
  _ = Foo.Bar()
  _ = Foo.Bar.prop
  _ = Foo.Bar.meth
  let _ : () = Foo.Bar.meth()
  _ = Foo.Bar.instMeth

  _ = Foo.Bar // expected-error{{expected member name or constructor call after type name}} expected-note{{add arguments}} {{14-14=()}} expected-note{{use '.self'}} {{14-14=.self}}
  _ = Foo.Bar.dynamicType // expected-error {{type 'Foo.Bar' has no member 'dynamicType'}}
                          // expected-error@-1 {{'.dynamicType' is deprecated. Use 'type(of: ...)' instead}} {{7-7=type(of: }} {{14-26=)}}
}

// We allow '.Type' in expr context
func metaType() {
  let _ = Foo.Type.self // expected-warning{{self expression applied to a metatype results in a meta-metatype, which may be unexpected}} {{15-19=}}
  // expected-note@-1 {{add parentheses to silence this warning}}

  let _ = Foo.Type // expected-error{{expected member name or constructor call after type name}}
  // expected-note@-1 {{use '.self' to reference the type object}}

  let _ = type(of: Foo.Type) // expected-error{{expected member name or constructor call after type name}}
  // expected-note@-1 {{use '.self' to reference the type object}}
}

func genType() {
  _ = Gen<Foo>.self
  _ = Gen<Foo>()
  _ = Gen<Foo>.prop
  _ = Gen<Foo>.meth
  let _ : () = Gen<Foo>.meth()
  _ = Gen<Foo>.instMeth
  _ = Gen<Foo> // expected-error{{expected member name or constructor call after type name}}
               // expected-note@-1{{use '.self' to reference the type object}}
               // expected-note@-2{{add arguments after the type to construct a value of the type}}
}

func genQualifiedType() {
  _ = Gen<Foo>.Bar.self
  _ = Gen<Foo>.Bar()
  _ = Gen<Foo>.Bar.prop
  _ = Gen<Foo>.Bar.meth
  let _ : () = Gen<Foo>.Bar.meth()
  _ = Gen<Foo>.Bar.instMeth

  _ = Gen<Foo>.Bar // expected-error{{expected member name or constructor call after type name}}
                   // expected-note@-1{{add arguments after the type to construct a value of the type}}
                   // expected-note@-2{{use '.self' to reference the type object}}
  _ = Gen<Foo>.Bar.dynamicType // expected-error {{type 'Gen<Foo>.Bar' has no member 'dynamicType'}}
                               // expected-error@-1 {{'.dynamicType' is deprecated. Use 'type(of: ...)' instead}} {{7-7=type(of: }} {{19-31=)}}
}

func typeOfShadowing() {
  // Try to shadow type(of:)
  func type<T>(of t: T.Type, flag: Bool) -> T.Type { // expected-note {{'type(of:flag:)' declared here}}
    return t
  }

  func type<T, U>(of t: T.Type, _ : U) -> T.Type {
    return t
  }
  
  func type<T>(_ t: T.Type) -> T.Type {
    return t
  }

  func type<T>(fo t: T.Type) -> T.Type {
    return t
  }

  _ = type(of: Gen<Foo>.Bar) // expected-error{{missing argument for parameter 'flag' in call}} {{28-28=, flag: <#Bool#>}}
  _ = type(Gen<Foo>.Bar) // expected-error{{expected member name or constructor call after type name}}
  // expected-note@-1{{add arguments after the type to construct a value of the type}}
  // expected-note@-2{{use '.self' to reference the type object}}
  _ = type(of: Gen<Foo>.Bar.self, flag: false) // No error here.
  _ = type(fo: Foo.Bar.self) // No error here.
  _ = type(of: Foo.Bar.self, [1, 2, 3]) // No error here.
}

func archetype<T: Zim>(_: T) {
  _ = T.self
  _ = T()
  // TODO let prop = T.prop
  _ = T.meth
  let _ : () = T.meth()

  _ = T // expected-error{{expected member name or constructor call after type name}} expected-note{{add arguments}} {{8-8=()}} expected-note{{use '.self'}} {{8-8=.self}}
}

func assocType<T: Zim>(_: T) where T.Zang: Zim {
  _ = T.Zang.self
  _ = T.Zang()
  // TODO _ = T.Zang.prop
  _ = T.Zang.meth
  let _ : () = T.Zang.meth()

  _ = T.Zang // expected-error{{expected member name or constructor call after type name}} expected-note{{add arguments}} {{13-13=()}} expected-note{{use '.self'}} {{13-13=.self}}
}

class B {
  class func baseMethod() {}
}
class D: B {
  class func derivedMethod() {}
}

func derivedType() {
  let _: B.Type = D.self
  _ = D.baseMethod
  let _ : () = D.baseMethod()

  let _: D.Type = D.self
  _ = D.derivedMethod
  let _ : () = D.derivedMethod()

  let _: B.Type = D // expected-error{{expected member name or constructor call after type name}} expected-note{{add arguments}} {{20-20=()}} expected-note{{use '.self'}} {{20-20=.self}}
  let _: D.Type = D // expected-error{{expected member name or constructor call after type name}} expected-note{{add arguments}} {{20-20=()}} expected-note{{use '.self'}} {{20-20=.self}}
}

// Referencing a nonexistent member or constructor should not trigger errors
// about the type expression.
func nonexistentMember() {
  let cons = Foo("this constructor does not exist") // expected-error{{argument passed to call that takes no arguments}}
  let prop = Foo.nonexistent // expected-error{{type 'Foo' has no member 'nonexistent'}}
  let meth = Foo.nonexistent() // expected-error{{type 'Foo' has no member 'nonexistent'}}
}

protocol P {}

func meta_metatypes() {
  let _: P.Protocol = P.self
  _ = P.Type.self // expected-warning{{self expression applied to a metatype results in a meta-metatype, which may be unexpected}} {{9-13=}}
  // expected-note@-1 {{add parentheses to silence this warning}}
  _ = P.Protocol.self // expected-warning{{self expression applied to a metatype results in a meta-metatype, which may be unexpected}} {{9-17=}}
  // expected-note@-1 {{add parentheses to silence this warning}}
  _ = P.Protocol.Protocol.self // expected-error{{cannot use 'Protocol' with non-protocol type 'P.Protocol'}}
  _ = P.Protocol.Type.self // expected-warning{{self expression applied to a metatype results in a meta-metatype, which may be unexpected}} {{18-22=}}
  // expected-note@-1 {{add parentheses to silence this warning}}
  _ = (B.Type).self
}

class E {
  private init() {}
}

func inAccessibleInit() {
  _ = E // expected-error {{expected member name or constructor call after type name}} expected-note {{use '.self'}} {{8-8=.self}}
}

enum F: Int {
  case A, B
}

struct G {
  var x: Int
}

func implicitInit() {
  _ = F // expected-error {{expected member name or constructor call after type name}} expected-note {{add arguments}} {{8-8=()}} expected-note {{use '.self'}} {{8-8=.self}}
  _ = G // expected-error {{expected member name or constructor call after type name}} expected-note {{add arguments}} {{8-8=()}} expected-note {{use '.self'}} {{8-8=.self}}
}

// https://bugs.swift.org/browse/SR-502
func testFunctionCollectionTypes() {
  _ = [(Int) -> Int]()
  _ = [(Int, Int) -> Int]()
  _ = [(x: Int, y: Int) -> Int]()
  // Make sure associativity is correct
  let a = [(Int) -> (Int) -> Int]()
  let b: Int = a[0](5)(4)

  _ = [String: (Int) -> Int]()
  _ = [String: (Int, Int) -> Int]()

  _ = [1 -> Int]() // expected-error {{expected type before '->'}}
  _ = [Int -> 1]() // expected-error {{expected type after '->'}}
    // expected-error@-1 {{single argument function types require parentheses}}

  // Should parse () as void type when before or after arrow
  _ = [() -> Int]()
  _ = [(Int) -> ()]()

  _ = 2 + () -> Int // expected-error {{expected type before '->'}}
  _ = () -> (Int, Int).2 // expected-error {{expected type after '->'}}
  _ = (Int) -> Int // expected-error {{expected member name or constructor call after type name}} expected-note{{use '.self' to reference the type object}}

  _ = @convention(c) () -> Int // expected-error{{expected member name or constructor call after type name}} expected-note{{use '.self' to reference the type object}}
  _ = 1 + (@convention(c) () -> Int).self // expected-error{{cannot convert value of type '(@convention(c) () -> Int).Type' to expected argument type 'Int'}}
  _ = (@autoclosure () -> Int) -> (Int, Int).2 // expected-error {{expected type after '->'}}
  _ = ((@autoclosure () -> Int) -> (Int, Int)).1 // expected-error {{type '(@autoclosure () -> Int) -> (Int, Int)' has no member '1'}}
  _ = ((inout Int) -> Void).self

  _ = [(Int) throws -> Int]()
  _ = [@convention(swift) (Int) throws -> Int]().count
  _ = [(inout Int) throws -> (inout () -> Void) -> Void]().count
  _ = [String: (@autoclosure (Int) -> Int32) -> Void]().keys // expected-error {{argument type of @autoclosure parameter must be '()'}}
  let _ = [(Int) -> throws Int]() // expected-error{{'throws' may only occur before '->'}}
  let _ = [Int throws Int](); // expected-error{{'throws' may only occur before '->'}} expected-error {{consecutive statements on a line must be separated by ';'}}
}

protocol P1 {}
protocol P2 {}
protocol P3 {}
func compositionType() {
  _ = P1 & P2 // expected-error {{expected member name or constructor call after type name}} expected-note{{use '.self'}} {{7-7=(}} {{14-14=).self}}
  _ = P1 & P2.self // expected-error {{binary operator '&' cannot be applied to operands of type 'P1.Protocol' and 'P2.Protocol'}}
  _ = (P1 & P2).self // Ok.
  _ = (P1 & (P2)).self // FIXME: OK? while `typealias P = P1 & (P2)` is rejected.
  _ = (P1 & (P2, P3)).self // expected-error {{non-protocol, non-class type '(P2, P3)' cannot be used within a protocol-constrained type}}
  _ = (P1 & Int).self // expected-error {{non-protocol, non-class type 'Int' cannot be used within a protocol-constrained type}}
  _ = (P1? & P2).self // expected-error {{non-protocol, non-class type 'P1?' cannot be used within a protocol-constrained type}}

  _ = (P1 & P2.Type).self // expected-error {{non-protocol, non-class type 'P2.Type' cannot be used within a protocol-constrained type}}

  _ = P1 & P2 -> P3
  // expected-error @-1 {{single argument function types require parentheses}} {{7-7=(}} {{14-14=)}}
  // expected-error @-2 {{expected member name or constructor call after type name}}
  // expected-note @-3 {{use '.self'}} {{7-7=(}} {{20-20=).self}}

  _ = P1 & P2 -> P3 & P1 -> Int
  // expected-error @-1 {{single argument function types require parentheses}} {{18-18=(}} {{25-25=)}}
  // expected-error @-2 {{single argument function types require parentheses}} {{7-7=(}} {{14-14=)}}
  // expected-error @-3 {{expected member name or constructor call after type name}}
  // expected-note @-4 {{use '.self'}} {{7-7=(}} {{32-32=).self}}

  _ = (() -> P1 & P2).self // Ok
  _ = (P1 & P2 -> P3 & P2).self // expected-error {{single argument function types require parentheses}} {{8-8=(}} {{15-15=)}}
  _ = ((P1 & P2) -> (P3 & P2) -> P1 & Any).self // Ok
}

func complexSequence() {
  // (assign_expr
  //   (discard_assignment_expr)
  //   (try_expr
  //     (type_expr typerepr='P1 & P2 throws -> P3 & P1')))
  _ = try P1 & P2 throws -> P3 & P1
  // expected-warning @-1 {{no calls to throwing functions occur within 'try' expression}}
  // expected-error @-2 {{single argument function types require parentheses}} {{11-11=(}} {{18-18=)}}
  // expected-error @-3 {{expected member name or constructor call after type name}}
  // expected-note @-4 {{use '.self' to reference the type object}} {{11-11=(}} {{36-36=).self}}
}

func takesVoid(f: Void -> ()) {} // expected-error {{single argument function types require parentheses}} {{19-23=()}}

func takesOneArg<T>(_: T.Type) {}
func takesTwoArgs<T>(_: T.Type, _: Int) {}

func testMissingSelf() {
  // None of these were not caught in Swift 3.
  // See test/Compatibility/type_expr.swift.

  takesOneArg(Int)
  // expected-error@-1 {{expected member name or constructor call after type name}}
  // expected-note@-2 {{add arguments after the type to construct a value of the type}}
  // expected-note@-3 {{use '.self' to reference the type object}}

  takesOneArg(Swift.Int)
  // expected-error@-1 {{expected member name or constructor call after type name}}
  // expected-note@-2 {{add arguments after the type to construct a value of the type}}
  // expected-note@-3 {{use '.self' to reference the type object}}

  takesTwoArgs(Int, 0)
  // expected-error@-1 {{expected member name or constructor call after type name}}
  // expected-note@-2 {{add arguments after the type to construct a value of the type}}
  // expected-note@-3 {{use '.self' to reference the type object}}

  takesTwoArgs(Swift.Int, 0)
  // expected-error@-1 {{expected member name or constructor call after type name}}
  // expected-note@-2 {{add arguments after the type to construct a value of the type}}
  // expected-note@-3 {{use '.self' to reference the type object}}

  Swift.Int // expected-warning {{expression of type 'Int.Type' is unused}}
  // expected-error@-1 {{expected member name or constructor call after type name}}
  // expected-note@-2 {{add arguments after the type to construct a value of the type}}
  // expected-note@-3 {{use '.self' to reference the type object}}

  _ = Swift.Int
  // expected-error@-1 {{expected member name or constructor call after type name}}
  // expected-note@-2 {{add arguments after the type to construct a value of the type}}
  // expected-note@-3 {{use '.self' to reference the type object}}
}
