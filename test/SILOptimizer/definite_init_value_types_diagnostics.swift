// RUN: %target-swift-frontend -emit-sil %s -o /dev/null -verify

struct EmptyStruct {}

struct ValueStruct {
  var ivar: EmptyStruct // expected-note {{'self.ivar' not initialized}}

  init() { ivar = EmptyStruct() }

  init(a: Int) {
    _ = ivar // expected-error {{'self' used before 'self.init' call or assignment to 'self'}}
    self.init()
  }

  init(c: Bool) {
    if c {
      return
    }

    self.init()
  } // expected-error {{'self.init' isn't called on all paths before returning from initializer}}

  init(d: Bool) {
    if d {
      return // expected-error {{return from initializer without initializing all stored properties}}
    }

    self = ValueStruct()
  }
}

enum ValueEnum {
  case Dinosaur, Train, Truck

  init() { self = .Train }

  init(a: Int) {
    _ = self // expected-error {{'self' used before 'self.init' call or assignment to 'self'}}
    self.init()
  }

  init(c: Bool) {
    if c {
      return
    }

    self.init()
  } // expected-error {{'self.init' isn't called on all paths before returning from initializer}}

  init(d: Bool) {
    if d {
      return
    }

    self = ValueEnum()
  } // expected-error {{'self.init' isn't called on all paths before returning from initializer}}
}

struct AddressStruct {
  var ivar: EmptyStruct // expected-note {{'self.ivar' not initialized}}
  var any: Any?

  init() { ivar = EmptyStruct(); any = nil }

  init(c: Bool) {
    if c {
      return
    }

    self.init()
  } // expected-error {{'self.init' isn't called on all paths before returning from initializer}}

  init(d: Bool) {
    if d {
      return
    }

    self = AddressStruct()
  } // expected-error {{return from initializer without initializing all stored properties}}
}
