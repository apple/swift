// RUN: %target-typecheck-verify-swift

var global = 42

@dynamicMemberLookup
struct Gettable {
  subscript(dynamicMember member: StaticString) -> Int {
    return 42
  }
}

@dynamicMemberLookup
struct Settable {
  subscript(dynamicMember member: StaticString) -> Int {
    get {return 42}
    set {}
  }
}

@dynamicMemberLookup
struct MutGettable {
  subscript(dynamicMember member: StaticString) -> Int {
    mutating get {
      return 42
    }
  }
}

@dynamicMemberLookup
struct NonMutSettable {
  subscript(dynamicMember member: StaticString) -> Int {
    get { return 42 }
    nonmutating set {}
  }
}

func test(a: Gettable, b: Settable, c: MutGettable, d: NonMutSettable) {
  global = a.wyverns
  a.flavor = global // expected-error {{cannot assign through dynamic lookup property: 'a' is a 'let' constant}}

  global = b.flavor
  b.universal = global // expected-error {{cannot assign through dynamic lookup property: 'b' is a 'let' constant}}
  b.thing += 1 // expected-error {{left side of mutating operator isn't mutable: 'b' is a 'let' constant}}

  var bm = b
  global = bm.flavor
  bm.universal = global
  bm.thing += 1

  var cm = c
  global = c.dragons // expected-error {{cannot use mutating getter on immutable value: 'c' is a 'let' constant}}
  global = c[dynamicMember: "dragons"] // expected-error {{cannot use mutating getter on immutable value: 'c' is a 'let' constant}}
  global = cm.dragons
  c.woof = global // expected-error {{cannot use mutating getter on immutable value: 'c' is a 'let' constant}}

  var dm = d
  global = d.dragons  // ok
  global = dm.dragons // ok
  d.woof = global     // ok
  dm.woof = global    // ok
}

func testIUO(a: Gettable!, b: Settable!) {
  global = a.wyverns
  a.flavor = global // expected-error {{cannot assign through dynamic lookup property: 'a' is a 'let' constant}}

  global = b.flavor
  b.universal = global // expected-error {{cannot assign through dynamic lookup property: 'b' is a 'let' constant}}

  var bm: Settable! = b

  global = bm.flavor
  bm.universal = global
}

//===----------------------------------------------------------------------===//
// Returning a function
//===----------------------------------------------------------------------===//

@dynamicMemberLookup
struct FnTest {
  subscript(dynamicMember member: StaticString) -> (Int) -> () {
    return { x in () }
  }
}
func testFunction(x: FnTest) {
  x.phunky(12)
}
func testFunctionIUO(x: FnTest!) {
  x.flavor(12)
}

//===----------------------------------------------------------------------===//
// Explicitly declared members take precedence
//===----------------------------------------------------------------------===//

@dynamicMemberLookup
struct Dog {
  public var name = "Kaylee"

  subscript(dynamicMember member: String) -> String {
    return "Zoey"
  }
}

func testDog(person: Dog) -> String {
  return person.name + person.otherName
}

//===----------------------------------------------------------------------===//
// Returning an IUO
//===----------------------------------------------------------------------===//

@dynamicMemberLookup
struct IUOResult {
  subscript(dynamicMember member: StaticString) -> Int! {
    get { return 42 }
    nonmutating set {}
  }
}

func testIUOResult(x: IUOResult) {
  x.foo?.negate() // Test mutating writeback.

  let _: Int = x.bar // Test implicitly forced optional
  let b = x.bar
  // expected-note@-1{{short-circuit}}
  // expected-note@-2{{coalesce}}
  // expected-note@-3{{force-unwrap}}

  let _: Int = b // expected-error {{value of optional type 'Int?' must be unwrapped to a value of type 'Int'}}
  // expected-note@-1{{coalesce}}
  // expected-note@-2{{force-unwrap}}
}

//===----------------------------------------------------------------------===//
// Error cases
//===----------------------------------------------------------------------===//

// Subscript index must be ExpressibleByStringLiteral.
@dynamicMemberLookup
struct Invalid1 {
  // expected-error @+1 {{@dynamicMemberLookup attribute requires 'Invalid1' to have a 'subscript(dynamicMember:)' method that accepts either 'ExpressibleByStringLiteral' or a key path}}
  subscript(dynamicMember member: Int) -> Int {
    return 42
  }
}

// Subscript may not be variadic.
@dynamicMemberLookup
struct Invalid2 {
  // expected-error @+1 {{@dynamicMemberLookup attribute requires 'Invalid2' to have a 'subscript(dynamicMember:)' method that accepts either 'ExpressibleByStringLiteral' or a key path}}
  subscript(dynamicMember member: String...) -> Int {
    return 42
  }
}

// References to overloads are resolved just like normal subscript lookup:
// they are either contextually disambiguated or are invalid.
@dynamicMemberLookup
struct Ambiguity {
  subscript(dynamicMember member: String) -> Int {
    return 42
  }
  subscript(dynamicMember member: String) -> Float {
    return 42
  }
}

func testAmbiguity(a: Ambiguity) {
  let _: Int = a.flexibility
  let _: Float = a.dynamism
  _ = a.dynamism // expected-error {{ambiguous use of 'subscript(dynamicMember:)'}}
}

// expected-error @+1 {{'@dynamicMemberLookup' attribute cannot be applied to this declaration}}
@dynamicMemberLookup
extension Int {
  subscript(dynamicMember member: String) -> Int {
    fatalError()
  }
}

// expected-error @+1 {{'@dynamicMemberLookup' attribute cannot be applied to this declaration}}
@dynamicMemberLookup
func NotAllowedOnFunc() {}

// @dynamicMemberLookup cannot be declared on a base class and fulfilled with a
// derived class.

// expected-error @+1 {{@dynamicMemberLookup attribute requires 'InvalidBase' to have a 'subscript(dynamicMember:)' method that accepts either 'ExpressibleByStringLiteral' or a key path}}
@dynamicMemberLookup
class InvalidBase {}

class InvalidDerived : InvalidBase { subscript(dynamicMember: String) -> Int { get {}} }

// expected-error @+1 {{value of type 'InvalidDerived' has no member 'dynamicallyLookedUp'}}
_ = InvalidDerived().dynamicallyLookedUp

//===----------------------------------------------------------------------===//
// Existentials
//===----------------------------------------------------------------------===//

@dynamicMemberLookup
protocol DynamicProtocol {
  subscript(dynamicMember member: StaticString) -> DynamicProtocol {
    get nonmutating set
  }
}

struct MyDynamicStruct : DynamicProtocol {
  subscript(dynamicMember member: StaticString) -> DynamicProtocol {
    get { fatalError() }
    nonmutating set {}
  }
}

func testMutableExistential(a: DynamicProtocol,
                            b: MyDynamicStruct) -> DynamicProtocol {
  a.x.y = b
  b.x.y = b
  return a.foo.bar.baz
}

// Verify protocol compositions and protocol refinements work.
protocol SubDynamicProtocol : DynamicProtocol {}
typealias ProtocolComp = AnyObject & DynamicProtocol

func testMutableExistential2(a: AnyObject & DynamicProtocol,
                             b: SubDynamicProtocol,
                             c: ProtocolComp & AnyObject) {
  a.x.y = b
  b.x.y = b
  c.x.y = b
}

@dynamicMemberLookup
protocol ProtoExt {}
extension ProtoExt {
  subscript(dynamicMember member: String) -> String {
    get {}
  }
}

extension String: ProtoExt {}

func testProtoExt() -> String {
  let str = "test"
  return str.sdfsdfsdf
}

//===----------------------------------------------------------------------===//
// JSON example
//===----------------------------------------------------------------------===//

@dynamicMemberLookup
enum JSON {
  case IntValue(Int)
  case StringValue(String)
  case ArrayValue(Array<JSON>)
  case DictionaryValue(Dictionary<String, JSON>)

  var stringValue: String? {
    if case .StringValue(let str) = self {
      return str
    }
    return nil
  }
  subscript(index: Int) -> JSON? {
    if case .ArrayValue(let arr) = self {
      return index < arr.count ? arr[index] : nil
    }
    return nil
  }
  subscript(key: String) -> JSON? {
    if case .DictionaryValue(let dict) = self {
      return dict[key]
    }
    return nil
  }

  subscript(dynamicMember member: String) -> JSON? {
    if case .DictionaryValue(let dict) = self {
      return dict[member]
    }
    return nil
  }
}
func testJsonExample(x: JSON) -> String? {
  _ = x.name?.first
  return x.name?.first?.stringValue
}

//===----------------------------------------------------------------------===//
// Class inheritance tests
//===----------------------------------------------------------------------===//

@dynamicMemberLookup
class BaseClass {
  subscript(dynamicMember member: String) -> Int {
    return 42
  }
}
class DerivedClass : BaseClass {}

func testDerivedClass(x: BaseClass, y: DerivedClass) -> Int {
  return x.life - y.the + x.universe - y.and + x.everything
}

// Test that derived classes can add a setter.
class DerivedClassWithSetter : BaseClass {
  override subscript(dynamicMember member: String) -> Int {
    get { return super[dynamicMember: member] }
    set {}
  }
}

func testOverrideSubscript(a: BaseClass, b: DerivedClassWithSetter) {
  let x = a.frotz + b.garbalaz
  b.baranozo = x
  a.balboza = 12 // expected-error {{cannot assign through dynamic lookup property: 'a' is a 'let' constant}}
}

//===----------------------------------------------------------------------===//
// Generics
//===----------------------------------------------------------------------===//

@dynamicMemberLookup
struct SettableGeneric1<T> {
  subscript(dynamicMember member: StaticString) -> T? {
    get {}
    nonmutating set {}
  }
}

func testGenericType<T>(a: SettableGeneric1<T>, b: T) -> T? {
  a.dfasdf = b
  return a.dfsdffff
}

func testConcreteGenericType(a: SettableGeneric1<Int>) -> Int? {
  a.dfasdf = 42
  return a.dfsdffff
}

@dynamicMemberLookup
struct SettableGeneric2<T> {
  subscript<U: ExpressibleByStringLiteral>(dynamicMember member: U) -> T {
    get {}
    nonmutating set {}
  }
}

func testGenericType2<T>(a: SettableGeneric2<T>, b: T) -> T? {
  a[dynamicMember: "fasdf"] = b
  a.dfasdf = b
  return a.dfsdffff
}

func testConcreteGenericType2(a: SettableGeneric2<Int>) -> Int? {
  a.dfasdf = 42
  return a.dfsdffff
}

/// https://github.com/apple/swift/issues/50610
/// `subscript(dynamicMember:)` works as a `@dynamicMemberLookup` protocol
/// requirement

@dynamicMemberLookup
protocol GenericProtocol {
  associatedtype S: ExpressibleByStringLiteral
  associatedtype T
  subscript(dynamicMember member: S) -> T { get }
}

@dynamicMemberLookup
class GenericClass<S: ExpressibleByStringLiteral, T> {
  let t: T
  init(_ t: T) { self.t = t }
  subscript(dynamicMember member: S) -> T { return t }
}

func testGenerics<S, T, P: GenericProtocol>(
  a: P,
  b: AnyObject & GenericClass<S, T>
) where P.S == S, P.T == T {
  let _: T = a.wew
  let _: T = b.lad
}

//===----------------------------------------------------------------------===//
// Keypaths
//===----------------------------------------------------------------------===//

@dynamicMemberLookup
class KP {
  subscript(dynamicMember member: String) -> Int { return 7 }
}
_ = \KP.[dynamicMember: "hi"]
_ = \KP.testLookup

/* KeyPath based dynamic lookup */

struct Point {
  var x: Int
  let y: Int // expected-note 2 {{change 'let' to 'var' to make it mutable}}

  private let z: Int = 0 // expected-note 10 {{declared here}}
}

struct Rectangle {
  var topLeft, bottomRight: Point
}

@dynamicMemberLookup
struct Lens<T> {
  var obj: T

  init(_ obj: T) {
    self.obj = obj
  }

  subscript<U>(dynamicMember member: KeyPath<T, U>) -> Lens<U> {
    get { return Lens<U>(obj[keyPath: member]) }
  }

  subscript<U>(dynamicMember member: WritableKeyPath<T, U>) -> Lens<U> {
    get { return Lens<U>(obj[keyPath: member]) }
    set { obj[keyPath: member] = newValue.obj }
  }
}

var topLeft = Point(x: 0, y: 0)
var bottomRight = Point(x: 10, y: 10)

var lens = Lens(Rectangle(topLeft: topLeft,
                          bottomRight: bottomRight))

_ = lens.topLeft
_ = lens.topLeft.x
_ = lens.topLeft.y
_ = lens.topLeft.z // expected-error {{'z' is inaccessible due to 'private' protection level}}

_ = lens.bottomRight
_ = lens.bottomRight.x
_ = lens.bottomRight.y
_ = lens.bottomRight.z // expected-error {{'z' is inaccessible due to 'private' protection level}}

_ = \Lens<Point>.x
_ = \Lens<Point>.y
_ = \Lens<Point>.z // expected-error {{'z' is inaccessible due to 'private' protection level}}
_ = \Lens<Rectangle>.topLeft.x
_ = \Lens<Rectangle>.topLeft.y
_ = \Lens<Rectangle>.topLeft.z // expected-error {{'z' is inaccessible due to 'private' protection level}}
_ = \Lens<[Int]>.count
_ = \Lens<[Int]>.[0]
_ = \Lens<[[Int]]>.[0].count

lens.topLeft = Lens(Point(x: 1, y: 2)) // Ok
lens.bottomRight.x = Lens(11)          // Ok
lens.bottomRight.y = Lens(12)          // expected-error {{cannot assign to property: 'y' is a 'let' constant}}
lens.bottomRight.z = Lens(13)          // expected-error {{'z' is inaccessible due to 'private' protection level}}

func acceptKeyPathDynamicLookup(_: Lens<Int>) {}

acceptKeyPathDynamicLookup(lens.topLeft.x)
acceptKeyPathDynamicLookup(lens.topLeft.y)
acceptKeyPathDynamicLookup(lens.topLeft.z) // expected-error {{'z' is inaccessible due to 'private' protection level}}

var tupleLens = Lens<(String, Int)>(("ultimate question", 42))
_ = tupleLens.0.count
_ = tupleLens.1

var namedTupleLens = Lens<(question: String, answer: Int)>((question: "ultimate question", answer: 42))
_ = namedTupleLens.question.count
_ = namedTupleLens.answer

@dynamicMemberLookup
class A<T> {
  var value: T

  init(_ v: T) {
    self.value = v
  }

  subscript<U>(dynamicMember member: KeyPath<T, U>) -> U {
    get { return value[keyPath: member] }
  }
}

// Let's make sure that keypath dynamic member lookup
// works with inheritance

class B<T> : A<T> {}

func bar(_ b: B<Point>) {
  let _: Int = b.x
  let _ = b.y
  let _: Float = b.y // expected-error {{cannot convert value of type 'Int' to specified type 'Float'}}
  let _ = b.z // expected-error {{'z' is inaccessible due to 'private' protection level}}
}

// Existentials and IUOs

@dynamicMemberLookup
protocol KeyPathLookup {
  associatedtype T

  var value: T { get }

  subscript(dynamicMember member: KeyPath<T, Int>) -> Int! { get }
}

extension KeyPathLookup {
  subscript(dynamicMember member: KeyPath<T, Int>) -> Int! {
    get { return value[keyPath: member] }
  }
}

class C<T> : KeyPathLookup {
  var value: T
  init(_ v: T) {
    self.value = v
  }
}

func baz(_ c: C<Point>) {
  let _: Int = c.x
  let _ = c.y
  let _: Float = c.y // expected-error {{cannot convert value of type 'Int?' to specified type 'Float'}}
  let _ = c.z // expected-error {{'z' is inaccessible due to 'private' protection level}}
}

@dynamicMemberLookup
class D<T> {
  var value: T

  init(_ v: T) {
    self.value = v
  }

  subscript<U: Numeric>(dynamicMember member: KeyPath<T, U>) -> (U) -> U {
    get { return { offset in self.value[keyPath: member] + offset } }
  }
}

func faz(_ d: D<Point>) {
  let _: Int = d.x(42)
  let _ = d.y(1 + 0)
  let _: Float = d.y(1 + 0) // expected-error {{cannot convert value of type 'Int' to specified type 'Float'}}
  let _ = d.z(1 + 0)        // expected-error {{'z' is inaccessible due to 'private' protection level}}
}

@dynamicMemberLookup
struct SubscriptLens<T> {
  var value: T

  var counter: Int = 0

  subscript(foo: String) -> Int {
    get { return 42 }
  }

  subscript(offset: Int) -> Int {
    get { return counter }
    set { counter = counter + newValue }
  }

  subscript<U>(dynamicMember member: KeyPath<T, U>) -> U! {
    get { return value[keyPath: member] }
  }

  subscript<U>(dynamicMember member: WritableKeyPath<T, U>) -> U {
    get { return value[keyPath: member] }
    set { value[keyPath: member] = newValue }
  }
}

func keypath_with_subscripts(_ arr: SubscriptLens<[Int]>,
                             _ dict: inout SubscriptLens<[String: Int]>) {
  _ = arr[0..<3]
  for idx in 0..<arr.count {
    let _ = arr[idx]
    print(arr[idx])
  }

  _ = arr["hello"]  // Ok
  _ = dict["hello"] // Ok

  _ = arr["hello"] = 42 // expected-error {{cannot assign through subscript: subscript is get-only}}
  _ = dict["hello"] = 0 // Ok

  _ = arr[0] = 42 // expected-error {{cannot assign through subscript: 'arr' is a 'let' constant}}
  _ = dict[0] = 1 // Ok

  if let index = dict.value.firstIndex(where: { $0.value == 42 }) {
    let _ = dict[index]
  }
  dict["ultimate question"] = 42
}

func keypath_with_incorrect_return_type(_ arr: Lens<Array<Int>>) {
  for idx in 0..<arr.count {
    // expected-error@-1 {{cannot convert value of type 'Lens<Int>' to expected argument type 'Int'}}
    let _ = arr[idx]
  }
}

struct WithTrailingClosure {
  subscript(fn: () -> Int) -> Int {
    get { return fn() }
    nonmutating set { _ = fn() + newValue }
  }

  subscript(offset: Int, _ fn: () -> Int) -> Int {
    get { return offset + fn() }
  }
}

func keypath_with_trailing_closure_subscript(_ ty: inout SubscriptLens<WithTrailingClosure>) {
  _ = ty[0] { 42 } // expected-error {{subscript index of type '() -> Int' in a key path must be Hashable}}
  _ = ty[0] { 42 } = 0 // expected-error {{cannot assign through subscript: subscript is get-only}}
  // expected-error@-1 {{subscript index of type '() -> Int' in a key path must be Hashable}}
  _ = ty[] { 42 }  // expected-error {{subscript index of type '() -> Int' in a key path must be Hashable}}
  _ = ty[] { 42 } = 0 // expected-error {{subscript index of type '() -> Int' in a key path must be Hashable}}
}

func keypath_to_subscript_to_property(_ lens: inout Lens<Array<Rectangle>>) {
  _ = lens[0].topLeft.x
  _ = lens[0].topLeft.y
  _ = lens[0].topLeft.x = Lens(0)
  _ = lens[0].topLeft.y = Lens(1)
  // expected-error@-1 {{cannot assign to property: 'y' is a 'let' constant}}
}

@dynamicMemberLookup
struct SingleChoiceLens<T> {
  var obj: T

  init(_ obj: T) {
    self.obj = obj
  }

  subscript<U>(dynamicMember member: WritableKeyPath<T, U>) -> U {
    get { return obj[keyPath: member] }
    set { obj[keyPath: member] = newValue }
  }
}

// Make sure that disjunction filtering optimization doesn't
// impede keypath dynamic member lookup by eagerly trying to
// simplify disjunctions with a single viable choice.
func test_lens_with_a_single_choice(a: inout SingleChoiceLens<[Int]>) {
  a[0] = 1 // Ok
}

func test_chain_of_recursive_lookups(_ lens: Lens<Lens<Lens<Point>>>) {
  _ = lens.x
  _ = lens.y
  _ = lens.z // expected-error {{'z' is inaccessible due to 'private' protection level}}
  // Make sure that 'obj' field could be retrieved at any level
  _ = lens.obj
  _ = lens.obj.obj
  _ = lens.obj.x
  _ = lens.obj.obj.x

  _ = \Lens<Lens<Point>>.x
  _ = \Lens<Lens<Point>>.obj.x
}

// KeyPath Dynamic Member Lookup can't refer to methods, mutating setters and static members
// because of the KeyPath limitations
func invalid_refs_through_dynamic_lookup() {
  struct S {
    static var foo: Int = 42
    func bar() -> Q { return Q() }
    static func baz(_: String) -> Int { return 0 }
  }

  struct Q {
    var faz: Int = 0
  }

  func test(_ lens: A<S>) {
    _ = lens.foo           // expected-error {{static member 'foo' cannot be used on instance of type 'S'}}
    _ = lens.bar()         // expected-error {{dynamic key path member lookup cannot refer to instance method 'bar()'}}
    _ = lens.bar().faz + 1 // expected-error {{dynamic key path member lookup cannot refer to instance method 'bar()'}}
    _ = lens.baz("hello")  // expected-error {{dynamic key path member lookup cannot refer to static method 'baz'}}
  }
}

// https://github.com/apple/swift/issues/52997

final class C1_52997 {}

@dynamicMemberLookup
struct S1_52997<T> {
  var obj: T
  init(_ obj: T) { self.obj = obj }
  subscript<U>(dynamicMember member: KeyPath<T, U>) -> U {
    return obj[keyPath: member]
  }
  var wooo: C1_52997 { C1_52997() } // expected-note {{declared here}}
}
do {
  _ = S1_52997<C1_52997>(C1_52997()).wooooo
  // expected-error@-1 {{value of type 'S1_52997<C1_52997>' has no dynamic member 'wooooo' using key path from root type 'C1_52997'; did you mean 'wooo'?}}
  _ = S1_52997<C1_52997>(C1_52997()).bla
  // expected-error@-1 {{value of type 'S1_52997<C1_52997>' has no dynamic member 'bla' using key path from root type 'C1_52997'}}
}

final class C2_52997 {
  var woo: Int? // expected-note 2 {{'woo' declared here}}
}

@dynamicMemberLookup
struct S2_52997<T> {
  var obj: T
  init(_ obj: T) { self.obj = obj }
  subscript<U>(dynamicMember member: KeyPath<T, U>) -> U {
    return obj[keyPath: member]
  }
}
do {
  _ = S2_52997<C2_52997>(C2_52997()).wooo
  // expected-error@-1 {{value of type 'S2_52997<C2_52997>' has no dynamic member 'wooo' using key path from root type 'C2_52997'; did you mean 'woo'?}}
  _ = S2_52997<C2_52997>(C2_52997()).bla
  // expected-error@-1 {{value of type 'S2_52997<C2_52997>' has no dynamic member 'bla' using key path from root type 'C2_52997'}}
}

// https://github.com/apple/swift/issues/52957

@dynamicMemberLookup
struct S1_52957 {
  subscript(dynamicMember: String) -> String { // expected-error {{@dynamicMemberLookup attribute requires 'S1_52957' to have a 'subscript(dynamicMember:)' method that accepts either 'ExpressibleByStringLiteral' or a key path}}
  // expected-note@-1 {{add an explicit argument label to this subscript to satisfy the @dynamicMemberLookup requirement}}{{13-13=dynamicMember }}
    fatalError()
  }
}

@dynamicMemberLookup
struct S2_52957 {
  subscript(foo bar: String) -> String { // expected-error {{@dynamicMemberLookup attribute requires 'S2_52957' to have a 'subscript(dynamicMember:)' method that accepts either 'ExpressibleByStringLiteral' or a key path}}
    fatalError()
  }

  subscript(foo: String) -> String { // expected-error {{@dynamicMemberLookup attribute requires 'S2_52957' to have a 'subscript(dynamicMember:)' method that accepts either 'ExpressibleByStringLiteral' or a key path}}
  // expected-note@-1 {{add an explicit argument label to this subscript to satisfy the @dynamicMemberLookup requirement}} {{13-13=dynamicMember }}
    fatalError()
  }
}

// https://github.com/apple/swift/issues/54292

@dynamicMemberLookup
struct S_54292 {
  subscript(dynamicMember member: Substring) -> Int { 0 }
}
do {
  _ = \S_54292.okay
}

func test_infinite_self_recursion() {
  @dynamicMemberLookup
  struct Recurse<T> {
    subscript<U>(dynamicMember member: KeyPath<Recurse<T>, U>) -> Int {
      return 1
    }
  }

  _ = Recurse<Int>().foo
  // expected-error@-1 {{value of type 'Recurse<Int>' has no dynamic member 'foo' using key path from root type 'Recurse<Int>'}}
}

// rdar://problem/60225883 - crash during solution application (ExprRewriter::buildKeyPathDynamicMemberIndexExpr)
func test_combination_of_keypath_and_string_lookups() {
  @dynamicMemberLookup
  struct Outer {
    subscript(dynamicMember member: String) -> Outer {
      Outer()
    }

    subscript(dynamicMember member: KeyPath<Inner, Inner>) -> Outer {
      Outer()
    }
  }

  @dynamicMemberLookup
  struct Inner {
    subscript(dynamicMember member: String) -> Inner {
      Inner()
    }
  }

  func test(outer: Outer) {
    _ = outer.hello.world // Ok
  }
}

// https://github.com/apple/swift/issues/55070

@dynamicMemberLookup
struct S_55070 {
  var i: Int

  subscript(dynamicMember member: KeyPath<S_55070, Int>) -> Int {
    get { self[keyPath: member] }
    set { self[keyPath: member] = newValue } // expected-error {{cannot assign through subscript: 'member' is a read-only key path}}
  }
}

// https://github.com/apple/swift/issues/54864

public struct S1_54864 {}

@dynamicMemberLookup
public struct S2_54864 {}

internal var s2_54864_instance: S2_54864 = S2_54864()

public extension S2_54864 {
  subscript<T>(dynamicMember member: WritableKeyPath<S1_54864, T>) -> T {
      get { s2_54864_instance[keyPath: member] } // expected-error {{key path with root type 'S1_54864' cannot be applied to a base of type 'S2_54864'}}
      set { s2_54864_instance[keyPath: member] = newValue } // expected-error {{key path with root type 'S1_54864' cannot be applied to a base of type 'S2_54864'}}
  }
}

@dynamicMemberLookup
public struct S3_54864 {}

public extension S3_54864 {
  subscript<T>(dynamicMember member: KeyPath<S3_54864, T>) -> T {
    get { s2_54864_instance[keyPath: member] } // expected-error {{key path with root type 'S3_54864' cannot be applied to a base of type 'S2_54864'}}
  }
}
