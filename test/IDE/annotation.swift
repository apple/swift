// RUN: %target-swift-ide-test -annotate -source-filename %s | %FileCheck %s

// CHECK: import struct <iMod>Swift</iMod>.<iStruct@>Int</iStruct>
import struct Swift.Int
// CHECK: import func <iMod>Swift</iMod>.print_new
import func Swift.print_new

// CHECK: struct <Struct>S</Struct> {
// CHECK-NEXT:   var <Var>x</Var>: <iStruct@>Int</iStruct> = 0
// CHECK-NEXT:   var <Var>y</Var>: <iMod>Swift</iMod>.<iStruct@>Int</iStruct> = 0
// CHECK-NEXT: }
struct S {
  var x: Int = 0
  var y: Swift.Int = 0
}

// CHECK: typealias <TypeAlias>TypealiasForS</TypeAlias> = <Struct@[[@LINE-5]]:8>S</Struct>
typealias TypealiasForS = S

func test6(p: S) {
  // CHECK: <Param@[[@LINE-1]]:12>p</Param>.<Var@[[@LINE-8]]:7>x</Var> <iFunc@>+</iFunc> 0
  p.x + 0
}

// CHECK: struct <Struct>PropagatedTypesInPatterns</Struct> {
// CHECK-NEXT:   var <Var>a</Var>, <Var>b</Var>: <iStruct@>Int</iStruct>
// CHECK-NEXT:   var <Var>c</Var>: <iStruct@>Int</iStruct>, <Var>d</Var>, <Var>e</Var>: <iStruct@>Double</iStruct>
// CHECK-NEXT:   var <Var>f</Var>, <Var>g</Var>, <Var>h</Var>: <iStruct@>Int</iStruct>, <Var>i</Var>: <iStruct@>Float</iStruct>, <Var>j</Var>, <Var>k</Var>, <Var>l</Var>, <Var>m</Var>: <iStruct@>Double</iStruct>
// CHECK-NEXT:   var (<Var>x</Var>, <Var>y</Var>): (<iStruct@>Int</iStruct>, <iStruct@>Float</iStruct>)
// CHECK-NEXT: }
struct PropagatedTypesInPatterns {
  var a, b: Int
  var c: Int, d, e: Double
  var f, g, h: Int, i: Float, j, k, l, m: Double
  var (x, y): (Int, Float)
}

// CHECK: class <Class>MyCls</Class> {
// CHECK:   var <Var>www</Var> : <iStruct@>Int</iStruct>
// CHECK:   func <Func>foo</Func>(_ <Param>x</Param> : <iStruct@>Int</iStruct>) {}
// CHECK: }
class MyCls {
  var www : Int = 0
  func foo(_ x : Int) {}
}

// CHECK: func <Func>foo</Func>(<Param>n</Param> : <iStruct@>Float</iStruct>) -> <iStruct@>Int</iStruct> {
// CHECK:   var <Var>q</Var> = <Ctor@[[@LINE-6]]:7-Class@[[@LINE-6]]:7>MyCls</Ctor>()
// CHECK:   var <Var>ee</Var> = "yoo";
// CHECK:   return 100009
// CHECK: }
func foo(n : Float) -> Int {
  var q = MyCls()
  var ee = "yoo";
  return 100009
}

// CHECK-LABEL: protocol <Protocol>Prot</Protocol> {
// CHECK-NEXT:   associatedtype <AssociatedType>Blarg</AssociatedType>
// CHECK-NEXT:   func <Func>protMeth</Func>(_ <Param>x</Param>: <iStruct@>Int</iStruct>)
// CHECK-NEXT:   var <Var>protocolProperty1</Var>: <iStruct@>Int</iStruct> { get }
// CHECK-NEXT:   var <Var>protocolProperty2</Var>: <iStruct@>Int</iStruct> { get set }
// CHECK-NEXT: }
protocol Prot {
  associatedtype Blarg
  func protMeth(_ x: Int)
  var protocolProperty1: Int { get }
  var protocolProperty2: Int { get set }
}
// CHECK: protocol <Protocol>Prot2</Protocol> {}
protocol Prot2 {}

// CHECK: class <Class>SubCls</Class> : <Class@[[@LINE-31]]:7>MyCls</Class>, <Protocol@[[@LINE-9]]:10>Prot</Protocol> {
// CHECK:   typealias <TypeAlias>Blarg</TypeAlias> = <Protocol@[[@LINE-3]]:10>Prot2</Protocol>
// CHECK:   func <Func>protMeth</Func>(_ <Param>x</Param>: <iStruct@>Int</iStruct>) {}
// CHECK: }
class SubCls : MyCls, Prot {
  typealias Blarg = Prot2
  func protMeth(_ x: Int) {}
  var protocolProperty1 = 0
  var protocolProperty2 = 0
}

// CHECK: func <Func>genFn</Func><<GenericTypeParam>T</GenericTypeParam> : <Protocol@64:10>Prot</Protocol>>(_ <Param>p</Param> : <GenericTypeParam@85:12>T</GenericTypeParam>) -> <iStruct@>Int</iStruct> where <GenericTypeParam@85:12>T</GenericTypeParam>.<AssociatedType@65:18>Blarg</AssociatedType> : <Protocol@71:10>Prot2</Protocol> {}{{$}}
func genFn<T : Prot>(_ p : T) -> Int where T.Blarg : Prot2 {}

func test(_ x: Int) {
  // CHECK: <Func@[[@LINE-3]]:6>genFn</Func>(<Class@[[@LINE-11]]:7>SubCls</Class>())
  genFn(SubCls())
  // CHECK: "This is string \(<Func@[[@LINE-5]]:6>genFn</Func>({(<Param>a</Param>:<iStruct@>Int</iStruct>) in <Class@[[@LINE-13]]:7>SubCls</Class>()}(<Param@[[@LINE-3]]:13>x</Param>))) interpolation"
  "This is string \(genFn({(a:Int) in SubCls()}(x))) interpolation"
}

// CHECK: func <Func>bar</Func>(<Param>x</Param>: <iStruct@>Int</iStruct>) -> (<iStruct@>Int</iStruct>, <iStruct@>Float</iStruct>) {
func bar(x: Int) -> (Int, Float) {
  // CHECK: <Ctor@[[@LINE-84]]:8-TypeAlias@[[@LINE-78]]:11>TypealiasForS</Ctor>()
  _ = TypealiasForS()
}

class C2 {
  typealias WW = Int
  var p = 0

  func meth(_ x: Int) {}
}

func test2(x: C2) {
  // CHECK: <Param@[[@LINE-1]]:12>x</Param>.<Func@[[@LINE-4]]:8>meth</Func>(0)
  x.meth(0)
}

// CHECK: class <Class>GenCls</Class><<GenericTypeParam>T</GenericTypeParam>> {
class GenCls<T> {
  // CHECK: <Constructor>init</Constructor>() {}
  init() {}

  // CHECK: <Subscript>subscript</Subscript> (<Param>i</Param> : <iStruct@>Int</iStruct>, <Param>j</Param> : <iStruct@>Int</iStruct>) -> <iStruct@>Int</iStruct> {
  subscript (i : Int, j : Int) -> Int {
    get {
      // CHECK: return <Param@[[@LINE-2]]:14>i</Param> <iFunc@>+</iFunc> <Param@[[@LINE-2]]:23>j</Param>
      return i + j
    }
    // CHECK: set(<Param>v</Param>) {
    set(v) {
      // CHECK: <Param@[[@LINE-1]]:9>v</Param> <iFunc@>+</iFunc> <Param@[[@LINE-7]]:14>i</Param> <iFunc@>-</iFunc> <Param@[[@LINE-7]]:23>j</Param>
      v + i - j
    }
  }
}

func test2() {
  // CHECK: <Ctor@[[@LINE-17]]:3-Class@[[@LINE-19]]:7>GenCls</Ctor><<iStruct@>Int</iStruct>>()
  GenCls<Int>()
}

func test3(name: Int, x: Int) {
  // CHECK: <Param@[[@LINE-1]]:23>x</Param> = 0
  name = 0;  x = 0
}

class C4 {
  class In {
    class func goo() {}
    class func foo() {
      // CHECK: <Class@[[@LINE-3]]:9>In</Class>.<Func@[[@LINE-2]]:16>goo</Func>()
      In.goo()
      // CHECK: <Class@[[@LINE-6]]:7>C4</Class>.<Class@[[@LINE-5]]:9>In</Class>.<Func@[[@LINE-4]]:16>goo</Func>()
      C4.In.goo()
      // CHECK: <Class@[[@LINE-8]]:7>C4</Class>.<Ctor@[[@LINE-7]]:9-Class@[[@LINE-7]]:9>In</Ctor>()
      C4.In()
    }
  }
}

class GenC<T1,T2> {
  class func foo() {}
}
func test4() {
  // CHECK: <Class@[[@LINE-4]]:7>GenC</Class><<iStruct@>Int</iStruct>, <TypeAlias@[[@LINE-141]]:11>TypealiasForS</TypeAlias>>.<Func@[[@LINE-3]]:14>foo</Func>()
  GenC<Int, TypealiasForS>.foo()
}

class C5 {}
protocol P5 {}
// CHECK: extension <Class@[[@LINE-2]]:7>C5</Class> : <Protocol@[[@LINE-1]]:10>P5</Protocol> {}
extension C5 : P5 {}

extension Array : P5 {}
// CHECK: extension <iStruct@>Array</iStruct> : <Protocol{{.*}}>P5</Protocol> {}
extension Optional : P5 {}
// CHECK: extension <iEnum@>Optional</iEnum> : <Protocol{{.*}}>P5</Protocol> {}

class C6 {
  func meth() {
    // CHECK: <Func@[[@LINE+5]]:8>meth2</Func>()
    meth2()
    // CHECK: <Param@[[@LINE-3]]:8>self</Param>.<Func@[[@LINE+3]]:8>meth2</Func>()
    self.meth2();
  }
  func meth2() {}
}

func test5() {
  var x: Int
  // CHECK: for <Var>i</Var> in 1<iFunc@>..<</iFunc>100 {
  for i in 1..<100 {
    // CHECK: <Var@[[@LINE-3]]:7>x</Var> = <Var@[[@LINE-1]]:7>i</Var>
    x = i
  }
}

class C7 {
  var c7ivar: Int
  func meth(_ p: Undeclared) {
    // CHECK: <Var@[[@LINE-2]]:7>c7ivar</Var> = 0
    c7ivar = 0
  }
}

class C8 {
  init(_ x: Int) {
    // CHECK: <Param@[[@LINE-1]]:3>self</Param>.<Ctor@[[@LINE-1]]:3>init</Ctor>(1)
    self.init(1)
  }
}

class SubC8 : C8 {
  init(x: Int) {
    // CHECK: super.<Ctor@[[@LINE-8]]:3>init</Ctor>(2)
    super.init(2)
  }
}

class Observers {
  func doit() {}

  var p1 : Int {
    // CHECK: willSet(<Param>newValue</Param>) { <Func@[[@LINE-3]]:8>doit</Func>() }
    willSet(newValue) { doit() }
    // CHECK: didSet { <Func@[[@LINE-5]]:8>doit</Func>() }
    didSet { doit() }
  }
}

class C9 {}
// CHECK: func <Func>test6</Func>(_ <Param>o</Param>: <iTypeAlias@>AnyObject</iTypeAlias>) {
func test6(_ o: AnyObject) {
  // CHECK: let <Var>x</Var> = <Param@[[@LINE-1]]:14>o</Param> as! <Class@[[@LINE-3]]:7>C9</Class>
  let x = o as! C9
}

// CHECK: enum <Enum>myCoolEnum</Enum> {
enum myCoolEnum {
  // CHECK: case <EnumElement>water</EnumElement>
  case water
  // CHECK-NEXT: case
  case
}

// rdar://19614869, do not crash
class E {
  lazy var u: T {
    return 1
  }
}

class C10 {
  init(int: Int, andThis: Float) {}
  func meth(_ x: Int, withFloat: Float) {}
}

// CHECK: var <Var>c10</Var> = <Ctor@[[@LINE-4]]:3-Class@[[@LINE-5]]:7>C10</Ctor>(<Ctor@[[@LINE-4]]:3#int>int</Ctor>: 0, <Ctor@[[@LINE-4]]:3#andThis>andThis</Ctor>: 0)
var c10 = C10(int: 0, andThis: 0)
// CHECK: <Var@[[@LINE-1]]:5>c10</Var>.<Func@[[@LINE-5]]:8>meth</Func>(0, <Func@[[@LINE-5]]:8#withFloat>withFloat</Func>: 0)
c10.meth(0, withFloat: 0)

func test7(int x: Int, andThis y: Float) {}
// CHECK: <Func@[[@LINE-1]]:6>test7</Func>(<Func@[[@LINE-1]]:6#int>int</Func>: 0, <Func@[[@LINE-1]]:6#andThis>andThis</Func>: 0)
test7(int: 0, andThis: 0)

func test8<T : Prot2>(_ x: T) {}
// CHECK: func <Func>test8</Func><<GenericTypeParam>T</GenericTypeParam> : <Protocol@71:10>Prot2</Protocol>>(_ <Param>x</Param>: <GenericTypeParam@261:12>T</GenericTypeParam>) {}{{$}}

class C11 {
  // CHECK: var <Var>a</Var>: <iStruct@>Int</iStruct> = { var <Var>tmp</Var> = 0; return <Var@[[@LINE+1]]:22>tmp</Var> }()
  var a: Int = { var tmp = 0; return tmp }()
  // CHECK: lazy var <Var>b</Var>: <iStruct@>Int</iStruct> = { var <Var>tmp</Var> = 0; return <Var@[[@LINE+1]]:27>tmp</Var> }()
  lazy var b: Int = { var tmp = 0; return tmp }()
  // CHECK: var <Var>c</Var>: <iStruct@>Int</iStruct> { get { {var <Var>tmp</Var> = 0; return <Var@[[@LINE+1]]:27>tmp</Var>}() } }
  var c: Int { get { {var tmp = 0; return tmp}() } }
}

// CHECK: var <Var>g1</Var> = { (<Param>x</Param>: <iStruct@>Int</iStruct>) -> <iStruct@>Int</iStruct> in return 0 }
var g1 = { (x: Int) -> Int in return 0 }

class C12 {
  class Inn {}
  typealias AliasInn = Inn
}
typealias AliasC12 = C12

// CHECK: extension <Class@[[@LINE-6]]:7>C12</Class>.<Class@[[@LINE-5]]:9>Inn</Class> {}
extension C12.Inn {}
// CHECK: extension <TypeAlias@[[@LINE-4]]:11>AliasC12</TypeAlias>.<TypeAlias@[[@LINE-6]]:13>AliasInn</TypeAlias> {}
extension AliasC12.AliasInn {}

typealias AliasPH = C12
func testPH(_ x: Int) {}
func testPH(_ x: AliasPH) {}
// CHECK: <Func@[[@LINE-1]]:6>testPH</Func>(<#T##x: AliasPH##AliasPH##C12#>)
testPH(<#T##x: AliasPH##AliasPH##C12#>)

class NumberTooLarge {}
// CHECK: guard case let <Var>error</Var> as <Class@[[@LINE-1]]:7>NumberTooLarge</Class> = <Ctor@[[@LINE-1]]:7-Class@[[@LINE-1]]:7>NumberTooLarge</Ctor>() else {}
guard case let error as NumberTooLarge = NumberTooLarge() else {}

// CHECK: <Func>testMod</Func>(_ <Param>x</Param>: <iMod>Swift</iMod>.<iStruct@>String</iStruct>) {
func testMod(_ x: Swift.String) {
// CHECK: let <Var>x</Var> = <iMod>Swift</iMod>
  let x = Swift
}

func +(a: C12, b: C12) {}
func test9(a: C12, b: C12) {
  // CHECK: <Param@[[@LINE-1]]:12>a</Param><Func@[[@LINE-2]]:6>+</Func><Param@[[@LINE-1]]:20>b</Param>
  a+b
}

func test10(a: [Int], i: Int) {
  // CHECK: <Param@[[@LINE-1]]:13>a</Param><iSubscript@>[</iSubscript><Param@[[@LINE-1]]:23>i</Param><iSubscript@>]</iSubscript>
  a[i]
}

class CWS {
  subscript (i : Int, j : Int) -> Int { return 0 }
}
func test11(a: CWS, i: Int) {
  // CHECK: <Param@[[@LINE-1]]:13>a</Param><Subscript@[[@LINE-3]]:3>[</Subscript><Param@[[@LINE-1]]:21>i</Param>,<Param@[[@LINE-1]]:21>i</Param><Subscript@[[@LINE-3]]:3>]</Subscript>
  a[i,i]
}

// CHECK: func <Func>test_defer</Func>() {
func test_defer() {
  defer {
    // CHECK: <Func@[[@LINE-2]]:6>test_defer</Func>()
    test_defer()
  }
}

func test_arg_tuple1(_: Int, _: Int) {}
func test_arg_tuple2(p1: Int, _: Int) {}
func test_arg_tuple3(_: Int, p2: Int) {}
func test_arg_tuple4(p1: Int, p2: Int) {}
// CHECK: <Func@[[@LINE-4]]:6>test_arg_tuple1</Func>(0,0)
test_arg_tuple1(0,0)
// CHECK: <Func@[[@LINE-5]]:6>test_arg_tuple2</Func>(<Func@[[@LINE-5]]:6#p1>p1</Func>:0,0)
test_arg_tuple2(p1:0,0)
// CHECK: <Func@[[@LINE-6]]:6>test_arg_tuple3</Func>(0,<Func@[[@LINE-6]]:6#p2>p2</Func>:0)
test_arg_tuple3(0,p2:0)
// CHECK: <Func@[[@LINE-7]]:6>test_arg_tuple4</Func>(<Func@[[@LINE-7]]:6#p1>p1</Func>:0,<Func@[[@LINE-7]]:6#p2>p2</Func>:0)
test_arg_tuple4(p1:0,p2:0)


@dynamicMemberLookup
struct Lens<T> {
  var obj: T
  init(_ obj: T) {
    self.obj = obj
  }
  subscript<U>(dynamicMember member: WritableKeyPath<T, U>) -> Lens<U> {
    get { return Lens<U>(obj[keyPath: member]) }
    set { obj[keyPath: member] = newValue.obj }
  }
}
struct Point {
  var x: Int
  var y: Int
}
struct Rectangle {
  var topLeft: Point
  var bottomRight: Point
}
func testDynamicMemberLookup(r: Lens<Rectangle>) {
  _ = r.topLeft
  // CHECK: _ = <Param@[[@LINE-2]]:30>r</Param>.<Var@[[@LINE-5]]:7>topLeft</Var>
  _ = r.bottomRight.y
  // CHECK: _ = <Param@[[@LINE-4]]:30>r</Param>.<Var@[[@LINE-6]]:7>bottomRight</Var>.<Var@[[@LINE-10]]:7>y</Var>
}
func keyPathTester<V>(keyPath: KeyPath<Lens<Rectangle>, V>) {}
func testKeyPath() {
  keyPathTester(keyPath: \.topLeft)
  // CHECK: <Func@[[@LINE-3]]:6>keyPathTester</Func>(<Func@[[@LINE-3]]:6#keyPath>keyPath</Func>: \.<Var@[[@LINE-12]]:7>topLeft</Var>)
}
