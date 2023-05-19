// RUN: %target-swift-frontend -emit-ir %s -g -o - \
// RUN:    -parse-as-library -module-name a -disable-availability-checking | %FileCheck %s

public func f1<each T>(ts: repeat each T) {
  // CHECK: define {{.*}} @"$s1a2f12tsyxxQp_tRvzlF"(%swift.opaque** {{.*}}, i{{32|64}} [[COUNT1_1:.*]], %swift.type** {{.*}})
  // CHECK-DAG: store i{{32|64}} [[COUNT1_1]], i{{32|64}}* %[[COUNT1_1_A:.*]], align
  // CHECK-DAG: call void @llvm.dbg.declare({{.*}}[[COUNT1_1_A]], metadata ![[COUNT1_1_VAR:[0-9]+]], metadata !DIExpression())
  // CHECK-LABEL: ret void
}

public func f2<each U, each V>(us: repeat each U, vs: repeat each V) {
  // CHECK: define {{.*}} @"$s1a2f22us2vsyxxQp_q_q_QptRvzRv_r0_lF"(%swift.opaque** {{.*}}, %swift.opaque** {{.*}}, i{{32|64}} [[COUNT2_1:.*]], i{{32|64}} [[COUNT2_2:.*]], %swift.type** {{.*}}, %swift.type** {{.*}})
  // CHECK-DAG: store i{{32|64}} [[COUNT2_1]], i{{32|64}}* %[[COUNT2_1_A:.*]], align
  // CHECK-DAG: store i{{32|64}} [[COUNT2_2]], i{{32|64}}* %[[COUNT2_2_A:.*]], align
  // CHECK-DAG: call void @llvm.dbg.declare({{.*}}[[COUNT2_1_A]], metadata ![[COUNT2_1_VAR:[0-9]+]], metadata !DIExpression())
  // CHECK-DAG: call void @llvm.dbg.declare({{.*}}[[COUNT2_2_A]], metadata ![[COUNT2_2_VAR:[0-9]+]], metadata !DIExpression())
  // CHECK-LABEL: ret void
}

public func f3<each T>(ts: repeat each T, more_ts: repeat each T) {
  // CHECK: define {{.*}} @"$s1a2f32ts05more_B0yxxQp_xxQptRvzlF"(%swift.opaque** {{.*}}, %swift.opaque** {{.*}}, i{{32|64}} [[COUNT3_1:.*]], %swift.type** {{.*}})
  // CHECK-DAG: store i{{32|64}} [[COUNT3_1]], i{{32|64}}* %[[COUNT3_1_A:.*]], align
  // CHECK-DAG: call void @llvm.dbg.declare({{.*}}[[COUNT3_1_A]], metadata ![[COUNT3_1_VAR:[0-9]+]], metadata !DIExpression())
  // CHECK-LABEL: ret void
}

public func f4<each U, each V>(us: repeat (each U, each V)) {
  // CHECK: define {{.*}} @"$s1a2f42usyx_q_txQp_tRvzRv_q_Rhzr0_lF"(%swift.opaque** {{.*}}, i{{32|64}} [[COUNT4_1:.*]], %swift.type** {{.*}}, %swift.type** {{.*}})
  // CHECK-DAG: store i{{32|64}} [[COUNT4_1]], i{{32|64}}* %[[COUNT4_1_A:.*]], align
  // CHECK-DAG: call void @llvm.dbg.declare({{.*}}[[COUNT4_1_A]], metadata ![[COUNT4_1_VAR:[0-9]+]], metadata !DIExpression())
  // CHECK-LABEL: ret void
}

public struct S<each T> {
    let vals: (repeat each T)

    public func f5() {
    // CHECK: define {{.*}} @"$s1a1SV2f5yyF"(%swift.type* {{.*}}, %T1a1SV* {{.*}} %0)
    }
}

public func f6<each T>(s: S<repeat each T>) {
  // CHECK: define {{.*}} @"$s1a2f61syAA1SVyxxQp_QPG_tRvzlF"(%T1a1SV* {{.*}}, i{{32|64}} [[COUNT6_1:.*]], %swift.type** {{.*}})
  // CHECK-DAG: store i{{32|64}} [[COUNT6_1]], i{{32|64}}* %[[COUNT6_1_A:.*]], align
  // CHECK-DAG: call void @llvm.dbg.declare({{.*}}[[COUNT6_1_A]], metadata ![[COUNT6_1_VAR:[0-9]+]], metadata !DIExpression())
}

// CHECK-LABEL: !DICompileUnit
// CHECK-DAG: [[COUNT1_1_VAR]] = !DILocalVariable(name: "$pack_count_0",{{.*}} flags: DIFlagArtificial)
// CHECK-DAG: [[COUNT2_1_VAR]] = !DILocalVariable(name: "$pack_count_0",{{.*}} flags: DIFlagArtificial)
// CHECK-DAG: [[COUNT2_2_VAR]] = !DILocalVariable(name: "$pack_count_1",{{.*}} flags: DIFlagArtificial)
// CHECK-DAG: [[COUNT3_1_VAR]] = !DILocalVariable(name: "$pack_count_0",{{.*}} flags: DIFlagArtificial)
// CHECK-DAG: [[COUNT4_1_VAR]] = !DILocalVariable(name: "$pack_count_0",{{.*}} flags: DIFlagArtificial)
// CHECK-DAG: [[COUNT6_1_VAR]] = !DILocalVariable(name: "$pack_count_0",{{.*}} flags: DIFlagArtificial)
