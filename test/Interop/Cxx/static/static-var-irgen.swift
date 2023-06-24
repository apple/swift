// RUN: %target-swift-emit-ir %use_no_opaque_pointers -I %S/Inputs -enable-experimental-cxx-interop %s | %FileCheck %s
// RUN: %target-swift-emit-ir -I %S/Inputs -enable-experimental-cxx-interop %s

import StaticVar

public func initStaticVars() -> CInt {
  return staticVar + staticVarInit + staticVarInlineInit + staticConst + staticConstInit
    + staticConstInlineInit + staticNonTrivial.val + staticConstNonTrivial.val
}

// CHECK: @{{_ZL9staticVar|staticVar}} = internal global i32 2, align 4
// CHECK: @{{_ZL13staticVarInit|staticVarInit}} = internal global i32 0, align 4
// CHECK: @{{_ZL19staticVarInlineInit|staticVarInlineInit}} = internal global i32 0, align 4
// CHECK: @{{_ZL11staticConst|staticConst}} = internal constant i32 4, align 4
// CHECK: @{{_ZL15staticConstInit|staticConstInit}} = internal global i32 0, align 4
// CHECK: @{{_ZL21staticConstInlineInit|staticConstInlineInit}} = internal global i32 0, align 4
// CHECK: @{{_ZL16staticNonTrivial|staticNonTrivial}} = internal global %class.NonTrivial zeroinitializer, align 4
// CHECK: @{{_ZL21staticConstNonTrivial|staticConstNonTrivial}} = internal global %class.NonTrivial zeroinitializer, align 4

// CHECK: define internal void @{{__cxx_global_var_init|"\?\?__EstaticVarInit@@YAXXZ"}}()
// CHECK: %call = call {{.*}}i32 @{{_Z13makeStaticVarv|"\?makeStaticVar@@YAHXZ"}}()
// CHECK: store i32 %call, i32* @{{_ZL13staticVarInit|staticVarInit}}, align 4

// CHECK: declare {{.*}}i32 @{{_Z13makeStaticVarv|"\?makeStaticVar@@YAHXZ"}}()

// CHECK: define internal void @{{__cxx_global_var_init.1|"\?\?__EstaticVarInlineInit@@YAXXZ"}}()
// CHECK: %call = call {{.*}}i32 @{{_Z19inlineMakeStaticVarv|"\?inlineMakeStaticVar@@YAHXZ"}}()
// CHECK: store i32 %call, i32* @{{_ZL19staticVarInlineInit|staticVarInlineInit}}, align 4

// CHECK: define {{.*}}i32 @{{_Z19inlineMakeStaticVarv|"\?inlineMakeStaticVar@@YAHXZ"}}()
// CHECK: ret i32 8

// CHECK: define internal void @{{__cxx_global_var_init.2|"\?\?__EstaticConstInit@@YAXXZ"}}()
// CHECK: %call = call {{.*}}i32 @{{_Z15makeStaticConstv|"\?makeStaticConst@@YAHXZ"}}()
// CHECK: store i32 %call, i32* @{{_ZL15staticConstInit|staticConstInit}}, align 4

// CHECK: declare {{.*}}i32 @{{_Z15makeStaticConstv|"\?makeStaticConst@@YAHXZ"}}()

// CHECK: define internal void @{{__cxx_global_var_init.3|"\?\?__EstaticConstInlineInit@@YAXXZ"}}()
// CHECK: %call = call {{.*}}i32 @{{_Z21inlineMakeStaticConstv|"\?inlineMakeStaticConst@@YAHXZ"}}()
// CHECK: store i32 %call, i32* @{{_ZL21staticConstInlineInit|staticConstInlineInit}}, align 4

// CHECK: define {{.*}}i32 @{{_Z21inlineMakeStaticConstv|"\?inlineMakeStaticConst@@YAHXZ"}}()
// CHECK: ret i32 16

// CHECK: define internal void @{{__cxx_global_var_init.4|"\?\?__EstaticNonTrivial@@YAXXZ"}}()
// CHECK: call{{.*}} {{void|%class.NonTrivial\*}} {{@_ZN10NonTrivialC[12]Ei\(%class.NonTrivial\* .*@_ZL16staticNonTrivial, i32 .*1024\)|@"\?\?0NonTrivial@@QEAA@H@Z"\(%class.NonTrivial\* .*@staticNonTrivial, i32 .*1024\)}}

// CHECK: define internal void @{{__cxx_global_var_init.5|"\?\?__EstaticConstNonTrivial@@YAXXZ"}}()
// CHECK: call{{.*}} {{void|%class.NonTrivial\*}} {{@_ZN10NonTrivialC[12]Ei\(%class.NonTrivial\* .*@_ZL21staticConstNonTrivial, i32 .*2048\)|@"\?\?0NonTrivial@@QEAA@H@Z"\(%class.NonTrivial\* .*@staticConstNonTrivial, i32 .*2048\)}}

public func readStaticVar() -> CInt {
  return staticVar
}

// CHECK: define {{.*}}i32 @"$s4main13readStaticVars5Int32VyF"()
// CHECK: [[VALUE:%.*]] = load i32, i32* getelementptr inbounds (%Ts5Int32V, %Ts5Int32V* bitcast (i32* @{{_ZL9staticVar|staticVar}} to %Ts5Int32V*), i32 0, i32 0), align 4
// CHECK: ret i32 [[VALUE]]

public func writeStaticVar(_ v: CInt) {
  staticVar = v
}

// CHECK: define {{.*}}void @"$s4main14writeStaticVaryys5Int32VF"(i32 {{.*}}%0)
// CHECK: store i32 %0, i32* getelementptr inbounds (%Ts5Int32V, %Ts5Int32V* bitcast (i32* @{{_ZL9staticVar|staticVar}} to %Ts5Int32V*), i32 0, i32 0), align 4

public func readStaticNonTrivial() -> NonTrivial {
  return staticNonTrivial
}

// CHECK: define {{.*}}i32 @"$s4main20readStaticNonTrivialSo0dE0VyF"()
// CHECK: [[VALUE:%.*]] = load i32, i32* getelementptr inbounds (%TSo10NonTrivialV, %TSo10NonTrivialV* bitcast (%class.NonTrivial* @{{_ZL16staticNonTrivial|staticNonTrivial}} to %TSo10NonTrivialV*), i32 0, i32 0, i32 0), align 4
// CHECK: ret i32 [[VALUE]]

public func writeStaticNonTrivial(_ i: NonTrivial) {
  staticNonTrivial = i
}

// CHECK: define {{.*}}void @"$s4main21writeStaticNonTrivialyySo0dE0VF"(i32 {{.*}}%0)
// CHECK: store i32 %0, i32* getelementptr inbounds (%TSo10NonTrivialV, %TSo10NonTrivialV* bitcast (%class.NonTrivial* @{{_ZL16staticNonTrivial|staticNonTrivial}} to %TSo10NonTrivialV*), i32 0, i32 0, i32 0), align 4

func modifyInout(_ c: inout CInt) {
  c = 42
}

public func passingVarAsInout() {
  modifyInout(&staticVar)
}
// CHECK: define {{.*}}void @"$s4main17passingVarAsInoutyyF"()
// CHECK: call swiftcc void @"$s4main11modifyInoutyys5Int32VzF"(%Ts5Int32V* nocapture dereferenceable(4) bitcast (i32* @{{_ZL9staticVar|staticVar}} to %Ts5Int32V*))

// CHECK: define internal void @_GLOBAL__sub_I__swift_imported_modules_()
// CHECK: call void @{{__cxx_global_var_init|"\?\?__EstaticVarInit@@YAXXZ"}}()
// CHECK: call void @{{__cxx_global_var_init.1|"\?\?__EstaticVarInlineInit@@YAXXZ"}}()
// CHECK: call void @{{__cxx_global_var_init.2|"\?\?__EstaticConstInit@@YAXXZ"}}()
// CHECK: call void @{{__cxx_global_var_init.3|"\?\?__EstaticConstInlineInit@@YAXXZ"}}()
// CHECK: call void @{{__cxx_global_var_init.4|"\?\?__EstaticNonTrivial@@YAXXZ"}}()
// CHECK: call void @{{__cxx_global_var_init.5|"\?\?__EstaticConstNonTrivial@@YAXXZ"}}()
