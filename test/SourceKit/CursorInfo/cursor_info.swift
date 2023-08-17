import Foo
import FooSwiftModule

var glob : Int

func foo(_ x: Int) {}

func goo(_ x: Int) {
  foo(glob+x+Int(fooIntVar)+fooSwiftFunc())
}

/// Aaa.  S1.  Bbb.
struct S1 {}
var w : S1
func test2(_ x: S1) {}

class CC {
  init(x: Int) {
    self.init(x:0)
  }
}

var testString = "testString"
let testLetString = "testString"

func testLetParam(arg1 : Int) {
}
func testInoutParam(arg1 : inout Int) {
}

func testDefaultParam(arg1: Int = 0) {
}

fooSubFunc1(0)

func myFunc(arg1: String) {
}
func myFunc(arg1: String, options: Int) {
}

var derivedObj = FooClassDerived()

typealias MyInt = Int
var x: MyInt

import FooHelper.FooHelperSub

class C2 {
  lazy var lazy_bar : Int = {
    return x
  }()
}

func test1(_ foo: FooUnavailableMembers) {
  foo.availabilityIntroduced()
  foo.swiftUnavailable()
  foo.unavailable()
  foo.availabilityIntroducedMsg()
  foo.availabilityDeprecated()
}

public class SubscriptCursorTest {
  public subscript(i: Int) -> Int {
    return 0
  }

  public static func test() {
    let s = SubscriptCursorTest()
    let a = s[1234] + s[4321]
  }
}

class C3 {
  deinit {}
  init!(x: Int) { return nil }
  init?(y: Int) { return nil }
  init(z: Int) throws {}
}

struct S2<T, U where T == U> {
  func foo<V, W where V == W> (_: V, _: W, _ closure: ()->()) -> ()->() { return closure }
}
class C4<T, U where T == U> {}
enum E1<T, U where T == U> {}

func nonDefaultArgNames(external1 local1: Int, _ local2: Int, external3 local3: Int, external4 _: Int, _: Int) {}

func nestedFunctionType(closure: (_ y: (_ z: Int) -> Int) -> Int) -> (_ y: (_ z: Int) -> Int) -> Int { return closure }

enum E2 {
  case C1
  case C2(x: Int, y: String), C3(Int)
}

enum E3: String {
  case C = "a"
}

func refEnumElements() {
  let w = E2.C1
  let x = E2.C2(x: 1, y: "")
  let y: E2 = .C2(x: 2, y: "")
  let z: E3 = .C
}

class C4 {
  static var v1: Int = 0
  final class var v2: Int = 0
  static func f1() {}
  final class func f2() {}
}

protocol P1 {
  associatedtype T
}

func genReq<U, V: P1>(_ u: U, v: V) where V.T == U {}

@objc class C5 {

  @objc(mmm1)
  func m1() {}

  private(set)
  public
  var v1: Int = 1
}

let tupleVar1: (((Int, Int), y: Int), z: Int)
let tupleVar2: (f: ()->(), g: (_ x: Int)->Int)
let tupleVar3: (f: (_ x: inout (Int, Int)) throws ->(), Int)

enum E4: Int {
  case A = -1
  case B = 0
  case C = 1
}
enum E5: Int {
  case A  // implicit = 0
}
enum E6: Float {
  case A = -0.0
  case B = 1e10
}

class C6: C4, P1 {
  typealias T = Int
}

protocol P2: class, P1 {}

typealias MyAlias<T, U> = (T, U, T, U)
typealias MyAlias2<A, B> = MyAlias<A, B>

func paramAutoclosureNoescape1(_ msg: ()->String) {}
func paramAutoclosureNoescape2(_ msg: @autoclosure ()->String) {}
func paramAutoclosureNoescape3(_ msg: @autoclosure @escaping ()->String) {}

func paramDefaultPlaceholder(_ f: StaticString = #function, file: StaticString = #file, line: UInt = #line, col: UInt = #column, arr: [Int] = [], dict: [Int: Int] = [:], opt: Int? = nil, reg: Int = 1) {}

protocol P3 {
  func f(_ s: Self) -> Self
}
extension P3: P2 {
  func f(_ s: Self) -> Self { return s }
}

class C7 {
  func f() -> Self { return self }
}

public protocol P4 {
  /// foo1 comment from P4
  func foo1()
  /// foo2 comment from P4
  func foo2()
}

public class C8 : P4 {
  public func foo1() {
  }
  /// foo2 comment from C1
  public func foo2() {
  }
}

func foo2(_ f: C8) {
  f.foo1()
  f.foo2()
}

func tupleInParam1(t: (Int, Int)) {}
func tupleInParam2(t: ()) {}
func tupleInParam2(t: () -> ()) {}
func tupleResult1() -> (Int, Int) {}
func tupleResult2(f: () -> Void) {}
typealias MyVoid = ()

func rethrowingFunction1(_: (Int) throws -> Void) rethrows -> Void {}

func convention1(_: @convention(thick) ()->()) {}
func convention2(_: @convention(thin) ()->()) {}
func convention3(_: @convention(block) ()->()) {}
func convention4(_: @convention(c) ()->()) {}
func convention4a(_: @convention(c, cType: "void *(void *, int[])") () -> () {}
func convention5(_: @convention(method) ()->()) {}
func convention6(_: @convention(objc_method) ()->()) {}
func convention7(_: @convention(witness_method: P1) ()->()) {}

/// Brief.
///
/// - LocalizationKey: ABC
struct HasLocalizationKey {}

/// - LocalizationKey: ABC
func hasLocalizationKey2() {}

/// doc
func funcWithNestedEscaping(a : (@escaping () -> ()) -> ()) {}

/// doc
typealias typeWithNestedAutoclosure = (@autoclosure () -> ()) -> ()

typealias GenericAlias<T, U> = MyAlias<T, U> where T: P1

enum E7: String {
    case a = "\u{001B}"
    case b = "\u{66}"
}

func checkAnyIsAKeyword(x: Any) {}

var binExpr = 1 + 2 + 3

struct A: ExpressibleByIntegerLiteral {
    init(integerLiteral value: Int) {
        self.value = value
    }
    let value: Int
}
var a: A = 42

let stringStr = "str"
let strInterpolation = "This is a \(stringStr + "ing") interpolation"

// REQUIRES: objc_interop
// RUN: %empty-directory(%t.tmp)
// RUN: %swiftc_driver -emit-module -o %t.tmp/FooSwiftModule.swiftmodule %S/Inputs/FooSwiftModule.swift
// RUN: %sourcekitd-test -req=cursor -pos=9:8 %s -- -F %S/../Inputs/libIDE-mock-sdk %s | %FileCheck -check-prefix=CHECK1 %s
// CHECK1:      source.lang.swift.ref.var.global (4:5-4:9)
// CHECK1-NEXT: glob
// CHECK1-NEXT: s:11cursor_info4globSivp{{$}}
// CHECK1-NEXT: source.lang.swift
// CHECK1-NEXT: Int

// FIXME(integers): Disabling the checks. See <rdar://problem/31207310>
// XUN: %sourcekitd-test -req=cursor -pos=9:13 %s -- -F %S/../Inputs/libIDE-mock-sdk -I %t.tmp %s | %FileCheck -check-prefix=CHECK2 %s
// XCHECK2:      source.lang.swift.ref.function.operator.infix ()
// XCHECK2-NEXT: +
// XCHECK2-NEXT: s:s1poiyS2i_SitF
// XCHECK2-NEXT: source.lang.swift
// XCHECK2-NEXT: (Int, Int) -> Int{{$}}
// XCHECK2-NEXT: $sS2i_SitcD
// XCHECK2-NEXT: Swift{{$}}
// XCHECK2-NEXT: <Group>Math/Integers</Group>
// XCHECK2-NEXT: SYSTEM
// XCHECK2-NEXT: <Declaration>func +(lhs: <Type usr="s:Si">Int</Type>, rhs: <Type usr="s:Si">Int</Type>) -&gt; <Type usr="s:Si">Int</Type></Declaration>
// XCHECK2-NEXT: <decl.function.operator.infix><syntaxtype.keyword>func</syntaxtype.keyword> <decl.name>+</decl.name>(<decl.var.parameter><decl.var.parameter.name>lhs</decl.var.parameter.name>: <decl.var.parameter.type><ref.struct usr="s:Si">Int</ref.struct></decl.var.parameter.type></decl.var.parameter>, <decl.var.parameter><decl.var.parameter.name>rhs</decl.var.parameter.name>: <decl.var.parameter.type><ref.struct usr="s:Si">Int</ref.struct></decl.var.parameter.type></decl.var.parameter>) -&gt; <decl.function.returntype><ref.struct usr="s:Si">Int</ref.struct></decl.function.returntype></decl.function.operator.infix>

// RUN: %sourcekitd-test -req=cursor -pos=9:12 %s -- -F %S/../Inputs/libIDE-mock-sdk -I %t.tmp %s | %FileCheck -check-prefix=CHECK3 %s
// CHECK3:      source.lang.swift.ref.var.local (8:12-8:13)
// CHECK3-NEXT: x{{$}}
// CHECK3-NEXT: s:11cursor_info3gooyySiF1xL_Sivp{{$}}
// CHECK3-NEXT: source.lang.swift
// CHECK3-NEXT: Int{{$}}
// CHECK3-NEXT: $sSiD
// CHECK3-NEXT: cursor_info{{$}}
// CHECK3-NEXT: <Declaration>let x: <Type usr="s:Si">Int</Type></Declaration>
// CHECK3-NEXT: <decl.var.parameter><syntaxtype.keyword>let</syntaxtype.keyword> <decl.var.parameter.name>x</decl.var.parameter.name>: <decl.var.parameter.type><ref.struct usr="s:Si">Int</ref.struct></decl.var.parameter.type></decl.var.parameter>

// RUN: %sourcekitd-test -req=cursor -pos=9:18 %s -- -F %S/../Inputs/libIDE-mock-sdk -I %t.tmp %s | %FileCheck -check-prefix=CHECK4 %s
// CHECK4:      source.lang.swift.ref.var.global ({{.*}}Foo.framework/Headers/Foo.h:63:12-63:21)
// CHECK4-NEXT: fooIntVar{{$}}
// CHECK4-NEXT: c:@fooIntVar{{$}}
// CHECK4-NEXT: source.lang.objc
// CHECK4-NEXT: Int32{{$}}
// CHECK4-NEXT: $ss5Int32VD
// CHECK4-NEXT: Foo{{$}}
// CHECK4-NEXT: <Declaration>var fooIntVar: <Type usr="s:s5Int32V">Int32</Type></Declaration>
// CHECK4-NEXT: <decl.var.global><syntaxtype.keyword>var</syntaxtype.keyword> <decl.name>fooIntVar</decl.name>: <decl.var.type><ref.struct usr="s:s5Int32V">Int32</ref.struct></decl.var.type></decl.var.global>
// CHECK4-NEXT: <Variable file="{{[^"]+}}Foo.h" line="{{[0-9]+}}" column="{{[0-9]+}}"><Name>fooIntVar</Name><USR>c:@fooIntVar</USR><Declaration>var fooIntVar: Int32</Declaration><Abstract><Para> Aaa. fooIntVar. Bbb.</Para></Abstract></Variable>

// RUN: %sourcekitd-test -req=cursor -pos=8:7 %s -- -F %S/../Inputs/libIDE-mock-sdk -I %t.tmp %s | %FileCheck -check-prefix=CHECK5 %s
// CHECK5:      source.lang.swift.decl.function.free (8:6-8:19)
// CHECK5-NEXT: goo(_:){{$}}
// CHECK5-NEXT: s:11cursor_info3gooyySiF{{$}}
// CHECK5-NEXT: source.lang.swift
// CHECK5-NEXT: (Int) -> (){{$}}

// RUN: %sourcekitd-test -req=cursor -pos=9:32 %s -- -F %S/../Inputs/libIDE-mock-sdk -I %t.tmp %s | %FileCheck -check-prefix=CHECK6 %s
// CHECK6:      source.lang.swift.ref.function.free ({{.*}}/FooSwiftModule.swift:2:13-2:25)
// CHECK6-NEXT: fooSwiftFunc
// CHECK6-NEXT: s:14FooSwiftModule03fooB4FuncSiyF
// CHECK6-NEXT: source.lang.swift
// CHECK6-NEXT: () -> Int
// CHECK6-NEXT: $sSiycD
// CHECK6-NEXT: FooSwiftModule
// CHECK6-NEXT: <Declaration>func fooSwiftFunc() -&gt; <Type usr="s:Si">Int</Type></Declaration>
// CHECK6-NEXT: <decl.function.free><syntaxtype.keyword>func</syntaxtype.keyword> <decl.name>fooSwiftFunc</decl.name>() -&gt; <decl.function.returntype><ref.struct usr="s:Si">Int</ref.struct></decl.function.returntype></decl.function.free>
// CHECK6-NEXT: {{^}}<Function file="{{.*}}/FooSwiftModule.swift" line="2" column="13"><Name>fooSwiftFunc()</Name><USR>s:14FooSwiftModule03fooB4FuncSiyF</USR><Declaration>func fooSwiftFunc() -&gt; Int</Declaration><CommentParts><Abstract><Para>This is ‘fooSwiftFunc’ from ‘FooSwiftModule’.</Para></Abstract></CommentParts></Function>{{$}}

// RUN: %sourcekitd-test -req=cursor -pos=14:10 %s -- -F %S/../Inputs/libIDE-mock-sdk -I %t.tmp %s | %FileCheck -check-prefix=CHECK7 %s
// CHECK7:      source.lang.swift.ref.struct (13:8-13:10)
// CHECK7-NEXT: S1
// CHECK7-NEXT: s:11cursor_info2S1V
// CHECK7-NEXT: source.lang.swift
// CHECK7-NEXT: S1.Type
// CHECK7-NEXT: $s
// CHECK7-NEXT: cursor_info{{$}}
// CHECK7-NEXT: <Declaration>struct S1</Declaration>
// CHECK7-NEXT: <decl.struct><syntaxtype.keyword>struct</syntaxtype.keyword> <decl.name>S1</decl.name></decl.struct>
// CHECK7-NEXT: <Class file="{{[^"]+}}cursor_info.swift" line="13" column="8"><Name>S1</Name><USR>s:11cursor_info2S1V</USR><Declaration>struct S1</Declaration><CommentParts><Abstract><Para>Aaa.  S1.  Bbb.</Para></Abstract></CommentParts></Class>

// RUN: %sourcekitd-test -req=cursor -pos=19:12 %s -- -F %S/../Inputs/libIDE-mock-sdk -I %t.tmp %s | %FileCheck -check-prefix=CHECK8 %s
// CHECK8:      source.lang.swift.ref.function.constructor (18:3-18:15)
// CHECK8-NEXT: init
// CHECK8-NEXT: s:11cursor_info2CCC1xACSi_tcfc
// CHECK8-NEXT: source.lang.swift
// CHECK8-NEXT: (CC.Type) -> (Int) -> CC
// CHECK8-NEXT: $s1x11cursor_info2CCCSi_tcD
// CHECK8-NEXT: <Container>$s11cursor_info2CCCD</Container>
// CHECK8-NEXT: cursor_info{{$}}
// CHECK8-NEXT: <Declaration>init(x: <Type usr="s:Si">Int</Type>)</Declaration>
// CHECK8-NEXT: <decl.function.constructor><syntaxtype.keyword>init</syntaxtype.keyword>(<decl.var.parameter><decl.var.parameter.argument_label>x</decl.var.parameter.argument_label>: <decl.var.parameter.type><ref.struct usr="s:Si">Int</ref.struct></decl.var.parameter.type></decl.var.parameter>)</decl.function.constructor>

// RUN: %sourcekitd-test -req=cursor -pos=23:6 %s -- -F %S/../Inputs/libIDE-mock-sdk -I %t.tmp %s | %FileCheck -check-prefix=CHECK9 %s
// CHECK9:      source.lang.swift.decl.var.global (23:5-23:15)
// CHECK9: <Declaration>var testString: <Type usr="s:SS">String</Type></Declaration>
// CHECK9: <decl.var.global><syntaxtype.keyword>var</syntaxtype.keyword> <decl.name>testString</decl.name>: <decl.var.type><ref.struct usr="s:SS">String</ref.struct></decl.var.type></decl.var.global>

// RUN: %sourcekitd-test -req=cursor -pos=24:6 %s -- -F %S/../Inputs/libIDE-mock-sdk -I %t.tmp %s | %FileCheck -check-prefix=CHECK10 %s
// CHECK10: source.lang.swift.decl.var.global (24:5-24:18)
// CHECK10: <Declaration>let testLetString: <Type usr="s:SS">String</Type></Declaration>
// CHECK10: <decl.var.global><syntaxtype.keyword>let</syntaxtype.keyword> <decl.name>testLetString</decl.name>: <decl.var.type><ref.struct usr="s:SS">String</ref.struct></decl.var.type></decl.var.global>

// RUN: %sourcekitd-test -req=cursor -pos=26:20 %s -- -F %S/../Inputs/libIDE-mock-sdk -I %t.tmp %s | %FileCheck -check-prefix=CHECK11 %s
// CHECK11: source.lang.swift.decl.var.parameter (26:19-26:23)
// CHECK11: <Declaration>let arg1: <Type usr="s:Si">Int</Type></Declaration>
// CHECK11: <decl.var.parameter><syntaxtype.keyword>let</syntaxtype.keyword> <decl.var.parameter.name>arg1</decl.var.parameter.name>: <decl.var.parameter.type><ref.struct usr="s:Si">Int</ref.struct></decl.var.parameter.type></decl.var.parameter>

// RUN: %sourcekitd-test -req=cursor -pos=28:24 %s -- -F %S/../Inputs/libIDE-mock-sdk -I %t.tmp %s | %FileCheck -check-prefix=CHECK12 %s
// CHECK12: source.lang.swift.decl.var.parameter (28:21-28:25)
// CHECK12: <Declaration>var arg1: inout <Type usr="s:Si">Int</Type></Declaration>
// CHECK12: <decl.var.parameter><syntaxtype.keyword>var</syntaxtype.keyword> <decl.var.parameter.name>arg1</decl.var.parameter.name>: <decl.var.parameter.type><syntaxtype.keyword>inout</syntaxtype.keyword> <ref.struct usr="s:Si">Int</ref.struct></decl.var.parameter.type></decl.var.parameter>

// RUN: %sourcekitd-test -req=cursor -pos=31:7 %s -- -F %S/../Inputs/libIDE-mock-sdk -I %t.tmp %s | %FileCheck -check-prefix=CHECK13 %s
// CHECK13: source.lang.swift.decl.function.free (31:6-31:37)
// CHECK13: <Declaration>func testDefaultParam(arg1: <Type usr="s:Si">Int</Type> = 0)</Declaration>
// CHECK13: <decl.function.free><syntaxtype.keyword>func</syntaxtype.keyword> <decl.name>testDefaultParam</decl.name>(<decl.var.parameter><decl.var.parameter.argument_label>arg1</decl.var.parameter.argument_label>: <decl.var.parameter.type><ref.struct usr="s:Si">Int</ref.struct></decl.var.parameter.type> = 0</decl.var.parameter>)</decl.function.free>

// RUN: %sourcekitd-test -req=cursor -pos=34:4 %s -- -F %S/../Inputs/libIDE-mock-sdk -I %t.tmp %s | %FileCheck -check-prefix=CHECK14 %s
// CHECK14: source.lang.swift.ref.function.free ({{.*}}Foo.framework/Frameworks/FooSub.framework/Headers/FooSub.h:4:5-4:16)
// CHECK14: fooSubFunc1
// CHECK14: c:@F@fooSubFunc1
// CHECK14: source.lang.objc
// CHECK14: Foo.FooSub{{$}}

// RUN: %sourcekitd-test -req=cursor -pos=38:8 %s -- -F %S/../Inputs/libIDE-mock-sdk -I %t.tmp %s | %FileCheck -check-prefix=CHECK15 %s
// CHECK15: source.lang.swift.decl.function.free (38:6-38:40)
// CHECK15: myFunc
// CHECK15: <Declaration>func myFunc(arg1: <Type usr="s:SS">String</Type>, options: <Type usr="s:Si">Int</Type>)</Declaration>
// CHECK15: <decl.function.free><syntaxtype.keyword>func</syntaxtype.keyword> <decl.name>myFunc</decl.name>(<decl.var.parameter><decl.var.parameter.argument_label>arg1</decl.var.parameter.argument_label>: <decl.var.parameter.type><ref.struct usr="s:SS">String</ref.struct></decl.var.parameter.type></decl.var.parameter>, <decl.var.parameter><decl.var.parameter.argument_label>options</decl.var.parameter.argument_label>: <decl.var.parameter.type><ref.struct usr="s:Si">Int</ref.struct></decl.var.parameter.type></decl.var.parameter>)</decl.function.free>
// CHECK15: RELATED BEGIN
// CHECK15-NEXT: <RelatedName usr="s:11cursor_info6myFunc4arg1ySS_tF">myFunc(arg1:)</RelatedName>
// CHECK15-NEXT: RELATED END

// RUN: %sourcekitd-test -req=cursor -pos=41:26 %s -- -F %S/../Inputs/libIDE-mock-sdk -I %t.tmp %s | %FileCheck -check-prefix=CHECK16 %s
// CHECK16:      source.lang.swift.ref.class ({{.*}}Foo.framework/Headers/Foo.h:158:12-158:27)
// CHECK16-NEXT: FooClassDerived
// CHECK16-NEXT: c:objc(cs)FooClassDerived
// CHECK16-NEXT: source.lang.objc
// CHECK16: <Declaration>class FooClassDerived : <Type usr="c:objc(cs)FooClassBase">FooClassBase</Type>, <Type usr="c:objc(pl)FooProtocolDerived">FooProtocolDerived</Type></Declaration>
// CHECK16-NEXT: <decl.class><syntaxtype.keyword>class</syntaxtype.keyword> <decl.name>FooClassDerived</decl.name> : <ref.class usr="c:objc(cs)FooClassBase">FooClassBase</ref.class>, <ref.protocol usr="c:objc(pl)FooProtocolDerived">FooProtocolDerived</ref.protocol></decl.class>

// RUN: %sourcekitd-test -req=cursor -pos=1:10 %s -- -F %S/../Inputs/libIDE-mock-sdk -I %t.tmp %s | %FileCheck -check-prefix=CHECK17 %s
// CHECK17:      source.lang.swift.ref.module ()
// CHECK17-NEXT: Foo{{$}}

// RUN: %sourcekitd-test -req=cursor -pos=44:10 %s -- -F %S/../Inputs/libIDE-mock-sdk -I %t.tmp %s | %FileCheck -check-prefix=CHECK18 %s
// CHECK18: source.lang.swift.ref.typealias (43:11-43:16)
// CHECK18: <Declaration>typealias MyInt = <Type usr="s:Si">Int</Type></Declaration>
// CHECK18: <decl.typealias><syntaxtype.keyword>typealias</syntaxtype.keyword> <decl.name>MyInt</decl.name> = <ref.struct usr="s:Si">Int</ref.struct></decl.typealias>

// RUN: %sourcekitd-test -req=cursor -pos=46:10 %s -- -F %S/../Inputs/libIDE-mock-sdk -I %t.tmp %s | %FileCheck -check-prefix=CHECK19 %s
// CHECK19:      source.lang.swift.ref.module ()
// CHECK19-NEXT: FooHelper{{$}}

// RUN: %sourcekitd-test -req=cursor -pos=46:25 %s -- -F %S/../Inputs/libIDE-mock-sdk -I %t.tmp %s | %FileCheck -check-prefix=CHECK20 %s
// CHECK20:      source.lang.swift.ref.module ()
// CHECK20-NEXT: FooHelperSub{{$}}

// RUN: %sourcekitd-test -req=cursor -pos=50:12 %s -- -F %S/../Inputs/libIDE-mock-sdk -I %t.tmp %s | %FileCheck -check-prefix=CHECK21 %s
// CHECK21:      source.lang.swift.ref.var.global (44:5-44:6)
// CHECK21-NEXT:  {{^}}x{{$}}

// RUN: %sourcekitd-test -req=cursor -pos=55:15 %s -- -F %S/../Inputs/libIDE-mock-sdk -I %t.tmp %s | %FileCheck -check-prefix=CHECK22 %s
// CHECK22: <Declaration>func availabilityIntroduced()</Declaration>
// CHECK22: <decl.function.method.instance><syntaxtype.keyword>func</syntaxtype.keyword> <decl.name>availabilityIntroduced</decl.name>()</decl.function.method.instance>

// RUN: %sourcekitd-test -req=cursor -pos=56:15 %s -- -F %S/../Inputs/libIDE-mock-sdk -I %t.tmp %s | %FileCheck -check-prefix=CHECK23 %s
// CHECK23: <empty cursor info; internal diagnostic: "Unavailable in the current compilation context.">

// RUN: %sourcekitd-test -req=cursor -pos=57:15 %s -- -F %S/../Inputs/libIDE-mock-sdk -I %t.tmp %s | %FileCheck -check-prefix=CHECK24 %s
// CHECK24: <empty cursor info; internal diagnostic: "Unavailable in the current compilation context.">

// RUN: %sourcekitd-test -req=cursor -pos=58:15 %s -- -F %S/../Inputs/libIDE-mock-sdk -I %t.tmp %s | %FileCheck -check-prefix=CHECK25 %s
// CHECK25: <Declaration>func availabilityIntroducedMsg()</Declaration>
// CHECK25: <decl.function.method.instance><syntaxtype.keyword>func</syntaxtype.keyword> <decl.name>availabilityIntroducedMsg</decl.name>()</decl.function.method.instance>

// RUN: %sourcekitd-test -req=cursor -pos=59:15 %s -- -F %S/../Inputs/libIDE-mock-sdk -I %t.tmp %s | %FileCheck -check-prefix=CHECK26 %s
// CHECK26: <empty cursor info; internal diagnostic: "Unavailable in the current compilation context.">

// RUN: %sourcekitd-test -req=cursor -pos=69:14 %s -- -F %S/../Inputs/libIDE-mock-sdk -I %t.tmp %s | %FileCheck -check-prefix=CHECK27 %s
// CHECK27: <Declaration>public subscript(i: <Type usr="s:Si">Int</Type>) -&gt; <Type usr="s:Si">Int</Type> { get }</Declaration>
// CHECK27: <decl.function.subscript><syntaxtype.keyword>public</syntaxtype.keyword> <syntaxtype.keyword>subscript</syntaxtype.keyword>(<decl.var.parameter><decl.var.parameter.name>i</decl.var.parameter.name>: <decl.var.parameter.type><ref.struct usr="s:Si">Int</ref.struct></decl.var.parameter.type></decl.var.parameter>) -&gt; <decl.function.returntype><ref.struct usr="s:Si">Int</ref.struct></decl.function.returntype> { <syntaxtype.keyword>get</syntaxtype.keyword> }</decl.function.subscript>

// RUN: %sourcekitd-test -req=cursor -pos=69:19 %s -- -F %S/../Inputs/libIDE-mock-sdk -I %t.tmp %s | %FileCheck -check-prefix=CHECK28 %s
// CHECK28: <Declaration>public subscript(i: <Type usr="s:Si">Int</Type>) -&gt; <Type usr="s:Si">Int</Type> { get }</Declaration>
// CHECK28: <decl.function.subscript><syntaxtype.keyword>public</syntaxtype.keyword> <syntaxtype.keyword>subscript</syntaxtype.keyword>(<decl.var.parameter><decl.var.parameter.name>i</decl.var.parameter.name>: <decl.var.parameter.type><ref.struct usr="s:Si">Int</ref.struct></decl.var.parameter.type></decl.var.parameter>) -&gt; <decl.function.returntype><ref.struct usr="s:Si">Int</ref.struct></decl.function.returntype> { <syntaxtype.keyword>get</syntaxtype.keyword> }</decl.function.subscript>

// RUN: %sourcekitd-test -req=cursor -pos=74:3 %s -- -F %S/../Inputs/libIDE-mock-sdk -I %t.tmp %s | %FileCheck %s -check-prefix=CHECK29
// CHECK29: source.lang.swift.decl.function.destructor (74:3-74:9)
// CHECK29-NEXT: deinit
// CHECK29-NEXT: s:11cursor_info2C3Cfd
// CHECK29-NEXT: source.lang.swift
// CHECK29-NEXT: (C3) -> ()
// CHECK29-NEXT: $syycD
// CHECK29-NEXT: cursor_info{{$}}
// CHECK29-NEXT: <Declaration>deinit</Declaration>
// CHECK29-NEXT: <decl.function.destructor><syntaxtype.keyword>deinit</syntaxtype.keyword></decl.function.destructor>

// RUN: %sourcekitd-test -req=cursor -pos=75:3 %s -- -F %S/../Inputs/libIDE-mock-sdk -I %t.tmp %s | %FileCheck %s -check-prefix=CHECK30
// CHECK30: source.lang.swift.decl.function.constructor (75:3-75:16)
// CHECK30-NEXT: init(x:)
// CHECK30-NEXT: s:11cursor_info2C3C1xACSgSi_tcfc
// CHECK30-NEXT: source.lang.swift
// CHECK30-NEXT: (C3.Type) -> (Int) -> C3?
// CHECK30-NEXT: $s1x11cursor_info2C3CSgSi_tcD
// CHECK30-NEXT: cursor_info{{$}}
// CHECK30-NEXT: <Declaration>init!(x: <Type usr="s:Si">Int</Type>)</Declaration>
// CHECK30-NEXT: <decl.function.constructor><syntaxtype.keyword>init</syntaxtype.keyword>!(<decl.var.parameter><decl.var.parameter.argument_label>x</decl.var.parameter.argument_label>: <decl.var.parameter.type><ref.struct usr="s:Si">Int</ref.struct></decl.var.parameter.type></decl.var.parameter>)</decl.function.constructor>

// RUN: %sourcekitd-test -req=cursor -pos=76:3 %s -- -F %S/../Inputs/libIDE-mock-sdk -I %t.tmp %s | %FileCheck %s -check-prefix=CHECK31
// CHECK31: source.lang.swift.decl.function.constructor (76:3-76:16)
// CHECK31-NEXT: init(y:)
// CHECK31-NEXT: s:11cursor_info2C3C1yACSgSi_tcfc
// CHECK31-NEXT: source.lang.swift
// CHECK31-NEXT: (C3.Type) -> (Int) -> C3?
// CHECK31-NEXT: $s1y11cursor_info2C3CSgSi_tcD
// CHECK31-NEXT: cursor_info{{$}}
// CHECK31-NEXT: <Declaration>init?(y: <Type usr="s:Si">Int</Type>)</Declaration>
// CHECK31-NEXT: <decl.function.constructor><syntaxtype.keyword>init</syntaxtype.keyword>?(<decl.var.parameter><decl.var.parameter.argument_label>y</decl.var.parameter.argument_label>: <decl.var.parameter.type><ref.struct usr="s:Si">Int</ref.struct></decl.var.parameter.type></decl.var.parameter>)</decl.function.constructor>

// RUN: %sourcekitd-test -req=cursor -pos=77:3 %s -- -F %S/../Inputs/libIDE-mock-sdk -I %t.tmp %s | %FileCheck %s -check-prefix=CHECK32
// CHECK32: source.lang.swift.decl.function.constructor (77:3-77:15)
// CHECK32-NEXT: init(z:)
// CHECK32-NEXT: s:11cursor_info2C3C1zACSi_tKcfc
// CHECK32-NEXT: source.lang.swift
// CHECK32-NEXT: (C3.Type) -> (Int) throws -> C3
// CHECK32-NEXT: $s1z11cursor_info2C3CSi_tKcD
// CHECK32-NEXT: cursor_info{{$}}
// CHECK32-NEXT: <Declaration>init(z: <Type usr="s:Si">Int</Type>) throws</Declaration>
// CHECK32-NEXT: <decl.function.constructor><syntaxtype.keyword>init</syntaxtype.keyword>(<decl.var.parameter><decl.var.parameter.argument_label>z</decl.var.parameter.argument_label>: <decl.var.parameter.type><ref.struct usr="s:Si">Int</ref.struct></decl.var.parameter.type></decl.var.parameter>) <syntaxtype.keyword>throws</syntaxtype.keyword></decl.function.constructor>

// RUN: %sourcekitd-test -req=cursor -pos=80:8 %s -- -F %S/../Inputs/libIDE-mock-sdk -I %t.tmp %s | %FileCheck %s -check-prefix=CHECK33
// CHECK33: source.lang.swift.decl.struct (80:8-80:10)
// CHECK33-NEXT: S2
// CHECK33-NEXT: s:11cursor_info2S2V
// CHECK33-NEXT: source.lang.swift
// CHECK33-NEXT: S2<T, U>.Type
// CHECK33: <Declaration>struct S2&lt;T, U&gt; where <Type usr="s:11cursor_info2S2V1Txmfp">T</Type> == <Type usr="s:11cursor_info2S2V1Uq_mfp">U</Type></Declaration>
// CHECK33-NEXT: <decl.struct><syntaxtype.keyword>struct</syntaxtype.keyword> <decl.name>S2</decl.name>&lt;<decl.generic_type_param usr="s:11cursor_info2S2V1Txmfp"><decl.generic_type_param.name>T</decl.generic_type_param.name></decl.generic_type_param>, <decl.generic_type_param usr="s:11cursor_info2S2V1Uq_mfp"><decl.generic_type_param.name>U</decl.generic_type_param.name></decl.generic_type_param>&gt; <syntaxtype.keyword>where</syntaxtype.keyword> <decl.generic_type_requirement><ref.generic_type_param usr="s:11cursor_info2S2V1Txmfp">T</ref.generic_type_param> == <ref.generic_type_param usr="s:11cursor_info2S2V1Uq_mfp">U</ref.generic_type_param></decl.generic_type_requirement></decl.struct>

// RUN: %sourcekitd-test -req=cursor -pos=81:8 %s -- -F %S/../Inputs/libIDE-mock-sdk -I %t.tmp %s | %FileCheck %s -check-prefix=CHECK34
// CHECK34: source.lang.swift.decl.function.method.instance (81:8-81:62)
// CHECK34-NEXT: foo(_:_:_:)
// CHECK34-NEXT: s:11cursor_info2S2V3fooyyycqd___qd__yyXEtqd_0_Rsd__r0_lF
// CHECK34-NEXT: source.lang.swift
// CHECK34-NEXT: <T, U, V, W where T == U, V == W> (S2<T, U>) -> (V, W, () -> ()) -> () -> ()
// CHECK34: <Declaration>func foo&lt;V, W&gt;(_: <Type usr="s:11cursor_info2S2V3fooyyycqd___qd__yyXEtqd_0_Rsd__r0_lF1VL_qd__mfp">V</Type>, _: <Type usr="s:11cursor_info2S2V3fooyyycqd___qd__yyXEtqd_0_Rsd__r0_lF1WL_qd_0_mfp">W</Type>, _ closure: () -&gt; ()) -&gt; () -&gt; () where <Type usr="s:11cursor_info2S2V3fooyyycqd___qd__yyXEtqd_0_Rsd__r0_lF1VL_qd__mfp">V</Type> == <Type usr="s:11cursor_info2S2V3fooyyycqd___qd__yyXEtqd_0_Rsd__r0_lF1WL_qd_0_mfp">W</Type></Declaration>
// CHECK34: <decl.function.method.instance><syntaxtype.keyword>func</syntaxtype.keyword> <decl.name>foo</decl.name>&lt;<decl.generic_type_param usr="s:11cursor_info2S2V3fooyyycqd___qd__yyXEtqd_0_Rsd__r0_lF1VL_qd__mfp"><decl.generic_type_param.name>V</decl.generic_type_param.name></decl.generic_type_param>, <decl.generic_type_param usr="s:11cursor_info2S2V3fooyyycqd___qd__yyXEtqd_0_Rsd__r0_lF1WL_qd_0_mfp"><decl.generic_type_param.name>W</decl.generic_type_param.name></decl.generic_type_param>&gt;(<decl.var.parameter><decl.var.parameter.argument_label>_</decl.var.parameter.argument_label>: <decl.var.parameter.type><ref.generic_type_param usr="s:11cursor_info2S2V3fooyyycqd___qd__yyXEtqd_0_Rsd__r0_lF1VL_qd__mfp">V</ref.generic_type_param></decl.var.parameter.type></decl.var.parameter>, <decl.var.parameter><decl.var.parameter.argument_label>_</decl.var.parameter.argument_label>: <decl.var.parameter.type><ref.generic_type_param usr="s:11cursor_info2S2V3fooyyycqd___qd__yyXEtqd_0_Rsd__r0_lF1WL_qd_0_mfp">W</ref.generic_type_param></decl.var.parameter.type></decl.var.parameter>, <decl.var.parameter><decl.var.parameter.argument_label>_</decl.var.parameter.argument_label> <decl.var.parameter.name>closure</decl.var.parameter.name>: <decl.var.parameter.type>() -&gt; <decl.function.returntype><tuple>()</tuple></decl.function.returntype></decl.var.parameter.type></decl.var.parameter>) -&gt; <decl.function.returntype>() -&gt; <decl.function.returntype><tuple>()</tuple></decl.function.returntype></decl.function.returntype> <syntaxtype.keyword>where</syntaxtype.keyword> <decl.generic_type_requirement><ref.generic_type_param usr="s:11cursor_info2S2V3fooyyycqd___qd__yyXEtqd_0_Rsd__r0_lF1VL_qd__mfp">V</ref.generic_type_param> == <ref.generic_type_param usr="s:11cursor_info2S2V3fooyyycqd___qd__yyXEtqd_0_Rsd__r0_lF1WL_qd_0_mfp">W</ref.generic_type_param></decl.generic_type_requirement></decl.function.method.instance>

// RUN: %sourcekitd-test -req=cursor -pos=83:7 %s -- -F %S/../Inputs/libIDE-mock-sdk -I %t.tmp %s | %FileCheck %s -check-prefix=CHECK35
// CHECK35: source.lang.swift.decl.class (83:7-83:9)
// CHECK35-NEXT: C4
// CHECK35-NEXT: s:11cursor_info2C4C
// CHECK35-NEXT: source.lang.swift
// CHECK35-NEXT: C4<T, U>.Type
// CHECK35: <Declaration>class C4&lt;T, U&gt; where <Type usr="s:11cursor_info2C4C1Txmfp">T</Type> == <Type usr="s:11cursor_info2C4C1Uq_mfp">U</Type></Declaration>
// CHECK35-NEXT: <decl.class><syntaxtype.keyword>class</syntaxtype.keyword> <decl.name>C4</decl.name>&lt;<decl.generic_type_param usr="s:11cursor_info2C4C1Txmfp"><decl.generic_type_param.name>T</decl.generic_type_param.name></decl.generic_type_param>, <decl.generic_type_param usr="s:11cursor_info2C4C1Uq_mfp"><decl.generic_type_param.name>U</decl.generic_type_param.name></decl.generic_type_param>&gt; <syntaxtype.keyword>where</syntaxtype.keyword> <decl.generic_type_requirement><ref.generic_type_param usr="s:11cursor_info2C4C1Txmfp">T</ref.generic_type_param> == <ref.generic_type_param usr="s:11cursor_info2C4C1Uq_mfp">U</ref.generic_type_param></decl.generic_type_requirement></decl.class>

// RUN: %sourcekitd-test -req=cursor -pos=84:6 %s -- -F %S/../Inputs/libIDE-mock-sdk -I %t.tmp %s | %FileCheck %s -check-prefix=CHECK36
// CHECK36: source.lang.swift.decl.enum (84:6-84:8)
// CHECK36-NEXT: E1
// CHECK36-NEXT: s:11cursor_info2E1O
// CHECK36-NEXT: source.lang.swift
// CHECK36-NEXT: E1<T, U>.Type
// CHECK36: <Declaration>enum E1&lt;T, U&gt; where <Type usr="s:11cursor_info2E1O1Txmfp">T</Type> == <Type usr="s:11cursor_info2E1O1Uq_mfp">U</Type></Declaration>
// CHECK36-NEXT: <decl.enum><syntaxtype.keyword>enum</syntaxtype.keyword> <decl.name>E1</decl.name>&lt;<decl.generic_type_param usr="s:11cursor_info2E1O1Txmfp"><decl.generic_type_param.name>T</decl.generic_type_param.name></decl.generic_type_param>, <decl.generic_type_param usr="s:11cursor_info2E1O1Uq_mfp"><decl.generic_type_param.name>U</decl.generic_type_param.name></decl.generic_type_param>&gt; <syntaxtype.keyword>where</syntaxtype.keyword> <decl.generic_type_requirement><ref.generic_type_param usr="s:11cursor_info2E1O1Txmfp">T</ref.generic_type_param> == <ref.generic_type_param usr="s:11cursor_info2E1O1Uq_mfp">U</ref.generic_type_param></decl.generic_type_requirement></decl.enum>

// RUN: %sourcekitd-test -req=cursor -pos=86:6 %s -- -F %S/../Inputs/libIDE-mock-sdk -I %t.tmp %s | %FileCheck %s -check-prefix=CHECK37
// CHECK37: source.lang.swift.decl.function.free (86:6-86:111)
// CHECK37-NEXT: nonDefaultArgNames(external1:_:external3:external4:_:)
// CHECK37-NEXT: s:11cursor_info18nonDefaultArgNames9external1_9external39external4_ySi_S4itF
// CHECK37-NEXT: source.lang.swift
// CHECK37-NEXT: (Int, Int, Int, Int, Int) -> ()
// CHECK37: <Declaration>func nonDefaultArgNames(external1 local1: <Type usr="s:Si">Int</Type>, _ local2: <Type usr="s:Si">Int</Type>, external3 local3: <Type usr="s:Si">Int</Type>, external4 _: <Type usr="s:Si">Int</Type>, _: <Type usr="s:Si">Int</Type>)</Declaration>
// CHECK37-NEXT: <decl.function.free><syntaxtype.keyword>func</syntaxtype.keyword> <decl.name>nonDefaultArgNames</decl.name>(<decl.var.parameter><decl.var.parameter.argument_label>external1</decl.var.parameter.argument_label> <decl.var.parameter.name>local1</decl.var.parameter.name>: <decl.var.parameter.type><ref.struct usr="s:Si">Int</ref.struct></decl.var.parameter.type></decl.var.parameter>, <decl.var.parameter><decl.var.parameter.argument_label>_</decl.var.parameter.argument_label> <decl.var.parameter.name>local2</decl.var.parameter.name>: <decl.var.parameter.type><ref.struct usr="s:Si">Int</ref.struct></decl.var.parameter.type></decl.var.parameter>, <decl.var.parameter><decl.var.parameter.argument_label>external3</decl.var.parameter.argument_label> <decl.var.parameter.name>local3</decl.var.parameter.name>: <decl.var.parameter.type><ref.struct usr="s:Si">Int</ref.struct></decl.var.parameter.type></decl.var.parameter>, <decl.var.parameter><decl.var.parameter.argument_label>external4</decl.var.parameter.argument_label> <decl.var.parameter.name>_</decl.var.parameter.name>: <decl.var.parameter.type><ref.struct usr="s:Si">Int</ref.struct></decl.var.parameter.type></decl.var.parameter>, <decl.var.parameter><decl.var.parameter.argument_label>_</decl.var.parameter.argument_label>: <decl.var.parameter.type><ref.struct usr="s:Si">Int</ref.struct></decl.var.parameter.type></decl.var.parameter>)</decl.function.free>

// RUN: %sourcekitd-test -req=cursor -pos=88:6 %s -- -F %S/../Inputs/libIDE-mock-sdk -I %t.tmp %s | %FileCheck %s -check-prefix=CHECK38
// CHECK38: <decl.function.free><syntaxtype.keyword>func</syntaxtype.keyword> <decl.name>nestedFunctionType</decl.name>(<decl.var.parameter><decl.var.parameter.argument_label>closure</decl.var.parameter.argument_label>: <decl.var.parameter.type>(<decl.var.parameter><decl.var.parameter.argument_label>_</decl.var.parameter.argument_label> <decl.var.parameter.name>y</decl.var.parameter.name>: <decl.var.parameter.type>(<decl.var.parameter><decl.var.parameter.argument_label>_</decl.var.parameter.argument_label> <decl.var.parameter.name>z</decl.var.parameter.name>: <decl.var.parameter.type><ref.struct usr="s:Si">Int</ref.struct></decl.var.parameter.type></decl.var.parameter>) -&gt; <decl.function.returntype><ref.struct usr="s:Si">Int</ref.struct></decl.function.returntype></decl.var.parameter.type></decl.var.parameter>) -&gt; <decl.function.returntype><ref.struct usr="s:Si">Int</ref.struct></decl.function.returntype></decl.var.parameter.type></decl.var.parameter>) -&gt; <decl.function.returntype>(<decl.var.parameter><decl.var.parameter.argument_label>_</decl.var.parameter.argument_label> <decl.var.parameter.name>y</decl.var.parameter.name>: <decl.var.parameter.type>(<decl.var.parameter><decl.var.parameter.argument_label>_</decl.var.parameter.argument_label> <decl.var.parameter.name>z</decl.var.parameter.name>: <decl.var.parameter.type><ref.struct usr="s:Si">Int</ref.struct></decl.var.parameter.type></decl.var.parameter>) -&gt; <decl.function.returntype><ref.struct usr="s:Si">Int</ref.struct></decl.function.returntype></decl.var.parameter.type></decl.var.parameter>) -&gt; <decl.function.returntype><ref.struct usr="s:Si">Int</ref.struct></decl.function.returntype></decl.function.returntype></decl.function.free>

// RUN: %sourcekitd-test -req=cursor -pos=91:8 %s -- -F %S/../Inputs/libIDE-mock-sdk -I %t.tmp %s | %FileCheck %s -check-prefix=CHECK39
// CHECK39: source.lang.swift.decl.enumelement (91:8-91:10)
// CHECK39-NEXT: C1
// CHECK39-NEXT: s:11cursor_info2E2O2C1yA2CmF
// CHECK39-NEXT: source.lang.swift
// CHECK39-NEXT: (E2.Type) -> E2
// CHECK39: <Declaration>case C1</Declaration>
// CHECK39-NEXT: <decl.enumelement><syntaxtype.keyword>case</syntaxtype.keyword> <decl.name>C1</decl.name></decl.enumelement>

// RUN: %sourcekitd-test -req=cursor -pos=92:8 %s -- -F %S/../Inputs/libIDE-mock-sdk -I %t.tmp %s | %FileCheck %s -check-prefix=CHECK40
// CHECK40: source.lang.swift.decl.enumelement (92:8-92:10)
// CHECK40-NEXT: C2
// CHECK40-NEXT: s:11cursor_info2E2O2C2yACSi_SStcACmF
// CHECK40-NEXT: source.lang.swift
// CHECK40-NEXT: (E2.Type) -> (Int, String) -> E2
// CHECK40: <Declaration>case C2(x: <Type usr="s:Si">Int</Type>, y: <Type usr="s:SS">String</Type>)</Declaration>
// CHECK40-NEXT: <decl.enumelement><syntaxtype.keyword>case</syntaxtype.keyword> <decl.name>C2</decl.name>(<decl.var.parameter><decl.var.parameter.argument_label>x</decl.var.parameter.argument_label>: <decl.var.parameter.type><ref.struct usr="s:Si">Int</ref.struct></decl.var.parameter.type></decl.var.parameter>, <decl.var.parameter><decl.var.parameter.argument_label>y</decl.var.parameter.argument_label>: <decl.var.parameter.type><ref.struct usr="s:SS">String</ref.struct></decl.var.parameter.type></decl.var.parameter>)</decl.enumelement>

// RUN: %sourcekitd-test -req=cursor -pos=92:31 %s -- -F %S/../Inputs/libIDE-mock-sdk -I %t.tmp %s | %FileCheck %s -check-prefix=CHECK41
// CHECK41: source.lang.swift.decl.enumelement (92:31-92:33)
// CHECK41-NEXT: C3
// CHECK41-NEXT: s:11cursor_info2E2O2C3yACSicACmF
// CHECK41-NEXT: source.lang.swift
// CHECK41-NEXT: (E2.Type) -> (Int) -> E2
// CHECK41: <Declaration>case C3(<Type usr="s:Si">Int</Type>)</Declaration>
// CHECK41-NEXT: <decl.enumelement><syntaxtype.keyword>case</syntaxtype.keyword> <decl.name>C3</decl.name>(<decl.var.parameter><decl.var.parameter.type><ref.struct usr="s:Si">Int</ref.struct></decl.var.parameter.type></decl.var.parameter>)</decl.enumelement>

// RUN: %sourcekitd-test -req=cursor -pos=96:8 %s -- -F %S/../Inputs/libIDE-mock-sdk -I %t.tmp %s | %FileCheck %s -check-prefix=CHECK42
// CHECK42: source.lang.swift.decl.enumelement (96:8-96:9)
// CHECK42-NEXT: C
// CHECK42-NEXT: s:11cursor_info2E3O1CyA2CmF
// CHECK42-NEXT: source.lang.swift
// CHECK42-NEXT: (E3.Type) -> E3
// CHECK42: <Declaration>case C = &quot;a&quot;</Declaration>
// CHECK42-NEXT: <decl.enumelement><syntaxtype.keyword>case</syntaxtype.keyword> <decl.name>C</decl.name> = <syntaxtype.string>&quot;a&quot;</syntaxtype.string></decl.enumelement>

// RUN: %sourcekitd-test -req=cursor -pos=100:14 %s -- -F %S/../Inputs/libIDE-mock-sdk -I %t.tmp %s | %FileCheck %s -check-prefix=CHECK43
// CHECK43: source.lang.swift.ref.enumelement (91:8-91:10)
// CHECK43-NEXT: C1
// CHECK43: <Declaration>case C1</Declaration>
// CHECK43-NEXT: <decl.enumelement><syntaxtype.keyword>case</syntaxtype.keyword> <decl.name>C1</decl.name></decl.enumelement>

// RUN: %sourcekitd-test -req=cursor -pos=101:14 %s -- -F %S/../Inputs/libIDE-mock-sdk -I %t.tmp %s | %FileCheck %s -check-prefix=CHECK44
// CHECK44: source.lang.swift.ref.enumelement (92:8-92:10)
// CHECK44-NEXT: C2
// CHECK44: <Declaration>case C2(x: <Type usr="s:Si">Int</Type>, y: <Type usr="s:SS">String</Type>)</Declaration>
// CHECK44-NEXT: <decl.enumelement><syntaxtype.keyword>case</syntaxtype.keyword> <decl.name>C2</decl.name>(<decl.var.parameter><decl.var.parameter.argument_label>x</decl.var.parameter.argument_label>: <decl.var.parameter.type><ref

// RUN: %sourcekitd-test -req=cursor -pos=102:16 %s -- -F %S/../Inputs/libIDE-mock-sdk -I %t.tmp %s | %FileCheck %s -check-prefix=CHECK45
// CHECK45: source.lang.swift.ref.enumelement (92:8-92:10)
// CHECK45-NEXT: C2
// CHECK45: <Declaration>case C2(x: <Type usr="s:Si">Int</Type>, y: <Type usr="s:SS">String</Type>)</Declaration>
// CHECK45-NEXT: <decl.enumelement><syntaxtype.keyword>case</syntaxtype.keyword> <decl.name>C2</decl.name>(<decl.var.parameter><decl.var.parameter.argument_label>x

// RUN: %sourcekitd-test -req=cursor -pos=103:16 %s -- -F %S/../Inputs/libIDE-mock-sdk -I %t.tmp %s | %FileCheck %s -check-prefix=CHECK46
// CHECK46: source.lang.swift.ref.enumelement (96:8-96:9)
// CHECK46-NEXT: C
// CHECK46: <Declaration>case C = &quot;a&quot;</Declaration>
// CHECK46-NEXT: <decl.enumelement><syntaxtype.keyword>case</syntaxtype.keyword> <decl.name>C</decl.name> = <syntaxtype.string>&quot;a&quot;</syntaxtype.string></decl.enumelement>

// RUN: %sourcekitd-test -req=cursor -pos=80:11 %s -- -F %S/../Inputs/libIDE-mock-sdk -I %t.tmp %s | %FileCheck %s -check-prefix=CHECK47
// CHECK47: source.lang.swift.decl.generic_type_param (80:11-80:12)
// CHECK47-NEXT: T
// CHECK47-NEXT: s:11cursor_info2S2V1Txmfp
// CHECK47-NEXT: source.lang.swift
// CHECK47-NEXT: T.Type
// CHECK47: <Declaration>T</Declaration>
// CHECK47-NEXT: <decl.generic_type_param><decl.generic_type_param.name>T</decl.generic_type_param.name></decl.generic_type_param>

// RUN: %sourcekitd-test -req=cursor -pos=107:14 %s -- -F %S/../Inputs/libIDE-mock-sdk -I %t.tmp %s | %FileCheck %s -check-prefix=CHECK48
// CHECK48: source.lang.swift.decl.var.static (107:14-107:16)
// CHECK48: <decl.var.static><syntaxtype.keyword>static</syntaxtype.keyword> <syntaxtype.keyword>var</syntaxtype.keyword>

// RUN: %sourcekitd-test -req=cursor -pos=108:19 %s -- -F %S/../Inputs/libIDE-mock-sdk -I %t.tmp %s | %FileCheck %s -check-prefix=CHECK49
// CHECK49: source.lang.swift.decl.var.class (108:19-108:21)
// CHECK49: <decl.var.class><syntaxtype.keyword>final</syntaxtype.keyword> <syntaxtype.keyword>class</syntaxtype.keyword> <syntaxtype.keyword>var</syntaxtype.keyword>

// RUN: %sourcekitd-test -req=cursor -pos=109:15 %s -- -F %S/../Inputs/libIDE-mock-sdk -I %t.tmp %s | %FileCheck %s -check-prefix=CHECK50
// CHECK50: source.lang.swift.decl.function.method.static (109:15-109:19)
// CHECK50: <decl.function.method.static><syntaxtype.keyword>static</syntaxtype.keyword> <syntaxtype.keyword>func</syntaxtype.keyword>

// RUN: %sourcekitd-test -req=cursor -pos=110:20 %s -- -F %S/../Inputs/libIDE-mock-sdk -I %t.tmp %s | %FileCheck %s -check-prefix=CHECK51
// CHECK51: source.lang.swift.decl.function.method.class (110:20-110:24)
// CHECK51: <decl.function.method.class><syntaxtype.keyword>final</syntaxtype.keyword> <syntaxtype.keyword>class</syntaxtype.keyword> <syntaxtype.keyword>func</syntaxtype.keyword>

// RUN: %sourcekitd-test -req=cursor -pos=117:6 %s -- -F %S/../Inputs/libIDE-mock-sdk -I %t.tmp %s | %FileCheck %s -check-prefix=CHECK52
// CHECK52: source.lang.swift.decl.function.free (117:6-117:36)
// CHECK52: <U, V where U == V.T, V : P1> (U, v: V) -> ()
// CHECK52: <decl.function.free><syntaxtype.keyword>func</syntaxtype.keyword> <decl.name>genReq</decl.name>&lt;<decl.generic_type_param usr="s:11cursor_info6genReq_1vyx_q_t1TQy_RszAA2P1R_r0_lF1UL_xmfp"><decl.generic_type_param.name>U</decl.generic_type_param.name></decl.generic_type_param>, <decl.generic_type_param usr="s:11cursor_info6genReq_1vyx_q_t1TQy_RszAA2P1R_r0_lF1VL_q_mfp"><decl.generic_type_param.name>V</decl.generic_type_param.name></decl.generic_type_param>&gt;(<decl.var.parameter><decl.var.parameter.argument_label>_</decl.var.parameter.argument_label> <decl.var.parameter.name>u</decl.var.parameter.name>: <decl.var.parameter.type><ref.generic_type_param usr="s:11cursor_info6genReq_1vyx_q_t1TQy_RszAA2P1R_r0_lF1UL_xmfp">U</ref.generic_type_param></decl.var.parameter.type></decl.var.parameter>, <decl.var.parameter><decl.var.parameter.argument_label>v</decl.var.parameter.argument_label>: <decl.var.parameter.type><ref.generic_type_param usr="s:11cursor_info6genReq_1vyx_q_t1TQy_RszAA2P1R_r0_lF1VL_q_mfp">V</ref.generic_type_param></decl.var.parameter.type></decl.var.parameter>) <syntaxtype.keyword>where</syntaxtype.keyword> <decl.generic_type_requirement><ref.generic_type_param usr="s:11cursor_info6genReq_1vyx_q_t1TQy_RszAA2P1R_r0_lF1UL_xmfp">U</ref.generic_type_param> == <ref.generic_type_param usr="s:11cursor_info6genReq_1vyx_q_t1TQy_RszAA2P1R_r0_lF1VL_q_mfp">V</ref.generic_type_param>.<ref.associatedtype usr="s:11cursor_info2P1P1TQa">T</ref.associatedtype></decl.generic_type_requirement>, <decl.generic_type_requirement><ref.generic_type_param usr="s:11cursor_info6genReq_1vyx_q_t1TQy_RszAA2P1R_r0_lF1VL_q_mfp">V</ref.generic_type_param> : <ref.protocol usr="s:11cursor_info2P1P">P1</ref.protocol></decl.generic_type_requirement></decl.function.free>

// RUN: %sourcekitd-test -req=cursor -pos=117:16 %s -- -F %S/../Inputs/libIDE-mock-sdk -I %t.tmp %s | %FileCheck %s -check-prefix=CHECK53
// CHECK53: source.lang.swift.decl.generic_type_param (117:16-117:17)
// CHECK53: <decl.generic_type_param><decl.generic_type_param.name>V</decl.generic_type_param.name> : <decl.generic_type_param.constraint><ref.protocol usr="{{.*}}">P1</ref.protocol></decl.generic_type_param.constraint></decl.generic_type_param>

// RUN: %sourcekitd-test -req=cursor -pos=119:13 %s -- -F %S/../Inputs/libIDE-mock-sdk -I %t.tmp %s | %FileCheck %s -check-prefix=CHECK54
// CHECK54: source.lang.swift.decl.class (119:13-119:15)
// CHECK54: <decl.class><syntaxtype.attribute.builtin><syntaxtype.attribute.name>@objc</syntaxtype.attribute.name></syntaxtype.attribute.builtin> <syntaxtype.keyword>class

// RUN: %sourcekitd-test -req=cursor -pos=122:8 %s -- -F %S/../Inputs/libIDE-mock-sdk -I %t.tmp %s | %FileCheck %s -check-prefix=CHECK55
// CHECK55: source.lang.swift.decl.function.method.instance (122:8-122:12)
// CHECK55: <decl.function.method.instance><syntaxtype.attribute.builtin><syntaxtype.attribute.name>@objc</syntaxtype.attribute.name>(mmm1)</syntaxtype.attribute.builtin> <syntaxtype.keyword>func

// RUN: %sourcekitd-test -req=cursor -pos=126:7 %s -- -F %S/../Inputs/libIDE-mock-sdk -I %t.tmp %s | %FileCheck %s -check-prefix=CHECK56
// CHECK56: source.lang.swift.decl.var.instance (126:7-126:9)
// CHECK56: <decl.var.instance><syntaxtype.keyword>private</syntaxtype.keyword>(set) <syntaxtype.keyword>public</syntaxtype.keyword> <syntaxtype.keyword>var

// RUN: %sourcekitd-test -req=cursor -pos=129:5 %s -- -F %S/../Inputs/libIDE-mock-sdk -I %t.tmp %s | %FileCheck %s -check-prefix=CHECK57
// CHECK57: source.lang.swift.decl.var.global (129:5-129:14)
// CHECK57: <decl.var.global><syntaxtype.keyword>let</syntaxtype.keyword> <decl.name>tupleVar1</decl.name>: <decl.var.type><tuple>(<tuple.element><tuple.element.type><tuple>(<tuple.element><tuple.element.type><tuple>(<tuple.element><tuple.element.type><ref.struct usr="s:Si">Int</ref.struct></tuple.element.type></tuple.element>, <tuple.element><tuple.element.type><ref.struct usr="s:Si">Int</ref.struct></tuple.element.type></tuple.element>)</tuple></tuple.element.type></tuple.element>, <tuple.element><tuple.element.argument_label>y</tuple.element.argument_label>: <tuple.element.type><ref.struct usr="s:Si">Int</ref.struct></tuple.element.type></tuple.element>)</tuple></tuple.element.type></tuple.element>, <tuple.element><tuple.element.argument_label>z</tuple.element.argument_label>: <tuple.element.type><ref.struct usr="s:Si">Int</ref.struct></tuple.element.type></tuple.element>)</tuple></decl.var.type></decl.var.global>

// RUN: %sourcekitd-test -req=cursor -pos=130:5 %s -- -F %S/../Inputs/libIDE-mock-sdk -I %t.tmp %s | %FileCheck %s -check-prefix=CHECK58
// CHECK58: source.lang.swift.decl.var.global (130:5-130:14)
// CHECK58: <decl.var.global><syntaxtype.keyword>let</syntaxtype.keyword> <decl.name>tupleVar2</decl.name>: <decl.var.type><tuple>(<tuple.element><tuple.element.argument_label>f</tuple.element.argument_label>: <tuple.element.type>() -&gt; <decl.function.returntype><tuple>()</tuple></decl.function.returntype></tuple.element.type></tuple.element>, <tuple.element><tuple.element.argument_label>g</tuple.element.argument_label>: <tuple.element.type>(<decl.var.parameter><decl.var.parameter.argument_label>_</decl.var.parameter.argument_label> <decl.var.parameter.name>x</decl.var.parameter.name>: <decl.var.parameter.type><ref.struct usr="s:Si">Int</ref.struct></decl.var.parameter.type></decl.var.parameter>) -&gt; <decl.function.returntype><ref.struct usr="s:Si">Int</ref.struct></decl.function.returntype></tuple.element.type></tuple.element>)</tuple></decl.var.type></decl.var.global>

// RUN: %sourcekitd-test -req=cursor -pos=131:5 %s -- -F %S/../Inputs/libIDE-mock-sdk -I %t.tmp %s | %FileCheck %s -check-prefix=CHECK59
// CHECK59: source.lang.swift.decl.var.global (131:5-131:14)
// CHECK59: <decl.var.global><syntaxtype.keyword>let</syntaxtype.keyword> <decl.name>tupleVar3</decl.name>: <decl.var.type><tuple>(<tuple.element><tuple.element.argument_label>f</tuple.element.argument_label>: <tuple.element.type>(<decl.var.parameter><decl.var.parameter.argument_label>_</decl.var.parameter.argument_label> <decl.var.parameter.name>x</decl.var.parameter.name>: <decl.var.parameter.type><syntaxtype.keyword>inout</syntaxtype.keyword> <tuple>(<tuple.element><tuple.element.type><ref.struct usr="s:Si">Int</ref.struct></tuple.element.type></tuple.element>, <tuple.element><tuple.element.type><ref.struct usr="s:Si">Int</ref.struct></tuple.element.type></tuple.element>)</tuple></decl.var.parameter.type></decl.var.parameter>) <syntaxtype.keyword>throws</syntaxtype.keyword> -&gt; <decl.function.returntype><tuple>()</tuple></decl.function.returntype></tuple.element.type></tuple.element>, <tuple.element><tuple.element.type><ref.struct usr="s:Si">Int</ref.struct></tuple.element.type></tuple.element>)</tuple></decl.var.type></decl.var.global>

// RUN: %sourcekitd-test -req=cursor -pos=134:8 %s -- -F %S/../Inputs/libIDE-mock-sdk -I %t.tmp %s | %FileCheck %s -check-prefix=CHECK60
// CHECK60: <decl.enumelement><syntaxtype.keyword>case</syntaxtype.keyword> <decl.name>A</decl.name> = <syntaxtype.number>-1</syntaxtype.number></decl.enumelement>
// RUN: %sourcekitd-test -req=cursor -pos=135:8 %s -- -F %S/../Inputs/libIDE-mock-sdk -I %t.tmp %s | %FileCheck %s -check-prefix=CHECK61
// CHECK61: <decl.enumelement><syntaxtype.keyword>case</syntaxtype.keyword> <decl.name>B</decl.name> = <syntaxtype.number>0</syntaxtype.number></decl.enumelement>
// RUN: %sourcekitd-test -req=cursor -pos=136:8 %s -- -F %S/../Inputs/libIDE-mock-sdk -I %t.tmp %s | %FileCheck %s -check-prefix=CHECK62
// CHECK62: <decl.enumelement><syntaxtype.keyword>case</syntaxtype.keyword> <decl.name>C</decl.name> = <syntaxtype.number>1</syntaxtype.number></decl.enumelement>

// RUN: %sourcekitd-test -req=cursor -pos=142:8 %s -- -F %S/../Inputs/libIDE-mock-sdk -I %t.tmp %s | %FileCheck %s -check-prefix=CHECK63
// CHECK63: <decl.enumelement><syntaxtype.keyword>case</syntaxtype.keyword> <decl.name>A</decl.name> = <syntaxtype.number>-0.0</syntaxtype.number></decl.enumelement>
// RUN: %sourcekitd-test -req=cursor -pos=143:8 %s -- -F %S/../Inputs/libIDE-mock-sdk -I %t.tmp %s | %FileCheck %s -check-prefix=CHECK64
// CHECK64: <decl.enumelement><syntaxtype.keyword>case</syntaxtype.keyword> <decl.name>B</decl.name> = <syntaxtype.number>1e10</syntaxtype.number></decl.enumelement>

// RUN: %sourcekitd-test -req=cursor -pos=146:7 %s -- -F %S/../Inputs/libIDE-mock-sdk -I %t.tmp %s | %FileCheck %s -check-prefix=CHECK65
// CHECK65: <decl.class><syntaxtype.keyword>class</syntaxtype.keyword> <decl.name>C6</decl.name> : C4, <ref.protocol usr="s:11cursor_info2P1P">P1</ref.protocol></decl.class>
// FIXME: ref.class - rdar://problem/25014968

// RUN: %sourcekitd-test -req=cursor -pos=150:10 %s -- -F %S/../Inputs/libIDE-mock-sdk -I %t.tmp %s | %FileCheck %s -check-prefix=CHECK66
// CHECK66: <decl.protocol><syntaxtype.keyword>protocol</syntaxtype.keyword> <decl.name>P2</decl.name> : AnyObject, <ref.protocol usr="s:11cursor_info2P1P">P1</ref.protocol></decl.protocol>

// RUN: %sourcekitd-test -req=cursor -pos=114:18 %s -- -F %S/../Inputs/libIDE-mock-sdk -I %t.tmp %s | %FileCheck %s -check-prefix=CHECK67
// CHECK67: source.lang.swift.decl.associatedtype (114:18-114:19)
// CHECK67-NEXT: T
// CHECK67-NEXT: s:11cursor_info2P1P1T
// CHECK67-NEXT: source.lang.swift
// CHECK67-NEXT: T.Type
// CHECK67: <Declaration>associatedtype T</Declaration>
// CHECK67-NEXT: <decl.associatedtype><syntaxtype.keyword>associatedtype</syntaxtype.keyword> <decl.name>T</decl.name></decl.associatedtype>

// RUN: %sourcekitd-test -req=cursor -pos=152:11 %s -- -F %S/../Inputs/libIDE-mock-sdk -I %t.tmp %s | %FileCheck %s -check-prefix=CHECK68
// CHECK68: source.lang.swift.decl.typealias (152:11-152:18)
// CHECK68-NEXT: MyAlias
// CHECK68-NEXT: s:11cursor_info7MyAliasa
// CHECK68-NEXT: source.lang.swift
// CHECK68-NEXT: (T, U, T, U).Type
// CHECK68: <Declaration>typealias MyAlias&lt;T, U&gt; = (<Type usr="s:11cursor_info7MyAliasa1Txmfp">T</Type>, <Type usr="s:11cursor_info7MyAliasa1Uq_mfp">U</Type>, <Type usr="s:11cursor_info7MyAliasa1Txmfp">T</Type>, <Type usr="s:11cursor_info7MyAliasa1Uq_mfp">U</Type>)</Declaration>
// CHECK68-NEXT: <decl.typealias><syntaxtype.keyword>typealias</syntaxtype.keyword> <decl.name>MyAlias</decl.name>&lt;<decl.generic_type_param usr="{{.*}}"><decl.generic_type_param.name>T</decl.generic_type_param.name></decl.generic_type_param>, <decl.generic_type_param usr="{{.*}}"><decl.generic_type_param.name>U</decl.generic_type_param.name></decl.generic_type_param>&gt; = <tuple>(<tuple.element><tuple.element.type><ref.generic_type_param usr="{{.*}}">T</ref.generic_type_param></tuple.element.type></tuple.element>, <tuple.element><tuple.element.type><ref.generic_type_param usr="{{.*}}">U</ref.generic_type_param></tuple.element.type></tuple.element>, <tuple.element><tuple.element.type><ref.generic_type_param usr="{{.*}}">T</ref.generic_type_param></tuple.element.type></tuple.element>, <tuple.element><tuple.element.type><ref.generic_type_param usr="{{.*}}">U</ref.generic_type_param></tuple.element.type></tuple.element>)</tuple></decl.typealias>

// RUN: %sourcekitd-test -req=cursor -pos=153:28 %s -- -F %S/../Inputs/libIDE-mock-sdk -I %t.tmp %s | %FileCheck %s -check-prefix=CHECK69
// CHECK69: source.lang.swift.ref.typealias (152:11-152:18)
// CHECK69-NEXT: MyAlias
// CHECK69-NEXT: s:11cursor_info7MyAliasa
// CHECK69-NEXT: source.lang.swift
// CHECK69-NEXT: (T, U, T, U).Type
// CHECK69: <Declaration>typealias MyAlias&lt;T, U&gt; = (<Type usr="s:11cursor_info7MyAliasa1Txmfp">T</Type>, <Type usr="s:11cursor_info7MyAliasa1Uq_mfp">U</Type>, <Type usr="s:11cursor_info7MyAliasa1Txmfp">T</Type>, <Type usr="s:11cursor_info7MyAliasa1Uq_mfp">U</Type>)</Declaration>
// CHECK69-NEXT: <decl.typealias><syntaxtype.keyword>typealias</syntaxtype.keyword> <decl.name>MyAlias</decl.name>&lt;<decl.generic_type_param usr="{{.*}}"><decl.generic_type_param.name>T</decl.generic_type_param.name></decl.generic_type_param>, <decl.generic_type_param usr="{{.*}}"><decl.generic_type_param.name>U</decl.generic_type_param.name></decl.generic_type_param>&gt; = <tuple>(<tuple.element><tuple.element.type><ref.generic_type_param usr="{{.*}}">T</ref.generic_type_param></tuple.element.type></tuple.element>, <tuple.element><tuple.element.type><ref.generic_type_param usr="{{.*}}">U</ref.generic_type_param></tuple.element.type></tuple.element>, <tuple.element><tuple.element.type><ref.generic_type_param usr="{{.*}}">T</ref.generic_type_param></tuple.element.type></tuple.element>, <tuple.element><tuple.element.type><ref.generic_type_param usr="{{.*}}">U</ref.generic_type_param></tuple.element.type></tuple.element>)</tuple></decl.typealias>

// RUN: %sourcekitd-test -req=cursor -pos=155:6 %s -- -F %S/../Inputs/libIDE-mock-sdk -I %t.tmp %s | %FileCheck %s -check-prefix=CHECK70
// CHECK70: <decl.function.free><syntaxtype.keyword>func</syntaxtype.keyword> <decl.name>paramAutoclosureNoescape1</decl.name>(<decl.var.parameter><decl.var.parameter.argument_label>_</decl.var.parameter.argument_label> <decl.var.parameter.name>msg</decl.var.parameter.name>: <decl.var.parameter.type>() -&gt; <decl.function.returntype><ref.struct usr="s:SS">String</ref.struct></decl.function.returntype></decl.var.parameter.type></decl.var.parameter>)</decl.function.free>

// RUN: %sourcekitd-test -req=cursor -pos=156:6 %s -- -F %S/../Inputs/libIDE-mock-sdk -I %t.tmp %s | %FileCheck %s -check-prefix=CHECK71
// CHECK71: <decl.function.free><syntaxtype.keyword>func</syntaxtype.keyword> <decl.name>paramAutoclosureNoescape2</decl.name>(<decl.var.parameter><decl.var.parameter.argument_label>_</decl.var.parameter.argument_label> <decl.var.parameter.name>msg</decl.var.parameter.name>: <decl.var.parameter.type><syntaxtype.attribute.builtin><syntaxtype.attribute.name>@autoclosure</syntaxtype.attribute.name></syntaxtype.attribute.builtin> () -&gt; <decl.function.returntype><ref.struct usr="s:SS">String</ref.struct></decl.function.returntype></decl.var.parameter.type></decl.var.parameter>)</decl.function.free>

// RUN: %sourcekitd-test -req=cursor -pos=157:6 %s -- -F %S/../Inputs/libIDE-mock-sdk -I %t.tmp %s | %FileCheck %s -check-prefix=CHECK72
// CHECK72: <decl.function.free><syntaxtype.keyword>func</syntaxtype.keyword> <decl.name>paramAutoclosureNoescape3</decl.name>(<decl.var.parameter><decl.var.parameter.argument_label>_</decl.var.parameter.argument_label> <decl.var.parameter.name>msg</decl.var.parameter.name>: <decl.var.parameter.type><syntaxtype.attribute.builtin><syntaxtype.attribute.name>@autoclosure</syntaxtype.attribute.name></syntaxtype.attribute.builtin> <syntaxtype.attribute.builtin><syntaxtype.attribute.name>@escaping</syntaxtype.attribute.name></syntaxtype.attribute.builtin> () -&gt; <decl.function.returntype><ref.struct usr="s:SS">String</ref.struct></decl.function.returntype></decl.var.parameter.type></decl.var.parameter>)</decl.function.free>

// RUN: %sourcekitd-test -req=cursor -pos=159:6 %s -- -F %S/../Inputs/libIDE-mock-sdk -I %t.tmp %s | %FileCheck %s -check-prefix=CHECK73
// CHECK73: <decl.function.free>
// CHECK73-SAME: = <syntaxtype.keyword>#function</syntaxtype.keyword>
// CHECK73-SAME: = <syntaxtype.keyword>#file</syntaxtype.keyword>
// CHECK73-SAME: = <syntaxtype.keyword>#line</syntaxtype.keyword>
// CHECK73-SAME: = <syntaxtype.keyword>#column</syntaxtype.keyword>
// CHECK73-SAME: = []
// CHECK73-SAME: = [:]
// FIXME: should be <syntaxtype.keyword>nil</syntaxtype.keyword>
// CHECK73-SAME: = nil
// CHECK73-SAME: = 1

// RUN: %sourcekitd-test -req=cursor -pos=162:8 %s -- -F %S/../Inputs/libIDE-mock-sdk -I %t.tmp %s | %FileCheck %s -check-prefix=CHECK74
// CHECK74: source.lang.swift.decl.function.method.instance (162:8-162:20)
// CHECK74: <Self where Self : P3> (Self) -> (Self) -> Self
// CHECK74: <Declaration>func f(_ s: <Type usr="s:11cursor_info2P3P4Selfxmfp">Self</Type>) -&gt; <Type usr="s:11cursor_info2P3P4Selfxmfp">Self</Type></Declaration>
// CHECK74: <decl.var.parameter.type><ref.generic_type_param usr="s:11cursor_info2P3P4Selfxmfp">Self</ref.generic_type_param></decl.var.parameter.type>
// CHECK74-SAME: <decl.function.returntype><ref.generic_type_param usr="s:11cursor_info2P3P4Selfxmfp">Self</ref.generic_type_param></decl.function.returntype>

// RUN: %sourcekitd-test -req=cursor -pos=165:8 %s -- -F %S/../Inputs/libIDE-mock-sdk -I %t.tmp %s | %FileCheck %s -check-prefix=CHECK75
// CHECK75: source.lang.swift.decl.function.method.instance (165:8-165:20)
// CHECK75: <Self where Self : P3> (Self) -> (Self) -> Self
// CHECK75: <Declaration>func f(_ s: <Type usr="s:11cursor_info2P3PAAE4Selfxmfp">Self</Type>) -&gt; <Type usr="s:11cursor_info2P3PAAE4Selfxmfp">Self</Type></Declaration>
// CHECK75: <decl.var.parameter.type><ref.generic_type_param usr="s:11cursor_info2P3PAAE4Selfxmfp">Self</ref.generic_type_param></decl.var.parameter.type>
// CHECK75-SAME: <decl.function.returntype><ref.generic_type_param usr="s:11cursor_info2P3PAAE4Selfxmfp">Self</ref.generic_type_param></decl.function.returntype>

// RUN: %sourcekitd-test -req=cursor -pos=169:8 %s -- -F %S/../Inputs/libIDE-mock-sdk -I %t.tmp %s | %FileCheck %s -check-prefix=CHECK76
// CHECK76: source.lang.swift.decl.function.method.instance (169:8-169:11)
// CHECK76: (C7) -> () -> Self
// CHECK76: <Declaration>func f() -&gt; <Type usr="s:11cursor_info2C7C">Self</Type></Declaration>
// CHECK76: <decl.function.returntype><ref.class usr="s:11cursor_info2C7C">Self</ref.class></decl.function.returntype>

// RUN: %sourcekitd-test -req=cursor -pos=188:7 %s -- -F %S/../Inputs/libIDE-mock-sdk -I %t.tmp %s | %FileCheck -check-prefix=CHECK77 %s
// RUN: %sourcekitd-test -req=cursor -pos=189:7 %s -- -F %S/../Inputs/libIDE-mock-sdk -I %t.tmp %s | %FileCheck -check-prefix=CHECK78 %s

// CHECK77: foo1 comment from P4
// CHECK78: foo2 comment from C1

// RUN: %sourcekitd-test -req=cursor -pos=192:6 %s -- -F %S/../Inputs/libIDE-mock-sdk -I %t.tmp %s | %FileCheck -check-prefix=CHECK79 %s
// CHECK79: <decl.var.parameter><decl.var.parameter.argument_label>t</decl.var.parameter.argument_label>:
// CHECK79-SAME: <decl.var.parameter.type><tuple>(
// CHECK79-SAME:   <tuple.element><tuple.element.type><ref.struct usr="s:Si">Int</ref.struct></tuple.element.type></tuple.element>,
// CHECK79-SAME:   <tuple.element><tuple.element.type><ref.struct usr="s:Si">Int</ref.struct></tuple.element.type></tuple.element>
// CHECK79-SAME: )</tuple></decl.var.parameter.type></decl.var.parameter>

// RUN: %sourcekitd-test -req=cursor -pos=193:6 %s -- -F %S/../Inputs/libIDE-mock-sdk -I %t.tmp %s | %FileCheck -check-prefix=CHECK80 %s
// CHECK80: <decl.var.parameter.type><tuple>()</tuple>

// RUN: %sourcekitd-test -req=cursor -pos=194:6 %s -- -F %S/../Inputs/libIDE-mock-sdk -I %t.tmp %s | %FileCheck -check-prefix=CHECK81 %s
// CHECK81: <decl.var.parameter.type>() -&gt; <decl.function.returntype><tuple>()</tuple></decl.function.returntype></decl.var.parameter.type>

// RUN: %sourcekitd-test -req=cursor -pos=195:6 %s -- -F %S/../Inputs/libIDE-mock-sdk -I %t.tmp %s | %FileCheck -check-prefix=CHECK82 %s
// CHECK82: <decl.function.returntype><tuple>(<tuple.element><tuple.element.type><ref.struct usr="s:Si">Int</ref.struct></tuple.element.type></tuple.element>, <tuple.element><tuple.element.type><ref.struct usr="s:Si">Int</ref.struct></tuple.element.type></tuple.element>)</tuple>

// RUN: %sourcekitd-test -req=cursor -pos=196:6 %s -- -F %S/../Inputs/libIDE-mock-sdk -I %t.tmp %s | %FileCheck -check-prefix=CHECK83 %s
// CHECK83: <decl.var.parameter.type>() -&gt; <decl.function.returntype><ref.typealias usr="s:s4Voida">Void</ref.typealias>

// RUN: %sourcekitd-test -req=cursor -pos=197:11 %s -- -F %S/../Inputs/libIDE-mock-sdk -I %t.tmp %s | %FileCheck -check-prefix=CHECK84 %s
// CHECK84: <decl.typealias><syntaxtype.keyword>typealias</syntaxtype.keyword> <decl.name>MyVoid</decl.name> = <tuple>()</tuple></decl.typealias>

// RUN: %sourcekitd-test -req=cursor -pos=199:6 %s -- -F %S/../Inputs/libIDE-mock-sdk -I %t.tmp %s | %FileCheck -check-prefix=CHECK85 %s
// CHECK85-NOT: @rethrows
// CHECK85: <Declaration>func rethrowingFunction1({{.*}}) rethrows</Declaration>
// CHECK85-NOT: @rethrows
// CHECK85: <decl.function.free><syntaxtype.keyword>func</syntaxtype.keyword> <decl.name>rethrowingFunction1</decl.name>({{.*}}) <syntaxtype.keyword>rethrows</syntaxtype.keyword></decl.function.free>
// CHECK85-NOT: @rethrows

// RUN: %sourcekitd-test -req=cursor -pos=201:6 %s -- -F %S/../Inputs/libIDE-mock-sdk -I %t.tmp %s | %FileCheck -check-prefix=CHECK86 %s
// RUN: %sourcekitd-test -req=cursor -pos=202:6 %s -- -F %S/../Inputs/libIDE-mock-sdk -I %t.tmp %s | %FileCheck -check-prefix=CHECK86 %s
// RUN: %sourcekitd-test -req=cursor -pos=203:6 %s -- -F %S/../Inputs/libIDE-mock-sdk -I %t.tmp %s | %FileCheck -check-prefix=CHECK86 %s
// RUN: %sourcekitd-test -req=cursor -pos=204:6 %s -- -F %S/../Inputs/libIDE-mock-sdk -I %t.tmp %s | %FileCheck -check-prefix=CHECK86 %s
// RUN: %sourcekitd-test -req=cursor -pos=205:6 %s -- -F %S/../Inputs/libIDE-mock-sdk -I %t.tmp %s | %FileCheck -check-prefix=CHECK86 %s
// RUN: %sourcekitd-test -req=cursor -pos=206:6 %s -- -F %S/../Inputs/libIDE-mock-sdk -I %t.tmp %s | %FileCheck -check-prefix=CHECK86 %s
// RUN: %sourcekitd-test -req=cursor -pos=207:6 %s -- -F %S/../Inputs/libIDE-mock-sdk -I %t.tmp %s | %FileCheck -check-prefix=CHECK86 %s
// RUN: %sourcekitd-test -req=cursor -pos=208:6 %s -- -F %S/../Inputs/libIDE-mock-sdk -I %t.tmp %s | %FileCheck -check-prefix=CHECK86 %s
// CHECK86: <syntaxtype.attribute.builtin><syntaxtype.attribute.name>@convention</syntaxtype.attribute.name>({{[a-z_]*((, [a-zA-Z_]*: &quot;[^&]*&quot;)|(: [a-zA-Z0-9_]*))?}})</syntaxtype.attribute.builtin>

// RUN: %sourcekitd-test -req=cursor -pos=213:8 %s -- -F %S/../Inputs/libIDE-mock-sdk -I %t.tmp %s | %FileCheck -check-prefix=CHECK87 %s
// CHECK87:      source.lang.swift.decl.struct (213:8-213:26)
// CHECK87-NEXT: HasLocalizationKey
// CHECK87-NEXT: s:11cursor_info18HasLocalizationKeyV
// CHECK87-NEXT: source.lang.swift
// CHECK87-NEXT: HasLocalizationKey.Type
// CHECK87-NEXT: $s
// CHECK87-NEXT: cursor_info{{$}}
// CHECK87-NEXT: <Declaration>struct HasLocalizationKey</Declaration>
// CHECK87-NEXT: <decl.struct><syntaxtype.keyword>struct</syntaxtype.keyword> <decl.name>HasLocalizationKey</decl.name></decl.struct>
// CHECK87-NEXT: <Class file="{{[^"]+}}cursor_info.swift" line="213" column="8"><Name>HasLocalizationKey</Name><USR>s:11cursor_info18HasLocalizationKeyV</USR><Declaration>struct HasLocalizationKey</Declaration><CommentParts><Abstract><Para>Brief.</Para></Abstract></CommentParts></Class>
// CHECK87-NEXT: <LocalizationKey>ABC</LocalizationKey>

// RUN: %sourcekitd-test -req=cursor -pos=216:6 %s -- -F %S/../Inputs/libIDE-mock-sdk -I %t.tmp %s | %FileCheck -check-prefix=CHECK88 %s
// CHECK88:      source.lang.swift.decl.function.free (216:6-216:27)
// CHECK88-NEXT: hasLocalizationKey2
// CHECK88-NEXT: s:11cursor_info19hasLocalizationKey2yyF
// CHECK88-NEXT: source.lang.swift
// CHECK88-NEXT: () -> ()
// CHECK88-NEXT: $s
// CHECK88-NEXT: cursor_info{{$}}
// CHECK88-NEXT: <Declaration>func hasLocalizationKey2()</Declaration>
// CHECK88-NEXT: <decl.function.free><syntaxtype.keyword>func</syntaxtype.keyword> <decl.name>hasLocalizationKey2</decl.name>()</decl.function.free>
// CHECK88-NEXT: <Function file="{{[^"]+}}cursor_info.swift" line="216" column="6"><Name>hasLocalizationKey2()</Name><USR>s:11cursor_info19hasLocalizationKey2yyF</USR><Declaration>func hasLocalizationKey2()</Declaration><CommentParts></CommentParts></Function>
// CHECK88-NEXT: <LocalizationKey>ABC</LocalizationKey>

// RUN: %sourcekitd-test -req=cursor -pos=219:6 %s -- -F %S/../Inputs/libIDE-mock-sdk -I %t.tmp %s | %FileCheck -check-prefix=CHECK89 %s
// CHECK89: <Declaration>func funcWithNestedEscaping(a: (@escaping () -&gt; ()) -&gt; ())</Declaration>
// CHECK89-NEXT: <decl.var.parameter.type>(<decl.var.parameter><decl.var.parameter.type><syntaxtype.attribute.builtin><syntaxtype.attribute.name>@escaping</syntaxtype.attribute.name></syntaxtype.attribute.builtin> () -&gt; <decl.function.returntype><tuple>()</tuple></decl.function.returntype></decl.var.parameter.type></decl.var.parameter>) -&gt; <decl.function.returntype><tuple>()</tuple></decl.function.returntype></decl.var.parameter.type>

// RUN: %sourcekitd-test -req=cursor -pos=222:11 %s -- -F %S/../Inputs/libIDE-mock-sdk -I %t.tmp %s | %FileCheck -check-prefix=CHECK90 %s
// CHECK90: <Declaration>typealias typeWithNestedAutoclosure = (@autoclosure () -&gt; ()) -&gt; ()</Declaration>
// CHECK90-NEXT: <decl.var.parameter.type><syntaxtype.attribute.builtin><syntaxtype.attribute.name>@autoclosure</syntaxtype.attribute.name></syntaxtype.attribute.builtin> () -&gt; <decl.function.returntype><tuple>()</tuple></decl.function.returntype></decl.var.parameter.type>

// RUN: %sourcekitd-test -req=cursor -pos=224:11 %s -- -F %S/../Inputs/libIDE-mock-sdk -I %t.tmp %s | %FileCheck -check-prefix=CHECK91 %s
// CHECK91: <Declaration>typealias GenericAlias&lt;T, U&gt; = <Type usr="s:11cursor_info7MyAliasa">MyAlias</Type>&lt;<Type usr="s:11cursor_info12GenericAliasa1Txmfp">T</Type>, <Type usr="s:11cursor_info12GenericAliasa1Uq_mfp">U</Type>&gt; where <Type usr="s:11cursor_info12GenericAliasa1Txmfp">T</Type> : <Type usr="s:11cursor_info2P1P">P1</Type></Declaration>
// CHECK91-NEXT: <decl.typealias><syntaxtype.keyword>typealias</syntaxtype.keyword> <decl.name>GenericAlias</decl.name>&lt;<decl.generic_type_param usr="s:11cursor_info12GenericAliasa1Txmfp"><decl.generic_type_param.name>T</decl.generic_type_param.name></decl.generic_type_param>, <decl.generic_type_param usr="s:11cursor_info12GenericAliasa1Uq_mfp"><decl.generic_type_param.name>U</decl.generic_type_param.name></decl.generic_type_param>&gt; = <ref.typealias usr="s:11cursor_info7MyAliasa">MyAlias</ref.typealias>&lt;<ref.generic_type_param usr="s:11cursor_info12GenericAliasa1Txmfp">T</ref.generic_type_param>, <ref.generic_type_param usr="s:11cursor_info12GenericAliasa1Uq_mfp">U</ref.generic_type_param>&gt; <syntaxtype.keyword>where</syntaxtype.keyword> <decl.generic_type_requirement><ref.generic_type_param usr="s:11cursor_info12GenericAliasa1Txmfp">T</ref.generic_type_param> : <ref.protocol usr="s:11cursor_info2P1P">P1</ref.protocol></decl.generic_type_requirement></decl.typealias>

// RUN: %sourcekitd-test -req=cursor -pos=227:10 %s -- -F %S/../Inputs/libIDE-mock-sdk -I %t.tmp %s | %FileCheck -check-prefix=CHECK92 %s
// CHECK92: <Declaration>case a = &quot;\u{1B}&quot;</Declaration>
// CHECK92-NEXT: <decl.enumelement><syntaxtype.keyword>case</syntaxtype.keyword> <decl.name>a</decl.name> = <syntaxtype.string>&quot;\u{1B}&quot;</syntaxtype.string></decl.enumelement>

// RUN: %sourcekitd-test -req=cursor -pos=228:10 %s -- -F %S/../Inputs/libIDE-mock-sdk -I %t.tmp %s | %FileCheck -check-prefix=CHECK93 %s
// CHECK93: <Declaration>case b = &quot;f&quot;</Declaration>
// CHECK93-NEXT: <decl.enumelement><syntaxtype.keyword>case</syntaxtype.keyword> <decl.name>b</decl.name> = <syntaxtype.string>&quot;f&quot;</syntaxtype.string></decl.enumelement>

// RUN: %sourcekitd-test -req=cursor -pos=227:14 %s -- -F %S/../Inputs/libIDE-mock-sdk -I %t.tmp %s | %FileCheck -check-prefix=CHECK94 %s
// CHECK94: source.lang.swift.ref.struct
// CHECK94-NEXT: String
// CHECK94-NEXT: s:SS
// CHECK94-NEXT: source.lang.swift
// CHECK94-NEXT: String.Type
// CHECK94-NEXT: $sSSmD
// CHECK94-NEXT: Swift
// CHECK94-NEXT: <Group>String</Group>
// CHECK94-NEXT: SYSTEM
// CHECK94-NEXT: <Declaration>@frozen @_eagerMove struct String</Declaration>

// RUN:  %sourcekitd-test -req=cursor -pos=231:6 %s -- -F %S/../Inputs/libIDE-mock-sdk -I %t.tmp %s | %FileCheck -check-prefix=CHECK95 %s
// CHECK95: <Declaration>func checkAnyIsAKeyword(x: Any)</Declaration>
// CHECK95-NEXT: <decl.function.free><syntaxtype.keyword>func</syntaxtype.keyword> <decl.name>checkAnyIsAKeyword</decl.name>(<decl.var.parameter><decl.var.parameter.argument_label>x</decl.var.parameter.argument_label>: <decl.var.parameter.type><syntaxtype.keyword>Any</syntaxtype.keyword></decl.var.parameter.type></decl.var.parameter>)</decl.function.free>

// RUN: %sourcekitd-test -req=cursor -pos=23:23 %s -- -F %S/../Inputs/libIDE-mock-sdk -I %t.tmp %s | %FileCheck -check-prefix=CHECK96 %s
// CHECK96: source.lang.swift.ref.struct
// CHECK96-NEXT: String
// CHECK96-NEXT: s:SS
// CHECK96-NEXT: source.lang.swift
// CHECK96-NEXT: String.Type
// CHECK96-NEXT: $sSSmD
// CHECK96-NEXT: Swift
// CHECK96-NEXT: <Group>String</Group>
// CHECK96-NEXT: SYSTEM
// CHECK96-NEXT: <Declaration>@frozen @_eagerMove struct String</Declaration>

// RUN: %sourcekitd-test -req=cursor -pos=233:19 %s -- -F %S/../Inputs/libIDE-mock-sdk -I %t.tmp %s | %FileCheck -check-prefix=CHECK97 %s
// CHECK97: source.lang.swift.ref.struct
// CHECK97-NEXT: Int
// CHECK97-NEXT: s:Si
// CHECK97-NEXT: source.lang.swift
// CHECK97-NEXT: Int.Type
// CHECK97-NEXT: $sSimD
// CHECK97-NEXT: Swift
// CHECK97-NEXT: <Group>Math/Integers</Group>
// CHECK97-NEXT: SYSTEM

// RUN: %sourcekitd-test -req=cursor -pos=241:12 %s -- -F %S/../Inputs/libIDE-mock-sdk -I %t.tmp %s | %FileCheck -check-prefix=CHECK98 %s
// CHECK98: source.lang.swift.ref.function.constructor (236:5-236:36)
// CHECK98-NEXT: init(integerLiteral:)
// CHECK98-NEXT: s:11cursor_info1AV14integerLiteralACSi_tcfc
// CHECK98-NEXT: source.lang.swift
// CHECK98-NEXT: (A.Type) -> (Int) -> A
// CHECK98-NEXT: $s14integerLiteral11cursor_info1AVSi_tcD
// CHECK98-NEXT: cursor_info

// RUN: %sourcekitd-test -req=cursor -pos=244:51 %s -- -F %S/../Inputs/libIDE-mock-sdk -I %t.tmp %s | %FileCheck -check-prefix=CHECK99 %s
// CHECK99: source.lang.swift.ref.struct ()
// CHECK99-NEXT: String
// CHECK99-NEXT: s:SS
// CHECK99-NEXT: source.lang.swift
// CHECK99-NEXT: String.Type
// CHECK99-NEXT: $sSSmD
// CHECK99-NEXT: Swift
// CHECK99-NEXT: <Group>String</Group>
// CHECK99-NEXT: SYSTEM

// RUN: %sourcekitd-test -req=cursor -pos=244:61 %s -- -F %S/../Inputs/libIDE-mock-sdk -I %t.tmp %s | %FileCheck -check-prefix=CHECK100 %s
// CHECK100: source.lang.swift.ref.function.constructor ()
// CHECK100-NEXT: init(stringInterpolation:)
// CHECK100-NEXT: s:SS19stringInterpolationSSs013DefaultStringB0V_tcfc
// CHECK100-NEXT: source.lang.swift
// CHECK100-NEXT: (String.Type) -> (DefaultStringInterpolation) -> String
// CHECK100-NEXT: $s19stringInterpolationSSs013DefaultStringB0V_tcD
// CHECK100-NEXT: Swift
// CHECK100-NEXT: <Group>String</Group>
// CHECK100-NEXT: SYSTEM
