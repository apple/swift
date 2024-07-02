// RUN: %target-run-simple-swift( -Xfrontend -disable-availability-checking %import-libdispatch -parse-as-library )

// REQUIRES: executable_test
// REQUIRES: concurrency
// REQUIRES: libdispatch

// rdar://76038845
// REQUIRES: concurrency_runtime
// UNSUPPORTED: back_deployment_runtime
// UNSUPPORTED: back_deploy_concurrency

// rdar://101077408 – Temporarily disable on watchOS & iOS simulator
// UNSUPPORTED: DARWIN_SIMULATOR=watchos
// UNSUPPORTED: DARWIN_SIMULATOR=ios
// UNSUPPORTED: DARWIN_SIMULATOR=tvos
// UNSUPPORTED: single_threaded_concurrency

import StdlibUnittest
import Dispatch
#if canImport(Darwin)
import Darwin
#elseif canImport(Glibc)
import Glibc
#elseif canImport(Android)
import Android
#elseif os(WASI)
import WASILibc
#elseif os(Windows)
import CRT
import WinSDK
#endif

func loopUntil(priority: TaskPriority) async {
  while (Task.currentPriority != priority) {
    await Task.sleep(1_000_000_000)
  }
}

func print(_ s: String = "") {
  fputs("\(s)\n", stderr)
}

func expectedBasePri(priority: TaskPriority) -> TaskPriority {
  let basePri = Task.basePriority!

  print("Testing basePri matching expected pri - \(basePri) == \(priority)")
  expectEqual(basePri, priority)
  return basePri
}

func expectedCurrentPri(priority: TaskPriority) -> TaskPriority {
  let curPri = Task.currentPriority
  print("Testing curPri matching expected pri - \(curPri) == \(priority)")
  expectEqual(curPri, priority)
  return curPri
}

func testNestedTaskPriority(basePri: TaskPriority, curPri: TaskPriority) async {
    let _ = expectedBasePri(priority: basePri)
    let _ = expectedCurrentPri(priority: curPri)
}

@main struct Main {
  static func main() async {

    let top_level = detach { /* To detach from main actor when running work */

      let tests = TestSuite("Task base priority")
      if #available(SwiftStdlib 5.1, *) {

        tests.test("Structured concurrency base priority propagation") {
          let task = Task(priority: .background) {
            await loopUntil(priority: .default)

            let basePri = expectedBasePri(priority: .background)
            let curPri = expectedCurrentPri(priority: .default)

            // Structured concurrency via async let, escalated priority of
            // parent should propagate
            print("Testing propagation for async let structured concurrency child")
            async let child = testNestedTaskPriority(basePri: basePri, curPri: curPri)
            await child

            let dispatchGroup = DispatchGroup()
            // Structured concurrency via task groups, escalated priority should
            // propagate
            await withTaskGroup(of: Void.self, returning: Void.self) { group in
              dispatchGroup.enter()
              group.addTask {
                print("Testing propagation for task group regular child")
                let _ = await testNestedTaskPriority(basePri: basePri, curPri: curPri)
                dispatchGroup.leave()
                return
              }

              dispatchGroup.enter()
              group.addTask(priority: .utility) {
                print("Testing propagation for task group child with specified priority")
                let _ = await testNestedTaskPriority(basePri: .utility, curPri: curPri)
                dispatchGroup.leave()
                return
              }

              // Wait for child tasks to finish running, don't await since that
              // will escalate them
              dispatchGroup.wait()
            }
          }

          await task.value // Escalate task BG->DEF
        }

        tests.test("Unstructured base priority propagation") {
          let task = Task(priority : .background) {
            await loopUntil(priority: .default)

            let basePri = expectedBasePri(priority: .background)
            let _ = expectedCurrentPri(priority: .default)

            let group = DispatchGroup()

            // Create an unstructured task
            group.enter()
            let _ = Task {
              let _ = await testNestedTaskPriority(basePri: basePri, curPri: basePri)
              group.leave()
              return
            }

            // Wait for unstructured task to finish running, don't await it
            // since that will escalate
            group.wait()
          }

          await task.value // Escalate task BG->DEF
        }

      }
      await runAllTestsAsync()
    }

    await top_level.value
  }
}
