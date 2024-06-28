// RUN: %target-run-simple-swift(-Xfrontend -disable-availability-checking -swift-version 6)

// REQUIRES: concurrency
// REQUIRES: executable_test

// rdar://102155748
// UNSUPPORTED: back_deployment_runtime

func runIn10ms(_ closure: @escaping @Sendable () -> Void) {
  Task {
    try! await Task.sleep(nanoseconds: 10_000_000)
    closure()
  }
}

final class Weak: Sendable {
  let property = "Self exists"

  func test() {
    runIn10ms { [weak self] in
      if let self {
        // Use implicit self -- this should not result in a strong capture
        _ = property
        fatalError("Self was unexpectedly captured strongly")
      } else {
        print("Self was captured weakly (1)")
      }
    }

    runIn10ms { [weak self] in
      guard let self else {
        print("Self was captured weakly (2)")
        return
      }

      // Use implicit self -- this should not result in a strong capture
      _ = property

      runIn10ms { [self] in
        // Use implicit self -- this should not result in a strong capture
        _ = property
        fatalError("Self was unexpectedly captured strongly")
      }
    }
  }
}

Weak().test()
try await Task.sleep(nanoseconds: 30_000_000)
