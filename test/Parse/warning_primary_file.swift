// RUN: %target-swift-frontend -emit-sil -verify -primary-file %s %S/Inputs/warning_nonprimary_file.swift

// Tests that parse warnings are only emitted for the primary
// input. Below, we expect a warning diagnostic to be emitted
// because this is the primary file.
//
// However, we also pull in ./warning_nonprimary_file.swift,
// which would emit a warning for an unknown operating system
// configuration argument, but doesn't because it's not the
// primary input.

public func foo(x: Int, y: Int) -> Int {
  return x + y
}

public func baz(x: Int, y: Int) -> Int {
  var z = x // expected-warning {{variable 'z' was never mutated}}
  return z + y
}

