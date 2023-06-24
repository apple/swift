// RUN: %target-swift-emit-ir %use_no_opaque_pointers %s -I %S/Inputs -enable-experimental-cxx-interop | %FileCheck %s
// RUN: %target-swift-emit-ir %s -I %S/Inputs -enable-experimental-cxx-interop

import ExternVar

public func getCounter() -> CInt {
  return counter
}

// CHECK: @{{counter|"\?counter@@3HA"}} = external {{(dso_local )?}}global i32, align 4
// CHECK: @{{_ZN10Namespaced7counterE|"\?counter@Namespaced@@3HA"}} = external {{(dso_local )?}}global i32, align 4

// CHECK: define {{(protected |dllexport )?}}swiftcc i32 @"$s4main10getCounters5Int32VyF"() #0
// CHECK: [[LOAD:%.*]] = load i32, i32* getelementptr inbounds (%Ts5Int32V, %Ts5Int32V* bitcast (i32* @{{counter|"\?counter@@3HA"}} to %Ts5Int32V*), i32 0, i32 0), align 4
// CHECK: ret i32 [[LOAD]]

public func setCounter(_ c: CInt) {
  counter = c
}

// CHECK: define {{(protected |dllexport )?}}swiftcc void @"$s4main10setCounteryys5Int32VF"(i32 %0) #0
// CHECK: store i32 %0, i32* getelementptr inbounds (%Ts5Int32V, %Ts5Int32V* bitcast (i32* @{{counter|"\?counter@@3HA"}} to %Ts5Int32V*), i32 0, i32 0), align 4

public func getNamespacedCounter() -> CInt {
  return Namespaced.counter
}

// CHECK: define {{(protected |dllexport )?}}swiftcc i32 @"$s4main20getNamespacedCounters5Int32VyF"() #0
// CHECK: load i32, i32* getelementptr inbounds (%Ts5Int32V, %Ts5Int32V* bitcast (i32* @{{_ZN10Namespaced7counterE|"\?counter@Namespaced@@3HA"}} to %Ts5Int32V*), i32 0, i32 0), align 4
// CHECK: ret i32 %1

public func setNamespacedCounter(_ c: CInt) {
  Namespaced.counter = c
}

// CHECK: define {{(protected |dllexport )?}}swiftcc void @"$s4main20setNamespacedCounteryys5Int32VF"(i32 %0) #0
// CHECK: store i32 %0, i32* getelementptr inbounds (%Ts5Int32V, %Ts5Int32V* bitcast (i32* @{{_ZN10Namespaced7counterE|"\?counter@Namespaced@@3HA"}} to %Ts5Int32V*), i32 0, i32 0), align 4

func modifyInout(_ c: inout CInt) {
  c = 42
}

public func passingVarAsInout() {
  modifyInout(&counter)
}

// CHECK: define {{(protected |dllexport )?}}swiftcc void @"$s4main17passingVarAsInoutyyF"() #0
// CHECK: call swiftcc void @"$s4main11modifyInoutyys5Int32VzF"(%Ts5Int32V* nocapture dereferenceable(4) bitcast (i32* @{{counter|"\?counter@@3HA"}} to %Ts5Int32V*))
