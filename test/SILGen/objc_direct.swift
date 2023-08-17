// RUN: %target-swift-emit-silgen -import-objc-header %S/../Inputs/objc_direct.h -o - %s | %FileCheck %s

// REQUIRES: objc_interop

// NOTE: `[[BYTE01]]` should match the byte `0b01`.

func markUsed<T>(_ t: T) {}

protocol BarProtocol {
    func directProtocolMethod() -> String!
}

extension Bar: BarProtocol {}

let bar = Bar()
markUsed(Bar(value: 0))
// CHECK: function_ref @$sSo3BarC5valueABSgs5Int32V_tcfC
markUsed(Bar.init(value: 0))
// CHECK: function_ref @$sSo3BarC5valueABSgs5Int32V_tcfC

bar.directProperty = 123
// CHECK: function_ref @[[BYTE01:.]]-[Bar setDirectProperty:]

markUsed(bar.directProperty)
// CHECK: function_ref @[[BYTE01]]-[Bar directProperty]

bar.directProperty2 = 456
// CHECK: function_ref @[[BYTE01]]-[Bar setDirectProperty2:]

markUsed(bar.directProperty2)
// CHECK: function_ref @[[BYTE01]]-[Bar directProperty2]

bar[0] = 789
// CHECK: function_ref @[[BYTE01]]-[Bar setObject:atIndexedSubscript:]

markUsed(bar[0])
// CHECK: function_ref @[[BYTE01]]-[Bar objectAtIndexedSubscript:]

markUsed(bar.directMethod())
// CHECK: function_ref @[[BYTE01]]-[Bar directMethod]

markUsed(bar.directMethod2())
// CHECK: function_ref @[[BYTE01]]-[Bar directMethod2]

markUsed(Bar.directClassMethod())
// CHECK: function_ref @[[BYTE01]]+[Bar directClassMethod]

markUsed(Bar.directClassMethod2())
// CHECK: function_ref @[[BYTE01]]+[Bar directClassMethod2]

markUsed(bar.directProtocolMethod())
// CHECK: function_ref @[[BYTE01]]-[Bar directProtocolMethod]

// CHECK-LABEL: sil shared [serialized] [ossa] @$sSo3BarC5valueABSgs5Int32V_tcfC : $@convention(method) (Int32, @thick Bar.Type) -> @owned Optional<Bar> {
// CHECK: {{.*}} = function_ref @$sSo3BarC5valueABSgs5Int32V_tcfcTO : $@convention(method)
// CHECK: } // end sil function '$sSo3BarC5valueABSgs5Int32V_tcfC'

// CHECK-DAG: sil @[[BYTE01]]-[Bar setDirectProperty:] : $@convention(objc_method)
// CHECK-DAG: sil @[[BYTE01]]-[Bar directProperty] : $@convention(objc_method)
// CHECK-DAG: sil @[[BYTE01]]-[Bar setDirectProperty2:] : $@convention(objc_method)
// CHECK-DAG: sil @[[BYTE01]]-[Bar directProperty2] : $@convention(objc_method)
// CHECK-DAG: sil @[[BYTE01]]-[Bar objectAtIndexedSubscript:] : $@convention(objc_method)
// CHECK-DAG: sil @[[BYTE01]]-[Bar setObject:atIndexedSubscript:] : $@convention(objc_method)
// CHECK-DAG: sil [clang Bar.directMethod] @[[BYTE01]]-[Bar directMethod] : $@convention(objc_method)
// CHECK-DAG: sil [clang Bar.directMethod2] @[[BYTE01]]-[Bar directMethod2] : $@convention(objc_method)
// CHECK-DAG: sil [clang Bar.directClassMethod] @[[BYTE01]]+[Bar directClassMethod] : $@convention(objc_method)
// CHECK-DAG: sil [clang Bar.directClassMethod2] @[[BYTE01]]+[Bar directClassMethod2] : $@convention(objc_method)

// CHECK: sil [clang Bar.directProtocolMethod] @[[BYTE01]]-[Bar directProtocolMethod] : $@convention(objc_method)

// CHECK-LABEL: sil{{.*}}@$sSo3BarC5valueABSgs5Int32V_tcfcTO : $@convention(method) (Int32, @owned Bar) -> @owned Optional<Bar> {
// CHECK: function_ref @[[BYTE01]]-[Bar initWithValue:]
// CHECK: } // end sil function '$sSo3BarC5valueABSgs5Int32V_tcfcTO'
