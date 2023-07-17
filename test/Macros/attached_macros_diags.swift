// REQUIRES: swift_swift_parser

// RUN: %empty-directory(%t)
// RUN: %host-build-swift -swift-version 5 -emit-library -o %t/%target-library-name(MacroDefinition) -parse-as-library -module-name=MacroDefinition %S/Inputs/syntax_macro_definitions.swift -g -no-toolchain-stdlib-rpath
// RUN: %target-typecheck-verify-swift -swift-version 5 -load-plugin-library %t/%target-library-name(MacroDefinition) -disable-availability-checking -module-name MacrosTest

@attached(peer) macro m1() = #externalMacro(module: "MacroDefinition", type: "EmptyPeerMacro")

@attached(peer) macro m2(_: Int) = #externalMacro(module: "MacroDefinition", type: "EmptyPeerMacro")
// expected-note@-1{{candidate has partially matching parameter list (Int)}}
// expected-note@-2{{candidate expects value of type 'Int' for parameter #1 (got 'String')}}

@attached(peer) macro m2(_: Double) = #externalMacro(module: "MacroDefinition", type: "EmptyPeerMacro")
// expected-note@-1{{candidate has partially matching parameter list (Double)}}
// expected-note@-2{{candidate expects value of type 'Double' for parameter #1 (got 'String')}}

@attached(peer) macro m3(message: String) = #externalMacro(module: "MacroDefinition", type: "EmptyPeerMacro")

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
  //expected-note@-1{{did you mean 'x'?}}

  func test() {
    let _: m1<Int> = _x
    // expected-error@-1{{cannot find '_x' in scope}}
  }
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
