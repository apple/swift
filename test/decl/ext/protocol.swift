// RUN: %target-typecheck-verify-swift

// ----------------------------------------------------------------------------
// Using protocol requirements from inside protocol extensions
// ----------------------------------------------------------------------------
protocol P1 {
  @discardableResult
  func reqP1a() -> Bool
}

extension P1 {
  func extP1a() -> Bool { return !reqP1a() }

  var extP1b: Bool {
    return self.reqP1a()
  }

  var extP1c: Bool {
    return extP1b && self.extP1a()
  }
}

protocol P2 {
  associatedtype AssocP2 : P1

  func reqP2a() -> AssocP2
}

extension P2 {
  func extP2a() -> AssocP2? { return reqP2a() }

  func extP2b() {
    self.reqP2a().reqP1a()
  }

  func extP2c() -> Self.AssocP2 { return extP2a()! }
}

protocol P3 {
  associatedtype AssocP3 : P2

  func reqP3a() -> AssocP3
}

extension P3 {
  func extP3a() -> AssocP3.AssocP2 {
    return reqP3a().reqP2a()
  }
}

protocol P4 {
  associatedtype AssocP4

  func reqP4a() -> AssocP4
}

// ----------------------------------------------------------------------------
// Using generics from inside protocol extensions
// ----------------------------------------------------------------------------
func acceptsP1<T : P1>(_ t: T) { }

extension P1 {
  func extP1d() { acceptsP1(self) }
}

func acceptsP2<T : P2>(_ t: T) { }

extension P2 {
  func extP2acceptsP1() { acceptsP1(reqP2a()) }
  func extP2acceptsP2() { acceptsP2(self) }
}

// Use of 'Self' as a return type within a protocol extension.
protocol SelfP1 {
  associatedtype AssocType
}

protocol SelfP2 {
}

func acceptSelfP1<T, U : SelfP1>(_ t: T, _ u: U) -> T where U.AssocType == T {
  return t
}

extension SelfP1 {
  func tryAcceptSelfP1<Z : SelfP1>(_ z: Z)-> Self where Z.AssocType == Self  {
    return acceptSelfP1(self, z)
  }
}

// ----------------------------------------------------------------------------
// Initializers in protocol extensions
// ----------------------------------------------------------------------------
protocol InitP1 {
  init(string: String)
}

extension InitP1 {
  init(int: Int) { self.init(string: "integer") }
}

struct InitS1 : InitP1 {
  init(string: String) { }
}

class InitC1 : InitP1 {
  required init(string: String) { }
}

func testInitP1() {
  var is1 = InitS1(int: 5)
  is1 = InitS1(string: "blah") // check type
  _ = is1

  var ic1 = InitC1(int: 5)
  ic1 = InitC1(string: "blah") // check type
  _ = ic1
}

// ----------------------------------------------------------------------------
// Subscript in protocol extensions
// ----------------------------------------------------------------------------
protocol SubscriptP1 {
  func readAt(_ i: Int) -> String
  func writeAt(_ i: Int, string: String)
}

extension SubscriptP1 {
  subscript(i: Int) -> String {
    get { return readAt(i) }
    set(newValue) { writeAt(i, string: newValue) }
  }
}

struct SubscriptS1 : SubscriptP1 {
  func readAt(_ i: Int) -> String { return "hello" }
  func writeAt(_ i: Int, string: String) { }
}

struct SubscriptC1 : SubscriptP1 {
  func readAt(_ i: Int) -> String { return "hello" }
  func writeAt(_ i: Int, string: String) { }
}

func testSubscriptP1(_ ss1: SubscriptS1, sc1: SubscriptC1,
                     i: Int, s: String) {
  var ss1 = ss1
  var sc1 = sc1
  _ = ss1[i]
  ss1[i] = s

  _ = sc1[i]
  sc1[i] = s
}

// ----------------------------------------------------------------------------
// Using protocol extensions on types that conform to the protocols.
// ----------------------------------------------------------------------------
struct S1 : P1 {
  @discardableResult
  func reqP1a() -> Bool { return true }

  func once() -> Bool {
    return extP1a() && extP1b
  }
}

func useS1(_ s1: S1) -> Bool {
  s1.reqP1a()
  return s1.extP1a() && s1.extP1b
}

extension S1 {
  func twice() -> Bool {
    return extP1a() && extP1b
  }
}

// ----------------------------------------------------------------------------
// Protocol extensions with redundant requirements
// ----------------------------------------------------------------------------

protocol FooProtocol {}
extension FooProtocol where Self: FooProtocol {} // expected-warning {{requirement of 'Self' to 'FooProtocol' is redundant in an extension of 'FooProtocol'}}

protocol AnotherFooProtocol {}
protocol BazProtocol {}
extension AnotherFooProtocol where Self: BazProtocol, Self: AnotherFooProtocol {} // expected-warning {{requirement of 'Self' to 'AnotherFooProtocol' is redundant in an extension of 'AnotherFooProtocol'}}

protocol AnotherBazProtocol {
  associatedtype BazValue
}

extension AnotherBazProtocol where BazValue: AnotherBazProtocol {} // ok, does not warn because BazValue is not Self

// ----------------------------------------------------------------------------
// Protocol extensions with additional requirements
// ----------------------------------------------------------------------------
extension P4 where Self.AssocP4 : P1 {
// expected-note@-1 {{candidate requires that 'Int' conform to 'P1' (requirement specified as 'Self.AssocP4' == 'P1')}}
// expected-note@-2 {{candidate requires that 'S4aHelper' conform to 'P1' (requirement specified as 'Self.AssocP4' == 'P1')}}
  func extP4a() {
    acceptsP1(reqP4a())
  }
}

struct S4aHelper { }
struct S4bHelper : P1 {
  func reqP1a() -> Bool { return true }
}

struct S4a : P4 {
  func reqP4a() -> S4aHelper { return S4aHelper() }
}

struct S4b : P4 {
  func reqP4a() -> S4bHelper { return S4bHelper() }
}

struct S4c : P4 {
  func reqP4a() -> Int { return 0 }
}

struct S4d : P4 {
  func reqP4a() -> Bool { return false }
}

extension P4 where Self.AssocP4 == Int { // expected-note {{where 'Self.AssocP4' = 'Bool'}}
  func extP4Int() { }
}

extension P4 where Self.AssocP4 == Bool {
// expected-note@-1 {{candidate requires that the types 'Int' and 'Bool' be equivalent (requirement specified as 'Self.AssocP4' == 'Bool')}}
// expected-note@-2 {{candidate requires that the types 'S4aHelper' and 'Bool' be equivalent (requirement specified as 'Self.AssocP4' == 'Bool')}}
  func extP4a() -> Bool { return reqP4a() }
}

func testP4(_ s4a: S4a, s4b: S4b, s4c: S4c, s4d: S4d) {
  s4a.extP4a() // expected-error{{no exact matches in call to instance method 'extP4a'}}
  s4b.extP4a() // ok
  s4c.extP4a() // expected-error{{no exact matches in call to instance method 'extP4a'}}
  s4c.extP4Int() // okay
  var b1 = s4d.extP4a() // okay, "Bool" version
  b1 = true // checks type above
  s4d.extP4Int() // expected-error{{referencing instance method 'extP4Int()' on 'P4' requires the types 'Bool' and 'Int' be equivalent}}
  _ = b1
}


// ----------------------------------------------------------------------------
// Protocol extensions with a superclass constraint on Self
// ----------------------------------------------------------------------------

protocol ConformedProtocol {
  typealias ConcreteConformanceAlias = Self
}

class BaseWithAlias<T> : ConformedProtocol {
  typealias ConcreteAlias = T

  struct NestedNominal {}

  func baseMethod(_: T) {}
}

class DerivedWithAlias : BaseWithAlias<Int> {}

protocol ExtendedProtocol {
  typealias AbstractConformanceAlias = Self
}

extension ExtendedProtocol where Self : DerivedWithAlias {
  func f0(x: T) {} // expected-error {{cannot find type 'T' in scope}}

  func f1(x: ConcreteAlias) {
    let _: Int = x
    baseMethod(x)
  }

  func f2(x: ConcreteConformanceAlias) {
    let _: DerivedWithAlias = x
  }

  func f3(x: AbstractConformanceAlias) {
    let _: DerivedWithAlias = x
  }

  func f4(x: NestedNominal) {}
}

// rdar://problem/21991470 & https://bugs.swift.org/browse/SR-5022
class NonPolymorphicInit {
  init() { } // expected-note {{selected non-required initializer 'init()'}}
}

protocol EmptyProtocol { }

// The diagnostic is not very accurate, but at least we reject this.

extension EmptyProtocol where Self : NonPolymorphicInit {
  init(string: String) {
    self.init()
    // expected-error@-1 {{constructing an object of class type 'Self' with a metatype value must use a 'required' initializer}}
  }
}

// ----------------------------------------------------------------------------
// Using protocol extensions to satisfy requirements
// ----------------------------------------------------------------------------
protocol P5 {
  func reqP5a()
}

// extension of P5 provides a witness for P6
extension P5 {
  func reqP6a() { reqP5a() }
}

protocol P6 {
  func reqP6a()
}

// S6a uses P5.reqP6a
struct S6a : P5 { 
  func reqP5a() { }
}

extension S6a : P6 { }

// S6b uses P5.reqP6a
struct S6b : P5, P6 { 
  func reqP5a() { }
}

// S6c uses P5.reqP6a
struct S6c : P6 { 
}

extension S6c : P5 {
  func reqP5a() { }
}

// S6d does not use P5.reqP6a
struct S6d : P6 { 
  func reqP6a() { }
}

extension S6d : P5 {
  func reqP5a() { }
}

protocol P7 {
  associatedtype P7Assoc

  func getP7Assoc() -> P7Assoc
}

struct P7FromP8<T> { }

protocol P8 {
  associatedtype P8Assoc
  func getP8Assoc() -> P8Assoc
}

// extension of P8 provides conformance to P7Assoc
extension P8 {
  func getP7Assoc() -> P7FromP8<P8Assoc> { return P7FromP8() }
}

// Okay, P7 requirements satisfied by P8
struct P8a : P8, P7 {
  func getP8Assoc() -> Bool { return true }
}

func testP8a(_ p8a: P8a) {
  var p7 = p8a.getP7Assoc()
  p7 = P7FromP8<Bool>() // okay, check type of above
  _ = p7
}

// Okay, P7 requirements explicitly specified
struct P8b : P8, P7 {
  func getP7Assoc() -> Int { return 5 }
  func getP8Assoc() -> Bool { return true }
}

func testP8b(_ p8b: P8b) {
  var p7 = p8b.getP7Assoc()
  p7 = 17 // check type of above
  _ = p7
}

protocol PConforms1 {
}

extension PConforms1 {
  func pc2() { } // expected-note{{candidate exactly matches}}
}

protocol PConforms2 : PConforms1, MakePC2Ambiguous {
  func pc2() // expected-note{{multiple matching functions named 'pc2()' with type '() -> ()'}}
}

protocol MakePC2Ambiguous {
}

extension MakePC2Ambiguous {
  func pc2() { } // expected-note{{candidate exactly matches}}
}

struct SConforms2a : PConforms2 { } // expected-error{{type 'SConforms2a' does not conform to protocol 'PConforms2'}}

struct SConforms2b : PConforms2 {
  func pc2() { }
}

// Satisfying requirements via protocol extensions for fun and profit
protocol _MySeq { }

protocol MySeq : _MySeq {
  associatedtype Generator : IteratorProtocol
  func myGenerate() -> Generator
}

protocol _MyCollection : _MySeq {
  associatedtype Index : Strideable

  var myStartIndex : Index { get }
  var myEndIndex : Index { get }

  associatedtype _Element
  subscript (i: Index) -> _Element { get }
}

protocol MyCollection : _MyCollection {
}

struct MyIndexedIterator<C : _MyCollection> : IteratorProtocol {
  var container: C
  var index: C.Index

  mutating func next() -> C._Element? {
    if index == container.myEndIndex { return nil }
    let result = container[index]
    index = index.advanced(by: 1)
    return result
  }
}

struct OtherIndexedIterator<C : _MyCollection> : IteratorProtocol {
  var container: C
  var index: C.Index

  mutating func next() -> C._Element? {
    if index == container.myEndIndex { return nil }
    let result = container[index]
    index = index.advanced(by: 1)
    return result
  }
}

extension _MyCollection {
  func myGenerate() -> MyIndexedIterator<Self> {
    return MyIndexedIterator(container: self, index: self.myEndIndex)
  }
}

struct SomeCollection1 : MyCollection {
  var myStartIndex: Int { return 0 }
  var myEndIndex: Int { return 10 }

  subscript (i: Int) -> String {
    return "blah"
  }
}

struct SomeCollection2 : MyCollection {
  var myStartIndex: Int { return 0 }
  var myEndIndex: Int { return 10 }

  subscript (i: Int) -> String {
    return "blah"
  }

  func myGenerate() -> OtherIndexedIterator<SomeCollection2> {
    return OtherIndexedIterator(container: self, index: self.myEndIndex)
  }
}

func testSomeCollections(_ sc1: SomeCollection1, sc2: SomeCollection2) {
  var mig = sc1.myGenerate()
  mig = MyIndexedIterator(container: sc1, index: sc1.myStartIndex)
  _ = mig

  var ig = sc2.myGenerate()
  ig = MyIndexedIterator(container: sc2, index: sc2.myStartIndex) // expected-error {{cannot assign value of type 'MyIndexedIterator<SomeCollection2>' to type 'OtherIndexedIterator<SomeCollection2>'}}
  _ = ig
}

public protocol PConforms3 {}
extension PConforms3 {
  public var z: Int {
    return 0
  }
}

public protocol PConforms4 : PConforms3 {
  var z: Int { get }
}

struct PConforms4Impl : PConforms4 {}
let pc4z = PConforms4Impl().z

// rdar://problem/20608438
protocol PConforms5 {
  func f() -> Int
}

protocol PConforms6 : PConforms5 {}

extension PConforms6 {
  func f() -> Int { return 42 }
}

func test<T: PConforms6>(_ x: T) -> Int { return x.f() }

struct PConforms6Impl : PConforms6 { }

// Extensions of a protocol that directly satisfy requirements (i.e.,
// default implementations hack N+1).
protocol PConforms7 {
  func method()

  var property: Int { get }

  subscript (i: Int) -> Int { get }
}

extension PConforms7 {
  func method() { }
  var property: Int { return 5 }
  subscript (i: Int) -> Int { return i }
}

struct SConforms7a : PConforms7 { }

protocol PConforms8 {
  associatedtype Assoc

  func method() -> Assoc
  var property: Assoc { get }
  subscript (i: Assoc) -> Assoc { get }
}

extension PConforms8 {
  func method() -> Int { return 5 }
  var property: Int { return 5 }
  subscript (i: Int) -> Int { return i }
}

struct SConforms8a : PConforms8 { }

struct SConforms8b : PConforms8 { 
  func method() -> String { return "" }
  var property: String { return "" }
  subscript (i: String) -> String { return i }
}

func testSConforms8b() {
  let s: SConforms8b.Assoc = "hello"
  _ = s
}

struct SConforms8c : PConforms8 { 
  func method() -> String { return "" } // no warning in type definition
}

func testSConforms8c() {
  let s: SConforms8c.Assoc = "hello" // expected-error{{cannot convert value of type 'String' to specified type 'SConforms8c.Assoc' (aka 'Int')}}
  _ = s
  let i: SConforms8c.Assoc = 5
  _ = i
}

protocol DefaultInitializable {
  init()
}

extension String : DefaultInitializable { }
extension Int : DefaultInitializable { }

protocol PConforms9 {
  associatedtype Assoc : DefaultInitializable // expected-note{{protocol requires nested type 'Assoc'}}

  func method() -> Assoc
  var property: Assoc { get }
  subscript (i: Assoc) -> Assoc { get }
}

extension PConforms9 {
  func method() -> Self.Assoc { return Assoc() }
  var property: Self.Assoc { return Assoc() }
  subscript (i: Self.Assoc) -> Self.Assoc { return Assoc() }
}

struct SConforms9a : PConforms9 { // expected-error{{type 'SConforms9a' does not conform to protocol 'PConforms9'}}
}

struct SConforms9b : PConforms9 {
  typealias Assoc = Int
}

func testSConforms9b(_ s9b: SConforms9b) {
  var p = s9b.property
  p = 5
  _ = p
}

struct SConforms9c : PConforms9 {
  typealias Assoc = String
}

func testSConforms9c(_ s9c: SConforms9c) {
  var p = s9c.property
  p = "hello"
  _ = p
}

struct SConforms9d : PConforms9 {
  func method() -> Int { return 5 }
}

func testSConforms9d(_ s9d: SConforms9d) {
  var p = s9d.property
  p = 6
  _ = p
}

protocol PConforms10 {}
extension PConforms10 {
 func f() {}
}
protocol PConforms11 {
 func f()
}
struct SConforms11 : PConforms10, PConforms11 {}

// ----------------------------------------------------------------------------
// Typealiases in protocol extensions.
// ----------------------------------------------------------------------------

// Basic support
protocol PTypeAlias1 {
  associatedtype AssocType1
}

extension PTypeAlias1 {
  typealias ArrayOfAssocType1 = [AssocType1]
}

struct STypeAlias1a: PTypeAlias1 {
  typealias AssocType1 = Int
}

struct STypeAlias1b<T>: PTypeAlias1 {
  typealias AssocType1 = T
}

func testPTypeAlias1() {
  var a: STypeAlias1a.ArrayOfAssocType1 = []
  a.append(1)

  var b: STypeAlias1b<String>.ArrayOfAssocType1 = []
  b.append("hello")
}

// Defaulted implementations to satisfy a requirement.
struct TypeAliasHelper<T> { }

protocol PTypeAliasSuper2 {
}

extension PTypeAliasSuper2 {
  func foo() -> TypeAliasHelper<Self> { return TypeAliasHelper() }
}

protocol PTypeAliasSub2 : PTypeAliasSuper2 {
  associatedtype Helper
  func foo() -> Helper
}

struct STypeAliasSub2a : PTypeAliasSub2 { }

struct STypeAliasSub2b<T, U> : PTypeAliasSub2 { }





// ----------------------------------------------------------------------------
// Partial ordering of protocol extension members
// ----------------------------------------------------------------------------

// Partial ordering between members of protocol extensions and members
// of concrete types.
struct S1b : P1 {
  func reqP1a() -> Bool { return true }

  func extP1a() -> Int { return 0 }
}

func useS1b(_ s1b: S1b) {
  var x = s1b.extP1a() // uses S1b.extP1a due to partial ordering
  x = 5 // checks that "x" deduced to "Int" above
  _ = x
  var _: Bool = s1b.extP1a() // still uses P1.ext1Pa due to type annotation
}

// Partial ordering between members of protocol extensions for
// different protocols.
protocol PInherit1 { }

protocol PInherit2 : PInherit1 { }

protocol PInherit3 : PInherit2 { }

protocol PInherit4 : PInherit2 { }

extension PInherit1 {
  func order1() -> Int { return 0 }
}

extension PInherit2 {
  func order1() -> Bool { return true }
}

extension PInherit3 {
  func order1() -> Double { return 1.0 }
}

extension PInherit4 {
  func order1() -> String { return "hello" }
}

struct SInherit1 : PInherit1 { }
struct SInherit2 : PInherit2 { }
struct SInherit3 : PInherit3 { }
struct SInherit4 : PInherit4 { }

func testPInherit(_ si2 : SInherit2, si3: SInherit3, si4: SInherit4) {
  var b1 = si2.order1() // PInherit2.order1
  b1 = true // check that the above returned Bool
  _ = b1

  var d1 = si3.order1() // PInherit3.order1
  d1 = 3.14159 // check that the above returned Double
  _ = d1

  var s1 = si4.order1() // PInherit4.order1
  s1 = "hello" // check that the above returned String
  _ = s1

  // Other versions are still visible, since they may have different
  // types.
  b1 = si3.order1() // PInherit2.order1
  var _: Int = si3.order1() // PInherit1.order1

}

protocol PConstrained1 {
  associatedtype AssocTypePC1
}

extension PConstrained1 {
  func pc1() -> Int { return 0 }
}

extension PConstrained1 where AssocTypePC1 : PInherit2 {
  func pc1() -> Bool { return true }
}

extension PConstrained1 where Self.AssocTypePC1 : PInherit3 {
  func pc1() -> String { return "hello" }
}

struct SConstrained1 : PConstrained1 {
  typealias AssocTypePC1 = SInherit1
}

struct SConstrained2 : PConstrained1 {
  typealias AssocTypePC1 = SInherit2
}

struct SConstrained3 : PConstrained1 {
  typealias AssocTypePC1 = SInherit3
}

func testPConstrained1(_ sc1: SConstrained1, sc2: SConstrained2,
                       sc3: SConstrained3) {
  var i = sc1.pc1() // PConstrained1.pc1
  i = 17 // checks type of above
  _ = i

  var b = sc2.pc1() // PConstrained1 (with PInherit2).pc1
  b = true // checks type of above
  _ = b

  var s = sc3.pc1() // PConstrained1 (with PInherit3).pc1
  s = "hello" // checks type of above
  _ = s
}

protocol PConstrained2 {
  associatedtype AssocTypePC2
}

protocol PConstrained3 : PConstrained2 {
}

extension PConstrained2 where Self.AssocTypePC2 : PInherit1 {
  func pc2() -> Bool { return true }
}

extension PConstrained3 {
  func pc2() -> String { return "hello" }
}

struct SConstrained3a : PConstrained3 {
  typealias AssocTypePC2 = Int
}

struct SConstrained3b : PConstrained3 {
  typealias AssocTypePC2 = SInherit3
}

func testSConstrained3(_ sc3a: SConstrained3a, sc3b: SConstrained3b) {
  var s = sc3a.pc2() // PConstrained3.pc2
  s = "hello"
  _ = s

  _ = sc3b.pc2()
  s = sc3b.pc2()
  var _: Bool = sc3b.pc2()
}

extension PConstrained3 where AssocTypePC2 : PInherit1 { }

// Extending via a superclass constraint.
class Superclass {
  func foo() { }
  static func bar() { }

  typealias Foo = Int
}

protocol PConstrained4 { }

extension PConstrained4 where Self : Superclass {
  func testFoo() -> Foo {
    foo()
    self.foo()

    return Foo(5)
  }

  static func testBar() {
    bar()
    self.bar()
  }
}

protocol PConstrained5 { }
protocol PConstrained6 {
  associatedtype Assoc

  func foo()
}

protocol PConstrained7 { }

extension PConstrained6 {
  var prop1: Int { return 0 }
  var prop2: Int { return 0 } // expected-note{{'prop2' previously declared here}}

  subscript (key: Int) -> Int { return key }
  subscript (key: Double) -> Double { return key } // expected-note{{'subscript(_:)' previously declared here}}
}

extension PConstrained6 {
  var prop2: Int { return 0 } // expected-error{{invalid redeclaration of 'prop2'}}
  subscript (key: Double) -> Double { return key } // expected-error{{invalid redeclaration of 'subscript(_:)'}}
}

extension PConstrained6 where Assoc : PConstrained5 {
  var prop1: Int { return 0 } // okay
  var prop3: Int { return 0 } // expected-note{{'prop3' previously declared here}}
  subscript (key: Int) -> Int { return key } // ok
  subscript (key: String) -> String { return key } // expected-note{{'subscript(_:)' previously declared here}}

  func foo() { } // expected-note{{'foo()' previously declared here}}
}

extension PConstrained6 where Assoc : PConstrained5 {
  var prop3: Int { return 0 } // expected-error{{invalid redeclaration of 'prop3'}}
  subscript (key: String) -> String { return key } // expected-error{{invalid redeclaration of 'subscript(_:)'}}
  func foo() { } // expected-error{{invalid redeclaration of 'foo()'}}
}

extension PConstrained6 where Assoc : PConstrained7 {
  var prop1: Int { return 0 } // okay
  subscript (key: Int) -> Int { return key } // okay
   func foo() { } // okay
}

extension PConstrained6 where Assoc == Int {
  var prop4: Int { return 0 }
  subscript (key: Character) -> Character { return key }
  func foo() { } // okay
}

extension PConstrained6 where Assoc == Double {
  var prop4: Int { return 0 } // okay
  subscript (key: Character) -> Character { return key } // okay
  func foo() { } // okay
}

// Interaction between RawRepresentable and protocol extensions.
public protocol ReallyRaw : RawRepresentable {
}

public extension ReallyRaw where RawValue: SignedInteger {
  // expected-warning@+1 {{'public' modifier is redundant for initializer declared in a public extension}}
  public init?(rawValue: RawValue) {
    self = unsafeBitCast(rawValue, to: Self.self)
  }
}

enum Foo : Int, ReallyRaw {
  case a = 0
}

// ----------------------------------------------------------------------------
// Semantic restrictions
// ----------------------------------------------------------------------------

// Extension cannot have an inheritance clause.
protocol BadProto1 { }
protocol BadProto2 { }

extension BadProto1 : BadProto2 { } // expected-error{{protocol extensions with conformances must currently be public}} expected-error{{inheritance clause in extension of protocol 'BadProto1'. use -enable-conforming-protocol-extensions to enable this feature}}

extension BadProto2 {
  struct S { } // expected-error{{type 'S' cannot be nested in protocol extension of 'BadProto2'}}
  class C { } // expected-error{{type 'C' cannot be nested in protocol extension of 'BadProto2'}}
  enum E { } // expected-error{{type 'E' cannot be nested in protocol extension of 'BadProto2'}}
}

extension BadProto1 {
  func foo() { }
  var prop: Int { return 0 }
  subscript (i: Int) -> String {
    return "hello"
  }
}

// rdar://problem/20756244
protocol BadProto3 { }
typealias BadProto4 = BadProto3
extension BadProto4 { } // okay

typealias RawRepresentableAlias = RawRepresentable
extension RawRepresentableAlias { } // okay

extension AnyObject { } // expected-error{{non-nominal type 'AnyObject' cannot be extended}}

// Members of protocol extensions cannot be overridden.
// rdar://problem/21075287
class BadClass1 : BadProto1 {
  func foo() { }
  override var prop: Int { return 5 } // expected-error{{property does not override any property from its superclass}}
}

protocol BadProto5 {
  associatedtype T1 // expected-note{{protocol requires nested type 'T1'}}
  associatedtype T2 // expected-note{{protocol requires nested type 'T2'}}
  associatedtype T3 // expected-note{{protocol requires nested type 'T3'}}
}

class BadClass5 : BadProto5 {} // expected-error{{type 'BadClass5' does not conform to protocol 'BadProto5'}}

typealias A = BadProto1
typealias B = BadProto1

extension A & B {} // expected-warning {{extending a protocol composition is not supported; extending 'BadProto1' instead}}

// Suppress near-miss warning for unlabeled initializers.
protocol P9 {
  init(_: Int)
  init(_: Double)
}

extension P9 {
  init(_ i: Int) {
    self.init(Double(i))
  }
}

struct X9 : P9 {
  init(_: Float) { }
}

extension X9 {
  init(_: Double) { }
}

// Suppress near-miss warning for unlabeled subscripts.
protocol P10 {
  subscript (_: Int) -> Int { get }
  subscript (_: Double) -> Double { get }
}

extension P10 {
  subscript(i: Int) -> Int {
    return Int(self[Double(i)])
  }
}

struct X10 : P10 {
  subscript(f: Float) -> Float { return f }
}

extension X10 {
  subscript(d: Double) -> Double { return d }
}

protocol Empty1 {}
protocol Empty2 {}

struct Concrete1 {}
extension Concrete1 : Empty1 & Empty2 {}

typealias TA = Empty1 & Empty2
struct Concrete2 {}
extension Concrete2 : TA {}

func f<T : Empty1 & Empty2>(_: T) {}

func useF() {
  f(Concrete1())
  f(Concrete2())
}
