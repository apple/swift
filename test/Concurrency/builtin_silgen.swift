// RUN: %target-swift-frontend  -disable-availability-checking %s -parse-as-library -enable-builtin-module -emit-sil -o - | %FileCheck %s

// REQUIRES: concurrency

import Builtin

@MainActor
func suspend() async {}

// Builtin.hopToActor should generate a mandatory hop_to_executor
// before releasing the actor and reaching a suspend.
//
// CHECK-LABEL: sil private @$s14builtin_silgen11runDetachedyyFyyYacfU_ : $@convention(thin) @async @substituted <τ_0_0> (@guaranteed Optional<any Actor>) -> @out τ_0_0 for <()> {
// CHECK:   [[ACTOR:%.*]] = apply {{%.*}}({{%.*}}) : $@convention(method) (@thick MainActor.Type) -> @owned MainActor
// CHECK:   hop_to_executor [mandatory] [[ACTOR]] : $MainActor
// CHECK:   strong_release [[ACTOR]] : $MainActor
// CHECK:   apply %{{.*}}() : $@convention(thin) @async () -> ()
@available(SwiftStdlib 5.1, *)
func runDetached() {
  Task.detached {
    Builtin.hopToActor(MainActor.shared)
    await suspend()
  }
}

// CHECK-LABEL: sil{{.*}} @$s14builtin_silgen13testRunInlineyxxyYaXElF : {{.*}} {
// CHECK:       {{bb[0-9]+}}([[RESULT:%[^,]+]] : $*T, [[CLOSURE:%[^,]+]] : $@noescape @async @callee_guaranteed @substituted <τ_0_0> () -> @out τ_0_0 for <T>):
// CHECK:         builtin "taskRunInline"<T>([[RESULT]] : $*T, [[CLOSURE]] : $@noescape @async @callee_guaranteed @substituted <τ_0_0> () -> @out τ_0_0 for <T>) : $()
// CHECK-LABEL: } // end sil function '$s14builtin_silgen13testRunInlineyxxyYaXElF'
@available(SwiftStdlib 5.1, *)
func testRunInline<T>(_ cl: () async -> T) -> T {
  return Builtin.taskRunInline(cl)
}
