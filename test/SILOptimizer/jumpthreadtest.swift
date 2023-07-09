// RUN: %target-swift-frontend -O -parse-as-library -emit-sil -enable-ossa-modules %s | %FileCheck %s
// REQUIRES: PTRSIZE=32,swift_stdlib_asserts

import Swift

// CHECK-LABEL: sil [noinline] @$s14jumpthreadtest3fooys6UInt64Vs5UInt8VF :
// CHECK: bb0
// CHECK:  [[FUNC:%.*]] = function_ref @$ss17FixedWidthIntegerPsE15_truncatingInityxqd__SzRd__lFZs6UInt64V_s5UInt8VTgmq5 :
// CHECK: apply [[FUNC]]
// CHECK-NOT: bb1
// CHECK-LABEL: } // end sil function '$s14jumpthreadtest3fooys6UInt64Vs5UInt8VF'
@inlinable
@inline(never)
public func foo(_ p:UInt8) -> UInt64 {
  let q = UInt64(truncatingIfNeeded: p)
  return q
}
