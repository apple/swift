// RUN: %target-swift-frontend -O -Xllvm -sil-disable-pass=FunctionSignatureOpts -emit-sil %s | %FileCheck %s

// We want to check two things here:
// - Correctness
// - That certain "is" checks are eliminated based on static analysis at compile-time
//
// In ideal world, all those testNN functions should be simplified down to a single basic block
// which returns either true or false, i.e. all type checks should folded statically.

// REQUIRES: objc_interop

import Foundation

class ObjCX : NSObject {}

struct CX: _ObjectiveCBridgeable {
  func _bridgeToObjectiveC() -> ObjCX {
    return ObjCX()
  }

  static func _forceBridgeFromObjectiveC(_ source: ObjCX, result: inout CX?) {}

  static func _conditionallyBridgeFromObjectiveC(_ source: ObjCX, result: inout CX?) -> Bool {
    return false
  }

  static func _unconditionallyBridgeFromObjectiveC(_ source: ObjCX?)
      -> CX {
    var result: CX?
    _forceBridgeFromObjectiveC(source!, result: &result)
    return result!
  }
}

// Check casts to types which are _ObjectiveCBridgeable
func cast0(_ o: AnyObject) -> Bool {
  return o is CX
}

// CHECK-LABEL: sil hidden [noinline] @$s17cast_folding_objc5test0SbyF
// CHECK: bb0
// Check that cast is not eliminated even though cast0 is a conversion
// from a class to struct, because it casts to a struct implementing
// the _BridgedToObjectiveC protocol
// CHECK: checked_cast
// CHECK: return
@inline(never)
func test0() -> Bool {
  return cast0(NSNumber(value:1))
}

// Check that this cast does not get eliminated, because
// the compiler does not statically know if this object
// is NSNumber can be converted into Int.

// CHECK-LABEL: sil [noinline] @$s17cast_folding_objc35testMayBeBridgedCastFromObjCtoSwiftySiyXlF
// CHECK: unconditional_checked_cast_addr
// CHECK: return
@inline(never)
public func testMayBeBridgedCastFromObjCtoSwift(_ o: AnyObject) -> Int {
  return o as! Int
}

// Check that this cast does not get eliminated, because
// the compiler does not statically know if this object
// is NSString can be converted into String.

// CHECK-LABEL: sil [noinline] @$s17cast_folding_objc41testConditionalBridgedCastFromObjCtoSwiftySSSgyXlF
// CHECK: unconditional_checked_cast_addr
// CHECK: return
@inline(never)
public func testConditionalBridgedCastFromObjCtoSwift(_ o: AnyObject) -> String? {
  return o as? String
}

public func castObjCToSwift<T>(_ t: T) -> Int {
  return t as! Int
}

// Check that compiler understands that this cast always fails
// CHECK-LABEL: sil [noinline] {{.*}}@$s17cast_folding_objc37testFailingBridgedCastFromObjCtoSwiftySiSo8NSStringCF
// CHECK: [[ONE:%[0-9]+]] = integer_literal $Builtin.Int1, -1
// CHECK: cond_fail [[ONE]] : $Builtin.Int1, "failed cast"
// CHECK-NEXT: unreachable
// CHECK-NEXT: }
@inline(never)
public func testFailingBridgedCastFromObjCtoSwift(_ ns: NSString) -> Int {
  return castObjCToSwift(ns)
}

// Check that compiler understands that this cast always fails
// CHECK-LABEL: sil [noinline] {{.*}}@$s17cast_folding_objc37testFailingBridgedCastFromSwiftToObjCySiSSF
// CHECK: [[ONE:%[0-9]+]] = integer_literal $Builtin.Int1, -1
// CHECK: cond_fail [[ONE]] : $Builtin.Int1, "failed cast"
// CHECK-NEXT: unreachable
// CHECK-NEXT: }
@inline(never)
public func testFailingBridgedCastFromSwiftToObjC(_ s: String) -> NSInteger {
  return s as! NSInteger
}

@inline(never)
public func testCastNSObjectToAnyClass(_ o: NSObject) -> AnyClass {
  return o as! AnyClass
}

@inline(never)
public func testCastNSObjectToClassObject(_ o: NSObject) -> NSObject.Type {
  return o as! NSObject.Type
}

@inline(never)
public func testCastNSObjectToAnyType(_ o: NSObject) -> Any.Type {
  return o as! Any.Type
}

@inline(never)
public func testCastNSObjectToEveryType<T>(_ o: NSObject) -> T.Type {
  return o as! T.Type
}

@inline(never)
public func testCastNSObjectToNonClassType(_ o: NSObject) -> Int.Type {
  return o as! Int.Type
}

@inline(never)
public func testCastAnyObjectToAnyClass(_ o: AnyObject) -> AnyClass {
  return o as! AnyClass
}

@inline(never)
public func testCastAnyObjectToClassObject(_ o: AnyObject) -> AnyObject.Type {
  return o as! AnyObject.Type
}

@inline(never)
public func testCastAnyObjectToAnyType(_ o: AnyObject) -> Any.Type {
  return o as! Any.Type
}

@inline(never)
public func testCastAnyObjectToEveryType<T>(_ o: AnyObject) -> T.Type {
  return o as! T.Type
}

@inline(never)
public func testCastAnyObjectToNonClassType(_ o: AnyObject) -> Int.Type {
  return o as! Int.Type
}

@inline(never)
public func testCastAnyToAny2Class(_ o: Any) -> AnyClass {
  return o as! AnyClass
}

@inline(never)
public func testCastAnyToClassObject(_ o: Any) -> AnyObject.Type {
  return o as! AnyObject.Type
}

@inline(never)
public func testCastAnyToAny2Type(_ o: Any) -> Any.Type {
  return o as! Any.Type
}

@inline(never)
public func testCastAnyToEveryType<T>(_ o: Any) -> T.Type {
  return o as! T.Type
}

@inline(never)
public func testCastAnyToNonClassType(_ o: Any) -> Int.Type {
  return o as! Int.Type
}

@inline(never)
public func testCastEveryToAnyClass<T>(_ o: T) -> AnyClass {
  return o as! AnyClass
}

@inline(never)
public func testCastEveryToClassObject<T>(_ o: T) -> AnyObject.Type {
  return o as! AnyObject.Type
}

@inline(never)
public func testCastEveryToAnyType<T>(_ o: T) -> Any.Type {
  return o as! Any.Type
}

@inline(never)
public func testCastEveryToEvery2Type<T, U>(_ o: U) -> T.Type {
  return o as! T.Type
}

@inline(never)
public func testCastEveryToNonClassType<T>(_ o: T) -> Int.Type {
  return o as! Int.Type
}

func cast<U, V>(_ u: U.Type) -> V? {
  return u as? V
}

public protocol P {
}

// Any casts from P.Protocol to P.Type should fail.
@inline(never)
public func testCastPProtocolToPType() -> ObjCP.Type? {
  return cast(ObjCP.self)
}

@objc
public protocol ObjCP {
}

@inline(never)
public func testCastObjCPProtocolToObjCPType() -> ObjCP.Type? {
  return cast(ObjCP.self)
}

@inline(never)
public func testCastProtocolCompositionProtocolToProtocolCompositionType() -> (P & ObjCP).Type? {
  return cast((P & ObjCP).self)
}

@inline(never)
public func testCastProtocolCompositionProtocolToProtocolType () -> P.Type? {
  return (P & ObjCP).self as? P.Type
}

print("test0=\(test0())")


// CHECK-LABEL: sil [noinline] {{.*}}@{{.*}}testCastNSObjectToEveryType{{.*}}
// CHECK:         unconditional_checked_cast_addr

// CHECK-LABEL: sil [noinline] {{.*}}@{{.*}}testCastNSObjectToNonClassType
// CHECK: [[ONE:%[0-9]+]] = integer_literal $Builtin.Int1, -1
// CHECK: cond_fail [[ONE]] : $Builtin.Int1, "failed cast"

// CHECK-LABEL: sil [noinline] {{.*}}@{{.*}}testCastAnyObjectToEveryType{{.*}}
// CHECK:         unconditional_checked_cast_addr

// CHECK-LABEL: sil [noinline] {{.*}}@{{.*}}testCastAnyObjectToNonClassType
// CHECK: [[MT:%[0-9]+]] = metatype $@thin Int.Type
// CHECK: return [[MT]]

// CHECK-LABEL: sil [noinline] {{.*}}@{{.*}}testCastAnyToAny2Class{{.*}}
// CHECK:         unconditional_checked_cast_addr

// CHECK-LABEL: sil [noinline] {{.*}}@{{.*}}testCastAnyToClassObject{{.*}}
// CHECK:         unconditional_checked_cast_addr

// CHECK-LABEL: sil [noinline] {{.*}}@{{.*}}testCastAnyToAny2Type{{.*}}
// CHECK:         unconditional_checked_cast_addr

// CHECK-LABEL: sil [noinline] {{.*}}@{{.*}}testCastAnyToEveryType{{.*}}
// CHECK:         unconditional_checked_cast_addr

// CHECK-LABEL: sil [noinline] {{.*}}@{{.*}}testCastAnyToNonClassType
// CHECK:         unconditional_checked_cast_addr

// CHECK-LABEL: sil [noinline] {{.*}}@{{.*}}testCastEveryToAnyClass{{.*}}
// CHECK:         unconditional_checked_cast_addr

// CHECK-LABEL: sil [noinline] {{.*}}@{{.*}}testCastEveryToClassObject{{.*}}
// CHECK:         unconditional_checked_cast_addr

// CHECK-LABEL: sil [noinline] {{.*}}@{{.*}}testCastEveryToAnyType{{.*}}
// CHECK:         unconditional_checked_cast_addr

// CHECK-LABEL: sil [noinline] {{.*}}@{{.*}}testCastEveryToEvery2Type{{.*}}
// CHECK:         unconditional_checked_cast_addr

// CHECK-LABEL: sil [noinline] {{.*}}@{{.*}}testCastEveryToNonClassType
// CHECK:         unconditional_checked_cast_addr

// CHECK-LABEL: sil [noinline] {{.*}}@{{.*}}testCastPProtocolToPType
// CHECK: %0 = enum $Optional{{.*}}, #Optional.none!enumelt
// CHECK-NEXT: return %0

// CHECK-LABEL: sil [noinline] {{.*}}@{{.*}}testCastObjCPProtocolTo{{.*}}PType
// CHECK: %0 = enum $Optional{{.*}}, #Optional.none!enumelt
// CHECK-NEXT: return %0

// CHECK-LABEL: sil [noinline] {{.*}}@{{.*}}testCastProtocolComposition{{.*}}Type
// CHECK: %0 = enum $Optional{{.*}}, #Optional.none!enumelt
// CHECK-NEXT: return %0

// Check that compiler understands that this cast always succeeds.
// Since it is can be statically proven that NSString is bridgeable to String,
// _forceBridgeFromObjectiveC from String should be invoked instead of
// a more general, but less effective swift_bridgeNonVerbatimFromObjectiveC, which
// also performs conformance checks at runtime.
@inline(never)
public func testBridgedCastFromObjCtoSwift(_ ns: NSString) -> String {
  return ns as String
}

// Check that compiler understands that this cast always succeeds

// CHECK-LABEL: sil [noinline] @$s17cast_folding_objc30testBridgedCastFromSwiftToObjCySo8NSStringCSSF
// CHECK-NOT: {{ cast}}
// CHECK: function_ref @$sSS10FoundationE19_bridgeToObjectiveC{{[_0-9a-zA-Z]*}}F
// CHECK: apply
// CHECK: return
@inline(never)
public func testBridgedCastFromSwiftToObjC(_ s: String) -> NSString {
  return s as NSString
}

public class MyString: NSString {}

// Check that the cast-optimizer bails out on a conditional downcast to a subclass of a
// bridged ObjC class.
// CHECK-LABEL: sil [noinline] @{{.*}}testConditionalBridgedCastFromSwiftToNSObjectDerivedClass{{.*}}
// CHECK: bb0([[ARG:%.*]] : $String):
// CHECK:   [[FUNC:%.*]] = function_ref @$sSS10FoundationE19_bridgeToObjectiveC{{[_0-9a-zA-Z]*}}F
// CHECK:   [[BRIDGED_VALUE:%.*]] = apply [[FUNC]]([[ARG]])
// CHECK-NOT: apply
// CHECK-NOT: unconditional_checked_cast
// CHECK: checked_cast_br [[BRIDGED_VALUE]] : $NSString to MyString, [[SUCC_BB:bb[0-9]+]], [[FAIL_BB:bb[0-9]+]]
//
// CHECK: [[SUCC_BB]]([[CAST_BRIDGED_VALUE:%.*]] : $MyString)
// CHECK:   [[SOME:%.*]] = enum $Optional<MyString>, #Optional.some!enumelt, [[CAST_BRIDGED_VALUE]] : $MyString
// CHECK:   br [[CONT_BB:bb[0-9]+]]([[SOME]] :
//
// CHECK: [[FAIL_BB]]:
// CHECK:   strong_release [[BRIDGED_VALUE]]
// CHECK:   [[NONE:%.*]] = enum $Optional<MyString>, #Optional.none!enumelt
// CHECK:   br [[CONT_BB]]([[NONE]] :
//
// CHECK: [[CONT_BB:bb[0-9]+]]([[RESULT:%.*]] :
// CHECK:   return [[RESULT]]
// CHECK: } // end sil function '${{.*}}testConditionalBridgedCastFromSwiftToNSObjectDerivedClass{{.*}}'
@inline(never)
public func testConditionalBridgedCastFromSwiftToNSObjectDerivedClass(_ s: String) -> MyString? {
  return s as? MyString
}

// Check that the cast-optimizer does not bail out on an unconditional downcast to a subclass of a
// bridged ObjC class.
// CHECK-LABEL: sil [noinline] @{{.*}}testForcedBridgedCastFromSwiftToNSObjectDerivedClass{{.*}}
// CHECK: function_ref @$sSS10FoundationE19_bridgeToObjectiveC{{[_0-9a-zA-Z]*}}F
// CHECK: apply
// CHECK-NOT: apply
// CHECK-NOT: checked_cast_br
// CHECK: unconditional_checked_cast
// CHECK-NOT: apply
// CHECK-NOT: unconditional
// CHECK-NOT: checked_cast
// CHECK: return
@inline(never)
public func testForcedBridgedCastFromSwiftToNSObjectDerivedClass(_ s: String) -> MyString {
    return s as! MyString
}

// rdar://problem/51078136
func foo(x: CFMutableDictionary) -> [AnyHashable:AnyObject]? {
  return x as? [AnyHashable:AnyObject]
}
