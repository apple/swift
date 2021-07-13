// RUN: %target-swift-frontend -emit-silgen %s -module-name test -swift-version 5 -enable-experimental-concurrency -parse-stdlib -sil-verify-all | %FileCheck %s --dump-input always
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
  // CHECK: [[ASYNC_LET_START:%.*]] = builtin "startAsyncLetWithLocalBuffer"<Int>({{.*}}, [[BUFFER:%[0-9]+]] : $Builtin.RawPointer)
  async let i = await getInt()

  // CHECK: [[ASYNC_LET_GET:%.*]] = function_ref @swift_asyncLet_get
  // CHECK: apply [[ASYNC_LET_GET]]([[ASYNC_LET_START]], [[BUFFER]])
  // CHECK: [[ADDR:%.*]] = pointer_to_address [[BUFFER]] : $Builtin.RawPointer to [strict] [invariant] $*Int
  // CHECK: [[INT_RESULT_VALUE:%.*]] = load [trivial] [[ADDR]] : $*Int
  return await i

  // CHECK: [[FINISH:%.*]] = function_ref @swift_asyncLet_finish
  // CHECK: apply [[FINISH]]([[ASYNC_LET_START]], [[BUFFER]])
  // CHECK: builtin "endAsyncLetLifetime"([[ASYNC_LET_START]] : $Builtin.RawPointer)
}

func testAsyncLetWithThrows(cond: Bool) async throws -> String {
  async let i = await getInt()
  async let s = await getString()

  if cond {
    throw SomeError.boom
  }

  return await s
}

// CHECK-LABEL: sil hidden [ossa] @$s4test0A14AsyncLetThrowsSSyYaKF : $@convention(thin) @async () -> (@owned String, @error Error) {
func testAsyncLetThrows() async throws -> String {
  async let s = try await getStringThrowingly()

  // CHECK: [[ASYNC_LET_GET_THROWING:%.*]] = function_ref @swift_asyncLet_get_throwing
  // CHECK: try_apply [[ASYNC_LET_GET_THROWING]]
  return try await s
}

// CHECK-LABEL: sil hidden [ossa] @$s4test0A14DecomposeAwait4condSiSb_tYaF : $@convention(thin) @async (Bool) -> Int {
func testDecomposeAwait(cond: Bool) async -> Int {
  // CHECK: [[ASYNC_LET_START:%.*]] = builtin "startAsyncLetWithLocalBuffer"<(Int, String)>({{.*}}, [[BUFFER:%[0-9]+]] : $Builtin.RawPointer)
  async let (i, s) = await getIntAndString()

  if cond {
    // CHECK: [[ASYNC_LET_GET:%.*]] = function_ref @swift_asyncLet_get
    // CHECK: apply [[ASYNC_LET_GET]]([[ASYNC_LET_START]], [[BUFFER]])
    // CHECK: [[ADDR:%.*]] = pointer_to_address [[BUFFER]] : $Builtin.RawPointer to [strict] [invariant] $*(Int, String)
    // CHECK: [[ELT:%.*]] = tuple_element_addr [[ADDR]] : $*(Int, String), 1
    // CHECK: load [copy] [[ELT]] : $*String
    return await Int(s)!
  }

  // CHECK: [[ASYNC_LET_GET:%.*]] = function_ref @swift_asyncLet_get
  // CHECK: apply [[ASYNC_LET_GET]]([[ASYNC_LET_START]], [[BUFFER]])
  // CHECK: [[ADDR:%.*]] = pointer_to_address [[BUFFER]] : $Builtin.RawPointer to [strict] [invariant] $*(Int, String)
  // CHECK: [[ELT:%.*]] = tuple_element_addr [[ADDR]] : $*(Int, String), 0
  // CHECK: load [trivial] [[ELT]] : $*Int
  return await i
  // CHECK: [[FINISH:%.*]] = function_ref @swift_asyncLet_finish
  // CHECK: apply [[FINISH]]([[ASYNC_LET_START]], [[BUFFER]])
  // CHECK: builtin "endAsyncLetLifetime"([[ASYNC_LET_START]] : $Builtin.RawPointer)
}
