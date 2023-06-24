// RUN: %empty-directory(%t)
// RUN: %build-irgen-test-overlays
// RUN: %target-swift-frontend %use_no_opaque_pointers -sdk %S/Inputs -I %t -primary-file %s -emit-ir -disable-objc-attr-requires-foundation-module | %FileCheck %s
// RUN: %target-swift-frontend -sdk %S/Inputs -I %t -primary-file %s -emit-ir -disable-objc-attr-requires-foundation-module

// REQUIRES: CPU=x86_64
// REQUIRES: objc_interop

// CHECK-DAG: %swift.refcounted = type
// CHECK-DAG: [[HOOZIT:%T17objc_class_export6HoozitC]] = type <{ [[REF:%swift.refcounted]] }>
// CHECK-DAG: [[FOO:%T17objc_class_export3FooC]] = type <{ [[REF]], %TSi }>
// CHECK-DAG: [[INT:%TSi]] = type <{ i64 }>
// CHECK-DAG: [[DOUBLE:%TSd]] = type <{ double }>
// CHECK-DAG: [[NSRECT:%TSo6NSRectV]] = type <{ %TSo7NSPointV, %TSo6NSSizeV }>
// CHECK-DAG: [[NSPOINT:%TSo7NSPointV]] = type <{ %TSd, %TSd }>
// CHECK-DAG: [[NSSIZE:%TSo6NSSizeV]] = type <{ %TSd, %TSd }>

// CHECK: @"OBJC_METACLASS_$__TtC17objc_class_export3Foo" = hidden global %objc_class {
// CHECK-SAME:   %objc_class* @"OBJC_METACLASS_$_{{(_TtCs12_)?}}SwiftObject",
// CHECK-SAME:   %objc_class* @"OBJC_METACLASS_$_{{(_TtCs12_)?}}SwiftObject",
// CHECK-SAME:   %swift.opaque* @_objc_empty_cache,
// CHECK-SAME:   %swift.opaque* null,
// CHECK-SAME:   i64 ptrtoint ({{.*}}* @_METACLASS_DATA__TtC17objc_class_export3Foo to i64)
// CHECK-SAME: }
// CHECK: [[FOO_NAME:@.*]] = private unnamed_addr constant [28 x i8] c"_TtC17objc_class_export3Foo\00"
// CHECK: @_METACLASS_DATA__TtC17objc_class_export3Foo = internal constant {{.*\*}} } {
// CHECK-SAME:   i32 129,
// CHECK-SAME:   i32 40,
// CHECK-SAME:   i32 40,
// CHECK-SAME:   i32 0,
// CHECK-SAME:   i8* null,
// CHECK-SAME:   i8* getelementptr inbounds ([{{[0-9]+}} x i8], [{{[0-9]+}} x i8]* [[FOO_NAME]], i64 0, i64 0),
// CHECK-SAME:   @_CLASS_METHODS__TtC17objc_class_export3Foo,
// CHECK-SAME:   i8* null,
// CHECK-SAME:   i8* null,
// CHECK-SAME:   i8* null,
// CHECK-SAME:   i8* null
// CHECK-SAME: }, section "__DATA, {{.*}}", align 8
// CHECK: @_DATA__TtC17objc_class_export3Foo = internal constant {{.*\*}} } {
// CHECK-SAME:   i32 128,
// CHECK-SAME:   i32 16,
// CHECK-SAME:   i32 24,
// CHECK-SAME:   i32 0,
// CHECK-SAME:   i8* null,
// CHECK-SAME:   i8* getelementptr inbounds ([{{[0-9]+}} x i8], [{{[0-9]+}} x i8]* [[FOO_NAME]], i64 0, i64 0),
// CHECK-SAME:   { i32, i32, [6 x { i8*, i8*, i8* }] }* @_INSTANCE_METHODS__TtC17objc_class_export3Foo,
// CHECK-SAME:   i8* null,
// CHECK-SAME:   @_IVARS__TtC17objc_class_export3Foo,
// CHECK-SAME:   i8* null,
// CHECK-SAME:   _PROPERTIES__TtC17objc_class_export3Foo
// CHECK-SAME: }, section "__DATA, {{.*}}", align 8
// CHECK: @"$s17objc_class_export3FooCMf" = internal global <{{.*}} }> <{
// CHECK-SAME:   void ([[FOO]]*)* @"$s17objc_class_export3FooCfD",
// CHECK-SAME:   i8** @"$sBOWV",
// CHECK-SAME:   i64 ptrtoint (%objc_class* @"OBJC_METACLASS_$__TtC17objc_class_export3Foo" to i64),
// CHECK-SAME:   %objc_class* @"OBJC_CLASS_$_{{(_TtCs12_)?}}SwiftObject",
// CHECK-SAME:   %swift.opaque* @_objc_empty_cache,
// CHECK-SAME:   %swift.opaque* null,
// CHECK-SAME:   i64 add (i64 ptrtoint ({{.*}}* @_DATA__TtC17objc_class_export3Foo to i64), i64 {{1|2}}),
// CHECK-SAME:   [[FOO]]* (%swift.type*)* @"$s17objc_class_export3FooC6createACyFZ",
// CHECK-SAME:   void (double, double, double, double, [[FOO]]*)* @"$s17objc_class_export3FooC10drawInRect5dirtyySo6NSRectV_tF"
// CHECK-SAME: }>, section "__DATA,__objc_data, regular"
// -- TODO: The OBJC_CLASS symbol should reflect the qualified runtime name of
//    Foo.
// CHECK: @"$s17objc_class_export3FooCN" = hidden alias %swift.type, bitcast (i64* getelementptr inbounds ({{.*}} @"$s17objc_class_export3FooCMf", i32 0, i32 3) to %swift.type*)
// CHECK: @"OBJC_CLASS_$__TtC17objc_class_export3Foo" = hidden alias %swift.type, %swift.type* @"$s17objc_class_export3FooCN"

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
  // CHECK: define internal void @"$s17objc_class_export3FooC10drawInRect5dirtyySo6NSRectV_tFTo"([[OPAQUE:%.*]]* %0, i8* %1, [[NSRECT]]* byval({{.*}}) align 8 %2) {{[#0-9]*}} {
  // CHECK:   [[CAST:%[a-zA-Z0-9]+]] = bitcast [[OPAQUE]]* %0 to [[FOO]]*
  // CHECK:   call swiftcc void @"$s17objc_class_export3FooC10drawInRect5dirtyySo6NSRectV_tF"(double {{.*}}, double {{.*}}, double {{.*}}, double {{.*}}, [[FOO]]* swiftself [[CAST]])
  // CHECK: }

  @objc func bounds() -> NSRect {
    return NSRect(origin: NSPoint(x: 0, y: 0), 
                  size: NSSize(width: 0, height: 0))
  }
  // CHECK: define internal void @"$s17objc_class_export3FooC6boundsSo6NSRectVyFTo"([[NSRECT]]* noalias nocapture sret({{.*}}) %0, [[OPAQUE4:%.*]]* %1, i8* %2) {{[#0-9]*}} {
  // CHECK:   [[CAST:%[a-zA-Z0-9]+]] = bitcast [[OPAQUE4]]* %1 to [[FOO]]*
  // CHECK:   call swiftcc { double, double, double, double } @"$s17objc_class_export3FooC6boundsSo6NSRectVyF"([[FOO]]* swiftself [[CAST]])

  @objc func convertRectToBacking(r r: NSRect) -> NSRect {
    return r
  }
  // CHECK: define internal void @"$s17objc_class_export3FooC20convertRectToBacking1rSo6NSRectVAG_tFTo"([[NSRECT]]* noalias nocapture sret({{.*}}) %0, [[OPAQUE5:%.*]]* %1, i8* %2, [[NSRECT]]* byval({{.*}} align 8 %3) {{[#0-9]*}} {
  // CHECK:   [[CAST:%[a-zA-Z0-9]+]] = bitcast [[OPAQUE5]]* %1 to [[FOO]]*
  // CHECK:   call swiftcc { double, double, double, double } @"$s17objc_class_export3FooC20convertRectToBacking1rSo6NSRectVAG_tF"(double {{.*}}, double {{.*}}, double {{.*}}, double {{.*}}, [[FOO]]* swiftself [[CAST]])

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

