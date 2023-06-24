// RUN: %target-swift-frontend -disable-availability-checking -primary-file %s -emit-sil -O -g | %FileCheck %s --check-prefix CHECK-SIL
// RUN: %target-swift-frontend -disable-availability-checking -primary-file %s -emit-ir -disable-llvm-optzns -O -g | %FileCheck %s

// UNSUPPORTED: OS=watchos

protocol External {
  func use(str: String);
  func decode<T>(_: T.Type) -> T
}

struct Data {
  var a: String
  var b: String
}

func test(cond: Int, external: External) async {
  // CHECK-DAG: ![[VAR:[0-9]+]] = !DILocalVariable(name: "data", scope: !{{[0-9]+}}, file: !{{[0-9]+}}, line: [[# @LINE + 1]], type: !{{[0-9]+}})
  let data: Data
  switch cond {
  // CHECK-DAG: ![[LOC1:[0-9]+]] = !DILocation(line: [[# @LINE + 1]], column: 12, scope: !{{.*}})
  case 42: data = external.decode(Data.self)
  // CHECK-DAG: ![[LOC2:[0-9]+]] = !DILocation(line: [[# @LINE + 1]], column: 12, scope: !{{.*}})
  default: data = external.decode(Data.self)
  }
  external.use(str: data.a)
  external.use(str: data.b)
}

// CHECK-SIL: debug_value %{{.*}} : $String, let, (name "data", {{.*}}), type $*Data, expr op_fragment:#Data.a
// CHECK-SIL: debug_value %{{.*}} : $String, let, (name "data", {{.*}}), type $*Data, expr op_fragment:#Data.b
// CHECK-SIL: debug_value %{{.*}} : $String, let, (name "data", {{.*}}), type $*Data, expr op_fragment:#Data.a
// CHECK-SIL: debug_value %{{.*}} : $String, let, (name "data", {{.*}}), type $*Data, expr op_fragment:#Data.b

// CHECK-DAG: llvm.dbg.declare{{.*}} metadata ![[VAR:[0-9]+]], metadata !DIExpression(DW_OP_LLVM_fragment, 192, 64){{.*}} !dbg ![[LOC1]]
// CHECK-DAG: llvm.dbg.declare{{.*}} metadata ![[VAR]], metadata !DIExpression(DW_OP_LLVM_fragment, 128, 64){{.*}} !dbg ![[LOC1]]
// CHECK-DAG: llvm.dbg.declare{{.*}} metadata ![[VAR]], metadata !DIExpression(DW_OP_LLVM_fragment, 64, 64){{.*}} !dbg ![[LOC1]]
// CHECK-DAG: llvm.dbg.declare{{.*}} metadata ![[VAR]], metadata !DIExpression(DW_OP_LLVM_fragment, 0, 64){{.*}} !dbg ![[LOC1]]
//
// CHECK-DAG: llvm.dbg.declare{{.*}} metadata ![[VAR]], metadata !DIExpression(DW_OP_LLVM_fragment, 192, 64){{.*}} !dbg ![[LOC2]]
// CHECK-DAG: llvm.dbg.declare{{.*}} metadata ![[VAR]], metadata !DIExpression(DW_OP_LLVM_fragment, 128, 64){{.*}} !dbg ![[LOC2]]
// CHECK-DAG: llvm.dbg.declare{{.*}} metadata ![[VAR]], metadata !DIExpression(DW_OP_LLVM_fragment, 64, 64){{.*}} !dbg ![[LOC2]]
// CHECK-DAG: llvm.dbg.declare{{.*}} metadata ![[VAR]], metadata !DIExpression(DW_OP_LLVM_fragment, 0, 64){{.*}} !dbg ![[LOC2]]
