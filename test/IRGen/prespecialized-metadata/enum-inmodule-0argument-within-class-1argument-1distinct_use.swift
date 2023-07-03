// RUN: %swift -prespecialize-generic-metadata -target %module-target-future -emit-ir %s | %FileCheck %s -DINT=i%target-ptrsize -DALIGNMENT=%target-alignment

// REQUIRES: VENDOR=apple || OS=linux-gnu
// UNSUPPORTED: CPU=i386 && OS=ios
// UNSUPPORTED: CPU=armv7 && OS=ios
// UNSUPPORTED: CPU=armv7s && OS=ios

// CHECK: @"$s4main9NamespaceC5ValueOySi_GMf" = linkonce_odr hidden constant <{
// CHECk-SAME:    ptr,
// CHECK-SAME:    [[INT]],
// CHECK-SAME:    ptr,
// CHECK-SAME:    ptr,
// CHECK-SAME: }> <{
//                ptr getelementptr inbounds (
//                  %swift.enum_vwtable, 
//                  ptr @"$s4main9NamespaceC5ValueOySi_GWV", 
//                  i32 0, 
//                  i32 0
//                ),
// CHECK-SAME:    [[INT]] 513,
// CHECK-SAME:    $s4main9NamespaceC5ValueOMn
// CHECK-SAME:    $sSiN
// CHECK-SAME:    i64 3
// CHECK-SAME: }>, align [[ALIGNMENT]]


final class Namespace<First> {
  enum Value {
    case first(First)
  }
}

@inline(never)
func consume<T>(_ t: T) {
  withExtendedLifetime(t) { t in
  }
}

// CHECK: define hidden swiftcc void @"$s4main4doityyF"() #{{[0-9]+}} {
// CHECK:   call swiftcc void @"$s4main7consumeyyxlF"(
// CHECK-SAME:     ptr noalias nocapture %{{[0-9]+}}, 
// CHECK-SAME:     ptr getelementptr inbounds (
// CHECK-SAME:       %swift.full_type, 
// CHECK-SAME:       $s4main9NamespaceC5ValueOySi_GMf
// CHECK-SAME:       i32 0, 
// CHECK-SAME:       i32 2
// CHECK-SAME:     )
// CHECK-SAME:   )
// CHECK: }
func doit() {
  consume( Namespace.Value.first(13) )
}
doit()

// CHECK: ; Function Attrs: noinline nounwind readnone
// CHECK: define hidden swiftcc %swift.metadata_response @"$s4main9NamespaceC5ValueOMa"([[INT]] %0, ptr %1) #{{[0-9]+}} {{(section)?.*}}{
// CHECK:      call swiftcc %swift.metadata_response @__swift_instantiateCanonicalPrespecializedGenericMetadata(
// CHECK-SAME:     [[INT]] %0, 
// CHECK-SAME:     ptr %1, 
// CHECK-SAME:     ptr undef, 
// CHECK-SAME:     ptr undef, 
// CHECK-SAME:     $s4main9NamespaceC5ValueOMn
// CHECK:   ret %swift.metadata_response {{%[0-9]+}}
// CHECK: }
