// Tests that under -enable-llvm-vfe, IRGen marks vtables and vcall sites with
// the right attributes and intrinsics.

// RUN: %target-build-swift -Xfrontend -disable-objc-interop -Xfrontend -enable-llvm-vfe \
// RUN:    %s -emit-ir -o - | %FileCheck %s --check-prefix=CHECK --check-prefix=CHECK-%target-ptrsize

// UNSUPPORTED: OS=windows-msvc

class MyClass {
  func foo() { print("MyClass.foo") }
  func bar() { print("MyClass.bar") }
}

class MyDerivedClass: MyClass {
  override func foo() { print("MyDerivedClass.foo") }
  override func bar() { print("MyDerivedClass.bar") }
}

// Has vfunc slots at offsets 56, 64, 72 (in swift.method_descriptor structs).
// CHECK:      @"$s4main7MyClassCMn" =
// CHECK-SAME:     align 4, !type !0, !type !1, !type !2, !vcall_visibility !3, !typed_global_not_for_cfi !4

// Has vfunc slots at offsets 72, 80, 88 (on 64-bit) / 48, 52, 56 (on 32-bit).
// CHECK:         @"$s4main7MyClassCMf" =
// CHECK-64-SAME:     align 8, !type !5, !type !6, !type !7, !vcall_visibility !8, !typed_global_not_for_cfi !4
// CHECK-32-SAME:     align 4, !type !5, !type !6, !type !7, !vcall_visibility !8, !typed_global_not_for_cfi !4

// Has vfunc slots at offsets 56, 68, 80 (in swift.method_override_descriptor structs).
// CHECK:      @"$s4main14MyDerivedClassCMn" =
// CHECK-SAME:     align 4, !type !0, !type !9, !type !10, !vcall_visibility !11, !typed_global_not_for_cfi !4

// Has vfunc slots at offsets 72, 80, 88 (on 64-bit) / 48, 52, 56 (on 32-bit)
// CHECK:         @"$s4main14MyDerivedClassCMf" =
// CHECK-64-SAME:     align 8, !type !5, !type !6, !type !7, !vcall_visibility !8, !typed_global_not_for_cfi !4
// CHECK-32-SAME:     align 4, !type !5, !type !6, !type !7, !vcall_visibility !8, !typed_global_not_for_cfi !4


func func1() {
  // CHECK: define hidden swiftcc void @"$s4main5func1yyF"()
  let o: MyClass = MyDerivedClass()
  o.foo()
  // CHECK:  [[SLOT:%.*]] = getelementptr inbounds ptr, ptr {{.*}}, {{i64|i32}} {{.*}}
  // CHECK:  call { ptr, i1 } @llvm.type.checked.load(ptr [[SLOT]], i32 0, metadata !"$s4main7MyClassC3fooyyFTq")
}

func func2() {
  // CHECK: define hidden swiftcc void @"$s4main5func2yyF"()
  let o: MyDerivedClass = MyDerivedClass()
  o.foo()
  // CHECK:  [[SLOT:%.*]] = getelementptr inbounds ptr, ptr {{.*}}, {{i64|i32}} {{.*}}
  // CHECK:  call { ptr, i1 } @llvm.type.checked.load(ptr [[SLOT]], i32 0, metadata !"$s4main7MyClassC3fooyyFTq")
}

// CHECK-64: !0 = !{i64 56, !"$s4main7MyClassC3fooyyFTq"}
// CHECK-64: !1 = !{i64 64, !"$s4main7MyClassC3baryyFTq"}
// CHECK-64: !2 = !{i64 72, !"$s4main7MyClassCACycfCTq"}
// CHECK-64: !3 = !{i64 1, i64 56, i64 76}
// CHECK-64: !4 = !{}
// CHECK-64: !5 = !{i64 80, !"$s4main7MyClassC3fooyyFTq"}
// CHECK-64: !6 = !{i64 88, !"$s4main7MyClassC3baryyFTq"}
// CHECK-64: !7 = !{i64 96, !"$s4main7MyClassCACycfCTq"}
// CHECK-64: !8 = !{i64 1, i64 80, i64 100}
// CHECK-64: !9 = !{i64 68, !"$s4main7MyClassC3baryyFTq"}
// CHECK-64: !10 = !{i64 80, !"$s4main7MyClassCACycfCTq"}
// CHECK-64: !11 = !{i64 1, i64 56, i64 84}

// CHECK-32: !0 = !{i64 56, !"$s4main7MyClassC3fooyyFTq"}
// CHECK-32: !1 = !{i64 64, !"$s4main7MyClassC3baryyFTq"}
// CHECK-32: !2 = !{i64 72, !"$s4main7MyClassCACycfCTq"}
// CHECK-32: !3 = !{i64 1, i64 56, i64 76}
// CHECK-32: !4 = !{}
// CHECK-32: !5 = !{i64 52, !"$s4main7MyClassC3fooyyFTq"}
// CHECK-32: !6 = !{i64 56, !"$s4main7MyClassC3baryyFTq"}
// CHECK-32: !7 = !{i64 60, !"$s4main7MyClassCACycfCTq"}
// CHECK-32: !8 = !{i64 1, i64 52, i64 64}
// CHECK-32: !9 = !{i64 68, !"$s4main7MyClassC3baryyFTq"}
// CHECK-32: !10 = !{i64 80, !"$s4main7MyClassCACycfCTq"}
// CHECK-32: !11 = !{i64 1, i64 56, i64 84}
