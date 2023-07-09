// Tests that under -enable-llvm-wme, IRGen marks wtables and wcall sites with
// the right attributes and intrinsics.

// RUN: %target-build-swift -Xfrontend -disable-objc-interop -Xfrontend -enable-llvm-wme \
// RUN:    %s -emit-ir -o - | %FileCheck %s --check-prefix=CHECK --check-prefix=CHECK-%target-ptrsize

protocol TheProtocol {
  func func1_live()
  func func2_dead()
}

struct MyStruct : TheProtocol {
  func func1_live() { print("MyStruct.func1_live") }
  func func2_dead() { print("MyStruct.func2_dead") }
}

// CHECK:         @"$s4main8MyStructVAA11TheProtocolAAWP" =
// CHECK-SAME:    ptr @"$s4main8MyStructVAA11TheProtocolAAMc{{(.ptrauth)?}}"
// CHECK-SAME:    ptr @"$s4main8MyStructVAA11TheProtocolA2aDP10func1_liveyyFTW{{(.ptrauth)?}}"
// CHECK-SAME:    ptr @"$s4main8MyStructVAA11TheProtocolA2aDP10func2_deadyyFTW{{(.ptrauth)?}}"
// CHECK-64-SAME: align 8, !type !0, !type !1, !vcall_visibility !2, !typed_global_not_for_cfi !3
// CHECK-32-SAME: align 4, !type !0, !type !1, !vcall_visibility !2, !typed_global_not_for_cfi !3

func test1() {
  // CHECK: define hidden swiftcc void @"$s4main5test1yyF"()
  let x: MyStruct = MyStruct()
  x.func1_live()
  // CHECK:      call swiftcc void @"$s4main8MyStructVACycfC"()
  // CHECK-NEXT: call swiftcc void @"$s4main8MyStructV10func1_liveyyF"()
  // CHECK-NEXT: ret void
}

func test2() {
  // CHECK: define hidden swiftcc void @"$s4main5test2yyF"()
  let x: TheProtocol = MyStruct()
  x.func1_live()
  // CHECK: load
  // CHECK:  [[WTABLE:%.*]]    = load ptr, ptr {{.*}}
  // CHECK:  [[SLOT:%.*]]      = getelementptr inbounds {{.*}}, ptr [[WTABLE]], i32 1
  // CHECK:                      call { ptr, i1 } @llvm.type.checked.load(ptr [[SLOT]], i32 0, metadata !"$s4main11TheProtocolP10func1_liveyyFTq")
}

// CHECK-64: !0 = !{i64 8, !"$s4main11TheProtocolP10func1_liveyyFTq"}
// CHECK-64: !1 = !{i64 16, !"$s4main11TheProtocolP10func2_deadyyFTq"}
// CHECK-64: !2 = !{i64 1, i64 8, i64 20}
// CHECK-64: !3 = !{}

// CHECK-32: !0 = !{i64 4, !"$s4main11TheProtocolP10func1_liveyyFTq"}
// CHECK-32: !1 = !{i64 8, !"$s4main11TheProtocolP10func2_deadyyFTq"}
// CHECK-32: !2 = !{i64 1, i64 4, i64 12}
// CHECK-32: !3 = !{}
