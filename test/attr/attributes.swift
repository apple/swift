// RUN: %target-typecheck-verify-swift -enable-objc-interop

@unknown func f0() {} // expected-error{{unknown attribute 'unknown'}}
@unknown(x,y) func f1() {} // expected-error{{unknown attribute 'unknown'}}

enum binary { 
  case Zero
  case One
  init() { self = .Zero }
}

func f5(x: inout binary) {}

//===---
//===--- IB attributes
//===---

@IBDesignable
class IBDesignableClassTy {
  @IBDesignable func foo() {} // expected-error {{'@IBDesignable' attribute cannot be applied to this declaration}} {{3-17=}}
}

@IBDesignable // expected-error {{'@IBDesignable' attribute cannot be applied to this declaration}} {{1-15=}}
struct IBDesignableStructTy {}

@IBDesignable // expected-error {{'@IBDesignable' attribute cannot be applied to this declaration}} {{1-15=}}
protocol IBDesignableProtTy {}

@IBDesignable // expected-error {{@IBDesignable can only be applied to classes and extensions of classes}} {{1-15=}}
extension IBDesignableStructTy {}

class IBDesignableClassExtensionTy {}
@IBDesignable // okay
extension IBDesignableClassExtensionTy {}

class Inspect {
  @IBInspectable var value : Int = 0 // okay
  @GKInspectable var value2: Int = 0 // okay

  @IBInspectable func foo() {} // expected-error {{@IBInspectable may only be used on 'var' declarations}} {{3-18=}}
  @GKInspectable func foo2() {} // expected-error {{@GKInspectable may only be used on 'var' declarations}} {{3-18=}}

  @IBInspectable class var cval: Int { return 0 } // expected-error {{only class instance properties can be declared @IBInspectable}} {{3-18=}}
  @GKInspectable class var cval2: Int { return 0 } // expected-error {{only class instance properties can be declared @GKInspectable}} {{3-18=}}
}
@IBInspectable var ibinspectable_global : Int // expected-error {{only class instance properties can be declared @IBInspectable}} {{1-16=}}
@GKInspectable var gkinspectable_global : Int // expected-error {{only class instance properties can be declared @GKInspectable}} {{1-16=}}

struct inspectableWithStruct {
  @IBInspectable var IBInspectableInStruct: Int // expected-error {{only class instance properties can be declared @IBInspectable}} {{3-18=}}
  @GKInspectable var GKInspectableInStruct: Int // expected-error {{only class instance properties can be declared @GKInspectable}} {{3-18=}}
}

func foo(x: @convention(block) Int) {} // expected-error {{@convention attribute only applies to function types}}
func foo(x: @convention(block) (Int) -> Int) {}

@_transparent
func zim() {}
@_transparent
func zung<T>(_: T) {}
@_transparent // expected-error{{'@_transparent' attribute cannot be applied to stored properties}} {{1-15=}}
var zippity : Int
func zoom(x: @_transparent () -> ()) { } // expected-error{{attribute can only be applied to declarations, not types}} {{1-1=@_transparent }} {{14-28=}}
protocol ProtoWithTransparent {
  @_transparent// expected-error{{'@_transparent' attribute is not supported on declarations within protocols}} {{3-16=}}
  func transInProto()
}
class TestTranspClass : ProtoWithTransparent {
  @_transparent  // expected-error{{'@_transparent' attribute is not supported on declarations within classes}} {{3-17=}}
  init () {}
  @_transparent // expected-error{{'@_transparent' attribute cannot be applied to this declaration}} {{3-17=}}
  deinit {}
  @_transparent // expected-error{{'@_transparent' attribute is not supported on declarations within classes}} {{3-17=}}
  class func transStatic() {}
  @_transparent// expected-error{{'@_transparent' attribute is not supported on declarations within classes}} {{3-16=}}
  func transInProto() {}
}
struct TestTranspStruct : ProtoWithTransparent{
  @_transparent
  init () {}
  @_transparent
  init <T> (x : T) { }
  @_transparent
  static func transStatic() {}
  @_transparent
  func transInProto() {}
}
@_transparent // expected-error{{'@_transparent' attribute cannot be applied to this declaration}} {{1-15=}}
struct CannotHaveTransparentStruct {
  func m1() {}
}
@_transparent // expected-error{{'@_transparent' attribute cannot be applied to this declaration}} {{1-15=}}
extension TestTranspClass {
  func tr1() {}
}
@_transparent // expected-error{{'@_transparent' attribute cannot be applied to this declaration}} {{1-15=}}
extension TestTranspStruct {
  func tr1() {}
}
@_transparent // expected-error{{'@_transparent' attribute cannot be applied to this declaration}} {{1-15=}}
extension binary {
  func tr1() {}
}

class transparentOnClassVar {
  @_transparent var max: Int { return 0xFF }; // expected-error {{'@_transparent' attribute is not supported on declarations within classes}} {{3-17=}}
  func blah () {
    var _: Int = max
  }
};

class transparentOnClassVar2 {
  var max: Int {
    @_transparent // expected-error {{'@_transparent' attribute is not supported on declarations within classes}} {{5-19=}}
    get {
      return 0xFF
    }
  }
  func blah () {
    var _: Int = max
  }
};

@thin  // expected-error {{attribute can only be applied to types, not declarations}}
func testThinDecl() -> () {}

protocol Class : class {}
protocol NonClass {}

@objc
class Ty0 : Class, NonClass {
  init() { }
}

// Attributes that should be reported by parser as unknown
// See rdar://19533915
@__setterAccess struct S__accessibility {} // expected-error{{unknown attribute '__setterAccess'}}
@__raw_doc_comment struct S__raw_doc_comment {} // expected-error{{unknown attribute '__raw_doc_comment'}}
@__objc_bridged struct S__objc_bridged {} // expected-error{{unknown attribute '__objc_bridged'}}

weak
var weak0 : Ty0?

weak
var weak0x : Ty0?

weak unowned var weak1 : Ty0? // expected-error {{duplicate modifier}} expected-note {{modifier already specified here}}
weak weak var weak2 : Ty0? // expected-error {{duplicate modifier}} expected-note {{modifier already specified here}}
unowned var weak3 : Ty0

unowned var weak3a : Ty0
unowned(safe) var weak3b : Ty0
unowned(unsafe) var weak3c : Ty0


unowned unowned var weak4 : Ty0  // expected-error {{duplicate modifier}}  expected-note {{modifier already specified here}}

unowned weak var weak5 : Ty0 // expected-error {{duplicate modifier}}  expected-note {{modifier already specified here}}

weak
var weak6 : Int? // expected-error {{'weak' may only be applied to class and class-bound protocol types, not 'Int'}}
unowned
var weak7 : Int // expected-error {{'unowned' may only be applied to class and class-bound protocol types, not 'Int'}}
weak
var weak8 : Class? = Ty0()
// expected-warning@-1 {{instance will be immediately deallocated because variable 'weak8' is 'weak'}}
// expected-note@-2 {{a strong reference is required to prevent the instance from being deallocated}}
// expected-note@-3 {{'weak8' declared here}}

unowned var weak9 : Class = Ty0()
// expected-warning@-1 {{instance will be immediately deallocated because variable 'weak9' is 'unowned'}}
// expected-note@-2 {{a strong reference is required to prevent the instance from being deallocated}}
// expected-note@-3 {{'weak9' declared here}}

weak
var weak10 : NonClass? = Ty0() // expected-error {{'weak' must not be applied to non-class-bound 'any NonClass'; consider adding a protocol conformance that has a class bound}}
unowned
var weak11 : NonClass = Ty0() // expected-error {{'unowned' must not be applied to non-class-bound 'any NonClass'; consider adding a protocol conformance that has a class bound}}

unowned
var weak12 : NonClass = Ty0() // expected-error {{'unowned' must not be applied to non-class-bound 'any NonClass'; consider adding a protocol conformance that has a class bound}}
unowned
var weak13 : NonClass = Ty0() // expected-error {{'unowned' must not be applied to non-class-bound 'any NonClass'; consider adding a protocol conformance that has a class bound}}

weak
var weak14 : Ty0 // expected-error {{'weak' variable should have optional type 'Ty0?'}}
weak
var weak15 : Class // expected-error {{'weak' variable should have optional type '(any Class)?'}}

weak var weak16 : Class!

@weak var weak17 : Class? // expected-error {{'weak' is a declaration modifier, not an attribute}} {{1-2=}}


@_exported var exportVar: Int // expected-error {{@_exported may only be used on 'import' declarations}}{{1-12=}}
@_exported func exportFunc() {} // expected-error {{@_exported may only be used on 'import' declarations}}{{1-12=}}
@_exported struct ExportStruct {} // expected-error {{@_exported may only be used on 'import' declarations}}{{1-12=}}


// Function result type attributes.
var func_result_type_attr : () -> @xyz Int  // expected-error {{unknown attribute 'xyz'}}

func func_result_attr() -> @xyz Int {       // expected-error {{unknown attribute 'xyz'}}
  return 4
}

func func_with_unknown_attr1(@unknown(*) x: Int) {} // expected-error {{unknown attribute 'unknown'}}
func func_with_unknown_attr2(x: @unknown(_) Int) {} // expected-error {{unknown attribute 'unknown'}}
func func_with_unknown_attr3(x: @unknown(Int) -> Int) {} // expected-error {{unknown attribute 'unknown'}}
func func_with_unknown_attr4(x: @unknown(Int) throws -> Int) {} // expected-error {{unknown attribute 'unknown'}}
func func_with_unknown_attr5(x: @unknown (x: Int, y: Int)) {} // expected-error {{unknown attribute 'unknown'}}
func func_with_unknown_attr6(x: @unknown(x: Int, y: Int)) {} // expected-error {{unknown attribute 'unknown'}}
func func_with_unknown_attr7(x: @unknown (Int) () -> Int) {} // expected-error {{unknown attribute 'unknown'}}

func func_type_attribute_with_space(x: @convention (c) () -> Int) {} // OK. Known attributes can have space before its paren.

// @thin and @pseudogeneric are not supported except in SIL.
var thinFunc : @thin () -> () // expected-error {{unknown attribute 'thin'}}
var pseudoGenericFunc : @pseudogeneric () -> () // expected-error {{unknown attribute 'pseudogeneric'}}

@inline(never) func nolineFunc() {}
@inline(never) var noinlineVar : Int { return 0 }
@inline(never) class FooClass { // expected-error {{'@inline(never)' attribute cannot be applied to this declaration}} {{1-16=}}
}

@inline(__always) func AlwaysInlineFunc() {}
@inline(__always) var alwaysInlineVar : Int { return 0 }
@inline(__always) class FooClass2 { // expected-error {{'@inline(__always)' attribute cannot be applied to this declaration}} {{1-19=}}
}

@_optimize(speed) func OspeedFunc() {}
@_optimize(speed) var OpeedVar : Int // expected-error {{'@_optimize(speed)' attribute cannot be applied to stored properties}} {{1-19=}}
@_optimize(speed) class OspeedClass { // expected-error {{'@_optimize(speed)' attribute cannot be applied to this declaration}} {{1-19=}}
}

class A {
  @inline(never) init(a : Int) {}
  var b : Int {
    @inline(never) get {
      return 42
    }
    @inline(never) set {
    }
  }
}

class B {
  @inline(__always) init(a : Int) {}
  var b : Int {
    @inline(__always) get {
      return 42
    }
    @inline(__always) set {
    }
  }
}

class C {
  @_optimize(speed) init(a : Int) {}
  var b : Int {
    @_optimize(none) get {
      return 42
    }
    @_optimize(size) set {
    }
  }
  @_optimize(size) var c : Int // expected-error {{'@_optimize(size)' attribute cannot be applied to stored properties}}
}


@exclusivity(checked) // ok
var globalCheckedVar = 1

@exclusivity(unchecked) // ok
var globalUncheckedVar = 1

@exclusivity(abc) // // expected-error {{unknown option 'abc' for attribute 'exclusivity'}}
var globalUnknownVar = 1

struct ExclusivityAttrStruct {
  @exclusivity(unchecked) // expected-error {{@exclusivity can only be used on class properties, static properties and global variables}}
  var instanceVar: Int = 27

  @exclusivity(unchecked) // ok
  static var staticVar: Int = 27

  @exclusivity(unchecked) // expected-error {{@exclusivity can only be used on stored properties}}
  static var staticComputedVar: Int { return 1 }
}

class ExclusivityAttrClass {
  @exclusivity(unchecked) // ok
  var instanceVar: Int = 27

  @exclusivity(unchecked) // ok
  static var staticVar: Int = 27

  @exclusivity(unchecked) // expected-error {{@exclusivity can only be used on stored properties}}
  static var staticComputedVar: Int { return 1 }
}

class HasStorage {
  @_hasStorage var x : Int = 42  // ok, _hasStorage is allowed here
}

@_show_in_interface protocol _underscored {}
@_show_in_interface class _notapplicable {} // expected-error {{may only be used on 'protocol' declarations}}

// Error recovery after one invalid attribute
@_invalid_attribute_ // expected-error {{unknown attribute '_invalid_attribute_'}}
@inline(__always)
public func sillyFunction() {}

// rdar://problem/45732251: unowned/unowned(unsafe) optional lets are permitted
func unownedOptionals(x: C) {
  unowned let y: C? = x
  unowned(unsafe) let y2: C? = x

  _ = y
  _ = y2
}

// @_nonEphemeral attribute
struct S1<T> {
  func foo(@_nonEphemeral _ x: String) {} // expected-error {{@_nonEphemeral attribute only applies to pointer types}}
  func bar(@_nonEphemeral _ x: T) {} // expected-error {{@_nonEphemeral attribute only applies to pointer types}}

  func baz<U>(@_nonEphemeral _ x: U) {} // expected-error {{@_nonEphemeral attribute only applies to pointer types}}

  func qux(@_nonEphemeral _ x: UnsafeMutableRawPointer) {}
  func quux(@_nonEphemeral _ x: UnsafeMutablePointer<Int>?) {}
}

@_nonEphemeral struct S2 {} // expected-error {{@_nonEphemeral may only be used on 'parameter' declarations}}

protocol P {}
extension P {
  // Allow @_nonEphemeral on the protocol Self type, as the protocol could be adopted by a pointer type.
  func foo(@_nonEphemeral _ x: Self) {}
  func bar(@_nonEphemeral _ x: Self?) {}
}

enum E1 {
  case str(@_nonEphemeral _: String) // expected-error {{expected parameter name followed by ':'}}
  case ptr(@_nonEphemeral _: UnsafeMutableRawPointer) // expected-error {{expected parameter name followed by ':'}}

  func foo() -> @_nonEphemeral UnsafeMutableRawPointer? { return nil } // expected-error {{attribute can only be applied to declarations, not types}}
}

@_custom func testCustomAttribute() {} // expected-error {{unknown attribute '_custom'}}

// https://github.com/apple/swift/issues/65705
struct GI65705<A> {}
struct I65705 {
  let m1: @discardableResult () -> Int // expected-error {{attribute can only be applied to declarations, not types}} {{11-30=}} {{none}}
  var m2: @discardableResult () -> Int // expected-error {{attribute can only be applied to declarations, not types}} {{11-30=}} {{none}}
  let m3: GI65705<@discardableResult () -> Int> // expected-error{{attribute can only be applied to declarations, not types}} {{19-37=}} {{none}}

  func f1(_: inout @discardableResult Int) {} // expected-error {{attribute can only be applied to declarations, not types}} {{20-39=}} {{3-3=@discardableResult }} {{none}}
  func f2(_: @discardableResult Int) {} // expected-error {{attribute can only be applied to declarations, not types}} {{14-33=}} {{3-3=@discardableResult }} {{none}}

  func stmt(_ a: Int?) {
    if let _: @discardableResult Int = a { // expected-error {{attribute can only be applied to declarations, not types}} {{15-34=}} 
    }
    if var _: @discardableResult Int = a { // expected-error {{attribute can only be applied to declarations, not types}} {{15-34=}}
    }
  }
}
