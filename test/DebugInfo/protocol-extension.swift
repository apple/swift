// RUN: %target-swift-frontend -primary-file %s -emit-ir -g -o - | %FileCheck %s

public protocol P {
  var v : Int32 { get };
}

public extension P {
  // CHECK: define {{.*}}swiftcc i32 @"$s4main1PPAAE1fs5Int32VyF"
  public func f() -> Int32 {
    // CHECK-NEXT: entry:
    // CHECK-NEXT: %[[ALLOCA:.*]] = alloca ptr,
    // CHECK-NEXT: @llvm.dbg.declare(metadata ptr %[[ALLOCA]],
    // CHECK-SAME:    metadata ![[SELFMETA:.*]], metadata !DIExpression())
    return v
  }
}

// CHECK: ![[SELFMETA]] = !DILocalVariable(name: "$\CF\84_0_0",
// CHECK-SAME: type: ![[SELFTY:[0-9]+]], flags: DIFlagArtificial)
// CHECK: ![[SELFTY]] = !DIDerivedType(tag: DW_TAG_typedef, name: "Self"
