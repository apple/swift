// RUN: %target-swift-frontend %s -emit-ir -g -o - | %FileCheck %s
public struct S<Type>
{
  let value : Type
}

public func foo<Type>(_ values : [S<Type>])
{
  // CHECK: define {{.*}}$s12generic_arg53fooyySayAA1SVyxGGlFAESgAEXEfU_
  // CHECK: call void @llvm.dbg.declare
  // CHECK: call void @llvm.dbg.declare(metadata ptr %[[ALLOCA:[^,]+]],
  // CHECK-SAME:       metadata ![[ARG:[0-9]+]],
  // CHECK-SAME:       metadata !DIExpression(DW_OP_deref))
  // CHECK: store ptr %1, ptr %[[ALLOCA]], align
  // CHECK: ![[TYP:[0-9]+]] = !DICompositeType({{.*}}, name: "$s12generic_arg51SVyxGD"
  // The argument is a by-ref struct and thus needs to be dereferenced.
  // CHECK: ![[ARG]] = !DILocalVariable(name: "arg", arg: 1,
  // CHECK-SAME:                        line: [[@LINE+8]],
  // CHECK-SAME:                        type: ![[LET_TYP:[0-9]+]])
  // CHECK: ![[LET_TYP]] = !DIDerivedType(tag: DW_TAG_const_type,
  // CHECK-SAME:                          baseType: ![[TYP_CONTAINER:[0-9]+]])
  // CHECK: ![[TYP_CONTAINER]] = !DICompositeType({{.*}}elements: ![[TYP_ELTS:[0-9]+]]
  // CHECK: ![[TYP_ELTS]] = !{![[TYP_MEMBER:[0-9]+]]}
  // CHECK: ![[TYP_MEMBER]] = !DIDerivedType(tag: DW_TAG_member, {{.*}}baseType: ![[TYP_:[0-9]+]]
  // CHECK: ![[TYP_]] = !DICompositeType({{.*}}, name: "$s12generic_arg51SVyxGD"
  let _ = values.flatMap { arg in
    return .some(arg)
  }
 
}
