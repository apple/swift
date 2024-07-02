// RUN: %empty-directory(%t)
// RUN: %target-build-swift -Xfrontend -disable-availability-checking %import-libdispatch -parse-as-library %s -o %t/a.out
// RUN: %target-codesign %t/a.out
// RUN: %env-SWIFT_IS_CURRENT_EXECUTOR_LEGACY_MODE_OVERRIDE=swift6 %target-run %t/a.out

// REQUIRES: executable_test
// REQUIRES: concurrency
// REQUIRES: concurrency_runtime

// REQUIRES: libdispatch

// UNSUPPORTED: back_deployment_runtime
// UNSUPPORTED: back_deploy_concurrency
// UNSUPPORTED: use_os_stdlib
// UNSUPPORTED: freestanding

import Dispatch

#if os(macOS)
import Darwin
#elseif canImport(Glibc)
import Glibc
#elseif canImport(Musl)
import Musl
#elseif canImport(Android)
import Android
#endif

@main struct Main {
  static func main() {
    if #available(SwiftStdlib 6.0, *) {

      Swift.print("start detached")
      Task.detached {
        Swift.print("DispatchQueue.main.async { MainActor.precondition }")

        DispatchQueue.main.async {
          // In Swift 6 mode we're allowed to notice that we're asking for main
          // queue and use dispatch's assertions directly.
          //
          // The same code would be crashing without swift6 mode where we're allowed to use 'checkIsolated'
          MainActor.preconditionIsolated("I know I'm on the main queue")

          Swift.print("OK")
          exit(0)
        }
      }

      dispatchMain()
    }
  }
}
