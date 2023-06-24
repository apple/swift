// RUN: %target-swift-frontend -disable-generic-metadata-prespecialization %s -emit-ir -parse-as-library | %FileCheck %s

// UNSUPPORTED: OS=windows-msvc
// REQUIRES: CPU=x86_64

// FIXME: Not a SIL test because we can't parse dynamic Self in SIL.
// <rdar://problem/16931299>

// CHECK: [[TYPE:%.+]] = type <{ [8 x i8] }>

@inline(never) func id<T>(_ t: T) -> T {
  return t
}
// CHECK-LABEL: define hidden swiftcc void @"$s21dynamic_self_metadata2idyxxlF"

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
  // CHECK-LABEL: define hidden swiftcc i64 @"$s21dynamic_self_metadata1CC12fromMetatypeACXDSgyFZ"(ptr swiftself %0)
  // CHECK: ret i64 0

  func fromInstance() -> Self? { return nil }
  // CHECK-LABEL: define hidden swiftcc i64 @"$s21dynamic_self_metadata1CC12fromInstanceACXDSgyF"(ptr swiftself %0)
  // CHECK: ret i64 0

  func dynamicSelfArgument() -> Self? {
    return id(nil)
  }
  // CHECK-LABEL: define hidden swiftcc i64 @"$s21dynamic_self_metadata1CC0A12SelfArgumentACXDSgyF"(ptr swiftself %0)
  // CHECK: [[TYPE1:%.+]] = load {{.*}} %0
  // CHECK: [[T0:%.+]] = call swiftcc %swift.metadata_response @"$sSqMa"(i64 0, ptr [[TYPE1]])
  // CHECK: [[TYPE2:%.+]] = extractvalue %swift.metadata_response [[T0]], 0
  // CHECK: call swiftcc void @"$s21dynamic_self_metadata2idyxxlF"({{.*}}, ptr [[TYPE2]])

  func dynamicSelfConformingType() -> Self? {
    _ = G(t: self).f()
    return nil
  }
  // CHECK-LABEL: define hidden swiftcc i64 @"$s21dynamic_self_metadata1CC0A18SelfConformingTypeACXDSgyF"(ptr swiftself %0)
  // CHECK: [[SELF_TYPE:%.+]] = load {{.*}} %0
  // CHECK: [[METADATA_RESPONSE:%.*]] = call swiftcc %swift.metadata_response @"$s21dynamic_self_metadata1GVMa"(i64 0, ptr [[SELF_TYPE]])
  // CHECK: [[METADATA:%.*]] =  extractvalue %swift.metadata_response [[METADATA_RESPONSE]], 0
  // CHECK: call ptr @swift_getWitnessTable(ptr @"$s21dynamic_self_metadata1GVyxGAA1PAAMc", ptr [[METADATA]], ptr undef)
}
