// RUN: %target-swift-frontend -parse %s -verify

@freestanding(expression) // expected-error {{expected expression}}
macro self() = #externalMacro(module: "MacroDefinition", type: "InvalidMacro")

func sync() {}

@freestanding(expression) // expected-error {{expected expression}}
macro Self() = #externalMacro(module: "MacroDefinition", type: "InvalidMacro")

func testSelfAsFreestandingMacro() {
  _ = #self
}

func testCapitalSelfAsFreestandingMacro() {
  _ = #Self
}
 
func testSelfAsAttachedMacro() {
  @self // expected-error {{expected expression}}
  struct Foo {}
}

func testCapitalSelfAsAttachedMacro() {
  @Self // expected-error {{expected expression}}
  struct Foo {}
}
