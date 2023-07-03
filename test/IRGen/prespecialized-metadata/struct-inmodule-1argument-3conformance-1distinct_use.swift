// RUN: %swift -prespecialize-generic-metadata -target %module-target-future -emit-ir %s | %FileCheck %s -DINT=i%target-ptrsize -DALIGNMENT=%target-alignment

// REQUIRES: VENDOR=apple || OS=linux-gnu
// UNSUPPORTED: CPU=i386 && OS=ios
// UNSUPPORTED: CPU=armv7 && OS=ios
// UNSUPPORTED: CPU=armv7s && OS=ios

// CHECK: @"$sytN" = external{{( dllimport)?}} global %swift.full_existential_type

// CHECK: @"$s4main5ValueVySiGMf" = linkonce_odr hidden constant <{
// CHECK-SAME:    ptr,
// CHECK-SAME:    [[INT]],
// CHECK-SAME:    ptr,
// CHECK-SAME:    ptr,
// CHECK-SAME:    ptr,
// CHECK-SAME:    ptr,
// CHECK-SAME:    ptr,
// CHECK-SAME:    i32{{(, \[4 x i8\])?}},
// CHECK-SAME:    i64
// CHECK-SAME: }> <{
//                ptr @"$sB[[INT]]_WV",
//                ptr {{[^@]*}}@"$s4main5ValueVySiGWV"{{[^,]*}},
// CHECK-SAME:    [[INT]] 512,
// CHECK-SAME:    $s4main5ValueVMn
// CHECK-SAME:    $sSiN
// CHECK-SAME:    $sSi4main1PAAWP
// CHECK-SAME:    $sSi4main1QAAWP
// CHECK-SAME:    $sSi4main1RAAWP
// CHECK-SAME:    i32 0{{(, \[4 x i8\] zeroinitializer)?}},
// CHECK-SAME:    i64 3
// CHECK-SAME: }>, align [[ALIGNMENT]]

protocol P {}
protocol Q {}
protocol R {}
extension Int : P {}
extension Int : Q {}
extension Int : R {}
struct Value<First : P & Q & R> {
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
// CHECK-SAME:     $s4main5ValueVySiGMf
// CHECK-SAME:     i32 0, 
// CHECK-SAME:     i32 2
// CHECK-SAME:   )
// CHECK-SAME: )
// CHECK: }
func doit() {
  consume( Value(first: 13) )
}
doit()

// CHECK: ; Function Attrs: noinline nounwind
// CHECK: define hidden swiftcc %swift.metadata_response @"$s4main5ValueVMa"([[INT]] %0, ptr %1) #{{[0-9]+}} {{(section)?.*}}{
// CHECK: entry:
// CHECK:   {{%[0-9]+}} = call swiftcc %swift.metadata_response @swift_getCanonicalPrespecializedGenericMetadata(
// CHECK-SAME:   [[INT]] %0, 
// CHECK-SAME:   ptr %1, 
// CHECK-SAME:   $s4main5ValueVMn
// CHECK-SAME:   $s4main5ValueVMz
// CHECK:   ret %swift.metadata_response {{%[0-9]+}}
// CHECK: }
