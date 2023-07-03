// RUN: %empty-directory(%t)
// RUN: %build-irgen-test-overlays(mock-sdk-directory: %S/../Inputs)
// REQUIRES: VENDOR=apple || OS=linux-gnu
// RUN: %target-swift-frontend(mock-sdk: -sdk %S/../Inputs -I %t) -prespecialize-generic-metadata -target %module-target-future -primary-file %s -emit-ir | %FileCheck %s -DINT=i%target-ptrsize

// UNSUPPORTED: CPU=i386 && OS=ios
// UNSUPPORTED: CPU=armv7 && OS=ios
// UNSUPPORTED: CPU=armv7s && OS=ios

// REQUIRES: objc_interop

import Foundation

@inline(never)
func consume<T>(_ t: T) {
  withExtendedLifetime(t) { t in
  }
}

struct Value<T> {
  let value: T

  init(_ value: T) {
    self.value = value
  }
}

// CHECK: define hidden swiftcc void @"$s4main4doityyF"() #{{[0-9]+}} {
// CHECK:   [[TYPE:%[0-9]+]] = call ptr @__swift_instantiateConcreteTypeFromMangledName(ptr @"$s4main5ValueVySo12NSDictionaryCGMD")
// CHECK:      call swiftcc void @"$s4main7consumeyyxlF"(
// CHECK-SAME:   ptr noalias nocapture {{%.*}}, 
// CHECK-SAME:   ptr [[TYPE]])
// CHECK: }
func doit() {
  consume(Value(NSDictionary()))
}
doit()

// CHECK: ; Function Attrs: noinline nounwind readnone
// CHECK: define hidden swiftcc %swift.metadata_response @"$s4main5ValueVMa"([[INT]] %0, ptr %1) #{{[0-9]+}} {{(section)?.*}}{
// CHECK: entry:
// CHECK:   {{%[0-9]+}} = call swiftcc %swift.metadata_response @__swift_instantiateGenericMetadata(
// CHECK-SAME:   [[INT]] %0, 
// CHECK-SAME:   ptr %1, 
// CHECK-SAME:   ptr undef, 
// CHECK-SAME:   ptr undef, 
// CHECK-SAME:   $s4main5ValueVMn
// CHECK:   ret %swift.metadata_response {{%[0-9]+}}
// CHECK: }
