// RUN: %target-swift-frontend %s -emit-ir -g -o - | %FileCheck %s
class C {
  func withClosure(_ : () -> ()) -> () {}

  func f() {
    // CHECK-LABEL: define{{.*}}$s11capturelist1CC1fyyFyyXEfU_
    // There should not be a local weak variable "self" shadowing the
    // implicit self argument.
    // let self
    // CHECK: call void @llvm.dbg
    // let s
    // CHECK: call void @llvm.dbg
    // var weak self
    // CHECK-NOT: call void @llvm.dbg
    // CHECK-LABEL: ret void
    withClosure { [weak self] in
      guard let s = self else { return }
    }
  }
}
