// RUN: %target-swift-frontend -module-name a -parse-as-library -emit-sil -g %s | %FileCheck %s
func consume<T>(_ t: T) {}

// CHECK: sil_scope [[F:[0-9]+]] { loc "{{.*}}":9:13 parent @$s1a1fyyShySiGSg_ADtF
// CHECK: sil_scope [[S0:[0-9]+]] { loc "{{.*}}":10:3 parent [[F]] }
// CHECK: sil_scope [[S1:[0-9]+]] { loc "{{.*}}":11:3 parent [[S0]] }
// CHECK: sil_scope [[S2:[0-9]+]] { loc "{{.*}}":13:5 parent [[S1]] }
// CHECK: sil_scope [[S3:[0-9]+]] { loc "{{.*}}":14:3 parent [[S0]] }
public func f(_ s1: Set<Int>?, _ s2: Set<Int>?) {
  switch (s1, s2) {
  case (nil, let a), (let a, nil):
  // CHECK: debug_value {{.*}} $Optional<Set<Int>>, let, name "a", {{.*}}:11:18, scope [[S2]]
    consume(a)
  case (let a?, _):
  // CHECK: debug_value {{.*}} $Set<Int>, let, name "a", {{.*}}:14:13, scope [[S3]]
    consume((a))
  }
}
