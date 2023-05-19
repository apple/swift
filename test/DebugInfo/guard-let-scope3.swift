// RUN: %target-swift-frontend -g -emit-sil %s -parse-as-library -module-name a | %FileCheck %s
public class C {}
public enum MyError : Error {
  init() { self.init() }
}
public class S {
  private var c = [Int : C?]()
  public func f(_ i: Int) throws  -> C {
    guard let x = c[i], let x else {
      // CHECK: sil_scope [[P:[0-9]+]] { loc "{{.*}}":[[@LINE-1]]:5
      // CHECK: sil_scope [[X1:[0-9]+]] { loc "{{.*}}":[[@LINE-2]]:19 parent [[P]]
      // CHECK: sil_scope [[X2:[0-9]+]] { loc "{{.*}}":[[@LINE-3]]:29 parent [[X1]]
      // CHECK: sil_scope [[GUARD:[0-9]+]] { loc "{{.*}}":[[@LINE-4]]:36 parent [[P]]
      // CHECK: debug_value {{.*}} : $Optional<C>, let, name "x", {{.*}}, scope [[X1]]
      // CHECK: debug_value {{.*}} : $C, let, name "x", {{.*}}, scope [[X2]]
      // CHECK-NEXT:  scope [[X2]]
      throw MyError()
      // CHECK: function_ref {{.*}}MyError{{.*}}:[[@LINE-1]]:13, scope [[GUARD]]
    }
    return x
  }
}
