// RUN: %target-swift-frontend -module-name access_enforcement_noescape -enforce-exclusivity=checked -Onone -emit-sil -swift-version 4 -verify -parse-as-library %s
// REQUIRES: asserts

// This is the subset of tests from access_enforcement_noescape.swift
// that cause compile-time errors. All the tests are organized in one
// place so it's easy to see that coverage of all the combinations of
// access patterns is exhaustive. But the ones that cause compile time
// errors are commented out in the original file and compiled here
// instead with the '-verify' option.

// Helper
func doOne(_ f: () -> ()) {
  f()
}

// Helper
func doOneInout(_: ()->(), _: inout Int) {}

// Error: noescape read + write inout.
func readWriteInout() {
  var x = 3
  // Around the call: [modify] [static]
  // Inside closure: [modify] [static]
  // expected-error@+2{{overlapping accesses to 'x', but modification requires exclusive access; consider copying to a local variable}}
  // expected-note@+1{{conflicting access is here}}
  doOneInout({ _ = x }, &x)
}

// Error: noescape read + write inout of an inout.
func inoutReadWriteInout(x: inout Int) {
  // Around the call: [modify] [static]
  // Inside closure: [modify] [static]
  // expected-error@+2{{overlapping accesses to 'x', but modification requires exclusive access; consider copying to a local variable}}
  // expected-note@+1{{conflicting access is here}}
  doOneInout({ _ = x }, &x)
}

// Error: on noescape write + write inout.
func writeWriteInout() {
  var x = 3
  // Around the call: [modify] [static]
  // Inside closure: [modify] [static]
  // expected-error@+2{{overlapping accesses to 'x', but modification requires exclusive access; consider copying to a local variable}}
  // expected-note@+1{{conflicting access is here}}
  doOneInout({ x = 42 }, &x)
}

// Error: on noescape write + write inout.
func inoutWriteWriteInout(x: inout Int) {
  // Around the call: [modify] [static]
  // Inside closure: [modify] [static]
  // expected-error@+2{{overlapping accesses to 'x', but modification requires exclusive access; consider copying to a local variable}}
  // expected-note@+1{{conflicting access is here}}
  doOneInout({ x = 42 }, &x)
}

// Helper
func doBlockInout(_: @convention(block) ()->(), _: inout Int) {}

func readBlockWriteInout() {
  var x = 3
  // Around the call: [modify] [static]
  // Inside closure: [read] [static]
  // expected-error@+2{{overlapping accesses to 'x', but modification requires exclusive access; consider copying to a local variable}}
  // expected-note@+1{{conflicting access is here}}
  doBlockInout({ _ = x }, &x)
}

// Test AccessSummaryAnalysis.
//
// The captured @inout_aliasable argument to `doOne` is re-partially applied,
// then stored is a box before passing it to doBlockInout.
func noEscapeBlock() {
  var x = 3
  doOne {
    // expected-error@+2{{overlapping accesses to 'x', but modification requires exclusive access; consider copying to a local variable}}
    // expected-note@+1{{conflicting access is here}}
    doBlockInout({ _ = x }, &x)
  }
}
