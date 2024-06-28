// RUN: %target-swift-frontend -emit-sil %s -module-name test -swift-version 5 -sil-verify-all | %FileCheck %s
// REQUIRES: concurrency

@available(SwiftStdlib 5.1, *)
func f(isolatedTo actor: isolated (any Actor)?) async -> Int { 0 }

@available(SwiftStdlib 5.1, *)
actor A {
  let number: Int

  // CHECK-LABEL: sil hidden{{.*}}@$s4test1ACACyYacfc : $@convention(method) @async (@sil_isolated @owned A) -> @owned A
  init() async {
    // First use of #isolation, which is replaced by 'nil'.
    // CHECK: [[ISOLATION_1:%.*]] = enum $Optional<any Actor>, #Optional.none!enumelt
    // CHECK: [[F_1:%.*]] = function_ref @$s4test1f10isolatedToSiScA_pSgYi_tYaF
    // CHECK-NEXT: [[F_RESULT:%.*]] = apply [[F_1]]([[ISOLATION_1]])

    // Assignment to "number" of the result.
    // CHECK: [[NUMBER:%.*]] = ref_element_addr {{%.*}} : $A, #A.number
    // CHECK: store [[F_RESULT]] to [[NUMBER]]
    self.number = await f(isolatedTo: #isolation)

    // Second use of #isolation, which uses 'self' injected into (any Actor)?.
    // CHECK: [[ACTOR_COPY:%.*]] = end_init_let_ref %0 : $A
    // CHECK: strong_retain [[ACTOR_COPY]] : $A 
    // CHECK: [[ACTOR_EXISTENTIAL:%.*]] = init_existential_ref [[ACTOR_COPY]] : $A : $A, $any Actor
    // CHECK: [[ISOLATION_2:%.*]] = enum $Optional<any Actor>, #Optional.some!enumelt, [[ACTOR_EXISTENTIAL]]
    // CHECK: [[F_2:%.*]] = function_ref @$s4test1f10isolatedToSiScA_pSgYi_tYaF
    // CHECK-NEXT: apply [[F_2]]([[ISOLATION_2]])
    _ = await f(isolatedTo: #isolation)
  }
}
