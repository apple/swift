// RUN: %target-swift-frontend -enable-experimental-feature InitAccessors -enable-copy-propagation=requested-passes-only -emit-sil -primary-file %s -o /dev/null -verify

// REQUIRES: asserts

struct Test1 {
  var x: Int // expected-note {{variable defined here}}
  var y: Int // expected-note {{variable defined here}}
  var full: (Int, Int)

  var test1: (Int, Int) {
    init(initialValue) initializes(y, full) accesses(x) {
      self.full = (self.x, self.y) // expected-error {{variable 'y' used before being initialized}}
    }

    get { full }
    set { self.full = newValue }
  }

  var pointY: Int {
    init(initialValue) initializes(y) {
      self.y = initialValue // Ok
    }

    get { y }
    set {}
  }

  var errorPoint1: (Int, Int) {
    init(initialValue) initializes(x, y) {
      // expected-error@-1 {{property 'x' not initialized by init accessor}}
      // expected-error@-2 {{property 'y' not initialized by init accessor}}
    }

    get { (x, y) }
    set { }
  }

  var errorPoint2: (Int, Int) {
    init(initialValue) initializes(x, y) {
      // expected-error@-1 {{property 'y' not initialized by init accessor}}
      self.x = initialValue.0
    }

    get { (x, y) }
    set { }
  }

  var errorPoint3: (Int, Int) {
    init(initialValue) initializes(x, y) {
      self.y = initialValue.1
      print(y) // Ok
      print(x) // expected-error {{variable 'x' used before being initialized}}
    }

    get { (x, y) }
    set { }
  }

  init(x: Int, y: Int) {
    self.x = x
    self.y = y
    self.full = (x, y)
  }
}

struct TestPartial {
  var x: Int
  var y: Int

  var point: (Int, Int) {
    init(initialValue) initializes(x, y) {
      self.x = initialValue.0
      self.y = initialValue.1
    }

    get { (x, y) }
    set { }
  }

  init(x: Int, y: Int) {
    self.x = x
    self.point = (x, y) // Ok (x is going to get `destroy_addr`)
  }

  init(_ x: Int, _ y: Int) {
    self.x = x
    self.y = y
    self.point = (x, y) // Ok (calls a setter)
  }
}

struct TestDoubleInit1 {
  let x: Int // expected-note {{change 'let' to 'var' to make it mutable}}

  var invalidPointX: Int {
    init(initialValue) initializes(x) {
      self.x = initialValue
      self.x = 42 // expected-error {{immutable value 'x' may only be initialized once}}
    }

    get { x }
    set { }
  }
}

struct TestDoubleInit2 {
  let x: Int // expected-note {{change 'let' to 'var' to make it mutable}}

  var pointX: Int {
    init(initialValue) initializes(x) {
      self.x = initialValue
    }

    get { x }
    set { }
  }

  init(x: Int) {
    self.pointX = x
    self.x = 0 // expected-error {{immutable value 'self.x' may only be initialized once}}
  }
}

struct TestAccessBeforeInit {
  var _x: Int
  var x: Int {
    init(initialValue) initializes(_x) accesses(y) {
      _x = initialValue
    }

    get { _x }
    set {}
  }

  var y: Int

  init(x: Int, y: Int) {
    self.x = x // expected-error {{variable 'self.y' used before being initialized}}
    self.y = y
  }
}
