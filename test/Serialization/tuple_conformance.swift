// RUN: %empty-directory(%t)
// RUN: %target-swift-frontend -emit-module %S/Inputs/tuple_conformance_other.swift -emit-module-path %t/lib.swiftmodule -module-name lib
// RUN: %target-swift-frontend -emit-sil %s -I %t | %FileCheck %s
import lib

public func doStuff() {
  print(makeVoidG())
}

// CHECK-LABEL: sil public_external [transparent] @$s3lib9makeVoidGAA1GVyytGyF : $@convention(thin) () -> G<()> {
// CHECK: [[FN:%.*]] = function_ref @$s3lib5makeGyAA1GVyxGxms8SendableRzlF : $@convention(thin) <τ_0_0 where τ_0_0 : Sendable> (@thick τ_0_0.Type) -> G<τ_0_0>
// CHECK-NEXT: [[RESULT:%.*]] = apply [[FN]]<()>({{%.*}}) : $@convention(thin) <τ_0_0 where τ_0_0 : Sendable> (@thick τ_0_0.Type) -> G<τ_0_0>
// CHECK-NEXT: return [[RESULT]] : $G<()>
