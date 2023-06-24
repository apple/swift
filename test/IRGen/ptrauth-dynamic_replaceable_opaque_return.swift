// RUN: %target-swift-frontend %use_no_opaque_pointers -disable-availability-checking -module-name A -swift-version 5 -primary-file %s -emit-ir | %FileCheck %s
// RUN: %target-swift-frontend -disable-availability-checking -module-name A -swift-version 5 -primary-file %s -emit-ir

// REQUIRES: CPU=arm64e

// CHECK: @"$s1A3baryQrSiFQOMk" = global %swift.dyn_repl_link_entry { {{.*}}@"$s1A3baryQrSiFQOMh.ptrauth" to i8*), %swift.dyn_repl_link_entry* null }
// CHECK: @"$s1A3baryQrSiFQOMh.ptrauth" = private constant { i8*, i32, i64, i64 } { {{.*}}@"$s1A3baryQrSiFQOMh" to i8*), i32 0, i64 ptrtoint (%swift.dyn_repl_link_entry* @"$s1A3baryQrSiFQOMk" to i64), i64 44678 }, section "llvm.ptrauth"
// CHECK: @"$s1A3baryQrSiFQOMj" = constant %swift.dyn_repl_key { {{.*}}%swift.dyn_repl_link_entry* @"$s1A3baryQrSiFQOMk"{{.*}}, i32 44678 }, section "__TEXT,__const"
// CHECK: @"$s1A16_replacement_bar1yQrSi_tFQOMk" = global %swift.dyn_repl_link_entry zeroinitializer
// CHECK: @"\01l_unnamed_dynamic_replacements" =
// CHECK:   private constant { i32, i32, [2 x { i32, i32, i32, i32 }] }
// CHECK:   { i32 0, i32 2, [2 x { i32, i32, i32, i32 }] [
// CHECK:     { i32, i32, i32, i32 } { {{.*}}%swift.dyn_repl_key* @"$s1A3baryQrSiFTx"{{.*}}@"$s1A16_replacement_bar1yQrSi_tF"{{.*}}%swift.dyn_repl_link_entry* @"$s1A16_replacement_bar1yQrSi_tFTX"{{.*}}, i32 0 },
// CHECK:     { i32, i32, i32, i32 } { {{.*}}%swift.dyn_repl_key* @"$s1A3baryQrSiFQOMj"{{.*}},{{.*}}@"$s1A16_replacement_bar1yQrSi_tFQOMg"{{.*}},{{.*}}@"$s1A16_replacement_bar1yQrSi_tFQOMk"{{.*}}, i32 0 }] }, section "__TEXT,__const", no_sanitize_address, align 8

public protocol P {
  func myValue() -> Int
}

extension Int: P {
  public func myValue() -> Int {
    return self
  }
}
// Opaque result type descriptor accessor for bar.
// CHECK-LABEL: define swiftcc %swift.type_descriptor* @"$s1A3baryQrSiFQOMg"()
// CHECK: entry:
// CHECK:   %0 = load i8*, i8** getelementptr inbounds (%swift.dyn_repl_link_entry, %swift.dyn_repl_link_entry* @"$s1A3baryQrSiFQOMk", i32 0, i32 0)
// CHECK:   %1 = bitcast i8* %0 to %swift.type_descriptor* ()*
// CHECK:   %2 = call i64 @llvm.ptrauth.blend(i64 ptrtoint (%swift.dyn_repl_link_entry* @"$s1A3baryQrSiFQOMk" to i64), i64 44678)
// CHECK:   %3 = tail call swiftcc %swift.type_descriptor* %1() [ "ptrauth"(i32 0, i64 %2) ]
// CHECK:   ret %swift.type_descriptor* %3
// CHECK: }

// Opaque result type descriptor accessor impl.
// CHECK-LABEL: define swiftcc %swift.type_descriptor* @"$s1A3baryQrSiFQOMh"()
// CHECK: entry:
// CHECK:   ret %swift.type_descriptor* bitcast ({{.*}}* @"$s1A3baryQrSiFQOMQ" to %swift.type_descriptor*)
// CHECK: }

public dynamic func bar(_ x: Int) -> some P {
  return x
}

struct Pair : P {
  var x = 0
  var y = 1
  func myValue() -> Int{
    return y
  }
}
// Opaque result type descriptor accessor for _replacement_bar.
// CHECK: define swiftcc %swift.type_descriptor* @"$s1A16_replacement_bar1yQrSi_tFQOMg"()
// CHECK: entry:
// CHECK:   ret %swift.type_descriptor* bitcast ({{.*}} @"$s1A16_replacement_bar1yQrSi_tFQOMQ" to %swift.type_descriptor*)
// CHECK: }
@_dynamicReplacement(for:bar(_:))
public func _replacement_bar(y x: Int) -> some P {
  return Pair()
}
