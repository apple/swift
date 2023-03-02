// RUN: %target-typecheck-verify-swift -enable-experimental-move-only -debug-constraints 2>&1 | %FileCheck %s

class Klass {}

struct S {
    var k = Klass()
}

struct S2 {
    var s = S()
}

func useKlass(_ k: Klass) {}
func useS2(_ s2: S2) {}

func test1() {
    var s2 = S2()
    s2 = S2()

    // CHECK-LABEL: (load_expr implicit type='S2' location={{.*}}/immutable_lvalue.swift:23:11 range=[{{.*}}/immutable_lvalue.swift:23:11 - line:23:19]
    // CHECK: (borrow_expr type='@lvalue @immutable S2' location={{.*}}/immutable_lvalue.swift:23:11 range=[{{.*}}/immutable_lvalue.swift:23:11 - line:23:19]
    // CHECK: (declref_expr type='@lvalue @immutable S2' location={{.*}}/immutable_lvalue.swift:23:19 range=[{{.*}}/immutable_lvalue.swift:23:19 - line:23:19] decl=immutable_lvalue.(file).test1().s2@{{.*}}/immutable_lvalue.swift:17:9 function_ref=unapplied))))))
    useS2(_borrow s2)

    // CHECK-LABEL: (load_expr implicit type='Klass' location={{.*}}/immutable_lvalue.swift:30:14 range=[{{.*}}/immutable_lvalue.swift:30:14 - line:30:27]
    // CHECK: (borrow_expr type='@lvalue @immutable Klass' location={{.*}}/immutable_lvalue.swift:30:14 range=[{{.*}}/immutable_lvalue.swift:30:14 - line:30:27]
    // CHECK: (member_ref_expr type='@lvalue @immutable Klass' location={{.*}}/immutable_lvalue.swift:30:27 range=[{{.*}}/immutable_lvalue.swift:30:22 - line:30:27] decl=immutable_lvalue.(file).S.k@{{.*}}/immutable_lvalue.swift:6:9
    // CHECK: (member_ref_expr type='@lvalue @immutable S' location={{.*}}/immutable_lvalue.swift:30:25 range=[{{.*}}/immutable_lvalue.swift:30:22 - line:30:25] decl=immutable_lvalue.(file).S2.s@{{.*}}/immutable_lvalue.swift:10:9
    // CHECK: (declref_expr type='@lvalue @immutable S2' location={{.*}}/immutable_lvalue.swift:30:22 range=[{{.*}}/immutable_lvalue.swift:30:22 - line:30:22] decl=immutable_lvalue.(file).test1().s2@{{.*}}/immutable_lvalue.swift:17:9 function_ref=unapplied))))))))
    useKlass(_borrow s2.s.k)
}
