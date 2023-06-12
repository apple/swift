// RUN: %empty-directory(%t)
// RUN: %target-build-swift %s -Xfrontend -disable-availability-checking -parse-as-library -o %t/async_task_priority
// RUN: %target-codesign %t/async_task_priority
// RUN: %target-run %t/async_task_priority

// REQUIRES: VENDOR=apple
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

// rdar://107390341 - Temporarily disable for arm64e
// UNSUPPORTED: CPU=arm64e

import Darwin
@preconcurrency import Dispatch
import StdlibUnittest

func loopUntil(priority: TaskPriority) async {
  var currentPriority = Task.currentPriority
  while (currentPriority != priority) {
    print("Current priority = \(currentPriority) != \(priority)")
    await Task.sleep(1_000_000)
    currentPriority = Task.currentPriority
  }
}

func print(_ s: String = "") {
  fputs("\(s)\n", stderr)
}

func expectedBasePri(priority: TaskPriority) -> TaskPriority {
  let basePri = Task.basePriority!
  print("Testing basePri matching expected pri - \(basePri) == \(priority)")
  expectEqual(basePri, priority)
  withUnsafeCurrentTask { unsafeTask in
    guard let unsafeTask else {
      fatalError("Expected to be able to get current task, but could not!")
    }
    // The UnsafeCurrentTask must return the same value
    expectEqual(basePri, unsafeTask.basePriority)
  }

  return basePri
}

func expectedEscalatedPri(priority: TaskPriority) -> TaskPriority {
  let curPri = Task.currentPriority
  print("Testing escalated matching expected pri - \(curPri) == \(priority)")
  expectEqual(curPri, priority)

  return curPri
}

func testNestedTaskPriority(basePri: TaskPriority, curPri: TaskPriority) async {
    let _ = expectedBasePri(priority: basePri)
    let _ = expectedEscalatedPri(priority: curPri)
}

func childTaskWaitingForEscalation(sem: DispatchSemaphore, basePri: TaskPriority, curPri : TaskPriority) async {
    sem.wait() /* Wait to be escalated */
    let _ = await testNestedTaskPriority(basePri: basePri, curPri: curPri)
}

actor Test {
  private var value = 0
  init() { }

  func increment() -> Int {
    let cur = value
    value = value + 1
    return cur
  }

  func blockActorThenIncrement(semToSignal: DispatchSemaphore, semToWait : DispatchSemaphore, priExpected: TaskPriority) -> Int {
    semToSignal.signal()

    semToWait.wait();

    sleep(1)
    // TODO: insert a test to verify that thread priority has actually escalated
    // to match priExpected
    return increment()
  }

}


@main struct Main {
  static func main() async {

    let top_level = detach { /* To detach from main actor when running work */

      let tests = TestSuite("Task Priority manipulations")
      if #available(SwiftStdlib 5.1, *) {

        tests.test("Basic escalation test when task is running") {
            let parentPri = Task.currentPriority

            let sem = DispatchSemaphore(value: 0)
            let task = Task(priority: .background) {
                let _ = expectedBasePri(priority: .background)

                // Wait until task is running before asking to be escalated
                sem.signal()
                sleep(1)

                let _ = expectedEscalatedPri(priority: parentPri)
            }

            // Wait till child runs and asks to be escalated
            sem.wait()
            await task.value
        }

        tests.test("Basic escalation when task is suspended") {
            let parentPri = Task.currentPriority

            let task = Task(priority: .background) {
                await loopUntil(priority: parentPri) /* Suspend task until it is escalated */

                let _ = expectedEscalatedPri(priority: parentPri)
            }
            await task.value // Escalate task BG -> DEF
        }

        tests.test("Structured concurrency priority propagation") {
          let task = Task(priority: .background) {
            await loopUntil(priority: .default)

            let basePri = expectedBasePri(priority: .background)
            let curPri = expectedEscalatedPri(priority: .default)

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

        tests.test("Unstructured tasks priority propagation") {
          let task = Task(priority : .background) {
            await loopUntil(priority: .default)

            let basePri = expectedBasePri(priority: .background)
            let _ = expectedEscalatedPri(priority: .default)

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

        tests.test("Task escalation propagation to SC children") {
            // Create a task tree and then escalate the parent
            let parentPri = Task.currentPriority
            let basePri : TaskPriority = .background

            let sem = DispatchSemaphore(value: 0)
            let sem2 = DispatchSemaphore(value : 0)

            let task = Task(priority: basePri) {

                async let child = childTaskWaitingForEscalation(sem: sem2, basePri: basePri, curPri: parentPri)

                await withTaskGroup(of: Void.self, returning: Void.self) { group in
                    group.addTask {
                        let _ = await childTaskWaitingForEscalation(sem: sem2, basePri: basePri, curPri: parentPri)
                    }
                    group.addTask(priority: .utility) {
                        let _ = await childTaskWaitingForEscalation(sem: sem2, basePri: .utility, curPri: parentPri)
                    }

                    sem.signal() // Ask for escalation after creating full task tree
                    sleep(1)

                    let _ = expectedBasePri(priority: basePri)
                    let _ = expectedEscalatedPri(priority: parentPri)

                    sem2.signal() // Ask async let child to evaluate
                    sem2.signal() // Ask task group child 1 to evaluate
                    sem2.signal() // Ask task group child 2 to evaluate
                }
            }

            // Wait until children are created and then ask for escalation of
            // top level
            sem.wait()
            await task.value
        }

        tests.test("Simple task escalation to a future") {
            let task1Pri: TaskPriority = .background
            let task2Pri: TaskPriority = .utility
            let parentPri: TaskPriority =  Task.currentPriority
            print("Top level task current priority = \(parentPri)")

            //  After task2 has suspended waiting for task1, escalating task2
            //  should cause task1 to escalate

            let task1 = Task(priority: task1Pri) {
                // Wait until task2 has blocked on task1 and escalated it
                sleep(1)
                expectedEscalatedPri(priority: task2Pri)

                // Wait until task2 itself has been escalated
                sleep(5)
                expectedEscalatedPri(priority: parentPri)
            }

            let task2 = Task(priority: task2Pri) {
                await task1.value
            }

            // Wait for task2 and task1 to run and for task2 to now block on
            // task1
            sleep(3)

            await task2.value
        }

        tests.test("Simple task escalation to a future 2") {
            // top level task -> unstructured task2 -> child task -> unstructured
            // task1
            let task1Pri: TaskPriority = .background
            let task2Pri: TaskPriority = .utility
            let parentPri: TaskPriority =  Task.currentPriority
            print("Top level task current priority = \(parentPri)")

            let task1 = Task(priority: task1Pri) {
                await loopUntil(priority: parentPri)
            }

            sleep(1) // Wait for task1 to start running

            let task2 = Task(priority: task2Pri) {
                func childTask() async {
                    await task1.value
                }
                async let child = childTask()
            }

            sleep(1) // Wait for task2 to start running
            await task2.value
        }

        tests.test("Task escalation of a task enqueued on an actor") {
          let task1Pri: TaskPriority = .background
          let task2Pri: TaskPriority = .background
          let parentPri: TaskPriority =  Task.currentPriority

          let sem1 = DispatchSemaphore(value: 0) // to unblock enqueue of task2
          let sem2 = DispatchSemaphore(value: 0)
          let testActor = Test()

          let task1 = Task(priority: task1Pri) {
            expectedBasePri(priority: task1Pri);
            await testActor.blockActorThenIncrement(semToSignal: sem1, semToWait: sem2, priExpected: parentPri);
          }

          sem1.wait() // Wait until task1 is on the actor

          let task2 = Task(priority: task2Pri) {
            expectedBasePri(priority: task2Pri);
            await testActor.increment()
          }

          sleep(1)
          sem2.signal() // task2 is probably enqueued on the actor at this point, unblock task1

          await task2.value // Escalate task2 which should be queued behind task1 on the actor
        }

      }
      await runAllTestsAsync()
    }

    await top_level.value
  }
}
