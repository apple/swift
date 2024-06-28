// RUN: %target-swiftxx-frontend -I %S/Inputs -emit-sil %s | %FileCheck --dump-input-filter=all %s

// REQUIRES: OS=macosx || OS=linux-android

import Closure

// CHECK: sil @$s4main18testClosureToBlockyyF : $@convention(thin) () -> () {
// CHECK: %[[V0:.*]] = function_ref @$s4main18testClosureToBlockyyFySo10NonTrivialVcfU_ : $@convention(thin) (@in_guaranteed NonTrivial) -> ()
// CHECK: %[[V1:.*]] = thin_to_thick_function %[[V0]] : $@convention(thin) (@in_guaranteed NonTrivial) -> () to $@callee_guaranteed (@in_guaranteed NonTrivial) -> ()
// CHECK: %[[V2:.*]] = alloc_stack $@block_storage @callee_guaranteed (@in_guaranteed NonTrivial) -> ()
// CHECK: %[[V3:.*]] = project_block_storage %[[V2]] : $*@block_storage @callee_guaranteed (@in_guaranteed NonTrivial) -> ()
// CHECK: store %[[V1]] to %[[V3]] : $*@callee_guaranteed (@in_guaranteed NonTrivial) -> ()
// CHECK: %[[V7:.*]] = function_ref @$sSo10NonTrivialVIegn_ABIeyBX_TR : $@convention(c) (@inout_aliasable @block_storage @callee_guaranteed (@in_guaranteed NonTrivial) -> (), @in_cxx NonTrivial) -> ()
// CHECK: %[[V6:.*]] = init_block_storage_header %[[V2]] : $*@block_storage @callee_guaranteed (@in_guaranteed NonTrivial) -> (), invoke %[[V7]] : $@convention(c) (@inout_aliasable @block_storage @callee_guaranteed (@in_guaranteed NonTrivial) -> (), @in_cxx NonTrivial) -> (), type $@convention(block) (@in_cxx NonTrivial) -> ()
// CHECK: %[[V8:.*]] = copy_block %[[V6]] : $@convention(block) (@in_cxx NonTrivial) -> ()
// CHECK: dealloc_stack %[[V2]] : $*@block_storage @callee_guaranteed (@in_guaranteed NonTrivial) -> ()
// CHECK: %[[V11:.*]] = function_ref @_Z5cfuncU13block_pointerFv10NonTrivialE : $@convention(c) (@convention(block) (@in_cxx NonTrivial) -> ()) -> ()
// CHECK: apply %[[V11]](%[[V8]]) : $@convention(c) (@convention(block) (@in_cxx NonTrivial) -> ()) -> ()
// CHECK: strong_release %[[V8]] : $@convention(block) (@in_cxx NonTrivial) -> ()
// CHECK: %[[V12:.*]] = tuple ()
// CHECK: return %[[V12]] : $()

// CHECK: sil shared [transparent] [reabstraction_thunk] @$sSo10NonTrivialVIegn_ABIeyBX_TR : $@convention(c) (@inout_aliasable @block_storage @callee_guaranteed (@in_guaranteed NonTrivial) -> (), @in_cxx NonTrivial) -> () {
// CHECK: bb0(%[[V0:.*]] : $*@block_storage @callee_guaranteed (@in_guaranteed NonTrivial) -> (), %[[V1:.*]] : $*NonTrivial):
// CHECK: %[[V2:.*]] = project_block_storage %[[V0]] : $*@block_storage @callee_guaranteed (@in_guaranteed NonTrivial) -> ()
// CHECK: %[[V3:.*]] = load %[[V2]] : $*@callee_guaranteed (@in_guaranteed NonTrivial) -> ()
// CHECK: strong_retain %[[V3]] : $@callee_guaranteed (@in_guaranteed NonTrivial) -> ()
// CHECK: apply %[[V3]](%[[V1]]) : $@callee_guaranteed (@in_guaranteed NonTrivial) -> ()
// CHECK: %[[V6:.*]] = tuple ()
// CHECK: strong_release %[[V3]] : $@callee_guaranteed (@in_guaranteed NonTrivial) -> ()
// CHECK: return %[[V6]] : $()

// NonTrivial is destroyed by the caller.
public func testClosureToBlock() {
  cfunc({NonTrivial in})
}

// CHECK: sil @$s4main20testClosureToFuncPtryyF : $@convention(thin) () -> () {
// CHECK: %[[V0:.*]] = function_ref @$s4main20testClosureToFuncPtryyFySo10NonTrivialVcfU_To : $@convention(c) (@in_cxx NonTrivial) -> ()
// CHECK: %[[V1:.*]] = function_ref @_Z6cfunc2PFv10NonTrivialE : $@convention(c) (@convention(c) (@in_cxx NonTrivial) -> ()) -> ()
// CHECK: apply %[[V1]](%[[V0]]) : $@convention(c) (@convention(c) (@in_cxx NonTrivial) -> ()) -> ()
// CHECK: %[[V3:.*]] = tuple ()
// CHECK: return %[[V3]] : $()

// CHECK: sil private [thunk] @$s4main20testClosureToFuncPtryyFySo10NonTrivialVcfU_To : $@convention(c) (@in_cxx NonTrivial) -> () {
// CHECK: bb0(%[[V0:.*]] : $*NonTrivial):
// CHECK: %[[V1:.*]] = alloc_stack $NonTrivial
// CHECK: copy_addr %[[V0]] to [init] %[[V1]] : $*NonTrivial
// CHECK: %[[V3:.*]] = function_ref @$s4main20testClosureToFuncPtryyFySo10NonTrivialVcfU_ : $@convention(thin) (@in_guaranteed NonTrivial) -> ()
// CHECK: %[[V4:.*]] = apply %[[V3]](%[[V1]]) : $@convention(thin) (@in_guaranteed NonTrivial) -> ()
// CHECK: destroy_addr %[[V1]] : $*NonTrivial
// CHECK: dealloc_stack %[[V1]] : $*NonTrivial
// CHECK: return %[[V4]] : $()

public func testClosureToFuncPtr() {
  cfunc2({N in})
}

// CHECK: sil @$s4main13returnFuncPtrySo10NonTrivialVcyF : $@convention(thin) () -> @owned @callee_guaranteed (@in_guaranteed NonTrivial) -> () {
// CHECK: %[[V0:.*]] = function_ref @_Z8getFnPtrv : $@convention(c) () -> @convention(c) (@in_cxx NonTrivial) -> ()
// CHECK: %[[V1:.*]] = apply %[[V0]]() : $@convention(c) () -> @convention(c) (@in_cxx NonTrivial) -> ()
// CHECK: %[[V2:.*]] = function_ref @$sSo10NonTrivialVIetCX_ABIegn_TR : $@convention(thin) (@in_guaranteed NonTrivial, @convention(c) (@in_cxx NonTrivial) -> ()) -> ()
// CHECK: %[[V3:.*]] = partial_apply [callee_guaranteed] %[[V2]](%[[V1]]) : $@convention(thin) (@in_guaranteed NonTrivial, @convention(c) (@in_cxx NonTrivial) -> ()) -> ()
// CHECK: return %[[V3]] : $@callee_guaranteed (@in_guaranteed NonTrivial) -> ()

// CHECK: sil shared [transparent] [reabstraction_thunk] @$sSo10NonTrivialVIetCX_ABIegn_TR : $@convention(thin) (@in_guaranteed NonTrivial, @convention(c) (@in_cxx NonTrivial) -> ()) -> () {
// CHECK: bb0(%[[V0:.*]] : $*NonTrivial, %[[V1:.*]] : $@convention(c) (@in_cxx NonTrivial) -> ()):
// CHECK: %[[V2:.*]] = alloc_stack $NonTrivial
// CHECK: copy_addr %[[V0]] to [init] %[[V2]] : $*NonTrivial
// CHECK: apply %[[V1]](%[[V2]]) : $@convention(c) (@in_cxx NonTrivial) -> ()
// CHECK: destroy_addr %[[V2]] : $*NonTrivial
// CHECK: %[[V6:.*]] = tuple ()
// CHECK: dealloc_stack %[[V2]] : $*NonTrivial
// CHECK: return %[[V6]] : $()

public func returnFuncPtr() -> (NonTrivial) -> () {
  return getFnPtr()
}

// CHECK: sil private [thunk] @$s4main36testClosureToFuncPtrReturnNonTrivialyyFSo0hI0VycfU_To : $@convention(c) () -> @out NonTrivial {
// CHECK: bb0(%[[V0:.*]] : $*NonTrivial):
// CHECK: %[[V1:.*]] = function_ref @$s4main36testClosureToFuncPtrReturnNonTrivialyyFSo0hI0VycfU_ : $@convention(thin) () -> @out NonTrivial
// CHECK: %[[V2:.*]] = apply %[[V1]](%[[V0]]) : $@convention(thin) () -> @out NonTrivial
// CHECK: return %[[V2]] : $()

public func testClosureToFuncPtrReturnNonTrivial() {
  cfuncReturnNonTrivial2({() -> NonTrivial in return NonTrivial()});
}
