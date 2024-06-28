// RUN: %target-swift-frontend -emit-ir -g -o - -module-name test -strict-concurrency=complete -swift-version 5 -enable-upcoming-feature SendingArgsAndResults -disable-availability-checking %s | %FileCheck %s

// Test that we can properly reconstruct sending from various tests when
// emitting debug info. Only place examples in here that have already failed.

public struct SendableStruct: Sendable {
}

// This verifies that we can properly type reconstruct:
//
//   $ss6ResultOy4test14SendableStructVs5Error_pGIeggT_D
//
// Which is:
//
//   @escaping @callee_guaranteed (@guaranteed sending Swift.Result<test.SendableStruct, Swift.Error>) -> ()
//
// CHECK: !{{[0-9]+}} = !DICompositeType(tag: DW_TAG_structure_type, name: "$ss6ResultOy4test14SendableStructVs5Error_pGIeggT_D",
func testReconstructingEscapingClosureWithSendingParam() async throws -> SendableStruct {
  func callSendableFunction(_ x: @Sendable () -> ()) {}

  func helper(_ completion: @escaping (__shared sending Result<SendableStruct, Swift.Error>) -> Void) {
    fatalError()
  }

  return try await withCheckedThrowingContinuation { continuation in
    callSendableFunction {
      helper(continuation.resume(with:))
    }
  }
}
