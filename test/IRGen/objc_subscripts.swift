// RUN: %target-swift-frontend -primary-file %s -emit-ir -enable-objc-interop -disable-objc-attr-requires-foundation-module | %FileCheck %s -check-prefix=CHECK -check-prefix=CHECK-%target-ptrauth

// REQUIRES: PTRSIZE=64

// CHECK: @_INSTANCE_METHODS__TtC15objc_subscripts10SomeObject = 
// CHECK:   internal constant { i32, i32, [5 x { ptr, ptr, ptr }] } 
// CHECK:   { i32 24, i32 5, [5 x { ptr, ptr, ptr }] 
// CHECK:     [
// CHECK:       { ptr, ptr, ptr } 
// CHECK:         { 
// CHECK:           ptr @"\01L_selector_data(objectAtIndexedSubscript:)", 
// CHECK:           ptr @".str.10.@24@0:8q16",
// CHECK-noptrauth: ptr @"$s15objc_subscripts10SomeObjectCyACSicigTo"
// CHECK-ptrauth:   ptr @"$s15objc_subscripts10SomeObjectCyACSicigTo.ptrauth"
// CHECK:         }, 
// CHECK:       { ptr, ptr, ptr } 
// CHECK:         { 
// CHECK:           ptr @"\01L_selector_data(setObject:atIndexedSubscript:)", 
// CHECK:           ptr @".str.13.v32@0:8@16q24",
// CHECK-noptrauth: ptr @"$s15objc_subscripts10SomeObjectCyACSicisTo"
// CHECK-ptrauth:   ptr @"$s15objc_subscripts10SomeObjectCyACSicisTo.ptrauth"
// CHECK:         },
// CHECK:       { ptr, ptr, ptr } 
// CHECK:         { 
// CHECK:           ptr @"\01L_selector_data(objectForKeyedSubscript:)", 
// CHECK:           ptr @".str.10.q24@0:8@16",
// CHECK-noptrauth: ptr @"$s15objc_subscripts10SomeObjectCySiACcigTo"
// CHECK-ptrauth:   ptr @"$s15objc_subscripts10SomeObjectCySiACcigTo.ptrauth"
// CHECK:         }, 
// CHECK:       { ptr, ptr, ptr } 
// CHECK:         { 
// CHECK:           ptr @"\01L_selector_data(setObject:forKeyedSubscript:)", 
// CHECK:           ptr @".str.13.v32@0:8q16@24",
// CHECK-noptrauth: ptr @"$s15objc_subscripts10SomeObjectCySiACcisTo"
// CHECK-ptrauth:   ptr @"$s15objc_subscripts10SomeObjectCySiACcisTo.ptrauth"
// CHECK:         },
// CHECK:       { ptr, ptr, ptr } 
// CHECK:         {
// CHECK:           ptr @"\01L_selector_data(init)",
// CHECK:           ptr @".str.7.@16@0:8",
// CHECK-noptrauth: ptr @"$s15objc_subscripts10SomeObjectCACycfcTo"
// CHECK-ptrauth:   ptr @"$s15objc_subscripts10SomeObjectCACycfcTo.ptrauth"
// CHECK:         }
// CHECK:    ]
// CHECK:  }

@objc class SomeObject {
  @objc subscript (i : Int) -> SomeObject {
    // CHECK-noptrauth: define internal ptr @"$s15objc_subscripts10SomeObjectCyACSicigTo"(ptr %0, ptr %1, i64 %2) {{[#0-9]*}} {
    // CHECK-ptrauth:   define internal ptr @"$s15objc_subscripts10SomeObjectCyACSicigTo"(ptr %0, ptr %1, i64 %2) {{[#0-9]*}} {
    get {
      // CHECK: call swiftcc ptr @"$s15objc_subscripts10SomeObjectCyACSicig"
      return self
    }

    // CHECK-LABEL: define internal void @"$s15objc_subscripts10SomeObjectCyACSicisTo"
    set {
      // CHECK: swiftcc void @"$s15objc_subscripts10SomeObjectCyACSicis"
    }
  }

  @objc subscript (s : SomeObject) -> Int {
  // CHECK-LABEL: define internal i64 @"$s15objc_subscripts10SomeObjectCySiACcigTo"
    get {
      // CHECK: call swiftcc i64 @"$s15objc_subscripts10SomeObjectCySiACcig"
      return 5
    }

    // CHECK-LABEL: define internal void @"$s15objc_subscripts10SomeObjectCySiACcisTo"
    set {
      // CHECK: call swiftcc void @"$s15objc_subscripts10SomeObjectCySiACcis"
    }
  }

  @objc init() {}
}

