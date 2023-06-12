// REQUIRES: swift_swift_parser

// RUN: %target-typecheck-verify-swift -swift-version 5 -module-name MacrosTest

@attached(accessor) macro m1() = #externalMacro(module: "MyMacros", type: "Macro1")
// expected-warning@-1{{external macro implementation type 'MyMacros.Macro1' could not be found for macro 'm1()'}}
// expected-note@-2{{'m1()' declared here}}

@attached(accessor) macro m2(_: Int) = #externalMacro(module: "MyMacros", type: "Macro2")
// expected-warning@-1{{external macro implementation type 'MyMacros.Macro2' could not be found for macro 'm2'}}
// expected-note@-2{{candidate has partially matching parameter list (Int)}}
// expected-note@-3{{candidate expects value of type 'Int' for parameter #1 (got 'String')}}

@attached(accessor) macro m2(_: Double) = #externalMacro(module: "MyMacros", type: "Macro2")
// expected-warning@-1{{external macro implementation type 'MyMacros.Macro2' could not be found for macro 'm2'}}
// expected-note@-2{{candidate has partially matching parameter list (Double)}}
// expected-note@-3{{candidate expects value of type 'Double' for parameter #1 (got 'String')}}

@attached(accessor) macro m3(message: String) = #externalMacro(module: "MyMacros", type: "Macro3")
// expected-warning@-1{{external macro implementation type 'MyMacros.Macro3' could not be found for macro 'm3(message:)'}}

@freestanding(expression) macro stringify<T>(_ value: T) -> (T, String) = #externalMacro(module: "MyMacros", type: "StringifyMacro")
// expected-warning@-1{{external macro implementation type 'MyMacros.StringifyMacro' could not be found for macro 'stringify'}}
// expected-note@-2{{'stringify' declared here}}

@m1 struct X1 { }

@m2 struct X2 { } // expected-error{{no exact matches in call to macro 'm2'}}

// Check for nesting rules.
struct SkipNestedType {
  @propertyWrapper
  struct m1<T> {
    init() { }

    var wrappedValue: T
  }

  // We select the macro, not the property wrapper.
  @m1 var x: Int = 0
  // expected-error@-1{{external macro implementation type 'MyMacros.Macro1' could not be found for macro 'm1()'}}
}

struct TestMacroArgs {
  @m1("extra arg") struct Args1 {} // expected-error{{argument passed to macro expansion that takes no arguments}}

  @m2(10) struct Args2 {}

  @m2(10.0) struct Args3 {}

  @m2("") struct Args4 {} // expected-error{{no exact matches in call to macro 'm2'}}

  @m2(Nested.x) struct Args5 {}

  struct Nested {
    static let x = 10

    @m2(x) struct Args1 {}

    @m2(Nested.x) struct Args2 {}
  }

  @m3(message: stringify(Nested.x).1) struct Args6 {}
  // expected-error@-1{{expansion of macro 'stringify' requires leading '#'}}

  @m3(message: #stringify().1) struct Args7 {}
  // expected-error@-1{{missing argument for parameter #1 in macro expansion}}

  @m3(message: #stringify(Nested.x).1) struct Args8 {}
}
