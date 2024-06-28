// RUN: %target-swift-frontend -emit-sil -verify -o /dev/null -disable-availability-checking %s -swift-version 6 -strict-concurrency=complete

// REQUIRES: concurrency
// REQUIRES: asserts

actor A {
  func g() { }
  func h() throws { }
  
  func f() async throws {
    await withTaskGroup(of: Int.self, returning: Void.self) { group in
      self.g()
    }

    try await withThrowingTaskGroup(of: String.self, returning: Void.self) { group in
      try self.h()
    }

    await withDiscardingTaskGroup(returning: Void.self) { group in
      self.g()
    }

    try await withThrowingDiscardingTaskGroup(returning: Void.self) { group in
      try self.h()
    }
  }
}
