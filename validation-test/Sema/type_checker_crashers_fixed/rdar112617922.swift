// RUN: not %target-swift-frontend %s -typecheck

enum MyError: Error {
  case unknown
}

extension Result {
  init<each S>(success: repeat each S, failure: Failure?) where Success == (repeat each S.Wrapped), Failure == any Error, repeat each S: OptionalProtocol {
  }
}

protocol OptionalProtocol {
  associatedtype Wrapped
  var asOptional: Optional<Wrapped> { get }
}

extension Optional: OptionalProtocol {
  var asOptional: Optional<Wrapped> { self }
}

func test<each EncodedResult: OptionalProtocol>(
    _ transformed: @escaping (Result<(repeat each EncodedResult.Wrapped), any Error>) -> Void
) -> ((any Error)?, repeat each EncodedResult) -> Void where repeat each EncodedResult.Wrapped: DataProtocol {
  return {
    transformed(Result(success: $1, failure: $0))
  }
}
