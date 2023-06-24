// RUN: %target-swift-frontend -g -emit-sil %s -parse-as-library -module-name a | %FileCheck %s
func use<T>(_ t: T) {}
public func f(value: String?) {
  // CHECK: sil_scope [[S0:[0-9]+]] { loc "{{.*}}":3:13
  if let value, let value = Int(value) {
    // CHECK: sil_scope [[S1:[0-9]+]] { loc "{{.*}}":5:3
    // CHECK: sil_scope [[S2:[0-9]+]] { loc "{{.*}}":5:10
    // CHECK: sil_scope [[S3:[0-9]+]] { loc "{{.*}}":5:29 parent [[S2]] }
    // CHECK: sil_scope [[S4:[0-9]+]] { loc "{{.*}}":5:29 parent [[S2]] }
    // CHECK: sil_scope [[S5:[0-9]+]] { loc "{{.*}}":5:40 parent [[S4]] }
    // CHECK: debug_value {{.*}} : $Optional<String>, let, name "value", {{.*}}, scope [[S0]]
    // CHECK: debug_value {{.*}} : $String, let, name "value", {{.*}}, scope [[S2]]
    // CHECK: debug_value {{.*}} : $Int, let, name "value", {{.*}}, scope [[S4]]
    use((value))
  }
}
