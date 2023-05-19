// RUN: %empty-directory(%t)
// RUN: %target-swift-frontend -enable-copy-propagation=requested-passes-only -enable-lexical-lifetimes=false -Xllvm -sil-disable-pass=Simplification -emit-module-path %t/Test.swiftmodule -emit-sil -o /dev/null -module-name Test %s -sdk "" -import-objc-header %S/Inputs/serialization-sil.h
// RUN: %target-sil-func-extractor %t/Test.swiftmodule -sil-print-debuginfo  -func='$s4Test16testPartialApplyyySoAA_pF' -o - | %FileCheck %s

// REQUIRES: objc_interop

// @_transparent to force serialization.
@_transparent
public func testPartialApply(_ obj: Test) {
  // CHECK-LABEL: @$s4Test16testPartialApplyyySoAA_pF : $@convention(thin) (@guaranteed any Test) -> () {
  if let curried1 = obj.normalObject {
    // CHECK: dynamic_method_br [[CURRIED1_OBJ:%.+]] : $@opened([[CURRIED1_EXISTENTIAL:.+]], any Test) Self, #Test.normalObject!foreign, [[CURRIED1_TRUE:[^,]+]], [[CURRIED1_FALSE:[^,]+]]
    // CHECK: [[CURRIED1_FALSE]]:
    // CHECK: [[CURRIED1_TRUE]]([[CURRIED1_METHOD:%.+]] : $@convention(objc_method) (@opened([[CURRIED1_EXISTENTIAL]], any Test) Self) -> @autoreleased AnyObject):
    // CHECK:   [[CURRIED1_OBJECT_COPY:%.*]] = copy_value [[CURRIED1_OBJ]]
    // CHECK:   [[CURRIED1_PARTIAL:%.+]] = partial_apply [callee_guaranteed] [[CURRIED1_METHOD]]([[CURRIED1_OBJECT_COPY]]) : $@convention(objc_method) (@opened([[CURRIED1_EXISTENTIAL]], any Test) Self) -> @autoreleased AnyObject
    // CHECK:   [[CURRIED1_THUNK:%.+]] = function_ref @$syXlIego_ypIegr_TR : $@convention(thin) (@guaranteed @callee_guaranteed () -> @owned AnyObject) -> @out Any
    // CHECK:   = partial_apply [callee_guaranteed] [[CURRIED1_THUNK]]([[CURRIED1_PARTIAL]])
    curried1()
  }
  if let curried2 = obj.innerPointer {
    // CHECK: dynamic_method_br [[CURRIED2_OBJ:%.+]] : $@opened([[CURRIED2_EXISTENTIAL:.+]], any Test) Self, #Test.innerPointer!foreign, [[CURRIED2_TRUE:[^,]+]], [[CURRIED2_FALSE:[^,]+]]
    // CHECK: [[CURRIED2_FALSE]]:
    // CHECK: [[CURRIED2_TRUE]]([[CURRIED2_METHOD:%.+]] : $@convention(objc_method) (@opened([[CURRIED2_EXISTENTIAL]], any Test) Self) -> @unowned_inner_pointer UnsafeMutableRawPointer):
    // CHECK: [[CURRIED2_OBJ_COPY:%.*]] = copy_value [[CURRIED2_OBJ]]
    // CHECK: [[CURRIED2_PARTIAL:%.+]] = partial_apply [callee_guaranteed] [[CURRIED2_METHOD]]([[CURRIED2_OBJ_COPY]]) : $@convention(objc_method) (@opened([[CURRIED2_EXISTENTIAL]], any Test) Self) -> @unowned_inner_pointer UnsafeMutableRawPointer
    curried2()
  }
  if let prop1 = obj.normalObjectProp {
    // CHECK: dynamic_method_br [[PROP1_OBJ:%.+]] : $@opened([[PROP1_EXISTENTIAL:.+]], any Test) Self, #Test.normalObjectProp!getter.foreign, [[PROP1_TRUE:[^,]+]], [[PROP1_FALSE:[^,]+]]
    // CHECK: [[PROP1_FALSE]]:
    // CHECK: [[PROP1_TRUE]]([[PROP1_METHOD:%.+]] : $@convention(objc_method) (@opened([[PROP1_EXISTENTIAL]], any Test) Self) -> @autoreleased AnyObject):
    // CHECK: [[PROP1_OBJ_COPY:%.*]] = copy_value [[PROP1_OBJ]]
    // CHECK: [[PROP1_PARTIAL:%.+]] = partial_apply [callee_guaranteed] [[PROP1_METHOD]]([[PROP1_OBJ_COPY]]) : $@convention(objc_method) (@opened([[PROP1_EXISTENTIAL]], any Test) Self) -> @autoreleased AnyObject
    // CHECK: = apply [[PROP1_PARTIAL]]() : $@callee_guaranteed () -> @owned AnyObject
    _ = prop1
  }
  if let prop2 = obj.innerPointerProp {
    // CHECK: dynamic_method_br [[PROP2_OBJ:%.+]] : $@opened([[PROP2_EXISTENTIAL:.+]], any Test) Self, #Test.innerPointerProp!getter.foreign, [[PROP2_TRUE:[^,]+]], [[PROP2_FALSE:[^,]+]]
    // CHECK: [[PROP2_FALSE]]:
    // CHECK: [[PROP2_TRUE]]([[PROP2_METHOD:%.+]] : $@convention(objc_method) (@opened([[PROP2_EXISTENTIAL]], any Test) Self) -> @unowned_inner_pointer UnsafeMutableRawPointer):
    // CHECK: [[PROP2_OBJ_COPY:%.*]] = copy_value [[PROP2_OBJ]]
    // CHECK: [[PROP2_PARTIAL:%.+]] = partial_apply [callee_guaranteed] [[PROP2_METHOD]]([[PROP2_OBJ_COPY]]) : $@convention(objc_method) (@opened([[PROP2_EXISTENTIAL]], any Test) Self) -> @unowned_inner_pointer UnsafeMutableRawPointer
    // CHECK: = apply [[PROP2_PARTIAL]]() : $@callee_guaranteed () -> UnsafeMutableRawPointer
    _ = prop2
  }
} // CHECK: // end sil function '$s4Test16testPartialApplyyySoAA_pF'
