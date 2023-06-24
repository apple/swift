// RUN: %target-swift-frontend -module-name a -parse-as-library -emit-sil -g %s | %FileCheck %s

enum E {
case integerValue(Int?)
}

func getE() -> E? { return .integerValue(0) }

func f() -> Int {
    if case .integerValue(let nextValue) = getE(), let nextValue = nextValue {
// CHECK: sil_scope [[F:[0-9]+]] { loc "{{.*}}":9:6 parent @$s1a1fSiyF
// CHECK: sil_scope [[S0:[0-9]+]] { loc "{{.*}}":10:5 parent [[F]] }
// CHECK: sil_scope [[S1:[0-9]+]] { loc "{{.*}}":10:44 parent [[S0]] }
// CHECK: sil_scope [[S2:[0-9]+]] { loc "{{.*}}":10:44 parent [[S0]] }
// CHECK: sil_scope [[S3:[0-9]+]] { loc "{{.*}}":10:68 parent [[S2]] }
// CHECK: sil_scope [[S4:[0-9]+]] { loc "{{.*}}":10:78 parent [[S3]] }
// CHECK: debug_value {{.*}}: $Optional<Int>, let, name "nextValue", {{.*}}:10:31, scope [[S2]]
// CHECK: debug_value {{.*}}: $Int, let, name "nextValue", {{.*}}:10:56, scope [[S3]]
        return nextValue
    }
    return 0
}
