// RUN: %target-typecheck-verify-swift -parse-as-library
// REQUIRES: OS=macosx
@available(macOS 998.0, *)
@discardableResult
func foo() -> Int { return 0 }

@available(macOS 999.0, *)
@discardableResult
func bar() -> Int { return 0 }

// Verify that #unavailable is the opposite of #available.
// expected-note@+1 *{{add @available attribute to enclosing global function}}
func testUnavailable() {
  if #unavailable(macOS 998.0) {
    foo() // expected-error{{'foo()' is only available in macOS 998.0 or newer}}
    // expected-note@-1 {{add 'if #available' version check}}
    bar() // expected-error{{'bar()' is only available in macOS 999.0 or newer}}
    // expected-note@-1 {{add 'if #available' version check}}
  } else {
    foo()
    bar() // expected-error{{'bar()' is only available in macOS 999.0 or newer}}
    // expected-note@-1 {{add 'if #available' version check}}
    if #unavailable(macOS 999.0) {
      foo()
      bar() // expected-error{{'bar()' is only available in macOS 999.0 or newer}}
      // expected-note@-1 {{add 'if #available' version check}}
    } else {
      foo()
      bar()
    }
  }
}

// Verify that #unavailable doesn't complain about useless specs.
// expected-note@+1 *{{add @available attribute to enclosing global function}}
func testUnavailableDoesntWarnUselessSpecs() {
  if #unavailable(macOS 998.0), #unavailable(macOS 999.0) {
    foo() // expected-error{{'foo()' is only available in macOS 998.0 or newer}}
    // expected-note@-1 {{add 'if #available' version check}}
    bar() // expected-error{{'bar()' is only available in macOS 999.0 or newer}}
    // expected-note@-1 {{add 'if #available' version check}}
  } else {
    foo()
    bar()
  }
}

// Verify that #unavailable refines the availability of all "else" paths.
// expected-note@+1 *{{add @available attribute to enclosing global function}}
func testUnavailableExpandAllElsePaths() {
  if #unavailable(macOS 998.0) {
    foo() // expected-error{{'foo()' is only available in macOS 998.0 or newer}}
    // expected-note@-1 {{add 'if #available' version check}}
  } else if 1 == 2 {
    foo()
  } else if 1 == 3 {
    foo()
  } else {
    foo()
  }
} 