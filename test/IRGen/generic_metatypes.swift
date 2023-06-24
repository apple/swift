
// RUN: %swift %use_no_opaque_pointers -disable-generic-metadata-prespecialization -module-name generic_metatypes -target x86_64-apple-macosx10.9  -emit-ir -disable-legacy-type-info -parse-stdlib -primary-file %s | %FileCheck --check-prefix=CHECK --check-prefix=CHECK-64 -DINT=i64 %s
// RUN: %swift %use_no_opaque_pointers -disable-generic-metadata-prespecialization -module-name generic_metatypes -target i386-apple-ios7.0        -emit-ir -disable-legacy-type-info -parse-stdlib -primary-file %s | %FileCheck --check-prefix=CHECK --check-prefix=CHECK-32 -DINT=i32 %s
// RUN: %swift %use_no_opaque_pointers -disable-generic-metadata-prespecialization -module-name generic_metatypes -target x86_64-apple-ios7.0      -emit-ir -disable-legacy-type-info -parse-stdlib -primary-file %s | %FileCheck --check-prefix=CHECK --check-prefix=CHECK-64 -DINT=i64  %s
// RUN: %swift %use_no_opaque_pointers -disable-generic-metadata-prespecialization -module-name generic_metatypes -target x86_64-apple-tvos9.0     -emit-ir -disable-legacy-type-info -parse-stdlib -primary-file %s | %FileCheck --check-prefix=CHECK --check-prefix=CHECK-64 -DINT=i64  %s
// RUN: %swift %use_no_opaque_pointers -disable-generic-metadata-prespecialization -module-name generic_metatypes -target i386-apple-watchos2.0    -emit-ir -disable-legacy-type-info -parse-stdlib -primary-file %s | %FileCheck --check-prefix=CHECK --check-prefix=CHECK-32 -DINT=i32  %s
// RUN: %swift %use_no_opaque_pointers -disable-generic-metadata-prespecialization -module-name generic_metatypes -target x86_64-unknown-linux-gnu -disable-objc-interop -emit-ir -disable-legacy-type-info -parse-stdlib -primary-file %s | %FileCheck --check-prefix=CHECK --check-prefix=CHECK-64 -DINT=i64 %s

// RUN: %swift %use_no_opaque_pointers -disable-generic-metadata-prespecialization -module-name generic_metatypes -target armv7-apple-ios7.0       -emit-ir -disable-legacy-type-info -parse-stdlib -primary-file %s | %FileCheck --check-prefix=CHECK --check-prefix=CHECK-32 -DINT=i32 %s
// RUN: %swift %use_no_opaque_pointers -disable-generic-metadata-prespecialization -module-name generic_metatypes -target arm64-apple-ios7.0       -emit-ir -disable-legacy-type-info -parse-stdlib -primary-file %s | %FileCheck --check-prefix=CHECK --check-prefix=CHECK-64 -DINT=i64 %s
// RUN: %swift %use_no_opaque_pointers -disable-generic-metadata-prespecialization -module-name generic_metatypes -target arm64-apple-tvos9.0      -emit-ir -disable-legacy-type-info -parse-stdlib -primary-file %s | %FileCheck --check-prefix=CHECK --check-prefix=CHECK-64 -DINT=i64 %s
// RUN: %swift %use_no_opaque_pointers -disable-generic-metadata-prespecialization -module-name generic_metatypes -target armv7k-apple-watchos2.0  -emit-ir -disable-legacy-type-info -parse-stdlib -primary-file %s | %FileCheck --check-prefix=CHECK --check-prefix=CHECK-32 -DINT=i32 %s

// REQUIRES: CODEGENERATOR=X86
// REQUIRES: CODEGENERATOR=ARM

enum Never {}

func never() -> Never { return never() }

@_semantics("typechecker.type(of:)")
public func type<T, Metatype>(of value: T) -> Metatype {
  never()
}

// CHECK: define hidden swiftcc %swift.type* [[GENERIC_TYPEOF:@"\$s17generic_metatypes0A6TypeofyxmxlF"]](%swift.opaque* noalias nocapture %0, %swift.type* [[TYPE:%.*]])
func genericTypeof<T>(_ x: T) -> T.Type {
  // CHECK: [[METATYPE:%.*]] = call %swift.type* @swift_getDynamicType(%swift.opaque* {{.*}}, %swift.type* [[TYPE]], i1 false)
  // CHECK: ret %swift.type* [[METATYPE]]
  return type(of: x)
}

struct Foo {}
class Bar {}

// CHECK-LABEL: define hidden swiftcc %swift.type* @"$s17generic_metatypes27remapToSubstitutedMetatypes{{.*}}"(%T17generic_metatypes3BarC* %0) {{.*}} {
func remapToSubstitutedMetatypes(_ x: Foo, y: Bar)
  -> (Foo.Type, Bar.Type)
{
  // CHECK: call swiftcc %swift.type* [[GENERIC_TYPEOF]](%swift.opaque* noalias nocapture undef, %swift.type* {{.*}} @"$s17generic_metatypes3FooVMf", {{.*}})
  // CHECK: [[BAR_REQUEST:%.*]] = call {{.*}}@"$s17generic_metatypes3BarCMa"
  // CHECK: [[BAR:%.*]] = extractvalue {{.*}} [[BAR_REQUEST]]
  // CHECK: [[BAR_META:%.*]] = call swiftcc %swift.type* [[GENERIC_TYPEOF]](%swift.opaque* noalias nocapture {{%.*}}, %swift.type* [[BAR]])
  // CHECK: ret %swift.type* [[BAR_META]]
  return (genericTypeof(x), genericTypeof(y))
}


// CHECK-LABEL: define hidden swiftcc void @"$s17generic_metatypes23remapToGenericMetatypesyyF"()
func remapToGenericMetatypes() {
  // CHECK: [[BAR_REQUEST:%.*]] = call {{.*}}@"$s17generic_metatypes3BarCMa"
  // CHECK: [[BAR:%.*]] = extractvalue {{.*}} [[BAR_REQUEST]]
  // CHECK: call swiftcc void @"$s17generic_metatypes0A9Metatypes{{.*}}"(%swift.type* {{.*}} @"$s17generic_metatypes3FooVMf", {{.*}} %swift.type* [[BAR]], %swift.type* {{.*}} @"$s17generic_metatypes3FooVMf", {{.*}} %swift.type* [[BAR]])
  genericMetatypes(Foo.self, Bar.self)
}

func genericMetatypes<T, U>(_ t: T.Type, _ u: U.Type) {}

protocol Bas {}

// CHECK: define hidden swiftcc { %swift.type*, i8** } @"$s17generic_metatypes14protocolTypeof{{.*}}"(%T17generic_metatypes3BasP* noalias nocapture dereferenceable({{.*}}) %0)
func protocolTypeof(_ x: Bas) -> Bas.Type {
  // CHECK: [[METADATA_ADDR:%.*]] = getelementptr inbounds %T17generic_metatypes3BasP, %T17generic_metatypes3BasP* [[X:%.*]], i32 0, i32 1
  // CHECK: [[METADATA:%.*]] = load %swift.type*, %swift.type** [[METADATA_ADDR]]
  // CHECK: [[BUFFER:%.*]] = bitcast %T17generic_metatypes3BasP* [[X]] to %__opaque_existential_type_1*
  // CHECK: [[VALUE_ADDR:%.*]] = call %swift.opaque* @__swift_project_boxed_opaque_existential_1(%__opaque_existential_type_1* [[BUFFER]], %swift.type* [[METADATA]])
  // CHECK: [[METATYPE:%.*]] = call %swift.type* @swift_getDynamicType(%swift.opaque* [[VALUE_ADDR]], %swift.type* [[METADATA]], i1 true)
  // CHECK: [[WTABLE_ADDR:%.*]] = getelementptr inbounds %T17generic_metatypes3BasP, %T17generic_metatypes3BasP* %0, i32 0, i32 2
  // CHECK: [[WTABLE:%.*]] = load i8**, i8*** [[WTABLE_ADDR]]
  // CHECK-NOT: call void @__swift_destroy_boxed_opaque_existential_1(%T17generic_metatypes3BasP* %0)
  // CHECK: [[T0:%.*]] = insertvalue { %swift.type*, i8** } undef, %swift.type* [[METATYPE]], 0
  // CHECK: [[T1:%.*]] = insertvalue { %swift.type*, i8** } [[T0]], i8** [[WTABLE]], 1
  // CHECK: ret { %swift.type*, i8** } [[T1]]
  return type(of: x)
}

struct Zim : Bas {}
class Zang : Bas {}

// CHECK-LABEL: define hidden swiftcc { %swift.type*, i8** } @"$s17generic_metatypes15metatypeErasureyAA3Bas_pXpAA3ZimVmF"() #0
func metatypeErasure(_ z: Zim.Type) -> Bas.Type {
  // CHECK: ret { %swift.type*, i8** } {{.*}} @"$s17generic_metatypes3ZimVMf", {{.*}} @"$s17generic_metatypes3ZimVAA3BasAAWP"
  return z
}

// CHECK-LABEL: define hidden swiftcc { %swift.type*, i8** } @"$s17generic_metatypes15metatypeErasureyAA3Bas_pXpAA4ZangCmF"(%swift.type* %0)
func metatypeErasure(_ z: Zang.Type) -> Bas.Type {
  // CHECK: [[RET:%.*]] = insertvalue { %swift.type*, i8** } undef, %swift.type* %0, 0
  // CHECK: [[RET2:%.*]] = insertvalue { %swift.type*, i8** } [[RET]], i8** getelementptr inbounds ([1 x i8*], [1 x i8*]* @"$s17generic_metatypes4ZangCAA3BasAAWP", i32 0, i32 0), 1
  // CHECK: ret { %swift.type*, i8** } [[RET2]]
  return z
}

struct OneArg<T> {}
struct TwoArgs<T, U> {}
struct ThreeArgs<T, U, V> {}
struct FourArgs<T, U, V, W> {}
struct FiveArgs<T, U, V, W, X> {}

func genericMetatype<A>(_ x: A.Type) {}

// CHECK-LABEL: define hidden swiftcc void @"$s17generic_metatypes20makeGenericMetatypesyyF"() {{.*}} {
func makeGenericMetatypes() {
  // CHECK: call {{.*}} @__swift_instantiateConcreteTypeFromMangledName{{.*}}({{.*}} @"$s17generic_metatypes6OneArgVyAA3FooVGMD") [[NOUNWIND_READNONE:#[0-9]+]]
  genericMetatype(OneArg<Foo>.self)

  // CHECK: call {{.*}} @__swift_instantiateConcreteTypeFromMangledName{{.*}}({{.*}} @"$s17generic_metatypes7TwoArgsVyAA3FooVAA3BarCGMD") [[NOUNWIND_READNONE]]
  genericMetatype(TwoArgs<Foo, Bar>.self)

  // CHECK: call {{.*}} @__swift_instantiateConcreteTypeFromMangledName{{.*}}({{.*}} @"$s17generic_metatypes9ThreeArgsVyAA3FooVAA3BarCAEGMD") [[NOUNWIND_READNONE]]
  genericMetatype(ThreeArgs<Foo, Bar, Foo>.self)

  // CHECK: call {{.*}} @__swift_instantiateConcreteTypeFromMangledName{{.*}}({{.*}} @"$s17generic_metatypes8FourArgsVyAA3FooVAA3BarCAeGGMD") [[NOUNWIND_READNONE]]
  genericMetatype(FourArgs<Foo, Bar, Foo, Bar>.self)

  // CHECK: call {{.*}} @__swift_instantiateConcreteTypeFromMangledName{{.*}}({{.*}} @"$s17generic_metatypes8FiveArgsVyAA3FooVAA3BarCAegEGMD") [[NOUNWIND_READNONE]]
  genericMetatype(FiveArgs<Foo, Bar, Foo, Bar, Foo>.self)
}

// CHECK-LABEL: define hidden swiftcc %swift.metadata_response @"$s17generic_metatypes6OneArgVMa"
// CHECK-SAME:    ([[INT]] %0, %swift.type* %1)
// CHECK:   [[BITCAST_1:%.*]] = bitcast {{.*}} %1
// CHECK:   [[T0:%.*]] = call swiftcc %swift.metadata_response @__swift_instantiateGenericMetadata([[INT]] %0, i8* [[BITCAST_1]], i8* undef, i8* undef, %swift.type_descriptor* {{.*}} @"$s17generic_metatypes6OneArgVMn" {{.*}})
// CHECK:   [[METADATA:%.*]] = extractvalue %swift.metadata_response [[T0]], 0

// CHECK-LABEL: define hidden swiftcc %swift.metadata_response @"$s17generic_metatypes7TwoArgsVMa"
// CHECK-SAME:    ([[INT]] %0, %swift.type* %1, %swift.type* %2)
// CHECK:   [[BITCAST_1:%.*]] = bitcast {{.*}} %1
// CHECK:   [[BITCAST_2:%.*]] = bitcast {{.*}} %2
// CHECK:   [[T0:%.*]] = call swiftcc %swift.metadata_response @__swift_instantiateGenericMetadata([[INT]] %0, i8* [[BITCAST_1]], i8* [[BITCAST_2]], i8* undef, %swift.type_descriptor* {{.*}} @"$s17generic_metatypes7TwoArgsVMn" {{.*}})
// CHECK:   [[METADATA:%.*]] = extractvalue %swift.metadata_response [[T0]], 0

// CHECK-LABEL: define hidden swiftcc %swift.metadata_response @"$s17generic_metatypes9ThreeArgsVMa"
// CHECK-SAME:    ({{i[0-9]+}} %0, %swift.type* %1, %swift.type* %2, %swift.type* %3)
// CHECK:   [[BITCAST_1:%.*]] = bitcast {{.*}} %1
// CHECK:   [[BITCAST_2:%.*]] = bitcast {{.*}} %2
// CHECK:   [[BITCAST_3:%.*]] = bitcast {{.*}} %3
// CHECK:   [[T0:%.*]] = call swiftcc %swift.metadata_response @__swift_instantiateGenericMetadata([[INT]] %0, i8* [[BITCAST_1]], i8* [[BITCAST_2]], i8* [[BITCAST_3]], %swift.type_descriptor* {{.*}} @"$s17generic_metatypes9ThreeArgsVMn" {{.*}})
// CHECK:   [[METADATA:%.*]] = extractvalue %swift.metadata_response [[T0]], 0

// CHECK-LABEL: define hidden swiftcc %swift.metadata_response @"$s17generic_metatypes8FiveArgsVMa"
// CHECK-SAME:    ([[INT]] %0, i8** %1) [[NOUNWIND_OPT:#[0-9]+]]
// CHECK-NOT: alloc
// CHECK:   call swiftcc %swift.metadata_response @swift_getGenericMetadata([[INT]] %0, i8* {{.*}}, %swift.type_descriptor* {{.*}} @"$s17generic_metatypes8FiveArgsVMn" {{.*}})
// CHECK-NOT: call void @llvm.lifetime.end
// CHECK:   ret %swift.metadata_response

// CHECK-DAG: attributes [[NOUNWIND_READNONE]] = { nounwind readonly }
// CHECK-DAG: attributes [[NOUNWIND_OPT]] = { noinline nounwind "
