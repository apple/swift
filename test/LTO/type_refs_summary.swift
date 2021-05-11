// RUN: %empty-directory(%t)
// RUN: %target-swift-frontend -emit-sib -emit-module-summary-path %t/type_refs.swiftmodule.summary -module-name type_refs -Xllvm -module-summary-embed-debug-name %s
// RUN: llvm-bcanalyzer -dump %t/type_refs.swiftmodule.summary | %FileCheck %s -check-prefix BCANALYZER

// BCANALYZER-NOT: UnknownCode

// RUN: %swift-module-summary-test --to-yaml %t/type_refs.swiftmodule.summary -o %t/type_refs.summary.yaml

// Ensure that type reference to S, C and D are recorded
// RUN: cat %t/type_refs.summary.yaml | %FileCheck %s -check-prefix SIMPLE-COERCE

// SIMPLE-COERCE:      name:            '$s9type_refs11coerceToAnyyypAA1DCF'
// SIMPLE-COERCE-NEXT: guid:            14079990488171131
// SIMPLE-COERCE-NEXT: live:            false
// SIMPLE-COERCE-NEXT: preserved:       false
// SIMPLE-COERCE-NEXT: calls:           []
// SIMPLE-COERCE-NEXT: type_refs:
// SIMPLE-COERCE-NEXT:   - name:            '$s9type_refs1DC'
// SIMPLE-COERCE-NEXT:     guid:            9126595621082655001

// SIMPLE-COERCE:      name:            '$s9type_refs11coerceToAnyyypAA1CCF'
// SIMPLE-COERCE-NEXT: guid:            1940219073901329473
// SIMPLE-COERCE-NEXT: live:            false
// SIMPLE-COERCE-NEXT: preserved:       false
// SIMPLE-COERCE-NEXT: calls:           []
// SIMPLE-COERCE-NEXT: type_refs:
// SIMPLE-COERCE-NEXT:   - name:            '$s9type_refs1CC'
// SIMPLE-COERCE-NEXT:     guid:            3331627721515121492

// SIMPLE-COERCE:      name:            '$s9type_refs9coerceToPyAA1P_pAA1SVF'
// SIMPLE-COERCE-NEXT: guid:            15452386893050095333
// SIMPLE-COERCE-NEXT: live:            false
// SIMPLE-COERCE-NEXT: preserved:       false
// SIMPLE-COERCE-NEXT: calls:           []
// SIMPLE-COERCE-NEXT: type_refs:
// SIMPLE-COERCE-NEXT:   - name:            '$s9type_refs1SV'
// SIMPLE-COERCE-NEXT:     guid:            5397591673202260225



// Ensure that witness impl of S.foo for P has type ref to S
// RUN: cat %t/type_refs.summary.yaml | %FileCheck %s -check-prefix WITNESS-IMPL

// WITNESS-IMPL:      12925277474523063582:
// WITNESS-IMPL-NEXT:   name:            '$s9type_refs1SVAA1PA2aDP3fooyyFTW'

// WITNESS-IMPL:      witness_tables:
// WITNESS-IMPL-NEXT:   17891631795932606560:
// WITNESS-IMPL-NEXT:     - guid:            12925277474523063582
// WITNESS-IMPL-NEXT:       type_guid:       5397591673202260225


// Ensure that vtable impl of C.bar and D.bar have type ref to C
// RUN: cat %t/type_refs.summary.yaml | %FileCheck %s -check-prefix WITNESS-IMPL


// VTABLE-IMPL:      14897920476774525675:
// VTABLE-IMPL-NEXT:   name:            '$s9type_refs1CC3baryyF'
// VTABLE-IMPL:      16977749031506698911:
// VTABLE-IMPL-NEXT:   name:            '$s9type_refs1DC3baryyF'

// VTABLE-IMPL:      14897920476774525675:
// VTABLE-IMPL-NEXT:   - guid:            14897920476774525675
// VTABLE-IMPL-NEXT:     type_guid:       3331627721515121492
// VTABLE-IMPL-NEXT:   - guid:            16977749031506698911
// VTABLE-IMPL-NEXT:     type_guid:       9126595621082655001


// RUN: %swift_frontend_plain -merge-module-summary %t/type_refs.swiftmodule.summary -module-summary-embed-debug-name -o %t/type_refs.swiftmodule.merged-summary
// RUN: %swift-module-summary-test --to-yaml %t/type_refs.swiftmodule.merged-summary -o %t/type_refs.merged-summary.yaml
// Ensure that WT of V is not used.
// RUN: cat %t/type_refs.merged-summary.yaml | %FileCheck %s -check-prefix USED-TYPE

// USED-TYPE-NOT: s9type_refs1VVXMt

protocol P {
    func foo()
}

struct S : P {
    func foo() {}
} 

struct V : P {
    func foo() {}
}

class C {
    func bar() {}
}

class D : C {
    override func bar() {}
}

func coerceToP(_ x: S) -> P { return x }

_ = coerceToP(S())

func coerceToAny(_ x: C) -> Any { return x }
func coerceToAny(_ x: D) -> Any { return x }
