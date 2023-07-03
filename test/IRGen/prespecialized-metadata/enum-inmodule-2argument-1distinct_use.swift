// RUN: %swift -prespecialize-generic-metadata -target %module-target-future -emit-ir %s | %FileCheck %s -DINT=i%target-ptrsize -DALIGNMENT=%target-alignment

// REQUIRES: VENDOR=apple || OS=linux-gnu
// UNSUPPORTED: CPU=i386 && OS=ios
// UNSUPPORTED: CPU=armv7 && OS=ios
// UNSUPPORTED: CPU=armv7s && OS=ios

// CHECK: @"$s4main5ValueOyS2iGWV" = linkonce_odr hidden constant %swift.enum_vwtable {
// CHECK-SAME:   $s4main5ValueOwCP
// CHECK-SAME:   $s4main5ValueOwxx
// CHECK-SAME:   $s4main5ValueOwcp
// CHECK-SAME:   $s4main5ValueOwca
// CHECK-SAME:   $s4main5ValueOwtk
// CHECK-SAME:   $s4main5ValueOwta
// CHECK-SAME:   swift_getMultiPayloadEnumTagSinglePayload
// CHECK-SAME:   swift_storeMultiPayloadEnumTagSinglePayload
// CHECK-SAME:   [[INT]] {{[0-9]+}},
// CHECK-SAME:   [[INT]] {{[0-9]+}},
// CHECK-SAME:   i32 {{[0-9]+}},
// CHECK-SAME:   i32 {{[0-9]+}},
// CHECK-SAME:   $s4main5ValueOwug
// CHECK-SAME:   $s4main5ValueOwup
// CHECK-SAME    ptr {{[^@]*}}@"$s4main5ValueOwui{{[^}]+}}
// CHECK-SAME: }, align [[ALIGNMENT]]
// CHECK: @"$s4main5ValueOyS2iGMf" = linkonce_odr hidden constant <{
// CHECK-SAME:   ptr,
// CHECK-SAME:   [[INT]],
// CHECK-SAME:   ptr,
// CHECK-SAME:   ptr,
// CHECK-SAME:   i64
// CHECK-SAME:   }> <{
// CHECK-SAME:   $s4main5ValueOyS2iGWV
// CHECK-SAME:   [[INT]] 513,
// CHECK-SAME:   $s4main5ValueOMn
// CHECK-SAME:   $sSiN
// CHECK-SAME:   $sSiN
// CHECK-SAME:   [[INT]] {{16|8}},
// CHECK-SAME:   i64 3
// CHECK-SAME: }>, align [[ALIGNMENT]]
enum Value<First, Second> {
  case first(First)
  case second(First, Second)
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
// CHECK-SAME:     $s4main5ValueOyS2iGMf{{[^,]}}
// CHECK-SAME:       i32 0, 
// CHECK-SAME:       i32 2
// CHECK-SAME:     )
// CHECK-SAME: )
// CHECK: }
func doit() {
  consume( Value.second(13, 13) )
}
doit()

// CHECK: ; Function Attrs: noinline nounwind readnone
// CHECK: define hidden swiftcc %swift.metadata_response @"$s4main5ValueOMa"([[INT]] %0, ptr %1, ptr %2) #{{[0-9]+}} {{(section)?.*}}{
// CHECK: entry:
// CHECK:   {{%[0-9]+}} = call swiftcc %swift.metadata_response @__swift_instantiateCanonicalPrespecializedGenericMetadata(
// CHECK-SAME:     [[INT]] %0, 
// CHECK-SAME:     ptr %1, 
// CHECK-SAME:     ptr %2, 
// CHECK-SAME:     ptr undef, 
// CHECK-SAME:     $s4main5ValueOMn{{[^,]}}
// CHECK-SAME:   )
// CHECK:   ret %swift.metadata_response {{%[0-9]+}}
// CHECK: }
