// RUN: %swift -prespecialize-generic-metadata -target %module-target-future -emit-ir %s | %FileCheck %s -DINT=i%target-ptrsize -DALIGNMENT=%target-alignment

// REQUIRES: VENDOR=apple || OS=linux-gnu
// UNSUPPORTED: CPU=i386 && OS=ios
// UNSUPPORTED: CPU=armv7 && OS=ios
// UNSUPPORTED: CPU=armv7s && OS=ios

// CHECK: @"$s4main5Value[[UNIQUE_ID_1:[0-9A-Z_]+]]VySiGMf" = linkonce_odr hidden constant <{
// CHECK-SAME:    ptr,
// CHECK-SAME:    [[INT]],
// CHECK-SAME:    ptr,
// CHECK-SAME:    ptr,
// CHECK-SAME:    i32,
// CHECK-SAME:    i64
// CHECK-SAME: }> <{
//                ptr @"$sB[[INT]]_WV",
//                ptr {{[^@]*}}@"$s4main5Value33_BD97D79156BC586FAA2AE8FB32F3D812LLVySiGWV"{{[^,]*}},
// CHECK-SAME:    [[INT]] 512,
// CHECK-SAME:    $s4main5Value[[UNIQUE_ID_1:[0-9A-Z_]+]]VMn
// CHECK-SAME:    $sSiN
// CHECK-SAME:    i32 0,
// CHECK-SAME:    i64 3
// CHECK-SAME: }>, align [[ALIGNMENT]]

fileprivate struct Value<First> {
  let first: First
}

@inline(never)
func consume<T>(_ t: T) {
  withExtendedLifetime(t) { t in
  }
}

// CHECK: define hidden swiftcc void @"$s4main4doityyF"() #{{[0-9]+}} {
// CHECK:   call swiftcc void @"$s4main7consumeyyxlF"(
// CHECK-SAME:   ptr noalias nocapture %{{[0-9]+}}, 
// CHECK-SAME:   ptr getelementptr inbounds (
// CHECK-SAME:     %swift.full_type, 
// CHECK-SAME:     $s4main5Value[[UNIQUE_ID_1]]VySiGMf
// CHECK-SAME:     i32 0, 
// CHECK-SAME:     i32 2
// CHECK-SAME:   )
// CHECK-SAME: )
// CHECK: }
func doit() {
  consume( Value(first: 13) )
}
doit()

// CHECK: ; Function Attrs: noinline nounwind readnone
// CHECK: define internal swiftcc %swift.metadata_response @"$s4main5Value[[UNIQUE_ID_1]]VMa"([[INT]] %0, ptr %1) #{{[0-9]+}} {{(section)?.*}}{
// CHECK: entry:
// CHECK:   {{%[0-9]+}} = call swiftcc %swift.metadata_response @__swift_instantiateCanonicalPrespecializedGenericMetadata(
// CHECK-SAME:   [[INT]] %0, 
// CHECK-SAME:   ptr %1, 
// CHECK-SAME:   ptr undef, ptr undef, 
// CHECK-SAME:   $s4main5Value[[UNIQUE_ID_1]]VMn
// CHECK-SAME:   $s4main5Value[[UNIQUE_ID_1]]VMz
// CHECK:   ret %swift.metadata_response {{%[0-9]+}}
// CHECK: }
