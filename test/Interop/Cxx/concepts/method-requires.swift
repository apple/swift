// RUN: %target-swiftxx-frontend -emit-ir -Xcc -std=gnu++20 -I %S/Inputs %s | %FileCheck %s
//
// REQUIRES: OS=macosx || OS=linux-gnu

import MethodRequires

var s = MyStruct()
s.foo(123)
// CHECK-NOT: calledFromConceptBody
// CHECK: calledFromMethodBody
