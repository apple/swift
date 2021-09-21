// RUN: %target-swift-emit-silgen -parse-stdlib %s -disable-access-control -disable-objc-attr-requires-foundation-module | %FileCheck %s

import Swift

class Klass {}

// CHECK-LABEL: sil hidden [ossa] @$s8moveonly7useMoveyAA5KlassCADF : $@convention(thin) (@guaranteed Klass) -> @owned @move_only Klass {
// CHECK: bb0([[ARG:%.*]] :
// CHECK-NEXT: debug_value
// CHECK-NEXT: copy_value
// CHECK-NEXT: move_value
// CHECK-NEXT: return
// CHECK: } // end sil function '$s8moveonly7useMoveyAA5KlassCADF'
func useMove(_ k: Klass) -> @_moveOnly Klass {
    Builtin.move(k)
}

// Disable this test until we figure out the type lowering issue.
//
// XCHECK-LABEL: sil hidden [ossa] @$s8moveonly7useMoveyxxRlzClF : $@convention(thin) <T where T : AnyObject> (@guaranteed T) -> @owned T {
// XCHECK: bb0([[ARG:%.*]] :
// XCHECK-NEXT: debug_value
// XCHECK-NEXT: copy_value
// XCHECK-NEXT: move_value
// XCHECK-NEXT: return
// XCHECK: } // end sil function '$s8moveonly7useMoveyxxRlzClF'
//func useMove<T : AnyObject>(_ k: T) -> @_moveOnly T {
//    Builtin.move(k)
//}
