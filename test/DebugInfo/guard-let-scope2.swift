// REQUIRES: objc_interop
// RUN: %target-swift-frontend -emit-sil -Xllvm -sil-print-debuginfo %s \
// RUN:  | %FileCheck %s
import Foundation

func takeClosure2 (_ closure: @escaping () -> Bool) {  assert(closure()) }

struct SomeObject {
  var s = ""
  var today = Date()
}

public func f(x: String?) throws {
  var s : SomeObject? =  nil
  takeClosure2 {
    s = SomeObject()
    return s != nil
  }
  // CHECK: sil_scope [[S1:[0-9]+]] { {{.*}}:13:13 parent @{{.*}}1f
  // CHECK: sil_scope [[S2:[0-9]+]] { {{.*}}:14:7  parent [[S1]] }
  // CHECK: sil_scope [[S3:[0-9]+]] { {{.*}}:14:26 parent [[S1]] }
  // CHECK: sil_scope [[S4:[0-9]+]] { {{.*}}:25:3 parent [[S2]] }
  // CHECK: sil_scope [[S5:[0-9]+]] { {{.*}}:25:17 parent [[S4]] }
  // CHECK: alloc_stack {{.*}} $SomeObject, let, name "s", {{.*}} scope [[S5]]
  guard let s = s else {
    assert(false)
    return
  }
}
