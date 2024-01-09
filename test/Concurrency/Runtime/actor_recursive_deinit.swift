// RUN: %target-run-simple-swift(-Xfrontend -disable-availability-checking -parse-stdlib -parse-as-library) | %FileCheck %s

// REQUIRES: executable_test
// REQUIRES: concurrency

// REQUIRES: concurrency_runtime
// UNSUPPORTED: back_deployment_runtime

import Swift
import _Concurrency

#if canImport(Darwin)
import Darwin
typealias ThreadID = pthread_t
func getCurrentThreadID() -> ThreadID { pthread_self() }
func equalThreadIDs(_ a: ThreadID, _ b: ThreadID) -> Bool { pthread_equal(a, b) != 0 }
#elseif canImport(Glibc)
import Glibc
typealias ThreadID = pthread_t
func getCurrentThreadID() -> ThreadID { pthread_self() }
func equalThreadIDs(_ a: ThreadID, _ b: ThreadID) -> Bool { pthread_equal(a, b) != 0 }
#elseif os(Windows)
import WinSDK
typealias ThreadID = UInt32
func getCurrentThreadID() -> ThreadID { GetCurrentThreadId() }
func equalThreadIDs(_ a: ThreadID, _ b: ThreadID) -> Bool { a == b }
#endif

var mainThread: ThreadID?
func isMainThread() -> Bool {
    return equalThreadIDs(getCurrentThreadID(), mainThread!)
}

@_silgen_name("swift_task_isCurrentExecutor")
private func isCurrentExecutor(_ executor: Builtin.Executor) -> Bool

func getExecutor(_ a: AnyActor) -> Builtin.Executor {
  let pack = (a, UnsafeRawPointer?.none)
  return unsafeBitCast(pack, to: Builtin.Executor.self)
}

func isCurrent(_ a: AnyActor) -> Bool {
  return isCurrentExecutor(getExecutor(a))
}

func isMainExecutor() -> Bool {
  isCurrentExecutor(Builtin.buildMainActorExecutorRef())
}

actor Foo {
    let name: String
    let child: Foo?

    init(_ name: String, _ child: Foo?) {
        self.name = name
        self.child = child
    }

    isolated deinit {
        print("DEINIT: \(name) isolated:\(isCurrent(self)) mainThread:\(isMainThread())")
    }
}

// CHECK: DEINIT: a isolated:true mainThread:true
// CHECK: DEINIT: b isolated:true mainThread:true
// CHECK: DEINIT: c isolated:true mainThread:true
// CHECK: DEINIT: d isolated:true mainThread:true
// CHECK: DONE

@main
struct Main {
    static func main() async {
        mainThread = getCurrentThreadID()
        do {
            _ = Foo("a", Foo("b", Foo("c", Foo("d", nil))))
        }
        print("DONE")
    }
}
