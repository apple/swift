// RUN: %empty-directory(%t)

// RUN: %target-build-swift -enable-library-evolution -emit-library -module-name TestModule -module-link-name TestModule %S/Inputs/struct-public-frozen-0argument.swift -emit-module-interface -swift-version 5 -o %t/%target-library-name(TestModule)
// RUN: %target-swift-frontend -prespecialize-generic-metadata -target %module-target-future -emit-ir -I %t -L %t %s | %FileCheck %s -DINT=i%target-ptrsize -DALIGNMENT=%target-alignment

// REQUIRES: VENDOR=apple || OS=linux-gnu
// UNSUPPORTED: CPU=i386 && OS=ios
// UNSUPPORTED: CPU=armv7 && OS=ios
// UNSUPPORTED: CPU=armv7s && OS=ios

import TestModule

// CHECK: @"$s4main5ValueOy10TestModule7IntegerVGMf" = linkonce_odr hidden constant <{ 
// CHECK-SAME:   ptr, 
// CHECK-SAME:   [[INT]], 
// CHECK-SAME:   ptr, 
// CHECK-SAME:   ptr, 
// CHECK-SAME:   i64 
// CHECK-SAME: }> 
// CHECK-SAME: <{ 
// CHECK-SAME:   $s4main5ValueOy10TestModule7IntegerVGWV
// CHECK-SAME:   [[INT]] 513, 
// CHECK-SAME:   $s4main5ValueOMn
// CHECK-SAME:   $s10TestModule7IntegerVN
// CHECK-SAME:   i64 3 
// CHECK-SAME: }>, align [[ALIGNMENT]]

@inline(never)
func consume<T>(_ t: T) {
  withExtendedLifetime(t) { t in
  }
}

enum Value<First> {
  case only(First)
}

// CHECK: define hidden swiftcc void @"$s4main4doityyF"() #{{[0-9]+}} {
// CHECK:   call swiftcc void @"$s4main7consumeyyxlF"(
// CHECK-SAME:   ptr noalias nocapture %{{[0-9]+}}, 
// CHECK-SAME:   ptr getelementptr inbounds (
// CHECK-SAME:     %swift.full_type, 
// CHECK-SAME:     $s4main5ValueOy10TestModule7IntegerVGMf
// CHECK-SAME:     i32 0, 
// CHECK-SAME:     i32 2
// CHECK-SAME:   )
// CHECK-SAME: )
// CHECK: }
func doit() {
  consume( Value.only(Integer(13)) )
}
doit()

// CHECK: ; Function Attrs: noinline nounwind readnone
// CHECK: define hidden swiftcc %swift.metadata_response @"$s4main5ValueOMa"([[INT]] %0, ptr %1) #{{[0-9]+}} {{(section)?.*}}{
// CHECK: entry:
// CHECK:   {{%[0-9]+}} = call swiftcc %swift.metadata_response @__swift_instantiateCanonicalPrespecializedGenericMetadata(
// CHECK-SAME:   [[INT]] %0, 
// CHECK-SAME:   ptr %1, 
// CHECK-SAME:   ptr undef, 
// CHECK-SAME:   ptr undef, 
// CHECK-SAME:   $s4main5ValueOMn
// CHECK:   ret %swift.metadata_response {{%[0-9]+}}
// CHECK: }
