// RUN: %target-typecheck-verify-swift -swift-version 5 -package-name myPkg

// ---------------------------------------------------------------------------
// Property wrapper type definitions
// ---------------------------------------------------------------------------
@propertyWrapper
struct Wrapper<T> {
  private var _stored: T
  init(stored: T) {
    self._stored = stored
  }

  var wrappedValue: T {
    get { _stored }
    set { _stored = newValue }
  }
}

@propertyWrapper
struct WrapperWithInitialValue<T> {
  var wrappedValue: T

  init(wrappedValue initialValue: T) {
    self.wrappedValue = initialValue
  }
}

@propertyWrapper
struct WrapperWithDefaultInit<T> {
  private var stored: T?

  var wrappedValue: T {
    get { stored! }
    set { stored = newValue }
  }

  init() {
    self.stored = nil
  }
}

@propertyWrapper
struct WrapperAcceptingAutoclosure<T> {
  private let fn: () -> T

  var wrappedValue: T {
    return fn()
  }

  init(wrappedValue fn: @autoclosure @escaping () -> T) {
    self.fn = fn
  }

  init(body fn: @escaping () -> T) {
    self.fn = fn
  }
}

@propertyWrapper
struct MissingValue<T> { }
// expected-error@-1{{property wrapper type 'MissingValue' does not contain a non-static property named 'wrappedValue'}} {{educational-notes=property-wrapper-requirements}}{{25-25=\nvar wrappedValue: <#Value#>}}

@propertyWrapper
struct StaticValue {
  static var wrappedValue: Int = 17
}
// expected-error@-3{{property wrapper type 'StaticValue' does not contain a non-static property named 'wrappedValue'}}{{21-21=\nvar wrappedValue: <#Value#>}}


// expected-error@+1{{'@propertyWrapper' attribute cannot be applied to this declaration}}
@propertyWrapper
protocol CannotBeAWrapper {
  associatedtype Value
  var wrappedValue: Value { get set }
}

@propertyWrapper
struct NonVisibleValueWrapper<Value> {
  private var wrappedValue: Value // expected-error{{private property 'wrappedValue' cannot have more restrictive access than its enclosing property wrapper type 'NonVisibleValueWrapper' (which is internal)}} {{educational-notes=property-wrapper-requirements}}
}

@propertyWrapper
struct NonVisibleInitWrapper<Value> {
  var wrappedValue: Value

  private init(wrappedValue initialValue: Value) { // expected-error{{private initializer 'init(wrappedValue:)' cannot have more restrictive access than its enclosing property wrapper type 'NonVisibleInitWrapper' (which is internal)}}
    self.wrappedValue = initialValue
  }
}

@propertyWrapper
struct InitialValueTypeMismatch<Value> {
  var wrappedValue: Value // expected-note{{'wrappedValue' declared here}}

  init(wrappedValue initialValue: Value?) { // expected-error{{'init(wrappedValue:)' parameter type ('Value?') must be the same as its 'wrappedValue' property type ('Value') or an @autoclosure thereof}} {{educational-notes=property-wrapper-requirements}}
    self.wrappedValue = initialValue!
  }
}

@propertyWrapper
struct MultipleInitialValues<Value> {
  var wrappedValue: Value? = nil // expected-note 2{{'wrappedValue' declared here}}

  init(wrappedValue initialValue: Int) { // expected-error{{'init(wrappedValue:)' parameter type ('Int') must be the same as its 'wrappedValue' property type ('Value?') or an @autoclosure thereof}}
  }

  init(wrappedValue initialValue: Double) { // expected-error{{'init(wrappedValue:)' parameter type ('Double') must be the same as its 'wrappedValue' property type ('Value?') or an @autoclosure thereof}}
  }
}

@propertyWrapper
struct InitialValueFailable<Value> {
  var wrappedValue: Value

  init?(wrappedValue initialValue: Value) { // expected-error{{property wrapper initializer 'init(wrappedValue:)' cannot be failable}} {{educational-notes=property-wrapper-requirements}}
    return nil
  }
}

@propertyWrapper
struct InitialValueFailableIUO<Value> {
  var wrappedValue: Value

  init!(wrappedValue initialValue: Value) {  // expected-error{{property wrapper initializer 'init(wrappedValue:)' cannot be failable}}
    return nil
  }
}

// ---------------------------------------------------------------------------
// Property wrapper type definitions
// ---------------------------------------------------------------------------
@propertyWrapper
struct _lowercaseWrapper<T> {
  var wrappedValue: T
}

@propertyWrapper
struct _UppercaseWrapper<T> {
  var wrappedValue: T
}

// ---------------------------------------------------------------------------
// Local property wrappers
// ---------------------------------------------------------------------------

func testLocalContext() {
  @WrapperWithInitialValue
  var x = 17
  x = 42
  let _: Int = x
  let _: WrapperWithInitialValue = _x

  @WrapperWithInitialValue(wrappedValue: 17)
  var initialValue
  let _: Int = initialValue
  let _: WrapperWithInitialValue = _initialValue

  @Clamping(min: 0, max: 100)
  var percent = 50
  let _: Int = percent
  let _: Clamping = _percent

  @WrapperA @WrapperB
  var composed = "hello"
  let _: WrapperA<WrapperB> = _composed

  @WrapperWithStorageRef
  var hasProjection = 10
  let _: Wrapper = $hasProjection

  @WrapperWithInitialValue
  var uninitialized: Int { // expected-error {{non-member observing properties require an initializer}}
    didSet {}
  }
}

// ---------------------------------------------------------------------------
// Limitations on where property wrappers can be used
// ---------------------------------------------------------------------------

enum SomeEnum {
  case foo

  @Wrapper(stored: 17)
  var bar: Int // expected-error{{property 'bar' declared inside an enum cannot have a wrapper}}
  // expected-error@-1{{enums must not contain stored properties}}

  @Wrapper(stored: 17)
  static var x: Int
}

protocol SomeProtocol {
  @Wrapper(stored: 17)
  var bar: Int // expected-error{{property 'bar' declared inside a protocol cannot have a wrapper}}
  // expected-error@-1{{property in protocol must have explicit { get } or { get set } specifier}}

  @Wrapper(stored: 17)
  static var x: Int // expected-error{{property 'x' declared inside a protocol cannot have a wrapper}}
  // expected-error@-1{{property in protocol must have explicit { get } or { get set } specifier}}
}

struct HasWrapper { }

extension HasWrapper {
  @Wrapper(stored: 17)
  var inExt: Int // expected-error{{property 'inExt' declared inside an extension cannot have a wrapper}}
  // expected-error@-1{{extensions must not contain stored properties}}

  @Wrapper(stored: 17)
  static var x: Int
}

class ClassWithWrappers {
  @Wrapper(stored: 17)
  var x: Int
}

class Superclass {
  var x: Int = 0
}

class SubclassOfClassWithWrappers: ClassWithWrappers {
  override var x: Int {
    get { return super.x }
    set { super.x = newValue }
  }
}

class SubclassWithWrapper: Superclass {
  @Wrapper(stored: 17)
  override var x: Int { get { return 0 } set { } } // expected-error{{property 'x' with attached wrapper cannot override another property}}
}

class C { }

struct BadCombinations {
  @WrapperWithInitialValue
  lazy var x: C = C() // expected-error{{property 'x' with a wrapper cannot also be lazy}}
  @Wrapper
  weak var y: C? // expected-error{{property 'y' with a wrapper cannot also be weak}}
  @Wrapper
  unowned var z: C // expected-error{{property 'z' with a wrapper cannot also be unowned}}
}

struct MultipleWrappers {
  // FIXME: The diagnostics here aren't great. The problem is that we're
  // attempting to splice a 'wrappedValue:' argument into the call to Wrapper's
  // init, but it doesn't have a matching init. We're then attempting to access
  // the nested 'wrappedValue', but Wrapper's 'wrappedValue' is Int.
  @Wrapper(stored: 17) // expected-error{{cannot convert value of type 'Int' to expected argument type 'WrapperWithInitialValue<Int>'}}
  @WrapperWithInitialValue // expected-error{{extra argument 'wrappedValue' in call}}
  var x: Int = 17

  @WrapperWithInitialValue // expected-error 2{{property wrapper can only apply to a single variable}}
  var (y, z) = (1, 2)
}

// ---------------------------------------------------------------------------
// Initialization
// ---------------------------------------------------------------------------

struct Initialization {
  @Wrapper(stored: 17)
  var x: Int

  @Wrapper(stored: 17)
  var x2: Double

  @Wrapper(stored: 17)
  var x3 = 42 // expected-error {{extra argument 'wrappedValue' in call}}

  @Wrapper(stored: 17)
  var x4

  @WrapperWithInitialValue
  var y = true

  @WrapperWithInitialValue<Int>
  var y2 = true // expected-error{{cannot convert value of type 'Bool' to specified type 'Int'}}

  mutating func checkTypes(s: String) {
    x2 = s // expected-error{{cannot assign value of type 'String' to type 'Double'}}
    x4 = s // expected-error{{cannot assign value of type 'String' to type 'Int'}}
    y = s // expected-error{{cannot assign value of type 'String' to type 'Bool'}}
  }
}

@propertyWrapper
struct Clamping<V: Comparable> {
  var value: V
  let min: V
  let max: V

  init(wrappedValue initialValue: V, min: V, max: V) {
    value = initialValue
    self.min = min
    self.max = max
    assert(value >= min && value <= max)
  }

  var wrappedValue: V {
    get { return value }
    set {
      if newValue < min {
        value = min
      } else if newValue > max {
        value = max
      } else {
        value = newValue
      }
    }
  }
}

struct Color {
  @Clamping(min: 0, max: 255) var red: Int = 127
  @Clamping(min: 0, max: 255) var green: Int = 127
  @Clamping(min: 0, max: 255) var blue: Int = 127
  @Clamping(min: 0, max: 255) var alpha: Int = 255
}

func testColor() {
  _ = Color(green: 17)
}

// ---------------------------------------------------------------------------
// Wrapper type formation
// ---------------------------------------------------------------------------
@propertyWrapper
struct IntWrapper {
  var wrappedValue: Int
}

@propertyWrapper
struct WrapperForHashable<T: Hashable> { // expected-note{{where 'T' = 'NotHashable'}}
  var wrappedValue: T
}

@propertyWrapper
struct WrapperWithTwoParams<T, U> {
  var wrappedValue: (T, U)
}

struct NotHashable { }

struct UseWrappersWithDifferentForm {
  @IntWrapper
  var x: Int

  @WrapperForHashable // expected-error {{generic struct 'WrapperForHashable' requires that 'NotHashable' conform to 'Hashable'}}
  var y: NotHashable

  @WrapperForHashable
  var yOkay: Int

  @WrapperWithTwoParams
  var zOkay: (Int, Float)

  @HasNestedWrapper.NestedWrapper // expected-error {{generic parameter 'T' could not be inferred}} expected-note {{explicitly specify}}
  var w: Int

  @HasNestedWrapper<Double>.NestedWrapper
  var wOkay: Int

  @HasNestedWrapper.ConcreteNestedWrapper
  var wOkay2: Int
}

@propertyWrapper
struct Function<T, U> { // expected-note{{'U' declared as parameter to type 'Function'}}
  var wrappedValue: (T) -> U?
}

struct TestFunction {
  @Function var f: (Int) -> Float?

  // FIXME: This diagnostic should be more specific
  @Function var f2: (Int) -> Float // expected-error {{property type '(Int) -> Float' does not match 'wrappedValue' type '(Int) -> U?'}}
  // expected-error@-1 {{generic parameter 'U' could not be inferred}}
  // expected-note@-2 {{explicitly specify}}

  func test() {
    let _: Int = _f // expected-error{{cannot convert value of type 'Function<Int, Float>' to specified type 'Int'}}
  }
}

// ---------------------------------------------------------------------------
// Nested wrappers
// ---------------------------------------------------------------------------
struct HasNestedWrapper<T> { // expected-note {{'T' declared as parameter to type 'HasNestedWrapper'}}
  @propertyWrapper
  struct NestedWrapper<U> {
    var wrappedValue: U
    init(wrappedValue initialValue: U) {
      self.wrappedValue = initialValue
    }
  }

  @propertyWrapper
  struct ConcreteNestedWrapper {
    var wrappedValue: T
    init(wrappedValue initialValue: T) {
      self.wrappedValue = initialValue
    }
  }

  @NestedWrapper
  var y: [T] = []
}

struct UsesNestedWrapper<V> {
  @HasNestedWrapper<V>.NestedWrapper
  var y: [V]
}

// ---------------------------------------------------------------------------
// Referencing the backing store
// ---------------------------------------------------------------------------
struct BackingStore<T> {
  @Wrapper
  var x: T

  @WrapperWithInitialValue
  private var y = true  // expected-note{{'y' declared here}}

  func getXStorage() -> Wrapper<T> {
    return _x
  }

  func getYStorage() -> WrapperWithInitialValue<Bool> {
    return self._y
  }
}

func testBackingStore<T>(bs: BackingStore<T>) {
  _ = bs.x
  _ = bs.y // expected-error{{'y' is inaccessible due to 'private' protection level}}
}

// ---------------------------------------------------------------------------
// Explicitly-specified accessors
// ---------------------------------------------------------------------------
struct WrapperWithAccessors {
  @Wrapper  // expected-error{{property wrapper cannot be applied to a computed property}}
  var x: Int {
    return 17
  }
}

struct UseWillSetDidSet {
  @Wrapper
  var x: Int {
    willSet {
      print(newValue)
    }
  }

  @Wrapper
  var y: Int {
    didSet {
      print(oldValue)
    }
  }

  @Wrapper
  var z: Int {
    willSet {
      print(newValue)
    }

    didSet {
      print(oldValue)
    }
  }
}

struct DidSetUsesSelf {
  @Wrapper
  var x: Int {
    didSet {
      print(self)
    }
  }
}

// ---------------------------------------------------------------------------
// Mutating/nonmutating
// ---------------------------------------------------------------------------
@propertyWrapper
struct WrapperWithNonMutatingSetter<Value> {
  class Box {
    var wrappedValue: Value
    init(wrappedValue: Value) {
      self.wrappedValue = wrappedValue
    }
  }

  var box: Box

  init(wrappedValue initialValue: Value) {
    self.box = Box(wrappedValue: initialValue)
  }

  var wrappedValue: Value {
    get { return box.wrappedValue }
    nonmutating set { box.wrappedValue = newValue }
  }
}

@propertyWrapper
struct WrapperWithMutatingGetter<Value> {
  var readCount = 0
  var writeCount = 0
  var stored: Value

  init(wrappedValue initialValue: Value) {
    self.stored = initialValue
  }

  var wrappedValue: Value {
    mutating get {
      readCount += 1
      return stored
    }
    set {
      writeCount += 1
      stored = newValue
    }
  }
}

@propertyWrapper
class ClassWrapper<Value> {
  var wrappedValue: Value

  init(wrappedValue initialValue: Value) {
    self.wrappedValue = initialValue
  }
}

struct UseMutatingnessWrappers {
  @WrapperWithNonMutatingSetter
  var x = true

  @WrapperWithMutatingGetter
  var y = 17

  @WrapperWithNonMutatingSetter // expected-error{{property wrapper can only be applied to a 'var'}}
  let z = 3.14159 // expected-note 2{{change 'let' to 'var' to make it mutable}}

  @ClassWrapper
  var w = "Hello"
}

func testMutatingness() {
  var mutable = UseMutatingnessWrappers()

  _ = mutable.x
  mutable.x = false

  _ = mutable.y
  mutable.y = 42

  _ = mutable.z
  mutable.z = 2.71828 // expected-error{{cannot assign to property: 'z' is a 'let' constant}}

  _ = mutable.w
  mutable.w = "Goodbye"

  let nonmutable = UseMutatingnessWrappers() // expected-note 2{{change 'let' to 'var' to make it mutable}}

  // Okay due to nonmutating setter
  _ = nonmutable.x
  nonmutable.x = false

  _ = nonmutable.y // expected-error{{cannot use mutating getter on immutable value: 'nonmutable' is a 'let' constant}}
  nonmutable.y = 42 // expected-error{{cannot use mutating getter on immutable value: 'nonmutable' is a 'let' constant}}

  _ = nonmutable.z
  nonmutable.z = 2.71828 // expected-error{{cannot assign to property: 'z' is a 'let' constant}}

  // Okay due to implicitly nonmutating setter
  _ = nonmutable.w
  nonmutable.w = "World"
}

// ---------------------------------------------------------------------------
// Access control
// ---------------------------------------------------------------------------
struct HasPrivateWrapper<T> {
  @propertyWrapper
  private struct PrivateWrapper<U> { // expected-note{{type declared here}}
    var wrappedValue: U
    init(wrappedValue initialValue: U) {
      self.wrappedValue = initialValue
    }
  }

  @PrivateWrapper
  var y: [T] = []
  // expected-error@-1{{property must be declared private because its property wrapper type uses a private type}}

  // Okay to reference private entities from a private property
  @PrivateWrapper
  private var z: [T]
}

public struct HasUsableFromInlineWrapper<T> {
  @propertyWrapper
  struct InternalWrapper<U> { // expected-note{{type declared here}}
    var wrappedValue: U
    init(wrappedValue initialValue: U) {
      self.wrappedValue = initialValue
    }
  }

  @InternalWrapper
  @usableFromInline
  var y: [T] = []
  // expected-error@-1{{property wrapper type referenced from a '@usableFromInline' property must be '@usableFromInline' or public}}
}

public struct HasUsableFromInlinePackageWrapper<T> {
  @propertyWrapper
  package struct PackageWrapper<U> { // expected-note{{type declared here}}
    package var wrappedValue: U
    package init(wrappedValue initialValue: U) {
      self.wrappedValue = initialValue
    }
  }

  @PackageWrapper
  @usableFromInline
  package var y: [T] = []
  // expected-error@-1{{property wrapper type referenced from a '@usableFromInline' property must be '@usableFromInline' or public}}
}

@propertyWrapper
class Box<Value> {
  private(set) var wrappedValue: Value

  init(wrappedValue initialValue: Value) {
    self.wrappedValue = initialValue
  }
}

struct UseBox {
  @Box
  var x = 17 // expected-note{{'_x' declared here}}
}

func testBox(ub: UseBox) {
  _ = ub.x
  ub.x = 5 // expected-error{{cannot assign to property: 'x' is a get-only property}}

  var mutableUB = ub
  mutableUB = ub
}

func backingVarIsPrivate(ub: UseBox) {
  _ = ub._x // expected-error{{'_x' is inaccessible due to 'private' protection level}}
}

// ---------------------------------------------------------------------------
// Memberwise initializers
// ---------------------------------------------------------------------------
struct MemberwiseInits<T> {
  @Wrapper
  var x: Bool

  @WrapperWithInitialValue
  var y: T
}

func testMemberwiseInits() {
  // expected-error@+1{{type '(Wrapper<Bool>, Double) -> MemberwiseInits<Double>'}}
  let _: Int = MemberwiseInits<Double>.init

  _ = MemberwiseInits(x: Wrapper(stored: true), y: 17)
}

struct DefaultedMemberwiseInits {
  @Wrapper(stored: true)
  var x: Bool

  @WrapperWithInitialValue
  var y: Int = 17

  @WrapperWithInitialValue(wrappedValue: 17)
  var z: Int

  @WrapperWithDefaultInit
  var w: Int

  @WrapperWithDefaultInit
  var optViaDefaultInit: Int?

  @WrapperWithInitialValue
  var optViaInitialValue: Int?
}


struct CannotDefaultMemberwiseOptionalInit { // expected-note{{'init(x:)' declared here}}
  @Wrapper
  var x: Int?
}

func testDefaultedMemberwiseInits() {
  _ = DefaultedMemberwiseInits()
  _ = DefaultedMemberwiseInits(
    x: Wrapper(stored: false),
    y: 42,
    z: WrapperWithInitialValue(wrappedValue: 42))

  _ = DefaultedMemberwiseInits(y: 42)
  _ = DefaultedMemberwiseInits(x: Wrapper(stored: false))
  _ = DefaultedMemberwiseInits(z: WrapperWithInitialValue(wrappedValue: 42))
  _ = DefaultedMemberwiseInits(w: WrapperWithDefaultInit())
  _ = DefaultedMemberwiseInits(optViaDefaultInit: WrapperWithDefaultInit())
  _ = DefaultedMemberwiseInits(optViaInitialValue: nil)
  _ = DefaultedMemberwiseInits(optViaInitialValue: 42)

  _ = CannotDefaultMemberwiseOptionalInit() // expected-error{{missing argument for parameter 'x' in call}}
  _ = CannotDefaultMemberwiseOptionalInit(x: Wrapper(stored: nil))
}

// ---------------------------------------------------------------------------
// Default initializers
// ---------------------------------------------------------------------------
struct DefaultInitializerStruct {
  @Wrapper(stored: true)
  var x

  @WrapperWithInitialValue
  var y: Int = 10
}

struct NoDefaultInitializerStruct { // expected-note{{'init(x:)' declared here}}
  @Wrapper
  var x: Bool
}

class DefaultInitializerClass {
  @Wrapper(stored: true)
  var x

  @WrapperWithInitialValue
  final var y: Int = 10
}

class NoDefaultInitializerClass { // expected-error{{class 'NoDefaultInitializerClass' has no initializers}}
  @Wrapper
  final var x: Bool  // expected-note{{stored property 'x' without initial value prevents synthesized initializers}}
}

func testDefaultInitializers() {
  _ = DefaultInitializerStruct()
  _ = DefaultInitializerClass()
  _ = NoDefaultInitializerStruct() // expected-error{{missing argument for parameter 'x' in call}}
}

struct DefaultedPrivateMemberwiseLets {
  @Wrapper(stored: true)
  private var x: Bool

  @WrapperWithInitialValue
  var y: Int = 17

  @WrapperWithInitialValue(wrappedValue: 17)
  private var z: Int
}

func testDefaultedPrivateMemberwiseLets() {
  _ = DefaultedPrivateMemberwiseLets()
  _ = DefaultedPrivateMemberwiseLets(y: 42)
  _ = DefaultedPrivateMemberwiseLets(x: Wrapper(stored: false)) // expected-error{{argument passed to call that takes no arguments}}
}


// ---------------------------------------------------------------------------
// Storage references
// ---------------------------------------------------------------------------
@propertyWrapper
struct WrapperWithStorageRef<T> {
  var wrappedValue: T

  var projectedValue: Wrapper<T> {
    return Wrapper(stored: wrappedValue)
  }
}

extension Wrapper {
  var wrapperOnlyAPI: Int { return 17 }
}

struct TestStorageRef {
  @WrapperWithStorageRef var x: Int // expected-note{{'_x' declared here}}

  init(x: Int) {
    self._x = WrapperWithStorageRef(wrappedValue: x)
  }

  mutating func test() {
    let _: Wrapper = $x
    let i = $x.wrapperOnlyAPI
    let _: Int = i

    // x is mutable, $x is not
    x = 17
    $x = Wrapper(stored: 42) // expected-error{{cannot assign to property: '$x' is immutable}}
  }
}

func testStorageRef(tsr: TestStorageRef) {
  let _: Wrapper = tsr.$x
  _ = tsr._x // expected-error{{'_x' is inaccessible due to 'private' protection level}}
}

struct TestStorageRefPrivate {
  @WrapperWithStorageRef private(set) var x: Int

  init() {
    self._x = WrapperWithStorageRef(wrappedValue: 5)
  }
}

func testStorageRefPrivate() {
  var tsr = TestStorageRefPrivate()
  let a = tsr.$x // okay, getter is internal
  tsr.$x = a  // expected-error{{cannot assign to property: '$x' is immutable}}
}


// rdar://problem/50873275 - crash when using wrapper with projectedValue in
// generic type.
@propertyWrapper
struct InitialValueWrapperWithStorageRef<T> {
  var wrappedValue: T

  init(wrappedValue initialValue: T) {
    wrappedValue = initialValue
  }

  var projectedValue: Wrapper<T> {
    return Wrapper(stored: wrappedValue)
  }
}

struct TestGenericStorageRef<T> {
  struct Inner { }
  @InitialValueWrapperWithStorageRef var inner: Inner = Inner()
}

// Wiring up the _projectedValueProperty attribute.
struct TestProjectionValuePropertyAttr {
  @_projectedValueProperty(wrapperA)
  @WrapperWithStorageRef var a: String

  var wrapperA: Wrapper<String> {
    Wrapper(stored: "blah")
  }

  @_projectedValueProperty(wrapperB) // expected-error{{could not find projection value property 'wrapperB'}}
  @WrapperWithStorageRef var b: String
}


// ---------------------------------------------------------------------------
// Misc. semantic issues
// ---------------------------------------------------------------------------
@propertyWrapper
struct BrokenLazy { }
// expected-error@-1{{property wrapper type 'BrokenLazy' does not contain a non-static property named 'wrappedValue'}}

struct S {
  @BrokenLazy
  var wrappedValue: Int
}

@propertyWrapper
struct DynamicSelfStruct {
  var wrappedValue: Self { self } // okay
  var projectedValue: Self { self } // okay
}

@propertyWrapper
class DynamicSelf {
  var wrappedValue: Self { self } // expected-error {{property wrapper wrapped value cannot have dynamic Self type}}
  var projectedValue: Self? { self } // expected-error {{property wrapper projected value cannot have dynamic Self type}}
}

struct UseDynamicSelfWrapper {
  @DynamicSelf() var value
}

// ---------------------------------------------------------------------------
// Invalid redeclaration
// ---------------------------------------------------------------------------
@propertyWrapper
struct WrapperWithProjectedValue<T> {
  var wrappedValue: T
  var projectedValue: T { return wrappedValue }
}

class TestInvalidRedeclaration1 {

  @WrapperWithProjectedValue var i = 17
  // expected-note@-1 {{'i' previously declared here}}
  // expected-note@-2 {{'$i' synthesized for property wrapper projected value}}
  // expected-note@-3 {{'_i' synthesized for property wrapper backing storage}}

  @WrapperWithProjectedValue var i = 39
  // expected-error@-1 {{invalid redeclaration of 'i'}}
  // expected-error@-2 {{invalid redeclaration of synthesized property '$i'}}
  // expected-error@-3 {{invalid redeclaration of synthesized property '_i'}}
}

// https://github.com/apple/swift/issues/55285

struct TestInvalidRedeclaration2 {
  var _foo1 = 123 // expected-error {{invalid redeclaration of synthesized property '_foo1'}}
  @WrapperWithInitialValue var foo1 = 123 // expected-note {{'_foo1' synthesized for property wrapper backing storage}}
}

struct TestInvalidRedeclaration3 {
  @WrapperWithInitialValue var foo1 = 123 // expected-note {{'_foo1' synthesized for property wrapper backing storage}}
  var _foo1 = 123 // expected-error {{invalid redeclaration of synthesized property '_foo1'}}
}

// Diagnose when wrapped property uses the name we use for lazy variable storage property.
struct TestInvalidRedeclaration4 {
  @WrapperWithProjectedValue var __lazy_storage_$_foo: Int
  // expected-error@-1 {{invalid redeclaration of synthesized property '$__lazy_storage_$_foo'}}
  // expected-note@-2 {{'$__lazy_storage_$_foo' synthesized for property wrapper projected value}}
  lazy var foo = 1
}

// ---------------------------------------------------------------------------
// Closures in initializers
// ---------------------------------------------------------------------------
struct UsesExplicitClosures {
  @WrapperAcceptingAutoclosure(body: { 42 })
  var x: Int

  @WrapperAcceptingAutoclosure(body: { return 42 })
  var y: Int
}

// ---------------------------------------------------------------------------
// Enclosing instance diagnostics
// ---------------------------------------------------------------------------
@propertyWrapper
struct Observable<Value> {
  private var stored: Value

  init(wrappedValue: Value) {
    self.stored = wrappedValue
  }

  @available(*, unavailable, message: "must be in a class")
  var wrappedValue: Value { // expected-note{{'wrappedValue' has been explicitly marked unavailable here}}
    get { fatalError("called wrappedValue getter") }
    set { fatalError("called wrappedValue setter") }
  }

  static subscript<EnclosingSelf>(
      _enclosingInstance observed: EnclosingSelf,
      wrapped wrappedKeyPath: ReferenceWritableKeyPath<EnclosingSelf, Value>,
      storage storageKeyPath: ReferenceWritableKeyPath<EnclosingSelf, Self>
    ) -> Value {
    get {
      observed[keyPath: storageKeyPath].stored
    }
    set {
      observed[keyPath: storageKeyPath].stored = newValue
    }
  }
}

struct MyObservedValueType {
  @Observable // expected-error{{'wrappedValue' is unavailable: must be in a class}}
  var observedProperty = 17
}

// ---------------------------------------------------------------------------
// Miscellaneous bugs
// ---------------------------------------------------------------------------

// rdar://problem/50822051 - compiler assertion / hang
@propertyWrapper
struct PD<Value> {
  var wrappedValue: Value

  init<A>(wrappedValue initialValue: Value, a: A) {
    self.wrappedValue = initialValue
  }
}

struct TestPD {
  @PD(a: "foo") var foo: Int = 42
}

protocol P { }

@propertyWrapper
struct WrapperRequiresP<T: P> {
  var wrappedValue: T
  var projectedValue: T { return wrappedValue }
}

struct UsesWrapperRequiringP {
  // expected-note@-1{{in declaration of}}
  
  @WrapperRequiresP var x.: UsesWrapperRequiringP
  // expected-error@-1{{expected member name following '.'}}
  // expected-error@-2{{expected declaration}}
  // expected-error@-3{{type annotation missing in pattern}}
}

// rdar://problem/51588022
// https://github.com/apple/swift/issues/53289
do {
  @propertyWrapper
  struct Wrapper {
    var wrappedValue: String { "hi" }
  }

  struct S {
    @Wrapper var thing: Bool // expected-error{{property type 'Bool' does not match 'wrappedValue' type 'String'}}
  }
}

// https://github.com/apple/swift/issues/57080
do {
  @propertyWrapper
  struct StringWrappedValue {
    var wrappedValue: String
  }

  struct S {
    // expected-error@+1 {{property type '() -> String' does not match 'wrappedValue' type 'String'}}
    @StringWrappedValue var value: () -> String
  }
}

// rdar://problem/52593304
// https://github.com/apple/swift/issues/53453
// Assertion with DeclContext mismatches

class SomeValue {
	@SomeA(closure: { $0 }) var some: Int = 100
}

@propertyWrapper
struct SomeA<T> {
	var wrappedValue: T
	let closure: (T) -> (T)

	init(wrappedValue initialValue: T, closure: @escaping (T) -> (T)) {
		self.wrappedValue = initialValue
		self.closure = closure
	}
}

// rdar://problem/51989272 - crash when the property wrapper is a generic type
// alias
typealias Alias<T> = WrapperWithInitialValue<T>

struct TestAlias {
  @Alias var foo = 17
}

// rdar://problem/52969503 - crash due to invalid source ranges in ill-formed
// code.
@propertyWrapper
struct Wrap52969503<T> {
  var wrappedValue: T

  init(blah: Int, wrappedValue: T) { }
}

struct Test52969503 {
  @Wrap52969503(blah: 5) var foo: Int = 1 // expected-error{{argument 'blah' must precede argument 'wrappedValue'}}
}


// 
// ---------------------------------------------------------------------------
// Property wrapper composition
// ---------------------------------------------------------------------------
@propertyWrapper
struct WrapperA<Value> {
  var wrappedValue: Value

  init(wrappedValue initialValue: Value) {
    wrappedValue = initialValue
  }
}

@propertyWrapper
struct WrapperB<Value> {
  var wrappedValue: Value

  init(wrappedValue initialValue: Value) {
    wrappedValue = initialValue
  }
}

@propertyWrapper
struct WrapperC<Value> { // expected-note {{'Value' declared as parameter to type 'WrapperC'}}
  var wrappedValue: Value?

  init(wrappedValue initialValue: Value?) {
    wrappedValue = initialValue
  }
}

@propertyWrapper
struct WrapperD<Value, X, Y> {
  var wrappedValue: Value
}

@propertyWrapper
struct WrapperE<Value> {
  var wrappedValue: Value
}

struct TestComposition {
  @WrapperA @WrapperB @WrapperC var p1: Int?
  @WrapperA @WrapperB @WrapperC var p2 = "Hello"
  @WrapperD<WrapperE, Int, String> @WrapperE var p3: Int?
  @WrapperD<WrapperC, Int, String> @WrapperC var p4: Int?
  @WrapperD<WrapperC, Int, String> @WrapperE var p5: Int // expected-error{{generic parameter 'Value' could not be inferred}}
    // expected-note@-1 {{explicitly specify the generic arguments to fix this issue}}
    // expected-error@-2 {{composed wrapper type 'WrapperE<Int>' does not match type of 'WrapperD<WrapperC<Value>, Int, String>.wrappedValue', which is 'WrapperC<Value>'}}

  @Wrapper<String> @Wrapper var value: Int // expected-error{{composed wrapper type 'Wrapper<Int>' does not match type of 'Wrapper<String>.wrappedValue', which is 'String'}}

	func triggerErrors(d: Double) { // expected-note 6 {{mark method 'mutating' to make 'self' mutable}} {{2-2=mutating }}
		p1 = d // expected-error{{cannot assign value of type 'Double' to type 'Int?'}} {{8-8=Int(}} {{9-9=)}}
    // expected-error@-1 {{cannot assign to property: 'self' is immutable}}
		p2 = d // expected-error{{cannot assign value of type 'Double' to type 'String?'}}
    // expected-error@-1 {{cannot assign to property: 'self' is immutable}}
    // TODO(diagnostics): Looks like source range for 'd' here is reported as starting at 10, but it should be 8
    p3 = d // expected-error{{cannot assign value of type 'Double' to type 'Int?'}} {{10-10=Int(}} {{11-11=)}}
    // expected-error@-1 {{cannot assign to property: 'self' is immutable}}

		_p1 = d // expected-error{{cannot assign value of type 'Double' to type 'WrapperA<WrapperB<WrapperC<Int>>>'}}
    // expected-error@-1 {{cannot assign to property: 'self' is immutable}}
		_p2 = d // expected-error{{cannot assign value of type 'Double' to type 'WrapperA<WrapperB<WrapperC<String>>>'}}
    // expected-error@-1 {{cannot assign to property: 'self' is immutable}}
    _p3 = d // expected-error{{cannot assign value of type 'Double' to type 'WrapperD<WrapperE<Int?>, Int, String>'}}
    // expected-error@-1 {{cannot assign to property: 'self' is immutable}}
	}
}

// ---------------------------------------------------------------------------
// Property wrapper composition type inference
// ---------------------------------------------------------------------------

protocol DefaultValue {
  static var defaultValue: Self { get }
}

extension Int: DefaultValue {
  static var defaultValue: Int { 0 }
}

struct TestCompositionTypeInference {
  @propertyWrapper
  struct A<Value: DefaultValue> {
    var wrappedValue: Value

    init(wrappedValue: Value = .defaultValue, key: String) {
      self.wrappedValue = wrappedValue
    }
  }

  @propertyWrapper
  struct B<Value: DefaultValue>: DefaultValue {
    var wrappedValue: Value

    init(wrappedValue: Value = .defaultValue) {
      self.wrappedValue = wrappedValue
    }

    static var defaultValue: B<Value> { B() }
  }

  // All of these are okay

  @A(key: "b") @B
  var a: Int = 0

  @A(key: "b") @B
  var b: Int

  @A(key: "c") @B @B
  var c: Int
}

// ---------------------------------------------------------------------------
// Missing Property Wrapper Unwrap Diagnostics
// ---------------------------------------------------------------------------
@propertyWrapper
struct Foo<T> { // expected-note {{arguments to generic parameter 'T' ('W' and 'Int') are expected to be equal}}
  var wrappedValue: T {
    get {
      fatalError("boom")
    }

    set {
    }
  }

  var prop: Int = 42

  func foo() {}
  func bar(x: Int) {}

  subscript(q: String) -> Int {
    get { return 42 }
    set { }
  }

  subscript(x x: Int) -> Int {
    get { return 42 }
    set { }
  }

  subscript(q q: String, a: Int) -> Bool {
    get { return false }
    set { }
  }
}

extension Foo : Equatable where T : Equatable {
  static func == (lhs: Foo, rhs: Foo) -> Bool {
    lhs.wrappedValue == rhs.wrappedValue
  }
}

extension Foo : Hashable where T : Hashable {
  func hash(into hasher: inout Hasher) {
    hasher.combine(wrappedValue)
  }
}

@propertyWrapper
struct Bar<T, V> {
  var wrappedValue: T

  func bar() {}

  // TODO(diagnostics): We need to figure out what to do about subscripts.
  // The problem standing in our way - keypath application choice
  // is always added to results even if it's not applicable.
}

@propertyWrapper
struct Baz<T> {
  var wrappedValue: T
  
  func onPropertyWrapper() {}

  var projectedValue: V {
    return V()
  }
}

extension Bar where V == String { // expected-note {{where 'V' = 'Bool'}}
  func barWhereVIsString() {}
}

struct V {
  func onProjectedValue() {}
}

struct W {
  func onWrapped() {}
}

struct MissingPropertyWrapperUnwrap {
  @Foo var w: W
  @Foo var x: Int
  @Bar<Int, Bool> var y: Int
  @Bar<Int, String> var z: Int
  @Baz var usesProjectedValue: W
  
  func a<T>(_: Foo<T>) {}
  func a<T>(named: Foo<T>) {}
  func b(_: Foo<Int>) {}
  func c(_: V) {}
  func d(_: W) {}
  func e(_: Foo<W>) {}

  subscript<T : Hashable>(takesFoo x: Foo<T>) -> Foo<T> { x }

  func baz() {
    self.x.foo() // expected-error {{referencing instance method 'foo()' requires wrapper 'Foo<Int>'}}{{10-10=_}}
    self.x.prop  // expected-error {{referencing property 'prop' requires wrapper 'Foo<Int>'}} {{10-10=_}}
    self.x.bar(x: 42) // expected-error {{referencing instance method 'bar(x:)' requires wrapper 'Foo<Int>'}} {{10-10=_}}
    self.y.bar() // expected-error {{referencing instance method 'bar()' requires wrapper 'Bar<Int, Bool>'}}{{10-10=_}}
    self.y.barWhereVIsString() // expected-error {{referencing instance method 'barWhereVIsString()' requires wrapper 'Bar<Int, Bool>'}}{{10-10=_}}
    // expected-error@-1 {{referencing instance method 'barWhereVIsString()' on 'Bar' requires the types 'Bool' and 'String' be equivalent}}
    self.z.barWhereVIsString() // expected-error {{referencing instance method 'barWhereVIsString()' requires wrapper 'Bar<Int, String>'}}{{10-10=_}}
    self.usesProjectedValue.onPropertyWrapper() // expected-error {{referencing instance method 'onPropertyWrapper()' requires wrapper 'Baz<W>'}}{{10-10=_}}

    self._w.onWrapped() // expected-error {{referencing instance method 'onWrapped()' requires wrapped value of type 'W'}}{{10-11=}}
    self.usesProjectedValue.onProjectedValue() // expected-error {{referencing instance method 'onProjectedValue()' requires wrapper 'V'}}{{10-10=$}}
    self.$usesProjectedValue.onWrapped() // expected-error {{referencing instance method 'onWrapped()' requires wrapped value of type 'W'}}{{10-11=}}
    self._usesProjectedValue.onWrapped() // expected-error {{referencing instance method 'onWrapped()' requires wrapped value of type 'W'}}{{10-11=}}
    
    a(self.w) // expected-error {{cannot convert value 'w' of type 'W' to expected type 'Foo<W>', use wrapper instead}}{{12-12=_}}
    b(self.x) // expected-error {{cannot convert value 'x' of type 'Int' to expected type 'Foo<Int>', use wrapper instead}}{{12-12=_}}
    b(self.w) // expected-error {{cannot convert value of type 'W' to expected argument type 'Foo<Int>'}}
    e(self.w) // expected-error {{cannot convert value 'w' of type 'W' to expected type 'Foo<W>', use wrapper instead}}{{12-12=_}}
    b(self._w) // expected-error {{cannot convert value of type 'Foo<W>' to expected argument type 'Foo<Int>'}}

    c(self.usesProjectedValue) // expected-error {{cannot convert value 'usesProjectedValue' of type 'W' to expected type 'V', use wrapper instead}}{{12-12=$}}
    d(self.$usesProjectedValue) // expected-error {{cannot convert value '$usesProjectedValue' of type 'V' to expected type 'W', use wrapped value instead}}{{12-13=}}
    d(self._usesProjectedValue) // expected-error {{cannot convert value '_usesProjectedValue' of type 'Baz<W>' to expected type 'W', use wrapped value instead}}{{12-13=}}

    self.x["ultimate question"] // expected-error {{referencing subscript 'subscript(_:)' requires wrapper 'Foo<Int>'}} {{10-10=_}}
    self.x["ultimate question"] = 42 // expected-error {{referencing subscript 'subscript(_:)' requires wrapper 'Foo<Int>'}} {{10-10=_}}

    self.x[x: 42] // expected-error {{referencing subscript 'subscript(x:)' requires wrapper 'Foo<Int>'}} {{10-10=_}}
    self.x[x: 42] = 0 // expected-error {{referencing subscript 'subscript(x:)' requires wrapper 'Foo<Int>'}} {{10-10=_}}

    self.x[q: "ultimate question", 42] // expected-error {{referencing subscript 'subscript(q:_:)' requires wrapper 'Foo<Int>'}} {{10-10=_}}
    self.x[q: "ultimate question", 42] = true // expected-error {{referencing subscript 'subscript(q:_:)' requires wrapper 'Foo<Int>'}} {{10-10=_}}

    // https://github.com/apple/swift/issues/53876
    _ = \Self.[takesFoo: self.x] // expected-error {{cannot convert value 'x' of type 'Int' to expected type 'Foo<Int>', use wrapper instead}}{{31-31=_}}
    _ = \Foo<W>.[x: self._x] // expected-error {{cannot convert value '_x' of type 'Foo<Int>' to expected type 'Int', use wrapped value instead}} {{26-27=}}
  }
}

struct InvalidPropertyDelegateUse {
  // TODO(diagnostics): We need to a tailored diagnostic for extraneous arguments in property delegate initialization
  @Foo var x: Int = 42 // expected-error@:21 {{extra argument 'wrappedValue' in call}}

  func test() {
    self.x.foo() // expected-error {{value of type 'Int' has no member 'foo'}}
  }
}

// https://github.com/apple/swift/issues/53452
do {
  class C {
    @Wrapper var property: Int = 1234 // expected-error {{missing argument for parameter 'string' in property wrapper initializer; add 'wrappedValue' and 'string' arguments in '@Wrapper(...)'}}
  }

  @propertyWrapper
  struct Wrapper {
    var wrappedValue: Int

    init(wrappedValue: Int, string: String) { // expected-note {{'init(wrappedValue:string:)' declared here}}
      self.wrappedValue = wrappedValue
    }
  }
}

// https://github.com/apple/swift/issues/53534
// Check that all possible compositions of nonmutating/mutating accessors
// on wrappers produce wrapped properties with the correct settability and
// mutatiness in all compositions.

@propertyWrapper
struct NonmutatingGetWrapper<T> {
  var wrappedValue: T {
    nonmutating get { fatalError() }
  }
}

@propertyWrapper
struct MutatingGetWrapper<T> {
  var wrappedValue: T {
    mutating get { fatalError() }
  }
}

@propertyWrapper
struct NonmutatingGetNonmutatingSetWrapper<T> {
  var wrappedValue: T {
    nonmutating get { fatalError() }
    nonmutating set { fatalError() }
  }
}

@propertyWrapper
struct MutatingGetNonmutatingSetWrapper<T> {
  var wrappedValue: T {
    mutating get { fatalError() }
    nonmutating set { fatalError() }
  }
}

@propertyWrapper
struct NonmutatingGetMutatingSetWrapper<T> {
  var wrappedValue: T {
    nonmutating get { fatalError() }
    mutating set { fatalError() }
  }
}

@propertyWrapper
struct MutatingGetMutatingSetWrapper<T> {
  var wrappedValue: T {
    mutating get { fatalError() }
    mutating set { fatalError() }
  }
}

struct AllCompositionsStruct {
  // Should have nonmutating getter, undefined setter
  @NonmutatingGetWrapper @NonmutatingGetWrapper
  var ngxs_ngxs: Int

  // Should be disallowed
  @NonmutatingGetWrapper @MutatingGetWrapper // expected-error{{cannot be composed}}
  var ngxs_mgxs: Int

  // Should have nonmutating getter, nonmutating setter
  @NonmutatingGetWrapper @NonmutatingGetNonmutatingSetWrapper
  var ngxs_ngns: Int

  // Should be disallowed
  @NonmutatingGetWrapper @MutatingGetNonmutatingSetWrapper // expected-error{{cannot be composed}}
  var ngxs_mgns: Int

  // Should have nonmutating getter, undefined setter
  @NonmutatingGetWrapper @NonmutatingGetMutatingSetWrapper
  var ngxs_ngms: Int

  // Should be disallowed
  @NonmutatingGetWrapper @MutatingGetMutatingSetWrapper // expected-error{{cannot be composed}}
  var ngxs_mgms: Int

  ////

  // Should have mutating getter, undefined setter
  @MutatingGetWrapper @NonmutatingGetWrapper
  var mgxs_ngxs: Int

  // Should be disallowed
  @MutatingGetWrapper @MutatingGetWrapper // expected-error{{cannot be composed}}
  var mgxs_mgxs: Int

  // Should have mutating getter, mutating setter
  @MutatingGetWrapper @NonmutatingGetNonmutatingSetWrapper
  var mgxs_ngns: Int

  // Should be disallowed
  @MutatingGetWrapper @MutatingGetNonmutatingSetWrapper // expected-error{{cannot be composed}}
  var mgxs_mgns: Int

  // Should have mutating getter, undefined setter
  @MutatingGetWrapper @NonmutatingGetMutatingSetWrapper
  var mgxs_ngms: Int

  // Should be disallowed
  @MutatingGetWrapper @MutatingGetMutatingSetWrapper // expected-error{{cannot be composed}}
  var mgxs_mgms: Int

  ////

  // Should have nonmutating getter, undefined setter
  @NonmutatingGetNonmutatingSetWrapper @NonmutatingGetWrapper
  var ngns_ngxs: Int

  // Should have nonmutating getter, undefined setter
  @NonmutatingGetNonmutatingSetWrapper @MutatingGetWrapper
  var ngns_mgxs: Int

  // Should have nonmutating getter, nonmutating setter
  @NonmutatingGetNonmutatingSetWrapper @NonmutatingGetNonmutatingSetWrapper
  var ngns_ngns: Int

  // Should have nonmutating getter, nonmutating setter
  @NonmutatingGetNonmutatingSetWrapper @MutatingGetNonmutatingSetWrapper
  var ngns_mgns: Int

  // Should have nonmutating getter, nonmutating setter
  @NonmutatingGetNonmutatingSetWrapper @NonmutatingGetMutatingSetWrapper
  var ngns_ngms: Int

  // Should have nonmutating getter, nonmutating setter
  @NonmutatingGetNonmutatingSetWrapper @MutatingGetMutatingSetWrapper
  var ngns_mgms: Int

  ////

  // Should have mutating getter, undefined setter
  @MutatingGetNonmutatingSetWrapper @NonmutatingGetWrapper
  var mgns_ngxs: Int

  // Should have mutating getter, undefined setter
  @MutatingGetNonmutatingSetWrapper @MutatingGetWrapper
  var mgns_mgxs: Int

  // Should have mutating getter, mutating setter
  @MutatingGetNonmutatingSetWrapper @NonmutatingGetNonmutatingSetWrapper
  var mgns_ngns: Int

  // Should have mutating getter, mutating setter
  @MutatingGetNonmutatingSetWrapper @MutatingGetNonmutatingSetWrapper
  var mgns_mgns: Int

  // Should have mutating getter, mutating setter
  @MutatingGetNonmutatingSetWrapper @NonmutatingGetMutatingSetWrapper
  var mgns_ngms: Int

  // Should have mutating getter, mutating setter
  @MutatingGetNonmutatingSetWrapper @MutatingGetMutatingSetWrapper
  var mgns_mgms: Int

  ////

  // Should have nonmutating getter, undefined setter
  @NonmutatingGetMutatingSetWrapper @NonmutatingGetWrapper
  var ngms_ngxs: Int

  // Should have mutating getter, undefined setter
  @NonmutatingGetMutatingSetWrapper @MutatingGetWrapper
  var ngms_mgxs: Int

  // Should have nonmutating getter, nonmutating setter
  @NonmutatingGetMutatingSetWrapper @NonmutatingGetNonmutatingSetWrapper
  var ngms_ngns: Int

  // Should have mutating getter, nonmutating setter
  @NonmutatingGetMutatingSetWrapper @MutatingGetNonmutatingSetWrapper
  var ngms_mgns: Int

  // Should have nonmutating getter, mutating setter
  @NonmutatingGetMutatingSetWrapper @NonmutatingGetMutatingSetWrapper
  var ngms_ngms: Int

  // Should have mutating getter, mutating setter
  @NonmutatingGetMutatingSetWrapper @MutatingGetMutatingSetWrapper
  var ngms_mgms: Int

  ////

  // Should have mutating getter, undefined setter
  @MutatingGetMutatingSetWrapper @NonmutatingGetWrapper
  var mgms_ngxs: Int

  // Should have mutating getter, undefined setter
  @MutatingGetMutatingSetWrapper @MutatingGetWrapper
  var mgms_mgxs: Int

  // Should have mutating getter, mutating setter
  @MutatingGetMutatingSetWrapper @NonmutatingGetNonmutatingSetWrapper
  var mgms_ngns: Int

  // Should have mutating getter, mutating setter
  @MutatingGetMutatingSetWrapper @MutatingGetNonmutatingSetWrapper
  var mgms_mgns: Int

  // Should have mutating getter, mutating setter
  @MutatingGetMutatingSetWrapper @NonmutatingGetMutatingSetWrapper
  var mgms_ngms: Int

  // Should have mutating getter, mutating setter
  @MutatingGetMutatingSetWrapper @MutatingGetMutatingSetWrapper
  var mgms_mgms: Int

  func readonlyContext(x: Int) { // expected-note *{{}}
    _ = ngxs_ngxs 
    // _ = ngxs_mgxs
    _ = ngxs_ngns
    // _ = ngxs_mgns
    _ = ngxs_ngms
    // _ = ngxs_mgms

    _ = mgxs_ngxs // expected-error{{}}
    // _ = mgxs_mgxs
    _ = mgxs_ngns // expected-error{{}}
    // _ = mgxs_mgns
    _ = mgxs_ngms // expected-error{{}}
    // _ = mgxs_mgms

    _ = ngns_ngxs
    _ = ngns_mgxs
    _ = ngns_ngns
    _ = ngns_mgns
    _ = ngns_ngms
    _ = ngns_mgms

    _ = mgns_ngxs // expected-error{{}}
    _ = mgns_mgxs // expected-error{{}}
    _ = mgns_ngns // expected-error{{}}
    _ = mgns_mgns // expected-error{{}}
    _ = mgns_ngms // expected-error{{}}
    _ = mgns_mgms // expected-error{{}}

    _ = ngms_ngxs
    _ = ngms_mgxs // expected-error{{}}
    _ = ngms_ngns
    _ = ngms_mgns // expected-error{{}}
    _ = ngms_ngms
    _ = ngms_mgms // expected-error{{}}

    _ = mgms_ngxs // expected-error{{}}
    _ = mgms_mgxs // expected-error{{}}
    _ = mgms_ngns // expected-error{{}}
    _ = mgms_mgns // expected-error{{}}
    _ = mgms_ngms // expected-error{{}}
    _ = mgms_mgms // expected-error{{}}

    ////

    ngxs_ngxs = x // expected-error{{}}
    // ngxs_mgxs = x
    ngxs_ngns = x
    // ngxs_mgns = x
    ngxs_ngms = x // expected-error{{}}
    // ngxs_mgms = x

    mgxs_ngxs = x // expected-error{{}}
    // mgxs_mgxs = x
    mgxs_ngns = x // expected-error{{}}
    // mgxs_mgns = x
    mgxs_ngms = x // expected-error{{}}
    // mgxs_mgms = x

    ngns_ngxs = x // expected-error{{}}
    ngns_mgxs = x // expected-error{{}}
    ngns_ngns = x
    ngns_mgns = x
    ngns_ngms = x
    ngns_mgms = x

    mgns_ngxs = x // expected-error{{}}
    mgns_mgxs = x // expected-error{{}}
    mgns_ngns = x // expected-error{{}}
    mgns_mgns = x // expected-error{{}}
    mgns_ngms = x // expected-error{{}}
    mgns_mgms = x // expected-error{{}}

    ngms_ngxs = x // expected-error{{}}
    ngms_mgxs = x // expected-error{{}}
    ngms_ngns = x
    // FIXME: This ought to be allowed because it's a pure set, so the mutating
    // get should not come into play.
    ngms_mgns = x // expected-error{{cannot use mutating getter}}
    ngms_ngms = x // expected-error{{}}
    ngms_mgms = x // expected-error{{}}

    mgms_ngxs = x // expected-error{{}}
    mgms_mgxs = x // expected-error{{}}
    mgms_ngns = x // expected-error{{}}
    mgms_mgns = x // expected-error{{}}
    mgms_ngms = x // expected-error{{}}
    mgms_mgms = x // expected-error{{}}
  }

  mutating func mutatingContext(x: Int) {
    _ = ngxs_ngxs 
    // _ = ngxs_mgxs
    _ = ngxs_ngns
    // _ = ngxs_mgns
    _ = ngxs_ngms
    // _ = ngxs_mgms

    _ = mgxs_ngxs
    // _ = mgxs_mgxs
    _ = mgxs_ngns
    // _ = mgxs_mgns
    _ = mgxs_ngms
    // _ = mgxs_mgms

    _ = ngns_ngxs
    _ = ngns_mgxs
    _ = ngns_ngns
    _ = ngns_mgns
    _ = ngns_ngms
    _ = ngns_mgms

    _ = mgns_ngxs
    _ = mgns_mgxs
    _ = mgns_ngns
    _ = mgns_mgns
    _ = mgns_ngms
    _ = mgns_mgms

    _ = ngms_ngxs
    _ = ngms_mgxs
    _ = ngms_ngns
    _ = ngms_mgns
    _ = ngms_ngms
    _ = ngms_mgms

    _ = mgms_ngxs
    _ = mgms_mgxs
    _ = mgms_ngns
    _ = mgms_mgns
    _ = mgms_ngms
    _ = mgms_mgms

    ////

    ngxs_ngxs = x // expected-error{{}}
    // ngxs_mgxs = x
    ngxs_ngns = x
    // ngxs_mgns = x
    ngxs_ngms = x // expected-error{{}}
    // ngxs_mgms = x

    mgxs_ngxs = x // expected-error{{}}
    // mgxs_mgxs = x
    mgxs_ngns = x
    // mgxs_mgns = x
    mgxs_ngms = x // expected-error{{}}
    // mgxs_mgms = x

    ngns_ngxs = x // expected-error{{}}
    ngns_mgxs = x // expected-error{{}}
    ngns_ngns = x
    ngns_mgns = x
    ngns_ngms = x
    ngns_mgms = x

    mgns_ngxs = x // expected-error{{}}
    mgns_mgxs = x // expected-error{{}}
    mgns_ngns = x
    mgns_mgns = x
    mgns_ngms = x
    mgns_mgms = x

    ngms_ngxs = x // expected-error{{}}
    ngms_mgxs = x // expected-error{{}}
    ngms_ngns = x
    ngms_mgns = x
    ngms_ngms = x
    ngms_mgms = x

    mgms_ngxs = x // expected-error{{}}
    mgms_mgxs = x // expected-error{{}}
    mgms_ngns = x
    mgms_mgns = x
    mgms_ngms = x
    mgms_mgms = x
  }
}

// rdar://problem/54184846 - crash while trying to retrieve wrapper info on l-value base type
func test_missing_method_with_lvalue_base() {
  @propertyWrapper
  struct Ref<T> {
    var wrappedValue: T
  }

  struct S<T> where T: RandomAccessCollection, T.Element: Equatable {
    @Ref var v: T.Element

    init(items: T, v: Ref<T.Element>) {
      self.v.binding = v // expected-error {{value of type 'T.Element' has no member 'binding'}}
    }
  }
}

// https://github.com/apple/swift/issues/53689
// Look into the protocols that the type conforms to.

// typealias as propertyWrapper //

@propertyWrapper
struct W_53689 {
  var wrappedValue: Int
}

protocol P1_53689 {
  typealias Wrapper1_53689 = W_53689
}

struct S1_53689: P1_53689 {
  @Wrapper1_53689 var answer = 42 // Okay
}

// associatedtype as propertyWrapper //

protocol P2_53689 {
  associatedtype Wrapper2_53689 = W_53689
}

struct S2_53689: P2_53689 {
  @Wrapper2_53689 var answer = 42 // expected-error {{unknown attribute 'Wrapper2_53689'}}
}

protocol P3_53689 {
  associatedtype Wrapper3_53689
}

struct S3_53689: P3_53689 {
  typealias Wrapper3_53689 = W_53689
  @Wrapper3_53689 var answer = 42 // Okay
}

// typealias as propertyWrapper in a constrained protocol extension //

protocol P4_53689 {}
extension P4_53689 where Self: AnyObject { // expected-note {{requirement specified as 'Self' : 'AnyObject' [with Self = S4_53689]}}
  typealias Wrapper4_53689 = W_53689
}

struct S4_53689: P4_53689 {
  @Wrapper4_53689 var answer = 42 // expected-error {{'Self.Wrapper4_53689' (aka 'W_53689') requires that 'S4_53689' be a class type}}
}

class C4_53689: P4_53689 {
  @Wrapper4_53689 var answer = 42 // Okay
}

// typealias as propertyWrapper in a generic type //

protocol P5_53689 {
  typealias Wrapper5_53689 = W_53689
}

struct S5_53689<T>: P5_53689 {
  @Wrapper5_53689 var answer = 42 // Okay
}

// https://github.com/apple/swift/issues/53794

protocol Copyable: AnyObject {
    func copy() -> Self
}

@propertyWrapper
struct CopyOnWrite<Value: Copyable> {
  init(wrappedValue: Value) {
    self.wrappedValue = wrappedValue
  }

  var wrappedValue: Value

  var projectedValue: Value {
    mutating get {
      if !isKnownUniquelyReferenced(&wrappedValue) {
        wrappedValue = wrappedValue.copy()
      }
      return wrappedValue
    }
    set {
      wrappedValue = newValue
    }
  }
}

final class CopyOnWriteTest: Copyable {
    let a: Int
    init(a: Int) {
        self.a = a
    }

    func copy() -> Self {
        Self.init(a: a)
    }
}

struct CopyOnWriteDemo1 {
  @CopyOnWrite var a = CopyOnWriteTest(a: 3)

  func foo() { // expected-note{{mark method 'mutating' to make 'self' mutable}}
    _ = $a // expected-error{{cannot use mutating getter on immutable value: 'self' is immutable}}
  }
}

@propertyWrapper
struct NonMutatingProjectedValueSetWrapper<Value> {
  var wrappedValue: Value
  var projectedValue: Value {
    get { wrappedValue }
    nonmutating set { } 
  }
}

struct UseNonMutatingProjectedValueSet {
  @NonMutatingProjectedValueSetWrapper var x = 17

  func test() { // expected-note{{mark method 'mutating' to make 'self' mutable}}
    $x = 42 // okay
    x = 42  // expected-error{{cannot assign to property: 'self' is immutable}}
  }
}

// https://github.com/apple/swift/issues/53878
do {
@propertyWrapper
  struct Wrapper<Value> {
    var wrappedValue: Value
  }

  class C1 {
    @Wrapper static var bool1: Bool = true // Ok
    @Wrapper class var bool2: Bool = true // expected-error {{class stored properties not supported in classes; did you mean 'static'?}}
    @Wrapper class final var bool3: Bool = true // expected-error {{class stored properties not supported in classes; did you mean 'static'?}}
  }

  final class C2 {
    @Wrapper static var bool1: Bool = true // Ok
    @Wrapper class var bool2: Bool = true // expected-error {{class stored properties not supported in classes; did you mean 'static'?}}
    @Wrapper class final var bool3: Bool = true // expected-error {{class stored properties not supported in classes; did you mean 'static'?}}
  }
}

// https://github.com/apple/swift/issues/53782
do {
@propertyWrapper
  struct Wrapper<T> {
    init(wrappedValue: T) {}
    var wrappedValue: T { get {} }
  }

  struct S {
    @Wrapper var foo: Int = nil // expected-error {{'nil' is not compatible with expected argument type 'Int'}}
  }
}

// rdar://problem/53349209 - regression in property wrapper inference
struct Concrete1: P {}

@propertyWrapper struct ConcreteWrapper {
  var wrappedValue: Concrete1 { get { fatalError() } }
}

struct TestConcrete1 {
  @ConcreteWrapper() var s1

  func f() {
    // Good:
    let _: P = self.s1

    // Bad:
    self.g(s1: self.s1)

    // Ugly:
    self.g(s1: self.s1 as P)
  }

  func g(s1: P) {
    // ...
  }
}

// https://github.com/apple/swift/issues/53877

// Two initializers that can default initialize the wrapper //

@propertyWrapper
struct W1_53877 { // Okay
  let name: String

  init() {
    self.name = "Init"
  }

  init(name: String = "DefaultParamInit") {
    self.name = name
  }

  var wrappedValue: Int {
    get { return 0 }
  }
}

// Two initializers with default arguments that can default initialize the wrapper //

@propertyWrapper
struct W2_53877 { // Okay
  let name: String

  init(anotherName: String = "DefaultParamInit1") {
    self.name = anotherName
  }

  init(name: String = "DefaultParamInit2") {
    self.name = name
  }

  var wrappedValue: Int {
    get { return 0 }
  }
}

// Single initializer that can default initialize the wrapper //

@propertyWrapper
struct W3_53877 { // Okay
  let name: String

  init() {
    self.name = "Init"
  }

  var wrappedValue: Int {
    get { return 0 }
  }
}

// rdar://problem/56213175 - backward compatibility issue with Swift 5.1,
// which unconditionally skipped protocol members.
protocol ProtocolWithWrapper {
  associatedtype Wrapper = Float // expected-note{{associated type 'Wrapper' declared here}}
}

struct UsesProtocolWithWrapper: ProtocolWithWrapper {
  @Wrapper var foo: Int // expected-warning{{ignoring associated type 'Wrapper' in favor of module-scoped property wrapper 'Wrapper'; please qualify the reference with 'property_wrappers'}}{{4-4=property_wrappers.}}
}

// rdar://problem/56350060 - [Dynamic key path member lookup] Assertion when subscripting with a key path
func test_rdar56350060() {
  @propertyWrapper
  @dynamicMemberLookup
  struct DynamicWrapper<Value> {
    var wrappedValue: Value { fatalError() }

    subscript<T>(keyPath keyPath: KeyPath<Value, T>) -> DynamicWrapper<T> {
      fatalError()
    }

    subscript<T>(dynamicMember keyPath: KeyPath<Value, T>) -> DynamicWrapper<T> {
      return self[keyPath: keyPath] // Ok
    }
  }
}

// rdar://problem/57411331 - crash due to incorrectly synthesized "nil" default
// argument.
@propertyWrapper
struct Blah<Value> {
  init(blah _: Int) { }

  var wrappedValue: Value {
    let val: Value? = nil
    return val!
  }
}

struct UseRdar57411331 {
  let x = Rdar57411331(other: 5)
}

struct Rdar57411331 {
  @Blah(blah: 17) var something: Int?

  var other: Int
}

// https://github.com/apple/swift/issues/54428
@propertyWrapper
open class OpenPropertyWrapperWithPublicInit {
  public init(wrappedValue: String) { // Okay
    self.wrappedValue = wrappedValue
  }

  open var wrappedValue: String = "Hello, world"
}

// https://github.com/apple/swift/issues/54065
do {
  struct S {}

  class C {
    @Foo var property: S?
  }

  func f<T>(_ argument: T?) -> T? {
    return argument
  }

  let c = C()
  _ = f(c.property) // Okay
}

// rdar://problem/59471019 - property wrapper initializer requires empty parens
// for default init
@propertyWrapper
struct DefaultableIntWrapper {
  var wrappedValue: Int

  init() {
    self.wrappedValue = 0
  }
}

struct TestDefaultableIntWrapper {
  @DefaultableIntWrapper var x
  @DefaultableIntWrapper() var y
  @DefaultableIntWrapper var z: Int

  mutating func test() {
    x = y
    y = z
  }
}

@propertyWrapper
public struct NonVisibleImplicitInit {
// expected-error@-1 {{internal initializer 'init()' cannot have more restrictive access than its enclosing property wrapper type 'NonVisibleImplicitInit' (which is public)}}
  public var wrappedValue: Bool {
    return false
  }
}

@propertyWrapper
struct OptionalWrapper<T> {
  init() {}
  var wrappedValue: T? { nil }
}

struct UseOptionalWrapper {
  @OptionalWrapper var p: Int?? // Okay
}

@propertyWrapper
struct WrapperWithFailableInit<Value> {
  var wrappedValue: Value

  init(_ base: WrapperWithFailableInit<Value>) { fatalError() }

  init?(_ base: WrapperWithFailableInit<Value?>) { fatalError() }
}

struct TestInitError {
  // FIXME: bad diagnostics when a wrapper does not support init from wrapped value

  // expected-error@+2 {{extraneous argument label 'wrappedValue:' in call}}
  // expected-error@+1 {{cannot convert value of type 'Int' to specified type 'WrapperWithFailableInit<Int>'}}
  @WrapperWithFailableInit var value: Int = 10
}
