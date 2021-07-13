// RUN: %target-typecheck-verify-swift -swift-version 5

struct S0<T> {
  func foo(_ other: Self) { }
}

class C0<T> {
  func foo(_ other: Self) { } // expected-error{{covariant 'Self' or 'Self?' can only appear as the type of a property, subscript or method result; did you mean 'C0'?}}
}

enum E0<T> {
  func foo(_ other: Self) { }
}

// rdar://problem/21745221
struct X {
  typealias T = Int
}

extension X {
  struct Inner {
  }
}

extension X.Inner {
  func foo(_ other: Self) { }
}

// SR-695
class Mario {
  func getFriend() -> Self { return self } // expected-note{{overridden declaration is here}}
  func getEnemy() -> Mario { return self }
}
class SuperMario : Mario {
  override func getFriend() -> SuperMario { // expected-error{{cannot override a Self return type with a non-Self return type}}
    return SuperMario()
  }
  override func getEnemy() -> Self { return self }
}
final class FinalMario : Mario {
    override func getFriend() -> FinalMario {
        return FinalMario()
    }
}

// These references to Self are now possible (SE-0068)

class A<T> {
  typealias _Self = Self
  // expected-error@-1 {{covariant 'Self' or 'Self?' can only appear as the type of a property, subscript or method result; did you mean 'A'?}}
  let b: Int
  required init(a: Int) {
    print("\(Self.self).\(#function)")
    Self.y()
    b = a
  }
  static func z(n: Self? = nil) {
    // expected-error@-1 {{covariant 'Self' or 'Self?' can only appear as the type of a property, subscript or method result; did you mean 'A'?}}
    print("\(Self.self).\(#function)")
  }
  class func y() {
    print("\(Self.self).\(#function)")
    Self.z()
  }
  func x() -> A? {
    print("\(Self.self).\(#function)")
    Self.y()
    Self.z()
    let _: Self = Self.init(a: 66)
    return Self.init(a: 77) as? Self as? A
    // expected-warning@-1 {{conditional cast from 'Self' to 'Self' always succeeds}}
    // expected-warning@-2 {{conditional downcast from 'Self?' to 'A<T>' is equivalent to an implicit conversion to an optional 'A<T>'}}
  }
  func copy() -> Self {
    let copy = Self.init(a: 11)
    return copy
  }

  subscript (i: Int) -> Self { // expected-error {{mutable subscript cannot have covariant 'Self' type}}
    get {
      return Self.init(a: i)
    }
    set(newValue) {
    }
  }
}

class B: A<Int> {
  let a: Int
  required convenience init(a: Int) {
    print("\(Self.self).\(#function)")
    self.init()
  }
  init() {
    print("\(Self.self).\(#function)")
    Self.y()
    Self.z()
    a = 99
    super.init(a: 88)
  }
  override class func y() {
    print("override \(Self.self).\(#function)")
  }
}

class C {
  required init() {
  }

  func f() {
    func g(_: Self) {}
    let x: Self = self as! Self
    g(x)
    typealias _Self = Self
  }
  func g() {
    _ = Self.init() as? Self
    // expected-warning@-1 {{conditional cast from 'Self' to 'Self' always succeeds}}
  }
  func h(j: () -> Self) -> () -> Self {
    // expected-error@-1 {{covariant 'Self' or 'Self?' can only appear at the top level of method result type}}
    return { return self }
  }
  func i() -> (Self, Self) {}
  // expected-error@-1 {{covariant 'Self' or 'Self?' can only appear at the top level of method result type}}

  func j() -> Self.Type {}
  // expected-error@-1 {{covariant 'Self' or 'Self?' can only appear at the top level of method result type}}

  let p0: Self? // expected-error {{stored property cannot have covariant 'Self' type}}
  var p1: Self? // expected-error {{stored property cannot have covariant 'Self' type}}

  static func staticFunc() -> Self {}
  let stored: Self = Self.staticFunc() // expected-error {{stored property cannot have covariant 'Self' type}}
  // expected-error@-1 {{covariant 'Self' type cannot be referenced from a stored property initializer}}

  var prop: Self { // expected-error {{mutable property cannot have covariant 'Self' type}}
    get {
      return self
    }
    set (newValue) {
    }
  }
  subscript (i: Int) -> Self { // expected-error {{mutable subscript cannot have covariant 'Self' type}}
    get {
      return self
    }
    set (newValue) {
    }
  }
}

extension C {
  static var rdar57188331 = Self.staticFunc() // expected-error {{covariant 'Self' type cannot be referenced from a stored property initializer}} expected-error {{stored property cannot have covariant 'Self' type}}
  static var rdar57188331Var = ""
  static let rdar57188331Ref = UnsafeRawPointer(&Self.rdar57188331Var) // expected-error {{covariant 'Self' type cannot be referenced from a stored property initializer}}
}


struct S1 {
  typealias _SELF = Self
  let j = 99.1
  subscript (i: Int) -> Self {
    get {
      return self
    }
    set(newValue) {
    }
  }
  var foo: Self {
    get {
      return self// as! Self
    }
    set (newValue) {
    }
  }
  func x(y: () -> Self, z: Self) {
  }
}

struct S2 {
  let x = 99
  struct S3<T> {
    let x = 99
    static func x() {
      Self.y()
    }
    func f() {
      func g(_: Self) {}
    }
    static func y() {
      print("HERE")
    }
    func foo(a: [Self]) -> Self? {
      Self.x()
      return self as? Self
      // expected-warning@-1 {{conditional cast from 'S2.S3<T>' to 'S2.S3<T>' always succeeds}}
    }
  }
  func copy() -> Self {
    let copy = Self.init()
    return copy
  }

  var copied: Self {
    let copy = Self.init()
    return copy
  }
}

extension S2 {
  static func x() {
    Self.y()
  }
  static func y() {
    print("HERE")
  }
  func f() {
    func g(_: Self) {}
  }
  func foo(a: [Self]) -> Self? {
    Self.x()
    return Self.init() as? Self
    // expected-warning@-1 {{conditional cast from 'S2' to 'S2' always succeeds}}
  }
  subscript (i: Int) -> Self {
    get {
      return Self.init()
    }
    set(newValue) {
    }
  }
}

enum E {
  static func f() {
    func g(_: Self) {}
    print("f()")
  }
  case e
  func h(h: Self) -> Self {
    Self.f()
    return .e
  }
}

class SelfStoredPropertyInit {
  static func myValue() -> Int { return 123 }

  var value = Self.myValue() // expected-error {{covariant 'Self' type cannot be referenced from a stored property initializer}}
}

// rdar://problem/55273931 - erroneously rejecting 'Self' in lazy initializer
class Foo {
  static var value: Int = 17

  lazy var doubledValue: Int = {
    Self.value * 2
  }()
}

// https://bugs.swift.org/browse/SR-11681 - duplicate diagnostics
struct Box<T> {
  let boxed: T
}

class Boxer {
  lazy var s = Box<Self>(boxed: self as! Self)
  // expected-error@-1 {{stored property cannot have covariant 'Self' type}}
  // expected-error@-2 {{mutable property cannot have covariant 'Self' type}}

  var t = Box<Self>(boxed: Self())
  // expected-error@-1 {{stored property cannot have covariant 'Self' type}}
  // expected-error@-2 {{covariant 'Self' type cannot be referenced from a stored property initializer}}

  required init() {}
}

// https://bugs.swift.org/browse/SR-12133 - a type named 'Self' should be found first
struct OuterType {
  struct `Self` {
    let string: String
  }

  var foo: `Self`? {
    let optional: String? = "foo"
    return optional.map { `Self`(string: $0) }
  }
}

// rdar://69804933 - CSApply assigns wrong type to MemberRefExpr for property with
// DynamicSelfType
class HasDynamicSelfProperty {
  var me: Self {
    return self
  }
}

// SILGen doesn't care about the MemberRefExpr's type, so it's hard to come up with an
// example that actually fails. Here, the rogue 'Self' type was diagnosed as being invalid
// in a stored property initializer.
class UsesDynamicSelfProperty {
  var c = HasDynamicSelfProperty().me
}

// Test that dynamic 'Self' gets substituted with the object type of the
// associated 'self' parameter in 'super'-based invocations.
do {
  class A {
    required init() {}

    func method() -> Self { self }
    var property: Self { self }
    subscript() -> Self { self }

    class func method() -> Self { self.init() }
    class var property: Self { self.init() }
    class subscript() -> Self { self.init() }
  }

  class B: A {
    override func method() -> Self { super.method() }
    override var property: Self { super.property }
    override subscript() -> Self { super[] }

    override class func method() -> Self {
      // Constructors must always have dynamic 'Self' replaced with the base
      // object type.
      // FIXME: Statically dispatches to the superclass init, but constructs an
      // object of the 'Self' type.
      let _: Self = super.init() // expected-error {{cannot convert value of type 'A' to specified type 'Self'}}
      return super.method()
    }
    override class var property: Self { super.property }
    override class subscript() -> Self { super[] }
  }

  class C: B {}

  class D: C {
    func testWithStaticSelf() {
      let _: Self = super.method() // expected-error {{cannot convert value of type 'D' to specified type 'Self'}}
      let _: Self = super.property // expected-error {{cannot convert value of type 'D' to specified type 'Self'}}
      let _: Self = super[] // expected-error {{cannot convert value of type 'D' to specified type 'Self'}}
      let _: () -> Self = super.method // expected-error {{cannot convert value of type '() -> D' to specified type '() -> Self'}}
    }

    static func testWithStaticSelfStatic() {
      // Constructors must always have dynamic 'Self' replaced with the base
      // object type.
      // FIXME: Statically dispatches to the superclass init, but constructs an
      // object of the 'Self' type.
      let _: Self = super.init() // expected-error {{cannot convert value of type 'C' to specified type 'Self'}}
      let _: Self = super.method() // expected-error {{cannot convert value of type 'D' to specified type 'Self'}}
      let _: Self = super.property // expected-error {{cannot convert value of type 'D' to specified type 'Self'}}
      let _: Self = super[] // expected-error {{cannot convert value of type 'D' to specified type 'Self'}}
      let _: () -> Self = super.method
      // expected-error@-1 {{partial application of 'super' instance method with metatype base is not allowed}}
      // expected-error@-2 {{cannot convert value of type '(C) -> () -> D' to specified type '() -> Self'}}
    }
  }
}

// https://bugs.swift.org/browse/SR-14731
struct Generic<T> {
  func foo() -> Self<Int> {}
  // expected-error@-1 {{cannot specialize 'Self'}}
  // expected-note@-2 {{did you mean to explicitly reference 'Generic' instead?}}{{17-21=Generic}}
}

struct NonGeneric {
  func foo() -> Self<Int> {}
  // expected-error@-1 {{cannot specialize 'Self'}}
}

protocol P {
  func foo() -> Self<Int>
  // expected-error@-1 {{cannot specialize non-generic type 'Self'}}
}

// https://bugs.swift.org/browse/SR-4559 - function called 'self' can be confused with regular 'self'
struct TypeWithSelfMethod {
    
    let property = self // expected-warning {{'self' refers to the method 'TypeWithSelfMethod.self', which may be unexpected}} expected-note{{use 'TypeWithSelfMethod.self' to silence this warning}} {{20-20=TypeWithSelfMethod.}}
    
    // Existing warning expected, not confusable
    let property2 = self() // expected-error {{cannot use instance member 'self' within property initializer; property initializers run before 'self' is available}}
    
    let propertyFromClosure: () = {
        print(self) // expected-warning {{'self' refers to the method 'TypeWithSelfMethod.self', which may be unexpected}} expected-note{{use 'TypeWithSelfMethod.self' to silence this warning}} {{15-15=TypeWithSelfMethod.}}
    }()
    
    let propertyFromFunc = f(self) // expected-warning {{'self' refers to the method 'TypeWithSelfMethod.self', which may be unexpected}} expected-note{{use 'TypeWithSelfMethod.self' to silence this warning}} {{30-30=TypeWithSelfMethod.}}
    
    func `self`() {
        
    }
    
    static func f(_ any: Any) -> Any {
        any
    }
}
