// RUN: %empty-directory(%t)
// RUN: %target-swift-frontend-emit-module -emit-module-path %t/FakeDistributedActorSystems.swiftmodule -module-name FakeDistributedActorSystems -disable-availability-checking %S/Inputs/FakeDistributedActorSystems.swift

// RUN: %target-swift-frontend -emit-silgen %s -module-name test -swift-version 5 -disable-availability-checking -I %t | %FileCheck %s

// REQUIRES: concurrency
// REQUIRES: asserts
// REQUIRES: distributed

import Distributed
import FakeDistributedActorSystems

typealias DefaultDistributedActorSystem = FakeActorSystem

func takeInheritingAsyncIsolatedAny(@_inheritActorContext fn: @escaping @isolated(any) @Sendable () async -> ()) {}

// CHECK-LABEL: sil hidden [distributed] [ossa] @$s4test2DAC0A20DistributedIsolationyyF
// CHECK:         // function_ref closure #1
// CHECK-NEXT:    [[CLOSURE_FN:%.*]] = function_ref @$s4test2DAC0A20DistributedIsolationyyFyyYaYbcfU_ : $@convention(thin) @Sendable @async (@guaranteed Optional<any Actor>, @sil_isolated @guaranteed DA) -> ()
// CHECK-NEXT:    [[CAPTURE:%.*]] = copy_value %0 : $DA
// CHECK-NEXT:    [[CAPTURE_FOR_ISOLATION:%.*]] = copy_value [[CAPTURE]] : $DA
//   The conformance here is special, but we don't record that in the printed SIL.
// CHECK-NEXT:    [[ISOLATION_OBJECT:%.*]] = init_existential_ref [[CAPTURE_FOR_ISOLATION]] : $DA : $DA, $any Actor
// CHECK-NEXT:    [[ISOLATION:%.*]] = enum $Optional<any Actor>, #Optional.some!enumelt, [[ISOLATION_OBJECT]] : $any Actor
// CHECK-NEXT:    [[CLOSURE:%.*]] = partial_apply [callee_guaranteed] [isolated_any] [[CLOSURE_FN]]([[ISOLATION]], [[CAPTURE]])
// CHECK-NEXT:    // function_ref
// CHECK-NEXT:    [[TAKE_FN:%.*]] = function_ref @$s4test30takeInheritingAsyncIsolatedAny2fnyyyYaYbYAc_tF
// CHECK-NEXT:    apply [[TAKE_FN]]([[CLOSURE]])
// CHECK-NEXT:    destroy_value [[CLOSURE]]
distributed actor DA {
  distributed func testDistributedIsolation() {
    takeInheritingAsyncIsolatedAny {
      await self.asyncAction()
    }
  }

  func asyncAction() async {}
}
