// RUN: %target-swift-frontend -parse %s -verify

@freestanding(expression) // expected-error {{expected expression}}
macro self() = #externalMacro(module: "MacroDefinition", type: "InvalidMacro")

func sync() {}

@freestanding(expression) // expected-error {{expected expression}}
macro Self() = #externalMacro(module: "MacroDefinition", type: "InvalidMacro")

func testSelfAsFreestandingMacro() {
  _ = #self // expected-error {{keyword 'self' cannot be used as an identifier here}} expected-note {{use backticks to escape it}}
}

func testCapitalSelfAsFreestandingMacro() {
  _ = #Self // expected-error {{keyword 'Self' cannot be used as an identifier here}} expected-note {{use backticks to escape it}}
}
 
func testSelfAsAttachedMacro() {
  @self // expected-error {{expected expression}}
  struct Foo {}
}

func testCapitalSelfAsAttachedMacro() {
  @Self // expected-error {{expected expression}}
  struct Foo {}
}
