// RUN: %target-swift-frontend  -disable-availability-checking %s -emit-sil -o /dev/null -verify
// RUN: %target-swift-frontend  -disable-availability-checking %s -emit-sil -o /dev/null -verify -strict-concurrency=targeted
// RUN: %target-swift-frontend  -disable-availability-checking %s -emit-sil -o /dev/null -verify -strict-concurrency=complete -verify-additional-prefix tns-

// REQUIRES: concurrency
// REQUIRES: asserts
// REQUIRES: libdispatch

@available(SwiftStdlib 5.1, *)
func asyncFunc() async -> Int { 42 }
@available(SwiftStdlib 5.1, *)
func asyncThrowsFunc() async throws -> Int { 42 }
@available(SwiftStdlib 5.1, *)
func asyncThrowsOnCancel() async throws -> Int {
  // terrible suspend-spin-loop -- do not do this
  // only for purposes of demonstration
  while Task.isCancelled {
    try? await Task.sleep(nanoseconds: 1_000_000_000)
  }

  throw CancellationError()
}

@available(SwiftStdlib 5.1, *)
func test_taskGroup_add() async throws -> Int {
  try await withThrowingTaskGroup(of: Int.self) { group in
    group.addTask {
      await asyncFunc()
    }

    group.addTask {
      await asyncFunc()
    }

    var sum = 0
    while let v = try await group.next() {
      sum += v
    }
    return sum
  } // implicitly awaits
}

// ==== ------------------------------------------------------------------------
// MARK: Example group Usages

struct Boom: Error {}
@available(SwiftStdlib 5.1, *)
func work() async -> Int { 42 }
@available(SwiftStdlib 5.1, *)
func boom() async throws -> Int { throw Boom() }

@available(SwiftStdlib 5.1, *)
func first_allMustSucceed() async throws {

  let first: Int = try await withThrowingTaskGroup(of: Int.self) { group in
    group.addTask { await work() }
    group.addTask { await work() }
    group.addTask { try await boom() }

    if let first = try await group.next() {
      return first
    } else {
      fatalError("Should never happen, we either throw, or get a result from any of the tasks")
    }
    // implicitly await: boom
  }
  _ = first
  // Expected: re-thrown Boom
}

@available(SwiftStdlib 5.1, *)
func first_ignoreFailures() async throws {
  @Sendable func work() async -> Int { 42 }
  @Sendable func boom() async throws -> Int { throw Boom() }

  let first: Int = try await withThrowingTaskGroup(of: Int.self) { group in
    group.addTask { await work() }
    group.addTask { await work() }
    group.addTask {
      do {
        return try await boom()
      } catch {
        return 0 // TODO: until try? await works properly
      }
    }

    var result: Int = 0
    while let v = try await group.next() {
      result = v

      if result != 0 {
        break
      }
    }

    return result
  }
  _ = first
  // Expected: re-thrown Boom
}

// ==== ------------------------------------------------------------------------
// MARK: Advanced Custom Task Group Usage

@available(SwiftStdlib 5.1, *)
func test_taskGroup_quorum_thenCancel() async {
  // imitates a typical "gather quorum" routine that is typical in distributed systems programming
  enum Vote {
    case yay
    case nay
  }
  struct Follower: Sendable {
    init(_ name: String) {}
    func vote() async throws -> Vote {
      // "randomly" vote yes or no
      return .yay
    }
  }

  /// Performs a simple quorum vote among the followers.
  ///
  /// - Returns: `true` iff `N/2 + 1` followers return `.yay`, `false` otherwise.
  func gatherQuorum(followers: [Follower]) async -> Bool {
    try! await withThrowingTaskGroup(of: Vote.self) { group in
      for follower in followers {
        group.addTask { try await follower.vote() }
      }

      defer {
        group.cancelAll()
      }

      var yays: Int = 0
      var nays: Int = 0
      let quorum = Int(followers.count / 2) + 1
      while let vote = try await group.next() {
        switch vote {
        case .yay:
          yays += 1
          if yays >= quorum {
            // cancel all remaining voters, we already reached quorum
            return true
          }
        case .nay:
          nays += 1
          if nays >= quorum {
            return false
          }
        }
      }

      return false
    }
  }

  _ = await gatherQuorum(followers: [Follower("A"), Follower("B"), Follower("C")])
}

// FIXME: this is a workaround since (A, B) today isn't inferred to be Sendable
//        and causes an error, but should be a warning (this year at least)
@available(SwiftStdlib 5.1, *)
struct SendableTuple2<A: Sendable, B: Sendable>: Sendable {
  let first: A
  let second: B

  init(_ first: A, _ second: B) {
    self.first = first
    self.second = second
  }
}

@available(SwiftStdlib 5.1, *)
extension Collection where Self: Sendable, Element: Sendable, Self.Index: Sendable {

  /// Just another example of how one might use task groups.
  func map<T: Sendable>(
    parallelism requestedParallelism: Int? = nil/*system default*/,
    // ordered: Bool = true, /
    _ transform: @Sendable (Element) async throws -> T // expected-note {{parameter 'transform' is implicitly non-escaping}}
  ) async throws -> [T] { // TODO: can't use rethrows here, maybe that's just life though; rdar://71479187 (rethrows is a bit limiting with async functions that use task groups)
    let defaultParallelism = 2
    let parallelism = requestedParallelism ?? defaultParallelism

    let n = self.count
    if n == 0 {
      return []
    }

    return try await withThrowingTaskGroup(of: SendableTuple2<Int, T>.self) { group in
      var result = ContiguousArray<T>()
      result.reserveCapacity(n)

      var i = self.startIndex
      var submitted = 0

      func submitNext() async throws {
        // The reason that we emit an error here is b/c we capture the var box
        // to i and that is task isolated. This is the region isolation version
        // of the 'escaping closure captures non-escaping parameter' error.
        //
        // TODO: When we have isolation history, isolation history will be able
        // to tell us what is going on.
        group.addTask { [submitted,i] in // expected-error {{escaping closure captures non-escaping parameter 'transform'}}
          // expected-tns-warning @-1 {{task-isolated value of type '() async throws -> SendableTuple2<Int, T>' passed as a strongly transferred parameter}}
          let _ = try await transform(self[i]) // expected-note {{captured here}}
          let value: T? = nil
          return SendableTuple2(submitted, value!)
        }
        submitted += 1
        formIndex(after: &i)
      }

      // submit first initial tasks
      for _ in 0..<parallelism {
        try await submitNext()
      }

      while let tuple = try await group.next() {
        let index = tuple.first
        let taskResult = tuple.second
        result[index] = taskResult

        try Task.checkCancellation()
        try await submitNext()
      }

      assert(result.count == n)
      return Array(result)
    }
  }
}

