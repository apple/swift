// RUN: %empty-directory(%t)
// RUN: %build-irgen-test-overlays
// RUN: %target-swift-frontend(mock-sdk: -sdk %S/Inputs -I %t) -primary-file %s -emit-ir > %t/out.txt
// RUN: %FileCheck %s -DINT=i%target-ptrsize < %t/out.txt
// RUN: %FileCheck %s --check-prefix=NEGATIVE < %t/out.txt


// REQUIRES: CPU=x86_64
// REQUIRES: objc_interop

import Foundation
import gizmo

// CHECK: @"$sSo16NSRuncingOptionsVMn" = linkonce_odr hidden constant
// CHECK: @"$sSo16NSRuncingOptionsVMf" = linkonce_odr hidden constant
//   CHECK-SAME: @"$sBi{{[0-9]+}}_WV"
// CHECK: @"$sSo16NSRuncingOptionsVSQSCMc" = linkonce_odr hidden constant

// NEGATIVE-NOT: @"$sSo28NeverActuallyMentionedByNameVSQSCWp" = linkonce_odr hidden constant

// CHECK-LABEL: define{{( dllexport)?}}{{( protected)?}} i32 @main
// CHECK:         call swiftcc %swift.metadata_response @"$sSo16NSRuncingOptionsVMa"(i64 0)

func use_metadata<T: Equatable>(_ t:T){}
use_metadata(NSRuncingOptions.mince)

// CHECK-LABEL: define linkonce_odr hidden swiftcc %swift.metadata_response @"$sSo16NSRuncingOptionsVMa"(i64 %0)
// CHECK:         call swiftcc %swift.metadata_response @swift_getForeignTypeMetadata([[INT]] %0,
// CHECK-SAME:    @"$sSo16NSRuncingOptionsVMf"
// CHECK-SAME:    [[NOUNWIND_READNONE:#[0-9]+]]

// CHECK: define hidden swiftcc i16 @"$s12objc_ns_enum09imported_C9_inject_aSo16NSRuncingOptionsVyF"()
// CHECK:   ret i16 123
func imported_enum_inject_a() -> NSRuncingOptions {
  return .mince
}

// CHECK: define hidden swiftcc i16 @"$s12objc_ns_enum09imported_C9_inject_bSo16NSRuncingOptionsVyF"()
// CHECK:   ret i16 4567
func imported_enum_inject_b() -> NSRuncingOptions {
  return .quinceSliced
}

// CHECK: define hidden swiftcc i16 @"$s12objc_ns_enum09imported_C9_inject_cSo16NSRuncingOptionsVyF"()
// CHECK:   ret i16 5678
func imported_enum_inject_c() -> NSRuncingOptions {
  return .quinceJulienned
}

// CHECK: define hidden swiftcc i16 @"$s12objc_ns_enum09imported_C9_inject_dSo16NSRuncingOptionsVyF"()
// CHECK:   ret i16 6789
func imported_enum_inject_d() -> NSRuncingOptions {
  return .quinceDiced
}

// CHECK: define hidden swiftcc i32 @"$s12objc_ns_enum09imported_C17_inject_radixed_aSo16NSRadixedOptionsVyF"() {{.*}} {
// -- octal 0755
// CHECK:   ret i32 493
func imported_enum_inject_radixed_a() -> NSRadixedOptions {
  return .octal
}

// CHECK: define hidden swiftcc i32 @"$s12objc_ns_enum09imported_C17_inject_radixed_bSo16NSRadixedOptionsVyF"() {{.*}} {
// -- hex 0xFFFF
// CHECK:   ret i32 65535
func imported_enum_inject_radixed_b() -> NSRadixedOptions {
  return .hex
}

// CHECK: define hidden swiftcc i32 @"$s12objc_ns_enum09imported_C18_inject_negative_aSo17NSNegativeOptionsVyF"() {{.*}} {
// CHECK:   ret i32 -1
func imported_enum_inject_negative_a() -> NSNegativeOptions {
  return .foo
}

// CHECK: define hidden swiftcc i32 @"$s12objc_ns_enum09imported_C18_inject_negative_bSo17NSNegativeOptionsVyF"() {{.*}} {
// CHECK:   ret i32 -2147483648
func imported_enum_inject_negative_b() -> NSNegativeOptions {
  return .bar
}

// CHECK: define hidden swiftcc i32 @"$s12objc_ns_enum09imported_C27_inject_negative_unsigned_aSo25NSNegativeUnsignedOptionsVyF"() {{.*}} {
// CHECK:   ret i32 -1
func imported_enum_inject_negative_unsigned_a() -> NSNegativeUnsignedOptions {
  return .foo
}

// CHECK: define hidden swiftcc i32 @"$s12objc_ns_enum09imported_C27_inject_negative_unsigned_bSo25NSNegativeUnsignedOptionsVyF"() {{.*}} {
// CHECK:   ret i32 -2147483648
func imported_enum_inject_negative_unsigned_b() -> NSNegativeUnsignedOptions {
  return .bar
}

func test_enum_without_name_Equatable(_ obj: TestThatEnumType) -> Bool {
  return obj.getValue() != .ValueOfThatEnumType
}

@objc enum ExportedToObjC: Int {
  case Foo = -1, Bar, Bas
}

@objc class ObjCEnumMethods : NSObject {
  // CHECK: define internal void @"$s12objc_ns_enum15ObjCEnumMethodsC0C2InyyAA010ExportedToD1COFTo"(ptr %0, ptr %1, i64 %2)
  @objc dynamic func enumIn(_ x: ExportedToObjC) {}
  // CHECK: define internal i64 @"$s12objc_ns_enum15ObjCEnumMethodsC0C3OutAA010ExportedToD1COyFTo"(ptr %0, ptr %1)
  @objc dynamic func enumOut() -> ExportedToObjC { return .Foo }

  // CHECK: define internal i64 @"$s12objc_ns_enum15ObjCEnumMethodsC4propAA010ExportedToD1COvgTo"(ptr %0, ptr %1)
  // CHECK: define internal void @"$s12objc_ns_enum15ObjCEnumMethodsC4propAA010ExportedToD1COvsTo"(ptr %0, ptr %1, i64 %2)
  @objc dynamic var prop: ExportedToObjC = .Foo
}

// CHECK-LABEL: define hidden swiftcc void @"$s12objc_ns_enum0a1_C13_method_callsyyAA15ObjCEnumMethodsCF"(ptr %0)
func objc_enum_method_calls(_ x: ObjCEnumMethods) {
  
  // CHECK: call i64 @objc_msgSend
  // CHECK: call void @objc_msgSend
  x.enumIn(x.enumOut())
  // CHECK: call i64 @objc_msgSend
  // CHECK: call void @objc_msgSend
  x.enumIn(x.prop)
  // CHECK: call i64 @objc_msgSend
  // CHECK: call void @objc_msgSend
  x.prop = x.enumOut()
}

// CHECK: attributes [[NOUNWIND_READNONE]] = { nounwind readnone }

