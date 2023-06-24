// RUN: %empty-directory(%t)
// RUN: %build-irgen-test-overlays
// RUN: %target-swift-frontend(mock-sdk: -sdk %S/Inputs -I %t) %use_no_opaque_pointers -primary-file %s -emit-ir | %FileCheck %s
// RUN: %target-swift-frontend(mock-sdk: -sdk %S/Inputs -I %t) -primary-file %s -emit-ir

// REQUIRES: CPU=x86_64
// REQUIRES: objc_interop

import Foundation
import gizmo

// CHECK: [[GIZMO:%TSo5GizmoC]] = type opaque
// CHECK: [[NSRECT:%TSo6NSRectV]] = type <{ [[NSPOINT:%TSo7NSPointV]], [[NSSIZE:%TSo6NSSizeV]] }>
// CHECK: [[NSPOINT]] = type <{ [[DOUBLE:%TSd]], [[DOUBLE]] }>
// CHECK: [[DOUBLE]] = type <{ double }>
// CHECK: [[NSSIZE]] = type <{ [[DOUBLE]], [[DOUBLE]] }>
// CHECK: [[NSSTRING:%TSo8NSStringC]] = type opaque
// CHECK: [[NSVIEW:%TSo6NSViewC]] = type opaque

// CHECK: define hidden swiftcc { double, double, double, double } @"$s12objc_structs8getFrame{{[_0-9a-zA-Z]*}}F"([[GIZMO]]* %0) {{.*}} {
func getFrame(_ g: Gizmo) -> NSRect {
  // CHECK: load i8*, i8** @"\01L_selector(frame)"
  // CHECK: call void bitcast (void ()* @objc_msgSend_stret to void ([[NSRECT]]*, [[OPAQUE0:.*]]*, i8*)*)([[NSRECT]]* noalias nocapture sret({{.*}}) {{.*}}, [[OPAQUE0:.*]]* {{.*}}, i8* {{.*}})
  return g.frame()
}
// CHECK: }

// CHECK: define hidden swiftcc void @"$s12objc_structs8setFrame{{[_0-9a-zA-Z]*}}F"(%TSo5GizmoC* %0, double %1, double %2, double %3, double %4) {{.*}} {
func setFrame(_ g: Gizmo, frame: NSRect) {
  // CHECK: load i8*, i8** @"\01L_selector(setFrame:)"
  // CHECK: call void bitcast (void ()* @objc_msgSend to void ([[OPAQUE0:.*]]*, i8*, [[NSRECT]]*)*)([[OPAQUE0:.*]]* {{.*}}, i8* {{.*}}, [[NSRECT]]* byval({{.*}}) align 8 {{.*}})
  g.setFrame(frame)
}
// CHECK: }

// CHECK: define hidden swiftcc { double, double, double, double } @"$s12objc_structs8makeRect{{[_0-9a-zA-Z]*}}F"(double %0, double %1, double %2, double %3)
func makeRect(_ a: Double, b: Double, c: Double, d: Double) -> NSRect {
  // CHECK: call void @NSMakeRect(%struct.NSRect* noalias nocapture sret({{.*}}) {{.*}}, double {{.*}}, double {{.*}}, double {{.*}}, double {{.*}})
  return NSMakeRect(a,b,c,d)
}
// CHECK: }

// CHECK: define hidden swiftcc [[stringLayout:[^@]*]] @"$s12objc_structs14stringFromRect{{[_0-9a-zA-Z]*}}F"(double %0, double %1, double %2, double %3) {{.*}} {
func stringFromRect(_ r: NSRect) -> String {
  // CHECK: call [[OPAQUE0:.*]]* @NSStringFromRect(%struct.NSRect* byval({{.*}}) align 8 {{.*}})
  return NSStringFromRect(r)
}
// CHECK: }

// CHECK: define hidden swiftcc { double, double, double, double } @"$s12objc_structs9insetRect{{[_0-9a-zA-Z]*}}F"(double %0, double %1, double %2, double %3, double %4, double %5)
func insetRect(_ r: NSRect, x: Double, y: Double) -> NSRect {
  // CHECK: call void @NSInsetRect(%struct.NSRect* noalias nocapture sret({{.*}}) {{.*}}, %struct.NSRect* byval({{.*}}) align 8 {{.*}}, double {{.*}}, double {{.*}})
  return NSInsetRect(r, x, y)
}
// CHECK: }

// CHECK: define hidden swiftcc { double, double, double, double } @"$s12objc_structs19convertRectFromBase{{[_0-9a-zA-Z]*}}F"([[NSVIEW]]* %0, double %1, double %2, double %3, double %4)
func convertRectFromBase(_ v: NSView, r: NSRect) -> NSRect {
  // CHECK: load i8*, i8** @"\01L_selector(convertRectFromBase:)", align 8
  // CHECK: call void bitcast (void ()* @objc_msgSend_stret to void ([[NSRECT]]*, [[OPAQUE0:.*]]*, i8*, [[NSRECT]]*)*)([[NSRECT]]* noalias nocapture sret({{.*}}) {{.*}}, [[OPAQUE0:.*]]* {{.*}}, i8* {{.*}}, [[NSRECT]]* byval({{.*}}) align 8 {{.*}})
  return v.convertRect(fromBase: r)
}
// CHECK: }

// CHECK: define hidden swiftcc { {{.*}}*, {{.*}}*, {{.*}}*, {{.*}}* } @"$s12objc_structs20useStructOfNSStringsySo0deF0VADF"({{.*}}* %0, {{.*}}* %1, {{.*}}* %2, {{.*}}* %3)
// CHECK:   call void @useStructOfNSStringsInObjC(%struct.StructOfNSStrings* noalias nocapture sret({{.*}}) {{%.*}}, %struct.StructOfNSStrings* byval({{.*}}) align 8 {{%.*}})
func useStructOfNSStrings(_ s: StructOfNSStrings) -> StructOfNSStrings {
  return useStructOfNSStringsInObjC(s)
}
