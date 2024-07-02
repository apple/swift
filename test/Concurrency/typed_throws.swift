// RUN: %target-typecheck-verify-swift

// REQUIRES: concurrency

enum MyError: Error {
  case failed
  case epicFailed
}

@available(SwiftStdlib 6.0, *)
func testAsyncFor<S: AsyncSequence>(seq: S) async throws(MyError) {
  // expected-error@+1{{thrown expression type 'S.Failure' cannot be converted to error type 'MyError'}}
  for try await _ in seq {
  }
}

@available(SwiftStdlib 6.0, *)
func testAsyncFor<S: AsyncSequence>(seq: S) async throws(MyError)
  where S.Failure == MyError
{
  for try await _ in seq {
  }
}

@available(SwiftStdlib 6.0, *)
func testTask() async throws(MyError)  {
  let t: Task<Int, MyError> = Task { () throws(MyError) -> Int in
    throw MyError.failed
  }

  _ = try await t.value
}

@available(SwiftStdlib 6.0, *)
func testTaskDetached() async throws(MyError)  {
  let t: Task<Int, MyError> = Task.detached { () throws(MyError) -> Int in
    throw MyError.failed
  }

  _ = try await t.value
}
