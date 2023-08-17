// RUN: %target-run-simple-swift( -Xfrontend -disable-availability-checking -parse-as-library)

// REQUIRES: executable_test
// REQUIRES: concurrency
// REQUIRES: concurrency_runtime
// REQUIRES: reflection
// UNSUPPORTED: back_deployment_runtime

import _Concurrency
import StdlibUnittest

struct TestError: Error {}

@main struct Main {
  static func main() async {
    var tests = TestSuite("CheckedContinuation")

    if #available(SwiftStdlib 5.1, *) {
      #if !os(WASI)
      tests.test("trap on double resume non-throwing continuation") {
        expectCrashLater()

        let task = detach {
          let _: Int = await withCheckedContinuation { c in
            c.resume(returning: 17)
            c.resume(returning: 38)
          }
        }
        await task.get()
      }

      tests.test("trap on double resume throwing continuation") {
        expectCrashLater()

        let task = detach {
          do {
            let _: Int = try await withCheckedThrowingContinuation { c in
              c.resume(returning: 17)
              c.resume(throwing: TestError())
            }
          } catch {
          }
        }
        await task.get()
      }
      #endif

      tests.test("test withCheckedThrowingContinuation") {
        let task2 = detach {
          do {
            let x: Int = try await withCheckedThrowingContinuation { c in
              c.resume(returning: 17)
            }
            expectEqual(17, x)
          } catch {
          }
        }

        let task = detach {
          do {
            let x: Int = try await withCheckedThrowingContinuation { c in
              c.resume(returning: 17)
            }
            expectEqual(17, x)
          } catch {
          }
        }
        await task.get()
        await task2.get()
      }
    }

    await runAllTestsAsync()
  }
}
