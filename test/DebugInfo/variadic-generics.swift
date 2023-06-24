// RUN: %target-swift-frontend -emit-ir %s -g -o - \
// RUN:    -parse-as-library -module-name a | %IRGenFileCheck %s

public func foo<each T>(args: repeat each T) {
  // CHECK: define {{.*}} @"$s1a3foo4argsyxxQp_tRvzlF"
  // CHECK-SAME: ptr {{.*}} %[[ARG_0:.*]], i{{.*}} %{{.*}},
  // CHECK-SAME: ptr %[[TYPE_PACK_ARG:.*]])
  // CHECK: %[[TYPE_PACK_ALLOCA:.*]] = alloca ptr
  // CHECK: call void @llvm.dbg.declare(metadata ptr %[[TYPE_PACK_ALLOCA]], metadata ![[TYPE_PACK_VAR:[0-9]+]], metadata !DIExpression())
  // CHECK: %[[ARGS_ALLOCA:.*]] = alloca ptr
  // CHECK-DAG: call void @llvm.dbg.declare(metadata ptr %[[ARGS_ALLOCA]], metadata ![[ARGS_VAR:[0-9]+]], metadata !DIExpression(DW_OP_deref))
  // CHECK-DAG: %[[TYPE_PACK_ARG_INT:[^,]+]] = ptrtoint ptr %[[TYPE_PACK_ARG]] to [[INT]]
  // CHECK-DAG: %[[TYPE_PACK_ARG_MASKED_INT:[^,]+]] = and [[INT]] %[[TYPE_PACK_ARG_INT]], -2
  // CHECK-DAG: %[[TYPE_PACK_ARG_MASKED:[^,]+]] = inttoptr [[INT]] %[[TYPE_PACK_ARG_MASKED_INT]] to ptr
  // CHECK-DAG: store ptr %[[TYPE_PACK_ARG_MASKED]], ptr %[[TYPE_PACK_ALLOCA]]
  // CHECK-DAG: store ptr %[[ARG_0]], ptr %[[ARGS_ALLOCA]]
  // CHECK-DAG: ![[ARGS_VAR]] = !DILocalVariable(name: "args", arg: 1, {{.*}}line: [[@LINE-13]], type: ![[ARGS_LET_TY:[0-9]+]])
  // CHECK-DAG: ![[ARGS_LET_TY]] = !DIDerivedType(tag: DW_TAG_const_type, baseType: ![[ARGS_TY:[0-9]+]])
  // CHECK-DAG: ![[ARGS_TY]] = !DICompositeType({{.*}}identifier: "$sxxQp_QSiD")
  // CHECK-DAG: ![[TYPE_PACK_VAR]] = !DILocalVariable(name: "$\CF\84_0_0", {{.*}}type: ![[TYPE_PACK_TYD:[0-9]+]], flags: DIFlagArtificial)
  // CHECK-DAG: ![[TYPE_PACK_TYD]] = !DIDerivedType(tag: DW_TAG_typedef, name: "T", {{.*}}baseType: ![[TYPE_PACK_TY:[0-9]+]]
  // CHECK-DAG: ![[TYPE_PACK_TY]] = !DIDerivedType(tag: DW_TAG_pointer_type, name: "$sBpD"
}

