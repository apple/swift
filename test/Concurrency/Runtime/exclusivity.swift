// RUN: %target-run-simple-swift(-Xfrontend -disable-availability-checking -parse-as-library)

// REQUIRES: executable_test
// REQUIRES: concurrency
// UNSUPPORTED: freestanding

// REQUIRES: concurrency_runtime
// UNSUPPORTED: back_deployment_runtime
// UNSUPPORTED: OS=wasi
// UNSUPPORTED: back_deploy_concurrency

// This test makes sure that:
//
// 1. Tasks have separate exclusivity sets.
// 2. Exercise the pushing/popping of access sets from tasks.

// NOTE: The cases that we are talking about handling below refer to the cases
// documented in Exclusivity.cpp.
//
// NOTE: We test cases that involve custom executors in
// custom_executors_exclusivity.cpp.

import _Concurrency
import StdlibUnittest

var global1: Int = 5
var global2: Int = 6
var global3: Int = 7

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

@available(SwiftStdlib 5.1, *)
@main
struct Runner {
    @MainActor
    @inline(never)
    static func withExclusiveAccessAsync<T, U>(to x: inout T, f: (inout T) async -> U) async -> U {
        await f(&x)
    }

    @MainActor
    @inline(never)
    static func withExclusiveAccess<T, U>(to x: inout T, f: (inout T) -> U) -> U {
        f(&x)
    }

    @inline(never)
    @MainActor
    static func doSomething() async { }

    @inline(never)
    @Sendable
    static func useGlobal(_ x: inout Int) { debugLog("FORCE ACCESS") }

    @MainActor static func main() async {
        var exclusivityTests = TestSuite("Async Exclusivity")

        // First make sure that if we do not introduce a new task, we still get
        // our expected conflict.
        exclusivityTests.test("testSameTaskBlowsUpSinceSameSet") { @MainActor in
            expectCrashLater(withMessage: "Fatal access conflict detected")

            let callee2 = { @MainActor (_ x: inout Int) -> Void in
                debugLog("==> Enter callee2")
                debugLog("Value: x: \(x)")
                debugLog("==> Exit callee2")
            }

            // We add an inline never here to make sure that we do not eliminate
            // the dynamic access after inlining.
            @MainActor
            @inline(never)
            func callee1(_ x: inout Int) async -> () {
                debugLog("==> Enter callee1")

                // Second access. Same Task so not ok.
                await callee2(&global1)

                debugLog("==> Exit callee1")

            }

            debugLog("==> Enter Main")
            // First access begins here.
            await callee1(&global1)
            debugLog("==> Exit Main")
        }

        // Then do a simple test with a single access to make sure that we do
        // not hit any successes b/c we introduced the Task.
        exclusivityTests.test("testDifferentTasksHaveDifferentExclusivityAccessSets") { @MainActor in
            let callee2 = { @MainActor (_ x: inout Int) -> Void in
                debugLog("==> Enter callee2")
                debugLog("==> Exit callee2")
            }

            // We add an inline never here to make sure that we do not eliminate
            // the dynamic access after inlining.
            @MainActor
            @inline(never)
            func callee1(_ x: inout Int) async -> () {
                debugLog("==> Enter callee1")
                // This task is what prevents this example from crashing.
                let handle = Task { @MainActor in
                    debugLog("==> Enter callee1 Closure")
                    // Second access. Different Task so it is ok.
                    await callee2(&global1)
                    debugLog("==> Exit callee1 Closure")
                }
                await handle.value
                debugLog("==> Exit callee1")
            }

            debugLog("==> Enter Main")
            // First access begins here.
            await callee1(&global1)
            debugLog("==> Exit Main")
        }

        // Make sure we correctly handle cases where we have multiple accesses
        // open at the same time.
        exclusivityTests.test("testDifferentTasksWith2Accesses") { @MainActor in
            let callee2 = { @MainActor (_ x: inout Int, _ y: inout Int) -> Void in
                debugLog("==> Enter callee2")
                debugLog("==> Exit callee2")
            }

            // We add an inline never here to make sure that we do not eliminate
            // the dynamic access after inlining.
            @MainActor
            @inline(never)
            func callee1(_ x: inout Int, _ y: inout Int) async -> () {
                debugLog("==> Enter callee1")
                let handle = Task { @MainActor in
                    debugLog("==> Enter callee1 Closure")
                    // Second access. Different Task so it is ok.
                    await callee2(&global1, &global2)
                    debugLog("==> Exit callee1 Closure")
                }
                await handle.value
                debugLog("==> Exit callee1")
            }

            debugLog("==> Enter Main")
            // First access begins here.
            await callee1(&global1, &global2)
            debugLog("==> Exit Main")
        }

        // Make sure we correctly handle cases where we have multiple accesses
        // open at the same time.
        exclusivityTests.test("testDifferentTasksWith3Accesses") { @MainActor in
            let callee2 = { @MainActor (_ x: inout Int, _ y: inout Int, _ z: inout Int) -> Void in
                debugLog("==> Enter callee2")
                debugLog("==> Exit callee2")
            }

            // We add an inline never here to make sure that we do not eliminate
            // the dynamic access after inlining.
            @MainActor
            @inline(never)
            func callee1(_ x: inout Int, _ y: inout Int, _ z: inout Int) async -> () {
                debugLog("==> Enter callee1")
                let handle = Task { @MainActor in
                    debugLog("==> Enter callee1 Closure")
                    // Second access. Different Task so it is ok.
                    await callee2(&global1, &global2, &global3)
                    debugLog("==> Exit callee1 Closure")
                }
                await handle.value
                debugLog("==> Exit callee1")
            }

            debugLog("==> Enter Main")
            // First access begins here.
            await callee1(&global1, &global2, &global3)
            debugLog("==> Exit Main")
        }

        // Now that we have tested our tests with various numbers of accesses,
        // lets make specific tests for each case in Exclusivity.cpp.
        //
        // Case 1: (F, F, F) - No Live Accesses at Task Start, No Live Sync
        // Accesses When Push, No Live Task Accesses when pop.
        //
        // This case is the case where we do not have any accesses in our code
        // at all or if the task cleans up the tasks before it awaits again. We
        // test the task cleanup case.
        exclusivityTests.test("case1") { @MainActor in
            @inline(never)
            @Sendable func callee2(_ x: inout Int, _ y: inout Int, _ z: inout Int) -> Void {
                debugLog("==> Enter callee2")
                debugLog("==> Exit callee2")
            }

            // We add an inline never here to make sure that we do not eliminate
            // the dynamic access after inlining.
            @MainActor
            @inline(never)
            func callee1() async -> () {
                debugLog("==> Enter callee1")
                let handle = Task { @MainActor in
                    debugLog("==> Enter callee1 Closure")
                    // These accesses end before we await in the task.
                    do {
                        callee2(&global1, &global2, &global3)
                    }
                    await doSomething()
                    debugLog("==> Exit callee1 Closure")
                }
                await handle.value
                debugLog("==> Exit callee1")
            }

            await callee1()
        }

        // In case 2, our task does not start with any live accesses, but it is
        // awaited upon after the live access begins. We want to make sure that
        // we fail here since we properly restored the callee state.
        exclusivityTests.test("case2") { @MainActor in
            expectCrashLater(withMessage: "Fatal access conflict detected")

            let callee2 = { @MainActor (_ x: inout Int) -> Void in
                debugLog("==> Enter callee2")
                debugLog("==> Exit callee2")
            }

            // We add an inline never here to make sure that we do not eliminate
            // the dynamic access after inlining.
            @MainActor
            @inline(never)
            func callee1(_ x: inout Int) async -> () {
                debugLog("==> Enter callee1")
                // This task is what prevents this example from crashing.
                let handle = Task { @MainActor in
                    debugLog("==> Enter callee1 Closure")
                    // Second access. Different Task so it is ok.
                    await callee2(&global1)
                    debugLog("==> Exit callee1 Closure")
                }
                await handle.value

                useGlobal(&global1) // We should crash here.

                debugLog("==> Exit callee1")
            }

            debugLog("==> Enter Main")
            // First access begins here.
            await callee1(&global1)
            debugLog("==> Exit Main")
        }

        // In case 5, our task starts with live accesses, but we finish the
        // accesses before we return. So we do not crash. The key thing is the
        // access lives over a suspension/resume.
        exclusivityTests.test("case5") { @MainActor in
            let callee2 = { @MainActor (_ x: inout Int) -> Void in
                debugLog("==> Enter callee2")
                debugLog("==> Exit callee2")
            }

            // We add an inline never here to make sure that we do not eliminate
            // the dynamic access after inlining.
            @MainActor
            @inline(never)
            func callee1(_ x: inout Int) async -> () {
                debugLog("==> Enter callee1")
                // This task is what prevents this example from crashing.
                let handle = Task { @MainActor in
                    debugLog("==> Enter callee1 Closure")
                    // Second access. Different Task so it is ok.
                    await callee2(&global1)
                    debugLog("==> Exit callee1 Closure")
                }
                await handle.value

                debugLog("==> Exit callee1")
            }

            @MainActor
            @inline(never)
            func callCallee1() async {
                await callee1(&global1)
            }

            debugLog("==> Enter Main")
            // First access begins here.
            await callCallee1()
            useGlobal(&global1) // We should not crash here since we cleaned up
            // the access in callCallee1 after we returned
            // from the await there.
            debugLog("==> Exit Main")
        }

        // In case 6, our task starts with live accesses, and we only finish
        // some of the accesses before we return. So we want to validate that by
        // running the same code twice, one testing we can access the pointer we
        // can fix up and a second that we can not.
        exclusivityTests.test("case6") { @MainActor in
            let callee2 = { @MainActor (_ x: inout Int) -> Void in
                debugLog("==> Enter callee2")
                debugLog("==> Exit callee2")
            }

            // We add an inline never here to make sure that we do not eliminate
            // the dynamic access after inlining.
            @MainActor
            @inline(never)
            func callee1(_ x: inout Int) async -> () {
                debugLog("==> Enter callee1")
                // This task is what prevents this example from crashing.
                let handle = Task { @MainActor in
                    debugLog("==> Enter callee1 Closure")
                    // Second access. Different Task so it is ok.
                    await callee2(&global1)
                    debugLog("==> Exit callee1 Closure")
                }
                await handle.value

                debugLog("==> Exit callee1")
            }

            @MainActor
            @inline(never)
            func callCallee1() async {
                await callee1(&global1)
            }

            debugLog("==> Enter Main")
            // First access begins here.
            await callCallee1()
            useGlobal(&global1) // We should not crash here since we cleaned up
            // the access in callCallee1 after we returned
            // from the await there.
            debugLog("==> Exit Main")
        }

        // These are additional tests that used to be FileChecked but FileCheck
        // was too hard to use in a concurrent context.
        exclusivityTests.test("case1") { @MainActor in
            @inline(never)
            @Sendable func callee2(_ x: inout Int, _ y: inout Int, _ z: inout Int) -> Void {
                debugLog("==> Enter callee2")
                debugLog("==> Exit callee2")
            }
            
            // We add an inline never here to make sure that we do not eliminate
            // the dynamic access after inlining.
            @MainActor
            @inline(never)
            func callee1() async -> () {
                debugLog("==> Enter callee1")
                let handle = Task { @MainActor in
                    debugLog("==> Enter callee1 Closure")
                    
                    // These accesses end before we await in the task.
                    do {
                        callee2(&global1, &global2, &global3)
                    }
                    let handle2 = Task { @MainActor in
                        debugLog("==> Enter handle2!")
                        debugLog("==> Exit handle2!")
                    }
                    await handle2.value
                    debugLog("==> Exit callee1 Closure")
                }
                await handle.value
                debugLog("==> Exit callee1")
            }
            
            debugLog("==> Enter 'testCase1'")
            await callee1()
            debugLog("==> Exit 'testCase1'")
        }

        // Case 2: (F, F, T). In case 2, our task does not start with a live access
        // and nothing from the outside synchronous context, but does pop with a new
        // access.
        //
        // We use a suspend point and a withExclusiveAccessAsync(to:) to test this.
        exclusivityTests.test("case2.filecheck.nocrash") { @MainActor in
            debugLog("==> Enter 'testCase2'")

            let handle = Task { @MainActor in
                debugLog("==> Inner Handle")
                await withExclusiveAccessAsync(to: &global1) { @MainActor (x: inout Int) async -> Void in
                    let innerTaskHandle = Task { @MainActor in
                        // Different task, shouldn't crash.
                        withExclusiveAccess(to: &global1) { _ in
                            debugLog("==> No crash!")
                        }
                        debugLog("==> End Inner Task Handle")
                    }
                    // This will cause us to serialize the access to global1. If
                    // we had an access here, we would crash.
                    await innerTaskHandle.value
                    debugLog("==> After")
                }
                // Access is over. We shouldn't crash here.
                withExclusiveAccess(to: &global1) { _ in
                    debugLog("==> No crash!")
                }
                debugLog("==> Inner Handle: After exclusive access")
            }

            await handle.value
            debugLog("==> After exclusive access")
            let handle2 = Task { @MainActor in
                debugLog("==> Enter handle2!")
                debugLog("==> Exit handle2!")
            }
            await handle2.value
            debugLog("==> Exit 'testCase2'")
        }

        exclusivityTests.test("case2.filecheck.crash") { @MainActor in
            expectCrashLater(withMessage: "Fatal access conflict detected")
            debugLog("==> Enter 'testCase2'")

            let handle = Task { @MainActor in
                debugLog("==> Inner Handle")
                await withExclusiveAccessAsync(to: &global1) { @MainActor (x: inout Int) async -> Void in
                    let innerTaskHandle = Task { @MainActor in
                        debugLog("==> End Inner Task Handle")
                    }
                    await innerTaskHandle.value
                    // We will crash here if we properly brought back in the
                    // access to global1 despite running code on a different
                    // task.
                    withExclusiveAccess(to: &global1) { _ in
                        debugLog("==> Got a crash!")
                    }
                    debugLog("==> After")
                }
                debugLog("==> Inner Handle: After exclusive access")
            }

            await handle.value
            debugLog("==> After exclusive access")
            let handle2 = Task { @MainActor in
                debugLog("==> Enter handle2!")
                debugLog("==> Exit handle2!")
            }
            await handle2.value
            debugLog("==> Exit 'testCase2'")
        }

        // Case 5: (T,F,F). To test case 5, we use with exclusive access to to
        // create an exclusivity scope that goes over a suspension point. We are
        // interesting in the case where we return after the suspension point. That
        // push/pop is going to have our outer task bring in state and end it.
        //
        // CHECK-LABEL: ==> Enter 'testCase5'
        // CHECK: ==> Task: [[TASK:0x[0-9a-f]+]]
        // CHECK: Inserting new access: [[LLNODE:0x[a-z0-9]+]]
        // CHECK-NEXT: Tracking!
        // CHECK-NEXT: Access. Pointer: [[ACCESS:0x[a-z0-9]+]]
        // CHECK: Exiting Thread Local Context. Before Swizzle. Task: [[TASK]]
        // CHECK-NEXT: SwiftTaskThreadLocalContext: (FirstAccess,LastAccess): (0x0, 0x0)
        // CHECK-NEXT: Access. Pointer: [[ACCESS]]. PC:
        // CHECK: Exiting Thread Local Context. After Swizzle. Task: [[TASK]]
        // CHECK-NEXT: SwiftTaskThreadLocalContext: (FirstAccess,LastAccess): ([[LLNODE]], [[LLNODE]])
        // CHECK-NEXT: No Accesses.
        //
        // CHECK-NOT: Removing access:
        // CHECK: ==> End Inner Task Handle
        // CHECK: ==> After
        // CHECK: Removing access: [[LLNODE]]
        // CHECK: ==> After exclusive access
        // CHECK: Exiting Thread Local Context. Before Swizzle. Task: [[TASK]]
        // CHECK-NEXT:         SwiftTaskThreadLocalContext: (FirstAccess,LastAccess): (0x0, 0x0)
        // CHECK-NEXT:         No Accesses.
        // CHECK: Exiting Thread Local Context. After Swizzle. Task: [[TASK]]
        // CHECK-NEXT:         SwiftTaskThreadLocalContext: (FirstAccess,LastAccess): (0x0, 0x0)
        // CHECK-NEXT:         No Accesses.
        //
        // CHECK: ==> Exit 'testCase5'
        exclusivityTests.test("case5.filecheck") { @MainActor in
            debugLog("==> Enter 'testCase5'")

            let outerHandle = Task { @MainActor in
                await withExclusiveAccessAsync(to: &global1) { @MainActor (x: inout Int) async -> Void in
                    let innerTaskHandle = Task { @MainActor in
                        debugLog("==> End Inner Task Handle")
                    }
                    await innerTaskHandle.value
                    debugLog("==> After")
                }
                debugLog("==> After exclusive access")
                let handle2 = Task { @MainActor in
                    debugLog("==> Enter handle2!")
                    debugLog("==> Exit handle2!")
                }
                await handle2.value
            }
            await outerHandle.value
            debugLog("==> Exit 'testCase5'")
        }

        exclusivityTests.test("case5.filecheck.crash") { @MainActor in
            expectCrashLater(withMessage: "Fatal access conflict detected")
            debugLog("==> Enter 'testCase5'")

            let outerHandle = Task { @MainActor in
                await withExclusiveAccessAsync(to: &global1) { @MainActor (x: inout Int) async -> Void in
                    let innerTaskHandle = Task { @MainActor in
                        debugLog("==> End Inner Task Handle")
                    }
                    await innerTaskHandle.value
                    debugLog("==> After")
                    withExclusiveAccess(to: &global1) { _ in
                        debugLog("==> Crash here")
                    }
                }
                debugLog("==> After exclusive access")
                let handle2 = Task { @MainActor in
                    debugLog("==> Enter handle2!")
                    debugLog("==> Exit handle2!")
                }
                await handle2.value
            }
            await outerHandle.value
            debugLog("==> Exit 'testCase5'")
        }

        // Case 6: (T, F, T). In case 6, our task starts with live accesses and is
        // popped with live accesses. There are no sync accesses.
        //
        // We test this by looking at the behavior of the runtime after we
        // finish executing handle2. In this case, we first check that things
        // just work normally and as a 2nd case perform a conflicting access to
        // make sure we crash.
        exclusivityTests.test("case6.filecheck") { @MainActor in
            let outerHandle = Task { @MainActor in
                let callee2 = { @MainActor (_ x: inout Int) -> Void in
                    debugLog("==> Enter callee2")
                    debugLog("==> Exit callee2")
                }

                // We add an inline never here to make sure that we do not eliminate
                // the dynamic access after inlining.
                @MainActor
                @inline(never)
                func callee1(_ x: inout Int) async -> () {
                    debugLog("==> Enter callee1")
                    // This task is what prevents this example from crashing.
                    let handle = Task { @MainActor in
                        debugLog("==> Enter callee1 Closure")
                        // Second access. Different Task so it is ok.
                        await withExclusiveAccessAsync(to: &global1) {
                            await callee2(&$0)
                        }
                        debugLog("==> Exit callee1 Closure")
                    }
                    await handle.value
                    debugLog("==> callee1 after first await")
                    // Force an await here so we can see that we properly swizzle.
                    let handle2 = Task { @MainActor in
                        debugLog("==> Enter handle2!")
                        debugLog("==> Exit handle2!")
                    }
                    await handle2.value
                    debugLog("==> Exit callee1")
                }

                // First access begins here.
                await callee1(&global1)
            }
            debugLog("==> Enter 'testCase6'")
            await outerHandle.value
            debugLog("==> Exit 'testCase6'")
        }

        exclusivityTests.test("case6.filecheck.crash") { @MainActor in
            expectCrashLater(withMessage: "Fatal access conflict detected")
            let outerHandle = Task { @MainActor in
                let callee2 = { @MainActor (_ x: inout Int) -> Void in
                    debugLog("==> Enter callee2")
                    debugLog("==> Exit callee2")
                }

                // We add an inline never here to make sure that we do not eliminate
                // the dynamic access after inlining.
                @MainActor
                @inline(never)
                func callee1(_ x: inout Int) async -> () {
                    debugLog("==> Enter callee1")
                    // This task is what prevents this example from crashing.
                    let handle = Task { @MainActor in
                        debugLog("==> Enter callee1 Closure")
                        // Second access. Different Task so it is ok.
                        await withExclusiveAccessAsync(to: &global1) {
                            await callee2(&$0)
                        }
                        debugLog("==> Exit callee1 Closure")
                    }
                    await handle.value
                    debugLog("==> callee1 after first await")
                    // Force an await here so we can see that we properly swizzle.
                    let handle2 = Task { @MainActor in
                        debugLog("==> Enter handle2!")
                        debugLog("==> Exit handle2!")
                    }
                    await handle2.value
                    // Make sure we brought back in the access to x so we crash
                    // here.
                    withExclusiveAccess(to: &global1) { _ in
                        debugLog("==> Will crash here!")
                    }
                    debugLog("==> Exit callee1")
                }

                // First access begins here.
                await callee1(&global1)
            }
            debugLog("==> Enter 'testCase6'")
            await outerHandle.value
            debugLog("==> Exit 'testCase6'")
        }

        await runAllTestsAsync()
    }
}
