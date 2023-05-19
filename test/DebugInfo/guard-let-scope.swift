// RUN: %target-swift-frontend -emit-sil -Xllvm -sil-print-debuginfo %s \
// RUN:  | %FileCheck %s
func use<T>(_ t: T) {}

func f(c: AnyObject??) {
  let x = c
  guard let x = x, let x = x else {
  // CHECK: sil_scope [[S3:[0-9]+]] { {{.*}} parent @{{.*}}1f
  // CHECK: sil_scope [[S4:[0-9]+]] { {{.*}} parent [[S3]] }
  // CHECK: sil_scope [[S5:[0-9]+]] { {{.*}} parent [[S3]] }
  // CHECK: sil_scope [[S6:[0-9]+]] { loc "{{.*}}":7:3 parent [[S5]] }
  // CHECK: sil_scope [[S7:[0-9]+]] { loc "{{.*}}":7:17 parent [[S6]] }
  // CHECK: sil_scope [[S8:[0-9]+]] { loc "{{.*}}":7:28 parent [[S7]] }
  // CHECK: debug_value %{{.*}} : $Optional<Optional<AnyObject>>, let, name "x"{{.*}} scope [[S5]]
  // CHECK: debug_value %{{.*}} : $Optional<AnyObject>, let, name "x", {{.*}} scope [[S7]]
  // CHECK: debug_value %{{.*}} : $AnyObject, let, name "x", {{.*}} scope [[S8]]
    fatalError()
  }
  // CHECK: function_ref {{.*3use.*}} scope [[S8]]
  use(x)
}
