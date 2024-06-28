// RUN: %target-swift-frontend %s -emit-sil \
// RUN:   -sil-verify-all \
// RUN:   -module-name test \
// RUN:   -enable-experimental-feature NonescapableTypes \
// RUN:   2>&1 | %FileCheck %s

// REQUIRES: asserts
// REQUIRES: swift_in_compiler

// Test LifetimeDependenceScopeFixup.

struct BV : ~Escapable {
  let p: UnsafeRawPointer
  let c: Int

  public var isEmpty: Bool { c == 0 }

  init(_ p: UnsafeRawPointer, _ c: Int) -> dependsOn(p) Self {
    self.p = p
    self.c = c
  }
}

struct NC : ~Copyable {
  let p: UnsafeRawPointer
  let c: Int

  // Requires a borrow.
  borrowing func getBV() -> dependsOn(self) BV {
    BV(p, c)
  }
}

// Rewrite the mark_dependence to depend on the incoming argument rather than the nested access.
//
// CHECK-LABEL: sil hidden @$s4test13bv_get_mutate9containerAA2BVVAA2NCVzYls_tF : $@convention(thin) (@inout NC) -> _scope(0) @owned BV {
// CHECK: bb0(%0 : $*NC):
// CHECK:   [[A:%.*]] = begin_access [read] [static] %0 : $*NC
// CHECK:   [[L:%.*]] = load [[A]] : $*NC
// CHECK:   [[R:%.*]] = apply %{{.*}}([[L]]) : $@convention(method) (@guaranteed NC) -> _scope(0) @owned BV
// CHECK:   [[M:%.*]] = mark_dependence [nonescaping] [[R]] : $BV on %0 : $*NC
// CHECK-LABEL: } // end sil function '$s4test13bv_get_mutate9containerAA2BVVAA2NCVzYls_tF'
func bv_get_mutate(container: inout NC) -> dependsOn(container) BV {
  container.getBV()
}
