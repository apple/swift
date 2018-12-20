// RUN: %target-swift-frontend -emit-sil %s | %FileCheck %s
// RUN: %target-swift-frontend -emit-sil -Xllvm -differentiation-use-vjp=false %s | %FileCheck %s

@inline(never)
func print<T>(_ x: T) {
  Swift.print(x)
}

func squared(_ x: Float) -> Float {
  print("test output")
  return x * x
}

_ = gradient(at: 20, in: squared)

// CHECK-LABEL: sil hidden @{{.*}}squared{{.*}}__primal_src_0_wrt_0
// CHECK: [[PV:%.*]] = struct ${{.*}}squared{{.*}}__Type__src_0_wrt_0 ({{.*}} : $Builtin.FPIEEE32)
// CHECK: [[RESULT:%.*]] = tuple ([[PV]] : ${{.*}}squared{{.*}}__Type__src_0_wrt_0, {{.*}} : $Float)
// CHECK: return %19 : $({{.*}}squared{{.*}}__Type__src_0_wrt_0, Float)
