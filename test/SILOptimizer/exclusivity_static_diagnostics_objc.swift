// RUN: %target-swift-frontend(mock-sdk: %clang-importer-sdk) -import-objc-header %S/Inputs/optional_closure_bridging.h -enforce-exclusivity=checked -swift-version 4 -emit-sil -primary-file %s -o /dev/null -verify
// REQUIRES: objc_interop

import Foundation

class SomeClass {
  @objc
  func testCallNoEscapeBlockDirectly(_ c: @convention(block) () -> ()) {
    c()
  }
}

func testOptionalImport() {
  var x = 0
  // expected-error@+2{{overlapping accesses to 'x', but modification requires exclusive access; consider copying to a local variable}}
  // expected-note@+1{{conflicting access is here}}
  SomeObjCInterface.perform(&x) { x += 1 }
}
