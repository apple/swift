// RUN: %target-typecheck-verify-swift -parse-as-library -swift-version 4 -enable-objc-interop

@objc class ObjCClassA {}
@objc class ObjCClassB : ObjCClassA {}

class A { 
  func f1() { } // expected-note{{overri}}
  func f2() -> A { } // expected-note{{overri}}

  @objc func f3() { } // expected-note{{overri}}
  @objc func f4() -> ObjCClassA { } // expected-note{{overri}}
  @objc var v1: Int { return 0 } // expected-note{{overri}}
  @objc var v2: Int { return 0 } // expected-note{{overri}}
  @objc var v3: Int = 0 // expected-note{{overri}}

  @objc dynamic func f3D() { }
  @objc dynamic func f4D() -> ObjCClassA { }
}

extension A {
  func f5() { } // expected-note{{overri}}
  func f6() -> A { } // expected-note{{overri}}

  @objc func f7() { }
  @objc func f8() -> ObjCClassA { }
}

class B : A { }

extension B { 
  func f1() { }  // expected-error{{overri}}
  func f2() -> B { } // expected-error{{overri}}

  override func f3() { } // expected-error{{cannot override a non-dynamic class declaration from an extension}}
  override func f4() -> ObjCClassB { } // expected-error{{cannot override a non-dynamic class declaration from an extension}}
  override var v1: Int { return 1 } // expected-error{{cannot override a non-dynamic class declaration from an extension}}
  override var v2: Int { // expected-error{{cannot override a non-dynamic class declaration from an extension}}
    get { return 1 }
    set { }
  }
  override var v3: Int { // expected-error{{cannot override a non-dynamic class declaration from an extension}}
    willSet { }
    didSet { }
  }

  override func f3D() { }
  override func f4D() -> ObjCClassB { }

  func f5() { }  // expected-error{{overri}}
  func f6() -> A { }  // expected-error{{instance method 'f6()' is declared in extension of 'A' and cannot be overridden}}

  @objc override func f7() { }
  @objc override func f8() -> ObjCClassA { }
}

func callOverridden(_ b: B) {
  b.f3()
  _ = b.f4()
  b.f7()
  _ = b.f8()
}

@objc
class Base {
  func meth(_ x: Undeclared) {} // expected-error {{cannot find type 'Undeclared' in scope}}
}
@objc
class Sub : Base {
  func meth(_ x: Undeclared) {} // expected-error {{cannot find type 'Undeclared' in scope}}
}

// Objective-C method overriding

@objc class ObjCSuper {
  func method(_ x: Int, withInt y: Int) { }

  func method2(_ x: Sub, withInt y: Int) { }

  @objc func method3(_ x: Base, withInt y: Int) { } // expected-note{{method 'method3(_:withInt:)' declared here}}
}

class ObjCSub : ObjCSuper {
  override func method(_ x: Int, withInt y: Int) { } // okay, overrides exactly

  override func method2(_ x: Base, withInt y: Int) { } // okay, overrides trivially

  @objc(method3:withInt:) func method3(_ x: Sub, with y: Int) { } // expected-error{{method3(_:with:)' with Objective-C selector 'method3:withInt:' conflicts with method 'method3(_:withInt:)' from superclass 'ObjCSuper' with the same Objective-C selector}}
}

// Context: https://github.com/apple/swift/issues/57499
// FixIt for 'override func' that should be 'override var' and vice-versa
func overrideInLieu() {
  
  class BaseClass {
    var someValueX: Bool {
      get { return false }
      set { }
    }
    
    func someFuncX() -> Int { return 0 }
  }

  class Inherited: BaseClass {
    override var someFuncX: Int { // expected-error {{property does not override any property from its superclass}}
    // expected-note@-1 {{did you mean to override the method 'someFuncX'?}}
      get { return 0 }
      set { }
    }
    
    override func someValueX() -> Bool { return false } // expected-error {{method does not override any method from its superclass}}
    // expected-note@-1 {{did you mean to override the property 'someValueX'?}}
  }
}
