// RUN: %empty-directory(%t)
// RUN: %target-swift-frontend -emit-module -enable-library-evolution -enable-testing -emit-module-path=%t/resilient_struct.swiftmodule %S/../Inputs/resilient_struct.swift
// RUN: %target-swift-emit-silgen -I %t %s | %FileCheck %s

@testable import resilient_struct

// CHECK-LABEL: sil [ossa] @$s26struct_resilience_testable37takesResilientStructWithInternalFieldySi010resilient_A00eghI0VF : $@convention(thin) (@in_guaranteed ResilientWithInternalField) -> Int
// CHECK: [[FN:%.*]] = function_ref @$s16resilient_struct26ResilientWithInternalFieldV1xSivg : $@convention(method) (@in_guaranteed ResilientWithInternalField) -> Int
// CHECK: [[RESULT:%.*]] = apply [[FN]](%0)
// CHECK: return [[RESULT]]

public func takesResilientStructWithInternalField(_ s: ResilientWithInternalField) -> Int {
  return s.x
}
