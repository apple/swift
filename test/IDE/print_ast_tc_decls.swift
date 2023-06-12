// RUN: %empty-directory(%t)
//
// Build swift modules this test depends on.
// RUN: %target-swift-frontend -emit-module -o %t %S/Inputs/foo_swift_module.swift -enable-objc-interop -disable-objc-attr-requires-foundation-module
//
// FIXME: BEGIN -enable-source-import hackaround
// RUN:  %target-swift-frontend(mock-sdk: -sdk %S/../Inputs/clang-importer-sdk -I %t) -emit-module -o %t  %S/../Inputs/clang-importer-sdk/swift-modules/ObjectiveC.swift -enable-objc-interop -disable-objc-attr-requires-foundation-module
// FIXME: END -enable-source-import hackaround
//
// This file should not have any syntax or type checker errors.
// RUN: %target-swift-frontend(mock-sdk: -sdk %S/../Inputs/clang-importer-sdk -I %t) -swift-version 4 -D ERRORS -typecheck -verify %s -F %S/Inputs/mock-sdk -enable-objc-interop -disable-objc-attr-requires-foundation-module
//
// RUN: %target-swift-ide-test(mock-sdk: -sdk %S/../Inputs/clang-importer-sdk -I %t) -swift-version 4 -skip-deinit=false -print-ast-typechecked -source-filename %s -F %S/Inputs/mock-sdk -function-definitions=false -prefer-type-repr=false -print-implicit-attrs=true -enable-objc-interop -disable-objc-attr-requires-foundation-module > %t.printed.txt
// RUN: %FileCheck %s -check-prefix=PASS_COMMON -strict-whitespace < %t.printed.txt
// RUN: %FileCheck %s -check-prefixes=PASS_PRINT_AST,PASS_PRINT_AST_TYPE -strict-whitespace < %t.printed.txt
// RUN: %FileCheck %s -check-prefix=PASS_RW_PROP_GET_SET -strict-whitespace < %t.printed.txt
// RUN: %FileCheck %s -check-prefix=PASS_2200 -strict-whitespace < %t.printed.txt
// RUN: %FileCheck %s -check-prefix=PASS_2500 -strict-whitespace < %t.printed.txt
// RUN: %FileCheck %s -check-prefix=PASS_ONE_LINE -strict-whitespace < %t.printed.txt
// RUN: %FileCheck %s -check-prefix=PASS_ONE_LINE_TYPE -strict-whitespace < %t.printed.txt
// RUN: %FileCheck %s -check-prefix=PREFER_TYPE_PRINTING -strict-whitespace < %t.printed.txt
// RUN: %FileCheck %s -check-prefix=PASS_QUAL_UNQUAL -strict-whitespace < %t.printed.txt
//
// RUN: %target-swift-ide-test(mock-sdk: -sdk %S/../Inputs/clang-importer-sdk -I %t) -swift-version 4 -skip-deinit=false -print-ast-typechecked -source-filename %s -F %S/Inputs/mock-sdk -function-definitions=false -prefer-type-repr=true -print-implicit-attrs=true -enable-objc-interop -disable-objc-attr-requires-foundation-module > %t.printed.txt
// RUN: %FileCheck %s -check-prefix=PASS_COMMON -strict-whitespace < %t.printed.txt
// RUN: %FileCheck %s -check-prefixes=PASS_PRINT_AST,PASS_PRINT_AST_TYPEREPR -strict-whitespace < %t.printed.txt
// RUN: %FileCheck %s -check-prefix=PASS_RW_PROP_GET_SET -strict-whitespace < %t.printed.txt
// RUN: %FileCheck %s -check-prefix=PASS_2200 -strict-whitespace < %t.printed.txt
// RUN: %FileCheck %s -check-prefix=PASS_2500 -strict-whitespace < %t.printed.txt
// RUN: %FileCheck %s -check-prefix=PASS_ONE_LINE -strict-whitespace < %t.printed.txt
// RUN: %FileCheck %s -check-prefix=PASS_ONE_LINE_TYPEREPR -strict-whitespace < %t.printed.txt
// RUN: %FileCheck %s -check-prefix=PREFER_TYPE_REPR_PRINTING -strict-whitespace < %t.printed.txt
// RUN: %FileCheck %s -check-prefix=PASS_QUAL_UNQUAL -strict-whitespace < %t.printed.txt
//
// RUN: %target-swift-frontend(mock-sdk: -sdk %S/../Inputs/clang-importer-sdk -I %t) -swift-version 4 -emit-module -o %t -F %S/Inputs/mock-sdk -enable-objc-interop -disable-objc-attr-requires-foundation-module %s
// RUN: %target-swift-ide-test(mock-sdk: -sdk %S/../Inputs/clang-importer-sdk -I %t) -swift-version 4 -skip-deinit=false -print-module -source-filename %s -F %S/Inputs/mock-sdk -module-to-print=print_ast_tc_decls -print-implicit-attrs=true -enable-objc-interop -disable-objc-attr-requires-foundation-module > %t.printed.txt
// RUN: %FileCheck %s -check-prefix=PASS_COMMON -strict-whitespace < %t.printed.txt
// RUN: %FileCheck %s -check-prefix=PASS_PRINT_MODULE_INTERFACE -strict-whitespace < %t.printed.txt
// RUN: %FileCheck %s -check-prefix=PASS_RW_PROP_NO_GET_SET -strict-whitespace < %t.printed.txt
// RUN: %FileCheck %s -check-prefix=PASS_2200_DESERIALIZED -strict-whitespace < %t.printed.txt
// FIXME: rdar://15167697
// FIXME: %FileCheck %s -check-prefix=PASS_2500 -strict-whitespace < %t.printed.txt
// RUN: %FileCheck %s -check-prefix=PASS_ONE_LINE -strict-whitespace < %t.printed.txt
// RUN: %FileCheck %s -check-prefix=PREFER_TYPE_REPR_PRINTING -strict-whitespace < %t.printed.txt
// RUN: %FileCheck %s -check-prefix=PASS_QUAL_UNQUAL -strict-whitespace < %t.printed.txt
// RUN: %FileCheck %s -check-prefix=PASS_EXPLODE_PATTERN -strict-whitespace < %t.printed.txt
//
// RUN: %target-swift-ide-test(mock-sdk: -sdk %S/../Inputs/clang-importer-sdk -I %t) -skip-deinit=false -print-module -source-filename %s -F %S/Inputs/mock-sdk -I %t -module-to-print=print_ast_tc_decls -synthesize-sugar-on-types=true -print-implicit-attrs=true -enable-objc-interop -disable-objc-attr-requires-foundation-module > %t.printed.txt
// RUN: %FileCheck %s -check-prefix=PASS_PRINT_MODULE_INTERFACE -strict-whitespace < %t.printed.txt
// RUN: %FileCheck %s -check-prefix=PASS_QUAL_UNQUAL -strict-whitespace < %t.printed.txt
// RUN: %FileCheck %s -check-prefix=SYNTHESIZE_SUGAR_ON_TYPES -strict-whitespace < %t.printed.txt
// RUN: %FileCheck %s -check-prefix=PASS_EXPLODE_PATTERN -strict-whitespace < %t.printed.txt

// RUN: %target-swift-ide-test(mock-sdk: -sdk %S/../Inputs/clang-importer-sdk -I %t) -skip-deinit=false -print-module -source-filename %s -F %S/Inputs/mock-sdk -I %t -module-to-print=print_ast_tc_decls -synthesize-sugar-on-types=true -fully-qualified-types-if-ambiguous=true -print-implicit-attrs=true -enable-objc-interop -disable-objc-attr-requires-foundation-module > %t.printed.txt
// RUN: %FileCheck %s -check-prefix=PASS_PRINT_MODULE_INTERFACE -strict-whitespace < %t.printed.txt
// RUN: %FileCheck %s -check-prefix=PASS_QUAL_IF_AMBIGUOUS -strict-whitespace < %t.printed.txt
// RUN: %FileCheck %s -check-prefix=SYNTHESIZE_SUGAR_ON_TYPES -strict-whitespace < %t.printed.txt
// FIXME: %FileCheck %s -check-prefix=PASS_EXPLODE_PATTERN -strict-whitespace < %t.printed.txt

// FIXME: rdar://problem/19648117 Needs splitting objc parts out
// REQUIRES: objc_interop

import Bar
import ObjectiveC
import class Foo.FooClassBase
import struct Foo.FooStruct1
import func Foo.fooFunc1
@_exported import FooHelper
import foo_swift_module

// FIXME: enum tests
//import enum FooClangModule.FooEnum1

// PASS_COMMON: {{^}}import Bar{{$}}
// PASS_COMMON: {{^}}import class Foo.FooClassBase{{$}}
// PASS_COMMON: {{^}}import struct Foo.FooStruct1{{$}}
// PASS_COMMON: {{^}}import func Foo.fooFunc1{{$}}
// PASS_COMMON: {{^}}@_exported import FooHelper{{$}}
// PASS_COMMON: {{^}}import foo_swift_module{{$}}

//===---
//===--- Helper types.
//===---

struct FooStruct {}

class FooClass {}
class BarClass {}

protocol FooProtocol {}
protocol BarProtocol {}
protocol BazProtocol { func baz() }
protocol QuxProtocol {
  associatedtype Qux
}

protocol SubFooProtocol : FooProtocol { }

class FooProtocolImpl : FooProtocol {}
class FooBarProtocolImpl : FooProtocol, BarProtocol {}
class BazProtocolImpl : BazProtocol { func baz() {} }

//===---
//===--- Basic smoketest.
//===---

struct d0100_FooStruct {
// PASS_COMMON-LABEL: {{^}}struct d0100_FooStruct {{{$}}

  var instanceVar1: Int = 0
// PASS_COMMON-NEXT: {{^}}  @_hasInitialValue var instanceVar1: Int{{$}}

  var computedProp1: Int {
    get {
      return 42
    }
  }
// PASS_COMMON-NEXT: {{^}}  var computedProp1: Int { get }{{$}}

  func instanceFunc0() {}
// PASS_COMMON-NEXT: {{^}}  func instanceFunc0(){{$}}

  func instanceFunc1(a: Int) {}
// PASS_COMMON-NEXT: {{^}}  func instanceFunc1(a: Int){{$}}

  func instanceFunc2(a: Int, b: inout Double) {}
// PASS_COMMON-NEXT: {{^}}  func instanceFunc2(a: Int, b: inout Double){{$}}

  func instanceFunc3(a: Int, b: Double) { var a = a; a = 1; _ = a }
// PASS_COMMON-NEXT: {{^}}  func instanceFunc3(a: Int, b: Double){{$}}

  func instanceFuncWithDefaultArg1(a: Int = 0) {}
// PASS_COMMON-NEXT: {{^}}  func instanceFuncWithDefaultArg1(a: Int = 0){{$}}

  func instanceFuncWithDefaultArg2(a: Int = 0, b: Double = 0) {}
// PASS_COMMON-NEXT: {{^}}  func instanceFuncWithDefaultArg2(a: Int = 0, b: Double = 0){{$}}

  func varargInstanceFunc0(v: Int...) {}
// PASS_COMMON-NEXT: {{^}}  func varargInstanceFunc0(v: Int...){{$}}

  func varargInstanceFunc1(a: Float, v: Int...) {}
// PASS_COMMON-NEXT: {{^}}  func varargInstanceFunc1(a: Float, v: Int...){{$}}

  func varargInstanceFunc2(a: Float, b: Double, v: Int...) {}
// PASS_COMMON-NEXT: {{^}}  func varargInstanceFunc2(a: Float, b: Double, v: Int...){{$}}

  func overloadedInstanceFunc1() -> Int { return 0; }
// PASS_COMMON-NEXT: {{^}}  func overloadedInstanceFunc1() -> Int{{$}}

  func overloadedInstanceFunc1() -> Double { return 0.0; }
// PASS_COMMON-NEXT: {{^}}  func overloadedInstanceFunc1() -> Double{{$}}

  func overloadedInstanceFunc2(x: Int) -> Int { return 0; }
// PASS_COMMON-NEXT: {{^}}  func overloadedInstanceFunc2(x: Int) -> Int{{$}}

  func overloadedInstanceFunc2(x: Double) -> Int { return 0; }
// PASS_COMMON-NEXT: {{^}}  func overloadedInstanceFunc2(x: Double) -> Int{{$}}

  func builderFunc1(a: Int) -> d0100_FooStruct { return d0100_FooStruct(); }
// PASS_COMMON-NEXT: {{^}}  func builderFunc1(a: Int) -> d0100_FooStruct{{$}}

  subscript(i: Int) -> Double {
    get {
      return Double(i)
    }
  }
// PASS_COMMON-NEXT: {{^}}  subscript(i: Int) -> Double { get }{{$}}

  subscript(i: Int, j: Int) -> Double {
    get {
      return Double(i + j)
    }
  }
// PASS_COMMON-NEXT: {{^}}  subscript(i: Int, j: Int) -> Double { get }{{$}}
  
  static subscript(i: Int) -> Double {
    get {
      return Double(i)
    }
  }
// PASS_COMMON-NEXT: {{^}}  static subscript(i: Int) -> Double { get }{{$}}

  func bodyNameVoidFunc1(a: Int, b x: Float) {}
// PASS_COMMON-NEXT: {{^}}  func bodyNameVoidFunc1(a: Int, b x: Float){{$}}

  func bodyNameVoidFunc2(a: Int, b x: Float, c y: Double) {}
// PASS_COMMON-NEXT: {{^}}  func bodyNameVoidFunc2(a: Int, b x: Float, c y: Double){{$}}

  func bodyNameStringFunc1(a: Int, b x: Float) -> String { return "" }
// PASS_COMMON-NEXT: {{^}}  func bodyNameStringFunc1(a: Int, b x: Float) -> String{{$}}

  func bodyNameStringFunc2(a: Int, b x: Float, c y: Double) -> String { return "" }
// PASS_COMMON-NEXT: {{^}}  func bodyNameStringFunc2(a: Int, b x: Float, c y: Double) -> String{{$}}

  struct NestedStruct {}
// PASS_COMMON-NEXT: {{^}}  struct NestedStruct {{{$}}
// PASS_COMMON-NEXT: {{^}}    init(){{$}}
// PASS_COMMON-NEXT: {{^}}  }{{$}}

  class NestedClass {}
// PASS_COMMON-NEXT: {{^}}  class NestedClass {{{$}}
// PASS_COMMON-NEXT: {{^}}    @objc deinit{{$}}
// PASS_COMMON-NEXT: {{^}}    init(){{$}}
// PASS_COMMON-NEXT: {{^}}  }{{$}}

  enum NestedEnum {}
// PASS_COMMON-NEXT: {{^}}  enum NestedEnum {{{$}}
// PASS_COMMON-NEXT: {{^}}  }{{$}}

  // Cannot declare a nested protocol.
  // protocol NestedProtocol {}

  typealias NestedTypealias = Int
// PASS_COMMON-NEXT: {{^}}  typealias NestedTypealias = Int{{$}}

  static var staticVar1: Int = 42
// PASS_COMMON-NEXT: {{^}}  @_hasInitialValue static var staticVar1: Int{{$}}

  static var computedStaticProp1: Int {
    get {
      return 42
    }
  }
// PASS_COMMON-NEXT: {{^}}  static var computedStaticProp1: Int { get }{{$}}

  static func staticFunc0() {}
// PASS_COMMON-NEXT: {{^}}  static func staticFunc0(){{$}}

  static func staticFunc1(a: Int) {}
// PASS_COMMON-NEXT: {{^}}  static func staticFunc1(a: Int){{$}}

  static func overloadedStaticFunc1() -> Int { return 0 }
// PASS_COMMON-NEXT: {{^}}  static func overloadedStaticFunc1() -> Int{{$}}

  static func overloadedStaticFunc1() -> Double { return 0.0 }
// PASS_COMMON-NEXT: {{^}}  static func overloadedStaticFunc1() -> Double{{$}}

  static func overloadedStaticFunc2(x: Int) -> Int { return 0 }
// PASS_COMMON-NEXT: {{^}}  static func overloadedStaticFunc2(x: Int) -> Int{{$}}

  static func overloadedStaticFunc2(x: Double) -> Int { return 0 }
// PASS_COMMON-NEXT: {{^}}  static func overloadedStaticFunc2(x: Double) -> Int{{$}}
}
// PASS_COMMON-NEXT: {{^}}  init(){{$}}
// PASS_COMMON-NEXT: {{^}}  init(instanceVar1: Int = 0){{$}}
// PASS_COMMON-NEXT: {{^}}}{{$}}

extension d0100_FooStruct {
// PASS_COMMON-LABEL: {{^}}extension d0100_FooStruct {{{$}}

  var extProp: Int {
    get {
      return 42
    }
  }
// PASS_COMMON-NEXT: {{^}}  var extProp: Int { get }{{$}}

  func extFunc0() {}
// PASS_COMMON-NEXT: {{^}}  func extFunc0(){{$}}

  static var extStaticProp: Int {
    get {
      return 42
    }
  }
// PASS_COMMON-NEXT: {{^}}  static var extStaticProp: Int { get }{{$}}

  static func extStaticFunc0() {}
// PASS_COMMON-NEXT: {{^}}  static func extStaticFunc0(){{$}}

  struct ExtNestedStruct {}
// PASS_COMMON-NEXT: {{^}}  struct ExtNestedStruct {{{$}}
// PASS_COMMON-NEXT: {{^}}    init(){{$}}
// PASS_COMMON-NEXT: {{^}}  }{{$}}

  class ExtNestedClass {}
// PASS_COMMON-NEXT: {{^}}  class ExtNestedClass {{{$}}
// PASS_COMMON-NEXT: {{^}}    @objc deinit{{$}}
// PASS_COMMON-NEXT: {{^}}    init(){{$}}
// PASS_COMMON-NEXT: {{^}}  }{{$}}

  enum ExtNestedEnum {
    case ExtEnumX(Int)
  }
// PASS_COMMON-NEXT: {{^}}  enum ExtNestedEnum {{{$}}
// PASS_COMMON-NEXT: {{^}}    case ExtEnumX(Int){{$}}
// PASS_COMMON-NEXT: {{^}}  }{{$}}

  typealias ExtNestedTypealias = Int
// PASS_COMMON-NEXT: {{^}}  typealias ExtNestedTypealias = Int{{$}}
}
// PASS_COMMON-NEXT: {{^}}}{{$}}

extension d0100_FooStruct.NestedStruct {
// PASS_COMMON-LABEL: {{^}}extension d0100_FooStruct.NestedStruct {{{$}}
  struct ExtNestedStruct2 {}
// PASS_COMMON-NEXT: {{^}}  struct ExtNestedStruct2 {{{$}}
// PASS_COMMON-NEXT: {{^}}    init(){{$}}
// PASS_COMMON-NEXT: {{^}}  }{{$}}
}

extension d0100_FooStruct.ExtNestedStruct {
// PASS_COMMON-LABEL: {{^}}extension d0100_FooStruct.ExtNestedStruct {{{$}}
  struct ExtNestedStruct3 {}
// PASS_COMMON-NEXT: {{^}}  struct ExtNestedStruct3 {{{$}}
// PASS_COMMON-NEXT: {{^}}    init(){{$}}
// PASS_COMMON-NEXT: {{^}}  }{{$}}
}

// PASS_COMMON-NEXT: {{^}}}{{$}}

var fooObject: d0100_FooStruct = d0100_FooStruct()
// PASS_ONE_LINE-DAG: {{^}}@_hasInitialValue var fooObject: d0100_FooStruct{{$}}

struct d0110_ReadWriteProperties {
// PASS_RW_PROP_GET_SET-LABEL:    {{^}}struct d0110_ReadWriteProperties {{{$}}
// PASS_RW_PROP_NO_GET_SET-LABEL: {{^}}struct d0110_ReadWriteProperties {{{$}}

  var computedProp1: Int {
    get {
      return 42
    }
    set {}
  }
// PASS_RW_PROP_GET_SET-NEXT:    {{^}}  var computedProp1: Int { get set }{{$}}
// PASS_RW_PROP_NO_GET_SET-NEXT: {{^}}  var computedProp1: Int{{$}}

  subscript(i: Int) -> Int {
    get {
      return 42
    }
    set {}
  }
// PASS_RW_PROP_GET_SET-NEXT:    {{^}}  subscript(i: Int) -> Int { get set }{{$}}
// PASS_RW_PROP_NO_GET_SET-NEXT: {{^}}  subscript(i: Int) -> Int{{$}}

  static var computedStaticProp1: Int {
    get {
      return 42
    }
    set {}
  }
// PASS_RW_PROP_GET_SET-NEXT:    {{^}}  static var computedStaticProp1: Int { get set }{{$}}
// PASS_RW_PROP_NO_GET_SET-NEXT: {{^}}  static var computedStaticProp1: Int{{$}}

  var computedProp2: Int {
    mutating get {
      return 42
    }
    set {}
  }
// PASS_RW_PROP_GET_SET-NEXT:    {{^}}  var computedProp2: Int { mutating get set }{{$}}
// PASS_RW_PROP_NO_GET_SET-NEXT: {{^}}  var computedProp2: Int { mutating get set }{{$}}

  var computedProp3: Int {
    get {
      return 42
    }
    nonmutating set {}
  }
// PASS_RW_PROP_GET_SET-NEXT:    {{^}}  var computedProp3: Int { get nonmutating set }{{$}}
// PASS_RW_PROP_NO_GET_SET-NEXT: {{^}}  var computedProp3: Int { get nonmutating set }{{$}}

  var computedProp4: Int {
    mutating get {
      return 42
    }
    nonmutating set {}
  }
// PASS_RW_PROP_GET_SET-NEXT:    {{^}}  var computedProp4: Int { mutating get nonmutating set }{{$}}
// PASS_RW_PROP_NO_GET_SET-NEXT: {{^}}  var computedProp4: Int { mutating get nonmutating set }{{$}}

  subscript(i: Float) -> Int {
    get {
      return 42
    }
    nonmutating set {}
  }
// PASS_RW_PROP_GET_SET-NEXT:    {{^}}  subscript(i: Float) -> Int { get nonmutating set }{{$}}
// PASS_RW_PROP_NO_GET_SET-NEXT: {{^}}  subscript(i: Float) -> Int { get nonmutating set }{{$}}
}
// PASS_RW_PROP_GET_SET-NEXT:    {{^}}  init(){{$}}
// PASS_RW_PROP_GET_SET-NEXT:    {{^}}}{{$}}
// PASS_RW_PROP_NO_GET_SET-NEXT: {{^}}  init(){{$}}
// PASS_RW_PROP_NO_GET_SET-NEXT: {{^}}}{{$}}

extension d0110_ReadWriteProperties {
// PASS_RW_PROP_GET_SET-LABEL:    {{^}}extension d0110_ReadWriteProperties {{{$}}
// PASS_RW_PROP_NO_GET_SET-LABEL: {{^}}extension d0110_ReadWriteProperties {{{$}}

  var extProp: Int {
    get {
      return 42
    }
    set(v) {}
  }
// PASS_RW_PROP_GET_SET-NEXT:    {{^}}  var extProp: Int { get set }{{$}}
// PASS_RW_PROP_NO_GET_SET-NEXT: {{^}}  var extProp: Int{{$}}

  static var extStaticProp: Int {
    get {
      return 42
    }
    set(v) {}
  }
// PASS_RW_PROP_GET_SET-NEXT:    {{^}}  static var extStaticProp: Int { get set }{{$}}
// PASS_RW_PROP_NO_GET_SET-NEXT: {{^}}  static var extStaticProp: Int{{$}}
}
// PASS_RW_PROP_GET_SET-NEXT:    {{^}}}{{$}}
// PASS_RW_PROP_NO_GET_SET-NEXT: {{^}}}{{$}}

class d0120_TestClassBase {
// PASS_COMMON-LABEL: {{^}}class d0120_TestClassBase {{{$}}

  required init() {}
// PASS_COMMON-NEXT: {{^}}  required init(){{$}}

  init?(fail: String) {}
// PASS_COMMON-NEXT: {{^}}  init?(fail: String){{$}}

  init!(iuoFail: String) {}
// PASS_COMMON-NEXT: {{^}}  init!(iuoFail: String){{$}}

  final func baseFunc1() {}
// PASS_COMMON-NEXT: {{^}}  final func baseFunc1(){{$}}

  func baseFunc2() {}
// PASS_COMMON-NEXT: {{^}}  func baseFunc2(){{$}}

  subscript(i: Int) -> Int {
    return 0
  }
// PASS_COMMON-NEXT: {{^}}  subscript(i: Int) -> Int { get }{{$}}

  class var baseClassVar1: Int { return 0 }
// PASS_COMMON-NEXT: {{^}}  class var baseClassVar1: Int { get }{{$}}

// FIXME: final class var not allowed to have storage, but static is?
#if ERRORS
  final class var baseClassVar2: Int = 0
  // expected-error@-1 {{class stored properties not supported in classes; did you mean 'static'?}}
#endif

  final class var baseClassVar3: Int { return 0 }
// PASS_COMMON: {{^}}  final class var baseClassVar3: Int { get }{{$}}
  static var baseClassVar4: Int = 0
// PASS_COMMON-NEXT: {{^}}  @_hasInitialValue static var baseClassVar4: Int{{$}}
  static var baseClassVar5: Int { return 0 }
// PASS_COMMON-NEXT: {{^}}  static var baseClassVar5: Int { get }{{$}}

  class func baseClassFunc1() {}
// PASS_COMMON-NEXT: {{^}}  class func baseClassFunc1(){{$}}
  final class func baseClassFunc2() {}
// PASS_COMMON-NEXT: {{^}}  final class func baseClassFunc2(){{$}}
  static func baseClassFunc3() {}
// PASS_COMMON-NEXT: {{^}}  static func baseClassFunc3(){{$}}
}

class d0121_TestClassDerived : d0120_TestClassBase {
// PASS_COMMON-LABEL: {{^}}@_inheritsConvenienceInitializers {{()?}}class d0121_TestClassDerived : d0120_TestClassBase {{{$}}

  required init() { super.init() }
// PASS_COMMON-NEXT: {{^}}  required init(){{$}}

  override init?(fail: String) { nil }
// PASS_COMMON-NEXT: {{^}}  override init?(fail: String){{$}}

  override init!(iuoFail: String) { nil }
// PASS_COMMON-NEXT: {{^}}  override init!(iuoFail: String){{$}}

  final override func baseFunc2() {}
// PASS_COMMON-NEXT: {{^}}  {{(override |final )+}}func baseFunc2(){{$}}

  override final subscript(i: Int) -> Int {
    return 0
  }
// PASS_COMMON-NEXT: {{^}}  override final subscript(i: Int) -> Int { get }{{$}}
}

protocol d0130_TestProtocol {
// PASS_COMMON-LABEL: {{^}}protocol d0130_TestProtocol {{{$}}

  associatedtype NestedTypealias
// PASS_COMMON-NEXT: {{^}}  associatedtype NestedTypealias{{$}}

  var property1: Int { get }
// PASS_COMMON-NEXT: {{^}}  var property1: Int { get }{{$}}

  var property2: Int { get set }
// PASS_COMMON-NEXT: {{^}}  var property2: Int { get set }{{$}}

  func protocolFunc1()
// PASS_COMMON-NEXT: {{^}}  func protocolFunc1(){{$}}
}

@objc protocol d0140_TestObjCProtocol {
// PASS_COMMON-LABEL: {{^}}@objc protocol d0140_TestObjCProtocol {{{$}}

  @objc optional var property1: Int { get }
// PASS_COMMON-NEXT: {{^}}  @objc optional var property1: Int { get }{{$}}

  @objc optional func protocolFunc1()
// PASS_COMMON-NEXT: {{^}}  @objc optional func protocolFunc1(){{$}}
}

protocol d0150_TestClassProtocol : class {}
// PASS_COMMON-LABEL: {{^}}protocol d0150_TestClassProtocol : AnyObject {{{$}}

@objc protocol d0151_TestClassProtocol {}
// PASS_COMMON-LABEL: {{^}}@objc protocol d0151_TestClassProtocol {{{$}}


class d0170_TestAvailability {
// PASS_COMMON-LABEL: {{^}}class d0170_TestAvailability {{{$}}

  @available(*, unavailable)
  func f1() {}
// PASS_COMMON-NEXT: {{^}}  @available(*, unavailable){{$}}
// PASS_COMMON-NEXT: {{^}}  func f1(){{$}}

  @available(*, unavailable, message: "aaa \"bbb\" ccc\nddd\0eee")
  func f2() {}
// PASS_COMMON-NEXT: {{^}}  @available(*, unavailable, message: "aaa \"bbb\" ccc\nddd\0eee"){{$}}
// PASS_COMMON-NEXT: {{^}}  func f2(){{$}}

  @available(iOS, unavailable)
  @available(OSX, unavailable)
  func f3() {}
// PASS_COMMON-NEXT: {{^}}  @available(iOS, unavailable){{$}}
// PASS_COMMON-NEXT: {{^}}  @available(macOS, unavailable){{$}}
// PASS_COMMON-NEXT: {{^}}  func f3(){{$}}

  @available(iOS 8.0, OSX 10.10, *)
  func f4() {}
// PASS_COMMON-NEXT: {{^}}  @available(iOS 8.0, macOS 10.10, *){{$}}
// PASS_COMMON-NEXT: {{^}}  func f4(){{$}}

// Convert long-form @available() to short form when possible.
  @available(iOS, introduced: 8.0)
  @available(OSX, introduced: 10.10)
  func f5() {}
// PASS_COMMON-NEXT: {{^}}  @available(iOS 8.0, macOS 10.10, *){{$}}
// PASS_COMMON-NEXT: {{^}}  func f5(){{$}}
}

@objc class d0180_TestIBAttrs {
// PASS_COMMON-LABEL: {{^}}@objc class d0180_TestIBAttrs {{{$}}

  @IBAction func anAction(_: AnyObject) {}
// Tolerate different attribute orders to support both reading from source
// and deserializing from swiftmodule.
// PASS_COMMON-NEXT: {{^}}  @objc
// PASS_COMMON-DAG: @IBAction
// PASS_COMMON-DAG: @MainActor
// PASS_COMMON: func anAction(_: AnyObject){{$}}

  @IBSegueAction func aSegueAction(_ coder: AnyObject, sender: AnyObject, identifier: AnyObject?) -> Any? { fatalError() }
// PASS_COMMON-NEXT: {{^}}  @objc @IBSegueAction func aSegueAction(_ coder: AnyObject, sender: AnyObject, identifier: AnyObject?) -> Any?{{$}}

  @IBDesignable
  class ADesignableClass {}
// PASS_COMMON-NEXT: {{^}}  @IBDesignable class ADesignableClass {{{$}}

}

@objc class d0181_TestIBAttrs {
// PASS_EXPLODE_PATTERN-LABEL: {{^}}@objc class d0181_TestIBAttrs {{{$}}

  @IBOutlet weak var anOutlet: d0181_TestIBAttrs!
// PASS_EXPLODE_PATTERN-NEXT: {{^}}  @objc @IBOutlet @_hasInitialValue weak var anOutlet: @sil_weak d0181_TestIBAttrs!{{$}}

  @IBInspectable var inspectableProp: Int = 0
// PASS_EXPLODE_PATTERN-NEXT: {{^}}  @objc @IBInspectable @_hasInitialValue var inspectableProp: Int{{$}}

  @GKInspectable var inspectableProp2: Int = 0
// PASS_EXPLODE_PATTERN-NEXT: {{^}}  @objc @GKInspectable @_hasInitialValue var inspectableProp2: Int{{$}}
}

struct d0190_LetVarDecls {
// PASS_PRINT_AST-LABEL: {{^}}struct d0190_LetVarDecls {{{$}}
// PASS_PRINT_MODULE_INTERFACE-LABEL: {{^}}struct d0190_LetVarDecls {{{$}}

  let instanceVar1: Int = 0
// PASS_PRINT_AST-NEXT: {{^}}  @_hasInitialValue let instanceVar1: Int{{$}}
// PASS_PRINT_MODULE_INTERFACE-NEXT: {{^}}  @_hasInitialValue let instanceVar1: Int{{$}}

  let instanceVar2 = 0
// PASS_PRINT_AST-NEXT: {{^}}  @_hasInitialValue let instanceVar2: Int{{$}}
// PASS_PRINT_MODULE_INTERFACE-NEXT: {{^}}  @_hasInitialValue let instanceVar2: Int{{$}}

  static let staticVar1: Int = 42
// PASS_PRINT_AST-NEXT: {{^}}  @_hasInitialValue static let staticVar1: Int{{$}}
// PASS_PRINT_MODULE_INTERFACE-NEXT: {{^}}  @_hasInitialValue static let staticVar1: Int{{$}}

  static let staticVar2 = 42
  // FIXME: PRINTED_WITHOUT_TYPE
// PASS_PRINT_AST-NEXT: {{^}}  @_hasInitialValue static let staticVar2: Int{{$}}
// PASS_PRINT_MODULE_INTERFACE-NEXT: {{^}}  @_hasInitialValue static let staticVar2: Int{{$}}
}

struct d0200_EscapedIdentifiers {
// PASS_COMMON-LABEL: {{^}}struct d0200_EscapedIdentifiers {{{$}}

  struct `struct` {}
// PASS_COMMON-NEXT: {{^}}  struct `struct` {{{$}}
// PASS_COMMON-NEXT: {{^}}    init(){{$}}
// PASS_COMMON-NEXT: {{^}}  }{{$}}

  enum `enum` {
    case `case`
  }
// PASS_COMMON-NEXT: {{^}}  enum `enum` {{{$}}
// PASS_COMMON-NEXT: {{^}}    case `case`{{$}}
// PASS_COMMON-NEXT: {{^}}    {{.*}}static func __derived_enum_equals(_ a: d0200_EscapedIdentifiers.`enum`, _ b: d0200_EscapedIdentifiers.`enum`) -> Bool
// PASS_COMMON-NEXT: {{^}}    func hash(into hasher: inout Hasher)
// PASS_COMMON-NEXT: {{^}}    var hashValue: Int { get }{{$}}
// PASS_COMMON-NEXT: {{^}}  }{{$}}

  class `class` {}
// PASS_COMMON-NEXT: {{^}}  class `class` {{{$}}
// PASS_COMMON-NEXT: {{^}}    @objc deinit{{$}}
// PASS_COMMON-NEXT: {{^}}    init(){{$}}
// PASS_COMMON-NEXT: {{^}}  }{{$}}

  typealias `protocol` = `class`
// PASS_ONE_LINE_TYPE-DAG: {{^}}  typealias `protocol` = d0200_EscapedIdentifiers.`class`{{$}}
// PASS_ONE_LINE_TYPEREPR-DAG: {{^}}  typealias `protocol` = `class`{{$}}

  class `extension` : `class` {}
// PASS_ONE_LINE_TYPE-DAG: {{^}}  @_inheritsConvenienceInitializers class `extension` : d0200_EscapedIdentifiers.`class` {{{$}}
// PASS_ONE_LINE_TYPEREPR-DAG: {{^}}  @_inheritsConvenienceInitializers class `extension` : `class` {{{$}}
// PASS_COMMON:      {{^}}    {{(override )?}}init(){{$}}
// PASS_COMMON-NEXT: {{^}}    @objc deinit{{$}}
// PASS_COMMON-NEXT: {{^}}  }{{$}}

  func `func`<`let`: `protocol`, `where`>(
      class: Int, struct: `protocol`, foo: `let`, bar: `where`) where `where` : `protocol` {}
// PASS_COMMON-NEXT: {{^}}  func `func`<`let`, `where`>(class: Int, struct: {{(d0200_EscapedIdentifiers.)?}}`protocol`, foo: `let`, bar: `where`) where `let` : {{(d0200_EscapedIdentifiers.)?}}`class`, `where` : {{(d0200_EscapedIdentifiers.)?}}`class`{{$}}

  var `var`: `struct` = `struct`()
// PASS_COMMON-NEXT: {{^}}  @_hasInitialValue var `var`: {{(d0200_EscapedIdentifiers.)?}}`struct`{{$}}

  var tupleType: (`var`: Int, `let`: `struct`)
// PASS_COMMON-NEXT: {{^}}  var tupleType: (var: Int, let: {{(d0200_EscapedIdentifiers.)?}}`struct`){{$}}

  var accessors1: Int {
    get { return 0 }
    set(`let`) {}
  }
// PASS_COMMON-NEXT: {{^}}  var accessors1: Int{{( { get set })?}}{{$}}

  static func `static`(protocol: Int) {}
// PASS_COMMON-NEXT: {{^}}  static func `static`(protocol: Int){{$}}

// PASS_COMMON-NEXT: {{^}}  init(var: {{(d0200_EscapedIdentifiers.)?}}`struct` = {{(d0200_EscapedIdentifiers.)?}}`struct`(), tupleType: (var: Int, let: {{(d0200_EscapedIdentifiers.)?}}`struct`)){{$}}
// PASS_COMMON-NEXT: {{^}}}{{$}}
}

struct d0210_Qualifications {
// PASS_QUAL_UNQUAL:       {{^}}struct d0210_Qualifications {{{$}}
// PASS_QUAL_IF_AMBIGUOUS: {{^}}struct d0210_Qualifications {{{$}}

  var propFromStdlib1: Int = 0
// PASS_QUAL_UNQUAL-NEXT:       {{^}}  @_hasInitialValue var propFromStdlib1: Int{{$}}
// PASS_QUAL_IF_AMBIGUOUS-NEXT: {{^}}  @_hasInitialValue var propFromStdlib1: Int{{$}}

  var propFromSwift1: FooSwiftStruct = FooSwiftStruct()
// PASS_QUAL_UNQUAL-NEXT:       {{^}}  @_hasInitialValue var propFromSwift1: FooSwiftStruct{{$}}
// PASS_QUAL_IF_AMBIGUOUS-NEXT: {{^}}  @_hasInitialValue var propFromSwift1: foo_swift_module.FooSwiftStruct{{$}}

  var propFromClang1: FooStruct1 = FooStruct1(x: 0, y: 0.0)
// PASS_QUAL_UNQUAL-NEXT:       {{^}}  @_hasInitialValue var propFromClang1: FooStruct1{{$}}
// PASS_QUAL_IF_AMBIGUOUS-NEXT: {{^}}  @_hasInitialValue var propFromClang1: FooStruct1{{$}}


  func instanceFuncFromStdlib1(a: Int) -> Float {
    return 0.0
  }
// PASS_QUAL_UNQUAL-NEXT:       {{^}}  func instanceFuncFromStdlib1(a: Int) -> Float{{$}}
// PASS_QUAL_IF_AMBIGUOUS-NEXT: {{^}}  func instanceFuncFromStdlib1(a: Int) -> Float{{$}}

  func instanceFuncFromStdlib2(a: ObjCBool) {}
// PASS_QUAL_UNQUAL-NEXT:       {{^}}  func instanceFuncFromStdlib2(a: ObjCBool){{$}}
// PASS_QUAL_IF_AMBIGUOUS-NEXT: {{^}}  func instanceFuncFromStdlib2(a: ObjCBool){{$}}

  func instanceFuncFromSwift1(a: FooSwiftStruct) -> FooSwiftStruct {
    return FooSwiftStruct()
  }
// PASS_QUAL_UNQUAL-NEXT:       {{^}}  func instanceFuncFromSwift1(a: FooSwiftStruct) -> FooSwiftStruct{{$}}
// PASS_QUAL_IF_AMBIGUOUS-NEXT: {{^}}  func instanceFuncFromSwift1(a: foo_swift_module.FooSwiftStruct) -> foo_swift_module.FooSwiftStruct{{$}}

  func instanceFuncFromClang1(a: FooStruct1) -> FooStruct1 {
    return FooStruct1(x: 0, y: 0.0)
  }
// PASS_QUAL_UNQUAL-NEXT:       {{^}}  func instanceFuncFromClang1(a: FooStruct1) -> FooStruct1{{$}}
// PASS_QUAL_IF_AMBIGUOUS-NEXT: {{^}}  func instanceFuncFromClang1(a: FooStruct1) -> FooStruct1{{$}}
}

// FIXME: this should be printed reasonably in case we use
// -prefer-type-repr=true.  Either we should print the types we inferred, or we
// should print the initializers.
class d0250_ExplodePattern {
// PASS_EXPLODE_PATTERN-LABEL: {{^}}class d0250_ExplodePattern {{{$}}
  var instanceVar1 = 0
  var instanceVar2 = 0.0
  var instanceVar3 = ""
// PASS_EXPLODE_PATTERN: {{^}}  @_hasInitialValue var instanceVar1: Int{{$}}
// PASS_EXPLODE_PATTERN: {{^}}  @_hasInitialValue var instanceVar2: Double{{$}}
// PASS_EXPLODE_PATTERN: {{^}}  @_hasInitialValue var instanceVar3: String{{$}}

  var instanceVar4 = FooStruct()
  var (instanceVar5, instanceVar6) = (FooStruct(), FooStruct())
  var (instanceVar7, instanceVar8) = (FooStruct(), FooStruct())
  var (instanceVar9, instanceVar10) : (FooStruct, FooStruct) = (FooStruct(), FooStruct())
  final var (instanceVar11, instanceVar12) = (FooStruct(), FooStruct())
// PASS_EXPLODE_PATTERN: {{^}}  @_hasInitialValue var instanceVar4: FooStruct{{$}}
// PASS_EXPLODE_PATTERN: {{^}}  @_hasInitialValue var instanceVar5: FooStruct{{$}}
// PASS_EXPLODE_PATTERN: {{^}}  @_hasInitialValue var instanceVar6: FooStruct{{$}}
// PASS_EXPLODE_PATTERN: {{^}}  @_hasInitialValue var instanceVar7: FooStruct{{$}}
// PASS_EXPLODE_PATTERN: {{^}}  @_hasInitialValue var instanceVar8: FooStruct{{$}}
// PASS_EXPLODE_PATTERN: {{^}}  @_hasInitialValue var instanceVar9: FooStruct{{$}}
// PASS_EXPLODE_PATTERN: {{^}}  @_hasInitialValue var instanceVar10: FooStruct{{$}}
// PASS_EXPLODE_PATTERN: {{^}}  @_hasInitialValue final var instanceVar11: FooStruct{{$}}
// PASS_EXPLODE_PATTERN: {{^}}  @_hasInitialValue final var instanceVar12: FooStruct{{$}}

  let instanceLet1 = 0
  let instanceLet2 = 0.0
  let instanceLet3 = ""
// PASS_EXPLODE_PATTERN: {{^}}  @_hasInitialValue final let instanceLet1: Int{{$}}
// PASS_EXPLODE_PATTERN: {{^}}  @_hasInitialValue final let instanceLet2: Double{{$}}
// PASS_EXPLODE_PATTERN: {{^}}  @_hasInitialValue final let instanceLet3: String{{$}}

  let instanceLet4 = FooStruct()
  let (instanceLet5, instanceLet6) = (FooStruct(), FooStruct())
  let (instanceLet7, instanceLet8) = (FooStruct(), FooStruct())
  let (instanceLet9, instanceLet10) : (FooStruct, FooStruct) = (FooStruct(), FooStruct())
// PASS_EXPLODE_PATTERN: {{^}}  @_hasInitialValue final let instanceLet4: FooStruct{{$}}
// PASS_EXPLODE_PATTERN: {{^}}  @_hasInitialValue final let instanceLet5: FooStruct{{$}}
// PASS_EXPLODE_PATTERN: {{^}}  @_hasInitialValue final let instanceLet6: FooStruct{{$}}
// PASS_EXPLODE_PATTERN: {{^}}  @_hasInitialValue final let instanceLet7: FooStruct{{$}}
// PASS_EXPLODE_PATTERN: {{^}}  @_hasInitialValue final let instanceLet8: FooStruct{{$}}
// PASS_EXPLODE_PATTERN: {{^}}  @_hasInitialValue final let instanceLet9: FooStruct{{$}}
// PASS_EXPLODE_PATTERN: {{^}}  @_hasInitialValue final let instanceLet10: FooStruct{{$}}
}

class d0260_ExplodePattern_TestClassBase {
// PASS_EXPLODE_PATTERN-LABEL: {{^}}class d0260_ExplodePattern_TestClassBase {{{$}}

  init() {
    baseProp1 = 0
  }
// PASS_EXPLODE_PATTERN-NEXT: {{^}}  init(){{$}}

  final var baseProp1: Int
// PASS_EXPLODE_PATTERN-NEXT: {{^}}  final var baseProp1: Int{{$}}

  var baseProp2: Int {
    get {
      return 0
    }
    set {}
  }
// PASS_EXPLODE_PATTERN-NEXT: {{^}}  var baseProp2: Int{{$}}
}

class d0261_ExplodePattern_TestClassDerived : d0260_ExplodePattern_TestClassBase {
// PASS_EXPLODE_PATTERN-LABEL: {{^}}@_inheritsConvenienceInitializers class d0261_ExplodePattern_TestClassDerived : d0260_ExplodePattern_TestClassBase {{{$}}

  override final var baseProp2: Int {
    get {
      return 0
    }
    set {}
  }
// PASS_EXPLODE_PATTERN-NEXT: {{^}}  override final var baseProp2: Int{{$}}
}

//===---
//===--- Inheritance list in structs.
//===---

struct StructWithoutInheritance1 {}
// PASS_ONE_LINE-DAG: {{^}}struct StructWithoutInheritance1 {{{$}}

struct StructWithInheritance1 : FooProtocol {}
// PASS_ONE_LINE-DAG: {{^}}struct StructWithInheritance1 : FooProtocol {{{$}}

struct StructWithInheritance2 : FooProtocol, BarProtocol {}
// PASS_ONE_LINE-DAG: {{^}}struct StructWithInheritance2 : FooProtocol, BarProtocol {{{$}}

struct StructWithInheritance3 : QuxProtocol, SubFooProtocol {
  typealias Qux = Int
}
// PASS_ONE_LINE-DAG: {{^}}struct StructWithInheritance3 : QuxProtocol, SubFooProtocol {{{$}}

//===---
//===--- Inheritance list in classes.
//===---

class ClassWithoutInheritance1 {}
// PASS_ONE_LINE-DAG: {{^}}class ClassWithoutInheritance1 {{{$}}

class ClassWithInheritance1 : FooProtocol {}
// PASS_ONE_LINE-DAG: {{^}}class ClassWithInheritance1 : FooProtocol {{{$}}

class ClassWithInheritance2 : FooProtocol, BarProtocol {}
// PASS_ONE_LINE-DAG: {{^}}class ClassWithInheritance2 : FooProtocol, BarProtocol {{{$}}

class ClassWithInheritance3 : FooClass {}
// PASS_ONE_LINE-DAG: {{^}}@_inheritsConvenienceInitializers class ClassWithInheritance3 : FooClass {{{$}}

class ClassWithInheritance4 : FooClass, FooProtocol {}
// PASS_ONE_LINE-DAG: {{^}}@_inheritsConvenienceInitializers class ClassWithInheritance4 : FooClass, FooProtocol {{{$}}

class ClassWithInheritance5 : FooClass, FooProtocol, BarProtocol {}
// PASS_ONE_LINE-DAG: {{^}}@_inheritsConvenienceInitializers class ClassWithInheritance5 : FooClass, FooProtocol, BarProtocol {{{$}}

class ClassWithInheritance6 : QuxProtocol, SubFooProtocol {
  typealias Qux = Int
}
// PASS_ONE_LINE-DAG: {{^}}class ClassWithInheritance6 : QuxProtocol, SubFooProtocol {{{$}}

//===---
//===--- Inheritance list in enums.
//===---

enum EnumWithoutInheritance1 {}
// PASS_ONE_LINE-DAG: {{^}}enum EnumWithoutInheritance1 {{{$}}

enum EnumWithInheritance1 : FooProtocol {}
// PASS_ONE_LINE-DAG: {{^}}enum EnumWithInheritance1 : FooProtocol {{{$}}

enum EnumWithInheritance2 : FooProtocol, BarProtocol {}
// PASS_ONE_LINE-DAG: {{^}}enum EnumWithInheritance2 : FooProtocol, BarProtocol {{{$}}

enum EnumDeclWithUnderlyingType1 : Int { case X }
// PASS_ONE_LINE-DAG: {{^}}enum EnumDeclWithUnderlyingType1 : Int {{{$}}

enum EnumDeclWithUnderlyingType2 : Int, FooProtocol { case X }
// PASS_ONE_LINE-DAG: {{^}}enum EnumDeclWithUnderlyingType2 : Int, FooProtocol {{{$}}

enum EnumWithInheritance3 : QuxProtocol, SubFooProtocol {
  typealias Qux = Int
}
// PASS_ONE_LINE-DAG: {{^}}enum EnumWithInheritance3 : QuxProtocol, SubFooProtocol {{{$}}

//===---
//===--- Inheritance list in protocols.
//===---

protocol ProtocolWithoutInheritance1 {}
// PASS_ONE_LINE-DAG: {{^}}protocol ProtocolWithoutInheritance1 {{{$}}

protocol ProtocolWithInheritance1 : FooProtocol {}
// PASS_ONE_LINE-DAG: {{^}}protocol ProtocolWithInheritance1 : FooProtocol {{{$}}

protocol ProtocolWithInheritance2 : FooProtocol, BarProtocol { }
// PASS_ONE_LINE-DAG: {{^}}protocol ProtocolWithInheritance2 : BarProtocol, FooProtocol {{{$}}

protocol ProtocolWithInheritance3 : QuxProtocol, SubFooProtocol {
}
// PASS_ONE_LINE-DAG: {{^}}protocol ProtocolWithInheritance3 : QuxProtocol, SubFooProtocol {{{$}}

//===---
//===--- Inheritance list in extensions
//===---

struct StructInherited { }

// PASS_ONE_LINE-DAG: {{.*}}extension StructInherited : QuxProtocol, SubFooProtocol {{{$}}
extension StructInherited : QuxProtocol, SubFooProtocol {
  typealias Qux = Int
}

//===---
//===--- Typealias printing.
//===---

// Normal typealiases.

typealias SimpleTypealias1 = FooProtocol
// PASS_ONE_LINE-DAG: {{^}}typealias SimpleTypealias1 = FooProtocol{{$}}

// Associated types.

protocol AssociatedType1 {
  associatedtype AssociatedTypeDecl1 = Int
// PASS_ONE_LINE-DAG: {{^}}  associatedtype AssociatedTypeDecl1 = Int{{$}}

  associatedtype AssociatedTypeDecl2 : FooProtocol
// PASS_ONE_LINE-DAG: {{^}}  associatedtype AssociatedTypeDecl2 : FooProtocol{{$}}

  associatedtype AssociatedTypeDecl3 : FooProtocol, BarProtocol
// PASS_ONE_LINE_TYPEREPR-DAG: {{^}}  associatedtype AssociatedTypeDecl3 : BarProtocol, FooProtocol{{$}}

  associatedtype AssociatedTypeDecl4 where AssociatedTypeDecl4 : QuxProtocol, AssociatedTypeDecl4.Qux == Int
// PASS_ONE_LINE_TYPEREPR-DAG: {{^}}  associatedtype AssociatedTypeDecl4 : QuxProtocol where Self.AssociatedTypeDecl4.Qux == Int{{$}}

  associatedtype AssociatedTypeDecl5: FooClass
// PASS_ONE_LINE_TYPEREPR-DAG: {{^}}  associatedtype AssociatedTypeDecl5 : FooClass{{$}}
}

//===---
//===--- Variable declaration printing.
//===---

var d0300_topLevelVar1: Int = 42
// PASS_COMMON: {{^}}@_hasInitialValue var d0300_topLevelVar1: Int{{$}}
// PASS_COMMON-NOT: d0300_topLevelVar1

var d0400_topLevelVar2: Int = 42
// PASS_COMMON: {{^}}@_hasInitialValue var d0400_topLevelVar2: Int{{$}}
// PASS_COMMON-NOT: d0400_topLevelVar2

var d0500_topLevelVar2: Int {
  get {
    return 42
  }
}
// PASS_COMMON: {{^}}var d0500_topLevelVar2: Int { get }{{$}}
// PASS_COMMON-NOT: d0500_topLevelVar2

class d0600_InClassVar1 {
// PASS_O600-LABEL: d0600_InClassVar1

  var instanceVar1: Int
// PASS_COMMON: {{^}}  var instanceVar1: Int{{$}}
// PASS_COMMON-NOT: instanceVar1

  var instanceVar2: Int = 42
// PASS_COMMON: {{^}}  @_hasInitialValue var instanceVar2: Int{{$}}
// PASS_COMMON-NOT: instanceVar2

  // FIXME: this is sometimes printed without a type, see PASS_EXPLODE_PATTERN.
  // FIXME: PRINTED_WITHOUT_TYPE
  var instanceVar3 = 42
// PASS_COMMON: {{^}}  @_hasInitialValue var instanceVar3
// PASS_COMMON-NOT: instanceVar3

  var instanceVar4: Int {
    get {
      return 42
    }
  }
// PASS_COMMON: {{^}}  var instanceVar4: Int { get }{{$}}
// PASS_COMMON-NOT: instanceVar4

  static var staticVar1: Int = 42
// PASS_COMMON: {{^}}  @_hasInitialValue static var staticVar1: Int{{$}}
// PASS_COMMON-NOT: staticVar1

  static var staticVar2: Int { 42 }
// PASS_COMMON: {{^}}  static var staticVar2: Int { get }{{$}}
// PASS_COMMON-NOT: staticVar2

  init() {
    instanceVar1 = 10
  }
}

//===---
//===--- Subscript declaration printing.
//===---

class d0700_InClassSubscript1 {
// PASS_COMMON-LABEL: d0700_InClassSubscript1
  subscript(i: Int) -> Int {
    get {
      return 42
    }
  }
  subscript(index i: Float) -> Int { return 42 }
  class `class` {}
  subscript(x: Float) -> `class` { return `class`() }
// PASS_COMMON: {{^}}  subscript(i: Int) -> Int { get }{{$}}
// PASS_COMMON: {{^}}  subscript(index i: Float) -> Int { get }{{$}}
// PASS_COMMON: {{^}}  subscript(x: Float) -> {{.*}} { get }{{$}}
// PASS_COMMON-NOT: subscript

// PASS_ONE_LINE_TYPE: {{^}}  subscript(x: Float) -> d0700_InClassSubscript1.`class` { get }{{$}}
// PASS_ONE_LINE_TYPEREPR: {{^}}  subscript(x: Float) -> `class` { get }{{$}}
}
// PASS_COMMON: {{^}}}{{$}}


//===---
//===--- Constructor declaration printing.
//===---

struct d0800_ExplicitConstructors1 {
// PASS_COMMON-LABEL: d0800_ExplicitConstructors1

  init() {}
// PASS_COMMON: {{^}}  init(){{$}}

  init(a: Int) {}
// PASS_COMMON: {{^}}  init(a: Int){{$}}
}

struct d0900_ExplicitConstructorsSelector1 {
// PASS_COMMON-LABEL: d0900_ExplicitConstructorsSelector1

  init(int a: Int) {}
// PASS_COMMON: {{^}}  init(int a: Int){{$}}

  init(int a: Int, andFloat b: Float) {}
// PASS_COMMON: {{^}}  init(int a: Int, andFloat b: Float){{$}}
}

struct d1000_ExplicitConstructorsSelector2 {
// PASS_COMMON-LABEL: d1000_ExplicitConstructorsSelector2

  init(noArgs _: ()) {}
// PASS_COMMON: {{^}}  init(noArgs _: ()){{$}}

  init(_ a: Int) {}
// PASS_COMMON: {{^}}  init(_ a: Int){{$}}

  init(_ a: Int, withFloat b: Float) {}
// PASS_COMMON: {{^}}  init(_ a: Int, withFloat b: Float){{$}}

  init(int a: Int, _ b: Float) {}
// PASS_COMMON: {{^}}  init(int a: Int, _ b: Float){{$}}
}

//===---
//===--- Destructor declaration printing.
//===---

class d1100_ExplicitDestructor1 {
// PASS_COMMON-LABEL: d1100_ExplicitDestructor1

  deinit {}
// PASS_COMMON: {{^}}  @objc deinit{{$}}
}

//===---
//===--- Enum declaration printing.
//===---

enum d2000_EnumDecl1 {
  case ED1_First
  case ED1_Second
}
// PASS_COMMON: {{^}}enum d2000_EnumDecl1 {{{$}}
// PASS_COMMON-NEXT: {{^}}  case ED1_First{{$}}
// PASS_COMMON-NEXT: {{^}}  case ED1_Second{{$}}
// PASS_COMMON-NEXT: {{^}}  {{.*}}static func __derived_enum_equals(_ a: d2000_EnumDecl1, _ b: d2000_EnumDecl1) -> Bool
// PASS_COMMON-NEXT: {{^}}  func hash(into hasher: inout Hasher)
// PASS_COMMON-NEXT: {{^}}  var hashValue: Int { get }{{$}}
// PASS_COMMON-NEXT: {{^}}}{{$}}

enum d2100_EnumDecl2 {
  case ED2_A(Int)
  case ED2_B(Float)
  case ED2_C(Int, Float)
  case ED2_D(x: Int, y: Float)
  case ED2_E(x: Int, y: (Float, Double))
  case ED2_F(x: Int, (y: Float, z: Double))
}
// PASS_COMMON: {{^}}enum d2100_EnumDecl2 {{{$}}
// PASS_COMMON-NEXT: {{^}}  case ED2_A(Int){{$}}
// PASS_COMMON-NEXT: {{^}}  case ED2_B(Float){{$}}
// PASS_COMMON-NEXT: {{^}}  case ED2_C(Int, Float){{$}}
// PASS_COMMON-NEXT: {{^}}  case ED2_D(x: Int, y: Float){{$}}
// PASS_COMMON-NEXT: {{^}}  case ED2_E(x: Int, y: (Float, Double)){{$}}
// PASS_COMMON-NEXT: {{^}}  case ED2_F(x: Int, (y: Float, z: Double)){{$}}
// PASS_COMMON-NEXT: {{^}}}{{$}}

enum d2200_EnumDecl3 {
  case ED3_A, ED3_B
  case ED3_C(Int), ED3_D
  case ED3_E, ED3_F(Int)
  case ED3_G(Int), ED3_H(Int)
  case ED3_I(Int), ED3_J(Int), ED3_K
}
// PASS_2200: {{^}}enum d2200_EnumDecl3 {{{$}}
// PASS_2200-NEXT: {{^}}  case ED3_A, ED3_B{{$}}
// PASS_2200-NEXT: {{^}}  case ED3_C(Int), ED3_D{{$}}
// PASS_2200-NEXT: {{^}}  case ED3_E, ED3_F(Int){{$}}
// PASS_2200-NEXT: {{^}}  case ED3_G(Int), ED3_H(Int){{$}}
// PASS_2200-NEXT: {{^}}  case ED3_I(Int), ED3_J(Int), ED3_K{{$}}
// PASS_2200-NEXT: {{^}}}{{$}}

// PASS_2200_DESERIALIZED: {{^}}enum d2200_EnumDecl3 {{{$}}
// PASS_2200_DESERIALIZED-NEXT: {{^}}  case ED3_A{{$}}
// PASS_2200_DESERIALIZED-NEXT: {{^}}  case ED3_B{{$}}
// PASS_2200_DESERIALIZED-NEXT: {{^}}  case ED3_C(Int){{$}}
// PASS_2200_DESERIALIZED-NEXT: {{^}}  case ED3_D{{$}}
// PASS_2200_DESERIALIZED-NEXT: {{^}}  case ED3_E{{$}}
// PASS_2200_DESERIALIZED-NEXT: {{^}}  case ED3_F(Int){{$}}
// PASS_2200_DESERIALIZED-NEXT: {{^}}  case ED3_G(Int){{$}}
// PASS_2200_DESERIALIZED-NEXT: {{^}}  case ED3_H(Int){{$}}
// PASS_2200_DESERIALIZED-NEXT: {{^}}  case ED3_I(Int){{$}}
// PASS_2200_DESERIALIZED-NEXT: {{^}}  case ED3_J(Int){{$}}
// PASS_2200_DESERIALIZED-NEXT: {{^}}  case ED3_K{{$}}
// PASS_2200_DESERIALIZED-NEXT: {{^}}}{{$}}

enum d2300_EnumDeclWithValues1 : Int {
  case EDV2_First = 10
  case EDV2_Second
}
// PASS_COMMON: {{^}}enum d2300_EnumDeclWithValues1 : Int {{{$}}
// PASS_COMMON-NEXT: {{^}}  case EDV2_First{{$}}
// PASS_COMMON-NEXT: {{^}}  case EDV2_Second{{$}}
// PASS_COMMON-DAG: {{^}}  typealias RawValue = Int
// PASS_COMMON-DAG: {{^}}  init?(rawValue: Int){{$}}
// PASS_COMMON-DAG: {{^}}  var rawValue: Int { get }{{$}}
// PASS_COMMON: {{^}}}{{$}}

enum d2400_EnumDeclWithValues2 : Double {
  case EDV3_First = 10
  case EDV3_Second
}
// PASS_COMMON: {{^}}enum d2400_EnumDeclWithValues2 : Double {{{$}}
// PASS_COMMON-NEXT: {{^}}  case EDV3_First{{$}}
// PASS_COMMON-NEXT: {{^}}  case EDV3_Second{{$}}
// PASS_COMMON-DAG: {{^}}  typealias RawValue = Double
// PASS_COMMON-DAG: {{^}}  init?(rawValue: Double){{$}}
// PASS_COMMON-DAG: {{^}}  var rawValue: Double { get }{{$}}
// PASS_COMMON: {{^}}}{{$}}

//===---
//===--- Custom operator printing.
//===---

postfix operator <*>

// PASS_2500-LABEL: {{^}}postfix operator <*>{{$}}

protocol d2600_ProtocolWithOperator1 {
  static postfix func <*>(_: Self)
}
// PASS_2500: {{^}}protocol d2600_ProtocolWithOperator1 {{{$}}
// PASS_2500-NEXT: {{^}}  postfix static func <*> (_: Self){{$}}
// PASS_2500-NEXT: {{^}}}{{$}}

struct d2601_TestAssignment {}
infix operator %%%
func %%%(lhs: inout d2601_TestAssignment, rhs: d2601_TestAssignment) -> Int {
  return 0
}
// PASS_2500-LABEL: {{^}}infix operator %%% : DefaultPrecedence{{$}}
// PASS_2500: {{^}}func %%% (lhs: inout d2601_TestAssignment, rhs: d2601_TestAssignment) -> Int{{$}}

precedencegroup BoringPrecedence {
// PASS_2500-LABEL: {{^}}precedencegroup BoringPrecedence {{{$}}
  associativity: left
// PASS_2500-NEXT: {{^}}  associativity: left{{$}}
  higherThan: AssignmentPrecedence
// PASS_2500-NEXT: {{^}}  higherThan: AssignmentPrecedence{{$}}
// PASS_2500-NOT:         assignment
// PASS_2500-NOT:         lowerThan
}

precedencegroup ReallyBoringPrecedence {
// PASS_2500-LABEL: {{^}}precedencegroup ReallyBoringPrecedence {{{$}}
  associativity: right
// PASS_2500-NEXT: {{^}}  associativity: right{{$}}
// PASS_2500-NOT: higherThan
// PASS_2500-NOT: lowerThan
// PASS_2500-NOT: assignment
}

precedencegroup BoringAssignmentPrecedence {
// PASS_2500-LABEL: {{^}}precedencegroup BoringAssignmentPrecedence {{{$}}
  lowerThan: AssignmentPrecedence
  assignment: true
// PASS_2500-NEXT: {{^}}  assignment: true{{$}}
// PASS_2500-NEXT: {{^}}  lowerThan: AssignmentPrecedence{{$}}
// PASS_2500-NOT: associativity
// PASS_2500-NOT: higherThan
}
// PASS_2500: {{^}}}{{$}}

//===---
//===--- Printing of deduced associated types.
//===---

protocol d2700_ProtocolWithAssociatedType1 {
  associatedtype TA1
  func returnsTA1() -> TA1
}

// PREFER_TYPE_PRINTING: {{^}}protocol d2700_ProtocolWithAssociatedType1 {{{$}}
// PREFER_TYPE_PRINTING-NEXT: {{^}}  associatedtype TA1{{$}}
// PREFER_TYPE_PRINTING-NEXT: {{^}}  func returnsTA1() -> Self.TA1{{$}}
// PREFER_TYPE_PRINTING-NEXT: {{^}}}{{$}}

// PREFER_TYPEREPR_PRINTING: {{^}}protocol d2700_ProtocolWithAssociatedType1 {{{$}}
// PREFER_TYPEREPR_PRINTING-NEXT: {{^}}  associatedtype TA1{{$}}
// PREFER_TYPEREPR_PRINTING-NEXT: {{^}}  func returnsTA1() -> TA1{{$}}
// PREFER_TYPEREPR_PRINTING-NEXT: {{^}}}{{$}}

struct d2800_ProtocolWithAssociatedType1Impl : d2700_ProtocolWithAssociatedType1 {
  func returnsTA1() -> Int {
    return 42
  }
}

// PASS_COMMON: {{^}}struct d2800_ProtocolWithAssociatedType1Impl : d2700_ProtocolWithAssociatedType1 {{{$}}
// PASS_COMMON-NEXT: {{^}}  func returnsTA1() -> Int{{$}}
// PASS_COMMON-NEXT: {{^}}  typealias TA1 = Int
// PASS_COMMON-NEXT: {{^}}  init(){{$}}
// PASS_COMMON-NEXT: {{^}}}{{$}}

//===---
//===--- Generic parameter list printing.
//===---

struct GenericParams1<
    StructGenericFoo : FooProtocol,
    StructGenericFooX : FooClass,
    StructGenericBar : FooProtocol & BarProtocol,
    StructGenericBaz> {
// PASS_ONE_LINE_TYPE-DAG: {{^}}struct GenericParams1<StructGenericFoo, StructGenericFooX, StructGenericBar, StructGenericBaz> where StructGenericFoo : FooProtocol, StructGenericFooX : FooClass, StructGenericBar : BarProtocol, StructGenericBar : FooProtocol {{{$}}
// PASS_ONE_LINE_TYPEREPR-DAG: {{^}}struct GenericParams1<StructGenericFoo, StructGenericFooX, StructGenericBar, StructGenericBaz> where StructGenericFoo : FooProtocol, StructGenericFooX : FooClass, StructGenericBar : BarProtocol, StructGenericBar : FooProtocol {{{$}}
  init<
      GenericFoo : FooProtocol,
      GenericFooX : FooClass,
      GenericBar : FooProtocol & BarProtocol,
      GenericBaz>(a: StructGenericFoo, b: StructGenericBar, c: StructGenericBaz,
                  d: GenericFoo, e: GenericFooX, f: GenericBar, g: GenericBaz)
  {}
// PASS_ONE_LINE_TYPE-DAG: {{^}}  init<GenericFoo, GenericFooX, GenericBar, GenericBaz>(a: StructGenericFoo, b: StructGenericBar, c: StructGenericBaz, d: GenericFoo, e: GenericFooX, f: GenericBar, g: GenericBaz) where GenericFoo : FooProtocol, GenericFooX : FooClass, GenericBar : BarProtocol, GenericBar : FooProtocol{{$}}
// FIXME: in protocol compositions protocols are listed in reverse order.
//
// PASS_ONE_LINE_TYPEREPR-DAG: {{^}}  init<GenericFoo, GenericFooX, GenericBar, GenericBaz>(a: StructGenericFoo, b: StructGenericBar, c: StructGenericBaz, d: GenericFoo, e: GenericFooX, f: GenericBar, g: GenericBaz) where GenericFoo : FooProtocol, GenericFooX : FooClass, GenericBar : BarProtocol, GenericBar : FooProtocol{{$}}

  func genericParams1<
      GenericFoo : FooProtocol,
      GenericFooX : FooClass,
      GenericBar : FooProtocol & BarProtocol,
      GenericBaz>(a: StructGenericFoo, b: StructGenericBar, c: StructGenericBaz,
                  d: GenericFoo, e: GenericFooX, f: GenericBar, g: GenericBaz)
  {}
// PASS_ONE_LINE_TYPE-DAG: {{^}}  func genericParams1<GenericFoo, GenericFooX, GenericBar, GenericBaz>(a: StructGenericFoo, b: StructGenericBar, c: StructGenericBaz, d: GenericFoo, e: GenericFooX, f: GenericBar, g: GenericBaz) where GenericFoo : FooProtocol, GenericFooX : FooClass, GenericBar : BarProtocol, GenericBar : FooProtocol{{$}}
// FIXME: in protocol compositions protocols are listed in reverse order.
//
// PASS_ONE_LINE_TYPEREPR-DAG: {{^}}  func genericParams1<GenericFoo, GenericFooX, GenericBar, GenericBaz>(a: StructGenericFoo, b: StructGenericBar, c: StructGenericBaz, d: GenericFoo, e: GenericFooX, f: GenericBar, g: GenericBaz) where GenericFoo : FooProtocol, GenericFooX : FooClass, GenericBar : BarProtocol, GenericBar : FooProtocol{{$}}

  func contextualWhereClause1() where StructGenericBaz == Never {}
  // PASS_PRINT_AST: func contextualWhereClause1() where StructGenericBaz == Never{{$}}

  subscript(index: Int) -> Never where StructGenericBaz: FooProtocol {
    return fatalError()
  }
  // PASS_PRINT_AST: subscript(index: Int) -> Never where StructGenericBaz : FooProtocol { get }{{$}}
}
extension GenericParams1 where StructGenericBaz: FooProtocol {
  static func contextualWhereClause2() where StructGenericBaz: FooClass {}
  // PASS_PRINT_AST: static func contextualWhereClause2() where StructGenericBaz : FooClass{{$}}

  typealias ContextualWhereClause3 = Never where StructGenericBaz: QuxProtocol, StructGenericBaz.Qux == Void
  // PASS_PRINT_AST: typealias ContextualWhereClause3 = Never where StructGenericBaz : QuxProtocol, StructGenericBaz.Qux == (){{$}}
}

struct GenericParams2<T : FooProtocol> where T : BarProtocol {}
// PASS_ONE_LINE-DAG: {{^}}struct GenericParams2<T> where T : BarProtocol, T : FooProtocol {{{$}}

struct GenericParams3<T : FooProtocol> where T : BarProtocol, T : QuxProtocol {}
// PASS_ONE_LINE-DAG: {{^}}struct GenericParams3<T> where T : BarProtocol, T : FooProtocol, T : QuxProtocol {{{$}}

struct GenericParams4<T : QuxProtocol> where T.Qux : FooProtocol {}
// PASS_ONE_LINE-DAG: {{^}}struct GenericParams4<T> where T : QuxProtocol, T.Qux : FooProtocol {{{$}}

struct GenericParams5<T : QuxProtocol> where T.Qux : FooProtocol & BarProtocol {}
// PREFER_TYPE_PRINTING: {{^}}struct GenericParams5<T> where T : QuxProtocol, T.Qux : BarProtocol, T.Qux : FooProtocol {{{$}}
// PREFER_TYPE_REPR_PRINTING: {{^}}struct GenericParams5<T> where T : QuxProtocol, T.Qux : BarProtocol, T.Qux : FooProtocol {{{$}}

struct GenericParams6<T : QuxProtocol, U : QuxProtocol> where T.Qux == U.Qux {}
// PREFER_TYPE_PRINTING: {{^}}struct GenericParams6<T, U> where T : QuxProtocol, U : QuxProtocol, T.Qux == U.Qux {{{$}}
// PREFER_TYPE_REPR_PRINTING: {{^}}struct GenericParams6<T, U> where T : QuxProtocol, U : QuxProtocol, T.Qux == U.Qux {{{$}}

struct GenericParams7<T : QuxProtocol, U : QuxProtocol> where T.Qux : QuxProtocol, U.Qux : QuxProtocol, T.Qux.Qux == U.Qux.Qux {}
// PREFER_TYPE_PRINTING: {{^}}struct GenericParams7<T, U> where T : QuxProtocol, U : QuxProtocol, T.Qux : QuxProtocol, U.Qux : QuxProtocol, T.Qux.Qux == U.Qux.Qux {{{$}}
// PREFER_TYPE_REPR_PRINTING: {{^}}struct GenericParams7<T, U> where T : QuxProtocol, U : QuxProtocol, T.Qux : QuxProtocol, U.Qux : QuxProtocol, T.Qux.Qux == U.Qux.Qux {{{$}}

//===---
//===--- Tupe sugar for library types.
//===---

struct d2900_TypeSugar1 {
// PASS_COMMON-LABEL: {{^}}struct d2900_TypeSugar1 {{{$}}
// SYNTHESIZE_SUGAR_ON_TYPES-LABEL: {{^}}struct d2900_TypeSugar1 {{{$}}

  func f1(x: [Int]) {}
// PASS_COMMON-NEXT: {{^}}  func f1(x: [Int]){{$}}
// SYNTHESIZE_SUGAR_ON_TYPES-NEXT: {{^}}  func f1(x: [Int]){{$}}

  func f2(x: Array<Int>) {}
// PASS_COMMON-NEXT: {{^}}  func f2(x: Array<Int>){{$}}
// SYNTHESIZE_SUGAR_ON_TYPES-NEXT: {{^}}  func f2(x: [Int]){{$}}

  func f3(x: Int?) {}
// PASS_COMMON-NEXT: {{^}}  func f3(x: Int?){{$}}
// SYNTHESIZE_SUGAR_ON_TYPES-NEXT: {{^}}  func f3(x: Int?){{$}}

  func f4(x: Optional<Int>) {}
// PASS_COMMON-NEXT: {{^}}  func f4(x: Optional<Int>){{$}}
// SYNTHESIZE_SUGAR_ON_TYPES-NEXT: {{^}}  func f4(x: Int?){{$}}

  func f5(x: [Int]...) {}
// PASS_COMMON-NEXT: {{^}}  func f5(x: [Int]...){{$}}
// SYNTHESIZE_SUGAR_ON_TYPES-NEXT: {{^}}  func f5(x: [Int]...){{$}}

  func f6(x: Array<Int>...) {}
// PASS_COMMON-NEXT: {{^}}  func f6(x: Array<Int>...){{$}}
// SYNTHESIZE_SUGAR_ON_TYPES-NEXT: {{^}}  func f6(x: [Int]...){{$}}

  func f7(x: [Int : Int]...) {}
// PASS_COMMON-NEXT: {{^}}  func f7(x: [Int : Int]...){{$}}
// SYNTHESIZE_SUGAR_ON_TYPES-NEXT: {{^}}  func f7(x: [Int : Int]...){{$}}

  func f8(x: Dictionary<String, Int>...) {}
// PASS_COMMON-NEXT: {{^}}  func f8(x: Dictionary<String, Int>...){{$}}
// SYNTHESIZE_SUGAR_ON_TYPES-NEXT: {{^}}  func f8(x: [String : Int]...){{$}}
}
// PASS_COMMON-NEXT: {{^}}  init(){{$}}
// PASS_COMMON-NEXT: {{^}}}{{$}}

// @discardableResult attribute
public struct DiscardableThingy {
    // PASS_PRINT_AST: @discardableResult
    // PASS_PRINT_AST-NEXT: public init()
    @discardableResult
    public init() {}

    // PASS_PRINT_AST: @discardableResult
    // PASS_PRINT_AST-NEXT: public func useless() -> Int
    @discardableResult
    public func useless() -> Int { return 0 }
}


// Parameter Attributes.


// <rdar://problem/19775868> Swift 1.2b1: Header gen puts @autoclosure in the wrong place
// PASS_PRINT_AST: public func ParamAttrs1(a: @autoclosure () -> ())
public func ParamAttrs1(a : @autoclosure () -> ()) {
  a()
}

// PASS_PRINT_AST: public func ParamAttrs2(a: @autoclosure @escaping () -> ())
public func ParamAttrs2(a : @autoclosure @escaping () -> ()) {
  a()
}

// PASS_PRINT_AST: public func ParamAttrs3(a: () -> ())
public func ParamAttrs3(a : () -> ()) {
  a()
}

// PASS_PRINT_AST: public func ParamAttrs4(a: @escaping () -> ())
public func ParamAttrs4(a : @escaping () -> ()) {
  a()
}

// PASS_PRINT_AST: public func ParamAttrs5(a: (@escaping () -> ()) -> ())
public func ParamAttrs5(a : (@escaping () -> ()) -> ()) {
}

// PASS_PRINT_AST: public typealias ParamAttrs6 = (@autoclosure () -> ()) -> ()
public typealias ParamAttrs6 = (@autoclosure () -> ()) -> ()

// The following type only has the internal paramter name inferred from the
// closure on the right-hand side of `=`. Thus, it is only part of the `Type`
// and not part of the `TypeRepr`.
// PASS_PRINT_AST_TYPE: public var ParamAttrs7: (_ f: @escaping () -> ()) -> ()
// PASS_PRINT_AST_TYPEREPR: public var ParamAttrs7: (@escaping () -> ()) -> ()
public var ParamAttrs7: (@escaping () -> ()) -> () = { f in f() }

// PASS_PRINT_AST: public var ParamAttrs8: (_ f: @escaping () -> ()) -> ()
public var ParamAttrs8: (_ f: @escaping () -> ()) -> () = { f in f() }

// Setter
// PASS_PRINT_AST: class FooClassComputed {
class FooClassComputed {

// PASS_PRINT_AST:   var stored: (((Int) -> Int) -> Int)?
  var stored : (((Int) -> Int) -> Int)? = nil

// PASS_PRINT_AST:   var computed: ((Int) -> Int) -> Int { get set }
  var computed : ((Int) -> Int) -> Int {
    get { return stored! }
    set { stored = newValue }
  }

// PASS_PRINT_AST: }
}

// PASS_PRINT_AST: struct HasDefaultTupleOfNils {
// PASS_PRINT_AST:   var x: (Int?, Int?)
// PASS_PRINT_AST:   var y: Int?
// PASS_PRINT_AST:   var z: Int
// PASS_PRINT_AST:   var w: ((Int?, (), Int?), (Int?, Int?))
// PASS_PRINT_AST:   init(x: (Int?, Int?) = (nil, nil), y: Int? = nil, z: Int, w: ((Int?, (), Int?), (Int?, Int?)) = ((nil, (), nil), (nil, nil)))
// PASS_PRINT_AST: }
struct HasDefaultTupleOfNils {
  var x: (Int?, Int?)
  var y: Int?
  var z: Int
  var w: ((Int?, (), Int?), (Int?, Int?))
}

// Protocol extensions

protocol ProtocolToExtend {
  associatedtype Assoc
}

extension ProtocolToExtend where Self.Assoc == Int {}
// PREFER_TYPE_REPR_PRINTING: extension ProtocolToExtend where Self.Assoc == Int {

// Protocol with where clauses

protocol ProtocolWithWhereClause : QuxProtocol where Qux == Int {}
// PREFER_TYPE_REPR_PRINTING: protocol ProtocolWithWhereClause : QuxProtocol where Self.Qux == Int {

protocol ProtocolWithWhereClauseAndAssoc : QuxProtocol where Qux == Int {
// PREFER_TYPE_REPR_PRINTING-DAG: protocol ProtocolWithWhereClauseAndAssoc : QuxProtocol where Self.Qux == Int {
  associatedtype A1 : QuxProtocol where A1 : FooProtocol, A1.Qux : QuxProtocol, Int == A1.Qux.Qux
// PREFER_TYPE_REPR_PRINTING-DAG: {{^}}  associatedtype A1 : FooProtocol, QuxProtocol where Self.A1.Qux : QuxProtocol, Self.A1.Qux.Qux == Int{{$}}

  associatedtype A2 : QuxProtocol where A2.Qux == Self
// PREFER_TYPE_REPR_PRINTING-DAG: {{^}}  associatedtype A2 : QuxProtocol where Self == Self.A2.Qux{{$}}
}

#if true
#elseif false
#else
#endif
// PASS_PRINT_AST: #if
// PASS_PRINT_AST: #elseif
// PASS_PRINT_AST: #else
// PASS_PRINT_AST: #endif

public struct MyPair<A, B> { var a: A, b: B }
public typealias MyPairI<B> = MyPair<Int, B>
// PASS_PRINT_AST: public typealias MyPairI<B> = MyPair<Int, B>
public typealias MyPairAlias<T, U> = MyPair<T, U>
// PASS_PRINT_AST: public typealias MyPairAlias<T, U> = MyPair<T, U>
typealias MyPairAlias2<T: FooProtocol, U> = MyPair<T, U> where U: BarProtocol
// PASS_PRINT_AST: typealias MyPairAlias2<T, U> = MyPair<T, U> where T : FooProtocol, U : BarProtocol
