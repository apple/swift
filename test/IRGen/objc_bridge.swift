// RUN: %empty-directory(%t)
// RUN: %build-irgen-test-overlays
// RUN: %target-swift-frontend(mock-sdk: -sdk %S/Inputs -I %t) -emit-ir -primary-file %s | %FileCheck %s -check-prefix=CHECK -check-prefix=CHECK-%target-ptrauth

// REQUIRES: PTRSIZE=64
// REQUIRES: objc_interop

import Foundation

// CHECK: @_INSTANCE_METHODS__TtC11objc_bridge3Bas = internal constant { i32, i32, [17 x { ptr, ptr, ptr }] } {
// CHECK:   i32 24,
// CHECK:   i32 17,
// CHECK:   [17 x { ptr, ptr, ptr }] [
// CHECK:     { ptr, ptr, ptr } {
// CHECK:       ptr @"\01L_selector_data(strRealProp)",
// CHECK:       ptr @".str.7.@16@0:8",
// CHECK-noptrauth: @"$s11objc_bridge3BasC11strRealPropSSvgTo"
// CHECK-ptrauth:   @"$s11objc_bridge3BasC11strRealPropSSvgTo.ptrauth"
// CHECK:     },
// CHECK:     { ptr, ptr, ptr } {
// CHECK:       ptr @"\01L_selector_data(setStrRealProp:)",
// CHECK:       ptr @".str.10.v24@0:8@16",
// CHECK-noptrauth: ptr @"$s11objc_bridge3BasC11strRealPropSSvsTo"
// CHECK-ptrauth:   ptr @"$s11objc_bridge3BasC11strRealPropSSvsTo.ptrauth"
// CHECK:     },
// CHECK:     { ptr, ptr, ptr } {
// CHECK:       ptr @"\01L_selector_data(strFakeProp)",
// CHECK:       ptr @".str.7.@16@0:8",
// CHECK-noptrauth: ptr @"$s11objc_bridge3BasC11strFakePropSSvgTo"
// CHECK-ptrauth:   ptr @"$s11objc_bridge3BasC11strFakePropSSvgTo.ptrauth"
// CHECK:     },
// CHECK:     { ptr, ptr, ptr } {
// CHECK:       ptr @"\01L_selector_data(setStrFakeProp:)",
// CHECK:       ptr @".str.10.v24@0:8@16",
// CHECK-noptrauth: ptr @"$s11objc_bridge3BasC11strFakePropSSvsTo"
// CHECK-ptrauth:   ptr @"$s11objc_bridge3BasC11strFakePropSSvsTo.ptrauth"
// CHECK:     },
// CHECK:     { ptr, ptr, ptr } {
// CHECK:       ptr @"\01L_selector_data(nsstrRealProp)",
// CHECK:       ptr @".str.7.@16@0:8",
// CHECK-noptrauth: ptr @"$s11objc_bridge3BasC13nsstrRealPropSo8NSStringCvgTo"
// CHECK-ptrauth:   ptr @"$s11objc_bridge3BasC13nsstrRealPropSo8NSStringCvgTo.ptrauth"
// CHECK:     },
// CHECK:     { ptr, ptr, ptr } {
// CHECK:       ptr @"\01L_selector_data(setNsstrRealProp:)",
// CHECK:       ptr @".str.10.v24@0:8@16",
// CHECK-noptrauth: ptr @"$s11objc_bridge3BasC13nsstrRealPropSo8NSStringCvsTo"
// CHECK-ptrauth:   ptr @"$s11objc_bridge3BasC13nsstrRealPropSo8NSStringCvsTo.ptrauth"
// CHECK:     },
// CHECK:     { ptr, ptr, ptr } {
// CHECK:       ptr @"\01L_selector_data(nsstrFakeProp)",
// CHECK:       ptr @".str.7.@16@0:8",
// CHECK-noptrauth: ptr @"$s11objc_bridge3BasC13nsstrFakePropSo8NSStringCvgTo"
// CHECK-ptrauth:   ptr @"$s11objc_bridge3BasC13nsstrFakePropSo8NSStringCvgTo.ptrauth"
// CHECK:     },
// CHECK:     { ptr, ptr, ptr } {
// CHECK:       ptr @"\01L_selector_data(setNsstrFakeProp:)",
// CHECK:       ptr @".str.10.v24@0:8@16",
// CHECK-noptrauth: ptr @"$s11objc_bridge3BasC13nsstrFakePropSo8NSStringCvsTo"
// CHECK-ptrauth:   ptr @"$s11objc_bridge3BasC13nsstrFakePropSo8NSStringCvsTo.ptrauth"
// CHECK:     },
// CHECK:     { ptr, ptr, ptr } {
// CHECK:       ptr @"\01L_selector_data(strResult)",
// CHECK:       ptr @".str.7.@16@0:8",
// CHECK-noptrauth: ptr @"$s11objc_bridge3BasC9strResultSSyFTo"
// CHECK-ptrauth:   ptr @"$s11objc_bridge3BasC9strResultSSyFTo.ptrauth"
// CHECK:     },
// CHECK:     { ptr, ptr, ptr } {
// CHECK:       ptr @"\01L_selector_data(strArgWithS:)"
// CHECK:       ptr @".str.10.v24@0:8@16",
// CHECK-noptrauth: ptr @"$s11objc_bridge3BasC6strArg1sySS_tFTo"
// CHECK-ptrauth:   ptr @"$s11objc_bridge3BasC6strArg1sySS_tFTo.ptrauth"
// CHECK:     },
// CHECK:     { ptr, ptr, ptr } {
// CHECK:       ptr @"\01L_selector_data(nsstrResult)",
// CHECK:       ptr @".str.7.@16@0:8",
// CHECK-noptrauth: ptr @"$s11objc_bridge3BasC11nsstrResultSo8NSStringCyFTo"
// CHECK-ptrauth:   ptr @"$s11objc_bridge3BasC11nsstrResultSo8NSStringCyFTo.ptrauth"
// CHECK:     },
// CHECK:     { ptr, ptr, ptr } {
// CHECK:       ptr @"\01L_selector_data(nsstrArgWithS:)",
// CHECK:       ptr @".str.10.v24@0:8@16",
// CHECK-noptrauth: ptr @"$s11objc_bridge3BasC8nsstrArg1sySo8NSStringC_tFTo"
// CHECK-ptrauth:   ptr @"$s11objc_bridge3BasC8nsstrArg1sySo8NSStringC_tFTo.ptrauth"
// CHECK:     },
// CHECK:     { ptr, ptr, ptr } { 
// CHECK:       ptr @"\01L_selector_data(init)", 
// CHECK:       ptr @".str.7.@16@0:8",
// CHECK-noptrauth: ptr @"$s11objc_bridge3BasCACycfcTo"
// CHECK-ptrauth:   ptr @"$s11objc_bridge3BasCACycfcTo.ptrauth"
// CHECK:     },
// CHECK:     { ptr, ptr, ptr } { 
// CHECK:       ptr @"\01L_selector_data(dealloc)", 
// CHECK:       ptr @".str.7.v16@0:8",
// CHECK-noptrauth: ptr @"$s11objc_bridge3BasCfDTo"
// CHECK-ptrauth:   ptr @"$s11objc_bridge3BasCfDTo.ptrauth"
// CHECK:     },
// CHECK:     { ptr, ptr, ptr } {
// CHECK:       ptr @"\01L_selector_data(acceptSet:)",
// CHECK:       ptr @".str.10.v24@0:8@16",
// CHECK-noptrauth: ptr @"$s11objc_bridge3BasC9acceptSetyyShyACGFTo"
// CHECK-ptrauth:   ptr @"$s11objc_bridge3BasC9acceptSetyyShyACGFTo.ptrauth"
// CHECK:     }
// CHECK:     { ptr, ptr, ptr } { 
// CHECK:       ptr @"\01L_selector_data(.cxx_destruct)", 
// CHECK:       ptr @"{{[^"]*}}",
// CHECK-noptrauth: ptr @"$s11objc_bridge3BasCfETo"
// CHECK-ptrauth:   ptr @"$s11objc_bridge3BasCfETo.ptrauth"
// CHECK:     }
// CHECK:   ]
// CHECK: }, section "__DATA, {{.*}}", align 8

// CHECK: @_PROPERTIES__TtC11objc_bridge3Bas = internal constant { i32, i32, [5 x { ptr, ptr }] } {

// CHECK: [[OBJC_BLOCK_PROPERTY:@.*]] = private unnamed_addr constant [8 x i8] c"T@?,N,C\00"
// CHECK: @_PROPERTIES__TtC11objc_bridge21OptionalBlockProperty = internal constant {{.*}} [[OBJC_BLOCK_PROPERTY]]

func getDescription(_ o: NSObject) -> String {
  return o.description
}

func getUppercaseString(_ s: NSString) -> String {
  return s.uppercase()
}

// @interface Foo -(void) setFoo: (NSString*)s; @end
func setFoo(_ f: Foo, s: String) {
  f.setFoo(s)
}

// NSString *bar(int);
func callBar() -> String {
  return bar(0)
}

// void setBar(NSString *s);
func callSetBar(_ s: String) {
  setBar(s)
}

var NSS : NSString = NSString()

// -- NSString methods don't convert 'self'
extension NSString {
  // CHECK: define internal ptr @"$sSo8NSStringC11objc_bridgeE13nsstrFakePropABvgTo"(ptr %0, ptr %1) {{[#0-9]*}} {
  // CHECK: define internal void @"$sSo8NSStringC11objc_bridgeE13nsstrFakePropABvsTo"(ptr %0, ptr %1, ptr %2) {{[#0-9]*}} {
  @objc var nsstrFakeProp : NSString {
    get {
      return NSS
    }
    set {}
  }

  // CHECK: define internal ptr @"$sSo8NSStringC11objc_bridgeE11nsstrResultAByFTo"(ptr %0, ptr %1) {{[#0-9]*}} {
  @objc func nsstrResult() -> NSString { return NSS }

  // CHECK: define internal void @"$sSo8NSStringC11objc_bridgeE8nsstrArg1syAB_tFTo"(ptr %0, ptr %1, ptr %2) {{[#0-9]*}} {
  @objc func nsstrArg(s s: NSString) { }
}

class Bas : NSObject {
  // CHECK: define internal ptr @"$s11objc_bridge3BasC11strRealPropSSvgTo"(ptr %0, ptr %1) {{[#0-9]*}} {
  // CHECK: define internal void @"$s11objc_bridge3BasC11strRealPropSSvsTo"(ptr %0, ptr %1, ptr %2) {{[#0-9]*}} {
  @objc var strRealProp : String

  // CHECK: define internal ptr @"$s11objc_bridge3BasC11strFakePropSSvgTo"(ptr %0, ptr %1) {{[#0-9]*}} {
  // CHECK: define internal void @"$s11objc_bridge3BasC11strFakePropSSvsTo"(ptr %0, ptr %1, ptr %2) {{[#0-9]*}} {
  @objc var strFakeProp : String {
    get {
      return ""
    }
    set {}
  }

  // CHECK: define internal ptr @"$s11objc_bridge3BasC13nsstrRealPropSo8NSStringCvgTo"(ptr %0, ptr %1) {{[#0-9]*}} {
  // CHECK: define internal void @"$s11objc_bridge3BasC13nsstrRealPropSo8NSStringCvsTo"(ptr %0, ptr %1, ptr %2) {{[#0-9]*}} {
  @objc var nsstrRealProp : NSString

  // CHECK: define hidden swiftcc ptr @"$s11objc_bridge3BasC13nsstrFakePropSo8NSStringCvg"(ptr swiftself %0) {{.*}} {
  // CHECK: define internal void @"$s11objc_bridge3BasC13nsstrFakePropSo8NSStringCvsTo"(ptr %0, ptr %1, ptr %2) {{[#0-9]*}} {
  @objc var nsstrFakeProp : NSString {
    get {
      return NSS
    }
    set {}
  }

  // CHECK: define internal ptr @"$s11objc_bridge3BasC9strResultSSyFTo"(ptr %0, ptr %1) {{[#0-9]*}} {
  @objc func strResult() -> String { return "" }
  // CHECK: define internal void @"$s11objc_bridge3BasC6strArg1sySS_tFTo"(ptr %0, ptr %1, ptr %2) {{[#0-9]*}} {
  @objc func strArg(s s: String) { }

  // CHECK: define internal ptr @"$s11objc_bridge3BasC11nsstrResultSo8NSStringCyFTo"(ptr %0, ptr %1) {{[#0-9]*}} {
  @objc func nsstrResult() -> NSString { return NSS }
  // CHECK: define internal void @"$s11objc_bridge3BasC8nsstrArg1sySo8NSStringC_tFTo"(ptr %0, ptr %1, ptr %2) {{[#0-9]*}} {
  @objc func nsstrArg(s s: NSString) { }

  override init() { 
    strRealProp = String()
    nsstrRealProp = NSString()
    super.init()
  }

  deinit { var x = 10 }

  override var hash: Int { return 0 }

  @objc func acceptSet(_ set: Set<Bas>) { }
}

func ==(lhs: Bas, rhs: Bas) -> Bool { return true }

class OptionalBlockProperty: NSObject {
  @objc var x: (([AnyObject]) -> [AnyObject])?
}
