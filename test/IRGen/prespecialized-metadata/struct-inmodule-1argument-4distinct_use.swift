// RUN: %swift -prespecialize-generic-metadata -target %module-target-future -emit-ir %s | %FileCheck %s -DINT=i%target-ptrsize -DALIGNMENT=%target-alignment

// REQUIRES: VENDOR=apple || OS=linux-gnu
// UNSUPPORTED: CPU=i386 && OS=ios
// UNSUPPORTED: CPU=armv7 && OS=ios
// UNSUPPORTED: CPU=armv7s && OS=ios

// CHECK: @"$s4main5ValueVys5UInt8VGMf" = linkonce_odr hidden constant <{
// CHECK-SAME:    ptr,
// CHECK-SAME:    [[INT]],
// CHECK-SAME:    ptr,
// CHECK-SAME:    ptr,
// CHECK-SAME:    i32,
// CHECK-SAME:    i64
// CHECK-SAME: }> <{
//                ptr @"$sBi8_WV",
//                ptr {{[^@]*}}@"$s4main5ValueVys5UInt8VGWV"{{[^,]*}},
// CHECK-SAME:    [[INT]] 512,
// CHECK-SAME:    $s4main5ValueVMn
// CHECK-SAME:    $ss5UInt8VN
// CHECK-SAME:    i32 0,
// CHECK-SAME:    i64 3
// CHECK-SAME: }>, align [[ALIGNMENT]]

// CHECK:      @"$s4main5ValueVySSGMf" = linkonce_odr hidden constant <{
// CHECK-SAME:   $s4main5ValueVMn
// CHECK-SAME:   $sSSN
// CHECK-SAME:   i32 0, 
// CHECK-SAME:   i64 3 
// CHECK-SAME: }>, align [[ALIGNMENT]]

// CHECK: @"$s4main5ValueVySdGMf" = linkonce_odr hidden constant <{
// CHECK-SAME:    ptr,
// CHECK-SAME:    [[INT]],
// CHECK-SAME:    ptr,
// CHECK-SAME:    ptr,
// CHECK-SAME:    i32,
// CHECK-SAME:    i64
// CHECK-SAME: }> <{
//                ptr @"$sBi64_WV",
//                ptr {{[^@]*}}@"$s4main5ValueVySdGWV"{{[^,]*}},
// CHECK-SAME:    [[INT]] 512,
// CHECK-SAME:    $s4main5ValueVMn
// CHECK-SAME:    $sSdN
// CHECK-SAME:    i32 0,
// CHECK-SAME:    i64 3
// CHECK-SAME: }>, align [[ALIGNMENT]]

// CHECK: @"$s4main5ValueVySiGMf" = linkonce_odr hidden constant <{
// CHECK-SAME:    ptr,
// CHECK-SAME:    [[INT]],
// CHECK-SAME:    ptr,
// CHECK-SAME:    ptr,
// CHECK-SAME:    i32,
// CHECK-SAME:    i64
// CHECK-SAME: }> <{
//                ptr @"$sB[[INT]]_WV",
//                ptr {{[^@]*}}@"$s4main5ValueVySiGWV"{{[^,]*}},
// CHECk-SAME:    [[INT]] 512,
// CHECK-SAME:    $s4main5ValueVMn
// CHECK-SAME:    $sSiN
// CHECK-SAME:    i32 0,
// CHECK-SAME:    i64 3
// CHECK-SAME: }>, align [[ALIGNMENT]]
struct Value<First> {
  let first: First
}

@inline(never)
func consume<T>(_ t: T) {
  withExtendedLifetime(t) { t in
  }
}

// CHECK: define hidden swiftcc void @"$s4main4doityyF"() #{{[0-9]+}} {
// CHECK:    call swiftcc void @"$s4main7consumeyyxlF"(
// CHECK-SAME:   ptr noalias nocapture %{{[0-9]+}}, 
// CHECK-SAME:   ptr getelementptr inbounds (
// CHECK-SAME:     %swift.full_type, 
// CHECK-SAME:     $s4main5ValueVySiGMf
// CHECK-SAME:     i32 0, 
// CHECK-SAME:     i32 2
// CHECK-SAME:   )
// CHECK-SAME: )
// CHECK:    call swiftcc void @"$s4main7consumeyyxlF"(
// CHECK-SAME:   ptr noalias nocapture %{{[0-9]+}}, 
// CHECK-SAME:   ptr getelementptr inbounds (
// CHECK-SAME:     %swift.full_type, 
// CHECK-SAME:     $s4main5ValueVySdGMf
// CHECK-SAME:     i32 0, 
// CHECK-SAME:     i32 2
// CHECK-SAME:   )
// CHECK-SAME: )
// CHECK:    call swiftcc void @"$s4main7consumeyyxlF"(
// CHECK-SAME:   ptr noalias nocapture %{{[0-9]+}}, 
// CHECK-SAME:   ptr getelementptr inbounds (
// CHECK-SAME:     %swift.full_type, 
// CHECK-SAME:     $s4main5ValueVySSGMf
// CHECK-SAME:     i32 0, 
// CHECK-SAME:     i32 2
// CHECK-SAME:   )
// CHECK-SAME: )
// CHECK:    call swiftcc void @"$s4main7consumeyyxlF"(
// CHECK-SAME:   ptr noalias nocapture %{{[0-9]+}}, 
// CHECK-SAME:   ptr getelementptr inbounds (
// CHECK-SAME:     %swift.full_type, 
// CHECK-SAME:     $s4main5ValueVys5UInt8VGMf
// CHECK-SAME:     i32 0, 
// CHECK-SAME:     i32 2
// CHECK-SAME:   )
// CHECK-SAME: )
// CHECK: }
func doit() {
  consume( Value(first: 13) )
  consume( Value(first: 13.0) )
  consume( Value(first: "13") )
  consume( Value(first: 13 as UInt8) )
}
doit()

// CHECK: ; Function Attrs: noinline nounwind readnone
// CHECK: define hidden swiftcc %swift.metadata_response @"$s4main5ValueVMa"([[INT]] %0, ptr %1) #{{[0-9]+}} {{(section)?.*}}{
// CHECK: entry:
// CHECK:   {{%[0-9]+}} = call swiftcc %swift.metadata_response @__swift_instantiateCanonicalPrespecializedGenericMetadata(
// CHECK-SAME:   [[INT]] %0, 
// CHECK-SAME:   ptr %1, 
// CHECK-SAME:   ptr undef, 
// CHECK-SAME:   ptr undef, 
// CHECK-SAME:   ptr {{.+}}$s4main5ValueVMn{{.+}}, 
// CHECK-SAME:   $s4main5ValueVMz
// CHECK:   ret %swift.metadata_response {{%[0-9]+}}
// CHECK: }
