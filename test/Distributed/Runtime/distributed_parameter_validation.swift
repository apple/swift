// RUN: %empty-directory(%t)
// RUN: %target-swift-frontend-emit-module -emit-module-path %t/FakeDistributedActorSystems.swiftmodule -module-name FakeDistributedActorSystems -disable-availability-checking %S/../Inputs/FakeDistributedActorSystems.swift
// RUN: %target-build-swift -module-name main  -Xfrontend -disable-availability-checking -j2 -parse-as-library -I %t %s %S/../Inputs/FakeDistributedActorSystems.swift -o %t/a.out
// RUN: %target-codesign %t/a.out
// RUN: %target-run %t/a.out | %FileCheck %s --color --dump-input=always

// REQUIRES: executable_test
// REQUIRES: concurrency
// REQUIRES: distributed

// UNSUPPORTED: use_os_stdlib
// UNSUPPORTED: back_deployment_runtime

import Distributed
typealias DefaultDistributedActorSystem = FakeRoundtripActorSystem

@propertyWrapper
struct Validate {
  // TODO: we can't throw from here, but if we could, this would be ideal for throwing validation
  //       Today we can fatalError here if we wanted to though.
  init(wrappedValue: Int, in range: ClosedRange<Int>) {
    //precondition(range.contains(wrappedValue), "Value [\(wrappedValue)] did not fit required range \(range)")
    if !range.contains(wrappedValue) {
      print("VALIDATION WARNING: Value [\(wrappedValue)] did not fit required range \(range)")
    }
  }

  var wrappedValue: Int {
    12
  }
}

distributed actor CheckTheNums {

  distributed func check(@Validate(in: 1...100) num: Int) {
    let i: Int = num
    _ = i
  }
}

// ==== Execute ----------------------------------------------------------------

func test() async throws {
  let system = DefaultDistributedActorSystem()

  let da = CheckTheNums(actorSystem: system)
//  try await da.check(num: 1000)

  let remote = try CheckTheNums.resolve(id: da.id, using: system)
  try await remote.check(num: 10000)

  // CHECK: > execute distributed target: main.CheckTheNums.check(num:)
  // CHECK: > decode argument: 10000
  // CHECK: VALIDATION WARNING: Value [10000] did not fit required range 1...100

  // CHECK: OK
  print("OK")
}

@main struct Main {
  static func main() async throws {
    try await test()
  }
}
