// RUN: %target-typecheck-verify-swift

//===----------------------------------------------------------------------===//
// Tests for various simple enum constructs
//===----------------------------------------------------------------------===//


public enum unionSearchFlags {
  case none
  case backwards
  case anchored

  init() { self = .none }
}

func test1() -> unionSearchFlags {
  let _ : unionSearchFlags
  var b = unionSearchFlags.none
  b = unionSearchFlags.anchored
  _ = b

  return unionSearchFlags.backwards
}

func test1a() -> unionSearchFlags {
  var _ : unionSearchFlags
  var b : unionSearchFlags = .none
  b = .anchored
  _ = b

  // ForwardIndex use of MaybeInt.
  _ = MaybeInt.none

  return .backwards
}

func test1b(_ b : Bool) {
  _ = 123
  _ = .description == 1
  // expected-error@-1 {{instance member 'description' of type 'Int' cannot be used in static context}}
  // expected-error@-2 {{member 'description' in 'Int' produces result of type 'String', but context expects 'Int'}}
}

enum MaybeInt {
  case none
  case some(Int) // expected-note {{'some' declared here}}

  init(_ i: Int) { self = MaybeInt.some(i) }
}

func test2(_ a: Int, _ b: Int, _ c: MaybeInt) {
  _ = MaybeInt.some(4)
  _ = MaybeInt.some
  _ = MaybeInt.some(b)

  test2(1, 2, .none)
}

enum ZeroOneTwoThree {
  case Zero
  case One(Int)
  case Two(Int, Int)
  case Three(Int,Int,Int)
  case Unknown(MaybeInt, MaybeInt, MaybeInt)

  init (_ i: Int) { self = .One(i) }
  init (_ i: Int, _ j: Int, _ k: Int) { self = .Three(i, j, k) }
  init (_ i: MaybeInt, _ j: MaybeInt, _ k: MaybeInt) { self = .Unknown(i, j, k) }
}

func test3(_ a: ZeroOneTwoThree) {
  _ = ZeroOneTwoThree.Three(1,2,3)
  _ = ZeroOneTwoThree.Unknown(
    MaybeInt.none, MaybeInt.some(4), MaybeInt.some(32))
  _ = ZeroOneTwoThree(MaybeInt.none, MaybeInt(4), MaybeInt(32))

  var _ : Int =
     ZeroOneTwoThree.Zero // expected-error {{cannot convert value of type 'ZeroOneTwoThree' to specified type 'Int'}}

  // expected-warning @+1 {{unused}}
  test3 ZeroOneTwoThree.Zero // expected-error {{function is unused}} expected-error{{consecutive statements}} {{8-8=;}}
  test3 (ZeroOneTwoThree.Zero)
  test3(ZeroOneTwoThree.Zero)
  test3 // expected-error {{function is unused}}
  // expected-warning @+1 {{unused}}
  (ZeroOneTwoThree.Zero)
  
  var _ : ZeroOneTwoThree = .One(4)
  
  var _ : (Int,Int) -> ZeroOneTwoThree = .Two // expected-error{{type '(Int, Int) -> ZeroOneTwoThree' has no member 'Two'}}
  var _ : Int = .Two // expected-error{{type 'Int' has no member 'Two'}}
  var _ : MaybeInt = 0 > 3 ? .none : .soma(3) // expected-error {{type 'MaybeInt' has no member 'soma'; did you mean 'some'?}}
}

func test3a(_ a: ZeroOneTwoThree) {
  var e : ZeroOneTwoThree = (.Three(1, 2, 3))
  var f = ZeroOneTwoThree.Unknown(.none, .some(4), .some(32))

  var g = .none  // expected-error {{reference to member 'none' cannot be resolved without a contextual type}}

  // Overload resolution can resolve this to the right constructor.
  var h = ZeroOneTwoThree(1)
  
  var i = 0 > 3 ? .none : .some(3) // expected-error {{cannot infer contextual base in reference to member 'none'}}

  test3a;  // expected-error {{function is unused}}
  .Zero   // expected-error {{reference to member 'Zero' cannot be resolved without a contextual type}}
  test3a   // expected-error {{function is unused}}
  (.Zero) // expected-error {{reference to member 'Zero' cannot be resolved without a contextual type}}
  test3a(.Zero)
}


struct CGPoint { var x : Int, y : Int }
typealias OtherPoint = (x : Int, y : Int)

func test4() {
  var a : CGPoint
  // Note: we reject the following because it conflicts with the current
  // "init" hack.
  var b = CGPoint.CGPoint(1, 2) // expected-error {{type 'CGPoint' has no member 'CGPoint'}}
  var c = CGPoint(x: 2, y : 1)   // Using injected name.

  var e = CGPoint.x // expected-error {{member 'x' of type 'CGPoint' cannot be used in static context}}
  var f = OtherPoint.x  // expected-error {{type 'OtherPoint' (aka '(x: Int, y: Int)') has no member 'x'}}
}



struct CGSize { var width : Int, height : Int }

extension CGSize {
  func area() -> Int {
    return width*self.height
  }
  
  func area_wrapper() -> Int {
    return area()
  }
}

struct CGRect { 
  var origin : CGPoint,
  size : CGSize
  
  func area() -> Int {
    return self.size.area()
  }
}

func area(_ r: CGRect) -> Int {
  return r.size.area()
}

extension CGRect {
  func search(_ x: Int) -> CGSize {}
  func bad_search(_: Int) -> CGSize {}
}

func test5(_ myorigin: CGPoint) {
  let x1 = CGRect(origin: myorigin, size: CGSize(width: 42, height: 123))
  let x2 = x1

  _ = 4+5

  // Dot syntax.
  _ = x2.origin.x
  _ = x1.size.area()
  _ = (r : x1.size).r.area() // expected-error {{cannot create a single-element tuple with an element label}}
  _ = x1.size.area()
  _ = (r : x1.size).r.area() // expected-error {{cannot create a single-element tuple with an element label}}
  
  _ = x1.area

  _ = x1.search(42)
  _ = x1.search(42).width

  // TODO: something like this (name binding on the LHS):
  // var (CGSize(width, height)) = CGSize(1,2)

  // TODO: something like this, how do we get it in scope in the {} block?
  //if (var some(x) = somemaybeint) { ... }

  
}

struct StructTest1 {
  var a : Int, c, b : Int


  typealias ElementType = Int
}

enum UnionTest1 {
  case x
  case y(Int)

  func foo() {}

  init() { self = .x }
}


extension UnionTest1 {
  func food() {}
  func bar() {}

  // Type method.
  static func baz() {}
}

struct EmptyStruct {
  func foo() {}
}

func f() { 
  let a : UnionTest1
  a.bar()
  UnionTest1.baz()  // dot syntax access to a static method.
  
  // Test that we can get the "address of a member".
  var _ : () -> () = UnionTest1.baz
  var _ : (UnionTest1) -> () -> () = UnionTest1.bar
}

func union_error(_ a: ZeroOneTwoThree) {
  var _ : ZeroOneTwoThree = .Zero(1) // expected-error {{enum case 'Zero' has no associated values}}
  var _ : ZeroOneTwoThree = .Zero() // expected-error {{enum case 'Zero' has no associated values}} {{34-36=}}
  var _ : ZeroOneTwoThree = .One // expected-error {{member 'One' expects argument of type 'Int'}}
  var _ : ZeroOneTwoThree = .foo // expected-error {{type 'ZeroOneTwoThree' has no member 'foo'}}
  var _ : ZeroOneTwoThree = .foo() // expected-error {{type 'ZeroOneTwoThree' has no member 'foo'}}
}

func local_struct() {
  struct s { func y() {} }
}

//===----------------------------------------------------------------------===//
// A silly units example showing "user defined literals".
//===----------------------------------------------------------------------===//

struct distance { var v : Int }

func - (lhs: distance, rhs: distance) -> distance {}

extension Int {
  func km() -> enumtest.distance {}
  func cm() -> enumtest.distance {}
}

func units(_ x: Int) -> distance {
  return x.km() - 4.cm() - 42.km()
}



var %% : distance -> distance // expected-error {{expected pattern}} 

func badTupleElement() {
  typealias X = (x : Int, y : Int)
  var y = X.y // expected-error{{type 'X' (aka '(x: Int, y: Int)') has no member 'y'}}
  var z = X.z // expected-error{{type 'X' (aka '(x: Int, y: Int)') has no member 'z'}}
}

enum Direction {
  case North(distance: Int)
  case NorthEast(distanceNorth: Int, distanceEast: Int)
}

func testDirection() {
  var dir: Direction = .North(distance: 5)
  dir = .NorthEast(distanceNorth: 5, distanceEast: 7)

  var i: Int
  switch dir {
  case .North(let x):
    i = x
    break

  case .NorthEast(let x): // expected-warning {{enum case 'NorthEast' has 2 associated values; matching them as a tuple is deprecated}}
                          // expected-note@-14 {{'NorthEast(distanceNorth:distanceEast:)' declared here}}
    i = x.distanceEast
    break
  }
  _ = i
}

enum NestedSingleElementTuple {
  case Case(x: (y: Int)) // expected-error{{cannot create a single-element tuple with an element label}} {{17-20=}}
}

enum SimpleEnum {
  case X, Y
}

func testSimpleEnum() {
  let _ : SimpleEnum = .X
  let _ : SimpleEnum = (.X)
  let _ : SimpleEnum=.X    // expected-error {{'=' must have consistent whitespace on both sides}}
}

// https://github.com/apple/swift/issues/43127
enum E_43127: String {
    case Thing = "thing"
    case Bob = {"test"} // expected-error {{raw value for enum case must be a literal}}
}


// <rdar://problem/21269142> Diagnostic should say why enum has no .rawValue member
enum E21269142 {  // expected-note {{did you mean to specify a raw type on the enum declaration?}}
  case Foo
}

print(E21269142.Foo.rawValue)  // expected-error {{value of type 'E21269142' has no member 'rawValue'}}

// Check that typo correction does something sensible with synthesized members.
enum SyntheticMember { // expected-note {{property 'hashValue' is implicitly declared}}
  case Foo
}

func useSynthesizedMember() {
  print(SyntheticMember.Foo.hasValue) // expected-error {{value of type 'SyntheticMember' has no member 'hasValue'; did you mean 'hashValue'?}}
}

// Non-materializable argument type
enum Lens<T> {
  case foo(inout T) // expected-error {{'inout' may only be used on function or initializer parameters}}
  case bar(inout T, Int) // expected-error {{'inout' may only be used on function or initializer parameters}}

  case baz((inout T) -> ()) // ok
  case quux((inout T, inout T) -> ()) // ok
}

// In the long term, these should be legal, but we don't support them right
// now and we shouldn't pretend to.
// rdar://46684504
enum HasVariadic {
  case variadic(x: Int...) // expected-error {{variadic enum cases are not supported}}
}

// https://github.com/apple/swift/issues/44784

enum Foo {
  case bar
  case none
}

let _: Foo? = .none // expected-warning {{assuming you mean 'Optional<Foo>.none'; did you mean 'Foo.none' instead?}}
// expected-note@-1 {{explicitly specify 'Optional' to silence this warning}} {{15-15=Optional}}
// expected-note@-2 {{use 'Foo.none' instead}} {{15-15=Foo}}
let _: Foo?? = .none // expected-warning {{assuming you mean 'Optional<Optional<Foo>>.none'; did you mean 'Foo.none' instead?}}
// expected-note@-1 {{explicitly specify 'Optional' to silence this warning}} {{16-16=Optional}}
// expected-note@-2 {{use 'Foo.none' instead}} {{16-16=Foo}}

let _: Foo = .none // ok
let _: Foo = .bar // ok
let _: Foo? = .bar // ok
let _: Foo?? = .bar // ok
let _: Foo = Foo.bar // ok
let _: Foo = Foo.none // ok
let _: Foo? = Foo.none // ok
let _: Foo?? = Foo.none // ok

func baz(_: Foo?) {}
baz(.none) // expected-warning {{assuming you mean 'Optional<Foo>.none'; did you mean 'Foo.none' instead?}}
// expected-note@-1 {{explicitly specify 'Optional' to silence this warning}} {{5-5=Optional}}
// expected-note@-2 {{use 'Foo.none' instead}} {{5-5=Foo}}

let test: Foo? = .none // expected-warning {{assuming you mean 'Optional<Foo>.none'; did you mean 'Foo.none' instead?}}
// expected-note@-1 {{explicitly specify 'Optional' to silence this warning}} {{18-18=Optional}}
// expected-note@-2 {{use 'Foo.none' instead}} {{18-18=Foo}}
let answer = test == .none // expected-warning {{assuming you mean 'Optional<Foo>.none'; did you mean 'Foo.none' instead?}}
// expected-note@-1 {{explicitly specify 'Optional' to silence this warning}} {{22-22=Optional}}
// expected-note@-2 {{use 'Foo.none' instead}} {{22-22=Foo}}

enum Bar {
  case baz
}

let _: Bar? = .none // ok
let _: Bar?? = .none // ok
let _: Bar? = .baz // ok
let _: Bar?? = .baz // ok
let _: Bar = .baz // ok

enum AnotherFoo {
  case none(Any)
}

let _: AnotherFoo? = .none // ok
let _: AnotherFoo? = .none(0) // ok

struct FooStruct {
  static let none = FooStruct()
  static let one = FooStruct()
}

let _: FooStruct? = .none // expected-warning {{assuming you mean 'Optional<FooStruct>.none'; did you mean 'FooStruct.none' instead?}}
// expected-note@-1 {{explicitly specify 'Optional' to silence this warning}} {{21-21=Optional}}
// expected-note@-2 {{use 'FooStruct.none' instead}} {{21-21=FooStruct}}
let _: FooStruct?? = .none // expected-warning {{assuming you mean 'Optional<Optional<FooStruct>>.none'; did you mean 'FooStruct.none' instead?}}
// expected-note@-1 {{explicitly specify 'Optional' to silence this warning}} {{22-22=Optional}}
// expected-note@-2 {{use 'FooStruct.none' instead}} {{22-22=FooStruct}}
let _: FooStruct = .none // ok
let _: FooStruct = .one // ok
let _: FooStruct? = .one // ok
let _: FooStruct?? = .one // ok

struct NestedBazEnum {
  enum Baz {
    case one
    case none
  }
}

let _: NestedBazEnum.Baz? = .none // expected-warning {{assuming you mean 'Optional<NestedBazEnum.Baz>.none'; did you mean 'NestedBazEnum.Baz.none' instead?}}
// expected-note@-1 {{explicitly specify 'Optional' to silence this warning}} {{29-29=Optional}}
// expected-note@-2 {{use 'NestedBazEnum.Baz.none' instead}} {{29-29=NestedBazEnum.Baz}}
let _: NestedBazEnum.Baz?? = .none // expected-warning {{assuming you mean 'Optional<Optional<NestedBazEnum.Baz>>.none'; did you mean 'NestedBazEnum.Baz.none' instead?}}
// expected-note@-1 {{explicitly specify 'Optional' to silence this warning}} {{30-30=Optional}}
// expected-note@-2 {{use 'NestedBazEnum.Baz.none' instead}} {{30-30=NestedBazEnum.Baz}}
let _: NestedBazEnum.Baz = .none // ok
let _: NestedBazEnum.Baz = .one // ok
let _: NestedBazEnum.Baz? = .one // ok
let _: NestedBazEnum.Baz?? = .one // ok

struct NestedBazEnumGeneric {
  enum Baz<T> {
    case one
    case none
  }
}

let _: NestedBazEnumGeneric.Baz<Int>? = .none // expected-warning {{assuming you mean 'Optional<NestedBazEnumGeneric.Baz<Int>>.none'; did you mean 'NestedBazEnumGeneric.Baz<Int>.none' instead?}}
// expected-note@-1 {{explicitly specify 'Optional' to silence this warning}} {{41-41=Optional}}
// expected-note@-2 {{use 'NestedBazEnumGeneric.Baz<Int>.none' instead}} {{41-41=NestedBazEnumGeneric.Baz<Int>}}
let _: NestedBazEnumGeneric.Baz<Int>?? = .none // expected-warning {{assuming you mean 'Optional<Optional<NestedBazEnumGeneric.Baz<Int>>>.none'; did you mean 'NestedBazEnumGeneric.Baz<Int>.none' instead?}}
// expected-note@-1 {{explicitly specify 'Optional' to silence this warning}} {{42-42=Optional}}
// expected-note@-2 {{use 'NestedBazEnumGeneric.Baz<Int>.none' instead}} {{42-42=NestedBazEnumGeneric.Baz<Int>}}
let _: NestedBazEnumGeneric.Baz<Int> = .none // ok
let _: NestedBazEnumGeneric.Baz<Int> = .one // ok
let _: NestedBazEnumGeneric.Baz<Int>? = .one // ok
let _: NestedBazEnumGeneric.Baz<Int>?? = .one // ok

class C {}
protocol P {}

enum E : C & P {}
// expected-error@-1 {{inheritance from class-constrained protocol composition type 'C & P'}}

// https://github.com/apple/swift/issues/53923

enum EnumWithStaticNone1 {
  case a
  static let none = 1
}

enum EnumWithStaticNone2 {
  case a
  static let none = EnumWithStaticNone2.a
}

enum EnumWithStaticNone3 {
  case a
  static let none = EnumWithStaticNone3.a
  var none: EnumWithStaticNone3 { return .a }
}

enum EnumWithStaticNone4 {
  case a
  var none: EnumWithStaticNone4 { return .a }
  static let none = EnumWithStaticNone4.a
}

enum EnumWithStaticFuncNone1 {
  case a
  static func none() -> Int { return 1 }
}

enum EnumWithStaticFuncNone2 {
  case a
  static func none() -> EnumWithStaticFuncNone2 { return .a }
}

/// Make sure we don't diagnose 'static let none = 1', but do diagnose 'static let none = TheEnum.anotherCase' ///

let _: EnumWithStaticNone1? = .none // Okay
let _: EnumWithStaticNone2? = .none // expected-warning {{assuming you mean 'Optional<EnumWithStaticNone2>.none'; did you mean 'EnumWithStaticNone2.none' instead?}}
// expected-note@-1 {{explicitly specify 'Optional' to silence this warning}}{{31-31=Optional}}
// expected-note@-2 {{use 'EnumWithStaticNone2.none' instead}}{{31-31=EnumWithStaticNone2}}

/// Make sure we diagnose if we have both static and instance 'none' member regardless of source order ///

let _: EnumWithStaticNone3? = .none // expected-warning {{assuming you mean 'Optional<EnumWithStaticNone3>.none'; did you mean 'EnumWithStaticNone3.none' instead?}}
// expected-note@-1 {{explicitly specify 'Optional' to silence this warning}}{{31-31=Optional}}
// expected-note@-2 {{use 'EnumWithStaticNone3.none' instead}}{{31-31=EnumWithStaticNone3}}
let _: EnumWithStaticNone4? = .none // expected-warning {{assuming you mean 'Optional<EnumWithStaticNone4>.none'; did you mean 'EnumWithStaticNone4.none' instead?}}
// expected-note@-1 {{explicitly specify 'Optional' to silence this warning}}{{31-31=Optional}}
// expected-note@-2 {{use 'EnumWithStaticNone4.none' instead}}{{31-31=EnumWithStaticNone4}}

/// Make sure we don't diagnose 'static func none -> T' ///

let _: EnumWithStaticFuncNone1? = .none // Okay
let _: EnumWithStaticFuncNone2? = .none // Okay

/// Make sure we diagnose generic ones as well including conditional ones ///

enum GenericEnumWithStaticNone<T> {
  case a
  static var none: GenericEnumWithStaticNone<Int> { .a }
}

let _: GenericEnumWithStaticNone<Int>? = .none // expected-warning {{assuming you mean 'Optional<GenericEnumWithStaticNone<Int>>.none'; did you mean 'GenericEnumWithStaticNone<Int>.none' instead?}}
// expected-note@-1 {{explicitly specify 'Optional' to silence this warning}}{{42-42=Optional}}
// expected-note@-2 {{use 'GenericEnumWithStaticNone<Int>.none' instead}}{{42-42=GenericEnumWithStaticNone<Int>}}
let _: GenericEnumWithStaticNone<String>? = .none // Okay

let _: GenericEnumWithStaticNone? = .none // expected-warning {{assuming you mean 'GenericEnumWithStaticNone<Int>.none'; did you mean 'Optional<GenericEnumWithStaticNone<Int>>.none' instead?}}
// expected-note@-1 {{use 'Optional<GenericEnumWithStaticNone<Int>>.none' instead}} {{37-37=Optional<GenericEnumWithStaticNone<Int>>}} 
// expected-note@-2 {{use 'GenericEnumWithStaticNone<Int>.none' instead}} {{37-37=GenericEnumWithStaticNone<Int>}} 

enum GenericStructWithStaticNone<T> {
  init() {}
  static var none: GenericStructWithStaticNone<Int> { GenericStructWithStaticNone<Int>() }
}

let _: GenericStructWithStaticNone<Int>? = .none // expected-warning {{assuming you mean 'Optional<GenericStructWithStaticNone<Int>>.none'; did you mean 'GenericStructWithStaticNone<Int>.none' instead?}}
// expected-note@-1 {{explicitly specify 'Optional' to silence this warning}}{{44-44=Optional}}
// expected-note@-2 {{use 'GenericStructWithStaticNone<Int>.none' instead}}{{44-44=GenericStructWithStaticNone<Int>}}
let _: GenericStructWithStaticNone<String>? = .none // Okay

let _: GenericStructWithStaticNone? = .none // expected-warning {{assuming you mean 'GenericStructWithStaticNone<Int>.none'; did you mean 'Optional<GenericStructWithStaticNone<Int>>.none' instead?}}
// expected-note@-1 {{use 'Optional<GenericStructWithStaticNone<Int>>.none' instead}} {{39-39=Optional<GenericStructWithStaticNone<Int>>}} 
// expected-note@-2 {{use 'GenericStructWithStaticNone<Int>.none' instead}} {{39-39=GenericStructWithStaticNone<Int>}} 

enum GenericEnumWithoutNone<T> {
  case a
}

extension GenericEnumWithoutNone where T == Int {
  static var none: GenericEnumWithoutNone<Int> { .a }
}

let _: GenericEnumWithoutNone<Int>? = .none // expected-warning {{assuming you mean 'Optional<GenericEnumWithoutNone<Int>>.none'; did you mean 'GenericEnumWithoutNone<Int>.none' instead?}}
// expected-note@-1 {{explicitly specify 'Optional' to silence this warning}}{{39-39=Optional}}
// expected-note@-2 {{use 'GenericEnumWithoutNone<Int>.none' instead}}{{39-39=GenericEnumWithoutNone<Int>}}
let _: GenericEnumWithoutNone<String>? = .none // Okay

// A couple of edge cases that shouldn't trigger the warning //

enum EnumWithStructNone {
  case bar
  struct none {}
}

enum EnumWithTypealiasNone {
  case bar
  typealias none = EnumWithTypealiasNone
}

enum EnumWithBothStructAndComputedNone {
  case bar
  struct none {}
  var none: EnumWithBothStructAndComputedNone { . bar }
}

enum EnumWithBothTypealiasAndComputedNone {
  case bar
  typealias none = EnumWithBothTypealiasAndComputedNone
  var none: EnumWithBothTypealiasAndComputedNone { . bar }
}

let _: EnumWithStructNone? = .none // Okay
let _: EnumWithTypealiasNone? = .none // Okay
let _: EnumWithBothStructAndComputedNone? = .none // Okay
let _: EnumWithBothTypealiasAndComputedNone? = .none // Okay

// https://github.com/apple/swift/issues/54499

let foo1: Foo? = Foo.none
let foo2: Foo?? = Foo.none

switch foo1 {
  case .none: break 
  // expected-warning@-1 {{assuming you mean 'Optional<Foo>.none'; did you mean 'Foo.none' instead?}}
  // expected-note@-2 {{use 'nil' to silence this warning}}{{8-13=nil}}
  // expected-note@-3 {{use 'none?' instead}}{{9-13=none?}}
  case .bar: break
  default: break
}

switch foo2 {
  case .none: break 
  // expected-warning@-1 {{assuming you mean 'Optional<Optional<Foo>>.none'; did you mean 'Foo.none' instead?}}
  // expected-note@-2 {{use 'nil' to silence this warning}}{{8-13=nil}}
  // expected-note@-3 {{use 'none??' instead}}{{9-13=none??}}
  case .bar: break
  default: break
}

if case .none = foo1 {}
// expected-warning@-1 {{assuming you mean 'Optional<Foo>.none'; did you mean 'Foo.none' instead?}}
// expected-note@-2 {{use 'nil' to silence this warning}}{{9-14=nil}}
// expected-note@-3 {{use 'none?' instead}}{{10-14=none?}}

if case .none = foo2 {}
// expected-warning@-1 {{assuming you mean 'Optional<Optional<Foo>>.none'; did you mean 'Foo.none' instead?}}
// expected-note@-2 {{use 'nil' to silence this warning}}{{9-14=nil}}
// expected-note@-3 {{use 'none??' instead}}{{10-14=none??}}

switch foo1 {
  case nil: break // Okay
  case .bar: break
  default: break
}

switch foo1 {
  case .none?: break // Okay
  case .bar: break
  default: break
}

switch foo2 {
  case nil: break // Okay
  case .bar: break
  default: break
}

switch foo2 {
  case .none??: break // Okay
  case .bar: break
  default: break
}

if case nil = foo1 {} // Okay
if case .none? = foo1 {} // Okay
if case nil = foo2 {} // Okay
if case .none?? = foo2 {} // Okay
