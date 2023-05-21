// RUN: %empty-directory(%t)
// RUN: %target-swift-frontend-emit-module -emit-module-path %t/FakeDistributedActorSystems.swiftmodule -module-name FakeDistributedActorSystems -disable-availability-checking %S/../Inputs/FakeDistributedActorSystems.swift
// RUN: %target-build-swift -module-name main -O -Xfrontend -disable-availability-checking -j2 -parse-as-library -I %t %s %S/../Inputs/FakeDistributedActorSystems.swift -o %t/a.out
// RUN: %target-codesign %t/a.out
// RUN: %target-run %t/a.out | %FileCheck %s --color --dump-input=always

// REQUIRES: executable_test
// REQUIRES: concurrency
// REQUIRES: distributed

// rdar://76038845
// UNSUPPORTED: use_os_stdlib
// UNSUPPORTED: back_deployment_runtime

// FIXME(distributed): Distributed actors currently have some issues on windows, isRemote always returns false. rdar://82593574
// UNSUPPORTED: OS=windows-msvc

import Distributed
import FakeDistributedActorSystems

typealias DefaultDistributedActorSystem = FakeRoundtripActorSystem

final class SomeClass<T>: Sendable, Codable {
  let file: String
  let line: UInt
  init(file: String = #fileID, line: UInt = #line) {
    self.file = file
    self.line = line
    print("SomeClass: init @ \(file):\(line)")
  }
  deinit {
    print("SomeClass: deinit @ \(file):\(line)")
  }
}

struct S<T> : Codable {
  var data: SomeClass<T>
}

distributed actor Greeter {
  distributed func test1<T>(_ value: SomeClass<T>) {
  }
  distributed func test2<T>(_ value: S<T>) {}
}

func test() async throws {
  let system = DefaultDistributedActorSystem()
  defer { system.shutdown() }

  let local = Greeter(actorSystem: system)
  let ref = try Greeter.resolve(id: local.id, using: system)

  try await ref.test1(SomeClass<Int>())
  // CHECK: SomeClass: init
  // CHECK: SomeClass: deinit

  try await ref.test2(S(data: SomeClass<Int>()))
  // CHECK: SomeClass: init
  // CHECK: SomeClass: deinit
}

@main struct Main {
  static func main() async {
    try! await test()
  }
}
