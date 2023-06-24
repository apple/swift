// RUN: %target-swift-frontend -prespecialize-generic-metadata %s -target %module-target-future -emit-ir -parse-as-library | %FileCheck %s -DINT=i%target-ptrsize -DALIGNMENT=%target-alignment


// REQUIRES: VENDOR=apple || OS=linux-gnu
// REQUIRES: CPU=x86_64

// FIXME: Not a SIL test because we can't parse dynamic Self in SIL.
// <rdar://problem/16931299>

// CHECK: [[TYPE:%.+]] = type <{ [8 x i8] }>

@inline(never) func id<T>(_ t: T) -> T {
  return t
}
// CHECK-LABEL: define hidden swiftcc void @"$s28dynamic_self_metadata_future2idyxxlF"

protocol P {
  associatedtype T
}

extension P {
  func f() {}
}

struct G<T> : P {
  var t: T
}

class C {
  class func fromMetatype() -> Self? { return nil }
  // CHECK-LABEL: define hidden swiftcc i64 @"$s28dynamic_self_metadata_future1CC12fromMetatypeACXDSgyFZ"(ptr swiftself %0)
  // CHECK: ret i64 0

  func fromInstance() -> Self? { return nil }
  // CHECK-LABEL: define hidden swiftcc i64 @"$s28dynamic_self_metadata_future1CC12fromInstanceACXDSgyF"(ptr swiftself %0)
  // CHECK: ret i64 0

  func dynamicSelfArgument() -> Self? {
    return id(nil)
  }
  // CHECK-LABEL: define hidden swiftcc i64 @"$s28dynamic_self_metadata_future1CC0A12SelfArgumentACXDSgyF"(ptr swiftself %0)
  // CHECK: [[TYPE1:%.+]] = load {{.*}} %0
  // CHECK: [[T0:%.+]] = call swiftcc %swift.metadata_response @"$sSqMa"(i64 0, ptr [[TYPE1]])
  // CHECK: [[TYPE2:%.+]] = extractvalue %swift.metadata_response [[T0]], 0
  // CHECK: call swiftcc void @"$s28dynamic_self_metadata_future2idyxxlF"({{.*}}, ptr [[TYPE2]])

  func dynamicSelfConformingType() -> Self? {
    _ = G(t: self).f()
    return nil
  }
  // CHECK-LABEL: define hidden swiftcc i64 @"$s28dynamic_self_metadata_future1CC0A18SelfConformingTypeACXDSgyF"(ptr swiftself %0)
  // CHECK: [[SELF_TYPE:%.+]] = load {{.*}} %0
  // CHECK: call ptr @swift_getWitnessTable(
  // CHECK-SAME:   ptr  @"$s28dynamic_self_metadata_future1GVyxGAA1PAAMc"
  // CHECK-SAME:   ptr %{{[0-9]+}},
  // CHECK-SAME:   ptr undef
  // CHECK-SAME: )
}
