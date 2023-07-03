// RUN: %swift -prespecialize-generic-metadata -target %module-target-future -emit-ir %s | %FileCheck %s -DINT=i%target-ptrsize -DALIGNMENT=%target-alignment

// REQUIRES: VENDOR=apple || OS=linux-gnu
// UNSUPPORTED: CPU=i386 && OS=ios
// UNSUPPORTED: CPU=armv7 && OS=ios
// UNSUPPORTED: CPU=armv7s && OS=ios

// CHECK: @"$s4main5ValueVySSSdGWV" = linkonce_odr hidden constant %swift.vwtable {
// CHECK-SAME:    $s4main5ValueVwCP
// CHECK-SAME:    $s4main5ValueVwxx
// CHECK-SAME:    $s4main5ValueVwcp
// CHECK-SAME:    $s4main5ValueVwca
// CHECK-SAME:    $s4main5ValueVwtk
// CHECK-SAME:    $s4main5ValueVwta
// CHECK-SAME:    $s4main5ValueVwet
// CHECK-SAME:    $s4main5ValueVwst
// CHECK-SAME:    [[INT]] {{[0-9]+}},
// CHECK-SAME:    [[INT]] {{[0-9]+}},
// CHECK-SAME:    i32 {{[0-9]+}},
// CHECK-SAME:    i32 {{[0-9]+}}
// CHECK-SAME: },
// NOTE: ignore COMDAT on PE/COFF targets
// CHECK-SAME: align [[ALIGNMENT]]

// CHECK:      @"$s4main5ValueVySSSdGMf" = linkonce_odr hidden constant <{ 
// CHECK-SAME:   $s4main5ValueVMn
// CHECK-SAME:   $sSSN
// CHECK-SAME:   $sSdN
// CHECK-SAME:   i32 0, 
// CHECK-SAME:   i32 16, 
// CHECK-SAME:   i64 3 
// CHECK-SAME: }>, align [[ALIGNMENT]]

// CHECK: @"$s4main5ValueVySdSiGWV" = linkonce_odr hidden constant %swift.vwtable {
// CHECK-SAME:    $s4main5ValueVwCP
// CHECK-SAME:    $s4main5ValueVwxx
// CHECK-SAME:    $s4main5ValueVwcp
// CHECK-SAME:    $s4main5ValueVwca
// CHECK-SAME:    $s4main5ValueVwtk
// CHECK-SAME:    $s4main5ValueVwta
// CHECK-SAME:    $s4main5ValueVwet
// CHECK-SAME:    $s4main5ValueVwst
// CHECK-SAME:    [[INT]] {{[0-9]+}},
// CHECK-SAME:    [[INT]] {{[0-9]+}},
// CHECK-SAME:    i32 {{[0-9]+}},
// CHECK-SAME:    i32 {{[0-9]+}}
// CHECK-SAME: },
// NOTE: ignore COMDAT on PE/COFF targets
// CHECK-SAME: align [[ALIGNMENT]]

// CHECK:      @"$s4main5ValueVySdSiGMf" = linkonce_odr hidden constant <{
// CHECK-SAME:   $s4main5ValueVMn
// CHECK-SAME:   $sSdN
// CHECK-SAME:   $sSiN
// CHECK-SAME:   i32 0, 
// CHECK-SAME:   i32 8, 
// CHECK-SAME:   i64 3 
// CHECK-SAME: }>, align [[ALIGNMENT]]

// CHECK: @"$s4main5ValueVyS2iGWV" = linkonce_odr hidden constant %swift.vwtable {
// CHECK-SAME:    $s4main5ValueVwCP
// CHECK-SAME:    $s4main5ValueVwxx
// CHECK-SAME:    $s4main5ValueVwcp
// CHECK-SAME:    $s4main5ValueVwca
// CHECK-SAME:    $s4main5ValueVwtk
// CHECK-SAME:    $s4main5ValueVwta
// CHECK-SAME:    $s4main5ValueVwet
// CHECK-SAME:    $s4main5ValueVwst
// CHECK-SAME:    [[INT]] {{[0-9]+}},
// CHECK-SAMEL    [[INT]] {{[0-9]+}},
// CHECK-SAME:    i32 {{[0-9]+}},
// CHECK-SAME:    i32 {{[0-9]+}}
// CHECK-SAME: },
// NOTE: ignore COMDAT on PE/COFF targets
// CHECK-SAME: align [[ALIGNMENT]]

// CHECK:      @"$s4main5ValueVyS2iGMf" = linkonce_odr hidden constant <{
// CHECK-SAME:   $s4main5ValueVMn
// CHECK-SAME:   $sSiN
// CHECK-SAME:   $sSiN
// CHECK-SAME:   i32 0, 
// CHECK-SAME:   i32 [[ALIGNMENT]], 
// CHECK-SAME:   i64 3 
// CHECK-SAME: }>, align [[ALIGNMENT]]
struct Value<First, Second> {
  let first: First
  let second: Second
}

@inline(never)
func consume<T>(_ t: T) {
  withExtendedLifetime(t) { t in
  }
}

// CHECK: define hidden swiftcc void @"$s4main4doityyF"() #{{[0-9]+}} {
// CHECK:      call swiftcc void @"$s4main7consumeyyxlF"(
// CHECK-SAME:   ptr noalias nocapture %{{[0-9]+}}, 
// CHECK-SAME:   ptr getelementptr inbounds (
// CHECK-SAME:     %swift.full_type, 
// CHECK-SAME:     $s4main5ValueVyS2iGMf
// CHECK-SAME:     i32 0, 
// CHECK-SAME:     i32 2
// CHECK-SAME:   )
// CHECK-SAME: )
// CHECK:      call swiftcc void @"$s4main7consumeyyxlF"(
// CHECK-SAME:   ptr noalias nocapture %{{[0-9]+}}, 
// CHECK-SAME:   ptr getelementptr inbounds (
// CHECK-SAME:     %swift.full_type, 
// CHECK-SAME:     $s4main5ValueVySdSiGMf
// CHECK-SAME:     i32 0, 
// CHECK-SAME:     i32 2
// CHECK-SAME:   )
// CHECK-SAME: )
// CHECK:      call swiftcc void @"$s4main7consumeyyxlF"(
// CHECK-SAME:   ptr noalias nocapture %{{[0-9]+}}, 
// CHECK-SAME:   ptr getelementptr inbounds (
// CHECK-SAME:     %swift.full_type, 
// CHECK-SAME:     $s4main5ValueVySSSdGMf
// CHECK-SAME:     i32 0, 
// CHECK-SAME:     i32 2
// CHECK-SAME:   )
// CHECK-SAME: )
// CHECK: }
func doit() {
  consume( Value(first: 13, second: 13) )
  consume( Value(first: 13.0, second: 13) )
  consume( Value(first: "13.0", second: 13.0) )
}
doit()

// CHECK: ; Function Attrs: noinline nounwind readnone
// CHECK: define hidden swiftcc %swift.metadata_response @"$s4main5ValueVMa"([[INT]] %0, ptr %1, ptr %2) #{{[0-9]+}} {{(section)?.*}}{
// CHECK: entry:
// CHECK:      call swiftcc %swift.metadata_response @__swift_instantiateCanonicalPrespecializedGenericMetadata(
// CHECK-SAME:   [[INT]] %0, 
// CHECK-SAME:   ptr %1, 
// CHECK-SAME:   ptr %2, 
// CHECK-SAME:   ptr undef, 
// CHECK-SAME:   $s4main5ValueVMn
// CHECK-SAME:   $s4main5ValueVMz
// CHECK:   ret %swift.metadata_response {{%[0-9]+}}
// CHECK: }
