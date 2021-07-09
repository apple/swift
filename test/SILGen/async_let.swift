// RUN: %target-swift-frontend -emit-silgen %s -module-name test -swift-version 5 -enable-experimental-concurrency -parse-stdlib -sil-verify-all | %FileCheck %s --dump-input fail
// REQUIRES: concurrency

import Swift
import _Concurrency


func getInt() async -> Int { 0 }
func getString() async -> String { "" }
func getStringThrowingly() async throws -> String { "" }
func getIntAndString() async -> (Int, String) { (5, "hello") }

enum SomeError: Error {
  case boom
}

// CHECK-LABEL: sil hidden [ossa] @$s4test0A11AsyncLetIntSiyYaF : $@convention(thin) @async () -> Int
func testAsyncLetInt() async -> Int {
  // CHECK: [[I:%.*]] = mark_uninitialized [var] %0
  // CHECK: [[OPTIONS_ARG:%.*]] = enum $Optional<Builtin.RawPointer>, #Optional.none!enumelt
  // CHECK: [[CLOSURE:%.*]] = function_ref @$s4test0A11AsyncLetIntSiyYaFSiyYaYbcfu_ : $@convention(thin) @Sendable @async () -> Int
  // CHECK: [[THICK_CLOSURE:%.*]] = thin_to_thick_function [[CLOSURE]] : $@convention(thin) @Sendable @async () -> Int to $@Sendable @async @callee_guaranteed () -> Int
  // CHECK: [[REABSTRACT_THUNK:%.*]] = function_ref @$sSiIeghHd_Sis5Error_pIegHrzo_TR : $@convention(thin) @async (@guaranteed @Sendable @async @callee_guaranteed () -> Int) -> (@out Int, @error Error)
  // CHECK: [[REABSTRACT_CLOSURE:%.*]] = partial_apply [callee_guaranteed] [[REABSTRACT_THUNK]]([[THICK_CLOSURE]]) : $@convention(thin) @async (@guaranteed @Sendable @async @callee_guaranteed () -> Int) -> (@out Int, @error Error)
  // CHECK: [[ESCAPING_CLOSURE:%.*]] = convert_function [[REABSTRACT_CLOSURE]] : $@async @callee_guaranteed () -> (@out Int, @error Error) to $@async @callee_guaranteed @substituted <τ_0_0> () -> (@out τ_0_0, @error Error) for <Int>
  // CHECK: [[CLOSURE_ARG:%.*]] = convert_escape_to_noescape [not_guaranteed] [[ESCAPING_CLOSURE]] : $@async @callee_guaranteed @substituted <τ_0_0> () -> (@out τ_0_0, @error Error) for <Int> to $@noescape @async @callee_guaranteed @substituted <τ_0_0> () -> (@out τ_0_0, @error Error) for <Int>
  // CHECK: [[ASYNC_LET_START:%.*]] = builtin "startAsyncLet"<Int>([[OPTIONS_ARG]] : $Optional<Builtin.RawPointer>, [[CLOSURE_ARG]] : $@noescape @async @callee_guaranteed @substituted <τ_0_0> () -> (@out τ_0_0, @error Error) for <Int>) : $Builtin.RawPointer
  async let i = await getInt()

  // CHECK: [[ASYNC_LET_GET:%.*]] = function_ref @swift_asyncLet_wait_with_options : $@convention(thin) @async <τ_0_0> (Builtin.RawPointer, Optional<Builtin.RawPointer>) -> @out τ_0_0
  // CHECK: [[INT_RESULT:%.*]] = alloc_stack $Int
  // CHECK: apply [[ASYNC_LET_GET]]<Int>([[INT_RESULT]], [[ASYNC_LET_START]], [[TASK_OPTS:%.*]]) : $@convention(thin) @async <τ_0_0> (Builtin.RawPointer, Optional<Builtin.RawPointer>) -> @out τ_0_0
  // CHECK: [[INT_RESULT_VALUE:%.*]] = load [trivial] [[INT_RESULT]] : $*Int
  // CHECK: assign [[INT_RESULT_VALUE]] to [[I]] : $*Int
  return await i

  // CHECK: [[ASYNC_LET_END:%.*]] = builtin "endAsyncLet"([[ASYNC_LET_START]] : $Builtin.RawPointer, [[TASK_OPTS:%.*]] : $Optional<Builtin.RawPointer>) : $()
}

func testAsyncLetWithThrows(cond: Bool) async throws -> String {
  async let i = await getInt()
  async let s = await getString()

  if cond {
    throw SomeError.boom
  }

  _ = await i
  return await s
}

// CHECK-LABEL: sil hidden [ossa] @$s4test0A14AsyncLetThrowsSSyYaKF : $@convention(thin) @async () -> (@owned String, @error Error) {
func testAsyncLetThrows() async throws -> String {
  async let s = try await getStringThrowingly()

  // CHECK: [[ASYNC_LET_WAIT_THROWING:%.*]] = function_ref @swift_asyncLet_wait_throwing_with_options : $@convention(thin) @async <τ_0_0> (Builtin.RawPointer, Optional<Builtin.RawPointer>) -> (@out τ_0_0, @error Error)
  // CHECK: try_apply [[ASYNC_LET_WAIT_THROWING]]<String>
  return try await s
}

// CHECK-LABEL: sil hidden [ossa] @$s4test0A14DecomposeAwait4condSiSb_tYaF : $@convention(thin) @async (Bool) -> Int {
func testDecomposeAwait(cond: Bool) async -> Int {
  // CHECK: [[I_VAR:%.*]] = alloc_stack $Int, let, name "i"
  // CHECK: [[I:%.*]] = mark_uninitialized [var] [[I_VAR]] : $*Int
  // CHECK: [[S_VAR:%.*]] = alloc_stack $String, let, name "s"
  // CHECK: [[S:%.*]] = mark_uninitialized [var] [[S_VAR]] : $*String
  async let (i, s) = await getIntAndString()

  if cond {
    // CHECK: [[ASYNC_LET_GET:%.*]] = function_ref @swift_asyncLet_wait_with_options : $@convention(thin) @async <τ_0_0> (Builtin.RawPointer, Optional<Builtin.RawPointer>) -> @out τ_0_0
    // CHECK: [[TUPLE_RESULT:%.*]] = alloc_stack $(Int, String)
    // CHECK: apply [[ASYNC_LET_GET]]<(Int, String)>([[TUPLE_RESULT]], {{%.*}}, [[TASK_OPTS:%.*]]) : $@convention(thin) @async <τ_0_0> (Builtin.RawPointer, Optional<Builtin.RawPointer>) -> @out τ_0_0
    // CHECK: [[TUPLE_RESULT_VAL:%.*]] = load [take] [[TUPLE_RESULT]] : $*(Int, String)
    // CHECK: ([[FIRST_VAL:%.*]], [[SECOND_VAL:%.*]]) = destructure_tuple [[TUPLE_RESULT_VAL]] : $(Int, String)
    // CHECK: assign [[FIRST_VAL]] to [[I]] : $*Int
    // CHECK: assign [[SECOND_VAL]] to [[S]] : $*String
    return await Int(s)!
  }

  return await i
}
