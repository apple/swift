// RUN: %swift %use_no_opaque_pointers -prespecialize-generic-metadata -target %module-target-future -emit-ir %s | %FileCheck %s -DINT=i%target-ptrsize -DALIGNMENT=%target-alignment
// RUN: %swift -prespecialize-generic-metadata -target %module-target-future -emit-ir %s

// REQUIRES: VENDOR=apple || OS=linux-gnu
// UNSUPPORTED: CPU=i386 && OS=ios
// UNSUPPORTED: CPU=armv7 && OS=ios
// UNSUPPORTED: CPU=armv7s && OS=ios

// CHECK: @"$s4main9NamespaceVAAq_RszrlE5ValueVyS2i_SSGMf" = linkonce_odr hidden constant <{ 
// CHECK-SAME:   i8**, 
// CHECK-SAME:   [[INT]], 
// CHECK-SAME:   %swift.type_descriptor*, 
// CHECK-SAME:   %swift.type*, 
// CHECK-SAME:   %swift.type*, 
// CHECK-SAME:   i32, 
// CHECK-SAME:   {{(\[4 x i8\],)?}}
// CHECK-SAME:   i64 
// CHECK-SAME: }> <{ 
//               i8** getelementptr inbounds (
//                 %swift.vwtable, 
//                 %swift.vwtable* @"$s4main9NamespaceVAAq_RszrlE5ValueVyS2i_SSGWV", 
//                 i32 0, 
//                 i32 0
//               ), 
// CHECK-SAME:   [[INT]] 512, 
// CHECK-SAME:   %swift.type_descriptor* bitcast ({{.*}}), 
// CHECK-SAME:   %swift.type* @"$sSiN", 
// CHECK-SAME:   %swift.type* @"$sSSN", 
// CHECK-SAME:   i32 0, 
// CHECK-SAME:   {{(\[4 x i8\] zeroinitializer,)?}}
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
// CHECK-SAME:     %swift.opaque* noalias nocapture %{{[0-9]+}}, 
// CHECK-SAME:     %swift.type* getelementptr inbounds (
// CHECK-SAME:       %swift.full_type, 
// CHECK-SAME:       %swift.full_type* bitcast (
// CHECK-SAME:         <{ 
// CHECK-SAME:           i8**, 
// CHECK-SAME:           [[INT]], 
// CHECK-SAME:           %swift.type_descriptor*, 
// CHECK-SAME:           %swift.type*, 
// CHECK-SAME:           i32,
// CHECK-SAME:           {{(\[4 x i8\],)?}} 
// CHECK-SAME:           i64 
// CHECK-SAME:         }>* @"$s4main9NamespaceVAAq_RszrlE5ValueVyS2i_SSGMf" 
// CHECK-SAME:         to %swift.full_type*
// CHECK-SAME:       ), 
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
// CHECK: define hidden swiftcc %swift.metadata_response @"$s4main9NamespaceVAAq_RszrlE5ValueVMa"([[INT]] %0, %swift.type* [[TYPE_1:%[0-9]+]], %swift.type* [[TYPE_2:%[0-9]+]]) #{{[0-9]+}} {{(section)?.*}}{
// CHECK: entry:
// CHECK:   [[ERASED_TYPE_1:%[0-9]+]] = bitcast %swift.type* [[TYPE_1]] to i8*
// CHECK:   [[ERASED_TYPE_2:%[0-9]+]] = bitcast %swift.type* [[TYPE_2]] to i8*
// CHECK:   {{%[0-9]+}} = call swiftcc %swift.metadata_response @__swift_instantiateCanonicalPrespecializedGenericMetadata(
// CHECK-SAME:     [[INT]] %0, 
// CHECK-SAME:     i8* [[ERASED_TYPE_1]], 
// CHECK-SAME:     i8* [[ERASED_TYPE_2]], 
// CHECK-SAME:     i8* undef, 
// CHECK-SAME:     %swift.type_descriptor* bitcast ({{[^)]*}})
// CHECK-SAME:   ) #{{[0-9]+}}
// CHECK:   ret %swift.metadata_response {{%[0-9]+}}
// CHECK: }
