// RUN: %target-run-simple-swift(-Xfrontend -enable-implicit-dynamic) | %FileCheck %s

// REQUIRES: executable_test
// REQUIRES: objc_interop

import Foundation

class C : NSObject {
  @nonobjc func foo() {
    print("original")
  }
}

extension C {
  @_dynamicReplacement(for: foo()) @nonobjc private func replacement_for_foo() {
    print("replacement")
  }
}

func doit() {
  let c = C()
  c.foo()
}

// CHECK: replacement
doit()
