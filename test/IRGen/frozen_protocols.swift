// RUN: %empty-directory(%t)
// RUN: %target-swift-frontend -emit-module -enable-library-evolution -emit-module-path=%t/resilient_protocol.swiftmodule -module-name=resilient_protocol %S/../Inputs/resilient_protocol.swift
// RUN: %target-swift-frontend -I %t -emit-ir -enable-library-evolution %s | %FileCheck %s -DINT=i%target-ptrsize
// RUN: %target-swift-frontend -I %t -emit-ir -enable-library-evolution -O %s

import resilient_protocol

@_fixed_layout public protocol FrozenProtocol {
  func protocolMethod()
}

public func callOtherProtocolMethod<T : OtherFrozenProtocol>(_ t: T) {
  t.protocolMethod()
}

public struct ConformsToFrozenProtocol : FrozenProtocol {
  public func protocolMethod() {}
}

// CHECK-LABEL: @"$s16frozen_protocols24ConformsToFrozenProtocolVAA0eF0AAMc" = {{(dllexport )?}}{{(protected )?}}constant %swift.protocol_conformance_descriptor

// -- the protocol
// CHECK-SAME: @"$s16frozen_protocols14FrozenProtocolMp"

// -- the conforming type
// CHECK-SAME: @"$s16frozen_protocols24ConformsToFrozenProtocolVMn"

// -- the witness table
// CHECK-SAME: @"$s16frozen_protocols24ConformsToFrozenProtocolVAA0eF0AAWP"

// -- flags
// CHECK-SAME: i32 0
// CHECK-SAME: }

// @_fixed_layout protocols still emit protocol requirement descriptors though,
// which allows for a protocol to be retroactively declared as @_fixed_layout
// CHECK-LABEL: @"$s16frozen_protocols14FrozenProtocolP14protocolMethodyyFTq" = {{(dllexport )?}}{{(protected )?}}alias %swift.protocol_requirement

// Requirements in @_fixed_layout protocols are called by direct witness
// table lookup

// CHECK-LABEL: define{{( dllexport)?}}{{( protected)?}} swiftcc void @"$s16frozen_protocols23callOtherProtocolMethodyyx18resilient_protocol0d6FrozenE0RzlF"(ptr noalias nocapture %0, ptr %T, ptr %T.OtherFrozenProtocol)
// CHECK: [[ADDR:%.*]] = getelementptr inbounds ptr, ptr %T.OtherFrozenProtocol, i32 1
// CHECK: [[FN:%.*]] = load ptr, ptr [[ADDR]]
// CHECK: call swiftcc void [[FN]](ptr noalias nocapture swiftself %0, ptr %T, ptr %T.OtherFrozenProtocol)
// CHECK: ret void

// @_fixed_layout protocols still emit method dispatch thunks though, which
// allows for a protocol to be retroactively declared as @_fixed_layout
// CHECK-LABEL: define{{( dllexport)?}}{{( protected)?}} swiftcc void @"$s16frozen_protocols14FrozenProtocolP14protocolMethodyyFTj"(

