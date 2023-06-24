// RUN: %target-swift-frontend %use_no_opaque_pointers -primary-file %s -emit-ir -enable-objc-interop -disable-objc-attr-requires-foundation-module | %FileCheck %s
// RUN: %target-swift-frontend -primary-file %s -emit-ir -enable-objc-interop -disable-objc-attr-requires-foundation-module

// REQUIRES: CPU=x86_64

// FIXME: These should be SIL tests, but we can't parse generic types in SIL
// yet.

@objc protocol ObjC {
  func method()
}

// CHECK-LABEL: define hidden swiftcc void @"$s32sil_generic_witness_methods_objc05call_E7_method{{[_0-9a-zA-Z]*}}F"(%objc_object* %0, %swift.type* %T) {{.*}} {
// CHECK:         [[SEL:%.*]] = load i8*, i8** @"\01L_selector(method)", align 8
// CHECK:         [[CAST:%.*]] = bitcast %objc_object* %0 to [[SELFTYPE:%?.*]]*
// CHECK:         call void bitcast (void ()* @objc_msgSend to void ([[SELFTYPE]]*, i8*)*)([[SELFTYPE]]* [[CAST]], i8* [[SEL]])
func call_objc_method<T: ObjC>(_ x: T) {
  x.method()
}

// CHECK-LABEL: define hidden swiftcc void @"$s32sil_generic_witness_methods_objc05call_f1_E7_method{{[_0-9a-zA-Z]*}}F"(%objc_object* %0, %swift.type* %T) {{.*}} {
// CHECK:         call swiftcc void @"$s32sil_generic_witness_methods_objc05call_E7_method{{[_0-9a-zA-Z]*}}F"(%objc_object* %0, %swift.type* %T)
func call_call_objc_method<T: ObjC>(_ x: T) {
  call_objc_method(x)
}

// CHECK-LABEL: define hidden swiftcc void @"$s32sil_generic_witness_methods_objc05call_E19_existential_method{{[_0-9a-zA-Z]*}}F"(%objc_object* %0) {{.*}} {
// CHECK:         [[SEL:%.*]] = load i8*, i8** @"\01L_selector(method)", align 8
// CHECK:         [[CAST:%.*]] = bitcast %objc_object* %0 to [[SELFTYPE:%?.*]]*
// CHECK:         call void bitcast (void ()* @objc_msgSend to void ([[SELFTYPE]]*, i8*)*)([[SELFTYPE]]* [[CAST]], i8* [[SEL]])
func call_objc_existential_method(_ x: ObjC) {
  x.method()
}
