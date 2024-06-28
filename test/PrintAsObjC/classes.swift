// Please keep this file in alphabetical order!

// REQUIRES: objc_interop

// RUN: %empty-directory(%t)

// FIXME: BEGIN -enable-source-import hackaround
// RUN:  %target-swift-frontend(mock-sdk: -sdk %S/../Inputs/clang-importer-sdk -I %t) -emit-module -o %t %S/../Inputs/clang-importer-sdk/swift-modules/ObjectiveC.swift -disable-objc-attr-requires-foundation-module
// RUN:  %target-swift-frontend(mock-sdk: -sdk %S/../Inputs/clang-importer-sdk -I %t) -emit-module -o %t  %S/../Inputs/clang-importer-sdk/swift-modules/CoreGraphics.swift
// RUN:  %target-swift-frontend(mock-sdk: -sdk %S/../Inputs/clang-importer-sdk -I %t) -emit-module -o %t  %S/../Inputs/clang-importer-sdk/swift-modules/Foundation.swift
// RUN:  %target-swift-frontend(mock-sdk: -sdk %S/../Inputs/clang-importer-sdk -I %t) -emit-module -o %t  %S/../Inputs/clang-importer-sdk/swift-modules/AppKit.swift
// FIXME: END -enable-source-import hackaround


// RUN: %target-swift-frontend(mock-sdk: -sdk %S/../Inputs/clang-importer-sdk -I %t) -emit-module -I %S/Inputs/custom-modules -o %t %s -disable-objc-attr-requires-foundation-module
// RUN: %target-swift-frontend(mock-sdk: -sdk %S/../Inputs/clang-importer-sdk -I %t) -parse-as-library %t/classes.swiftmodule -typecheck -I %S/Inputs/custom-modules -emit-objc-header-path %t/classes.h -import-objc-header %S/../Inputs/empty.h -disable-objc-attr-requires-foundation-module
// RUN: %FileCheck %s --input-file %t/classes.h
// RUN: %FileCheck --check-prefix=NEGATIVE %s --input-file %t/classes.h
// RUN: %check-in-clang -I %S/Inputs/custom-modules/ %t/classes.h
// RUN: not %check-in-clang -I %S/Inputs/custom-modules/ -fno-modules -Qunused-arguments %t/classes.h
// RUN: %check-in-clang -I %S/Inputs/custom-modules/ -fno-modules -Qunused-arguments %t/classes.h -include CoreFoundation.h -include objc_generics.h -include SingleGenericClass.h -include CompatibilityAlias.h

// CHECK-NOT: AppKit;
// CHECK-NOT: Properties;
// CHECK-NOT: Swift;
// CHECK-LABEL: @import CompatibilityAlias;
// CHECK-NEXT: @import CoreFoundation;
// CHECK-NEXT: @import CoreGraphics;
// CHECK-NEXT: @import Foundation;
// CHECK-NEXT: @import ObjectiveC;
// CHECK-NEXT: @import SingleGenericClass;
// CHECK-NEXT: @import objc_generics;
// CHECK-NOT: AppKit;
// CHECK-NOT: Swift;
import Foundation
import objc_generics
import AppKit // only used in implementations
import CoreFoundation
import CompatibilityAlias
import SingleGenericClass

// CHECK-LABEL: @interface A1{{$}}
// CHECK-NEXT: init
// CHECK-NEXT: @end
@objc @objcMembers class A1 {}

// CHECK-LABEL: @interface B1 : A1
// CHECK-NEXT: init
// CHECK-NEXT: @end
@objc class B1 : A1 {}

// Used in BridgedTypes test case
struct Notification: _ObjectiveCBridgeable {
  func _bridgeToObjectiveC() -> NSNotification { fatalError() }
  static func _forceBridgeFromObjectiveC(
    _ source: NSNotification,
    result: inout Self?
  ) { fatalError() }
  @discardableResult
  static func _conditionallyBridgeFromObjectiveC(
    _ source: NSNotification,
    result: inout Self?
  ) -> Bool { fatalError() }
  @_effects(readonly)
  static func _unconditionallyBridgeFromObjectiveC(_ source: _ObjectiveCType?)
    -> Self { fatalError() }
}

// CHECK-LABEL: @interface BridgedTypes
// CHECK-NEXT: - (NSDictionary * _Nonnull)dictBridge:(NSDictionary * _Nonnull)x SWIFT_WARN_UNUSED_RESULT;
// CHECK-NEXT: - (NSNotification * _Nonnull)noteBridge:(NSNotification * _Nonnull)x SWIFT_WARN_UNUSED_RESULT;
// CHECK-NEXT: - (NSSet * _Nonnull)setBridge:(NSSet * _Nonnull)x SWIFT_WARN_UNUSED_RESULT;
// CHECK-NEXT: init
// CHECK-NEXT: @end
@objc @objcMembers class BridgedTypes {
  @objc func dictBridge(_ x: Dictionary<NSObject, AnyObject>) -> Dictionary<NSObject, AnyObject> {
    return x
  }

  @objc func noteBridge(_ x: Notification) -> Notification {
    return x
  }

  @objc func setBridge(_ x: Set<NSObject>) -> Set<NSObject> {
    return x
  }
}

// CHECK: @class CustomName2;
// CHECK-LABEL: SWIFT_CLASS_NAMED("ClassWithCustomName")
// CHECK-NEXT: @interface CustomName{{$}}
// CHECK-NEXT: - (void)forwardCustomName:(CustomName2 * _Nonnull)_;
// CHECK-NEXT: init
// CHECK-NEXT: @end
@objc(CustomName)
@objcMembers
 class ClassWithCustomName {
  @objc func forwardCustomName(_: ClassWithCustomName2) {}
}
  
// CHECK-LABEL: SWIFT_CLASS_NAMED("ClassWithCustomName2")
// CHECK-NEXT: @interface CustomName2{{$}}
// CHECK-NEXT: init
// CHECK-NEXT: @end
@objc(CustomName2)
@objcMembers
class ClassWithCustomName2 {}
  
// CHECK-LABEL: SWIFT_CLASS_NAMED("ClassWithCustomNameSub")
// CHECK-NEXT: @interface CustomNameSub : CustomName{{$}}
// CHECK-NEXT: init
// CHECK-NEXT: @end
@objc(CustomNameSub)
class ClassWithCustomNameSub : ClassWithCustomName {}

// CHECK-LABEL: @interface DiscardableResult : NSObject
// CHECK-NEXT: - (NSInteger)nonDiscardable:(NSInteger)x SWIFT_WARN_UNUSED_RESULT;
// CHECK-NEXT: - (NSInteger)discardable:(NSInteger)x;
// CHECK-NEXT: - (nonnull instancetype)init OBJC_DESIGNATED_INITIALIZER;
// CHECK-NEXT: @end
class DiscardableResult : NSObject {
  @objc func nonDiscardable(_ x: Int) -> Int { return x }
  @discardableResult @objc func discardable(_ x: Int) -> Int { return x }
}

// CHECK-LABEL: @interface Initializers
// CHECK-NEXT: - (nonnull instancetype)init OBJC_DESIGNATED_INITIALIZER;
// CHECK-NEXT: - (nonnull instancetype)initWithInt:(NSInteger)_;
// CHECK-NEXT: - (nonnull instancetype)initWithFloat:(float)f;
// CHECK-NEXT: - (nonnull instancetype)initWithString:(NSString * _Nonnull)s boolean:(BOOL)b;
// CHECK-NEXT: - (nullable instancetype)initWithBoolean:(BOOL)b;
// CHECK-NEXT: - (nonnull instancetype)foo_initWithInt:(NSInteger)_ SWIFT_METHOD_FAMILY(init);
// CHECK-NEXT: - (nonnull instancetype)initializeWithX:(NSInteger)_ SWIFT_METHOD_FAMILY(init);
// CHECK-NEXT: - (nonnull instancetype)initForFun OBJC_DESIGNATED_INITIALIZER;
// CHECK-NEXT: - (nonnull instancetype)initWithMoreFun OBJC_DESIGNATED_INITIALIZER;
// CHECK-NEXT: - (nonnull instancetype)initWithEvenMoreFun OBJC_DESIGNATED_INITIALIZER;
// CHECK-NEXT: @end
@objc class Initializers {
  @objc init() {}

  @objc convenience init(int _: Int) { self.init() }

  @objc convenience init(float f: Float) { self.init() }
  @objc convenience init(string s: String, boolean b: ObjCBool) { self.init() }

  @objc convenience init?(boolean b: ObjCBool) { self.init() }

  @objc(foo_initWithInt:) convenience init(foo_int _: Int) { self.init() }
  @objc(initializeWithX:) convenience init(X _: Int) { self.init() }

  @objc init(forFun: ()) { }

  @objc init(moreFun: ()) { }

  @objc init(evenMoreFun: ()) { }
}

// CHECK-LABEL: @interface InheritedInitializers
// CHECK-NEXT: - (nonnull instancetype)init OBJC_DESIGNATED_INITIALIZER;
// CHECK-NEXT: - (nonnull instancetype)initWithFloat:(float)f SWIFT_UNAVAILABLE;
// CHECK-NEXT: - (nonnull instancetype)initWithMoreFun SWIFT_UNAVAILABLE;
// CHECK-NEXT: - (nonnull instancetype)initForFun SWIFT_UNAVAILABLE;
// CHECK-NEXT: - (nonnull instancetype)initWithEvenMoreFun SWIFT_UNAVAILABLE;
// CHECK-NEXT: @end
@objc class InheritedInitializers : Initializers {
  override init() {
    super.init()
  }

  private convenience init(float f: Float) { self.init() }

  private override init(moreFun: ()) { super.init() }
}

// CHECK-LABEL: @interface InheritedInitializersAgain
// CHECK-NEXT: - (nonnull instancetype)init OBJC_DESIGNATED_INITIALIZER;
// CHECK-NEXT: - (nonnull instancetype)initWithEvenMoreFun OBJC_DESIGNATED_INITIALIZER;
// CHECK-NEXT: @end
@objc class InheritedInitializersAgain : InheritedInitializers {
  override init() {
    super.init()
  }

  @objc init(evenMoreFun: ()) { super.init() }
}

// CHECK-LABEL: @interface InheritedInitializersRequired
// CHECK-NEXT: - (nonnull instancetype)initWithEvenMoreFun OBJC_DESIGNATED_INITIALIZER;
// CHECK-NEXT: - (nonnull instancetype)init SWIFT_UNAVAILABLE;
// CHECK-NEXT: + (nonnull instancetype)new SWIFT_DEPRECATED_MSG("-init is unavailable");
// CHECK-NEXT: @end
@objc class InheritedInitializersRequired : InheritedInitializers {
  @objc required init(evenMoreFun: ()) { super.init() }
}

// NEGATIVE-NOT: NotObjC
class NotObjC {}

// CHECK-LABEL: @interface Methods{{$}}
// CHECK-NEXT: - (void)test;
// CHECK-NEXT: + (void)test2;
// CHECK-NEXT: - (void * _Nonnull)testPrimitives:(BOOL)b i:(NSInteger)i f:(float)f d:(double)d u:(NSUInteger)u SWIFT_WARN_UNUSED_RESULT;
// CHECK-NEXT: - (void)testString:(NSString * _Nonnull)s;
// CHECK-NEXT: - (void)testSelector:(SEL _Nonnull)sel boolean:(BOOL)b;
// CHECK-NEXT: - (void)testCSignedTypes:(signed char)a b:(short)b c:(int)c d:(long)d e:(long long)e;
// CHECK-NEXT: - (void)testCUnsignedTypes:(unsigned char)a b:(unsigned short)b c:(unsigned int)c d:(unsigned long)d e:(unsigned long long)e;
// CHECK-NEXT: - (void)testCChars:(char)basic wchar:(wchar_t)wide char16:(char16_t)char16 char32:(char32_t)char32;
// CHECK-NEXT: - (void)testCFloats:(float)a b:(double)b;
// CHECK-NEXT: - (void)testCBool:(bool)a;
// CHECK-NEXT: - (void)testSizedSignedTypes:(int8_t)a b:(int16_t)b c:(int32_t)c d:(int64_t)d;
// CHECK-NEXT: - (void)testSizedUnsignedTypes:(uint8_t)a b:(uint16_t)b c:(uint32_t)c d:(uint64_t)d;
// CHECK-NEXT: - (void)testSizedFloats:(float)a b:(double)b;
// CHECK-NEXT: - (nonnull instancetype)getDynamicSelf SWIFT_WARN_UNUSED_RESULT;
// CHECK-NEXT: - (null_unspecified instancetype)getDynamicSelfIUO SWIFT_WARN_UNUSED_RESULT;
// CHECK-NEXT: + (SWIFT_METATYPE(Methods) _Nonnull)getSelf SWIFT_WARN_UNUSED_RESULT;
// CHECK-NEXT: - (Methods * _Nullable)maybeGetSelf SWIFT_WARN_UNUSED_RESULT;
// CHECK-NEXT: + (SWIFT_METATYPE(Methods) _Nullable)maybeGetSelf SWIFT_WARN_UNUSED_RESULT;
// CHECK-NEXT: - (Methods * _Null_unspecified)uncheckedGetSelf SWIFT_WARN_UNUSED_RESULT;
// CHECK-NEXT: + (SWIFT_METATYPE(Methods) _Null_unspecified)uncheckedGetSelf SWIFT_WARN_UNUSED_RESULT;
// CHECK-NEXT: + (SWIFT_METATYPE(CustomName) _Nonnull)getCustomNameType SWIFT_WARN_UNUSED_RESULT;
// CHECK-NEXT: - (void)testParens:(NSInteger)a;
// CHECK-NEXT: - (void)testIgnoredParam:(NSInteger)_;
// CHECK-NEXT: - (void)testIgnoredParams:(NSInteger)_ again:(NSInteger)_;
// CHECK-NEXT: - (void)testArrayBridging:(NSArray<Methods *> * _Nonnull)a;
// CHECK-NEXT: - (void)testArrayBridging2:(NSArray * _Nonnull)a;
// CHECK-NEXT: - (void)testArrayBridging3:(NSArray<NSString *> * _Nonnull)a;
// CHECK-NEXT: - (void)testDictionaryBridging:(NSDictionary * _Nonnull)a;
// CHECK-NEXT: - (void)testDictionaryBridging2:(NSDictionary<NSNumber *, Methods *> * _Nonnull)a;
// CHECK-NEXT: - (void)testDictionaryBridging3:(NSDictionary<NSString *, NSString *> * _Nonnull)a;
// CHECK-NEXT: - (void)testSetBridging:(NSSet * _Nonnull)a;
// CHECK-NEXT: - (IBAction)actionMethod:(id _Nonnull)_;
// CHECK-NEXT: - (IBSegueAction NSObject * _Nonnull)segueActionMethod:(NSCoder * _Nonnull)coder sender:(id _Nonnull)sender SWIFT_WARN_UNUSED_RESULT;
// CHECK-NEXT: - (void)methodWithReservedParameterNames:(id _Nonnull)long_ protected:(id _Nonnull)protected_;
// CHECK-NEXT: - (void)honorRenames:(CustomName * _Nonnull)_;
// CHECK-NEXT: - (Methods * _Nullable __unsafe_unretained)unmanaged:(id _Nonnull __unsafe_unretained)_ SWIFT_WARN_UNUSED_RESULT;
// CHECK-NEXT: - (void)initAllTheThings SWIFT_METHOD_FAMILY(none);
// CHECK-NEXT: - (void)initTheOtherThings SWIFT_METHOD_FAMILY(none);
// CHECK-NEXT: - (void)initializeEvenMoreThings;
// CHECK-NEXT: + (Methods * _Nonnull)newWithFoo:(NSInteger)foo SWIFT_WARN_UNUSED_RESULT;
// CHECK-NEXT: init
// CHECK-NEXT: @end
@objc class Methods {
  @objc func test() {}
  @objc class func test2() {}

  @objc func testPrimitives(_ b: Bool, i: Int, f: Float, d: Double, u: UInt)
    -> OpaquePointer { return OpaquePointer(bitPattern: -1)! }
  @objc func testString(_ s: String) {}
  @objc func testSelector(_ sel: Selector, boolean b: ObjCBool) {}

  @objc func testCSignedTypes(_ a: CSignedChar, b: CShort, c: CInt, d: CLong, e: CLongLong) {}
  @objc func testCUnsignedTypes(_ a: CUnsignedChar, b: CUnsignedShort, c: CUnsignedInt, d: CUnsignedLong, e: CUnsignedLongLong) {}
  @objc func testCChars(_ basic: CChar, wchar wide: CWideChar, char16: CChar16, char32: CChar32) {}
  @objc func testCFloats(_ a: CFloat, b: CDouble) {}
  @objc func testCBool(_ a: CBool) {}

  @objc func testSizedSignedTypes(_ a: Int8, b: Int16, c: Int32, d: Int64) {}
  @objc func testSizedUnsignedTypes(_ a: UInt8, b: UInt16, c: UInt32, d: UInt64) {}
  @objc func testSizedFloats(_ a: Float32, b: Float64) {}

  @objc func getDynamicSelf() -> Self { return self }
  @objc func getDynamicSelfIUO() -> Self! { return self }
  @objc class func getSelf() -> Methods.Type { return self }

  @objc func maybeGetSelf() -> Methods? { return nil }
  @objc class func maybeGetSelf() -> Methods.Type? { return self }
  @objc func uncheckedGetSelf() -> Methods! { return self }
  @objc class func uncheckedGetSelf() -> Methods.Type! { return self }

  @objc class func getCustomNameType() -> ClassWithCustomName.Type {
    return ClassWithCustomName.self
  }

  @objc func testParens(_ a: ((Int))) {}

  @objc func testIgnoredParam(_: Int) {}
  @objc func testIgnoredParams(_: Int, again _: Int) {}

  @objc func testArrayBridging(_ a: [Methods]) {}
  @objc func testArrayBridging2(_ a: [AnyObject]) {}
  @objc func testArrayBridging3(_ a: [String]) {}

  @objc func testDictionaryBridging(_ a: [NSObject : AnyObject]) {}
  @objc func testDictionaryBridging2(_ a: [NSNumber : Methods]) {}
  @objc func testDictionaryBridging3(_ a: [String : String]) {}

  @objc func testSetBridging(_ a: Set<NSObject>) {}

  @IBAction func actionMethod(_: AnyObject) {}
  @IBSegueAction func segueActionMethod(_ coder: NSCoder, sender: Any) -> NSObject { fatalError() }

  @objc func methodWithReservedParameterNames(_ long: AnyObject, protected: AnyObject) {}

  @objc func honorRenames(_: ClassWithCustomName) {}

  @objc func unmanaged(_: Unmanaged<AnyObject>) -> Unmanaged<Methods>? { return nil }

  @objc func initAllTheThings() {}
  @objc(initTheOtherThings) func setUpOtherThings() {}
  @objc func initializeEvenMoreThings() {}

  @objc(newWithFoo:) class func make(foo: Int) -> Methods { return Methods() }

  @objc init() {}
}

typealias AliasForNSRect = NSRect

// CHECK-LABEL: @class NSURL;
// NEGATIVE-NOT: @class CFTree
// CHECK-LABEL: @interface MethodsWithImports
// CHECK-NEXT: - (NSPoint)getOrigin:(NSRect)r SWIFT_WARN_UNUSED_RESULT;
// CHECK-NEXT: - (CGFloat)getOriginX:(NSRect)r SWIFT_WARN_UNUSED_RESULT;
// CHECK-NEXT: - (CGFloat)getOriginY:(CGRect)r SWIFT_WARN_UNUSED_RESULT;
// CHECK-NEXT: - (NSArray * _Nonnull)emptyArray SWIFT_WARN_UNUSED_RESULT;
// CHECK-NEXT: - (NSArray * _Nullable)maybeArray SWIFT_WARN_UNUSED_RESULT;
// CHECK-NEXT: - (NSRuncingMode)someEnum SWIFT_WARN_UNUSED_RESULT;
// CHECK-NEXT: - (Class <NSCoding> _Nullable)protocolClass SWIFT_WARN_UNUSED_RESULT;
// CHECK-NEXT: - (struct _NSZone * _Nullable)zone SWIFT_WARN_UNUSED_RESULT;
// CHECK-NEXT: - (CFTypeRef _Nullable)cf:(CFTreeRef _Nonnull)x str:(CFStringRef _Nonnull)str str2:(CFMutableStringRef _Nonnull)str2 obj:(CFAliasForTypeRef _Nonnull)obj SWIFT_WARN_UNUSED_RESULT;
// CHECK-NEXT: - (void)appKitInImplementation;
// CHECK-NEXT: - (NSURL * _Nullable)returnsURL SWIFT_WARN_UNUSED_RESULT;
// CHECK-NEXT: init
// CHECK-NEXT: @end
@objc @objcMembers class MethodsWithImports {
  @objc func getOrigin(_ r: NSRect) -> NSPoint { return r.origin }
  @objc func getOriginX(_ r: AliasForNSRect) -> CGFloat { return r.origin.x }
  @objc func getOriginY(_ r: CGRect) -> CGFloat { return r.origin.y }

  @objc func emptyArray() -> NSArray { return NSArray() }
  @objc func maybeArray() -> NSArray? { return nil }

  @objc func someEnum() -> NSRuncingMode { return .mince }
  @objc func protocolClass() -> NSCoding.Type? { return nil }

  @objc func zone() -> NSZone? { return nil }

  @objc func cf(_ x: CFTree, str: CFString, str2: CFMutableString, obj: CFAliasForType) -> CFTypeRef? { return nil }

  @objc func appKitInImplementation() {
    let _ : NSResponder?
  }

  @objc func returnsURL() -> NSURL? { return nil }
}

// CHECK-LABEL: @interface MethodsWithPointers
// CHECK-NEXT: - (id _Nonnull * _Nonnull)test:(NSInteger * _Nonnull)a SWIFT_WARN_UNUSED_RESULT;
// CHECK-NEXT: - (void)testNested:(NSInteger * _Nonnull * _Nonnull)a;
// CHECK-NEXT: - (void)testBridging:(NSInteger const * _Nonnull)a b:(NSInteger * _Nonnull)b c:(Methods * _Nonnull * _Nonnull)c;
// CHECK-NEXT: - (void)testBridgingVoid:(void * _Nonnull)a b:(void const * _Nonnull)b;
// CHECK-NEXT: - (void)testBridgingOptionality:(NSInteger const * _Nullable)a b:(NSInteger * _Null_unspecified)b c:(Methods * _Nullable * _Nullable)c;
// CHECK-NEXT: init
// CHECK-NEXT: @end
@objc @objcMembers class MethodsWithPointers {
  @objc func test(_ a: UnsafeMutablePointer<Int>) -> UnsafeMutablePointer<AnyObject> {
    return UnsafeMutablePointer(bitPattern: -1)!
  }

  @objc func testNested(_ a: UnsafeMutablePointer<UnsafeMutablePointer<Int>>) {}

  @objc func testBridging(_ a: UnsafePointer<Int>, b: UnsafeMutablePointer<Int>, c: AutoreleasingUnsafeMutablePointer<Methods>) {}
  @objc func testBridgingVoid(_ a: UnsafeMutableRawPointer, b: UnsafeRawPointer) {}

  @objc func testBridgingOptionality(_ a: UnsafePointer<Int>?, b: UnsafeMutablePointer<Int>!, c: AutoreleasingUnsafeMutablePointer<Methods?>?) {}
}

// CHECK-LABEL: IB_DESIGNABLE
// CHECK-NEXT: SWIFT_CLASS(
// CHECK-NEXT: @interface MyDesignableObject : NSObject
// CHECK-NEXT: init
// CHECK-NEXT: @end
// NEGATIVE-NOT: @interface NSObject
@IBDesignable class MyDesignableObject : NSObject {}

// CHECK-LABEL: @interface MyObject : NSObject
// CHECK-NEXT: init
// CHECK-NEXT: @end
// NEGATIVE-NOT: @interface NSObject
class MyObject : NSObject {}

// CHECK-LABEL: @protocol MyProtocol <NSObject>
// CHECK-NEXT: @end
@objc protocol MyProtocol : NSObjectProtocol {}

// CHECK-LABEL: @protocol MyProtocolMetaOnly;
// CHECK-LABEL: @interface MyProtocolMetaCheck
// CHECK-NEXT: - (void)test:(Class <MyProtocolMetaOnly> _Nullable)x;
// CHECK-NEXT: init
// CHECK-NEXT: @end
@objc @objcMembers class MyProtocolMetaCheck {
  @objc func test(_ x: MyProtocolMetaOnly.Type?) {}
}
// CHECK-LABEL: @protocol MyProtocolMetaOnly
// CHECK-NEXT: @end
@objc protocol MyProtocolMetaOnly {}

// CHECK-LABEL: @interface Nested
// CHECK-NEXT: init
// CHECK-NEXT: @end
@objc @objcMembers class Nested {
  // CHECK-LABEL: @interface Inner
  // CHECK-NEXT: init
  // CHECK-NEXT: @end
  @objc @objcMembers class Inner {
    // CHECK-LABEL: @interface DeeperIn
    // CHECK-NEXT: init
    // CHECK-NEXT: @end
    @objc @objcMembers class DeeperIn {}
  }

  // CHECK-LABEL: SWIFT_CLASS_NAMED("CustomNameInner")
  // CHECK-NEXT: @interface MyInnerClass
  // CHECK-NEXT: init
  // CHECK-NEXT: @end
  @objc(MyInnerClass) @objcMembers class CustomNameInner {}

  // CHECK-LABEL: @interface AnotherInner : A1
  // CHECK-NEXT: init
  // CHECK-NEXT: @end
  @objc @objcMembers class AnotherInner : A1 {}

  // NEGATIVE-NOT: NonObjCInner
  class NonObjCInner {}

  // NEGATIVE-NOT: ImplicitObjCInner
  class ImplicitObjCInner : A1 {}
}

// CHECK-LABEL: @interface NestedCollision1Identical
// CHECK-NEXT: - (void)before
// CHECK-NEXT: init
// CHECK-NEXT: @end
// CHECK: @interface NestedCollision2Identical
// CHECK-NEXT: - (void)after
// CHECK-NEXT: init
// CHECK-NEXT: @end

// We're intentionally declaring NestedCollision2 before NestedCollision1 to
// make sure they're being sorted based on their names, not their source order.
@objc @objcMembers class NestedCollision2 {
  @objc(NestedCollision2Identical) @objcMembers class Identical: NSObject {
    @objc func after() {}
  }
}
@objc @objcMembers class NestedCollision1 {
  @objc(NestedCollision1Identical) @objcMembers class Identical: NSObject {
    @objc func before() {}
  }
}

// CHECK-LABEL: @class Inner2;
// CHECK-LABEL: @interface NestedMembers
// CHECK-NEXT: @property (nonatomic, strong) Inner2 * _Nullable ref2;
// CHECK-NEXT: @property (nonatomic, strong) Inner3 * _Nullable ref3;
// CHECK-NEXT: init
// CHECK-NEXT: @end
@objc @objcMembers class NestedMembers {
  // NEGATIVE-NOT: @class NestedMembers;
  // CHECK-LABEL: @interface Inner2
  // CHECK-NEXT: @property (nonatomic, strong) NestedMembers * _Nullable ref;
  // CHECK-NEXT: init
  // CHECK-NEXT: @end
  @objc @objcMembers class Inner2 {
    @objc var ref: NestedMembers?
  }

  @objc var ref2: Inner2?
  @objc var ref3: Inner3?

  // CHECK-LABEL: @interface Inner3
  // CHECK-NEXT: @property (nonatomic, strong) NestedMembers * _Nullable ref;
  // CHECK-NEXT: init
  // CHECK-NEXT: @end
  @objc @objcMembers class Inner3 {
    @objc var ref: NestedMembers?
  }
}

// CHECK-LABEL: @interface NestedSuperclass
// CHECK-NEXT: init
// CHECK-NEXT: @end
@objc @objcMembers class NestedSuperclass {
  // CHECK-LABEL: @interface Subclass : NestedSuperclass
  // CHECK-NEXT: init
  // CHECK-NEXT: @end
  @objc @objcMembers class Subclass : NestedSuperclass {}
}

// CHECK-LABEL: @interface NewBanned
// CHECK-NEXT: - (nonnull instancetype)initWithArbitraryArgument:(NSInteger)arbitraryArgument OBJC_DESIGNATED_INITIALIZER;
// CHECK-NEXT: - (nonnull instancetype)init SWIFT_UNAVAILABLE;
// CHECK-NEXT: + (nonnull instancetype)new SWIFT_DEPRECATED_MSG("-init is unavailable");
// CHECK-NEXT: @end
@objc @objcMembers class NewBanned : NSObject {
  init(arbitraryArgument: Int) { super.init() }
}

// CHECK-LABEL: @interface NewBanned
// CHECK-NEXT: - (nonnull instancetype)initWithDifferentArbitraryArgument:(NSInteger)differentArbitraryArgument OBJC_DESIGNATED_INITIALIZER;
// CHECK-NEXT: - (nonnull instancetype)initWithArbitraryArgument:(NSInteger)arbitraryArgument SWIFT_UNAVAILABLE;
// CHECK-NEXT: @end
@objc @objcMembers class NewBannedStill : NewBanned {
  init(differentArbitraryArgument: Int) { super.init(arbitraryArgument: 0) }
}

// CHECK-LABEL: @interface NewUnbanned
// CHECK-NEXT: - (nonnull instancetype)init OBJC_DESIGNATED_INITIALIZER;
// CHECK-NEXT: + (nonnull instancetype)new;
// CHECK-NEXT: - (nonnull instancetype)initWithArbitraryArgument:(NSInteger)arbitraryArgument SWIFT_UNAVAILABLE;
// CHECK-NEXT: @end
@objc @objcMembers class NewUnbanned : NewBanned {
  init() { super.init(arbitraryArgument: 0) }
}

// CHECK-LABEL: @interface NewUnbannedDouble
// CHECK-NEXT: - (nonnull instancetype)init OBJC_DESIGNATED_INITIALIZER;
// CHECK-NEXT: + (nonnull instancetype)new;
// CHECK-NEXT: - (nonnull instancetype)initWithDifferentArbitraryArgument:(NSInteger)differentArbitraryArgument SWIFT_UNAVAILABLE;
// CHECK-NEXT: @end
@objc @objcMembers class NewUnbannedDouble : NewBannedStill {
  init() { super.init(differentArbitraryArgument: 0) }
}

// NEGATIVE-NOT: @interface Private :
private class Private : A1 {}

// CHECK-LABEL: @interface PrivateMembers
// CHECK-NEXT: @end
@objc class PrivateMembers {
  private var i = 0
  private func foo() {}
  private init() {}
  @objc private func bar() {}
}

public class NonObjCClass { }

// CHECK-LABEL: @interface Properties
// CHECK-NEXT: @property (nonatomic) NSInteger i;
// CHECK-NEXT: @property (nonatomic, readonly, strong) Properties * _Nonnull mySelf;
// CHECK-NEXT: @property (nonatomic, readonly) double pi;
// CHECK-NEXT: @property (nonatomic) NSInteger computed;
// CHECK-NEXT: SWIFT_CLASS_PROPERTY(@property (nonatomic, class, strong) Properties * _Nonnull shared;)
// CHECK-NEXT: + (Properties * _Nonnull)shared SWIFT_WARN_UNUSED_RESULT;
// CHECK-NEXT: + (void)setShared:(Properties * _Nonnull)newValue;
// CHECK-NEXT: SWIFT_CLASS_PROPERTY(@property (nonatomic, class, readonly, strong) Properties * _Nonnull sharedRO;)
// CHECK-NEXT: + (Properties * _Nonnull)sharedRO SWIFT_WARN_UNUSED_RESULT;
// CHECK-NEXT: @property (nonatomic, weak) Properties * _Nullable weakOther;
// CHECK-NEXT: @property (nonatomic, unsafe_unretained) Properties * _Nonnull unownedOther;
// CHECK-NEXT: @property (nonatomic, unsafe_unretained) Properties * _Nonnull unmanagedOther;
// CHECK-NEXT: @property (nonatomic, unsafe_unretained) Properties * _Nullable unmanagedByDecl;
// CHECK-NEXT: @property (nonatomic, weak) id <MyProtocol> _Nullable weakProto;
// CHECK-NEXT: @property (nonatomic) CFTypeRef _Nullable weakCF;
// CHECK-NEXT: @property (nonatomic) CFStringRef _Nullable weakCFString;
// CHECK-NEXT: @property (nonatomic) CFTypeRef _Nullable strongCF;
// CHECK-NEXT: @property (nonatomic) CFTypeRef _Nullable strongCFAlias;
// CHECK-NEXT: @property (nonatomic) CFAliasForTypeRef _Nullable anyCF;
// CHECK-NEXT: @property (nonatomic) CFAliasForTypeRef _Nullable anyCF2;
// CHECK-NEXT: @property (nonatomic, weak) IBOutlet id _Null_unspecified outlet;
// CHECK-NEXT: @property (nonatomic, strong) IBOutlet Properties * _Null_unspecified typedOutlet;
// CHECK-NEXT: @property (nonatomic) IBInspectable NSInteger inspectable;
// CHECK-NEXT: @property (nonatomic, copy) NSString * _Nonnull string;
// CHECK-NEXT: @property (nonatomic, copy) NSArray * _Nonnull array;
// CHECK-NEXT: @property (nonatomic, copy) NSArray<NSArray<NSNumber *> *> * _Nonnull arrayOfArrays;
// CHECK-NEXT: @property (nonatomic, copy) NSArray<BOOL (^)(id _Nonnull, NSInteger)> * _Nonnull arrayOfBlocks;
// CHECK-NEXT: @property (nonatomic, copy) NSArray<NSArray<void (^)(void)> *> * _Nonnull arrayOfArrayOfBlocks;
// CHECK-NEXT: @property (nonatomic, copy) NSDictionary<NSString *, NSString *> * _Nonnull dictionary;
// CHECK-NEXT: @property (nonatomic, copy) NSDictionary<NSString *, NSNumber *> * _Nonnull dictStringInt;
// CHECK-NEXT: @property (nonatomic, copy) NSSet<NSString *> * _Nonnull stringSet;
// CHECK-NEXT: @property (nonatomic, copy) NSSet<NSNumber *> * _Nonnull intSet;
// CHECK-NEXT: @property (nonatomic, copy) NSArray<NSNumber *> * _Nonnull cgFloatArray;
// CHECK-NEXT: @property (nonatomic, copy) NSArray<NSValue *> * _Nonnull rangeArray;
// CHECK-NEXT: @property (nonatomic, copy) IBOutletCollection(Properties) NSArray<Properties *> * _Null_unspecified outletCollection;
// CHECK-NEXT: @property (nonatomic, copy) IBOutletCollection(CustomName) NSArray<CustomName *> *  _Nullable outletCollectionOptional;
// CHECK-NEXT: @property (nonatomic, copy) IBOutletCollection(id) NSArray * _Nullable outletCollectionAnyObject;
// CHECK-NEXT: @property (nonatomic, copy) IBOutletCollection(id) NSArray<id <NSObject>> * _Nullable outletCollectionProto;
// CHECK-NEXT: SWIFT_CLASS_PROPERTY(@property (nonatomic, class, readonly) NSInteger staticInt;)
// CHECK-NEXT: + (NSInteger)staticInt SWIFT_WARN_UNUSED_RESULT;
// CHECK-NEXT: SWIFT_CLASS_PROPERTY(@property (nonatomic, class, copy) NSString * _Nonnull staticString;)
// CHECK-NEXT: + (NSString * _Nonnull)staticString SWIFT_WARN_UNUSED_RESULT;
// CHECK-NEXT: + (void)setStaticString:(NSString * _Nonnull)value;
// CHECK-NEXT: SWIFT_CLASS_PROPERTY(@property (nonatomic, class, readonly) double staticDouble;)
// CHECK-NEXT: + (double)staticDouble SWIFT_WARN_UNUSED_RESULT;
// CHECK-NEXT: SWIFT_CLASS_PROPERTY(@property (nonatomic, class, readonly, copy) NSDictionary<NSString *, NSString *> * _Nonnull staticDictionary;)
// CHECK-NEXT: + (NSDictionary<NSString *, NSString *> * _Nonnull)staticDictionary SWIFT_WARN_UNUSED_RESULT;
// CHECK-NEXT: @property (nonatomic, strong) Properties * _Nullable wobble;
// CHECK-NEXT: @property (nonatomic, getter=isEnabled, setter=setIsEnabled:) BOOL enabled;
// CHECK-NEXT: @property (nonatomic) BOOL isAnimated;
// CHECK-NEXT: @property (nonatomic, getter=register, setter=setRegister:) BOOL register_;
// CHECK-NEXT: @property (nonatomic, readonly, strong, getter=this) Properties * _Nonnull this_;
// CHECK-NEXT: @property (nonatomic, readonly) NSInteger privateSetter;
// CHECK-NEXT: @property (nonatomic, readonly, getter=customGetterNameForPrivateSetter) BOOL privateSetterCustomNames;
// CHECK-NEXT: SWIFT_CLASS_PROPERTY(@property (nonatomic, class, readonly) NSInteger privateSetter;)
// CHECK-NEXT: + (NSInteger)privateSetter SWIFT_WARN_UNUSED_RESULT;
// CHECK-NEXT: SWIFT_CLASS_PROPERTY(@property (nonatomic, class, readonly, getter=customGetterNameForPrivateSetter) BOOL privateSetterCustomNames;)
// CHECK-NEXT: + (BOOL)customGetterNameForPrivateSetter SWIFT_WARN_UNUSED_RESULT;
// CHECK-NEXT: SWIFT_CLASS_PROPERTY(@property (nonatomic, class, readonly) NSInteger sharedConstant;)
// CHECK-NEXT: + (NSInteger)sharedConstant SWIFT_WARN_UNUSED_RESULT;
// CHECK-NEXT: @property (nonatomic) NSInteger initContext;
// CHECK-NEXT: - (NSInteger)initContext SWIFT_METHOD_FAMILY(none) SWIFT_WARN_UNUSED_RESULT;
// CHECK-NEXT: @property (nonatomic, readonly) NSInteger initContextRO;
// CHECK-NEXT: - (NSInteger)initContextRO SWIFT_METHOD_FAMILY(none) SWIFT_WARN_UNUSED_RESULT;
// CHECK-NEXT: @property (nonatomic, getter=initGetter) BOOL getterIsInit;
// CHECK-NEXT: - (BOOL)initGetter SWIFT_METHOD_FAMILY(none) SWIFT_WARN_UNUSED_RESULT;
// CHECK-NEXT: @property (nonatomic, setter=initSetter:) BOOL setterIsInit;
// CHECK-NEXT: - (void)initSetter:(BOOL)newValue SWIFT_METHOD_FAMILY(none);
// CHECK-NEXT: @property (nonatomic, copy) NSURL * _Nullable customValueTypeProp;
// CHECK-NEXT: init
// CHECK-NEXT: @end
@objc class Properties {
  @objc var i: Int = 1
  @objc var mySelf: Properties {
    return self
  }
  @objc let pi = 3.14
  @objc var computed: Int {
    get {
      return 42
    }
    set {
      // Ignore it.
    }
  }

  @objc class var shared: Properties {
    get { return Properties() }
    set { }
  }

  @objc class var sharedRO: Properties {
    get { return Properties() }
  }

  @objc weak var weakOther: Properties?
  @objc unowned var unownedOther: Properties = .shared
  @objc unowned(unsafe) var unmanagedOther: Properties = .shared
  @objc var unmanagedByDecl: Unmanaged<Properties>?

  @objc weak var weakProto: MyProtocol?
  @objc weak var weakCF: CFTypeRef?
  @objc weak var weakCFString: CFString?

  typealias CFTypeRefAlias = CFTypeRef

  @objc var strongCF: CFTypeRef?
  @objc var strongCFAlias: CFTypeRefAlias?

  @objc var anyCF: CFAliasForType?
  @objc var anyCF2: CFAliasForType?

  @IBOutlet weak var outlet: AnyObject!
  @IBOutlet var typedOutlet: Properties!
  @IBInspectable var inspectable: Int = 0

  @objc var string = "abc"
  @objc var array: Array<AnyObject> = []
  @objc var arrayOfArrays: Array<Array<Int>> = []
  @objc var arrayOfBlocks: Array<@convention(block) (AnyObject, Int) -> Bool> = []
  @objc var arrayOfArrayOfBlocks: Array<Array<@convention(block) () -> Void>> = []
  @objc var dictionary: Dictionary<String, String> = [:]
  @objc var dictStringInt: Dictionary<String, Int> = [:]
  @objc var stringSet: Set<String> = []
  @objc var intSet: Set<Int> = []
  @objc var cgFloatArray: Array<CGFloat> = []
  @objc var rangeArray: Array<NSRange> = []

  @IBOutlet var outletCollection: [Properties]!
  @IBOutlet var outletCollectionOptional: [ClassWithCustomName]? = []
  @IBOutlet var outletCollectionAnyObject: [AnyObject]?
  @IBOutlet var outletCollectionProto: [NSObjectProtocol]?

  @objc static let staticInt = 2
  @objc static var staticString = "Hello"
  @objc static var staticDouble: Double {
    return 2.0
  }
  @objc static var staticDictionary: [String: String] { return [:] }

  @objc(wobble) var wibble: Properties?

  @objc var enabled: Bool {
    @objc(isEnabled) get { return true }
    @objc(setIsEnabled:) set { }
  }

  @objc var isAnimated: Bool = true

  @objc var register: Bool = false
  @objc var this: Properties { return self }

  @objc private(set) var privateSetter = 2
  @objc private(set) var privateSetterCustomNames: Bool {
    @objc(customGetterNameForPrivateSetter) get { return true }
    @objc(customSetterNameForPrivateSetter:) set {}
  }

  @objc static private(set) var privateSetter = 2
  @objc class private(set) var privateSetterCustomNames: Bool {
    @objc(customGetterNameForPrivateSetter) get { return true }
    @objc(customSetterNameForPrivateSetter:) set {}
  }
  @objc static let sharedConstant = 2

  @objc var initContext = 4
  @objc var initContextRO: Int { return 4 }
  @objc var getterIsInit: Bool {
    @objc(initGetter) get { return true }
    set {}
  }
  @objc var setterIsInit: Bool {
    get { return true }
    @objc(initSetter:) set {}
  }

  @objc var customValueTypeProp: URL?

  @objc init() {}
}

// CHECK-LABEL: @interface PropertiesOverridden
// CHECK-NEXT: @property (nonatomic, copy) NSArray<Bee *> * _Nonnull bees;
// CHECK-NEXT: - (null_unspecified instancetype)init
// CHECK-NEXT: - (null_unspecified instancetype)initWithCoder:(NSCoder * _Null_unspecified)aDecoder OBJC_DESIGNATED_INITIALIZER;
// CHECK-NEXT: @end
@objc class PropertiesOverridden : Hive {
  override var bees : [Bee] {
    get {
      return super.bees
    }
    set {
      // Ignore it.
    }
  }
}

// CHECK-LABEL: @interface ReversedOrder2{{$}}
// CHECK-NEXT: init
// CHECK-NEXT: @end
// CHECK: SWIFT_CLASS("_TtC7classes14ReversedOrder1")
// CHECK-NEXT: @interface ReversedOrder1 : ReversedOrder2
// CHECK-NEXT: init
// CHECK-NEXT: @end
@objc class ReversedOrder1 : ReversedOrder2 {}
@objc @objcMembers class ReversedOrder2 {}


// CHECK-LABEL: @interface Subscripts1
// CHECK-NEXT: - (Subscripts1 * _Nonnull)objectAtIndexedSubscript:(NSInteger)i SWIFT_WARN_UNUSED_RESULT;
// CHECK-NEXT: - (Subscripts1 * _Nonnull)objectForKeyedSubscript:(Subscripts1 * _Nonnull)o SWIFT_WARN_UNUSED_RESULT;
// CHECK-NEXT: init
// CHECK-NEXT: @end
@objc @objcMembers class Subscripts1 {
  @objc subscript (i: Int) -> Subscripts1 {
    return self
  }

  @objc subscript (o: Subscripts1) -> Subscripts1 {
    return self
  }
}

// CHECK-LABEL: @interface Subscripts2
// CHECK-NEXT: - (Subscripts2 * _Nonnull)objectAtIndexedSubscript:(int16_t)i SWIFT_WARN_UNUSED_RESULT;
// CHECK-NEXT: - (void)setObject:(Subscripts2 * _Nonnull)newValue atIndexedSubscript:(int16_t)i;
// CHECK-NEXT: - (NSObject * _Nonnull)objectForKeyedSubscript:(NSObject * _Nonnull)o SWIFT_WARN_UNUSED_RESULT;
// CHECK-NEXT: - (void)setObject:(NSObject * _Nonnull)newValue forKeyedSubscript:(NSObject * _Nonnull)o;
// CHECK-NEXT: @property (nonatomic, copy) NSArray<NSString *> * _Nonnull cardPaths;
// CHECK-NEXT: init
// CHECK-NEXT: @end
@objc @objcMembers class Subscripts2 {
  @objc subscript (i: Int16) -> Subscripts2 {
    get {
      return self
    }
    set {
      // Ignore it.
    }
  }

  @objc subscript (o: NSObject) -> NSObject {
    get {
      return o
    }
    set {
      // Ignore it.
    }
  }

  // <rdar://problem/17165953> Swift: lazy property reflects back into Objective-C with two properties, one for underlying storage
  @objc lazy var cardPaths : [String] = []
}

// CHECK-LABEL: @interface Subscripts3
// CHECK-NEXT: - (Subscripts3 * _Nonnull)objectAtIndexedSubscript:(unsigned long)_ SWIFT_WARN_UNUSED_RESULT;
// CHECK-NEXT: init
// CHECK-NEXT: @end
@objc @objcMembers class Subscripts3 {
  @objc subscript (_: CUnsignedLong) -> Subscripts3 {
    return self
  }

  subscript (multi: Int, multi2: Int) -> () {
    return ()
  }
}

// CHECK-LABEL: @interface Throwing1
// CHECK-NEXT: - (BOOL)method1AndReturnError:(NSError * _Nullable * _Nullable)error;
// CHECK-NEXT: - (Throwing1 * _Nullable)method2AndReturnError:(NSError * _Nullable * _Nullable)error SWIFT_WARN_UNUSED_RESULT;
// CHECK-NEXT: - (NSArray<NSString *> * _Nullable)method3:(NSInteger)x error:(NSError * _Nullable * _Nullable)error SWIFT_WARN_UNUSED_RESULT;
// CHECK-NEXT: - (nullable instancetype)method4AndReturnError:(NSError * _Nullable * _Nullable)error SWIFT_WARN_UNUSED_RESULT;
// CHECK-NEXT: - (nullable instancetype)initAndReturnError:(NSError * _Nullable * _Nullable)error OBJC_DESIGNATED_INITIALIZER;
// CHECK-NEXT: - (nullable instancetype)initWithString:(NSString * _Nonnull)string error:(NSError * _Nullable * _Nullable)error OBJC_DESIGNATED_INITIALIZER;
// CHECK-NEXT: - (nullable instancetype)initAndReturnError:(NSError * _Nullable * _Nullable)error fn:(SWIFT_NOESCAPE NSInteger (^ _Nonnull)(NSInteger))fn OBJC_DESIGNATED_INITIALIZER;
// CHECK-NEXT: @end
@objc class Throwing1 {
  @objc func method1() throws { }
  @objc func method2() throws -> Throwing1 { return self }
  @objc func method3(_ x: Int) throws -> [String] { return [] }
  @objc func method4() throws -> Self { return self }

  @objc init() throws { }
  @objc init(string: String) throws { }
  @objc init(fn: (Int) -> Int) throws { }
}

@objc class Spoon: Fungible {}

// CHECK-LABEL: @interface UsesCompatibilityAlias
@objc class UsesCompatibilityAlias : NSObject {
  // CHECK-NEXT: - (StringCheese * _Nullable)foo SWIFT_WARN_UNUSED_RESULT;
  @objc func foo() -> StringCheese? { return nil }

  // CHECK-NEXT: - (GymClass<StringCheese *> * _Nullable)foosball SWIFT_WARN_UNUSED_RESULT;
  @objc func foosball() -> GymClass<StringCheese>? { return nil }

  // CHECK-NEXT: - (nonnull instancetype)init OBJC_DESIGNATED_INITIALIZER;
}
// CHECK-NEXT: @end

// CHECK-LABEL: @interface UsesImportedGenerics
@objc class UsesImportedGenerics {
  // CHECK: - (GenericClass<id> * _Nonnull)takeAndReturnGenericClass:(GenericClass<NSString *> * _Nullable)x SWIFT_WARN_UNUSED_RESULT;
  @objc func takeAndReturnGenericClass(_ x: GenericClass<NSString>?) -> GenericClass<AnyObject> { fatalError("") }
  // CHECK: - (FungibleContainer<id <Fungible>> * _Null_unspecified)takeAndReturnFungibleContainer:(FungibleContainer<Spoon *> * _Nonnull)x SWIFT_WARN_UNUSED_RESULT;
  @objc func takeAndReturnFungibleContainer(_ x: FungibleContainer<Spoon>) -> FungibleContainer<Fungible>! { fatalError("") }

  typealias Dipper = Spoon
  // CHECK: - (FungibleContainer<FungibleObject> * _Nonnull)fungibleContainerWithAliases:(FungibleContainer<Spoon *> * _Nullable)x SWIFT_WARN_UNUSED_RESULT;
  @objc func fungibleContainerWithAliases(_ x: FungibleContainer<Dipper>?) -> FungibleContainer<FungibleObject> { fatalError("") }

  // CHECK: - (void)referenceSingleGenericClass:(SingleImportedObjCGeneric<id> * _Nullable)_;
  @objc func referenceSingleGenericClass(_: SingleImportedObjCGeneric<AnyObject>?) {}
}
// CHECK: @end

// CHECK: SWIFT_WEAK_IMPORT
// CHECK-NEXT: SWIFT_CLASS("_TtC7classes17WeakImportedClass")
// CHECK-NEXT: @interface WeakImportedClass
// CHECK-NEXT: @end
@_weakLinked @objc class WeakImportedClass {}
