// RUN: %target-swift-frontend -experimental-performance-annotations %s -sil-verify-all -module-name=test -emit-sil | %FileCheck %s

// REQUIRES: swift_in_compiler,swift_stdlib_no_asserts,optimized_stdlib
// UNSUPPORTED: swift_test_mode_optimize

public struct Stack<T> {
    var size = 42
}

// CHECK-LABEL: sil [no_allocation] @$s4test11createStackyyF :
// CHECK:         [[F:%[0-9]+]] = function_ref @$s4test5StackVACyxGycfCSi_Tgm5
// CHECK:         [[S:%[0-9]+]] = apply [[F]]()
// CHECK:         debug_value [[S]]
// CHECK:       } // end sil function '$s4test11createStackyyF'
@_noAllocation
public func createStack() {
    let s = Stack<Int>()
    _ = s.size
}

