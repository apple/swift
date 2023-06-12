// RUN: %target-swift-emit-sil -sil-verify-all -module-name moveonly_lifetime -o /dev/null -Xllvm -sil-print-canonical-module -Onone -verify -enable-experimental-feature MoveOnlyClasses %s 2>&1 | %FileCheck %s

struct C : ~Copyable {
    deinit {}
}

@_silgen_name("getC")
func getC() -> C

@_silgen_name("takeC")
func takeC(_ c: __owned C)

@_silgen_name("borrowC")
func borrowC(_ c: borrowing C)

@_silgen_name("something")
func something()

// Verify that a move-only value's lifetime extends to the end of the
// non-consuming scope where it appears but not further (which would require a
// copy).
//
// NOTE: We do not properly maximize lifetimes yet in the address checker. So
// the lifetimes are shortened in the sides of the diamonds inappropriately.
//
// CHECK-LABEL: sil hidden [ossa] @test_diamond__consume_r__use_l : $@convention(thin) (Bool) -> () {
// CHECK:         [[STACK:%.*]] = alloc_stack
// CHECK:         cond_br {{%[^,]+}}, [[LEFT:bb[0-9]+]], [[RIGHT:bb[0-9]+]]
//
// CHECK:       [[RIGHT]]:
// CHECK:         [[INSTANCE:%.*]] = load [take] [[STACK]]
// CHECK:         [[TAKE_C:%[^,]+]] = function_ref @takeC
// CHECK:         apply [[TAKE_C]]([[INSTANCE]])
//
// CHECK:       [[LEFT]]:
// CHECK:         [[INSTANCE:%.*]] = load_borrow [[STACK]]
// CHECK:         [[BORROW_C:%[^,]+]] = function_ref @borrowC
// CHECK:         apply [[BORROW_C]]([[INSTANCE]])
//
// TODO: Once we maximize lifetimes this should be below something.
// CHECK:         destroy_addr [[STACK]]
//
// CHECK:         [[SOMETHING:%[^,]+]] = function_ref @something
// CHECK:         apply [[SOMETHING]]
// CHECK-LABEL: } // end sil function 'test_diamond__consume_r__use_l'
@_silgen_name("test_diamond__consume_r__use_l")
func test_diamond(_ condition: Bool) {
  let c = getC()
  if condition {
    borrowC(c)
    something()
  } else {
    takeC(c)
  }
}
