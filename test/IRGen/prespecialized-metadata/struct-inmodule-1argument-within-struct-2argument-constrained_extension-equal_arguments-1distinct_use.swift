// RUN: %swift -prespecialize-generic-metadata -target %module-target-future -emit-ir %s | %FileCheck %s -DINT=i%target-ptrsize -DALIGNMENT=%target-alignment

// REQUIRES: VENDOR=apple || OS=linux-gnu
// UNSUPPORTED: CPU=i386 && OS=ios
// UNSUPPORTED: CPU=armv7 && OS=ios
// UNSUPPORTED: CPU=armv7s && OS=ios

// CHECK: @"$s4main9NamespaceVAAq_RszrlE5ValueVyS2i_SSGMf" = linkonce_odr hidden constant <{ 
// CHECK-SAME:   ptr, 
// CHECK-SAME:   [[INT]], 
// CHECK-SAME:   ptr, 
// CHECK-SAME:   ptr, 
// CHECK-SAME:   ptr, 
// CHECK-SAME:   i32, 
// CHECK-SAME:   i64 
// CHECK-SAME: }> <{ 
//               ptr getelementptr inbounds (
//                 %swift.vwtable, 
//                 ptr @"$s4main9NamespaceVAAq_RszrlE5ValueVyS2i_SSGWV", 
//                 i32 0, 
//                 i32 0
//               ), 
// CHECK-SAME:   [[INT]] 512, 
// CHECK-SAME:   ptr {{[^,]*}}, 
// CHECK-SAME:   $sSiN
// CHECK-SAME:   $sSSN
// CHECK-SAME:   i32 0, 
// CHECK-SAME:   i64 3 
// CHECK-SAME: }>, 
// CHECK-SAME: align [[ALIGNMENT]]

struct Namespace<First, Second> {
}
extension Namespace where First == Second {
  struct Value<Third> {
    let value: (First, Third)
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
// CHECK-SAME:       $s4main9NamespaceVAAq_RszrlE5ValueVyS2i_SSGMf
// CHECK-SAME:       i32 0, 
// CHECK-SAME:       i32 2
// CHECK-SAME:     )
// CHECK-SAME:   )
// CHECK: }
func doit() {
  consume( Namespace.Value(value: (1, "two")) )
}
doit()

// CHECK: ; Function Attrs: noinline nounwind readnone
// CHECK: define hidden swiftcc %swift.metadata_response @"$s4main9NamespaceVAAq_RszrlE5ValueVMa"([[INT]] %0, ptr [[TYPE_1:%[0-9]+]], ptr [[TYPE_2:%[0-9]+]]) #{{[0-9]+}} {{(section)?.*}}{
// CHECK: entry:
// CHECK:      call swiftcc %swift.metadata_response @__swift_instantiateCanonicalPrespecializedGenericMetadata(
// CHECK-SAME:   [[INT]] %0, 
// CHECK-SAME:   ptr [[TYPE_1]], 
// CHECK-SAME:   ptr [[TYPE_2]], 
// CHECK-SAME:   ptr undef, 
// CHECK-SAME:   ptr {{[^)]*}}
// CHECK-SAME: )
// CHECK:   ret %swift.metadata_response {{%[0-9]+}}
// CHECK: }
