// RUN: %sourcekitd-test  -req=diags %s -- %s %S/Inputs/generic_class.swift -enable-experimental-feature Embedded 2>&1 | %FileCheck %s

// check that SourceKit does not crash on this

public func testit() -> X<Int> {
  // CHECK: key.diagnostics
  return X(27)
}
