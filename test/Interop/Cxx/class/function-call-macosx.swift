// RUN: %target-swiftxx-frontend -I %S/Inputs -emit-silgen %s | %FileCheck --dump-input-filter=all %s

// REQUIRES: OS=macosx

import Closure

// CHECK: sil [ossa] @$s4main11testARCWeakyyF : $@convention(thin) () -> () {
// CHECK: %[[V0:.*]] = alloc_stack $ARCWeak
// CHECK: %[[V2:.*]] = function_ref @_ZN7ARCWeakC1Ev : $@convention(c) () -> @out ARCWeak
// CHECK: %[[V3:.*]] = apply %[[V2]](%[[V0]]) : $@convention(c) () -> @out ARCWeak
// CHECK: %[[V4:.*]] = function_ref @_Z12cfuncARCWeak7ARCWeak : $@convention(c) (@in ARCWeak) -> ()
// CHECK: %[[V7:.*]] = apply %[[V4]](%[[V0]]) : $@convention(c) (@in ARCWeak) -> ()
// CHECK-NOT: destroy_addr
// CHECK: dealloc_stack %[[V0]] : $*ARCWeak

public func testARCWeak() {
  cfuncARCWeak(ARCWeak());
}

// CHECK: sil [ossa] @$s4main26testARCWeakFunctionPointeryyF : $@convention(thin) () -> () {
// CHECK: %[[V0:.*]] = function_ref @_Z9getFnPtr2v : $@convention(c) () -> @convention(c) (@in ARCWeak) -> ()
// CHECK: %[[V1:.*]] = apply %[[V0]]() : $@convention(c) () -> @convention(c) (@in ARCWeak) -> ()
// CHECK: %[[V3:.*]] = alloc_stack $ARCWeak
// CHECK: %[[V6:.*]] = function_ref @_ZN7ARCWeakC1Ev : $@convention(c) () -> @out ARCWeak
// CHECK: apply %[[V6]](%[[V3]]) : $@convention(c) () -> @out ARCWeak
// CHECK: apply %[[V1]](%[[V3]]) : $@convention(c) (@in ARCWeak) -> ()
// CHECK-NEXT: dealloc_stack %[[V3]] : $*ARCWeak
// CHECK-NEXT: %[[V9:.*]] = tuple ()
// CHECK-NEXT: return %[[V9]] : $()

public func testARCWeakFunctionPointer() {
  let f = getFnPtr2()
  f(ARCWeak())
}
