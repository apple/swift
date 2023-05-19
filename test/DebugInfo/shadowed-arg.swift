// RUN: %target-swift-frontend %s -parse-as-library -emit-ir -g -o - \
// RUN:   -module-name main | %FileCheck %s
// RUN: %target-swift-frontend %s -parse-as-library -emit-sil \
// RUN:   -Xllvm -sil-print-debuginfo -o - \
// RUN:   -module-name main | %FileCheck %s --check-prefix=SIL

// The variable i and the argument i must be in different scopes or the debugger
// doesn't know which one shadows the other.
public func f(i: Int) {
    let i = [i, i]
    print(i)
}

// CHECK: ![[S1:[0-9]+]] = distinct !DISubprogram(name: "f",
// CHECK: !DILocalVariable(name: "i", arg: 1, scope: ![[S1]],
// CHECK: !DILocalVariable(name: "i", scope: ![[S3:[0-9]+]],
// CHECK: ![[S3]] = distinct !DILexicalBlock(scope: ![[S1]],
// SIL: sil_scope [[S1:[0-9]+]] { {{.*}} parent @$s4main1f1iySi_tF
// SIL: sil_scope [[S2:[0-9]+]] { {{.*}} parent [[S1]] }
// SIL: sil_scope [[S3:[0-9]+]] { {{.*}} parent [[S1]] }
// SIL: debug_value %0 : $Int, let, name "i", argno 1,{{.*}}, scope [[S1]]
// SIL: debug_value {{.*}} : $Array<Int>, let, name "i", {{.*}}, scope [[S3]]
