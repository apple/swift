// RUN: %target-typecheck-verify-swift -parse-as-library

lazy func lazy_func() {} // expected-error {{'lazy' may only be used on 'var' declarations}} {{1-6=}}

lazy var b = 42  // expected-error {{'lazy' cannot be used on an already-lazy global}} {{1-6=}}

struct S {
  lazy static var lazy_global = 42 // expected-error {{'lazy' cannot be used on an already-lazy global}} {{3-8=}}
}

protocol SomeProtocol {
  lazy var x : Int  // expected-error {{'lazy' cannot be used on a protocol requirement}} {{3-8=}}
  // expected-error@-1 {{property in protocol must have explicit { get } or { get set } specifier}} {{19-19= { get <#set#> \}}}
  // expected-error@-2 {{lazy properties must have an initializer}}
  lazy var y : Int { get } // expected-error {{'lazy' cannot be used on a protocol requirement}} {{3-8=}}
  // expected-error@-1 {{'lazy' cannot be used on a computed property}}
  // expected-error@-2 {{lazy properties must have an initializer}}
}


class TestClass {
  lazy var a = 42
  lazy var a1 : Int = 42

  lazy let b = 42  // expected-error {{'lazy' cannot be used on a let}} {{3-8=}}

  lazy var c : Int { return 42 } // expected-error {{'lazy' cannot be used on a computed property}} {{3-8=}}
  // expected-error@-1 {{lazy properties must have an initializer}}

  lazy var d : Int  // expected-error {{lazy properties must have an initializer}} {{3-8=}}

  lazy var (e, f) = (1,2)  // expected-error 2{{'lazy' cannot destructure an initializer}} {{3-8=}}

  lazy var g = { 0 }()   // single-expr closure

  lazy var h : Int = { 0 }()   // single-expr closure

  lazy var i = { () -> Int in return 0 }()+1  // multi-stmt closure

  lazy var j : Int = { return 0 }()+1  // multi-stmt closure

  lazy var k : Int = { () -> Int in return 0 }()+1  // multi-stmt closure

  lazy var l : Int = 42 {  // Okay
    didSet {}
    willSet {}
  }

  lazy var m : Int = 42 { // Okay
    didSet {}
  }

  lazy var n : Int = 42 {
    willSet {} // Okay
  }

  init() {
    lazy var localvar = 42 // Okay
    localvar += 1
    _ = localvar
  }
}


struct StructTest {
  lazy var p1 : Int = 42

  mutating func f1() -> Int {
    return p1
  }
  
  // expected-note @+1 {{mark method 'mutating' to make 'self' mutable}} {{3-3=mutating }}
  func f2() -> Int {
    return p1  // expected-error {{cannot use mutating getter on immutable value: 'self' is immutable}}
  }

  static func testStructInits() {
    let a = StructTest()         // default init
    let b = StructTest(p1: 42)  // Override the lazily init'd value.
    _ = a; _ = b
  }
}


// <rdar://problem/16889110> capture lists in lazy member properties cannot use self
class CaptureListInLazyProperty {
  lazy var closure1 = { [weak self] in return self!.i }
  lazy var closure2: () -> Int = { [weak self] in return self!.i }
  var i = 42
}


// Crash when initializer expression is type-checked both to infer the
// property type and also as part of the getter
class WeShouldNotReTypeCheckStatements {
  lazy var firstCase = {
    _ = nil // expected-error{{'nil' requires a contextual type}}
    _ = ()
  }

  lazy var secondCase = {
    _ = ()
    _ = ()
  }
}

protocol MyProtocol {
  func f() -> Int
}

struct MyStruct : MyProtocol {
  func f() -> Int { return 0 }
}

struct Outer {
  static let p: MyProtocol = MyStruct()

  struct Inner {
    lazy var x = p.f()

    lazy var y = {_ = 3}()
    // expected-warning@-1 {{variable 'y' inferred to have type '()', which may be unexpected}}
    // expected-note@-2 {{add an explicit type annotation to silence this warning}} {{15-15=: ()}}
  }
}

// https://github.com/apple/swift/issues/45221

struct Construction {
  init(x: Int, y: Int? = 42) { }
}

class Constructor {
  lazy var myQ = Construction(x: 3)
}


// Problems with self references
class BaseClass {
  var baseInstanceProp = 42
  static var baseStaticProp = 42
}

class ReferenceSelfInLazyProperty : BaseClass {
  lazy var refs = (i, f())
  lazy var trefs: (Int, Int) = (i, f())

  lazy var qrefs = (self.i, self.f())
  lazy var qtrefs: (Int, Int) = (self.i, self.f())

  lazy var crefs = { (i, f()) }()
  lazy var ctrefs: (Int, Int) = { (i, f()) }()

  lazy var cqrefs = { (self.i, self.f()) }()
  lazy var cqtrefs: (Int, Int) = { (self.i, self.f()) }()

  lazy var mrefs = { () -> (Int, Int) in return (i, f()) }()
  lazy var mtrefs: (Int, Int) = { return (i, f()) }()

  lazy var mqrefs = { () -> (Int, Int) in (self.i, self.f()) }()
  lazy var mqtrefs: (Int, Int) = { return (self.i, self.f()) }()

  lazy var lcqrefs = { [unowned self] in (self.i, self.f()) }()
  lazy var lcqtrefs: (Int, Int) = { [unowned self] in (self.i, self.f()) }()

  lazy var lmrefs = { [unowned self] () -> (Int, Int) in return (i, f()) }()
  lazy var lmtrefs: (Int, Int) = { [unowned self] in return (i, f()) }()

  lazy var lmqrefs = { [unowned self] () -> (Int, Int) in (self.i, self.f()) }()
  lazy var lmqtrefs: (Int, Int) = { [unowned self] in return (self.i, self.f()) }()

  var i = 42
  func f() -> Int { return 0 }

  lazy var refBaseClassProp = baseInstanceProp
}

class ReferenceStaticInLazyProperty {
  lazy var refs1 = i
  // expected-error@-1 {{static member 'i' cannot be used on instance of type 'ReferenceStaticInLazyProperty'}}
  lazy var refs2 = f()
  // expected-error@-1 {{static member 'f' cannot be used on instance of type 'ReferenceStaticInLazyProperty'}}

  lazy var trefs1: Int = i
  // expected-error@-1 {{static member 'i' cannot be used on instance of type 'ReferenceStaticInLazyProperty'}}

  lazy var trefs2: Int = f()
  // expected-error@-1 {{static member 'f' cannot be used on instance of type 'ReferenceStaticInLazyProperty'}}

  static var i = 42
  static func f() -> Int { return 0 }
}

// Explicit access to the lazy variable storage
class LazyVarContainer {
  lazy var foo: Int = {
    return 0
  }()

  func accessLazyStorage() {
    $__lazy_storage_$_foo = nil // expected-error {{access to the underlying storage of a lazy property is not allowed}}
    print($__lazy_storage_$_foo!) // expected-error {{access to the underlying storage of a lazy property is not allowed}}
    _ = $__lazy_storage_$_foo == nil // expected-error {{access to the underlying storage of a lazy property is not allowed}}
  }
}

// Make sure we can still access a synthesized variable with the same name as a lazy storage variable
// i.e. $__lazy_storage_$_{property_name} when using property wrapper where the property name is 
// '__lazy_storage_$_{property_name}'.
@propertyWrapper
struct Wrapper {
  var wrappedValue: Int { 1 }
  var projectedValue: Int { 1 }
}

struct PropertyWrapperContainer {
  @Wrapper var __lazy_storage_$_foo
  func test() {
    _ = $__lazy_storage_$_foo  // This is okay.
  }
}

// rdar://problem/129255769
struct X {
  struct Y { }

  func f() {
    _ = {
      lazy var x: [Y] = []
      _ = Y()
    }
  }
}
