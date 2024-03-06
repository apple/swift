// RUN: %target-swift-frontend %s -emit-sil \
// RUN:   -sil-verify-all \
// RUN:   -module-name test \
// RUN:   -disable-experimental-parser-round-trip \
// RUN:   -enable-experimental-feature NonescapableTypes \
// RUN:   -Xllvm -enable-lifetime-dependence-insertion \
// RUN:   2>&1 | %FileCheck %s

// REQUIRES: asserts
// REQUIRES: swift_in_compiler

@_nonescapable
struct BV {
  let p: UnsafeRawPointer
  let i: Int

  @_unsafeNonescapableResult
  init(_ p: UnsafeRawPointer, _ i: Int) {
    self.p = p
    self.i = i
  }
}

struct NC : ~Copyable {
  let p: UnsafeRawPointer
  let i: Int

  // Requires a borrow.
  borrowing func getBV() -> _borrow(self) BV {
    BV(p, i)
  }
}

@_silgen_name("use")
func use(_ o : borrowing BV)

// CHECK-LABEL: sil hidden @$s4test13bv_borrow_var1p1iySV_SitF : $@convention(thin) (UnsafeRawPointer, Int) -> () {
// CHECK: [[A:%.*]] = begin_access [read] [static] %{{.*}} : $*NC
// CHECK: [[L:%.*]] = load [[A]] : $*NC
// CHECK: [[R:%.*]] = apply %{{.*}}([[L]]) : $@convention(method) (@guaranteed NC) -> _scope(0) @owned BV
// CHECK: [[M:%.*]] = mark_dependence [nonescaping] [[R]] : $BV on [[A]] : $*NC
// CHECK: %{{.*}} = apply %{{.*}}([[M]]) : $@convention(thin) (@guaranteed BV) -> ()
// CHECK: end_access [[A]] : $*NC
// CHECK-LABEL: } // end sil function '$s4test13bv_borrow_var1p1iySV_SitF'
func bv_borrow_var(p: UnsafeRawPointer, i: Int) {
  var nc = NC(p: p, i: i)
  let bv = nc.getBV()
  use(bv)
}
