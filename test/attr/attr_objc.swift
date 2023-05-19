// RUN: %empty-directory(%t)
// RUN: %{python} %S/Inputs/access-note-gen.py %s %t/attr_objc_access_note.swift %t/attr_objc_access_note.accessnotes

// Test with @objc attrs, without access notes
// RUN: %target-swift-frontend -disable-objc-attr-requires-foundation-module -typecheck -verify -verify-ignore-unknown %s -swift-version 4 -enable-source-import -I %S/Inputs -enable-swift3-objc-inference
// RUN: %target-swift-ide-test -skip-deinit=false -print-ast-typechecked -source-filename %s -function-definitions=true -prefer-type-repr=false -print-implicit-attrs=true -explode-pattern-binding-decls=true -disable-objc-attr-requires-foundation-module -swift-version 4 -enable-source-import -I %S/Inputs -enable-swift3-objc-inference | %FileCheck %s
// RUN: not %target-swift-frontend -typecheck -dump-ast -disable-objc-attr-requires-foundation-module %s -swift-version 4 -enable-source-import -I %S/Inputs -enable-swift3-objc-inference > %t/attr_objc.ast
// RUN: %FileCheck -check-prefix CHECK-DUMP %s < %t/attr_objc.ast

// Test without @objc attrs, with access notes
// RUN: %target-swift-frontend -disable-objc-attr-requires-foundation-module -typecheck -verify -verify-ignore-unknown %t/attr_objc_access_note.swift -access-notes-path %t/attr_objc_access_note.accessnotes -swift-version 4 -enable-source-import -I %S/Inputs -enable-swift3-objc-inference
// RUN: %target-swift-ide-test -skip-deinit=false -print-ast-typechecked -source-filename %t/attr_objc_access_note.swift -access-notes-path %t/attr_objc_access_note.accessnotes -function-definitions=true -prefer-type-repr=false -print-implicit-attrs=true -explode-pattern-binding-decls=true -disable-objc-attr-requires-foundation-module -swift-version 4 -enable-source-import -I %S/Inputs -enable-swift3-objc-inference | %FileCheck %t/attr_objc_access_note.swift
// RUN: not %target-swift-frontend -typecheck -dump-ast -disable-objc-attr-requires-foundation-module %t/attr_objc_access_note.swift -access-notes-path %t/attr_objc_access_note.accessnotes -swift-version 4 -enable-source-import -I %S/Inputs -enable-swift3-objc-inference > %t/attr_objc_access_note.ast
// RUN: %FileCheck -check-prefix CHECK-DUMP %t/attr_objc_access_note.swift < %t/attr_objc_access_note.ast

// Test with both @objc attrs and access notes
// RUN: %target-swift-frontend -disable-objc-attr-requires-foundation-module -typecheck -verify -verify-ignore-unknown %s -access-notes-path %t/attr_objc_access_note.accessnotes -swift-version 4 -enable-source-import -I %S/Inputs -enable-swift3-objc-inference
// RUN: %target-swift-ide-test -skip-deinit=false -print-ast-typechecked -source-filename %s -access-notes-path %t/attr_objc_access_note.accessnotes -function-definitions=true -prefer-type-repr=false -print-implicit-attrs=true -explode-pattern-binding-decls=true -disable-objc-attr-requires-foundation-module -swift-version 4 -enable-source-import -I %S/Inputs -enable-swift3-objc-inference | %FileCheck %s
// RUN: not %target-swift-frontend -typecheck -dump-ast -disable-objc-attr-requires-foundation-module %s -access-notes-path %t/attr_objc_access_note.accessnotes -swift-version 4 -enable-source-import -I %S/Inputs -enable-swift3-objc-inference > %t/attr_objc_2.ast
// RUN: %FileCheck -check-prefix CHECK-DUMP %s < %t/attr_objc_2.ast

// REQUIRES: objc_interop

// HOW TO WORK ON THIS TEST
//
// This file is primarily used to test '@objc' in source files, but it is also
// processed to produce test files for access notes, which can annotate
// declarations with '@objc' based on a sidecar file. This processing produces
// a source file with most of the '@objc' annotations removed, plus an access
// note file which adds back the removed '@objc' annotations. The three files
// are then tested in various combinations.
//
// The processing step uses the following special commands, which must appear
// at the beginning of a line comment to be recognized:
//
// * `access-note-adjust @±offset` (where the offset is optional) modifies the
//   rest of the line comment to:
//
//   1. Change expected errors to expected remarks
//   2. Adjust the line offsets of all expected diagnostics by the offset
//   3. Change the phrase "marked @objc" to "marked @objc by an access note"
//   4. Change all expected fix-its to "{{none}}"
//
// * `access-note-move @±offset {{name}}` (where the offset is optional) can
//   only appear immediately after an `@objc` or `@objc(someName)` attribute.
//   It removes the attribute from the source code, adds a corresponding
//   access note for `name` to the access note file, and does the same
//   processing to the rest of the line as access-note-adjust. Note that in this
//   case, the offset is @+1, not @+0, unless something else is specified.
//
// Note that, in some cases, we need additional access notes to be added that
// don't directly correspond to any attribute in the source code (usually
// because one @objc attribute covers several declarations). When this happens,
// we write a commented-out @objc attribute and use the `access-note-move`
// command.


import Foundation

class PlainClass {}
struct PlainStruct {}
enum PlainEnum {}
protocol PlainProtocol {} // expected-note {{protocol 'PlainProtocol' declared here}}

enum ErrorEnum : Error {
  case failed
}

@objc // access-note-move{{Class_ObjC1}}
class Class_ObjC1 {}

protocol Protocol_Class1 : class {} // expected-note {{protocol 'Protocol_Class1' declared here}}

protocol Protocol_Class2 : class {}

@objc // access-note-move{{Protocol_ObjC1}}
protocol Protocol_ObjC1 {}

@objc // access-note-move{{Protocol_ObjC2}}
protocol Protocol_ObjC2 {}




//===--- Subjects of @objc attribute.

@objc // expected-error{{'@objc' can only be applied to an extension of a class}}{{1-7=}}
extension PlainStruct { }

class FáncyName {}

@objc(FancyName)
extension FáncyName {}

@objc // bad-access-note-move{{subject_globalVar}} expected-error {{@objc can only be used with members of classes, @objc protocols, and concrete extensions of classes}}
var subject_globalVar: Int

var subject_getterSetter: Int {
  @objc // bad-access-note-move{{getter:subject_getterSetter()}} expected-error {{@objc can only be used with members of classes, @objc protocols, and concrete extensions of classes}} {{3-9=}}
  get {
    return 0
  }
  @objc // bad-access-note-move{{setter:subject_getterSetter()}} expected-error {{@objc can only be used with members of classes, @objc protocols, and concrete extensions of classes}} {{3-9=}}
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
    @objc // bad-access-note-move{{getter:subject_getterSetter1.instanceVar1()}} expected-error {{'@objc' getter for non-'@objc' property}} {{5-11=}}
    get {
      return 0
    }
  }

  var instanceVar2: Int {
    get {
      return 0
    }
    @objc // bad-access-note-move{{setter:subject_getterSetter1.instanceVar2()}} expected-error {{'@objc' setter for non-'@objc' property}} {{5-11=}}
    set {
    }
  }

  var instanceVar3: Int {
    @objc // bad-access-note-move{{getter:subject_getterSetter1.instanceVar3()}} expected-error {{'@objc' getter for non-'@objc' property}} {{5-11=}}
    get {
      return 0
    }
    @objc // bad-access-note-move{{setter:subject_getterSetter1.instanceVar3()}} expected-error {{'@objc' setter for non-'@objc' property}} {{5-11=}}
    set {
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
  @objc // access-note-move{{subject_staticVar1.staticVar1}}
  class var staticVar1: Int = 42 // expected-error {{class stored properties not supported}}

  @objc // access-note-move{{subject_staticVar1.staticVar2}}
  class var staticVar2: Int { return 42 }
}

@objc // bad-access-note-move{{subject_freeFunc()}} expected-error {{@objc can only be used with members of classes, @objc protocols, and concrete extensions of classes}} {{1-7=}}
func subject_freeFunc() {
  @objc // expected-error {{@objc can only be used with members of classes, @objc protocols, and concrete extensions of classes}} {{3-9=}}
  var subject_localVar: Int
  // expected-warning@-1 {{variable 'subject_localVar' was never used; consider replacing with '_' or removing it}}

  @objc // expected-error {{@objc can only be used with members of classes, @objc protocols, and concrete extensions of classes}} {{3-9=}}
  func subject_nestedFreeFunc() {
  }
}

@objc // bad-access-note-move{{subject_genericFunc(t:)}} expected-error {{@objc can only be used with members of classes, @objc protocols, and concrete extensions of classes}} {{1-7=}}
func subject_genericFunc<T>(t: T) {
  @objc // expected-error {{@objc can only be used with members of classes, @objc protocols, and concrete extensions of classes}} {{3-9=}}
  var subject_localVar: Int
  // expected-warning@-1 {{variable 'subject_localVar' was never used; consider replacing with '_' or removing it}}

  @objc // expected-error {{@objc can only be used with members of classes, @objc protocols, and concrete extensions of classes}} {{3-9=}}
  func subject_instanceFunc() {}
}

func subject_funcParam(a: @objc Int) { // expected-error {{attribute can only be applied to declarations, not types}} {{1-1=@objc }} {{27-33=}}
}

@objc // bad-access-note-move{{subject_struct}} expected-error {{'@objc' attribute cannot be applied to this declaration}} {{1-7=}}
struct subject_struct {
  @objc // bad-access-note-move{{subject_struct.subject_instanceVar}} expected-error {{@objc can only be used with members of classes, @objc protocols, and concrete extensions of classes}} {{3-9=}}
  var subject_instanceVar: Int

  @objc // bad-access-note-move{{subject_struct.init()}} expected-error {{@objc can only be used with members of classes, @objc protocols, and concrete extensions of classes}} {{3-9=}}
  init() {}

  @objc // bad-access-note-move{{subject_struct.subject_instanceFunc()}} expected-error {{@objc can only be used with members of classes, @objc protocols, and concrete extensions of classes}} {{3-9=}}
  func subject_instanceFunc() {}
}

@objc // bad-access-note-move{{subject_genericStruct}} expected-error {{'@objc' attribute cannot be applied to this declaration}} {{1-7=}}
struct subject_genericStruct<T> {
  @objc // bad-access-note-move{{subject_genericStruct.subject_instanceVar}} expected-error {{@objc can only be used with members of classes, @objc protocols, and concrete extensions of classes}} {{3-9=}}
  var subject_instanceVar: Int

  @objc // bad-access-note-move{{subject_genericStruct.init()}} expected-error {{@objc can only be used with members of classes, @objc protocols, and concrete extensions of classes}} {{3-9=}}
  init() {}

  @objc // bad-access-note-move{{subject_genericStruct.subject_instanceFunc()}} expected-error {{@objc can only be used with members of classes, @objc protocols, and concrete extensions of classes}} {{3-9=}}
  func subject_instanceFunc() {}
}

@objc // access-note-move{{subject_class1}}
class subject_class1 { // no-error
  @objc // access-note-move{{subject_class1.subject_instanceVar}}
  var subject_instanceVar: Int // no-error

  @objc // access-note-move{{subject_class1.init()}}
  init() {} // no-error

  @objc // access-note-move{{subject_class1.subject_instanceFunc()}}
  func subject_instanceFunc() {} // no-error
  
  
}

@objc // access-note-move{{subject_class2}}
class subject_class2 : Protocol_Class1, PlainProtocol { // no-error
}

@objc // bad-access-note-move{{subject_genericClass}} expected-error{{generic subclasses of '@objc' classes cannot have an explicit '@objc' because they are not directly visible from Objective-C}} {{1-7=}}
class subject_genericClass<T> {
  @objc // access-note-move{{subject_genericClass.subject_instanceVar}}
  var subject_instanceVar: Int // no-error

  @objc // access-note-move{{subject_genericClass.init()}}
  init() {} // no-error

  @objc // access-note-move{{subject_genericClass.subject_instanceFunc()}}
  func subject_instanceFunc() {} // no_error
}

@objc // bad-access-note-move{{subject_genericClass2}} expected-error{{generic subclasses of '@objc' classes cannot have an explicit '@objc' because they are not directly visible from Objective-C}} {{1-7=}}
class subject_genericClass2<T> : Class_ObjC1 {
  @objc // access-note-move{{subject_genericClass2.subject_instanceVar}}
  var subject_instanceVar: Int // no-error

  @objc // access-note-move{{subject_genericClass2.init(foo:)}}
  init(foo: Int) {} // no-error

  @objc // access-note-move{{subject_genericClass2.subject_instanceFunc()}}
  func subject_instanceFunc() {} // no_error
}

extension subject_genericClass where T : Hashable {
  @objc // bad-access-note-move{{subject_genericClass.prop}}
  var prop: Int { return 0 } // access-note-adjust{{@objc}} expected-error{{members of constrained extensions cannot be declared @objc}}
}

extension subject_genericClass {
  @objc // bad-access-note-move{{subject_genericClass.extProp}}
  var extProp: Int { return 0 } // access-note-adjust{{@objc}} expected-error{{extensions of generic classes cannot contain '@objc' members}}
  
  @objc // bad-access-note-move{{subject_genericClass.extFoo()}}
  func extFoo() {} // access-note-adjust{{@objc}} expected-error{{extensions of generic classes cannot contain '@objc' members}}
}

@objc // access-note-move{{subject_enum}}
enum subject_enum: Int {
  @objc // bad-access-note-move{{subject_enum.subject_enumElement1}} expected-error {{attribute has no effect; cases within an '@objc' enum are already exposed to Objective-C}} {{3-9=}}
  case subject_enumElement1

  @objc(subject_enumElement2) // access-note-move{{subject_enum.subject_enumElement2}}
  case subject_enumElement2

  // Fake for access notes: @objc(subject_enumElement3) // bad-access-note-move@+2{{subject_enum.subject_enumElement4}}
  @objc(subject_enumElement3) // bad-access-note-move{{subject_enum.subject_enumElement3}} expected-error {{'@objc' enum case declaration defines multiple enum cases with the same Objective-C name}}{{3-31=}}
  case subject_enumElement3, subject_enumElement4
  // Because of the fake access-note-move above, we expect to see extra diagnostics when we run this test with both explicit @objc attributes *and* access notes:
  // expected-remark@-2 * {{'@objc' enum case declaration defines multiple enum cases with the same Objective-C name}} expected-note@-2 *{{attribute 'objc' was added by access note for fancy tests}}

  // Fake for access notes: @objc // bad-access-note-move@+2{{subject_enum.subject_enumElement6}}
  @objc // bad-access-note-move{{subject_enum.subject_enumElement5}} expected-error {{attribute has no effect; cases within an '@objc' enum are already exposed to Objective-C}} {{3-9=}}
  case subject_enumElement5, subject_enumElement6
  // Because of the fake access-note-move above, we expect to see extra diagnostics when we run this test with both explicit @objc attributes *and* access notes:
  // expected-remark@-2 * {{attribute has no effect; cases within an '@objc' enum are already exposed to Objective-C}} expected-note@-2 *{{attribute 'objc' was added by access note for fancy tests}}

  @nonobjc // expected-error {{'@nonobjc' attribute cannot be applied to this declaration}}
  case subject_enumElement7

  @objc // bad-access-note-move{{subject_enum.init()}} expected-error {{@objc can only be used with members of classes, @objc protocols, and concrete extensions of classes}} {{3-9=}}
  init() {}

  @objc // bad-access-note-move{{subject_enum.subject_instanceFunc()}} expected-error {{@objc can only be used with members of classes, @objc protocols, and concrete extensions of classes}} {{3-9=}}
  func subject_instanceFunc() {}
}

enum subject_enum2 {
  @objc(subject_enum2Element1) // bad-access-note-move{{subject_enum2.subject_enumElement1}} expected-error{{'@objc' enum case is not allowed outside of an '@objc' enum}}{{3-32=}}
  case subject_enumElement1
}

@objc // access-note-move{{subject_protocol1}}
protocol subject_protocol1 {
  @objc // access-note-move{{subject_protocol1.subject_instanceVar}}
  var subject_instanceVar: Int { get }

  @objc // access-note-move{{subject_protocol1.subject_instanceFunc()}}
  func subject_instanceFunc()
}

@objc // access-note-move{{subject_protocol2}} // no-error
protocol subject_protocol2 {}
// CHECK-LABEL: @objc protocol subject_protocol2 {

@objc // access-note-move{{subject_protocol3}} // no-error
protocol subject_protocol3 {}
// CHECK-LABEL: @objc protocol subject_protocol3 {

@objc // access-note-move{{subject_protocol4}}
protocol subject_protocol4 : PlainProtocol {} // expected-error {{@objc protocol 'subject_protocol4' cannot refine non-@objc protocol 'PlainProtocol'}}

@objc // access-note-move{{subject_protocol5}}
protocol subject_protocol5 : Protocol_Class1 {} // expected-error {{@objc protocol 'subject_protocol5' cannot refine non-@objc protocol 'Protocol_Class1'}}

@objc // access-note-move{{subject_protocol6}}
protocol subject_protocol6 : Protocol_ObjC1 {}

protocol subject_containerProtocol1 {
  @objc // bad-access-note-move{{subject_containerProtocol1.subject_instanceVar}} expected-error {{@objc can only be used with members of classes, @objc protocols, and concrete extensions of classes}}
  var subject_instanceVar: Int { get }

  @objc // bad-access-note-move{{subject_containerProtocol1.subject_instanceFunc()}} expected-error {{@objc can only be used with members of classes, @objc protocols, and concrete extensions of classes}}
  func subject_instanceFunc()

  @objc // bad-access-note-move{{subject_containerProtocol1.subject_staticFunc()}} expected-error {{@objc can only be used with members of classes, @objc protocols, and concrete extensions of classes}}
  static func subject_staticFunc()
}

@objc // access-note-move{{subject_containerObjCProtocol1}}
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

@objc // access-note-move{{subject_containerObjCProtocol2}}
protocol subject_containerObjCProtocol2 {
  init(a: Int)
  // expected-note@-1 {{'init' previously declared here}}

  @objc // FIXME: Access notes can't distinguish between init(a:) overloads
  init(a: Double)
  // expected-warning@-1 {{initializer 'init(a:)' with Objective-C selector 'initWithA:' conflicts with previous declaration with the same Objective-C selector; this is an error in Swift 6}}

  func func1() -> Int
  @objc // access-note-move{{subject_containerObjCProtocol2.func1_()}}
  func func1_() -> Int

  var instanceVar1: Int { get set }
  @objc // access-note-move{{subject_containerObjCProtocol2.instanceVar1_}}
  var instanceVar1_: Int { get set }

  subscript(i: Int) -> Int { get set }

  @objc // FIXME: Access notes can't distinguish between subscript(_:) overloads
  subscript(i: String) -> Int { get set}
}

protocol nonObjCProtocol {
  @objc // bad-access-note-move{{nonObjCProtocol.objcRequirement()}} expected-error{{@objc can only be used with members of classes, @objc protocols, and concrete extensions of classes}}
  func objcRequirement()
}

func concreteContext1() {
  @objc
  class subject_inConcreteContext {}
}

class ConcreteContext2 {
  @objc // access-note-move{{ConcreteContext2.subject_inConcreteContext}}
  class subject_inConcreteContext {}
}

class ConcreteContext3 {

  func dynamicSelf1() -> Self { return self }

  @objc // access-note-move{{ConcreteContext3.dynamicSelf1_()}}
  func dynamicSelf1_() -> Self { return self }

  @objc // bad-access-note-move{{ConcreteContext3.genericParams()}}
  func genericParams<T: NSObject>() -> [T] { return [] } // access-note-adjust{{@objc}} expected-error{{instance method cannot be marked @objc because it has generic parameters}}

  @objc // bad-access-note-move{{ConcreteContext3.returnObjCProtocolMetatype()}}
  func returnObjCProtocolMetatype() -> NSCoding.Protocol { return NSCoding.self } // access-note-adjust{{@objc}} expected-error{{method cannot be marked @objc because its result type cannot be represented in Objective-C}}

  typealias AnotherNSCoding = NSCoding
  typealias MetaNSCoding1 = NSCoding.Protocol
  typealias MetaNSCoding2 = AnotherNSCoding.Protocol

  @objc // bad-access-note-move{{ConcreteContext3.returnObjCAliasProtocolMetatype1()}}
  func returnObjCAliasProtocolMetatype1() -> AnotherNSCoding.Protocol { return NSCoding.self } // access-note-adjust{{@objc}} expected-error{{method cannot be marked @objc because its result type cannot be represented in Objective-C}}

  @objc // bad-access-note-move{{ConcreteContext3.returnObjCAliasProtocolMetatype2()}}
  func returnObjCAliasProtocolMetatype2() -> MetaNSCoding1 { return NSCoding.self } // access-note-adjust{{@objc}} expected-error{{method cannot be marked @objc because its result type cannot be represented in Objective-C}}

  @objc // bad-access-note-move{{ConcreteContext3.returnObjCAliasProtocolMetatype3()}}
  func returnObjCAliasProtocolMetatype3() -> MetaNSCoding2 { return NSCoding.self } // access-note-adjust{{@objc}} expected-error{{method cannot be marked @objc because its result type cannot be represented in Objective-C}}

  typealias Composition = NSCopying & NSCoding

  @objc // bad-access-note-move{{ConcreteContext3.returnCompositionMetatype1()}}
  func returnCompositionMetatype1() -> Composition.Protocol { return Composition.self } // access-note-adjust{{@objc}} expected-error{{method cannot be marked @objc because its result type cannot be represented in Objective-C}}

  @objc // bad-access-note-move{{ConcreteContext3.returnCompositionMetatype2()}}
  func returnCompositionMetatype2() -> (NSCopying & NSCoding).Protocol { return (NSCopying & NSCoding).self } // access-note-adjust{{@objc}} expected-error{{method cannot be marked @objc because its result type cannot be represented in Objective-C}}

  typealias NSCodingExistential = NSCoding.Type

  @objc // bad-access-note-move{{ConcreteContext3.inoutFunc(a:)}}
  func inoutFunc(a: inout Int) {} // access-note-adjust{{@objc}} expected-error{{method cannot be marked @objc because inout parameters cannot be represented in Objective-C}}

  @objc // bad-access-note-move{{ConcreteContext3.metatypeOfExistentialMetatypePram1(a:)}}
  func metatypeOfExistentialMetatypePram1(a: NSCodingExistential.Protocol) {} // access-note-adjust{{@objc}} expected-error{{method cannot be marked @objc because the type of the parameter cannot be represented in Objective-C}}

  @objc // bad-access-note-move{{ConcreteContext3.metatypeOfExistentialMetatypePram2(a:)}}
  func metatypeOfExistentialMetatypePram2(a: NSCoding.Type.Protocol) {} // access-note-adjust{{@objc}} expected-error{{method cannot be marked @objc because the type of the parameter cannot be represented in Objective-C}}
}

func genericContext1<T>(_: T) {
  @objc // expected-error {{generic subclasses of '@objc' classes cannot have an explicit '@objc' because they are not directly visible from Objective-C}} {{3-9=}}
  class subject_inGenericContext {} // expected-error {{type 'subject_inGenericContext' cannot be nested in generic function 'genericContext1'}}

  @objc // expected-error {{generic subclasses of '@objc' classes cannot have an explicit '@objc'}} {{3-9=}}
  class subject_inGenericContext2 : Class_ObjC1 {} // expected-error {{type 'subject_inGenericContext2' cannot be nested in generic function 'genericContext1'}}

  class subject_constructor_inGenericContext { // expected-error {{type 'subject_constructor_inGenericContext' cannot be nested in generic function 'genericContext1'}}
    @objc
    init() {} // no-error
  }

  class subject_var_inGenericContext { // expected-error {{type 'subject_var_inGenericContext' cannot be nested in generic function 'genericContext1'}}
    @objc
    var subject_instanceVar: Int = 0 // no-error
  }

  class subject_func_inGenericContext { // expected-error {{type 'subject_func_inGenericContext' cannot be nested in generic function 'genericContext1'}}
    @objc
    func f() {} // no-error
  }
}

class GenericContext2<T> {
  @objc // bad-access-note-move{{GenericContext2.subject_inGenericContext}} expected-error{{generic subclasses of '@objc' classes cannot have an explicit '@objc' because they are not directly visible from Objective-C}} {{3-9=}}
  class subject_inGenericContext {}

  @objc // bad-access-note-move{{GenericContext2.subject_inGenericContext2}} expected-error{{generic subclasses of '@objc' classes cannot have an explicit '@objc' because they are not directly visible from Objective-C}} {{3-9=}}
  class subject_inGenericContext2 : Class_ObjC1 {}

  @objc // access-note-move{{GenericContext2.f()}}
  func f() {} // no-error
}

class GenericContext3<T> {
  class MoreNested {
    @objc // bad-access-note-move{{GenericContext3.MoreNested.subject_inGenericContext}} expected-error{{generic subclasses of '@objc' classes cannot have an explicit '@objc' because they are not directly visible from Objective-C}} {{5-11=}}
    class subject_inGenericContext {}

    @objc // bad-access-note-move{{GenericContext3.MoreNested.subject_inGenericContext2}} expected-error{{generic subclasses of '@objc' classes cannot have an explicit '@objc' because they are not directly visible from Objective-C}} {{5-11=}}
    class subject_inGenericContext2 : Class_ObjC1 {}

    @objc // access-note-move{{GenericContext3.MoreNested.f()}}
    func f() {} // no-error
  }
}

class GenericContext4<T> {
  @objc // bad-access-note-move{{GenericContext4.foo()}}
  func foo() where T: Hashable { } // access-note-adjust{{@objc}} expected-error {{instance method cannot be marked @objc because it has a 'where' clause}}
}

@objc // bad-access-note-move{{ConcreteSubclassOfGeneric}} expected-error{{generic subclasses of '@objc' classes cannot have an explicit '@objc' because they are not directly visible from Objective-C}} {{1-7=}}
class ConcreteSubclassOfGeneric : GenericContext3<Int> {}

extension ConcreteSubclassOfGeneric {
  @objc // access-note-move{{ConcreteSubclassOfGeneric.foo()}}
  func foo() {} // okay
}

@objc // bad-access-note-move{{ConcreteSubclassOfGeneric2}} expected-error{{generic subclasses of '@objc' classes cannot have an explicit '@objc' because they are not directly visible from Objective-C}} {{1-7=}}
class ConcreteSubclassOfGeneric2 : subject_genericClass2<Int> {}

extension ConcreteSubclassOfGeneric2 {
  @objc // access-note-move{{ConcreteSubclassOfGeneric2.foo()}}
  func foo() {} // okay
}

@objc(CustomNameForSubclassOfGeneric) // access-note-move{{ConcreteSubclassOfGeneric3}} no-error
class ConcreteSubclassOfGeneric3 : GenericContext3<Int> {}

extension ConcreteSubclassOfGeneric3 {
  @objc // access-note-move{{ConcreteSubclassOfGeneric3.foo()}}
  func foo() {} // okay
}

class subject_subscriptIndexed1 {
  @objc // access-note-move{{subject_subscriptIndexed1.subscript(_:)}}
  subscript(a: Int) -> Int { // no-error
    get { return 0 }
  }
}
class subject_subscriptIndexed2 {
  @objc // access-note-move{{subject_subscriptIndexed2.subscript(_:)}}
  subscript(a: Int8) -> Int { // no-error
    get { return 0 }
  }
}
class subject_subscriptIndexed3 {
  @objc // access-note-move{{subject_subscriptIndexed3.subscript(_:)}}
  subscript(a: UInt8) -> Int { // no-error
    get { return 0 }
  }
}

class subject_subscriptKeyed1 {
  @objc // access-note-move{{subject_subscriptKeyed1.subscript(_:)}}
  subscript(a: String) -> Int { // no-error
    get { return 0 }
  }
}
class subject_subscriptKeyed2 {
  @objc // access-note-move{{subject_subscriptKeyed2.subscript(_:)}}
  subscript(a: Class_ObjC1) -> Int { // no-error
    get { return 0 }
  }
}
class subject_subscriptKeyed3 {
  @objc // access-note-move{{subject_subscriptKeyed3.subscript(_:)}}
  subscript(a: Class_ObjC1.Type) -> Int { // no-error
    get { return 0 }
  }
}
class subject_subscriptKeyed4 {
  @objc // access-note-move{{subject_subscriptKeyed4.subscript(_:)}}
  subscript(a: Protocol_ObjC1) -> Int { // no-error
    get { return 0 }
  }
}
class subject_subscriptKeyed5 {
  @objc // access-note-move{{subject_subscriptKeyed5.subscript(_:)}}
  subscript(a: Protocol_ObjC1.Type) -> Int { // no-error
    get { return 0 }
  }
}
class subject_subscriptKeyed6 {
  @objc // access-note-move{{subject_subscriptKeyed6.subscript(_:)}}
  subscript(a: Protocol_ObjC1 & Protocol_ObjC2) -> Int { // no-error
    get { return 0 }
  }
}
class subject_subscriptKeyed7 {
  @objc // access-note-move{{subject_subscriptKeyed7.subscript(_:)}}
  subscript(a: (Protocol_ObjC1 & Protocol_ObjC2).Type) -> Int { // no-error
    get { return 0 }
  }
}

class subject_subscriptBridgedFloat {
  @objc // access-note-move{{subject_subscriptBridgedFloat.subscript(_:)}}
  subscript(a: Float32) -> Int {
    get { return 0 }
  }
}

class subject_subscriptGeneric<T> {
  @objc // access-note-move{{subject_subscriptGeneric.subscript(_:)}}
  subscript(a: Int) -> Int { // no-error
    get { return 0 }
  }
}

class subject_subscriptInvalid1 {
  @objc // bad-access-note-move{{subject_subscriptInvalid1.subscript(_:)}}
  class subscript(_ i: Int) -> AnyObject? { // access-note-adjust{{@objc}} expected-error {{class subscript cannot be marked @objc}}
    return nil
  }
}

class subject_subscriptInvalid2 {
  @objc // bad-access-note-move{{subject_subscriptInvalid2.subscript(_:)}}
  subscript(a: PlainClass) -> Int {
  // access-note-adjust{{@objc}} expected-error@-1 {{subscript cannot be marked @objc because its type cannot be represented in Objective-C}}
  // expected-note@-2 {{classes not annotated with @objc cannot be represented in Objective-C}}
    get { return 0 }
  }
}
class subject_subscriptInvalid3 {
  @objc // bad-access-note-move{{subject_subscriptInvalid3.subscript(_:)}}
  subscript(a: PlainClass.Type) -> Int { // access-note-adjust{{@objc}} expected-error {{subscript cannot be marked @objc because its type cannot be represented in Objective-C}}
    get { return 0 }
  }
}
class subject_subscriptInvalid4 {
  @objc // bad-access-note-move{{subject_subscriptInvalid4.subscript(_:)}}
  subscript(a: PlainStruct) -> Int { // access-note-adjust{{@objc}} expected-error {{subscript cannot be marked @objc because its type cannot be represented in Objective-C}}
    // expected-note@-1{{Swift structs cannot be represented in Objective-C}}
    get { return 0 }
  }
}
class subject_subscriptInvalid5 {
  @objc // bad-access-note-move{{subject_subscriptInvalid5.subscript(_:)}}
  subscript(a: PlainEnum) -> Int { // access-note-adjust{{@objc}} expected-error {{subscript cannot be marked @objc because its type cannot be represented in Objective-C}}
    // expected-note@-1{{enums cannot be represented in Objective-C}}
    get { return 0 }
  }
}
class subject_subscriptInvalid6 {
  @objc // bad-access-note-move{{subject_subscriptInvalid6.subscript(_:)}}
  subscript(a: PlainProtocol) -> Int { // access-note-adjust{{@objc}} expected-error {{subscript cannot be marked @objc because its type cannot be represented in Objective-C}}
    // expected-note@-1{{protocol-constrained type containing protocol 'PlainProtocol' cannot be represented in Objective-C}}
    get { return 0 }
  }
}
class subject_subscriptInvalid7 {
  @objc // bad-access-note-move{{subject_subscriptInvalid7.subscript(_:)}}
  subscript(a: Protocol_Class1) -> Int { // access-note-adjust{{@objc}} expected-error {{subscript cannot be marked @objc because its type cannot be represented in Objective-C}}
    // expected-note@-1{{protocol-constrained type containing protocol 'Protocol_Class1' cannot be represented in Objective-C}}
    get { return 0 }
  }
}
class subject_subscriptInvalid8 {
  @objc // bad-access-note-move{{subject_subscriptInvalid8.subscript(_:)}}
  subscript(a: Protocol_Class1 & Protocol_Class2) -> Int { // access-note-adjust{{@objc}} expected-error {{subscript cannot be marked @objc because its type cannot be represented in Objective-C}}
    // expected-note@-1{{protocol-constrained type containing protocol 'Protocol_Class1' cannot be represented in Objective-C}}
    get { return 0 }
  }
}

class subject_propertyInvalid1 {
  @objc // bad-access-note-move{{subject_propertyInvalid1.plainStruct}}
  let plainStruct = PlainStruct() // access-note-adjust{{@objc}} expected-error {{property cannot be marked @objc because its type cannot be represented in Objective-C}}
  // expected-note@-1{{Swift structs cannot be represented in Objective-C}}
}

//===--- Tests for @objc inference.

@objc // access-note-move{{infer_instanceFunc1}}
class infer_instanceFunc1 {
// CHECK-LABEL: @objc class infer_instanceFunc1 {

  func func1() {}
// CHECK-LABEL: @objc func func1() {

  @objc // access-note-move{{infer_instanceFunc1.func1_()}}
  func func1_() {} // no-error

  func func2(a: Int) {}
// CHECK-LABEL: @objc func func2(a: Int) {

  @objc // access-note-move{{infer_instanceFunc1.func2_(a:)}}
  func func2_(a: Int) {} // no-error

  func func3(a: Int) -> Int {}
// CHECK-LABEL: @objc func func3(a: Int) -> Int {

  @objc // access-note-move{{infer_instanceFunc1.func3_(a:)}}
  func func3_(a: Int) -> Int {} // no-error

  func func4(a: Int, b: Double) {}
// CHECK-LABEL: @objc func func4(a: Int, b: Double) {

  @objc // access-note-move{{infer_instanceFunc1.func4_(a:b:)}}
  func func4_(a: Int, b: Double) {} // no-error

  func func5(a: String) {}
// CHECK-LABEL: @objc func func5(a: String) {

  @objc // access-note-move{{infer_instanceFunc1.func5_(a:)}}
  func func5_(a: String) {} // no-error

  func func6() -> String {}
// CHECK-LABEL: @objc func func6() -> String {

  @objc // access-note-move{{infer_instanceFunc1.func6_()}}
  func func6_() -> String {} // no-error

  func func7(a: PlainClass) {}
// CHECK-LABEL: {{^}} func func7(a: PlainClass) {

  @objc // bad-access-note-move{{infer_instanceFunc1.func7_(a:)}}
  func func7_(a: PlainClass) {}
  // access-note-adjust{{@objc}} expected-error@-1 {{method cannot be marked @objc because the type of the parameter cannot be represented in Objective-C}}
  // expected-note@-2 {{classes not annotated with @objc cannot be represented in Objective-C}}

  func func7m(a: PlainClass.Type) {}
// CHECK-LABEL: {{^}} func func7m(a: PlainClass.Type) {

  @objc // bad-access-note-move{{infer_instanceFunc1.func7m_(a:)}}
  func func7m_(a: PlainClass.Type) {}
  // access-note-adjust{{@objc}} expected-error@-1 {{method cannot be marked @objc because the type of the parameter cannot be represented in Objective-C}}

  func func8() -> PlainClass {}
// CHECK-LABEL: {{^}} func func8() -> PlainClass {

  @objc // bad-access-note-move{{infer_instanceFunc1.func8_()}}
  func func8_() -> PlainClass {}
  // access-note-adjust{{@objc}} expected-error@-1 {{method cannot be marked @objc because its result type cannot be represented in Objective-C}}
  // expected-note@-2 {{classes not annotated with @objc cannot be represented in Objective-C}}

  func func8m() -> PlainClass.Type {}
// CHECK-LABEL: {{^}} func func8m() -> PlainClass.Type {

  @objc // bad-access-note-move{{infer_instanceFunc1.func8m_()}}
  func func8m_() -> PlainClass.Type {}
  // access-note-adjust{{@objc}} expected-error@-1 {{method cannot be marked @objc because its result type cannot be represented in Objective-C}}

  func func9(a: PlainStruct) {}
// CHECK-LABEL: {{^}} func func9(a: PlainStruct) {

  @objc // bad-access-note-move{{infer_instanceFunc1.func9_(a:)}}
  func func9_(a: PlainStruct) {}
  // access-note-adjust{{@objc}} expected-error@-1 {{method cannot be marked @objc because the type of the parameter cannot be represented in Objective-C}}
  // expected-note@-2 {{Swift structs cannot be represented in Objective-C}}

  func func10() -> PlainStruct {}
// CHECK-LABEL: {{^}} func func10() -> PlainStruct {

  @objc // bad-access-note-move{{infer_instanceFunc1.func10_()}}
  func func10_() -> PlainStruct {}
  // access-note-adjust{{@objc}} expected-error@-1 {{method cannot be marked @objc because its result type cannot be represented in Objective-C}}
  // expected-note@-2 {{Swift structs cannot be represented in Objective-C}}

  func func11(a: PlainEnum) {}
// CHECK-LABEL: {{^}} func func11(a: PlainEnum) {

  @objc // bad-access-note-move{{infer_instanceFunc1.func11_(a:)}}
  func func11_(a: PlainEnum) {}
  // access-note-adjust{{@objc}} expected-error@-1 {{method cannot be marked @objc because the type of the parameter cannot be represented in Objective-C}}
  // expected-note@-2 {{non-'@objc' enums cannot be represented in Objective-C}}

  func func12(a: PlainProtocol) {}
// CHECK-LABEL: {{^}} func func12(a: any PlainProtocol) {

  @objc // bad-access-note-move{{infer_instanceFunc1.func12_(a:)}}
  func func12_(a: PlainProtocol) {}
  // access-note-adjust{{@objc}} expected-error@-1 {{method cannot be marked @objc because the type of the parameter cannot be represented in Objective-C}}
  // expected-note@-2 {{protocol-constrained type containing protocol 'PlainProtocol' cannot be represented in Objective-C}}

  func func13(a: Class_ObjC1) {}
// CHECK-LABEL: @objc func func13(a: Class_ObjC1) {

  @objc // access-note-move{{infer_instanceFunc1.func13_(a:)}}
  func func13_(a: Class_ObjC1) {} // no-error

  func func14(a: Protocol_Class1) {}
// CHECK-LABEL: {{^}} func func14(a: any Protocol_Class1) {

  @objc // bad-access-note-move{{infer_instanceFunc1.func14_(a:)}}
  func func14_(a: Protocol_Class1) {}
  // access-note-adjust{{@objc}} expected-error@-1 {{method cannot be marked @objc because the type of the parameter cannot be represented in Objective-C}}
  // expected-note@-2 {{protocol-constrained type containing protocol 'Protocol_Class1' cannot be represented in Objective-C}}

  func func15(a: Protocol_ObjC1) {}
// CHECK-LABEL: @objc func func15(a: any Protocol_ObjC1) {
  @objc // access-note-move{{infer_instanceFunc1.func15_(a:)}}
  func func15_(a: Protocol_ObjC1) {} // no-error

  func func16(a: AnyObject) {}
// CHECK-LABEL: @objc func func16(a: AnyObject) {

  @objc // access-note-move{{infer_instanceFunc1.func16_(a:)}}
  func func16_(a: AnyObject) {} // no-error

  func func17(a: @escaping () -> ()) {}
// CHECK-LABEL: {{^}}  @objc func func17(a: @escaping () -> ()) {

  @objc // access-note-move{{infer_instanceFunc1.func17_(a:)}}
  func func17_(a: @escaping () -> ()) {}

  func func18(a: @escaping (Int) -> (), b: Int) {}
// CHECK-LABEL: {{^}}  @objc func func18(a: @escaping (Int) -> (), b: Int)

  @objc // access-note-move{{infer_instanceFunc1.func18_(a:b:)}}
  func func18_(a: @escaping (Int) -> (), b: Int) {}

  func func19(a: @escaping (String) -> (), b: Int) {}
// CHECK-LABEL: {{^}}  @objc func func19(a: @escaping (String) -> (), b: Int) {

  @objc // access-note-move{{infer_instanceFunc1.func19_(a:b:)}}
  func func19_(a: @escaping (String) -> (), b: Int) {}

  func func_FunctionReturn1() -> () -> () {}
// CHECK-LABEL: {{^}}  @objc func func_FunctionReturn1() -> () -> () {

  @objc // access-note-move{{infer_instanceFunc1.func_FunctionReturn1_()}}
  func func_FunctionReturn1_() -> () -> () {}

  func func_FunctionReturn2() -> (Int) -> () {}
// CHECK-LABEL: {{^}}  @objc func func_FunctionReturn2() -> (Int) -> () {

  @objc // access-note-move{{infer_instanceFunc1.func_FunctionReturn2_()}}
  func func_FunctionReturn2_() -> (Int) -> () {}

  func func_FunctionReturn3() -> () -> Int {}
// CHECK-LABEL: {{^}}  @objc func func_FunctionReturn3() -> () -> Int {

  @objc // access-note-move{{infer_instanceFunc1.func_FunctionReturn3_()}}
  func func_FunctionReturn3_() -> () -> Int {}

  func func_FunctionReturn4() -> (String) -> () {}
// CHECK-LABEL: {{^}}  @objc func func_FunctionReturn4() -> (String) -> () {

  @objc // access-note-move{{infer_instanceFunc1.func_FunctionReturn4_()}}
  func func_FunctionReturn4_() -> (String) -> () {}

  func func_FunctionReturn5() -> () -> String {}
// CHECK-LABEL: {{^}}  @objc func func_FunctionReturn5() -> () -> String {

  @objc // access-note-move{{infer_instanceFunc1.func_FunctionReturn5_()}}
  func func_FunctionReturn5_() -> () -> String {}


  func func_ZeroParams1() {}
// CHECK-LABEL: @objc func func_ZeroParams1() {

  @objc // access-note-move{{infer_instanceFunc1.func_ZeroParams1a()}}
  func func_ZeroParams1a() {} // no-error


  func func_OneParam1(a: Int) {}
// CHECK-LABEL: @objc func func_OneParam1(a: Int) {

  @objc // access-note-move{{infer_instanceFunc1.func_OneParam1a(a:)}}
  func func_OneParam1a(a: Int) {} // no-error


  func func_TupleStyle1(a: Int, b: Int) {}
  // CHECK-LABEL: {{^}} @objc func func_TupleStyle1(a: Int, b: Int) {

  @objc // access-note-move{{infer_instanceFunc1.func_TupleStyle1a(a:b:)}}
  func func_TupleStyle1a(a: Int, b: Int) {}

  func func_TupleStyle2(a: Int, b: Int, c: Int) {}
// CHECK-LABEL: {{^}} @objc func func_TupleStyle2(a: Int, b: Int, c: Int) {

  @objc // access-note-move{{infer_instanceFunc1.func_TupleStyle2a(a:b:c:)}}
  func func_TupleStyle2a(a: Int, b: Int, c: Int) {}

  // Check that we produce diagnostics for every parameter and return type.
  @objc // bad-access-note-move{{infer_instanceFunc1.func_MultipleDiags(a:b:)}}
  func func_MultipleDiags(a: PlainStruct, b: PlainEnum) -> Any {}
  // access-note-adjust{{@objc}} expected-error@-1 {{method cannot be marked @objc because the type of the parameter 1 cannot be represented in Objective-C}}
  // expected-note@-2 {{Swift structs cannot be represented in Objective-C}}
  // access-note-adjust{{@objc}} expected-error@-3 {{method cannot be marked @objc because the type of the parameter 2 cannot be represented in Objective-C}}
  // expected-note@-4 {{non-'@objc' enums cannot be represented in Objective-C}}
  // Produces an extra: expected-note@-5 * {{attribute 'objc' was added by access note for fancy tests}}

  @objc // access-note-move{{infer_instanceFunc1.func_UnnamedParam1(_:)}}
  func func_UnnamedParam1(_: Int) {} // no-error

  @objc // bad-access-note-move{{infer_instanceFunc1.func_UnnamedParam2(_:)}}
  func func_UnnamedParam2(_: PlainStruct) {}
  // access-note-adjust{{@objc}} expected-error@-1 {{method cannot be marked @objc because the type of the parameter cannot be represented in Objective-C}}
  // expected-note@-2 {{Swift structs cannot be represented in Objective-C}}

  @objc // access-note-move{{infer_instanceFunc1.func_varParam1(a:)}}
  func func_varParam1(a: AnyObject) {
    var a = a
    let b = a; a = b
  }

  func func_varParam2(a: AnyObject) {
    var a = a
    let b = a; a = b
  }
// CHECK-LABEL: @objc func func_varParam2(a: AnyObject) {
}

@objc // access-note-move{{infer_constructor1}}
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

@objc // access-note-move{{infer_destructor1}}
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

@objc // access-note-move{{infer_instanceVar1}}
class infer_instanceVar1 {
// CHECK-LABEL: @objc class infer_instanceVar1 {
  init() {}

  var instanceVar1: Int
  // CHECK: @objc var instanceVar1: Int

  var (instanceVar2, instanceVar3): (Int, PlainProtocol)
  // CHECK: @objc var instanceVar2: Int
  // CHECK: {{^}}  var instanceVar3: any PlainProtocol

  @objc // bad-access-note-move{{infer_instanceVar1.instanceVar1_}}
  var (instanceVar1_, instanceVar2_): (Int, PlainProtocol)
  // access-note-adjust{{@objc}} expected-error@-1 {{property cannot be marked @objc because its type cannot be represented in Objective-C}}
  // expected-note@-2 {{protocol-constrained type containing protocol 'PlainProtocol' cannot be represented in Objective-C}}
  // Fake for access notes: @objc // access-note-move@-3{{infer_instanceVar1.instanceVar2_}}

  var instanceVar4: Int {
  // CHECK: @objc var instanceVar4: Int {
    get {}
    // CHECK-NEXT: @objc get {}
  }

  var instanceVar5: Int {
  // CHECK: @objc var instanceVar5: Int {
    get {}
    // CHECK-NEXT: @objc get {}
    set {}
    // CHECK-NEXT: @objc set {}
  }

  @objc // access-note-move{{infer_instanceVar1.instanceVar5_}}
  var instanceVar5_: Int {
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

  @objc // access-note-move{{infer_instanceVar1.observingAccessorsVar1_}}
  var observingAccessorsVar1_: Int {
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

  @objc // bad-access-note-move{{infer_instanceVar1.var_tuple1_}}
  var var_tuple1_: ()
  // access-note-adjust{{@objc}} expected-error@-1 {{property cannot be marked @objc because its type cannot be represented in Objective-C}}
  // expected-note@-2 {{empty tuple type cannot be represented in Objective-C}}

  var var_tuple2: Void
// CHECK-LABEL: {{^}} @_hasInitialValue var var_tuple2: Void

  @objc // bad-access-note-move{{infer_instanceVar1.var_tuple2_}}
  var var_tuple2_: Void
  // access-note-adjust{{@objc}} expected-error@-1 {{property cannot be marked @objc because its type cannot be represented in Objective-C}}
  // expected-note@-2 {{empty tuple type cannot be represented in Objective-C}}

  var var_tuple3: (Int)
// CHECK-LABEL: @objc var var_tuple3: (Int)

  @objc // access-note-move{{infer_instanceVar1.var_tuple3_}}
  var var_tuple3_: (Int) // no-error

  var var_tuple4: (Int, Int)
// CHECK-LABEL: {{^}} var var_tuple4: (Int, Int)

  @objc // bad-access-note-move{{infer_instanceVar1.var_tuple4_}}
  var var_tuple4_: (Int, Int)
  // access-note-adjust{{@objc}} expected-error@-1 {{property cannot be marked @objc because its type cannot be represented in Objective-C}}
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

  @objc // bad-access-note-move{{infer_instanceVar1.var_PlainClass_}}
  var var_PlainClass_: PlainClass
  // access-note-adjust{{@objc}} expected-error@-1 {{property cannot be marked @objc because its type cannot be represented in Objective-C}}
  // expected-note@-2 {{classes not annotated with @objc cannot be represented in Objective-C}}

  var var_PlainStruct: PlainStruct
// CHECK-LABEL: {{^}}  var var_PlainStruct: PlainStruct

  @objc // bad-access-note-move{{infer_instanceVar1.var_PlainStruct_}}
  var var_PlainStruct_: PlainStruct
  // access-note-adjust{{@objc}} expected-error@-1 {{property cannot be marked @objc because its type cannot be represented in Objective-C}}
  // expected-note@-2 {{Swift structs cannot be represented in Objective-C}}

  var var_PlainEnum: PlainEnum
// CHECK-LABEL: {{^}} var var_PlainEnum: PlainEnum

  @objc // bad-access-note-move{{infer_instanceVar1.var_PlainEnum_}}
  var var_PlainEnum_: PlainEnum
  // access-note-adjust{{@objc}} expected-error@-1 {{property cannot be marked @objc because its type cannot be represented in Objective-C}}
  // expected-note@-2 {{non-'@objc' enums cannot be represented in Objective-C}}

  var var_PlainProtocol: PlainProtocol
// CHECK-LABEL: {{^}}  var var_PlainProtocol: any PlainProtocol

  @objc // bad-access-note-move{{infer_instanceVar1.var_PlainProtocol_}}
  var var_PlainProtocol_: PlainProtocol
  // access-note-adjust{{@objc}} expected-error@-1 {{property cannot be marked @objc because its type cannot be represented in Objective-C}}
  // expected-note@-2 {{protocol-constrained type containing protocol 'PlainProtocol' cannot be represented in Objective-C}}

  var var_ClassObjC: Class_ObjC1
// CHECK-LABEL: @objc var var_ClassObjC: Class_ObjC1

  @objc // access-note-move{{infer_instanceVar1.var_ClassObjC_}}
  var var_ClassObjC_: Class_ObjC1 // no-error

  var var_ProtocolClass: Protocol_Class1
// CHECK-LABEL: {{^}}  var var_ProtocolClass: any Protocol_Class1

  @objc // bad-access-note-move{{infer_instanceVar1.var_ProtocolClass_}}
  var var_ProtocolClass_: Protocol_Class1
  // access-note-adjust{{@objc}} expected-error@-1 {{property cannot be marked @objc because its type cannot be represented in Objective-C}}
  // expected-note@-2 {{protocol-constrained type containing protocol 'Protocol_Class1' cannot be represented in Objective-C}}

  var var_ProtocolObjC: Protocol_ObjC1
// CHECK-LABEL: @objc var var_ProtocolObjC: any Protocol_ObjC1

  @objc // access-note-move{{infer_instanceVar1.var_ProtocolObjC_}}
  var var_ProtocolObjC_: Protocol_ObjC1 // no-error


  var var_PlainClassMetatype: PlainClass.Type
// CHECK-LABEL: {{^}}  var var_PlainClassMetatype: PlainClass.Type

  @objc // bad-access-note-move{{infer_instanceVar1.var_PlainClassMetatype_}}
  var var_PlainClassMetatype_: PlainClass.Type
  // access-note-adjust{{@objc}} expected-error@-1 {{property cannot be marked @objc because its type cannot be represented in Objective-C}}

  var var_PlainStructMetatype: PlainStruct.Type
// CHECK-LABEL: {{^}}  var var_PlainStructMetatype: PlainStruct.Type

  @objc // bad-access-note-move{{infer_instanceVar1.var_PlainStructMetatype_}}
  var var_PlainStructMetatype_: PlainStruct.Type
  // access-note-adjust{{@objc}} expected-error@-1 {{property cannot be marked @objc because its type cannot be represented in Objective-C}}

  var var_PlainEnumMetatype: PlainEnum.Type
// CHECK-LABEL: {{^}}  var var_PlainEnumMetatype: PlainEnum.Type

  @objc // bad-access-note-move{{infer_instanceVar1.var_PlainEnumMetatype_}}
  var var_PlainEnumMetatype_: PlainEnum.Type
  // access-note-adjust{{@objc}} expected-error@-1 {{property cannot be marked @objc because its type cannot be represented in Objective-C}}

  var var_PlainExistentialMetatype: PlainProtocol.Type
// CHECK-LABEL: {{^}}  var var_PlainExistentialMetatype: any PlainProtocol.Type

  @objc // bad-access-note-move{{infer_instanceVar1.var_PlainExistentialMetatype_}}
  var var_PlainExistentialMetatype_: PlainProtocol.Type
  // access-note-adjust{{@objc}} expected-error@-1 {{property cannot be marked @objc because its type cannot be represented in Objective-C}}

  var var_ClassObjCMetatype: Class_ObjC1.Type
// CHECK-LABEL: @objc var var_ClassObjCMetatype: Class_ObjC1.Type

  @objc // access-note-move{{infer_instanceVar1.var_ClassObjCMetatype_}}
  var var_ClassObjCMetatype_: Class_ObjC1.Type // no-error

  var var_ProtocolClassMetatype: Protocol_Class1.Type
// CHECK-LABEL: {{^}}  var var_ProtocolClassMetatype: any Protocol_Class1.Type

  @objc // bad-access-note-move{{infer_instanceVar1.var_ProtocolClassMetatype_}}
  var var_ProtocolClassMetatype_: Protocol_Class1.Type
  // access-note-adjust{{@objc}} expected-error@-1 {{property cannot be marked @objc because its type cannot be represented in Objective-C}}

  var var_ProtocolObjCMetatype1: Protocol_ObjC1.Type
// CHECK-LABEL: @objc var var_ProtocolObjCMetatype1: any Protocol_ObjC1.Type

  @objc // access-note-move{{infer_instanceVar1.var_ProtocolObjCMetatype1_}}
  var var_ProtocolObjCMetatype1_: Protocol_ObjC1.Type // no-error

  var var_ProtocolObjCMetatype2: Protocol_ObjC2.Type
// CHECK-LABEL: @objc var var_ProtocolObjCMetatype2: any Protocol_ObjC2.Type

  @objc // access-note-move{{infer_instanceVar1.var_ProtocolObjCMetatype2_}}
  var var_ProtocolObjCMetatype2_: Protocol_ObjC2.Type // no-error

  var var_AnyObject1: AnyObject
  var var_AnyObject2: AnyObject.Type
// CHECK-LABEL: @objc var var_AnyObject1: AnyObject
// CHECK-LABEL: @objc var var_AnyObject2: any AnyObject.Type

  var var_Existential0: Any
// CHECK-LABEL: @objc var var_Existential0: Any

  @objc // access-note-move{{infer_instanceVar1.var_Existential0_}}
  var var_Existential0_: Any

  var var_Existential1: PlainProtocol
  // CHECK-LABEL: {{^}}  var var_Existential1: any PlainProtocol

  @objc // bad-access-note-move{{infer_instanceVar1.var_Existential1_}}
  var var_Existential1_: PlainProtocol
  // access-note-adjust{{@objc}} expected-error@-1 {{property cannot be marked @objc because its type cannot be represented in Objective-C}}
  // expected-note@-2 {{protocol-constrained type containing protocol 'PlainProtocol' cannot be represented in Objective-C}}

  var var_Existential2: PlainProtocol & PlainProtocol
// CHECK-LABEL: {{^}}  var var_Existential2: any PlainProtocol

  @objc // bad-access-note-move{{infer_instanceVar1.var_Existential2_}}
  var var_Existential2_: PlainProtocol & PlainProtocol
  // access-note-adjust{{@objc}} expected-error@-1 {{property cannot be marked @objc because its type cannot be represented in Objective-C}}
  // expected-note@-2 {{protocol-constrained type containing protocol 'PlainProtocol' cannot be represented in Objective-C}}

  var var_Existential3: PlainProtocol & Protocol_Class1
// CHECK-LABEL: {{^}}  var var_Existential3: any PlainProtocol & Protocol_Class1

  @objc // bad-access-note-move{{infer_instanceVar1.var_Existential3_}}
  var var_Existential3_: PlainProtocol & Protocol_Class1
  // access-note-adjust{{@objc}} expected-error@-1 {{property cannot be marked @objc because its type cannot be represented in Objective-C}}
  // expected-note@-2 {{protocol-constrained type containing protocol 'PlainProtocol' cannot be represented in Objective-C}}

  var var_Existential4: PlainProtocol & Protocol_ObjC1
// CHECK-LABEL: {{^}}  var var_Existential4: any PlainProtocol & Protocol_ObjC1

  @objc // bad-access-note-move{{infer_instanceVar1.var_Existential4_}}
  var var_Existential4_: PlainProtocol & Protocol_ObjC1
  // access-note-adjust{{@objc}} expected-error@-1 {{property cannot be marked @objc because its type cannot be represented in Objective-C}}
  // expected-note@-2 {{protocol-constrained type containing protocol 'PlainProtocol' cannot be represented in Objective-C}}

  var var_Existential5: Protocol_Class1
  // CHECK-LABEL: {{^}}  var var_Existential5: any Protocol_Class1

  @objc // bad-access-note-move{{infer_instanceVar1.var_Existential5_}}
  var var_Existential5_: Protocol_Class1
  // access-note-adjust{{@objc}} expected-error@-1 {{property cannot be marked @objc because its type cannot be represented in Objective-C}}
  // expected-note@-2 {{protocol-constrained type containing protocol 'Protocol_Class1' cannot be represented in Objective-C}}

  var var_Existential6: Protocol_Class1 & Protocol_Class2
// CHECK-LABEL: {{^}}  var var_Existential6: any Protocol_Class1 & Protocol_Class2

  @objc // bad-access-note-move{{infer_instanceVar1.var_Existential6_}}
  var var_Existential6_: Protocol_Class1 & Protocol_Class2
  // access-note-adjust{{@objc}} expected-error@-1 {{property cannot be marked @objc because its type cannot be represented in Objective-C}}
  // expected-note@-2 {{protocol-constrained type containing protocol 'Protocol_Class1' cannot be represented in Objective-C}}

  var var_Existential7: Protocol_Class1 & Protocol_ObjC1
// CHECK-LABEL: {{^}}  var var_Existential7: any Protocol_Class1 & Protocol_ObjC1

  @objc // bad-access-note-move{{infer_instanceVar1.var_Existential7_}}
  var var_Existential7_: Protocol_Class1 & Protocol_ObjC1
  // access-note-adjust{{@objc}} expected-error@-1 {{property cannot be marked @objc because its type cannot be represented in Objective-C}}
  // expected-note@-2 {{protocol-constrained type containing protocol 'Protocol_Class1' cannot be represented in Objective-C}}

  var var_Existential8: Protocol_ObjC1
// CHECK-LABEL: @objc var var_Existential8: any Protocol_ObjC1

  @objc // access-note-move{{infer_instanceVar1.var_Existential8_}}
  var var_Existential8_: Protocol_ObjC1 // no-error

  var var_Existential9: Protocol_ObjC1 & Protocol_ObjC2
// CHECK-LABEL: @objc var var_Existential9: any Protocol_ObjC1 & Protocol_ObjC2

  @objc // access-note-move{{infer_instanceVar1.var_Existential9_}}
  var var_Existential9_: Protocol_ObjC1 & Protocol_ObjC2 // no-error


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
// CHECK-LABEL: {{^}}  var var_ExistentialMetatype0: any Any.Type
// CHECK-LABEL: {{^}}  var var_ExistentialMetatype1: any PlainProtocol.Type
// CHECK-LABEL: {{^}}  var var_ExistentialMetatype2: any (PlainProtocol).Type
// CHECK-LABEL: {{^}}  var var_ExistentialMetatype3: any (PlainProtocol & Protocol_Class1).Type
// CHECK-LABEL: {{^}}  var var_ExistentialMetatype4: any (PlainProtocol & Protocol_ObjC1).Type
// CHECK-LABEL: {{^}}  var var_ExistentialMetatype5: any (Protocol_Class1).Type
// CHECK-LABEL: {{^}}  var var_ExistentialMetatype6: any (Protocol_Class1 & Protocol_Class2).Type
// CHECK-LABEL: {{^}}  var var_ExistentialMetatype7: any (Protocol_Class1 & Protocol_ObjC1).Type
// CHECK-LABEL: @objc var var_ExistentialMetatype8: any Protocol_ObjC1.Type
// CHECK-LABEL: @objc var var_ExistentialMetatype9: any (Protocol_ObjC1 & Protocol_ObjC2).Type


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
// CHECK-LABEL: {{^}}  var var_UnsafeMutablePointer11: UnsafeMutablePointer<any PlainProtocol>
// CHECK-LABEL: @objc var var_UnsafeMutablePointer12: UnsafeMutablePointer<AnyObject>
// CHECK-LABEL: var var_UnsafeMutablePointer13: UnsafeMutablePointer<any AnyObject.Type>
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
// CHECK-LABEL: @objc @_hasInitialValue var var_Optional2: (any Protocol_ObjC1)?
// CHECK-LABEL: @objc @_hasInitialValue var var_Optional3: Class_ObjC1.Type?
// CHECK-LABEL: @objc @_hasInitialValue var var_Optional4: (any Protocol_ObjC1.Type)?
// CHECK-LABEL: @objc @_hasInitialValue var var_Optional5: AnyObject?
// CHECK-LABEL: @objc @_hasInitialValue var var_Optional6: (any AnyObject.Type)?
// CHECK-LABEL: @objc @_hasInitialValue var var_Optional7: String?
// CHECK-LABEL: @objc @_hasInitialValue var var_Optional8: (any Protocol_ObjC1)?
// CHECK-LABEL: @objc @_hasInitialValue var var_Optional9: (any Protocol_ObjC1.Type)?
// CHECK-LABEL: @objc @_hasInitialValue var var_Optional10: (any Protocol_ObjC1 & Protocol_ObjC2)?
// CHECK-LABEL: @objc @_hasInitialValue var var_Optional11: (any (Protocol_ObjC1 & Protocol_ObjC2).Type)?
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
// CHECK-LABEL: @objc @_hasInitialValue var var_ImplicitlyUnwrappedOptional2: (any Protocol_ObjC1)!
// CHECK-LABEL: @objc @_hasInitialValue var var_ImplicitlyUnwrappedOptional3: Class_ObjC1.Type!
// CHECK-LABEL: @objc @_hasInitialValue var var_ImplicitlyUnwrappedOptional4: (any Protocol_ObjC1.Type)!
// CHECK-LABEL: @objc @_hasInitialValue var var_ImplicitlyUnwrappedOptional5: AnyObject!
// CHECK-LABEL: @objc @_hasInitialValue var var_ImplicitlyUnwrappedOptional6: (any AnyObject.Type)!
// CHECK-LABEL: @objc @_hasInitialValue var var_ImplicitlyUnwrappedOptional7: String!
// CHECK-LABEL: @objc @_hasInitialValue var var_ImplicitlyUnwrappedOptional8: (any Protocol_ObjC1)!
// CHECK-LABEL: @objc @_hasInitialValue var var_ImplicitlyUnwrappedOptional9: (any Protocol_ObjC1 & Protocol_ObjC2)!

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
  // CHECK-LABEL: {{^}} var var_CFunctionPointer_invalid_2: <<error type>>
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

// CHECK-LABEL: @objc @_hasInitialValue weak var var_Weak1: @sil_weak Class_ObjC1?
// CHECK-LABEL: @objc @_hasInitialValue weak var var_Weak2: @sil_weak (any Protocol_ObjC1)?
// CHECK-LABEL: @objc @_hasInitialValue weak var var_Weak5: @sil_weak AnyObject?
// CHECK-LABEL: @objc @_hasInitialValue weak var var_Weak7: @sil_weak (any Protocol_ObjC1)?
// CHECK-LABEL: @objc @_hasInitialValue weak var var_Weak8: @sil_weak (any Protocol_ObjC1 & Protocol_ObjC2)?

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
// CHECK-LABEL: @objc unowned var var_Unowned2: @sil_unowned any Protocol_ObjC1
// CHECK-LABEL: @objc unowned var var_Unowned5: @sil_unowned AnyObject
// CHECK-LABEL: @objc unowned var var_Unowned7: @sil_unowned any Protocol_ObjC1
// CHECK-LABEL: @objc unowned var var_Unowned8: @sil_unowned any Protocol_ObjC1 & Protocol_ObjC2


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
// CHECK-LABEL: {{^}}  var var_FunctionType12: (any PlainProtocol) -> ()

  var var_FunctionType13: (Class_ObjC1) -> ()
// CHECK-LABEL: {{^}}  @objc var var_FunctionType13: (Class_ObjC1) -> ()

  var var_FunctionType14: (Protocol_Class1) -> ()
// CHECK-LABEL: {{^}}  var var_FunctionType14: (any Protocol_Class1) -> ()

  var var_FunctionType15: (Protocol_ObjC1) -> ()
// CHECK-LABEL: {{^}}  @objc var var_FunctionType15: (any Protocol_ObjC1) -> ()

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

  @objc // access-note-move{{infer_instanceVar1.var_FunctionTypeReturn1_}}
  var var_FunctionTypeReturn1_: () -> () -> () // no-error

  var var_FunctionTypeReturn2: () -> (Int) -> ()
// CHECK-LABEL: {{^}}  @objc var var_FunctionTypeReturn2: () -> (Int) -> ()

  @objc // access-note-move{{infer_instanceVar1.var_FunctionTypeReturn2_}}
  var var_FunctionTypeReturn2_: () -> (Int) -> () // no-error

  var var_FunctionTypeReturn3: () -> () -> Int
// CHECK-LABEL: {{^}}  @objc var var_FunctionTypeReturn3: () -> () -> Int

  @objc // access-note-move{{infer_instanceVar1.var_FunctionTypeReturn3_}}
  var var_FunctionTypeReturn3_: () -> () -> Int // no-error

  var var_FunctionTypeReturn4: () -> (String) -> ()
// CHECK-LABEL: {{^}}  @objc var var_FunctionTypeReturn4: () -> (String) -> ()

  @objc // access-note-move{{infer_instanceVar1.var_FunctionTypeReturn4_}}
  var var_FunctionTypeReturn4_: () -> (String) -> () // no-error

  var var_FunctionTypeReturn5: () -> () -> String
// CHECK-LABEL: {{^}}  @objc var var_FunctionTypeReturn5: () -> () -> String

  @objc // access-note-move{{infer_instanceVar1.var_FunctionTypeReturn5_}}
  var var_FunctionTypeReturn5_: () -> () -> String // no-error


  var var_BlockFunctionType1: @convention(block) () -> ()
// CHECK-LABEL: @objc var var_BlockFunctionType1: @convention(block) () -> ()

  @objc // access-note-move{{infer_instanceVar1.var_BlockFunctionType1_}}
  var var_BlockFunctionType1_: @convention(block) () -> () // no-error

  var var_ArrayType1: [AnyObject]
  // CHECK-LABEL: {{^}}  @objc var var_ArrayType1: [AnyObject]

  @objc // access-note-move{{infer_instanceVar1.var_ArrayType1_}}
  var var_ArrayType1_: [AnyObject] // no-error

  var var_ArrayType2: [@convention(block) (AnyObject) -> AnyObject] // no-error
  // CHECK-LABEL: {{^}}  @objc var var_ArrayType2: [@convention(block) (AnyObject) -> AnyObject]

  @objc // access-note-move{{infer_instanceVar1.var_ArrayType2_}}
  var var_ArrayType2_: [@convention(block) (AnyObject) -> AnyObject] // no-error

  var var_ArrayType3: [PlainStruct]
  // CHECK-LABEL: {{^}}  var var_ArrayType3: [PlainStruct]

  @objc // bad-access-note-move{{infer_instanceVar1.var_ArrayType3_}}
  var var_ArrayType3_: [PlainStruct]
  // access-note-adjust{{@objc}} expected-error @-1{{property cannot be marked @objc because its type cannot be represented in Objective-C}}
  // expected-note@-2 {{Swift structs cannot be represented in Objective-C}}

  var var_ArrayType4: [(AnyObject) -> AnyObject] // no-error
  // CHECK-LABEL: {{^}}  var var_ArrayType4: [(AnyObject) -> AnyObject]

  @objc // bad-access-note-move{{infer_instanceVar1.var_ArrayType4_}}
  var var_ArrayType4_: [(AnyObject) -> AnyObject]
  // access-note-adjust{{@objc}} expected-error @-1{{property cannot be marked @objc because its type cannot be represented in Objective-C}}
  // expected-note@-2 {{Swift structs cannot be represented in Objective-C}}

  var var_ArrayType5: [Protocol_ObjC1]
  // CHECK-LABEL: {{^}}  @objc var var_ArrayType5: [any Protocol_ObjC1]

  @objc // access-note-move{{infer_instanceVar1.var_ArrayType5_}}
  var var_ArrayType5_: [Protocol_ObjC1] // no-error

  var var_ArrayType6: [Class_ObjC1]
  // CHECK-LABEL: {{^}}  @objc var var_ArrayType6: [Class_ObjC1]

  @objc // access-note-move{{infer_instanceVar1.var_ArrayType6_}}
  var var_ArrayType6_: [Class_ObjC1] // no-error

  var var_ArrayType7: [PlainClass]
  // CHECK-LABEL: {{^}}  var var_ArrayType7: [PlainClass]

  @objc // bad-access-note-move{{infer_instanceVar1.var_ArrayType7_}}
  var var_ArrayType7_: [PlainClass]
  // access-note-adjust{{@objc}} expected-error @-1{{property cannot be marked @objc because its type cannot be represented in Objective-C}}
  // expected-note@-2 {{Swift structs cannot be represented in Objective-C}}

  var var_ArrayType8: [PlainProtocol]
  // CHECK-LABEL: {{^}}  var var_ArrayType8: [any PlainProtocol]

  @objc // bad-access-note-move{{infer_instanceVar1.var_ArrayType8_}}
  var var_ArrayType8_: [PlainProtocol]
  // access-note-adjust{{@objc}} expected-error @-1{{property cannot be marked @objc because its type cannot be represented in Objective-C}}
  // expected-note@-2 {{Swift structs cannot be represented in Objective-C}}

  var var_ArrayType9: [Protocol_ObjC1 & PlainProtocol]
  // CHECK-LABEL: {{^}}  var var_ArrayType9: [any PlainProtocol & Protocol_ObjC1]

  @objc // bad-access-note-move{{infer_instanceVar1.var_ArrayType9_}}
  var var_ArrayType9_: [Protocol_ObjC1 & PlainProtocol]
  // access-note-adjust{{@objc}} expected-error @-1{{property cannot be marked @objc because its type cannot be represented in Objective-C}}
  // expected-note@-2 {{Swift structs cannot be represented in Objective-C}}

  var var_ArrayType10: [Protocol_ObjC1 & Protocol_ObjC2]
  // CHECK-LABEL: {{^}}  @objc var var_ArrayType10: [any Protocol_ObjC1 & Protocol_ObjC2]

  @objc // access-note-move{{infer_instanceVar1.var_ArrayType10_}}
  var var_ArrayType10_: [Protocol_ObjC1 & Protocol_ObjC2]
  // no-error

  var var_ArrayType11: [Any]
  // CHECK-LABEL: @objc var var_ArrayType11: [Any]

  @objc // access-note-move{{infer_instanceVar1.var_ArrayType11_}}
  var var_ArrayType11_: [Any]

  var var_ArrayType13: [Any?]
  // CHECK-LABEL: {{^}}  var var_ArrayType13: [Any?]

  @objc // bad-access-note-move{{infer_instanceVar1.var_ArrayType13_}}
  var var_ArrayType13_: [Any?]
  // access-note-adjust{{@objc}} expected-error @-1{{property cannot be marked @objc because its type cannot be represented in Objective-C}}
  // expected-note@-2 {{Swift structs cannot be represented in Objective-C}}

  var var_ArrayType15: [AnyObject?]
  // CHECK-LABEL: {{^}}  var var_ArrayType15: [AnyObject?]

  @objc // bad-access-note-move{{infer_instanceVar1.var_ArrayType15_}}
  var var_ArrayType15_: [AnyObject?]
  // access-note-adjust{{@objc}} expected-error @-1{{property cannot be marked @objc because its type cannot be represented in Objective-C}}
  // expected-note@-2 {{Swift structs cannot be represented in Objective-C}}

  var var_ArrayType16: [[@convention(block) (AnyObject) -> AnyObject]] // no-error
  // CHECK-LABEL: {{^}}  @objc var var_ArrayType16: {{\[}}[@convention(block) (AnyObject) -> AnyObject]]

  @objc // access-note-move{{infer_instanceVar1.var_ArrayType16_}}
  var var_ArrayType16_: [[@convention(block) (AnyObject) -> AnyObject]] // no-error

  var var_ArrayType17: [[(AnyObject) -> AnyObject]] // no-error
  // CHECK-LABEL: {{^}}  var var_ArrayType17: {{\[}}[(AnyObject) -> AnyObject]]

  @objc // bad-access-note-move{{infer_instanceVar1.var_ArrayType17_}}
  var var_ArrayType17_: [[(AnyObject) -> AnyObject]]
  // access-note-adjust{{@objc}} expected-error @-1{{property cannot be marked @objc because its type cannot be represented in Objective-C}}
  // expected-note@-2 {{Swift structs cannot be represented in Objective-C}}
}

@objc // access-note-move{{ObjCBase}}
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

  @objc // bad-access-note-move{{infer_instanceVar2.var_GP_Unconstrained_}}
  var var_GP_Unconstrained_: GP_Unconstrained
  // access-note-adjust{{@objc}} expected-error@-1 {{property cannot be marked @objc because its type cannot be represented in Objective-C}}
  // expected-note@-2 {{generic type parameters cannot be represented in Objective-C}}

  var var_GP_PlainClass: GP_PlainClass
// CHECK-LABEL: {{^}}  var var_GP_PlainClass: GP_PlainClass

  @objc // bad-access-note-move{{infer_instanceVar2.var_GP_PlainClass_}}
  var var_GP_PlainClass_: GP_PlainClass
  // access-note-adjust{{@objc}} expected-error@-1 {{property cannot be marked @objc because its type cannot be represented in Objective-C}}
  // expected-note@-2 {{generic type parameters cannot be represented in Objective-C}}

  var var_GP_PlainProtocol: GP_PlainProtocol
// CHECK-LABEL: {{^}}  var var_GP_PlainProtocol: GP_PlainProtocol

  @objc // bad-access-note-move{{infer_instanceVar2.var_GP_PlainProtocol_}}
  var var_GP_PlainProtocol_: GP_PlainProtocol
  // access-note-adjust{{@objc}} expected-error@-1 {{property cannot be marked @objc because its type cannot be represented in Objective-C}}
  // expected-note@-2 {{generic type parameters cannot be represented in Objective-C}}

  var var_GP_Class_ObjC: GP_Class_ObjC
// CHECK-LABEL: {{^}}  var var_GP_Class_ObjC: GP_Class_ObjC

  @objc // bad-access-note-move{{infer_instanceVar2.var_GP_Class_ObjC_}}
  var var_GP_Class_ObjC_: GP_Class_ObjC
  // access-note-adjust{{@objc}} expected-error@-1 {{property cannot be marked @objc because its type cannot be represented in Objective-C}}
  // expected-note@-2 {{generic type parameters cannot be represented in Objective-C}}

  var var_GP_Protocol_Class: GP_Protocol_Class
// CHECK-LABEL: {{^}}  var var_GP_Protocol_Class: GP_Protocol_Class

  @objc // bad-access-note-move{{infer_instanceVar2.var_GP_Protocol_Class_}}
  var var_GP_Protocol_Class_: GP_Protocol_Class
  // access-note-adjust{{@objc}} expected-error@-1 {{property cannot be marked @objc because its type cannot be represented in Objective-C}}
  // expected-note@-2 {{generic type parameters cannot be represented in Objective-C}}

  var var_GP_Protocol_ObjC: GP_Protocol_ObjC
// CHECK-LABEL: {{^}}  var var_GP_Protocol_ObjC: GP_Protocol_ObjC

  @objc // bad-access-note-move{{infer_instanceVar2.var_GP_Protocol_ObjCa}}
  var var_GP_Protocol_ObjCa: GP_Protocol_ObjC
  // access-note-adjust{{@objc}} expected-error@-1 {{property cannot be marked @objc because its type cannot be represented in Objective-C}}
  // expected-note@-2 {{generic type parameters cannot be represented in Objective-C}}

  func func_GP_Unconstrained(a: GP_Unconstrained) {}
// CHECK-LABEL: {{^}} func func_GP_Unconstrained(a: GP_Unconstrained) {

  @objc // bad-access-note-move{{infer_instanceVar2.func_GP_Unconstrained_(a:)}}
  func func_GP_Unconstrained_(a: GP_Unconstrained) {}
  // access-note-adjust{{@objc}} expected-error@-1 {{method cannot be marked @objc because the type of the parameter cannot be represented in Objective-C}}
  // expected-note@-2 {{generic type parameters cannot be represented in Objective-C}}

  @objc // bad-access-note-move{{infer_instanceVar2.func_GP_Unconstrained_()}}
  func func_GP_Unconstrained_() -> GP_Unconstrained {}
  // access-note-adjust{{@objc}} expected-error@-1 {{method cannot be marked @objc because its result type cannot be represented in Objective-C}}
  // expected-note@-2 {{generic type parameters cannot be represented in Objective-C}}

  @objc // bad-access-note-move{{infer_instanceVar2.func_GP_Class_ObjC__()}}
  func func_GP_Class_ObjC__() -> GP_Class_ObjC {}
  // access-note-adjust{{@objc}} expected-error@-1 {{method cannot be marked @objc because its result type cannot be represented in Objective-C}}
  // expected-note@-2 {{generic type parameters cannot be represented in Objective-C}}
}

class infer_instanceVar3 : Class_ObjC1 {
// CHECK-LABEL: @objc @_inheritsConvenienceInitializers class infer_instanceVar3 : Class_ObjC1 {

  var v1: Int = 0
// CHECK-LABEL: @objc @_hasInitialValue var v1: Int
}


@objc // access-note-move{{infer_instanceVar4}}
protocol infer_instanceVar4 {
// CHECK-LABEL: @objc protocol infer_instanceVar4 {

  var v1: Int { get }
// CHECK-LABEL: @objc var v1: Int { get }
}

// @!objc
class infer_instanceVar5 {
// CHECK-LABEL: {{^}}class infer_instanceVar5 {

  @objc // access-note-move{{infer_instanceVar5.instanceVar1}}
  var instanceVar1: Int {
  // CHECK: @objc var instanceVar1: Int
    get {}
    // CHECK: @objc get {}
    set {}
    // CHECK: @objc set {}
  }
}

@objc // access-note-move{{infer_staticVar1}}
class infer_staticVar1 {
// CHECK-LABEL: @objc class infer_staticVar1 {

  class var staticVar1: Int = 42 // expected-error {{class stored properties not supported}}
  // CHECK: @objc @_hasInitialValue class var staticVar1: Int
}

// @!objc
class infer_subscript1 {
// CHECK-LABEL: class infer_subscript1

  @objc // access-note-move{{infer_subscript1.subscript(_:)}}
  subscript(i: Int) -> Int {
  // CHECK: @objc subscript(i: Int) -> Int
    get {}
    // CHECK: @objc get {}
    set {}
    // CHECK: @objc set {}
  }
}


@objc // access-note-move{{infer_throughConformanceProto1}}
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
  // CHECK: {{^}}  @objc @IBAction @MainActor func goodAction(_ sender: AnyObject?) {

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

@objc // access-note-move{{TakesCPointers}}
class TakesCPointers {
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
@objc(NSObjC2) // access-note-move{{Class_ObjC2}}
class Class_ObjC2 {
// CHECK-LABEL: @objc(NSObjC2) class Class_ObjC2

  @objc(initWithMalice) // access-note-move{{Class_ObjC2.init(foo:)}}
  init(foo: ()) { }

  @objc(initWithIntent) // access-note-move{{Class_ObjC2.init(bar:)}}
  init(bar _: ()) { }

  @objc(initForMurder) // access-note-move{{Class_ObjC2.init()}}
  init() { }

  @objc(isFoo) // access-note-move{{Class_ObjC2.foo()}}
  func foo() -> Bool {}
  // CHECK-LABEL: @objc(isFoo) func foo() -> Bool {
}

@objc() // expected-error {{expected name within parentheses of @objc attribute}}
class Class_ObjC3 { 
}

// @objc with selector names
extension PlainClass {
  // CHECK-LABEL: @objc(setFoo:) dynamic func
  @objc(setFoo:) // access-note-move{{PlainClass.foo(b:)}}
  func foo(b: Bool) { }

  // CHECK-LABEL: @objc(setWithRed:green:blue:alpha:) dynamic func set
  @objc(setWithRed:green:blue:alpha:) // access-note-move{{PlainClass.set(_:green:blue:alpha:)}}
  func set(_: Float, green: Float, blue: Float, alpha: Float) { }

  // CHECK-LABEL: @objc(createWithRed:green:blue:alpha:) dynamic class func createWith
  @objc(createWithRed:green blue:alpha)
  class func createWithRed(_: Float, green: Float, blue: Float, alpha: Float) { }
  // expected-error@-2{{missing ':' after selector piece in @objc attribute}}{{28-28=:}}
  // expected-error@-3{{missing ':' after selector piece in @objc attribute}}{{39-39=:}}

  // CHECK-LABEL: @objc(::) dynamic func badlyNamed
  @objc(::) // access-note-move{{PlainClass.badlyNamed(_:y:)}}
  func badlyNamed(_: Int, y: Int) {}
}

@objc(Class:) // bad-access-note-move{{BadClass1}} expected-error{{'@objc' class must have a simple name}}{{12-13=}}
class BadClass1 { }

@objc(Protocol:) // bad-access-note-move{{BadProto1}} expected-error{{'@objc' protocol must have a simple name}}{{15-16=}}
protocol BadProto1 { }

@objc(Enum:) // bad-access-note-move{{BadEnum1}} expected-error{{'@objc' enum must have a simple name}}{{11-12=}}
enum BadEnum1: Int { case X }

@objc // access-note-move{{BadEnum2}}
enum BadEnum2: Int {
  @objc(X:) // bad-access-note-move{{BadEnum2.X}} expected-error{{'@objc' enum case must have a simple name}}{{10-11=}}
  case X
}

class BadClass2 {
  @objc(realDealloc) // expected-error{{'@objc' deinitializer cannot have a name}}
  deinit { }

  @objc(badprop:foo:wibble:) // bad-access-note-move{{BadClass2.badprop}} expected-error{{'@objc' property must have a simple name}}{{16-28=}}
  var badprop: Int = 5

  @objc(foo) // bad-access-note-move{{BadClass2.subscript(_:)}} expected-error{{'@objc' subscript cannot have a name; did you mean to put the name on the getter or setter?}}
  subscript (i: Int) -> Int {
    get {
      return i
    }
  }

  @objc(foo) // bad-access-note-move{{BadClass2.noArgNamesOneParam(x:)}} expected-error{{'@objc' method name provides names for 0 arguments, but method has one parameter}}
  func noArgNamesOneParam(x: Int) { }
  
  @objc(foo) // bad-access-note-move{{BadClass2.noArgNamesOneParam2(_:)}} expected-error{{'@objc' method name provides names for 0 arguments, but method has one parameter}}
  func noArgNamesOneParam2(_: Int) { }

  @objc(foo) // bad-access-note-move{{BadClass2.noArgNamesTwoParams(_:y:)}} expected-error{{'@objc' method name provides names for 0 arguments, but method has 2 parameters}}
  func noArgNamesTwoParams(_: Int, y: Int) { }

  @objc(foo:) // bad-access-note-move{{BadClass2.oneArgNameTwoParams(_:y:)}} expected-error{{'@objc' method name provides one argument name, but method has 2 parameters}}
  func oneArgNameTwoParams(_: Int, y: Int) { }

  @objc(foo:) // bad-access-note-move{{BadClass2.oneArgNameNoParams()}} expected-error{{'@objc' method name provides one argument name, but method has 0 parameters}}
  func oneArgNameNoParams() { }

  @objc(foo:) // bad-access-note-move{{BadClass2.init()}} expected-error{{'@objc' initializer name provides one argument name, but initializer has 0 parameters}}
  init() { }

  var _prop = 5
  @objc // access-note-move{{BadClass2.prop}}
  var prop: Int {
    @objc(property) // access-note-move{{getter:BadClass2.prop()}}
    get { return _prop }
    @objc(setProperty:) // access-note-move{{setter:BadClass2.prop()}}
    set { _prop = newValue }
  }

  var prop2: Int {
    @objc(property) // bad-access-note-move{{getter:BadClass2.prop2()}} expected-error{{'@objc' getter for non-'@objc' property}} {{5-21=}}
    get { return _prop }
    @objc(setProperty:) // bad-access-note-move{{setter:BadClass2.prop2()}} expected-error{{'@objc' setter for non-'@objc' property}} {{5-25=}}
    set { _prop = newValue }
  }

  var prop3: Int {
    @objc(setProperty:) // expected-error{{observing accessors are not allowed to be marked @objc}} {{5-25=}}
    didSet { }
  }
}

// FIXME: This could be part of BadClass except that access notes can't
// distinguish between overloads of `subscript(_:)`.
class GoodClass {
  @objc // access-note-move{{GoodClass.subscript(_:)}}
  subscript (c: Class_ObjC1) -> Class_ObjC1 {
    @objc(getAtClass:) // access-note-move{{getter:GoodClass.subscript(_:)}}
    get {
      return c
    }

    @objc(setAtClass:class:) // access-note-move{{setter:GoodClass.subscript(_:)}}
    set {
    }
  }
}

// Swift overrides that aren't also @objc overrides.
class Super {
  @objc(renamedFoo) // access-note-move{{Super.foo}}
  var foo: Int { get { return 3 } } // expected-note 2{{overridden declaration is here}}

  @objc // access-note-move{{Super.process(i:)}}
  func process(i: Int) -> Int { } // expected-note {{overriding '@objc' method 'process(i:)' here}}
}

class Sub1 : Super {
  @objc(foo) // bad-access-note-move{{Sub1.foo}} expected-error{{Objective-C property has a different name from the property it overrides ('foo' vs. 'renamedFoo')}}{{9-12=renamedFoo}}
  override var foo: Int { get { return 5 } }

  override func process(i: Int?) -> Int { } // expected-error{{method cannot be an @objc override because the type of the parameter cannot be represented in Objective-C}}
}

class Sub2 : Super {
  @objc // bad-access-note-move{{Sub2.foo}} -- @objc is already implied by overriding an @objc attribute, so access notes shouldn't emit a remark
  override var foo: Int { get { return 5 } }
}

class Sub3 : Super {
  override var foo: Int { get { return 5 } }
}

class Sub4 : Super {
  @objc(renamedFoo) // access-note-move{{Sub4.foo}}
  override var foo: Int { get { return 5 } }
}

class Sub5 : Super {
  @objc(wrongFoo) // bad-access-note-move{{Sub5.foo}} expected-error{{Objective-C property has a different name from the property it overrides ('wrongFoo' vs. 'renamedFoo')}} {{9-17=renamedFoo}}
  override var foo: Int { get { return 5 } }
}

enum NotObjCEnum { case X }
struct NotObjCStruct {}

// Closure arguments can only be @objc if their parameters and returns are.
// CHECK-LABEL: @objc class ClosureArguments
@objc // access-note-move{{ClosureArguments}}
class ClosureArguments {
  // CHECK: @objc func foo
  @objc // access-note-move{{ClosureArguments.foo(f:)}}
  func foo(f: (Int) -> ()) {}
  // CHECK: @objc func bar
  @objc // bad-access-note-move{{ClosureArguments.bar(f:)}}
  func bar(f: (NotObjCEnum) -> NotObjCStruct) {} // access-note-adjust{{@objc}} expected-error{{method cannot be marked @objc because the type of the parameter cannot be represented in Objective-C}} expected-note{{function types cannot be represented in Objective-C unless their parameters and returns can be}}
  // CHECK: @objc func bas
  @objc // bad-access-note-move{{ClosureArguments.bas(f:)}}
  func bas(f: (NotObjCEnum) -> ()) {} // access-note-adjust{{@objc}} expected-error{{method cannot be marked @objc because the type of the parameter cannot be represented in Objective-C}} expected-note{{function types cannot be represented in Objective-C unless their parameters and returns can be}}
  // CHECK: @objc func zim
  @objc // bad-access-note-move{{ClosureArguments.zim(f:)}}
  func zim(f: () -> NotObjCStruct) {} // access-note-adjust{{@objc}} expected-error{{method cannot be marked @objc because the type of the parameter cannot be represented in Objective-C}} expected-note{{function types cannot be represented in Objective-C unless their parameters and returns can be}}
  // CHECK: @objc func zang
  @objc // bad-access-note-move{{ClosureArguments.zang(f:)}}
  func zang(f: (NotObjCEnum, NotObjCStruct) -> ()) {} // access-note-adjust{{@objc}} expected-error{{method cannot be marked @objc because the type of the parameter cannot be represented in Objective-C}} expected-note{{function types cannot be represented in Objective-C unless their parameters and returns can be}}
  @objc // bad-access-note-move{{ClosureArguments.zangZang(f:)}}
  func zangZang(f: (Int...) -> ()) {} // access-note-adjust{{@objc}} expected-error{{method cannot be marked @objc because the type of the parameter cannot be represented in Objective-C}} expected-note{{function types cannot be represented in Objective-C unless their parameters and returns can be}}
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


@objc // access-note-move{{AccessControl}}
class AccessControl {
  // CHECK: @objc func foo
  func foo() {}
  // CHECK: {{^}} private func bar
  private func bar() {}
  // CHECK: @objc private func baz
  @objc // access-note-move{{AccessControl.baz()}}
  private func baz() {}
}

//===--- Ban @objc +load methods
class Load1 {
  // Okay: not @objc
  class func load() { }
  class func alloc() {}
  class func allocWithZone(_: Int) {}
  class func initialize() {}
}

@objc // access-note-move{{Load2}}
class Load2 {
  class func load() { } // expected-error {{method 'load()' defines Objective-C class method 'load', which is not permitted by Swift}}
  class func alloc() {} // expected-error {{method 'alloc()' defines Objective-C class method 'alloc', which is not permitted by Swift}}
  class func allocWithZone(_: Int) {} // expected-error {{method 'allocWithZone' defines Objective-C class method 'allocWithZone:', which is not permitted by Swift}}
  class func initialize() {} // expected-error {{method 'initialize()' defines Objective-C class method 'initialize', which is not permitted by Swift}}
}

@objc // access-note-move{{Load3}}
class Load3 {
  class var load: Load3 {
    get { return Load3() } // expected-error {{getter for 'load' defines Objective-C class method 'load', which is not permitted by Swift}}
    set { }
  }

  @objc(alloc) // access-note-move{{Load3.prop}}
  class var prop: Int { return 0 } // expected-error {{getter for 'prop' defines Objective-C class method 'alloc', which is not permitted by Swift}}
  @objc(allocWithZone:) // access-note-move{{Load3.fooWithZone(_:)}}
  class func fooWithZone(_: Int) {} // expected-error {{method 'fooWithZone' defines Objective-C class method 'allocWithZone:', which is not permitted by Swift}}
  @objc(initialize) // access-note-move{{Load3.barnitialize()}}
  class func barnitialize() {} // expected-error {{method 'barnitialize()' defines Objective-C class method 'initialize', which is not permitted by Swift}}
}

// Members of protocol extensions cannot be @objc

extension PlainProtocol {
  @objc // bad-access-note-move{{PlainProtocol.property}} expected-error{{@objc can only be used with members of classes, @objc protocols, and concrete extensions of classes}}
  var property: Int { return 5 }

  @objc // bad-access-note-move{{PlainProtocol.subscript(_:)}} expected-error{{@objc can only be used with members of classes, @objc protocols, and concrete extensions of classes}}
  subscript(x: Int) -> Class_ObjC1 { return Class_ObjC1() }

  @objc // bad-access-note-move{{PlainProtocol.fun()}} expected-error{{@objc can only be used with members of classes, @objc protocols, and concrete extensions of classes}}
  func fun() { }
}

extension Protocol_ObjC1 {
  @objc // bad-access-note-move{{Protocol_ObjC1.property}} expected-error{{@objc can only be used with members of classes, @objc protocols, and concrete extensions of classes}}
  var property: Int { return 5 }

  @objc // bad-access-note-move{{Protocol_ObjC1.subscript(_:)}} expected-error{{@objc can only be used with members of classes, @objc protocols, and concrete extensions of classes}}
  subscript(x: Int) -> Class_ObjC1 { return Class_ObjC1() }

  @objc // bad-access-note-move{{Protocol_ObjC1.fun()}} expected-error{{@objc can only be used with members of classes, @objc protocols, and concrete extensions of classes}}
  func fun() { }
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
  @objc // access-note-move{{ClassThrows1.methodReturnsVoid()}}
  func methodReturnsVoid() throws { }

  // CHECK: @objc func methodReturnsObjCClass() throws -> Class_ObjC1
  @objc // access-note-move{{ClassThrows1.methodReturnsObjCClass()}}
  func methodReturnsObjCClass() throws -> Class_ObjC1 {
    return Class_ObjC1()
  }

  // CHECK: @objc func methodReturnsBridged() throws -> String
  @objc // access-note-move{{ClassThrows1.methodReturnsBridged()}}
  func methodReturnsBridged() throws -> String { return String() }

  // CHECK: @objc func methodReturnsArray() throws -> [String]
  @objc // access-note-move{{ClassThrows1.methodReturnsArray()}}
  func methodReturnsArray() throws -> [String] { return [String]() }

  // CHECK: @objc init(degrees: Double) throws
  @objc // access-note-move{{ClassThrows1.init(degrees:)}}
  init(degrees: Double) throws { }

  // Errors

  @objc // bad-access-note-move{{ClassThrows1.methodReturnsOptionalObjCClass()}}
  func methodReturnsOptionalObjCClass() throws -> Class_ObjC1? { return nil } // access-note-adjust{{@objc}} expected-error{{throwing method cannot be marked @objc because it returns a value of optional type 'Class_ObjC1?'; 'nil' indicates failure to Objective-C}}

  @objc // bad-access-note-move{{ClassThrows1.methodReturnsOptionalArray()}}
  func methodReturnsOptionalArray() throws -> [String]? { return nil } // access-note-adjust{{@objc}} expected-error{{throwing method cannot be marked @objc because it returns a value of optional type '[String]?'; 'nil' indicates failure to Objective-C}}

  @objc // bad-access-note-move{{ClassThrows1.methodReturnsInt()}}
  func methodReturnsInt() throws -> Int { return 0 } // access-note-adjust{{@objc}} expected-error{{throwing method cannot be marked @objc because it returns a value of type 'Int'; return 'Void' or a type that bridges to an Objective-C class}}

  @objc // bad-access-note-move{{ClassThrows1.methodAcceptsThrowingFunc(fn:)}}
  func methodAcceptsThrowingFunc(fn: (String) throws -> Int) { }
  // access-note-adjust{{@objc}} expected-error@-1{{method cannot be marked @objc because the type of the parameter cannot be represented in Objective-C}}
  // expected-note@-2{{throwing function types cannot be represented in Objective-C}}

  @objc // bad-access-note-move{{ClassThrows1.init(radians:)}}
  init?(radians: Double) throws { } // access-note-adjust{{@objc}} expected-error{{a failable and throwing initializer cannot be marked @objc because 'nil' indicates failure to Objective-C}}

  @objc // bad-access-note-move{{ClassThrows1.init(string:)}}
  init!(string: String) throws { } // access-note-adjust{{@objc}} expected-error{{a failable and throwing initializer cannot be marked @objc because 'nil' indicates failure to Objective-C}}

  @objc // bad-access-note-move{{ClassThrows1.fooWithErrorEnum1(x:)}}
  func fooWithErrorEnum1(x: ErrorEnum) {}
  // access-note-adjust{{@objc}} expected-error@-1{{method cannot be marked @objc because the type of the parameter cannot be represented in Objective-C}}
  // expected-note@-2{{non-'@objc' enums cannot be represented in Objective-C}}

  // CHECK: {{^}} func fooWithErrorEnum2(x: ErrorEnum)
  func fooWithErrorEnum2(x: ErrorEnum) {}

  @objc // bad-access-note-move{{ClassThrows1.fooWithErrorProtocolComposition1(x:)}}
  func fooWithErrorProtocolComposition1(x: Error & Protocol_ObjC1) { }
  // access-note-adjust{{@objc}} expected-error@-1{{method cannot be marked @objc because the type of the parameter cannot be represented in Objective-C}}
  // expected-note@-2{{protocol-constrained type containing 'Error' cannot be represented in Objective-C}}

  // CHECK: {{^}} func fooWithErrorProtocolComposition2(x: any Error & Protocol_ObjC1)
  func fooWithErrorProtocolComposition2(x: Error & Protocol_ObjC1) { }
}


// CHECK-DUMP-LABEL: class_decl{{.*}}"ImplicitClassThrows1"
@objc // access-note-move{{ImplicitClassThrows1}}
class ImplicitClassThrows1 {
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

  @objc // bad-access-note-move{{ImplicitClassThrows1.methodReturnsBridgedValueType2()}}
  func methodReturnsBridgedValueType2() throws -> NSRange {
    return NSRange()
  }
  // access-note-adjust{{@objc}} expected-error@-3{{throwing method cannot be marked @objc because it returns a value of type 'NSRange' (aka '_NSRange'); return 'Void' or a type that bridges to an Objective-C class}}

  // CHECK: {{^}} @objc func methodReturnsError() throws -> any Error
  func methodReturnsError() throws -> Error { return ErrorEnum.failed }

  // CHECK: @objc func methodReturnStaticBridged() throws -> ((Int) -> (Int) -> Int)
  func methodReturnStaticBridged() throws -> ((Int) -> (Int) -> Int) {
    func add(x: Int) -> (Int) -> Int { 
      return { x + $0 }
    }
  }
}

// CHECK-DUMP-LABEL: class_decl{{.*}}"SubclassImplicitClassThrows1"
@objc // access-note-move{{SubclassImplicitClassThrows1}}
class SubclassImplicitClassThrows1 : ImplicitClassThrows1 {
  // CHECK: @objc override func methodWithTrailingClosures(_ s: String, fn1: @escaping ((Int) -> Int), fn2: @escaping ((Int) -> Int), fn3: @escaping ((Int) -> Int))
  // CHECK-DUMP: func_decl{{.*}}"methodWithTrailingClosures(_:fn1:fn2:fn3:)"{{.*}}foreign_error=ZeroResult,unowned,param=1,paramtype=Optional<AutoreleasingUnsafeMutablePointer<Optional<NSError>>>,resulttype=ObjCBool
  override func methodWithTrailingClosures(_ s: String, fn1: (@escaping (Int) -> Int), fn2: (@escaping (Int) -> Int), fn3: (@escaping (Int) -> Int)) throws { }
}

class ThrowsRedecl1 {
  @objc // access-note-move{{ThrowsRedecl1.method1(_:error:)}}
  func method1(_ x: Int, error: Class_ObjC1) { } // expected-note{{declared here}}
  @objc // bad-access-note-move{{ThrowsRedecl1.method1(_:)}}
  func method1(_ x: Int) throws { } // access-note-adjust{{@objc}} expected-error {{method 'method1' with Objective-C selector 'method1:error:' conflicts with method 'method1(_:error:)' with the same Objective-C selector}}

  @objc // access-note-move{{ThrowsRedecl1.method2AndReturnError(_:)}}
  func method2AndReturnError(_ x: Int) { } // expected-note{{declared here}}
  @objc // bad-access-note-move{{ThrowsRedecl1.method2()}}
  func method2() throws { } // access-note-adjust{{@objc}} expected-error {{method 'method2()' with Objective-C selector 'method2AndReturnError:' conflicts with method 'method2AndReturnError' with the same Objective-C selector}}

  @objc // access-note-move{{ThrowsRedecl1.method3(_:error:closure:)}}
  func method3(_ x: Int, error: Int, closure: @escaping (Int) -> Int) { }  // expected-note{{declared here}}
  @objc // bad-access-note-move{{ThrowsRedecl1.method3(_:closure:)}}
  func method3(_ x: Int, closure: (Int) -> Int) throws { } // access-note-adjust{{@objc}} expected-error {{method 'method3(_:closure:)' with Objective-C selector 'method3:error:closure:' conflicts with method 'method3(_:error:closure:)' with the same Objective-C selector}}

  @objc(initAndReturnError:) // access-note-move{{ThrowsRedecl1.initMethod1(error:)}}
  func initMethod1(error: Int) { } // expected-note{{declared here}}
  @objc // bad-access-note-move{{ThrowsRedecl1.init()}}
  init() throws { } // access-note-adjust{{@objc}} expected-error {{initializer 'init()' with Objective-C selector 'initAndReturnError:' conflicts with method 'initMethod1(error:)' with the same Objective-C selector}}

  @objc(initWithString:error:) // access-note-move{{ThrowsRedecl1.initMethod2(string:error:)}}
  func initMethod2(string: String, error: Int) { } // expected-note{{declared here}}
  @objc // bad-access-note-move{{ThrowsRedecl1.init(string:)}}
  init(string: String) throws { } // access-note-adjust{{@objc}} expected-error {{initializer 'init(string:)' with Objective-C selector 'initWithString:error:' conflicts with method 'initMethod2(string:error:)' with the same Objective-C selector}}

  @objc(initAndReturnError:fn:) // access-note-move{{ThrowsRedecl1.initMethod3(error:fn:)}}
  func initMethod3(error: Int, fn: @escaping (Int) -> Int) { } // expected-note{{declared here}}
  @objc // bad-access-note-move{{ThrowsRedecl1.init(fn:)}}
  init(fn: (Int) -> Int) throws { } // access-note-adjust{{@objc}} expected-error {{initializer 'init(fn:)' with Objective-C selector 'initAndReturnError:fn:' conflicts with method 'initMethod3(error:fn:)' with the same Objective-C selector}}
}

class ThrowsObjCName {
  @objc(method4:closure:error:) // access-note-move{{ThrowsObjCName.method4(x:closure:)}}
  func method4(x: Int, closure: @escaping (Int) -> Int) throws { }

  @objc(method5AndReturnError:x:closure:) // access-note-move{{ThrowsObjCName.method5(x:closure:)}}
  func method5(x: Int, closure: @escaping (Int) -> Int) throws { }

  @objc(method6) // bad-access-note-move{{ThrowsObjCName.method6()}} expected-error{{'@objc' method name provides names for 0 arguments, but method has one parameter (the error parameter)}}
  func method6() throws { }

  @objc(method7) // bad-access-note-move{{ThrowsObjCName.method7(x:)}} expected-error{{'@objc' method name provides names for 0 arguments, but method has 2 parameters (including the error parameter)}}
  func method7(x: Int) throws { }

  // CHECK-DUMP: func_decl{{.*}}"method8(_:fn1:fn2:)"{{.*}}foreign_error=ZeroResult,unowned,param=2,paramtype=Optional<AutoreleasingUnsafeMutablePointer<Optional<NSError>>>,resulttype=ObjCBool
  @objc(method8:fn1:error:fn2:) // access-note-move{{ThrowsObjCName.method8(_:fn1:fn2:)}}
  func method8(_ s: String, fn1: (@escaping (Int) -> Int), fn2: @escaping (Int) -> Int) throws { }

  // CHECK-DUMP: func_decl{{.*}}"method9(_:fn1:fn2:)"{{.*}}foreign_error=ZeroResult,unowned,param=0,paramtype=Optional<AutoreleasingUnsafeMutablePointer<Optional<NSError>>>,resulttype=ObjCBool
  @objc(method9AndReturnError:s:fn1:fn2:) // access-note-move{{ThrowsObjCName.method9(_:fn1:fn2:)}}
  func method9(_ s: String, fn1: (@escaping (Int) -> Int), fn2: @escaping (Int) -> Int) throws { }
}

class SubclassThrowsObjCName : ThrowsObjCName {
  // CHECK-DUMP: func_decl{{.*}}"method8(_:fn1:fn2:)"{{.*}}foreign_error=ZeroResult,unowned,param=2,paramtype=Optional<AutoreleasingUnsafeMutablePointer<Optional<NSError>>>,resulttype=ObjCBool
  override func method8(_ s: String, fn1: (@escaping (Int) -> Int), fn2: @escaping (Int) -> Int) throws { }

  // CHECK-DUMP: func_decl{{.*}}"method9(_:fn1:fn2:)"{{.*}}foreign_error=ZeroResult,unowned,param=0,paramtype=Optional<AutoreleasingUnsafeMutablePointer<Optional<NSError>>>,resulttype=ObjCBool
  override func method9(_ s: String, fn1: (@escaping (Int) -> Int), fn2: @escaping (Int) -> Int) throws { }
}

@objc // access-note-move{{ProtocolThrowsObjCName}}
protocol ProtocolThrowsObjCName {
  @objc // Access notes don't allow the `optional` keyword, that's fine, honestly.
  optional func doThing(_ x: String) throws -> String // expected-note{{requirement 'doThing' declared here}}
}

class ConformsToProtocolThrowsObjCName1 : ProtocolThrowsObjCName {
  @objc // bad-access-note-move{{ConformsToProtocolThrowsObjCName1.doThing(_:)}} -- @objc inherited, so no remarks
  func doThing(_ x: String) throws -> String { return x } // okay
}

class ConformsToProtocolThrowsObjCName2 : ProtocolThrowsObjCName {
  @objc // access-note-move{{ConformsToProtocolThrowsObjCName2.doThing(_:)}}
  func doThing(_ x: Int) throws -> String { return "" }
  // expected-warning@-1{{instance method 'doThing' nearly matches optional requirement 'doThing' of protocol 'ProtocolThrowsObjCName'}}
  // expected-note@-2{{move 'doThing' to an extension to silence this warning}}
  // expected-note@-3{{make 'doThing' private to silence this warning}}{{3-3=private }}
  // expected-note@-4{{candidate has non-matching type '(Int) throws -> String'}}
}

@objc // access-note-move{{DictionaryTest}}
class DictionaryTest {
  // CHECK-LABEL: @objc func func_dictionary1a(x: Dictionary<ObjC_Class1, ObjC_Class1>)
  func func_dictionary1a(x: Dictionary<ObjC_Class1, ObjC_Class1>) { }

  // CHECK-LABEL: @objc func func_dictionary1b(x: Dictionary<ObjC_Class1, ObjC_Class1>)
  @objc // access-note-move{{DictionaryTest.func_dictionary1b(x:)}}
  func func_dictionary1b(x: Dictionary<ObjC_Class1, ObjC_Class1>) { }

  func func_dictionary2a(x: Dictionary<String, Int>) { }
  @objc // access-note-move{{DictionaryTest.func_dictionary2b(x:)}}
  func func_dictionary2b(x: Dictionary<String, Int>) { }
}

@objc
extension PlainClass {
  // CHECK-LABEL: @objc final func objc_ext_objc_okay(_: Int) {
  final func objc_ext_objc_okay(_: Int) { }

  final func objc_ext_objc_not_okay(_: PlainStruct) { }
  // expected-error@-1{{method cannot be in an @objc extension of a class (without @nonobjc) because the type of the parameter cannot be represented in Objective-C}}
  // expected-note@-2 {{Swift structs cannot be represented in Objective-C}}

  // CHECK-LABEL: {{^}} @nonobjc final func objc_ext_objc_explicit_nonobjc(_: PlainStruct) {
  @nonobjc final func objc_ext_objc_explicit_nonobjc(_: PlainStruct) { }
}

@objc // access-note-move{{ObjC_Class1}}
class ObjC_Class1 : Hashable {
  func hash(into hasher: inout Hasher) {}
}

func ==(lhs: ObjC_Class1, rhs: ObjC_Class1) -> Bool {
  return true
}

// CHECK-LABEL: @objc class OperatorInClass
@objc // access-note-move{{OperatorInClass}}
class OperatorInClass {
  // CHECK: {{^}} static func == (lhs: OperatorInClass, rhs: OperatorInClass) -> Bool
  static func ==(lhs: OperatorInClass, rhs: OperatorInClass) -> Bool {
    return true
  }
  // CHECK: {{^}} @objc static func + (lhs: OperatorInClass, rhs: OperatorInClass) -> OperatorInClass
  @objc
  static func +(lhs: OperatorInClass, rhs: OperatorInClass) -> OperatorInClass { // expected-error {{operator methods cannot be declared @objc}}
    return lhs
  }
} // CHECK: {{^}$}}

@objc // access-note-move{{OperatorInProtocol}}
protocol OperatorInProtocol {
  static func +(lhs: Self, rhs: Self) -> Self // expected-error {{@objc protocols must not have operator requirements}}
}

class AdoptsOperatorInProtocol : OperatorInProtocol {
  static func +(lhs: AdoptsOperatorInProtocol, rhs: AdoptsOperatorInProtocol) -> Self {}
  // expected-error@-1 {{operator methods cannot be declared @objc}}
}

//===--- @objc inference for witnesses

@objc // access-note-move{{InferFromProtocol}}
protocol InferFromProtocol {
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

@objc // access-note-move{{NeverReturningMethod}}
class NeverReturningMethod {
  @objc // access-note-move{{NeverReturningMethod.doesNotReturn()}}
  func doesNotReturn() -> Never {}
}

// https://github.com/apple/swift/issues/47601
class User: NSObject {
}

@objc
extension User {
	var name: String {
		get {
			return "No name"
		}
		set {
			// Nothing
		}
	}

	var other: String {
    unsafeAddress { // expected-error {{addressors are not allowed to be marked @objc}}
    }
  }
}

// 'dynamic' methods cannot be @inlinable.
class BadClass {
  @objc // access-note-move{{BadClass.badMethod1()}}
  @inlinable dynamic func badMethod1() {}
  // expected-error@-1 {{'@inlinable' attribute cannot be applied to 'dynamic' declarations}}
}

@objc // access-note-move{{ObjCProtocolWithWeakProperty}}
protocol ObjCProtocolWithWeakProperty {
   weak var weakProp: AnyObject? { get set } // okay
}

@objc // access-note-move{{ObjCProtocolWithUnownedProperty}}
protocol ObjCProtocolWithUnownedProperty {
   unowned var unownedProp: AnyObject { get set } // okay
}

// rdar://problem/46699152: errors about read/modify accessors being implicitly
// marked @objc.
@objc // access-note-move{{MyObjCClass}}
class MyObjCClass: NSObject {}

@objc
extension MyObjCClass {
    @objc // access-note-move{{MyObjCClass.objCVarInObjCExtension}}
    static var objCVarInObjCExtension: Bool {
        get {
            return true
        }
        set {}
    }

    // CHECK: {{^}} @objc private dynamic func stillExposedToObjCDespiteBeingPrivate()
    private func stillExposedToObjCDespiteBeingPrivate() {}
}

@objc
private extension MyObjCClass {
  // CHECK: {{^}} @objc dynamic func alsoExposedToObjCDespiteBeingPrivate()
  func alsoExposedToObjCDespiteBeingPrivate() {}
}

@objcMembers class VeryObjCClass: NSObject {
  // CHECK: {{^}} private func notExposedToObjC()
  private func notExposedToObjC() {}
}

// https://github.com/apple/swift/issues/51538

class issue51538_C {}

@objc // access-note-move{{issue51538_P}}
protocol issue51538_P {
  func throwingMethod1() throws -> Unmanaged<CFArray> // Ok
  func throwingMethod2() throws -> Unmanaged<issue51538_C> // expected-error {{method cannot be a member of an @objc protocol because its result type cannot be represented in Objective-C}}
  // expected-note@-1 {{inferring '@objc' because the declaration is a member of an '@objc' protocol}}
  // expected-note@-2 {{Swift structs cannot be represented in Objective-C}}
}

// https://github.com/apple/swift/issues/55246
// Make sure we reject an @objc generic subscript.
class issue55246 {
  @objc // bad-access-note-move{{issue55246.subscript(_:)}}
  subscript<T>(foo : [T]) -> Int { return 0 }
  // access-note-adjust{{@objc}} expected-error@-1 {{subscript cannot be marked @objc because it has generic parameters}}
}

// @backDeployed

public class BackDeployClass {
  @backDeployed(before: macOS 12.0) // expected-error {{'@backDeployed' must not be used on an '@objc' instance method}}
  @objc
  final public func objcMethod() {}
}
