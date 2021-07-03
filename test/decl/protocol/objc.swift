// RUN: %target-typecheck-verify-swift -enable-objc-interop

// Test requirements and conformance for Objective-C protocols.

@objc class ObjCClass { }

@objc protocol P1 {
  func method1()

  var property1: ObjCClass { get }
  var property2: ObjCClass { get set }
}

@objc class C1 : P1 {
  @objc(method1renamed)
  func method1() { // expected-error{{Objective-C method 'method1renamed' provided by method 'method1()' does not match the requirement's selector ('method1')}}{{9-23=method1}}
  }

  var property1: ObjCClass {
    @objc(getProperty1) get { // expected-error{{Objective-C method 'getProperty1' provided by getter for 'property1' does not match the requirement's selector ('property1')}}{{11-23=property1}}
      return ObjCClass()
    } 
  }

  var property2: ObjCClass {
    get { return ObjCClass() }
    @objc(setProperty2Please:) set { } // expected-error{{Objective-C method 'setProperty2Please:' provided by setter for 'property2' does not match the requirement's selector ('setProperty2:')}}{{11-30=setProperty2:}}
  }
}

class C1b : P1 {
  func method1() { }
  var property1: ObjCClass = ObjCClass()
  var property2: ObjCClass = ObjCClass()
}

@objc protocol P2 {
  @objc(methodWithInt:withClass:)
  func method(_: Int, class: ObjCClass)

  var empty: Bool { @objc(checkIfEmpty) get }
}

class C2a : P2 {
  func method(_: Int, class: ObjCClass) { }

  var empty: Bool {
    get { }
  }
}

class C2b : P2 {
  @objc func method(_: Int, class: ObjCClass) { }

  @objc var empty: Bool {
    @objc get { }
  }
}

@objc protocol P3 {
  @objc optional func doSomething(x: Int)
  @objc optional func doSomething(y: Int)
}

class C3a : P3 {
  @objc func doSomething(x: Int) { }
}

// Complain about optional requirements that aren't satisfied
// according to Swift, but would conflict in Objective-C.
@objc protocol OptP1 {
  @objc optional func method() // expected-note 2{{requirement 'method()' declared here}}

  @objc optional var property1: ObjCClass { get } // expected-note 2{{requirement 'property1' declared here}}
  @objc optional var property2: ObjCClass { get set } // expected-note{{requirement 'property2' declared here}}
}

@objc class OptC1a : OptP1 { // expected-note 3{{class 'OptC1a' declares conformance to protocol 'OptP1' here}}
  @objc(method) func otherMethod() { } // expected-error{{Objective-C method 'method' provided by method 'otherMethod()' conflicts with optional requirement method 'method()' in protocol 'OptP1'}}
  // expected-note@-1{{rename method to match requirement 'method()'}}{{22-33=method}}

  @objc var otherProp1: ObjCClass {
    @objc(property1) get { return ObjCClass() } // expected-error{{Objective-C method 'property1' provided by getter for 'otherProp1' conflicts with optional requirement getter for 'property1' in protocol 'OptP1'}}
  }

  @objc var otherProp2: ObjCClass {
    get { return ObjCClass() }
    @objc(setProperty2:) set { } // expected-error{{Objective-C method 'setProperty2:' provided by setter for 'otherProp2' conflicts with optional requirement setter for 'property2' in protocol 'OptP1'}}
  }
}

@objc class OptC1b : OptP1 { // expected-note 2{{class 'OptC1b' declares conformance to protocol 'OptP1' here}}
  @objc(property1) func someMethod() { } // expected-error{{Objective-C method 'property1' provided by method 'someMethod()' conflicts with optional requirement getter for 'property1' in protocol 'OptP1'}}

  @objc var someProp: ObjCClass {
    @objc(method) get { return ObjCClass() } // expected-error{{Objective-C method 'method' provided by getter for 'someProp' conflicts with optional requirement method 'method()' in protocol 'OptP1'}}
  }
}

// rdar://problem/19879598
@objc protocol Foo {
  init()
}

class Bar: Foo {
  required init() {}
}

@objc protocol P4 {
  @objc(foo:bar:)
  func method(x: Int, y: Int)
}

// Infer @objc and selector from requirement.
class C4a : P4 {
  func method(x: Int, y: Int) { }
}

// Infer selector from requirement.
class C4b : P4 {
  @objc
  func method(x: Int, y: Int) { }
}

@objc protocol P5 {
  @objc(wibble:wobble:)
  func method(x: Int, y: Int)
}

// Don't infer when there is an ambiguity.
class C4_5a : P4, P5 {
  func method(x: Int, y: Int) { }
  // expected-error@-1{{ambiguous inference of Objective-C name for instance method 'method(x:y:)' ('foo:bar:' vs 'wibble:wobble:')}}
  // expected-note@-2{{'method(x:y:)' (in protocol 'P4') provides Objective-C name 'foo:bar:'}}{{3-3=@objc(foo:bar:) }}
  // expected-note@-3{{'method(x:y:)' (in protocol 'P5') provides Objective-C name 'wibble:wobble:'}}{{3-3=@objc(wibble:wobble:) }}
  // expected-note@-4{{add '@nonobjc' to silence this error}}{{3-3=@nonobjc }}
  // expected-error@-5{{Objective-C method 'foo:bar:' provided by method 'method(x:y:)' does not match the requirement's selector ('wibble:wobble:')}}
}

// Don't complain about a selector mismatch in cases where the
// selector will be inferred.
@objc protocol P6 {
  func doSomething(_ sender: AnyObject?) // expected-note{{requirement 'doSomething' declared here}}
}

class C6a : P6 {
  func doSomething(sender: AnyObject?) { } // expected-error{{method 'doSomething(sender:)' has different argument labels from those required by protocol 'P6' ('doSomething')}}{{20-20=_ }}{{none}}
}



@objc protocol P7 {
  var prop: Int {
    @objc(getTheProp) get
    @objc(setTheProp:) set
  }
}

class C7 : P7 {
  var prop: Int {
    get { return 0 }
    set {}
  }
}

class C7a : P7 {
  @objc var prop: Int {
    get { return 0 }
    set {}
  }
}

class C7b : P7 {
  @objc var prop: Int {
    @objc(getTheProp) get { return 0 }
    @objc(setTheProp:) set {}
  }
}

class C7c : P7 {
  var prop: Int = 0
}

class C7d : P7 {
  @objc var prop: Int = 0
}

class C7e : P7 {
  // FIXME: This should probably still complain.
  @objc(notProp) var prop: Int {
    get { return 0 }
    set {}
  }
}

class C7f : P7 {
  var prop: Int {
    @objc(getProp) get { return 0 } // expected-error {{Objective-C method 'getProp' provided by getter for 'prop' does not match the requirement's selector ('getTheProp')}} {{11-18=getTheProp}}
    set {}
  }
}

class C7g : P7 {
  var prop: Int {
    get { return 0 }
    @objc(prop:) set {} // expected-error {{Objective-C method 'prop:' provided by setter for 'prop' does not match the requirement's selector ('setTheProp:')}} {{11-16=setTheProp:}}
  }
}

class C7h : P7 {
  @objc var prop: Int = 0 {
    didSet {}
  }
}

class C7i : P7 {
  @objc var prop: Int {
    unsafeAddress { fatalError() }
    unsafeMutableAddress { fatalError() }
  }
}

@objc protocol P8 {
  @objc optional var prop: Int {
    @objc(getTheProp) get
  }
}

class C8Base: P8 {}
class C8Sub: C8Base {
  var prop: Int { return 0 } // expected-note {{getter for 'prop' declared here}}

  @objc(getTheProp) func collision() {} // expected-error {{method 'collision()' with Objective-C selector 'getTheProp' conflicts with getter for 'prop' with the same Objective-C selector}}
}
class C8SubA: C8Base {
  var prop: Int {
    @objc get { return 0 } // expected-note {{getter for 'prop' declared here}}
  }

  @objc(getTheProp) func collision() {} // expected-error {{method 'collision()' with Objective-C selector 'getTheProp' conflicts with getter for 'prop' with the same Objective-C selector}}
}
class C8SubB: C8Base {
  var prop: Int {
    @objc(getProp) get { return 0 }
  }

  @objc(getTheProp) func collision() {} // okay
}

// These used to crash because the requirement is get-only.
class C8SubRW: C8Base {
  var prop: Int {
    get { return 0 }
    set {}
  }
}

class C8SubRW2: C8Base {
  var prop: Int = 0
}