// RUN: %swift -typecheck -verify -parse-stdlib -module-name Swift -target x86_64-apple-macosx10.10 %s

// Fake declarations of some standard library features for -parse-stdlib.
precedencegroup AssignmentPrecedence {}
enum Optional<T> {
  case none
  case some(T)
}


@available(OSX, introduced: 10.5, deprecated: 10.8, obsoleted: 10.9,
              message: "you don't want to do that anyway")
func doSomething() { }
// expected-note @-1{{'doSomething()' was obsoleted in macOS 10.9}}

doSomething() // expected-error{{'doSomething()' is unavailable in macOS: you don't want to do that anyway}}


// Preservation of major.minor.micro
@available(OSX, introduced: 10.5, deprecated: 10.8, obsoleted: 10.9.1)
func doSomethingElse() { }
// expected-note @-1{{'doSomethingElse()' was obsoleted in macOS 10.9.1}}

doSomethingElse() // expected-error{{'doSomethingElse()' is unavailable in macOS}}

// Preservation of minor-only version
@available(OSX, introduced: 8.0, deprecated: 8.5, obsoleted: 10)
func doSomethingReallyOld() { }
// expected-note @-1{{'doSomethingReallyOld()' was obsoleted in macOS 10}}

doSomethingReallyOld() // expected-error{{'doSomethingReallyOld()' is unavailable in macOS}}

// Test deprecations in 10.10 and later

@available(OSX, introduced: 10.5, deprecated: 10.10,
              message: "Use another function")
func deprecatedFunctionWithMessage() { }

deprecatedFunctionWithMessage() // expected-warning{{'deprecatedFunctionWithMessage()' was deprecated in macOS 10.10: Use another function}}


@available(OSX, introduced: 10.5, deprecated: 10.10)
func deprecatedFunctionWithoutMessage() { }

deprecatedFunctionWithoutMessage() // expected-warning{{'deprecatedFunctionWithoutMessage()' was deprecated in macOS 10.10}}

@available(OSX, introduced: 10.5, deprecated: 10.10,
              message: "Use BetterClass instead")
class DeprecatedClass { }

func functionWithDeprecatedParameter(p: DeprecatedClass) { } // expected-warning{{'DeprecatedClass' was deprecated in macOS 10.10: Use BetterClass instead}}

@available(OSX, introduced: 10.5, deprecated: 10.11,
              message: "Use BetterClass instead")
class DeprecatedClassIn10_11 { }

// Elements deprecated later than the minimum deployment target (which is 10.10, in this case) should not generate warnings
func functionWithDeprecatedLaterParameter(p: DeprecatedClassIn10_11) { }

// Unconditional platform unavailability
@available(OSX, unavailable)
func doSomethingNotOnOSX() { }
// expected-note @-1{{'doSomethingNotOnOSX()' has been explicitly marked unavailable here}}

doSomethingNotOnOSX() // expected-error{{'doSomethingNotOnOSX()' is unavailable in macOS}}

@available(iOS, unavailable)
func doSomethingNotOniOS() { }

doSomethingNotOniOS() // okay

// Unconditional platform deprecation
@available(OSX, deprecated)
func doSomethingDeprecatedOnOSX() { }

doSomethingDeprecatedOnOSX() // expected-warning{{'doSomethingDeprecatedOnOSX()' is deprecated in macOS}}

@available(iOS, deprecated)
func doSomethingDeprecatedOniOS() { }

doSomethingDeprecatedOniOS() // okay

@available(macOS 10.10, *)
struct TestStruct {} // expected-note 2 {{enclosing scope requires availability of macOS 10.10 or newer}}

@available(macOS 10.10, *)
extension TestStruct { // expected-note {{enclosing scope requires availability of macOS 10.10 or newer}}
  @available(swift 400)
  func doTheThing() {} // expected-note {{'doTheThing()' was introduced in Swift 400}}

  @available(macOS 10.9, *) // expected-error {{instance method cannot be more available than enclosing scope}}
  @available(swift 400)
  func doAnotherThing() {} // expected-note {{'doAnotherThing()' was introduced in Swift 400}}

  @available(macOS 10.12, *)
  @available(swift 400)
  func doThirdThing() {} // expected-note {{'doThirdThing()' was introduced in Swift 400}}

  @available(macOS 10.12, *)
  @available(swift 1)
  func doFourthThing() {}

  @available(*, deprecated)
  func doDeprecatedThing() {}
}

extension TestStruct {
  @available(macOS 10.9, *) // expected-warning {{instance method cannot be more available than enclosing scope}}
  func doFifthThing() {}

  struct NestedStruct {
    @available(macOS 10.9, *) // expected-warning {{instance method cannot be more available than enclosing scope}}
    func doSixthThing() {}
  }
}

@available(macOS 10.11, *)
func testMemberAvailability() { // expected-note {{update @available attribute for macOS from '10.11' to '10.12' to meet the requirements of 'doFourthThing'}} {{117:18-23=10.12}}
  TestStruct().doTheThing() // expected-error {{'doTheThing()' is unavailable}}
  TestStruct().doAnotherThing() // expected-error {{'doAnotherThing()' is unavailable}}
  TestStruct().doThirdThing() // expected-error {{'doThirdThing()' is unavailable}}
  TestStruct().doFourthThing() // expected-error {{'doFourthThing()' is only available in macOS 10.12 or newer}} expected-note {{'if #available'}}
  TestStruct().doDeprecatedThing() // expected-warning {{'doDeprecatedThing()' is deprecated}}
}

extension TestStruct {
  struct Data {
    mutating func mutate() {}
  }

  var unavailableGetter: Data {
    @available(macOS, unavailable, message: "bad getter")
    get { return Data() } // expected-note 2 {{here}}
    set {}
  }

  var unavailableSetter: Data {
    get { return Data() }
    @available(macOS, obsoleted: 10.5, message: "bad setter")
    set {} // expected-note 2 {{setter for 'unavailableSetter' was obsoleted in macOS 10.5}}
  }
}

func testAccessors() {
  var t = TestStruct()
  _ = t.unavailableGetter // expected-error {{getter for 'unavailableGetter' is unavailable in macOS}}
  t.unavailableGetter = .init()
  t.unavailableGetter.mutate() // expected-error {{getter for 'unavailableGetter' is unavailable in macOS}}

  _ = t.unavailableSetter
  t.unavailableSetter = .init() // expected-error {{setter for 'unavailableSetter' is unavailable in macOS: bad setter}}
  t.unavailableSetter.mutate() // expected-error {{setter for 'unavailableSetter' is unavailable in macOS: bad setter}}
}

// Check available on extensions

@available(macOS, unavailable)
extension TestStruct {
  func unavailInExtension() {} // expected-note 2 {{'unavailInExtension()' has been explicitly marked unavailable here}}
}

@available(macOS, obsoleted: 10.0)
extension TestStruct {
  func obsoletedInExtension() {} // expected-note 2 {{'obsoletedInExtension()' was obsoleted in macOS 10.0}}
}

@available(macOS, deprecated: 10.0)
extension TestStruct {
  func deprecatedInExtension() {}
}

@available(swift, introduced: 50.0)
extension TestStruct {
  func introducedInExtensionSwift() {} // expected-note 2 {{'introducedInExtensionSwift()' was introduced in Swift 50.0}}
}

@available(macOS, introduced: 10.50)
extension TestStruct {
  func introducedInExtensionMacOS() {}
}

TestStruct().unavailInExtension() // expected-error {{'unavailInExtension()' is unavailable in macOS}}
TestStruct().obsoletedInExtension() // expected-error {{'obsoletedInExtension()' is unavailable}}
TestStruct().deprecatedInExtension() // expected-warning {{'deprecatedInExtension()' was deprecated in macOS 10.0}}
TestStruct().introducedInExtensionSwift() // expected-error {{'introducedInExtensionSwift()' is unavailable}}
TestStruct().introducedInExtensionMacOS() // expected-error {{'introducedInExtensionMacOS()' is only available in macOS 10.50 or newer}}
// expected-note@-1{{add 'if #available' version check}}

extension TestStruct {
  func availableFunc() {
    unavailInExtension() // expected-error {{'unavailInExtension()' is unavailable in macOS}}
    obsoletedInExtension() // expected-error {{'obsoletedInExtension()' is unavailable}}
    deprecatedInExtension() // expected-warning {{'deprecatedInExtension()' was deprecated in macOS 10.0}}
    introducedInExtensionSwift() // expected-error {{'introducedInExtensionSwift()' is unavailable}}
  }
}

extension TestStruct { // expected-note{{add @available attribute to enclosing extension}}
  func availableFuncMacOS() { // expected-note{{add @available attribute to enclosing instance method}}
    introducedInExtensionMacOS() // expected-error {{'introducedInExtensionMacOS()' is only available in macOS 10.50 or newer}}
    // expected-note@-1{{add 'if #available' version check}}
  }
}

@available(macOS, introduced: 10.50)
extension TestStruct {
  func futureFuncMacOS() {
    introducedInExtensionMacOS()
  }
}

@available(macOS, unavailable)
struct UnavailableStruct { }

@available(macOS, unavailable)
extension UnavailableStruct { } // no-error

#if os(macOS)
@available(macOS, unavailable)
extension UnavailableStruct { } // no-error
#endif
