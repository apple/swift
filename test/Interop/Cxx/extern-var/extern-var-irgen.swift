// RUN: %target-swift-emit-ir %s -I %S/Inputs -enable-experimental-cxx-interop | %FileCheck %s

import ExternVar

public func getCounter() -> CInt {
  return counter
}

// CHECK: @{{counter|"\?counter@@3HA"}} = external {{(dso_local )?}}global i32, align 4
// CHECK: @{{_ZN10Namespaced7counterE|"\?counter@Namespaced@@3HA"}} = external {{(dso_local )?}}global i32, align 4

// CHECK: define {{(protected |dllexport )?}}swiftcc i32 @"$s4main10getCounters5Int32VyF"() #0
// CHECK: [[LOAD:%.*]] = load i32, ptr @{{counter|"\?counter@@3HA"}}, align 4
// CHECK: ret i32 [[LOAD]]

public func setCounter(_ c: CInt) {
  counter = c
}

// CHECK: define {{(protected |dllexport )?}}swiftcc void @"$s4main10setCounteryys5Int32VF"(i32 %0) #0
// CHECK: store i32 %0, ptr @{{counter|"\?counter@@3HA"}}, align 4

public func getNamespacedCounter() -> CInt {
  return Namespaced.counter
}

// CHECK: define {{(protected |dllexport )?}}swiftcc i32 @"$s4main20getNamespacedCounters5Int32VyF"() #0
// CHECK: load i32, ptr @{{_ZN10Namespaced7counterE|"\?counter@Namespaced@@3HA"}}, align 4
// CHECK: ret i32 %0

public func setNamespacedCounter(_ c: CInt) {
  Namespaced.counter = c
}

// CHECK: define {{(protected |dllexport )?}}swiftcc void @"$s4main20setNamespacedCounteryys5Int32VF"(i32 %0) #0
// CHECK: store i32 %0, ptr @{{_ZN10Namespaced7counterE|"\?counter@Namespaced@@3HA"}}, align 4

func modifyInout(_ c: inout CInt) {
  c = 42
}

public func passingVarAsInout() {
  modifyInout(&counter)
}

// CHECK: define {{(protected |dllexport )?}}swiftcc void @"$s4main17passingVarAsInoutyyF"() #0
// CHECK: call swiftcc void @"$s4main11modifyInoutyys5Int32VzF"(ptr nocapture dereferenceable(4) @{{counter|"\?counter@@3HA"}})
