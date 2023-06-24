// RUN: %target-swift-emit-ir %use_no_opaque_pointers -I %S/Inputs -enable-experimental-cxx-interop %s | %FileCheck %s
// RUN: %target-swift-emit-ir -I %S/Inputs -enable-experimental-cxx-interop %s

import InlineStaticMemberVar

public func readStaticMember() -> CInt {
  return WithInlineStaticMember.staticMember
}

// CHECK: @{{_ZN22WithInlineStaticMember12staticMemberE|"\?staticMember@WithInlineStaticMember@@2HA"}} = linkonce_odr {{(dso_local )?}}global i32 12, {{(comdat, )?}}align 4

// CHECK: define {{(protected |dllexport )?}}swiftcc i32 @"$s4main16readStaticMembers5Int32VyF"()
// CHECK: [[VALUE:%.*]] = load i32, i32* getelementptr inbounds (%Ts5Int32V, %Ts5Int32V* bitcast (i32* @{{_ZN22WithInlineStaticMember12staticMemberE|"\?staticMember@WithInlineStaticMember@@2HA"}} to %Ts5Int32V*), i32 0, i32 0), align 4
// CHECK: ret i32 [[VALUE]]

public func writeStaticMember(_ c: CInt) {
  WithInlineStaticMember.staticMember = c
}

// CHECK: define {{(protected |dllexport )?}}swiftcc void @"$s4main17writeStaticMemberyys5Int32VF"(i32 %0)
// CHECK: store i32 %0, i32* getelementptr inbounds (%Ts5Int32V, %Ts5Int32V* bitcast (i32* @{{_ZN22WithInlineStaticMember12staticMemberE|"\?staticMember@WithInlineStaticMember@@2HA"}} to %Ts5Int32V*), i32 0, i32 0), align 4

func modifyInout(_ c: inout CInt) {
  c = 42
}

public func passingVarAsInout() {
  modifyInout(&WithInlineStaticMember.staticMember)
}

// CHECK: define {{(protected |dllexport )?}}swiftcc void @"$s4main17passingVarAsInoutyyF"()
// CHECK: call swiftcc void @"$s4main11modifyInoutyys5Int32VzF"(%Ts5Int32V* nocapture dereferenceable(4) bitcast (i32* @{{_ZN22WithInlineStaticMember12staticMemberE|"\?staticMember@WithInlineStaticMember@@2HA"}} to %Ts5Int32V*))
