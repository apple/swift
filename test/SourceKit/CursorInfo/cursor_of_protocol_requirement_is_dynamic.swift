protocol MyProto {
  // RUN: %sourcekitd-test -req=cursor -pos=%(line + 1):9 %s -- %s | %FileCheck %s
  func foo()
}

// CHECK: DYNAMIC
// CHECK: RECEIVERS BEGIN
// CHECK: s:41cursor_of_protocol_requirement_is_dynamic7MyProtoP
// CHECK: RECEIVERS END

class ClassA: MyProto {
  func foo() {}
}

class ClassB: MyProto {
  func foo() {}
}
