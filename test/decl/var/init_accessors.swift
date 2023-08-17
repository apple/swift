// RUN: %target-typecheck-verify-swift

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
    @storageRestrictions(initializes: other)
    init(initialValue) {}
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
      @storageRestrictions(initializes: x, accesses: y)
      init(initialValue) { // Ok
      }

      get { x }
      set { }
    }

    var _y: String {
      get { y }

      @storageRestrictions(initializes: y)
      // expected-error@-1 {{@storageRestrictions attribute could only be used with init accessors}}
      set(initialValue) {}
    }

    var _q: String {
      get { y }
      @storageRestrictions(accesses: x)
      // expected-error@-1 {{@storageRestrictions attribute could only be used with init accessors}}
      set(initialValue) {}
    }

    init(x: Int, y: String) {
      self.y = y
      self._x = x
    }
  }
}

func test_invalid_refs_in_init_attrs() {
  struct Test {
    var c: Int { get { 42 } }
    var x: Int {
      @storageRestrictions(initializes: a, accesses: b, c)
      // expected-error@-1 {{find type 'a' in scope}}
      // expected-error@-2 {{find type 'b' in scope}}
      // expected-error@-3 {{init accessor cannot refer to property 'c'; init accessors can refer only to stored properties}}
      init(initialValue) {}

      get { 0 }
    }

    var y: String {
      @storageRestrictions(initializes: test)
      // expected-error@-1 {{ambiguous reference to member 'test'}}
      init(initialValue) {}

      get { "" }
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
      @storageRestrictions(initializes: x, accesses: y)
      init(initialValue) {
        self.x = initialValue // Ok
        self.y = 42 // expected-error {{cannot assign to property: 'y' is a 'let' constant}}
      }

      get { x }
    }

    var point: (Int, Int) {
      @storageRestrictions(initializes: x, y)
      init(initialValue) {
        self.x = initialValue.0 // Ok
        self.y = initialValue.1 // Ok
      }

      get { (x, y) }
      set { }
    }

    init(x: Int, y: Int) {
      self.point = (x, y)
    }
  }
}

func test_duplicate_and_computed_lazy_properties() {
  struct Test1 {
    var _a: Int
    var _b: Int

    var a: Int {
      @storageRestrictions(initializes: _b, _a, accesses: _a)
      // expected-error@-1 {{property '_a' cannot be both initialized and accessed}}
      init(initialValue) {
      }

      get { _a }
      set { }
    }

    init() {
    }
  }

  struct Test2 {
    var _a: Int

    var a: Int {
      @storageRestrictions(initializes: a, c, accesses: _a, b)
      // expected-error@-1 {{init accessor cannot refer to property 'a'; init accessors can refer only to stored properties}}
      // expected-error@-2 {{init accessor cannot refer to property 'b'; init accessors can refer only to stored properties}}
      // expected-error@-3 {{init accessor cannot refer to property 'c'; init accessors can refer only to stored properties}}
      init(initialValue) {}

      get { _a }
    }

    var b: Int {
      get { 42 }
    }

    lazy var c: Int = 42
  }
}

func test_invalid_self_uses() {
  func capture<T>(_: T) -> Int? { nil }

  struct Test {
    var a: Int {
      init(initialValue) {
        let x = self
        // expected-error@-1 {{'self' within init accessors can only be used to reference properties listed in 'initializes' and 'accesses'; init accessors are run before 'self' is fully available}}

        _ = {
          print(self)
          // expected-error@-1 {{'self' within init accessors can only be used to reference properties listed in 'initializes' and 'accesses'; init accessors are run before 'self' is fully available}}
        }

        _ = { [weak self] in
          // expected-error@-1 {{'self' within init accessors can only be used to reference properties listed in 'initializes' and 'accesses'; init accessors are run before 'self' is fully available}}
        }

        _ = {
          guard let _ = capture(self) else {
            // expected-error@-1 {{'self' within init accessors can only be used to reference properties listed in 'initializes' and 'accesses'; init accessors are run before 'self' is fully available}}
            return
          }
        }

        Test.test(self)
        // expected-error@-1 {{'self' within init accessors can only be used to reference properties listed in 'initializes' and 'accesses'; init accessors are run before 'self' is fully available}}
      }

      get { 42 }
      set { }
    }

    static func test<T>(_: T) {}
  }
}

func test_invalid_references() {
  struct Test {
    var _a: Int
    var _b: Int

    var c: String
    static var c: String = ""

    var _d: String

    var data: Int {
      @storageRestrictions(initializes: _a, accesses: _d)
      init(initialValue) {
        _a = initialValue // Ok

        print(_d) // Ok
        self._d = "" // Ok

        if self._b > 0 { // expected-error {{cannot reference instance member '_b'; init accessors can only refer to instance properties listed in 'initializes' and 'accesses' attributes}}
        }

        let x = c.lowercased()
        // expected-error@-1 {{static member 'c' cannot be used on instance of type 'Test'}}

        print(Test.c.lowercased()) // Ok

        guard let v = test() else {
          // expected-error@-1 {{cannot reference instance member 'test'; init accessors can only refer to instance properties listed in 'initializes' and 'accesses' attributes}}
          return
        }

        _ = {
          if true {
            print(_b)
            // expected-error@-1 {{cannot reference instance member '_b'; init accessors can only refer to instance properties listed in 'initializes' and 'accesses' attributes}}
            print(self._b)
            // expected-error@-1 {{cannot reference instance member '_b'; init accessors can only refer to instance properties listed in 'initializes' and 'accesses' attributes}}
          }
        }
      }

      get { _a }
      set { }
    }

    func test() -> Int? { 42 }

    init() {}
  }
}

func test_memberwise_with_overlaps_dont_synthesize_inits() {
  struct Test1<T, U> {
    var _a: T
    var _b: Int

    var a: T {
      @storageRestrictions(initializes: _a)
      init(initialValue) {
        _a = initialValue
      }

      get { _a }
      set { }
    }

    var pair: (T, Int) {
      @storageRestrictions(initializes: _a, _b)
      init(initialValue) {
        _a = initialValue.0
        _b = initialValue.1
      }

      get { (_a, _b) }
      set { }
    }

    var c: U
  }

  _ = Test1<String, [Double]>(a: "a", pair: ("b", 1), c: [3.0])
  // expected-error@-1 {{'Test1<String, [Double]>' cannot be constructed because it has no accessible initializers}}

  struct Test2<T, U> {
    var _a: T
    var _b: Int

    var a: T {
      @storageRestrictions(initializes: _a)
      init(initialValue) {
        _a = initialValue
      }

      get { _a }
      set { }
    }

    var b: Int {
      @storageRestrictions(initializes: _b)
      init(initialValue) {
        _b = initialValue
      }

      get { _b }
      set { }
    }

    var _c: U

    var pair: (T, U) {
      @storageRestrictions(initializes: _a, _c)
      init(initialValue) {
        _a = initialValue.0
        _c = initialValue.1
      }

      get { (_a, _c) }
      set { }
    }
  }

  _ = Test2<String, Int>(a: "a", pair: ("c", 2), b: 0)
  // expected-error@-1 {{'Test2<String, Int>' cannot be constructed because it has no accessible initializers}}

  struct Test3<T, U> {
    var _a: T
    var _b: Int

    var a: T {
      @storageRestrictions(initializes: _a)
      init(initialValue) {
        _a = initialValue
      }

      get { _a }
      set { }
    }

    var b: Int {
      @storageRestrictions(initializes: _b)
      init(initialValue) {
        _b = initialValue
      }

      get { _b }
      set { }
    }

    var _c: U

    var c: U {
      @storageRestrictions(initializes: _c)
      init(initialValue) {
        _c = initialValue
      }

      get { _c }
      set { }
    }

    var triple: (T, Int, U) {
      @storageRestrictions(initializes: _a, _b, _c)
      init(initialValue) {
        _a = initialValue.0
        _b = initialValue.1
        _c = initialValue.2
      }

      get { (_a, _b, _c) }
      set { }
    }
  }

  _ = Test3<String, [Double]>(a: "a", triple: ("b", 2, [1.0, 2.0]), b: 0, c: [1.0])
  // expected-error@-1 {{'Test3<String, [Double]>' cannot be constructed because it has no accessible initializers}}
}

func test_memberwise_ordering() {
  struct Test1 {
    var _a: Int
    var _b: Int

    var a: Int {
      @storageRestrictions(initializes: _a, accesses: _b)
      init(initialValue) {
        _a = initialValue
      }

      get { _a }
      set { }
    }
  }

  _ = Test1(_b: 42, a: 0) // Ok

  struct Test2 { // expected-error {{cannot synthesize memberwise initializer}}
    var _a: Int

    var a: Int {
      @storageRestrictions(initializes: _a, accesses: _b)
      init(initialValue) {
        // expected-note@-1 {{init accessor for 'a' cannot access stored property '_b' because it is called before '_b' is initialized}}
        _a = initialValue
      }

      get { _a }
    }

    var _b: Int
  }

  struct Test3 {
    var _a: Int

    var pair: (Int, Int) {
      @storageRestrictions(initializes: _a, _b)
      init(initialValue) {
        _a = initialValue.0
        _b = initialValue.1
      }

      get { (_a, _b) }
      set { }
    }

    var _b: Int
  }

  _ = Test3(pair: (0, 1)) // Ok

  struct Test4 {
    var _a: Int
    var _b: Int

    var pair: (Int, Int) {
      @storageRestrictions(accesses: _a, _b)
      init(initalValue) {
      }

      get { (_a, _b) }
      set { }
    }

    var _c: Int
  }

  _ = Test4(_a: 0, _b: 1, pair: (1, 2), _c: 2) // Ok

  struct Test5 {
    var _a: Int
    var _b: Int

    var c: Int {
      @storageRestrictions(initializes: _c, accesses: _a, _b)
      init(initalValue) {
      }

      get { _c }
      set { }
    }

    var _c: Int
  }

  _ = Test5(_a: 0, _b: 1, c: 2) // Ok
}

func test_default_arguments_are_analyzed() {
  struct Test {
    var pair: (Int, Int) = (0, 1) { // Ok
      init {}

      get { (0, 1) }
    }

    var other: (Int, String) = ("", 42) {
      // expected-error@-1 {{cannot convert value of type '(String, Int)' to specified type '(Int, String)'}}
      init(initialValue) {}

      get { (0, "") }
    }

    var otherPair = (0, 1) {
      // expected-error@-1 {{computed property must have an explicit type}}
      init(initalValue) {}

      get { 42 }
      // expected-error@-1 {{cannot convert return expression of type 'Int' to return type '(Int, Int)'}}
    }
  }
}

struct TestStructPropWithoutSetter {
  var _x: Int

  var x: Int {
    @storageRestrictions(initializes: _x)
    init(initialValue) {
      self._x = initialValue
    }

    get { _x }
  }

  init(v: Int) {
    x = v // Ok
  }
}

extension TestStructPropWithoutSetter {
  init(other: Int) {
    x = other // Ok
  }

  init(other: inout TestStructPropWithoutSetter, v: Int) {
    other.x = v // expected-error {{cannot assign to property: 'x' is immutable}}
  }
}

do {
  class TestClassPropWithoutSetter {
    var x: Int {
      init {
      }

      get { 0 }
    }
  }

  class SubTestPropWithoutSetter : TestClassPropWithoutSetter {
    init(otherV: Int) {
      x = otherV // Ok
    }
  }

  class OtherWithoutSetter<U> {
    var data: U {
      init {
      }

      get { fatalError() }
    }

    init(data: U) {
      self.data = data // Ok
    }
  }
}

func test_invalid_storage_restrictions() {
  struct Test {
    var _a: Int = 0
    var _b: Int = 0

    var a: Int {
      @storageRestrictions()
      // expected-error@-1 {{missing label in @storageRestrictions attribute}}
      init {}

      get { _a }
    }

    var b: Int {
      @storageRestrictions(initializes:)
      // expected-error@-1 {{expected property name in @storageRestrictions list}}
      init {}

      get { _b }
    }

    var c: Int {
      @storageRestrictions(initializes: a, initializes: b)
      // expected-error@-1 {{duplicate label 'initializes' in @storageRestrictions attribute}}
      init {}

      get { 0 }
    }

    var d: Int {
      @storageRestrictions(accesses: a, accesses: c)
      // expected-error@-1 {{duplicate label 'accesses' in @storageRestrictions attribute}}
      init {}

      get { 0 }
    }

    var e: Int {
      @storageRestrictions(initialize: a, b, accesses: c, d)
      // expected-error@-1 {{unexpected label 'initialize' in @storageRestrictions attribute}}
      init {}

      get { 0 }
    }

    var f: Int {
      @storageRestrictions(initializes: _a, accesses: _b, _a)
      // expected-error@-1 {{property '_a' cannot be both initialized and accessed}}
      init {}
      // expected-error@-1 {{variable with an init accessor must also have a getter}}
    }

    var g: Int {
      @storageRestrictions(initializes: _a)
      // expected-error@-1 {{@storageRestrictions attribute could only be used with init accessors}}
      get { 0 }
    }

    init() {}
  }
}
