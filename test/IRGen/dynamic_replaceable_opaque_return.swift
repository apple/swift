// RUN: %target-swift-frontend -disable-availability-checking -module-name A -swift-version 5 -primary-file %s -emit-ir | %FileCheck %s

// The arm64e test is in ptrauth-dynamic_replaceable.sil.
// UNSUPPORTED: CPU=arm64e
// REQUIRES: objc_interop

// No 32bit for now.
// UNSUPPORTED: CPU=armv7
// UNSUPPORTED: CPU=armv7s
// UNSUPPORTED: CPU=i386

// CHECK: @"$s1A3baryQrSiFQOMk" = global %swift.dyn_repl_link_entry { ptr @"$s1A3baryQrSiFQOMh", ptr null }
// CHECK: @"$s1A3baryQrSiFQOMj" = constant %swift.dyn_repl_key { {{.*}}ptr @"$s1A3baryQrSiFQOMk"{{.*}}, i32 0 }, section "__TEXT,__const"
// CHECK: @"$s1A16_replacement_bar1yQrSi_tFQOMk" = global %swift.dyn_repl_link_entry zeroinitializer
// CHECK: @"\01l_unnamed_dynamic_replacements" =
// CHECK-SAME:   private constant { i32, i32, [4 x { i32, i32, i32, i32 }] }
// CHECK-SAME:   { i32 0, i32 4, [4 x { i32, i32, i32, i32 }] [
// CHECK-SAME:     { i32, i32, i32, i32 } { {{.*}}ptr @"$s1A3baryQrSiFTx"{{.*}}@"$s1A16_replacement_bar1yQrSi_tF"{{.*}}ptr @"$s1A16_replacement_bar1yQrSi_tFTX"{{.*}}, i32 0 }, { i32, i32, i32, i32 } { {{.*}}ptr @"$s1A9ContainerV4propQrvgTx"{{.*}}@"$s1A9ContainerV7_r_propQrvg"{{.*}}, i32 0 }, { i32, i32, i32, i32 } { {{.*}}ptr @"$s1A3baryQrSiFQOMj"{{.*}},{{.*}}@"$s1A16_replacement_bar1yQrSi_tFQOMg"{{.*}},{{.*}}@"$s1A16_replacement_bar1yQrSi_tFQOMk"{{.*}}, i32 0 }, { i32, i32, i32, i32 } { {{.*}}ptr @"$s1A9ContainerV4propQrvpQOMj"{{.*}},{{.*}}@"$s1A9ContainerV7_r_propQrvpQOMg"{{.*}},{{.*}}@"$s1A9ContainerV7_r_propQrvpQOMk"{{.*}}, i32 0 }] }
// CHECK: , section "__TEXT,__const"

public protocol P {
  func myValue() -> Int
}

extension Int: P {
  public func myValue() -> Int {
    return self
  }
}
// Opaque result type descriptor accessor for bar.
// CHECK-LABEL: define{{.*}} swiftcc ptr @"$s1A3baryQrSiFQOMg"()
// CHECK: entry:
// CHECK:   %0 = load ptr, ptr @"$s1A3baryQrSiFQOMk"
// CHECK:   %1 = tail call swiftcc ptr %0()
// CHECK:   ret ptr %1
// CHECK: }

// Opaque result type descriptor accessor impl.
// CHECK-LABEL: define{{.*}} swiftcc ptr @"$s1A3baryQrSiFQOMh"()
// CHECK: entry:
// CHECK:   ret ptr @"$s1A3baryQrSiFQOMQ"
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
// CHECK: define{{.*}} swiftcc ptr @"$s1A16_replacement_bar1yQrSi_tFQOMg"()
// CHECK: entry:
// CHECK:   ret ptr @"$s1A16_replacement_bar1yQrSi_tFQOMQ"
// CHECK: }
@_dynamicReplacement(for:bar(_:))
public func _replacement_bar(y x: Int) -> some P {
  return Pair()
}

struct Container {
  dynamic var prop : some P {
    get {
      return 0
    }
  }
}

// CHECK: define{{.*}} hidden swiftcc ptr @"$s1A9ContainerV4propQrvpQOMg"()
// CHECK: entry:
// CHECK:   %0 = load ptr, ptr @"$s1A9ContainerV4propQrvpQOMk"
// CHECK:   %1 = tail call swiftcc ptr %0()
// CHECK:   ret ptr %1

// CHECK: define{{.*}} hidden swiftcc ptr @"$s1A9ContainerV4propQrvpQOMh"()
// CHECK: entry:
// CHECK:   ret ptr @"$s1A9ContainerV4propQrvpQOMQ"

extension Container {
  @_dynamicReplacement(for: prop)
  var _r_prop : some P {
    get {
      return 1
    }
  }
}

// CHECK: define{{.*}} hidden swiftcc ptr @"$s1A9ContainerV7_r_propQrvpQOMg"()
// CHECK: entry:
// CHECK:  ret ptr @"$s1A9ContainerV7_r_propQrvpQOMQ"


// CHECK-NOT: s1A16noOpaqueAccessor{{.*}}Mg
public func noOpaqueAccessor() -> some P {
  return 0
}
