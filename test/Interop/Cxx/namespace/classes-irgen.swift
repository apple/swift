// RUN: %target-swift-emit-ir -I %S/Inputs -enable-experimental-cxx-interop %s -Xcc -fignore-exceptions | %FileCheck %s

import Classes

// CHECK-LABEL: define {{.*}}void @"$s4main10basicTestsyyF"()
// CHECK: call ptr @{{_ZN10ClassesNS111BasicStruct11basicMemberEv|"\?basicMember@BasicStruct@ClassesNS1@@QEAAPEBDXZ"}}(ptr
// CHECK: call ptr @{{_ZN10ClassesNS110ClassesNS211BasicStruct11basicMemberEv|"\?basicMember@BasicStruct@ClassesNS2@ClassesNS1@@QEAAPEBDXZ"}}(ptr
// CHECK: call ptr @{{_ZN10ClassesNS311BasicStruct11basicMemberEv|"\?basicMember@BasicStruct@ClassesNS3@@QEAAPEBDXZ"}}(ptr
// CHECK: call ptr @{{_ZN10ClassesNS111BasicStruct11basicMemberEv|"\?basicMember@BasicStruct@ClassesNS1@@QEAAPEBDXZ"}}(ptr
// CHECK: ret void
public func basicTests() {
  var basicStructInst = ClassesNS1.BasicStruct()
  basicStructInst.basicMember()

  var nestedBasicStructInst = ClassesNS1.ClassesNS2.BasicStruct()
  nestedBasicStructInst.basicMember()

  var siblingBasicStruct = ClassesNS3.BasicStruct()
  siblingBasicStruct.basicMember()

  var basicStructViaAlias = ClassesNS4.AliasToGlobalNS1.BasicStruct()
  basicStructViaAlias.basicMember()
}

// CHECK-LABEL: define {{.*}}void @"$s4main15forwardDeclaredyyF"()
// CHECK: call ptr @{{_ZN10ClassesNS121ForwardDeclaredStruct11basicMemberEv|"\?basicMember@ForwardDeclaredStruct@ClassesNS1@@QEAAPEBDXZ"}}(ptr
// CHECK: call ptr @{{_ZN10ClassesNS110ClassesNS221ForwardDeclaredStruct11basicMemberEv|"\?basicMember@ForwardDeclaredStruct@ClassesNS2@ClassesNS1@@QEAAPEBDXZ"}}(ptr
// CHECK: ret void
public func forwardDeclared() {
  var forwardDeclaredStruct = ClassesNS1.ForwardDeclaredStruct()
  forwardDeclaredStruct.basicMember()

  var nestedForwardDeclaredStruct = ClassesNS1.ClassesNS2.ForwardDeclaredStruct()
  nestedForwardDeclaredStruct.basicMember()
}
