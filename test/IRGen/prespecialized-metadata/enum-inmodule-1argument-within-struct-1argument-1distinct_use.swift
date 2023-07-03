// RUN: %swift -prespecialize-generic-metadata -target %module-target-future -emit-ir %s | %FileCheck %s -DINT=i%target-ptrsize -DALIGNMENT=%target-alignment

// REQUIRES: VENDOR=apple || OS=linux-gnu
// UNSUPPORTED: CPU=i386 && OS=ios
// UNSUPPORTED: CPU=armv7 && OS=ios
// UNSUPPORTED: CPU=armv7s && OS=ios

// CHECK: @"$s4main9NamespaceV5ValueOySS_SiGMf" = linkonce_odr hidden constant <{
// CHECK-SAME:    ptr,
// CHECK-SAME:    [[INT]],
// CHECK-SAME:    ptr,
// CHECK-SAME:    ptr,
// CHECK-SAME:    ptr,
// CHECK-SAME:    i64
// CHECK-SAME: }> <{
//                ptr {{[^@]*}}@"$s4main9NamespaceV5ValueOySS_SiGWV"{{[^,]*}},
// CHECK-SAME:    [[INT]] 513,
// CHECK-SAME:    $s4main9NamespaceV5ValueOMn
// CHECK-SAME:    $sSSN
// CHECK-SAME:    $sSiN
// CHECK-SAME:    i64 3
// CHECK-SAME: }>, align [[ALIGNMENT]]

struct Namespace<Arg> {
  enum Value<First> {
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
// CHECK-SAME:       $s4main9NamespaceV5ValueOySS_SiGMf
// CHECK-SAME:       i32 0, 
// CHECK-SAME:       i32 2
// CHECK-SAME:     )
// CHECK-SAME:   )
// CHECK: }
func doit() {
  consume( Namespace<String>.Value.first(13) )
}
doit()

// CHECK: ; Function Attrs: noinline nounwind readnone
// CHECK: define hidden swiftcc %swift.metadata_response @"$s4main9NamespaceV5ValueOMa"([[INT]] %0, ptr %1, ptr %2) #{{[0-9]+}} {{(section)?.*}}{
// CHECK: entry:
// CHECK:   {{%[0-9]+}} = call swiftcc %swift.metadata_response @__swift_instantiateCanonicalPrespecializedGenericMetadata(
// CHECK-SAME:     [[INT]] %0, 
// CHECK-SAME:     ptr %1, 
// CHECK-SAME:     ptr %2, 
// CHECK-SAME:     ptr undef, 
// CHECK-SAME:     $s4main9NamespaceV5ValueOMn
// CHECK:   ret %swift.metadata_response {{%[0-9]+}}
// CHECK: }
