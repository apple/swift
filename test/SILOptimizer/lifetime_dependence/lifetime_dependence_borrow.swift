// RUN: %target-swift-frontend %s -emit-sil \
// RUN:   -o /dev/null \
// RUN:   -verify \
// RUN:   -sil-verify-all \
// RUN:   -module-name test \
// RUN:   -enable-experimental-feature NonescapableTypes

// REQUIRES: asserts
// REQUIRES: swift_in_compiler

// Some container-ish thing.
struct CN: ~Copyable {
  let p: UnsafeRawPointer
  let i: Int
}

// Some Bufferview-ish thing.
struct BV : ~Escapable {
  let p: UnsafeRawPointer
  let i: Int

  public var isEmpty: Bool { i == 0 }

  init(_ p: UnsafeRawPointer, _ i: Int) -> dependsOn(p) Self {
    self.p = p
    self.i = i
  }

  init(_ cn: borrowing CN) {
    self.p = cn.p
    self.i = cn.i
  }
}

// Some MutableBufferview-ish thing.
struct MBV : ~Escapable, ~Copyable {
  let p: UnsafeRawPointer
  let i: Int
  
  init(_ p: UnsafeRawPointer, _ i: Int) -> dependsOn(p) Self {
    self.p = p
    self.i = i
  }

  // Requires a borrow.
  borrowing func getBV() -> dependsOn(self) BV {
    BV(p, i)
  }
}

// Nonescapable wrapper.
struct NEBV : ~Escapable {
  var bv: BV

  init(_ bv: consuming BV) {
    self.bv = bv
  }
}

// Propagate a borrow.
func bv_get_borrow(container: borrowing MBV) -> dependsOn(container) BV {
  container.getBV()
}

// Copy a borrow.
func bv_get_copy(container: borrowing MBV) -> dependsOn(container) BV {
  return container.getBV()
}

// Recognize nested accesses as part of the same dependence scope.
func bv_get_mutate(container: inout MBV) -> dependsOn(container) BV {
  container.getBV()
}

// Create and decompose a nonescapable aggregate.
func ne_wrap_and_extract_member(cn: borrowing CN) -> dependsOn(scoped cn) BV {
  let bv = BV(cn)
  let ne = NEBV(bv)
  return ne.bv
}
