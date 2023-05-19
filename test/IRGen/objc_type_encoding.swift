// RUN: %empty-directory(%t)
// RUN: %build-irgen-test-overlays
// RUN: %target-swift-frontend(mock-sdk: -sdk %S/Inputs -I %t) %s -emit-ir -disable-objc-attr-requires-foundation-module -target %target-swift-abi-5.8-triple | %FileCheck %s -check-prefix=CHECK-%target-os

// REQUIRES: CPU=x86_64
// REQUIRES: objc_interop

// rdar://76863553
// UNSUPPORTED: OS=watchos && CPU=x86_64

import ObjectiveC
import Foundation
import gizmo

@objc protocol P1 {}
@objc protocol P2 {}
@objc protocol P3 {}

// CHECK-macosx: private unnamed_addr constant [8 x i8] c"#16@0:8\00"
// CHECK-ios: private unnamed_addr constant [8 x i8] c"#16@0:8\00"
// CHECK-tvos: private unnamed_addr constant [8 x i8] c"#16@0:8\00"
// CHECK-watchos: private unnamed_addr constant [8 x i8] c"#16@0:8\00"

@objc class Methods {
  @objc func testSizedUnsignedTypes(_ a: UInt8, b: UInt16, c: UInt32, d: UInt64) {}
// CHECK-macosx: private unnamed_addr constant [20 x i8] c"v36@0:8C16S20I24Q28\00"
// CHECK-ios: private unnamed_addr constant [20 x i8] c"v36@0:8C16S20I24Q28\00"
// CHECK-tvos: private unnamed_addr constant [20 x i8] c"v36@0:8C16S20I24Q28\00"
// CHECK-watchos: private unnamed_addr constant [20 x i8] c"v36@0:8C16S20I24Q28\00"

  @objc func testSizedFloats(_ a: Float32, b: Float64) {}
// CHECK-macosx: private unnamed_addr constant [14 x i8] c"v28@0:8f16d20\00"
// CHECK-ios: private unnamed_addr constant [14 x i8] c"v28@0:8f16d20\00"
// CHECK-tvos: private unnamed_addr constant [14 x i8] c"v28@0:8f16d20\00"
// CHECK-watchos: private unnamed_addr constant [14 x i8] c"v28@0:8f16d20\00"

  @objc func testParens(_ a: ((Int))) {}
// CHECK-macosx: private unnamed_addr constant [11 x i8] c"v24@0:8q16\00"
// CHECK-ios: private unnamed_addr constant [11 x i8] c"v24@0:8q16\00"
// CHECK-tvos: private unnamed_addr constant [11 x i8] c"v24@0:8q16\00"
// CHECK-watchos: private unnamed_addr constant [11 x i8] c"v24@0:8q16\00"

  @objc func testPrimitives(_ b: CBool, i: Int, f: Float, d: Double)
    -> OpaquePointer { fatalError() }
// CHECK-macosx: private unnamed_addr constant [21 x i8] c"^v40@0:8c16q20f28d32\00"
// CHECK-ios: private unnamed_addr constant [21 x i8] c"^v40@0:8B16q20f28d32\00"
// CHECK-tvos: private unnamed_addr constant [21 x i8] c"^v40@0:8B16q20f28d32\00"
// CHECK-watchos: private unnamed_addr constant [21 x i8] c"^v40@0:8B16q20f28d32\00"

  @objc func testOptionalPrimitives()
    -> OpaquePointer? { return nil }
// CHECK-macosx: private unnamed_addr constant [9 x i8] c"^v16@0:8\00"
// CHECK-ios: private unnamed_addr constant [9 x i8] c"^v16@0:8\00"
// CHECK-tvos: private unnamed_addr constant [9 x i8] c"^v16@0:8\00"
// CHECK-watchos: private unnamed_addr constant [9 x i8] c"^v16@0:8\00"

  @objc func testCSignedTypes(_ a: CSignedChar, b: CShort, c: CInt, d: CLong, e: CLongLong) {}
// CHECK-macosx: private unnamed_addr constant [23 x i8] c"v44@0:8c16s20i24q28q36\00"
// CHECK-ios: private unnamed_addr constant [23 x i8] c"v44@0:8c16s20i24q28q36\00"
// CHECK-tvos: private unnamed_addr constant [23 x i8] c"v44@0:8c16s20i24q28q36\00"
// CHECK-watchos: private unnamed_addr constant [23 x i8] c"v44@0:8c16s20i24q28q36\00"

  @objc func testCUnsignedTypes(_ a: CUnsignedChar, b: CUnsignedShort, c: CUnsignedInt, d: CUnsignedLong, e: CUnsignedLongLong) {}
// CHECK-macosx: private unnamed_addr constant [23 x i8] c"v44@0:8C16S20I24Q28Q36\00"
// CHECK-ios: private unnamed_addr constant [23 x i8] c"v44@0:8C16S20I24Q28Q36\00"
// CHECK-tvos: private unnamed_addr constant [23 x i8] c"v44@0:8C16S20I24Q28Q36\00"
// CHECK-watchos: private unnamed_addr constant [23 x i8] c"v44@0:8C16S20I24Q28Q36\00"

  @objc func testCChars(_ basic: CChar, wchar wide: CWideChar, char16: CChar16, char32: CChar32) {}
// CHECK-macosx: private unnamed_addr constant [20 x i8] c"v32@0:8c16i20S24i28\00"
// CHECK-ios: private unnamed_addr constant [20 x i8] c"v32@0:8c16i20S24i28\00"
// CHECK-tvos: private unnamed_addr constant [20 x i8] c"v32@0:8c16i20S24i28\00"
// CHECK-watchos: private unnamed_addr constant [20 x i8] c"v32@0:8c16i20S24i28\00"

  @objc func testCBool(_ a: CBool) {}
// CHECK-macosx: private unnamed_addr constant [11 x i8] c"v20@0:8c16\00"
// CHECK-ios: private unnamed_addr constant [11 x i8] c"v20@0:8B16\00"
// CHECK-tvos: private unnamed_addr constant [11 x i8] c"v20@0:8B16\00"
// CHECK-watchos: private unnamed_addr constant [11 x i8] c"v20@0:8B16\00"

  @objc func testSizedSignedTypes(_ a: Int8, b: Int16, c: Int32, d: Int64) {}
// CHECK-macosx: private unnamed_addr constant [20 x i8] c"v36@0:8c16s20i24q28\00"
// CHECK-ios: private unnamed_addr constant [20 x i8] c"v36@0:8c16s20i24q28\00"
// CHECK-tvos: private unnamed_addr constant [20 x i8] c"v36@0:8c16s20i24q28\00"
// CHECK-watchos: private unnamed_addr constant [20 x i8] c"v36@0:8c16s20i24q28\00"

  @objc class func getSelf() -> Methods.Type { return self }
// These strings are required for another purpose and so are tested above.

  @objc func getDynamicSelf() -> Self { return self }
// CHECK-macosx: private unnamed_addr constant [8 x i8] c"@16@0:8\00"
// CHECK-ios: private unnamed_addr constant [8 x i8] c"@16@0:8\00"
// CHECK-tvos: private unnamed_addr constant [8 x i8] c"@16@0:8\00"
// CHECK-watchos: private unnamed_addr constant [8 x i8] c"@16@0:8\00"

  @objc func testId(_ s: AnyObject) -> AnyObject { return self }
// CHECK-macosx: private unnamed_addr constant [11 x i8] c"@24@0:8@16\00"
// CHECK-ios: private unnamed_addr constant [11 x i8] c"@24@0:8@16\00"
// CHECK-tvos: private unnamed_addr constant [11 x i8] c"@24@0:8@16\00"
// CHECK-watchos: private unnamed_addr constant [11 x i8] c"@24@0:8@16\00"

  @objc func comp1(_ a: P1 & P2, b: P1 & P2 & P3) -> P1 & P2 { return a }
// CHECK-macosx: private unnamed_addr constant [14 x i8] c"@32@0:8@16@24\00"
// CHECK-ios: private unnamed_addr constant [14 x i8] c"@32@0:8@16@24\00"
// CHECK-tvos: private unnamed_addr constant [14 x i8] c"@32@0:8@16@24\00"
// CHECK-watchos: private unnamed_addr constant [14 x i8] c"@32@0:8@16@24\00"

  @objc func returnsBool(_ b : Bool) -> Bool { return b }
// CHECK-macosx: private unnamed_addr constant [11 x i8] c"c20@0:8c16\00"
// CHECK-ios: private unnamed_addr constant [11 x i8] c"B20@0:8B16\00"
// CHECK-tvos: private unnamed_addr constant [11 x i8] c"B20@0:8B16\00"
// CHECK-watchos: private unnamed_addr constant [11 x i8] c"B20@0:8B16\00"

  @objc func comp1(_ a: Methods, b: Methods, c: Methods) -> Methods { return a }
// CHECK-macosx: private unnamed_addr constant [17 x i8] c"@40@0:8@16@24@32\00"
// CHECK-ios: private unnamed_addr constant [17 x i8] c"@40@0:8@16@24@32\00"
// CHECK-tvos: private unnamed_addr constant [17 x i8] c"@40@0:8@16@24@32\00"
// CHECK-watchos: private unnamed_addr constant [17 x i8] c"@40@0:8@16@24@32\00"

  @objc func passSelector(_ aSelector : Selector) {}
// CHECK-macosx: private unnamed_addr constant [11 x i8] c"v24@0:8:16\00"
// CHECK-ios: private unnamed_addr constant [11 x i8] c"v24@0:8:16\00"
// CHECK-tvos: private unnamed_addr constant [11 x i8] c"v24@0:8:16\00"
// CHECK-watchos: private unnamed_addr constant [11 x i8] c"v24@0:8:16\00"

  @objc func copyUnsafeMutablePointer(_ p: UnsafeMutablePointer<Int32>) -> UnsafeMutablePointer<Int32> { return p }
// CHECK-macosx: private unnamed_addr constant [13 x i8] c"^i24@0:8^i16\00"
// CHECK-ios: private unnamed_addr constant [13 x i8] c"^i24@0:8^i16\00"
// CHECK-tvos: private unnamed_addr constant [13 x i8] c"^i24@0:8^i16\00"
// CHECK-watchos: private unnamed_addr constant [13 x i8] c"^i24@0:8^i16\00"

  @objc func copyUnsafeMutablePointerInt(_ p: UnsafeMutablePointer<Int>) -> UnsafeMutablePointer<Int> { return p }
// CHECK-macosx: private unnamed_addr constant [13 x i8] c"^q24@0:8^q16\00"
// CHECK-ios: private unnamed_addr constant [13 x i8] c"^q24@0:8^q16\00"
// CHECK-tvos: private unnamed_addr constant [13 x i8] c"^q24@0:8^q16\00"
// CHECK-watchos: private unnamed_addr constant [13 x i8] c"^q24@0:8^q16\00"

  @objc func testArchetype(_ work: P3) {
  }
// CHECK-macosx: private unnamed_addr constant [11 x i8] c"v24@0:8@16\00"
// CHECK-ios: private unnamed_addr constant [11 x i8] c"v24@0:8@16\00"
// CHECK-tvos: private unnamed_addr constant [11 x i8] c"v24@0:8@16\00"
// CHECK-watchos: private unnamed_addr constant [11 x i8] c"v24@0:8@16\00"

  @objc func foo(_ x: (Int) -> Int) -> Int {
    return 1
  }
// CHECK-macosx: private unnamed_addr constant [12 x i8] c"q24@0:8@?16\00"
// CHECK-ios: private unnamed_addr constant [12 x i8] c"q24@0:8@?16\00"
// CHECK-tvos: private unnamed_addr constant [12 x i8] c"q24@0:8@?16\00"
// CHECK-watchos: private unnamed_addr constant [12 x i8] c"q24@0:8@?16\00"

  @objc func returnNSRadixedOptions() -> NSRadixedOptions {
    return .octal
  }
// CHECK-macosx: [[ENUMENCODING:@.*]] = private unnamed_addr constant [8 x i8] c"i16@0:8\00"
// CHECK-ios: [[ENUMENCODING:@.*]] = private unnamed_addr constant [8 x i8] c"i16@0:8\00"
// CHECK-tvos: [[ENUMENCODING:@.*]] = private unnamed_addr constant [8 x i8] c"i16@0:8\00"
// CHECK-watchos: [[ENUMENCODING:@.*]] = private unnamed_addr constant [8 x i8] c"i16@0:8\00"

  @objc func returnChoseNSRadixedOptions(_ choice: NSRadixedOptions) -> NSRadixedOptions {
    switch choice {
      case .octal: return .hex
      case .hex: return .octal
    }
  }
// CHECK-macosx: private unnamed_addr constant [11 x i8] c"i20@0:8i16\00"
// CHECK-ios: private unnamed_addr constant [11 x i8] c"i20@0:8i16\00"
// CHECK-tvos: private unnamed_addr constant [11 x i8] c"i20@0:8i16\00"
// CHECK-watchos: private unnamed_addr constant [11 x i8] c"i20@0:8i16\00"

  @objc func getRawEnumInGizmo() -> RawEnumInGizmo {
    return InGizmoTwo
  }
// CHECK-macosx: { i8* getelementptr inbounds ([18 x i8], [18 x i8]* @"\01L_selector_data(getRawEnumInGizmo)", i64 0, i64 0),
// CHECK-macosx: i8* getelementptr inbounds ([8 x i8], [8 x i8]* [[ENUMENCODING]], i64 0, i64 0)
// CHECK-ios: { i8* getelementptr inbounds ([18 x i8], [18 x i8]* @"\01L_selector_data(getRawEnumInGizmo)", i64 0, i64 0),
// CHECK-ios: i8* getelementptr inbounds ([8 x i8], [8 x i8]* [[ENUMENCODING]], i64 0, i64 0)
// CHECK-tvos: { i8* getelementptr inbounds ([18 x i8], [18 x i8]* @"\01L_selector_data(getRawEnumInGizmo)", i64 0, i64 0),
// CHECK-tvos: i8* getelementptr inbounds ([8 x i8], [8 x i8]* [[ENUMENCODING]], i64 0, i64 0)
// CHECK-watchos: { i8* getelementptr inbounds ([18 x i8], [18 x i8]* @"\01L_selector_data(getRawEnumInGizmo)", i64 0, i64 0),
// CHECK-watchos: i8* getelementptr inbounds ([8 x i8], [8 x i8]* [[ENUMENCODING]], i64 0, i64 0)

}

// rdar://16308986
@objc class MyCustomObject {
}

@objc protocol MyProtocol {
	func myMethod2(_ arg : NSRuncing & NSFunging)
  func readWithAuthorization(_ authData : Gizmo, reply : (NSView, NSSpoon) -> Void)
  func doSomething(_ context: MyCustomObject)
  func subclassComposition(_: MyCustomObject & NSRuncing & NSFunging)
}


// CHECK-macosx: [[ENC1:@.*]] = private unnamed_addr constant [35 x i8] c"v24@0:8@\22<NSFunging><NSRuncing>\2216\00"
// CHECK-macosx: [[ENC2:@.*]] = private unnamed_addr constant [46 x i8] c"v32@0:8@\22Gizmo\2216@?<v@?@\22NSView\22@\22NSSpoon\22>24\00"
// CHECK-macosx: [[ENC3:@.*]] = private unnamed_addr constant [53 x i8] c"v24@0:8@\22_TtC18objc_type_encoding14MyCustomObject\2216\00"
// CHECK-macosx: [[ENC4:@.*]] = private unnamed_addr constant [75 x i8] c"v24@0:8@\22_TtC18objc_type_encoding14MyCustomObject<NSFunging><NSRuncing>\2216\00"
// CHECK-macosx: @_PROTOCOL_METHOD_TYPES__TtP18objc_type_encoding10MyProtocol_ = weak hidden constant [4 x i8*] [i8* getelementptr inbounds ([35 x i8], [35 x i8]* [[ENC1]], i64 0, i64 0), i8* getelementptr inbounds ([46 x i8], [46 x i8]* [[ENC2]], i64 0, i64 0), i8* getelementptr inbounds ([53 x i8], [53 x i8]* [[ENC3]], i64 0, i64 0), i8* getelementptr inbounds ([75 x i8], [75 x i8]* [[ENC4]], i64 0, i64 0)]
// CHECK-ios: [[ENC1:@.*]] = private unnamed_addr constant [35 x i8] c"v24@0:8@\22<NSFunging><NSRuncing>\2216\00"
// CHECK-ios: [[ENC2:@.*]] = private unnamed_addr constant [46 x i8] c"v32@0:8@\22Gizmo\2216@?<v@?@\22NSView\22@\22NSSpoon\22>24\00"
// CHECK-ios: [[ENC3:@.*]] = private unnamed_addr constant [53 x i8] c"v24@0:8@\22_TtC18objc_type_encoding14MyCustomObject\2216\00"
// CHECK-ios: [[ENC4:@.*]] = private unnamed_addr constant [75 x i8] c"v24@0:8@\22_TtC18objc_type_encoding14MyCustomObject<NSFunging><NSRuncing>\2216\00"
// CHECK-ios: @_PROTOCOL_METHOD_TYPES__TtP18objc_type_encoding10MyProtocol_ = weak hidden constant [4 x i8*] [i8* getelementptr inbounds ([35 x i8], [35 x i8]* [[ENC1]], i64 0, i64 0), i8* getelementptr inbounds ([46 x i8], [46 x i8]* [[ENC2]], i64 0, i64 0), i8* getelementptr inbounds ([53 x i8], [53 x i8]* [[ENC3]], i64 0, i64 0), i8* getelementptr inbounds ([75 x i8], [75 x i8]* [[ENC4]], i64 0, i64 0)]
// CHECK-tvos: [[ENC1:@.*]] = private unnamed_addr constant [35 x i8] c"v24@0:8@\22<NSFunging><NSRuncing>\2216\00"
// CHECK-tvos: [[ENC2:@.*]] = private unnamed_addr constant [46 x i8] c"v32@0:8@\22Gizmo\2216@?<v@?@\22NSView\22@\22NSSpoon\22>24\00"
// CHECK-tvos: [[ENC3:@.*]] = private unnamed_addr constant [53 x i8] c"v24@0:8@\22_TtC18objc_type_encoding14MyCustomObject\2216\00"
// CHECK-tvos: [[ENC4:@.*]] = private unnamed_addr constant [75 x i8] c"v24@0:8@\22_TtC18objc_type_encoding14MyCustomObject<NSFunging><NSRuncing>\2216\00"
// CHECK-tvos: @_PROTOCOL_METHOD_TYPES__TtP18objc_type_encoding10MyProtocol_ = weak hidden constant [4 x i8*] [i8* getelementptr inbounds ([35 x i8], [35 x i8]* [[ENC1]], i64 0, i64 0), i8* getelementptr inbounds ([46 x i8], [46 x i8]* [[ENC2]], i64 0, i64 0), i8* getelementptr inbounds ([53 x i8], [53 x i8]* [[ENC3]], i64 0, i64 0), i8* getelementptr inbounds ([75 x i8], [75 x i8]* [[ENC4]], i64 0, i64 0)]
// CHECK-watchos: [[ENC1:@.*]] = private unnamed_addr constant [35 x i8] c"v24@0:8@\22<NSFunging><NSRuncing>\2216\00"
// CHECK-watchos: [[ENC2:@.*]] = private unnamed_addr constant [46 x i8] c"v32@0:8@\22Gizmo\2216@?<v@?@\22NSView\22@\22NSSpoon\22>24\00"
// CHECK-watchos: [[ENC3:@.*]] = private unnamed_addr constant [53 x i8] c"v24@0:8@\22_TtC18objc_type_encoding14MyCustomObject\2216\00"
// CHECK-watchos: [[ENC4:@.*]] = private unnamed_addr constant [75 x i8] c"v24@0:8@\22_TtC18objc_type_encoding14MyCustomObject<NSFunging><NSRuncing>\2216\00"
// CHECK-watchos: @_PROTOCOL_METHOD_TYPES__TtP18objc_type_encoding10MyProtocol_ = weak hidden constant [4 x i8*] [i8* getelementptr inbounds ([35 x i8], [35 x i8]* [[ENC1]], i64 0, i64 0), i8* getelementptr inbounds ([46 x i8], [46 x i8]* [[ENC2]], i64 0, i64 0), i8* getelementptr inbounds ([53 x i8], [53 x i8]* [[ENC3]], i64 0, i64 0), i8* getelementptr inbounds ([75 x i8], [75 x i8]* [[ENC4]], i64 0, i64 0)]

class C: P {
  func stuff() {}
}

// CHECK-macosx: @OBJC_METH_VAR_NAME_ = private unnamed_addr constant [6 x i8] c"stuff\00"
// CHECK-macosx: @OBJC_METH_VAR_TYPE_ = private unnamed_addr constant [9 x i8] c"Vv16@0:8\00"
// CHECK-macosx: @"_OBJC_$_PROTOCOL_INSTANCE_METHODS_P" = {{.*}}@OBJC_METH_VAR_NAME_{{.*}}@OBJC_METH_VAR_TYPE_{{.*}}
