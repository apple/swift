// RUN: %target-swift-frontend(mock-sdk: %clang-importer-sdk) -emit-silgen -swift-version 6 -disable-availability-checking -I %S/Inputs/custom-modules %s -verify | %FileCheck --enable-var-scope --check-prefix=CHECK --check-prefix=CHECK-%target-cpu %s
// REQUIRES: concurrency
// REQUIRES: objc_interop

import Foundation
import EffectfulProperties

// CHECK-LABEL: sil {{.*}}@${{.*}}13testJustAsync
func testJustAsync(eff : EffProps) async {
  // CHECK: [[RESUME_BUF:%.*]] = alloc_stack $NSObject
  // CHECK: [[METHOD:%.*]] = objc_method {{.*}} $@convention(objc_method) (@convention(block) (NSObject) -> (), EffProps) -> ()
  // CHECK: [[CONT:%.*]] = get_async_continuation_addr NSObject, [[RESUME_BUF]]
  // CHECK: [[WRAPPED:%.*]] = struct $UnsafeContinuation<NSObject, Never> ([[CONT]] : $Builtin.RawUnsafeContinuation)
  // CHECK: [[BLOCK_STORAGE:%.*]] = alloc_stack $@block_storage Any
  // CHECK: [[CONT_SLOT:%.*]] = project_block_storage [[BLOCK_STORAGE]]
  // CHECK: [[CHECKED_CONT_SLOT:%.*]] = init_existential_addr [[CONT_SLOT]] : $*Any, $CheckedContinuation<NSObject, Never>
  // CHECK: [[CHECKED_CONT_INIT_FN:%.*]] = function_ref @$ss26_createCheckedContinuationyScCyxs5NeverOGSccyxACGnlF : $@convention(thin) <τ_0_0> (UnsafeContinuation<τ_0_0, Never>) -> @out CheckedContinuation<τ_0_0, Never>
  // CHECK: [[CHECKED_CONT:%.*]] = alloc_stack $CheckedContinuation<NSObject, Never>
  // CHECK: {{.*}} = apply [[CHECKED_CONT_INIT_FN]]<NSObject>([[CHECKED_CONT]], [[WRAPPED]]) : $@convention(thin) <τ_0_0> (UnsafeContinuation<τ_0_0, Never>) -> @out CheckedContinuation<τ_0_0, Never>
  // CHECK: copy_addr [take] [[CHECKED_CONT]] to [init] [[CHECKED_CONT_SLOT]] : $*CheckedContinuation<NSObject, Never>
  // CHECK: [[BLOCK_IMPL:%.*]] = function_ref @[[NSO_COMPLETION_BLOCK:.*]] : $@convention(c) (@inout_aliasable @block_storage Any, NSObject) -> ()
  // CHECK: [[BLOCK:%.*]] = init_block_storage_header [[BLOCK_STORAGE]] {{.*}}, invoke [[BLOCK_IMPL]]
  // CHECK: apply [[METHOD]]([[BLOCK]], %0)
  // CHECK: await_async_continuation [[CONT]] {{.*}}, resume [[RESUME:bb[0-9]+]]

  // CHECK: [[RESUME]]:
  // CHECK: [[RESULT:%.*]] = load [take] [[RESUME_BUF]]
  // CHECK: dealloc_stack [[RESUME_BUF]]
  let _ = await eff.doggo
}

// CHECK-LABEL: sil {{.*}}@${{.*}}15testAsyncThrows
func testAsyncThrows(eff : EffProps) async {
  // CHECK: [[RESUME_BUF:%.*]] = alloc_stack $Optional<NSObject>
  // CHECK: [[METHOD:%.*]] = objc_method {{.*}} $@convention(objc_method) (@convention(block) (Optional<NSObject>, Optional<NSError>) -> (), EffProps) -> ()
  // CHECK: [[CONT:%.*]] = get_async_continuation_addr [throws] Optional<NSObject>, [[RESUME_BUF]]
  // CHECK: [[WRAPPED:%.*]] = struct $UnsafeContinuation<Optional<NSObject>, any Error> ([[CONT]] : $Builtin.RawUnsafeContinuation)
  // CHECK: [[BLOCK_STORAGE:%.*]] = alloc_stack $@block_storage Any
  // CHECK: [[CONT_SLOT:%.*]] = project_block_storage [[BLOCK_STORAGE]]
  // CHECK: [[CHECKED_CONT_SLOT:%.*]] = init_existential_addr [[CONT_SLOT]] : $*Any, $CheckedContinuation<Optional<NSObject>, any Error>
  // CHECK: [[CHECKED_CONT_INIT_FN:%.*]] = function_ref @$ss34_createCheckedThrowingContinuationyScCyxs5Error_pGSccyxsAB_pGnlF : $@convention(thin) <τ_0_0> (UnsafeContinuation<τ_0_0, any Error>) -> @out CheckedContinuation<τ_0_0, any Error>
  // CHECK: [[CHECKED_CONT:%.*]] = alloc_stack $CheckedContinuation<Optional<NSObject>, any Error>
  // CHECK: {{.*}} = apply [[CHECKED_CONT_INIT_FN:%.*]]<Optional<NSObject>>([[CHECKED_CONT]], [[WRAPPED]]) : $@convention(thin) <τ_0_0> (UnsafeContinuation<τ_0_0, any Error>) -> @out CheckedContinuation<τ_0_0, any Error>
  // CHECK: copy_addr [take] [[CHECKED_CONT]] to [init] [[CHECKED_CONT_SLOT]] : $*CheckedContinuation<Optional<NSObject>, any Error>
  // CHECK: [[BLOCK_IMPL:%.*]] = function_ref @[[NSO_COMPLETION_BLOCK:.*]] : $@convention(c) (@inout_aliasable @block_storage Any, Optional<NSObject>, Optional<NSError>) -> ()
  // CHECK: [[BLOCK:%.*]] = init_block_storage_header [[BLOCK_STORAGE]] {{.*}}, invoke [[BLOCK_IMPL]]
  // CHECK: apply [[METHOD]]([[BLOCK]], %0)
  // CHECK: await_async_continuation [[CONT]] {{.*}}, resume [[RESUME:bb[0-9]+]], error [[RESUME_ERROR:bb[0-9]+]]

  // CHECK: [[RESUME]]:
  // CHECK: [[RESULT:%.*]] = load [take] [[RESUME_BUF]]
  // CHECK: dealloc_stack [[RESUME_BUF]]

  // CHECK: [[RESUME_ERROR]]([[ERROR_VAL:%[0-9]+]] : @owned $any Error):
  // CHECK: dealloc_stack [[RESUME_BUF]]
  let _ = try? await eff.catto
}

// CHECK-LABEL: sil {{.*}}@${{.*}}17testMainActorProp
func testMainActorProp(eff : EffProps) async {
  // CHECK:         [[GENERIC_EXEC:%.*]] = enum $Optional<Builtin.Executor>, #Optional.none
  // CHECK-NEXT:    hop_to_executor [[GENERIC_EXEC]] :
  // CHECK:         hop_to_executor [[GENERIC_EXEC]] :
  // CHECK: } // end sil function '${{.*}}17testMainActorProp
  let _ = await eff.mainDogProp
}

// CHECK-LABEL: sil {{.*}}@${{.*}}19testMainActorMethod
func testMainActorMethod(eff : EffProps) async {
  // CHECK:         [[GENERIC_EXEC:%.*]] = enum $Optional<Builtin.Executor>, #Optional.none
  // CHECK-NEXT:    hop_to_executor [[GENERIC_EXEC]] :
  // CHECK:         hop_to_executor [[GENERIC_EXEC]] :
  // CHECK: } // end sil function '${{.*}}19testMainActorMethod
  let _ = await eff.regularMainDog()
}
