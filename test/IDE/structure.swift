// RUN: %swift-ide-test -structure -source-filename %s | %FileCheck %s

// CHECK: <class>class <name>MyCls</name> : <inherited><elem-typeref>OtherClass</elem-typeref></inherited> {
// CHECK:   <property>var <name>bar</name> : <type>Int</type></property>
// CHECK:   <property>var <name>anotherBar</name> : <type>Int</type> = 42</property>
// CHECK:   <cvar>class var <name>cbar</name> : <type>Int</type> = 0</cvar>
class MyCls : OtherClass {
  var bar : Int
  var anotherBar : Int = 42
  class var cbar : Int = 0

  // CHECK:   <ifunc>func <name>foo(<param>_ arg1: <type>Int</type></param>, <param><name>name</name>: <type>String</type></param>, <param><name>param</name> par: <type>String</type></param>)</name> {
  // CHECK:     <lvar>var <name>abc</name></lvar>
  // CHECK:     <if>if <elem-condexpr>1</elem-condexpr> <brace>{
  // CHECK:       <call><name>foo</name>(<arg>1</arg>, <arg><name>name</name>:"test"</arg>, <arg><name>param</name>:"test2"</arg>)</call>
  // CHECK:     }</brace>
  // CHECK:   }</ifunc>
  func foo(_ arg1: Int, name: String, param par: String) {
    var abc
    if 1 {
      foo(1, name:"test", param:"test2")
    }
  }

  // CHECK:   <ifunc><name>init (<param><name>x</name>: <type>Int</type></param>)</name></ifunc>
  init (x: Int)

  // CHECK:   <cfunc>class func <name>cfoo()</name></cfunc>
  class func cfoo()

// CHECK: }</class>
}

// CHECK: <struct>struct <name>MyStruc</name> {
// CHECK:   <property>var <name>myVar</name>: <type>Int</type></property>
// CHECK:   <svar>static var <name>sbar</name> : <type>Int</type> = 0</svar>
// CHECK:   <sfunc>static func <name>cfoo()</name></sfunc>
// CHECK: }</struct>
struct MyStruc {
  var myVar: Int
  static var sbar : Int = 0
  static func cfoo()
}

// CHECK: <protocol>protocol <name>MyProt</name> {
// CHECK:   <ifunc>func <name>foo()</name></ifunc>
// CHECK:   <ifunc>func <name>foo2()</name> throws</ifunc>
// CHECK:   <ifunc>func <name>foo3()</name> throws -> <type>Int</type></ifunc>
// CHECK:   <ifunc>func <name>foo4<<generic-param><name>T</name></generic-param>>()</name> where T: MyProt</ifunc>
// CHECK:   <ifunc><name>init()</name></ifunc>
// CHECK:   <ifunc><name>init(<param><name>a</name>: <type>Int</type></param>)</name> throws</ifunc>
// CHECK:   <ifunc><name>init<<generic-param><name>T</name></generic-param>>(<param><name>a</name>: <type>T</type></param>)</name> where T: MyProt</ifunc>
// CHECK: }</protocol>
protocol MyProt {
  func foo()
  func foo2() throws
  func foo3() throws -> Int
  func foo4<T>() where T: MyProt
  init()
  init(a: Int) throws
  init<T>(a: T) where T: MyProt
}

// CHECK: <extension>extension <name>MyStruc</name> {
// CHECK:   <ifunc>func <name>foo()</name> {
// CHECK:   }</ifunc>
// CHECK: }</extension>
extension MyStruc {
  func foo() {
  }
}

// CHECK: <gvar>var <name>gvar</name> : <type>Int</type> = 0</gvar>
var gvar : Int = 0

// CHECK: <ffunc>func <name>ffoo()</name> {}</ffunc>
func ffoo() {}

// CHECK: <foreach>for <elem-id><lvar><name>i</name></lvar></elem-id> in <elem-expr>0...5</elem-expr> <brace>{}</brace></foreach>
for i in 0...5 {}
// CHECK: <foreach>for <elem-id>var (<lvar><name>i</name></lvar>, <lvar><name>j</name></lvar>)</elem-id> in <elem-expr>array</elem-expr> <brace>{}</brace></foreach>
for var (i, j) in array {}

// CHECK: <while>while <elem-condexpr>var <lvar><name>v</name></lvar> = o, <lvar><name>z</name></lvar> = o where v > z</elem-condexpr> <brace>{}</brace></while>
while var v = o, z = o where v > z {}
// CHECK: <while>while <elem-condexpr>v == 0</elem-condexpr> <brace>{}</brace></while>
while v == 0 {}
// CHECK: <repeat-while>repeat <brace>{}</brace> while <elem-expr>v == 0</elem-expr></repeat-while>
repeat {} while v == 0
// CHECK: <if>if <elem-condexpr>var <lvar><name>v</name></lvar> = o, <lvar><name>z</name></lvar> = o where v > z</elem-condexpr> <brace>{}</brace></if>
if var v = o, z = o where v > z {}

// CHECK: <switch>switch <elem-expr>v</elem-expr> {
// CHECK:   <case>case <elem-pattern>1</elem-pattern>: break;</case>
// CHECK:   <case>case <elem-pattern>2</elem-pattern>, <elem-pattern>3</elem-pattern>: break;</case>
// CHECK:   <case>case <elem-pattern><call><name>Foo</name>(<arg>var <lvar><name>x</name></lvar></arg>, <arg>var <lvar><name>y</name></lvar></arg>)</call> where x < y</elem-pattern>: break;</case>
// CHECK:   <case>case <elem-pattern>2 where <call><name>foo</name>()</call></elem-pattern>, <elem-pattern>3 where <call><name>bar</name>()</call></elem-pattern>: break;</case>
// CHECK:   <case><elem-pattern>default</elem-pattern>: break;</case>
// CHECK: }</switch>
switch v {
  case 1: break;
  case 2, 3: break;
  case Foo(var x, var y) where x < y: break;
  case 2 where foo(), 3 where bar(): break;
  default: break;
}

// CHECK: <gvar>let <name>myArray</name> = <array>[<elem-expr>1</elem-expr>, <elem-expr>2</elem-expr>, <elem-expr>3</elem-expr>]</array></gvar>
let myArray = [1, 2, 3]
// CHECK: <gvar>let <name>myDict</name> = <dictionary>[<elem-expr>1</elem-expr>:<elem-expr>1</elem-expr>, <elem-expr>2</elem-expr>:<elem-expr>2</elem-expr>, <elem-expr>3</elem-expr>:<elem-expr>3</elem-expr>]</dictionary></gvar>
let myDict = [1:1, 2:2, 3:3]
// CHECK: <gvar>let <name>myArray2</name> = <array>[<elem-expr>1</elem-expr>]</array></gvar>
let myArray2 = [1]
// CHECK: <gvar>let <name>myDict2</name> = <dictionary>[<elem-expr>1</elem-expr>:<elem-expr>1</elem-expr>]</dictionary></gvar>
let myDict2 = [1:1]

// CHECK: <foreach>for <brace>{}</brace></foreach>
for {}

// CHECK: <class>class <name><#MyCls#></name> : <inherited><elem-typeref><#OtherClass#></elem-typeref></inherited> {}
class <#MyCls#> : <#OtherClass#> {}

// CHECK: <ffunc>func <name><#test1#> ()</name> {
// CHECK:   <foreach>for <elem-id><lvar><name><#name#></name></lvar></elem-id> in <elem-expr><#items#></elem-expr> <brace>{}</brace></foreach>
// CHECK: }</ffunc>
func <#test1#> () {
  for <#name#> in <#items#> {}
}

// CHECK: <gvar>let <name>myArray</name> = <array>[<elem-expr><#item1#></elem-expr>, <elem-expr><#item2#></elem-expr>]</array></gvar>
let myArray = [<#item1#>, <#item2#>]

// CHECK: <ffunc>func <name>test1()</name> {
// CHECK:   <call><name>dispatch_async</name>(<arg><call><name>dispatch_get_main_queue</name>()</call></arg>, <arg><closure><brace>{}</brace></closure></arg>)</call>
// CHECK:   <call><name>dispatch_async</name>(<arg><call><name>dispatch_get_main_queue</name>()</call></arg>) <arg><closure><brace>{}</brace></closure></arg></call>
// CHECK: }</ffunc>
func test1() {
  dispatch_async(dispatch_get_main_queue(), {})
  dispatch_async(dispatch_get_main_queue()) {}
}

// CHECK: <enum>enum <name>SomeEnum</name> {
// CHECK:   <enum-case>case <enum-elem><name>North</name></enum-elem></enum-case>
// CHECK:   <enum-case>case <enum-elem><name>South</name></enum-elem>, <enum-elem><name>East</name></enum-elem></enum-case>
// CHECK:   <enum-case>case <enum-elem><name>QRCode(<param><type>String</type></param>)</name></enum-elem></enum-case>
// CHECK:   <enum-case>case</enum-case>
// CHECK: }</enum>
enum SomeEnum {
  case North
  case South, East
  case QRCode(String)
  case
}

// CHECK: <enum>enum <name>Rawness</name> : <inherited><elem-typeref>Int</elem-typeref></inherited> {
// CHECK:   <enum-case>case <enum-elem><name>One</name> = <elem-initexpr>1</elem-initexpr></enum-elem></enum-case>
// CHECK:   <enum-case>case <enum-elem><name>Two</name> = <elem-initexpr>2</elem-initexpr></enum-elem>, <enum-elem><name>Three</name> = <elem-initexpr>3</elem-initexpr></enum-elem></enum-case>
// CHECK: }</enum>
enum Rawness : Int {
  case One = 1
  case Two = 2, Three = 3
}

// CHECK: <ffunc>func <name>rethrowFunc(<param>_ f: <type>() throws -> ()</type> = <closure><brace>{}</brace></closure></param>)</name> rethrows {}</ffunc>
func rethrowFunc(_ f: () throws -> () = {}) rethrows {}

class NestedPoundIf{
    func foo1() {
        #if os(macOS)
          var a = 1
            #if USE_METAL
              var b = 2
              #if os(iOS)
                var c = 3
              #else
                var c = 3
              #endif
            #else
              var b = 2
            #endif
           #else
            var a = 1
        #endif
    }
    func foo2() {}
    func foo3() {}
}
// CHECK: <ifunc>func <name>foo2()</name> {}</ifunc>
// CHECK: <ifunc>func <name>foo3()</name> {}</ifunc>

class A {
  func foo(_ i : Int, animations: () -> ()) {}
  func perform() {foo(5, animations: {})}
// CHECK:  <ifunc>func <name>perform()</name> {<call><name>foo</name>(<arg>5</arg>, <arg><name>animations</name>: <closure><brace>{}</brace></closure></arg>)</call>}</ifunc>
}

// CHECK: <typealias>typealias <name>OtherA</name> = A</typealias>
typealias OtherA = A

// CHECK: <typealias>typealias <name>EqBox</name><<generic-param><name>Boxed</name></generic-param>> = Box<Boxed> where Boxed: Equatable</typealias>
typealias EqBox<Boxed> = Box<Boxed> where Boxed: Equatable

class SubscriptTest {
  subscript(index: Int) -> Int {
    return 0
  }
  // CHECK: <subscript><name>subscript(<param>index: <type>Int</type></param>)</name> -> <type>Int</type> {
  // CHECK:  return 0
  // CHECK: }</subscript>

  subscript(string: String) -> Int {
    get {
      return 0
    }
    set(value) {
      print(value)
    }
  }
  // CHECK: <subscript><name>subscript(<param>string: <type>String</type></param>)</name> -> <type>Int</type> {
  // CHECK: get {
  // CHECK:   return 0
  // CHECK: }
  // CHECK: set(<param>value</param>) {
  // CHECK:   <call><name>print</name>(<arg>value</arg>)</call>
  // CHECK: }</subscript>
}

class ReturnType {
  func foo() -> Int { return 0 }
  // CHECK:  <ifunc>func <name>foo()</name> -> <type>Int</type> {
  // CHECK:  return 0
  // CHECK: }</ifunc>
  
  func foo2<T>() -> T {}
  // CHECK:  <ifunc>func <name>foo2<<generic-param><name>T</name></generic-param>>()</name> -> <type>T</type> {}</ifunc>
  
  func foo3() -> () -> Int {}
  // CHECK:  <ifunc>func <name>foo3()</name> -> <type>() -> Int</type> {}</ifunc>
}

protocol FooProtocol {
  associatedtype Bar
  // CHECK:  <associatedtype>associatedtype <name>Bar</name></associatedtype>
  associatedtype Baz: Equatable
  // CHECK:  <associatedtype>associatedtype <name>Baz</name>: Equatable</associatedtype>
  associatedtype Qux where Qux: Equatable
  // CHECK:  <associatedtype>associatedtype <name>Qux</name> where Qux: Equatable</associatedtype>
  associatedtype Bar2 = Int
  // CHECK:  <associatedtype>associatedtype <name>Bar2</name> = Int</associatedtype>
  associatedtype Baz2: Equatable = Int
  // CHECK:  <associatedtype>associatedtype <name>Baz2</name>: Equatable = Int</associatedtype>
  associatedtype Qux2 = Int where Qux2: Equatable
  // CHECK:  <associatedtype>associatedtype <name>Qux2</name> = Int where Qux2: Equatable</associatedtype>
}

// CHECK: <struct>struct <name>Generic</name><<generic-param><name>T</name>: <inherited><elem-typeref>Comparable</elem-typeref></inherited></generic-param>, <generic-param><name>X</name></generic-param>> {
// CHECK:   <subscript><name>subscript<<generic-param><name>U</name></generic-param>>(<param>generic: <type>U</type></param>)</name> -> <type>Int</type> { return 0 }</subscript>
// CHECK:   <typealias>typealias <name>Foo</name><<generic-param><name>Y</name></generic-param>> = Bar<Y></typealias>
// CHECK: }</struct>
struct Generic<T: Comparable, X> {
  subscript<U>(generic: U) -> Int { return 0 }
  typealias Foo<Y> = Bar<Y>
}

a.b(c: d?.e?.f, g: h)
// CHECK: <call><name>a.b</name>(<arg><name>c</name>: d?.e?.f</arg>, <arg><name>g</name>: h</arg>)</call>

struct Tuples {
  var foo: (Int, String) {
    return (1, "test")
    // CHECK: <tuple>(<elem-expr>1</elem-expr>, <elem-expr>"test"</elem-expr>)</tuple>
  }
  
  func foo2() {
    foo3(x: (1, 20))
    // CHECK: <call><name>foo3</name>(<arg><name>x</name>: <tuple>(<elem-expr>1</elem-expr>, <elem-expr>20</elem-expr>)</tuple></arg>)</call>
    let y = (x, foo4(a: 0))
    // CHECK: <lvar>let <name>y</name> = <tuple>(<elem-expr>x</elem-expr>, <elem-expr><call><name>foo4</name>(<arg><name>a</name>: 0</arg>)</call></elem-expr>)</tuple></lvar>
    
    let z = (name1: 1, name2: 2)
    // CHECK: <lvar>let <name>z</name> = <tuple>(name1: <elem-expr>1</elem-expr>, name2: <elem-expr>2</elem-expr>)</tuple></lvar>
  }
}

completion(a: 1) { (x: Any, y: Int) -> Int in
  return x as! Int + y
}
// CHECK: <call><name>completion</name>(<arg><name>a</name>: 1</arg>) <arg><closure><brace>{ (<param>x: <type>Any</type></param>, <param>y: <type>Int</type></param>) -> <type>Int</type> in
// CHECK:    return x as! Int + y
// CHECK: }</brace></closure></arg></call>

myFunc(foo: 0,
       bar: baz == 0)
// CHECK: <call><name>myFunc</name>(<arg><name>foo</name>: 0</arg>,
// CHECK:        <arg><name>bar</name>: baz == 0</arg>)</call>


enum FooEnum {
// CHECK: <enum>enum <name>FooEnum</name> {
  case blah(x: () -> () = {
  // CHECK: <enum-case>case <enum-elem><name>blah(<param><name>x</name>: <type>() -> ()</type> = <closure><brace>{
    @Tuples func foo(x: MyStruc) {}
    // CHECK: @Tuples <ffunc>func <name>foo(<param><name>x</name>: <type>MyStruc</type></param>)</name> {}</ffunc>
  })
  // CHECK: }</brace></closure></param>)</name></enum-elem></enum-case>
}
// CHECK: }</enum>

firstCall("\(1)", 1)
// CHECK: <call><name>firstCall</name>(<arg>"\(<arg>1</arg>)"</arg>, <arg>1</arg>)</call>

secondCall("\(a: {struct Foo {let x = 10}; return Foo().x}())", 1)
// CHECK: <call><name>secondCall</name>(<arg>"\(<arg><name>a</name>: <call><name><closure><brace>{<struct>struct <name>Foo</name> {<property>let <name>x</name> = 10</property>}</struct>; return <call><name>Foo</name>()</call>.x}</brace></closure></name>()</call></arg>)"</arg>, <arg>1</arg>)</call>

thirdCall("""
\("""
  \({
  return a()
  }())
  """)
""")
// CHECK: <call><name>thirdCall</name>(<arg>"""
// CHECK-NEXT: \(<arg>"""
// CHECK-NEXT:   \(<arg><call><name><closure><brace>{
// CHECK-NEXT:   return <call><name>a</name>()</call>
// CHECK-NEXT:   }</brace></closure></name>()</call></arg>)
// CHECK-NEXT:   """</arg>)
// CHECK-NEXT: """</arg>)</call>

fourthCall(a: @escaping () -> Int)
// CHECK: <call><name>fourthCall</name>(<arg><name>a</name>: @escaping () -> Int</arg>)</call>

// CHECK: <call><name>foo</name> <arg><closure><brace>{ [unowned <lvar><name>self</name></lvar>, <lvar><name>x</name></lvar>] in _ }</brace></closure></arg></call>
foo { [unowned self, x] in _ }
