// RUN: %swift -typecheck -primary-file %s %S/Inputs/availability_multi_other.swift -verify
// REQUIRES: OS=macosx

func callToEnsureNotInScriptMode() { }
// Add an expected error to express expectation that we're not in script mode
callToEnsureNotInScriptMode() // expected-error {{expressions are not allowed at the top level}}

@available(OSX, introduced: 10.9)
var globalAvailableOn10_9: Int = 9

@available(OSX, introduced: 99.51)
var globalAvailableOn99_51: Int = 10

@available(OSX, introduced: 99.52)
var globalAvailableOn99_52: Int = 11

// Top level should reflect the minimum deployment target.
let ignored1: Int = globalAvailableOn10_9

let ignored2: Int = globalAvailableOn99_51 // expected-error {{'globalAvailableOn99_51' is only available in macOS 99.51 or newer}}
    // expected-note@-1 {{add @available attribute to enclosing let}}

let ignored3: Int = globalAvailableOn99_52 // expected-error {{'globalAvailableOn99_52' is only available in macOS 99.52 or newer}}
    // expected-note@-1 {{add @available attribute to enclosing let}}

@available(OSX, introduced: 99.51)
func useFromOtherOn99_51() {
  // expected-note@-1 {{update @available attribute for macOS from '99.51' to '99.52' to meet the requirements of 'returns99_52Introduced99_52'}} {{26:29-34=99.52}}
  // expected-note@-2 {{update @available attribute for macOS from '99.51' to '99.52' to meet the requirements of 'OtherIntroduced99_52'}} {{26:29-34=99.52}}
  // expected-note@-3 {{update @available attribute for macOS from '99.51' to '99.52' to meet the requirements of 'extensionMethodOnOtherIntroduced99_51AvailableOn99_52'}} {{26:29-34=99.52}}
  // expected-note@-4 {{update @available attribute for macOS from '99.51' to '99.52' to meet the requirements of 'NestedIntroduced99_52'}} {{26:29-34=99.52}}

  // This will trigger validation of OtherIntroduced99_51 in
  // in availability_multi_other.swift
  let o99_51 = OtherIntroduced99_51()
  o99_51.extensionMethodOnOtherIntroduced99_51()

  let o10_9 = OtherIntroduced10_9()
  o10_9.extensionMethodOnOtherIntroduced10_9AvailableOn99_51(o99_51)
  _ = o99_51.returns99_52Introduced99_52() // expected-error {{'returns99_52Introduced99_52()' is only available in macOS 99.52 or newer}}
      // expected-note@-1 {{add 'if #available' version check}}

  _ = OtherIntroduced99_52()
      // expected-error@-1 {{'OtherIntroduced99_52' is only available in macOS 99.52 or newer}}
      // expected-note@-2 {{add 'if #available' version check}}

  o99_51.extensionMethodOnOtherIntroduced99_51AvailableOn99_52() // expected-error {{'extensionMethodOnOtherIntroduced99_51AvailableOn99_52()' is only available in macOS 99.52 or newer}}
      // expected-note@-1 {{add 'if #available' version check}}

  _ = OtherIntroduced99_51.NestedIntroduced99_52()
      // expected-error@-1 {{'NestedIntroduced99_52' is only available in macOS 99.52 or newer}}
      // expected-note@-2 {{add 'if #available' version check}}
}

@available(OSX, introduced: 99.52)
func useFromOtherOn99_52() {
  // expected-note@-1 {{update @available attribute for macOS from '99.52' to '99.53' to meet the requirements of 'returns99_53'}} {{55:29-34=99.53}}
  _ = OtherIntroduced99_52()

  let n99_52 = OtherIntroduced99_51.NestedIntroduced99_52()
  _ = n99_52.returns99_52()
  _ = n99_52.returns99_53() // expected-error {{'returns99_53()' is only available in macOS 99.53 or newer}}
      // expected-note@-1 {{add 'if #available' version check}}

  // This will trigger validation of the global in availability_in_multi_other.swift
  _ = globalFromOtherOn99_52
}
