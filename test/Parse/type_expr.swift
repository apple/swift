// RUN: %target-typecheck-verify-swift -swift-version 4 -module-name test

// Types in expression contexts must be followed by a member access or
// constructor call.

// Used to check if a type expression resolves to the correct type.
struct CheckType<T> {
  static func matches(_: T.Type) {}
}

protocol P1 {}
protocol P2 {}
protocol P3 {}

struct Foo {
  typealias P1 = test.P1
  typealias P2 = test.P2

  struct Bar {
    struct Baz {}

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
  typealias P1 = test.P1
  typealias P2 = test.P2

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

extension Optional {
  typealias Wrapped = Wrapped

  typealias P1 = test.P1
  typealias P2 = test.P2
}

extension Array {
  typealias P1 = test.P1
  typealias P2 = test.P2
}

extension Dictionary {
  typealias Value = Value

  typealias P1 = test.P1
  typealias P2 = test.P2
}

func unqualifiedType() {
  _ = Foo.self
  _ = Foo()
  _ = Foo.prop
  _ = Foo.meth
  let _ : () = Foo.meth()
  _ = Foo.instMeth

  _ = Foo // expected-error{{expected member name or initializer call after type name}} expected-note{{add arguments}} {{10-10=()}} expected-note{{use '.self'}} {{10-10=.self}}
  _ = Foo.dynamicType // expected-error {{type 'Foo' has no member 'dynamicType'}}

  _ = Bad // expected-error{{expected member name or initializer call after type name}}
  // expected-note@-1{{use '.self' to reference the type object}}{{10-10=.self}}

  CheckType<Foo>.matches((Foo).self)
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

  _ = Foo.Bar // expected-error{{expected member name or initializer call after type name}} expected-note{{add arguments}} {{14-14=()}} expected-note{{use '.self'}} {{14-14=.self}}
  _ = Foo.Bar.dynamicType // expected-error {{type 'Foo.Bar' has no member 'dynamicType'}}

  CheckType<Foo.Bar>.matches((Foo).Bar.self)
  CheckType<Foo.Bar.Baz>.matches(Foo.Bar.Baz.self)
  CheckType<Foo.Bar.Baz>.matches((Foo.Bar).Baz.self)
  CheckType<Foo.Bar.Baz>.matches(((Foo).Bar).Baz.self)
}

// We allow '.Type' in expr context
func metaType() {
  let _ = Foo.Type.self
  let _ = Foo.Type.self

  let _ = Foo.Type // expected-error{{expected member name or initializer call after type name}}
  // expected-note@-1 {{use '.self' to reference the type object}}

  let _ = type(of: Foo.Type) // expected-error{{expected member name or initializer call after type name}}
  // expected-note@-1 {{use '.self' to reference the type object}}
}

func genType() {
  _ = Gen<Foo>.self
  _ = Gen<Foo>()
  _ = Gen<Foo>.prop
  _ = Gen<Foo>.meth
  let _ : () = Gen<Foo>.meth()
  _ = Gen<Foo>.instMeth

  CheckType<Foo?>.matches(Foo?.self)
  CheckType<[Foo]>.matches([Foo].self)
  CheckType<[String : Foo]>.matches([String : Foo].self)

  // Test that 'canParseType()' succeeds for these generic arguments.

  CheckType<Gen<Foo.Bar>>.matches(Gen<(Foo).Bar>.self)

  CheckType<Gen<Foo>>.matches(Gen<Foo?.Wrapped>.self)
  CheckType<Gen<Foo>>.matches(Gen<(Foo)?.Wrapped>.self)
  CheckType<Gen<Foo>>.matches(Gen<(Foo?).Wrapped>.self)
  CheckType<Gen<Foo?>>.matches(Gen<Foo??.Wrapped>.self)
  CheckType<Gen<Foo>>.matches(Gen<Foo?.Wrapped?.Wrapped>.self)
  CheckType<Gen<Foo.Bar>>.matches(Gen<(Foo?.Wrapped).Bar>.self)
  CheckType<Gen<Foo>>.matches(Gen<[Foo].Element>.self)
  CheckType<Gen<Foo>>.matches(Gen<[Int : Foo].Value>.self)

  CheckType<Gen<Any & P1>>.matches(Gen<Any & P1>.self)
  CheckType<Gen<P1 & P2>>.matches(Gen<(P1) & (P2)>.self)
  CheckType<Gen<P1 & P2>>.matches(Gen<(Foo).P1 & (Foo).P2>.self)
  CheckType<Gen<P1 & P2>>.matches(Gen<Foo?.P1 & Foo?.P2>.self)
  CheckType<Gen<P1 & P2>>.matches(Gen<[Foo].P1 & [Foo].P2>.self)
  CheckType<Gen<P1 & P2>>.matches(Gen<[Int : Foo].P1 & [Int : Foo].P2>.self)

  // FIXME?: This needs to go last or else it won't parse as intended.
  _ = Gen<Foo> // expected-error{{expected member name or initializer call after type name}}
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

  _ = Gen<Foo>.Bar // expected-error{{expected member name or initializer call after type name}}
                   // expected-note@-1{{add arguments after the type to construct a value of the type}}
                   // expected-note@-2{{use '.self' to reference the type object}}
  _ = Gen<Foo>.Bar.dynamicType // expected-error {{type 'Gen<Foo>.Bar' has no member 'dynamicType'}}

  CheckType<Gen<Foo>.Bar>.matches((Gen<Foo>).Bar.self)
  CheckType<Foo>.matches(Foo?.Wrapped.self)
  CheckType<Foo>.matches((Foo)?.Wrapped.self)
  CheckType<Foo>.matches((Foo?).Wrapped.self)
  CheckType<Foo>.matches([Foo].Element.self)
  CheckType<Foo>.matches([String : Foo].Value.self)
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
  _ = type(Gen<Foo>.Bar) // expected-error{{expected member name or initializer call after type name}}
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

  _ = T // expected-error{{expected member name or initializer call after type name}} expected-note{{add arguments}} {{8-8=()}} expected-note{{use '.self'}} {{8-8=.self}}
}

func assocType<T: Zim>(_: T) where T.Zang: Zim {
  _ = T.Zang.self
  _ = T.Zang()
  // TODO _ = T.Zang.prop
  _ = T.Zang.meth
  let _ : () = T.Zang.meth()

  _ = T.Zang // expected-error{{expected member name or initializer call after type name}} expected-note{{add arguments}} {{13-13=()}} expected-note{{use '.self'}} {{13-13=.self}}
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

  let _: B.Type = D // expected-error{{expected member name or initializer call after type name}} expected-note{{add arguments}} {{20-20=()}} expected-note{{use '.self'}} {{20-20=.self}}
  let _: D.Type = D // expected-error{{expected member name or initializer call after type name}} expected-note{{add arguments}} {{20-20=()}} expected-note{{use '.self'}} {{20-20=.self}}
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
  _ = P.Type.self
  _ = P.Protocol.self
  _ = P.Protocol.Protocol.self // expected-error{{cannot use 'Protocol' with non-protocol type '(any P).Type'}}
  _ = P.Protocol.Type.self
  _ = B.Type.self
}

class E {
  private init() {}
}

func inAccessibleInit() {
  _ = E // expected-error {{expected member name or initializer call after type name}} expected-note {{use '.self'}} {{8-8=.self}}
}

enum F: Int {
  case A, B
}

struct G {
  var x: Int
}

func implicitInit() {
  _ = F // expected-error {{expected member name or initializer call after type name}} expected-note {{add arguments}} {{8-8=()}} expected-note {{use '.self'}} {{8-8=.self}}
  _ = G // expected-error {{expected member name or initializer call after type name}} expected-note {{add arguments}} {{8-8=()}} expected-note {{use '.self'}} {{8-8=.self}}
}

// https://github.com/apple/swift/issues/43119
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
  _ = (Int) -> Int // expected-error {{expected member name or initializer call after type name}} expected-note{{use '.self' to reference the type object}}

  _ = @convention(c) () -> Int // expected-error{{expected member name or initializer call after type name}} expected-note{{use '.self' to reference the type object}}
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

func compositionType() {
  _ = P1 & P2 // expected-error {{expected member name or initializer call after type name}} expected-note{{use '.self'}} {{7-7=(}} {{14-14=).self}}
  _ = any P1 & P1 // expected-error {{expected member name or initializer call after type name}} expected-note{{use '.self'}} {{7-7=(}} {{18-18=).self}}
  _ = P1 & P2.self // expected-error {{binary operator '&' cannot be applied to operands of type '(any P1).Type' and '(any P2).Type'}}
  _ = (P1 & P2).self // Ok.
  _ = (P1 & (P2)).self // Ok.
  _ = (P1 & (P2, P3)).self // expected-error {{non-protocol, non-class type '(any P2, any P3)' cannot be used within a protocol-constrained type}}
  _ = (P1 & Int).self // expected-error {{non-protocol, non-class type 'Int' cannot be used within a protocol-constrained type}}
  _ = (P1? & P2).self // expected-error {{non-protocol, non-class type '(any P1)?' cannot be used within a protocol-constrained type}}
  _ = (P1 & P2.Type).self // expected-error {{non-protocol, non-class type 'any P2.Type' cannot be used within a protocol-constrained type}}

  CheckType<P1 & P2>.matches(((P1) & (P2)).self)
  CheckType<P1 & P2>.matches((Foo.P1 & Foo.P2).self)
  CheckType<P1 & P2>.matches(((Foo).P1 & (Foo).P2).self)
  CheckType<P1 & P2>.matches((Gen<Foo>.P1 & Gen<Foo>.P2).self)
  CheckType<P1 & P2>.matches((Foo?.P1 & Foo?.P2).self)
  CheckType<P1 & P2>.matches(([Foo].P1 & [Foo].P2).self)
  CheckType<P1 & P2>.matches(([Int : Foo].P1 & [Int : Foo].P2).self)
}

func tupleType() {
  _ = (Foo, Foo)
  // expected-error@-1 {{expected member name or initializer call after type name}}
  // expected-note@-2 {{use '.self' to reference the type object}} {{17-17=.self}}
  _ = (Foo, Foo).self

  CheckType<(Foo, Foo)>.matches((Foo, Foo))
  // expected-error@-1 {{expected member name or initializer call after type name}}
  // expected-note@-2 {{use '.self' to reference the type object}} {{43-43=.self}}

  // Check that we resolve these type expressions correctly.

  CheckType<(Foo, Foo)>.matches((Foo, Foo).self)
  CheckType<(Foo, Foo)>.matches(((Foo), (Foo)).self)

  CheckType<(Foo.Bar, Foo.Bar)>.matches((Foo.Bar, Foo.Bar).self)
  CheckType<(Foo.Bar, Foo.Bar)>.matches(((Foo).Bar, (Foo).Bar).self)

  CheckType<(Gen<Foo>, Gen<Foo>)>.matches((Gen<Foo>, Gen<Foo>).self)
  CheckType<(Foo?, Foo?)>.matches((Foo?, Foo?).self)
  CheckType<([Foo], [Foo])>.matches(([Foo], [Foo]).self)
  CheckType<([Int : Foo], [Int : Foo])>.matches(([Int : Foo], [Int : Foo]).self)

  CheckType<(Gen<Foo>.Bar, Gen<Foo>.Bar)>.matches((Gen<Foo>.Bar, Gen<Foo>.Bar).self)
  CheckType<(Foo, Foo)>.matches((Foo?.Wrapped, Foo?.Wrapped).self)
  CheckType<(Foo, Foo)>.matches(([Foo].Element, [Foo].Element).self)
  CheckType<(Foo, Foo)>.matches(([Int : Foo].Value, [Int : Foo].Value).self)

  CheckType<(Foo.Type, Foo.Type)>.matches((Foo.Type, Foo.Type).self)
  CheckType<(P1.Protocol, P1.Protocol)>.matches((P1.Protocol, P1.Protocol).self)

  CheckType<(P1 & P2, P1 & P2)>.matches((P1 & P2, P1 & P2).self)

  // Trade exhaustivity for one complex test case.
  CheckType<
    (
      (Gen<Foo>.Bar) -> P1 & P2,
      (Foo.Bar, [Int : Foo?].Type),
      [Gen<Foo>.Bar],
      Foo.Bar.Baz
    )
  >.matches(
    (
      (Gen<Foo>.Bar) -> (P1) & Foo?.P2,
      (Foo.Bar, [Int : Foo?].Type),
      [(Gen<Foo>).Bar],
      [Foo.Bar.Baz].Element
    ).self
  )
}

func functionType() {
  _ = Foo -> Foo
  // expected-error@-1 {{single argument function types require parentheses}} {{7-7=(}} {{10-10=)}}
  // expected-error@-2 {{expected member name or initializer call after type name}}
  // expected-note@-3 {{use '.self' to reference the type object}} {{7-7=(}} {{17-17=).self}}
  _ = (Foo) -> Foo
  // expected-error@-1 {{expected member name or initializer call after type name}}
  // expected-note@-2 {{use '.self' to reference the type object}} {{7-7=(}} {{19-19=).self}}
  _ = (Foo) -> Foo -> Foo
  // expected-error@-1 {{single argument function types require parentheses}} {{16-16=(}} {{19-19=)}}
  // expected-error@-2 {{expected member name or initializer call after type name}}
  // expected-note@-3 {{use '.self' to reference the type object}} {{7-7=(}} {{26-26=).self}}
  _ = P1 & P2 -> Foo
  // expected-error @-1 {{single argument function types require parentheses}} {{7-7=(}} {{14-14=)}}
  // expected-error @-2 {{expected member name or initializer call after type name}}
  // expected-note @-3 {{use '.self' to reference the type object}} {{7-7=(}} {{21-21=).self}}
  _ = P1 & P2 -> P3 & P1 -> Foo
  // expected-error @-1 {{single argument function types require parentheses}} {{18-18=(}} {{25-25=)}}
  // expected-error @-2 {{single argument function types require parentheses}} {{7-7=(}} {{14-14=)}}
  // expected-error @-3 {{expected member name or initializer call after type name}}
  // expected-note @-4 {{use '.self'}} {{7-7=(}} {{32-32=).self}}
  _ = (Foo -> Foo).self // expected-error {{single argument function types require parentheses}} {{8-8=(}} {{11-11=)}}
  _ = (P1 & P2 -> P3 & P2).self // expected-error {{single argument function types require parentheses}} {{8-8=(}} {{15-15=)}}

  // Check that we resolve these type expressions correctly.

  CheckType<(Foo) -> Foo>.matches(((Foo) -> Foo).self)
  CheckType<(Foo) -> Foo>.matches((((Foo)) -> (Foo)).self)

  CheckType<(Foo.Bar) -> Foo.Bar>.matches(((Foo.Bar) -> Foo.Bar).self)
  CheckType<(Foo.Bar) -> Foo.Bar>.matches((((Foo).Bar) -> (Foo).Bar).self)

  CheckType<(Gen<Foo>) -> Gen<Foo>>.matches(((Gen<Foo>) -> Gen<Foo>).self)
  CheckType<(Foo?) -> Foo?>.matches(((Foo?) -> Foo?).self)
  CheckType<([Foo]) -> [Foo]>.matches((([Foo]) -> [Foo]).self)
  CheckType<([Int : Foo]) -> [Int : Foo]>.matches((([Int : Foo]) -> [Int : Foo]).self)

  CheckType<(Gen<Foo>.Bar) -> Gen<Foo>.Bar>.matches(((Gen<Foo>.Bar) -> Gen<Foo>.Bar).self)
  CheckType<(Foo) -> Foo>.matches(((Foo?.Wrapped) -> Foo?.Wrapped).self)
  CheckType<(Foo) -> Foo>.matches((([Foo].Element) -> [Foo].Element).self)
  CheckType<(Foo) -> Foo>.matches((([Int : Foo].Value) -> [Int : Foo].Value).self)

  CheckType<(Foo.Type) -> Foo.Type>.matches(((Foo.Type) -> Foo.Type).self)
  CheckType<(P1.Protocol) -> P1.Protocol>.matches(((P1.Protocol) -> P1.Protocol).self)

  CheckType<() -> P1 & P2>.matches((() -> P1 & P2).self)
  CheckType<(P1 & P2) -> P1 & P2>.matches(((P1 & P2) -> P1 & P2).self)
  CheckType<(P1 & P2) -> (P3 & P2) -> P1 & Any>
      .matches(((P1 & P2) -> (P3 & P2) -> P1 & Any).self)

  // Trade exhaustivity for one complex test case.
  CheckType<
    (
      P1 & P2,
      Gen<Foo>.Bar,
      (Foo, [Int : Foo?].Type)
    ) -> (
      [Foo.Bar]
    ) -> Foo
  >.matches(
    (
      (
        (P1) & Foo?.P2,
        Gen<Foo>.Bar,
        (Foo, [Int : Foo?].Type)
      ) -> (
        [(Foo).Bar]
      ) -> [Foo].Element
    ).self
  )
}

func complexSequence() {
  // (assign_expr
  //   (discard_assignment_expr)
  //   (try_expr
  //     (type_expr typerepr='P1 & P2 throws -> P3 & P1')))
  _ = try P1 & P2 throws -> P3 & P1
  // expected-warning @-1 {{no calls to throwing functions occur within 'try' expression}}
  // expected-error @-2 {{single argument function types require parentheses}} {{11-11=(}} {{18-18=)}}
  // expected-error @-3 {{expected member name or initializer call after type name}}
  // expected-note @-4 {{use '.self' to reference the type object}} {{11-11=(}} {{36-36=).self}}
}

func takesVoid(f: Void -> ()) {} // expected-error {{single argument function types require parentheses}} {{19-23=()}}

func takesOneArg<T>(_: T.Type) {}
func takesTwoArgs<T>(_: T.Type, _: Int) {}

func testMissingSelf() {
  // None of these were not caught in Swift 3.
  // See test/Compatibility/type_expr.swift.

  takesOneArg(Int)
  // expected-error@-1 {{expected member name or initializer call after type name}}
  // expected-note@-2 {{add arguments after the type to construct a value of the type}}
  // expected-note@-3 {{use '.self' to reference the type object}}

  takesOneArg(Swift.Int)
  // expected-error@-1 {{expected member name or initializer call after type name}}
  // expected-note@-2 {{add arguments after the type to construct a value of the type}}
  // expected-note@-3 {{use '.self' to reference the type object}}

  takesTwoArgs(Int, 0)
  // expected-error@-1 {{expected member name or initializer call after type name}}
  // expected-note@-2 {{add arguments after the type to construct a value of the type}}
  // expected-note@-3 {{use '.self' to reference the type object}}

  takesTwoArgs(Swift.Int, 0)
  // expected-error@-1 {{expected member name or initializer call after type name}}
  // expected-note@-2 {{add arguments after the type to construct a value of the type}}
  // expected-note@-3 {{use '.self' to reference the type object}}

  Swift.Int // expected-warning {{expression of type 'Int.Type' is unused}}
  // expected-error@-1 {{expected member name or initializer call after type name}}
  // expected-note@-2 {{add arguments after the type to construct a value of the type}}
  // expected-note@-3 {{use '.self' to reference the type object}}

  _ = Swift.Int
  // expected-error@-1 {{expected member name or initializer call after type name}}
  // expected-note@-2 {{add arguments after the type to construct a value of the type}}
  // expected-note@-3 {{use '.self' to reference the type object}}
}
