// RUN: %target-typecheck-verify-swift -parse-as-library -module-name MyModule

struct Nested {
  var b: Int

  @discardableResult
  mutating func setToZero() -> Int {
    let prev = b
    b = 0
    return prev
  }
}

struct StructValue {
  static var defaultValue: Self { .init(a: Nested(b: 1)) }

  var a: Nested

  subscript(_ i: Int) -> Nested {
    get { a }
    set { a = newValue }
  }

  @discardableResult
  mutating func setToZero() -> Int {
    let prev = a.b
    a.setToZero()
    return prev
  }
}

class ClassValue {
  var a: Nested

  required init(a: Nested) {
    self.a = a
  }

  subscript(_ i: Int) -> Nested {
    get { a }
    set { a = newValue }
  }

  @discardableResult func setToZero() -> Int {
    let prev = a.b
    a.setToZero()
    return prev
  }
}

struct BaseStruct<T> {
  var available: T {
    get { fatalError() }
    set { }
  }

  var unavailableGetter: T {
    @available(*, unavailable)
    get { fatalError() } // expected-note 67 {{getter for 'unavailableGetter' has been explicitly marked unavailable here}}
    set {}
  }

  var unavailableSetter: T {
    get { fatalError() }
    @available(*, unavailable)
    set { fatalError() } // expected-note 38 {{setter for 'unavailableSetter' has been explicitly marked unavailable here}}
  }

  var unavailableGetterAndSetter: T {
    @available(*, unavailable)
    get { fatalError() } // expected-note 67 {{getter for 'unavailableGetterAndSetter' has been explicitly marked unavailable here}}
    @available(*, unavailable)
    set { fatalError() } // expected-note 38 {{setter for 'unavailableGetterAndSetter' has been explicitly marked unavailable here}}
  }
}

struct SubscriptHelper {
  subscript<T>(available t: T) -> () {
    get { }
    set { }
  }

  subscript<T>(unavailableGetter t: T) -> () {
    @available(*, unavailable)
    get { } // expected-note {{getter for 'subscript(unavailableGetter:)' has been explicitly marked unavailable here}}
    set { }
  }

  subscript<T>(unavailableSetter t: T) -> () {
    get { }
    @available(*, unavailable)
    set { } // expected-note {{setter for 'subscript(unavailableSetter:)' has been explicitly marked unavailable here}}
  }

  subscript<T>(unavailableGetterAndSetter t: T) -> () {
    @available(*, unavailable)
    get { } // expected-note {{getter for 'subscript(unavailableGetterAndSetter:)' has been explicitly marked unavailable here}}
    @available(*, unavailable)
    set { } // expected-note {{setter for 'subscript(unavailableGetterAndSetter:)' has been explicitly marked unavailable here}}
  }

}

@discardableResult func takesInOut<T>(_ t: inout T) -> T {
  return t
}

func testDiscardedRValueLoads_Struct() {
  var x = BaseStruct<StructValue>() // expected-warning {{variable 'x' was never mutated; consider changing to 'let' constant}}

  _ = x.available
  _ = x.available.a
  _ = x.available[0]
  _ = x.available[0].b

  _ = x.unavailableGetter // expected-error {{getter for 'unavailableGetter' is unavailable}}
  _ = x.unavailableGetter.a // expected-error {{getter for 'unavailableGetter' is unavailable}}
  _ = x.unavailableGetter[0] // expected-error {{getter for 'unavailableGetter' is unavailable}}
  _ = x.unavailableGetter[0].b // expected-error {{getter for 'unavailableGetter' is unavailable}}

  _ = x.unavailableSetter
  _ = x.unavailableSetter.a
  _ = x.unavailableSetter[0]
  _ = x.unavailableSetter[0].b

  _ = x.unavailableGetterAndSetter // expected-error {{getter for 'unavailableGetterAndSetter' is unavailable}}
  _ = x.unavailableGetterAndSetter.a // expected-error {{getter for 'unavailableGetterAndSetter' is unavailable}}
  _ = x.unavailableGetterAndSetter[0] // expected-error {{getter for 'unavailableGetterAndSetter' is unavailable}}
  _ = x.unavailableGetterAndSetter[0].b // expected-error {{getter for 'unavailableGetterAndSetter' is unavailable}}
}

func testDiscardedRValueLoads_Class() {
  var x = BaseStruct<ClassValue>() // expected-warning {{variable 'x' was never mutated; consider changing to 'let' constant}}

  _ = x.available
  _ = x.available.a
  _ = x.available[0]
  _ = x.available[0].b

  _ = x.unavailableGetter // expected-error {{getter for 'unavailableGetter' is unavailable}}
  _ = x.unavailableGetter.a // expected-error {{getter for 'unavailableGetter' is unavailable}}
  _ = x.unavailableGetter[0] // expected-error {{getter for 'unavailableGetter' is unavailable}}
  _ = x.unavailableGetter[0].b // expected-error {{getter for 'unavailableGetter' is unavailable}}

  _ = x.unavailableSetter
  _ = x.unavailableSetter.a
  _ = x.unavailableSetter[0]
  _ = x.unavailableSetter[0].b

  _ = x.unavailableGetterAndSetter // expected-error {{getter for 'unavailableGetterAndSetter' is unavailable}}
  _ = x.unavailableGetterAndSetter.a // expected-error {{getter for 'unavailableGetterAndSetter' is unavailable}}
  _ = x.unavailableGetterAndSetter[0] // expected-error {{getter for 'unavailableGetterAndSetter' is unavailable}}
  _ = x.unavailableGetterAndSetter[0].b // expected-error {{getter for 'unavailableGetterAndSetter' is unavailable}}
}

func testLValueAssignments_Struct(_ someValue: StructValue) {
  var x = BaseStruct<StructValue>()

  x.available = someValue
  x.available.a = someValue.a
  x.available[0] = someValue.a
  x.available[0].b = 1

  x.unavailableGetter = someValue
  x.unavailableGetter.a = someValue.a // expected-error {{getter for 'unavailableGetter' is unavailable}}
  x.unavailableGetter[0] = someValue.a // expected-error {{getter for 'unavailableGetter' is unavailable}}
  x.unavailableGetter[0].b = 1 // expected-error {{getter for 'unavailableGetter' is unavailable}}

  x.unavailableSetter = someValue // expected-error {{setter for 'unavailableSetter' is unavailable}}
  x.unavailableSetter.a = someValue.a // expected-error {{setter for 'unavailableSetter' is unavailable}}
  x.unavailableSetter[0] = someValue.a // expected-error {{setter for 'unavailableSetter' is unavailable}}
  x.unavailableSetter[0].b = 1 // expected-error {{setter for 'unavailableSetter' is unavailable}}

  x.unavailableGetterAndSetter = someValue // expected-error {{setter for 'unavailableGetterAndSetter' is unavailable}}
  x.unavailableGetterAndSetter.a = someValue.a // expected-error {{getter for 'unavailableGetterAndSetter' is unavailable}} expected-error {{setter for 'unavailableGetterAndSetter' is unavailable}}
  x.unavailableGetterAndSetter[0] = someValue.a // expected-error {{getter for 'unavailableGetterAndSetter' is unavailable}} expected-error {{setter for 'unavailableGetterAndSetter' is unavailable}}
  x.unavailableGetterAndSetter[0].b = 1 // expected-error {{getter for 'unavailableGetterAndSetter' is unavailable}} expected-error {{setter for 'unavailableGetterAndSetter' is unavailable}}
}

func testLValueAssignments_Class(_ someValue: ClassValue) {
  var x = BaseStruct<ClassValue>()

  x.available = someValue
  x.available.a = someValue.a
  x.available[0] = someValue.a
  x.available[0].b = 1

  x.unavailableGetter = someValue
  x.unavailableGetter.a = someValue.a // expected-error {{getter for 'unavailableGetter' is unavailable}}
  x.unavailableGetter[0] = someValue.a // expected-error {{getter for 'unavailableGetter' is unavailable}}
  x.unavailableGetter[0].b = 1 // expected-error {{getter for 'unavailableGetter' is unavailable}}

  x.unavailableSetter = someValue // expected-error {{setter for 'unavailableSetter' is unavailable}}
  x.unavailableSetter.a = someValue.a
  x.unavailableSetter[0] = someValue.a
  x.unavailableSetter[0].b = 1

  x.unavailableGetterAndSetter = someValue // expected-error {{setter for 'unavailableGetterAndSetter' is unavailable}}
  x.unavailableGetterAndSetter.a = someValue.a // expected-error {{getter for 'unavailableGetterAndSetter' is unavailable}}
  x.unavailableGetterAndSetter[0] = someValue.a // expected-error {{getter for 'unavailableGetterAndSetter' is unavailable}}
  x.unavailableGetterAndSetter[0].b = 1 // expected-error {{getter for 'unavailableGetterAndSetter' is unavailable}}
}

func testSubscripts(_ s: BaseStruct<StructValue>) {
  var x = BaseStruct<SubscriptHelper>()

  // Available subscript, available member, varying argument availability
  x.available[available: s.available] = ()
  x.available[available: s.unavailableGetter] = () // expected-error {{getter for 'unavailableGetter' is unavailable}}
  x.available[available: s.unavailableSetter] = ()
  x.available[available: s.unavailableGetterAndSetter] = () // expected-error {{getter for 'unavailableGetterAndSetter' is unavailable}}

  _ = x.available[available: s.available]
  _ = x.available[available: s.unavailableGetter] // expected-error {{getter for 'unavailableGetter' is unavailable}}
  _ = x.available[available: s.unavailableSetter]
  _ = x.available[available: s.unavailableGetterAndSetter] // expected-error {{getter for 'unavailableGetterAndSetter' is unavailable}}

  // Varying subscript availability, available member, available argument
  x.available[unavailableGetter: s.available] = ()
  x.available[unavailableSetter: s.available] = () // expected-error {{setter for 'subscript(unavailableSetter:)' is unavailable}}
  x.available[unavailableGetterAndSetter: s.available] = () // expected-error {{setter for 'subscript(unavailableGetterAndSetter:)' is unavailable}}

  _ = x.available[unavailableGetter: s.available] // expected-error {{getter for 'subscript(unavailableGetter:)' is unavailable}}
  _ = x.available[unavailableSetter: s.available]
  _ = x.available[unavailableGetterAndSetter: s.available] // expected-error {{getter for 'subscript(unavailableGetterAndSetter:)' is unavailable}}

  // Available subscript, varying member availability, available argument
  x.unavailableGetter[available: s.available] = () // expected-error {{getter for 'unavailableGetter' is unavailable}}
  x.unavailableSetter[available: s.available] = () // expected-error {{setter for 'unavailableSetter' is unavailable}}
  x.unavailableGetterAndSetter[available: s.available] = () // expected-error {{getter for 'unavailableGetterAndSetter' is unavailable}} expected-error {{setter for 'unavailableGetterAndSetter' is unavailable}}

  _ = x.unavailableGetter[available: s.available] // expected-error {{getter for 'unavailableGetter' is unavailable}}
  _ = x.unavailableSetter[available: s.available]
  _ = x.unavailableGetterAndSetter[available: s.available] // expected-error {{getter for 'unavailableGetterAndSetter' is unavailable}}
}

func testDiscardedKeyPathLoads_Struct() {
  let a = [0]
  var x = BaseStruct<StructValue>()

  _ = x[keyPath: \.available]
  _ = x[keyPath: \.available.a]
  _ = x[keyPath: \.available[0]]
  _ = x[keyPath: \.available[0].b]
  _ = a[keyPath: \.[takesInOut(&x.available.a.b)]]
  _ = a[keyPath: \.[takesInOut(&x.available[0].b)]]

  _ = x[keyPath: \.unavailableGetter] // expected-error {{getter for 'unavailableGetter' is unavailable}}
  _ = x[keyPath: \.unavailableGetter.a] // expected-error {{getter for 'unavailableGetter' is unavailable}}
  _ = x[keyPath: \.unavailableGetter[0]] // expected-error {{getter for 'unavailableGetter' is unavailable}}
  _ = x[keyPath: \.unavailableGetter[0].b] // expected-error {{getter for 'unavailableGetter' is unavailable}}
  _ = a[keyPath: \.[takesInOut(&x.unavailableGetter.a.b)]] // expected-error {{getter for 'unavailableGetter' is unavailable}}
  _ = a[keyPath: \.[takesInOut(&x.unavailableGetter[0].b)]] // expected-error {{getter for 'unavailableGetter' is unavailable}}

  _ = x[keyPath: \.unavailableSetter]
  _ = x[keyPath: \.unavailableSetter.a]
  _ = x[keyPath: \.unavailableSetter[0]]
  _ = x[keyPath: \.unavailableSetter[0].b]
  _ = a[keyPath: \.[takesInOut(&x.unavailableSetter.a.b)]] // expected-error {{setter for 'unavailableSetter' is unavailable}}
  _ = a[keyPath: \.[takesInOut(&x.unavailableSetter[0].b)]] // expected-error {{setter for 'unavailableSetter' is unavailable}}

  _ = x[keyPath: \.unavailableGetterAndSetter] // expected-error {{getter for 'unavailableGetterAndSetter' is unavailable}}
  _ = x[keyPath: \.unavailableGetterAndSetter.a] // expected-error {{getter for 'unavailableGetterAndSetter' is unavailable}}
  _ = x[keyPath: \.unavailableGetterAndSetter[0]] // expected-error {{getter for 'unavailableGetterAndSetter' is unavailable}}
  _ = x[keyPath: \.unavailableGetterAndSetter[0].b] // expected-error {{getter for 'unavailableGetterAndSetter' is unavailable}}
  _ = a[keyPath: \.[takesInOut(&x.unavailableGetterAndSetter.a.b)]] // expected-error {{getter for 'unavailableGetterAndSetter' is unavailable}} // expected-error {{setter for 'unavailableGetterAndSetter' is unavailable}}
  _ = a[keyPath: \.[takesInOut(&x.unavailableGetterAndSetter[0].b)]] // expected-error {{getter for 'unavailableGetterAndSetter' is unavailable}} expected-error {{setter for 'unavailableGetterAndSetter' is unavailable}}
}

func testDiscardedKeyPathLoads_Class() {
  let a = [0]
  var x = BaseStruct<ClassValue>() // expected-warning {{variable 'x' was never mutated; consider changing to 'let' constant}}

  _ = x[keyPath: \.available]
  _ = x[keyPath: \.available.a]
  _ = x[keyPath: \.available[0]]
  _ = x[keyPath: \.available[0].b]
  _ = a[keyPath: \.[takesInOut(&x.available.a.b)]]
  _ = a[keyPath: \.[takesInOut(&x.available[0].b)]]

  _ = x[keyPath: \.unavailableGetter] // expected-error {{getter for 'unavailableGetter' is unavailable}}
  _ = x[keyPath: \.unavailableGetter.a] // expected-error {{getter for 'unavailableGetter' is unavailable}}
  _ = x[keyPath: \.unavailableGetter[0]] // expected-error {{getter for 'unavailableGetter' is unavailable}}
  _ = x[keyPath: \.unavailableGetter[0].b] // expected-error {{getter for 'unavailableGetter' is unavailable}}
  _ = a[keyPath: \.[takesInOut(&x.unavailableGetter.a.b)]] // expected-error {{getter for 'unavailableGetter' is unavailable}}
  _ = a[keyPath: \.[takesInOut(&x.unavailableGetter[0].b)]] // expected-error {{getter for 'unavailableGetter' is unavailable}}

  _ = x[keyPath: \.unavailableSetter]
  _ = x[keyPath: \.unavailableSetter.a]
  _ = x[keyPath: \.unavailableSetter[0]]
  _ = x[keyPath: \.unavailableSetter[0].b]
  _ = a[keyPath: \.[takesInOut(&x.unavailableSetter.a.b)]]
  _ = a[keyPath: \.[takesInOut(&x.unavailableSetter[0].b)]]

  _ = x[keyPath: \.unavailableGetterAndSetter] // expected-error {{getter for 'unavailableGetterAndSetter' is unavailable}}
  _ = x[keyPath: \.unavailableGetterAndSetter.a] // expected-error {{getter for 'unavailableGetterAndSetter' is unavailable}}
  _ = x[keyPath: \.unavailableGetterAndSetter[0]] // expected-error {{getter for 'unavailableGetterAndSetter' is unavailable}}
  _ = x[keyPath: \.unavailableGetterAndSetter[0].b] // expected-error {{getter for 'unavailableGetterAndSetter' is unavailable}}
  _ = a[keyPath: \.[takesInOut(&x.unavailableGetterAndSetter.a.b)]] // expected-error {{getter for 'unavailableGetterAndSetter' is unavailable}}
  _ = a[keyPath: \.[takesInOut(&x.unavailableGetterAndSetter[0].b)]] // expected-error {{getter for 'unavailableGetterAndSetter' is unavailable}}
}

func testKeyPathAssignments_Struct(_ someValue: StructValue) {
  var a = [0]
  var x = BaseStruct<StructValue>()

  x[keyPath: \.available] = someValue
  x[keyPath: \.available.a] = someValue.a
  x[keyPath: \.available[0]] = someValue.a
  x[keyPath: \.available[0].b] = 1
  x[keyPath: \.available] = someValue
  a[keyPath: \.[takesInOut(&x.available.a.b)]] = 0
  a[keyPath: \.[takesInOut(&x.available[0].b)]] = 0

  x[keyPath: \.unavailableGetter] = someValue
  x[keyPath: \.unavailableGetter.a] = someValue.a // FIXME: missing diagnostic for getter
  x[keyPath: \.unavailableGetter[0]] = someValue.a // FIXME: missing diagnostic for getter
  x[keyPath: \.unavailableGetter[0].b] = 1 // FIXME: missing diagnostic for getter
  a[keyPath: \.[takesInOut(&x.unavailableGetter.a.b)]] = 0 // expected-error {{getter for 'unavailableGetter' is unavailable}}
  a[keyPath: \.[takesInOut(&x.unavailableGetter[0].b)]] = 0 // expected-error {{getter for 'unavailableGetter' is unavailable}}

  x[keyPath: \.unavailableSetter] = someValue // expected-error {{setter for 'unavailableSetter' is unavailable}}
  x[keyPath: \.unavailableSetter.a] = someValue.a // expected-error {{setter for 'unavailableSetter' is unavailable}}
  x[keyPath: \.unavailableSetter[0]] = someValue.a // expected-error {{setter for 'unavailableSetter' is unavailable}}
  x[keyPath: \.unavailableSetter[0].b] = 1 // expected-error {{setter for 'unavailableSetter' is unavailable}}
  a[keyPath: \.[takesInOut(&x.unavailableSetter.a.b)]] = 0 // expected-error {{setter for 'unavailableSetter' is unavailable}}
  a[keyPath: \.[takesInOut(&x.unavailableSetter[0].b)]] = 0 // expected-error {{setter for 'unavailableSetter' is unavailable}}

  x[keyPath: \.unavailableGetterAndSetter] = someValue // expected-error {{setter for 'unavailableGetterAndSetter' is unavailable}}
  x[keyPath: \.unavailableGetterAndSetter.a] = someValue.a // expected-error {{setter for 'unavailableGetterAndSetter' is unavailable}}
  x[keyPath: \.unavailableGetterAndSetter[0]] = someValue.a // expected-error {{setter for 'unavailableGetterAndSetter' is unavailable}}
  x[keyPath: \.unavailableGetterAndSetter[0].b] = 1 // expected-error {{setter for 'unavailableGetterAndSetter' is unavailable}}
  a[keyPath: \.[takesInOut(&x.unavailableGetterAndSetter.a.b)]] = 0 // expected-error {{getter for 'unavailableGetterAndSetter' is unavailable}} expected-error {{setter for 'unavailableGetterAndSetter' is unavailable}}
  a[keyPath: \.[takesInOut(&x.unavailableGetterAndSetter[0].b)]] = 0 // expected-error {{getter for 'unavailableGetterAndSetter' is unavailable}} expected-error {{setter for 'unavailableGetterAndSetter' is unavailable}}
}

func testKeyPathAssignments_Class(_ someValue: ClassValue) {
  var a = [0]
  var x = BaseStruct<ClassValue>()

  x[keyPath: \.available] = someValue
  x[keyPath: \.available.a] = someValue.a
  x[keyPath: \.available[0]] = someValue.a
  x[keyPath: \.available[0].b] = 1
  x[keyPath: \.available] = someValue
  a[keyPath: \.[takesInOut(&x.available.a.b)]] = 0
  a[keyPath: \.[takesInOut(&x.available[0].b)]] = 0

  x[keyPath: \.unavailableGetter] = someValue
  x[keyPath: \.unavailableGetter.a] = someValue.a // FIXME: missing diagnostic for getter
  x[keyPath: \.unavailableGetter[0]] = someValue.a // FIXME: missing diagnostic for getter
  x[keyPath: \.unavailableGetter[0].b] = 1 // FIXME: missing diagnostic for getter
  a[keyPath: \.[takesInOut(&x.unavailableGetter.a.b)]] = 0 // expected-error {{getter for 'unavailableGetter' is unavailable}}
  a[keyPath: \.[takesInOut(&x.unavailableGetter[0].b)]] = 0 // expected-error {{getter for 'unavailableGetter' is unavailable}}

  x[keyPath: \.unavailableSetter] = someValue // expected-error {{setter for 'unavailableSetter' is unavailable}}
  // FIXME: spurious unavailable setter error
  x[keyPath: \.unavailableSetter.a] = someValue.a // expected-error {{setter for 'unavailableSetter' is unavailable}}
  // FIXME: spurious unavailable setter error
  x[keyPath: \.unavailableSetter[0]] = someValue.a // expected-error {{setter for 'unavailableSetter' is unavailable}}
  // FIXME: spurious unavailable setter error
  x[keyPath: \.unavailableSetter[0].b] = 1 // expected-error {{setter for 'unavailableSetter' is unavailable}}
  a[keyPath: \.[takesInOut(&x.unavailableSetter.a.b)]] = 0
  a[keyPath: \.[takesInOut(&x.unavailableSetter[0].b)]] = 0

  x[keyPath: \.unavailableGetterAndSetter] = someValue // expected-error {{setter for 'unavailableGetterAndSetter' is unavailable}}
  x[keyPath: \.unavailableGetterAndSetter.a] = someValue.a // expected-error {{setter for 'unavailableGetterAndSetter' is unavailable}}
  x[keyPath: \.unavailableGetterAndSetter[0]] = someValue.a // expected-error {{setter for 'unavailableGetterAndSetter' is unavailable}}
  // FIXME: spurious unavailable setter error
  x[keyPath: \.unavailableGetterAndSetter[0].b] = 1 // expected-error {{setter for 'unavailableGetterAndSetter' is unavailable}}
  a[keyPath: \.[takesInOut(&x.unavailableGetterAndSetter.a.b)]] = 0 // expected-error {{getter for 'unavailableGetterAndSetter' is unavailable}}
  a[keyPath: \.[takesInOut(&x.unavailableGetterAndSetter[0].b)]] = 0 // expected-error {{getter for 'unavailableGetterAndSetter' is unavailable}}
}

func testMutatingStructMember() {
  var x = BaseStruct<StructValue>()
  let a = [0]

  x.available.setToZero()
  x.available[0].setToZero()
  _ = a[x.available.setToZero()]
  _ = a[x.available.a.setToZero()]
  _ = a[x.available[0].setToZero()]

  x.unavailableGetter.setToZero() // expected-error {{getter for 'unavailableGetter' is unavailable}}
  x.unavailableGetter[0].setToZero() // expected-error {{getter for 'unavailableGetter' is unavailable}}
  _ = a[x.unavailableGetter.setToZero()] // expected-error {{getter for 'unavailableGetter' is unavailable}}
  _ = a[x.unavailableGetter.a.setToZero()] // expected-error {{getter for 'unavailableGetter' is unavailable}}
  _ = a[x.unavailableGetter[0].setToZero()] // expected-error {{getter for 'unavailableGetter' is unavailable}}

  x.unavailableSetter.setToZero() // expected-error {{setter for 'unavailableSetter' is unavailable}}
  x.unavailableSetter[0].setToZero() // expected-error {{setter for 'unavailableSetter' is unavailable}}
  _ = a[x.unavailableSetter.setToZero()] // expected-error {{setter for 'unavailableSetter' is unavailable}}
  _ = a[x.unavailableSetter.a.setToZero()] // expected-error {{setter for 'unavailableSetter' is unavailable}}
  _ = a[x.unavailableSetter[0].setToZero()] // expected-error {{setter for 'unavailableSetter' is unavailable}}

  x.unavailableGetterAndSetter.setToZero() // expected-error {{getter for 'unavailableGetterAndSetter' is unavailable}} expected-error {{setter for 'unavailableGetterAndSetter' is unavailable}}
  x.unavailableGetterAndSetter[0].setToZero() // expected-error {{getter for 'unavailableGetterAndSetter' is unavailable}} expected-error {{setter for 'unavailableGetterAndSetter' is unavailable}}
  _ = a[x.unavailableGetterAndSetter.setToZero()] // expected-error {{getter for 'unavailableGetterAndSetter' is unavailable}} expected-error {{setter for 'unavailableGetterAndSetter' is unavailable}}
  _ = a[x.unavailableGetterAndSetter.a.setToZero()] // expected-error {{getter for 'unavailableGetterAndSetter' is unavailable}} expected-error {{setter for 'unavailableGetterAndSetter' is unavailable}}
  _ = a[x.unavailableGetterAndSetter[0].setToZero()] // expected-error {{getter for 'unavailableGetterAndSetter' is unavailable}} expected-error {{setter for 'unavailableGetterAndSetter' is unavailable}}
}

func testIgnoredApplyOfFuncWithInOutParam_Struct() {
  var x = BaseStruct<StructValue>()

  takesInOut(&x.available)
  takesInOut(&x.available.a)
  takesInOut(&x.available[0])
  takesInOut(&x.available[0].b)

  takesInOut(&x.unavailableGetter) // expected-error {{getter for 'unavailableGetter' is unavailable}}
  takesInOut(&x.unavailableGetter.a) // expected-error {{getter for 'unavailableGetter' is unavailable}}
  takesInOut(&x.unavailableGetter[0]) // expected-error {{getter for 'unavailableGetter' is unavailable}}
  takesInOut(&x.unavailableGetter[0].b) // expected-error {{getter for 'unavailableGetter' is unavailable}}

  takesInOut(&x.unavailableSetter) // expected-error {{setter for 'unavailableSetter' is unavailable}}
  takesInOut(&x.unavailableSetter.a) // expected-error {{setter for 'unavailableSetter' is unavailable}}
  takesInOut(&x.unavailableSetter[0]) // expected-error {{setter for 'unavailableSetter' is unavailable}}
  takesInOut(&x.unavailableSetter[0].b) // expected-error {{setter for 'unavailableSetter' is unavailable}}

  takesInOut(&x.unavailableGetterAndSetter) // expected-error {{getter for 'unavailableGetterAndSetter' is unavailable}} expected-error {{setter for 'unavailableGetterAndSetter' is unavailable}}
  takesInOut(&x.unavailableGetterAndSetter.a) // expected-error {{getter for 'unavailableGetterAndSetter' is unavailable}} expected-error {{setter for 'unavailableGetterAndSetter' is unavailable}}
  takesInOut(&x.unavailableGetterAndSetter[0]) // expected-error {{getter for 'unavailableGetterAndSetter' is unavailable}} expected-error {{setter for 'unavailableGetterAndSetter' is unavailable}}
  takesInOut(&x.unavailableGetterAndSetter[0].b) // expected-error {{getter for 'unavailableGetterAndSetter' is unavailable}} expected-error {{setter for 'unavailableGetterAndSetter' is unavailable}}
}

func testIgnoredApplyOfFuncWithInOutParam_Class() {
  var x = BaseStruct<ClassValue>()

  takesInOut(&x.available)
  takesInOut(&x.available.a)
  takesInOut(&x.available[0])
  takesInOut(&x.available[0].b)

  takesInOut(&x.unavailableGetter) // expected-error {{getter for 'unavailableGetter' is unavailable}}
  takesInOut(&x.unavailableGetter.a) // expected-error {{getter for 'unavailableGetter' is unavailable}}
  takesInOut(&x.unavailableGetter[0]) // expected-error {{getter for 'unavailableGetter' is unavailable}}
  takesInOut(&x.unavailableGetter[0].b) // expected-error {{getter for 'unavailableGetter' is unavailable}}

  takesInOut(&x.unavailableSetter) // expected-error {{setter for 'unavailableSetter' is unavailable}}
  takesInOut(&x.unavailableSetter.a)
  takesInOut(&x.unavailableSetter[0])
  takesInOut(&x.unavailableSetter[0].b)

  takesInOut(&x.unavailableGetterAndSetter) // expected-error {{getter for 'unavailableGetterAndSetter' is unavailable}} expected-error {{setter for 'unavailableGetterAndSetter' is unavailable}}
  takesInOut(&x.unavailableGetterAndSetter.a) // expected-error {{getter for 'unavailableGetterAndSetter' is unavailable}}
  takesInOut(&x.unavailableGetterAndSetter[0]) // expected-error {{getter for 'unavailableGetterAndSetter' is unavailable}}
  takesInOut(&x.unavailableGetterAndSetter[0].b) // expected-error {{getter for 'unavailableGetterAndSetter' is unavailable}}
}

func testDiscardedApplyOfFuncWithInOutParam_Struct() {
  var x = BaseStruct<StructValue>()

  _ = takesInOut(&x.available)
  _ = takesInOut(&x.available).a
  _ = takesInOut(&x.available.a)
  _ = takesInOut(&x.available.a).b
  _ = takesInOut(&x.available[0])
  _ = takesInOut(&x.available[0]).b
  _ = takesInOut(&x.available[0].b)
  _ = takesInOut(&x.available[0].b).magnitude

  _ = takesInOut(&x.unavailableGetter) // expected-error {{getter for 'unavailableGetter' is unavailable}}
  _ = takesInOut(&x.unavailableGetter).a // expected-error {{getter for 'unavailableGetter' is unavailable}}
  _ = takesInOut(&x.unavailableGetter.a) // expected-error {{getter for 'unavailableGetter' is unavailable}}
  _ = takesInOut(&x.unavailableGetter.a).b // expected-error {{getter for 'unavailableGetter' is unavailable}}
  _ = takesInOut(&x.unavailableGetter[0]) // expected-error {{getter for 'unavailableGetter' is unavailable}}
  _ = takesInOut(&x.unavailableGetter[0]).b // expected-error {{getter for 'unavailableGetter' is unavailable}}
  _ = takesInOut(&x.unavailableGetter[0].b) // expected-error {{getter for 'unavailableGetter' is unavailable}}
  _ = takesInOut(&x.unavailableGetter[0].b).magnitude // expected-error {{getter for 'unavailableGetter' is unavailable}}

  _ = takesInOut(&x.unavailableSetter) // expected-error {{setter for 'unavailableSetter' is unavailable}}
  _ = takesInOut(&x.unavailableSetter).a // expected-error {{setter for 'unavailableSetter' is unavailable}}
  _ = takesInOut(&x.unavailableSetter.a) // expected-error {{setter for 'unavailableSetter' is unavailable}}
  _ = takesInOut(&x.unavailableSetter.a).b // expected-error {{setter for 'unavailableSetter' is unavailable}}
  _ = takesInOut(&x.unavailableSetter[0]) // expected-error {{setter for 'unavailableSetter' is unavailable}}
  _ = takesInOut(&x.unavailableSetter[0]).b // expected-error {{setter for 'unavailableSetter' is unavailable}}
  _ = takesInOut(&x.unavailableSetter[0].b) // expected-error {{setter for 'unavailableSetter' is unavailable}}
  _ = takesInOut(&x.unavailableSetter[0].b).magnitude // expected-error {{setter for 'unavailableSetter' is unavailable}}

  _ = takesInOut(&x.unavailableGetterAndSetter) // expected-error {{getter for 'unavailableGetterAndSetter' is unavailable}} expected-error {{setter for 'unavailableGetterAndSetter' is unavailable}}
  _ = takesInOut(&x.unavailableGetterAndSetter).a // expected-error {{getter for 'unavailableGetterAndSetter' is unavailable}} expected-error {{setter for 'unavailableGetterAndSetter' is unavailable}}
  _ = takesInOut(&x.unavailableGetterAndSetter.a) // expected-error {{getter for 'unavailableGetterAndSetter' is unavailable}} expected-error {{setter for 'unavailableGetterAndSetter' is unavailable}}
  _ = takesInOut(&x.unavailableGetterAndSetter.a).b // expected-error {{getter for 'unavailableGetterAndSetter' is unavailable}} expected-error {{setter for 'unavailableGetterAndSetter' is unavailable}}
  _ = takesInOut(&x.unavailableGetterAndSetter[0]) // expected-error {{getter for 'unavailableGetterAndSetter' is unavailable}} expected-error {{setter for 'unavailableGetterAndSetter' is unavailable}}
  _ = takesInOut(&x.unavailableGetterAndSetter[0]).b // expected-error {{getter for 'unavailableGetterAndSetter' is unavailable}} expected-error {{setter for 'unavailableGetterAndSetter' is unavailable}}
  _ = takesInOut(&x.unavailableGetterAndSetter[0].b) // expected-error {{getter for 'unavailableGetterAndSetter' is unavailable}} expected-error {{setter for 'unavailableGetterAndSetter' is unavailable}}
  _ = takesInOut(&x.unavailableGetterAndSetter[0].b).magnitude // expected-error {{getter for 'unavailableGetterAndSetter' is unavailable}} expected-error {{setter for 'unavailableGetterAndSetter' is unavailable}}
}

func testDiscardedApplyOfFuncWithInOutParam_Class() {
  var x = BaseStruct<ClassValue>()

  _ = takesInOut(&x.available)
  _ = takesInOut(&x.available).a
  _ = takesInOut(&x.available.a)
  _ = takesInOut(&x.available.a).b
  _ = takesInOut(&x.available[0])
  _ = takesInOut(&x.available[0]).b
  _ = takesInOut(&x.available[0].b)
  _ = takesInOut(&x.available[0].b).magnitude

  _ = takesInOut(&x.unavailableGetter) // expected-error {{getter for 'unavailableGetter' is unavailable}}
  _ = takesInOut(&x.unavailableGetter).a // expected-error {{getter for 'unavailableGetter' is unavailable}}
  _ = takesInOut(&x.unavailableGetter.a) // expected-error {{getter for 'unavailableGetter' is unavailable}}
  _ = takesInOut(&x.unavailableGetter.a).b // expected-error {{getter for 'unavailableGetter' is unavailable}}
  _ = takesInOut(&x.unavailableGetter[0]) // expected-error {{getter for 'unavailableGetter' is unavailable}}
  _ = takesInOut(&x.unavailableGetter[0]).b // expected-error {{getter for 'unavailableGetter' is unavailable}}
  _ = takesInOut(&x.unavailableGetter[0].b) // expected-error {{getter for 'unavailableGetter' is unavailable}}
  _ = takesInOut(&x.unavailableGetter[0].b).magnitude // expected-error {{getter for 'unavailableGetter' is unavailable}}

  _ = takesInOut(&x.unavailableSetter) // expected-error {{setter for 'unavailableSetter' is unavailable}}
  _ = takesInOut(&x.unavailableSetter).a // expected-error {{setter for 'unavailableSetter' is unavailable}}
  _ = takesInOut(&x.unavailableSetter.a)
  _ = takesInOut(&x.unavailableSetter.a).b
  _ = takesInOut(&x.unavailableSetter[0])
  _ = takesInOut(&x.unavailableSetter[0]).b
  _ = takesInOut(&x.unavailableSetter[0].b)
  _ = takesInOut(&x.unavailableSetter[0].b).magnitude

  _ = takesInOut(&x.unavailableGetterAndSetter) // expected-error {{getter for 'unavailableGetterAndSetter' is unavailable}} expected-error {{setter for 'unavailableGetterAndSetter' is unavailable}}
  _ = takesInOut(&x.unavailableGetterAndSetter).a // expected-error {{getter for 'unavailableGetterAndSetter' is unavailable}} expected-error {{setter for 'unavailableGetterAndSetter' is unavailable}}
  _ = takesInOut(&x.unavailableGetterAndSetter.a) // expected-error {{getter for 'unavailableGetterAndSetter' is unavailable}}
  _ = takesInOut(&x.unavailableGetterAndSetter.a).b // expected-error {{getter for 'unavailableGetterAndSetter' is unavailable}}
  _ = takesInOut(&x.unavailableGetterAndSetter[0]) // expected-error {{getter for 'unavailableGetterAndSetter' is unavailable}}
  _ = takesInOut(&x.unavailableGetterAndSetter[0]).b // expected-error {{getter for 'unavailableGetterAndSetter' is unavailable}}
  _ = takesInOut(&x.unavailableGetterAndSetter[0].b) // expected-error {{getter for 'unavailableGetterAndSetter' is unavailable}}
  _ = takesInOut(&x.unavailableGetterAndSetter[0].b).magnitude // expected-error {{getter for 'unavailableGetterAndSetter' is unavailable}}
}


var global = BaseStruct<StructValue>()

struct TestPatternBindingInitExprs {
  var available = global.available
  var available_a = global.available.a
  var available_0 = global.available[0]
  var available_0_b = global.available[0].b

  var unavailableGetter = global.unavailableGetter // expected-error {{getter for 'unavailableGetter' is unavailable}}
  var unavailableGetter_a = global.unavailableGetter.a // expected-error {{getter for 'unavailableGetter' is unavailable}}
  var unavailableGetter_0 = global.unavailableGetter[0] // expected-error {{getter for 'unavailableGetter' is unavailable}}
  var unavailableGetter_0_b = global.unavailableGetter[0].b // expected-error {{getter for 'unavailableGetter' is unavailable}}

  var unavailableSetter = global.unavailableSetter
  var unavailableSetter_a = global.unavailableSetter.a
  var unavailableSetter_0 = global.unavailableSetter[0]
  var unavailableSetter_0_b = global.unavailableSetter[0].b

  var unavailableGetterAndSetter = global.unavailableGetterAndSetter // expected-error {{getter for 'unavailableGetterAndSetter' is unavailable}}
  var unavailableGetterAndSetter_a = global.unavailableGetterAndSetter.a // expected-error {{getter for 'unavailableGetterAndSetter' is unavailable}}
  var unavailableGetterAndSetter_0 = global.unavailableGetterAndSetter[0] // expected-error {{getter for 'unavailableGetterAndSetter' is unavailable}}
  var unavailableGetterAndSetter_0_b = global.unavailableGetterAndSetter[0].b // expected-error {{getter for 'unavailableGetterAndSetter' is unavailable}}
}
