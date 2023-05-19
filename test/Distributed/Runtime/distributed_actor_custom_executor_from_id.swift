// RUN: %empty-directory(%t)
// RUN: %target-swift-frontend-emit-module -emit-module-path %t/FakeDistributedActorSystems.swiftmodule -module-name FakeDistributedActorSystems -disable-availability-checking %S/../Inputs/FakeDistributedActorSystems.swift
// RUN: %target-build-swift -module-name main -Xfrontend -enable-experimental-distributed -Xfrontend -disable-availability-checking -j2 -parse-as-library -I %t %s %S/../Inputs/FakeDistributedActorSystems.swift -o %t/a.out
// RUN: %target-codesign %t/a.out
// RUN: %target-run %t/a.out | %FileCheck %s --color

// REQUIRES: executable_test
// REQUIRES: concurrency
// REQUIRES: distributed

// rdar://76038845
// UNSUPPORTED: use_os_stdlib
// UNSUPPORTED: back_deployment_runtime

// FIXME(distributed): Distributed actors currently have some issues on windows rdar://82593574
// UNSUPPORTED: OS=windows-msvc
// https://github.com/apple/swift/issues/65529
// UNSUPPORTED: single_threaded_concurrency


import Distributed
import FakeDistributedActorSystems

typealias DefaultDistributedActorSystem = FakeRoundtripActorSystem

@available(SwiftStdlib 5.9, *)
distributed actor Worker {
  nonisolated var unownedExecutor: UnownedSerialExecutor {
    print("get unowned 'local' executor via ID")
    return self.id.executorPreference ?? buildDefaultDistributedRemoteActorExecutor(self)
  }

  distributed func test(x: Int) {
    print("executed: \(#function)")
    MainActor.assumeIsolated {
      print("assume: this distributed actor shares executor with MainActor")
    }
    self.assumeIsolated { isolatedSelf in
      // it of course is isolated by "itself"
      print("assume: this distributed actor is isolated by itself")
    }
    print("done executed: \(#function)")
  }
}

extension DefaultDistributedActorSystem.ActorID {
  var executorPreference: UnownedSerialExecutor? {
    MainActor.sharedUnownedExecutor
  }
}

@main struct Main {
  static func main() async {
    let worker = Worker(actorSystem: DefaultDistributedActorSystem())
    // CHECK: | assign id
    // CHECK: | actor ready

    precondition(__isLocalActor(worker), "must be local")

    try! await worker.test(x: 42)
    // CHECK: get unowned 'local' executor
    // CHECK: executed: test(x:)
    // CHECK: assume: this distributed actor shares executor with MainActor
    // CHECK: assume: this distributed actor is isolated by itself
    // CHECK: done executed: test(x:)

    print("OK") // CHECK: OK
  }
}
