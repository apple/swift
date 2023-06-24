// RUN: %target-swift-frontend -g -emit-sil %s -parse-as-library -module-name a | %FileCheck %s
open class C {
  public func fun() {}

  public func run() {
    { [weak self] in
      guard let self else { fatalError("cannot happen") }
      // CHECK: sil_scope [[LAMBDA:[0-9]+]] { loc "{{.*}}":6:5
      // CHECK: sil_scope [[BODY:[0-9]+]] { loc "{{.*}}":6:19 parent [[LAMBDA]]
      // CHECK: sil_scope [[LET:[0-9]+]] { loc "{{.*}}":7:7 parent [[BODY]]
      // CHECK: sil_scope [[GUARD:[0-9]+]] { loc "{{.*}}":7:17 parent [[LET]]
      // CHECK: debug_value {{.*}} : $C, let, name "self", {{.*}}, scope [[GUARD]]
      // CHECK: function_ref {{.*}}3fun{{.*}}, scope [[GUARD]]
      // CHECK-NEXT: apply {{.*}}, scope [[GUARD]]
      self.fun()
    }()
  }
}
