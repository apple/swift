// RUN: %empty-directory(%t)
// RUN: %target-swift-frontend-emit-module -emit-module-path %t/FakeDistributedActorSystems.swiftmodule -module-name FakeDistributedActorSystems -disable-availability-checking %S/../Inputs/FakeDistributedActorSystems.swift
// RUN: %target-swift-frontend -module-name default_deinit -primary-file %s -emit-sil -disable-availability-checking -I %t | %FileCheck %s --enable-var-scope --color
// REQUIRES: concurrency
// REQUIRES: distributed

// FIXME(distributed): test fails on optimized build: OSS - Swift (Tools Opt+No Assert,  Stdlib Opt+DebInfo, Test Simulator) - macOS
// REQUIRES: radar97074020

// This test checks that we resign the identity for local deallocations,
// destroy only the correct stored properties whether remote or local, and also
// destroy the executor. It checks that remote actor proxies are not isolated.
// Additionally, we add basic coverage for a non-distributed actor's deinit.

import Distributed
import FakeDistributedActorSystems

typealias DefaultDistributedActorSystem = FakeActorSystem

final class SomeClass: Sendable {}

@inline(never)
private nonisolated func doIt() {}

// MARK: - Distributed actor with nonisolated deinit

distributed actor MyDistActor {
  let localOnlyField: SomeClass

  init(system: FakeActorSystem) {
    self.actorSystem = system
    self.localOnlyField = SomeClass()
  }
}

// MARK: Destroying deinit

// CHECK-LABEL: // MyDistActor.deinit
// CHECK-NEXT: sil hidden{{.*}} @$s14default_deinit11MyDistActorCfd : $@convention(method) (@guaranteed MyDistActor) -> @owned Builtin.NativeObject {
// CHECK: bb0([[SELF:%[0-9]+]] : $MyDistActor):

// Resign the ID by calling actorSystem.resignID, before we destroy properties:
// CHECK:  [[ID_REF:%[0-9]+]] = ref_element_addr [[SELF]] : $MyDistActor, #MyDistActor.id
// CHECK:  [[SYS_REF:%[0-9]+]] = ref_element_addr [[SELF]] : $MyDistActor, #MyDistActor.actorSystem
// CHECK:  [[ID_LOAD:%[0-9]+]] = load [[ID_REF]] : $*ActorAddress
// CHECK:  [[SYS_LOAD:%[0-9]+]] = load [[SYS_REF]] : $*FakeActorSystem
// CHECK:  [[FN_REF:%[0-9]+]] = function_ref @$s27FakeDistributedActorSystems0aC6SystemV8resignIDyyAA0C7AddressVF : $@convention(method) (@guaranteed ActorAddress, @guaranteed FakeActorSystem) -> ()
// CHECK:  [[DROP:%[0-9]+]] = apply [[FN_REF]]([[ID_LOAD]], [[SYS_LOAD]]) : $@convention(method) (@guaranteed ActorAddress, @guaranteed FakeActorSystem) -> ()
// CHECK:  [[DROP:%[0-9]+]] = tuple ()

// Destroy implicit Distributed Actor field: id
// CHECK:  [[REF:%[0-9]+]] = ref_element_addr [[SELF]] : $MyDistActor, #MyDistActor.id
// CHECK:  [[ACCESS:%[0-9]+]] = begin_access [deinit] [static] [[REF]] : $*ActorAddress
// CHECK:  destroy_addr [[ACCESS]] : $*ActorAddress
// CHECK:  end_access [[ACCESS]] : $*ActorAddress

// Destroy implicit Distributed Actor field: actorSystem
// CHECK:  [[REF:%[0-9]+]] = ref_element_addr [[SELF]] : $MyDistActor, #MyDistActor.actorSystem
// CHECK:  [[ACCESS:%[0-9]+]] = begin_access [deinit] [static] [[REF]] : $*FakeActorSystem
// CHECK:  destroy_addr [[ACCESS]] : $*FakeActorSystem
// CHECK:  end_access [[ACCESS]] : $*FakeActorSystem

// Destroy local fields:
// CHECK:  [[REF:%[0-9]+]] = ref_element_addr [[SELF]] : $MyDistActor, #MyDistActor.localOnlyField
// CHECK:  [[ACCESS:%[0-9]+]] = begin_access [deinit] [static] [[REF]] : $*SomeClass
// CHECK:  destroy_addr [[ACCESS]] : $*SomeClass
// CHECK:  end_access [[ACCESS]] : $*SomeClass

// CHECK:  [[BUILTIN:%[0-9]+]] = builtin "destroyDefaultActor"([[SELF]] : $MyDistActor) : $()
// CHECK:  [[CAST:%[0-9]+]] = unchecked_ref_cast [[SELF]] : $MyDistActor to $Builtin.NativeObject
// CHECK:  return [[CAST]] : $Builtin.NativeObject

// CHECK: } // end sil function '$s14default_deinit11MyDistActorCfd'

// MARK: Isolated deallocating deinit

// ===== -----------------------------------------------------------------------
// CHECK-NOT: MyDistActor.__isolated_deallocating_deinit
// CHECK-NOT: @$s14default_deinit11MyDistActorCfZ

// MARK: Deallocating deinit

// ===== -----------------------------------------------------------------------
// CHECK-LABEL: MyDistActor.__deallocating_deinit
// CHECK-NEXT: sil hidden @$s14default_deinit11MyDistActorCfD : $@convention(method) (@owned MyDistActor) -> () {
// CHECK: bb0([[SELF:%[0-9]+]] : $MyDistActor):
// CHECK:  [[EXI_SELF:%[0-9]+]] = init_existential_ref [[SELF]] : $MyDistActor : $MyDistActor, $AnyObject
// CHECK:  [[IS_REMOTE_FN:%[0-9]+]] = function_ref @swift_distributed_actor_is_remote : $@convention(thin) (@guaranteed AnyObject) -> Bool
// CHECK:  [[IS_REMOTE:%[0-9]+]] = apply [[IS_REMOTE_FN]]([[EXI_SELF]])
// CHECK:  [[RAW_BOOL:%[0-9]+]] = struct_extract [[IS_REMOTE]] : $Bool, #Bool._value
// CHECK:  cond_br [[RAW_BOOL]], [[REMOTE_ACTOR_DEINIT_BB:bb[0-9]+]], [[LOCAL_ACTOR_DEINIT_BB:bb[0-9]+]]

// ===== -----------------------------------------------------------------------
// CHECK: // localActorDeinitBB
// CHECK: [[LOCAL_ACTOR_DEINIT_BB]]:
// CHECK: [[DESTROYER_REF:%[0-9]+]] = function_ref @$s14default_deinit11MyDistActorCfd : $@convention(method) (@guaranteed MyDistActor) -> @owned Builtin.NativeObject
// CHECK: [[UNTYPED_DESTOY_RESULT:%[0-9]+]] = apply [[DESTROYER_REF]]([[SELF]]) : $@convention(method) (@guaranteed MyDistActor) -> @owned Builtin.NativeObject
// CHECK: [[TYPED_DESTOY_RESULT:%[0-9]+]] = unchecked_ref_cast [[UNTYPED_DESTOY_RESULT]] : $Builtin.NativeObject to $MyDistActor
// CHECK: dealloc_ref [[TYPED_DESTOY_RESULT]] : $MyDistActor
// CHECK:  br [[FINISH_DEINIT_BB:bb[0-9]+]]

// ===== -----------------------------------------------------------------------
// CHECK: // finishDeinitBB
// CHECK: [[FINISH_DEINIT_BB]]:
// CHECK:  [[RESULT:%[0-9]+]] = tuple ()
// CHECK:  return [[RESULT]] : $()

// ===== -----------------------------------------------------------------------
// CHECK: // remoteActorDeinitBB
// CHECK: [[REMOTE_ACTOR_DEINIT_BB]]:

// Even just the remote deinit branch must destroy the ID
// CHECK:  [[REF:%[0-9]+]] = ref_element_addr [[SELF]] : $MyDistActor, #MyDistActor.id
// CHECK:  [[ACCESS:%[0-9]+]] = begin_access [deinit] [static] [[REF]] : $*ActorAddress
// CHECK:  destroy_addr [[ACCESS]] : $*ActorAddress
// CHECK:  end_access [[ACCESS]] : $*ActorAddress

// As well as the actor system:
// CHECK:  [[REF:%[0-9]+]] = ref_element_addr [[SELF]] : $MyDistActor, #MyDistActor.actorSystem
// CHECK:  [[ACCESS:%[0-9]+]] = begin_access [deinit] [static] [[REF]] : $*FakeActorSystem
// CHECK:  destroy_addr [[ACCESS]] : $*FakeActorSystem
// CHECK:  end_access [[ACCESS]] : $*FakeActorSystem

// Destroy default actor implementation
// CHECK:  [[BUILTIN:%[0-9]+]] = builtin "destroyDefaultActor"([[SELF]] : $MyDistActor) : $()

// And deallocate the proxy
// CHECK:  dealloc_ref [[SELF]] : $MyDistActor

// Return
// CHECK:  br [[FINISH_DEINIT_BB]]

// CHECK: } // end sil function '$s14default_deinit11MyDistActorCfD'

// ===== -----------------------------------------------------------------------

// MARK: - Distributed actor with isolated deinit

distributed actor MyDistActorIsolated {
  let localOnlyField: SomeClass

  init(system: FakeActorSystem) {
    self.actorSystem = system
    self.localOnlyField = SomeClass()
  }
    
  deinit {
    doIt()
  }
}

// MARK: Destroying deinit

// CHECK-LABEL: // MyDistActorIsolated.deinit
// CHECK-NEXT: sil hidden{{.*}} @$s14default_deinit19MyDistActorIsolatedCfd : $@convention(method) (@guaranteed MyDistActorIsolated) -> @owned Builtin.NativeObject {
// CHECK: bb0([[SELF:%[0-9]+]] : $MyDistActorIsolated):

// Resign the ID by calling actorSystem.resignID, before we destroy properties:
// CHECK:  [[ID_REF:%[0-9]+]] = ref_element_addr [[SELF]] : $MyDistActorIsolated, #MyDistActorIsolated.id
// CHECK:  [[SYS_REF:%[0-9]+]] = ref_element_addr [[SELF]] : $MyDistActorIsolated, #MyDistActorIsolated.actorSystem
// CHECK:  [[ID_LOAD:%[0-9]+]] = load [[ID_REF]] : $*ActorAddress
// CHECK:  [[SYS_LOAD:%[0-9]+]] = load [[SYS_REF]] : $*FakeActorSystem
// CHECK:  [[FN_REF:%[0-9]+]] = function_ref @$s27FakeDistributedActorSystems0aC6SystemV8resignIDyyAA0C7AddressVF : $@convention(method) (@guaranteed ActorAddress, @guaranteed FakeActorSystem) -> ()
// CHECK:  [[DROP:%[0-9]+]] = apply [[FN_REF]]([[ID_LOAD]], [[SYS_LOAD]]) : $@convention(method) (@guaranteed ActorAddress, @guaranteed FakeActorSystem) -> ()
// CHECK:  [[DROP:%[0-9]+]] = tuple ()

// Destroy implicit Distributed Actor field: id
// CHECK:  [[REF:%[0-9]+]] = ref_element_addr [[SELF]] : $MyDistActorIsolated, #MyDistActorIsolated.id
// CHECK:  [[ACCESS:%[0-9]+]] = begin_access [deinit] [static] [[REF]] : $*ActorAddress
// CHECK:  destroy_addr [[ACCESS]] : $*ActorAddress
// CHECK:  end_access [[ACCESS]] : $*ActorAddress

// Destroy implicit Distributed Actor field: actorSystem
// CHECK:  [[REF:%[0-9]+]] = ref_element_addr [[SELF]] : $MyDistActorIsolated, #MyDistActorIsolated.actorSystem
// CHECK:  [[ACCESS:%[0-9]+]] = begin_access [deinit] [static] [[REF]] : $*FakeActorSystem
// CHECK:  destroy_addr [[ACCESS]] : $*FakeActorSystem
// CHECK:  end_access [[ACCESS]] : $*FakeActorSystem

// Destroy local fields:
// CHECK:  [[REF:%[0-9]+]] = ref_element_addr [[SELF]] : $MyDistActorIsolated, #MyDistActorIsolated.localOnlyField
// CHECK:  [[ACCESS:%[0-9]+]] = begin_access [deinit] [static] [[REF]] : $*SomeClass
// CHECK:  destroy_addr [[ACCESS]] : $*SomeClass
// CHECK:  end_access [[ACCESS]] : $*SomeClass

// CHECK:  [[BUILTIN:%[0-9]+]] = builtin "destroyDefaultActor"([[SELF]] : $MyDistActorIsolated) : $()
// CHECK:  [[CAST:%[0-9]+]] = unchecked_ref_cast [[SELF]] : $MyDistActorIsolated to $Builtin.NativeObject
// CHECK:  return [[CAST]] : $Builtin.NativeObject

// CHECK: } // end sil function '$s14default_deinit19MyDistActorIsolatedCfd'

// MARK: Isolated deallocating deinit

// ===== -----------------------------------------------------------------------
// CHECK-LABEL: MyDistActorIsolated.__isolated_deallocating_deinit
// CHECK-NEXT: sil hidden @$s14default_deinit19MyDistActorIsolatedCfZ : $@convention(thin) (@owned MyDistActorIsolated) -> () {
// CHECK: bb0([[SELF:%[0-9]+]] : $MyDistActorIsolated):
// CHECK: [[DESTROYER_REF:%[0-9]+]] = function_ref @$s14default_deinit19MyDistActorIsolatedCfd : $@convention(method) (@guaranteed MyDistActorIsolated) -> @owned Builtin.NativeObject
// CHECK: [[UNTYPED_DESTOY_RESULT:%[0-9]+]] = apply [[DESTROYER_REF]]([[SELF]]) : $@convention(method) (@guaranteed MyDistActorIsolated) -> @owned Builtin.NativeObject
// CHECK: [[TYPED_DESTOY_RESULT:%[0-9]+]] = unchecked_ref_cast [[UNTYPED_DESTOY_RESULT]] : $Builtin.NativeObject to $MyDistActorIsolated
// CHECK: dealloc_ref [[TYPED_DESTOY_RESULT]] : $MyDistActorIsolated
// CHECK:  [[RESULT:%[0-9]+]] = tuple ()
// CHECK:  return [[RESULT]] : $()
// CHECK: } // end sil function '$s14default_deinit19MyDistActorIsolatedCfZ'

// MARK: Deallocating deinit

// ===== -----------------------------------------------------------------------
// CHECK-LABEL: MyDistActorIsolated.__deallocating_deinit
// CHECK-NEXT: sil hidden @$s14default_deinit19MyDistActorIsolatedCfD : $@convention(method) (@owned MyDistActorIsolated) -> () {
// CHECK: bb0([[SELF:%[0-9]+]] : $MyDistActorIsolated):
// CHECK:  [[EXI_SELF:%[0-9]+]] = init_existential_ref [[SELF]] : $MyDistActorIsolated : $MyDistActorIsolated, $AnyObject
// CHECK:  [[IS_REMOTE_FN:%[0-9]+]] = function_ref @swift_distributed_actor_is_remote : $@convention(thin) (@guaranteed AnyObject) -> Bool
// CHECK:  [[IS_REMOTE:%[0-9]+]] = apply [[IS_REMOTE_FN]]([[EXI_SELF]])
// CHECK:  [[RAW_BOOL:%[0-9]+]] = struct_extract [[IS_REMOTE]] : $Bool, #Bool._value
// CHECK:  cond_br [[RAW_BOOL]], [[REMOTE_ACTOR_DEINIT_BB:bb[0-9]+]], [[LOCAL_ACTOR_DEINIT_BB:bb[0-9]+]]

// ===== -----------------------------------------------------------------------
// CHECK: // localActorDeinitBB
// CHECK: [[LOCAL_ACTOR_DEINIT_BB]]:
// CHECK: [[ISOLATED_REF:%[0-9]+]] = function_ref @$s14default_deinit19MyDistActorIsolatedCfZ : $@convention(thin) (@owned MyDistActorIsolated) -> ()
// CHECK: [[EXECUTOR:%[0-9]+]] = extract_executor [[SELF]] : $MyDistActorIsolated
// CHECK: [[PERFORM_REF:%[0-9]+]] = function_ref @swift_task_deinitOnExecutor : $@convention(thin) (@owned AnyObject, @convention(thin) (@owned AnyObject) -> (), Builtin.Executor) -> ()
// CHECK: [[SELF_AS_ANY_OBJECT:%[0-9]+]] = unchecked_bitwise_cast [[SELF]] : $MyDistActorIsolated to $AnyObject
// CHECK: [[ISOLATED_CASTED:%[0-9]+]] = convert_function [[ISOLATED_REF]] : $@convention(thin) (@owned MyDistActorIsolated) -> () to $@convention(thin) (@owned AnyObject) -> ()
// CHECK: [[DROP:%[0-9]+]] = apply [[PERFORM_REF]]([[SELF_AS_ANY_OBJECT]], [[ISOLATED_CASTED]], [[EXECUTOR]]) : $@convention(thin) (@owned AnyObject, @convention(thin) (@owned AnyObject) -> (), Builtin.Executor) -> ()
// CHECK:  br [[FINISH_DEINIT_BB:bb[0-9]+]]

// ===== -----------------------------------------------------------------------
// CHECK: // finishDeinitBB
// CHECK: [[FINISH_DEINIT_BB]]:
// CHECK:  [[RESULT:%[0-9]+]] = tuple ()
// CHECK:  return [[RESULT]] : $()

// ===== -----------------------------------------------------------------------
// CHECK: // remoteActorDeinitBB
// CHECK: [[REMOTE_ACTOR_DEINIT_BB]]:

// Even just the remote deinit branch must destroy the ID
// CHECK:  [[REF:%[0-9]+]] = ref_element_addr [[SELF]] : $MyDistActorIsolated, #MyDistActorIsolated.id
// CHECK:  [[ACCESS:%[0-9]+]] = begin_access [deinit] [static] [[REF]] : $*ActorAddress
// CHECK:  destroy_addr [[ACCESS]] : $*ActorAddress
// CHECK:  end_access [[ACCESS]] : $*ActorAddress

// As well as the actor system:
// CHECK:  [[REF:%[0-9]+]] = ref_element_addr [[SELF]] : $MyDistActorIsolated, #MyDistActorIsolated.actorSystem
// CHECK:  [[ACCESS:%[0-9]+]] = begin_access [deinit] [static] [[REF]] : $*FakeActorSystem
// CHECK:  destroy_addr [[ACCESS]] : $*FakeActorSystem
// CHECK:  end_access [[ACCESS]] : $*FakeActorSystem

// Destroy default actor implementation
// CHECK:  [[BUILTIN:%[0-9]+]] = builtin "destroyDefaultActor"([[SELF]] : $MyDistActorIsolated) : $()

// And deallocate the proxy
// CHECK:  dealloc_ref [[SELF]] : $MyDistActorIsolated

// Return
// CHECK:  br [[FINISH_DEINIT_BB]]

// CHECK: } // end sil function '$s14default_deinit19MyDistActorIsolatedCfD'

// ===== -----------------------------------------------------------------------

// MARK: - Local actor with nonisolated deinit

@available(macOS 12, *)
actor SimpleActor {
  let someField: SomeClass
  init() {
    self.someField = SomeClass()
  }
}

// MARK: Destroying deinit

// ===== -----------------------------------------------------------------------
// CHECK-LABEL: // SimpleActor.deinit
// CHECK-NEXT: sil hidden{{.*}} @$s14default_deinit11SimpleActorCfd : $@convention(method) (@guaranteed SimpleActor) -> @owned Builtin.NativeObject {
// CHECK: bb0([[SELF:%[0-9]+]] : $SimpleActor):
// CHECK:   [[REF:%[0-9]+]] = ref_element_addr [[SELF]] : $SimpleActor, #SimpleActor.someField
// CHECK:   [[ACCESS:%[0-9]+]] = begin_access [deinit] [static] [[REF]]
// CHECK:   destroy_addr [[ACCESS]] : $*SomeClass
// CHECK:   end_access [[ACCESS]]
// CHECK:   builtin "destroyDefaultActor"([[SELF]] : $SimpleActor)
// CHECK:   [[CAST:%[0-9]+]] = unchecked_ref_cast [[SELF]]
// CHECK:   return [[CAST]] : $Builtin.NativeObject
// CHECK: } // end sil function '$s14default_deinit11SimpleActorCfd'

// MARK: Isolated deallocating deinit

// ===== -----------------------------------------------------------------------
// CHECK-NOT: SimpleActor.__isolated_deallocating_deinit
// CHECK-NOT: @$s14default_deinit11SimpleActorCfZ

// MARK: Deallocating deinit

// ===== -----------------------------------------------------------------------
// CHECK-LABEL: // SimpleActor.__deallocating_deinit
// CHECK-NEXT: sil hidden{{.*}} @$s14default_deinit11SimpleActorCfD : $@convention(method) (@owned SimpleActor) -> () {
// CHECK: bb0([[SELF:%[0-9]+]] : $SimpleActor):
// CHECK: [[DESTROYER_REF:%[0-9]+]] = function_ref @$s14default_deinit11SimpleActorCfd : $@convention(method) (@guaranteed SimpleActor) -> @owned Builtin.NativeObject
// CHECK: [[UNTYPED_DESTOY_RESULT:%[0-9]+]] = apply [[DESTROYER_REF]]([[SELF]]) : $@convention(method) (@guaranteed SimpleActor) -> @owned Builtin.NativeObject
// CHECK: [[TYPED_DESTOY_RESULT:%[0-9]+]] = unchecked_ref_cast [[UNTYPED_DESTOY_RESULT]] : $Builtin.NativeObject to $SimpleActor
// CHECK: dealloc_ref [[TYPED_DESTOY_RESULT]] : $SimpleActor
// CHECK:  [[RESULT:%[0-9]+]] = tuple ()
// CHECK:  return [[RESULT]] : $()
// CHECK: } // end sil function '$s14default_deinit11SimpleActorCfD'

// MARK: - Local actor with nonisolated deinit

@available(macOS 12, *)
actor SimpleActorIsolated {
  let someField: SomeClass
  init() {
    self.someField = SomeClass()
  }
  deinit {
    doIt()
  }
}

// MARK: Destroying deinit

// ===== -----------------------------------------------------------------------
// CHECK-LABEL: // SimpleActorIsolated.deinit
// CHECK-NEXT: sil hidden{{.*}} @$s14default_deinit19SimpleActorIsolatedCfd : $@convention(method) (@guaranteed SimpleActorIsolated) -> @owned Builtin.NativeObject {
// CHECK: bb0([[SELF:%[0-9]+]] : $SimpleActorIsolated):
// CHECK:   [[REF:%[0-9]+]] = ref_element_addr [[SELF]] : $SimpleActorIsolated, #SimpleActorIsolated.someField
// CHECK:   [[ACCESS:%[0-9]+]] = begin_access [deinit] [static] [[REF]]
// CHECK:   destroy_addr [[ACCESS]] : $*SomeClass
// CHECK:   end_access [[ACCESS]]
// CHECK:   builtin "destroyDefaultActor"([[SELF]] : $SimpleActorIsolated)
// CHECK:   [[CAST:%[0-9]+]] = unchecked_ref_cast [[SELF]]
// CHECK:   return [[CAST]] : $Builtin.NativeObject
// CHECK: } // end sil function '$s14default_deinit19SimpleActorIsolatedCfd'

// MARK: Isolated deallocating deinit

// ===== -----------------------------------------------------------------------
// CHECK-LABEL: // SimpleActorIsolated.__isolated_deallocating_deinit
// CHECK-NEXT: sil hidden{{.*}} @$s14default_deinit19SimpleActorIsolatedCfZ : $@convention(thin) (@owned SimpleActorIsolated) -> () {
// CHECK: bb0([[SELF:%[0-9]+]] : $SimpleActorIsolated):
// CHECK: [[DESTROYER_REF:%[0-9]+]] = function_ref @$s14default_deinit19SimpleActorIsolatedCfd : $@convention(method) (@guaranteed SimpleActorIsolated) -> @owned Builtin.NativeObject
// CHECK: [[UNTYPED_DESTOY_RESULT:%[0-9]+]] = apply [[DESTROYER_REF]]([[SELF]]) : $@convention(method) (@guaranteed SimpleActorIsolated) -> @owned Builtin.NativeObject
// CHECK: [[TYPED_DESTOY_RESULT:%[0-9]+]] = unchecked_ref_cast [[UNTYPED_DESTOY_RESULT]] : $Builtin.NativeObject to $SimpleActorIsolated
// CHECK: dealloc_ref [[TYPED_DESTOY_RESULT]] : $SimpleActorIsolated
// CHECK:  [[RESULT:%[0-9]+]] = tuple ()
// CHECK:  return [[RESULT]] : $()
// CHECK: } // end sil function '$s14default_deinit19SimpleActorIsolatedCfZ'

// MARK: Deallocating deinit

// ===== -----------------------------------------------------------------------
// CHECK-LABEL: // SimpleActorIsolated.__deallocating_deinit
// CHECK-NEXT: sil hidden{{.*}} @$s14default_deinit19SimpleActorIsolatedCfD : $@convention(method) (@owned SimpleActorIsolated) -> () {
// CHECK: bb0([[SELF:%[0-9]+]] : $SimpleActorIsolated):
// CHECK: [[ISOLATED_REF:%[0-9]+]] = function_ref @$s14default_deinit19SimpleActorIsolatedCfZ : $@convention(thin) (@owned SimpleActorIsolated) -> ()
// CHECK: [[EXECUTOR:%[0-9]+]] = extract_executor [[SELF]] : $SimpleActorIsolated
// CHECK: [[PERFORM_REF:%[0-9]+]] = function_ref @swift_task_deinitOnExecutor : $@convention(thin) (@owned AnyObject, @convention(thin) (@owned AnyObject) -> (), Builtin.Executor) -> ()
// CHECK: [[SELF_AS_ANY_OBJECT:%[0-9]+]] = unchecked_bitwise_cast [[SELF]] : $SimpleActorIsolated to $AnyObject
// CHECK: [[ISOLATED_CASTED:%[0-9]+]] = convert_function [[ISOLATED_REF]] : $@convention(thin) (@owned SimpleActorIsolated) -> () to $@convention(thin) (@owned AnyObject) -> ()
// CHECK: [[DROP:%[0-9]+]] = apply [[PERFORM_REF]]([[SELF_AS_ANY_OBJECT]], [[ISOLATED_CASTED]], [[EXECUTOR]]) : $@convention(thin) (@owned AnyObject, @convention(thin) (@owned AnyObject) -> (), Builtin.Executor) -> ()
// CHECK:  [[RESULT:%[0-9]+]] = tuple ()
// CHECK:  return [[RESULT]] : $()
// CHECK: } // end sil function '$s14default_deinit19SimpleActorIsolatedCfD'
