// Tests that under -enable-llvm-vfe, virtual function calls to classes defined
// by other modules are using thunk calls (instead of direct vtable loads).

// RUN: %empty-directory(%t)
// RUN: %target-build-swift -Xfrontend -disable-objc-interop -Xfrontend -enable-llvm-vfe -parse-as-library %s -DLIBRARY -module-name Library -emit-module -o %t/Library.swiftmodule
// RUN: %target-build-swift -Xfrontend -disable-objc-interop -Xfrontend -enable-llvm-vfe -parse-as-library %s -DCLIENT -module-name Main -I%t -emit-ir -o - | %FileCheck %s

// UNSUPPORTED: OS=windows-msvc

#if LIBRARY

public class MyLibraryClass {
  public func foo() { }
}

#endif

#if CLIENT

import Library

public class MyLocalClass {
  public func bar() { }
}

func func1(o: MyLocalClass) {
  // CHECK: define hidden swiftcc void @"$s4Main5func11oyAA12MyLocalClassC_tF"(ptr %0)
  o.bar()
  // CHECK:  [[SLOT:%.*]] = getelementptr inbounds ptr, ptr {{.*}}, {{i64|i32}} {{.*}}
  // CHECK:  call { ptr, i1 } @llvm.type.checked.load(ptr [[SLOT]], i32 0, metadata !"$s4Main12MyLocalClassC3baryyFTq")

  // CHECK: ret void
}

func func2(o: MyLibraryClass) {
  // CHECK: define hidden swiftcc void @"$s4Main5func21oy7Library02MyC5ClassC_tF"(ptr %0)
  o.foo()
  // CHECK: call swiftcc void @"$s7Library02MyA5ClassC3fooyyFTj"

  // CHECK-NOT: @llvm.type.checked.load
  // CHECK: ret void
}

#endif
