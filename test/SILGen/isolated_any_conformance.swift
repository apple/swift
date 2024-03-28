// RUN: %target-swift-frontend -emit-silgen -enable-experimental-feature IsolatedAny %s -module-name test -swift-version 6 -disable-availability-checking | %FileCheck %s
// REQUIRES: concurrency
// REQUIRES: asserts

// rdar://125394096.  This was a source compatibility failure in which adding
// @isolated(any) to TaskGroup.addTask caused problems with a project that
// introduced a protocol for different task group types.

struct A<T> {
  func enqueue(operation: @escaping @isolated(any) () async -> T) {}
}

protocol Enqueuer {
  associatedtype Result
  func enqueue(operation: @escaping @Sendable () async -> Result)
}

extension A : Enqueuer {}

// CHECK-LABEL: sil private [transparent] [thunk] [ossa] @$s4test1AVyxGAA8EnqueuerA2aEP7enqueue9operationy6ResultQzyYaYbc_tFTW :
// CHECK:       bb0(%0 : @guaranteed $@Sendable @async @callee_guaranteed @substituted <τ_0_0> () -> @out τ_0_0 for <τ_0_0>, %1 : $*A<τ_0_0>):
// CHECK-NEXT:  [[CONVERTED_FN:%.*]] = convert_function %0 : $@Sendable @async @callee_guaranteed @substituted <τ_0_0> () -> @out τ_0_0 for <τ_0_0> to $@Sendable @async @callee_guaranteed () -> @out τ_0_0
// CHECK-NEXT:  [[ISOLATION:%.*]] = enum $Optional<any Actor>, #Optional.none
// CHECK-NEXT:  [[FN_COPY:%.*]] = copy_value [[CONVERTED_FN]] : $@Sendable @async @callee_guaranteed () -> @out τ_0_0
// CHECK-NEXT:  // function_ref
// CHECK-NEXT:  [[THUNK:%.*]] = function_ref @$sxIeghHr_xIeAghHr_lTR
// CHECK-NEXT:  partial_apply [callee_guaranteed] [isolated_any] [[THUNK]]<τ_0_0>([[ISOLATION]], [[FN_COPY]])
