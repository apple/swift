// RUN: %target-swift-frontend %s -emit-ir -g -o - | %FileCheck %s

protocol AProtocol {
  func f() -> String
}
class AClass : AProtocol {
  func f() -> String { return "A" }
}

// CHECK: define hidden {{.*}}void @{{.*}}aFunction
// CHECK:  call void @llvm.dbg.declare(metadata ptr %{{.*}}, metadata ![[TYPEARG:.*]], metadata !DIExpression()),
// CHECK: ![[TYPEARG]] = !DILocalVariable(name: "$\CF\84_0_0"
// CHECK-SAME:                            type: ![[SWIFTMETATYPE:[^,)]+]]
// CHECK-SAME:                            flags: DIFlagArtificial
// CHECK: ![[SWIFTMETATYPE]] = !DIDerivedType(tag: DW_TAG_typedef, name: "T",
// CHECK-SAME:                                baseType: ![[VOIDPTR:[0-9]+]]
// CHECK: ![[VOIDPTR]] = !DIDerivedType(tag: DW_TAG_pointer_type, name: "$sBpD", baseType: null
func aFunction<T : AProtocol>(_ x: T) {
    print("I am in aFunction: \(x.f())")
}

aFunction(AClass())
