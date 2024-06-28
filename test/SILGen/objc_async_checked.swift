// RUN: %target-swift-frontend(mock-sdk: %clang-importer-sdk) -emit-silgen -swift-version 6 -I %S/Inputs/custom-modules -disable-availability-checking %s -verify | %FileCheck --check-prefix=CHECK --check-prefix=CHECK-%target-cpu %s

// REQUIRES: concurrency
// REQUIRES: objc_interop

import Foundation
import ObjCConcurrency

// CHECK-LABEL: sil {{.*}}@${{.*}}14testSlowServer
func testSlowServer(slowServer: SlowServer) async throws {
  // CHECK: [[RESUME_BUF:%.*]] = alloc_stack $Int
  // CHECK: [[STRINGINIT:%.*]] = function_ref @$sSS10FoundationE19_bridgeToObjectiveCSo8NSStringCyF :
  // CHECK: [[ARG:%.*]] = apply [[STRINGINIT]]
  // CHECK: [[METHOD:%.*]] = objc_method {{.*}} $@convention(objc_method) (NSString, @convention(block) (Int) -> (), SlowServer) -> ()
  // CHECK: [[CONT:%.*]] = get_async_continuation_addr Int, [[RESUME_BUF]]
  // CHECK: [[WRAPPED:%.*]] = struct $UnsafeContinuation<Int, Never> ([[CONT]] : $Builtin.RawUnsafeContinuation)
  // CHECK: [[BLOCK_STORAGE:%.*]] = alloc_stack $@block_storage Any
  // CHECK: [[CONT_SLOT:%.*]] = project_block_storage [[BLOCK_STORAGE]]
  // CHECK: [[CHECKED_CONT_SLOT:%.*]] = init_existential_addr [[CONT_SLOT]] : $*Any, $CheckedContinuation<Int, Never>
  // CHECK: [[CHECKED_CONT_INIT_FN:%.*]] = function_ref @$ss26_createCheckedContinuationyScCyxs5NeverOGSccyxACGnlF : $@convention(thin) <τ_0_0> (UnsafeContinuation<τ_0_0, Never>) -> @out CheckedContinuation<τ_0_0, Never>
  // CHECK: [[CHECKED_CONT:%.*]] = alloc_stack $CheckedContinuation<Int, Never>
  // CHECK: {{.*}} = apply [[CHECKED_CONT_INIT_FN]]<Int>([[CHECKED_CONT]], [[WRAPPED]]) : $@convention(thin) <τ_0_0> (UnsafeContinuation<τ_0_0, Never>) -> @out CheckedContinuation<τ_0_0, Never>
  // CHECK: copy_addr [take] [[CHECKED_CONT]] to [init] [[CHECKED_CONT_SLOT]] : $*CheckedContinuation<Int, Never>
  // CHECK: [[BLOCK_IMPL:%.*]] = function_ref @[[INT_COMPLETION_BLOCK:.*]] : $@convention(c) (@inout_aliasable @block_storage Any, Int) -> ()
  // CHECK: [[BLOCK:%.*]] = init_block_storage_header [[BLOCK_STORAGE]] {{.*}}, invoke [[BLOCK_IMPL]]
  // CHECK: apply [[METHOD]]([[ARG]], [[BLOCK]], %0)
  // CHECK: [[COPY:%.*]] = copy_value [[ARG]]
  // CHECK: destroy_value [[ARG]]
  // CHECK: await_async_continuation [[CONT]] {{.*}}, resume [[RESUME:bb[0-9]+]]
  // CHECK: [[RESUME]]:
  // CHECK: [[RESULT:%.*]] = load [trivial] [[RESUME_BUF]]
  // CHECK: fix_lifetime [[COPY]]
  // CHECK: destroy_value [[COPY]]
  // CHECK: dealloc_stack [[RESUME_BUF]]
  let _: Int = await slowServer.doSomethingSlow("mail")

  let _: Int = await slowServer.doSomethingSlowNullably("mail")

  // CHECK: [[RESUME_BUF:%.*]] = alloc_stack $String
  // CHECK: [[METHOD:%.*]] = objc_method {{.*}} $@convention(objc_method) (@convention(block) (Optional<NSString>, Optional<NSError>) -> (), SlowServer) -> ()
  // CHECK: [[CONT:%.*]] = get_async_continuation_addr [throws] String, [[RESUME_BUF]]
  // CHECK: [[WRAPPED:%.*]] = struct $UnsafeContinuation<String, any Error> ([[CONT]] : $Builtin.RawUnsafeContinuation)
  // CHECK: [[BLOCK_STORAGE:%.*]] = alloc_stack $@block_storage Any
  // CHECK: [[CONT_SLOT:%.*]] = project_block_storage [[BLOCK_STORAGE]]
  // CHECK: [[CHECKED_CONT_SLOT:%.*]] = init_existential_addr [[CONT_SLOT]] : $*Any, $CheckedContinuation<String, any Error>
  // CHECK: [[CHECKED_CONT_INIT_FN:%.*]] = function_ref @$ss34_createCheckedThrowingContinuationyScCyxs5Error_pGSccyxsAB_pGnlF : $@convention(thin) <τ_0_0> (UnsafeContinuation<τ_0_0, any Error>) -> @out CheckedContinuation<τ_0_0, any Error>
  // CHECK: [[CHECKED_CONT:%.*]] = alloc_stack $CheckedContinuation<String, any Error>
  // CHECK: {{.*}} = apply [[CHECKED_CONT_INIT_FN]]<String>([[CHECKED_CONT]], [[WRAPPED]]) : $@convention(thin) <τ_0_0> (UnsafeContinuation<τ_0_0, any Error>) -> @out CheckedContinuation<τ_0_0, any Error>
  // CHECK: copy_addr [take] [[CHECKED_CONT]] to [init] [[CHECKED_CONT_SLOT]] : $*CheckedContinuation<String, any Error>
  // CHECK: [[BLOCK_IMPL:%.*]] = function_ref @[[STRING_COMPLETION_THROW_BLOCK:.*]] : $@convention(c) (@inout_aliasable @block_storage Any, Optional<NSString>, Optional<NSError>) -> ()
  // CHECK: [[BLOCK:%.*]] = init_block_storage_header [[BLOCK_STORAGE]] {{.*}}, invoke [[BLOCK_IMPL]]
  // CHECK: apply [[METHOD]]([[BLOCK]], %0)
  // CHECK: await_async_continuation [[CONT]] {{.*}}, resume [[RESUME:bb[0-9]+]], error [[ERROR:bb[0-9]+]]
  // CHECK: [[RESUME]]:
  // CHECK: [[RESULT:%.*]] = load [take] [[RESUME_BUF]]
  // CHECK: destroy_value [[RESULT]]
  // CHECK: dealloc_stack [[RESUME_BUF]]
  let _: String = try await slowServer.findAnswer()

  // CHECK: objc_method {{.*}} $@convention(objc_method) (NSString, @convention(block) () -> (), SlowServer) -> ()
  // CHECK: [[BLOCK_IMPL:%.*]] = function_ref @[[VOID_COMPLETION_BLOCK:.*]] : $@convention(c) (@inout_aliasable @block_storage Any) -> ()
  await slowServer.serverRestart("somewhere")

  // CHECK: function_ref @[[STRING_NONZERO_FLAG_THROW_BLOCK:.*]] : $@convention(c) (@inout_aliasable @block_storage Any, {{.*}}Bool, Optional<NSString>, Optional<NSError>) -> ()
  let _: String = try await slowServer.doSomethingFlaggy()
  // CHECK: function_ref @[[STRING_ZERO_FLAG_THROW_BLOCK:.*]] : $@convention(c) (@inout_aliasable @block_storage Any, Optional<NSString>, {{.*}}Bool, Optional<NSError>) -> ()
  let _: String = try await slowServer.doSomethingZeroFlaggy()
  // CHECK: function_ref @[[STRING_STRING_ZERO_FLAG_THROW_BLOCK:.*]] : $@convention(c) (@inout_aliasable @block_storage Any, {{.*}}Bool, Optional<NSString>, Optional<NSError>, Optional<NSString>) -> ()
  let _: (String, String) = try await slowServer.doSomethingMultiResultFlaggy()

  // CHECK: [[BLOCK_IMPL:%.*]] = function_ref @[[NSSTRING_INT_THROW_COMPLETION_BLOCK:.*]] : $@convention(c) (@inout_aliasable @block_storage Any, Optional<NSString>, Int, Optional<NSError>) -> ()
  let (_, _): (String, Int) = try await slowServer.findMultipleAnswers()

  let (_, _): (Bool, Bool) = try await slowServer.findDifferentlyFlavoredBooleans()

  // CHECK: [[ERROR]]([[ERROR_VALUE:%.*]] : @owned $any Error):
  // CHECK:   dealloc_stack [[RESUME_BUF]]
  // CHECK:   br [[THROWBB:bb[0-9]+]]([[ERROR_VALUE]]
  // CHECK: [[THROWBB]]([[ERROR_VALUE:%.*]] : @owned $any Error):
  // CHECK:   throw [[ERROR_VALUE]]

  let _: String = await slowServer.findAnswerNullably("foo")
  let _: String = try await slowServer.doSomethingDangerousNullably("foo")

  let _: NSObject? = try await slowServer.stopRecording()
  let _: NSObject = try await slowServer.someObject()

  let _: () -> Void = await slowServer.performVoid2Void()
  let _: (Any) -> Void = await slowServer.performId2Void()
  let _: (Any) -> Any = await slowServer.performId2Id()
  let _: (String) -> String = await slowServer.performNSString2NSString()

  let _: ((String) -> String, String) = await slowServer.performNSString2NSStringNSString()
  let _: ((Any) -> Void, (Any) -> Void) = await slowServer.performId2VoidId2Void()

  let _: String = try await slowServer.findAnswerFailingly()

  let _: () -> Void = try await slowServer.obtainClosure()

  let _: Flavor = try await slowServer.iceCreamFlavor()
}

func testGeneric<T: AnyObject>(x: GenericObject<T>) async throws {
  let _: T? = try await x.doSomething()
  let _: GenericObject<T>? = await x.doAnotherThing()
}

func testGeneric2<T: AnyObject, U>(x: GenericObject<T>, y: U) async throws {
  let _: T? = try await x.doSomething()
  let _: GenericObject<T>? = await x.doAnotherThing()
}

// CHECK: sil{{.*}}@[[INT_COMPLETION_BLOCK]]
// CHECK:   [[CONT_ADDR:%.*]] = project_block_storage %0
// CHECK:   [[CONT_OPENED:%.*]] = open_existential_addr immutable_access [[CONT_ADDR]] : $*Any to $*@opened("{{.*}}", Any) Self
// CHECK:   [[CONT:%.*]] = unchecked_addr_cast [[CONT_OPENED]] : $*@opened("{{.*}}", Any) Self to $*CheckedContinuation<Int, Never>
// CHECK:   [[RESULT_BUF:%.*]] = alloc_stack $Int
// CHECK:   store %1 to [trivial] [[RESULT_BUF]]
// CHECK:   [[RESUME:%.*]] = function_ref @{{.*}}resumeCheckedContinuation
// CHECK:   apply [[RESUME]]<Int>([[CONT]], [[RESULT_BUF]])

// CHECK: sil{{.*}}@[[STRING_COMPLETION_THROW_BLOCK]]
// CHECK:   [[RESUME_IN:%.*]] = copy_value %1
// CHECK:   [[ERROR_IN:%.*]] = copy_value %2
// CHECK:   [[CONT_ADDR:%.*]] = project_block_storage %0
// CHECK:   [[CONT_OPENED:%.*]] = open_existential_addr immutable_access [[CONT_ADDR]] : $*Any to $*@opened("{{.*}}", Any) Self
// CHECK:   [[CONT:%.*]] = unchecked_addr_cast [[CONT_OPENED]] : $*@opened("{{.*}}", Any) Self to $*CheckedContinuation<String, any Error>
// CHECK:   [[ERROR_IN_B:%.*]] = begin_borrow [[ERROR_IN]]
// CHECK:   switch_enum [[ERROR_IN_B]] : {{.*}}, case #Optional.some!enumelt: [[ERROR_BB:bb[0-9]+]], case #Optional.none!enumelt: [[RESUME_BB:bb[0-9]+]]
// CHECK: [[RESUME_BB]]:
// CHECK:   [[RESULT_BUF:%.*]] = alloc_stack $String
// CHECK:   [[RESUME_CP:%.*]] = copy_value [[RESUME_IN]]
// CHECK:   [[BRIDGE:%.*]] = function_ref @{{.*}}unconditionallyBridgeFromObjectiveC
// CHECK:   [[BRIDGED_RESULT:%.*]] = apply [[BRIDGE]]([[RESUME_CP]]
// CHECK:   store [[BRIDGED_RESULT]] to [init] [[RESULT_BUF]]
// CHECK:   [[RESUME:%.*]] = function_ref @{{.*}}resumeCheckedThrowingContinuation
// CHECK:   apply [[RESUME]]<String>([[CONT]], [[RESULT_BUF]])
// CHECK:   br [[END_BB:bb[0-9]+]]
// CHECK: [[END_BB]]:
// CHECK:   return
// CHECK: [[ERROR_BB]]([[ERROR_IN_UNWRAPPED:%.*]] : @guaranteed $NSError):
// CHECK:   [[ERROR:%.*]] = init_existential_ref [[ERROR_IN_UNWRAPPED]]
// CHECK:   [[RESUME_WITH_ERROR:%.*]] = function_ref @{{.*}}resumeCheckedThrowingContinuationWithError
// CHECK:   [[ERROR_COPY:%.*]] = copy_value [[ERROR]]
// CHECK:   apply [[RESUME_WITH_ERROR]]<String>([[CONT]], [[ERROR_COPY]])
// CHECK:   br [[END_BB]]

// CHECK: sil {{.*}} @[[VOID_COMPLETION_BLOCK]]
// CHECK:   [[CONT_ADDR:%.*]] = project_block_storage %0
// CHECK:   [[CONT_OPENED:%.*]] = open_existential_addr immutable_access [[CONT_ADDR]] : $*Any to $*@opened("{{.*}}", Any) Self
// CHECK:   [[CONT:%.*]] = unchecked_addr_cast [[CONT_OPENED]] : $*@opened("{{.*}}", Any) Self to $*CheckedContinuation<(), Never>
// CHECK:   [[RESULT_BUF:%.*]] = alloc_stack $()
// CHECK:   [[RESUME:%.*]] = function_ref @{{.*}}resumeCheckedContinuation
// CHECK:   apply [[RESUME]]<()>([[CONT]], [[RESULT_BUF]])

// CHECK: sil{{.*}}@[[STRING_NONZERO_FLAG_THROW_BLOCK]]
// CHECK:   [[ZERO:%.*]] = integer_literal {{.*}}, 0
// CHECK:   switch_value {{.*}}, case [[ZERO]]: [[ZERO_BB:bb[0-9]+]], default [[NONZERO_BB:bb[0-9]+]]
// CHECK: [[ZERO_BB]]:
// CHECK:   function_ref{{.*}}resumeCheckedThrowingContinuation
// CHECK: [[NONZERO_BB]]:
// CHECK:   function_ref{{.*}}resumeCheckedThrowingContinuationWithError

// CHECK: sil{{.*}}@[[STRING_ZERO_FLAG_THROW_BLOCK]]
// CHECK:   [[ZERO:%.*]] = integer_literal {{.*}}, 0
// CHECK:   switch_value {{.*}}, case [[ZERO]]: [[ZERO_BB:bb[0-9]+]], default [[NONZERO_BB:bb[0-9]+]]
// CHECK: [[NONZERO_BB]]:
// CHECK:   function_ref{{.*}}resumeCheckedThrowingContinuation
// CHECK: [[ZERO_BB]]:
// CHECK:   function_ref{{.*}}resumeCheckedThrowingContinuationWithError

// CHECK: sil{{.*}}@[[STRING_STRING_ZERO_FLAG_THROW_BLOCK]]
// CHECK:   [[ZERO:%.*]] = integer_literal {{.*}}, 0
// CHECK:   switch_value {{.*}}, case [[ZERO]]: [[ZERO_BB:bb[0-9]+]], default [[NONZERO_BB:bb[0-9]+]]
// CHECK: [[NONZERO_BB]]:
// CHECK:   function_ref{{.*}}resumeCheckedThrowingContinuation
// CHECK: [[ZERO_BB]]:
// CHECK:   function_ref{{.*}}resumeCheckedThrowingContinuationWithError

// CHECK: sil{{.*}}@[[NSSTRING_INT_THROW_COMPLETION_BLOCK]]
// CHECK:   [[RESULT_BUF:%.*]] = alloc_stack $(String, Int)
// CHECK:   [[RESULT_0_BUF:%.*]] = tuple_element_addr [[RESULT_BUF]] {{.*}}, 0
// CHECK:   [[BRIDGE:%.*]] = function_ref @{{.*}}unconditionallyBridgeFromObjectiveC
// CHECK:   [[BRIDGED:%.*]] = apply [[BRIDGE]]
// CHECK:   store [[BRIDGED]] to [init] [[RESULT_0_BUF]]
// CHECK:   [[RESULT_1_BUF:%.*]] = tuple_element_addr [[RESULT_BUF]] {{.*}}, 1
// CHECK:   store %2 to [trivial] [[RESULT_1_BUF]]

// CHECK-LABEL: sil {{.*}}@${{.*}}22testSlowServerFromMain
@MainActor
func testSlowServerFromMain(slowServer: SlowServer) async throws {
  // CHECK: hop_to_executor {{%.*}} : $MainActor
  // CHECK: [[RESUME_BUF:%.*]] = alloc_stack $Int
  // CHECK: [[STRINGINIT:%.*]] = function_ref @$sSS10FoundationE19_bridgeToObjectiveCSo8NSStringCyF :
  // CHECK: [[ARG:%.*]] = apply [[STRINGINIT]]
  // CHECK: [[METHOD:%.*]] = objc_method {{.*}} $@convention(objc_method) (NSString, @convention(block) (Int) -> (), SlowServer) -> ()
  // CHECK: [[CONT:%.*]] = get_async_continuation_addr Int, [[RESUME_BUF]]
  // CHECK: [[WRAPPED:%.*]] = struct $UnsafeContinuation<Int, Never> ([[CONT]] : $Builtin.RawUnsafeContinuation)
  // CHECK: [[BLOCK_STORAGE:%.*]] = alloc_stack $@block_storage Any
  // CHECK: [[CONT_SLOT:%.*]] = project_block_storage [[BLOCK_STORAGE]]
  // CHECK: [[CHECKED_CONT_SLOT:%.*]] = init_existential_addr [[CONT_SLOT]] : $*Any, $CheckedContinuation<Int, Never>
  // CHECK: [[CHECKED_CONT_INIT_FN:%.*]] = function_ref @$ss26_createCheckedContinuationyScCyxs5NeverOGSccyxACGnlF : $@convention(thin) <τ_0_0> (UnsafeContinuation<τ_0_0, Never>) -> @out CheckedContinuation<τ_0_0, Never>
  // CHECK: [[CHECKED_CONT:%.*]] = alloc_stack $CheckedContinuation<Int, Never>
  // CHECK: {{.*}} = apply [[CHECKED_CONT_INIT_FN]]<Int>([[CHECKED_CONT]], [[WRAPPED]]) : $@convention(thin) <τ_0_0> (UnsafeContinuation<τ_0_0, Never>) -> @out CheckedContinuation<τ_0_0, Never>
  // CHECK: copy_addr [take] [[CHECKED_CONT]] to [init] [[CHECKED_CONT_SLOT]] : $*CheckedContinuation<Int, Never>
  // CHECK: [[BLOCK_IMPL:%.*]] = function_ref @[[INT_COMPLETION_BLOCK:.*]] : $@convention(c) (@inout_aliasable @block_storage Any, Int) -> ()
  // CHECK: [[BLOCK:%.*]] = init_block_storage_header [[BLOCK_STORAGE]] {{.*}}, invoke [[BLOCK_IMPL]]
  // CHECK: apply [[METHOD]]([[ARG]], [[BLOCK]], %0)
  // CHECK: [[COPY:%.*]] = copy_value [[ARG]]
  // CHECK: destroy_value [[ARG]]
  // CHECK: await_async_continuation [[CONT]] {{.*}}, resume [[RESUME:bb[0-9]+]]
  // CHECK: [[RESUME]]:
  // CHECK: hop_to_executor {{%.*}} : $MainActor
  // CHECK: [[RESULT:%.*]] = load [trivial] [[RESUME_BUF]]
  // CHECK: fix_lifetime [[COPY]]
  // CHECK: destroy_value [[COPY]]
  // CHECK: dealloc_stack [[RESUME_BUF]]
  let _: Int = await slowServer.doSomethingSlow("mail")
}

// CHECK-LABEL: sil hidden [ossa] @$s18objc_async_checked20testWithForeignError10slowServerySo04SlowI0C_tYaKF : $@convention(thin) @async (@guaranteed SlowServer) -> @error any Error
// CHECK: [[STR:%.*]] = alloc_stack $String
// CHECK: [[ERR:%.*]] = alloc_stack [dynamic_lifetime] $Optional<NSError>
// CHECK: inject_enum_addr [[ERR]] : $*Optional<NSError>, #Optional.none!enumelt
// CHECK: [[METHOD:%.*]] = objc_method %0 : $SlowServer, #SlowServer.findAnswerFailingly!foreign : (SlowServer) -> () async throws -> String, $@convention(objc_method) (Optional<AutoreleasingUnsafeMutablePointer<Optional<NSError>>>, @convention(block) (Optional<NSString>, Optional<NSError>) -> (), SlowServer) -> ObjCBool
// CHECK: [[RAW_CONT:%.*]] = get_async_continuation_addr [throws] String, {{.*}} : $*String
// CHECK: [[UNSAFE_CONT:%.*]] = struct $UnsafeContinuation<String, any Error> ([[RAW_CONT]] : $Builtin.RawUnsafeContinuation)
// CHECK: [[BLOCK_STORAGE:%.*]] = alloc_stack $@block_storage Any
// CHECK: [[PROJECTED:%.*]] = project_block_storage [[BLOCK_STORAGE]] : $*@block_storage Any
// CHECK: [[CHECKED_CONT_SLOT:%.*]] = init_existential_addr [[PROJECTED]] : $*Any, $CheckedContinuation<String, any Error>
// CHECK: [[CHECKED_CONT_INIT_FN:%.*]] = function_ref @$ss34_createCheckedThrowingContinuationyScCyxs5Error_pGSccyxsAB_pGnlF : $@convention(thin) <τ_0_0> (UnsafeContinuation<τ_0_0, any Error>) -> @out CheckedContinuation<τ_0_0, any Error>
// CHECK: [[CHECKED_CONT:%.*]] = alloc_stack $CheckedContinuation<String, any Error>
// CHECK: {{.*}} = apply [[CHECKED_CONT_INIT_FN]]<String>([[CHECKED_CONT]], [[UNSAFE_CONT]]) : $@convention(thin) <τ_0_0> (UnsafeContinuation<τ_0_0, any Error>) -> @out CheckedContinuation<τ_0_0, any Error>
// CHECK: copy_addr [take] [[CHECKED_CONT]] to [init] [[CHECKED_CONT_SLOT]] : $*CheckedContinuation<String, any Error>
// CHECK: cond_br {{.*}}, [[RESUME:bb[0-9]+]], [[ERROR:bb[0-9]+]]
//
// CHECK: [[ERROR]]:
// CHECK: [[ERR_VALUE:%.*]] = load [take] [[ERR]] : $*Optional<NSError>
// CHECK: [[CONV_FN:%.*]] = function_ref @$s10Foundation22_convertNSErrorToErrorys0E0_pSo0C0CSgF : $@convention(thin) (@guaranteed Optional<NSError>) -> @owned any Error
// CHECK: [[SWIFT_ERROR:%.*]] = apply [[CONV_FN]]([[ERR_VALUE]]) : $@convention(thin) (@guaranteed Optional<NSError>) -> @owned any Error
// CHECK: [[PROJECTED_STORAGE:%.*]] = project_block_storage [[BLOCK_STORAGE]] : $*@block_storage Any
// CHECK: [[CONT_OPENED:%.*]] = open_existential_addr immutable_access [[PROJECTED_STORAGE]] : $*Any to $*@opened("{{.*}}", Any) Self
// CHECK: [[CHECKED_CONT:%.*]] = unchecked_addr_cast [[CONT_OPENED]] : $*@opened("{{.*}}", Any) Self to $*CheckedContinuation<String, any Error>
// CHECK: [[ERROR_COPY:%.*]] = copy_value [[SWIFT_ERROR]] : $any Error
// CHECK: [[RESUME_FN:%.*]] = function_ref @$ss43_resumeCheckedThrowingContinuationWithErroryyScCyxs0F0_pG_sAB_pntlF : $@convention(thin) <τ_0_0> (@in_guaranteed CheckedContinuation<τ_0_0, any Error>, @owned any Error) -> ()
// CHECK: {{.*}} = apply [[RESUME_FN]]<String>([[CHECKED_CONT]], [[ERROR_COPY]]) : $@convention(thin) <τ_0_0> (@in_guaranteed CheckedContinuation<τ_0_0, any Error>, @owned any Error) -> ()
func testWithForeignError(slowServer: SlowServer) async throws {
  let _: String = try await slowServer.findAnswerFailingly()
}

// CHECK-LABEL: sil {{.*}}@${{.*}}26testThrowingMethodFromMain
@MainActor
func testThrowingMethodFromMain(slowServer: SlowServer) async -> String {
// CHECK:  [[RESULT_BUF:%.*]] = alloc_stack $String
// CHECK:  [[STRING_ARG:%.*]] = apply {{%.*}}({{%.*}}) : $@convention(method) (@guaranteed String) -> @owned NSString
// CHECK:  [[METH:%.*]] = objc_method {{%.*}} : $SlowServer, #SlowServer.doSomethingDangerous!foreign
// CHECK:  [[RAW_CONT:%.*]] = get_async_continuation_addr [throws] String, [[RESULT_BUF]] : $*String
// CHECK:  [[CONT:%.*]] = struct $UnsafeContinuation<String, any Error> ([[RAW_CONT]] : $Builtin.RawUnsafeContinuation)
// CHECK:  [[STORE_ALLOC:%.*]] = alloc_stack $@block_storage Any
// CHECK:  [[PROJECTED:%.*]] = project_block_storage [[STORE_ALLOC]] : $*@block_storage
// CHECK:  [[CHECKED_CONT_SLOT:%.*]] = init_existential_addr [[PROJECTED]] : $*Any, $CheckedContinuation<String, any Error>
// CHECK:  [[CHECKED_CONT_INIT_FN:%.*]] = function_ref @$ss34_createCheckedThrowingContinuationyScCyxs5Error_pGSccyxsAB_pGnlF : $@convention(thin) <τ_0_0> (UnsafeContinuation<τ_0_0, any Error>) -> @out CheckedContinuation<τ_0_0, any Error>
// CHECK:  [[CHECKED_CONT:%.*]] = alloc_stack $CheckedContinuation<String, any Error>
// CHECK:  {{.*}} = apply [[CHECKED_CONT_INIT_FN]]<String>([[CHECKED_CONT]], [[CONT]]) : $@convention(thin) <τ_0_0> (UnsafeContinuation<τ_0_0, any Error>) -> @out CheckedContinuation<τ_0_0, any Error>
// CHECK:  copy_addr [take] [[CHECKED_CONT]] to [init] [[CHECKED_CONT_SLOT]] : $*CheckedContinuation<String, any Error>
// CHECK:  [[INVOKER:%.*]] = function_ref @$sSo8NSStringCSgSo7NSErrorCSgIeyByy_SSTz_
// CHECK:  [[BLOCK:%.*]] = init_block_storage_header [[STORE_ALLOC]] {{.*}}, invoke [[INVOKER]]
// CHECK:  [[OPTIONAL_BLK:%.*]] = enum {{.*}}, #Optional.some!enumelt, [[BLOCK]]
// CHECK:  {{.*}} = apply [[METH]]([[STRING_ARG]], [[OPTIONAL_BLK]], {{%.*}}) : $@convention(objc_method) (NSString, Optional<@convention(block) (Optional<NSString>, Optional<NSError>) -> ()>, SlowServer) -> ()
// CHECK:  [[STRING_ARG_COPY:%.*]] = copy_value [[STRING_ARG]] : $NSString
// CHECK:  dealloc_stack [[CHECKED_CONT]] : $*CheckedContinuation<String, any Error>
// CHECK:  dealloc_stack [[STORE_ALLOC]] : $*@block_storage Any
// CHECK:  destroy_value [[STRING_ARG]] : $NSString
// CHECK:  await_async_continuation [[RAW_CONT]] : $Builtin.RawUnsafeContinuation, resume [[RESUME:bb[0-9]+]], error [[ERROR:bb[0-9]+]]

// CHECK: [[RESUME]]
// CHECK:   hop_to_executor {{%.*}} : $MainActor
// CHECK:   {{.*}} = load [take] [[RESULT_BUF]] : $*String
// CHECK:   fix_lifetime [[STRING_ARG_COPY]] : $NSString
// CHECK:   destroy_value [[STRING_ARG_COPY]] : $NSString
// CHECK:   dealloc_stack [[RESULT_BUF]] : $*String

// CHECK: [[ERROR]]
// CHECK:   hop_to_executor {{%.*}} : $MainActor
// CHECK:   fix_lifetime [[STRING_ARG_COPY]] : $NSString
// CHECK:   destroy_value [[STRING_ARG_COPY]] : $NSString
// CHECK:   dealloc_stack [[RESULT_BUF]] : $*String

  do {
    return try await slowServer.doSomethingDangerous("run-with-scissors")
  } catch {
    return "none"
  }
}

// rdar://91502776
// CHECK-LABEL: sil hidden [ossa] @$s{{.*}}21checkCostcoMembershipSbyYaF : $@convention(thin) @async () -> Bool {
// CHECK:    bb0:
// CHECK:        hop_to_executor {{%.*}} : $Optional<Builtin.Executor>
// CHECK:        [[FINAL_BUF:%.*]] = alloc_stack $Bool
// CHECK:        [[RESULT_BUF:%.*]] = alloc_stack $NSObject
// CHECK:        [[METH:%.*]] = objc_method {{%.*}} : $@objc_metatype Person.Type, #Person.asCustomer!foreign
// CHECK:        get_async_continuation_addr NSObject, [[RESULT_BUF]] : $*NSObject
// CHECK:        = apply [[METH]]
// CHECK:        dealloc_stack {{%.*}} : $*@block_storage
// CHECK:        await_async_continuation {{%.*}} : $Builtin.RawUnsafeContinuation, resume bb1
// CHECK:    bb1:
// CHECK:        hop_to_executor {{%.*}} : $Optional<Builtin.Executor>
// CHECK:        [[RESULT:%.*]] = load [take] [[RESULT_BUF]] : $*NSObject
// CHECK:        objc_method {{%.*}} : $CostcoManager, #CostcoManager.isCustomerEnrolled!foreign
// CHECK:        get_async_continuation_addr Bool, [[FINAL_BUF]] : $*Bool
// CHECK:        [[BLOCK_ARG:%.*]] = init_block_storage_header [[BLOCK_STORAGE:%.*]] : $*@block_storage
// CHECK:        = apply {{%.*}}([[RESULT]], [[BLOCK_ARG]], [[MANAGER:%.*]]) : $@convention(objc_method)
// CHECK:        [[EXTEND1:%.*]] = copy_value [[RESULT]] : $NSObject
// CHECK:        [[EXTEND2:%.*]] = copy_value [[MANAGER]] : $CostcoManager
// CHECK:        dealloc_stack [[BLOCK_STORAGE]] : $*@block_storage
// CHECK:        await_async_continuation {{%.*}} : $Builtin.RawUnsafeContinuation, resume bb2
// CHECK:    bb2:
// CHECK:        hop_to_executor {{%.*}} : $Optional<Builtin.Executor>
// CHECK:        [[ANSWER:%.*]] = load [trivial] [[FINAL_BUF]] : $*Bool
// CHECK:        fix_lifetime [[EXTEND2]] : $CostcoManager
// CHECK:        destroy_value [[EXTEND2]] : $CostcoManager
// CHECK:        fix_lifetime [[EXTEND1]] : $NSObject
// CHECK:        destroy_value [[EXTEND1]] : $NSObject
// CHECK:        return [[ANSWER]] : $Bool
// CHECK:  }
func checkCostcoMembership() async -> Bool {
  return await CostcoManager.shared().isCustomerEnrolled(inExecutiveProgram: Person.asCustomer())
}

// rdar://97646309 -- lookup and direct call of an optional global-actor constrained method would crash in SILGen
@MainActor
@objc protocol OptionalMemberLookups {
  @objc optional func generateMaybe() async
}

extension OptionalMemberLookups {
  // CHECK-LABEL: sil hidden [ossa] @$s18objc_async_checked21OptionalMemberLookupsPAAE19testForceDirectCallyyYaF
  // CHECK:         [[SELF:%[0-9]+]] = copy_value {{.*}} : $Self
  // CHECK:         [[METH:%[0-9]+]] = objc_method {{.*}} : $Self, #OptionalMemberLookups.generateMaybe!foreign : <Self where Self : OptionalMemberLookups> (Self) -> () async -> (), $@convention(objc_method) (@convention(block) () -> (), Self) -> ()
  // CHECK:         [[UNSAFE_CONT:%.*]] = struct $UnsafeContinuation<(), Never> ({{.*}} : $Builtin.RawUnsafeContinuation)
  // CHECK:         [[BLOCK_STORAGE:%.*]] = alloc_stack $@block_storage Any
  // CHECK:         [[PROJECTED:%.*]] = project_block_storage [[BLOCK_STORAGE]] : $*@block_storage Any
  // CHECK:         [[CHECKED_CONT_SLOT:%.*]] = init_existential_addr [[PROJECTED]] : $*Any, $CheckedContinuation<(), Never>
  // CHECK:         [[CHECKED_CONT_INIT_FN:%.*]] = function_ref @$ss26_createCheckedContinuationyScCyxs5NeverOGSccyxACGnlF : $@convention(thin) <τ_0_0> (UnsafeContinuation<τ_0_0, Never>) -> @out CheckedContinuation<τ_0_0, Never>
  // CHECK:         [[CHECKED_CONT:%.*]] = alloc_stack $CheckedContinuation<(), Never>
  // CHECK:         {{.*}} = apply [[CHECKED_CONT_INIT_FN]]<()>([[CHECKED_CONT]], [[UNSAFE_CONT]]) : $@convention(thin) <τ_0_0> (UnsafeContinuation<τ_0_0, Never>) -> @out CheckedContinuation<τ_0_0, Never>
  // CHECK:         copy_addr [take] [[CHECKED_CONT]] to [init] [[CHECKED_CONT_SLOT]] : $*CheckedContinuation<(), Never>
  // CHECK:         = function_ref @$sIeyB_yt18objc_async_checked21OptionalMemberLookupsRzlTz_ : $@convention(c) @pseudogeneric <τ_0_0 where τ_0_0 : OptionalMemberLookups> (@inout_aliasable @block_storage Any) -> ()
  // CHECK:         [[BLOCK:%[0-9]+]] = init_block_storage_header {{.*}} : $*@block_storage Any
  // CHECK:         = apply [[METH]]([[BLOCK]], [[SELF]]) : $@convention(objc_method) (@convention(block) () -> (), Self) -> ()
  // CHECK:         await_async_continuation {{.*}} : $Builtin.RawUnsafeContinuation, resume bb1
  // CHECK:         hop_to_executor {{.*}} : $MainActor
  // CHECK:        } // end sil function '$s18objc_async_checked21OptionalMemberLookupsPAAE19testForceDirectCallyyYaF'
  func testForceDirectCall() async -> Void {
    await self.generateMaybe!()
  }
}
