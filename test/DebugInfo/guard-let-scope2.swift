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
  // CHECK: sil_scope [[S1:[0-9]+]] { {{.*}} parent @{{.*}}1f
  // CHECK: sil_scope [[S2:[0-9]+]] { {{.*}} parent [[S1]] }
  // CHECK: sil_scope [[S3:[0-9]+]] { {{.*}} parent [[S1]] }
  // CHECK: sil_scope [[S4:[0-9]+]] { {{.*}} parent [[S2]] }
  // CHECK: alloc_stack {{.*}} $SomeObject, let, name "s", {{.*}} scope [[S4]]
  guard let s = s else {
    assert(false)
    return
  }
}
