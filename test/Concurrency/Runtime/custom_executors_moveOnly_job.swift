// RUN: %target-run-simple-swift( -Xfrontend -disable-availability-checking %import-libdispatch -parse-as-library) | %FileCheck %s

// REQUIRES: concurrency
// REQUIRES: executable_test

// rdar://106849189 move-only types should be supported in freestanding mode
// UNSUPPORTED: freestanding

// UNSUPPORTED: back_deployment_runtime
// REQUIRES: concurrency_runtime

@available(*, deprecated, message: "Test type to verify deprecated API still works")
final class InlineExecutor_UnownedJob: SerialExecutor, CustomStringConvertible {
  public func enqueue(_ job: UnownedJob) {
    job.runSynchronously(on: self.asUnownedSerialExecutor())
  }

  var description: Swift.String {
    "\(Self.self)()"
  }
}

@available(*, deprecated, message: "Test type to verify deprecated API still works")
final class InlineExecutor_Job: SerialExecutor, CustomStringConvertible {
  public func enqueue(_ job: __owned Job) {
    job.runSynchronously(on: self.asUnownedSerialExecutor())
  }

  var description: Swift.String {
    "\(Self.self)()"
  }
}

final class InlineExecutor_ExecutorJob: SerialExecutor, CustomStringConvertible {
  public func enqueue(_ job: __owned ExecutorJob) {
    job.runSynchronously(on: self.asUnownedSerialExecutor())
  }

  var description: Swift.String {
    "\(Self.self)()"
  }
}

let inlineExecutor_UnownedJob = InlineExecutor_UnownedJob()
let inlineExecutor_Job = InlineExecutor_Job()
let inlineExecutor_ExecutorJob = InlineExecutor_ExecutorJob()

actor Custom {
  var count = 0

  let selectedExecutor: any SerialExecutor

  nonisolated var unownedExecutor: UnownedSerialExecutor {
    print("unownedExecutor: \(self.selectedExecutor)")
    return selectedExecutor.asUnownedSerialExecutor()
  }

  init(selectedExecutor: some SerialExecutor) {
    self.selectedExecutor = selectedExecutor
  }

  func report() async {
    print("custom.count == \(count)")
    count += 1
  }
}

@available(SwiftStdlib 5.1, *)
@main struct Main {
  static func main() async {
    print("begin - unowned")
    let one = Custom(selectedExecutor: inlineExecutor_UnownedJob)
    await one.report()
    await one.report()

    print("begin - job")
    let two = Custom(selectedExecutor: inlineExecutor_Job)
    await two.report()
    await two.report()

    print("begin - executor job")
    let three = Custom(selectedExecutor: inlineExecutor_ExecutorJob)
    await three.report()
    await three.report()

    print("end")
  }
}

// CHECK:      begin - unowned
// CHECK-NEXT: unownedExecutor: InlineExecutor_UnownedJob
// CHECK-NEXT: custom.count == 0
// CHECK-NEXT: unownedExecutor: InlineExecutor_UnownedJob
// CHECK-NEXT: custom.count == 1

// CHECK:      begin - job
// CHECK-NEXT: unownedExecutor: InlineExecutor_Job
// CHECK-NEXT: custom.count == 0
// CHECK-NEXT: unownedExecutor: InlineExecutor_Job
// CHECK-NEXT: custom.count == 1

// CHECK:      begin - executor job
// CHECK-NEXT: unownedExecutor: InlineExecutor_ExecutorJob
// CHECK-NEXT: custom.count == 0
// CHECK-NEXT: unownedExecutor: InlineExecutor_ExecutorJob
// CHECK-NEXT: custom.count == 1
// CHECK-NEXT: end
