// RUN: %target-typecheck-verify-swift -swift-version 5

struct S0<T> {
  func foo(_ other: Self) { }
}

class C0<T> {
  func foo(_ other: Self) { } // expected-error{{'Self' is only available in a protocol or as the result of a method in a class; did you mean 'C0'?}}{{21-25=C0}}
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
  let b: Int
  required init(a: Int) {
    print("\(Self.self).\(#function)")
    Self.y()
    b = a
  }
  static func z(n: Self? = nil) {
    // expected-error@-1 {{'Self' is only available in a protocol or as the result of a method in a class; did you mean 'A'?}}
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
  func g() {
    _ = Self.init() as? Self
    // expected-warning@-1 {{conditional cast from 'Self' to 'Self' always succeeds}}
  }
}

struct S2 {
  let x = 99
  struct S3<T> {
    let x = 99
    static func x() {
      Self.y()
    }
    static func y() {
      print("HERE")
    }
    func foo(a: [Self]) -> Self? {
      Self.x()
      return Self.init() as? Self
      // expected-warning@-1 {{conditional cast from 'S2.S3<T>' to 'S2.S3<T>' always succeeds}}
    }
  }
}

extension S2 {
  static func x() {
    Self.y()
  }
  static func y() {
    print("HERE")
  }
  func foo(a: [Self]) -> Self? {
    Self.x()
    return Self.init() as? Self
    // expected-warning@-1 {{conditional cast from 'S2' to 'S2' always succeeds}}
  }
}

enum E {
  static func f() {
    print("f()")
  }
  case e
  func h(h: Self) -> Self {
    Self.f()
    return .e
  }
}
