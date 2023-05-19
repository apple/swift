// RUN: %empty-directory(%t)
// RUN: %target-swift-frontend -emit-module -parse-as-library -o %t %S/Inputs/ModuleA.swift
// RUN: %target-swift-frontend -emit-module -parse-as-library -o %t %S/Inputs/ModuleB.swift
// RUN: %target-swift-emit-sil -parse-as-library -I%t %s -Xllvm -sil-disable-pass=initialize-static-globals -O | %FileCheck %s

import ModuleA
import ModuleB

var mygg = 29

// Check if we have one token: from mygg.
// Initializers from other modules are never fragile.

// CHECK: sil_global {{.*}} @[[T3:.*]]Wz

//@inlinable
public func sum() -> Int {
  return mygg + get_gg_a() + get_gg_b()
}

// Check if all the addressors are inlined.

// CHECK-LABEL: sil {{.*}}@$s15fragile_globals3sumSiyF
// CHECK-DAG: global_addr @[[T1:.*]]Wz
// CHECK-DAG: function_ref @[[T1]]WZ
// CHECK-DAG: global_addr @$s15fragile_globals4myggSivp
// CHECK-DAG: function_ref @$s7ModuleA2ggSivau
// CHECK-DAG: function_ref @$s7ModuleB2ggSivau
// CHECK: return

