// RUN: %target-swift-frontend %use_no_opaque_pointers -primary-file %s -emit-ir | %FileCheck %s
// RUN: %target-swift-frontend -primary-file %s -emit-ir

// REQUIRES: CPU=i386 || CPU=x86_64

// CHECK: define hidden swiftcc void @"$s15indirect_return11generic_get{{[_0-9a-zA-Z]*}}F"
func generic_get<T>(p: UnsafeMutablePointer<T>) -> T {
  // CHECK-NOT: [[T0:%.*]] = call i8* @_TFVs20UnsafeMutablePointerl6memoryQ_(i8* %1, %swift.type* %T)
  // CHECK: [[T1:%.*]] = bitcast i8* {{%.*}} to %swift.opaque*
  // CHECK: call %swift.opaque* {{%.*}}(%swift.opaque* noalias %0, %swift.opaque* noalias [[T1]], %swift.type* %T)
  return p.pointee
}


protocol Number {}
extension Int: Number {}

// Make sure that the absence of the sret attribute matches.
// CHECK: define hidden swiftcc void @"$s15indirect_return3fooSS_S2SAA6Number_pAaC_ptyF"(<{ %TSS, %TSS, %TSS }>* noalias nocapture
func foo() -> (String, String, String, Number, Number) {
    return ("1", "2", "3", 42, 7)
}
// CHECK-LABEL: define{{.*}}testCall
func testCall() {
// CHECK: call swiftcc void @"$s15indirect_return3fooSS_S2SAA6Number_pAaC_ptyF"(<{ %TSS, %TSS, %TSS }>* noalias nocapture %{{.*}}
  print(foo())
}
