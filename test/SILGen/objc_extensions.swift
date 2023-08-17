
// RUN: %target-swift-emit-silgen -module-name objc_extensions -sdk %S/Inputs/ -I %S/Inputs -enable-source-import %s | %FileCheck %s

// REQUIRES: objc_interop

import Foundation
import objc_extensions_helper

class Sub : Base {}

// CHECK-LABEL: sil hidden [ossa] @$s15objc_extensions20testOverridePropertyyyAA3SubCF
func testOverrideProperty(_ obj: Sub) {
  // CHECK: bb0([[ARG:%.*]] : @guaranteed $Sub):
  // CHECK: = objc_method [[ARG]] : $Sub, #Sub.prop!setter.foreign : (Sub) -> (String?) -> ()
  obj.prop = "abc"
} // CHECK: } // end sil function '$s15objc_extensions20testOverridePropertyyyAA3SubCF'

testOverrideProperty(Sub())

extension Sub {
  @objc override var prop: String! {
    didSet(oldValue) {
      // Ignore it, but explicitly specify 'oldValue' because this test
      // checks that its correctly fetched.
    }

    // Make sure that we are generating the @objc thunk and are calling the actual method.
    //
    // CHECK-LABEL: sil private [thunk] [ossa] @$s15objc_extensions3SubC4propSSSgvgTo : $@convention(objc_method) (Sub) -> @autoreleased Optional<NSString> {
    // CHECK: bb0([[SELF:%.*]] : @unowned $Sub):
    // CHECK: [[SELF_COPY:%.*]] = copy_value [[SELF]]
    // CHECK: [[BORROWED_SELF_COPY:%.*]] = begin_borrow [[SELF_COPY]]
    // CHECK: [[GETTER_FUNC:%.*]] = function_ref @$s15objc_extensions3SubC4propSSSgvg : $@convention(method) (@guaranteed Sub) -> @owned Optional<String>
    // CHECK: apply [[GETTER_FUNC]]([[BORROWED_SELF_COPY]])
    // CHECK: end_borrow [[BORROWED_SELF_COPY]]
    // CHECK: destroy_value [[SELF_COPY]]
    // CHECK: } // end sil function '$s15objc_extensions3SubC4propSSSgvgTo'

    // Then check the body of the getter calls the super_method.
    // CHECK-LABEL: sil hidden [ossa] @$s15objc_extensions3SubC4propSSSgvg : $@convention(method) (@guaranteed Sub) -> @owned Optional<String> {
    // CHECK: bb0([[SELF:%.*]] : @guaranteed $Sub):
    // CHECK: [[SELF_COPY:%.*]] = copy_value [[SELF]]
    // CHECK: [[SELF_COPY_CAST:%.*]] = upcast [[SELF_COPY]] : $Sub to $Base
    // CHECK: [[BORROWED_SELF_COPY_CAST:%.*]] = begin_borrow [[SELF_COPY_CAST]]
    // CHECK: [[CAST_BACK:%.*]] = unchecked_ref_cast [[BORROWED_SELF_COPY_CAST]] : $Base to $Sub
    // CHECK: [[SUPER_METHOD:%.*]] = objc_super_method [[CAST_BACK]] : $Sub, #Base.prop!getter.foreign
    // CHECK: [[RESULT:%.*]] = apply [[SUPER_METHOD]]([[BORROWED_SELF_COPY_CAST]])
    // CHECK: bb3(
    // CHECK: end_borrow [[BORROWED_SELF_COPY_CAST]]
    // CHECK: destroy_value [[SELF_COPY_CAST]]
    // CHECK: } // end sil function '$s15objc_extensions3SubC4propSSSgvg'

    // Then check the setter @objc thunk.
    //
    // CHECK-LABEL: sil private [thunk] [ossa] @$s15objc_extensions3SubC4propSSSgvsTo : $@convention(objc_method) (Optional<NSString>, Sub) -> () {
    // CHECK: bb0([[NEW_VALUE:%.*]] : @unowned $Optional<NSString>, [[SELF:%.*]] : @unowned $Sub):
    // CHECK:   [[NEW_VALUE_COPY:%.*]] = copy_value [[NEW_VALUE]]
    // CHECK:   [[SELF_COPY:%.*]] = copy_value [[SELF]] : $Sub
    // CHECK:   switch_enum [[NEW_VALUE_COPY]] : $Optional<NSString>, case #Optional.some!enumelt: [[SUCC_BB:bb[0-9]+]], case #Optional.none!enumelt: [[FAIL_BB:bb[0-9]+]]
    // CHECK: [[SUCC_BB]]([[STR:%.*]] : @owned $NSString):
    // CHECK: [[FAIL_BB]]:
    // CHECK: bb3([[BRIDGED_NEW_VALUE:%.*]] : @owned $Optional<String>):
    // CHECK:   [[BORROWED_SELF_COPY:%.*]] = begin_borrow [[SELF_COPY]]
    // CHECK:   [[NORMAL_FUNC:%.*]] = function_ref @$s15objc_extensions3SubC4propSSSgvs : $@convention(method) (@owned Optional<String>, @guaranteed Sub) -> ()
    // CHECK:   apply [[NORMAL_FUNC]]([[BRIDGED_NEW_VALUE]], [[BORROWED_SELF_COPY]])
    // CHECK:   end_borrow [[BORROWED_SELF_COPY]]
    // CHECK:   destroy_value [[SELF_COPY]]
    // CHECK: } // end sil function '$s15objc_extensions3SubC4propSSSgvsTo'

    // Then check the body of the actually setter value and make sure that we
    // call the didSet function.
    // CHECK-LABEL: sil hidden [ossa] @$s15objc_extensions3SubC4propSSSgvs : $@convention(method) (@owned Optional<String>, @guaranteed Sub) -> () {

    // First we get the old value.
    // CHECK: bb0([[NEW_VALUE:%.*]] : @owned $Optional<String>, [[SELF:%.*]] : @guaranteed $Sub):
    // CHECK:   [[SELF_COPY:%.*]] = copy_value [[SELF]]
    // CHECK:   [[UPCAST_SELF_COPY:%.*]] = upcast [[SELF_COPY]] : $Sub to $Base
    // CHECK:   [[BORROWED_UPCAST_SELF_COPY:%.*]] = begin_borrow [[UPCAST_SELF_COPY]]
    // CHECK:   [[CAST_BACK:%.*]] = unchecked_ref_cast [[BORROWED_UPCAST_SELF_COPY]] : $Base to $Sub
    // CHECK:   [[GET_SUPER_METHOD:%.*]] = objc_super_method [[CAST_BACK]] : $Sub, #Base.prop!getter.foreign : (Base) -> () -> String?, $@convention(objc_method) (Base) -> @autoreleased Optional<NSString>
    // CHECK:   [[OLD_NSSTRING:%.*]] = apply [[GET_SUPER_METHOD]]([[BORROWED_UPCAST_SELF_COPY]])

    // CHECK: bb3([[OLD_NSSTRING_BRIDGED:%.*]] : @owned $Optional<String>):
    // This next line is completely not needed. But we are emitting it now.
    // CHECK:   destroy_value [[UPCAST_SELF_COPY]]
    // CHECK:   [[SELF_COPY:%.*]] = copy_value [[SELF]]
    // CHECK:   [[UPCAST_SELF_COPY:%.*]] = upcast [[SELF_COPY]] : $Sub to $Base
    // CHECK:   [[BORROWED_NEW_VALUE:%.*]] = begin_borrow [[NEW_VALUE]]
    // CHECK:   [[NEW_VALUE_COPY:%.*]] = copy_value [[BORROWED_NEW_VALUE]]
    // CHECK:   switch_enum [[NEW_VALUE_COPY]] : $Optional<String>, case #Optional.some!enumelt: [[SOME_BB:bb[0-9]+]], case #Optional.none!enumelt: [[NONE_BB:bb[0-9]+]]
    //
    // CHECK: bb4([[OLD_STRING:%.*]] : @owned $String):
    // CHECK: bb6([[BRIDGED_NEW_STRING:%.*]] : @owned $Optional<NSString>):
    // CHECK:   [[BORROWED_UPCAST_SELF:%.*]] = begin_borrow [[UPCAST_SELF_COPY]] : $Base
    // CHECK:   [[SUPERREF_DOWNCAST:%.*]] = unchecked_ref_cast [[BORROWED_UPCAST_SELF]] : $Base to $Sub
    // CHECK:   [[SET_SUPER_METHOD:%.*]] = objc_super_method [[SUPERREF_DOWNCAST]] : $Sub, #Base.prop!setter.foreign : (Base) -> (String?) -> (), $@convention(objc_method) (Optional<NSString>, Base) -> ()
    // CHECK:    apply [[SET_SUPER_METHOD]]([[BRIDGED_NEW_STRING]], [[UPCAST_SELF_COPY]])
    // CHECK:    destroy_value [[BRIDGED_NEW_STRING]]
    // CHECK:    destroy_value [[UPCAST_SELF_COPY]]
    // This is an identity cast that should be eliminated by SILGen peepholes.
    // CHECK:   [[BORROWED_OLD_NSSTRING_BRIDGED:%.*]] = begin_borrow [[OLD_NSSTRING_BRIDGED]]
    // CHECK:    [[DIDSET_NOTIFIER:%.*]] = function_ref @$s15objc_extensions3SubC4propSSSgvW : $@convention(method) (@guaranteed Optional<String>, @guaranteed Sub) -> ()
    // CHECK:    apply [[DIDSET_NOTIFIER]]([[BORROWED_OLD_NSSTRING_BRIDGED]], [[SELF]])
    // CHECK:    end_borrow [[BORROWED_OLD_NSSTRING_BRIDGED]]
    // CHECK:    destroy_value [[OLD_NSSTRING_BRIDGED]]
    // CHECK:    destroy_value [[NEW_VALUE]]
    // CHECK: } // end sil function '$s15objc_extensions3SubC4propSSSgvs'

  }

  @objc func foo() {
  }

  @objc override func objCBaseMethod() {}
}

// CHECK-LABEL: sil private [ossa] @$s15objc_extensions9testCurryyyAA3SubCFyycADcfu_ : $@convention(thin) (@guaranteed Sub) -> @owned @callee_guaranteed () -> ()
// CHECK: function_ref @$s15objc_extensions9testCurryyyAA3SubCFyycADcfu_yycfu0_ : $@convention(thin) (@guaranteed Sub) -> ()
// CHECK: } // end sil function '$s15objc_extensions9testCurryyyAA3SubCFyycADcfu_'

// CHECK-LABEL: sil private [ossa] @$s15objc_extensions9testCurryyyAA3SubCFyycADcfu_yycfu0_ : $@convention(thin) (@guaranteed Sub) -> ()
// CHECK: objc_method %0 : $Sub, #Sub.foo!foreign : (Sub) -> () -> (), $@convention(objc_method) (Sub) -> ()
// CHECK: } // end sil function '$s15objc_extensions9testCurryyyAA3SubCFyycADcfu_yycfu0_'
func testCurry(_ x: Sub) {
  _ = x.foo
}

extension Sub {
  @objc var otherProp: String {
    get { return "hello" }
    set { }
  }
}

class SubSub : Sub {
  // CHECK-LABEL: sil hidden [ossa] @$s15objc_extensions03SubC0C14objCBaseMethodyyF :
  // CHECK: bb0([[SELF:%.*]] : @guaranteed $SubSub):
  // CHECK:   [[SELF_COPY:%.*]] = copy_value [[SELF]]
  // CHECK:   [[UPCAST_SELF_COPY:%.*]] = upcast [[SELF_COPY]] : $SubSub to $Sub
  // CHECK:   [[BORROWED_UPCAST_SELF_COPY:%.*]] = begin_borrow [[UPCAST_SELF_COPY]]
  // CHECK:   [[DOWNCAST:%.*]] = unchecked_ref_cast [[BORROWED_UPCAST_SELF_COPY]] : $Sub to $SubSub
  // CHECK:   objc_super_method [[DOWNCAST]] : $SubSub, #Sub.objCBaseMethod!foreign : (Sub) -> () -> (), $@convention(objc_method) (Sub) -> ()
  // CHECK:   end_borrow [[BORROWED_UPCAST_SELF_COPY]]
  // CHECK: } // end sil function '$s15objc_extensions03SubC0C14objCBaseMethodyyF'
  override func objCBaseMethod() {
    super.objCBaseMethod()
  }
}

extension SubSub {
  // CHECK-LABEL: sil hidden [ossa] @$s15objc_extensions03SubC0C9otherPropSSvs
  // CHECK: bb0([[NEW_VALUE:%.*]] : @owned $String, [[SELF:%.*]] : @guaranteed $SubSub):
  // CHECK:   [[SELF_COPY_1:%.*]] = copy_value [[SELF]]
  // CHECK:   [[UPCAST_SELF_COPY_1:%.*]] = upcast [[SELF_COPY_1]] : $SubSub to $Sub
  // CHECK:   [[BORROWED_UPCAST_SELF_COPY_1:%.*]] = begin_borrow [[UPCAST_SELF_COPY_1]]
  // CHECK:   [[DOWNCAST_BORROWED_UPCAST_SELF_COPY_1:%.*]] = unchecked_ref_cast [[BORROWED_UPCAST_SELF_COPY_1]] : $Sub to $SubSub
  // CHECK:   = objc_super_method [[DOWNCAST_BORROWED_UPCAST_SELF_COPY_1]] : $SubSub, #Sub.otherProp!getter.foreign
  // CHECK:   end_borrow [[BORROWED_UPCAST_SELF_COPY_1]]

  // CHECK:   [[SELF_COPY_2:%.*]] = copy_value [[SELF]]
  // CHECK:   [[UPCAST_SELF_COPY_2:%.*]] = upcast [[SELF_COPY_2]] : $SubSub to $Sub
  // CHECK:   [[BORROWED_UPCAST_SELF_COPY_2:%.*]] = begin_borrow [[UPCAST_SELF_COPY_2]]
  // CHECK:   [[DOWNCAST_BORROWED_UPCAST_SELF_COPY_2:%.*]] = unchecked_ref_cast [[BORROWED_UPCAST_SELF_COPY_2]] : $Sub to $SubSub
  // CHECK:   = objc_super_method [[DOWNCAST_BORROWED_UPCAST_SELF_COPY_2]] : $SubSub, #Sub.otherProp!setter.foreign
  // CHECK:   end_borrow [[BORROWED_UPCAST_SELF_COPY_2]]
  // CHECK: } // end sil function '$s15objc_extensions03SubC0C9otherPropSSvs'
  @objc override var otherProp: String {
    didSet(oldValue) {
      // Ignore it.
    }
  }
}

// https://github.com/apple/swift/issues/43637

extension Base {
  fileprivate static var x = 1
}

// CHECK-LABEL: sil hidden [ossa] @$s15objc_extensions19testStaticVarAccessyyF
func testStaticVarAccess() {
  // CHECK: [[F:%.*]] = function_ref @$sSo4BaseC15objc_extensionsE1x33_1F05E59585E0BB585FCA206FBFF1A92DLLSivau
  // CHECK: [[PTR:%.*]] = apply [[F]]()
  // CHECK: [[ADDR:%.*]] = pointer_to_address [[PTR]]
  _ = Base.x
}
