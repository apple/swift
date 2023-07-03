// RUN: %empty-directory(%t)
// RUN: %build-irgen-test-overlays
// RUN: %target-swift-frontend -sdk %S/Inputs -I %t -primary-file %s -emit-ir -disable-objc-attr-requires-foundation-module | %FileCheck %s

// REQUIRES: CPU=x86_64
// REQUIRES: objc_interop

// CHECK-DAG: %swift.refcounted = type
// CHECK-DAG: [[INT:%TSi]] = type <{ i64 }>
// CHECK-DAG: [[DOUBLE:%TSd]] = type <{ double }>
// CHECK-DAG: [[NSRECT:%TSo6NSRectV]] = type <{ %TSo7NSPointV, %TSo6NSSizeV }>
// CHECK-DAG: [[NSPOINT:%TSo7NSPointV]] = type <{ %TSd, %TSd }>
// CHECK-DAG: [[NSSIZE:%TSo6NSSizeV]] = type <{ %TSd, %TSd }>

// CHECK: @"OBJC_METACLASS_$__TtC17objc_class_export3Foo" = hidden global %objc_class {
// CHECK-SAME:   ptr @"OBJC_METACLASS_$_{{(_TtCs12_)?}}SwiftObject",
// CHECK-SAME:   ptr @"OBJC_METACLASS_$_{{(_TtCs12_)?}}SwiftObject",
// CHECK-SAME:   ptr @_objc_empty_cache,
// CHECK-SAME:   ptr null,
// CHECK-SAME:   i64 ptrtoint (ptr @_METACLASS_DATA__TtC17objc_class_export3Foo to i64)
// CHECK-SAME: }
// CHECK: [[FOO_NAME:@.*]] = private unnamed_addr constant [28 x i8] c"_TtC17objc_class_export3Foo\00"
// CHECK: @_METACLASS_DATA__TtC17objc_class_export3Foo = internal constant { {{.*ptr}} } {
// CHECK-SAME:   i32 129,
// CHECK-SAME:   i32 40,
// CHECK-SAME:   i32 40,
// CHECK-SAME:   i32 0,
// CHECK-SAME:   ptr null,
// CHECK-SAME:   ptr  [[FOO_NAME]],
// CHECK-SAME:   @_CLASS_METHODS__TtC17objc_class_export3Foo,
// CHECK-SAME:   ptr null,
// CHECK-SAME:   ptr null,
// CHECK-SAME:   ptr null,
// CHECK-SAME:   ptr null
// CHECK-SAME: }, section "__DATA, {{.*}}", align 8
// CHECK: @_DATA__TtC17objc_class_export3Foo = internal constant { {{.*}}, ptr } {
// CHECK-SAME:   i32 128,
// CHECK-SAME:   i32 16,
// CHECK-SAME:   i32 24,
// CHECK-SAME:   i32 0,
// CHECK-SAME:   ptr null,
// CHECK-SAME:   ptr [[FOO_NAME]],
// CHECK-SAME:   ptr @_INSTANCE_METHODS__TtC17objc_class_export3Foo,
// CHECK-SAME:   ptr null,
// CHECK-SAME:   @_IVARS__TtC17objc_class_export3Foo,
// CHECK-SAME:   ptr null,
// CHECK-SAME:   _PROPERTIES__TtC17objc_class_export3Foo
// CHECK-SAME: }, section "__DATA, {{.*}}", align 8
// CHECK: @"$s17objc_class_export3FooCMf" = internal global <{{.*}} }> <{
// CHECK-SAME:   ptr @"$s17objc_class_export3FooCfD",
// CHECK-SAME:   ptr @"$sBOWV",
// CHECK-SAME:   i64 ptrtoint (ptr @"OBJC_METACLASS_$__TtC17objc_class_export3Foo" to i64),
// CHECK-SAME:   ptr @"OBJC_CLASS_$_{{(_TtCs12_)?}}SwiftObject",
// CHECK-SAME:   ptr @_objc_empty_cache,
// CHECK-SAME:   ptr null,
// CHECK-SAME:   i64 add (i64 ptrtoint (ptr @_DATA__TtC17objc_class_export3Foo to i64), i64 {{1|2}}),
// CHECK-SAME:   ptr @"$s17objc_class_export3FooC6createACyFZ",
// CHECK-SAME:   ptr @"$s17objc_class_export3FooC10drawInRect5dirtyySo6NSRectV_tF"
// CHECK-SAME: }>, section "__DATA,__objc_data, regular"
// -- TODO: The OBJC_CLASS symbol should reflect the qualified runtime name of
//    Foo.
// CHECK: @"$s17objc_class_export3FooCN" = hidden alias %swift.type, getelementptr inbounds ({{.*}} @"$s17objc_class_export3FooCMf", i32 0, i32 3)
// CHECK: @"OBJC_CLASS_$__TtC17objc_class_export3Foo" = hidden alias %swift.type, ptr @"$s17objc_class_export3FooCN"

import gizmo

class Hoozit {}

struct BigStructWithNativeObjects {
  var x, y, w : Double
  var h : Hoozit
}

@objc class Foo {
  @objc var x = 0
  @objc class func create() -> Foo {
    return Foo()
  }

  @objc func drawInRect(dirty dirty: NSRect) {
  }
  // CHECK: define internal void @"$s17objc_class_export3FooC10drawInRect5dirtyySo6NSRectV_tFTo"(ptr %0, ptr %1, ptr byval({{.*}}) align 8 %2) {{[#0-9]*}} {
  // CHECK:   call swiftcc void @"$s17objc_class_export3FooC10drawInRect5dirtyySo6NSRectV_tF"(double {{.*}}, double {{.*}}, double {{.*}}, double {{.*}}, ptr swiftself %0)
  // CHECK: }

  @objc func bounds() -> NSRect {
    return NSRect(origin: NSPoint(x: 0, y: 0), 
                  size: NSSize(width: 0, height: 0))
  }
  // CHECK: define internal void @"$s17objc_class_export3FooC6boundsSo6NSRectVyFTo"(ptr noalias nocapture sret({{.*}}) %0, ptr %1, ptr %2) {{[#0-9]*}} {
  // CHECK:   call swiftcc { double, double, double, double } @"$s17objc_class_export3FooC6boundsSo6NSRectVyF"(ptr swiftself %1)

  @objc func convertRectToBacking(r r: NSRect) -> NSRect {
    return r
  }
  // CHECK: define internal void @"$s17objc_class_export3FooC20convertRectToBacking1rSo6NSRectVAG_tFTo"(ptr noalias nocapture sret({{.*}}) %0, ptr %1, ptr %2, ptr byval({{.*}} align 8 %3) {{[#0-9]*}} {
  // CHECK:   call swiftcc { double, double, double, double } @"$s17objc_class_export3FooC20convertRectToBacking1rSo6NSRectVAG_tF"(double {{.*}}, double {{.*}}, double {{.*}}, double {{.*}}, ptr swiftself %1)

  func doStuffToSwiftSlice(f f: [Int]) {
  }
  // This function is not representable in Objective-C, so don't emit the objc entry point.
  // CHECK-NOT: @"$s17objc_class_export3FooC19doStuffToSwiftSlice1fySaySiG_tcAAFTo"

  func doStuffToBigSwiftStruct(f f: BigStructWithNativeObjects) {
  }
  // This function is not representable in Objective-C, so don't emit the objc entry point.
  // CHECK-NOT: @_TToFC17objc_class_export3Foo23doStuffToBigSwiftStruct1ffS_FTV17objc_class_export27BigStructWithNativeObjects_T_

  @objc init() { }
}

