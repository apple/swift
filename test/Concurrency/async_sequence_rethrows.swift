// RUN: %target-swift-frontend  -disable-availability-checking %s -emit-sil -o /dev/null -verify

// REQUIRES: concurrency

// Tests for the use of 'rethrows' on generic functions that have AsyncSequence
// and AsyncIteratorProtocol requirements.

func f1(_ seq: some AsyncSequence) async rethrows {
  for try await _ in seq { }
}

func f2(_ seq: some AsyncSequence, body: () throws -> Void) async rethrows {
  for try await _ in seq {
    try body()
  }
}

func f3(_ seq: some AsyncSequence, _ seq2: some AsyncSequence) async rethrows {
  for try await _ in seq {
  }

  for try await _ in seq2 {
  }
}

enum HomeworkError: Error {
case dogAteIt
}

func testCalls(x: some AsyncSequence<Int, Never>, y: any AsyncSequence<Int, any Error>) async throws {
  await f1(x)
  try await f1(y)

  await f2(x) { print("Hello") }
  try await f2(y) { print("Hello") }

  try await f2(x) { throw HomeworkError.dogAteIt }
  try await f2(y) { throw HomeworkError.dogAteIt }

  await f3(x, x)
  try await f3(x, y)
}

// Treat @rethrows protocols that inherit AsyncSequence like they are
// AsyncSequence for the purpose of rethrowing methods.
@rethrows
protocol InheritsAsyncSequence: AsyncSequence { }

extension InheritsAsyncSequence {
  func blah() async rethrows -> [Element] {
    try await self.reduce(into: [Element]()) { $0.append($1) }
  }
}

// Ensure that we can get the thrown error type from next().
struct Data { }

struct ErrorSequence<Element, Failure: Error>: AsyncSequence, AsyncIteratorProtocol {
    let throwError : Failure

    func makeAsyncIterator() -> ErrorSequence<Element, Failure> {
        self
    }

    mutating func next() async throws(Failure) -> Element? {
        throw throwError
    }
}

enum MyError: Error {
    case foo
}


func getASequence() -> any AsyncSequence<Data, MyError> {
    return ErrorSequence<Data, _>(throwError: MyError.foo) // ERROR: Cannot convert return expression of type 'any Error' to return type 'MyError'
}

// Test the default implementation of next() in terms of next(isolation:).
struct AsyncIteratorWithOnlyNextIsolation: AsyncIteratorProtocol {
  public mutating func next(isolation: (any Actor)?) throws(MyError) -> Int? { 0 }
}

// Test the default implementation of next(isolation:) in terms of next().
struct AsyncIteratorWithOnlyNext: AsyncIteratorProtocol {
  public mutating func next() throws(MyError) -> Int? { 0 }
}
