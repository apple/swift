// RUN: %target-swift-frontend -primary-file %s -emit-ir -enable-objc-interop -disable-objc-attr-requires-foundation-module | %FileCheck %s

// REQUIRES: CPU=x86_64

// FIXME: These should be SIL tests, but we can't parse generic types in SIL
// yet.

@objc protocol ObjC {
  func method()
}

// CHECK-LABEL: define hidden swiftcc void @"$s32sil_generic_witness_methods_objc05call_E7_method{{[_0-9a-zA-Z]*}}F"(ptr %0, ptr %T) {{.*}} {
// CHECK:         [[SEL:%.*]] = load ptr, ptr @"\01L_selector(method)", align 8
// CHECK:         call void @objc_msgSend(ptr %0, ptr [[SEL]])
func call_objc_method<T: ObjC>(_ x: T) {
  x.method()
}

// CHECK-LABEL: define hidden swiftcc void @"$s32sil_generic_witness_methods_objc05call_f1_E7_method{{[_0-9a-zA-Z]*}}F"(ptr %0, ptr %T) {{.*}} {
// CHECK:         call swiftcc void @"$s32sil_generic_witness_methods_objc05call_E7_method{{[_0-9a-zA-Z]*}}F"(ptr %0, ptr %T)
func call_call_objc_method<T: ObjC>(_ x: T) {
  call_objc_method(x)
}

// CHECK-LABEL: define hidden swiftcc void @"$s32sil_generic_witness_methods_objc05call_E19_existential_method{{[_0-9a-zA-Z]*}}F"(ptr %0) {{.*}} {
// CHECK:         [[SEL:%.*]] = load ptr, ptr @"\01L_selector(method)", align 8
// CHECK:         call void @objc_msgSend(ptr %0, ptr [[SEL]])
func call_objc_existential_method(_ x: ObjC) {
  x.method()
}
