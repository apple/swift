// RUN: %empty-directory(%t)
// RUN: %target-swift-frontend -emit-sib -emit-module-summary-path %t/tables.swiftmodulesummary -module-name tables -Xllvm -module-summary-embed-debug-name %s
// RUN: llvm-bcanalyzer -dump %t/tables.swiftmodulesummary | %FileCheck %s -check-prefix BCANALYZER

// BCANALYZER-NOT: UnknownCode

// RUN: %swift-module-summary-test --to-yaml %t/tables.swiftmodulesummary -o - | %FileCheck %s


// CHECK:      767048646313834908:
// CHECK-NEXT:     name:            '$s6tables1SVAA1PA2aDP11protoMemberyyFTW'
// CHECK:      11756327503593502600:
// CHECK-NEXT:    name:            '$s6tables1DC11classMemberyyF'
// CHECK:      17602567966448237004:
// CHECK-NEXT:    name:            '$s6tables1CC11classMemberyyF'

// `protoMember` witness is recorded on the table
// CHECK: witness_tables:
// CHECK:   2682576275888919121: [ 767048646313834908 ]
// `classMember` impls are recorded on the table
// CHECK: vtables:
// CHECK:   17602567966448237004: [ 17602567966448237004, 11756327503593502600 ]

protocol P {
    func protoMember()
}

struct S : P {
    func protoMember() {}
}


class C {
    func classMember() {}
}

class D : C {
    override func classMember() {}
}
