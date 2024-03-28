// RUN: %target-swift-frontend %s -emit-sil \
// RUN:   -o /dev/null \
// RUN:   -verify \
// RUN:   -sil-verify-all \
// RUN:   -module-name test \
// RUN:   -enable-experimental-feature NoncopyableGenerics \
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

  @_unsafeNonescapableResult
  init(_ p: UnsafeRawPointer, _ i: Int) {
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
  
  @_unsafeNonescapableResult
  init(_ p: UnsafeRawPointer, _ i: Int) {
    self.p = p
    self.i = i
  }

  // Requires a borrow.
  borrowing func getBV() -> _borrow(self) BV {
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
func bv_get_borrow(container: borrowing MBV) -> _borrow(container) BV {
  container.getBV()
}

// Copy a borrow.
func bv_get_copy(container: borrowing MBV) -> _copy(container) BV {
  return container.getBV()
}

// Recognize nested accesses as part of the same dependence scope.
func bv_get_mutate(container: inout MBV) -> _mutate(container) BV {
  container.getBV()
}

// Create and decompose a nonescapable aggregate.
func ne_wrap_and_extract_member(cn: borrowing CN) -> _borrow(cn) BV {
  let bv = BV(cn)
  let ne = NEBV(bv)
  return ne.bv
}
