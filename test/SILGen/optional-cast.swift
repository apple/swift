
// RUN: %target-swift-emit-silgen %s | %FileCheck %s

class A {}
class B : A {}

// CHECK-LABEL: sil hidden [ossa] @$s4main3fooyyAA1ACSgF : $@convention(thin) (@guaranteed Optional<A>) -> () {
// CHECK:    bb0([[ARG:%.*]] : @guaranteed $Optional<A>):
// CHECK:      [[X:%.*]] = alloc_box ${ var Optional<B> }, var, name "x"
// CHECK:      [[X_LIFETIME:%[^,]+]] = begin_borrow [lexical] [[X]]
// CHECK-NEXT: [[PB:%.*]] = project_box [[X_LIFETIME]]
//   Check whether the temporary holds a value.
// CHECK:      [[ARG_COPY:%.*]] = copy_value [[ARG]]
// CHECK:      switch_enum [[ARG_COPY]] : $Optional<A>, case #Optional.some!enumelt: [[IS_PRESENT:bb[0-9]+]], case #Optional.none!enumelt: [[NOT_PRESENT:bb[0-9]+]]
//
//   If so, pull the value out and check whether it's a B.
// CHECK:    [[IS_PRESENT]]([[VAL:%.*]] :
// CHECK-NEXT: [[X_VALUE:%.*]] = init_enum_data_addr [[PB]] : $*Optional<B>, #Optional.some
// CHECK-NEXT: checked_cast_br A in [[VAL]] : $A to B, [[IS_B:bb.*]], [[NOT_B:bb[0-9]+]]
//
//   If so, materialize that and inject it into x.
// CHECK:    [[IS_B]]([[T0:%.*]] : @owned $B):
// CHECK-NEXT: store [[T0]] to [init] [[X_VALUE]] : $*B
// CHECK-NEXT: inject_enum_addr [[PB]] : $*Optional<B>, #Optional.some
// CHECK-NEXT: br [[CONT:bb[0-9]+]]
//
//   If not, destroy_value the A and inject nothing into x.
// CHECK:    [[NOT_B]]([[ORIGINAL_VALUE:%.*]] : @owned $A):
// CHECK-NEXT: destroy_value [[ORIGINAL_VALUE]]
// CHECK-NEXT: inject_enum_addr [[PB]] : $*Optional<B>, #Optional.none
// CHECK-NEXT: br [[CONT]]
//
//   Finish the present path.
// CHECK:    [[CONT]]:
// CHECK-NEXT: br [[RETURN_BB:bb[0-9]+]]
//
//   Finish.
// CHECK:    [[RETURN_BB]]:
// CHECK-NEXT: end_borrow [[X_LIFETIME]]
// CHECK-NEXT: destroy_value [[X]]
// CHECK-NOT: destroy_value [[ARG]]
// CHECK-NEXT: tuple
// CHECK-NEXT: return
//
//   Finish the not-present path.
// CHECK:    [[NOT_PRESENT]]:
// CHECK-NEXT: inject_enum_addr [[PB]] {{.*}}none
// CHECK-NEXT: br [[RETURN_BB]]
func foo(_ y : A?) {
  var x = (y as? B)
}

// CHECK-LABEL: sil hidden [ossa] @$s4main3baryyAA1ACSgSgSgSgF : $@convention(thin) (@guaranteed Optional<Optional<Optional<Optional<A>>>>) -> () {
// CHECK:    bb0([[ARG:%.*]] : @guaranteed $Optional<Optional<Optional<Optional<A>>>>):
// CHECK:      [[X:%.*]] = alloc_box ${ var Optional<Optional<Optional<B>>> }, var, name "x"
// CHECK:      [[X_LIFETIME:%[^,]+]] = begin_borrow [lexical] [[X]]
// CHECK-NEXT: [[PB:%.*]] = project_box [[X_LIFETIME]]
// -- Check for some(...)
// CHECK-NEXT: [[ARG_COPY:%.*]] = copy_value [[ARG]]
// CHECK-NEXT: switch_enum [[ARG_COPY]] : ${{.*}}, case #Optional.some!enumelt: [[P:bb[0-9]+]], case #Optional.none!enumelt: [[NIL_DEPTH_1:bb[0-9]+]]
//
// CHECK: [[NIL_DEPTH_1]]:
// CHECK:   br [[FINISH_NIL_0:bb[0-9]+]]
//
//   If so, drill down another level and check for some(some(...)).
// CHECK:    [[P]]([[VALUE_OOOA:%.*]] :
// CHECK-NEXT: switch_enum [[VALUE_OOOA]] : ${{.*}}, case #Optional.some!enumelt: [[PP:bb[0-9]+]], case #Optional.none!enumelt: [[NIL_DEPTH_2:bb[0-9]+]]
//
// CHECK: [[NIL_DEPTH_2]]:
// CHECK:   br [[FINISH_NIL_0]]
//
//   If so, drill down another level and check for some(some(some(...))).
// CHECK:    [[PP]]([[VALUE_OOA:%.*]] :
// CHECK-NEXT: switch_enum [[VALUE_OOA]] : ${{.*}}, case #Optional.some!enumelt: [[PPP:bb[0-9]+]], case #Optional.none!enumelt: [[NIL_DEPTH_3:bb[0-9]+]]
//
//   If so, drill down another level and check for some(some(some(some(...)))).
// CHECK:    [[PPP]]([[VALUE_OA:%.*]] :
// CHECK-NEXT: switch_enum [[VALUE_OA]] : ${{.*}}, case #Optional.some!enumelt: [[PPPP:bb[0-9]+]], case #Optional.none!enumelt: [[NIL_DEPTH_4:bb[0-9]+]]
//
//   If so, pull out the A and check whether it's a B.
// CHECK:    [[PPPP]]([[VAL:%.*]] :
// CHECK-NEXT: checked_cast_br A in [[VAL]] : $A to B, [[IS_B:bb.*]], [[NOT_B:bb[0-9]+]]
//
//   If so, inject it back into an optional.
//   TODO: We're going to switch back out of this; we really should peephole it.
// CHECK:    [[IS_B]]([[T0:%.*]] : @owned $B):
// CHECK-NEXT: enum $Optional<B>, #Optional.some!enumelt, [[T0]]
// CHECK-NEXT: br [[SWITCH_OB2:bb[0-9]+]](
//
//   If not, inject nothing into an optional.
// CHECK:    [[NOT_B]]([[ORIGINAL_VALUE:%.*]] : @owned $A):
// CHECK-NEXT: destroy_value [[ORIGINAL_VALUE]]
// CHECK-NEXT: enum $Optional<B>, #Optional.none!enumelt
// CHECK-NEXT: br [[SWITCH_OB2]](
//
//   Switch out on the value in [[OB2]].
// CHECK:    [[SWITCH_OB2]]([[VAL:%[0-9]+]] : @owned $Optional<B>):
// CHECK-NEXT: switch_enum [[VAL]] : ${{.*}}, case #Optional.some!enumelt: [[HAVE_B:bb[0-9]+]], case #Optional.none!enumelt: [[FINISH_NIL_4:bb[0-9]+]]
//
// CHECK:    [[FINISH_NIL_4]]:
// CHECK:      br [[FINISH_NIL_0]]
//
// CHECK:    [[HAVE_B]]([[UNWRAPPED_VAL:%.*]] :
// CHECK:      [[REWRAPPED_VAL:%.*]] = enum $Optional<B>, #Optional.some!enumelt, [[UNWRAPPED_VAL]]
// CHECK:      br [[DONE_DEPTH0:bb[0-9]+]]
//
// CHECK:    [[DONE_DEPTH0]](
// CHECK-NEXT: enum $Optional<Optional<B>>, #Optional.some!enumelt,
// CHECK-NEXT: br [[DONE_DEPTH1:bb[0-9]+]]
//
//   Set X := some(OOB).
// CHECK:    [[DONE_DEPTH1]]
// CHECK-NEXT: enum $Optional<Optional<Optional<B>>>, #Optional.some!enumelt,
// CHECK:      br [[DONE_DEPTH2:bb[0-9]+]]
// CHECK:    [[DONE_DEPTH2]]
// CHECK-NEXT: end_borrow [[X_LIFETIME]]
// CHECK-NEXT: destroy_value [[X]]
// CHECK-NOT: destroy_value %0
// CHECK:      return
//
//   On various failure paths, set OOB := nil.
// CHECK: [[NIL_DEPTH_4]]:
// CHECK-NEXT: enum $Optional<B>, #Optional.none!enumelt
// CHECK-NEXT: br [[DONE_DEPTH0]]
//
// CHECK: [[NIL_DEPTH_3]]:
// CHECK-NEXT: enum $Optional<Optional<B>>, #Optional.none!enumelt
// CHECK-NEXT: br [[DONE_DEPTH1]]
//
//   On various failure paths, set X := nil.
// CHECK:    [[FINISH_NIL_0]]:
// CHECK-NEXT: inject_enum_addr [[PB]] {{.*}}none
// CHECK-NEXT: br [[DONE_DEPTH2]]
//   Done.
func bar(_ y : A????) {
  var x = (y as? B??)
}


// CHECK-LABEL: sil hidden [ossa] @$s4main3bazyyyXlSgF : $@convention(thin) (@guaranteed Optional<AnyObject>) -> () {
// CHECK:       bb0([[ARG:%.*]] : @guaranteed $Optional<AnyObject>):
// CHECK:         [[X:%.*]] = alloc_box ${ var Optional<B> }, var, name "x"
// CHECK:         [[X_LIFETIME:%[^,]+]] = begin_borrow [lexical] [[X]]
// CHECK-NEXT:    [[PB:%.*]] = project_box [[X_LIFETIME]]
// CHECK-NEXT:    [[ARG_COPY:%.*]] = copy_value [[ARG]]
// CHECK:         switch_enum [[ARG_COPY]]
// CHECK:       bb1([[VAL:%.*]] : @owned $AnyObject):
// CHECK-NEXT:    [[X_VALUE:%.*]] = init_enum_data_addr [[PB]] : $*Optional<B>, #Optional.some
// CHECK-NEXT:    checked_cast_br AnyObject in [[VAL]] : $AnyObject to B, [[IS_B:bb.*]], [[NOT_B:bb[0-9]+]]
// CHECK:       [[IS_B]]([[CASTED_VALUE:%.*]] : @owned $B):
// CHECK:         store [[CASTED_VALUE]] to [init] [[X_VALUE]]
// CHECK:       [[NOT_B]]([[ORIGINAL_VALUE:%.*]] : @owned $AnyObject):
// CHECK:         destroy_value [[ORIGINAL_VALUE]]
// CHECK: } // end sil function '$s4main3bazyyyXlSgF'
func baz(_ y : AnyObject?) {
  var x = (y as? B)
}


// <rdar://problem/17013042> T! <-> T? conversions should not produce a diamond

// CHECK-LABEL: sil hidden [ossa] @$s4main07opt_to_B8_trivialySiSgACF
// CHECK:       bb0(%0 : $Optional<Int>):
// CHECK-NEXT:  debug_value %0 : $Optional<Int>, let, name "x"
// CHECK-NEXT:  return %0 : $Optional<Int>
// CHECK-NEXT:}
func opt_to_opt_trivial(_ x: Int?) -> Int! {
  return x
}

// CHECK-LABEL: sil hidden [ossa] @$s4main07opt_to_B10_referenceyAA1CCSgAEF
// CHECK:  bb0([[ARG:%.*]] : @guaranteed $Optional<C>):
// CHECK:    debug_value [[ARG]] : $Optional<C>, let, name "x"
// CHECK:    [[RESULT:%.*]] = copy_value [[ARG]]
// CHECK-NOT:    destroy_value [[ARG]]
// CHECK:    return [[RESULT]] : $Optional<C>
// CHECK: } // end sil function '$s4main07opt_to_B10_referenceyAA1CCSgAEF'
func opt_to_opt_reference(_ x : C!) -> C? { return x }

// CHECK-LABEL: sil hidden [ossa] @$s4main07opt_to_B12_addressOnly{{[_0-9a-zA-Z]*}}F
// CHECK:       bb0(%0 : $*Optional<T>, %1 : $*Optional<T>):
// CHECK-NEXT:  debug_value %1 : $*Optional<T>, let, name "x", {{.*}} expr op_deref
// CHECK-NEXT:  copy_addr %1 to [init] %0
// CHECK-NOT:  destroy_addr %1
func opt_to_opt_addressOnly<T>(_ x : T!) -> T? { return x }

class C {}

public struct TestAddressOnlyStruct<T> {
  func f(_ a : T?) {}
  
  // CHECK-LABEL: sil hidden [ossa] @$s4main21TestAddressOnlyStructV8testCall{{[_0-9a-zA-Z]*}}F
  // CHECK: bb0(%0 : $*Optional<T>, %1 : $TestAddressOnlyStruct<T>):
  // CHECK: apply {{.*}}<T>(%0, %1)
  func testCall(_ a : T!) {
    f(a)
  }
}

// CHECK-LABEL: sil hidden [ossa] @$s4main35testContextualInitOfNonAddrOnlyTypeyySiSgF
// CHECK: bb0(%0 : $Optional<Int>):
// CHECK-NEXT: debug_value %0 : $Optional<Int>, let, name "a"
// CHECK-NEXT: [[X:%.*]] = alloc_box ${ var Optional<Int> }, var, name "x"
// CHECK-NEXT: [[PB:%.*]] = project_box [[X]]
// CHECK-NEXT: store %0 to [trivial] [[PB]] : $*Optional<Int>
// CHECK-NEXT: destroy_value [[X]] : ${ var Optional<Int> }
func testContextualInitOfNonAddrOnlyType(_ a : Int?) {
  var x: Int! = a
}
