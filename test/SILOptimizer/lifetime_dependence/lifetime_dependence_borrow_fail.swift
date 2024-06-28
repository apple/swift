// RUN: %target-swift-frontend %s -emit-sil \
// RUN:   -o /dev/null \
// RUN:   -verify \
// RUN:   -sil-verify-all \
// RUN:   -module-name test \
// RUN:   -enable-experimental-feature NonescapableTypes

// REQUIRES: asserts
// REQUIRES: swift_in_compiler

struct BV : ~Escapable {
  let p: UnsafeRawPointer
  let i: Int

  @_unsafeNonescapableResult
  init(_ p: UnsafeRawPointer, _ i: Int) -> dependsOn(p) Self {
    self.p = p
    self.i = i
  }
}

struct NC : ~Copyable {
  let p: UnsafeRawPointer
  let i: Int

  init(_ p: UnsafeRawPointer, _ i: Int) {
    self.p = p
    self.i = i
  }
  borrowing func getBV() -> dependsOn(self) BV {
    BV(p, i)
  }
}

struct NE : ~Escapable {
  let p: UnsafeRawPointer
  let i: Int

  @_unsafeNonescapableResult
  init(_ p: UnsafeRawPointer, _ i: Int) -> dependsOn(p) Self {
    self.p = p
    self.i = i
  }
  borrowing func getBV() -> dependsOn(scoped self) BV {
    BV(p, i)
  }
}

func bv_get_consume(container: consuming NE) -> BV {
  return container.getBV() // expected-error {{lifetime-dependent value escapes its scope}}
    // expected-note @-1{{it depends on this scoped access to variable 'container'}}
    // expected-note @-2{{this use causes the lifetime-dependent value to escape}}
}

struct Wrapper : ~Escapable {
  let bv: BV
}

func bv_incorrect_annotation1(_ bv1: borrowing BV, _ bv2: borrowing BV) -> dependsOn(bv2) BV { // expected-error {{lifetime-dependent variable 'bv1' escapes its scope}}
  return copy bv1                                                                              // expected-note @-1{{it depends on the lifetime of argument 'bv1'}}
}                                                                                              // expected-note @-1{{this use causes the lifetime-dependent value to escape}}

func bv_incorrect_annotation2(_ w1: borrowing Wrapper, _ w2: borrowing Wrapper) -> dependsOn(w2) BV { // expected-error {{lifetime-dependent variable 'w1' escapes its scope}}
  return w1.bv                                                                                        // expected-note @-1{{it depends on the lifetime of argument 'w1'}}
}                                                                                                     // expected-note @-1{{this use causes the lifetime-dependent value to escape}}

