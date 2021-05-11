// RUN: %empty-directory(%t)
// RUN: %target-swift-frontend -emit-sib -emit-module-summary-path %t/function_summary.swiftmodule.summary -module-name function_summary -Xllvm -module-summary-embed-debug-name %s
// RUN: llvm-bcanalyzer -dump %t/function_summary.swiftmodule.summary | %FileCheck %s -check-prefix BCANALYZER

// BCANALYZER-NOT: UnknownCode

// RUN: %swift-module-summary-test --to-yaml %t/function_summary.swiftmodule.summary -o %t/function_summary.summary.yaml

// `bar` references `foo` directly
// RUN: cat %t/function_summary.summary.yaml | %FileCheck %s -check-prefix DIRECT-CALL

// DIRECT-CALL:      15904760426814321987:
// DIRECT-CALL-NEXT:   name:            '$s16function_summary3baryyF'
// DIRECT-CALL-NEXT:   guid:            15904760426814321987
// DIRECT-CALL-NEXT:   live:            false
// DIRECT-CALL-NEXT:   preserved:       false
// DIRECT-CALL-NEXT:   calls:
// DIRECT-CALL-NEXT:     - callee_name:     '$s16function_summary3fooyyF'
// DIRECT-CALL-NEXT:       callee_guid:     3365867516365370991
// DIRECT-CALL-NEXT:       kind:            direct
func foo() {}

func bar() {
    foo()
}

// `useGenericP` and `useExistentialP` reference `#P.protoMember` through witness table
// RUN: cat %t/function_summary.summary.yaml | %FileCheck %s -check-prefix WITNESS-CALL

// WITNESS-CALL:      2534322708691595658:
// WITNESS-CALL-NEXT:   name:            '$s16function_summary15useExistentialPyyAA1P_pF'
// WITNESS-CALL-NEXT:   guid:            2534322708691595658
// WITNESS-CALL-NEXT:   live:            false
// WITNESS-CALL-NEXT:   preserved:       false
// WITNESS-CALL-NEXT:   calls:
// WITNESS-CALL-NEXT:     - callee_name:     '$s16function_summary1PP11protoMemberyyF'
// WITNESS-CALL-NEXT:       callee_guid:     12061107285276415735
// WITNESS-CALL-NEXT:       kind:            witness

protocol P {
    func protoMember()
}

func useExistentialP(_ v: P) {
    v.protoMember()
}

// `useClassC` reference `#P.classMember` through vtable
// RUN: cat %t/function_summary.summary.yaml | %FileCheck %s -check-prefix VTABLE-CALL

// VTABLE-CALL:      6451800047657108456:
// VTABLE-CALL-NEXT:   name:            '$s16function_summary9useClassCyyAA1CCF'
// VTABLE-CALL-NEXT:   guid:            6451800047657108456
// VTABLE-CALL-NEXT:   live:            false
// VTABLE-CALL-NEXT:   preserved:       false
// VTABLE-CALL-NEXT:   calls:
// VTABLE-CALL-NEXT:     - callee_name:     '$s16function_summary1CC11classMemberyyF'
// VTABLE-CALL-NEXT:       callee_guid:     7506985369146111998
// VTABLE-CALL-NEXT:       kind:            vtable

class C {
    func classMember() {}
}

class D : C {
    override func classMember() {}
}

func useClassC(_ v: C) {
    v.classMember()
}
