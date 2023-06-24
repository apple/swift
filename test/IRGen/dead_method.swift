// RUN: %target-swift-frontend %s -emit-ir -O | %FileCheck %s --check-prefix=CHECK --check-prefix=CHECK-%target-ptrsize -DINT=i%target-ptrsize

// Test to make sure that methods removed by dead code elimination still appear
// in the vtable in both the nominal type descriptor and class metadata.
//
// We should be dropping the implementation, not the actual entry -- it is still
// there but filled in with a placeholder.

public class Class {
  public init() {}
  public func live() {}
  private func dead() {}
}

// CHECK-LABEL: @"$s11dead_method5ClassCMn" ={{( dllexport)?}}{{( protected)?}} constant <{{.*}}> <{

// -- metadata accessor
// CHECK-SAME: "$s11dead_method5ClassCMa"

// -- vtable
// CHECK-SAME: %swift.method_descriptor {
// CHECK-SAME: i32 {{(1|-835911679)}},
// CHECK-SAME: @"$s11dead_method5ClassCACycfC"
// CHECK-SAME: }

// CHECK-SAME: %swift.method_descriptor {
// CHECK-SAME: i32 {{(16|-1609105392)}},
// CHECK-SAME: @"$s11dead_method5ClassC4liveyyF"
// CHECK-SAME: }

// CHECK-SAME: %swift.method_descriptor { i32 {{(16|-1887436784)}}, i32 0 }
// CHECK-SAME: }>

// CHECK-LABEL: @"$s11dead_method5ClassCMf" = internal global <{{.*}}> <{

// -- destructor
// CHECK-SAME:   ptr {{.*}}@"$s11dead_method5ClassCfD{{(.ptrauth)?}}"

// -- value witness table
// CHECK-SAME:   ptr {{@"\$sBoWV"|null}},

// -- nominal type descriptor
// CHECK-SAME:   @"$s11dead_method5ClassCMn{{(.ptrauth)?}}"

// -- ivar destroyer
// CHECK-SAME:   ptr null,

// -- vtable
// CHECK-SAME:   ptr {{.*}}@"$s11dead_method5ClassCACycfC{{(.ptrauth)?}}"
// CHECK-SAME:   ptr {{.*}}@"$s11dead_method5ClassC4liveyyF{{(.ptrauth)?}}"
// CHECK-SAME:   ptr {{.*}}@swift_deletedMethodError{{(.ptrauth)?}}

// CHECK-SAME: }>
