// RUN: %target-run-simple-swift( -plugin-path %swift-plugin-dir -Xfrontend -disable-availability-checking -parse-as-library %import-libdispatch) | %FileCheck %s --dump-input=always

// REQUIRES: executable_test
// REQUIRES: concurrency
// REQUIRES: libdispatch
// REQUIRES: concurrency_runtime
// UNSUPPORTED: back_deployment_runtime

@available(SwiftStdlib 5.1, *)
enum TL {
  @TaskLocal
  static var number: Int = 2
}

// ==== ------------------------------------------------------------------------

func bindAroundGroupAddTask() async {
  await TL.$number.withValue(1111) { // ok
    await withTaskGroup(of: Int.self) { group in
      TL.$number.withValue(2222) { // this is OK, there's no addTask being wrapped
        print("Survived, inside withValue, value: \(TL.number)") // CHECK: Survived, inside withValue, value: 2222
      }

      group.addTask {
        print("Survived, inside addTask, value: \(TL.number)") // CHECK: Survived, inside addTask, value: 1111
        return TL.number
      }
    }
    print("Survived, done") // CHECK: Survived, done
  }
}

@main struct Main {
  static func main() async {
    await bindAroundGroupAddTask()
  }
}
