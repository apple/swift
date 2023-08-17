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

@main struct Main {
  static func main() async {
    let tests = TestSuite("ContinuationValidation")

    if #available(SwiftStdlib 5.1, *) {
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
