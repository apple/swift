// RUN: %swift %use_no_opaque_pointers -prespecialize-generic-metadata -target %module-target-future -emit-ir %s | %FileCheck %s -DINT=i%target-ptrsize -DALIGNMENT=%target-alignment
// RUN: %swift -prespecialize-generic-metadata -target %module-target-future -emit-ir %s

// REQUIRES: VENDOR=apple || OS=linux-gnu
// UNSUPPORTED: CPU=i386 && OS=ios
// UNSUPPORTED: CPU=armv7 && OS=ios
// UNSUPPORTED: CPU=armv7s && OS=ios

// CHECK: @"$s4main5ValueOySiGWV" = linkonce_odr hidden constant %swift.enum_vwtable { 
// CHECK-SAME:   i8* bitcast ({{[^@]+}}@"$s4main5ValueOwCP{{[^\)]*}} to i8*), 
// CHECK-SAME:   i8* bitcast ({{[^@]+}}@"$s4main5ValueOwxx{{[^)]*}} to i8*), 
// CHECK-SAME:   i8* bitcast ({{[^@]+}}@"$s4main5ValueOwcp{{[^)]*}} to i8*), 
// CHECK-SAME:   i8* bitcast ({{[^@]+}}@"$s4main5ValueOwca{{[^)]*}} to i8*), 
// CHECK-SAME:   i8* bitcast ({{[^@]+}}@"$s4main5ValueOwtk{{[^)]*}} to i8*), 
// CHECK-SAME:   i8* bitcast ({{[^@]+}}@"$s4main5ValueOwta{{[^)]*}} to i8*), 
// CHECK-SAME:   i8* bitcast ({{[^@]+}}@"$s4main5ValueOwet{{[^)]+}} to i8*), 
// CHECK-SAME:   i8* bitcast ({{[^@]+}}@"$s4main5ValueOwst{{[^)]+}} to i8*), 
// CHECK-SAME:   [[INT]] [[ALIGNMENT]], 
// CHECK-SAME:   [[INT]] [[ALIGNMENT]], 
// CHECK-SAME:   i32 {{[0-9]+}}, 
// CHECK-SAME:   i32 0, 
// CHECK-SAME:   i8* bitcast ({{[^@]+}}@"$s4main5ValueOwug{{[^)]+}} to i8*), 
// CHECK-SAME:   i8* bitcast ({{[^@]+}}@"$s4main5ValueOwup{{[^)]+}} to i8*), 
// CHECK-SAME:   i8* bitcast ({{[^@]+}}@"$s4main5ValueOwui{{[^)]+}} to i8*) 
// CHECK-SAME: }, align [[ALIGNMENT]]
// CHECK: @"$s4main5ValueOySiGMf" = linkonce_odr hidden constant <{ 
// CHECK-SAME:   i8**, 
// CHECK-SAME:   [[INT]], 
// CHECK-SAME:   %swift.type_descriptor*, 
// CHECK-SAME:   %swift.type*, 
// CHECK-SAME:   i64 
// CHECK-SAME: }> <{ 
// CHECK-SAME: i8** getelementptr inbounds (%swift.enum_vwtable, %swift.enum_vwtable* @"$s4main5ValueOySiGWV", i32 0, i32 0), 
// CHECK-SAME: [[INT]] 513, 
// CHECK-SAME:   %swift.type_descriptor* bitcast (
// CHECK-SAME:     {{.*}}$s4main5ValueOMn{{.*}} to %swift.type_descriptor*
// CHECK-SAME:   ), 
// CHECK-SAME:   %swift.type* @"$sSiN", 
// CHECK-SAME:   i64 3 
// CHECK-SAME: }>, align [[ALIGNMENT]]
enum Value<First> {
  case only(First)
}

@inline(never)
func consume<T>(_ t: T) {
  withExtendedLifetime(t) { t in
  }
}

// CHECK: define hidden swiftcc void @"$s4main4doityyF"() #{{[0-9]+}} {
// CHECK:   call swiftcc void @"$s4main7consumeyyxlF"(
// CHECK-SAME:   %swift.opaque* noalias nocapture %{{[0-9]+}}, 
// CHECK-SAME:   %swift.type* getelementptr inbounds (%swift.full_type, %swift.full_type* bitcast (<{ i8*, i8**, [[INT]], %swift.type_descriptor*, %swift.type*, i64 }>* @"$s4main5ValueOySiGMf" to %swift.full_type*), i32 0, i32 2)
// CHECK-SAME: )
// CHECK: }
func doit() {
  consume( Value.only(13) )
}
doit()

// CHECK: ; Function Attrs: noinline nounwind readnone
// CHECK: define hidden swiftcc %swift.metadata_response @"$s4main5ValueOMa"([[INT]] %0, %swift.type* %1) #{{[0-9]+}} {{(section)?.*}}{
// CHECK: entry:
// CHECK:   [[ERASED_TYPE:%[0-9]+]] = bitcast %swift.type* %1 to i8*
// CHECK:   {{%[0-9]+}} = call swiftcc %swift.metadata_response @__swift_instantiateCanonicalPrespecializedGenericMetadata(
// CHECK-SAME:   [[INT]] %0, 
// CHECK-SAME:   i8* %2, 
// CHECK-SAME:   i8* undef, 
// CHECK-SAME:   i8* undef, 
// CHECK-SAME:   %swift.type_descriptor* bitcast (
// CHECK-SAME:     {{.*}}$s4main5ValueOMn{{.*}} to %swift.type_descriptor*
// CHECK-SAME:   )
// CHECK-SAME: ) #{{[0-9]+}}
// CHECK:   ret %swift.metadata_response {{%[0-9]+}}
// CHECK: }
