// RUN: %target-swift-frontend -emit-sil -disable-objc-attr-requires-foundation-module -verify %s

// High-level tests that DI rejects certain invalid idioms for early
// return from initializers.

// <rdar://problem/19267795> failable initializers that call noreturn function produces bogus diagnostics
class FailableInitThatFailsReallyHard {
  init?() {   // no diagnostics generated.
    fatalError("bad")
  }
}

// Failable initializers must produce correct diagnostics
struct A {
  var x: Int  // expected-note {{'self.x' not initialized}}
  init?(i: Int) {
    if i > 0 {
      self.x = i
    }
  } // expected-error {{return from initializer without initializing all stored properties}}
}

// Delegating, failable initializers that doesn't initialize along all paths must produce correct diagnostics.
extension Int {
  init?(i: Int) {
    if i > 0 {
      self.init(i)
    }
  } // expected-error {{'self.init' isn't called on all paths before returning from initializer}}
}

class BaseClass {}
final class DerivedClass : BaseClass {
  init(x : ()) {
    fatalError("bad")  // no diagnostics.
  }
}

func something(_ x: Int) {}

func something(_ x: inout Int) {}

func something(_ x: AnyObject) {}

func something(_ x: Any.Type) {}

// <rdar://problem/22946400> DI needs to diagnose self usages in error block
//
// FIXME: bad QoI
class ErrantBaseClass {
  init() throws {}
}

class ErrantClass : ErrantBaseClass {
  let x: Int
  var y: Int

  override init() throws {
    x = 10
    y = 10
    try super.init()
  }

  init(invalidEscapeDesignated: ()) {
    x = 10
    y = 10
    do {
      try super.init()
    } catch {}
  } // expected-error {{'self' used inside 'catch' block reachable from super.init call}}

  init(invalidEscapeDesignated2: ()) throws {
    x = 10
    y = 10
    do {
      try super.init()
    } catch {
      try super.init() // expected-error {{'self' used inside 'catch' block reachable from super.init call}}
    }
  }

  init(invalidEscapeDesignated3: ()) {
    x = 10
    y = 10
    do {
      try super.init()
    } catch {
      print(self.x) // expected-error {{'self' used inside 'catch' block reachable from super.init call}}
      self.y = 20 // expected-error {{'self' used inside 'catch' block reachable from super.init call}}
    }
  } // expected-error {{'self' used inside 'catch' block reachable from super.init call}}

  init(invalidEscapeDesignated4: ()) throws {
    x = 10
    y = 10
    do {
      try super.init()
    } catch let e {
      print(self.x) // expected-error {{'self' used inside 'catch' block reachable from super.init call}}
      throw e
    }
  }

  convenience init(invalidEscapeConvenience: ()) {
    do {
      try self.init()
    } catch {}
  } // expected-error {{'self.init' isn't called on all paths}}

  convenience init(okEscapeConvenience2: ()) throws {
    do {
      try self.init()
    } catch {
      try self.init()
    }
  }

  convenience init(invalidEscapeConvenience3: ()) throws {
    do {
      try self.init()
    } catch let e {
      print(self) // expected-error {{'self' used before 'self.init'}}
      throw e
    }
  }

  init(noEscapeDesignated: ()) throws {
    x = 10
    y = 10
    do {
      try super.init()
    } catch let e {
      throw e  // ok
    }
  }

  convenience init(noEscapeConvenience: ()) throws {
    do {
      try self.init()
    } catch let e {
      throw e  // ok
    }
  }

  convenience init(invalidAccess: ()) throws {
    do {
      try self.init()
    } catch let e {
      something(x) // expected-error {{'self' used before 'self.init'}}
      something(self.x) // expected-error {{'self' used before 'self.init'}}

      something(y) // expected-error {{'self' used before 'self.init'}}
      something(self.y) // expected-error {{'self' used before 'self.init'}}

      something(&y) // expected-error {{'self' used before 'self.init'}}
      something(&self.y) // expected-error {{'self' used before 'self.init'}}

      something(self) // expected-error {{'self' used before 'self.init'}}

      something(type(of: self))

      throw e
    }
  }
}
