// RUN: %target-swift-frontend %s -emit-sil \
// RUN:   -sil-verify-all \
// RUN:   -module-name test \
// RUN:   -enable-experimental-feature NoncopyableGenerics \
// RUN:   -enable-experimental-feature NonescapableTypes \
// RUN:   2>&1 | %FileCheck %s

// REQUIRES: asserts
// REQUIRES: swift_in_compiler

struct BV : ~Escapable {
  let p: UnsafeRawPointer
  let c: Int
  @_unsafeNonescapableResult
  init(_ p: UnsafeRawPointer, _ c: Int) {
    self.p = p
    self.c = c
  }
}

func bv_copy(_ bv: borrowing BV) -> _copy(bv) BV {
  copy bv
}

struct NCInt: ~Copyable {
  var value: Int
}

func takeClosure(_: () -> ()) {}

// No mark_dependence is needed for a inherited scope.
//
// CHECK-LABEL: sil hidden @$s4test14bv_borrow_copyyAA2BVVADYlsF : $@convention(thin) (@guaranteed BV) -> _scope(1) @owned BV {
// CHECK:      bb0(%0 : @noImplicitCopy $BV):
// CHECK:        apply %{{.*}}(%0) : $@convention(thin) (@guaranteed BV) -> _inherit(1) @owned BV // user: %4
// CHECK-NEXT:   return %3 : $BV
// CHECK-LABEL: } // end sil function '$s4test14bv_borrow_copyyAA2BVVADYlsF'
func bv_borrow_copy(_ bv: borrowing BV) -> _borrow(bv) BV {
  bv_copy(bv) 
}

// The mark_dependence for the borrow scope should be marked
// [nonescaping] after diagnostics.
//
// CHECK-LABEL: sil hidden @$s4test010bv_borrow_C00B0AA2BVVAEYls_tF : $@convention(thin) (@guaranteed BV) -> _scope(1) @owned BV {
// CHECK:       bb0(%0 : @noImplicitCopy $BV):
// CHECK:         [[R:%.*]] = apply %{{.*}}(%0) : $@convention(thin) (@guaranteed BV) -> _scope(1) @owned BV
// CHECK:         %{{.*}} = mark_dependence [nonescaping] [[R]] : $BV on %0 : $BV
// CHECK-NEXT:    return %{{.*}} : $BV
// CHECK-LABEL: } // end sil function '$s4test010bv_borrow_C00B0AA2BVVAEYls_tF'
func bv_borrow_borrow(bv: borrowing BV) -> _borrow(bv) BV {
  bv_borrow_copy(bv)
}

// This already has a mark_dependence [nonescaping] before diagnostics. If it triggers diagnostics again, it will fail
// because lifetime dependence does not expect a dependence directly on an 'inout' address without any 'begin_access'
// marker.
func ncint_capture(ncInt: inout NCInt) {
  takeClosure { _ = ncInt.value }
}
