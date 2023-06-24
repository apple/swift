// RUN: %target-swift-frontend -primary-file %s -emit-ir -g -o - | %FileCheck %s

struct __CurrentErrno {}
struct CErrorOr<T>
 {
  var value : T?
  init(x : __CurrentErrno) {
    // CHECK: define hidden {{.*}}void @"$s20generic_enum_closure8CErrorOrV1xACyxGAA14__CurrentErrnoV_tcfC"
    // CHECK-NOT: define
    // This is a SIL-level debug_value_addr instruction.
    // CHECK: call void @llvm.dbg.declare
    // Self is in a dynamic alloca, hence the shadow copy.
    // CHECK: call void @llvm.dbg.declare(
    // CHECK-SAME: metadata ptr %[[SHADOW:.*]], metadata ![[SELF:.*]], meta
    // CHECK-SAME: !DIExpression(DW_OP_deref))
    // CHECK-DAG: store ptr %[[DYN:.*]], ptr %[[SHADOW]]
    // CHECK-DAG: %[[DYN]] = alloca i8, i{{32|64}} %
    // CHECK-DAG: ![[SELF]] = !DILocalVariable(name: "self", scope:{{.*}}, type: ![[TY_CONTAINER:.*]])
    // CHECK-DAG: ![[TY_CONTAINER]] = !DICompositeType({{.*}}elements: ![[TY_ELTS:[0-9]+]]
    // CHECK-DAG: ![[TY_ELTS]] = !{![[TY_MEMBER:[0-9]+]]}
    // CHECK-DAG: ![[TY_MEMBER]] = !DIDerivedType(tag: DW_TAG_member, {{.*}}baseType: ![[TY:[0-9]+]]
    // CHECK-DAG: ![[TY]] = !DICompositeType({{.*}}, name: "$s20generic_enum_closure8CErrorOrVyxGD"
    value = .none
  }
}
