// RUN: %target-swift-frontend -emit-silgen %s | %FileCheck %s

// CHECK-LABEL: // checkDiscardedConstant()
func checkDiscardedConstant() {
  _ = 0
  // CHECK debug_value %{{.*}} : $Int, let, name "_", discarded //
  let `_` = 0
  // CHECK: debug_value %{{.*}} : $Int, let, name "_" //
}

// CHECK-LABEL: // checkDiscardedReference()
func checkDiscardedReference() {
  let x = 1
  // CHECK: debug_value %[[Value_1:.*]] : $Int, let, name "x" //
  _ = x
  // CHECK: debug_value %[[Value_1]] : $Int, let, name "_", discarded //
  let `_` = x
  // CHECK: debug_value %[[Value_1]] : $Int, let, name "_" //
}

// CHECK-LABEL: // checkDiscardedVar()
func checkDiscardedVar() {
  var x = 1
  // CHECK: alloc_box ${ var Int }, var, name "x" //
  _ = x
  // CHECK: debug_value_addr %{{.*}} : $*Int, let, name "_", discarded //
  var `_` = x
  // CHECK: alloc_box ${ var Int }, var, name "_" //
}

// CHECK-LABEL: // checkMultipleAssignments()
func checkMultipleAssignments() {
  _ = 0
  // CHECK debug_value %{{.*}} : $Int, let, name "_", discarded //
  _ = 1
  // CHECK debug_value %{{.*}} : $Int, let, name "_", discarded //
  let x = 1
  // CHECK: debug_value %[[Value_1:.*]] : $Int, let, name "x" //
  _ = x
  // CHECK: debug_value %[[Value_1]] : $Int, let, name "_", discarded //
}
