// RUN: %target-swift-frontend -swift-version 4 -parse-stdlib -Xllvm -sil-disable-pass=FunctionSignatureOpts -Xllvm -sil-disable-pass=GenericSpecializer -emit-ir -O %s | %FileCheck %s
//
// Check that the -O pipeline always preserves the runtime calls for Builtin access markers and that the KeyPath implementation is fully inlined.

@_silgen_name("marker1")
func marker1() -> ()

@_silgen_name("marker2")
func marker2() -> ()

@_silgen_name("marker3")
func marker3() -> ()

// IR-LABEL: define {{.*}}swiftcc void @"$s20preserve_exclusivity11beginAccessyyBp_BpxmtlF"(ptr, ptr, ptr{{.*}}, ptr{{.*}} %T1)
// IR:   call void @swift_beginAccess
// IR-NEXT: ret void

public func beginAccess<T1>(_ address: Builtin.RawPointer, _ scratch: Builtin.RawPointer, _ ty1: T1.Type) {
  marker1()
  Builtin.beginUnpairedModifyAccess(address, scratch, ty1);
}

// CHECK-LABEL: define {{.*}}swiftcc void @"$s20preserve_exclusivity9endAccessyyBpF"(ptr{{.*}})
// CHECK:   call void @swift_endAccess
// CHECK-NEXT: ret void
public func endAccess(_ address: Builtin.RawPointer) {
  marker2()
  Builtin.endUnpairedAccess(address)
}

// CHECK-LABEL: define {{.*}}swiftcc void @"$s20preserve_exclusivity10readAccessyyBp_xmtlF"(ptr %0, ptr{{.*}}, ptr{{.*}} %T1)
// CHECK:   call void @swift_beginAccess
// CHECK: ret void
public func readAccess<T1>(_ address: Builtin.RawPointer, _ ty1: T1.Type) {
  marker3()
  Builtin.performInstantaneousReadAccess(address, ty1);
}

// Make sure testAccess properly inlines in our functions.
//
// CHECK-LABEL: define {{.*}}swiftcc void @"$s20preserve_exclusivity10testAccessyyBpF"(ptr %0)
// CHECK: call swiftcc void @marker1
// CHECK: call void @swift_beginAccess
// CHECK: call swiftcc void @marker2
// CHECK: call void @swift_endAccess
// CHECK: call swiftcc void @marker3
// CHECK: call void @swift_beginAccess
// CHECK: ret void
public func testAccess(_ k1: Builtin.RawPointer) {
  beginAccess(k1, k1, Builtin.RawPointer.self)
  endAccess(k1)
  readAccess(k1, Builtin.RawPointer.self)
}
