// RUN: %cxx-all-targets(-I %S/Inputs %s -emit-ir | %FileCheck %s)

import Classes

// CHECK-LABEL: define {{.*}}void @"$s4main10basicTestsyyF"()
// CHECK: call i8* @{{_ZN10ClassesNS111BasicStruct11basicMemberEv|"\?basicMember@BasicStruct@ClassesNS1@@QEAAPEBDXZ"}}(%"struct.ClassesNS1::BasicStruct"*
// CHECK: call i8* @{{_ZN10ClassesNS110ClassesNS211BasicStruct11basicMemberEv|"\?basicMember@BasicStruct@ClassesNS2@ClassesNS1@@QEAAPEBDXZ"}}(%"struct.ClassesNS1::ClassesNS2::BasicStruct"*
// CHECK: call i8* @{{_ZN10ClassesNS311BasicStruct11basicMemberEv|"\?basicMember@BasicStruct@ClassesNS3@@QEAAPEBDXZ"}}(%"struct.ClassesNS3::BasicStruct"*
// CHECK: ret void
public func basicTests() {
  var basicStructInst = ClassesNS1.BasicStruct()
  basicStructInst.basicMember()

  var nestedBasicStructInst = ClassesNS1.ClassesNS2.BasicStruct()
  nestedBasicStructInst.basicMember()

  var siblingBasicStruct = ClassesNS3.BasicStruct()
  siblingBasicStruct.basicMember()
}

// CHECK-LABEL: define {{.*}}void @"$s4main15forwardDeclaredyyF"()
// CHECK: call i8* @{{_ZN10ClassesNS121ForwardDeclaredStruct11basicMemberEv|"\?basicMember@ForwardDeclaredStruct@ClassesNS1@@QEAAPEBDXZ"}}(%"struct.ClassesNS1::ForwardDeclaredStruct"*
// CHECK: call i8* @{{_ZN10ClassesNS110ClassesNS221ForwardDeclaredStruct11basicMemberEv|"\?basicMember@ForwardDeclaredStruct@ClassesNS2@ClassesNS1@@QEAAPEBDXZ"}}(%"struct.ClassesNS1::ClassesNS2::ForwardDeclaredStruct"*
// CHECK: ret void
public func forwardDeclared() {
  var forwardDeclaredStruct = ClassesNS1.ForwardDeclaredStruct()
  forwardDeclaredStruct.basicMember()

  var nestedForwardDeclaredStruct = ClassesNS1.ClassesNS2.ForwardDeclaredStruct()
  nestedForwardDeclaredStruct.basicMember()
}
