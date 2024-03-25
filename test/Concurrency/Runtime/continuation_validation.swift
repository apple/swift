// RUN: %empty-directory(%t)
// RUN: %target-build-swift -Xfrontend -disable-availability-checking -parse-as-library %s -o %t/a.out
// RUN: %target-codesign %t/a.out
// RUN: env %env-SWIFT_DEBUG_VALIDATE_UNCHECKED_CONTINUATIONS=1 %target-run %t/a.out

// REQUIRES: executable_test
// REQUIRES: concurrency
// REQUIRES: concurrency_runtime
// UNSUPPORTED: back_deployment_runtime

// Commit afc5116ef0de added support for the error check
// UNSUPPORTED: back_deploy_concurrency
// UNSUPPORTED: use_os_stdlib
// UNSUPPORTED: freestanding
// UNSUPPORTED: OS=wasi

import StdlibUnittest

@MainActor
@available(SwiftStdlib 5.1, *)
func test_isolation_withUnsafeContinuation() async {
  // This test specifically should have only one suspension point,
  // as it would trigger a problem with the previous @_unsafeInheritExecutor
  // implementation, where we optimize away a switch accidentally, causing
  // wrong isolation.
  await withUnsafeContinuation { continuation in
    MainActor.shared.assertIsolated() // OK
    continuation.resume(returning: ())
  }
}
@MainActor
@available(SwiftStdlib 5.1, *)
func test_isolation_withUnsafeThrowingContinuation() async {
  // See comment in `test_isolation_withUnsafeContinuation` about exact test case shape
  try! await withUnsafeThrowingContinuation { continuation in
    MainActor.shared.assertIsolated() // OK
    continuation.resume(returning: ())
  }
}
@MainActor
@available(SwiftStdlib 5.1, *)
func test_isolation_withCheckedContinuation() async {
  // See comment in `test_isolation_withUnsafeContinuation` about exact test case shape
  await withCheckedContinuation { continuation in
    MainActor.shared.assertIsolated() // OK
    continuation.resume(returning: ())
  }
}
@MainActor
@available(SwiftStdlib 5.1, *)
func test_isolation_withCheckedThrowingContinuation() async {
  // See comment in `test_isolation_withUnsafeContinuation` about exact test case shape
  try! await withCheckedThrowingContinuation { continuation in
    MainActor.shared.assertIsolated() // OK
    continuation.resume(returning: ())
  }
}

@main struct Main {
  static func main() async {
    let tests = TestSuite("ContinuationValidation")

    if #available(SwiftStdlib 5.1, *) {
      tests.test("withUnsafeThrowingContinuation: continuation should be on calling isolation") {
        await Task.detached {
          await test_isolation_withUnsafeThrowingContinuation()
        }.value
      }
      tests.test("withUnsafeContinuation: continuation should be on calling isolation") {
        await Task.detached {
          await test_isolation_withUnsafeContinuation()
        }.value
      }
      tests.test("withCheckedContinuation: continuation should be on calling isolation") {
        await Task.detached {
          await test_isolation_withCheckedContinuation()
        }.value
      }
      tests.test("withCheckedThrowingContinuation: continuation should be on calling isolation") {
        await Task.detached {
          await test_isolation_withCheckedThrowingContinuation()
        }.value
      }

      tests.test("trap on double resume of unchecked continuation") {
        expectCrashLater(withMessage: "may have already been resumed")

        await withUnsafeContinuation { c in
          c.resume(returning: ())
          c.resume(returning: ())
        }
      }
    }

    await runAllTestsAsync()
  }
}
