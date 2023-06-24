// RUN: %target-run-simple-swift(-Xfrontend -enable-experimental-feature -Xfrontend InitAccessors) | %FileCheck %s
// RUN: %target-run-simple-swift(-O -Xfrontend -enable-experimental-feature -Xfrontend InitAccessors) | %FileCheck %s

// REQUIRES: executable_test
// REQUIRES: asserts

struct TestInit {
  var x: Int
  var y: Int
  var full: (Int, Int)

  var point: (Int, Int) {
    init(initialValue) initializes(y, full) accesses(x) {
      self.y = initialValue.1
      self.full = (self.x, self.y)
    }

    get { full }
    set { full = newValue }
  }

  init(x: Int, y: Int) {
    self.x = x
    self.point = (x, y)
  }
}

do {
  let test = TestInit(x: 0, y: -1)
  print("test-init: \(test.point)")
  // CHECK: test-init: (0, -1)
}

struct TestSetter {
  var x: Int
  var y: Int

  var point: (Int, Int) {
    init(initialValue) accesses(x, y) {
    }

    get { (x, y) }
    set { }
  }

  init(x: Int, y: Int) {
    self.x = x
    self.y = y
    self.point = (x, y)
  }
}

do {
  let test = TestSetter(x: 0, y: -2)
  print("test-setter: \(test.point)")
  // CHECK: test-setter: (0, -2)
}

struct TestInitThenSetter {
  var x: Int
  var y: Int

  var point: (Int, Int) {
    init(initialValue) initializes(x, y) {
      self.x = initialValue.0
      self.y = initialValue.1
    }

    get { (x, y) }

    set {
      x = newValue.0
      y = newValue.1
    }
  }

  init(x: Int, y: Int) {
    self.point = (x, y)

    if x == 1 {
      self.point = (0, 0)
    }
  }
}

do {
  let test = TestInitThenSetter(x: 1, y: 2)
  print("test-init-then-setter: \(test.point)")
  // CHECK: test-init-then-setter: (0, 0)
}

struct TestPartialInt {
  var x: Int
  var y: Int

  var pointX: Int {
    init(newValue) initializes(x) {
      self.x = newValue
    }

    get { x }
    set { self.x = newValue }
  }

  var pointY: Int {
    init(newValue) initializes(y) {
      self.y = newValue
    }

    get { y }
    set { self.y = newValue }
  }

  init(x: Int, y: Int) {
    // Init
    self.pointX = x
    // Init
    self.pointY = y

    // Setter
    self.pointX = 1
    // Setter
    self.pointY = 2
  }
}

do {
  let test = TestPartialInt(x: 0, y: -1)
  print("test-partial-init: (\(test.pointX), \(test.pointY))")
  // CHECK: test-partial-init: (1, 2)
}

struct TestNoInitAndInit {
  var x: Int
  var y: Int

  var pointX: Int {
    init(initalValue) accesses(x) {
    }

    get { x }
    set { }
  }

  var pointY: Int {
    init(initialValue) initializes(y) {
      self.y = initialValue
    }

    get { y }
    set { }
  }

  init(x: Int, y: Int) {
    self.x = x
    self.pointX = x
    self.pointY = y
    print("TestNoInitAndInit(x: \(self.x), y: \(self.y))")
  }
}

do {
  _ = TestNoInitAndInit(x: 10, y: -10)
  // CHECK: TestNoInitAndInit(x: 10, y: -10)
}

class TestClass {
  var x: Int
  var y: (Int, [String])

  var data: (Int, (Int, [String])) {
    init(initialValue) initializes(x, y) {
      x = initialValue.0
      y = initialValue.1
    }

    get { (x, y) }
    set {
      x = newValue.0
      y = newValue.1
    }
  }

  init(x: Int, y: (Int, [String])) {
    self.data = (x, y)
  }
}

do {
  let test = TestClass(x: 20, y: (0, ["a", "b"]))
  print("test-class: \(test.data)")
  // CHECK: test-class: (20, (0, ["a", "b"]))
}

struct TestGeneric<T, U> {
  var a: T
  var b: T
  var c: U

  var data: (T, T) {
    init(initialValue) initializes(a, b) accesses(c) {
      a = initialValue.0
      b = initialValue.1
      print("TestGeneric(c: \(c))")
    }

    get { (a, b) }
    set { }
  }

  init(a: T, b: T, c: U) {
    self.c = c
    self.data = (a, b)
    self.data = (b, a)
  }
}

do {
  let test = TestGeneric(a: 42, b: 0, c: [42, "a"] as [Any])
  print("test-generic: data = \(test.data)")
  // CHECK: TestGeneric(c: [42, "a"])
  // CHECK-NEXT: test-generic: data = (42, 0)
}

func test_local_with_memberwise() {
  class MyValue {}

  struct TestMemberwiseConcrete {
    var a: Int
    var b: String

    var pair: (Int, String) {
      init(initialValue) initializes(a, b) {
        a = initialValue.0
        b = initialValue.1
      }

      get { (a, b) }
      set { }
    }

    var c: [MyValue]
  }

  let concrete = TestMemberwiseConcrete(pair: (0, "a"), c: [])
  print(concrete)

  struct TestMemberwiseGeneric<T, C> where C: RangeReplaceableCollection, C.Element == T {
    var _a: T
    var _b: String
    var _c: C

    var a: T {
      init(initialValue) initializes(_a) {
        _a = initialValue
      }

      get { _a }
      set { }
    }

    var pair: (String, C) {
      init(initialValue) initializes(_b, _c) accesses(_a) {
        _b = initialValue.0
        _c = initialValue.1
        _c.append(_a)
      }

      get { (_b, _c) }
      set { }
    }
  }

  let generic = TestMemberwiseGeneric(a: 1, pair: ("a", [0]))
  print(generic)
}

test_local_with_memberwise()
// CHECK: TestMemberwiseConcrete(a: 0, b: "a", c: [])
// CHECK-NEXT: TestMemberwiseGeneric<Int, Array<Int>>(_a: 1, _b: "a", _c: [0, 1])

func test_assignments() {
  struct Test {
    var _a: Int
    var _b: Int

    var a: Int {
      init(initialValue) initializes(_a) {
        self._a = initialValue
        print("a-init-accessor: \(self._a)")
      }
      get { _a }
      set { _a = newValue + 1 }
    }

    var pair: (Int, Int) {
      init(initialValue) initializes(_a, _b) {
        _a = initialValue.0
        _b = initialValue.1
      }

      get { (_a, _b) }
      set { }
    }

    init(a: Int) {
      // init
      self.a = a
      // re-assignment
      self.a = a + 1
      self._b = 42
      // set
      self.a = a + 2
    }

    init(a: Int, b: Int) {
      self.a = a
      self.pair = (0, b)
    }
  }

  let test1 = Test(a: 0)
  print("test-assignments-1: \(test1.pair)")

  let test2 = Test(a: 0, b: 2)
  print("test-assignments-2: \(test2.pair)")
}

test_assignments()
// CHECK: a-init-accessor: 0
// CHECK-NEXT: a-init-accessor: 1
// CHECK-NEXT: test-assignments-1: (3, 42)
// CHECK-NEXT: a-init-accessor: 0
// CHECK-NEXT: test-assignments-2: (0, 2)

func test_memberwise_ordering() {
  struct Test1 {
    var _a: Int
    var _b: Int

    var a: Int {
      init(initialValue) initializes(_a) accesses(_b) {
        _a = initialValue
      }

      get { _a }
      set { }
    }
  }

  let test1 = Test1(_b: 42, a: 0)
  print("test-memberwise-ordering-1: \(test1)")

  struct Test2 {
    var _a: Int

    var pair: (Int, Int) {
      init(initialValue) initializes(_a, _b) {
        _a = initialValue.0
        _b = initialValue.1
      }

      get { (_a, _b) }
      set { }
    }

    var _b: Int
  }

  let test2 = Test2(pair: (-1, -2))
  print("test-memberwise-ordering-2: \(test2)")

  struct Test3 {
    var _a: Int
    var _b: Int

    var pair: (Int, Int) {
      init(initialValue) accesses(_a, _b) {
      }

      get { (_a, _b) }
      set { }
    }

    var _c: Int
  }

  let test3 = Test3(_a: 1, _b: 2, _c: 3)
  print("test-memberwise-ordering-3: \(test3)")
}

test_memberwise_ordering()
// CHECK: test-memberwise-ordering-1: Test1(_a: 0, _b: 42)
// CHECK-NEXT: test-memberwise-ordering-2: Test2(_a: -1, _b: -2)
// CHECK-NEXT: test-memberwise-ordering-3: Test3(_a: 1, _b: 2, _c: 3)

func test_memberwise_with_default_args() {
  struct TestWithoutDefault {
    var _a: Int
    var _b: Int

    var pair: (Int, Int) = (-1, 42) {
      init(initialValue) initializes(_a, _b) {
        _a = initialValue.0
        _b = initialValue.1
      }

      get { (0, 42) }
      set { }
    }
  }

  let test1 = TestWithoutDefault()
  print("test-memberwise_with_default-1: \(test1)")

  let test2 = TestWithoutDefault(pair: (42, -1))
  print("test-memberwise_with_default-2: \(test2)")

  struct TestDefaulted {
    var _a: Int = 0
    var _b: Int = 0

    var pair: (Int, Int) = (1, 2) {
      init(initialValue) initializes(_a, _b) {
        _a = initialValue.0
        _b = initialValue.1
      }

      get { (_a, _b) }
      set { }
    }
  }

  let test3 = TestDefaulted()
  print("test-defaulted-1: \(test3)")

  let test4 = TestDefaulted(pair: (3, 4))
  print("test-defaulted-2: \(test4)")

  class TestClass {
    var _q: String = "<<default>>"
    var _a: Int = 1

    var pair: (String, Int) = ("", 42) {
      init(initialValue) initializes(_q, _a) {
        _q = initialValue.0
        _a = initialValue.1
      }

      get { (_q, _a) }
      set { }
    }
  }

  let test5 = TestClass()
  print("test-defaulted-class: \(test5.pair)")
}

test_memberwise_with_default_args()
// CHECK: test-memberwise_with_default-1: TestWithoutDefault(_a: -1, _b: 42)
// CHECK-NEXT: test-memberwise_with_default-2: TestWithoutDefault(_a: 42, _b: -1)
// CHECK-NEXT: test-defaulted-1: TestDefaulted(_a: 0, _b: 0)
// CHECK-NEXT: test-defaulted-2: TestDefaulted(_a: 3, _b: 4)
// CHECK-NEXT: test-defaulted-class: ("<<default>>", 1)
