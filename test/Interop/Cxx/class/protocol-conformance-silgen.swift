// Tests that a C++ class can conform to a Swift protocol.

// RUN: %target-swift-emit-silgen -I %S/Inputs -enable-cxx-interop %s | %FileCheck %s

import ProtocolConformance

protocol HasReturn42 {
  mutating func return42() -> CInt
}

// CHECK-LABEL: sil private [transparent] [thunk] [ossa] @$sSo18ConformsToProtocolV4main11HasReturn42A2cDP8return42s5Int32VyFTW : $@convention(witness_method: HasReturn42) (@inout ConformsToProtocol) -> Int32
// CHECK: bb0([[ARG:%.*]] : $*ConformsToProtocol):
// CHECK: [[FN:%.*]] = function_ref @[[FN_NAME:.*]] : $@convention(cxx_method) (@inout ConformsToProtocol) -> Int32
// CHECK: [[OUT:%.*]] = apply %1(%0) : $@convention(cxx_method) (@inout ConformsToProtocol) -> Int32
// CHECK: return [[OUT]] : $Int32

// sil [clang ConformsToProtocol.return42] @[[FN_NAME]] : $@convention(c) (@inout ConformsToProtocol) -> Int32
extension ConformsToProtocol : HasReturn42 {}
