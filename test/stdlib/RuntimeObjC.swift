// RUN: %empty-directory(%t)
//
// RUN: %target-clang %S/Inputs/Mirror/Mirror.mm -c -o %t/Mirror.mm.o -g
// RUN: %target-build-swift -parse-stdlib -Xfrontend -disable-access-control -module-name a -I %S/Inputs/Mirror/ -Xlinker %t/Mirror.mm.o %s -o %t.out
// RUN: %target-codesign %t.out
// RUN: %target-run %t.out
// REQUIRES: executable_test
// REQUIRES: objc_interop

import Swift
import StdlibUnittest


import Foundation
import CoreGraphics
import SwiftShims
import MirrorObjC

var nsObjectCanaryCount = 0
@objc class NSObjectCanary : NSObject {
  override init() {
    nsObjectCanaryCount += 1
  }
  deinit {
    nsObjectCanaryCount -= 1
  }
}

struct NSObjectCanaryStruct {
  var ref = NSObjectCanary()
}

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

@objc class ClassA {
  init(value: Int) {
    self.value = value
  }

  var value: Int
}

struct BridgedValueType : _ObjectiveCBridgeable {
  init(value: Int) {
    self.value = value
  }

  func _bridgeToObjectiveC() -> ClassA {
    return ClassA(value: value)
  }

  static func _forceBridgeFromObjectiveC(
    _ x: ClassA,
    result: inout BridgedValueType?
  ) {
    assert(x.value % 2 == 0, "not bridged to Objective-C")
    result = BridgedValueType(value: x.value)
  }

  static func _conditionallyBridgeFromObjectiveC(
    _ x: ClassA,
    result: inout BridgedValueType?
  ) -> Bool {
    if x.value % 2 == 0 {
      result = BridgedValueType(value: x.value)
      return true
    }

    result = nil
    return false
  }

  static func _unconditionallyBridgeFromObjectiveC(_ source: ClassA?)
      -> BridgedValueType {
    var result: BridgedValueType?
    _forceBridgeFromObjectiveC(source!, result: &result)
    return result!
  }

  var value: Int
  var canaryRef = SwiftObjectCanary()
}

struct BridgedLargeValueType : _ObjectiveCBridgeable {
  init(value: Int) {
    value0 = value
    value1 = value
    value2 = value
    value3 = value
    value4 = value
    value5 = value
    value6 = value
    value7 = value
  }

  func _bridgeToObjectiveC() -> ClassA {
    assert(value == value0)
    return ClassA(value: value0)
  }

  static func _forceBridgeFromObjectiveC(
    _ x: ClassA,
    result: inout BridgedLargeValueType?
  ) {
    assert(x.value % 2 == 0, "not bridged to Objective-C")
    result = BridgedLargeValueType(value: x.value)
  }

  static func _conditionallyBridgeFromObjectiveC(
    _ x: ClassA,
    result: inout BridgedLargeValueType?
  ) -> Bool {
    if x.value % 2 == 0 {
      result = BridgedLargeValueType(value: x.value)
      return true
    }

    result = nil
    return false
  }

  static func _unconditionallyBridgeFromObjectiveC(_ source: ClassA?)
      -> BridgedLargeValueType {
    var result: BridgedLargeValueType?
    _forceBridgeFromObjectiveC(source!, result: &result)
    return result!
  }

  var value: Int {
    let x = value0
    assert(value0 == x && value1 == x && value2 == x && value3 == x &&
           value4 == x && value5 == x && value6 == x && value7 == x)
    return x
  }

  var (value0, value1, value2, value3): (Int, Int, Int, Int)
  var (value4, value5, value6, value7): (Int, Int, Int, Int)
  var canaryRef = SwiftObjectCanary()
}

class BridgedVerbatimRefType {
  var value: Int = 42
  var canaryRef = SwiftObjectCanary()
}

func withSwiftObjectCanary<T>(
  _ createValue: () -> T,
  _ check: (T) -> Void,
  file: String = #file, line: UInt = #line
) {
  let stackTrace = SourceLocStack(SourceLoc(file, line))

  swiftObjectCanaryCount = 0
  autoreleasepool {
    let valueWithCanary = createValue()
    expectEqual(1, swiftObjectCanaryCount, stackTrace: stackTrace)
    check(valueWithCanary)
  }
  expectEqual(0, swiftObjectCanaryCount, stackTrace: stackTrace)
}

var Runtime = TestSuite("Runtime")

func _isClassOrObjCExistential_Opaque<T>(_ x: T.Type) -> Bool {
  return _isClassOrObjCExistential(_opaqueIdentity(x))
}

Runtime.test("_isClassOrObjCExistential") {
  expectTrue(_isClassOrObjCExistential(NSObjectCanary.self))
  expectTrue(_isClassOrObjCExistential_Opaque(NSObjectCanary.self))

  expectFalse(_isClassOrObjCExistential(NSObjectCanaryStruct.self))
  expectFalse(_isClassOrObjCExistential_Opaque(NSObjectCanaryStruct.self))

  expectTrue(_isClassOrObjCExistential(SwiftObjectCanary.self))
  expectTrue(_isClassOrObjCExistential_Opaque(SwiftObjectCanary.self))

  expectFalse(_isClassOrObjCExistential(SwiftObjectCanaryStruct.self))
  expectFalse(_isClassOrObjCExistential_Opaque(SwiftObjectCanaryStruct.self))

  typealias SwiftClosure = () -> ()
  expectFalse(_isClassOrObjCExistential(SwiftClosure.self))
  expectFalse(_isClassOrObjCExistential_Opaque(SwiftClosure.self))

  typealias ObjCClosure = @convention(block) () -> ()
  expectTrue(_isClassOrObjCExistential(ObjCClosure.self))
  expectTrue(_isClassOrObjCExistential_Opaque(ObjCClosure.self))

  expectTrue(_isClassOrObjCExistential(CFArray.self))
  expectTrue(_isClassOrObjCExistential_Opaque(CFArray.self))

  expectTrue(_isClassOrObjCExistential(CFArray.self))
  expectTrue(_isClassOrObjCExistential_Opaque(CFArray.self))

  expectTrue(_isClassOrObjCExistential(AnyObject.self))
  expectTrue(_isClassOrObjCExistential_Opaque(AnyObject.self))

  // AnyClass == AnyObject.Type
  expectFalse(_isClassOrObjCExistential(AnyClass.self))
  expectFalse(_isClassOrObjCExistential_Opaque(AnyClass.self))

  expectFalse(_isClassOrObjCExistential(AnyObject.Protocol.self))
  expectFalse(_isClassOrObjCExistential_Opaque(AnyObject.Protocol.self))

  expectFalse(_isClassOrObjCExistential(NSObjectCanary.Type.self))
  expectFalse(_isClassOrObjCExistential_Opaque(NSObjectCanary.Type.self))
}

Runtime.test("_canBeClass") {
  expectEqual(1, _canBeClass(NSObjectCanary.self))
  expectEqual(0, _canBeClass(NSObjectCanaryStruct.self))

  typealias ObjCClosure = @convention(block) () -> ()
  expectEqual(1, _canBeClass(ObjCClosure.self))

  expectEqual(1, _canBeClass(CFArray.self))
}

Runtime.test("bridgeToObjectiveC") {
  expectEqual(42, (_bridgeAnythingToObjectiveC(BridgedValueType(value: 42)) as! ClassA).value)

  expectEqual(42, (_bridgeAnythingToObjectiveC(BridgedLargeValueType(value: 42)) as! ClassA).value)

  let bridgedVerbatimRef = BridgedVerbatimRefType()
  expectTrue(_bridgeAnythingToObjectiveC(bridgedVerbatimRef) === bridgedVerbatimRef)
}

Runtime.test("bridgeToObjectiveC/NoLeak") {
  withSwiftObjectCanary(
    { BridgedValueType(value: 42) },
    { expectEqual(42, (_bridgeAnythingToObjectiveC($0) as! ClassA).value) })

  withSwiftObjectCanary(
    { BridgedLargeValueType(value: 42) },
    { expectEqual(42, (_bridgeAnythingToObjectiveC($0) as! ClassA).value) })

  withSwiftObjectCanary(
    { BridgedVerbatimRefType() },
    { expectTrue(_bridgeAnythingToObjectiveC($0) === $0) })
}

Runtime.test("forceBridgeFromObjectiveC") {

  // Bridge back using BridgedValueType.
  expectNil(_conditionallyBridgeFromObjectiveC(
      ClassA(value: 21), BridgedValueType.self))

  expectEqual(42, _forceBridgeFromObjectiveC(
      ClassA(value: 42), BridgedValueType.self).value)
  expectEqual(42, _conditionallyBridgeFromObjectiveC(
      ClassA(value: 42), BridgedValueType.self)!.value)

  expectNil(_conditionallyBridgeFromObjectiveC(
      BridgedVerbatimRefType(), BridgedValueType.self))

  // Bridge back using BridgedLargeValueType.
  expectNil(_conditionallyBridgeFromObjectiveC(
      ClassA(value: 21), BridgedLargeValueType.self))

  expectEqual(42, _forceBridgeFromObjectiveC(
      ClassA(value: 42), BridgedLargeValueType.self).value)
  expectEqual(42, _conditionallyBridgeFromObjectiveC(
      ClassA(value: 42), BridgedLargeValueType.self)!.value)

  expectNil(_conditionallyBridgeFromObjectiveC(
      BridgedVerbatimRefType(), BridgedLargeValueType.self))

  // Bridge back using BridgedVerbatimRefType.
  expectNil(_conditionallyBridgeFromObjectiveC(
      ClassA(value: 21), BridgedVerbatimRefType.self))

  expectNil(_conditionallyBridgeFromObjectiveC(
      ClassA(value: 42), BridgedVerbatimRefType.self))

  let bridgedVerbatimRef = BridgedVerbatimRefType()
  expectTrue(_forceBridgeFromObjectiveC(
      bridgedVerbatimRef, BridgedVerbatimRefType.self) === bridgedVerbatimRef)
  expectTrue(_conditionallyBridgeFromObjectiveC(
      bridgedVerbatimRef, BridgedVerbatimRefType.self)! === bridgedVerbatimRef)
}


Runtime.test("isBridgedToObjectiveC") {
  expectTrue(_isBridgedToObjectiveC(BridgedValueType.self))
  expectTrue(_isBridgedToObjectiveC(BridgedVerbatimRefType.self))
}

Runtime.test("isBridgedVerbatimToObjectiveC") {
  expectFalse(_isBridgedVerbatimToObjectiveC(BridgedValueType.self))
  expectTrue(_isBridgedVerbatimToObjectiveC(BridgedVerbatimRefType.self))
}

//===----------------------------------------------------------------------===//

class SomeClass {}
@objc class SomeObjCClass {}
class SomeNSObjectSubclass : NSObject {}

Runtime.test("typeName") {
  expectEqual("a.SomeObjCClass", _typeName(SomeObjCClass.self))
  expectEqual("a.SomeNSObjectSubclass", _typeName(SomeNSObjectSubclass.self))
  expectEqual("NSObject", _typeName(NSObject.self))

  var a : Any = SomeObjCClass()
  expectEqual("a.SomeObjCClass", _typeName(type(of: a)))
  
  a = SomeNSObjectSubclass()
  expectEqual("a.SomeNSObjectSubclass", _typeName(type(of: a)))

  a = NSObject()
  expectEqual("NSObject", _typeName(type(of: a)))
}

class GenericClass<T> {}
class MultiGenericClass<T, U> {}
struct GenericStruct<T> {}
enum GenericEnum<T> {}

struct PlainStruct {}
enum PlainEnum {}

protocol ProtocolA {}
protocol ProtocolB {}

class OuterClass {
    
    private class PrivateGeneric<T, U> {
      class InnerGeneric<X> {
        class Inner { }
      }
    }
    
    static func getPrivateGenericName() -> String {
      return NSStringFromClass(OuterClass.PrivateGeneric<Int, Bool>.self)
    }
    static func getInnerGenericName() -> String {
      return NSStringFromClass(OuterClass.PrivateGeneric<Int, Bool>.InnerGeneric<Float>.self)
    }
    static func getInnerName() -> String {
      return NSStringFromClass(OuterClass.PrivateGeneric<Int, Bool>.InnerGeneric<Float>.Inner.self)
    }
}

// The private discriminator is not deterministic.
// Replace it with a constant string.
func removePrivateDiscriminator(_ symbol: String) -> String {
  let regexp = try! NSRegularExpression(pattern: "P[0-9]+\\$[0-9a-f]+")
  let range = NSRange(0..<symbol.count)
  return regexp.stringByReplacingMatches(in: symbol, range: range, withTemplate: "XXX")
}

Runtime.test("Generic class ObjC runtime names") {
  expectEqual("_TtGC1a12GenericClassSi_",
              NSStringFromClass(GenericClass<Int>.self))
  expectEqual("_TtGC1a12GenericClassVS_11PlainStruct_",
              NSStringFromClass(GenericClass<PlainStruct>.self))
  expectEqual("_TtGC1a12GenericClassOS_9PlainEnum_",
              NSStringFromClass(GenericClass<PlainEnum>.self))
  expectEqual("_TtGC1a12GenericClassTVS_11PlainStructOS_9PlainEnumS1___",
              NSStringFromClass(GenericClass<(PlainStruct, PlainEnum, PlainStruct)>.self))
  expectEqual("_TtGC1a12GenericClassMVS_11PlainStruct_",
              NSStringFromClass(GenericClass<PlainStruct.Type>.self))
  expectEqual("_TtGC1a12GenericClassFMVS_11PlainStructS1__",
              NSStringFromClass(GenericClass<(PlainStruct.Type) -> PlainStruct>.self))

  expectEqual("_TtGC1a12GenericClassFzMVS_11PlainStructS1__",
              NSStringFromClass(GenericClass<(PlainStruct.Type) throws -> PlainStruct>.self))
  expectEqual("_TtGC1a12GenericClassFTVS_11PlainStructROS_9PlainEnum_Si_",
              NSStringFromClass(GenericClass<(PlainStruct, inout PlainEnum) -> Int>.self))

  expectEqual("_TtGC1a12GenericClassPS_9ProtocolA__",
              NSStringFromClass(GenericClass<ProtocolA>.self))
  expectEqual("_TtGC1a12GenericClassPS_9ProtocolAS_9ProtocolB__",
              NSStringFromClass(GenericClass<ProtocolA & ProtocolB>.self))
  expectEqual("_TtGC1a12GenericClassPMPS_9ProtocolAS_9ProtocolB__",
              NSStringFromClass(GenericClass<(ProtocolA & ProtocolB).Type>.self))
  expectEqual("_TtGC1a12GenericClassMPS_9ProtocolAS_9ProtocolB__",
              NSStringFromClass(GenericClass<(ProtocolB & ProtocolA).Protocol>.self))

  expectEqual("_TtGC1a12GenericClassaSo10CFArrayRef_",
              NSStringFromClass(GenericClass<CFArray>.self))
  expectEqual("_TtGC1a12GenericClassaSo9NSDecimal_",
              NSStringFromClass(GenericClass<Decimal>.self))
  expectEqual("_TtGC1a12GenericClassCSo8NSObject_",
              NSStringFromClass(GenericClass<NSObject>.self))
  expectEqual("_TtGC1a12GenericClassCSo8NSObject_",
              NSStringFromClass(GenericClass<NSObject>.self))
  expectEqual("_TtGC1a12GenericClassPSo9NSCopying__",
              NSStringFromClass(GenericClass<NSCopying>.self))
  expectEqual("_TtGC1a12GenericClassPSo9NSCopyingS_9ProtocolAS_9ProtocolB__",
              NSStringFromClass(GenericClass<ProtocolB & NSCopying & ProtocolA>.self))

  expectEqual("_TtGC1a12GenericClassXcCS_9SomeClassS_9ProtocolA__",
              NSStringFromClass(GenericClass<ProtocolA & SomeClass>.self))
  expectEqual("_TtGC1a12GenericClassPS_9ProtocolAs9AnyObject__",
              NSStringFromClass(GenericClass<ProtocolA & AnyObject>.self))
  expectEqual("_TtGC1a12GenericClassPs9AnyObject__",
              NSStringFromClass(GenericClass<AnyObject>.self))

  expectEqual("_TtGC1a17MultiGenericClassGVS_13GenericStructSi_GOS_11GenericEnumGS2_Si___",
              NSStringFromClass(MultiGenericClass<GenericStruct<Int>,
                                                  GenericEnum<GenericEnum<Int>>>.self))
  
  expectEqual("_TtGCC1a10OuterClassXXXPrivateGeneric_SiSb_",
              removePrivateDiscriminator(OuterClass.getPrivateGenericName()))
  expectEqual("_TtGCCC1a10OuterClassXXXPrivateGeneric12InnerGeneric_SiSb_Sf_",
              removePrivateDiscriminator(OuterClass.getInnerGenericName()))
  expectEqual("_TtGCCCC1a10OuterClassXXXPrivateGeneric12InnerGeneric5Inner_SiSb_Sf__",
              removePrivateDiscriminator(OuterClass.getInnerName()))
}

@objc protocol P {}
struct AnyObjStruct<T: AnyObject> {}

Runtime.test("typeByName") {
  // Make sure we don't crash if we have foreign classes in the
  // table -- those don't have NominalTypeDescriptors
  print(CFArray.self)
  expectTrue(_typeByName("a.SomeClass") == SomeClass.self)
  expectTrue(_typeByName("DoesNotExist") == nil)
  expectTrue(_typeByName("1a12AnyObjStructVyAA1P_pG") == AnyObjStruct<P>.self)
}

Runtime.test("casting AnyObject to class metatypes") {
  do {
    let ao: AnyObject = SomeClass.self
    expectTrue(ao as? Any.Type == SomeClass.self)
    expectTrue(ao as? AnyClass == SomeClass.self)
    expectTrue(ao as? SomeClass.Type == SomeClass.self)
  }
  
  do {
    var ao : AnyObject = SomeNSObjectSubclass()
    expectTrue(ao as? Any.Type == nil)
    expectTrue(ao as? AnyClass == nil)

    ao = SomeNSObjectSubclass.self
    expectTrue(ao as? Any.Type == SomeNSObjectSubclass.self)
    expectTrue(ao as? AnyClass == SomeNSObjectSubclass.self)
    expectTrue(ao as? SomeNSObjectSubclass.Type == SomeNSObjectSubclass.self)
  }

  do {
    let a : Any = SomeNSObjectSubclass()
    expectTrue(a as? Any.Type == nil)
    expectTrue(a as? AnyClass == nil)
  }

  do {
    var nso: NSObject = SomeNSObjectSubclass()
    expectTrue(nso as? AnyClass == nil)
    
    nso = (SomeNSObjectSubclass.self as AnyObject) as! NSObject
    expectTrue(nso as? Any.Type == SomeNSObjectSubclass.self)
    expectTrue(nso as? AnyClass == SomeNSObjectSubclass.self)
    expectTrue(nso as? SomeNSObjectSubclass.Type == SomeNSObjectSubclass.self)
  }
}

var RuntimeFoundationWrappers = TestSuite("RuntimeFoundationWrappers")

RuntimeFoundationWrappers.test("_stdlib_NSObject_isEqual/NoLeak") {
  nsObjectCanaryCount = 0
  autoreleasepool {
    let a = NSObjectCanary()
    let b = NSObjectCanary()
    expectEqual(2, nsObjectCanaryCount)
    _ = _stdlib_NSObject_isEqual(a, b)
  }
  expectEqual(0, nsObjectCanaryCount)
}

var nsStringCanaryCount = 0
@objc class NSStringCanary : NSString {
  override init() {
    nsStringCanaryCount += 1
    super.init()
  }
  required init(coder: NSCoder) {
    fatalError("don't call this initializer")
  }
  required init(itemProviderData data: Data, typeIdentifier: String) throws {
    fatalError("don't call this initializer")    
  }
  deinit {
    nsStringCanaryCount -= 1
  }
  @objc override var length: Int {
    return 0
  }
  @objc override func character(at index: Int) -> unichar {
    fatalError("out-of-bounds access")
  }
}

RuntimeFoundationWrappers.test("_stdlib_CFStringCreateCopy/NoLeak") {
  nsStringCanaryCount = 0
  autoreleasepool {
    let a = NSStringCanary()
    expectEqual(1, nsStringCanaryCount)
    _ = _stdlib_binary_CFStringCreateCopy(a)
  }
  expectEqual(0, nsStringCanaryCount)
}

RuntimeFoundationWrappers.test("_stdlib_CFStringGetLength/NoLeak") {
  nsStringCanaryCount = 0
  autoreleasepool {
    let a = NSStringCanary()
    expectEqual(1, nsStringCanaryCount)
    _ = _stdlib_binary_CFStringGetLength(a)
  }
  expectEqual(0, nsStringCanaryCount)
}

RuntimeFoundationWrappers.test("_stdlib_CFStringGetCharactersPtr/NoLeak") {
  nsStringCanaryCount = 0
  autoreleasepool {
    let a = NSStringCanary()
    expectEqual(1, nsStringCanaryCount)
    _ = _stdlib_binary_CFStringGetCharactersPtr(a)
  }
  expectEqual(0, nsStringCanaryCount)
}

RuntimeFoundationWrappers.test("bridgedNSArray") {
  var c = [NSObject]()
  autoreleasepool {
    let a = [NSObject]()
    let b = a as NSArray
    c = b as! [NSObject]
  }
  c.append(NSObject())
  // expect no crash.
}

var Reflection = TestSuite("Reflection")

class SwiftFooMoreDerivedObjCClass : FooMoreDerivedObjCClass {
  let first: Int = 123
  let second: String = "abc"
}

Reflection.test("Class/ObjectiveCBase/Default") {
  do {
    let value = SwiftFooMoreDerivedObjCClass()
    var output = ""
    dump(value, to: &output)

    let expected =
      "▿ This is FooObjCClass #0\n" +
      "  - super: FooMoreDerivedObjCClass\n" +
      "    - super: FooDerivedObjCClass\n" +
      "      - super: FooObjCClass\n" +
      "        - super: NSObject\n" +
      "  - first: 123\n" +
      "  - second: \"abc\"\n"

    expectEqual(expected, output)
  }
}
protocol SomeNativeProto {}
@objc protocol SomeObjCProto {}
extension SomeClass: SomeObjCProto {}

Reflection.test("MetatypeMirror") {
  do {
    let concreteClassMetatype = SomeClass.self
    let expectedSomeClass = "- a.SomeClass #0\n"
    let objcProtocolMetatype: SomeObjCProto.Type = SomeClass.self
    var output = ""
    dump(objcProtocolMetatype, to: &output)
    expectEqual(expectedSomeClass, output)

    let objcProtocolConcreteMetatype = SomeObjCProto.self
    let expectedObjCProtocolConcrete = "- a.SomeObjCProto #0\n"
    output = ""
    dump(objcProtocolConcreteMetatype, to: &output)
    expectEqual(expectedObjCProtocolConcrete, output)

    let compositionConcreteMetatype = (SomeNativeProto & SomeObjCProto).self
    let expectedComposition = "- a.SomeNativeProto & a.SomeObjCProto #0\n"
    output = ""
    dump(compositionConcreteMetatype, to: &output)
    expectEqual(expectedComposition, output)
    
    let objcDefinedProtoType = NSObjectProtocol.self
    expectEqual(String(describing: objcDefinedProtoType), "NSObject")
  }
}

Reflection.test("CGPoint") {
  var output = ""
  dump(CGPoint(x: 1.25, y: 2.75), to: &output)

  let expected =
    "▿ (1.25, 2.75)\n" +
    "  - x: 1.25\n" +
    "  - y: 2.75\n"

  expectEqual(expected, output)
}

Reflection.test("CGSize") {
  var output = ""
  dump(CGSize(width: 1.25, height: 2.75), to: &output)

  let expected =
    "▿ (1.25, 2.75)\n" +
    "  - width: 1.25\n" +
    "  - height: 2.75\n"

  expectEqual(expected, output)
}

Reflection.test("CGRect") {
  var output = ""
  dump(
    CGRect(
      origin: CGPoint(x: 1.25, y: 2.25),
      size: CGSize(width: 10.25, height: 11.75)),
    to: &output)

  let expected =
    "▿ (1.25, 2.25, 10.25, 11.75)\n" +
    "  ▿ origin: (1.25, 2.25)\n" +
    "    - x: 1.25\n" +
    "    - y: 2.25\n" +
    "  ▿ size: (10.25, 11.75)\n" +
    "    - width: 10.25\n" +
    "    - height: 11.75\n"

  expectEqual(expected, output)
}

Reflection.test("Unmanaged/nil") {
  var output = ""
  let optionalURL: Unmanaged<CFURL>? = nil
  dump(optionalURL, to: &output)

  let expected = "- nil\n"

  expectEqual(expected, output)
}

Reflection.test("Unmanaged/not-nil") {
  var output = ""
  let optionalURL: Unmanaged<CFURL>? =
    Unmanaged.passRetained(CFURLCreateWithString(nil, "http://llvm.org/" as CFString, nil))
  dump(optionalURL, to: &output)

  let expected =
    "▿ Optional(Swift.Unmanaged<__C.CFURLRef>(_value: http://llvm.org/))\n" +
    "  ▿ some: Swift.Unmanaged<__C.CFURLRef>\n" +
    "    - _value: http://llvm.org/ #0\n" +
    "      - super: NSObject\n"

  expectEqual(expected, output)

  optionalURL!.release()
}

Reflection.test("TupleMirror/NoLeak") {
  do {
    nsObjectCanaryCount = 0
    autoreleasepool {
      let tuple = (1, NSObjectCanary())
      expectEqual(1, nsObjectCanaryCount)
      var output = ""
      dump(tuple, to: &output)
    }
    expectEqual(0, nsObjectCanaryCount)
  }
  do {
    nsObjectCanaryCount = 0
    autoreleasepool {
      let tuple = (1, NSObjectCanaryStruct())
      expectEqual(1, nsObjectCanaryCount)
      var output = ""
      dump(tuple, to: &output)
    }
    expectEqual(0, nsObjectCanaryCount)
  }
  do {
    swiftObjectCanaryCount = 0
    autoreleasepool {
      let tuple = (1, SwiftObjectCanary())
      expectEqual(1, swiftObjectCanaryCount)
      var output = ""
      dump(tuple, to: &output)
    }
    expectEqual(0, swiftObjectCanaryCount)
  }
  do {
    swiftObjectCanaryCount = 0
    autoreleasepool {
      let tuple = (1, SwiftObjectCanaryStruct())
      expectEqual(1, swiftObjectCanaryCount)
      var output = ""
      dump(tuple, to: &output)
    }
    expectEqual(0, swiftObjectCanaryCount)
  }
}

@objc @objcMembers class TestArtificialSubclass: NSObject {
  dynamic var foo = "foo"
}

var KVOHandle = 0

Reflection.test("Name of metatype of artificial subclass") {
  let obj = TestArtificialSubclass()

  expectEqual("\(type(of: obj))", "TestArtificialSubclass")
  expectEqual(String(describing: type(of: obj)), "TestArtificialSubclass")
  expectEqual(String(reflecting: type(of: obj)), "a.TestArtificialSubclass")

  // Trigger the creation of a KVO subclass for TestArtificialSubclass.
  obj.addObserver(obj, forKeyPath: "foo", options: [.new], context: &KVOHandle)
  expectEqual("\(type(of: obj))", "TestArtificialSubclass")
  expectEqual(String(describing: type(of: obj)), "TestArtificialSubclass")
  expectEqual(String(reflecting: type(of: obj)), "a.TestArtificialSubclass")
  obj.removeObserver(obj, forKeyPath: "foo")

  expectEqual("\(type(of: obj))", "TestArtificialSubclass")
  expectEqual(String(describing: type(of: obj)), "TestArtificialSubclass")
  expectEqual(String(reflecting: type(of: obj)), "a.TestArtificialSubclass")
}

@objc class StringConvertibleInDebugAndOtherwise : NSObject {
  override var description: String { return "description" }
  override var debugDescription: String { return "debugDescription" }
}

Reflection.test("NSObject is properly CustomDebugStringConvertible") {
  let object = StringConvertibleInDebugAndOtherwise()
  expectEqual(String(reflecting: object), object.debugDescription)
}

#if swift(<5) // PlaygroundQuickLook(reflecting:) was obsoleted in Swift 5
Reflection.test("NSRange QuickLook") {
  let rng = NSRange(location:Int.min, length:5)
  let ql = PlaygroundQuickLook(reflecting: rng)
  switch ql {
  case .range(let loc, let len):
    expectEqual(loc, Int64(Int.min))
    expectEqual(len, 5)
  default:
    expectUnreachable("PlaygroundQuickLook for NSRange did not match Range")
  }
}
#endif

class SomeSubclass : SomeClass {}

var ObjCConformsToProtocolTestSuite = TestSuite("ObjCConformsToProtocol")

ObjCConformsToProtocolTestSuite.test("cast/instance") {
  expectTrue(SomeClass() is SomeObjCProto)
  expectTrue(SomeSubclass() is SomeObjCProto)
}

ObjCConformsToProtocolTestSuite.test("cast/metatype") {
  expectTrue(SomeClass.self is SomeObjCProto.Type)
  expectTrue(SomeSubclass.self is SomeObjCProto.Type)
}

// SR-7357

extension Optional where Wrapped == NSData {
    private class Inner {
    }

    var asInner: Inner {
        return Inner()
    }
}

var RuntimeClassNamesTestSuite = TestSuite("runtime class names")

RuntimeClassNamesTestSuite.test("private class nested in same-type-constrained extension") {
  let base: NSData? = nil
  let util = base.asInner

  let clas = unsafeBitCast(type(of: util), to: NSObject.self)
  let desc = clas.description
  expectEqual("_TtCE1a", desc.prefix(7))
  expectEqual("Inner", desc.suffix(5))
}

runAllTests()
