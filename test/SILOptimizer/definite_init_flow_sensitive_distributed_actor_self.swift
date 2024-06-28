// RUN: %target-swift-frontend -emit-sil %s -module-name test -swift-version 5 -sil-verify-all | %FileCheck %s
// REQUIRES: concurrency, distributed

import Distributed

@available(SwiftStdlib 5.1, *)
func f(isolatedTo actor: isolated (any Actor)?) async -> Int { 0 }

@available(SwiftStdlib 5.7, *)
distributed actor NotCodableDA<ActorSystem>
  where ActorSystem: DistributedActorSystem<any Codable> {
  let number: Int

  // CHECK-LABEL: sil hidden{{.*}}@$s4test12NotCodableDAC11actorSystemACyxGx_tYacfc : $@convention(method) @async <ActorSystem where ActorSystem : DistributedActorSystem, ActorSystem.SerializationRequirement == any Decodable & Encodable> (@in ActorSystem, @sil_isolated @owned NotCodableDA<ActorSystem>) -> @owned NotCodableDA<ActorSystem> {
  init(actorSystem: ActorSystem) async {
    self.actorSystem = actorSystem

    // First use of #isolation, which is replaced by 'nil'.
    // CHECK: [[ISOLATION_1:%.*]] = enum $Optional<any Actor>, #Optional.none!enumelt
    // CHECK: [[F_1:%.*]] = function_ref @$s4test1f10isolatedToSiScA_pSgYi_tYaF
    // CHECK-NEXT: [[F_RESULT:%.*]] = apply [[F_1]]([[ISOLATION_1]])

    // Assignment to "number" of the result.
    // CHECK: [[NUMBER:%.*]] = ref_element_addr {{%.*}} : $NotCodableDA<ActorSystem>, #NotCodableDA.number
    // CHECK: store [[F_RESULT]] to [[NUMBER]]
    self.number = await f(isolatedTo: #isolation)

    // Second use of #isolation, which uses 'self.asLocalActor''
    // CHECK: [[AS_LOCAL_ACTOR_FN:%.*]] = function_ref @$s11Distributed0A5ActorPAAE07asLocalB0ScA_pvg : $@convention(method) <τ_0_0 where τ_0_0 : DistributedActor> (@sil_isolated @guaranteed τ_0_0) -> @owned any Actor
    // CHECK-NEXT: [[ACTOR_EXISTENTIAL:%.*]] =  apply [[AS_LOCAL_ACTOR_FN]]<NotCodableDA<ActorSystem>>(%1) : $@convention(method) <τ_0_0 where τ_0_0 : DistributedActor> (@sil_isolated @guaranteed τ_0_0) -> @owned any Actor
    // CHECK: [[ISOLATION_2:%.*]] = enum $Optional<any Actor>, #Optional.some!enumelt, [[ACTOR_EXISTENTIAL]]
    // CHECK: [[F_2:%.*]] = function_ref @$s4test1f10isolatedToSiScA_pSgYi_tYaF
    // CHECK-NEXT: apply [[F_2]]([[ISOLATION_2]])

    _ = await f(isolatedTo: #isolation)
  }
}
