// RUN: %target-swift-emit-silgen -module-name=test -primary-file %s | %FileCheck %s
// RUN: %target-swift-emit-sil -O -sil-verify-all %s

@_moveOnly
public struct GenericMoveOnly<T> {
  var i: Int
  var s: T

  // CHECK-LABEL: sil [ossa] @$s4test15GenericMoveOnlyVfD : $@convention(method) <T> (@in GenericMoveOnly<T>) -> ()
  // CHECK:         [[DD:%.*]] = drop_deinit %0 : $*GenericMoveOnly<T>
  // CHECK:         [[SE:%.*]] = struct_element_addr %2 : $*GenericMoveOnly<T>, #GenericMoveOnly.s
  // CHECK:         [[A:%.*]] = begin_access [deinit] [static] %3 : $*T
  // CHECK:         destroy_addr [[A]] : $*T
  // CHECK:       } // end sil function '$s4test15GenericMoveOnlyVfD'
  deinit {
  }
}


