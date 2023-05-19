// RUN: %target-swift-frontend -g -emit-sil %s -parse-as-library -module-name a | %FileCheck %s
func use<T>(_ t: T) {}
public func f(value: String?) {
  // CHECK: sil_scope [[S0:[0-9]+]] { loc "{{.*}}":[[@LINE-1]]:13
  if let value, let value = Int(value) {
    // CHECK: sil_scope [[S1:[0-9]+]] { loc "{{.*}}":[[@LINE-1]]:10
    // CHECK: sil_scope [[S2:[0-9]+]] { loc "{{.*}}":[[@LINE-2]]:29 parent [[S1]] }
    // CHECK: debug_value {{.*}} : $Optional<String>, let, name "value", {{.*}}, scope [[S0]]
    // CHECK: debug_value {{.*}} : $String, let, name "value", {{.*}}, scope [[S1]]
    // CHECK: debug_value {{.*}} : $Int, let, name "value", {{.*}}, scope [[S2]]
    use((value))
  }
}
