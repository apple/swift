// RUN: %swift -typecheck -verify -target %target-cpu-apple-macosx11 %s

// REQUIRES: OS=macosx

@available(macOS 13, *)
enum MyError: Error {
  case fail
}

@available(macOS 12, *)
func throwMyErrorBadly() throws(MyError) { }
// expected-error@-1{{'MyError' is only available in macOS 13 or newer}}
// expected-note@-2 {{change the @available attribute of the global function on macOS from 12 to 13}}
