// RUN: %target-swift-frontend %s -emit-ir -g -o - | %FileCheck %s
protocol A {}
protocol B {}
typealias C = B & A
protocol D {}
var p: (C & D)?
// CHECK-DAG: !DIGlobalVariable(name: "p", {{.*}}type: ![[TY_CONTAINER:[0-9]+]]
// CHECK-DAG: ![[TY_CONTAINER]] = !DICompositeType({{.*}}elements: ![[TY_ELTS:[0-9]+]]
// CHECK-DAG: ![[TY_ELTS]] = !{![[TY_MEMBER:[0-9]+]]}
// CHECK-DAG: ![[TY_MEMBER]] = !DIDerivedType(tag: DW_TAG_member, {{.*}}baseType: ![[TY:[0-9]+]]
// CHECK-DAG: ![[TY]] = {{.*}}identifier: "$s4main1A_AA1BAA1DpSgD"
