// RUN: %empty-directory(%t)
// RUN: %build-irgen-test-overlays(mock-sdk-directory: %S/../Inputs)
// REQUIRES: VENDOR=apple || OS=linux-gnu
// RUN: %target-swift-frontend(mock-sdk: -sdk %S/../Inputs -I %t) %use_no_opaque_pointers -prespecialize-generic-metadata -target %module-target-future -primary-file %s -emit-ir | %FileCheck %s -DINT=i%target-ptrsize
// RUN: %target-swift-frontend(mock-sdk: -sdk %S/../Inputs -I %t) -prespecialize-generic-metadata -target %module-target-future -primary-file %s -emit-ir

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
// CHECK:   [[TYPE:%[0-9]+]] = call %swift.type* @__swift_instantiateConcreteTypeFromMangledName({ i32, i32 }* @"$s4main5ValueVySo12NSDictionaryCGMD") #{{[0-9]+}}
// CHECK:   call swiftcc void @"$s4main7consumeyyxlF"(%swift.opaque* noalias nocapture {{%.*}}, %swift.type* [[TYPE]])
// CHECK: }
func doit() {
  consume(Value(NSDictionary()))
}
doit()

// CHECK: ; Function Attrs: noinline nounwind readnone
// CHECK: define hidden swiftcc %swift.metadata_response @"$s4main5ValueVMa"([[INT]] %0, %swift.type* %1) #{{[0-9]+}} {{(section)?.*}}{
// CHECK: entry:
// CHECK:   [[ERASED_TYPE:%[0-9]+]] = bitcast %swift.type* %1 to i8*
// CHECK:   {{%[0-9]+}} = call swiftcc %swift.metadata_response @__swift_instantiateGenericMetadata([[INT]] %0, i8* [[ERASED_TYPE]], i8* undef, i8* undef, %swift.type_descriptor* bitcast ({{.+}}$s4main5ValueVMn{{.+}} to %swift.type_descriptor*)) #{{[0-9]+}}
// CHECK:   ret %swift.metadata_response {{%[0-9]+}}
// CHECK: }
