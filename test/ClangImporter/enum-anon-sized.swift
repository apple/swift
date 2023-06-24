// RUN: %target-swift-frontend -emit-ir %s -enable-objc-interop -import-objc-header %S/Inputs/enum-anon.h | %FileCheck -check-prefix=CHECK -check-prefix=CHECK-%target-runtime %s

func verifyIsInt(_: inout Int) { }
func verifyIsInt32(_: inout Int32) { }
func verifyIsUInt32(_: inout UInt32) { }

var a = Constant2
var b = VarConstant2

/// https://github.com/apple/swift/issues/45116
/// Anonymous enums used as ad-hoc types should not be naively imported
/// as `Int`.
var c = Struct().adhocAnonEnumField

verifyIsInt(&a)
#if !os(Windows)
verifyIsUInt32(&b)
verifyIsUInt32(&c)
#else
verifyIsInt32(&b)
verifyIsInt32(&c)
#endif

// CHECK: %TSo6StructV = type <{ %Ts5Int32V, [[ENUM_TYPE:%Ts5Int32V|%Ts6UInt32V]], %Ts5Int32V }>
// CHECK-LABEL: define{{.*}} i32 @"$s4main6testIR1xs5Int32VSPySo6StructVG_tF"(
public func testIR(x: UnsafePointer<Struct>) -> CInt {
  // CHECK: store i32 1, ptr @global, align 4
  global = VarConstant2

#if _runtime(_ObjC)
  // CHECK-objc: store i16 1, ptr @usGlobal, align 2
  usGlobal = USVarConstant2
#endif

  // Force the definition of the type above.
  // CHECK: ret
  return x.pointee.lastField
} // CHECK-NEXT: {{^}$}}

