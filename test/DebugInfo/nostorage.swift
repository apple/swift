// RUN: %target-swift-frontend %s -emit-ir -g -o %t
// RUN: cat %t | %FileCheck %s --check-prefix=CHECK1
// RUN: cat %t | %FileCheck %s --check-prefix=CHECK2
// RUN: cat %t | %FileCheck %s --check-prefix=CHECK3

func used<T>(_ t: T) {}

public class Foo {
    func foo() {
      { [weak self] in
      // CHECK1: call void @llvm.dbg.value(metadata i{{.*}} 0,
      // CHECK1-SAME:                      metadata ![[TYPE:.*]], metadata
      // CHECK1: ![[TYPE]] = !DILocalVariable(name: "type",
      // CHECK1-SAME:                         line: [[@LINE+6]],
      // CHECK1-SAME:                         type: ![[LET_METAFOO:[0-9]+]]
      // CHECK1: ![[LET_METAFOO]] = !DIDerivedType(tag: DW_TAG_const_type,
      // CHECK1-SAME:                             baseType: ![[METAFOO:[0-9]+]])
      // CHECK1: ![[METAFOO]] = !DICompositeType(tag: DW_TAG_structure_type,
      // CHECK1-SAME:                            flags:
            let type = Swift.type(of: self)
            used(type)
        }()
    }
}

struct AStruct {}

// CHECK2: define{{.*}}app
public func app() {
  // No members? No storage!
  // CHECK2: call void @llvm.dbg.value(metadata ptr undef,
  // CHECK2-SAME:                      metadata ![[AT:.*]], metadata
  // CHECK2: ![[AT]] = !DILocalVariable(name: "at",{{.*}}line: [[@LINE+1]]
  var at = AStruct()
  
  used(at)
}

public enum empty { case exists }
public let globalvar = empty.exists
// CHECK3: !DIGlobalVariableExpression(var: ![[VAR:[0-9]+]],
// CHECK3-SAME: expr: !DIExpression(DW_OP_constu, 0, DW_OP_stack_value))
// CHECK3: ![[VAR]] = distinct !DIGlobalVariable(name: "globalvar",
// CHECK3-SAME:          {{.*}}line: [[@LINE-4]], {{.*}}isLocal: false,
// CHECK3-SAME:          isDefinition: true)
