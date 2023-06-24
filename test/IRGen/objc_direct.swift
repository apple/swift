// RUN: %target-swift-emit-ir -import-objc-header %S/../Inputs/objc_direct.h -o - %s | %FileCheck %s

// REQUIRES: objc_interop

func markUsed<T>(_ t: T) {}

protocol BarProtocol {
    func directProtocolMethod() -> String!
}

extension Bar: BarProtocol {}

let bar = Bar()
markUsed(Bar(value: 0))
// CHECK: call swiftcc {{i32|i64}} @"$sSo3BarC5valueABSgs5Int32V_tcfC"
markUsed(Bar.init(value: 0))
// CHECK: call swiftcc {{i32|i64}} @"$sSo3BarC5valueABSgs5Int32V_tcfC"

bar.directProperty = 123
// CHECK: call void @"\01-[Bar setDirectProperty:]"(ptr %{{[0-9]+}}, i32 {{.*}})

markUsed(bar.directProperty)
// CHECK: call i32 @"\01-[Bar directProperty]"(ptr %{{[0-9]+}})

bar.directProperty2 = 456
// CHECK: call void @"\01-[Bar setDirectProperty2:]"(ptr %{{[0-9]+}}, i32 {{.*}})

markUsed(bar.directProperty2)
// CHECK: call i32 @"\01-[Bar directProperty2]"(ptr %{{[0-9]+}})

bar[0] = 789
// CHECK: call void @"\01-[Bar setObject:atIndexedSubscript:]"(ptr %{{[0-9]+}}, i32 789, i32 0)

markUsed(bar[0])
// CHECK: call i32 @"\01-[Bar objectAtIndexedSubscript:]"(ptr %{{[0-9]+}}, i32 0)

markUsed(bar.directMethod())
// CHECK: call {{.*}} @"\01-[Bar directMethod]"(ptr %{{[0-9]+}})

markUsed(bar.directMethod2())
// CHECK: call {{.*}} @"\01-[Bar directMethod2]"(ptr %{{[0-9]+}})

markUsed(Bar.directClassMethod())
// NOTE: The class must be realized before calling objc_direct class methods, even if
//       Swift avoids explicit class realization before calling regular class methods.
// CHECK: [[R0:%.*]] = load ptr, ptr @"OBJC_CLASS_REF_$_Bar"
// CHECK: [[R1:%.*]] = call ptr  @{{(swift_getInitializedObjCClass|objc_opt_self)}}(ptr [[R0]])
// CHECK: call {{.*}} @"\01+[Bar directClassMethod]"(ptr [[R1]])

markUsed(Bar.directClassMethod2())
// CHECK: [[R3:%.*]] = load ptr, ptr @"OBJC_CLASS_REF_$_Bar"
// CHECK: [[R4:%.*]] = call ptr @{{(swift_getInitializedObjCClass|objc_opt_self)}}(ptr [[R3]])
// CHECK: call {{.*}} @"\01+[Bar directClassMethod2]"(ptr [[R4]])

markUsed(bar.directProtocolMethod())
// CHECK: call {{.*}} @"\01-[Bar directProtocolMethod]"({{.*}})

// CHECK: define {{.*}} swiftcc {{i32|i64}} @"$sSo3BarC5valueABSgs5Int32V_tcfC"
// CHECK:   call swiftcc {{i32|i64}} @"$sSo3BarC5valueABSgs5Int32V_tcfcTO"
// CHECK: }

// CHECK-DAG: declare i32 @"\01-[Bar directProperty]"
// CHECK-DAG: declare void @"\01-[Bar setDirectProperty:]"
// CHECK-DAG: declare i32 @"\01-[Bar directProperty2]"
// CHECK-DAG: declare void @"\01-[Bar setDirectProperty2:]"
// CHECK-DAG: declare void @"\01-[Bar setObject:atIndexedSubscript:]"
// CHECK-DAG: declare i32 @"\01-[Bar objectAtIndexedSubscript:]"
// CHECK-DAG: declare {{.*}} @"\01-[Bar directMethod]"
// CHECK-DAG: declare {{.*}} @"\01-[Bar directMethod2]"
// CHECK-DAG: declare {{.*}} @"\01+[Bar directClassMethod]"
// CHECK-DAG: declare {{.*}} @"\01+[Bar directClassMethod2]"
// CHECK-DAG: declare {{.*}} @"\01-[Bar directProtocolMethod]"

// CHECK: define {{.*}} swiftcc {{i32|i64}} @"$sSo3BarC5valueABSgs5Int32V_tcfcTO"
// CHECK:   call {{.*}} @"\01-[Bar initWithValue:]"
// CHECK: }
