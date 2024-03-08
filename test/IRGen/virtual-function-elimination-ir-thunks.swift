// Tests that under -enable-llvm-vfe, virtual function calls to classes defined
// by other modules are using thunk calls (instead of direct vtable loads).

// RUN: %empty-directory(%t)
// RUN: %target-build-swift -Xfrontend -disable-objc-interop -Xfrontend -enable-llvm-vfe -parse-as-library %s -DLIBRARY -module-name Library -emit-module -o %t/Library.swiftmodule
// RUN: %target-build-swift -Xfrontend -disable-objc-interop -Xfrontend -enable-llvm-vfe -parse-as-library %s -DLIBRARY -module-name Library -emit-ir -o - | %FileCheck %s --check-prefix=LIBRARY
// RUN: %target-build-swift -Xfrontend -disable-objc-interop -Xfrontend -enable-llvm-vfe -parse-as-library %s -DCLIENT -module-name Main -I%t -emit-ir -o - | %FileCheck %s --check-prefix=CLIENT

// UNSUPPORTED: OS=windows-msvc

#if LIBRARY

public class MyLibraryClass {
  public func foo() { }
}

public class MyLibraryDerivedClass : MyLibraryClass {
  override public func foo() { }
}

// For now at least, dispatch thunks should pass metadata pointing to the base implementation of methods

// LIBRARY: define {{(protected )?}}swiftcc void @"$s7Library02MyA5ClassC3fooyyFTj"(ptr swiftself %0)
// LIBRARY: @llvm.type.checked.load(ptr %{{[0-9]+}}, i32 0, metadata !"$s7Library02MyA5ClassC3fooyyFTq")
// LIBRARY: ret void

// LIBRARY: define hidden swiftcc ptr @"$s7Library02MyA5ClassCACycfCTj"(ptr swiftself %0)
// LIBRARY: @llvm.type.checked.load(ptr %{{[0-9]+}}, i32 0, metadata !"$s7Library02MyA5ClassCACycfCTq")
// LIBRARY: ret

#endif

#if CLIENT

import Library

public class MyLocalClass {
  public func bar() { }
}

public class MyLocalDerivedClass : MyLocalClass {
  override public func bar() {  }
}

// Virtual calls made within the same module should load from the vtable directly

func func1(o: MyLocalClass) {
  // CHECK: define hidden swiftcc void @"$s4Main5func11oyAA12MyLocalClassC_tF"(ptr %0)
  o.bar()
  // CHECK:  [[SLOT:%.*]] = getelementptr inbounds ptr, ptr {{.*}}, {{i64|i32}} {{.*}}
  // CHECK:  call { ptr, i1 } @llvm.type.checked.load(ptr [[SLOT]], i32 0, metadata !"$s4Main12MyLocalClassC3baryyFTq")

  // CLIENT: ret void
}

// Direct loads should be made using metadata pointing to the most derived method known to be called

func func2(o: MyLocalDerivedClass) {
  // CLIENT: define hidden swiftcc void @"$s4Main5func21oyAA19MyLocalDerivedClassC_tF"(ptr %0)
  o.bar()
  // CLIENT:  [[SLOT:%.*]] = getelementptr inbounds ptr, ptr
  // CLIENT:  call { ptr, i1 } @llvm.type.checked.load(ptr [[SLOT]], i32 0, metadata !"$s4Main19MyLocalDerivedClassC3baryyFTq")

  // CLIENT: ret void
}

// Virtual calls accross module boundaries should always use the dispatch thunk

func func3(o: MyLibraryClass) {
  // CLIENT: define hidden swiftcc void @"$s4Main5func31oy7Library02MyC5ClassC_tF"(ptr %0)
  o.foo()
  // CLIENT: call swiftcc void @"$s7Library02MyA5ClassC3fooyyFTj"

  // CLIENT-NOT: @llvm.type.checked.load
  // CLIENT: ret void
}

// For now at least, the virtual calls should use the same dispatch thunk regardless of which class
// in the hierarchy they make the call through

func func4(o: MyLibraryDerivedClass) {
  // CLIENT; define hidden swiftcc void @"$s4Main5func41oy7Library02MyC12DerivedClassC_tF"(ptr %0)
  o.foo()
  // CLIENT: call swiftcc void @"$s7Library02MyA5ClassC3fooyyFTj"

  // CLIENT-NOT @llvm.type.checked.load
  // CLIENT: ret void
}

#endif
