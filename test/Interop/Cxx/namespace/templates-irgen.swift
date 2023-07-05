// RUN: %target-swift-emit-ir -I %S/Inputs -enable-experimental-cxx-interop %s -Xcc -fignore-exceptions | %FileCheck %s

import Templates

// CHECK-LABEL: define {{.*}}void @"$s4main10basicTestsyyF"()
// CHECK: call ptr @{{_ZN12TemplatesNS121basicFunctionTemplateIiEEPKcT_|"\?\?\$basicFunctionTemplate@H@TemplatesNS1@@YAPEBDH@Z"}}(i32 0)
// CHECK: call ptr @{{_ZN12TemplatesNS118BasicClassTemplateIcE11basicMemberEv|"\?basicMember@\?\$BasicClassTemplate@D@TemplatesNS1@@QEAAPEBDXZ"}}(ptr
// CHECK: call ptr @{{_ZN12TemplatesNS112TemplatesNS229takesClassTemplateFromSiblingENS_12TemplatesNS318BasicClassTemplateIcEE|"\?takesClassTemplateFromSibling@TemplatesNS2@TemplatesNS1@@YAPEBDU\?\$BasicClassTemplate@D@TemplatesNS3@2@@Z"}}({{(i8 %[0-9]+)?}})
// CHECK: ret void
public func basicTests() {
  TemplatesNS1.basicFunctionTemplate(Int32(0))

  var basicClassTemplateInst = TemplatesNS1.BasicClassTemplateChar()
  basicClassTemplateInst.basicMember()

  TemplatesNS1.TemplatesNS2.takesClassTemplateFromSibling(TemplatesNS1.TemplatesNS2.BasicClassTemplateChar())
}

// CHECK-LABEL: define {{.*}}void @"$s4main22forwardDeclaredClassesyyF"()
// CHECK: call ptr @{{_ZN12TemplatesNS112TemplatesNS231forwardDeclaredFunctionTemplateIiEEPKcT_|"\?\?\$forwardDeclaredFunctionTemplate@H@TemplatesNS2@TemplatesNS1@@YAPEBDH@Z"}}(i32 0)
// CHECK: call ptr @{{_ZN12TemplatesNS112TemplatesNS228ForwardDeclaredClassTemplateIcE11basicMemberEv|"\?basicMember@\?\$ForwardDeclaredClassTemplate@D@TemplatesNS2@TemplatesNS1@@QEAAPEBDXZ"}}(ptr
// CHECK: call ptr @{{_ZN12TemplatesNS112TemplatesNS240forwardDeclaredFunctionTemplateOutOfLineIiEEPKcT_|"\?\?\$forwardDeclaredFunctionTemplateOutOfLine@H@TemplatesNS2@TemplatesNS1@@YAPEBDH@Z"}}(i32 0)
// CHECK: call ptr @{{_ZN12TemplatesNS112TemplatesNS237ForwardDeclaredClassTemplateOutOfLineIcE11basicMemberEv|"\?basicMember@\?\$ForwardDeclaredClassTemplateOutOfLine@D@TemplatesNS2@TemplatesNS1@@QEAAPEBDXZ"}}(ptr
// CHECK: ret void
public func forwardDeclaredClasses() {
  TemplatesNS1.TemplatesNS2.forwardDeclaredFunctionTemplate(Int32(0))

  var forwardDeclaredClassTemplateInst = TemplatesNS1.ForwardDeclaredClassTemplateChar()
  forwardDeclaredClassTemplateInst.basicMember()

  TemplatesNS1.TemplatesNS2.forwardDeclaredFunctionTemplateOutOfLine(Int32(0))

  var forwardDeclaredClassTemplateOutOfLineInst = ForwardDeclaredClassTemplateOutOfLineChar()
  forwardDeclaredClassTemplateOutOfLineInst.basicMember()
}
