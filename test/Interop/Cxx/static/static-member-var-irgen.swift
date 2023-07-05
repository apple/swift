// RUN: %target-swift-emit-ir -I %S/Inputs -enable-experimental-cxx-interop %s | %FileCheck %s

// CHECK: @{{_ZN16WithStaticMember12staticMemberE|"\?staticMember@WithStaticMember@@2HA"}} = external {{(dso_local )?}}global i32, align 4
// CHECK: @{{_ZN26WithIncompleteStaticMember10selfMemberE|"\?selfMember@WithIncompleteStaticMember@@2V1@A"}} = {{external|linkonce_odr}} {{(dso_local )?}}global %class.WithIncompleteStaticMember, align 4

import StaticMemberVar

public func readStaticMember() -> CInt {
  return WithStaticMember.staticMember
}

// CHECK: define {{(protected |dllexport )?}}swiftcc i32 @"$s4main16readStaticMembers5Int32VyF"()
// CHECK: [[VALUE:%.*]] = load i32, ptr @{{_ZN16WithStaticMember12staticMemberE|"\?staticMember@WithStaticMember@@2HA"}}, align 4
// CHECK: ret i32 [[VALUE]]

public func writeStaticMember() {
  WithStaticMember.staticMember = -1
}

// CHECK: define {{(protected |dllexport )?}}swiftcc void @"$s4main17writeStaticMemberyyF"() #0
// CHECK: store i32 -1, ptr @{{_ZN16WithStaticMember12staticMemberE|"\?staticMember@WithStaticMember@@2HA"}}, align 4

public func readSelfMember() -> WithIncompleteStaticMember {
  return WithIncompleteStaticMember.selfMember
}

// CHECK: define {{(protected |dllexport )?}}swiftcc i32 @"$s4main14readSelfMemberSo020WithIncompleteStaticD0VyF"() #0
// CHECK: [[VALUE:%.*]] = load i32, ptr @{{_ZN26WithIncompleteStaticMember1|"\?selfMember@WithIncompleteStaticMember@@2V1@A"}}
// CHECK: ret i32 [[VALUE]]

public func writeSelfMember(_ m: WithIncompleteStaticMember) {
  WithIncompleteStaticMember.selfMember = m
}

// CHECK: define {{(protected |dllexport )?}}swiftcc void @"$s4main15writeSelfMemberyySo020WithIncompleteStaticD0VF"(i32 %0) #0
// CHECK: store i32 %0, ptr @{{_ZN26WithIncompleteStaticMember10selfMemberE|"\?selfMember@WithIncompleteStaticMember@@2V1@A"}}, align 4

// TODO: Currently, the generated code would try to load the value from
// a symbol that is not defined. We should inline the value instead.
// public func readNotDefinedConstMember() -> CInt {
//   return WithConstStaticMember.notDefined
// }
public func readDefinedConstMember() -> CInt {
  return WithConstStaticMember.defined
}

// CHECK: define {{(protected |dllexport )?}}swiftcc i32 @"$s4main22readDefinedConstMembers5Int32VyF"() #0
// CHECK: [[VALUE:%.*]] = load i32, ptr @{{_ZN21WithConstStaticMember7definedE|"\?defined@WithConstStaticMember@@2HB"}}, align 4
// CHECK: ret i32 [[VALUE]]
public func readDefinedOutOfLineConstMember() -> CInt {
  return WithConstStaticMember.definedOutOfLine
}

// CHECK: define {{(protected |dllexport )?}}swiftcc i32 @"$s4main31readDefinedOutOfLineConstMembers5Int32VyF"() #0
// CHECK: [[VALUE:%.*]] = load i32, ptr @{{_ZN21WithConstStaticMember16definedOutOfLineE|"\?definedOutOfLine@WithConstStaticMember@@2HB"}}, align 4
// CHECK: ret i32 [[VALUE]]
