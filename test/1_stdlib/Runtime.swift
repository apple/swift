// RUN: rm -rf %t  &&  mkdir %t
//
// RUN: %target-build-swift -parse-stdlib -module-name a %s -o %t.out
// RUN: %target-run %t.out
// REQUIRES: executable_test

import Swift
import StdlibUnittest
import SwiftShims



var swiftObjectCanaryCount = 0
class SwiftObjectCanary {
  init() {
    swiftObjectCanaryCount += 1
  }
  deinit {
    swiftObjectCanaryCount -= 1
  }
}

struct SwiftObjectCanaryStruct {
  var ref = SwiftObjectCanary()
}

var Runtime = TestSuite("Runtime")

Runtime.test("_canBeClass") {
  expectEqual(1, _canBeClass(SwiftObjectCanary.self))
  expectEqual(0, _canBeClass(SwiftObjectCanaryStruct.self))

  typealias SwiftClosure = () -> ()
  expectEqual(0, _canBeClass(SwiftClosure.self))
}

//===----------------------------------------------------------------------===//

// The protocol should be defined in the standard library, otherwise the cast
// does not work.
typealias P1 = Boolean
typealias P2 = CustomStringConvertible
protocol Q1 {}

// A small struct that can be stored inline in an opaque buffer.
struct StructConformsToP1 : Boolean, Q1 {
  var boolValue: Bool {
    return true
  }
}

// A small struct that can be stored inline in an opaque buffer.
struct Struct2ConformsToP1<T : Boolean> : Boolean, Q1 {
  init(_ value: T) {
    self.value = value
  }
  var boolValue: Bool {
    return value.boolValue
  }
  var value: T
}

// A large struct that cannot be stored inline in an opaque buffer.
struct Struct3ConformsToP2 : CustomStringConvertible, Q1 {
  var a: UInt64 = 10
  var b: UInt64 = 20
  var c: UInt64 = 30
  var d: UInt64 = 40

  var description: String {
    // Don't rely on string interpolation, it uses the casts that we are trying
    // to test.
    var result = ""
    result += _uint64ToString(a) + " "
    result += _uint64ToString(b) + " "
    result += _uint64ToString(c) + " "
    result += _uint64ToString(d)
    return result
  }
}

// A large struct that cannot be stored inline in an opaque buffer.
struct Struct4ConformsToP2<T : CustomStringConvertible> : CustomStringConvertible, Q1 {
  var value: T
  var e: UInt64 = 50
  var f: UInt64 = 60
  var g: UInt64 = 70
  var h: UInt64 = 80

  init(_ value: T) {
    self.value = value
  }

  var description: String {
    // Don't rely on string interpolation, it uses the casts that we are trying
    // to test.
    var result = value.description + " "
    result += _uint64ToString(e) + " "
    result += _uint64ToString(f) + " "
    result += _uint64ToString(g) + " "
    result += _uint64ToString(h)
    return result
  }
}

struct StructDoesNotConformToP1 : Q1 {}

class ClassConformsToP1 : Boolean, Q1 {
  var boolValue: Bool {
    return true
  }
}

class Class2ConformsToP1<T : Boolean> : Boolean, Q1 {
  init(_ value: T) {
    self.value = [ value ]
  }
  var boolValue: Bool {
    return value[0].boolValue
  }
  // FIXME: should be "var value: T", but we don't support it now.
  var value: Array<T>
}

class ClassDoesNotConformToP1 : Q1 {}

Runtime.test("dynamicCasting with as") {
  var someP1Value = StructConformsToP1()
  var someP1Value2 = Struct2ConformsToP1(true)
  var someNotP1Value = StructDoesNotConformToP1()
  var someP2Value = Struct3ConformsToP2()
  var someP2Value2 = Struct4ConformsToP2(Struct3ConformsToP2())
  var someP1Ref = ClassConformsToP1()
  var someP1Ref2 = Class2ConformsToP1(true)
  var someNotP1Ref = ClassDoesNotConformToP1()

  expectTrue(someP1Value is P1)
  expectTrue(someP1Value2 is P1)
  expectFalse(someNotP1Value is P1)
  expectTrue(someP2Value is P2)
  expectTrue(someP2Value2 is P2)
  expectTrue(someP1Ref is P1)
  expectTrue(someP1Ref2 is P1)
  expectFalse(someNotP1Ref is P1)

  expectTrue(someP1Value as P1 is P1)
  expectTrue(someP1Value2 as P1 is P1)
  expectTrue(someP2Value as P2 is P2)
  expectTrue(someP2Value2 as P2 is P2)
  expectTrue(someP1Ref as P1 is P1)

  expectTrue(someP1Value as Q1 is P1)
  expectTrue(someP1Value2 as Q1 is P1)
  expectFalse(someNotP1Value as Q1 is P1)
  expectTrue(someP2Value as Q1 is P2)
  expectTrue(someP2Value2 as Q1 is P2)
  expectTrue(someP1Ref as Q1 is P1)
  expectTrue(someP1Ref2 as Q1 is P1)
  expectFalse(someNotP1Ref as Q1 is P1)

  expectTrue(someP1Value as Any is P1)
  expectTrue(someP1Value2 as Any is P1)
  expectFalse(someNotP1Value as Any is P1)
  expectTrue(someP2Value as Any is P2)
  expectTrue(someP2Value2 as Any is P2)
  expectTrue(someP1Ref as Any is P1)
  expectTrue(someP1Ref2 as Any is P1)
  expectFalse(someNotP1Ref as Any is P1)

  expectTrue(someP1Ref as AnyObject is P1)
  expectTrue(someP1Ref2 as AnyObject is P1)
  expectFalse(someNotP1Ref as AnyObject is P1)

  expectTrue((someP1Value as P1).boolValue)
  expectTrue((someP1Value2 as P1).boolValue)
  expectEqual("10 20 30 40", (someP2Value as P2).description)
  expectEqual("10 20 30 40 50 60 70 80", (someP2Value2 as P2).description)

  expectTrue((someP1Ref as P1).boolValue)
  expectTrue((someP1Ref2 as P1).boolValue)

  expectTrue(((someP1Value as Q1) as! P1).boolValue)
  expectTrue(((someP1Value2 as Q1) as! P1).boolValue)
  expectEqual("10 20 30 40", ((someP2Value as Q1) as! P2).description)
  expectEqual("10 20 30 40 50 60 70 80",
    ((someP2Value2 as Q1) as! P2).description)
  expectTrue(((someP1Ref as Q1) as! P1).boolValue)
  expectTrue(((someP1Ref2 as Q1) as! P1).boolValue)

  expectTrue(((someP1Value as Any) as! P1).boolValue)
  expectTrue(((someP1Value2 as Any) as! P1).boolValue)
  expectEqual("10 20 30 40", ((someP2Value as Any) as! P2).description)
  expectEqual("10 20 30 40 50 60 70 80",
    ((someP2Value2 as Any) as! P2).description)
  expectTrue(((someP1Ref as Any) as! P1).boolValue)
  expectTrue(((someP1Ref2 as Any) as! P1).boolValue)

  expectTrue(((someP1Ref as AnyObject) as! P1).boolValue)

  expectEmpty((someNotP1Value as? P1))
  expectEmpty((someNotP1Ref as? P1))

  expectTrue(((someP1Value as Q1) as? P1)!.boolValue)
  expectTrue(((someP1Value2 as Q1) as? P1)!.boolValue)
  expectEmpty(((someNotP1Value as Q1) as? P1))
  expectEqual("10 20 30 40", ((someP2Value as Q1) as? P2)!.description)
  expectEqual("10 20 30 40 50 60 70 80",
    ((someP2Value2 as Q1) as? P2)!.description)
  expectTrue(((someP1Ref as Q1) as? P1)!.boolValue)
  expectTrue(((someP1Ref2 as Q1) as? P1)!.boolValue)
  expectEmpty(((someNotP1Ref as Q1) as? P1))

  expectTrue(((someP1Value as Any) as? P1)!.boolValue)
  expectTrue(((someP1Value2 as Any) as? P1)!.boolValue)
  expectEmpty(((someNotP1Value as Any) as? P1))
  expectEqual("10 20 30 40", ((someP2Value as Any) as? P2)!.description)
  expectEqual("10 20 30 40 50 60 70 80",
    ((someP2Value2 as Any) as? P2)!.description)
  expectTrue(((someP1Ref as Any) as? P1)!.boolValue)
  expectTrue(((someP1Ref2 as Any) as? P1)!.boolValue)
  expectEmpty(((someNotP1Ref as Any) as? P1))

  expectTrue(((someP1Ref as AnyObject) as? P1)!.boolValue)
  expectTrue(((someP1Ref2 as AnyObject) as? P1)!.boolValue)
  expectEmpty(((someNotP1Ref as AnyObject) as? P1))

  let doesThrow: Int throws -> Int = { $0 }
  let doesNotThrow: String -> String = { $0 }

  var any: Any = doesThrow

  expectTrue(doesThrow as Any is Int throws -> Int)
  expectFalse(doesThrow as Any is String throws -> Int)
  expectFalse(doesThrow as Any is String throws -> String)
  expectFalse(doesThrow as Any is Int throws -> String)
  expectFalse(doesThrow as Any is Int -> Int)
  expectFalse(doesThrow as Any is String throws -> String)
  expectFalse(doesThrow as Any is String -> String)
  expectTrue(doesNotThrow as Any is String throws -> String)
  expectTrue(doesNotThrow as Any is String -> String)
  expectFalse(doesNotThrow as Any is Int -> String)
  expectFalse(doesNotThrow as Any is Int -> Int)
  expectFalse(doesNotThrow as Any is String -> Int)
  expectFalse(doesNotThrow as Any is Int throws -> Int)
  expectFalse(doesNotThrow as Any is Int -> Int)
}

extension Int {
  class ExtensionClassConformsToP2 : P2 {
    var description: String { return "abc" }
  }

  private class PrivateExtensionClassConformsToP2 : P2 {
    var description: String { return "def" }
  }
}

Runtime.test("dynamic cast to existential with cross-module extensions") {
  let internalObj = Int.ExtensionClassConformsToP2()
  let privateObj = Int.PrivateExtensionClassConformsToP2()

  expectTrue(internalObj is P2)
  expectTrue(privateObj is P2)
}

class SomeClass {}
struct SomeStruct {}
enum SomeEnum {
  case A
  init() { self = .A }
}

Runtime.test("typeName") {
  expectEqual("a.SomeClass", _typeName(SomeClass.self))
  expectEqual("a.SomeStruct", _typeName(SomeStruct.self))
  expectEqual("a.SomeEnum", _typeName(SomeEnum.self))
  expectEqual("protocol<>.Protocol", _typeName(Any.Protocol.self))
  expectEqual("Swift.AnyObject.Protocol", _typeName(AnyObject.Protocol.self))
  expectEqual("Swift.AnyObject.Type.Protocol", _typeName(AnyClass.Protocol.self))
  expectEqual("Swift.Optional<Swift.AnyObject>.Type", _typeName((AnyObject?).Type.self))

  var a: Any = SomeClass()
  expectEqual("a.SomeClass", _typeName(a.dynamicType))

  a = SomeStruct()
  expectEqual("a.SomeStruct", _typeName(a.dynamicType))

  a = SomeEnum()
  expectEqual("a.SomeEnum", _typeName(a.dynamicType))

  a = AnyObject.self
  expectEqual("Swift.AnyObject.Protocol", _typeName(a.dynamicType))

  a = AnyClass.self
  expectEqual("Swift.AnyObject.Type.Protocol", _typeName(a.dynamicType))

  a = (AnyObject?).self
  expectEqual("Swift.Optional<Swift.AnyObject>.Type",
    _typeName(a.dynamicType))

  a = Any.self
  expectEqual("protocol<>.Protocol", _typeName(a.dynamicType))
}

class SomeSubclass : SomeClass {}

protocol SomeProtocol {}
class SomeConformingClass : SomeProtocol {}

Runtime.test("typeByName") {
  expectTrue(_typeByName("a.SomeClass") == SomeClass.self)
  expectTrue(_typeByName("a.SomeSubclass") == SomeSubclass.self)
  // name lookup will be via protocol conformance table
  expectTrue(_typeByName("a.SomeConformingClass") == SomeConformingClass.self)
  // FIXME: NonObjectiveCBase is slated to die, but I can't think of another
  // nongeneric public class in the stdlib...
  expectTrue(_typeByName("Swift.NonObjectiveCBase") == NonObjectiveCBase.self)
}

Runtime.test("demangleName") {
  expectEqual("", _stdlib_demangleName(""))
  expectEqual("abc", _stdlib_demangleName("abc"))
  expectEqual("\0", _stdlib_demangleName("\0"))
  expectEqual("Swift.Double", _stdlib_demangleName("_TtSd"))
  expectEqual("x.a : x.Foo<x.Foo<x.Foo<Swift.Int, Swift.Int>, x.Foo<Swift.Int, Swift.Int>>, x.Foo<x.Foo<Swift.Int, Swift.Int>, x.Foo<Swift.Int, Swift.Int>>>",
      _stdlib_demangleName("_Tv1x1aGCS_3FooGS0_GS0_SiSi_GS0_SiSi__GS0_GS0_SiSi_GS0_SiSi___"))
  expectEqual("Foobar", _stdlib_demangleName("_TtC13__lldb_expr_46Foobar"))
}

Runtime.test("_stdlib_atomicCompareExchangeStrongPtr") {
  typealias IntPtr = UnsafeMutablePointer<Int>
  var origP1 = IntPtr(bitPattern: 0x10101010)
  var origP2 = IntPtr(bitPattern: 0x20202020)
  var origP3 = IntPtr(bitPattern: 0x30303030)

  do {
    var object = origP1
    var expected = origP1
    let r = _stdlib_atomicCompareExchangeStrongPtr(
      object: &object, expected: &expected, desired: origP2)
    expectTrue(r)
    expectEqual(origP2, object)
    expectEqual(origP1, expected)
  }
  do {
    var object = origP1
    var expected = origP2
    let r = _stdlib_atomicCompareExchangeStrongPtr(
      object: &object, expected: &expected, desired: origP3)
    expectFalse(r)
    expectEqual(origP1, object)
    expectEqual(origP1, expected)
  }

  struct FooStruct {
    var i: Int
    var object: IntPtr
    var expected: IntPtr

    init(_ object: IntPtr, _ expected: IntPtr) {
      self.i = 0
      self.object = object
      self.expected = expected
    }
  }
  do {
    var foo = FooStruct(origP1, origP1)
    let r = _stdlib_atomicCompareExchangeStrongPtr(
      object: &foo.object, expected: &foo.expected, desired: origP2)
    expectTrue(r)
    expectEqual(origP2, foo.object)
    expectEqual(origP1, foo.expected)
  }
  do {
    var foo = FooStruct(origP1, origP2)
    let r = _stdlib_atomicCompareExchangeStrongPtr(
      object: &foo.object, expected: &foo.expected, desired: origP3)
    expectFalse(r)
    expectEqual(origP1, foo.object)
    expectEqual(origP1, foo.expected)
  }
}

Runtime.test("casting AnyObject to class metatypes") {
  do {
    var ao: AnyObject = SomeClass()
    expectTrue(ao as? Any.Type == nil)
    expectTrue(ao as? AnyClass == nil)
  }

  do {
    var a: Any = SomeClass()
    expectTrue(a as? Any.Type == nil)
    expectTrue(a as? AnyClass == nil)
    
    a = SomeClass.self
    expectTrue(a as? Any.Type == SomeClass.self)
    expectTrue(a as? AnyClass == SomeClass.self)
    expectTrue(a as? SomeClass.Type == SomeClass.self)
  }
}

class Malkovich: Malkovichable {
  var malkovich: String { return "malkovich" }
}
protocol Malkovichable: class {
  var malkovich: String { get }
}

struct GenericStructWithReferenceStorage<T> {
  var a: T
  unowned(safe)   var unownedConcrete: Malkovich
  unowned(unsafe) var unmanagedConcrete: Malkovich
  weak            var weakConcrete: Malkovich?

  unowned(safe)   var unownedProto: Malkovichable
  unowned(unsafe) var unmanagedProto: Malkovichable
  weak            var weakProto: Malkovichable?
}

func exerciseReferenceStorageInGenericContext<T>(
    _ x: GenericStructWithReferenceStorage<T>,
    forceCopy y: GenericStructWithReferenceStorage<T>
) {
  expectEqual(x.unownedConcrete.malkovich, "malkovich")
  expectEqual(x.unmanagedConcrete.malkovich, "malkovich")
  expectEqual(x.weakConcrete!.malkovich, "malkovich")
  expectEqual(x.unownedProto.malkovich, "malkovich")
  expectEqual(x.unmanagedProto.malkovich, "malkovich")
  expectEqual(x.weakProto!.malkovich, "malkovich")

  expectEqual(y.unownedConcrete.malkovich, "malkovich")
  expectEqual(y.unmanagedConcrete.malkovich, "malkovich")
  expectEqual(y.weakConcrete!.malkovich, "malkovich")
  expectEqual(y.unownedProto.malkovich, "malkovich")
  expectEqual(y.unmanagedProto.malkovich, "malkovich")
  expectEqual(y.weakProto!.malkovich, "malkovich")
}

Runtime.test("Struct layout with reference storage types") {
  let malkovich = Malkovich()

  let x = GenericStructWithReferenceStorage(a:                 malkovich,
                                            unownedConcrete:   malkovich,
                                            unmanagedConcrete: malkovich,
                                            weakConcrete:      malkovich,
                                            unownedProto:      malkovich,
                                            unmanagedProto:    malkovich,
                                            weakProto:         malkovich)
  exerciseReferenceStorageInGenericContext(x, forceCopy: x)

  expectEqual(x.unownedConcrete.malkovich, "malkovich")
  expectEqual(x.unmanagedConcrete.malkovich, "malkovich")
  expectEqual(x.weakConcrete!.malkovich, "malkovich")
  expectEqual(x.unownedProto.malkovich, "malkovich")
  expectEqual(x.unmanagedProto.malkovich, "malkovich")
  expectEqual(x.weakProto!.malkovich, "malkovich")

  // Make sure malkovich lives long enough.
  print(malkovich)
}

var Reflection = TestSuite("Reflection")

func wrap1   (_ x: Any) -> Any { return x }
func wrap2<T>(_ x: T)   -> Any { return wrap1(x) }
func wrap3   (_ x: Any) -> Any { return wrap2(x) }
func wrap4<T>(_ x: T)   -> Any { return wrap3(x) }
func wrap5   (_ x: Any) -> Any { return wrap4(x) }

class JustNeedAMetatype {}

Reflection.test("nested existential containers") {
  let wrapped = wrap5(JustNeedAMetatype.self)
  expectEqual("\(wrapped)", "JustNeedAMetatype")
}

Reflection.test("dumpToAStream") {
  var output = ""
  dump([ 42, 4242 ], to: &output)
  expectEqual("▿ 2 elements\n  - 42\n  - 4242\n", output)
}

struct StructWithDefaultMirror {
  let s: String

  init (_ s: String) {
    self.s = s
  }
}

Reflection.test("Struct/NonGeneric/DefaultMirror") {
  do {
    var output = ""
    dump(StructWithDefaultMirror("123"), to: &output)
    expectEqual("▿ a.StructWithDefaultMirror\n  - s: \"123\"\n", output)
  }

  do {
    // Build a String around an interpolation as a way of smoke-testing that
    // the internal _Mirror implementation gets memory management right.
    var output = ""
    dump(StructWithDefaultMirror("\(456)"), to: &output)
    expectEqual("▿ a.StructWithDefaultMirror\n  - s: \"456\"\n", output)
  }

  expectEqual(
    .`struct`,
    Mirror(reflecting: StructWithDefaultMirror("")).displayStyle)
}

struct GenericStructWithDefaultMirror<T, U> {
  let first: T
  let second: U
}

Reflection.test("Struct/Generic/DefaultMirror") {
  do {
    var value = GenericStructWithDefaultMirror<Int, [Any?]>(
      first: 123,
      second: [ "abc", 456, 789.25 ])
    var output = ""
    dump(value, to: &output)

    let expected =
      "▿ a.GenericStructWithDefaultMirror<Swift.Int, Swift.Array<Swift.Optional<protocol<>>>>\n" +
      "  - first: 123\n" +
      "  ▿ second: 3 elements\n" +
      "    ▿ Optional(\"abc\")\n" +
      "      - some: \"abc\"\n" +
      "    ▿ Optional(456)\n" +
      "      - some: 456\n" +
      "    ▿ Optional(789.25)\n" +
      "      - some: 789.25\n"

    expectEqual(expected, output)

  }
}

enum NoPayloadEnumWithDefaultMirror {
  case A, ß
}

Reflection.test("Enum/NoPayload/DefaultMirror") {
  do {
    let value: [NoPayloadEnumWithDefaultMirror] =
        [.A, .ß]
    var output = ""
    dump(value, to: &output)

    let expected =
      "▿ 2 elements\n" +
      "  - a.NoPayloadEnumWithDefaultMirror.A\n" +
      "  - a.NoPayloadEnumWithDefaultMirror.ß\n"

    expectEqual(expected, output)
  }
}

enum SingletonNonGenericEnumWithDefaultMirror {
  case OnlyOne(Int)
}

Reflection.test("Enum/SingletonNonGeneric/DefaultMirror") {
  do {
    let value = SingletonNonGenericEnumWithDefaultMirror.OnlyOne(5)
    var output = ""
    dump(value, to: &output)

    let expected =
      "▿ a.SingletonNonGenericEnumWithDefaultMirror.OnlyOne\n" +
      "  - OnlyOne: 5\n"

    expectEqual(expected, output)
  }
}

enum SingletonGenericEnumWithDefaultMirror<T> {
  case OnlyOne(T)
}

Reflection.test("Enum/SingletonGeneric/DefaultMirror") {
  do {
    let value = SingletonGenericEnumWithDefaultMirror.OnlyOne("IIfx")
    var output = ""
    dump(value, to: &output)

    let expected =
      "▿ a.SingletonGenericEnumWithDefaultMirror<Swift.String>.OnlyOne\n" +
      "  - OnlyOne: \"IIfx\"\n"

    expectEqual(expected, output)
  }
  expectEqual(0, LifetimeTracked.instances)
  do {
    let value = SingletonGenericEnumWithDefaultMirror.OnlyOne(
        LifetimeTracked(0))
    expectEqual(1, LifetimeTracked.instances)
    var output = ""
    dump(value, to: &output)
  }
  expectEqual(0, LifetimeTracked.instances)
}

enum SinglePayloadNonGenericEnumWithDefaultMirror {
  case Cat
  case Dog
  case Volleyball(String, Int)
}

Reflection.test("Enum/SinglePayloadNonGeneric/DefaultMirror") {
  do {
    let value: [SinglePayloadNonGenericEnumWithDefaultMirror] =
        [.Cat,
         .Dog,
         .Volleyball("Wilson", 2000)]
    var output = ""
    dump(value, to: &output)

    let expected =
      "▿ 3 elements\n" +
      "  - a.SinglePayloadNonGenericEnumWithDefaultMirror.Cat\n" +
      "  - a.SinglePayloadNonGenericEnumWithDefaultMirror.Dog\n" +
      "  ▿ a.SinglePayloadNonGenericEnumWithDefaultMirror.Volleyball\n" +
      "    ▿ Volleyball: (2 elements)\n" +
      "      - .0: \"Wilson\"\n" +
      "      - .1: 2000\n"

    expectEqual(expected, output)
  }
}

enum SinglePayloadGenericEnumWithDefaultMirror<T, U> {
  case Well
  case Faucet
  case Pipe(T, U)
}

Reflection.test("Enum/SinglePayloadGeneric/DefaultMirror") {
  do {
    let value: [SinglePayloadGenericEnumWithDefaultMirror<Int, [Int]>] =
        [.Well,
         .Faucet,
         .Pipe(408, [415])]
    var output = ""
    dump(value, to: &output)

    let expected =
      "▿ 3 elements\n" +
      "  - a.SinglePayloadGenericEnumWithDefaultMirror<Swift.Int, Swift.Array<Swift.Int>>.Well\n" +
      "  - a.SinglePayloadGenericEnumWithDefaultMirror<Swift.Int, Swift.Array<Swift.Int>>.Faucet\n" +
      "  ▿ a.SinglePayloadGenericEnumWithDefaultMirror<Swift.Int, Swift.Array<Swift.Int>>.Pipe\n" +
      "    ▿ Pipe: (2 elements)\n" +
      "      - .0: 408\n" +
      "      ▿ .1: 1 element\n" +
      "        - 415\n"

    expectEqual(expected, output)
  }
}

enum MultiPayloadTagBitsNonGenericEnumWithDefaultMirror {
  case Plus
  case SE30
  case Classic(mhz: Int)
  case Performa(model: Int)
}

Reflection.test("Enum/MultiPayloadTagBitsNonGeneric/DefaultMirror") {
  do {
    let value: [MultiPayloadTagBitsNonGenericEnumWithDefaultMirror] =
        [.Plus,
         .SE30,
         .Classic(mhz: 16),
         .Performa(model: 220)]
    var output = ""
    dump(value, to: &output)

    let expected =
      "▿ 4 elements\n" +
      "  - a.MultiPayloadTagBitsNonGenericEnumWithDefaultMirror.Plus\n" +
      "  - a.MultiPayloadTagBitsNonGenericEnumWithDefaultMirror.SE30\n" +
      "  ▿ a.MultiPayloadTagBitsNonGenericEnumWithDefaultMirror.Classic\n" +
      "    - Classic: 16\n" +
      "  ▿ a.MultiPayloadTagBitsNonGenericEnumWithDefaultMirror.Performa\n" +
      "    - Performa: 220\n"

    expectEqual(expected, output)
  }
}

class Floppy {
  let capacity: Int

  init(capacity: Int) { self.capacity = capacity }
}

class CDROM {
  let capacity: Int

  init(capacity: Int) { self.capacity = capacity }
}

enum MultiPayloadSpareBitsNonGenericEnumWithDefaultMirror {
  case MacWrite
  case MacPaint
  case FileMaker
  case ClarisWorks(floppy: Floppy)
  case HyperCard(cdrom: CDROM)
}

Reflection.test("Enum/MultiPayloadSpareBitsNonGeneric/DefaultMirror") {
  do {
    let value: [MultiPayloadSpareBitsNonGenericEnumWithDefaultMirror] =
        [.MacWrite,
         .MacPaint,
         .FileMaker,
         .ClarisWorks(floppy: Floppy(capacity: 800)),
         .HyperCard(cdrom: CDROM(capacity: 600))]

    var output = ""
    dump(value, to: &output)

    let expected =
      "▿ 5 elements\n" +
      "  - a.MultiPayloadSpareBitsNonGenericEnumWithDefaultMirror.MacWrite\n" +
      "  - a.MultiPayloadSpareBitsNonGenericEnumWithDefaultMirror.MacPaint\n" +
      "  - a.MultiPayloadSpareBitsNonGenericEnumWithDefaultMirror.FileMaker\n" +
      "  ▿ a.MultiPayloadSpareBitsNonGenericEnumWithDefaultMirror.ClarisWorks\n" +
      "    ▿ ClarisWorks: a.Floppy #0\n" +
      "      - capacity: 800\n" +
      "  ▿ a.MultiPayloadSpareBitsNonGenericEnumWithDefaultMirror.HyperCard\n" +
      "    ▿ HyperCard: a.CDROM #1\n" +
      "      - capacity: 600\n"

    expectEqual(expected, output)
  }
}

enum MultiPayloadTagBitsSmallNonGenericEnumWithDefaultMirror {
  case MacWrite
  case MacPaint
  case FileMaker
  case ClarisWorks(floppy: Bool)
  case HyperCard(cdrom: Bool)
}

Reflection.test("Enum/MultiPayloadTagBitsSmallNonGeneric/DefaultMirror") {
  do {
    let value: [MultiPayloadTagBitsSmallNonGenericEnumWithDefaultMirror] =
        [.MacWrite,
         .MacPaint,
         .FileMaker,
         .ClarisWorks(floppy: true),
         .HyperCard(cdrom: false)]

    var output = ""
    dump(value, to: &output)

    let expected =
      "▿ 5 elements\n" +
      "  - a.MultiPayloadTagBitsSmallNonGenericEnumWithDefaultMirror.MacWrite\n" +
      "  - a.MultiPayloadTagBitsSmallNonGenericEnumWithDefaultMirror.MacPaint\n" +
      "  - a.MultiPayloadTagBitsSmallNonGenericEnumWithDefaultMirror.FileMaker\n" +
      "  ▿ a.MultiPayloadTagBitsSmallNonGenericEnumWithDefaultMirror.ClarisWorks\n" +
      "    - ClarisWorks: true\n" +
      "  ▿ a.MultiPayloadTagBitsSmallNonGenericEnumWithDefaultMirror.HyperCard\n" +
      "    - HyperCard: false\n"

    expectEqual(expected, output)
  }
}

enum MultiPayloadGenericEnumWithDefaultMirror<T, U> {
  case IIe
  case IIgs
  case Centris(ram: T)
  case Quadra(hdd: U)
  case PowerBook170
  case PowerBookDuo220
}

Reflection.test("Enum/MultiPayloadGeneric/DefaultMirror") {
  do {
    let value: [MultiPayloadGenericEnumWithDefaultMirror<Int, String>] =
        [.IIe,
         .IIgs,
         .Centris(ram: 4096),
         .Quadra(hdd: "160MB"),
         .PowerBook170,
         .PowerBookDuo220]

    var output = ""
    dump(value, to: &output)

    let expected =
      "▿ 6 elements\n" +
      "  - a.MultiPayloadGenericEnumWithDefaultMirror<Swift.Int, Swift.String>.IIe\n" +
      "  - a.MultiPayloadGenericEnumWithDefaultMirror<Swift.Int, Swift.String>.IIgs\n" +
      "  ▿ a.MultiPayloadGenericEnumWithDefaultMirror<Swift.Int, Swift.String>.Centris\n" +
      "    - Centris: 4096\n" +
      "  ▿ a.MultiPayloadGenericEnumWithDefaultMirror<Swift.Int, Swift.String>.Quadra\n" +
      "    - Quadra: \"160MB\"\n" +
      "  - a.MultiPayloadGenericEnumWithDefaultMirror<Swift.Int, Swift.String>.PowerBook170\n" +
      "  - a.MultiPayloadGenericEnumWithDefaultMirror<Swift.Int, Swift.String>.PowerBookDuo220\n"

    expectEqual(expected, output)
  }
  expectEqual(0, LifetimeTracked.instances)
  do {
    let value = MultiPayloadGenericEnumWithDefaultMirror<LifetimeTracked,
                                                         LifetimeTracked>
        .Quadra(hdd: LifetimeTracked(0))
    expectEqual(1, LifetimeTracked.instances)
    var output = ""
    dump(value, to: &output)
  }
  expectEqual(0, LifetimeTracked.instances)
}

enum Foo<T> {
  indirect case Foo(Int)
  case Bar(T)
}

enum List<T> {
  case Nil
  indirect case Cons(first: T, rest: List<T>)
}

Reflection.test("Enum/IndirectGeneric/DefaultMirror") {
  let x = Foo<String>.Foo(22)
  let y = Foo<String>.Bar("twenty-two")

  expectEqual("\(x)", "Foo(22)")
  expectEqual("\(y)", "Bar(\"twenty-two\")")

  let list = List.Cons(first: 0, rest: .Cons(first: 1, rest: .Nil))
  expectEqual("\(list)",
              "Cons(0, a.List<Swift.Int>.Cons(1, a.List<Swift.Int>.Nil))")
}

class Brilliant : CustomReflectable {
  let first: Int
  let second: String

  init(_ fst: Int, _ snd: String) {
    self.first = fst
    self.second = snd
  }

  var customMirror: Mirror {
    return Mirror(self, children: ["first": first, "second": second, "self": self])
  }
}

/// Subclasses inherit their parents' custom mirrors.
class Irradiant : Brilliant {
  init() {
    super.init(400, "")
  }
}

Reflection.test("CustomMirror") {
  do {
    var output = ""
    dump(Brilliant(123, "four five six"), to: &output)

    let expected =
      "▿ a.Brilliant #0\n" +
      "  - first: 123\n" +
      "  - second: \"four five six\"\n" +
      "  ▿ self: a.Brilliant #0\n"

    expectEqual(expected, output)
  }

  do {
    var output = ""
    dump(Brilliant(123, "four five six"), to: &output, maxDepth: 0)
    expectEqual("▹ a.Brilliant #0\n", output)
  }

  do {
    var output = ""
    dump(Brilliant(123, "four five six"), to: &output, maxItems: 3)

    let expected =
      "▿ a.Brilliant #0\n" +
      "  - first: 123\n" +
      "  - second: \"four five six\"\n" +
      "    (1 more child)\n"

    expectEqual(expected, output)
  }

  do {
    var output = ""
    dump(Brilliant(123, "four five six"), to: &output, maxItems: 2)

    let expected =
      "▿ a.Brilliant #0\n" +
      "  - first: 123\n" +
      "    (2 more children)\n"

    expectEqual(expected, output)
  }

  do {
    var output = ""
    dump(Brilliant(123, "four five six"), to: &output, maxItems: 1)

    let expected =
      "▿ a.Brilliant #0\n" +
      "    (3 children)\n"

    expectEqual(expected, output)
  }

  do {
    // Check that object identifiers are unique to class instances.
    let a = Brilliant(1, "")
    let b = Brilliant(2, "")
    let c = Brilliant(3, "")

    // Equatable
    checkEquatable(true, ObjectIdentifier(a), ObjectIdentifier(a))
    checkEquatable(false, ObjectIdentifier(a), ObjectIdentifier(b))

    // Comparable
    func isComparable<X : Comparable>(_ x: X) {}
    isComparable(ObjectIdentifier(a))
    // Check the ObjectIdentifier created is stable
    expectTrue(
      (ObjectIdentifier(a) < ObjectIdentifier(b))
      != (ObjectIdentifier(a) > ObjectIdentifier(b)))
    expectFalse(
      ObjectIdentifier(a) >= ObjectIdentifier(b)
      && ObjectIdentifier(a) <= ObjectIdentifier(b))

    // Check that ordering is transitive.
    expectEqual(
      [ ObjectIdentifier(a), ObjectIdentifier(b), ObjectIdentifier(c) ].sorted(),
      [ ObjectIdentifier(c), ObjectIdentifier(b), ObjectIdentifier(a) ].sorted())
  }
}

Reflection.test("CustomMirrorIsInherited") {
  do {
    var output = ""
    dump(Irradiant(), to: &output)

    let expected =
      "▿ a.Brilliant #0\n" +
      "  - first: 400\n" +
      "  - second: \"\"\n" +
      "  ▿ self: a.Brilliant #0\n"

    expectEqual(expected, output)
  }
}

protocol SomeNativeProto {}
extension Int: SomeNativeProto {}

Reflection.test("MetatypeMirror") {
  do {
    var output = ""
    let concreteMetatype = Int.self
    dump(concreteMetatype, to: &output)

    let expectedInt = "- Swift.Int #0\n"
    expectEqual(expectedInt, output)

    let anyMetatype: Any.Type = Int.self
    output = ""
    dump(anyMetatype, to: &output)
    expectEqual(expectedInt, output)

    let nativeProtocolMetatype: SomeNativeProto.Type = Int.self
    output = ""
    dump(nativeProtocolMetatype, to: &output)
    expectEqual(expectedInt, output)

    let concreteClassMetatype = SomeClass.self
    let expectedSomeClass = "- a.SomeClass #0\n"
    output = ""
    dump(concreteClassMetatype, to: &output)
    expectEqual(expectedSomeClass, output)

    let nativeProtocolConcreteMetatype = SomeNativeProto.self
    let expectedNativeProtocolConcrete = "- a.SomeNativeProto #0\n"
    output = ""
    dump(nativeProtocolConcreteMetatype, to: &output)
    expectEqual(expectedNativeProtocolConcrete, output)
  }
}

Reflection.test("TupleMirror") {
  do {
    var output = ""
    let tuple =
      (Brilliant(384, "seven six eight"), StructWithDefaultMirror("nine"))
    dump(tuple, to: &output)

    let expected =
      "▿ (2 elements)\n" +
      "  ▿ .0: a.Brilliant #0\n" +
      "    - first: 384\n" +
      "    - second: \"seven six eight\"\n" +
      "    ▿ self: a.Brilliant #0\n" +
      "  ▿ .1: a.StructWithDefaultMirror\n" +
      "    - s: \"nine\"\n"

    expectEqual(expected, output)

    expectEqual(.tuple, Mirror(reflecting: tuple).displayStyle)
  }

  do {
    // A tuple of stdlib types with mirrors.
    var output = ""
    let tuple = (1, 2.5, false, "three")
    dump(tuple, to: &output)

    let expected =
      "▿ (4 elements)\n" +
      "  - .0: 1\n" +
      "  - .1: 2.5\n" +
      "  - .2: false\n" +
      "  - .3: \"three\"\n"

    expectEqual(expected, output)
  }

  do {
    // A nested tuple.
    var output = ""
    let tuple = (1, ("Hello", "World"))
    dump(tuple, to: &output)

    let expected =
      "▿ (2 elements)\n" +
      "  - .0: 1\n" +
      "  ▿ .1: (2 elements)\n" +
      "    - .0: \"Hello\"\n" +
      "    - .1: \"World\"\n"

    expectEqual(expected, output)
  }
}

class DullClass {}

Reflection.test("ClassReflection") {
  expectEqual(.`class`, Mirror(reflecting: DullClass()).displayStyle)
}

Reflection.test("String/Mirror") {
  do {
    var output = ""
    dump("", to: &output)

    let expected =
      "- \"\"\n"

    expectEqual(expected, output)
  }

  do {
    // U+0061 LATIN SMALL LETTER A
    // U+304B HIRAGANA LETTER KA
    // U+3099 COMBINING KATAKANA-HIRAGANA VOICED SOUND MARK
    // U+1F425 FRONT-FACING BABY CHICK
    var output = ""
    dump("\u{61}\u{304b}\u{3099}\u{1f425}", to: &output)

    let expected =
      "- \"\u{61}\u{304b}\u{3099}\u{1f425}\"\n"

    expectEqual(expected, output)
  }
}

Reflection.test("String.UTF8View/Mirror") {
  // U+0061 LATIN SMALL LETTER A
  // U+304B HIRAGANA LETTER KA
  // U+3099 COMBINING KATAKANA-HIRAGANA VOICED SOUND MARK
  var output = ""
  dump("\u{61}\u{304b}\u{3099}".utf8, to: &output)

  let expected =
    "▿ UTF8View(\"\u{61}\u{304b}\u{3099}\")\n" +
    "  - 97\n" +
    "  - 227\n" +
    "  - 129\n" +
    "  - 139\n" +
    "  - 227\n" +
    "  - 130\n" +
    "  - 153\n"

  expectEqual(expected, output)
}

Reflection.test("String.UTF16View/Mirror") {
  // U+0061 LATIN SMALL LETTER A
  // U+304B HIRAGANA LETTER KA
  // U+3099 COMBINING KATAKANA-HIRAGANA VOICED SOUND MARK
  // U+1F425 FRONT-FACING BABY CHICK
  var output = ""
  dump("\u{61}\u{304b}\u{3099}\u{1f425}".utf16, to: &output)

  let expected =
    "▿ StringUTF16(\"\u{61}\u{304b}\u{3099}\u{1f425}\")\n" +
    "  - 97\n" +
    "  - 12363\n" +
    "  - 12441\n" +
    "  - 55357\n" +
    "  - 56357\n"

  expectEqual(expected, output)
}

Reflection.test("String.UnicodeScalarView/Mirror") {
  // U+0061 LATIN SMALL LETTER A
  // U+304B HIRAGANA LETTER KA
  // U+3099 COMBINING KATAKANA-HIRAGANA VOICED SOUND MARK
  // U+1F425 FRONT-FACING BABY CHICK
  var output = ""
  dump("\u{61}\u{304b}\u{3099}\u{1f425}".unicodeScalars, to: &output)

  let expected =
    "▿ StringUnicodeScalarView(\"\u{61}\u{304b}\u{3099}\u{1f425}\")\n" +
    "  - \"\u{61}\"\n" +
    "  - \"\\u{304B}\"\n" +
    "  - \"\\u{3099}\"\n" +
    "  - \"\\u{0001F425}\"\n"

  expectEqual(expected, output)
}

Reflection.test("Character/Mirror") {
  do {
    // U+0061 LATIN SMALL LETTER A
    let input: Character = "\u{61}"
    var output = ""
    dump(input, to: &output)

    let expected =
      "- \"\u{61}\"\n"

    expectEqual(expected, output)
  }

  do {
    // U+304B HIRAGANA LETTER KA
    // U+3099 COMBINING KATAKANA-HIRAGANA VOICED SOUND MARK
    let input: Character = "\u{304b}\u{3099}"
    var output = ""
    dump(input, to: &output)

    let expected =
      "- \"\u{304b}\u{3099}\"\n"

    expectEqual(expected, output)
  }

  do {
    // U+1F425 FRONT-FACING BABY CHICK
    let input: Character = "\u{1f425}"
    var output = ""
    dump(input, to: &output)

    let expected =
      "- \"\u{1f425}\"\n"

    expectEqual(expected, output)
  }
}

Reflection.test("UnicodeScalar") {
  do {
    // U+0061 LATIN SMALL LETTER A
    let input: UnicodeScalar = "\u{61}"
    var output = ""
    dump(input, to: &output)

    let expected =
      "- \"\u{61}\"\n"

    expectEqual(expected, output)
  }

  do {
    // U+304B HIRAGANA LETTER KA
    let input: UnicodeScalar = "\u{304b}"
    var output = ""
    dump(input, to: &output)

    let expected =
      "- \"\\u{304B}\"\n"

    expectEqual(expected, output)
  }

  do {
    // U+3099 COMBINING KATAKANA-HIRAGANA VOICED SOUND MARK
    let input: UnicodeScalar = "\u{3099}"
    var output = ""
    dump(input, to: &output)

    let expected =
      "- \"\\u{3099}\"\n"

    expectEqual(expected, output)
  }

  do {
    // U+1F425 FRONT-FACING BABY CHICK
    let input: UnicodeScalar = "\u{1f425}"
    var output = ""
    dump(input, to: &output)

    let expected =
      "- \"\\u{0001F425}\"\n"

    expectEqual(expected, output)
  }
}

Reflection.test("Bool") {
  do {
    var output = ""
    dump(false, to: &output)

    let expected =
      "- false\n"

    expectEqual(expected, output)
  }

  do {
    var output = ""
    dump(true, to: &output)

    let expected =
      "- true\n"

    expectEqual(expected, output)
  }
}

// FIXME: these tests should cover Float80.
// FIXME: these tests should be automatically generated from the list of
// available floating point types.
Reflection.test("Float") {
  do {
    var output = ""
    dump(Float.nan, to: &output)

    let expected =
      "- nan\n"

    expectEqual(expected, output)
  }

  do {
    var output = ""
    dump(Float.infinity, to: &output)

    let expected =
      "- inf\n"

    expectEqual(expected, output)
  }

  do {
    var input: Float = 42.125
    var output = ""
    dump(input, to: &output)

    let expected =
      "- 42.125\n"

    expectEqual(expected, output)
  }
}

Reflection.test("Double") {
  do {
    var output = ""
    dump(Double.nan, to: &output)

    let expected =
      "- nan\n"

    expectEqual(expected, output)
  }

  do {
    var output = ""
    dump(Double.infinity, to: &output)

    let expected =
      "- inf\n"

    expectEqual(expected, output)
  }

  do {
    var input: Double = 42.125
    var output = ""
    dump(input, to: &output)

    let expected =
      "- 42.125\n"

    expectEqual(expected, output)
  }
}

// A struct type and class type whose NominalTypeDescriptor.FieldNames 
// data is exactly eight bytes long. FieldNames data of exactly 
// 4 or 8 or 16 bytes was once miscompiled on arm64.
struct EightByteFieldNamesStruct {
  let abcdef = 42
}
class EightByteFieldNamesClass {
  let abcdef = 42
}

Reflection.test("FieldNamesBug") {
  do {
    let expected =
      "▿ a.EightByteFieldNamesStruct\n" +
      "  - abcdef: 42\n"
    var output = ""
    dump(EightByteFieldNamesStruct(), to: &output)
    expectEqual(expected, output)
  }

  do {
    let expected =
      "▿ a.EightByteFieldNamesClass #0\n" +
      "  - abcdef: 42\n"
    var output = ""
    dump(EightByteFieldNamesClass(), to: &output)
    expectEqual(expected, output)
  }
}

Reflection.test("MirrorMirror") {
  var object = 1
  var mirror = Mirror(reflecting: object)
  var mirrorMirror = Mirror(reflecting: mirror)

  expectEqual(0, mirrorMirror.children.count)
}

Reflection.test("OpaquePointer/null") {
  // Don't crash on null pointers. rdar://problem/19708338
  var sequence: OpaquePointer = nil
  var mirror = Mirror(reflecting: sequence)
  var child = mirror.children.first!
  expectEqual("(Opaque Value)", "\(child.1)")
}

Reflection.test("StaticString/Mirror") {
  do {
    var output = ""
    dump("" as StaticString, to: &output)

    let expected =
      "- \"\"\n"

    expectEqual(expected, output)
  }

  do {
    // U+0061 LATIN SMALL LETTER A
    // U+304B HIRAGANA LETTER KA
    // U+3099 COMBINING KATAKANA-HIRAGANA VOICED SOUND MARK
    // U+1F425 FRONT-FACING BABY CHICK
    var output = ""
    dump("\u{61}\u{304b}\u{3099}\u{1f425}" as StaticString, to: &output)

    let expected =
      "- \"\u{61}\u{304b}\u{3099}\u{1f425}\"\n"

    expectEqual(expected, output)
  }
}

Reflection.test("DictionaryIterator/Mirror") {
  let d: [MinimalHashableValue : OpaqueValue<Int>] =
    [ MinimalHashableValue(0) : OpaqueValue(0) ]

  var output = ""
  dump(d.makeIterator(), to: &output)

  let expected =
    "- Swift.DictionaryIterator<StdlibUnittest.MinimalHashableValue, StdlibUnittest.OpaqueValue<Swift.Int>>\n"

  expectEqual(expected, output)
}

Reflection.test("SetIterator/Mirror") {
  let s: Set<MinimalHashableValue> = [ MinimalHashableValue(0)]

  var output = ""
  dump(s.makeIterator(), to: &output)

  let expected =
    "- Swift.SetIterator<StdlibUnittest.MinimalHashableValue>\n"

  expectEqual(expected, output)
}

var BitTwiddlingTestSuite = TestSuite("BitTwiddling")

func computeCountLeadingZeroes(_ x: Int64) -> Int64 {
  var x = x
  var r: Int64 = 64
  while x != 0 {
    x >>= 1
    r -= 1
  }
  return r
}

BitTwiddlingTestSuite.test("_pointerSize") {
#if arch(i386) || arch(arm)
  expectEqual(4, sizeof(Optional<AnyObject>.self))
#elseif arch(x86_64) || arch(arm64) || arch(powerpc64) || arch(powerpc64le)
  expectEqual(8, sizeof(Optional<AnyObject>.self))
#else
  fatalError("implement")
#endif
}

BitTwiddlingTestSuite.test("_countLeadingZeros") {
  for i in Int64(0)..<1000 {
    expectEqual(computeCountLeadingZeroes(i), _countLeadingZeros(i))
  }
  expectEqual(0, _countLeadingZeros(Int64.min))
}

BitTwiddlingTestSuite.test("_isPowerOf2/Int") {
  func asInt(_ a: Int) -> Int { return a }

  expectFalse(_isPowerOf2(asInt(-1025)))
  expectFalse(_isPowerOf2(asInt(-1024)))
  expectFalse(_isPowerOf2(asInt(-1023)))
  expectFalse(_isPowerOf2(asInt(-4)))
  expectFalse(_isPowerOf2(asInt(-3)))
  expectFalse(_isPowerOf2(asInt(-2)))
  expectFalse(_isPowerOf2(asInt(-1)))
  expectFalse(_isPowerOf2(asInt(0)))
  expectTrue(_isPowerOf2(asInt(1)))
  expectTrue(_isPowerOf2(asInt(2)))
  expectFalse(_isPowerOf2(asInt(3)))
  expectTrue(_isPowerOf2(asInt(1024)))
#if arch(i386) || arch(arm)
  // Not applicable to 32-bit architectures.
#elseif arch(x86_64) || arch(arm64) || arch(powerpc64) || arch(powerpc64le)
  expectTrue(_isPowerOf2(asInt(0x8000_0000)))
#else
  fatalError("implement")
#endif
  expectFalse(_isPowerOf2(Int.min))
  expectFalse(_isPowerOf2(Int.max))
}

BitTwiddlingTestSuite.test("_isPowerOf2/UInt") {
  func asUInt(_ a: UInt) -> UInt { return a }

  expectFalse(_isPowerOf2(asUInt(0)))
  expectTrue(_isPowerOf2(asUInt(1)))
  expectTrue(_isPowerOf2(asUInt(2)))
  expectFalse(_isPowerOf2(asUInt(3)))
  expectTrue(_isPowerOf2(asUInt(1024)))
  expectTrue(_isPowerOf2(asUInt(0x8000_0000)))
  expectFalse(_isPowerOf2(UInt.max))
}

BitTwiddlingTestSuite.test("_floorLog2") {
  expectEqual(_floorLog2(1), 0)
  expectEqual(_floorLog2(8), 3)
  expectEqual(_floorLog2(15), 3)
  expectEqual(_floorLog2(Int64.max), 62) // 63 minus 1 for sign bit.
}

var AvailabilityVersionsTestSuite = TestSuite("AvailabilityVersions")

AvailabilityVersionsTestSuite.test("lexicographic_compare") {
  func version(
    _ major: Int,
    _ minor: Int,
    _ patch: Int
  ) -> _SwiftNSOperatingSystemVersion {
    return _SwiftNSOperatingSystemVersion(
      majorVersion: major,
      minorVersion: minor,
      patchVersion: patch
    )
  }

  checkComparable(.eq, version(0, 0, 0), version(0, 0, 0))

  checkComparable(.lt, version(0, 0, 0), version(0, 0, 1))
  checkComparable(.lt, version(0, 0, 0), version(0, 1, 0))
  checkComparable(.lt, version(0, 0, 0), version(1, 0, 0))

  checkComparable(.lt,version(10, 9, 0), version(10, 10, 0))
  checkComparable(.lt,version(10, 9, 11), version(10, 10, 0))
  checkComparable(.lt,version(10, 10, 3), version(10, 11, 0))

  checkComparable(.lt, version(8, 3, 0), version(9, 0, 0))

  checkComparable(.lt, version(0, 11, 0), version(10, 10, 4))
  checkComparable(.lt, version(0, 10, 0), version(10, 10, 4))
  checkComparable(.lt, version(3, 2, 1), version(4, 3, 2))
  checkComparable(.lt, version(1, 2, 3), version(2, 3, 1))

  checkComparable(.eq, version(10, 11, 12), version(10, 11, 12))

  checkEquatable(true, version(1, 2, 3), version(1, 2, 3))
  checkEquatable(false, version(1, 2, 3), version(1, 2, 42))
  checkEquatable(false, version(1, 2, 3), version(1, 42, 3))
  checkEquatable(false, version(1, 2, 3), version(42, 2, 3))
}

AvailabilityVersionsTestSuite.test("_stdlib_isOSVersionAtLeast") {
  func isAtLeastOS(_ major: Int, _ minor: Int, _ patch: Int) -> Bool {
    return _getBool(_stdlib_isOSVersionAtLeast(major._builtinWordValue,
                                               minor._builtinWordValue,
                                               patch._builtinWordValue))
  }

// _stdlib_isOSVersionAtLeast is broken for
// watchOS. rdar://problem/20234735
#if os(OSX) || os(iOS) || os(tvOS) || os(watchOS)
  // This test assumes that no version component on an OS we test upon
  // will ever be greater than 1066 and that every major version will always
  // be greater than 1.
  expectFalse(isAtLeastOS(1066, 0, 0))
  expectTrue(isAtLeastOS(0, 1066, 0))
  expectTrue(isAtLeastOS(0, 0, 1066))
#endif
}

runAllTests()

