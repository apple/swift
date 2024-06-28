// RUN: %target-swift-frontend -emit-silgen %s -module-name test -swift-version 5 | %FileCheck %s
// REQUIRES: concurrency

@available(SwiftStdlib 5.1, *)
func f(isolatedTo actor: isolated (any Actor)?) async -> Int { 0 }

@available(SwiftStdlib 5.1, *)
actor A {
  let number: Int

  // CHECK-LABEL: sil hidden{{.*}}[ossa] @$s4test1ACACyYacfc : $@convention(method) @async (@sil_isolated @owned A) -> @owned A
  init() async {
    // First use of #isolation.
    // CHECK: [[ISOLATION_1:%.*]] = builtin "flowSensitiveSelfIsolation"<A>
    // CHECK: [[F_1:%.*]] = function_ref @$s4test1f10isolatedToSiScA_pSgYi_tYaF
    // CHECK-NEXT: [[F_RESULT:%.*]] = apply [[F_1]]([[ISOLATION_1]])

    // Assignment to "number" of the result.
    // CHECK: [[NUMBER:%.*]] = ref_element_addr {{%.*}} : $A, #A.number
    // CHECK: assign [[F_RESULT]] to [[NUMBER]]
    self.number = await f(isolatedTo: #isolation)

    // Second use of #isolation.
    // CHECK: [[ISOLATION_2:%.*]] = builtin "flowSensitiveSelfIsolation"<A>
    // CHECK: [[F_2:%.*]] = function_ref @$s4test1f10isolatedToSiScA_pSgYi_tYaF
    // CHECK-NEXT: apply [[F_2]]([[ISOLATION_2]])
    _ = await f(isolatedTo: #isolation)
  }
}
