// RUN: %target-swift-frontend %s -emit-sil \
// RUN:   -o /dev/null \
// RUN:   -verify \
// RUN:   -sil-verify-all \
// RUN:   -module-name test \
// RUN:   -enable-experimental-feature NoncopyableGenerics \
// RUN:   -enable-experimental-feature NonescapableTypes

// REQUIRES: asserts
// REQUIRES: swift_in_compiler

struct BV : ~Escapable {
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

  @_unsafeNonescapableResult
  init(_ p: UnsafeRawPointer, _ i: Int) {
    self.p = p
    self.i = i
  }
  borrowing func getBV() -> _borrow(self) BV {
    BV(p, i)
  }
}

struct NE : ~Escapable {
  let p: UnsafeRawPointer
  let i: Int

  @_unsafeNonescapableResult
  init(_ p: UnsafeRawPointer, _ i: Int) {
    self.p = p
    self.i = i
  }
  borrowing func getBV() -> _borrow(self) BV {
    BV(p, i)
  }
}

func bv_get_consume(container: consuming NE) -> BV {
  return container.getBV() // expected-error {{lifetime-dependent value escapes its scope}}
    // expected-note @-1{{it depends on this scoped access to variable 'container'}}
    // expected-note @-2{{this use causes the lifetime-dependent value to escape}}
}

