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
  let c: Int

  init(_ p: UnsafeRawPointer, _ c: Int) -> dependsOn(p) Self {
    self.p = p
    self.c = c
  }
}

struct NC : ~Copyable {
  let p: UnsafeRawPointer
  let c: Int

  func withBV<ResultType>(_ body: (BV) throws -> ResultType) rethrows
    -> ResultType {
    try body(BV(p, c))
  }
}

@_nonescapable
struct NE {
  var bv: BV

  init(_ bv: consuming BV) -> dependsOn(bv) Self {
    self.bv = bv
    return self
  }
}

func bv_assign_inout(bv: BV, other: inout BV) { // expected-error{{lifetime-dependent variable 'bv' escapes its scope}}
  // expected-note @-1 {{it depends on the lifetime of argument 'bv'}}
  other = bv // expected-note {{this use causes the lifetime-dependent value to escape}}
}

func bvmut_assign_inout(bv: inout BV, other: inout BV) {
  other = bv // expected-error{{lifetime-dependent value escapes its scope}}
  // expected-note @-2 {{it depends on the lifetime of argument 'bv'}}
  // expected-note @-2 {{this use causes the lifetime-dependent value to escape}}
}

func bvcons_assign_inout(bv: consuming BV, other: inout BV) {
  other = bv // expected-error{{lifetime-dependent value escapes its scope}}
  // expected-note @-2 {{it depends on the lifetime of argument 'bv'}}
  // expected-note @-2 {{this use causes the lifetime-dependent value to escape}}
}

func bv_assign_field(bv: BV, other: inout NE) { // expected-error{{lifetime-dependent variable 'bv' escapes its scope}}
  // expected-note @-1 {{it depends on the lifetime of argument 'bv'}}
  other.bv = bv // expected-note {{this use causes the lifetime-dependent value to escape}}
}

func bvmut_assign_field(bv: inout BV, other: inout NE) {
  other.bv = bv // expected-error{{lifetime-dependent value escapes its scope}}
  // expected-note @-2 {{it depends on the lifetime of argument 'bv'}}
  // expected-note @-2 {{this use causes the lifetime-dependent value to escape}}
}

func bvcons_assign_field(bv: consuming BV, other: inout NE) {
  other.bv = bv // expected-error{{lifetime-dependent value escapes its scope}}
  // expected-note @-2 {{it depends on the lifetime of argument 'bv'}}
  // expected-note @-2 {{this use causes the lifetime-dependent value to escape}}
}

func bv_capture_escape(bv: BV) -> ()->Int { // expected-error{{lifetime-dependent variable 'bv' escapes its scope}}
  // expected-note @-1 {{it depends on the lifetime of argument 'bv'}}
  return { bv.c } // expected-note {{this use causes the lifetime-dependent value to escape}}
}

// FIXME: Our debug locations on closure captures are incorrect.
// FIXME: We return two errors: one for the caller, and one for the closure.
func bvcons_capture_escape(bv: consuming BV) -> ()->Int { // expected-error *{{lifetime-dependent variable 'bv' escapes its scope}}
  // expected-note @-1 *{{it depends on the lifetime of argument 'bv'}}
  // expected-note @-2 *{{this use causes the lifetime-dependent value to escape}}
  return { bv.c }
}

func bv_capture_escapelet(bv: BV) -> ()->Int { // expected-error{{lifetime-dependent variable 'bv' escapes its scope}}
  // expected-note @-1 {{it depends on the lifetime of argument 'bv'}}
  let closure = { bv.c } // expected-note {{this use causes the lifetime-dependent value to escape}}
  return closure
}

// FIXME: Our debug locations on closure captures are incorrect.
// FIXME: We return two errors: one for the caller, and one for the closure.
func bvcons_capture_escapelet(bv: consuming BV) -> ()->Int { // expected-error *{{lifetime-dependent variable 'bv' escapes its scope}}
  // expected-note @-1 *{{it depends on the lifetime of argument 'bv'}}
  // expected-note @-2 *{{this use causes the lifetime-dependent value to escape}}
  let closure = { bv.c }
  return closure
}
