// RUN: %target-typecheck-verify-swift -enable-experimental-feature InitAccessors

// REQUIRES: asserts

func test_empty_init_accessor() {
  struct Test {
    var empty: Int {
      init { // Ok
      }

      get { 42 }
      set { }
    }

    var noArgs: Int {
      init(initialValue) { // Ok
      }

      get { 42 }
      set { }
    }
  }
}

func test_invalid_init_accessor_use() {
  var other: String = "" // expected-warning {{}}
  var x: Int {
    init(initialValue) initializes(other) {}
    // expected-error@-1 {{init accessors could only be associated with properties}}

    get { 42 }
  }

  struct X {
    subscript(x: Int) -> Bool {
      init(initialValue) {} // expected-error {{init accessors could only be associated with properties}}

      get { false }
    }
  }
}

func test_use_of_initializes_accesses_on_non_inits() {
  struct Test1 {
    var x: Int
    var y: String

    var _x: Int {
      init(initialValue) initializes(x) accesses(y) { // Ok
      }

      get { x }
    }

    var _y: String {
      get { y }
      set(initialValue) initializes(y) {}
      // expected-error@-1 {{initalizes(...) attribute could only be used with init accessors}}
    }

    var _q: String {
      get { y }
      set(initialValue) accesses(x) {}
      // expected-error@-1 {{accesses(...) attribute could only be used with init accessors}}
    }
  }
}

func test_invalid_refs_in_init_attrs() {
  struct Test {
    var c: Int { get { 42 } }
    var x: Int {
      init(initialValue) initializes(a) accesses(b, c) {}
      // expected-error@-1 {{find type 'a' in scope}}
      // expected-error@-2 {{find type 'b' in scope}}
      // expected-error@-3 {{init accessor cannot refer to property 'c'; init accessors can refer only to stored properties}}
    }

    var y: String {
      init(initialValue) initializes(test) {}
      // expected-error@-1 {{ambiguous reference to member 'test'}}
    }

    func test(_: Int) {} // expected-note {{'test' declared here}}
    func test(_: String) -> Int { 42 } // expected-note {{'test' declared here}}
  }
}

func test_assignment_to_let_properties() {
  struct Test {
    let x: Int
    let y: Int // expected-note {{change 'let' to 'var' to make it mutable}}

    var pointX: Int {
      init(initialValue) initializes(x) accesses(y) {
        self.x = initialValue // Ok
        self.y = 42 // expected-error {{cannot assign to property: 'y' is a 'let' constant}}
      }
    }

    var point: (Int, Int) {
      init(initialValue) initializes(x, y) {
        self.x = initialValue.0 // Ok
        self.y = initialValue.1 // Ok
      }
    }
  }
}

func test_duplicate_and_computed_lazy_properties() {
  struct Test1 {
    var _a: Int
    var _b: Int

    var a: Int {
      init(initialValue) initializes(_b, _a) accesses(_a) {
        // expected-error@-1 {{property '_a' cannot be both initialized and accessed}}
      }
    }
  }

  struct Test2 {
    var _a: Int

    var a: Int {
      init(initialValue) initializes(a, c) accesses(_a, b) {}
      // expected-error@-1 {{init accessor cannot refer to property 'a'; init accessors can refer only to stored properties}}
      // expected-error@-2 {{init accessor cannot refer to property 'b'; init accessors can refer only to stored properties}}
      // expected-error@-3 {{init accessor cannot refer to property 'c'; init accessors can refer only to stored properties}}
    }

    var b: Int {
      get { 42 }
    }

    lazy var c: Int = 42
  }
}
