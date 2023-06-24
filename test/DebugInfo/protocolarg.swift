// RUN: %target-swift-frontend %s -emit-ir -g -o - | %FileCheck %s

func markUsed<T>(_ t: T) {}
func use<T>(_ t: inout T) {}

public protocol IGiveOutInts {
  func callMe() -> Int64
}

// CHECK: define {{.*}}@"$s11protocolarg16printSomeNumbersyyAA12IGiveOutInts_pF"
// CHECK: @llvm.dbg.declare(metadata ptr %
// CHECK-SAME:              metadata ![[ARG:[0-9]+]],
// CHECK-SAME:              metadata !DIExpression(DW_OP_deref))
// CHECK: @llvm.dbg.declare(metadata ptr %
// CHECK-SAME:              metadata ![[VAR:.*]], metadata !DIExpression())

public func printSomeNumbers(_ gen: IGiveOutInts) {
  var gen = gen
  // FIXME: Should be DW_TAG_interface_type
  // CHECK: ![[PT:[0-9]+]] = !DICompositeType(tag: DW_TAG_structure_type, name: "IGiveOutInts"
  // CHECK: ![[ARG]] = !DILocalVariable(name: "gen", arg: 1,
  // CHECK-SAME:                        line: [[@LINE-5]],
  // CHECK-SAME:                        type: ![[LET_PT:[0-9]+]]
  // CHECK: ![[LET_PT]] = !DIDerivedType(tag: DW_TAG_const_type,
  // CHECK-SAME:                         baseType: ![[PT]])
  // CHECK: ![[VAR]] = !DILocalVariable(name: "gen", {{.*}} line: [[@LINE-8]],
  // CHECK-SAME:                        type: ![[PT]]
  markUsed(gen.callMe())
  use(&gen)
}

