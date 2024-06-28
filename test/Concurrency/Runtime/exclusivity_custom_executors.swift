// RUN: %target-run-simple-swift(-Xfrontend -disable-availability-checking -parse-as-library)

// REQUIRES: concurrency
// REQUIRES: executable_test

// rdar://106849189 move-only types should be supported in freestanding mode
// UNSUPPORTED: freestanding

// rdar://76038845
// UNSUPPORTED: back_deployment_runtime
// REQUIRES: concurrency_runtime
// UNSUPPORTED: back_deploy_concurrency

// Crash expectations can't be implemented on WASI/WebAssembly.
// UNSUPPORTED: OS=wasi

// This test makes sure that we properly save/restore access when we
// synchronously launch a task from a serial executor. The access from the task
// should be merged into the already created access set while it runs and then
// unmerged afterwards.

import _Concurrency
import StdlibUnittest

#if canImport(Darwin)
import Darwin
#elseif canImport(Glibc)
import Glibc
#elseif canImport(Android)
import Android
#elseif canImport(CRT)
import CRT
#endif

@inlinable
public func debugLog(_ s: String) {
    // Only enable this when debugging test failures against an asserts
    // runtime. Otherwise, the test is noisy and has non-windows
    // dependencies. We need stderr to ensure proper log output interleaving
    // with the runtime's own stderr emitted output.
#if DEBUG_LOGGING
    fputs(s + "\n", stderr)
    fflush(stderr)
#endif
}

@inline(never)
public func withExclusiveAccess<T, U>(to x: inout T, f: (inout T) -> U) -> U {
    debugLog("==> Enter 'withExclusiveAccess'")
    defer { debugLog("==> Exit 'withExclusiveAccess'") }
    return f(&x)
}

@available(SwiftStdlib 5.1, *)
@MainActor @inline(never)
func withExclusiveAccessAsync<T, U>(to x: inout T, f: (inout T) async -> U) async -> U {
    debugLog("==> Enter 'withExclusiveAccessAsync'")
    defer { debugLog("==> Exit 'withExclusiveAccessAsync'") }
    return await f(&x)
}

@available(SwiftStdlib 5.1, *)
public final class MySerialExecutor : SerialExecutor {
    public init() {
        debugLog("==> MySerialExecutor: Creating MySerialExecutor!")
    }
    public static var sharedSerialExecutor = MySerialExecutor()
    public static var sharedUnownedExecutor: UnownedSerialExecutor {
        debugLog("==> MySerialExecutor: Getting Shared Unowned Executor!")
        return UnownedSerialExecutor(ordinary: sharedSerialExecutor)
    }

    public func enqueue(_ job: UnownedJob) {
        debugLog("==> MySerialExecutor: Got an enqueue!")
        // This is the exclusive access that we are going to be swizzling
        // in/out.
        //
        // Make sure we have 2x synchronized to test.
        withExclusiveAccess(to: &global2) { _ in
            withExclusiveAccess(to: &global3) { _ in
                debugLog("==> MySerialExecutor: Inside access!")
                job._runSynchronously(on: asUnownedSerialExecutor())
                debugLog("==> MySerialExecutor: Inside access after run synchronously!")
            }
        }
        debugLog("==> MySerialExecutor: After access, after run synchronously")
    }

    public func asUnownedSerialExecutor() -> UnownedSerialExecutor {
        debugLog("==> MySerialExecutor: Getting Unowned Executor!")
        return UnownedSerialExecutor(ordinary: self)
    }
}

/// A singleton actor whose executor is equivalent to the main
/// dispatch queue.
@available(SwiftStdlib 5.1, *)
@globalActor public final actor MyMainActor: Executor {
    public static let shared = MyMainActor()
    public let executor = MySerialExecutor()

  @inlinable
  public nonisolated var unownedExecutor: UnownedSerialExecutor {
      debugLog("==> MyMainActor: Getting unowned executor!")
      return executor.asUnownedSerialExecutor()
  }

  @inlinable
  public static var sharedUnownedExecutor: UnownedSerialExecutor {
      debugLog("==> MyMainActor: Getting shared unowned executor!")
      return MySerialExecutor.sharedUnownedExecutor
  }

  @inlinable
  public nonisolated func enqueue(_ job: UnownedJob) {
      debugLog("==> MyMainActor: enqueuing!")
      executor.enqueue(job)
  }
}

/// An actor that we use to test that after eliminating the synchronous
/// accesses, we properly deserialize the task access set causing a crash in
/// unownedExecutor.
@available(SwiftStdlib 5.1, *)
@globalActor public final actor MyMainActorWithAccessInUnownedExecAccessor: Executor {
    public static let shared = MyMainActorWithAccessInUnownedExecAccessor()
    public let executor = MySerialExecutor()

  @inlinable
  public nonisolated var unownedExecutor: UnownedSerialExecutor {
      debugLog("==> MyMainActorWithAccessInUnownedExecAccessor: Getting unowned executor!")
      withExclusiveAccess(to: &global) { _ in debugLog("Crash!") }
      return executor.asUnownedSerialExecutor()
  }

  @inlinable
  public static var sharedUnownedExecutor: UnownedSerialExecutor {
      debugLog("==> MyMainActorWithAccessInUnownedExecAccessor: Getting shared unowned executor!")
      return MySerialExecutor.sharedUnownedExecutor
  }

  @inlinable
  public nonisolated func enqueue(_ job: UnownedJob) {
      debugLog("==> MyMainActorWithAccessInUnownedExecAccessor: enqueuing!")
      executor.enqueue(job)
  }
}

@available(SwiftStdlib 5.1, *)
actor Custom {
  var count = 0

  func report() async {
    debugLog("==> Custom: custom.count == \(count)")
    count += 1
  }
}

@available(SwiftStdlib 5.1, *)
@globalActor
struct CustomActor {
    static var shared: Custom {
        debugLog("==> CustomActor: Getting custom!")
        return Custom()
    }
}

public var global: Int = 5
public var global2: Int = 6
public var global3: Int = 7
public var global4: Int = 8

@available(SwiftStdlib 5.1, *)
@main
struct Runner {
    @MainActor static func main() async {
        var exclusivityTests = TestSuite("Async Exclusivity Custom Executors")

        // As a quick soundness test, make sure that the crash doesn't occur if we
        // don't have the withExclusiveAccess(to: ) from the case below.
        exclusivityTests.test("exclusivityAccessesPropagateFromExecutorIntoTasks NoConflict") {
            @MainActor in
            debugLog("==> Before handle")
            let handle = Task { @MyMainActor in
                debugLog("==> Main: In handle!")
                debugLog("==> No Crash!")
                debugLog("==> Main: All done!")
            }
            await handle.value
        }

        // Make sure that we crash here due to the task forming an access to
        // memory that the executor has exclusive access to.
        exclusivityTests.test("exclusivityAccessesPropagateFromExecutorIntoTasks Crash") { @MainActor in
            expectCrashLater(withMessage: "Fatal access conflict detected")
            debugLog("==> Before handle")
            let handle = Task { @MyMainActor in
                debugLog("==> Main: In handle!")
                withExclusiveAccess(to: &global2) { _ in
                    debugLog("==> Crash!")
                }
                debugLog("==> Main: All done!")
            }
            await handle.value
        }

        // If all of the previous tests passed, then we have basic soundness
        // done. Lets now test out our cases that involve a live sync access.
        //
        // We test cases 3,4,7,8 here. The other cases that do not involve a
        // custom executor are tested in Runtime/exclusivity.swift.

        // Now that we have tested our tests with various numbers of accesses,
        // lets make specific tests for each case in Exclusivity.cpp.
        //
        // Case 3: (F, T, F) - No Live Accesses at Task Start, Exiting Live Sync
        // Accesses When Push, No Live Task Accesses when pop.
        //
        // This case is the case where we do not have any accesses in our code
        // at all or if the task cleans up the tasks before it awaits again. We
        // test the task cleanup case.
        exclusivityTests.test("case3") { @MainActor in
            debugLog("==> Before handle")
            let handle = Task { @MyMainActor in
                debugLog("==> Main: In handle!")
                debugLog("==> No Crash!")
                debugLog("==> Main: All done!")

                withExclusiveAccess(to: &global) { _ in
                    debugLog("==> Making sure can push/pop access")
                }
                // In order to test that we properly hand off the access, we
                // need to await here.
                let handle2 = await Task { @CustomActor in
                    debugLog("==> In inner handle")
                    // Different task without sync so no access issue.
                    withExclusiveAccess(to: &global) { _ in
                        debugLog("==> Making sure can push/pop access")
                    }
                    withExclusiveAccess(to: &global2) { _ in
                        debugLog("==> Making sure can push/pop access")
                    }
                }
                await handle2.value
            }
            withExclusiveAccess(to: &global2) { _ in
                // Sync accesses should have not been injected yet.
                debugLog("==> No crash.")
            }
            withExclusiveAccess(to: &global) { _ in
                // Make sure that global is cleaned up appropriately.
                debugLog("==> No crash.")
            }
            await handle.value
            withExclusiveAccess(to: &global2) { _ in
                // Sync accesses should have been cleaned up by now. So no
                // crash.
                debugLog("==> No crash.")
            }
            withExclusiveAccess(to: &global) { _ in
                // Make sure that global is cleaned up appropriately.
                debugLog("==> No crash.")
            }
        }

        // Make sure that we crash when accessing &global.
        exclusivityTests.test("case3.crash") { @MainActor in
            expectCrashLater(withMessage: "Fatal access conflict detected")
            debugLog("==> Before handle")
            let handle = Task { @MyMainActor in
                debugLog("==> Main: In handle!")
                debugLog("==> No Crash!")
                debugLog("==> Main: All done!")

                withExclusiveAccess(to: &global) { _ in
                    debugLog("==> Making sure can push/pop access")
                }
                // In order to test that we properly hand off the access, we
                // need to await here.
                let handle2 = await Task { @CustomActor in
                    debugLog("==> In inner handle")
                    // No crash here.
                    withExclusiveAccess(to: &global) { _ in
                        debugLog("==> Making sure can push/pop access")
                    }
                    withExclusiveAccess(to: &global2) { _ in
                        debugLog("==> Making sure can push/pop access")
                    }
                }
                await handle2.value
                // But we should crash here if we restore appropriately.
                withExclusiveAccess(to: &global2) { _ in
                    debugLog("Crash!")
                }
            }
            await handle.value
        }

        // Case 4: (F, T, T). In case 4, our task does not start with a live
        // access but we have accesses from the outside synchronous context, and
        // do add new accesses when we pop.
        exclusivityTests.test("case4") { @MainActor in
            debugLog("==> Before handle")
            let handle = Task { @MyMainActor in
                debugLog("==> Main: In handle!")
                debugLog("==> No Crash!")
                debugLog("==> Main: All done!")

                await withExclusiveAccessAsync(to: &global) {
                    @MyMainActor (x: inout Int) async -> Void in
                    debugLog("==> Making sure can push/pop access")
                }
                // In order to test that we properly hand off the access, we
                // need to await here.
                let handle2 = await Task { @CustomActor in
                    debugLog("==> In inner handle")
                }
                await handle2.value
            }
            await handle.value
        }

        // Make sure we crash with the sync access despite mixing in the tasks
        // accesses.
        exclusivityTests.test("case4.execaccess.to_sync") { @MainActor in
            expectCrashLater(withMessage: "Fatal access conflict detected")
            debugLog("==> Before handle")
            let handle = Task { @MyMainActor in
                debugLog("==> Main: In handle!")
                debugLog("==> No Crash!")
                debugLog("==> Main: All done!")

                await withExclusiveAccessAsync(to: &global) {
                    @MyMainActor (x: inout Int) async -> Void in
                    withExclusiveAccess(to: &global2) { _ in debugLog("CRASH!") }
                    debugLog("==> Making sure can push/pop access")
                }
                // In order to test that we properly hand off the access, we
                // need to await here.
                let handle2 = await Task { @CustomActor in
                    debugLog("==> In inner handle")
                }
                await handle2.value
            }
            await handle.value
        }

        // Make sure we do not crash with the sync access despite mixing in the tasks
        // accesses.
        exclusivityTests.test("case4.no_crash") { @MainActor in
            debugLog("==> Before handle")
            let handle = Task { @MyMainActor in
                debugLog("==> Main: In handle!")
                debugLog("==> No Crash!")
                debugLog("==> Main: All done!")

                await withExclusiveAccessAsync(to: &global) {
                    @MyMainActor (x: inout Int) async -> Void in
                    debugLog("==> Making sure can push/pop access")
                }
                // In order to test that we properly hand off the access, we
                // need to await here.
                let handle2 = await Task { @CustomActor in
                    debugLog("==> In inner handle")
                }
                await handle2.value
            }
            await handle.value
        }

        // This test makes sure that despite us going through case4, that we
        // properly deserialize the task's access and hit a crash in the
        // UnownedExecAccessor.
        exclusivityTests.test("case4.crash_due_to_deserialized_task") { @MainActor in
            expectCrashLater(withMessage: "Fatal access conflict detected")
            debugLog("==> Before handle")
            let handle = Task { @MyMainActorWithAccessInUnownedExecAccessor in
                debugLog("==> Main: In handle!")
                debugLog("==> No Crash!")
                debugLog("==> Main: All done!")

                await withExclusiveAccessAsync(to: &global) {
                    @MyMainActorWithAccessInUnownedExecAccessor (x: inout Int) async -> Void in
                    print("do something to avoid optimizing away to executor switch")
                    debugLog("==> Making sure can push/pop access")
                }
                // In order to test that we properly hand off the access, we
                // need to await here.
                let handle2 = await Task { @CustomActor in
                    debugLog("==> In inner handle")
                }
                await handle2.value
            }
            await handle.value
        }

        // CHECK-LABEL: ==> Enter 'testCase4'
        // CHECK: ==> MySerialExecutor: Got an enqueue!
        // CHECK-NEXT: Inserting new access: [[SYNC_NODE_1:0x[0-9a-f]+]]
        // CHECK-NEXT:   Tracking!
        // CHECK-NEXT:         Access. Pointer: [[SYNC_ACCESS_1:0x[0-9a-f]+]]. PC:
        // CHECK: ==> Enter 'withExclusiveAccess'
        // CHECK-NEXT: Inserting new access: [[SYNC_NODE_2:0x[0-9a-f]+]]
        // CHECK-NEXT:   Tracking!
        // CHECK-NEXT:         Access. Pointer: [[SYNC_ACCESS_2]]. PC:
        // CHECK-NEXT:         Access. Pointer: [[SYNC_ACCESS_1]]. PC:
        // CHECK: ==> Enter 'withExclusiveAccess'
        // CHECK: ==> MySerialExecutor: Inside access!
        // CHECK: ==> MySerialExecutor: Getting Unowned Executor!
        // CHECK-NEXT: Entering Thread Local Context. Before Swizzle. Task: [[TASK:0x[0-9a-f]+]]
        // CHECK-NEXT:         SwiftTaskThreadLocalContext: (FirstAccess,LastAccess): (0x0, 0x0)
        // CHECK-NEXT:         Access. Pointer: [[SYNC_ACCESS_2]]. PC:
        // CHECK-NEXT:         Access. Pointer: [[SYNC_ACCESS_1]]. PC:
        // CHECK-NEXT: Entering Thread Local Context. After Swizzle. Task: [[TASK]]
        // CHECK-NEXT:         SwiftTaskThreadLocalContext: (FirstAccess,LastAccess): ([[SYNC_NODE_2]], 0x0)
        // CHECK-NEXT:         Access. Pointer: [[SYNC_ACCESS_2]]. PC:
        // CHECK-NEXT:         Access. Pointer: [[SYNC_ACCESS_1]]. PC:
        // CHECK: ==> Main: In handle!
        // CHECK: ==> No Crash!
        // CHECK: ==> Main: All done!
        // CHECK: Inserting new access: [[TASK_NODE:0x[0-9a-f]+]]
        // CHECK-NEXT:   Tracking!
        // CHECK-NEXT:         Access. Pointer: [[TASK_ACCESS:0x[0-9a-f]+]].
        // CHECK-NEXT:         Access. Pointer: [[SYNC_ACCESS_2]]. PC:
        // CHECK-NEXT:         Access. Pointer: [[SYNC_ACCESS_1]]. PC:
        // CHECK-NEXT: Exiting Thread Local Context. Before Swizzle. Task: [[TASK]]
        // CHECK-NEXT:         SwiftTaskThreadLocalContext: (FirstAccess,LastAccess): ([[SYNC_NODE_2]], 0x0)
        // CHECK-NEXT:         Access. Pointer: [[TASK_ACCESS]]. PC:
        // CHECK-NEXT:         Access. Pointer: [[SYNC_ACCESS_2]]. PC:
        // CHECK-NEXT:         Access. Pointer: [[SYNC_ACCESS_1]]. PC:
        // CHECK-NEXT: Exiting Thread Local Context. After Swizzle. Task: [[TASK]]
        // CHECK-NEXT:         SwiftTaskThreadLocalContext: (FirstAccess,LastAccess): ([[TASK_NODE]], [[TASK_NODE]])
        // CHECK-NEXT:         Access. Pointer: [[SYNC_ACCESS_2]]. PC:
        // CHECK-NEXT:         Access. Pointer: [[SYNC_ACCESS_1]]. PC:
        // CHECK: ==> MySerialExecutor: Inside access after run synchronously!
        // CHECK: ==> Exit 'testCase4'
        exclusivityTests.test("case4.filecheck") { @MainActor in
            debugLog("==> Enter 'testCase4'")
            defer { debugLog("==> Exit 'testCase4'") }

            debugLog("==> Before handle")
            let handle = Task { @MyMainActor in
                debugLog("==> Main: In handle!")
                debugLog("==> No Crash!")
                debugLog("==> Main: All done!")

                await withExclusiveAccessAsync(to: &global) {
                    @MyMainActor (x: inout Int) async -> Void in
                    debugLog("==> Making sure can push/pop access")
                }
                // In order to test that we properly hand off the access, we
                // need to await here.
                let handle2 = await Task { @CustomActor in
                    debugLog("==> In inner handle")
                }
                await handle2.value
            }
            await handle.value
        }

        // Case 7. (T, T, F). In case 7, our task starts with live accesses and
        // sync accesses, but the live accesses are popped before we return.
        @Sendable
        @MyMainActor @inline(never)
        func withExclusiveAccessAsyncCase7<T, U>(to x: inout T, f: (inout T) async -> U) async -> U {
            debugLog("==> Enter 'withExclusiveAccessAsyncCase7'")
            defer { debugLog("==> Exit 'withExclusiveAccessAsyncCase7'") }
            let t = Task { @MainActor in
                debugLog("==> Task to force serialization of MyMainActor by using MainActor")
            }
            await t.value
            return await f(&x)
        }

        exclusivityTests.test("case7") { @MainActor in
            debugLog("==> Before handle")
            let handle = Task { @MyMainActor in
                debugLog("==> Main: In handle!")
                debugLog("==> No Crash!")
                debugLog("==> Main: All done!")

                await withExclusiveAccessAsyncCase7(to: &global) {
                    @MyMainActor (x: inout Int) async -> Void in
                    debugLog("==> Making sure can push/pop access")
                }

                // In order to test that we properly hand off the access, we
                // need to await here.
                let handle2 = await Task { @CustomActor in
                    debugLog("==> In inner handle")
                }
                await handle2.value
            }
            await handle.value
        }

        @Sendable
        @MyMainActor @inline(never)
        func withExclusiveAccessAsyncCase7AccessGlobal<T, U>(to x: inout T, f: (inout T) async -> U) async -> U {
            debugLog("==> Enter 'withExclusiveAccessAsyncCase7'")
            defer { debugLog("==> Exit 'withExclusiveAccessAsyncCase7'") }
            let t = Task { @MainActor in
                debugLog("==> Task to force serialization of MyMainActor by using MainActor")
            }
            await t.value

            // We should crash here since x should also be global and we
            // properly deserialized.

            withExclusiveAccess(to: &global) { _ in }
            return await f(&x)
        }

        // Validate case7 by crashing due to a Task access <-> Task access conflict
        exclusivityTests.test("case7.crash.taskaccess_taskaccess_conflict") { @MainActor in
            expectCrashLater(withMessage: "Fatal access conflict detected")
            debugLog("==> Before handle")
            let handle = Task { @MyMainActor in
                debugLog("==> Main: In handle!")
                debugLog("==> No Crash!")
                debugLog("==> Main: All done!")

                await withExclusiveAccessAsyncCase7AccessGlobal(to: &global) {
                    @MyMainActor (x: inout Int) async -> Void in
                    debugLog("==> Making sure can push/pop access")
                }

                // In order to test that we properly hand off the access, we
                // need to await here.
                let handle2 = await Task { @CustomActor in
                    debugLog("==> In inner handle")
                }
                await handle2.value
            }
            await handle.value
        }

        @Sendable
        @MyMainActor @inline(never)
        func withExclusiveAccessAsyncCase7AccessGlobal2<T, U>(to x: inout T, f: (inout T) async -> U) async -> U {
            debugLog("==> Enter 'withExclusiveAccessAsyncCase7'")
            defer { debugLog("==> Exit 'withExclusiveAccessAsyncCase7'") }
            let t = Task { @MainActor in
                debugLog("==> Task to force serialization of MyMainActor by using MainActor")
            }
            await t.value

            // We should crash here since our executor had exclusive access to
            // global2.
            withExclusiveAccess(to: &global2) { _ in }
            return await f(&x)
        }

        // Validate case7 by crashing due to a Task access <-> Task access conflict
        exclusivityTests.test("case7.crash.syncaccess_taskaccess_conflict") { @MainActor in
            expectCrashLater(withMessage: "Fatal access conflict detected")
            debugLog("==> Before handle")
            let handle = Task { @MyMainActor in
                debugLog("==> Main: In handle!")
                debugLog("==> No Crash!")
                debugLog("==> Main: All done!")

                await withExclusiveAccessAsyncCase7AccessGlobal2(to: &global) {
                    @MyMainActor (x: inout Int) async -> Void in
                    debugLog("==> Making sure can push/pop access")
                }

                // In order to test that we properly hand off the access, we
                // need to await here.
                let handle2 = await Task { @CustomActor in
                    debugLog("==> In inner handle")
                }
                await handle2.value
            }
            await handle.value
        }

        // Case 8. (T, T, T). In case 8, our task starts with live accesses and
        // sync accesses, and we have remaining live accesses when we return.
        @Sendable
        @MyMainActor @inline(never)
        func withExclusiveAccessAsyncCase8<T, U>(to x: inout T, f: (inout T) async -> U) async -> U {
            debugLog("==> Enter 'withExclusiveAccessAsyncCase8'")
            defer { debugLog("==> Exit 'withExclusiveAccessAsyncCase8'") }
            let t = Task { @MainActor in
                debugLog("==> Task1 to force serialization of MyMainActor by using MainActor")
            }
            await t.value

            await f(&x)

            let t2 = Task { @MainActor in
                debugLog("==> Task2 to force serialization of MyMainActor by using MainActor")
            }
            await t2.value

            return await f(&x)
        }

        exclusivityTests.test("case8") { @MainActor in
            debugLog("==> Before handle")
            let handle = Task { @MyMainActor in
                debugLog("==> Main: In handle!")
                debugLog("==> No Crash!")
                debugLog("==> Main: All done!")

                await withExclusiveAccessAsyncCase8(to: &global) {
                    @MyMainActor (x: inout Int) async -> Void in
                    debugLog("==> Making sure can push/pop access")
                }

                // In order to test that we properly hand off the access, we
                // need to await here.
                let handle2 = await Task { @CustomActor in
                    debugLog("==> In inner handle")
                }
                await handle2.value
            }
            await handle.value
        }

        // Case 8. (T, T, T). In case 8, our task starts with live accesses and
        // sync accesses, and we have remaining live accesses when we return.
        @Sendable
        @MyMainActor @inline(never)
        func withExclusiveAccessAsyncCase8Access<T, U>(to x: inout T, f: (inout T) async -> U) async -> U {
            debugLog("==> Enter 'withExclusiveAccessAsyncCase8'")
            defer { debugLog("==> Exit 'withExclusiveAccessAsyncCase8'") }
            let t = Task { @MainActor in
                debugLog("==> Task1 to force serialization of MyMainActor by using MainActor")
            }
            await t.value

            await f(&x)

            let t2 = Task { @MainActor in
                debugLog("==> Task2 to force serialization of MyMainActor by using MainActor")
            }
            await t2.value

            // This is the time period we are testing works in the positive case.

            return await f(&x)
        }

        exclusivityTests.test("case8") { @MainActor in
            debugLog("==> Before handle")
            let handle = Task { @MyMainActor in
                debugLog("==> Main: In handle!")
                debugLog("==> No Crash!")
                debugLog("==> Main: All done!")

                await withExclusiveAccessAsyncCase8(to: &global) {
                    @MyMainActor (x: inout Int) async -> Void in
                    debugLog("==> Making sure can push/pop access")
                }

                // In order to test that we properly hand off the access, we
                // need to await here.
                let handle2 = Task { @CustomActor in
                    debugLog("==> In inner handle")
                }
                await handle2.value
            }
            await handle.value
        }

        @Sendable
        @MyMainActor @inline(never)
        func withExclusiveAccessAsyncCase8AccessGlobal<T, U>(to x: inout T, f: (inout T) async -> U) async -> U {
            debugLog("==> Enter 'withExclusiveAccessAsyncCase8'")
            defer { debugLog("==> Exit 'withExclusiveAccessAsyncCase8'") }
            let t = Task { @MainActor in
                debugLog("==> Task1 to force serialization of MyMainActor by using MainActor")
            }
            await t.value

            await f(&x)

            let t2 = Task { @MainActor in
                debugLog("==> Task2 to force serialization of MyMainActor by using MainActor")
            }
            await t2.value
            // Make sure we swizzled back in our serialized task state, so we
            // crash.
            withExclusiveAccess(to: &global) { _ in
                debugLog("==> TaskAccess + TaskAccess == Crash!")
            }
            return await f(&x)
        }

        exclusivityTests.test("case8.crash.taskaccess_taskaccess_conflict") { @MainActor in
            expectCrashLater(withMessage: "Fatal access conflict detected")
            debugLog("==> Before handle")
            let handle = Task { @MyMainActor in
                debugLog("==> Main: In handle!")
                debugLog("==> No Crash!")
                debugLog("==> Main: All done!")

                await withExclusiveAccessAsyncCase8AccessGlobal(to: &global) {
                    @MyMainActor (x: inout Int) async -> Void in
                    debugLog("==> Making sure can push/pop access")
                }

                // In order to test that we properly hand off the access, we
                // need to await here.
                let handle2 = await Task { @CustomActor in
                    debugLog("==> In inner handle")
                }
                await handle2.value
            }
            await handle.value
        }

        @Sendable
        @MyMainActor @inline(never)
        func withExclusiveAccessAsyncCase8AccessGlobal2<T, U>(to x: inout T, f: (inout T) async -> U) async -> U {
            debugLog("==> Enter 'withExclusiveAccessAsyncCase8'")
            defer { debugLog("==> Exit 'withExclusiveAccessAsyncCase8'") }
            let t = Task { @MainActor in
                debugLog("==> Task1 to force serialization of MyMainActor by using MainActor")
            }
            await t.value

            await f(&x)

            let t2 = Task { @MainActor in
                debugLog("==> Task2 to force serialization of MyMainActor by using MainActor")
            }
            await t2.value
            // Make sure we swizzled back in our serialized task state, so we
            // crash.
            withExclusiveAccess(to: &global2) { _ in
                debugLog("==> SyncAccess + TaskAccess == Crash!")
            }
            return await f(&x)
        }

        exclusivityTests.test("case8.crash.syncaccess_taskaccess_conflict") { @MainActor in
            expectCrashLater(withMessage: "Fatal access conflict detected")
            debugLog("==> Before handle")
            let handle = Task { @MyMainActor in
                debugLog("==> Main: In handle!")
                debugLog("==> No Crash!")
                debugLog("==> Main: All done!")

                await withExclusiveAccessAsyncCase8AccessGlobal2(to: &global) {
                    @MyMainActor (x: inout Int) async -> Void in
                    debugLog("==> Making sure can push/pop access")
                }

                // In order to test that we properly hand off the access, we
                // need to await here.
                let handle2 = Task { @CustomActor in
                    debugLog("==> In inner handle")
                }
                await handle2.value
            }
            await handle.value
        }

        await runAllTestsAsync()
    }
}
