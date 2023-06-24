// RUN: %target-swift-frontend %s -emit-ir -g -o - \
// RUN:    -module-name M  -disable-availability-checking \
// RUN:    -parse-as-library | %FileCheck %s --check-prefix=CHECK --check-prefix=CHECK-%target-ptrsize

// REQUIRES: concurrency

public func getVegetables() async -> [String] {
  return ["leek", "carrot"]  
}

// CHECK: define {{.*}} @"$s1M14chopVegetablesSaySSGyYaKFTQ0_"
public func chopVegetables() async throws -> [String] {
  let veggies = await getVegetables()
  // CHECK-NOT: {{^define }}
  // CHECK:  call void @llvm.dbg.declare(metadata ptr %0, metadata ![[V:[0-9]+]], metadata !DIExpression(DW_OP_deref, DW_OP_plus_uconst, {{[0-9]+}}, DW_OP_plus_uconst, {{[0-9]+}})
  // CHECK: ![[V]] = !DILocalVariable(name: "veggies"
  return veggies.map { "chopped \($0)" }
}
