// RUN: %empty-directory(%t)
// RUN: %target-swift-frontend -emit-sib -emit-module-summary-path %t/default_wt.swiftmodule.summary -module-name default_wt -Xllvm -module-summary-embed-debug-name %s
// RUN: %swift_frontend_plain -merge-module-summary %t/default_wt.swiftmodule.summary -Xllvm -module-summary-embed-debug-name -o %t/default_wt.swiftmodule.merged-summary
// RUN: %swift-module-summary-test --to-yaml %t/default_wt.swiftmodule.merged-summary -o %t/default_wt.merged-summary.yaml

// Ensure that optimizer won't eliminate PrimitiveSequenceType.getPrimitiveSequence
// RUN: %target-swift-frontend -c -module-summary-path %t/default_wt.swiftmodule.merged-summary default_wt.sib -o %t/default_wt.o
// RUN: %target-build-swift %t/default_wt.o -o %t/default_wt
// RUN: %target-run %t/default_wt

// RUN: cat %t/default_wt.merged-summary.yaml | %FileCheck %s
// CHECK:      8732890044670327403:
// CHECK-NEXT:    name:            '$s10default_wt11HappyStructV13requiredValueSiyF'
// CHECK-NEXT:    guid:            8732890044670327403
// CHECK-NEXT:    live:            true

protocol HappyProtocol {
    func requiredValue() -> Int
}

extension HappyProtocol {
    // Need to access requiredValue indirectly through wt
    @_optimize(none)
    func getRequiredValue() -> Int {
        return requiredValue()
    }
}

struct HappyStruct<T> : HappyProtocol {
    func requiredValue() -> Int { 1 }
}

func consume(_ v: HappyStruct<Int>) {
    _ = v.getRequiredValue()
}

consume(HappyStruct<Int>())
