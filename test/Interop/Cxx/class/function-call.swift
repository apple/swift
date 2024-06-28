// RUN: %target-swiftxx-frontend -I %S/Inputs -emit-sil %s | %FileCheck --dump-input-filter=all %s

// REQUIRES: OS=macosx || OS=linux-android

import Closure

// CHECK: sil @$s4main14testNonTrivialyyF : $@convention(thin) () -> () {
// CHECK: %[[V0:.*]] = alloc_stack $NonTrivial
// CHECK: %[[V2:.*]] = function_ref @_ZN10NonTrivialC1Ev : $@convention(c) () -> @out NonTrivial
// CHECK: %[[V3:.*]] = apply %[[V2]](%[[V0]]) : $@convention(c) () -> @out NonTrivial
// CHECK: %[[V4:.*]] = function_ref @_Z5cfunc10NonTrivial : $@convention(c) (@in_cxx NonTrivial) -> ()
// CHECK: %[[V7:.*]] = apply %[[V4]](%[[V0]]) : $@convention(c) (@in_cxx NonTrivial) -> ()
// CHECK: destroy_addr %[[V0]] : $*NonTrivial
// CHECK: dealloc_stack %[[V0]] : $*NonTrivial

public func testNonTrivial() {
  cfunc(NonTrivial());
}

// CHECK: sil @$s4main15testDestroyAddryyF : $@convention(thin) () -> () {
// CHECK: %[[V0:.*]] = alloc_stack [lexical] [var_decl] $NonTrivial, var, name "a"
// CHECK-NEXT: %[[V1:.*]] = alloc_stack $NonTrivial
// CHECK: %[[V2:.*]] = function_ref @_ZN10NonTrivialC1Ev : $@convention(c) () -> @out NonTrivial
// CHECK-NEXT: apply %[[V2]](%[[V1]]) : $@convention(c) () -> @out NonTrivial
// CHECK-NEXT: %[[V4:.*]] = integer_literal $Builtin.Int32, 1
// CHECK-NEXT: %[[V5:.*]] = struct $Int32 (%[[V4]] : $Builtin.Int32)
// CHECK-NEXT: %[[V6:.*]] = alloc_stack $NonTrivial
// CHECK: %[[V7:.*]] = function_ref @_ZN10NonTrivialC1Ev : $@convention(c) () -> @out NonTrivial
// CHECK-NEXT: apply %[[V7]](%[[V6]]) : $@convention(c) () -> @out NonTrivial
// CHECK: %[[V16:.*]] = function_ref @_Z6cfunc310NonTrivialiS_ : $@convention(c) (@in_cxx NonTrivial, Int32, @in_cxx NonTrivial) -> @out NonTrivial
// CHECK-NEXT: apply %[[V16]](%[[V0]], %[[V1]], %[[V5]], %[[V6]]) : $@convention(c) (@in_cxx NonTrivial, Int32, @in_cxx NonTrivial) -> @out NonTrivial
// CHECK-NEXT: destroy_addr %[[V6]] : $*NonTrivial
// CHECK-NEXT: destroy_addr %[[V1]] : $*NonTrivial
// CHECK-NEXT: dealloc_stack %[[V6]] : $*NonTrivial
// CHECK-NEXT: dealloc_stack %[[V1]] : $*NonTrivial
// CHECK-NEXT: destroy_addr %[[V0]] : $*NonTrivial
// CHECK-NEXT: dealloc_stack %[[V0]] : $*NonTrivial
// CHECK-NEXT: %[[V17:.*]] = tuple ()
// CHECK-NEXT: return %[[V17]] : $()

// Check that destroy_addr instructions are emitted in the expected order.
public func testDestroyAddr() {
  var a = cfunc3(NonTrivial(), 1, NonTrivial())
}

// CHECK: sil @$s4main29testNonTrivialFunctionPointeryyF : $@convention(thin) () -> () {
// CHECK: %[[V0:.*]] = function_ref @_Z8getFnPtrv : $@convention(c) () -> @convention(c) (@in_cxx NonTrivial) -> ()
// CHECK: %[[V1:.*]] = apply %[[V0]]() : $@convention(c) () -> @convention(c) (@in_cxx NonTrivial) -> ()
// CHECK: %[[V3:.*]] = alloc_stack $NonTrivial
// CHECK: %[[V7:.*]] = function_ref @_ZN10NonTrivialC1Ev : $@convention(c) () -> @out NonTrivial
// CHECK: apply %[[V7]](%[[V3]]) : $@convention(c) () -> @out NonTrivial
// CHECK: apply %[[V1]](%[[V3]]) : $@convention(c) (@in_cxx NonTrivial) -> ()
// CHECK: destroy_addr %[[V3]] : $*NonTrivial
// CHECK: dealloc_stack %[[V3]] : $*NonTrivial

public func testNonTrivialFunctionPointer() {
  let f = getFnPtr()
  f(NonTrivial())
}
