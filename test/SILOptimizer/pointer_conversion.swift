// RUN: %target-swift-frontend -parse-stdlib -parse-as-library -emit-sil -O %s | %FileCheck %s
// REQUIRES: optimized_stdlib,swift_stdlib_no_asserts
// REQUIRES: swift_in_compiler

import Swift

// Opaque, unoptimizable functions to call.
@_silgen_name("takesConstRawPointer")
func takesConstRawPointer(_ x: UnsafeRawPointer)
@_silgen_name("takesOptConstRawPointer")
func takesOptConstRawPointer(_ x: UnsafeRawPointer?)
@_silgen_name("takesMutableRawPointer")
func takesMutableRawPointer(_ x: UnsafeMutableRawPointer)
@_silgen_name("takesOptMutableRawPointer")
func takesOptMutableRawPointer(_ x: UnsafeMutableRawPointer?)

// Opaque function for generating values
@_silgen_name("get")
func get<T>() -> T

// The purpose of these tests is to make sure the storage is never released
// before the call to the opaque function.

// CHECK-LABEL: sil @$s18pointer_conversion9testArrayyyF
public func testArray() {
  let array: [Int] = get()
  takesConstRawPointer(array)
  // CHECK: [[POINTER:%.+]] = struct $UnsafeRawPointer (
  // CHECK-NEXT: [[DEP_POINTER:%.+]] = mark_dependence [[POINTER]] : $UnsafeRawPointer on {{.*}} : $__ContiguousArrayStorageBase
  // CHECK: [[FN:%.+]] = function_ref @takesConstRawPointer
  // CHECK-NEXT: apply [[FN]]([[DEP_POINTER]])
  // CHECK-NOT: release
  // CHECK-NOT: {{^bb[0-9]+:}}
  // CHECK: strong_release {{%.+}} : ${{Builtin[.]BridgeObject|__ContiguousArrayStorageBase}}
  // CHECK-NEXT: [[EMPTY:%.+]] = tuple ()
  // CHECK-NEXT: return [[EMPTY]]
}

// CHECK-LABEL: sil @$s18pointer_conversion19testArrayToOptionalyyF
public func testArrayToOptional() {
  let array: [Int] = get()
  takesOptConstRawPointer(array)
  // CHECK: [[POINTER:%.+]] = struct $UnsafeRawPointer (
  // CHECK-NEXT: [[DEP_POINTER:%.+]] = mark_dependence [[POINTER]] : $UnsafeRawPointer on {{.*}} : $__ContiguousArrayStorageBase
  // CHECK-NEXT: [[OPT_POINTER:%.+]] = enum $Optional<UnsafeRawPointer>, #Optional.some!enumelt, [[DEP_POINTER]]
  // CHECK: [[FN:%.+]] = function_ref @takesOptConstRawPointer
  // CHECK-NEXT: apply [[FN]]([[OPT_POINTER]])
  // CHECK-NOT: release
  // CHECK-NOT: {{^bb[0-9]+:}}
  // CHECK: strong_release {{%.+}} : ${{Builtin[.]BridgeObject|__ContiguousArrayStorageBase}}
  // CHECK-NEXT: [[EMPTY:%.+]] = tuple ()
  // CHECK-NEXT: return [[EMPTY]]
}

// CHECK-LABEL: sil @$s18pointer_conversion16testMutableArrayyyF
public func testMutableArray() {
  var array: [Int] = get()
  takesMutableRawPointer(&array)
  // CHECK: [[POINTER:%.+]] = struct $UnsafeMutableRawPointer (
  // CHECK-NEXT: [[DEP_POINTER:%.+]] = mark_dependence [[POINTER]] : $UnsafeMutableRawPointer on {{.*}} : $__ContiguousArrayStorageBase
  // CHECK: [[FN:%.+]] = function_ref @takesMutableRawPointer
  // CHECK: apply [[FN]]([[DEP_POINTER]])
  // CHECK-NOT: {{^bb[0-9]+:}}
  // CHECK: strong_release {{%.+}} : ${{Builtin[.]BridgeObject|__ContiguousArrayStorageBase}}
  // CHECK: dealloc_stack {{%.+}} : $*Array<Int>
  // CHECK-NEXT: [[EMPTY:%.+]] = tuple ()
  // CHECK-NEXT: return [[EMPTY]]
}

// CHECK-LABEL: sil @$s18pointer_conversion26testMutableArrayToOptionalyyF
public func testMutableArrayToOptional() {
  var array: [Int] = get()
  takesOptMutableRawPointer(&array)
  // CHECK: [[POINTER:%.+]] = struct $UnsafeMutableRawPointer (
  // CHECK-NEXT: [[DEP_POINTER:%.+]] = mark_dependence [[POINTER]] : $UnsafeMutableRawPointer on {{.*}} : $__ContiguousArrayStorageBase
  // CHECK-NEXT: [[OPT_POINTER:%.+]] = enum $Optional<UnsafeMutableRawPointer>, #Optional.some!enumelt, [[DEP_POINTER]]
  // CHECK: [[FN:%.+]] = function_ref @takesOptMutableRawPointer
  // CHECK: apply [[FN]]([[OPT_POINTER]])
  // CHECK-NOT: {{^bb[0-9]+:}}
  // CHECK: strong_release {{%.+}} : ${{Builtin[.]BridgeObject|__ContiguousArrayStorageBase}}
  // CHECK: dealloc_stack {{%.+}} : $*Array<Int>
  // CHECK-NEXT: [[EMPTY:%.+]] = tuple ()
  // CHECK-NEXT: return [[EMPTY]]
}

// CHECK-LABEL: sil @$s18pointer_conversion21arrayLiteralPromotionyyF
public func arrayLiteralPromotion() {
  takesConstRawPointer([-41,-42,-43,-44])
  
  // Outline the array literal.
  // CHECK: [[ARR:%.+]] = global_value
  // CHECK: [[CAST:%.+]] = upcast [[ARR]]
  // CHECK: [[TADDR:%.+]] = ref_tail_addr [[CAST]]
  // CHECK: [[RAWPTR:%.+]] = address_to_pointer [[TADDR]]
  // CHECK: [[UNSAFEPTR:%.+]] = struct $UnsafeRawPointer ([[RAWPTR]]
  // CHECK: [[PTR:%.+]] = mark_dependence [[UNSAFEPTR]]
  // CHECK: [[FN:%.+]] = function_ref @takesConstRawPointer
  // CHECK: apply [[FN]]([[PTR]])
}

// Test sil verification at -O with bind_memory and rebind_memory
// where the rebind is in a defer block.
//
// CHECK-LABEL: sil @$s18pointer_conversion21testWithMemoryRebound6rawPtr2to8capacity_q_Bp_xmSiq_SPyxGKXEtKr0_lF
// CHECK: [[BIND:%.*]] = bind_memory %1 : $Builtin.RawPointer, %{{.*}} : $Builtin.Word to $*T
// CHECK: rebind_memory %1 : $Builtin.RawPointer to [[BIND]] : $Builtin.Word
// CHECK-LABEL: } // end sil function '$s18pointer_conversion21testWithMemoryRebound6rawPtr2to8capacity_q_Bp_xmSiq_SPyxGKXEtKr0_lF'
public func testWithMemoryRebound<T, Result>(
  rawPtr: Builtin.RawPointer,
  to type: T.Type,
  capacity count: Int,
  _ body: (_ pointer: UnsafePointer<T>) throws -> Result
) rethrows -> Result {
  let binding =
    Builtin.bindMemory(rawPtr, count._builtinWordValue, T.self)
  defer {
    Builtin.rebindMemory(rawPtr, binding)
  }
  return try body(.init(rawPtr))
}
