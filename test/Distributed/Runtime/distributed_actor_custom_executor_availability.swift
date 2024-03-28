// RUN: %empty-directory(%t)
// RUN: %target-swift-frontend-emit-module -emit-module-path %t/FakeDistributedActorSystems.swiftmodule -module-name FakeDistributedActorSystems %S/../Inputs/FakeDistributedActorSystems.swift
// RUN: %target-build-swift -parse-as-library -target %target-swift-abi-5.7-triple -I %t %s %S/../Inputs/FakeDistributedActorSystems.swift %S/../Inputs/CustomSerialExecutorAvailability.swift -o %t/a.out
// RUN: %target-codesign %t/a.out
// RUN: %target-run %t/a.out

// These are the only platforms for which compiling a Swift 5.7 aligned deployment target is possible.
// REQUIRES: OS=macosx || OS=ios || OS=watchos || OS=tvos

// REQUIRES: executable_test
// REQUIRES: concurrency
// REQUIRES: distributed
// REQUIRES: concurrency_runtime
// UNSUPPORTED: back_deployment_runtime

// UNSUPPORTED: back_deploy_concurrency
// UNSUPPORTED: use_os_stdlib
// UNSUPPORTED: freestanding

import StdlibUnittest
import Distributed

@available(SwiftStdlib 5.7, *)
@main struct Main {
  static func main() async {
    if #available(SwiftStdlib 5.9, *) {
      let tests = TestSuite("DistributedActorExecutorAvailability")

      let system = LocalTestingDistributedActorSystem()

      tests.test("5.7 actor, no availability executor property => no custom executor") {
        expectCrashLater(withMessage: "Incorrect actor executor assumption; Expected MainActor executor")
        try! await FiveSevenActor_NothingExecutor(actorSystem: system).test(x: 42)
      }

      tests.test("5.9 actor, no availability executor property => custom executor") {
        try! await FiveNineActor_NothingExecutor(actorSystem: system).test(x: 42)
      }

      tests.test("5.7 actor, 5.9 executor property => no custom executor") {
        expectCrashLater(withMessage: "Incorrect actor executor assumption; Expected MainActor executor")
        try! await FiveSevenActor_FiveNineExecutor(actorSystem: system).test(x: 42)
      }

      await runAllTestsAsync()
    }
  }
}
