// REQUIRES: swift_swift_parser, asserts
// REQUIRES: OS=macosx

// RUN: %target-typecheck-verify-swift -swift-version 5 -module-name MacrosTest -target %target-cpu-apple-macos50

@freestanding(expression)
macro overloadedOnAvailability(_: Any) -> Int = #externalMacro(module: "MacroLibrary", type: "MyOldMacro")
//expected-warning@-1{{external macro implementation type 'MacroLibrary.MyOldMacro'}}
// expected-note@-2 2{{'overloadedOnAvailability' declared here}}

@available(macOS 60, *)
@freestanding(expression)
macro overloadedOnAvailability(_: Int) -> Double = #externalMacro(module: "MacroLibrary", type: "MyNewMacro")
//expected-warning@-1{{external macro implementation type 'MacroLibrary.MyNewMacro'}}
// expected-note@-2{{'overloadedOnAvailability' declared here}}


func mutateInt(_: inout Int) { }
func mutateDouble(_: inout Double) { }

func testAvailabilityOld() {
  var a = #overloadedOnAvailability(1)
  mutateInt(&a)
  // expected-error@-2{{external macro implementation type 'MacroLibrary.MyOldMacro'}}
}

@available(macOS 60, *)
func testAvailabilitNew(a: Any) {
  var a = #overloadedOnAvailability(1)
  mutateDouble(&a)
  // expected-error@-2{{external macro implementation type 'MacroLibrary.MyNewMa}}

  var b = #overloadedOnAvailability(a)
  mutateInt(&b)
  // expected-error@-2{{external macro implementation type 'MacroLibrary.MyOldMacro'}}
}
