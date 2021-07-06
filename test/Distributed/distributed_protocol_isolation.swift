// RUN: %target-typecheck-verify-swift -enable-experimental-distributed
// REQUIRES: concurrency
// REQUIRES: distributed

import _Distributed

// ==== -----------------------------------------------------------------------
// MARK: Good cases

@available(SwiftStdlib 5.5, *)
protocol DistProtocol: DistributedActor {
  func local() -> String
  func localAsync() async -> String

  distributed func dist() -> String
  distributed func dist(string: String) -> String

  distributed func distAsync() async -> String
  distributed func distThrows() throws -> String
  distributed func distAsyncThrows() async throws -> String
}

@available(SwiftStdlib 5.5, *)
distributed actor SpecificDist: DistProtocol {

  nonisolated func local() -> String { "hi" }
  nonisolated func localAsync() async -> String { "hi" }

  distributed func dist() -> String { "dist!" }
  distributed func dist(string: String) -> String { string }

  distributed func distAsync() async -> String { "dist!" }
  distributed func distThrows() throws -> String { "dist!" }
  distributed func distAsyncThrows() async throws -> String { "dist!" }

  func inside() async throws {
    _ = self.local() // ok
    _ = await self.localAsync() // ok

    _ = self.dist() // ok
    _ = self.dist(string: "") // ok
    _ = await self.distAsync() // ok
    _ = try self.distThrows() // ok
    _ = try await self.distAsyncThrows() // ok
  }
}

@available(SwiftStdlib 5.5, *)
func outside_good(dp: SpecificDist) async throws {
  _ = dp.local() // ok

  _ = try await dp.dist() // implicit async throws
  _ = try await dp.dist(string: "") // implicit async throws
  _ = try await dp.distAsync() // implicit throws
  _ = try await dp.distThrows() // implicit async
  _ = try await dp.distAsyncThrows() // ok
}

//@available(SwiftStdlib 5.5, *)
//func outside_good_generic<DP: DistProtocol>(dp: DP) async throws {
//  _ = try await dp.dist() // implicit async throws
////  _ = try await dp.dist(string: "") // implicit async throws
////  _ = try await dp.distAsync() // implicit throws
////  _ = try await dp.distThrows() // implicit async
////  _ = try await dp.distAsyncThrows() // ok
//}

//@available(SwiftStdlib 5.5, *)
//func outside_good_ext(dp: DistProtocol) async throws {
//  _ = try await dp.dist() // implicit async throws
////  _ = try await dp.dist(string: "") // implicit async throws
////  _ = try await dp.distAsync() // implicit throws
////  _ = try await dp.distThrows() // implicit async
////  _ = try await dp.distAsyncThrows() // ok
//}

// ==== -----------------------------------------------------------------------
// MARK: Error cases

@available(SwiftStdlib 5.5, *)
protocol ErrorCases: DistributedActor {
  distributed func unexpectedAsyncThrows() -> String
  // expected-note@-1{{protocol requires function 'unexpectedAsyncThrows()' with type '() -> String'; do you want to add a stub?}}
}

@available(SwiftStdlib 5.5, *)
distributed actor BadGreeter: ErrorCases {
  // expected-error@-1{{type 'BadGreeter' does not conform to protocol 'ErrorCases'}}

  distributed func unexpectedAsyncThrows() async throws -> String { "" }
  // expected-note@-1{{candidate is 'async', but protocol requirement is not}}
}
