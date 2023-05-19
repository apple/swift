// RUN: %target-swift-frontend -import-objc-header %S/Inputs/enum-stack-protection.h %s -Onone -module-name=test -emit-sil | %FileCheck %s

// Check that accessing an imported enum doesn't trigger stack protection.

// CHECK-LABEL: sil @$s4test6testityyF : $@convention(thin) () -> () {
// CHECK:        address_to_pointer %{{[0-9]+}}
// CHECK:        address_to_pointer %{{[0-9]+}}
// CHECK:      } // end sil function '$s4test6testityyF'
public func testit() {
  var s = S()
  s.a = 1
  s.b = 2
  s.c = 3
  useit(s)
}

@inline(never)
public func useit(_ s: S) {
}

