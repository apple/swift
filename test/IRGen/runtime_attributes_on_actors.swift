// RUN: %empty-directory(%t)
// RUN: %target-swift-frontend -emit-module-path=%t/RAD.swiftmodule -module-name=RAD -enable-experimental-feature RuntimeDiscoverableAttrs -disable-availability-checking %S/Inputs/runtime_attrs.swift
// RUN: %target-swift-frontend -primary-file %s -emit-ir -I %t -swift-version 5 -enable-experimental-feature RuntimeDiscoverableAttrs -disable-availability-checking | %IRGenFileCheck %s

// REQUIRES: asserts
// REQUIRES: OS=macosx
// REQUIRES: concurrency

// CHECK: @"$s28runtime_attributes_on_actors17FlagForAsyncFuncsVAA9TestActorC15asyncExternallyyyKFfaHF"
// CHECK: @"$s28runtime_attributes_on_actors17FlagForAsyncFuncsVAA9TestActorC11doSomethingyyYaKFfaHF"
// CHECK: @"$s28runtime_attributes_on_actors17FlagForAsyncFuncsVAA9TestActorC11doSomethingyySiYaFfaHF"
// CHECK: @"$s28runtime_attributes_on_actors17FlagForAsyncFuncsVAA9TestActorC11doSomething_1xySi_SaySiGztYaFfaHF"
// CHECK: @"$s28runtime_attributes_on_actors17FlagForAsyncFuncsVAA06globalG2FnSaySSGyYaFfaHF"

// CHECK: @"$s28runtime_attributes_on_actors17FlagForAsyncFuncsVHa" = internal constant
// CHECK-SAME: i32 5
// CHECK-SAME: ptr @"$s28runtime_attributes_on_actors17FlagForAsyncFuncsVAA9TestActorC15asyncExternallyyyKFfaHF"
// CHECK-SAME: ptr @"$s28runtime_attributes_on_actors17FlagForAsyncFuncsVAA9TestActorC11doSomethingyyYaKFfaHF"
// CHECK-SAME: ptr @"$s28runtime_attributes_on_actors17FlagForAsyncFuncsVAA9TestActorC11doSomethingyySiYaFfaHF"
// CHECK-SAME: ptr @"$s28runtime_attributes_on_actors17FlagForAsyncFuncsVAA9TestActorC11doSomething_1xySi_SaySiGztYaFfaHF"
// CHECK-SAME: ptr @"$s28runtime_attributes_on_actors17FlagForAsyncFuncsVAA06globalG2FnSaySSGyYaFfaHF"

@runtimeMetadata
struct FlagForAsyncFuncs {
  init<Act>(attachedTo: (Act) async throws -> Void) {}
  init<Act>(attachedTo: (Act, Int, inout [Int]) async throws -> Void) {}
  init<Act>(attachedTo: (Act, Int) async -> Void) {}
  init(attachedTo: () async -> [String]) {}
}

actor TestActor {
  // CHECK-LABEL: define hidden swiftcc void @"$s28runtime_attributes_on_actors17FlagForAsyncFuncsVAA9TestActorC15asyncExternallyyyKFfa"(ptr noalias nocapture sret(%T28runtime_attributes_on_actors17FlagForAsyncFuncsVSg) %0)
  @FlagForAsyncFuncs func asyncExternally() throws {
  }

  // CHECK-LABEL: define hidden swiftcc void @"$s28runtime_attributes_on_actors17FlagForAsyncFuncsVAA9TestActorC11doSomethingyyYaKFfa"(ptr noalias nocapture sret(%T28runtime_attributes_on_actors17FlagForAsyncFuncsVSg) %0)
  @FlagForAsyncFuncs func doSomething() async throws {
  }

  // CHECK-LABEL: define hidden swiftcc void @"$s28runtime_attributes_on_actors17FlagForAsyncFuncsVAA9TestActorC11doSomethingyySiYaFfa"(ptr noalias nocapture sret(%T28runtime_attributes_on_actors17FlagForAsyncFuncsVSg) %0)
  @FlagForAsyncFuncs nonisolated func doSomething(_: Int) async {
  }

  // CHECK-LABEL: define hidden swiftcc void @"$s28runtime_attributes_on_actors17FlagForAsyncFuncsVAA9TestActorC11doSomething_1xySi_SaySiGztYaFfa"(ptr noalias nocapture sret(%T28runtime_attributes_on_actors17FlagForAsyncFuncsVSg) %0)
  @FlagForAsyncFuncs func doSomething(_: Int, x: inout [Int]) async {
  }
}

// CHECK-LABEL: define hidden swiftcc void @"$s28runtime_attributes_on_actors17FlagForAsyncFuncsVAA06globalG2FnSaySSGyYaFfa"(ptr noalias nocapture sret(%T28runtime_attributes_on_actors17FlagForAsyncFuncsVSg) %0)
@FlagForAsyncFuncs
func globalAsyncFn() async -> [String] {
  return []
}
