// RUN: %target-swift-frontend -disable-objc-attr-requires-foundation-module -typecheck -verify -verify-ignore-unknown %s -swift-version 4 -enable-source-import -I %S/Inputs -enable-swift3-objc-inference
// RUN: %target-swift-ide-test -skip-deinit=false -print-ast-typechecked -source-filename %s -function-definitions=true -prefer-type-repr=false -print-implicit-attrs=true -explode-pattern-binding-decls=true -disable-objc-attr-requires-foundation-module -swift-version 4 -enable-source-import -I %S/Inputs -enable-swift3-objc-inference | %FileCheck %s
// RUN: not %target-swift-frontend -typecheck -dump-ast -disable-objc-attr-requires-foundation-module %s -swift-version 4 -enable-source-import -I %S/Inputs -enable-swift3-objc-inference > %t.ast
// RUN: %FileCheck -check-prefix CHECK-DUMP %s < %t.ast
// REQUIRES: objc_interop

import Foundation

class PlainClass {}
struct PlainStruct {}
enum PlainEnum {}
protocol PlainProtocol {} // expected-note {{protocol 'PlainProtocol' declared here}}

enum ErrorEnum : Error {
  case failed
}

@objc class Class_ObjC1 {}

protocol Protocol_Class1 : class {} // expected-note {{protocol 'Protocol_Class1' declared here}}

protocol Protocol_Class2 : class {}

@objc protocol Protocol_ObjC1 {}

@objc protocol Protocol_ObjC2 {}




//===--- Subjects of @objc attribute.

@objc extension PlainStruct { } // expected-error{{'@objc' can only be applied to an extension of a class}}{{1-7=}}

class FáncyName {}
@objc (FancyName) extension FáncyName {}

@objc // expected-error {{@objc can only be used with members of classes, @objc protocols, and concrete extensions of classes}}
var subject_globalVar: Int

var subject_getterSetter: Int {
  @objc // expected-error {{@objc can only be used with members of classes, @objc protocols, and concrete extensions of classes}}
  get {
    return 0
  }
  @objc // expected-error {{@objc can only be used with members of classes, @objc protocols, and concrete extensions of classes}} {{3-9=}}
  set {
  }
}

var subject_global_observingAccessorsVar1: Int = 0 {
  @objc // expected-error {{@objc can only be used with members of classes, @objc protocols, and concrete extensions of classes}} {{3-9=}}
  willSet {
  }
  @objc // expected-error {{@objc can only be used with members of classes, @objc protocols, and concrete extensions of classes}} {{3-9=}}
  didSet {
  }
}

class subject_getterSetter1 {
  var instanceVar1: Int {
    @objc
    get { // expected-error {{'@objc' getter for non-'@objc' property}}
      return 0
    }
  }

  var instanceVar2: Int {
    get {
      return 0
    }
    @objc
    set { // expected-error {{'@objc' setter for non-'@objc' property}}
    }
  }

  var instanceVar3: Int {
    @objc
    get { // expected-error {{'@objc' getter for non-'@objc' property}}
      return 0
    }
    @objc
    set { // expected-error {{'@objc' setter for non-'@objc' property}}
    }
  }

  var observingAccessorsVar1: Int = 0 {
    @objc // expected-error {{observing accessors are not allowed to be marked @objc}} {{5-11=}}
    willSet {
    }
    @objc // expected-error {{observing accessors are not allowed to be marked @objc}} {{5-11=}}
    didSet {
    }
  }
}

class subject_staticVar1 {
  @objc
  class var staticVar1: Int = 42 // expected-error {{class stored properties not supported}}

  @objc
  class var staticVar2: Int { return 42 }
}

@objc // expected-error {{@objc can only be used with members of classes, @objc protocols, and concrete extensions of classes}} {{1-7=}}
func subject_freeFunc() {
  @objc // expected-error {{@objc can only be used with members of classes, @objc protocols, and concrete extensions of classes}} {{3-9=}}
  var subject_localVar: Int

  @objc // expected-error {{@objc can only be used with members of classes, @objc protocols, and concrete extensions of classes}} {{3-9=}}
  func subject_nestedFreeFunc() {
  }
}

@objc // expected-error {{@objc can only be used with members of classes, @objc protocols, and concrete extensions of classes}} {{1-7=}}
func subject_genericFunc<T>(t: T) {
  @objc // expected-error {{@objc can only be used with members of classes, @objc protocols, and concrete extensions of classes}} {{3-9=}}
  var subject_localVar: Int

  @objc // expected-error {{@objc can only be used with members of classes, @objc protocols, and concrete extensions of classes}} {{3-9=}}
  func subject_instanceFunc() {}
}

func subject_funcParam(a: @objc Int) { // expected-error {{attribute can only be applied to declarations, not types}} {{1-1=@objc }} {{27-33=}}
}

@objc // expected-error {{'@objc' attribute cannot be applied to this declaration}} {{1-7=}}
struct subject_struct {
  @objc // expected-error {{@objc can only be used with members of classes, @objc protocols, and concrete extensions of classes}} {{3-9=}}
  var subject_instanceVar: Int

  @objc // expected-error {{@objc can only be used with members of classes, @objc protocols, and concrete extensions of classes}} {{3-9=}}
  init() {}

  @objc // expected-error {{@objc can only be used with members of classes, @objc protocols, and concrete extensions of classes}} {{3-9=}}
  func subject_instanceFunc() {}
}

@objc   // expected-error {{'@objc' attribute cannot be applied to this declaration}} {{1-7=}}
struct subject_genericStruct<T> {
  @objc // expected-error {{@objc can only be used with members of classes, @objc protocols, and concrete extensions of classes}} {{3-9=}}
  var subject_instanceVar: Int

  @objc // expected-error {{@objc can only be used with members of classes, @objc protocols, and concrete extensions of classes}} {{3-9=}}
  init() {}

  @objc // expected-error {{@objc can only be used with members of classes, @objc protocols, and concrete extensions of classes}} {{3-9=}}
  func subject_instanceFunc() {}
}

@objc
class subject_class1 { // no-error
  @objc
  var subject_instanceVar: Int // no-error

  @objc
  init() {} // no-error

  @objc
  func subject_instanceFunc() {} // no-error
  
  
}

@objc
class subject_class2 : Protocol_Class1, PlainProtocol { // no-error
}

@objc // expected-error{{generic subclasses of '@objc' classes cannot have an explicit '@objc' because they are not directly visible from Objective-C}} {{1-7=}}
class subject_genericClass<T> {
  @objc
  var subject_instanceVar: Int // no-error

  @objc
  init() {} // no-error

  @objc
  func subject_instanceFunc() {} // no_error
}

@objc // expected-error{{generic subclasses of '@objc' classes cannot have an explicit '@objc'}} {{1-7=}}
class subject_genericClass2<T> : Class_ObjC1 {
  @objc
  var subject_instanceVar: Int // no-error

  @objc
  init(foo: Int) {} // no-error

  @objc
  func subject_instanceFunc() {} // no_error
}

extension subject_genericClass where T : Hashable {
  @objc var prop: Int { return 0 } // expected-error{{members of constrained extensions cannot be declared @objc}}
}

extension subject_genericClass {
  @objc var extProp: Int { return 0 } // expected-error{{extensions of generic classes cannot contain '@objc' members}}
  
  @objc func extFoo() {} // expected-error{{extensions of generic classes cannot contain '@objc' members}}
}

@objc
enum subject_enum: Int {
  @objc   // expected-error {{attribute has no effect; cases within an '@objc' enum are already exposed to Objective-C}} {{3-9=}}
  case subject_enumElement1

  @objc(subject_enumElement2)
  case subject_enumElement2

  @objc(subject_enumElement3) // expected-error {{'@objc' enum case declaration defines multiple enum cases with the same Objective-C name}}{{3-31=}}
  case subject_enumElement3, subject_enumElement4

  @objc   // expected-error {{attribute has no effect; cases within an '@objc' enum are already exposed to Objective-C}} {{3-9=}}
  case subject_enumElement5, subject_enumElement6

  @nonobjc // expected-error {{'@nonobjc' attribute cannot be applied to this declaration}}
  case subject_enumElement7

  @objc // expected-error {{@objc can only be used with members of classes, @objc protocols, and concrete extensions of classes}} {{3-9=}}
  init() {}

  @objc // expected-error {{@objc can only be used with members of classes, @objc protocols, and concrete extensions of classes}} {{3-9=}}
  func subject_instanceFunc() {}
}

enum subject_enum2 {
  @objc(subject_enum2Element1) // expected-error{{'@objc' enum case is not allowed outside of an '@objc' enum}}{{3-32=}}
  case subject_enumElement1
}

@objc
protocol subject_protocol1 {
  @objc
  var subject_instanceVar: Int { get }

  @objc
  func subject_instanceFunc()
}

@objc protocol subject_protocol2 {} // no-error
// CHECK-LABEL: @objc protocol subject_protocol2 {

@objc protocol subject_protocol3 {} // no-error
// CHECK-LABEL: @objc protocol subject_protocol3 {

@objc
protocol subject_protocol4 : PlainProtocol {} // expected-error {{@objc protocol 'subject_protocol4' cannot refine non-@objc protocol 'PlainProtocol'}}

@objc
protocol subject_protocol5 : Protocol_Class1 {} // expected-error {{@objc protocol 'subject_protocol5' cannot refine non-@objc protocol 'Protocol_Class1'}}

@objc
protocol subject_protocol6 : Protocol_ObjC1 {}

protocol subject_containerProtocol1 {
  @objc // expected-error {{@objc can only be used with members of classes, @objc protocols, and concrete extensions of classes}}
  var subject_instanceVar: Int { get }

  @objc // expected-error {{@objc can only be used with members of classes, @objc protocols, and concrete extensions of classes}}
  func subject_instanceFunc()

  @objc // expected-error {{@objc can only be used with members of classes, @objc protocols, and concrete extensions of classes}}
  static func subject_staticFunc()
}

@objc
protocol subject_containerObjCProtocol1 {
  func func_FunctionReturn1() -> PlainStruct
  // expected-error@-1 {{method cannot be a member of an @objc protocol because its result type cannot be represented in Objective-C}}
  // expected-note@-2 {{Swift structs cannot be represented in Objective-C}}
  // expected-note@-3 {{inferring '@objc' because the declaration is a member of an '@objc' protocol}}

  func func_FunctionParam1(a: PlainStruct)
  // expected-error@-1 {{method cannot be a member of an @objc protocol because the type of the parameter cannot be represented in Objective-C}}
  // expected-note@-2 {{Swift structs cannot be represented in Objective-C}}
  // expected-note@-3 {{inferring '@objc' because the declaration is a member of an '@objc' protocol}}

  func func_Variadic(_: AnyObject...)
  // expected-error @-1{{method cannot be a member of an @objc protocol because it has a variadic parameter}}
  // expected-note @-2{{inferring '@objc' because the declaration is a member of an '@objc' protocol}}

  subscript(a: PlainStruct) -> Int { get }
  // expected-error@-1 {{subscript cannot be a member of an @objc protocol because its type cannot be represented in Objective-C}}
  // expected-note@-2 {{Swift structs cannot be represented in Objective-C}}
  // expected-note@-3 {{inferring '@objc' because the declaration is a member of an '@objc' protocol}}

  var varNonObjC1: PlainStruct { get }
  // expected-error@-1 {{property cannot be a member of an @objc protocol because its type cannot be represented in Objective-C}}
  // expected-note@-2 {{Swift structs cannot be represented in Objective-C}}
  // expected-note@-3 {{inferring '@objc' because the declaration is a member of an '@objc' protocol}}
}

@objc
protocol subject_containerObjCProtocol2 {
  init(a: Int)
  @objc init(a: Double)

  func func1() -> Int
  @objc func func1_() -> Int

  var instanceVar1: Int { get set }
  @objc var instanceVar1_: Int { get set }

  subscript(i: Int) -> Int { get set }
  @objc subscript(i: String) -> Int { get set}
}

protocol nonObjCProtocol {
  @objc func objcRequirement() // expected-error{{@objc can only be used with members of classes, @objc protocols, and concrete extensions of classes}}
}

func concreteContext1() {
  @objc
  class subject_inConcreteContext {}
}

class ConcreteContext2 {
  @objc
  class subject_inConcreteContext {}
}

class ConcreteContext3 {

  func dynamicSelf1() -> Self { return self }

  @objc func dynamicSelf1_() -> Self { return self }

  @objc func genericParams<T: NSObject>() -> [T] { return [] }
  // expected-error@-1{{method cannot be marked @objc because it has generic parameters}}

  @objc func returnObjCProtocolMetatype() -> NSCoding.Protocol { return NSCoding.self }
  // expected-error@-1{{method cannot be marked @objc because its result type cannot be represented in Objective-C}}

  typealias AnotherNSCoding = NSCoding
  typealias MetaNSCoding1 = NSCoding.Protocol
  typealias MetaNSCoding2 = AnotherNSCoding.Protocol

  @objc func returnObjCAliasProtocolMetatype1() -> AnotherNSCoding.Protocol { return NSCoding.self }
  // expected-error@-1{{method cannot be marked @objc because its result type cannot be represented in Objective-C}}

  @objc func returnObjCAliasProtocolMetatype2() -> MetaNSCoding1 { return NSCoding.self }
  // expected-error@-1{{method cannot be marked @objc because its result type cannot be represented in Objective-C}}

  @objc func returnObjCAliasProtocolMetatype3() -> MetaNSCoding2 { return NSCoding.self }
  // expected-error@-1{{method cannot be marked @objc because its result type cannot be represented in Objective-C}}

  typealias Composition = NSCopying & NSCoding

  @objc func returnCompositionMetatype1() -> Composition.Protocol { return Composition.self }
  // expected-error@-1{{method cannot be marked @objc because its result type cannot be represented in Objective-C}}

  @objc func returnCompositionMetatype2() -> (NSCopying & NSCoding).Protocol { return (NSCopying & NSCoding).self }
  // expected-error@-1{{method cannot be marked @objc because its result type cannot be represented in Objective-C}}

  typealias NSCodingExistential = NSCoding.Type

  @objc func inoutFunc(a: inout Int) {}
  // expected-error@-1{{method cannot be marked @objc because inout parameters cannot be represented in Objective-C}}
  @objc func metatypeOfExistentialMetatypePram1(a: NSCodingExistential.Protocol) {}
  // expected-error@-1{{method cannot be marked @objc because the type of the parameter cannot be represented in Objective-C}}

  @objc func metatypeOfExistentialMetatypePram2(a: NSCoding.Type.Protocol) {}
  // expected-error@-1{{method cannot be marked @objc because the type of the parameter cannot be represented in Objective-C}}
}

func genericContext1<T>(_: T) {
  @objc // expected-error{{generic subclasses of '@objc' classes cannot have an explicit '@objc' because they are not directly visible from Objective-C}} {{3-9=}}
  class subject_inGenericContext {} // expected-error{{type 'subject_inGenericContext' cannot be nested in generic function 'genericContext1'}}

  @objc // expected-error{{generic subclasses of '@objc' classes cannot have an explicit '@objc'}} {{3-9=}}
  class subject_inGenericContext2 : Class_ObjC1 {} // expected-error{{type 'subject_inGenericContext2' cannot be nested in generic function 'genericContext1'}}

  class subject_constructor_inGenericContext { // expected-error{{type 'subject_constructor_inGenericContext' cannot be nested in generic function 'genericContext1'}}
    @objc
    init() {} // no-error
  }

  class subject_var_inGenericContext { // expected-error{{type 'subject_var_inGenericContext' cannot be nested in generic function 'genericContext1'}}
    @objc
    var subject_instanceVar: Int = 0 // no-error
  }

  class subject_func_inGenericContext { // expected-error{{type 'subject_func_inGenericContext' cannot be nested in generic function 'genericContext1'}}
    @objc
    func f() {} // no-error
  }
}

class GenericContext2<T> {
  @objc // expected-error{{generic subclasses of '@objc' classes cannot have an explicit '@objc' because they are not directly visible from Objective-C}} {{3-9=}}
  class subject_inGenericContext {}

  @objc // expected-error{{generic subclasses of '@objc' classes cannot have an explicit '@objc'}} {{3-9=}}
  class subject_inGenericContext2 : Class_ObjC1 {}

  @objc
  func f() {} // no-error
}

class GenericContext3<T> {
  class MoreNested {
    @objc // expected-error{{generic subclasses of '@objc' classes cannot have an explicit '@objc' because they are not directly visible from Objective-C}} {{5-11=}}
    class subject_inGenericContext {}

    @objc // expected-error{{generic subclasses of '@objc' classes cannot have an explicit '@objc'}} {{5-11=}}
    class subject_inGenericContext2 : Class_ObjC1 {}

    @objc
    func f() {} // no-error
  }
}

class GenericContext4<T> {
  @objc
  func foo() where T: Hashable { } // expected-error {{instance method cannot be marked @objc because it has a 'where' clause}}
}

@objc // expected-error{{generic subclasses of '@objc' classes cannot have an explicit '@objc' because they are not directly visible from Objective-C}} {{1-7=}}
class ConcreteSubclassOfGeneric : GenericContext3<Int> {}

extension ConcreteSubclassOfGeneric {
  @objc func foo() {} // okay
}

@objc // expected-error{{generic subclasses of '@objc' classes cannot have an explicit '@objc'}} {{1-7=}}
class ConcreteSubclassOfGeneric2 : subject_genericClass2<Int> {}

extension ConcreteSubclassOfGeneric2 {
  @objc func foo() {} // okay
}

@objc(CustomNameForSubclassOfGeneric) // okay
class ConcreteSubclassOfGeneric3 : GenericContext3<Int> {}

extension ConcreteSubclassOfGeneric3 {
  @objc func foo() {} // okay
}

class subject_subscriptIndexed1 {
  @objc
  subscript(a: Int) -> Int { // no-error
    get { return 0 }
  }
}
class subject_subscriptIndexed2 {
  @objc
  subscript(a: Int8) -> Int { // no-error
    get { return 0 }
  }
}
class subject_subscriptIndexed3 {
  @objc
  subscript(a: UInt8) -> Int { // no-error
    get { return 0 }
  }
}

class subject_subscriptKeyed1 {
  @objc
  subscript(a: String) -> Int { // no-error
    get { return 0 }
  }
}
class subject_subscriptKeyed2 {
  @objc
  subscript(a: Class_ObjC1) -> Int { // no-error
    get { return 0 }
  }
}
class subject_subscriptKeyed3 {
  @objc
  subscript(a: Class_ObjC1.Type) -> Int { // no-error
    get { return 0 }
  }
}
class subject_subscriptKeyed4 {
  @objc
  subscript(a: Protocol_ObjC1) -> Int { // no-error
    get { return 0 }
  }
}
class subject_subscriptKeyed5 {
  @objc
  subscript(a: Protocol_ObjC1.Type) -> Int { // no-error
    get { return 0 }
  }
}
class subject_subscriptKeyed6 {
  @objc
  subscript(a: Protocol_ObjC1 & Protocol_ObjC2) -> Int { // no-error
    get { return 0 }
  }
}
class subject_subscriptKeyed7 {
  @objc
  subscript(a: (Protocol_ObjC1 & Protocol_ObjC2).Type) -> Int { // no-error
    get { return 0 }
  }
}

class subject_subscriptBridgedFloat {
  @objc
  subscript(a: Float32) -> Int {
    get { return 0 }
  }
}

class subject_subscriptGeneric<T> {
  @objc
  subscript(a: Int) -> Int { // no-error
    get { return 0 }
  }
}

class subject_subscriptInvalid1 {
  @objc class subscript(_ i: Int) -> AnyObject? {
  // expected-error@-1 {{class subscript cannot be marked @objc}}
    return nil
  }
}

class subject_subscriptInvalid2 {
  @objc
  subscript(a: PlainClass) -> Int {
  // expected-error@-1 {{subscript cannot be marked @objc because its type cannot be represented in Objective-C}}
  // expected-note@-2 {{classes not annotated with @objc cannot be represented in Objective-C}}
    get { return 0 }
  }
}
class subject_subscriptInvalid3 {
  @objc
  subscript(a: PlainClass.Type) -> Int { // expected-error {{subscript cannot be marked @objc because its type cannot be represented in Objective-C}}
    get { return 0 }
  }
}
class subject_subscriptInvalid4 {
  @objc
  subscript(a: PlainStruct) -> Int { // expected-error {{subscript cannot be marked @objc because its type cannot be represented in Objective-C}}
    // expected-note@-1{{Swift structs cannot be represented in Objective-C}}
    get { return 0 }
  }
}
class subject_subscriptInvalid5 {
  @objc
  subscript(a: PlainEnum) -> Int { // expected-error {{subscript cannot be marked @objc because its type cannot be represented in Objective-C}}
    // expected-note@-1{{enums cannot be represented in Objective-C}}
    get { return 0 }
  }
}
class subject_subscriptInvalid6 {
  @objc
  subscript(a: PlainProtocol) -> Int { // expected-error {{subscript cannot be marked @objc because its type cannot be represented in Objective-C}}
    // expected-note@-1{{protocol-constrained type containing protocol 'PlainProtocol' cannot be represented in Objective-C}}
    get { return 0 }
  }
}
class subject_subscriptInvalid7 {
  @objc
  subscript(a: Protocol_Class1) -> Int { // expected-error {{subscript cannot be marked @objc because its type cannot be represented in Objective-C}}
    // expected-note@-1{{protocol-constrained type containing protocol 'Protocol_Class1' cannot be represented in Objective-C}}
    get { return 0 }
  }
}
class subject_subscriptInvalid8 {
  @objc
  subscript(a: Protocol_Class1 & Protocol_Class2) -> Int { // expected-error {{subscript cannot be marked @objc because its type cannot be represented in Objective-C}}
    // expected-note@-1{{protocol-constrained type containing protocol 'Protocol_Class1' cannot be represented in Objective-C}}
    get { return 0 }
  }
}

class subject_propertyInvalid1 {
  @objc
  let plainStruct = PlainStruct() // expected-error {{property cannot be marked @objc because its type cannot be represented in Objective-C}}
  // expected-note@-1{{Swift structs cannot be represented in Objective-C}}
}

//===--- Tests for @objc inference.

@objc
class infer_instanceFunc1 {
// CHECK-LABEL: @objc class infer_instanceFunc1 {

  func func1() {}
// CHECK-LABEL: @objc func func1() {

  @objc func func1_() {} // no-error

  func func2(a: Int) {}
// CHECK-LABEL: @objc func func2(a: Int) {

  @objc func func2_(a: Int) {} // no-error

  func func3(a: Int) -> Int {}
// CHECK-LABEL: @objc func func3(a: Int) -> Int {

  @objc func func3_(a: Int) -> Int {} // no-error

  func func4(a: Int, b: Double) {}
// CHECK-LABEL: @objc func func4(a: Int, b: Double) {

  @objc func func4_(a: Int, b: Double) {} // no-error

  func func5(a: String) {}
// CHECK-LABEL: @objc func func5(a: String) {

  @objc func func5_(a: String) {} // no-error

  func func6() -> String {}
// CHECK-LABEL: @objc func func6() -> String {

  @objc func func6_() -> String {} // no-error

  func func7(a: PlainClass) {}
// CHECK-LABEL: {{^}} func func7(a: PlainClass) {

  @objc func func7_(a: PlainClass) {}
  // expected-error@-1 {{method cannot be marked @objc because the type of the parameter cannot be represented in Objective-C}}
  // expected-note@-2 {{classes not annotated with @objc cannot be represented in Objective-C}}

  func func7m(a: PlainClass.Type) {}
// CHECK-LABEL: {{^}} func func7m(a: PlainClass.Type) {

  @objc func func7m_(a: PlainClass.Type) {}
  // expected-error@-1 {{method cannot be marked @objc because the type of the parameter cannot be represented in Objective-C}}

  func func8() -> PlainClass {}
// CHECK-LABEL: {{^}} func func8() -> PlainClass {

  @objc func func8_() -> PlainClass {}
  // expected-error@-1 {{method cannot be marked @objc because its result type cannot be represented in Objective-C}}
  // expected-note@-2 {{classes not annotated with @objc cannot be represented in Objective-C}}

  func func8m() -> PlainClass.Type {}
// CHECK-LABEL: {{^}} func func8m() -> PlainClass.Type {

  @objc func func8m_() -> PlainClass.Type {}
  // expected-error@-1 {{method cannot be marked @objc because its result type cannot be represented in Objective-C}}

  func func9(a: PlainStruct) {}
// CHECK-LABEL: {{^}} func func9(a: PlainStruct) {

  @objc func func9_(a: PlainStruct) {}
  // expected-error@-1 {{method cannot be marked @objc because the type of the parameter cannot be represented in Objective-C}}
  // expected-note@-2 {{Swift structs cannot be represented in Objective-C}}

  func func10() -> PlainStruct {}
// CHECK-LABEL: {{^}} func func10() -> PlainStruct {

  @objc func func10_() -> PlainStruct {}
  // expected-error@-1 {{method cannot be marked @objc because its result type cannot be represented in Objective-C}}
  // expected-note@-2 {{Swift structs cannot be represented in Objective-C}}

  func func11(a: PlainEnum) {}
// CHECK-LABEL: {{^}} func func11(a: PlainEnum) {

  @objc func func11_(a: PlainEnum) {}
  // expected-error@-1 {{method cannot be marked @objc because the type of the parameter cannot be represented in Objective-C}}
  // expected-note@-2 {{non-'@objc' enums cannot be represented in Objective-C}}

  func func12(a: PlainProtocol) {}
// CHECK-LABEL: {{^}} func func12(a: PlainProtocol) {

  @objc func func12_(a: PlainProtocol) {}
  // expected-error@-1 {{method cannot be marked @objc because the type of the parameter cannot be represented in Objective-C}}
  // expected-note@-2 {{protocol-constrained type containing protocol 'PlainProtocol' cannot be represented in Objective-C}}

  func func13(a: Class_ObjC1) {}
// CHECK-LABEL: @objc func func13(a: Class_ObjC1) {

  @objc func func13_(a: Class_ObjC1) {} // no-error

  func func14(a: Protocol_Class1) {}
// CHECK-LABEL: {{^}} func func14(a: Protocol_Class1) {

  @objc func func14_(a: Protocol_Class1) {}
  // expected-error@-1 {{method cannot be marked @objc because the type of the parameter cannot be represented in Objective-C}}
  // expected-note@-2 {{protocol-constrained type containing protocol 'Protocol_Class1' cannot be represented in Objective-C}}

  func func15(a: Protocol_ObjC1) {}
// CHECK-LABEL: @objc func func15(a: Protocol_ObjC1) {
  @objc func func15_(a: Protocol_ObjC1) {} // no-error

  func func16(a: AnyObject) {}
// CHECK-LABEL: @objc func func16(a: AnyObject) {

  @objc func func16_(a: AnyObject) {} // no-error

  func func17(a: @escaping () -> ()) {}
// CHECK-LABEL: {{^}}  @objc func func17(a: @escaping () -> ()) {

  @objc func func17_(a: @escaping () -> ()) {}

  func func18(a: @escaping (Int) -> (), b: Int) {}
// CHECK-LABEL: {{^}}  @objc func func18(a: @escaping (Int) -> (), b: Int)

  @objc func func18_(a: @escaping (Int) -> (), b: Int) {}

  func func19(a: @escaping (String) -> (), b: Int) {}
// CHECK-LABEL: {{^}}  @objc func func19(a: @escaping (String) -> (), b: Int) {

  @objc func func19_(a: @escaping (String) -> (), b: Int) {}

  func func_FunctionReturn1() -> () -> () {}
// CHECK-LABEL: {{^}}  @objc func func_FunctionReturn1() -> () -> () {

  @objc func func_FunctionReturn1_() -> () -> () {}

  func func_FunctionReturn2() -> (Int) -> () {}
// CHECK-LABEL: {{^}}  @objc func func_FunctionReturn2() -> (Int) -> () {

  @objc func func_FunctionReturn2_() -> (Int) -> () {}

  func func_FunctionReturn3() -> () -> Int {}
// CHECK-LABEL: {{^}}  @objc func func_FunctionReturn3() -> () -> Int {

  @objc func func_FunctionReturn3_() -> () -> Int {}

  func func_FunctionReturn4() -> (String) -> () {}
// CHECK-LABEL: {{^}}  @objc func func_FunctionReturn4() -> (String) -> () {

  @objc func func_FunctionReturn4_() -> (String) -> () {}

  func func_FunctionReturn5() -> () -> String {}
// CHECK-LABEL: {{^}}  @objc func func_FunctionReturn5() -> () -> String {

  @objc func func_FunctionReturn5_() -> () -> String {}


  func func_ZeroParams1() {}
// CHECK-LABEL: @objc func func_ZeroParams1() {

  @objc func func_ZeroParams1a() {} // no-error


  func func_OneParam1(a: Int) {}
// CHECK-LABEL: @objc func func_OneParam1(a: Int) {

  @objc func func_OneParam1a(a: Int) {} // no-error


  func func_TupleStyle1(a: Int, b: Int) {}
  // CHECK-LABEL: {{^}} @objc func func_TupleStyle1(a: Int, b: Int) {

  @objc func func_TupleStyle1a(a: Int, b: Int) {}

  func func_TupleStyle2(a: Int, b: Int, c: Int) {}
// CHECK-LABEL: {{^}} @objc func func_TupleStyle2(a: Int, b: Int, c: Int) {

  @objc func func_TupleStyle2a(a: Int, b: Int, c: Int) {}

  // Check that we produce diagnostics for every parameter and return type.
  @objc func func_MultipleDiags(a: PlainStruct, b: PlainEnum) -> Any {}
  // expected-error@-1 {{method cannot be marked @objc because the type of the parameter 1 cannot be represented in Objective-C}}
  // expected-note@-2 {{Swift structs cannot be represented in Objective-C}}
  // expected-error@-3 {{method cannot be marked @objc because the type of the parameter 2 cannot be represented in Objective-C}}
  // expected-note@-4 {{non-'@objc' enums cannot be represented in Objective-C}}

  @objc func func_UnnamedParam1(_: Int) {} // no-error

  @objc func func_UnnamedParam2(_: PlainStruct) {}
  // expected-error@-1 {{method cannot be marked @objc because the type of the parameter cannot be represented in Objective-C}}
  // expected-note@-2 {{Swift structs cannot be represented in Objective-C}}

  @objc func func_varParam1(a: AnyObject) {
    var a = a
    let b = a; a = b
  }

  func func_varParam2(a: AnyObject) {
    var a = a
    let b = a; a = b
  }
// CHECK-LABEL: @objc func func_varParam2(a: AnyObject) {
}

@objc
class infer_constructor1 {
// CHECK-LABEL: @objc class infer_constructor1

  init() {}
  // CHECK: @objc init()

  init(a: Int) {}
  // CHECK: @objc init(a: Int)

  init(a: PlainStruct) {}
  // CHECK: {{^}} init(a: PlainStruct)

  init(malice: ()) {}
  // CHECK: @objc init(malice: ())

  init(forMurder _: ()) {}
  // CHECK: @objc init(forMurder _: ())
}

@objc
class infer_destructor1 {
// CHECK-LABEL: @objc class infer_destructor1

  deinit {}
  // CHECK: @objc deinit
}

// @!objc
class infer_destructor2 {
// CHECK-LABEL: {{^}}class infer_destructor2

  deinit {}
  // CHECK: @objc deinit
}

@objc
class infer_instanceVar1 {
// CHECK-LABEL: @objc class infer_instanceVar1 {
  init() {}

  var instanceVar1: Int
  // CHECK: @objc var instanceVar1: Int

  var (instanceVar2, instanceVar3): (Int, PlainProtocol)
  // CHECK: @objc var instanceVar2: Int
  // CHECK: {{^}}  var instanceVar3: PlainProtocol

  @objc var (instanceVar1_, instanceVar2_): (Int, PlainProtocol)
  // expected-error@-1 {{property cannot be marked @objc because its type cannot be represented in Objective-C}}
  // expected-note@-2 {{protocol-constrained type containing protocol 'PlainProtocol' cannot be represented in Objective-C}}

  var intstanceVar4: Int {
  // CHECK: @objc var intstanceVar4: Int {
    get {}
    // CHECK-NEXT: @objc get {}
  }

  var intstanceVar5: Int {
  // CHECK: @objc var intstanceVar5: Int {
    get {}
    // CHECK-NEXT: @objc get {}
    set {}
    // CHECK-NEXT: @objc set {}
  }

  @objc var instanceVar5_: Int {
  // CHECK: @objc var instanceVar5_: Int {
    get {}
    // CHECK-NEXT: @objc get {}
    set {}
    // CHECK-NEXT: @objc set {}
  }

  var observingAccessorsVar1: Int {
  // CHECK: @_hasStorage @objc var observingAccessorsVar1: Int {
    willSet {}
    // CHECK-NEXT: {{^}} @objc get {
    // CHECK-NEXT:    return
    // CHECK-NEXT: }
    didSet {}
    // CHECK-NEXT: {{^}} @objc set {
  }

  @objc var observingAccessorsVar1_: Int {
  // CHECK: {{^}} @objc @_hasStorage var observingAccessorsVar1_: Int {
    willSet {}
    // CHECK-NEXT: {{^}} @objc get {
    // CHECK-NEXT:   return
    // CHECK-NEXT: }
    didSet {}
    // CHECK-NEXT: {{^}} @objc set {
  }


  var var_Int: Int
// CHECK-LABEL: @objc var var_Int: Int

  var var_Bool: Bool
// CHECK-LABEL: @objc var var_Bool: Bool

  var var_CBool: CBool
// CHECK-LABEL: @objc var var_CBool: CBool

  var var_String: String
// CHECK-LABEL: @objc var var_String: String

  var var_Float: Float
  var var_Double: Double
// CHECK-LABEL: @objc var var_Float: Float
// CHECK-LABEL: @objc var var_Double: Double

  var var_Char: Unicode.Scalar
// CHECK-LABEL: @objc var var_Char: Unicode.Scalar

  //===--- Tuples.

  var var_tuple1: ()
// CHECK-LABEL: {{^}} @_hasInitialValue var var_tuple1: ()

  @objc var var_tuple1_: ()
  // expected-error@-1 {{property cannot be marked @objc because its type cannot be represented in Objective-C}}
  // expected-note@-2 {{empty tuple type cannot be represented in Objective-C}}

  var var_tuple2: Void
// CHECK-LABEL: {{^}} @_hasInitialValue var var_tuple2: Void

  @objc var var_tuple2_: Void
  // expected-error@-1 {{property cannot be marked @objc because its type cannot be represented in Objective-C}}
  // expected-note@-2 {{empty tuple type cannot be represented in Objective-C}}

  var var_tuple3: (Int)
// CHECK-LABEL: @objc var var_tuple3: (Int)

  @objc var var_tuple3_: (Int) // no-error

  var var_tuple4: (Int, Int)
// CHECK-LABEL: {{^}} var var_tuple4: (Int, Int)

  @objc var var_tuple4_: (Int, Int)
  // expected-error@-1 {{property cannot be marked @objc because its type cannot be represented in Objective-C}}
  // expected-note@-2 {{tuples cannot be represented in Objective-C}}

  //===--- Stdlib integer types.

  var var_Int8: Int8
  var var_Int16: Int16
  var var_Int32: Int32
  var var_Int64: Int64
// CHECK-LABEL: @objc var var_Int8: Int8
// CHECK-LABEL: @objc var var_Int16: Int16
// CHECK-LABEL: @objc var var_Int32: Int32
// CHECK-LABEL: @objc var var_Int64: Int64

  var var_UInt8: UInt8
  var var_UInt16: UInt16
  var var_UInt32: UInt32
  var var_UInt64: UInt64
// CHECK-LABEL: @objc var var_UInt8: UInt8
// CHECK-LABEL: @objc var var_UInt16: UInt16
// CHECK-LABEL: @objc var var_UInt32: UInt32
// CHECK-LABEL: @objc var var_UInt64: UInt64

  var var_OpaquePointer: OpaquePointer
// CHECK-LABEL: @objc var var_OpaquePointer: OpaquePointer

  var var_PlainClass: PlainClass
// CHECK-LABEL: {{^}}  var var_PlainClass: PlainClass

  @objc var var_PlainClass_: PlainClass
  // expected-error@-1 {{property cannot be marked @objc because its type cannot be represented in Objective-C}}
  // expected-note@-2 {{classes not annotated with @objc cannot be represented in Objective-C}}

  var var_PlainStruct: PlainStruct
// CHECK-LABEL: {{^}}  var var_PlainStruct: PlainStruct

  @objc var var_PlainStruct_: PlainStruct
  // expected-error@-1 {{property cannot be marked @objc because its type cannot be represented in Objective-C}}
  // expected-note@-2 {{Swift structs cannot be represented in Objective-C}}

  var var_PlainEnum: PlainEnum
// CHECK-LABEL: {{^}} var var_PlainEnum: PlainEnum

  @objc var var_PlainEnum_: PlainEnum
  // expected-error@-1 {{property cannot be marked @objc because its type cannot be represented in Objective-C}}
  // expected-note@-2 {{non-'@objc' enums cannot be represented in Objective-C}}

  var var_PlainProtocol: PlainProtocol
// CHECK-LABEL: {{^}}  var var_PlainProtocol: PlainProtocol

  @objc var var_PlainProtocol_: PlainProtocol
  // expected-error@-1 {{property cannot be marked @objc because its type cannot be represented in Objective-C}}
  // expected-note@-2 {{protocol-constrained type containing protocol 'PlainProtocol' cannot be represented in Objective-C}}

  var var_ClassObjC: Class_ObjC1
// CHECK-LABEL: @objc var var_ClassObjC: Class_ObjC1

  @objc var var_ClassObjC_: Class_ObjC1 // no-error

  var var_ProtocolClass: Protocol_Class1
// CHECK-LABEL: {{^}}  var var_ProtocolClass: Protocol_Class1

  @objc var var_ProtocolClass_: Protocol_Class1
  // expected-error@-1 {{property cannot be marked @objc because its type cannot be represented in Objective-C}}
  // expected-note@-2 {{protocol-constrained type containing protocol 'Protocol_Class1' cannot be represented in Objective-C}}

  var var_ProtocolObjC: Protocol_ObjC1
// CHECK-LABEL: @objc var var_ProtocolObjC: Protocol_ObjC1

  @objc var var_ProtocolObjC_: Protocol_ObjC1 // no-error


  var var_PlainClassMetatype: PlainClass.Type
// CHECK-LABEL: {{^}}  var var_PlainClassMetatype: PlainClass.Type

  @objc var var_PlainClassMetatype_: PlainClass.Type
  // expected-error@-1 {{property cannot be marked @objc because its type cannot be represented in Objective-C}}

  var var_PlainStructMetatype: PlainStruct.Type
// CHECK-LABEL: {{^}}  var var_PlainStructMetatype: PlainStruct.Type

  @objc var var_PlainStructMetatype_: PlainStruct.Type
  // expected-error@-1 {{property cannot be marked @objc because its type cannot be represented in Objective-C}}

  var var_PlainEnumMetatype: PlainEnum.Type
// CHECK-LABEL: {{^}}  var var_PlainEnumMetatype: PlainEnum.Type

  @objc var var_PlainEnumMetatype_: PlainEnum.Type
  // expected-error@-1 {{property cannot be marked @objc because its type cannot be represented in Objective-C}}

  var var_PlainExistentialMetatype: PlainProtocol.Type
// CHECK-LABEL: {{^}}  var var_PlainExistentialMetatype: PlainProtocol.Type

  @objc var var_PlainExistentialMetatype_: PlainProtocol.Type
  // expected-error@-1 {{property cannot be marked @objc because its type cannot be represented in Objective-C}}

  var var_ClassObjCMetatype: Class_ObjC1.Type
// CHECK-LABEL: @objc var var_ClassObjCMetatype: Class_ObjC1.Type

  @objc var var_ClassObjCMetatype_: Class_ObjC1.Type // no-error

  var var_ProtocolClassMetatype: Protocol_Class1.Type
// CHECK-LABEL: {{^}}  var var_ProtocolClassMetatype: Protocol_Class1.Type

  @objc var var_ProtocolClassMetatype_: Protocol_Class1.Type
  // expected-error@-1 {{property cannot be marked @objc because its type cannot be represented in Objective-C}}

  var var_ProtocolObjCMetatype1: Protocol_ObjC1.Type
// CHECK-LABEL: @objc var var_ProtocolObjCMetatype1: Protocol_ObjC1.Type

  @objc var var_ProtocolObjCMetatype1_: Protocol_ObjC1.Type // no-error

  var var_ProtocolObjCMetatype2: Protocol_ObjC2.Type
// CHECK-LABEL: @objc var var_ProtocolObjCMetatype2: Protocol_ObjC2.Type

  @objc var var_ProtocolObjCMetatype2_: Protocol_ObjC2.Type // no-error

  var var_AnyObject1: AnyObject
  var var_AnyObject2: AnyObject.Type
// CHECK-LABEL: @objc var var_AnyObject1: AnyObject
// CHECK-LABEL: @objc var var_AnyObject2: AnyObject.Type

  var var_Existential0: Any
// CHECK-LABEL: @objc var var_Existential0: Any

  @objc var var_Existential0_: Any

  var var_Existential1: PlainProtocol
  // CHECK-LABEL: {{^}}  var var_Existential1: PlainProtocol

  @objc var var_Existential1_: PlainProtocol
  // expected-error@-1 {{property cannot be marked @objc because its type cannot be represented in Objective-C}}
  // expected-note@-2 {{protocol-constrained type containing protocol 'PlainProtocol' cannot be represented in Objective-C}}

  var var_Existential2: PlainProtocol & PlainProtocol
// CHECK-LABEL: {{^}}  var var_Existential2: PlainProtocol

  @objc var var_Existential2_: PlainProtocol & PlainProtocol
  // expected-error@-1 {{property cannot be marked @objc because its type cannot be represented in Objective-C}}
  // expected-note@-2 {{protocol-constrained type containing protocol 'PlainProtocol' cannot be represented in Objective-C}}

  var var_Existential3: PlainProtocol & Protocol_Class1
// CHECK-LABEL: {{^}}  var var_Existential3: PlainProtocol & Protocol_Class1

  @objc var var_Existential3_: PlainProtocol & Protocol_Class1
  // expected-error@-1 {{property cannot be marked @objc because its type cannot be represented in Objective-C}}
  // expected-note@-2 {{protocol-constrained type containing protocol 'PlainProtocol' cannot be represented in Objective-C}}

  var var_Existential4: PlainProtocol & Protocol_ObjC1
// CHECK-LABEL: {{^}}  var var_Existential4: PlainProtocol & Protocol_ObjC1

  @objc var var_Existential4_: PlainProtocol & Protocol_ObjC1
  // expected-error@-1 {{property cannot be marked @objc because its type cannot be represented in Objective-C}}
  // expected-note@-2 {{protocol-constrained type containing protocol 'PlainProtocol' cannot be represented in Objective-C}}

  var var_Existential5: Protocol_Class1
  // CHECK-LABEL: {{^}}  var var_Existential5: Protocol_Class1

  @objc var var_Existential5_: Protocol_Class1
  // expected-error@-1 {{property cannot be marked @objc because its type cannot be represented in Objective-C}}
  // expected-note@-2 {{protocol-constrained type containing protocol 'Protocol_Class1' cannot be represented in Objective-C}}

  var var_Existential6: Protocol_Class1 & Protocol_Class2
// CHECK-LABEL: {{^}}  var var_Existential6: Protocol_Class1 & Protocol_Class2

  @objc var var_Existential6_: Protocol_Class1 & Protocol_Class2
  // expected-error@-1 {{property cannot be marked @objc because its type cannot be represented in Objective-C}}
  // expected-note@-2 {{protocol-constrained type containing protocol 'Protocol_Class1' cannot be represented in Objective-C}}

  var var_Existential7: Protocol_Class1 & Protocol_ObjC1
// CHECK-LABEL: {{^}}  var var_Existential7: Protocol_Class1 & Protocol_ObjC1

  @objc var var_Existential7_: Protocol_Class1 & Protocol_ObjC1
  // expected-error@-1 {{property cannot be marked @objc because its type cannot be represented in Objective-C}}
  // expected-note@-2 {{protocol-constrained type containing protocol 'Protocol_Class1' cannot be represented in Objective-C}}

  var var_Existential8: Protocol_ObjC1
// CHECK-LABEL: @objc var var_Existential8: Protocol_ObjC1

  @objc var var_Existential8_: Protocol_ObjC1 // no-error

  var var_Existential9: Protocol_ObjC1 & Protocol_ObjC2
// CHECK-LABEL: @objc var var_Existential9: Protocol_ObjC1 & Protocol_ObjC2

  @objc var var_Existential9_: Protocol_ObjC1 & Protocol_ObjC2 // no-error


  var var_ExistentialMetatype0: Any.Type
  var var_ExistentialMetatype1: PlainProtocol.Type
  var var_ExistentialMetatype2: (PlainProtocol & PlainProtocol).Type
  var var_ExistentialMetatype3: (PlainProtocol & Protocol_Class1).Type
  var var_ExistentialMetatype4: (PlainProtocol & Protocol_ObjC1).Type
  var var_ExistentialMetatype5: (Protocol_Class1).Type
  var var_ExistentialMetatype6: (Protocol_Class1 & Protocol_Class2).Type
  var var_ExistentialMetatype7: (Protocol_Class1 & Protocol_ObjC1).Type
  var var_ExistentialMetatype8: Protocol_ObjC1.Type
  var var_ExistentialMetatype9: (Protocol_ObjC1 & Protocol_ObjC2).Type
// CHECK-LABEL: {{^}}  var var_ExistentialMetatype0: Any.Type
// CHECK-LABEL: {{^}}  var var_ExistentialMetatype1: PlainProtocol.Type
// CHECK-LABEL: {{^}}  var var_ExistentialMetatype2: (PlainProtocol).Type
// CHECK-LABEL: {{^}}  var var_ExistentialMetatype3: (PlainProtocol & Protocol_Class1).Type
// CHECK-LABEL: {{^}}  var var_ExistentialMetatype4: (PlainProtocol & Protocol_ObjC1).Type
// CHECK-LABEL: {{^}}  var var_ExistentialMetatype5: (Protocol_Class1).Type
// CHECK-LABEL: {{^}}  var var_ExistentialMetatype6: (Protocol_Class1 & Protocol_Class2).Type
// CHECK-LABEL: {{^}}  var var_ExistentialMetatype7: (Protocol_Class1 & Protocol_ObjC1).Type
// CHECK-LABEL: @objc var var_ExistentialMetatype8: Protocol_ObjC1.Type
// CHECK-LABEL: @objc var var_ExistentialMetatype9: (Protocol_ObjC1 & Protocol_ObjC2).Type


  var var_UnsafeMutablePointer1: UnsafeMutablePointer<Int>
  var var_UnsafeMutablePointer2: UnsafeMutablePointer<Bool>
  var var_UnsafeMutablePointer3: UnsafeMutablePointer<CBool>
  var var_UnsafeMutablePointer4: UnsafeMutablePointer<String>
  var var_UnsafeMutablePointer5: UnsafeMutablePointer<Float>
  var var_UnsafeMutablePointer6: UnsafeMutablePointer<Double>
  var var_UnsafeMutablePointer7: UnsafeMutablePointer<OpaquePointer>
  var var_UnsafeMutablePointer8: UnsafeMutablePointer<PlainClass>
  var var_UnsafeMutablePointer9: UnsafeMutablePointer<PlainStruct>
  var var_UnsafeMutablePointer10: UnsafeMutablePointer<PlainEnum>
  var var_UnsafeMutablePointer11: UnsafeMutablePointer<PlainProtocol>
  var var_UnsafeMutablePointer12: UnsafeMutablePointer<AnyObject>
  var var_UnsafeMutablePointer13: UnsafeMutablePointer<AnyObject.Type>
  var var_UnsafeMutablePointer100: UnsafeMutableRawPointer
  var var_UnsafeMutablePointer101: UnsafeMutableRawPointer
  var var_UnsafeMutablePointer102: UnsafeMutablePointer<(Int, Int)>
// CHECK-LABEL: @objc var var_UnsafeMutablePointer1: UnsafeMutablePointer<Int>
// CHECK-LABEL: @objc var var_UnsafeMutablePointer2: UnsafeMutablePointer<Bool>
// CHECK-LABEL: @objc var var_UnsafeMutablePointer3: UnsafeMutablePointer<CBool>
// CHECK-LABEL: {{^}}  var var_UnsafeMutablePointer4: UnsafeMutablePointer<String>
// CHECK-LABEL: @objc var var_UnsafeMutablePointer5: UnsafeMutablePointer<Float>
// CHECK-LABEL: @objc var var_UnsafeMutablePointer6: UnsafeMutablePointer<Double>
// CHECK-LABEL: @objc var var_UnsafeMutablePointer7: UnsafeMutablePointer<OpaquePointer>
// CHECK-LABEL: {{^}}  var var_UnsafeMutablePointer8: UnsafeMutablePointer<PlainClass>
// CHECK-LABEL: {{^}}  var var_UnsafeMutablePointer9: UnsafeMutablePointer<PlainStruct>
// CHECK-LABEL: {{^}}  var var_UnsafeMutablePointer10: UnsafeMutablePointer<PlainEnum>
// CHECK-LABEL: {{^}}  var var_UnsafeMutablePointer11: UnsafeMutablePointer<PlainProtocol>
// CHECK-LABEL: @objc var var_UnsafeMutablePointer12: UnsafeMutablePointer<AnyObject>
// CHECK-LABEL: var var_UnsafeMutablePointer13: UnsafeMutablePointer<AnyObject.Type>
// CHECK-LABEL: {{^}} @objc var var_UnsafeMutablePointer100: UnsafeMutableRawPointer
// CHECK-LABEL: {{^}} @objc var var_UnsafeMutablePointer101: UnsafeMutableRawPointer
// CHECK-LABEL: {{^}}  var var_UnsafeMutablePointer102: UnsafeMutablePointer<(Int, Int)>

  var var_Optional1: Class_ObjC1?
  var var_Optional2: Protocol_ObjC1?
  var var_Optional3: Class_ObjC1.Type?
  var var_Optional4: Protocol_ObjC1.Type?
  var var_Optional5: AnyObject?
  var var_Optional6: AnyObject.Type?
  var var_Optional7: String?
  var var_Optional8: Protocol_ObjC1?
  var var_Optional9: Protocol_ObjC1.Type?
  var var_Optional10: (Protocol_ObjC1 & Protocol_ObjC2)?
  var var_Optional11: (Protocol_ObjC1 & Protocol_ObjC2).Type?
  var var_Optional12: OpaquePointer?
  var var_Optional13: UnsafeMutablePointer<Int>?
  var var_Optional14: UnsafeMutablePointer<Class_ObjC1>?

// CHECK-LABEL: @objc @_hasInitialValue var var_Optional1: Class_ObjC1?
// CHECK-LABEL: @objc @_hasInitialValue var var_Optional2: Protocol_ObjC1?
// CHECK-LABEL: @objc @_hasInitialValue var var_Optional3: Class_ObjC1.Type?
// CHECK-LABEL: @objc @_hasInitialValue var var_Optional4: Protocol_ObjC1.Type?
// CHECK-LABEL: @objc @_hasInitialValue var var_Optional5: AnyObject?
// CHECK-LABEL: @objc @_hasInitialValue var var_Optional6: AnyObject.Type?
// CHECK-LABEL: @objc @_hasInitialValue var var_Optional7: String?
// CHECK-LABEL: @objc @_hasInitialValue var var_Optional8: Protocol_ObjC1?
// CHECK-LABEL: @objc @_hasInitialValue var var_Optional9: Protocol_ObjC1.Type?
// CHECK-LABEL: @objc @_hasInitialValue var var_Optional10: (Protocol_ObjC1 & Protocol_ObjC2)?
// CHECK-LABEL: @objc @_hasInitialValue var var_Optional11: (Protocol_ObjC1 & Protocol_ObjC2).Type?
// CHECK-LABEL: @objc @_hasInitialValue var var_Optional12: OpaquePointer?
// CHECK-LABEL: @objc @_hasInitialValue var var_Optional13: UnsafeMutablePointer<Int>?
// CHECK-LABEL: @objc @_hasInitialValue var var_Optional14: UnsafeMutablePointer<Class_ObjC1>?


  var var_ImplicitlyUnwrappedOptional1: Class_ObjC1!
  var var_ImplicitlyUnwrappedOptional2: Protocol_ObjC1!
  var var_ImplicitlyUnwrappedOptional3: Class_ObjC1.Type!
  var var_ImplicitlyUnwrappedOptional4: Protocol_ObjC1.Type!
  var var_ImplicitlyUnwrappedOptional5: AnyObject!
  var var_ImplicitlyUnwrappedOptional6: AnyObject.Type!
  var var_ImplicitlyUnwrappedOptional7: String!
  var var_ImplicitlyUnwrappedOptional8: Protocol_ObjC1!
  var var_ImplicitlyUnwrappedOptional9: (Protocol_ObjC1 & Protocol_ObjC2)!

// CHECK-LABEL: @objc @_hasInitialValue var var_ImplicitlyUnwrappedOptional1: Class_ObjC1!
// CHECK-LABEL: @objc @_hasInitialValue var var_ImplicitlyUnwrappedOptional2: Protocol_ObjC1!
// CHECK-LABEL: @objc @_hasInitialValue var var_ImplicitlyUnwrappedOptional3: Class_ObjC1.Type!
// CHECK-LABEL: @objc @_hasInitialValue var var_ImplicitlyUnwrappedOptional4: Protocol_ObjC1.Type!
// CHECK-LABEL: @objc @_hasInitialValue var var_ImplicitlyUnwrappedOptional5: AnyObject!
// CHECK-LABEL: @objc @_hasInitialValue var var_ImplicitlyUnwrappedOptional6: AnyObject.Type!
// CHECK-LABEL: @objc @_hasInitialValue var var_ImplicitlyUnwrappedOptional7: String!
// CHECK-LABEL: @objc @_hasInitialValue var var_ImplicitlyUnwrappedOptional8: Protocol_ObjC1!
// CHECK-LABEL: @objc @_hasInitialValue var var_ImplicitlyUnwrappedOptional9: (Protocol_ObjC1 & Protocol_ObjC2)!

  var var_Optional_fail1: PlainClass?
  var var_Optional_fail2: PlainClass.Type?
  var var_Optional_fail3: PlainClass!
  var var_Optional_fail4: PlainStruct?
  var var_Optional_fail5: PlainStruct.Type?
  var var_Optional_fail6: PlainEnum?
  var var_Optional_fail7: PlainEnum.Type?
  var var_Optional_fail8: PlainProtocol?
  var var_Optional_fail10: PlainProtocol?
  var var_Optional_fail11: (PlainProtocol & Protocol_ObjC1)?
  var var_Optional_fail12: Int?
  var var_Optional_fail13: Bool?
  var var_Optional_fail14: CBool?
  var var_Optional_fail20: AnyObject??
  var var_Optional_fail21: AnyObject.Type??
  var var_Optional_fail22: ComparisonResult? // a non-bridged imported value type
  var var_Optional_fail23: NSRange? // a bridged struct imported from C
// CHECK-NOT: @objc{{.*}}Optional_fail

  // CHECK-LABEL: @objc var var_CFunctionPointer_1: @convention(c) () -> ()
  var var_CFunctionPointer_1: @convention(c) () -> ()
  // CHECK-LABEL: @objc var var_CFunctionPointer_invalid_1: Int
  var var_CFunctionPointer_invalid_1: @convention(c) Int // expected-error {{@convention attribute only applies to function types}}
  // CHECK-LABEL: {{^}} var var_CFunctionPointer_invalid_2: @convention(c) (PlainStruct) -> Int
  var var_CFunctionPointer_invalid_2: @convention(c) (PlainStruct) -> Int // expected-error {{'(PlainStruct) -> Int' is not representable in Objective-C, so it cannot be used with '@convention(c)'}}
  
  // <rdar://problem/20918869> Confusing diagnostic for @convention(c) throws
  var var_CFunctionPointer_invalid_3 : @convention(c) (Int) throws -> Int // expected-error {{'(Int) throws -> Int' is not representable in Objective-C, so it cannot be used with '@convention(c)'}}

  weak var var_Weak1: Class_ObjC1?
  weak var var_Weak2: Protocol_ObjC1?
  // <rdar://problem/16473062> weak and unowned variables of metatypes are rejected
  //weak var var_Weak3: Class_ObjC1.Type?
  //weak var var_Weak4: Protocol_ObjC1.Type?
  weak var var_Weak5: AnyObject?
  //weak var var_Weak6: AnyObject.Type?
  weak var var_Weak7: Protocol_ObjC1?
  weak var var_Weak8: (Protocol_ObjC1 & Protocol_ObjC2)?

// CHECK-LABEL: @objc @_hasInitialValue weak var var_Weak1: @sil_weak Class_ObjC1
// CHECK-LABEL: @objc @_hasInitialValue weak var var_Weak2: @sil_weak Protocol_ObjC1
// CHECK-LABEL: @objc @_hasInitialValue weak var var_Weak5: @sil_weak AnyObject
// CHECK-LABEL: @objc @_hasInitialValue weak var var_Weak7: @sil_weak Protocol_ObjC1
// CHECK-LABEL: @objc @_hasInitialValue weak var var_Weak8: @sil_weak (Protocol_ObjC1 & Protocol_ObjC2)?

  weak var var_Weak_fail1: PlainClass?
  weak var var_Weak_bad2: PlainStruct?
  // expected-error@-1 {{'weak' may only be applied to class and class-bound protocol types, not 'PlainStruct'}}

  weak var var_Weak_bad3: PlainEnum?
  // expected-error@-1 {{'weak' may only be applied to class and class-bound protocol types, not 'PlainEnum'}}
  weak var var_Weak_bad4: String?
  // expected-error@-1 {{'weak' may only be applied to class and class-bound protocol types, not 'String'}}
// CHECK-NOT: @objc{{.*}}Weak_fail


  unowned var var_Unowned1: Class_ObjC1
  unowned var var_Unowned2: Protocol_ObjC1
  // <rdar://problem/16473062> weak and unowned variables of metatypes are rejected
  //unowned var var_Unowned3: Class_ObjC1.Type
  //unowned var var_Unowned4: Protocol_ObjC1.Type
  unowned var var_Unowned5: AnyObject
  //unowned var var_Unowned6: AnyObject.Type
  unowned var var_Unowned7: Protocol_ObjC1
  unowned var var_Unowned8: Protocol_ObjC1 & Protocol_ObjC2

// CHECK-LABEL: @objc unowned var var_Unowned1: @sil_unowned Class_ObjC1
// CHECK-LABEL: @objc unowned var var_Unowned2: @sil_unowned Protocol_ObjC1
// CHECK-LABEL: @objc unowned var var_Unowned5: @sil_unowned AnyObject
// CHECK-LABEL: @objc unowned var var_Unowned7: @sil_unowned Protocol_ObjC1
// CHECK-LABEL: @objc unowned var var_Unowned8: @sil_unowned Protocol_ObjC1 & Protocol_ObjC2


  unowned var var_Unowned_fail1: PlainClass
  unowned var var_Unowned_bad2: PlainStruct
  // expected-error@-1 {{'unowned' may only be applied to class and class-bound protocol types, not 'PlainStruct'}}
  unowned var var_Unowned_bad3: PlainEnum
  // expected-error@-1 {{'unowned' may only be applied to class and class-bound protocol types, not 'PlainEnum'}}
  unowned var var_Unowned_bad4: String
  // expected-error@-1 {{'unowned' may only be applied to class and class-bound protocol types, not 'String'}}
// CHECK-NOT: @objc{{.*}}Unowned_fail


  var var_FunctionType1: () -> ()
// CHECK-LABEL: {{^}}  @objc var var_FunctionType1: () -> ()

  var var_FunctionType2: (Int) -> ()
// CHECK-LABEL: {{^}}  @objc var var_FunctionType2: (Int) -> ()

  var var_FunctionType3: (Int) -> Int
// CHECK-LABEL: {{^}}  @objc var var_FunctionType3: (Int) -> Int

  var var_FunctionType4: (Int, Double) -> ()
// CHECK-LABEL: {{^}}  @objc var var_FunctionType4: (Int, Double) -> ()

  var var_FunctionType5: (String) -> ()
// CHECK-LABEL: {{^}}  @objc var var_FunctionType5: (String) -> ()

  var var_FunctionType6: () -> String
// CHECK-LABEL: {{^}}  @objc var var_FunctionType6: () -> String

  var var_FunctionType7: (PlainClass) -> ()
// CHECK-NOT: @objc var var_FunctionType7: (PlainClass) -> ()

  var var_FunctionType8: () -> PlainClass
// CHECK-NOT: @objc var var_FunctionType8: () -> PlainClass

  var var_FunctionType9: (PlainStruct) -> ()
// CHECK-LABEL: {{^}}  var var_FunctionType9: (PlainStruct) -> ()

  var var_FunctionType10: () -> PlainStruct
// CHECK-LABEL: {{^}}  var var_FunctionType10: () -> PlainStruct

  var var_FunctionType11: (PlainEnum) -> ()
// CHECK-LABEL: {{^}}  var var_FunctionType11: (PlainEnum) -> ()

  var var_FunctionType12: (PlainProtocol) -> ()
// CHECK-LABEL: {{^}}  var var_FunctionType12: (PlainProtocol) -> ()

  var var_FunctionType13: (Class_ObjC1) -> ()
// CHECK-LABEL: {{^}}  @objc var var_FunctionType13: (Class_ObjC1) -> ()

  var var_FunctionType14: (Protocol_Class1) -> ()
// CHECK-LABEL: {{^}}  var var_FunctionType14: (Protocol_Class1) -> ()

  var var_FunctionType15: (Protocol_ObjC1) -> ()
// CHECK-LABEL: {{^}}  @objc var var_FunctionType15: (Protocol_ObjC1) -> ()

  var var_FunctionType16: (AnyObject) -> ()
// CHECK-LABEL: {{^}}  @objc var var_FunctionType16: (AnyObject) -> ()

  var var_FunctionType17: (() -> ()) -> ()
// CHECK-LABEL: {{^}}  @objc var var_FunctionType17: (() -> ()) -> ()

  var var_FunctionType18: ((Int) -> (), Int) -> ()
// CHECK-LABEL: {{^}}  @objc var var_FunctionType18: ((Int) -> (), Int) -> ()

  var var_FunctionType19: ((String) -> (), Int) -> ()
// CHECK-LABEL: {{^}}  @objc var var_FunctionType19: ((String) -> (), Int) -> ()


  var var_FunctionTypeReturn1: () -> () -> ()
// CHECK-LABEL: {{^}}  @objc var var_FunctionTypeReturn1: () -> () -> ()

  @objc var var_FunctionTypeReturn1_: () -> () -> () // no-error

  var var_FunctionTypeReturn2: () -> (Int) -> ()
// CHECK-LABEL: {{^}}  @objc var var_FunctionTypeReturn2: () -> (Int) -> ()

  @objc var var_FunctionTypeReturn2_: () -> (Int) -> () // no-error

  var var_FunctionTypeReturn3: () -> () -> Int
// CHECK-LABEL: {{^}}  @objc var var_FunctionTypeReturn3: () -> () -> Int

  @objc var var_FunctionTypeReturn3_: () -> () -> Int // no-error

  var var_FunctionTypeReturn4: () -> (String) -> ()
// CHECK-LABEL: {{^}}  @objc var var_FunctionTypeReturn4: () -> (String) -> ()

  @objc var var_FunctionTypeReturn4_: () -> (String) -> () // no-error

  var var_FunctionTypeReturn5: () -> () -> String
// CHECK-LABEL: {{^}}  @objc var var_FunctionTypeReturn5: () -> () -> String

  @objc var var_FunctionTypeReturn5_: () -> () -> String // no-error


  var var_BlockFunctionType1: @convention(block) () -> ()
// CHECK-LABEL: @objc var var_BlockFunctionType1: @convention(block) () -> ()

  @objc var var_BlockFunctionType1_: @convention(block) () -> () // no-error

  var var_ArrayType1: [AnyObject]
  // CHECK-LABEL: {{^}}  @objc var var_ArrayType1: [AnyObject]

  @objc var var_ArrayType1_: [AnyObject] // no-error

  var var_ArrayType2: [@convention(block) (AnyObject) -> AnyObject] // no-error
  // CHECK-LABEL: {{^}}  @objc var var_ArrayType2: [@convention(block) (AnyObject) -> AnyObject]

  @objc var var_ArrayType2_: [@convention(block) (AnyObject) -> AnyObject] // no-error

  var var_ArrayType3: [PlainStruct]
  // CHECK-LABEL: {{^}}  var var_ArrayType3: [PlainStruct]

  @objc var var_ArrayType3_: [PlainStruct]
  // expected-error @-1{{property cannot be marked @objc because its type cannot be represented in Objective-C}}

  var var_ArrayType4: [(AnyObject) -> AnyObject] // no-error
  // CHECK-LABEL: {{^}}  var var_ArrayType4: [(AnyObject) -> AnyObject]

  @objc var var_ArrayType4_: [(AnyObject) -> AnyObject]
  // expected-error @-1{{property cannot be marked @objc because its type cannot be represented in Objective-C}}

  var var_ArrayType5: [Protocol_ObjC1]
  // CHECK-LABEL: {{^}}  @objc var var_ArrayType5: [Protocol_ObjC1]

  @objc var var_ArrayType5_: [Protocol_ObjC1] // no-error

  var var_ArrayType6: [Class_ObjC1]
  // CHECK-LABEL: {{^}}  @objc var var_ArrayType6: [Class_ObjC1]

  @objc var var_ArrayType6_: [Class_ObjC1] // no-error

  var var_ArrayType7: [PlainClass]
  // CHECK-LABEL: {{^}}  var var_ArrayType7: [PlainClass]

  @objc var var_ArrayType7_: [PlainClass]
  // expected-error @-1{{property cannot be marked @objc because its type cannot be represented in Objective-C}}

  var var_ArrayType8: [PlainProtocol]
  // CHECK-LABEL: {{^}}  var var_ArrayType8: [PlainProtocol]

  @objc var var_ArrayType8_: [PlainProtocol]
  // expected-error @-1{{property cannot be marked @objc because its type cannot be represented in Objective-C}}

  var var_ArrayType9: [Protocol_ObjC1 & PlainProtocol]
  // CHECK-LABEL: {{^}}  var var_ArrayType9: [PlainProtocol & Protocol_ObjC1]

  @objc var var_ArrayType9_: [Protocol_ObjC1 & PlainProtocol]
  // expected-error @-1{{property cannot be marked @objc because its type cannot be represented in Objective-C}}

  var var_ArrayType10: [Protocol_ObjC1 & Protocol_ObjC2]
  // CHECK-LABEL: {{^}}  @objc var var_ArrayType10: [Protocol_ObjC1 & Protocol_ObjC2]

  @objc var var_ArrayType10_: [Protocol_ObjC1 & Protocol_ObjC2]
  // no-error

  var var_ArrayType11: [Any]
  // CHECK-LABEL: @objc var var_ArrayType11: [Any]

  @objc var var_ArrayType11_: [Any]

  var var_ArrayType13: [Any?]
  // CHECK-LABEL: {{^}}  var var_ArrayType13: [Any?]

  @objc var var_ArrayType13_: [Any?]
  // expected-error @-1{{property cannot be marked @objc because its type cannot be represented in Objective-C}}

  var var_ArrayType15: [AnyObject?]
  // CHECK-LABEL: {{^}}  var var_ArrayType15: [AnyObject?]

  @objc var var_ArrayType15_: [AnyObject?]
  // expected-error @-1{{property cannot be marked @objc because its type cannot be represented in Objective-C}}

  var var_ArrayType16: [[@convention(block) (AnyObject) -> AnyObject]] // no-error
  // CHECK-LABEL: {{^}}  @objc var var_ArrayType16: {{\[}}[@convention(block) (AnyObject) -> AnyObject]]

  @objc var var_ArrayType16_: [[@convention(block) (AnyObject) -> AnyObject]] // no-error

  var var_ArrayType17: [[(AnyObject) -> AnyObject]] // no-error
  // CHECK-LABEL: {{^}}  var var_ArrayType17: {{\[}}[(AnyObject) -> AnyObject]]

  @objc var var_ArrayType17_: [[(AnyObject) -> AnyObject]]
  // expected-error @-1{{property cannot be marked @objc because its type cannot be represented in Objective-C}}
}

@objc
class ObjCBase {}

class infer_instanceVar2<
    GP_Unconstrained,
    GP_PlainClass : PlainClass,
    GP_PlainProtocol : PlainProtocol,
    GP_Class_ObjC : Class_ObjC1,
    GP_Protocol_Class : Protocol_Class1,
    GP_Protocol_ObjC : Protocol_ObjC1> : ObjCBase {
// CHECK-LABEL: class infer_instanceVar2<{{.*}}> : ObjCBase where {{.*}} {
  override init() {}

  var var_GP_Unconstrained: GP_Unconstrained
// CHECK-LABEL: {{^}}  var var_GP_Unconstrained: GP_Unconstrained

  @objc var var_GP_Unconstrained_: GP_Unconstrained
  // expected-error@-1 {{property cannot be marked @objc because its type cannot be represented in Objective-C}}
  // expected-note@-2 {{generic type parameters cannot be represented in Objective-C}}

  var var_GP_PlainClass: GP_PlainClass
// CHECK-LABEL: {{^}}  var var_GP_PlainClass: GP_PlainClass

  @objc var var_GP_PlainClass_: GP_PlainClass
  // expected-error@-1 {{property cannot be marked @objc because its type cannot be represented in Objective-C}}
  // expected-note@-2 {{generic type parameters cannot be represented in Objective-C}}

  var var_GP_PlainProtocol: GP_PlainProtocol
// CHECK-LABEL: {{^}}  var var_GP_PlainProtocol: GP_PlainProtocol

  @objc var var_GP_PlainProtocol_: GP_PlainProtocol
  // expected-error@-1 {{property cannot be marked @objc because its type cannot be represented in Objective-C}}
  // expected-note@-2 {{generic type parameters cannot be represented in Objective-C}}

  var var_GP_Class_ObjC: GP_Class_ObjC
// CHECK-LABEL: {{^}}  var var_GP_Class_ObjC: GP_Class_ObjC

  @objc var var_GP_Class_ObjC_: GP_Class_ObjC
  // expected-error@-1 {{property cannot be marked @objc because its type cannot be represented in Objective-C}}
  // expected-note@-2 {{generic type parameters cannot be represented in Objective-C}}

  var var_GP_Protocol_Class: GP_Protocol_Class
// CHECK-LABEL: {{^}}  var var_GP_Protocol_Class: GP_Protocol_Class

  @objc var var_GP_Protocol_Class_: GP_Protocol_Class
  // expected-error@-1 {{property cannot be marked @objc because its type cannot be represented in Objective-C}}
  // expected-note@-2 {{generic type parameters cannot be represented in Objective-C}}

  var var_GP_Protocol_ObjC: GP_Protocol_ObjC
// CHECK-LABEL: {{^}}  var var_GP_Protocol_ObjC: GP_Protocol_ObjC

  @objc var var_GP_Protocol_ObjCa: GP_Protocol_ObjC
  // expected-error@-1 {{property cannot be marked @objc because its type cannot be represented in Objective-C}}
  // expected-note@-2 {{generic type parameters cannot be represented in Objective-C}}

  func func_GP_Unconstrained(a: GP_Unconstrained) {}
// CHECK-LABEL: {{^}} func func_GP_Unconstrained(a: GP_Unconstrained) {

  @objc func func_GP_Unconstrained_(a: GP_Unconstrained) {}
  // expected-error@-1 {{method cannot be marked @objc because the type of the parameter cannot be represented in Objective-C}}
  // expected-note@-2 {{generic type parameters cannot be represented in Objective-C}}

  @objc func func_GP_Unconstrained_() -> GP_Unconstrained {}
  // expected-error@-1 {{method cannot be marked @objc because its result type cannot be represented in Objective-C}}
  // expected-note@-2 {{generic type parameters cannot be represented in Objective-C}}

  @objc func func_GP_Class_ObjC__() -> GP_Class_ObjC {}
  // expected-error@-1 {{method cannot be marked @objc because its result type cannot be represented in Objective-C}}
  // expected-note@-2 {{generic type parameters cannot be represented in Objective-C}}
}

class infer_instanceVar3 : Class_ObjC1 {
// CHECK-LABEL: @objc @_inheritsConvenienceInitializers class infer_instanceVar3 : Class_ObjC1 {

  var v1: Int = 0
// CHECK-LABEL: @objc @_hasInitialValue var v1: Int
}


@objc
protocol infer_instanceVar4 {
// CHECK-LABEL: @objc protocol infer_instanceVar4 {

  var v1: Int { get }
// CHECK-LABEL: @objc var v1: Int { get }
}

// @!objc
class infer_instanceVar5 {
// CHECK-LABEL: {{^}}class infer_instanceVar5 {

  @objc
  var intstanceVar1: Int {
  // CHECK: @objc var intstanceVar1: Int
    get {}
    // CHECK: @objc get {}
    set {}
    // CHECK: @objc set {}
  }
}

@objc
class infer_staticVar1 {
// CHECK-LABEL: @objc class infer_staticVar1 {

  class var staticVar1: Int = 42 // expected-error {{class stored properties not supported}}
  // CHECK: @objc @_hasInitialValue class var staticVar1: Int
}

// @!objc
class infer_subscript1 {
// CHECK-LABEL: class infer_subscript1

  @objc
  subscript(i: Int) -> Int {
  // CHECK: @objc subscript(i: Int) -> Int
    get {}
    // CHECK: @objc get {}
    set {}
    // CHECK: @objc set {}
  }
}


@objc
protocol infer_throughConformanceProto1 {
// CHECK-LABEL: @objc protocol infer_throughConformanceProto1 {

  func funcObjC1()
  var varObjC1: Int { get }
  var varObjC2: Int { get set }
  // CHECK: @objc func funcObjC1()
  // CHECK: @objc var varObjC1: Int { get }
  // CHECK: @objc var varObjC2: Int { get set }
}

class infer_class1 : PlainClass {}
// CHECK-LABEL: {{^}}@_inheritsConvenienceInitializers class infer_class1 : PlainClass {

class infer_class2 : Class_ObjC1 {}
// CHECK-LABEL: @objc @_inheritsConvenienceInitializers class infer_class2 : Class_ObjC1 {

class infer_class3 : infer_class2 {}
// CHECK-LABEL: @objc @_inheritsConvenienceInitializers class infer_class3 : infer_class2 {

class infer_class4 : Protocol_Class1 {}
// CHECK-LABEL: {{^}}class infer_class4 : Protocol_Class1 {

class infer_class5 : Protocol_ObjC1 {}
// CHECK-LABEL: {{^}}class infer_class5 : Protocol_ObjC1 {

//
// If a protocol conforms to an @objc protocol, this does not infer @objc on
// the protocol itself, or on the newly introduced requirements.  Only the
// inherited @objc requirements get @objc.
//
// Same rule applies to classes.
//

protocol infer_protocol1 {
// CHECK-LABEL: {{^}}protocol infer_protocol1 {
  func nonObjC1()
  // CHECK: {{^}} func nonObjC1()
}

protocol infer_protocol2 : Protocol_Class1 {
// CHECK-LABEL: {{^}}protocol infer_protocol2 : Protocol_Class1 {
  func nonObjC1()
  // CHECK: {{^}} func nonObjC1()
}

protocol infer_protocol3 : Protocol_ObjC1 {
// CHECK-LABEL: {{^}}protocol infer_protocol3 : Protocol_ObjC1 {
  func nonObjC1()
  // CHECK: {{^}} func nonObjC1()
}

protocol infer_protocol4 : Protocol_Class1, Protocol_ObjC1 {
// CHECK-LABEL: {{^}}protocol infer_protocol4 : Protocol_Class1, Protocol_ObjC1 {
  func nonObjC1()
  // CHECK: {{^}} func nonObjC1()
}

protocol infer_protocol5 : Protocol_ObjC1, Protocol_Class1 {
// CHECK-LABEL: {{^}}protocol infer_protocol5 : Protocol_Class1, Protocol_ObjC1 {
  func nonObjC1()
  // CHECK: {{^}} func nonObjC1()
}

class C {
  // Don't crash.
  @objc func foo(x: Undeclared) {} // expected-error {{cannot find type 'Undeclared' in scope}}
  @IBAction func myAction(sender: Undeclared) {} // expected-error {{cannot find type 'Undeclared' in scope}}
  @IBSegueAction func myAction(coder: Undeclared, sender: Undeclared) -> Undeclared {fatalError()} // expected-error {{cannot find type 'Undeclared' in scope}} expected-error {{cannot find type 'Undeclared' in scope}} expected-error {{cannot find type 'Undeclared' in scope}}
}

//===---
//===--- @IBOutlet implies @objc
//===---

class HasIBOutlet {
// CHECK-LABEL: {{^}}class HasIBOutlet {

  init() {}

  @IBOutlet weak var goodOutlet: Class_ObjC1!
  // CHECK-LABEL: {{^}} @objc @IBOutlet @_hasInitialValue weak var goodOutlet: @sil_weak Class_ObjC1!

  @IBOutlet var badOutlet: PlainStruct
  // expected-error@-1 {{@IBOutlet property cannot have non-object type 'PlainStruct'}} {{3-13=}}
  // expected-error@-2 {{@IBOutlet property has non-optional type 'PlainStruct'}}
  // expected-note@-3 {{add '?' to form the optional type 'PlainStruct?'}}
  // expected-note@-4 {{add '!' to form an implicitly unwrapped optional}}
  // CHECK-LABEL: {{^}}  @IBOutlet var badOutlet: PlainStruct
}

//===---
//===--- @IBAction implies @objc
//===---

// CHECK-LABEL: {{^}}class HasIBAction {
class HasIBAction {
  @IBAction func goodAction(_ sender: AnyObject?) { }
  // CHECK: {{^}}  @objc @IBAction func goodAction(_ sender: AnyObject?) {

  @IBAction func badAction(_ sender: PlainStruct?) { }
  // expected-error@-1{{method cannot be marked @IBAction because the type of the parameter cannot be represented in Objective-C}}
}

//===---
//===--- @IBSegueAction implies @objc
//===---

// CHECK-LABEL: {{^}}class HasIBSegueAction {
class HasIBSegueAction {
  @IBSegueAction func goodSegueAction(_ coder: AnyObject) -> AnyObject {fatalError()}
  // CHECK: {{^}}  @objc @IBSegueAction func goodSegueAction(_ coder: AnyObject) -> AnyObject {

  @IBSegueAction func badSegueAction(_ coder: PlainStruct?) -> Int? {fatalError()}
  // expected-error@-1{{method cannot be marked @IBSegueAction because the type of the parameter cannot be represented in Objective-C}}
}

//===---
//===--- @IBInspectable implies @objc
//===---

// CHECK-LABEL: {{^}}class HasIBInspectable {
class HasIBInspectable {
  @IBInspectable var goodProperty: AnyObject?
  // CHECK: {{^}}  @objc @IBInspectable @_hasInitialValue var goodProperty: AnyObject?
}

//===---
//===--- @GKInspectable implies @objc
//===---

// CHECK-LABEL: {{^}}class HasGKInspectable {
class HasGKInspectable {
  @GKInspectable var goodProperty: AnyObject?
  // CHECK: {{^}}  @objc @GKInspectable @_hasInitialValue var goodProperty: AnyObject?
}

//===---
//===--- @NSManaged implies @objc
//===---

class HasNSManaged {
// CHECK-LABEL: {{^}}class HasNSManaged {

  init() {}

  @NSManaged
  var goodManaged: Class_ObjC1
  // CHECK-LABEL: {{^}}  @objc @NSManaged dynamic var goodManaged: Class_ObjC1 {
  // CHECK-NEXT: {{^}} @objc get
  // CHECK-NEXT: {{^}} @objc set
  // CHECK-NEXT: {{^}} }

  @NSManaged
  var badManaged: PlainStruct
  // expected-error@-1 {{property cannot be marked @NSManaged because its type cannot be represented in Objective-C}}
  // expected-note@-2 {{Swift structs cannot be represented in Objective-C}}
  // CHECK-LABEL: {{^}}  @NSManaged dynamic var badManaged: PlainStruct {
  // CHECK-NEXT: {{^}} get
  // CHECK-NEXT: {{^}} set
  // CHECK-NEXT: {{^}} }
}

//===---
//===--- Pointer argument types
//===---

@objc class TakesCPointers {
// CHECK-LABEL: {{^}}@objc class TakesCPointers {
  func constUnsafeMutablePointer(p: UnsafePointer<Int>) {}
  // CHECK-LABEL: @objc func constUnsafeMutablePointer(p: UnsafePointer<Int>) {

  func constUnsafeMutablePointerToAnyObject(p: UnsafePointer<AnyObject>) {}
  // CHECK-LABEL: @objc func constUnsafeMutablePointerToAnyObject(p: UnsafePointer<AnyObject>) {

  func constUnsafeMutablePointerToClass(p: UnsafePointer<TakesCPointers>) {}
  // CHECK-LABEL: @objc func constUnsafeMutablePointerToClass(p: UnsafePointer<TakesCPointers>) {

  func mutableUnsafeMutablePointer(p: UnsafeMutablePointer<Int>) {}
  // CHECK-LABEL: @objc func mutableUnsafeMutablePointer(p: UnsafeMutablePointer<Int>) {

  func mutableStrongUnsafeMutablePointerToAnyObject(p: UnsafeMutablePointer<AnyObject>) {}
  // CHECK-LABEL: {{^}} @objc func mutableStrongUnsafeMutablePointerToAnyObject(p: UnsafeMutablePointer<AnyObject>) {

  func mutableAutoreleasingUnsafeMutablePointerToAnyObject(p: AutoreleasingUnsafeMutablePointer<AnyObject>) {}
  // CHECK-LABEL: {{^}} @objc func mutableAutoreleasingUnsafeMutablePointerToAnyObject(p: AutoreleasingUnsafeMutablePointer<AnyObject>) {
  }

// @objc with nullary names
@objc(NSObjC2)
class Class_ObjC2 {
// CHECK-LABEL: @objc(NSObjC2) class Class_ObjC2

  @objc(initWithMalice)
  init(foo: ()) { }

  @objc(initWithIntent)
  init(bar _: ()) { }

  @objc(initForMurder)
  init() { }

  @objc(isFoo)
  func foo() -> Bool {}
  // CHECK-LABEL: @objc(isFoo) func foo() -> Bool {
}

@objc() // expected-error{{expected name within parentheses of @objc attribute}}
class Class_ObjC3 { 
}

// @objc with selector names
extension PlainClass {
  // CHECK-LABEL: @objc(setFoo:) dynamic func
  @objc(setFoo:)
  func foo(b: Bool) { }

  // CHECK-LABEL: @objc(setWithRed:green:blue:alpha:) dynamic func set
  @objc(setWithRed:green:blue:alpha:)
  func set(_: Float, green: Float, blue: Float, alpha: Float) { }

  // CHECK-LABEL: @objc(createWithRed:green:blue:alpha:) dynamic class func createWith
  @objc(createWithRed:green blue:alpha)
  class func createWithRed(_: Float, green: Float, blue: Float, alpha: Float) { }
  // expected-error@-2{{missing ':' after selector piece in @objc attribute}}{{28-28=:}}
  // expected-error@-3{{missing ':' after selector piece in @objc attribute}}{{39-39=:}}

  // CHECK-LABEL: @objc(::) dynamic func badlyNamed
  @objc(::)
  func badlyNamed(_: Int, y: Int) {}
}

@objc(Class:) // expected-error{{'@objc' class must have a simple name}}{{12-13=}}
class BadClass1 { }

@objc(Protocol:) // expected-error{{'@objc' protocol must have a simple name}}{{15-16=}}
protocol BadProto1 { }

@objc(Enum:) // expected-error{{'@objc' enum must have a simple name}}{{11-12=}}
enum BadEnum1: Int { case X }

@objc
enum BadEnum2: Int {
  @objc(X:)   // expected-error{{'@objc' enum case must have a simple name}}{{10-11=}}
  case X
}

class BadClass2 {
  @objc(realDealloc) // expected-error{{'@objc' deinitializer cannot have a name}}
  deinit { }

  @objc(badprop:foo:wibble:) // expected-error{{'@objc' property must have a simple name}}{{16-28=}}
  var badprop: Int = 5

  @objc(foo) // expected-error{{'@objc' subscript cannot have a name; did you mean to put the name on the getter or setter?}}
  subscript (i: Int) -> Int {
    get {
      return i
    }
  }

  @objc(foo) // expected-error{{'@objc' method name provides names for 0 arguments, but method has one parameter}}
  func noArgNamesOneParam(x: Int) { }
  
  @objc(foo) // expected-error{{'@objc' method name provides names for 0 arguments, but method has one parameter}}
  func noArgNamesOneParam2(_: Int) { }

  @objc(foo) // expected-error{{'@objc' method name provides names for 0 arguments, but method has 2 parameters}}
  func noArgNamesTwoParams(_: Int, y: Int) { }

  @objc(foo:) // expected-error{{'@objc' method name provides one argument name, but method has 2 parameters}}
  func oneArgNameTwoParams(_: Int, y: Int) { }

  @objc(foo:) // expected-error{{'@objc' method name provides one argument name, but method has 0 parameters}}
  func oneArgNameNoParams() { }

  @objc(foo:) // expected-error{{'@objc' initializer name provides one argument name, but initializer has 0 parameters}}
  init() { }

  var _prop = 5
  @objc var prop: Int {
    @objc(property) get { return _prop }
    @objc(setProperty:) set { _prop = newValue }
  }

  var prop2: Int {
    @objc(property) get { return _prop } // expected-error{{'@objc' getter for non-'@objc' property}}
    @objc(setProperty:) set { _prop = newValue } // expected-error{{'@objc' setter for non-'@objc' property}}
  }

  var prop3: Int {
    @objc(setProperty:) didSet { } // expected-error{{observing accessors are not allowed to be marked @objc}} {{5-25=}}
  }

  @objc
  subscript (c: Class_ObjC1) -> Class_ObjC1 {
    @objc(getAtClass:) get {
      return c
    }

    @objc(setAtClass:class:) set {
    }
  }
}

// Swift overrides that aren't also @objc overrides.
class Super {
  @objc(renamedFoo)
  var foo: Int { get { return 3 } } // expected-note 2{{overridden declaration is here}}

  @objc func process(i: Int) -> Int { } // expected-note {{overriding '@objc' method 'process(i:)' here}}
}

class Sub1 : Super {
  @objc(foo) // expected-error{{Objective-C property has a different name from the property it overrides ('foo' vs. 'renamedFoo')}}{{9-12=renamedFoo}}
  override var foo: Int { get { return 5 } }

  override func process(i: Int?) -> Int { } // expected-error{{method cannot be an @objc override because the type of the parameter cannot be represented in Objective-C}}
}

class Sub2 : Super {
  @objc
  override var foo: Int { get { return 5 } }
}

class Sub3 : Super {
  override var foo: Int { get { return 5 } }
}

class Sub4 : Super {
  @objc(renamedFoo)
  override var foo: Int { get { return 5 } }
}

class Sub5 : Super {
  @objc(wrongFoo) // expected-error{{Objective-C property has a different name from the property it overrides ('wrongFoo' vs. 'renamedFoo')}} {{9-17=renamedFoo}}
  override var foo: Int { get { return 5 } }
}

enum NotObjCEnum { case X }
struct NotObjCStruct {}

// Closure arguments can only be @objc if their parameters and returns are.
// CHECK-LABEL: @objc class ClosureArguments
@objc class ClosureArguments {
  // CHECK: @objc func foo
  @objc func foo(f: (Int) -> ()) {}
  // CHECK: @objc func bar
  @objc func bar(f: (NotObjCEnum) -> NotObjCStruct) {} // expected-error{{method cannot be marked @objc because the type of the parameter cannot be represented in Objective-C}} expected-note{{function types cannot be represented in Objective-C unless their parameters and returns can be}}
  // CHECK: @objc func bas
  @objc func bas(f: (NotObjCEnum) -> ()) {} // expected-error{{method cannot be marked @objc because the type of the parameter cannot be represented in Objective-C}} expected-note{{function types cannot be represented in Objective-C unless their parameters and returns can be}}
  // CHECK: @objc func zim
  @objc func zim(f: () -> NotObjCStruct) {} // expected-error{{method cannot be marked @objc because the type of the parameter cannot be represented in Objective-C}} expected-note{{function types cannot be represented in Objective-C unless their parameters and returns can be}}
  // CHECK: @objc func zang
  @objc func zang(f: (NotObjCEnum, NotObjCStruct) -> ()) {} // expected-error{{method cannot be marked @objc because the type of the parameter cannot be represented in Objective-C}} expected-note{{function types cannot be represented in Objective-C unless their parameters and returns can be}}
  @objc func zangZang(f: (Int...) -> ()) {} // expected-error{{method cannot be marked @objc because the type of the parameter cannot be represented in Objective-C}} expected-note{{function types cannot be represented in Objective-C unless their parameters and returns can be}}
  // CHECK: @objc func fooImplicit
  func fooImplicit(f: (Int) -> ()) {}
  // CHECK: {{^}}  func barImplicit
  func barImplicit(f: (NotObjCEnum) -> NotObjCStruct) {}
  // CHECK: {{^}}  func basImplicit
  func basImplicit(f: (NotObjCEnum) -> ()) {}
  // CHECK: {{^}}  func zimImplicit
  func zimImplicit(f: () -> NotObjCStruct) {}
  // CHECK: {{^}}  func zangImplicit
  func zangImplicit(f: (NotObjCEnum, NotObjCStruct) -> ()) {}
  // CHECK: {{^}}  func zangZangImplicit
  func zangZangImplicit(f: (Int...) -> ()) {}
}

typealias GoodBlock = @convention(block) (Int) -> ()
typealias BadBlock = @convention(block) (NotObjCEnum) -> () // expected-error{{'(NotObjCEnum) -> ()' is not representable in Objective-C, so it cannot be used with '@convention(block)'}}


@objc class AccessControl {
  // CHECK: @objc func foo
  func foo() {}
  // CHECK: {{^}} private func bar
  private func bar() {}
  // CHECK: @objc private func baz
  @objc private func baz() {}
}

//===--- Ban @objc +load methods
class Load1 {
  // Okay: not @objc
  class func load() { }
  class func alloc() {}
  class func allocWithZone(_: Int) {}
  class func initialize() {}
}

@objc class Load2 {
  class func load() { } // expected-error{{method 'load()' defines Objective-C class method 'load', which is not permitted by Swift}}
  class func alloc() {} // expected-error{{method 'alloc()' defines Objective-C class method 'alloc', which is not permitted by Swift}}
  class func allocWithZone(_: Int) {} // expected-error{{method 'allocWithZone' defines Objective-C class method 'allocWithZone:', which is not permitted by Swift}}
  class func initialize() {} // expected-error{{method 'initialize()' defines Objective-C class method 'initialize', which is not permitted by Swift}}
}

@objc class Load3 {
  class var load: Load3 {
    get { return Load3() } // expected-error{{getter for 'load' defines Objective-C class method 'load', which is not permitted by Swift}}
    set { }
  }

  @objc(alloc) class var prop: Int { return 0 } // expected-error{{getter for 'prop' defines Objective-C class method 'alloc', which is not permitted by Swift}}
  @objc(allocWithZone:) class func fooWithZone(_: Int) {} // expected-error{{method 'fooWithZone' defines Objective-C class method 'allocWithZone:', which is not permitted by Swift}}
  @objc(initialize) class func barnitialize() {} // expected-error{{method 'barnitialize()' defines Objective-C class method 'initialize', which is not permitted by Swift}}
}

// Members of protocol extensions cannot be @objc

extension PlainProtocol {
  @objc var property: Int { return 5 } // expected-error{{@objc can only be used with members of classes, @objc protocols, and concrete extensions of classes}}
  @objc subscript(x: Int) -> Class_ObjC1 { return Class_ObjC1() } // expected-error{{@objc can only be used with members of classes, @objc protocols, and concrete extensions of classes}}
  @objc func fun() { } // expected-error{{@objc can only be used with members of classes, @objc protocols, and concrete extensions of classes}}
}

extension Protocol_ObjC1 {
  @objc var property: Int { return 5 } // expected-error{{@objc can only be used with members of classes, @objc protocols, and concrete extensions of classes}}
  @objc subscript(x: Int) -> Class_ObjC1 { return Class_ObjC1() } // expected-error{{@objc can only be used with members of classes, @objc protocols, and concrete extensions of classes}}
  @objc func fun() { } // expected-error{{@objc can only be used with members of classes, @objc protocols, and concrete extensions of classes}}
}

extension Protocol_ObjC1 {
  // Don't infer @objc for extensions of @objc protocols.

  // CHECK: {{^}} var propertyOK: Int
  var propertyOK: Int { return 5 }
}

//===---
//===--- Error handling
//===---
class ClassThrows1 {
  // CHECK: @objc func methodReturnsVoid() throws
  @objc func methodReturnsVoid() throws { }

  // CHECK: @objc func methodReturnsObjCClass() throws -> Class_ObjC1
  @objc func methodReturnsObjCClass() throws -> Class_ObjC1 {
    return Class_ObjC1()
  }

  // CHECK: @objc func methodReturnsBridged() throws -> String
  @objc func methodReturnsBridged() throws -> String { return String() }

  // CHECK: @objc func methodReturnsArray() throws -> [String]
  @objc func methodReturnsArray() throws -> [String] { return [String]() }

  // CHECK: @objc init(degrees: Double) throws
  @objc init(degrees: Double) throws { }

  // Errors

  @objc func methodReturnsOptionalObjCClass() throws -> Class_ObjC1? { return nil } // expected-error{{throwing method cannot be marked @objc because it returns a value of optional type 'Class_ObjC1?'; 'nil' indicates failure to Objective-C}}

  @objc func methodReturnsOptionalArray() throws -> [String]? { return nil } // expected-error{{throwing method cannot be marked @objc because it returns a value of optional type '[String]?'; 'nil' indicates failure to Objective-C}}

  @objc func methodReturnsInt() throws -> Int { return 0 } // expected-error{{throwing method cannot be marked @objc because it returns a value of type 'Int'; return 'Void' or a type that bridges to an Objective-C class}}

  @objc func methodAcceptsThrowingFunc(fn: (String) throws -> Int) { }
  // expected-error@-1{{method cannot be marked @objc because the type of the parameter cannot be represented in Objective-C}}
  // expected-note@-2{{throwing function types cannot be represented in Objective-C}}

  @objc init?(radians: Double) throws { } // expected-error{{a failable and throwing initializer cannot be marked @objc because 'nil' indicates failure to Objective-C}}

  @objc init!(string: String) throws { } // expected-error{{a failable and throwing initializer cannot be marked @objc because 'nil' indicates failure to Objective-C}}

  @objc func fooWithErrorEnum1(x: ErrorEnum) {}
  // expected-error@-1{{method cannot be marked @objc because the type of the parameter cannot be represented in Objective-C}}
  // expected-note@-2{{non-'@objc' enums cannot be represented in Objective-C}}

  // CHECK: {{^}} func fooWithErrorEnum2(x: ErrorEnum)
  func fooWithErrorEnum2(x: ErrorEnum) {}

  @objc func fooWithErrorProtocolComposition1(x: Error & Protocol_ObjC1) { }
  // expected-error@-1{{method cannot be marked @objc because the type of the parameter cannot be represented in Objective-C}}
  // expected-note@-2{{protocol-constrained type containing 'Error' cannot be represented in Objective-C}}

  // CHECK: {{^}} func fooWithErrorProtocolComposition2(x: Error & Protocol_ObjC1)
  func fooWithErrorProtocolComposition2(x: Error & Protocol_ObjC1) { }
}


// CHECK-DUMP-LABEL: class_decl{{.*}}"ImplicitClassThrows1"
@objc class ImplicitClassThrows1 {
  // CHECK: @objc func methodReturnsVoid() throws
  // CHECK-DUMP: func_decl{{.*}}"methodReturnsVoid()"{{.*}}foreign_error=ZeroResult,unowned,param=0,paramtype=Optional<AutoreleasingUnsafeMutablePointer<Optional<NSError>>>,resulttype=ObjCBool
  func methodReturnsVoid() throws { }

  // CHECK: @objc func methodReturnsObjCClass() throws -> Class_ObjC1
  // CHECK-DUMP: func_decl{{.*}}"methodReturnsObjCClass()" {{.*}}foreign_error=NilResult,unowned,param=0,paramtype=Optional<AutoreleasingUnsafeMutablePointer<Optional<NSError>>>
  func methodReturnsObjCClass() throws -> Class_ObjC1 {
    return Class_ObjC1()
  }

  // CHECK: @objc func methodReturnsBridged() throws -> String
  func methodReturnsBridged() throws -> String { return String() }

  // CHECK: @objc func methodReturnsArray() throws -> [String]
  func methodReturnsArray() throws -> [String] { return [String]() }

  // CHECK: {{^}} func methodReturnsOptionalObjCClass() throws -> Class_ObjC1?
  func methodReturnsOptionalObjCClass() throws -> Class_ObjC1? { return nil }

  // CHECK: @objc func methodWithTrailingClosures(_ s: String, fn1: @escaping ((Int) -> Int), fn2: @escaping (Int) -> Int, fn3: @escaping (Int) -> Int)
  // CHECK-DUMP: func_decl{{.*}}"methodWithTrailingClosures(_:fn1:fn2:fn3:)"{{.*}}foreign_error=ZeroResult,unowned,param=1,paramtype=Optional<AutoreleasingUnsafeMutablePointer<Optional<NSError>>>,resulttype=ObjCBool
  func methodWithTrailingClosures(_ s: String, fn1: (@escaping (Int) -> Int), fn2: @escaping (Int) -> Int, fn3: @escaping (Int) -> Int) throws { }

  // CHECK: @objc init(degrees: Double) throws
  // CHECK-DUMP: constructor_decl{{.*}}"init(degrees:)"{{.*}}foreign_error=NilResult,unowned,param=1,paramtype=Optional<AutoreleasingUnsafeMutablePointer<Optional<NSError>>>
  init(degrees: Double) throws { }

  // CHECK: {{^}} func methodReturnsBridgedValueType() throws -> NSRange
  func methodReturnsBridgedValueType() throws -> NSRange { return NSRange() }

  @objc func methodReturnsBridgedValueType2() throws -> NSRange {
    return NSRange()
  }
  // expected-error@-3{{throwing method cannot be marked @objc because it returns a value of type 'NSRange' (aka '_NSRange'); return 'Void' or a type that bridges to an Objective-C class}}

  // CHECK: {{^}} @objc func methodReturnsError() throws -> Error
  func methodReturnsError() throws -> Error { return ErrorEnum.failed }

  // CHECK: @objc func methodReturnStaticBridged() throws -> ((Int) -> (Int) -> Int)
  func methodReturnStaticBridged() throws -> ((Int) -> (Int) -> Int) {
    func add(x: Int) -> (Int) -> Int { 
      return { x + $0 }
    }
  }
}

// CHECK-DUMP-LABEL: class_decl{{.*}}"SubclassImplicitClassThrows1"
@objc class SubclassImplicitClassThrows1 : ImplicitClassThrows1 {
  // CHECK: @objc override func methodWithTrailingClosures(_ s: String, fn1: @escaping ((Int) -> Int), fn2: @escaping ((Int) -> Int), fn3: @escaping ((Int) -> Int))
  // CHECK-DUMP: func_decl{{.*}}"methodWithTrailingClosures(_:fn1:fn2:fn3:)"{{.*}}foreign_error=ZeroResult,unowned,param=1,paramtype=Optional<AutoreleasingUnsafeMutablePointer<Optional<NSError>>>,resulttype=ObjCBool
  override func methodWithTrailingClosures(_ s: String, fn1: (@escaping (Int) -> Int), fn2: (@escaping (Int) -> Int), fn3: (@escaping (Int) -> Int)) throws { }
}

class ThrowsRedecl1 {
  @objc func method1(_ x: Int, error: Class_ObjC1) { } // expected-note{{declared here}}
  @objc func method1(_ x: Int) throws { } // expected-error{{with Objective-C selector 'method1:error:'}}

  @objc func method2AndReturnError(_ x: Int) { } // expected-note{{declared here}}
  @objc func method2() throws { } // expected-error{{with Objective-C selector 'method2AndReturnError:'}}

  @objc func method3(_ x: Int, error: Int, closure: @escaping (Int) -> Int) { }  // expected-note{{declared here}}
  @objc func method3(_ x: Int, closure: (Int) -> Int) throws { } // expected-error{{with Objective-C selector 'method3:error:closure:'}}

  @objc(initAndReturnError:) func initMethod1(error: Int) { } // expected-note{{declared here}}
  @objc init() throws { } // expected-error{{with Objective-C selector 'initAndReturnError:'}}

  @objc(initWithString:error:) func initMethod2(string: String, error: Int) { } // expected-note{{declared here}}
  @objc init(string: String) throws { } // expected-error{{with Objective-C selector 'initWithString:error:'}}

  @objc(initAndReturnError:fn:) func initMethod3(error: Int, fn: @escaping (Int) -> Int) { } // expected-note{{declared here}}
  @objc init(fn: (Int) -> Int) throws { } // expected-error{{with Objective-C selector 'initAndReturnError:fn:'}}
}

class ThrowsObjCName {
  @objc(method4:closure:error:) func method4(x: Int, closure: @escaping (Int) -> Int) throws { }

  @objc(method5AndReturnError:x:closure:) func method5(x: Int, closure: @escaping (Int) -> Int) throws { }

  @objc(method6) func method6() throws { } // expected-error{{@objc' method name provides names for 0 arguments, but method has one parameter (the error parameter)}}

  @objc(method7) func method7(x: Int) throws { } // expected-error{{@objc' method name provides names for 0 arguments, but method has 2 parameters (including the error parameter)}}

  // CHECK-DUMP: func_decl{{.*}}"method8(_:fn1:fn2:)"{{.*}}foreign_error=ZeroResult,unowned,param=2,paramtype=Optional<AutoreleasingUnsafeMutablePointer<Optional<NSError>>>,resulttype=ObjCBool
  @objc(method8:fn1:error:fn2:)
  func method8(_ s: String, fn1: (@escaping (Int) -> Int), fn2: @escaping (Int) -> Int) throws { }

  // CHECK-DUMP: func_decl{{.*}}"method9(_:fn1:fn2:)"{{.*}}foreign_error=ZeroResult,unowned,param=0,paramtype=Optional<AutoreleasingUnsafeMutablePointer<Optional<NSError>>>,resulttype=ObjCBool
  @objc(method9AndReturnError:s:fn1:fn2:)
  func method9(_ s: String, fn1: (@escaping (Int) -> Int), fn2: @escaping (Int) -> Int) throws { }
}

class SubclassThrowsObjCName : ThrowsObjCName {
  // CHECK-DUMP: func_decl{{.*}}"method8(_:fn1:fn2:)"{{.*}}foreign_error=ZeroResult,unowned,param=2,paramtype=Optional<AutoreleasingUnsafeMutablePointer<Optional<NSError>>>,resulttype=ObjCBool
  override func method8(_ s: String, fn1: (@escaping (Int) -> Int), fn2: @escaping (Int) -> Int) throws { }

  // CHECK-DUMP: func_decl{{.*}}"method9(_:fn1:fn2:)"{{.*}}foreign_error=ZeroResult,unowned,param=0,paramtype=Optional<AutoreleasingUnsafeMutablePointer<Optional<NSError>>>,resulttype=ObjCBool
  override func method9(_ s: String, fn1: (@escaping (Int) -> Int), fn2: @escaping (Int) -> Int) throws { }
}

@objc protocol ProtocolThrowsObjCName {
  @objc optional func doThing(_ x: String) throws -> String // expected-note{{requirement 'doThing' declared here}}
}

class ConformsToProtocolThrowsObjCName1 : ProtocolThrowsObjCName {
  @objc func doThing(_ x: String) throws -> String { return x } // okay
}

class ConformsToProtocolThrowsObjCName2 : ProtocolThrowsObjCName {
  @objc func doThing(_ x: Int) throws -> String { return "" }
  // expected-warning@-1{{instance method 'doThing' nearly matches optional requirement 'doThing' of protocol 'ProtocolThrowsObjCName'}}
  // expected-note@-2{{move 'doThing' to an extension to silence this warning}}
  // expected-note@-3{{make 'doThing' private to silence this warning}}{{9-9=private }}
  // expected-note@-4{{candidate has non-matching type '(Int) throws -> String'}}
}

@objc class DictionaryTest {
  // CHECK-LABEL: @objc func func_dictionary1a(x: Dictionary<ObjC_Class1, ObjC_Class1>)
  func func_dictionary1a(x: Dictionary<ObjC_Class1, ObjC_Class1>) { }

  // CHECK-LABEL: @objc func func_dictionary1b(x: Dictionary<ObjC_Class1, ObjC_Class1>)
  @objc func func_dictionary1b(x: Dictionary<ObjC_Class1, ObjC_Class1>) { }

  func func_dictionary2a(x: Dictionary<String, Int>) { }
  @objc func func_dictionary2b(x: Dictionary<String, Int>) { }
}

@objc extension PlainClass {
  // CHECK-LABEL: @objc final func objc_ext_objc_okay(_: Int) {
  final func objc_ext_objc_okay(_: Int) { }

  final func objc_ext_objc_not_okay(_: PlainStruct) { }
  // expected-error@-1{{method cannot be in an @objc extension of a class (without @nonobjc) because the type of the parameter cannot be represented in Objective-C}}
  // expected-note@-2 {{Swift structs cannot be represented in Objective-C}}

  // CHECK-LABEL: {{^}} @nonobjc final func objc_ext_objc_explicit_nonobjc(_: PlainStruct) {
  @nonobjc final func objc_ext_objc_explicit_nonobjc(_: PlainStruct) { }
}

@objc class ObjC_Class1 : Hashable {
  func hash(into hasher: inout Hasher) {}
}

func ==(lhs: ObjC_Class1, rhs: ObjC_Class1) -> Bool {
  return true
}

// CHECK-LABEL: @objc class OperatorInClass
@objc class OperatorInClass {
  // CHECK: {{^}} static func == (lhs: OperatorInClass, rhs: OperatorInClass) -> Bool
  static func ==(lhs: OperatorInClass, rhs: OperatorInClass) -> Bool {
    return true
  }
  // CHECK: {{^}} @objc static func + (lhs: OperatorInClass, rhs: OperatorInClass) -> OperatorInClass
  @objc static func +(lhs: OperatorInClass, rhs: OperatorInClass) -> OperatorInClass { // expected-error {{operator methods cannot be declared @objc}}
    return lhs
  }
} // CHECK: {{^}$}}

@objc protocol OperatorInProtocol {
  static func +(lhs: Self, rhs: Self) -> Self // expected-error {{@objc protocols must not have operator requirements}}
}

class AdoptsOperatorInProtocol : OperatorInProtocol {
  static func +(lhs: AdoptsOperatorInProtocol, rhs: AdoptsOperatorInProtocol) -> Self {}
  // expected-error@-1 {{operator methods cannot be declared @objc}}
}

//===--- @objc inference for witnesses

@objc protocol InferFromProtocol {
  @objc(inferFromProtoMethod1:)
  optional func method1(value: Int)
}

// Infer when in the same declaration context.
// CHECK-LABEL: ClassInfersFromProtocol1
class ClassInfersFromProtocol1 : InferFromProtocol{
  // CHECK: {{^}} @objc func method1(value: Int)
  func method1(value: Int) { }
}

// Infer when in a different declaration context of the same class.
// CHECK-LABEL: ClassInfersFromProtocol2a
class ClassInfersFromProtocol2a {
  // CHECK: {{^}} @objc func method1(value: Int)
  func method1(value: Int) { }
}

extension ClassInfersFromProtocol2a : InferFromProtocol { }

// Infer when in a different declaration context of the same class.
class ClassInfersFromProtocol2b : InferFromProtocol { }

// CHECK-LABEL: ClassInfersFromProtocol2b
extension ClassInfersFromProtocol2b {
  // CHECK: {{^}} @objc dynamic func method1(value: Int)
  func method1(value: Int) { }
}

// Don't infer when there is a signature mismatch.
// CHECK-LABEL: ClassInfersFromProtocol3
class ClassInfersFromProtocol3 : InferFromProtocol {
}

extension ClassInfersFromProtocol3 {
  // CHECK: {{^}} func method1(value: String)
  func method1(value: String) { }
}

// Inference for subclasses.
class SuperclassImplementsProtocol : InferFromProtocol { }

class SubclassInfersFromProtocol1 : SuperclassImplementsProtocol {
  // CHECK: {{^}} @objc func method1(value: Int)
  func method1(value: Int) { }
}

class SubclassInfersFromProtocol2 : SuperclassImplementsProtocol {
}

extension SubclassInfersFromProtocol2 {
  // CHECK: {{^}} @objc dynamic func method1(value: Int)
  func method1(value: Int) { }
}

@objc class NeverReturningMethod {
  @objc func doesNotReturn() -> Never {}
}

// SR-5025
class User: NSObject {
}

@objc extension User {
  var name: String {
    get {
      return "No name"
    }
    set {
      // Nothing
    }
  }

  var other: String {
    unsafeAddress { // expected-error{{addressors are not allowed to be marked @objc}}
    }
  }
}

// 'dynamic' methods cannot be @inlinable.
class BadClass {
  @inlinable @objc dynamic func badMethod1() {}
  // expected-error@-1 {{'@inlinable' attribute cannot be applied to 'dynamic' declarations}}
}

@objc
protocol ObjCProtocolWithWeakProperty {
   weak var weakProp: AnyObject? { get set } // okay
}

@objc
protocol ObjCProtocolWithUnownedProperty {
   unowned var unownedProp: AnyObject { get set } // okay
}

// rdar://problem/46699152: errors about read/modify accessors being implicitly
// marked @objc.
@objc class MyObjCClass: NSObject {}

@objc
extension MyObjCClass {
    @objc
    static var objCVarInObjCExtension: Bool {
        get {
            return true
        }
        set {}
    }

    // CHECK: {{^}} @objc private dynamic func stillExposedToObjCDespiteBeingPrivate()
    private func stillExposedToObjCDespiteBeingPrivate() {}
}

@objc private extension MyObjCClass {
  // CHECK: {{^}} @objc dynamic func alsoExposedToObjCDespiteBeingPrivate()
  func alsoExposedToObjCDespiteBeingPrivate() {}
}

@objcMembers class VeryObjCClass: NSObject {
  // CHECK: {{^}} private func notExposedToObjC()
  private func notExposedToObjC() {}
}

// SR-9035

class SR_9035_C {}

@objc protocol SR_9035_P {
  func throwingMethod1() throws -> Unmanaged<CFArray> // Ok
  func throwingMethod2() throws -> Unmanaged<SR_9035_C> // expected-error {{method cannot be a member of an @objc protocol because its result type cannot be represented in Objective-C}}
  // expected-note@-1 {{inferring '@objc' because the declaration is a member of an '@objc' protocol}}
}

// SR-12801: Make sure we reject an @objc generic subscript.
class SR12801 {
  @objc subscript<T>(foo : [T]) -> Int { return 0 }
  // expected-error@-1 {{subscript cannot be marked @objc because it has generic parameters}}
}
