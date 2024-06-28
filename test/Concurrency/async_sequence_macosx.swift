// RUN: %target-swift-frontend -target %target-cpu-apple-macos14.0 %s -emit-sil -o /dev/null -verify -swift-version 6

// REQUIRES: concurrency, OS=macosx

func acceptClosure(_: () async throws -> Void) { }
  
@available(macOS 13.0, *)
func f<S: AsyncSequence>(s: S) async throws {
  acceptClosure {
    if #available(SwiftStdlib 6.0, *) {
      for try await x in s {
        print(x)
      }
    }
  }
}

// Make sure we don't complain about crossing a concurrency boundary here.
@MainActor
class Store<Action: Sendable> {
    private func intercept(_ action: Action) async throws {
        await withTaskGroup(of: Optional<Action>.self) { group in
            for await case let nextAction? in group {
                _ = nextAction
            }
        }

        try await withThrowingTaskGroup(of: Optional<Action>.self) { group in
            for try await case let nextAction? in group {
                _ = nextAction
            }
        }

    }
}
