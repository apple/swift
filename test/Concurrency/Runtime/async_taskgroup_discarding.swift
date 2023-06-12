// RUN: %target-run-simple-swift( -Xfrontend -disable-availability-checking -parse-as-library) | %FileCheck %s
// TODO: move to target-run-simple-leaks-swift once CI is using at least Xcode 14.3

// rdar://110025115 - Temporarily disable this test
// REQUIRES: rdar110025115

// REQUIRES: concurrency
// REQUIRES: executable_test
// REQUIRES: concurrency_runtime

// UNSUPPORTED: back_deployment_runtime
// UNSUPPORTED: freestanding

// FIXME: enable discarding taskgroup on windows; rdar://104762037
// UNSUPPORTED: OS=windows-msvc

import _Concurrency

final class PayloadFirst {}
final class PayloadSecond {}

final class ErrorFirst: Error {
  let first: PayloadFirst

  init(file: String = #fileID, line: UInt = #line) {
    first = .init()
  }
  deinit {
    print("deinit \(self)")
  }
}

final class ErrorSecond: Error {
  let second: PayloadSecond

  init(file: String = #fileID, line: UInt = #line) {
    second = .init()
  }

  deinit {
    print("deinit \(self)")
  }
}

func shouldStartWith(_ lhs: Any, _ rhs: Any) {
  let l = "\(lhs)"
  let r = "\(rhs)"
  precondition(l.prefix("\(r)".count) == r, "'\(l)' did not start with '\(r)'")
}

// NOTE: Not as StdlibUnittest/TestSuite since these types of tests are unreasonably slow to load/debug.

@main struct Main {
  static func main() async {
    do {
      let got = try await withThrowingDiscardingTaskGroup() { group in
        group.addTask {
          1
        }
        group.addTask {
          throw ErrorFirst()
        }

        group.addTask {
          throw ErrorSecond()
        }

        return 12
      }
      fatalError("expected error to be re-thrown, got: \(got)")
    } catch {
       shouldStartWith(error, "main.Error")
    }
    // CHECK: deinit main.Error
    // CHECK: deinit main.Error
  }
}
