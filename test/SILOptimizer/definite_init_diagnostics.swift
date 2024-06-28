// RUN: %target-swift-frontend -enable-copy-propagation=requested-passes-only -emit-sil -primary-file %s -o /dev/null -verify

import Swift

func markUsed<T>(_ t: T) {}

// These are tests for definite initialization, which is implemented by the
// memory promotion pass.

func test1() -> Int {
  // expected-warning @+1 {{variable 'a' was never mutated; consider changing to 'let' constant}} {{3-6=let}}
  var a : Int  // expected-note {{variable defined here}}
  return a     // expected-error {{variable 'a' used before being initialized}}
}

func takes_inout(_ a: inout Int) {}
func takes_inout_any(_ a: inout Any) {}
func takes_closure(_ fn: () -> ()) {}

class SomeClass { 
  var x : Int   // expected-note {{'self.x' not initialized}}
  
  var computedProperty : Int { return 42 }

  init() { x = 0 }
  init(b : Bool) {
    if (b) {}
  } // expected-error {{return from initializer without initializing all stored properties}}
  
  func baseMethod() {}
}

struct SomeStruct { var x = 1 }

func takesPointer<T>(_: UnsafePointer<T>) {}

func test2() {
  // inout.

  var a1 : Int        // expected-note {{variable defined here}}
  takes_inout(&a1)    // expected-error {{variable 'a1' passed by reference before being initialized}}

  var a2 = 4
  takes_inout(&a2)    // ok.

  var a3 : Int
  a3 = 4
  takes_inout(&a3)    // ok.
  
  var a4 : Int            // expected-note {{variable defined here}}
  takesPointer(&a4)  // expected-error {{address of variable 'a4' taken before it is initialized}}


  // Closures.

  // expected-warning @+1 {{variable 'b1' was never mutated}} {{3-6=let}}
  var b1 : Int        // expected-note {{variable defined here}}
  takes_closure {     // expected-error {{variable 'b1' captured by a closure before being initialized}}
    markUsed(b1)
  }

  var b1a : Int        // expected-note {{variable defined here}}
  takes_closure {     // expected-error {{variable 'b1a' captured by a closure before being initialized}}
    b1a += 1
    markUsed(b1a)
  }

  var b2 = 4
  takes_closure {     // ok.
    markUsed(b2)
  }
  b2 = 1

  var b3 : Int
  b3 = 4
  takes_closure {     // ok.
    markUsed(b3)
  }
    
  var b4 : Int?
  takes_closure {     // ok.
    markUsed(b4!)
  }
  b4 = 7
  
  // Structs
  var s1 : SomeStruct
  s1 = SomeStruct()   // ok
  _ = s1

  var s2 : SomeStruct  // expected-note {{variable defined here}}
  s2.x = 1             // expected-error {{struct 's2' must be completely initialized before a member is stored to}}


  // Classes
  // expected-warning @+1 {{variable 'c1' was never mutated}} {{3-6=let}}
  var c1 : SomeClass   // expected-note {{variable defined here}}
  markUsed(c1.x)          // expected-error {{variable 'c1' used before being initialized}}


  let c2 = SomeClass()
  markUsed(c2.x)          // ok
  
  
  // Weak
  weak var w1 : SomeClass?
  _ = w1                // ok: default-initialized

  // Note: with -enable-copy-propagation, we also expect: {{weak reference will always be nil because the referenced object is deallocated here}}
  // expected-warning@+3 {{instance will be immediately deallocated because variable 'w2' is 'weak'}}
  // expected-note@+2 {{a strong reference is required to prevent the instance from being deallocated}}
  // expected-note@+1 {{'w2' declared here}}
  weak var w2 = SomeClass()
  _ = w2                // ok
  
  
  // Unowned. This is immediately crashing code (it causes a retain of a
  // released object).
  // expected-warning @+1 {{variable 'u1' was never mutated; consider changing to 'let' constant}} {{11-14=let}}
  unowned var u1 : SomeClass // expected-note {{variable defined here}}
  _ = u1                // expected-error {{variable 'u1' used before being initialized}}

  // expected-warning@+3 {{instance will be immediately deallocated because variable 'u2' is 'unowned'}}
  // expected-note@+2 {{a strong reference is required to prevent the instance from being deallocated}}
  // expected-note@+1 {{'u2' declared here}}
  unowned let u2 = SomeClass()
  _ = u2                // ok
}



// Tuple field sensitivity.
func test4() {
  // expected-warning @+1 {{variable 't1' was never mutated; consider changing to 'let' constant}} {{3-6=let}}
  var t1 = (1, 2, 3)
  markUsed(t1.0 + t1.1 + t1.2)  // ok


  // expected-warning @+1 {{variable 't2' was never mutated; consider changing to 'let' constant}} {{3-6=let}}
  var t2 : (Int, Int, Int)   // expected-note 3 {{variable defined here}}
  markUsed(t2.0)   // expected-error {{variable 't2.0' used before being initialized}}
  markUsed(t2.1)   // expected-error {{variable 't2.1' used before being initialized}}
  markUsed(t2.2)   // expected-error {{variable 't2.2' used before being initialized}}


  var t3 : (Int, Int, Int)   // expected-note {{variable defined here}}
  t3.0 = 1; t3.2 = 42
  markUsed(t3.0)
  markUsed(t3.1)   // expected-error {{variable 't3.1' used before being initialized}}
  markUsed(t3.2)


  // Partially set, wholly read.
  var t4 : (Int, Int, Int)   // expected-note 1 {{variable defined here}}
  t4.0 = 1; t4.2 = 42
  _ = t4            // expected-error {{variable 't4.1' used before being initialized}}
  

  // Subelement sets.
  var t5 : (a : (Int, Int), b : Int)  // expected-note {{variable defined here}}
  t5.a = (1,2)
  markUsed(t5.a.0)
  markUsed(t5.a.1)
  markUsed(t5.b)       // expected-error {{variable 't5.b' used before being initialized}}
  

  var t6 : (a : (Int, Int), b : Int)  // expected-note {{variable defined here}}
  t6.b = 12; t6.a.1 = 1
  markUsed(t6.a.0)     // expected-error {{variable 't6.a.0' used before being initialized}}
  markUsed(t6.a.1)
  markUsed(t6.b)
}

func tupleinout(_ a: inout (lo: Int, hi: Int)) {
  markUsed(a.0)   // ok
  markUsed(a.1)   // ok
}

// Address only types
func test5<T>(_ x: T, y: T) {
  var a : ((T, T), T)  // expected-note {{variable defined here}}
  a.0 = (x, y)
  
  _ = a     // expected-error {{variable 'a.1' used before being initialized}}
}


struct IntFloatStruct { var a = 1, b = 2.0 }

func test6() -> Int {
  var a = IntFloatStruct()

  a.a = 1
  a.b = 2

  return a.a
}

// Control flow.
func test7(_ cond: Bool) {
  var a : Int
  
  if cond { a = 42 } else { a = 17 }
  markUsed(a)         // ok

  var b : Int      // expected-note {{variable defined here}}
  if cond { } else { b = 17 }
  markUsed(b)         // expected-error {{variable 'b' used before being initialized}}
}

protocol SomeProtocol {
  func protoMe()
}

protocol DerivedProtocol : SomeProtocol {}

func existentials(_ i: Int, dp: DerivedProtocol) {
  // expected-warning @+1 {{variable 'a' was written to, but never read}}
  var a : Any = ()
  a = i

  // expected-warning @+1 {{variable 'b' was written to, but never read}}
  var b : Any
  b = ()

  // expected-warning @+1 {{variable 'c' was never used}} {{7-8=_}}
  var c : Any   // no uses.
  
  // expected-warning @+1 {{variable 'd1' was never mutated}} {{3-6=let}}
  var d1 : Any  // expected-note {{variable defined here}}
  _ = d1   // expected-error {{variable 'd1' used before being initialized}}
  

  // expected-warning @+1 {{variable 'e' was never mutated}} {{3-6=let}}
  var e : SomeProtocol  // expected-note {{variable defined here}}
  e.protoMe()           // expected-error {{variable 'e' used before being initialized}}
  
  var f : SomeProtocol = dp  // ok, init'd by existential upcast.
  
  // expected-warning @+1 {{variable 'g' was never mutated}} {{3-6=let}}
  var g : DerivedProtocol   // expected-note {{variable defined here}}
  f = g                     // expected-error {{variable 'g' used before being initialized}}
  _ = f
}


// Tests for top level code.
var g1 : Int                 // expected-note {{variable defined here}}
var g2 : Int = 42

func testTopLevelCode() {    // expected-error {{variable 'g1' used by function definition before being initialized}}
  markUsed(g1)
  markUsed(g2)
}

var (g3,g4) : (Int,Int)          // expected-note 2 {{variable defined here}}
class DITLC_Class {
  init() {            // expected-error {{variable 'g3' used by function definition before being initialized}}
    markUsed(g3)
  }
  deinit {            // expected-error {{variable 'g4' used by function definition before being initialized}}
    markUsed(g4)
  }
}

struct EmptyStruct {}

func useEmptyStruct(_ a: EmptyStruct) {}

func emptyStructTest() {
  // <rdar://problem/20065892> Diagnostic for an uninitialized constant calls it a variable
  let a : EmptyStruct  // expected-note {{constant defined here}}
  useEmptyStruct(a)    // expected-error {{constant 'a' used before being initialized}}

  var (b,c,d) : (EmptyStruct,EmptyStruct,EmptyStruct) // expected-note 2 {{variable defined here}}
  
  c = EmptyStruct()

  useEmptyStruct(b)     // expected-error {{variable 'b' used before being initialized}}
  useEmptyStruct(c)
  useEmptyStruct(d)     // expected-error {{variable 'd' used before being initialized}}

  var (e,f) : (EmptyStruct,EmptyStruct)
  (e, f) = (EmptyStruct(),EmptyStruct())
  
  useEmptyStruct(e)
  useEmptyStruct(f)

  var g : (EmptyStruct,EmptyStruct)
  g = (EmptyStruct(),EmptyStruct())
  
  useEmptyStruct(g.0)
  useEmptyStruct(g.1)
}

func takesTuplePair(_ a : inout (SomeClass, SomeClass)) {}

// This tests cases where a store might be an init or assign based on control
// flow path reaching it.
func conditionalInitOrAssign(_ c : Bool, x : Int) {
  var t : Int  // Test trivial types.
  if c {
    t = 0
  }
  if c {
    t = x
  }
  t = 2
  _ = t
  
  // Nontrivial type
  var sc : SomeClass
  if c {
    sc = SomeClass()
  }
  sc = SomeClass()
  _ = sc
  
  // Tuple element types
  var tt : (SomeClass, SomeClass)

  if c {
    tt.0 = SomeClass()
  } else {
    tt.1 = SomeClass()
  }

  tt.0 = SomeClass()
  tt.1 = tt.0
  
  var t2 : (SomeClass, SomeClass)  // expected-note {{variable defined here}}
  t2.0 = SomeClass()
  takesTuplePair(&t2)   // expected-error {{variable 't2.1' passed by reference before being initialized}}
}

enum NotInitializedUnion {
  init() {
  } // expected-error {{'self.init' isn't called on all paths before returning from initializer}}
  case X
  case Y
}

extension NotInitializedUnion {
  init(a : Int) {
  } // expected-error {{'self.init' isn't called on all paths before returning from initializer}}
}

enum NotInitializedGenericUnion<T> {
  init() { 
  } // expected-error {{'self.init' isn't called on all paths before returning from initializer}}
  case X
}


class SomeDerivedClass : SomeClass {
  var y : Int
  override init() { 
    y = 42  // ok
    super.init()
  }
  
  init(a : Bool) {
    super.init() // expected-error {{property 'self.y' not initialized at super.init call}}
  }

  init(a : Bool, b : Bool) {
    // x is a superclass member.  It cannot be used before we are initialized.
    x = 17  // expected-error {{'self' used in property access 'x' before 'super.init' call}}
    y = 42
    super.init()
  }
  
  init(a : Bool, b : Bool, c : Bool) {
    y = 42
    super.init()
  }

  init(a : Bool, b : Bool, c : Bool, d : Bool) {
    y = 42
    super.init()
    super.init() // expected-error {{'super.init' called multiple times in initializer}}
  }

  init(a : Bool, b : Bool, c : Bool, d : Bool, e : Bool) {
    super.init()  // expected-error {{property 'self.y' not initialized at super.init call}}
    super.init()  // expected-error {{'super.init' called multiple times in initializer}}
  }
  
  init(a : Bool, b : Bool, c : Bool, d : Bool, e : Bool, f : Bool) {
    y = 11
    if a { super.init() }
    x = 42        // expected-error {{'self' used in property access 'x' before 'super.init' call}}
  }               // expected-error {{'super.init' isn't called on all paths before returning from initializer}}
  
  func someMethod() {}
  
  init(a : Int) {
    y = 42
    super.init()
  }

  init(a : Int, b : Bool) {
    y = 42
    someMethod() // expected-error {{'self' used in method call 'someMethod' before 'super.init' call}}
    super.init()
  }

  init(a : Int, b : Int) {
    y = 42
    baseMethod()  // expected-error {{'self' used in method call 'baseMethod' before 'super.init' call}}
    super.init()
  }
  
  init(a : Int, b : Int, c : Int) {
    y = computedProperty  // expected-error {{'self' used in property access 'computedProperty' before 'super.init' call}}
    super.init()
  }

}

//===----------------------------------------------------------------------===//
//  Delegating initializers
//===----------------------------------------------------------------------===//

class DelegatingCtorClass {
  var ivar: EmptyStruct

  init() { ivar = EmptyStruct() }

  convenience init(x: EmptyStruct) {
    self.init()
    _ = ivar // okay: ivar has been initialized by the delegation above
  }
  
  convenience init(x: EmptyStruct, y: EmptyStruct) {
    _ = ivar       // expected-error {{'self' used before 'self.init' call}}
    ivar = x       // expected-error {{'self' used before 'self.init' call}}
    self.init()
  }

  convenience init(x: EmptyStruct, y: EmptyStruct, z: EmptyStruct) {
    self.init()
    self.init()
  }

  convenience init(x: (EmptyStruct, EmptyStruct)) {
    method()       // expected-error {{'self' used before 'self.init' call}}
    self.init()
  }

  convenience init(c : Bool) {
    if c {
      return
    }
    self.init()
  }                // expected-error {{'self.init' isn't called on all paths before returning from initializer}}

  convenience init(bool: Bool) {
    doesNotReturn()
  }

  convenience init(double: Double) {
  } // expected-error{{'self.init' isn't called on all paths before returning from initializer}}

  func method() {}
}



//===----------------------------------------------------------------------===//
//  Delegating initializers vs extensions
//===----------------------------------------------------------------------===//

protocol TriviallyConstructible {
  init()
  func go(_ x: Int)
}

extension TriviallyConstructible {
  init(down: Int) {
    go(down) // expected-error {{'self' used before 'self.init' call or assignment to 'self'}}
    self.init()
  }
}

//===----------------------------------------------------------------------===//
//  Various bugs
//===----------------------------------------------------------------------===//

// rdar://16119509 - Dataflow problem where we reject valid code.
class rdar16119509_Buffer {
  init(x : Int) { }
}
class rdar16119509_Base {}
class rdar16119509_Derived : rdar16119509_Base {
  override init() {
    var capacity = 2
    while capacity < 2 {
      capacity <<= 1
    }
    buffer = rdar16119509_Buffer(x: capacity)
  }

  var buffer : rdar16119509_Buffer
}

// <rdar://problem/16797372> Bogus error: self.init called multiple times in initializer
extension Foo {
  convenience init() {
    for _ in 0..<42 {
    }
    self.init(a: 4)
  }
}

class Foo {
  init(a : Int) {}
}



func doesNotReturn() -> Never {
  while true {}
}

func doesReturn() {}

func testNoReturn1(_ b : Bool) -> Any {
  var a : Any
  if b {
    a = 42
  } else {
    doesNotReturn()
  }

  return a   // Ok, because the noreturn call path isn't viable.
}

func testNoReturn2(_ b : Bool) -> Any {
  var a : Any  // expected-note {{variable defined here}}
  if b {
    a = 42
  } else {
    doesReturn()
  }

  // Not ok, since doesReturn() doesn't kill control flow.
  return a   // expected-error {{variable 'a' used before being initialized}}
}

class PerpetualMotion {
  func start() -> Never {
    repeat {} while true
  }
  static func stop() -> Never {
    repeat {} while true
  }
}

func testNoReturn3(_ b : Bool) -> Any {
  let a : Int

  switch b {
  default:
    PerpetualMotion().start() // expected-note {{a call to a never-returning function}}
  }

  return a // expected-warning {{will never be executed}}
}

func testNoReturn4(_ b : Bool) -> Any {
  let a : Int

  switch b {
  default:
    PerpetualMotion.stop() // expected-note {{a call to a never-returning function}}
  }

  return a // expected-warning {{will never be executed}}
}


// <rdar://problem/16687555> [DI] New convenience initializers cannot call inherited convenience initializers
class BaseWithConvenienceInits {
 init(int: Int) {}
 convenience init() {
    self.init(int: 3)
 }
}

class DerivedUsingConvenienceInits : BaseWithConvenienceInits {
  convenience init(string: String) {
    self.init()
  }
}

// <rdar://problem/16660680> QoI: preconditionFailure() in init method complains about super.init being called multiple times
class ClassWhoseInitDoesntReturn : BaseWithConvenienceInits {
  init() {  
    preconditionFailure("leave me alone dude");
  }
}

// <rdar://problem/17233681> DI: Incorrectly diagnostic in delegating init with generic enum
enum r17233681Lazy<T> {
  case Thunk(() -> T)
  case Value(T)
  
  init(value: T) {
    self = .Value(value)
  }
}

extension r17233681Lazy {
  init(otherValue: T) {
    self.init(value: otherValue)
  }
}


// <rdar://problem/17556858> delegating init that delegates to @_transparent init fails
struct FortyTwo { }

extension Double {
  init(v : FortyTwo) {
    self.init(0.0)
  }
}

// <rdar://problem/17686667> If super.init is implicitly inserted, DI diagnostics have no location info
class r17686667Base {}
class r17686667Test : r17686667Base {
  var x: Int
  override init() {  // expected-error {{property 'self.x' not initialized at implicitly generated super.init call}}

  }
}

// <rdar://problem/18199087> DI doesn't catch use of super properties lexically inside super.init call
class r18199087BaseClass {
  let data: Int
  init(val: Int) {
    data = val
  }
}
class r18199087SubClassA: r18199087BaseClass {
  init() {
    super.init(val: self.data)  // expected-error {{'self' used in property access 'data' before 'super.init' call}}
  }
}

class r18199087BaseClassNonTrivial {
  let data: SomeClass
  init(val: SomeClass) {
    data = val
  }
}

class r18199087SubClassANonTrivial: r18199087BaseClassNonTrivial {
  init() {
    super.init(val: self.data)  // expected-error {{'self' used in property access 'data' before 'super.init' call}}
  }
}

// <rdar://problem/18414728> QoI: DI should talk about "implicit use of self" instead of individual properties in some cases
class rdar18414728Base {
  var prop:String? { return "boo" }

  // expected-note @+1 {{change 'let' to 'var' to make it mutable}} {{3-6=var}}
  let aaaaa:String  // expected-note 3 {{'self.aaaaa' not initialized}}

  init() {
    if let p1 = prop { // expected-error {{'self' used in property access 'prop' before all stored properties are initialized}}
      aaaaa = p1
    }
    aaaaa = "foo"  // expected-error {{immutable value 'self.aaaaa' may only be initialized once}}
  }

  init(a : ()) {
    method1(42)   // expected-error {{'self' used in method call 'method1' before all stored properties are initialized}}
    aaaaa = "foo"
  }

  init(b : ()) {
    final_method() // expected-error {{'self' used in method call 'final_method' before all stored properties are initialized}}
    aaaaa = "foo"
  }

  init(c : ()) {
    aaaaa = "foo"
    final_method()  // ok
  }

  func method1(_ a : Int) {}
  final func final_method() {}
}

class rdar18414728Derived : rdar18414728Base {
  var prop2:String? { return "boo" }

  // expected-note @+1 2 {{change 'let' to 'var' to make it mutable}} {{3-6=var}} {{3-6=var}}
  let aaaaa2:String

  override init() {
    if let p1 = prop2 {  // expected-error {{'self' used in property access 'prop2' before 'super.init' call}}
      aaaaa2 = p1
    }
    aaaaa2 = "foo"    // expected-error {{immutable value 'self.aaaaa2' may only be initialized once}}
    super.init()
  }

  override init(a : ()) {
    method2()            // expected-error {{'self' used in method call 'method2' before 'super.init' call}}
    aaaaa2 = "foo"
    super.init()
  }

  override init(b : ()) {
    aaaaa2 = "foo"
    method2()           // expected-error {{'self' used in method call 'method2' before 'super.init' call}}
    super.init()
  }

  override init(c : ()) {
    super.init()        // expected-error {{property 'self.aaaaa2' not initialized at super.init call}}
    aaaaa2 = "foo"      // expected-error {{immutable value 'self.aaaaa2' may only be initialized once}}
    method2()
  }

  func method2() {}
}

struct rdar18414728Struct {
  var computed:Int? { return 4 }

  var i : Int  // expected-note 2 {{'self.i' not initialized}}
  var j : Int  // expected-note {{'self.j' not initialized}}

  init() {
    j = 42
    if let p1 = computed { // expected-error {{'self' used before all stored properties are initialized}}
      i = p1
    }
    i = 1
  }
  init(a : ()) {
    method(42)     // expected-error {{'self' used before all stored properties are initialized}}
    i = 1
    j = 2
  }

  func method(_ a : Int) {}
}


extension Int {
  mutating func mutate() {}
  func inspect() {}
}

extension Array {
  subscript(replacing index: Int, with newValue: Element) -> Element {
    mutating get {
      let oldValue = self[index]
      self[index] = newValue
      return oldValue
    }
  }
}

func throwingSwap<T>(_ a: inout T, _ b: inout T) throws {}

// <rdar://problem/19035287> let properties should only be initializable, not reassignable
struct LetProperties {
  // expected-note @+1 2 {{change 'let' to 'var' to make it mutable}} {{3-6=var}}
  let arr : [Int]
  // expected-note @+1 2 {{change 'let' to 'var' to make it mutable}} {{3-6=var}} {{3-6=var}}
  let (u, v) : (Int, Int)
  // expected-note @+1 2 {{change 'let' to 'var' to make it mutable}} {{3-6=var}} {{3-6=var}}
  let w : (Int, Int)
  let x = 42
  // expected-note @+1 {{change 'let' to 'var' to make it mutable}} {{3-6=var}}
  let y : Int
  let z : Int?  // expected-note{{'self.z' not initialized}}

  func methodTakesInOut(_ x: inout Int) {}
  func throwingMethodTakesInOut(_ x: inout Int) throws {}

  // Let properties can be initialized naturally exactly once along any given
  // path through an initializer.
  init(cond : Bool) {
    if cond {
      w.0 = 4
      (u,v) = (4,2)
      y = 71
    } else {
      y = 13
      v = 2
      u = v+1
      w.0 = 7
    }
    w.1 = 19
    z = nil
    arr = []
  }

  // Multiple initializations are an error.
  init(a : Int) {
    y = a
    y = a   // expected-error {{immutable value 'self.y' may only be initialized once}}

    u = a
    v = a
    u = a   // expected-error {{immutable value 'self.u' may only be initialized once}}
    v = a   // expected-error {{immutable value 'self.v' may only be initialized once}}

    w.0 = a
    w.1 = a

    w.0 = a  // expected-error {{immutable value 'self.w.0' may only be initialized once}}
    w.1 = a  // expected-error {{immutable value 'self.w.1' may only be initialized once}}

    arr = []
    arr = [] // expected-error {{immutable value 'self.arr' may only be initialized once}}
  }  // expected-error {{return from initializer without initializing all stored properties}}

  // inout uses of let properties are an error.
  init() throws {
    u = 1; v = 13; w = (1,2); y = 1 ; z = u

    u.inspect()  // ok, non mutating.
    
    arr = []
    arr[12] = 17   // expected-error {{cannot mutate subscript of immutable value 'self.arr'}}
  }
}


// <rdar://problem/19215313> let properties don't work with protocol method dispatch
protocol TestMutabilityProtocol {
  func toIntMax()
  mutating func changeToIntMax()
}
 
class C<T : TestMutabilityProtocol> {
  let x : T
  let y : T
  
  init(a : T) {
    x = a; y = a
    x.toIntMax()
  }
}

struct MyMutabilityImplementation : TestMutabilityProtocol {
  func toIntMax() {
  }
  
  mutating func changeToIntMax() {
  }
}


// <rdar://problem/16181314> don't require immediate initialization of 'let' values
func testLocalProperties(_ b : Int) -> Int {
  // expected-note @+1 {{change 'let' to 'var' to make it mutable}} {{3-6=var}}
  let x : Int
  let y : Int // never assigned is ok    expected-warning {{immutable value 'y' was never used}} {{7-8=_}}

  x = b
  x = b    // expected-error {{immutable value 'x' may only be initialized once}}

  // This is ok, since it is assigned multiple times on different paths.
  let z : Int
  if true || false {
    z = b
  } else {
    z = b
  }

  _ = z
  return x
}

// Should be rejected as multiple assignment.
func testAddressOnlyProperty<T>(_ b : T) -> T {
  // expected-note @+1 {{change 'let' to 'var' to make it mutable}} {{3-6=var}}
  let x : T
  let y : T
  let z : T   // never assigned is ok.  expected-warning {{immutable value 'z' was never used}} {{7-8=_}}
  x = b
  y = b
  x = b   // expected-error {{immutable value 'x' may only be initialized once}}

  _ = x
  return y
}


// <rdar://problem/19254812> DI bug when referencing let member of a class
class r19254812Base {}
class r19254812Derived: r19254812Base{
  let pi = 3.14159265359
  
  init(x : ()) {
    markUsed(pi)  // ok, no diagnostic expected.
  }
}


// <rdar://problem/19259730> Using mutating methods in a struct initializer with a let property is rejected
struct StructMutatingMethodTest {
  let a, b: String  // expected-note 2 {{'self.b' not initialized}}
  
  init(x: String, y: String) {
    a = x
    b = y
    mutate()   // ok
  }
  
  init(x: String) {
    a = x
    mutate() // expected-error {{'self' used before all stored properties are initialized}}
    b = x
  }

  init() {
    a = ""
    nonmutate() // expected-error {{'self' used before all stored properties are initialized}}
    b = ""
  }

  mutating func mutate() {}
  func nonmutate() {}
}

 // <rdar://problem/19268443> DI should reject this call to transparent function
 class TransparentFunction {
  let x : Int
  let y : Int
  init() {
    x = 42
    y = 12
  }
}

@_transparent
func myTransparentFunction(_ x : inout Int) {}


// <rdar://problem/19782264> Immutable, optional class members can't have their subproperties read from during init()
class MyClassWithAnInt {
  let channelCount : Int = 42
}
class MyClassTestExample {
  let clientFormat : MyClassWithAnInt!

  init(){
    clientFormat = MyClassWithAnInt()
    _ = clientFormat.channelCount
  }
}


// <rdar://problem/19746552> QoI: variable "used before being initialized" instead of "returned uninitialized" in address-only enum/struct

struct AddressOnlyStructWithInit<T, U> {
  let a : T?
  let b : U?   // expected-note {{'self.b' not initialized}}
  
  init(a : T) {
    self.a = a
  }     // expected-error {{return from initializer without initializing all stored properties}}
}

enum AddressOnlyEnumWithInit<T> {
  case X(T), Y
  
  init() {
  } // expected-error {{'self.init' isn't called on all paths before returning from initializer}}
}


// <rdar://problem/20135113> QoI: enum failable init that doesn't assign to self produces poor error
enum MyAwesomeEnum {
  case One, Two

  init?() {

  } // expected-error {{'self.init' isn't called on all paths before returning from initializer}}
}

// <rdar://problem/20679379> DI crashes on initializers on protocol extensions
extension SomeProtocol {
  init?() {
    let a = self  // expected-error {{'self' used before 'self.init' call or assignment to 'self'}}
    self = a
  }

  init(a : Int) {
  } // expected-error {{'self.init' isn't called on all paths before returning from initializer}}

  init(c : Float) {
    protoMe()   // expected-error {{'self' used before 'self.init' call or assignment to 'self'}}
  } // expected-error {{'self.init' isn't called on all paths before returning from initializer}}
}



// Lvalue check when the archetypes are not the same.
struct LValueCheck<T> {
  // expected-note @+1 {{change 'let' to 'var' to make it mutable}} {{3-6=var}}
  let x = 0  // expected-note {{initial value already provided in 'let' declaration}}
}

extension LValueCheck {
  init(newY: Int) {
    x = 42  // expected-error {{immutable value 'self.x' may only be initialized once}}
  }
}

// <rdar://problem/20477982> Accessing let-property with default value in init() can throw spurious error
struct DontLoadFullStruct {
  let x: Int = 1
  let y: Int
  init() {
    y = x  // ok!
  }
}


func testReassignment() {
  let c : Int  // expected-note {{change 'let' to 'var' to make it mutable}} {{3-6=var}}
  c = 12
  c = 32  // expected-error {{immutable value 'c' may only be initialized once}}
  _ = c
}


// <rdar://problem/21295093> Swift protocol cannot implement default initializer
protocol ProtocolInitTest {
  init()
  init(a : Int)

  var i: Int { get set }
}

extension ProtocolInitTest {
  init() {
  }  // expected-error {{'self.init' isn't called on all paths before returning from initializer}}

  init(b : Float) {
    self.init(a: 42)  // ok
  }

  // <rdar://problem/21684596> QoI: Poor DI diagnostic in protocol extension initializer
  init(test1 ii: Int) {
    i = ii         // expected-error {{'self' used before 'self.init' call or assignment to 'self'}}
    self.init()
  }

  init(test2 ii: Int) {
    self = unsafeBitCast(0, to: Self.self)
    i = ii
  }

  init(test3 ii: Int) {
    i = ii                // expected-error {{'self' used before 'self.init' call or assignment to 'self'}}
    self = unsafeBitCast(0, to: Self.self)
  }

  init(test4 ii: Int) {
    i = ii         // expected-error {{'self' used before 'self.init' call or assignment to 'self'}}
  }                // expected-error {{'self.init' isn't called on all paths before returning from initializer}}
}

// <rdar://problem/22436880> Function accepting UnsafeMutablePointer is able to change value of immutable value
func bug22436880(_ x: UnsafeMutablePointer<Int>) {}
func test22436880() {
  let x: Int
  x = 1
  _ = x
}

// https://github.com/apple/swift/issues/42806
let x: String? // expected-note 2 {{constant defined here}}
print(x?.count as Any) // expected-error {{constant 'x' used before being initialized}}
print(x!) // expected-error {{constant 'x' used before being initialized}}


// <rdar://problem/22723281> QoI: [DI] Misleading error from Swift compiler when using an instance method in init()
protocol PMI {
  func getg()
}

extension PMI {
  func getg() {}
}

class WS: PMI {
  final let x: String  // expected-note {{'self.x' not initialized}}
  
  init() {
    getg()   // expected-error {{'self' used in method call 'getg' before all stored properties are initialized}}
    self.x = "foo"
  }
}

// <rdar://problem/23013334> DI QoI: Diagnostic claims that property is being used when it actually isn't
class r23013334 {
  var B: Int   // expected-note {{'self.B' not initialized}}
  var A: String
  
  init(A: String) throws {
    self.A = A
    self.A.withCString { cString -> () in  // expected-error {{'self' captured by a closure before all members were initialized}}

      print(self.A)
      return ()
    }
    
    self.B = 0
  }
  
}

class r23013334Derived : rdar16119509_Base {
  var B: Int   // expected-note {{'self.B' not initialized}}
  var A: String
  
  init(A: String) throws {
    self.A = A
    self.A.withCString { cString -> () in  // expected-error {{'self' captured by a closure before all members were initialized}}
      
      print(self.A)
      return ()
    }
    
    self.B = 0
  }

}

// https://github.com/apple/swift/issues/44078

struct S1_44078 {
  let a: Int
  let b: Int // expected-note {{'self.b' not initialized}}
  
  init?(x: Int, y: Int) {
    self.a = x
    if y == 42 {
      return // expected-error {{return from initializer without initializing all stored properties}}
    }
    // many lines later
    self.b = y
  }
}

struct S2_44078 {
  let a: Int
  let b: Int // expected-note {{'self.b' not initialized}}
  
  init?(x: Int, y: Int) {
    self.a = x
    return // expected-error {{return from initializer without initializing all stored properties}}
  }
}

struct S3_44078 {
  let a: Int
  let b: Int // expected-note {{'self.b' not initialized}}
  
  init?(x: Int, y: Int) {
    self.a = x
    if y == 42 {
      self.b = y
      return
    }
  } // expected-error {{return from initializer without initializing all stored properties}}
}

enum E1_44078 {
  case A, B
  
  init?(x: Int) {
    if x == 42 {
      return
    }
    // many lines later
    self = .A
  } // expected-error {{'self.init' isn't called on all paths before returning from initializer}}
}

enum E2_44078 {
  case A, B
  
  init?() {
    return
  } // expected-error {{'self.init' isn't called on all paths before returning from initializer}}
}
enum E3_44078 {
  case A, B
  
  init?(x: Int) {
    if x == 42 {
      self = .A
      return
    }
  } // expected-error {{'self.init' isn't called on all paths before returning from initializer}}
}

class BadFooSuper {
  init() {}
  init(_ x: BadFooSuper) {}
}

class BadFooSubclass: BadFooSuper {
  override init() {
    super.init(self) // expected-error {{'self' used before 'super.init' call}}
  }
}

class SuperConvenienceBase {
  public init(_ i: Int) {}
  public convenience init(_ i1: Int, _ i2: Int) {
    self.init(i2)
  }
}

class SuperConvenienceSub : SuperConvenienceBase {
  public override init(_ i: Int) {
    super.init(i)
  }
  public init(_ i1: Int, _ i2: Int, _ i3: Int) {
    self.init(i1, i1) // expected-error{{'self' used before 'super.init' call}}
  } // expected-error{{'super.init' isn't called}}
}

// While testing some changes I found this regression that wasn't
// covered by any existing tests
class Base {}

func makeAnAny() -> Any { return 3 }

class Derived : Base {
  var x: Int?
  var y: Int?

  override init() {
    x = makeAnAny() as? Int
    y = makeAnAny() as? Int
    super.init()
  }
}

// This test makes sure that we properly error (but don't crash) when calling a
// subclass method as an argument to a super.init.
class MethodTestParent {
  init(i: Int) {}
}

class MethodTestChild : MethodTestParent {
  init() {
    super.init(i: getInt()) // expected-error {{'self' used in method call 'getInt' before 'super.init' call}}
  }

  init(val: ()) {
    // Currently we squelch the inner error of using self in method call for 'getInt2'
    super.init(i: getInt2(x: self)) // expected-error {{'self' used in method call 'getInt2' before 'super.init' call}}
  }

  func getInt() -> Int {
    return 0
  }

  func getInt2(x: MethodTestChild) -> Int {
    return 0
  }
}

// This test makes sure that if we cast self to a protocol (implicitly or not), we properly error.
protocol ProtocolCastTestProtocol : class {
}

class ProtocolCastTestParent {
  init(foo f: ProtocolCastTestProtocol) {
  }

  init(foo2 f: Any) {
  }
}

class ProtocolCastTestChild : ProtocolCastTestParent, ProtocolCastTestProtocol {
  private let value: Int

  init(value1 v: Int) {
    value = v
    super.init(foo: self) // expected-error {{'self' used before 'super.init' call}}
  }

  init(value2 v: Int) {
    value = v
    super.init(foo: self as ProtocolCastTestProtocol) // expected-error {{'self' used before 'super.init' call}}
  }

  init(value3 v: Int) {
    value = v
    super.init(foo2: self) // expected-error {{'self' used before 'super.init' call}}
  }

  init(value4 v: Int) {
    value = v
    super.init(foo2: self as Any) // expected-error {{'self' used before 'super.init' call}}
  }
}

// Make sure we don't diagnose immediate deallocation of instances if we first
// assign them through strong variables.
func testDontDiagnoseUnownedImmediateDeallocationThroughStrong() {
  weak var c1: SomeClass?
  do {
    let tmp = SomeClass()
    c1 = tmp // Note: with -enable-copy-propagation, we also expect: {{weak reference will always be nil because the referenced object is deallocated here}}
  }

  unowned let c2: SomeClass
  do {
    let tmp = SomeClass()
    c2 = tmp
  }

  weak var c3: SomeClass?
  let c3Tmp = SomeClass()
  c3 = c3Tmp // Note: with -enable-copy-propagation, we also expect: {{weak reference will always be nil because the referenced object is deallocated here}}

  unowned let c4: SomeClass
  let c4Tmp = SomeClass()
  c4 = c4Tmp

  _ = c1; _ = c2; _ = c3; _ = c4
}

class ClassWithUnownedProperties {

  weak var c1: SomeClass?
  unowned var c2: SomeClass

  init(c2: SomeClass) {
    self.c2 = c2
  }

  func assignToC1() {
    let tmp = SomeClass()
    c1 = tmp
  }

  func assignToC2() {
    let tmp = SomeClass()
    c2 = tmp
  }
}

// Tests for DI when optionals are defined using unchecked_take_enum_data_addr
// <rdar://38624845>

func testOptionalDoubleWrite() -> String? {
  let sConst: String? // expected-note {{change 'let' to 'var' to make it mutable}}
  sConst = ""
  sConst? = "v2" // expected-error {{immutable value 'sConst' may only be initialized once}}
  return sConst
}

func testOptionalDoubleWrite2() -> Int? {
  let x: Int? // expected-note {{change 'let' to 'var' to make it mutable}}
  x = 0
  x? = 0 // expected-error {{immutable value 'x' may only be initialized once}}
  return x
}

protocol DIOptionalTestProtocol {
  var f: Int { get set }
}

func testOptionalDoubleWrite3(p1: DIOptionalTestProtocol) -> DIOptionalTestProtocol? {
  let x: DIOptionalTestProtocol? // expected-note {{change 'let' to 'var' to make it mutable}}
  x = p1
  x? = p1 // expected-error {{immutable value 'x' may only be initialized once}}
  return x
}

func testOptionalWrite() {
  let x: Int? // expected-note {{constant defined here}}
              // expected-warning@-1 {{immutable value 'x' was never used; consider removing it}}
  x? = 0 // expected-error {{constant 'x' used before being initialized}}
}

func testOptionalWriteGenerics<T>(p: T) -> T? {
  let x: T? // expected-note {{constant defined here}}
            // expected-note@-1 {{change 'let' to 'var' to make it mutable}}
  x? = p  // expected-error {{constant 'x' used before being initialized}}
  x = p   // expected-error {{immutable value 'x' may only be initialized once}}
  return x
}

func testOptionalWriteGenerics2<T>(p: T) -> T? {
  let x: T? // expected-note {{change 'let' to 'var' to make it mutable}}
  x = p
  x? = p  // expected-error {{immutable value 'x' may only be initialized once}}
  return x
}

enum TestOptionalEnum {
  case Cons(Int)
  case Nil
}

func testOptionalWithEnum(p: TestOptionalEnum) -> TestOptionalEnum? {
  let x: TestOptionalEnum? // expected-note {{change 'let' to 'var' to make it mutable}}
  x = p
  x? = p  // expected-error {{immutable value 'x' may only be initialized once}}
  return x
}

// Tests for optional chaining

class DIOptionalTestClass {
  var r: DIOptionalTestClass? = nil
  var f: Int = 0;
  let g: Int = 0;
}

func testOptionalChaining(p: DIOptionalTestClass?) {
  p?.f = 2
}

func testOptionalChaining2(p: DIOptionalTestClass?) -> DIOptionalTestClass? {
  let x: DIOptionalTestClass?
  x = p
  x?.f = 1
  p?.r?.f = 2
  return x
}

struct DIOptionalTestStruct {
  var f: Int
}

func testOptionalChaining3() -> DIOptionalTestStruct? {
  let x: DIOptionalTestStruct?  // expected-note {{change 'let' to 'var' to make it mutable}}
  x = DIOptionalTestStruct(f: 0)
  x?.f = 2  // expected-error {{immutable value 'x' may only be initialized once}}
  return x
}

extension DIOptionalTestStruct {
  public init?() {
    self.f = 0
  }
}

func testOptionalChaining4() -> DIOptionalTestStruct? {
  let x: DIOptionalTestStruct?  // expected-note {{change 'let' to 'var' to make it mutable}}
  x = DIOptionalTestStruct()
  x?.f = 2  // expected-error {{immutable value 'x' may only be initialized once}}
  return x
}

struct DIOptionalTestStructPair {
  var pair: (Int, Int)
}

func test6() -> DIOptionalTestStructPair? {
  let x: DIOptionalTestStructPair?  // expected-note {{change 'let' to 'var' to make it mutable}}
  x = DIOptionalTestStructPair(pair: (0, 0))
  x?.pair.0 = 1 // expected-error {{immutable value 'x' may only be initialized once}}
  return x
}

func testOptionalChainingWithGenerics<T: DIOptionalTestProtocol>(p: T) -> T? {
  let x: T? // expected-note {{constant defined here}}
            // expected-note@-1 {{constant defined here}}
            // expected-note@-2 {{constant defined here}}

  // note that here assignment to 'f' is a call to the setter.
  x?.f = 0  // expected-error {{constant 'x' used before being initialized}}
            // expected-error@-1 {{constant 'x' passed by reference before being initialized}}
  return x  // expected-error {{constant 'x' used before being initialized}}
}

// Test optional tuples

func testOptionalTupleUse(x: Bool) -> Int? {
  let optTuple: (Int, Int)? // expected-note {{constant defined here}}
  return optTuple?.1 // expected-error {{constant 'optTuple' used before being initialized}}
}

func testOptionalTupleOverwrite(x: Bool) -> (Int, Int)? {
  let tupleVar: (Int, Int)? // expected-note {{change 'let' to 'var' to make it mutable}}
  tupleVar = (0, 0)
  tupleVar?.1 = 1           // expected-error {{immutable value 'tupleVar' may only be initialized once}}
  return tupleVar
}

func testOptionalTupleNoError(x: Bool) -> Int? {
  let optTuple: (Int, Int)?
  optTuple = (0, 0)
  return optTuple?.1
}

func testOptionalTupleNoError2(x: Bool) -> (Int, Int)? {
  var tupleVar: (Int, Int)?
  tupleVar = (0, 0)
  tupleVar?.1 = 1
  return tupleVar
}

// Test forced unwrapping of optionals

func testOptionalUseByUnwrap() {
  let x: Int? // expected-warning {{immutable value 'x' was never used; consider removing it}}
  x = 0
}

func testOptionalWriteByUnwrap() -> Int? {
  let x: Int? // expected-note {{change 'let' to 'var' to make it mutable}}
  x = 0
  x = 0      // expected-error {{immutable value 'x' may only be initialized once}}
  return x
}

func testOptionalUnwrapNoError() -> Int? {
  let x: Int?
  x = 0
  return x!
}

// https://github.com/apple/swift/issues/51914

class StrongCycle {
  var c: StrongCycle
  var d: Int
  init(first: ()) {
    self.d = 10
    self.c = self // expected-error {{variable 'self.c' used before being initialized}}
  }

  init(second: ()) {
    self.c = self // expected-error {{variable 'self.c' used before being initialized}}
    self.d = 10
  }
}

class WeakCycle {
  weak var c: WeakCycle?
  var d: Int
  init(first: ()) { // FIXME: This is inconsistent with the strong reference behavior above
    self.d = 10
    self.c = self
  }
  init(second: ()) {
    self.c = self // expected-error {{variable 'self.d' used before being initialized}}
    self.d = 10
  }
}

// <rdar://51198592> DI was crashing as it wrongly detected a `type(of: self)`
// use in a delegating initializer, when there was none.
class DelegatingInitTest {
  convenience init(x: Int) {
    self // expected-warning {{expression of type 'DelegatingInitTest' is unused}}
      // expected-error@-1 {{'self' used before 'self.init' call or assignment to 'self'}}
  } // expected-error {{'self.init' isn't called on all paths before returning from initializer}}
}

class A {
  var a: Int

  init(x: Int) {
    self.a = x
  }

  convenience init(i: Int) {
    if i > 0 {
      self.init(x: i)
    }
    if i > -100 {
      self.init(x: i)
    }
  } // expected-error {{'self.init' isn't called on all paths before returning from initializer}}
}

@propertyWrapper
struct Wrapper<T> {
  var wrappedValue: T

  init(wrappedValue initialValue: T) {
    self.wrappedValue = initialValue
  }
}

func foo(_ d: DerivedWrappedProperty) {
  print(d)
}

class DerivedWrappedProperty : SomeClass {
  @Wrapper var y: String
  var z : String

  init(s: String) {
    y = s
    z = s
    foo(self)  // expected-error {{'self' used in method call 'foo' before 'super.init' call}}
  }  // expected-error {{'super.init' isn't called on all paths before returning from initializer}}

}

// rdar://129031705 ([error: ... used before being initialized)
// Related to treating 'let's as immutable RValues.
struct S {
  let rotation: (Int, Int)

  init() {
    rotation.0 = 0
    rotation.1 = rotation.0
  }
}

// rdar://128890586: Init accessors
final class HasInitAccessors {
  private var _ints: [Int] = []

  private var ints: [Int] {
    @storageRestrictions(initializes: _ints)
    init(initialValue) {
        _ints = initialValue
    }
    get {
        return _ints
    }
    set {
      _ints = newValue
    }
  }

  init() {
    ints.append(0)
  }
}
