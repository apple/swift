// RUN: %target-swift-frontend(mock-sdk: %clang-importer-sdk) -emit-sil -I %S/Inputs/custom-modules %s -verify -disable-experimental-clang-importer-diagnostics

// REQUIRES: objc_interop

import AppKit
import AVFoundation

import Newtype
import NewtypeSystem
import objc_ext
import TestProtocols
import TypeAndValue

import ObjCParseExtras
import ObjCParseExtrasToo
import ObjCParseExtrasSystem

func markUsed<T>(_ t: T) {}

func testAnyObject(_ obj: AnyObject) {
  _ = obj.nsstringProperty
}

// Construction
func construction() {
  _ = B()
}

// Subtyping
func treatBAsA(_ b: B) -> A {
  return b
}

// Instance method invocation
func instanceMethods(_ b: B) {
  var i = b.method(1, with: 2.5 as Float)
  i = i + b.method(1, with: 2.5 as Double)

  // BOOL
  b.setEnabled(true)

  // SEL
  b.perform(#selector(NSObject.isEqual(_:)), with: b)
  if let result = b.perform(#selector(B.getAsProto), with: nil) {
    _ = result.takeUnretainedValue()
  }

  // Renaming of redundant parameters.
  b.performAdd(1, withValue:2, withValue2:3, withValue:4) // expected-error{{argument 'withValue' must precede argument 'withValue2'}} {{32-32=withValue:4, }} {{44-57=}}
  b.performAdd(1, withValue:2, withValue:4, withValue2: 3)

  b.performAdd(1, 2, 3, 4) // expected-error{{missing argument labels 'withValue:withValue:withValue2:' in call}} {{19-19=withValue: }} {{22-22=withValue: }} {{25-25=withValue2: }}

  // Both class and instance methods exist.
  _ = b.description
  b.instanceTakesObjectClassTakesFloat(b)
  b.instanceTakesObjectClassTakesFloat(2.0)

  // Instance methods with keyword components
  var obj = NSObject()
  var prot = NSObjectProtocol.self
  b.`protocol`(prot, hasThing:obj)
  b.doThing(obj, protocol: prot)
}

// Class method invocation
func classMethods(_ b: B, other: NSObject) {
  var i = B.classMethod()
  i += B.classMethod(1)
  i += B.classMethod(1, with: 2)

  i += b.classMethod() // expected-error{{static member 'classMethod' cannot be used on instance of type 'B'}}

  // Both class and instance methods exist.
  B.description()
  B.instanceTakesObjectClassTakesFloat(2.0)
  // TODO(diagnostics): Once argument/parameter conversion diagnostics are implemented we should be able to
  // diagnose this as failed conversion from NSObject to Float, but right now the problem is that there
  // exists another overload `instanceTakesObjectClassTakesFloat: (Any?) -> Void` which makes this invocation
  // type-check iff base is an instance of `B`.
  B.instanceTakesObjectClassTakesFloat(other)
  // expected-error@-1 {{instance member 'instanceTakesObjectClassTakesFloat' of type 'B' cannot be used in static context; did you mean to use a value of this type instead?}}

  // Call an instance method of NSObject.
  var c: AnyClass = B.myClass() // no-warning
  c = b.myClass() // no-warning
}

// Instance method invocation on extensions
func instanceMethodsInExtensions(_ b: B) {
  b.method(1, onCat1:2.5)
  b.method(1, onExtA:2.5)
  b.method(1, onExtB:2.5)
  b.method(1, separateExtMethod:3.5)

  let m1 = b.method(_:onCat1:)
  _ = m1(1, 2.5)

  let m2 = b.method(_:onExtA:)
  _ = m2(1, 2.5)

  let m3 = b.method(_:onExtB:)
  _ = m3(1, 2.5)

  let m4 = b.method(_:separateExtMethod:)
  _ = m4(1, 2.5)
}

func dynamicLookupMethod(_ b: AnyObject) {
  if let m5 = b.method(_:separateExtMethod:) {
    _ = m5(1, 2.5)
  }
}

// Properties
func properties(_ b: B) {
  var i = b.counter
  b.counter = i + 1
  i = i + b.readCounter
  b.readCounter = i + 1 // expected-error{{cannot assign to property: 'readCounter' is a get-only property}}

  b.setCounter(5) // expected-error{{value of type 'B' has no member 'setCounter'}}

  // Informal properties in Objective-C map to methods, not variables.
  b.informalProp()

  // An informal property cannot be made formal in a subclass. The
  // formal property is simply ignored.
  b.informalMadeFormal()
  b.informalMadeFormal = i // expected-error{{cannot assign to value: 'informalMadeFormal' is a method}}
  b.setInformalMadeFormal(5)

  b.overriddenProp = 17

  // Dynamic properties.
  var obj : AnyObject = b
  var optStr = obj.nsstringProperty // optStr has type String??
  if optStr != nil {
    var s : String = optStr! // expected-error{{value of optional type 'String?' must be unwrapped}}
    // expected-note@-1{{coalesce}}
    // expected-note@-2{{force-unwrap}}
    var t : String = optStr!!
  }

  // Properties that are Swift keywords
  var prot = b.`protocol`

  // Properties whose accessors run afoul of selector splitting.
  _ = SelectorSplittingAccessors()
}

// Construction.
func newConstruction(_ a: A, aproxy: AProxy) {
  var b : B = B()
  b = B(int: 17)
  b = B(int:17)
  b = B(double:17.5, 3.14159)
  b = B(bbb:b)
  b = B(forWorldDomination:())
  b = B(int: 17, andDouble : 3.14159)
  b = B.new(with: a)
  B.alloc()._initFoo()
  b.notAnInit()

  // init methods are not imported by themselves.
  b.initWithInt(17) // expected-error{{value of type 'B' has no member 'initWithInt'}}

  // init methods on non-NSObject-rooted classes
  AProxy(int: 5) // expected-warning{{unused}}
}

// Indexed subscripting
func indexedSubscripting(_ b: B, idx: Int, a: A) {
  b[idx] = a
  _ = b[idx] as! A
}

// Keyed subscripting
func keyedSubscripting(_ b: B, idx: A, a: A) {
  b[a] = a
  var a2 = b[a] as! A

  let dict = NSMutableDictionary()
  dict[NSString()] = a
  let value = dict[NSString()]

  // notes attached to the partially matching declarations for both following subscripts:
  // - override subscript(_: Any) -> Any? -> 'nil' is not compatible with expected argument type 'Any' at position #1
  // - open subscript(key: NSCopying) -> Any? { get set } -> 'nil' is not compatible with expected argument type 'NSCopying' at position #1

  dict[nil] = a // expected-error {{no exact matches in call to subscript}}
  let q = dict[nil]  // expected-error {{no exact matches in call to subscript}}
  _ = q
}

// Typed indexed subscripting
func checkHive(_ hive: Hive, b: Bee) {
  let b2 = hive.bees[5] as Bee
  b2.buzz()
}

// Protocols
func testProtocols(_ b: B, bp: BProto) {
  var bp2 : BProto = b
  var b2 : B = bp // expected-error{{cannot convert value of type 'any BProto' to specified type 'B'}}
  bp.method(1, with: 2.5 as Float)
  bp.method(1, withFoo: 2.5) // expected-error{{incorrect argument label in call (have '_:withFoo:', expected '_:with:')}}
  bp2 = b.getAsProto()

  var c1 : Cat1Proto = b
  var bcat1 = b.getAsProtoWithCat()!
  c1 = bcat1
  bcat1 = c1 // expected-error{{value of type 'any Cat1Proto' does not conform to 'BProto' in assignment}}
}

// Methods only defined in a protocol
func testProtocolMethods(_ b: B, p2m: P2.Type) {
  b.otherMethod(1, with: 3.14159)
  b.p2Method()
  b.initViaP2(3.14159, second: 3.14159) // expected-error{{value of type 'B' has no member 'initViaP2'}}

  // Imported constructor.
  var b2 = B(viaP2: 3.14159, second: 3.14159)

  _ = p2m.init(viaP2:3.14159, second: 3.14159)
}

func testId(_ x: AnyObject) {
  x.perform!("foo:", with: x) // expected-warning{{no method declared with Objective-C selector 'foo:'}}
  // expected-warning @-1 {{result of call to function returning 'Unmanaged<AnyObject>?' is unused}}

  _ = x.performAdd(1, withValue: 2, withValue: 3, withValue2: 4)
  _ = x.performAdd!(1, withValue: 2, withValue: 3, withValue2: 4)
  _ = x.performAdd?(1, withValue: 2, withValue: 3, withValue2: 4)
}

class MySubclass : B {
  // Override a regular method.
  override func anotherMethodOnB() {}

  // Override a category method
  override func anotherCategoryMethod() {}
}

func getDescription(_ array: NSArray) {
  _ = array.description
}

// Method overriding with unfortunate ordering.
func overridingTest(_ srs: SuperRefsSub) {
  let rs : RefedSub
  rs.overridden()
}

func almostSubscriptableValueMismatch(_ as1: AlmostSubscriptable, a: A) {
  as1[a] // expected-error{{value of type 'AlmostSubscriptable' has no subscripts}}
}

func almostSubscriptableKeyMismatch(_ bc: BadCollection, key: NSString) {
  // FIXME: We end up importing this as read-only due to the mismatch between
  // getter/setter element types.
  var _ : Any = bc[key]
}

func almostSubscriptableKeyMismatchInherited(_ bc: BadCollectionChild,
                                             key: String) {
  var value : Any = bc[key]
  bc[key] = value // expected-error{{cannot assign through subscript: subscript is get-only}}
}

func almostSubscriptableKeyMismatchInherited(_ roc: ReadOnlyCollectionChild,
                                             key: String) {
  var value : Any = roc[key]
  roc[key] = value // expected-error{{cannot assign through subscript: subscript is get-only}}
}

// Use of 'Class' via dynamic lookup.
func classAnyObject(_ obj: NSObject) {
  _ = obj.myClass().description!()
}

// Protocol conformances
class Wobbler : NSWobbling {
  @objc func wobble() { }

  func returnMyself() -> Self { return self }
}

extension Wobbler : NSMaybeInitWobble { // expected-error{{type 'Wobbler' does not conform to protocol 'NSMaybeInitWobble'}}
}

@objc class Wobbler2 : NSObject, NSWobbling {
  func wobble() { }
  func returnMyself() -> Self { return self }
}

extension Wobbler2 : NSMaybeInitWobble { // expected-error{{type 'Wobbler2' does not conform to protocol 'NSMaybeInitWobble'}}
}

func optionalMemberAccess(_ w: NSWobbling) {
  w.wobble()
  w.wibble() // expected-error{{value of optional type '(() -> Void)?' must be unwrapped to a value of type '() -> Void'}}
  // expected-note@-1{{coalesce}}
  // expected-note@-2{{force-unwrap}}
  let x = w[5]!!
  _ = x
}

func protocolInheritance(_ s: NSString) {
  var _: NSCoding = s
}

func ivars(_ hive: Hive) {
  var d = hive.bees.description
  hive.queen.description() // expected-error{{value of type 'Hive' has no member 'queen'}}
}

class NSObjectable : NSObjectProtocol { // expected-error {{cannot declare conformance to 'NSObjectProtocol' in Swift; 'NSObjectable' should inherit 'NSObject' instead}}
  @objc var description : String { return "" }
  @objc(conformsToProtocol:) func conforms(to _: Protocol) -> Bool { return false }
  @objc(isKindOfClass:) func isKind(of aClass: AnyClass) -> Bool { return false }
}


// Properties with custom accessors
func customAccessors(_ hive: Hive, bee: Bee) {
  markUsed(hive.isMakingHoney)
  markUsed(hive.makingHoney()) // expected-error{{cannot call value of non-function type 'Bool'}}{{28-30=}}
  hive.setMakingHoney(true) // expected-error{{value of type 'Hive' has no member 'setMakingHoney'}}

  _ = (hive.`guard` as AnyObject).description // okay
  _ = (hive.`guard` as AnyObject).description! // no-warning
  hive.`guard` = bee // no-warning
}

// Properties with bool don't use the getter.
func boolProperties(_ hive: Hive) {
  markUsed(hive.empty)
}

// instancetype/Dynamic Self invocation.
func testDynamicSelf(_ queen: Bee, wobbler: NSWobbling) {
  var hive = Hive()

  // Factory method with instancetype result.
  var hive1 = Hive(queen: queen)
  hive1 = hive
  hive = hive1

  // Instance method with instancetype result.
  var hive2 = hive!.visit()
  hive2 = hive
  hive = hive2

  // Instance method on a protocol with instancetype result.
  var wobbler2 = wobbler.returnMyself()!
  var wobbler: NSWobbling = wobbler2
  wobbler2 = wobbler

  // Instance method on a base class with instancetype result, called on the
  // class itself.
  // FIXME: This should be accepted.
  let baseClass: ObjCParseExtras.Base.Type = ObjCParseExtras.Base.returnMyself()
  // expected-error@-1 {{instance member 'returnMyself' of type 'Base' cannot be used in static context; did you mean to use a value of this type instead?}}
  // expected-error@-2 {{cannot convert value of type 'Base?' to specified type 'Base.Type'}}
}

func testRepeatedProtocolAdoption(_ w: NSWindow) {
  _ = w.description
}

class ProtocolAdopter1 : FooProto {
  @objc var bar: CInt // no-warning

  init() { bar = 5 }
}
class ProtocolAdopter2 : FooProto {
  @objc var bar: CInt {
    get { return 42 }
    set { /* do nothing! */ }
  }
}
class ProtocolAdopterBad1 : FooProto { // expected-error {{type 'ProtocolAdopterBad1' does not conform to protocol 'FooProto'}}
  @objc var bar: Int = 0 // expected-note {{candidate has non-matching type 'Int'}}
}
class ProtocolAdopterBad2 : FooProto { // expected-error {{type 'ProtocolAdopterBad2' does not conform to protocol 'FooProto'}}
  let bar: CInt = 0 // expected-note {{candidate is not settable, but protocol requires it}}
}
class ProtocolAdopterBad3 : FooProto { // expected-error {{type 'ProtocolAdopterBad3' does not conform to protocol 'FooProto'}}
  var bar: CInt { // expected-note {{candidate is not settable, but protocol requires it}}
    return 42
  }
}

@objc protocol RefinedFooProtocol : FooProto {}

func testPreferClassMethodToCurriedInstanceMethod(_ obj: NSObject) {
  // FIXME: We shouldn't need the ": Bool" type annotation here.
  // <rdar://problem/18006008>
  let _: Bool = NSObject.isEqual(obj)
  _ = NSObject.isEqual(obj) as (NSObject?) -> Bool // no-warning
}


func testPropertyAndMethodCollision(_ obj: PropertyAndMethodCollision,
                                    rev: PropertyAndMethodReverseCollision) {
  obj.object = nil
  obj.object(obj, doSomething:#selector(getter: NSMenuItem.action))

  type(of: obj).classRef = nil
  type(of: obj).classRef(obj, doSomething:#selector(getter: NSMenuItem.action))

  rev.object = nil
  rev.object(rev, doSomething:#selector(getter: NSMenuItem.action))

  type(of: rev).classRef = nil
  type(of: rev).classRef(rev, doSomething:#selector(getter: NSMenuItem.action))

  var value: Any
  value = obj.protoProp()
  value = obj.protoPropRO()
  value = type(of: obj).protoClassProp()
  value = type(of: obj).protoClassPropRO()
  _ = value
}

func testPropertyAndMethodCollisionInOneClass(
  obj: PropertyAndMethodCollisionInOneClass,
  rev: PropertyAndMethodReverseCollisionInOneClass
) {
  obj.object = nil
  obj.object()

  type(of: obj).classRef = nil
  type(of: obj).classRef()

  rev.object = nil
  rev.object()

  type(of: rev).classRef = nil
  type(of: rev).classRef()
}

func testSubscriptAndPropertyRedeclaration(_ obj: SubscriptAndProperty) {
  _ = obj.x
  obj.x = 5
  _ = obj.objectAtIndexedSubscript(5) // expected-error{{'objectAtIndexedSubscript' is unavailable: use subscripting}}
  obj.setX(5) // expected-error{{value of type 'SubscriptAndProperty' has no member 'setX'}}

  _ = obj[0]
  obj[1] = obj
  obj.setObject(obj, atIndexedSubscript: 2) // expected-error{{'setObject(_:atIndexedSubscript:)' is unavailable: use subscripting}}
}

func testSubscriptAndPropertyWithProtocols(_ obj: SubscriptAndPropertyWithProto) {
  _ = obj.x
  obj.x = 5
  obj.setX(5) // expected-error{{value of type 'SubscriptAndPropertyWithProto' has no member 'setX'}}

  _ = obj[0]
  obj[1] = obj
  obj.setObject(obj, atIndexedSubscript: 2) // expected-error{{'setObject(_:atIndexedSubscript:)' is unavailable: use subscripting}}
}

func testProtocolMappingSameModule(_ obj: AVVideoCompositionInstruction, p: AVVideoCompositionInstructionProtocol) {
  markUsed(p.enablePostProcessing)
  markUsed(obj.enablePostProcessing)
  _ = obj.backgroundColor
}

func testProtocolMappingDifferentModules(_ obj: ObjCParseExtrasToo.ProtoOrClass, p: ObjCParseExtras.ProtoOrClass) {
  markUsed(p.thisIsTheProto)
  markUsed(obj.thisClassHasAnAwfulName)

  let _: ProtoOrClass? // expected-error{{'ProtoOrClass' is ambiguous for type lookup in this context}}

  _ = ObjCParseExtrasToo.ClassInHelper() // expected-error{{'any ClassInHelper' cannot be constructed because it has no accessible initializers}}
  _ = ObjCParseExtrasToo.ProtoInHelper()
  _ = ObjCParseExtrasTooHelper.ClassInHelper()
  _ = ObjCParseExtrasTooHelper.ProtoInHelper() // expected-error{{'any ProtoInHelper' cannot be constructed because it has no accessible initializers}}
}

func testProtocolClassShadowing(_ obj: ClassInHelper, p: ProtoInHelper) {
  let _: ObjCParseExtrasToo.ClassInHelper = obj
  let _: ObjCParseExtrasToo.ProtoInHelper = p
}


func testDealloc(_ obj: NSObject) {
  // dealloc is subsumed by deinit.
  obj.dealloc() // expected-error{{'dealloc()' is unavailable in Swift: use 'deinit' to define a de-initializer}}
}

func testConstantGlobals() {
  markUsed(MAX)
  markUsed(SomeImageName)
  markUsed(SomeNumber.description)

  MAX = 5 // expected-error{{cannot assign to value: 'MAX' is a 'let' constant}}
  SomeImageName = "abc" // expected-error{{cannot assign to value: 'SomeImageName' is a 'let' constant}}
  SomeNumber = nil // expected-error{{cannot assign to value: 'SomeNumber' is a 'let' constant}}
}

func testWeakVariable() {
  let _: AnyObject = globalWeakVar
}

class IncompleteProtocolAdopter : Incomplete, IncompleteOptional { // expected-error {{type 'IncompleteProtocolAdopter' cannot conform to protocol 'Incomplete' because it has requirements that cannot be satisfied}}
  @objc func getObject() -> AnyObject { return self }
}

func testNullarySelectorPieces(_ obj: AnyObject) {
  obj.foo(1, bar: 2, 3) // no-warning
  obj.foo(1, 2, bar: 3) // expected-error{{argument 'bar' must precede unnamed argument #2}}
}

func testFactoryMethodAvailability() {
  _ = DeprecatedFactoryMethod() // expected-warning{{'init()' is deprecated: use something newer}}
}

func testRepeatedMembers(_ obj: RepeatedMembers) {
  obj.repeatedMethod()
}

// rdar://problem/19726164
class FooDelegateImpl : NSObject, FooDelegate {
  var _started = false
  var isStarted: Bool {
    get { return _started }
    @objc(setStarted:) set { _started = newValue }
  }
}

class ProtoAdopter : NSObject, ExplicitSetterProto, OptionalSetterProto {
  var foo: Any? // no errors about conformance
  var bar: Any? // no errors about conformance
}

func testUnusedResults(_ ur: UnusedResults) {
  _ = ur.producesResult()
  ur.producesResult() // expected-warning{{result of call to 'producesResult()' is unused}}
}

func testStrangeSelectors(obj: StrangeSelectors) {
  StrangeSelectors.cStyle(0, 1, 2) // expected-error{{type 'StrangeSelectors' has no member 'cStyle'}}
  _ = StrangeSelectors(a: 0, b: 0) // okay
  obj.empty(1, 2) // okay
}

func testProtocolQualified(_ obj: CopyableNSObject, cell: CopyableSomeCell,
                           plainObj: NSObject, plainCell: SomeCell) {
  _ = obj as NSObject // expected-error {{'CopyableNSObject' (aka 'any NSCopying & NSObjectProtocol') is not convertible to 'NSObject'}}
  // expected-note@-1 {{did you mean to use 'as!' to force downcast?}} {{11-13=as!}}
  _ = obj as NSObjectProtocol
  _ = obj as NSCopying
  _ = obj as SomeCell // expected-error {{'CopyableNSObject' (aka 'any NSCopying & NSObjectProtocol') is not convertible to 'SomeCell'}}
  // expected-note@-1 {{did you mean to use 'as!' to force downcast?}} {{11-13=as!}}

  _ = cell as NSObject
  _ = cell as NSObjectProtocol
  _ = cell as NSCopying
  _ = cell as SomeCell
  
  _ = plainObj as CopyableNSObject // expected-error {{cannot convert value of type 'NSObject' to type 'CopyableNSObject' (aka 'any NSCopying & NSObjectProtocol') in coercion}}
  _ = plainCell as CopyableSomeCell // expected-error {{cannot convert value of type 'SomeCell' to type 'CopyableSomeCell' (aka 'any SomeCell & NSCopying') in coercion}}
}

extension Printing {
  func testImplicitWarnUnqualifiedAccess() {
    print() // expected-warning {{use of 'print' treated as a reference to instance method in class 'Printing'}}
    // expected-note@-1 {{use 'self.' to silence this warning}} {{5-5=self.}}
    // expected-note@-2 {{use 'Swift.' to reference the global function}} {{5-5=Swift.}}

    print(self) // expected-warning {{use of 'print' treated as a reference to instance method in class 'Printing'}}
    // expected-note@-1 {{use 'self.' to silence this warning}} {{5-5=self.}}
    // expected-note@-2 {{use 'Swift.' to reference the global function}} {{5-5=Swift.}}

    print(self, options: self) // no-warning
  }

  static func testImplicitWarnUnqualifiedAccess() {
    print() // expected-warning {{use of 'print' treated as a reference to class method in class 'Printing'}}
    // expected-note@-1 {{use 'self.' to silence this warning}} {{5-5=self.}}
    // expected-note@-2 {{use 'Swift.' to reference the global function}} {{5-5=Swift.}}

    print(self) // expected-warning {{use of 'print' treated as a reference to class method in class 'Printing'}}
    // expected-note@-1 {{use 'self.' to silence this warning}} {{5-5=self.}}
    // expected-note@-2 {{use 'Swift.' to reference the global function}} {{5-5=Swift.}}

    print(self, options: self) // no-warning
  }
}

// <rdar://problem/21979968> The "with array" initializers for NSCountedSet and NSMutable set should be properly disambiguated.
func testSetInitializers() {
  let a: [AnyObject] = [NSObject()]

  let _ = NSCountedSet(array: a)
  let _ = NSMutableSet(array: a)
}


func testNSUInteger(_ obj: NSUIntegerTests, uint: UInt, int: Int) {
  obj.consumeUnsigned(uint) // okay
  obj.consumeUnsigned(int) // expected-error {{cannot convert value of type 'Int' to expected argument type 'UInt'}}

  obj.consumeUnsigned(0, withTheNSUIntegerHere: uint) // expected-error {{cannot convert value of type 'UInt' to expected argument type 'Int'}}
  obj.consumeUnsigned(0, withTheNSUIntegerHere: int) // okay

  obj.consumeUnsigned(uint, andAnother: uint) // expected-error {{cannot convert value of type 'UInt' to expected argument type 'Int'}}
  obj.consumeUnsigned(uint, andAnother: int) // okay

  do {
    let x = obj.unsignedProducer()
    let _: String = x // expected-error {{cannot convert value of type 'UInt' to specified type 'String'}}
  }

  do {
    obj.unsignedProducer(uint, fromCount: uint) // expected-error {{cannot convert value of type 'UInt' to expected argument type 'Int'}}
    obj.unsignedProducer(int, fromCount: int) // expected-error {{cannot convert value of type 'Int' to expected argument type 'UInt'}}
    let x = obj.unsignedProducer(uint, fromCount: int) // okay
    let _: String = x // expected-error {{cannot convert value of type 'UInt' to specified type 'String'}}
  }

  do {
    obj.normalProducer(uint, fromUnsigned: uint) // expected-error {{cannot convert value of type 'UInt' to expected argument type 'Int'}}
    obj.normalProducer(int, fromUnsigned: int) // expected-error {{cannot convert value of type 'Int' to expected argument type 'UInt'}}
    let x = obj.normalProducer(int, fromUnsigned: uint)
    let _: String = x // expected-error {{cannot convert value of type 'Int' to specified type 'String'}}
  }

  let _: String = obj.normalProp // expected-error {{cannot convert value of type 'Int' to specified type 'String'}}
  let _: String = obj.unsignedProp // expected-error {{cannot convert value of type 'UInt' to specified type 'String'}}

  do {
    testUnsigned(int, uint) // expected-error {{cannot convert value of type 'Int' to expected argument type 'UInt'}}
    testUnsigned(uint, int) // expected-error {{cannot convert value of type 'Int' to expected argument type 'UInt'}}
    let x = testUnsigned(uint, uint) // okay
    let _: String = x // expected-error {{cannot convert value of type 'UInt' to specified type 'String'}}
  }

  // NSNumber
  let num = NSNumber(value: uint)
  let _: String = num.uintValue // expected-error {{cannot convert value of type 'UInt' to specified type 'String'}}
}

class NewtypeUser {
  @objc func stringNewtype(a: SNTErrorDomain) {} // expected-error {{'SNTErrorDomain' has been renamed to 'ErrorDomain'}}{{31-45=ErrorDomain}}
  @objc func stringNewtypeOptional(a: SNTErrorDomain?) {} // expected-error {{'SNTErrorDomain' has been renamed to 'ErrorDomain'}}{{39-53=ErrorDomain}}
  @objc func intNewtype(a: MyInt) {}
  @objc func intNewtypeOptional(a: MyInt?) {} // expected-error {{method cannot be marked @objc because the type of the parameter cannot be represented in Objective-C}}
  @objc func intNewtypeArray(a: [MyInt]) {} // expected-error {{method cannot be marked @objc because the type of the parameter cannot be represented in Objective-C}}
  // expected-note@-1 {{Swift structs cannot be represented in Objective-C}}
  @objc func intNewtypeDictionary(a: [MyInt: NSObject]) {} // expected-error {{method cannot be marked @objc because the type of the parameter cannot be represented in Objective-C}}
  // expected-note@-1 {{Swift structs cannot be represented in Objective-C}}
  @objc func cfNewtype(a: CFNewType) {}
  @objc func cfNewtypeArray(a: [CFNewType]) {} // expected-error {{method cannot be marked @objc because the type of the parameter cannot be represented in Objective-C}}
  // expected-note@-1 {{Swift structs cannot be represented in Objective-C}}

  typealias MyTuple = (Int, AnyObject?)
  typealias MyNamedTuple = (a: Int, b: AnyObject?)
  
  @objc func blockWithTypealias(_ input: @escaping (MyTuple) -> MyInt) {}
  // expected-error@-1{{method cannot be marked @objc because the type of the parameter cannot be represented in Objective-C}}
  // expected-note@-2{{function types cannot be represented in Objective-C}}

  @objc func blockWithTypealiasWithNames(_ input: (MyNamedTuple) -> MyInt) {}
  // expected-error@-1{{method cannot be marked @objc because the type of the parameter cannot be represented in Objective-C}}
  // expected-note@-2{{function types cannot be represented in Objective-C}}
}

func testTypeAndValue() {
  _ = testStruct()
  _ = testStruct(value: 0)
  let _: (testStruct) -> Void = testStruct
  let _: () -> testStruct = testStruct.init
  let _: (CInt) -> testStruct = testStruct.init
}

// rdar://problem/34913507
func testBridgedTypedef(bt: BridgedTypedefs) {
  let _: Int = bt.arrayOfArrayOfStrings // expected-error{{'[[String]]'}}
}

func testBridgeFunctionPointerTypedefs(fptrTypedef: FPTypedef) {
  // See also print_clang_bool_bridging.swift.
  let _: Int = fptrTypedef // expected-error{{'@convention(c) (String) -> String'}}
  let _: Int = getFP() // expected-error{{'@convention(c) (String) -> String'}}
}

func testNonTrivialStructs() {
  _ = NonTrivialToCopy() // expected-error {{cannot find 'NonTrivialToCopy' in scope}}
  _ = NonTrivialToCopyWrapper() // expected-error {{cannot find 'NonTrivialToCopyWrapper' in scope}}
  _ = TrivialToCopy() // okay
}

func testErrorNewtype() {
  _ = ErrorNewType(3) // expected-error {{argument type 'Int' does not conform to expected type 'Error'}}

  // Since we import NSError as Error, and Error is not Hashable...we end up
  // losing the types for these functions, even though the above assignment 
  // works.
  testErrorDictionary(3) // expected-error {{cannot convert value of type 'Int' to expected argument type '[AnyHashable : String]'}}
  testErrorDictionaryNewtype(3) // expected-error {{cannot convert value of type 'Int' to expected argument type '[AnyHashable : String]'}}
}

func testNSUIntegerNewtype() {
  let _: NSUIntegerNewType = NSUIntegerNewType(4)
  let _: UInt = NSUIntegerNewType(4).rawValue
  let _: NSUIntegerNewType = NSUIntegerNewType.constant

  let _: NSUIntegerSystemNewType = NSUIntegerSystemNewType(4)
  let _: Int = NSUIntegerSystemNewType(4).rawValue
  let _: NSUIntegerSystemNewType = NSUIntegerSystemNewType.constant
}
