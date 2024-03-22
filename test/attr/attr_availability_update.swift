// RUN: %swift -typecheck -verify -parse-stdlib -module-name Swift -target x86_64-apple-macosx10.10 %s

@available(SwiftStdlib 5.1, *)
func FooForDefineAvailability() {}

@available(SwiftStdlib 5.0, *)
func FooForDefineAvailabilityTest() {  
    FooForDefineAvailability()
    // expected-error@-1 {{'FooForDefineAvailability()' is only available in macOS 10.15 or newer}}
    // expected-note@-2 {{add 'if #available' version check}}
}

@available(*, unavailable)
func FooForUnavailable() {} // expected-note {{'FooForUnavailable()' has been explicitly marked unavailable here}}

func FooForUnavailableTest() {
    FooForUnavailable()
    // expected-error@-1 {{'FooForUnavailable()' is unavailable}}
}

@available(macOS, introduced: 10.15)
func FooForAvailability() {}

@available(macOS, introduced: 10.10)
func FooForAvailabilityTest() {
    // expected-note@-1 {{update @available attribute for macOS from '10.10' to '10.15' to meet the requirements of 'FooForAvailability'}} {{24:31-36=10.15}}
    FooForAvailability()
    // expected-error@-1 {{'FooForAvailability()' is only available in macOS 10.15 or newer}}
    // expected-note@-2 {{add 'if #available' version check}}
 }

@available(macOS 10.15, iOS 13, *)
func FooForAvailability2() {
}

@available(macOS 10.10, *)
func FooForAvailability2Test() {
    // expected-note@-1 {{update @available attribute for macOS from '10.10' to '10.15' to meet the requirements of 'FooForAvailability2'}} {{36:18-23=10.15}}
    FooForAvailability2()
    // expected-error@-1 {{'FooForAvailability2()' is only available in macOS 10.15 or newer}}
    // expected-note@-2 {{add 'if #available' version check}}
}

@available(macOS 10.15, *)
func FooForUnavailable2() {} 

@available(macOS, unavailable)
func FooForUnavailable2Test() {
    FooForUnavailable2()
    // expected-error@-1 {{'FooForUnavailable2()' is only available in macOS 10.15 or newer}}
    // expected-note@-2 {{add 'if #available' version check}}
}

@available(macOS 10.15, *)
func FooForDeprecated() {}

@available(macOS, deprecated: 12)
func FooForDeprecatedTest() {
    FooForDeprecated()
    // expected-error@-1 {{'FooForDeprecated()' is only available in macOS 10.15 or newer}}
    // expected-note@-2 {{add 'if #available' version check}}
}

@available(*, deprecated)
func FooForDeprecatedTest2() {
    FooForDeprecated()
    // expected-error@-1 {{'FooForDeprecated()' is only available in macOS 10.15 or newer}}
    // expected-note@-2 {{add 'if #available' version check}}
}