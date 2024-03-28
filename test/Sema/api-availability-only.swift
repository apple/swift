/// Verify that -check-api-availability-only has no effect since it is deprecated.
// RUN: %target-typecheck-verify-swift -check-api-availability-only -verify

// REQUIRES: OS=macosx

@available(macOS 10.52, *)
public struct S {}

@available(macOS 10.51, *)
public func newFunc() {
  _ = S() // expected-error {{'S' is only available in}}
  // expected-note @-1 {{add 'if #available' version check}}
  // expected-note @-2 {{change the @available attribute of the global function on macOS from 10.51 to 10.52}} {{9:18-23=10.52}}
}
