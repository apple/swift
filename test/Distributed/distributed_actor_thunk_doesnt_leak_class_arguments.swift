// RUN: %target-swift-frontend-emit-module -emit-module-path %t/FakeDistributedActorSystems.swiftmodule -module-name FakeDistributedActorSystems -disable-availability-checking %S/Inputs/FakeDistributedActorSystems.swift
// RUN: %target-swift-frontend -module-name no_to_arg_leaks -emit-irgen -disable-availability-checking -I %t 2>&1 %s | %IRGenFileCheck %s -check-prefix CHECK-%target-import-type

// UNSUPPORTED: back_deploy_concurrency
// REQUIRES: concurrency
// REQUIRES: distributed

// REQUIRES: CPU=x86_64 || CPU=arm64

// UNSUPPORTED: OS=windows-msvc

import Distributed
import FakeDistributedActorSystems

@available(SwiftStdlib 5.5, *)
typealias DefaultDistributedActorSystem = FakeActorSystem

final class SomeClass<T>: Sendable, Codable {
  init() {}
}

struct S<T> : Codable {
  var data: SomeClass<T>
}

distributed actor Greeter {
  // CHECK-LABEL: define linkonce_odr hidden swifttailcc void @"$s15no_to_arg_leaks7GreeterC5test1yyAA9SomeClassCyxGYaKlFTETF"
  // CHECK: [[ARG_ADDR:%.*]] = bitcast i8* {{.*}} to %T15no_to_arg_leaks9SomeClassC**
  // CHECK: %destroy = bitcast i8* {{.*}} to void (%swift.opaque*, %swift.type*)*
  // CHECK-NEXT: [[OPAQUE_ARG_ADDR:%.*]] = bitcast %T15no_to_arg_leaks9SomeClassC** [[ARG_ADDR]] to %swift.opaque*
  // CHECK-NEXT: call void %destroy(%swift.opaque* noalias [[OPAQUE_ARG_ADDR]], %swift.type* %arg_type)
  distributed func test1<T>(_: SomeClass<T>) {
  }

  // CHECK-LABEL: define linkonce_odr hidden swifttailcc void @"$s15no_to_arg_leaks7GreeterC5test2yyAA1SVyxGYaKlFTETF"
  // CHECK: [[ARG_ADDR:%.*]] = bitcast i8* {{.*}} to %T15no_to_arg_leaks1SV*
  // CHECK: %destroy = bitcast i8* {{.*}} to void (%swift.opaque*, %swift.type*)*
  // CHECK-NEXT: [[OPAQUE_ARG_ADDR:%.*]] = bitcast %T15no_to_arg_leaks1SV* [[ARG_ADDR]] to %swift.opaque*
  // CHECK-NEXT: call void %destroy(%swift.opaque* noalias [[OPAQUE_ARG_ADDR]], %swift.type* %arg_type)
  distributed func test2<T>(_: S<T>) {}
}

func test() async throws {
  let system = DefaultDistributedActorSystem()

  let local = Greeter(actorSystem: system)
  let ref = try Greeter.resolve(id: local.id, using: system)

  try await ref.test1(SomeClass<Int>())
  try await ref.test2(S(data: SomeClass<Int>()))
}
