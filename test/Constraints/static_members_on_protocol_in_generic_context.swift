// RUN: %target-typecheck-verify-swift -swift-version 5

protocol P {}

struct S : P {
  var other: S { S() }
}


struct G<T> : P {
  var other: G<T> { fatalError() }
}

extension P where Self == S {
  static var property: S { S() }

  static var iuoProp: S! { S() }
  static var optProp: S? { S() }

  static var fnProp: () -> S {
    { S() }
  }

  static func method() -> S {
    return S()
  }

  static subscript(_: Int) -> S {
    get { S() }
  }
}

extension P {
  static func genericFn<T>(_: T) -> G<T> where Self == G<T> { // expected-note 5 {{'G<T>' = 'G<Int>}} expected-note 2 {{'G<T>' = 'G<String>'}}
    return G<T>()
  }

  static subscript<T>(t t: T) -> G<T> where Self == G<T> { // expected-note 5 {{'G<T>' = 'G<Int>'}} expected-note 2 {{'G<T>' = 'G<String>'}}
    get { G<T>() }
  }
}

// References on protocol metatype are only allowed through a leading dot syntax

_ = P.property // expected-error {{static member 'property' cannot be used on protocol metatype '(any P).Type'}}
// expected-error@-1 {{static property 'property' requires the types 'Self' and 'S' be equivalent}}
_ = P.property.other // expected-error {{static member 'property' cannot be used on protocol metatype '(any P).Type'}}
// expected-error@-1 {{static property 'property' requires the types 'Self' and 'S' be equivalent}}
_ = P.iuoProp // expected-error {{static member 'iuoProp' cannot be used on protocol metatype '(any P).Type'}}
// expected-error@-1 {{static property 'iuoProp' requires the types 'Self' and 'S' be equivalent}}
_ = P.iuoProp.other // expected-error {{static member 'iuoProp' cannot be used on protocol metatype '(any P).Type'}}
// expected-error@-1 {{static property 'iuoProp' requires the types 'Self' and 'S' be equivalent}}
_ = P.optProp // expected-error {{static member 'optProp' cannot be used on protocol metatype '(any P).Type'}}
// expected-error@-1 {{static property 'optProp' requires the types 'Self' and 'S' be equivalent}}
_ = P.optProp?.other // expected-error {{static member 'optProp' cannot be used on protocol metatype '(any P).Type'}}
// expected-error@-1 {{static property 'optProp' requires the types 'Self' and 'S' be equivalent}}
_ = P.fnProp // expected-error {{static member 'fnProp' cannot be used on protocol metatype '(any P).Type'}}
// expected-error@-1 {{static property 'fnProp' requires the types 'Self' and 'S' be equivalent}}
_ = P.fnProp() // expected-error {{static member 'fnProp' cannot be used on protocol metatype '(any P).Type'}}
// expected-error@-1 {{static property 'fnProp' requires the types 'Self' and 'S' be equivalent}}
_ = P.fnProp().other // expected-error {{static member 'fnProp' cannot be used on protocol metatype '(any P).Type'}}
// expected-error@-1 {{static property 'fnProp' requires the types 'Self' and 'S' be equivalent}}
_ = P.method() // expected-error {{static member 'method' cannot be used on protocol metatype '(any P).Type'}}
// expected-error@-1 {{referencing static method 'method()' on 'P' requires the types 'Self' and 'S' be equivalent}}
_ = P.method   // expected-error {{static member 'method' cannot be used on protocol metatype '(any P).Type'}}
// expected-error@-1 {{referencing static method 'method()' on 'P' requires the types 'Self' and 'S' be equivalent}}
_ = P.method().other // expected-error {{static member 'method' cannot be used on protocol metatype '(any P).Type'}}
// expected-error@-1 {{referencing static method 'method()' on 'P' requires the types 'Self' and 'S' be equivalent}}
_ = P.genericFn(42) // expected-error {{static member 'genericFn' cannot be used on protocol metatype '(any P).Type'}}
// expected-error@-1 {{static method 'genericFn' requires the types 'Self' and 'G<Int>' be equivalent}}
_ = P.genericFn(42).other // expected-error {{static member 'genericFn' cannot be used on protocol metatype '(any P).Type'}}
// expected-error@-1 {{static method 'genericFn' requires the types 'Self' and 'G<Int>' be equivalent}}
_ = P[42] // expected-error {{static member 'subscript' cannot be used on protocol metatype '(any P).Type'}}
// expected-error@-1 {{referencing static subscript 'subscript(_:)' on 'P' requires the types 'Self' and 'S' be equivalent}}
_ = P[42].other // expected-error {{static member 'subscript' cannot be used on protocol metatype '(any P).Type'}}
// expected-error@-1 {{referencing static subscript 'subscript(_:)' on 'P' requires the types 'Self' and 'S' be equivalent}}
_ = P[t: 42] // expected-error {{static member 'subscript' cannot be used on protocol metatype '(any P).Type'}}
// expected-error@-1 {{static subscript 'subscript(t:)' requires the types 'Self' and 'G<Int>' be equivalent}}
_ = P[t: 42].other // expected-error {{static member 'subscript' cannot be used on protocol metatype '(any P).Type'}}
// expected-error@-1 {{static subscript 'subscript(t:)' requires the types 'Self' and 'G<Int>' be equivalent}}

let _: S = P.property // expected-error {{static member 'property' cannot be used on protocol metatype '(any P).Type'}}
// expected-error@-1 {{static property 'property' requires the types 'Self' and 'S' be equivalent}}
let _: S = P.property.other // expected-error {{static member 'property' cannot be used on protocol metatype '(any P).Type'}}
// expected-error@-1 {{static property 'property' requires the types 'Self' and 'S' be equivalent}}
let _: () -> S = P.fnProp // expected-error {{static member 'fnProp' cannot be used on protocol metatype '(any P).Type'}}
// expected-error@-1 {{static property 'fnProp' requires the types 'Self' and 'S' be equivalent}}
let _: S = P.fnProp() // expected-error {{static member 'fnProp' cannot be used on protocol metatype '(any P).Type'}}
// expected-error@-1 {{static property 'fnProp' requires the types 'Self' and 'S' be equivalent}}
let _: S = P.fnProp().other // expected-error {{static member 'fnProp' cannot be used on protocol metatype '(any P).Type'}}
// expected-error@-1 {{static property 'fnProp' requires the types 'Self' and 'S' be equivalent}}
let _: () -> S = P.method // expected-error {{static member 'method' cannot be used on protocol metatype '(any P).Type'}}
// expected-error@-1 {{referencing static method 'method()' on 'P' requires the types 'Self' and 'S' be equivalent}}
let _: S = P.method() // expected-error {{static member 'method' cannot be used on protocol metatype '(any P).Type'}}
// expected-error@-1 {{referencing static method 'method()' on 'P' requires the types 'Self' and 'S' be equivalent}}
let _: S = P.method().other // expected-error {{static member 'method' cannot be used on protocol metatype '(any P).Type'}}
// expected-error@-1 {{referencing static method 'method()' on 'P' requires the types 'Self' and 'S' be equivalent}}
let _: G<Int> = P.genericFn(42) // expected-error {{static member 'genericFn' cannot be used on protocol metatype '(any P).Type'}}
// expected-error@-1 {{static method 'genericFn' requires the types 'Self' and 'G<Int>' be equivalent}}
let _: G = P.genericFn(42) // expected-error {{static member 'genericFn' cannot be used on protocol metatype '(any P).Type'}}
// expected-error@-1 {{static method 'genericFn' requires the types 'Self' and 'G<Int>' be equivalent}}
let _: G<String> = P.genericFn(42) // expected-error {{cannot convert value of type 'Int' to expected argument type 'String'}}
// expected-error@-1 {{static member 'genericFn' cannot be used on protocol metatype '(any P).Type'}}
// expected-error@-2 {{static method 'genericFn' requires the types 'Self' and 'G<String>' be equivalent}}
let _: G<Int> = P.genericFn(42).other // expected-error {{static member 'genericFn' cannot be used on protocol metatype '(any P).Type'}}
// expected-error@-1 {{static method 'genericFn' requires the types 'Self' and 'G<Int>' be equivalent}}
let _: G<String> = P.genericFn(42).other // expected-error {{cannot convert value of type 'Int' to expected argument type 'String'}}
// expected-error@-1 {{static method 'genericFn' requires the types 'Self' and 'G<String>' be equivalent}}
// expected-error@-2 {{static member 'genericFn' cannot be used on protocol metatype '(any P).Type'}}
let _: S = P[42] // expected-error {{static member 'subscript' cannot be used on protocol metatype '(any P).Type'}}
// expected-error@-1 {{referencing static subscript 'subscript(_:)' on 'P' requires the types 'Self' and 'S' be equivalent}}
let _: S = P[42].other // expected-error {{static member 'subscript' cannot be used on protocol metatype '(any P).Type'}}
// expected-error@-1 {{referencing static subscript 'subscript(_:)' on 'P' requires the types 'Self' and 'S' be equivalent}}
let _: G<Int> = P[t: 42] // expected-error {{static member 'subscript' cannot be used on protocol metatype '(any P).Type'}}
// expected-error@-1 {{static subscript 'subscript(t:)' requires the types 'Self' and 'G<Int>' be equivalent}}
let _: G = P[t: 42] // expected-error {{static member 'subscript' cannot be used on protocol metatype '(any P).Type'}}
// expected-error@-1 {{static subscript 'subscript(t:)' requires the types 'Self' and 'G<Int>' be equivalent}}
let _: G<String> = P[t: 42] // expected-error {{cannot convert value of type 'Int' to expected argument type 'String'}}
// expected-error@-1 {{static member 'subscript' cannot be used on protocol metatype '(any P).Type'}}
// expected-error@-2 {{static subscript 'subscript(t:)' requires the types 'Self' and 'G<String>' be equivalent}}
let _: G<Int> = P[t: 42].other // expected-error {{static member 'subscript' cannot be used on protocol metatype '(any P).Type'}}
// expected-error@-1 {{static subscript 'subscript(t:)' requires the types 'Self' and 'G<Int>' be equivalent}}
let _: G<String> = P[t: 42].other // expected-error {{cannot convert value of type 'Int' to expected argument type 'String'}}
// expected-error@-1 {{static member 'subscript' cannot be used on protocol metatype '(any P).Type'}}
// expected-error@-2 {{static subscript 'subscript(t:)' requires the types 'Self' and 'G<String>' be equivalent}}

func test<T: P>(_: T) {}

test(.property) // Ok, base is inferred as Style.Type
test(.property.other) // Ok
test(.iuoProp) // Ok
test(.iuoProp.other) // Ok
test(.optProp!) // Ok
test(.optProp)
// expected-error@-1 {{value of optional type 'S?' must be unwrapped to a value of type 'S'}}
// expected-note@-2 {{coalesce using '??' to provide a default when the optional value contains 'nil'}}
// expected-note@-3 {{force-unwrap using '!' to abort execution if the optional value contains 'nil'}}
test(.optProp!.other) // Ok
test(.fnProp()) // Ok
test(.fnProp().other) // Ok
test(.method()) // Ok, static method call on the metatype
test(.method().other) // Ok
test(.genericFn(42)) // Ok
test(.genericFn(42).other) // Ok

protocol Q {}

func test_combo<T: P & Q>(_: T) {} // expected-note {{where 'T' = 'G<Int>'}}

extension Q where Self == S {
  static var otherProperty: S { S() }

  static func otherMethod() -> S {
    return S()
  }

  static func otherGeneric<T>(_: T) -> S {
    return S()
  }
}

extension S : Q {
}

test_combo(.property) // Ok
test_combo(.method()) // Ok
test_combo(.otherProperty) // Ok
test_combo(.otherProperty.other) // Ok
test_combo(.otherProperty.property) // expected-error {{static member 'property' cannot be used on instance of type 'S'}}
test_combo(.otherMethod()) // Ok
test_combo(.otherMethod().method()) // expected-error {{static member 'method' cannot be used on instance of type 'S'}}
test_combo(.otherGeneric(42)) // Ok

test_combo(.genericFn(42)) // expected-error {{global function 'test_combo' requires that 'G<Int>' conform to 'Q'}}

/* Invalid result types */

extension P { // expected-note 13 {{missing same-type requirement on 'Self'}} {{12-12= where Self == <#Type#>}}
  static func generic<T>(_: T) -> T { fatalError() }
  static func genericWithReqs<T: Collection, Q>(_: T) -> Q where T.Element == Q { // expected-note {{required by static method 'genericWithReqs' where 'T' = '()'}}
    fatalError()
  }
}

extension P { // expected-note 6 {{missing same-type requirement on 'Self'}}
  static var invalidProp: Int { 42 }
  static var selfProp: Self { fatalError() }
  static func invalidMethod() -> Int { 42 }
  static subscript(q q: String) -> Int { get { 42 } }
}

_ = P.doesntExist // expected-error {{type 'any P' has no member 'doesntExist'}}
_ = P.selfProp // expected-error {{static member 'selfProp' cannot be used on protocol metatype '(any P).Type'}}
_ = P.invalidProp
// expected-error@-1 {{static member 'invalidProp' cannot be used on protocol metatype '(any P).Type'}}
_ = P.invalidProp.other
// expected-error@-1 {{static member 'invalidProp' cannot be used on protocol metatype '(any P).Type'}}
// expected-error@-2 {{value of type 'Int' has no member 'other'}}
_ = P.invalidMethod // Partial application with an invalid base type
// expected-error@-1 {{static member 'invalidMethod' cannot be used on protocol metatype '(any P).Type'}}
_ = P.invalidMethod()
// expected-error@-1 {{static member 'invalidMethod' cannot be used on protocol metatype '(any P).Type'}}
_ = P.invalidMethod().other
// expected-error@-1 {{static member 'invalidMethod' cannot be used on protocol metatype '(any P).Type'}}
// expected-error@-2 {{value of type 'Int' has no member 'other'}}
_ = P.generic(42)
// expected-error@-1 {{static member 'generic' cannot be used on protocol metatype '(any P).Type'}}
_ = P.generic(42).other
// expected-error@-1 {{static member 'generic' cannot be used on protocol metatype '(any P).Type'}}
// expected-error@-2 {{value of type 'Int' has no member 'other'}}
_ = P.generic(S()) // expected-error {{static member 'generic' cannot be used on protocol metatype '(any P).Type'}}
_ = P.generic(S()).other // expected-error {{static member 'generic' cannot be used on protocol metatype '(any P).Type'}}
_ = P.generic(G<Int>()) // expected-error {{static member 'generic' cannot be used on protocol metatype '(any P).Type'}}
_ = P.genericWithReqs([S()]) // expected-error {{static member 'genericWithReqs' cannot be used on protocol metatype '(any P).Type'}}
_ = P.genericWithReqs([42])
// expected-error@-1 {{static member 'genericWithReqs' cannot be used on protocol metatype '(any P).Type'}}
_ = P.genericWithReqs(())
// expected-error@-1 {{type '()' cannot conform to 'Collection'}} expected-note@-1 {{only concrete types such as structs, enums and classes can conform to protocols}}
// expected-error@-2 {{static member 'genericWithReqs' cannot be used on protocol metatype '(any P).Type'}}
_ = P[q: ""]
// expected-error@-1 {{static member 'subscript' cannot be used on protocol metatype '(any P).Type'}}
_ = P[q: ""].other
// expected-error@-1 {{static member 'subscript' cannot be used on protocol metatype '(any P).Type'}}
// expected-error@-2 {{value of type 'Int' has no member 'other'}}

test(.doesntExist) // expected-error {{type 'P' has no member 'doesntExist'}}
test(.doesnt.exist()) // expected-error {{type 'P' has no member 'doesnt'}}
test(.invalidProp)
// expected-error@-1 {{contextual member reference to static property 'invalidProp' requires 'Self' constraint in the protocol extension}}
test(.invalidProp.other)
// expected-error@-1 {{contextual member reference to static property 'invalidProp' requires 'Self' constraint in the protocol extension}}
// expected-error@-2 {{value of type 'Int' has no member 'other'}}
test(.invalidMethod())
// expected-error@-1 {{contextual member reference to static method 'invalidMethod()' requires 'Self' constraint in the protocol extension}}
test(.invalidMethod().other)
// expected-error@-1 {{contextual member reference to static method 'invalidMethod()' requires 'Self' constraint in the protocol extension}}
// expected-error@-2 {{value of type 'Int' has no member 'other'}}
test(.generic(42))
// expected-error@-1 {{contextual member reference to static method 'generic' requires 'Self' constraint in the protocol extension}}
test(.generic(42).other)
// expected-error@-1 {{contextual member reference to static method 'generic' requires 'Self' constraint in the protocol extension}}
// expected-error@-2 {{value of type 'Int' has no member 'other'}}
test(.generic(S())) // expected-error {{contextual member reference to static method 'generic' requires 'Self' constraint in the protocol extension}}
test(.generic(G<Int>())) // expected-error {{contextual member reference to static method 'generic' requires 'Self' constraint in the protocol extension}}
test(.genericWithReqs([S()])) // expected-error {{contextual member reference to static method 'genericWithReqs' requires 'Self' constraint in the protocol extension}}
test(.genericWithReqs([42]))
// expected-error@-1 {{contextual member reference to static method 'genericWithReqs' requires 'Self' constraint in the protocol extension}}
test(.genericWithReqs(()))
// expected-error@-1 {{contextual member reference to static method 'genericWithReqs' requires 'Self' constraint in the protocol extension}}

test_combo(.doesntExist) // expected-error {{reference to member 'doesntExist' cannot be resolved without a contextual type}}
test_combo(.doesnt.exist()) // expected-error {{reference to member 'doesnt' cannot be resolved without a contextual type}}
test_combo(.invalidProp)
// expected-error@-1 {{contextual member reference to static property 'invalidProp' requires 'Self' constraint in the protocol extension}}
test_combo(.invalidMethod())
// expected-error@-1 {{contextual member reference to static method 'invalidMethod()' requires 'Self' constraint in the protocol extension}}
test_combo(.generic(42))
// expected-error@-1 {{contextual member reference to static method 'generic' requires 'Self' constraint in the protocol extension}}
test_combo(.generic(S())) // expected-error {{contextual member reference to static method 'generic' requires 'Self' constraint in the protocol extension}}
test_combo(.generic(G<Int>())) // expected-error {{contextual member reference to static method 'generic' requires 'Self' constraint in the protocol extension}}
test_combo(.genericWithReqs([S()])) // expected-error {{contextual member reference to static method 'genericWithReqs' requires 'Self' constraint in the protocol extension}}
test_combo(.genericWithReqs([42]))
// expected-error@-1 {{contextual member reference to static method 'genericWithReqs' requires 'Self' constraint in the protocol extension}}
test_combo(.genericWithReqs(()))
// expected-error@-1 {{contextual member reference to static method 'genericWithReqs' requires 'Self' constraint in the protocol extension}}

protocol TestWithAssoc {
  associatedtype U
}

struct S_With_U : P {
  typealias U = Int
}

extension TestWithAssoc where U == Int { // expected-note {{missing same-type requirement on 'Self'}} {{39-39=, Self == <#Type#> }}
  static var intVar: Int { 42 }
}

func test_fixit_with_where_clause() {
  func test_assoc<T: TestWithAssoc>(_: T) {}
  test_assoc(.intVar) // expected-error {{contextual member reference to static property 'intVar' requires 'Self' constraint in the protocol extension}}
}

// rdar://77700261 - incorrect warning about assuming non-optional base for unresolved member lookup
struct WithShadowedMember : P {}

extension WithShadowedMember {
  static var warnTest: WithShadowedMember { get { WithShadowedMember() } }
}

extension P where Self == WithShadowedMember {
  static var warnTest: WithShadowedMember { get { fatalError() } }
}

func test_no_warning_about_optional_base() {
  func test(_: WithShadowedMember?) {}

  test(.warnTest) // Ok and no warning even though the `warnTest` name is shadowed
}

// rdar://78425221 - invalid defaulting of literal argument when base is inferred from protocol

protocol Style {}

struct FormatString : ExpressibleByStringInterpolation {
  init(stringLiteral: String) {}
}

struct Number : ExpressibleByIntegerLiteral {
  init(integerLiteral: Int) {}
}

struct TestStyle: Style {
  public init(format: FormatString)  {
  }
}

extension Style where Self == TestStyle {
  static func formattedString(format: FormatString) -> TestStyle { fatalError() }
  static func number(_: Number) -> TestStyle { fatalError() }
}

func acceptStyle<S: Style>(_: S) {}

acceptStyle(.formattedString(format: "hi")) // Ok
acceptStyle(.number(42)) // Ok

protocol Container {
  associatedtype Content
}

struct Box<T>: Container { // expected-note {{'T' declared as parameter to type 'Box'}}
  typealias Content = T
  init(_: Content) {}
}

extension Container {
  // leading-dot syntax is going to use a typealias
  typealias box = Box
}

// rdar://88513939 - Allow to call init through a typealias using leading-dot syntax in generic context
func test_leading_dot_syntax_with_typelias() {
  func test<T: Container>(_: T) {} // expected-note {{required by local function 'test' where 'T' = 'Box<T>.Type'}}

  test(Container.box(1)) // Ok
  test(.box(1)) // Ok `Container.box(1)` means `Box.init(1)`

  test(.box) // expected-error {{type 'Box<T>.Type' cannot conform to 'Container'}} expected-note {{only concrete types such as structs, enums and classes can conform to protocols}}
  // expected-error@-1 {{generic parameter 'T' could not be inferred}}
}
