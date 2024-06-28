// RUN: %target-swiftxx-frontend -I %S/Inputs -enable-sil-opaque-values -emit-silgen %s | %FileCheck --dump-input-filter=all %s

// REQUIRES: OS=macosx

import Closure

// CHECK: sil shared [transparent] [serialized] [reabstraction_thunk] [ossa] @$sSo7ARCWeakVIetCi_ABIegn_TR : $@convention(thin) (@in_guaranteed ARCWeak, @convention(c) (@in ARCWeak) -> ()) -> () {
// CHECK: bb0(%[[V0:.*]] : @guaranteed $ARCWeak, %[[V1:.*]] : $@convention(c) (@in ARCWeak) -> ()):
// CHECK: %[[V2:.*]] = copy_value %[[V0]] : $ARCWeak
// CHECK: apply %[[V1]](%[[V2]]) : $@convention(c) (@in ARCWeak) -> ()

public func testARCWeakFunctionPointer2() -> (ARCWeak) -> () {
  return getFnPtr2()
}
