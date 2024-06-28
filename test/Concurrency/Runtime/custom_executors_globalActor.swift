// RUN: %target-run-simple-swift( %import-libdispatch -strict-concurrency=complete -parse-as-library) | %FileCheck %s

// REQUIRES: concurrency
// REQUIRES: executable_test
// REQUIRES: libdispatch
// UNSUPPORTED: freestanding

// UNSUPPORTED: back_deployment_runtime
// REQUIRES: concurrency_runtime

import Dispatch

let globalQueue = DispatchQueue(label: "SimpleQueue")

@available(SwiftStdlib 6.0, *)
final class NaiveQueueExecutor: SerialExecutor {
  public func enqueue(_ unowned: UnownedJob) {
    globalQueue.sync {
      unowned.runSynchronously(on: self.asUnownedSerialExecutor())
    }
  }

  public func asUnownedSerialExecutor() -> UnownedSerialExecutor {
    return UnownedSerialExecutor(ordinary: self)
  }

  func checkIsolated() {
    // ok
  }
}

@available(SwiftStdlib 6.0, *)
actor Simple {
  var count = 0
  let exec = NaiveQueueExecutor()

  func report() {
    print("simple.count == \(count)")
    count += 1
  }

  nonisolated var unownedExecutor: UnownedSerialExecutor {
    print("Simple.unownedExecutor")
    return exec.asUnownedSerialExecutor()
  }
}

@globalActor
@available(SwiftStdlib 6.0, *)
actor MyGlobalActor {
  static let simple = Simple()
  static let shared = MyGlobalActor()

  static var sharedUnownedExecutor: UnownedSerialExecutor {
    print("MyGlobalActor.sharedUnownedExecutor")
    return simple.unownedExecutor
  }
  nonisolated var unownedExecutor: UnownedSerialExecutor {
    print("MyGlobalActor.unownedExecutor")
    return Self.simple.unownedExecutor
  }
}

@MyGlobalActor
@available(SwiftStdlib 6.0, *)
final class Custom {
  var count = 0
  let simple = MyGlobalActor.simple

  nonisolated var unownedExecutor: UnownedSerialExecutor {
    return simple.unownedExecutor
  }

  func report() async {
    simple.preconditionIsolated()

    print("custom.count == \(count)")
    count += 1

    await simple.report()
  }
}

@available(SwiftStdlib 6.0, *)
@main struct Main {
  static func main() async {
    print("begin")
    let actor = Custom()
    await actor.report()
    print("end")
  }
}

// CHECK:      begin
// CHECK-NEXT: MyGlobalActor.unownedExecutor
// CHECK-NEXT: Simple.unownedExecutor
// CHECK-NEXT: Simple.unownedExecutor
// CHECK-NEXT: custom.count == 0
// CHECK-NEXT: Simple.unownedExecutor
// CHECK-NEXT: simple.count == 0
// CHECK-NEXT: end
