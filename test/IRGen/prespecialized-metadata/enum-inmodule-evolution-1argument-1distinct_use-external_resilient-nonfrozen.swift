// RUN: %empty-directory(%t)

// RUN: %target-build-swift -enable-library-evolution -emit-library -module-name TestModule -module-link-name TestModule %S/Inputs/struct-public-nonfrozen-0argument.swift -emit-module-interface -swift-version 5 -o %t/%target-library-name(TestModule)
// RUN: %target-swift-frontend -prespecialize-generic-metadata -target %module-target-future -emit-ir -I %t -L %t %s | %FileCheck %s -DINT=i%target-ptrsize -DALIGNMENT=%target-alignment

// UNSUPPORTED: CPU=i386 && OS=ios
// UNSUPPORTED: CPU=armv7 && OS=ios
// UNSUPPORTED: CPU=armv7s && OS=ios

import TestModule

// CHECK-NOT: @"$s4main5ValueOy10TestModule7IntegerVGMf"

@inline(never)
func consume<T>(_ t: T) {
  withExtendedLifetime(t) { t in
  }
}

enum Value<First> {
  case only(First)
}

// CHECK: define hidden swiftcc void @"$s4main4doityyF"() #{{[0-9]+}} {
// CHECK:      [[DEMANGLED_TYPE:%[0-9]+]] = call ptr @__swift_instantiateConcreteTypeFromMangledName(
// CHECK-SAME:   $s4main5ValueOy10TestModule7IntegerVGMD
// CHECK:   call swiftcc void @"$s4main7consumeyyxlF"(
// CHECK-SAME:   ptr noalias nocapture %{{[0-9]+}}, 
// CHECK-SAME:   ptr [[DEMANGLED_TYPE]]
// CHECK-SAME: )
// CHECK: }
func doit() {
  consume( Value.only(Integer(13)) )
}
doit()

// CHECK: ; Function Attrs: noinline nounwind readnone
// CHECK: define hidden swiftcc %swift.metadata_response @"$s4main5ValueOMa"([[INT]] %0, ptr %1) #{{[0-9]+}} {{(section)?.*}}{
// CHECK:   {{%[0-9]+}} = call swiftcc %swift.metadata_response @__swift_instantiateGenericMetadata(
// CHECK-SAME:     [[INT]] %0, 
// CHECK-SAME:     ptr %1, 
// CHECK-SAME:     ptr undef, 
// CHECK-SAME:     ptr undef, 
// CHECK-SAME:     $s4main5ValueOMn
// CHECK-SAME:   )
// CHECK:   ret %swift.metadata_response {{%[0-9]+}}
// CHECK: }
