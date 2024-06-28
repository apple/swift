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

  init(_ p: UnsafeRawPointer, _ i: Int) -> dependsOn(p) Self {
    self.p = p
    self.i = i
  }

  @_unsafeNonescapableResult
  init(independent p: UnsafeRawPointer, _ i: Int) {
    self.p = p
    self.i = i
  }

  consuming func derive() -> dependsOn(self) BV {
    // Technically, this "new" view does not depend on the 'view' argument.
    // This unsafely creates a new view with no dependence.
    return BV(independent: self.p, self.i)
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

// Test lifetime inheritance through chained consumes.
func bv_derive(bv: consuming BV) -> dependsOn(bv) BV {
  bv.derive()
}

// Test lifetime inheritance through stored properties.
func ne_extract_member(nebv: consuming NEBV) -> dependsOn(nebv) BV {
  return nebv.bv
}

func ne_yield_member(nebv: consuming NEBV) -> dependsOn(nebv) BV {
  return nebv.view
}

func bv_consume(_ x: consuming BV) {}

// It is ok to consume the aggregate before the property.
// The property's lifetime must exceed the aggregate's.
func nebv_consume_member(nebv: consuming NEBV) {
  let bv = nebv.bv
  _ = consume nebv
  bv_consume(bv)
}

func nebv_consume_after_yield(nebv: consuming NEBV) {
  let view = nebv.view
  _ = consume nebv
  bv_consume(view)
}
