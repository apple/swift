// RUN: %target-swift-frontend %s -emit-sil \
// RUN:   -o /dev/null \
// RUN:   -verify \
// RUN:   -sil-verify-all \
// RUN:   -enable-experimental-feature NonescapableTypes

// REQUIRES: asserts
// REQUIRES: swift_in_compiler

// Future tests for LifetimeDependenceDiagnostics.
// REQUIRES: disabled

struct BV : ~Escapable {
  let p: UnsafeRawPointer
  let i: Int

  @_unsafeNonescapableResult
  init(_ p: UnsafeRawPointer, _ i: Int) -> dependsOn(p) Self {
    self.p = p
    self.i = i
  }
}

// Nonescapable wrapper.
struct NEBV : ~Escapable {
  var bv: BV

  // Test lifetime inheritance through initialization.
  init(_ bv: consuming BV) {
    self.bv = bv
  }

  var view: BV {
    _read {
      yield bv
    }
    _modify {
      yield &bv
    }
  }

  func borrowedView() -> dependsOn(scoped self) BV {
    bv
  }
}

// Noncopyable, Nonescapable wrapper.
struct NCNEBV : ~Copyable, ~Escapable {
  var bv: BV

  // Test lifetime inheritance through initialization.
  init(_ bv: consuming BV) {
    self.bv = bv
  }

  var view: BV {
    _read {
      yield bv
    }
    _modify {
      yield &bv
    }
  }

  func borrowedView() -> dependsOn(scoped self) BV {
    bv
  }
}

func ncnebv_consume(_: consuming NCNEBV) {}
func bv_consume(_: consuming BV) {}

// =============================================================================
// Diagnostics that should fail.

// @_unsafeResultDependsOn: Test that an unsafe dependence requires
// Builtin.lifetime_dependence.
//
func bv_derive_local(bv: consuming BV) -> _consume(bv) BV {
  let bv2 = BV(bv.p, bv.i)
  return bv2.derive() // expected-error {{lifetime-dependent value escapes its scope}}
  // expected-note @-2 {{it depends on the lifetime of variable 'bv2'}}
  // expected-note @-2 {{this use causes the lifetime-dependent value to escape}}
}

// =============================================================================
// Debug locations

// FIXME: Our debug locations on closure captures are incorrect.
// FIXME: We return two errors: one for the caller, and one for the closure.
func bvcons_capture_escape(bv: consuming BV) -> ()->Int {
  return { bv.c }  // expected-error{{lifetime-dependent variable 'bv' escapes its scope}}
  // expected-note @-1 {{this use causes the lifetime-dependent value to escape}}
}

// FIXME: Our debug locations on closure captures are incorrect.
// FIXME: We return two errors: one for the caller, and one for the closure.
func bvcons_capture_escapelet(bv: consuming BV) -> ()->Int {
  let closure = { bv.c } // expected-error{{lifetime-dependent variable 'bv' escapes its scope}}
  // expected-note @-1 {{this use causes the lifetime-dependent value to escape}}
  return closure
}

// =============================================================================
// Can't write lit tests because move-only diagnostics are broken.

func nebv_consume(_: consuming NEBV) {}

// FIXME: consume operator diagnostics does not report a line number for "used here"
func nebv_consume_borrow(nebv: NEBV) {
  let bv = nebv.borrowedView() // expected-note {{conflicting access is here}}
  nebv_consume(nebv) // expected-error {{overlapping accesses to 'nebv', but deinitialization requires exclusive access}}
  bv_consume(bv)
}

// FIXME: consume operator diagnostics does not report a line number for "used here"
func nebv_consume_borrow(nebv: consuming NEBV) { // expected-error {{'nebv' used after consume}}
  let bv = nebv.borrowedView() // expected-note {{conflicting access is here}}
  _ = consume nebv // expected-error {{overlapping accesses to 'nebv', but modification requires exclusive access}}
  // expected-note @-1{{consumed here}}
  bv_consume(bv)
}

// FIXME: consume operator diagnostics does not report a line number for "used here"
func ncnebv_consume_borrow(ncnebv: NCNEBV) {
  let bv = ncnebv.borrowedView() // expected-note {{conflicting access is here}}
  ncnebv_consume(ncnebv) // expected-error {{overlapping accesses to 'nebv', but deinitialization requires exclusive access}}
  bv_consume(bv)
}
