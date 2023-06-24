7// RUN: %empty-directory(%t)
// RUN: %target-swift-emit-module-interface(%t/Library.swiftinterface) %s -module-name Library -enable-experimental-feature MoveOnlyResilientTypes
// RUN: %target-swift-typecheck-module-from-interface(%t/Library.swiftinterface) -I %t -enable-experimental-feature MoveOnlyResilientTypes
// RUN: %FileCheck %s < %t/Library.swiftinterface

// this test makes sure that decls containing a move-only type are guarded by the $MoveOnly feature flag

// CHECK:       #if compiler(>=5.3) && $MoveOnly
// CHECK-NEXT:    @_moveOnly public struct MoveOnlyStruct {

// CHECK:       #if compiler(>=5.3) && $MoveOnly
// CHECK-NEXT:    @_moveOnly public struct MoveOnlyStructSupp {

// CHECK:      #if compiler(>=5.3) && $MoveOnly
// CHECK-NEXT:   @_moveOnly public enum MoveOnlyEnum {

// CHECK:      #if compiler(>=5.3) && $MoveOnly
// CHECK-NEXT:   @_moveOnly public enum MoveOnlyEnumSupp {

// CHECK:      #if compiler(>=5.3) && $MoveOnly
// CHECK-NEXT:   public func someFn() -> Library.MoveOnlyEnum

// CHECK:     public class What {
// CHECK:       #if compiler(>=5.3) && $MoveOnly
// CHECK-NEXT:    public func diamonds(_ f: (borrowing Library.MoveOnlyStruct) -> Swift.Int)

// CHECK: #if compiler(>=5.3) && $MoveOnly
// CHECK-NEXT:  extension Library.MoveOnlyStruct {

@_moveOnly public struct MoveOnlyStruct {
  let x = 0
}

public struct MoveOnlyStructSupp: ~Copyable {
  let x = 0
}

@_moveOnly public enum MoveOnlyEnum {
  case depth
}

public enum MoveOnlyEnumSupp: ~Copyable {
  case depth
}

public func someFn() -> MoveOnlyEnum { return .depth }

public class What {
  public func diamonds(_ f: (borrowing MoveOnlyStruct) -> Int) {}
}

public extension MoveOnlyStruct {
  func who() {}
}
