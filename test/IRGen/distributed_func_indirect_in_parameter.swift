// RUN: %target-swift-frontend -emit-ir %s -Xllvm -sil-print-function=takeLarge -swift-version 5 -disable-availability-checking 2>&1 | %IRGenFileCheck %s --dump-input=always
// UNSUPPORTED: back_deploy_concurrency
// REQUIRES: concurrency
// REQUIRES: distributed

import Distributed

// This struct must be "very large" in order to cause it to be passed as `indirect @in`.
// Specifically, this needs to exercise the DistributedAccessor::decodeArgument paths for `Indirect_In` parameter convention.
public struct LargeValue : Codable {
  let field1 : Int64 = 1
  let field2 : Int64 = 2
  let field3 : Int64 = 3
  let field4 : Int64 = 4
  let field5 : Int64 = 5
  let field6 : Int64 = 6
  let field7 : Int64 = 7
  let field8 : Int64 = 8
}

distributed actor D {
  typealias ActorSystem = LocalTestingDistributedActorSystem

  // CHECK: sil hidden [distributed] @takeLarge : $@convention(method) (@in_guaranteed LargeValue, @guaranteed D) -> () {
  @_silgen_name("takeLarge")
  distributed func takeLarge(_ l: LargeValue) {}
}
