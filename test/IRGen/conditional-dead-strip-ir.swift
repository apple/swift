// Tests that with -conditional-runtime-records, IRGen marks class, struct,
// enum, protocol, and protocol conformance records as conditionally removable
// via !llvm.used.conditional metadata.

// RUN: %target-build-swift -Xfrontend -conditional-runtime-records -Xfrontend -disable-objc-interop %s -emit-ir -o - | %FileCheck %s

public protocol TheProtocol {
}

public class Class: TheProtocol {
}

public struct Struct {
}

public enum Enum {
}

// CHECK:      @llvm.{{(compiler.)?}}used = appending global [
// CHECK-SAME:   @"$s4main11TheProtocolHr"
// CHECK-SAME:   @"$s4main5ClassCAA11TheProtocolAAHc"
// CHECK-SAME:   @"$s4main5ClassCHn"
// CHECK-SAME:   @"$s4main6StructVHn"
// CHECK-SAME:   @"$s4main4EnumOHn"
// CHECK-SAME: ], section "llvm.metadata"

// CHECK:      !llvm.used.conditional = !{[[M1:!.*]], [[M2:!.*]], [[M3:!.*]], [[M4:!.*]], [[C1:!.*]], [[C2:!.*]], [[C3:!.*]], [[C4:!.*]], [[C5:!.*]]}

// CHECK-DAG:      [[M1]]  = !{{{.*}} @"$s4main11TheProtocol_pMF", i32 0, [[M1A:!.*]]}
// CHECK-DAG:      [[M1A]] =   {{.*}} @"$s4main11TheProtocolMp"
// CHECK-DAG:      [[M2]]  = !{{{.*}} @"$s4main5ClassCMF", i32 0, [[M2A:!.*]]}
// CHECK-DAG:      [[M2A]] =   {{.*}} @"$s4main5ClassCMn"
// CHECK-DAG:      [[M3]]  = !{{{.*}} @"$s4main6StructVMF", i32 0, [[M3A:!.*]]}
// CHECK-DAG:      [[M3A]] =   {{.*}} @"$s4main6StructVMn"
// CHECK-DAG:      [[M4]]  = !{{{.*}} @"$s4main4EnumOMF", i32 0, [[M4A:!.*]]}
// CHECK-DAG:      [[M4A]] =   {{.*}} @"$s4main4EnumOMn"

// CHECK-DAG:      [[C1]]  = !{{{.*}} @"$s4main11TheProtocolHr", i32 0, [[M1A:!.*]]}

// CHECK-DAG:      [[C2]]  = !{{{.*}} @"$s4main5ClassCAA11TheProtocolAAHc", i32 1, [[C2A:!.*]]}
// CHECK-DAG:      [[C2A]] =   {{.*}} @"$s4main11TheProtocolMp", {{.*}} @"$s4main5ClassCMn"}

// CHECK-DAG:      [[C3]]  = !{{{.*}} @"$s4main5ClassCHn", i32 0, [[M2A:!.*]]}

// CHECK-DAG:      [[C4]]  = !{{{.*}} @"$s4main6StructVHn", i32 0, [[M3A:!.*]]}

// CHECK-DAG:      [[C5]]  = !{{{.*}} @"$s4main4EnumOHn", i32 0, [[M4A:!.*]]}
