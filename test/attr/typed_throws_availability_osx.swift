// RUN: %swift -typecheck -verify -target %target-cpu-apple-macosx11 %s

// REQUIRES: OS=macosx

@available(macOS 13, *)
enum MyError: Error {
  case fail
}

@available(macOS 12, *)
func throwMyErrorBadly() throws(MyError) { } // expected-note {{update @available attribute for macOS from '12' to '13' to meet the requirements of 'MyError'}} {{10:18-20=13}}
// expected-error@-1{{'MyError' is only available in macOS 13 or newer}}
