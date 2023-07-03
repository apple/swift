
// RUN: %swift -prespecialize-generic-metadata -module-name generic_metatypes -target x86_64-apple-macosx99.99  -emit-ir -disable-legacy-type-info -parse-stdlib -primary-file %s | %FileCheck --check-prefix=CHECK --check-prefix=CHECK-64 -DINT=i64 %s
// RUN: %swift -prespecialize-generic-metadata -module-name generic_metatypes -target x86_64-apple-macosx99.99  -emit-ir -disable-legacy-type-info -parse-stdlib -primary-file %s
// RUN: %swift -prespecialize-generic-metadata -module-name generic_metatypes -target x86_64-apple-ios99.0      -emit-ir -disable-legacy-type-info -parse-stdlib -primary-file %s | %FileCheck --check-prefix=CHECK --check-prefix=CHECK-64 -DINT=i64  %s
// RUN: %swift -prespecialize-generic-metadata -module-name generic_metatypes -target x86_64-apple-tvos99.0     -emit-ir -disable-legacy-type-info -parse-stdlib -primary-file %s | %FileCheck --check-prefix=CHECK --check-prefix=CHECK-64 -DINT=i64  %s
// RUN: %swift -prespecialize-generic-metadata -module-name generic_metatypes -target i386-apple-watchos9.99    -emit-ir -disable-legacy-type-info -parse-stdlib -primary-file %s | %FileCheck --check-prefix=CHECK --check-prefix=CHECK-32 -DINT=i32  %s
// RUN: %swift -prespecialize-generic-metadata -module-name generic_metatypes -target x86_64-unknown-linux-gnu -disable-objc-interop -emit-ir -disable-legacy-type-info -parse-stdlib -primary-file %s | %FileCheck --check-prefix=CHECK --check-prefix=CHECK-64 -DINT=i64 %s

// RUN: %swift -prespecialize-generic-metadata -module-name generic_metatypes -target arm64-apple-ios99.0       -emit-ir -disable-legacy-type-info -parse-stdlib -primary-file %s | %FileCheck --check-prefix=CHECK --check-prefix=CHECK-64 -DINT=i64 %s
// RUN: %swift -prespecialize-generic-metadata -module-name generic_metatypes -target arm64-apple-tvos99.0      -emit-ir -disable-legacy-type-info -parse-stdlib -primary-file %s | %FileCheck --check-prefix=CHECK --check-prefix=CHECK-64 -DINT=i64 %s
// RUN: %swift -prespecialize-generic-metadata -module-name generic_metatypes -target armv7k-apple-watchos9.99  -emit-ir -disable-legacy-type-info -parse-stdlib -primary-file %s | %FileCheck --check-prefix=CHECK --check-prefix=CHECK-32 -DINT=i32 %s


// REQUIRES: VENDOR=apple || OS=linux-gnu
// REQUIRES: CODEGENERATOR=X86
// REQUIRES: CODEGENERATOR=ARM

enum Never {}

func never() -> Never { return never() }

@_semantics("typechecker.type(of:)")
public func type<T, Metatype>(of value: T) -> Metatype {
  never()
}

// CHECK: define hidden swiftcc ptr [[GENERIC_TYPEOF:@"\$s17generic_metatypes0A6TypeofyxmxlF"]](ptr noalias nocapture %0, ptr [[TYPE:%.*]])
func genericTypeof<T>(_ x: T) -> T.Type {
  // CHECK: [[METATYPE:%.*]] = call ptr @swift_getDynamicType(ptr {{.*}}, ptr [[TYPE]], i1 false)
  // CHECK: ret ptr [[METATYPE]]
  return type(of: x)
}

struct Foo {}
class Bar {}

// CHECK-LABEL: define hidden swiftcc ptr @"$s17generic_metatypes27remapToSubstitutedMetatypes{{.*}}"(ptr %0) {{.*}} {
func remapToSubstitutedMetatypes(_ x: Foo, y: Bar)
  -> (Foo.Type, Bar.Type)
{
  // CHECK: call swiftcc ptr [[GENERIC_TYPEOF]](ptr noalias nocapture undef, ptr {{.*}} @"$s17generic_metatypes3FooVMf", {{.*}})
  // CHECK: [[BAR_REQUEST:%.*]] = call {{.*}}@"$s17generic_metatypes3BarCMa"
  // CHECK: [[BAR:%.*]] = extractvalue {{.*}} [[BAR_REQUEST]]
  // CHECK: [[BAR_META:%.*]] = call swiftcc ptr [[GENERIC_TYPEOF]](ptr noalias nocapture {{%.*}}, ptr [[BAR]])
  // CHECK: ret ptr [[BAR_META]]
  return (genericTypeof(x), genericTypeof(y))
}


// CHECK-LABEL: define hidden swiftcc void @"$s17generic_metatypes23remapToGenericMetatypesyyF"()
func remapToGenericMetatypes() {
  // CHECK: [[BAR_REQUEST:%.*]] = call {{.*}}@"$s17generic_metatypes3BarCMa"
  // CHECK: [[BAR:%.*]] = extractvalue {{.*}} [[BAR_REQUEST]]
  // CHECK: call swiftcc void @"$s17generic_metatypes0A9Metatypes{{.*}}"(ptr {{.*}} @"$s17generic_metatypes3FooVMf", {{.*}} ptr [[BAR]], ptr {{.*}} @"$s17generic_metatypes3FooVMf", {{.*}} ptr [[BAR]])
  genericMetatypes(Foo.self, Bar.self)
}

func genericMetatypes<T, U>(_ t: T.Type, _ u: U.Type) {}

protocol Bas {}

// CHECK: define hidden swiftcc { ptr, ptr } @"$s17generic_metatypes14protocolTypeof{{.*}}"(ptr noalias nocapture dereferenceable({{.*}}) %0)
func protocolTypeof(_ x: Bas) -> Bas.Type {
  // CHECK: [[METADATA_ADDR:%.*]] = getelementptr inbounds %T17generic_metatypes3BasP, ptr [[X:%.*]], i32 0, i32 1
  // CHECK: [[METADATA:%.*]] = load ptr, ptr [[METADATA_ADDR]]
  // CHECK: [[VALUE_ADDR:%.*]] = call ptr @__swift_project_boxed_opaque_existential_1(ptr [[X]], ptr [[METADATA]])
  // CHECK: [[METATYPE:%.*]] = call ptr @swift_getDynamicType(ptr [[VALUE_ADDR]], ptr [[METADATA]], i1 true)
  // CHECK: [[WTABLE_ADDR:%.*]] = getelementptr inbounds %T17generic_metatypes3BasP, ptr %0, i32 0, i32 2
  // CHECK: [[WTABLE:%.*]] = load ptr, ptr [[WTABLE_ADDR]]
  // CHECK-NOT: call void @__swift_destroy_boxed_opaque_existential_1(ptr %0)
  // CHECK: [[T0:%.*]] = insertvalue { ptr, ptr } undef, ptr [[METATYPE]], 0
  // CHECK: [[T1:%.*]] = insertvalue { ptr, ptr } [[T0]], ptr [[WTABLE]], 1
  // CHECK: ret { ptr, ptr } [[T1]]
  return type(of: x)
}

struct Zim : Bas {}
class Zang : Bas {}

// CHECK-LABEL: define hidden swiftcc { ptr, ptr } @"$s17generic_metatypes15metatypeErasureyAA3Bas_pXpAA3ZimVmF"() #0
func metatypeErasure(_ z: Zim.Type) -> Bas.Type {
  // CHECK: ret { ptr, ptr } {{.*}} @"$s17generic_metatypes3ZimVMf", {{.*}} @"$s17generic_metatypes3ZimVAA3BasAAWP"
  return z
}

// CHECK-LABEL: define hidden swiftcc { ptr, ptr } @"$s17generic_metatypes15metatypeErasureyAA3Bas_pXpAA4ZangCmF"(ptr %0) #0
func metatypeErasure(_ z: Zang.Type) -> Bas.Type {
  // CHECK: [[RET:%.*]] = insertvalue { ptr, ptr } undef, ptr %0, 0
  // CHECK: [[RET2:%.*]] = insertvalue { ptr, ptr } [[RET]], ptr @"$s17generic_metatypes4ZangCAA3BasAAWP", 1
  // CHECK: ret { ptr, ptr } [[RET2]]
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
  // CHECK: call swiftcc void @"$s17generic_metatypes0A8MetatypeyyxmlF"(
  // CHECK-SAME:   ptr getelementptr inbounds (
  // CHECK-SAME:     %swift.full_type,
  // CHECK-SAME:     ptr @"$s17generic_metatypes6OneArgVyAA3FooVGMf",
  // CHECK-SAME:     i32 0, 
  // CHECK-SAME:     i32 2
  // CHECK-SAME:   ), 
  // CHECK-SAME:   ptr getelementptr inbounds (
  // CHECK-SAME:     %swift.full_type, 
  // CHECK-SAME:     ptr @"$s17generic_metatypes6OneArgVyAA3FooVGMf",
  // CHECK-SAME:     i32 0, 
  // CHECK-SAME:     i32 2
  // CHECK-SAME:   )
  // CHECK-SAME: )
  genericMetatype(OneArg<Foo>.self)

  // CHECK: call {{.*}} @__swift_instantiateConcreteTypeFromMangledName({{.*}} @"$s17generic_metatypes7TwoArgsVyAA3FooVAA3BarCGMD") [[NOUNWIND_READNONE:#[0-9]+]]
  genericMetatype(TwoArgs<Foo, Bar>.self)

  // CHECK: call {{.*}} @__swift_instantiateConcreteTypeFromMangledName({{.*}} @"$s17generic_metatypes9ThreeArgsVyAA3FooVAA3BarCAEGMD") [[NOUNWIND_READNONE]]
  genericMetatype(ThreeArgs<Foo, Bar, Foo>.self)

  // CHECK: call {{.*}} @__swift_instantiateConcreteTypeFromMangledName({{.*}} @"$s17generic_metatypes8FourArgsVyAA3FooVAA3BarCAeGGMD") [[NOUNWIND_READNONE]]
  genericMetatype(FourArgs<Foo, Bar, Foo, Bar>.self)

  // CHECK: call {{.*}} @__swift_instantiateConcreteTypeFromMangledName({{.*}} @"$s17generic_metatypes8FiveArgsVyAA3FooVAA3BarCAegEGMD") [[NOUNWIND_READNONE]]
  genericMetatype(FiveArgs<Foo, Bar, Foo, Bar, Foo>.self)
}

// CHECK-LABEL: define hidden swiftcc %swift.metadata_response @"$s17generic_metatypes6OneArgVMa"
// CHECK-SAME:    ([[INT]] %0, ptr %1)
// CHECK:   [[T0:%.*]] = call swiftcc %swift.metadata_response @__swift_instantiateCanonicalPrespecializedGenericMetadata([[INT]] %0, ptr %1, ptr undef, ptr undef, ptr @"$s17generic_metatypes6OneArgVMn", ptr @"$s17generic_metatypes6OneArgVMz")
// CHECK:   [[METADATA:%.*]] = extractvalue %swift.metadata_response [[T0]], 0

// CHECK-LABEL: define hidden swiftcc %swift.metadata_response @"$s17generic_metatypes7TwoArgsVMa"
// CHECK-SAME:    ([[INT]] %0, ptr %1, ptr %2)
// CHECK:   [[T0:%.*]] = call swiftcc %swift.metadata_response @__swift_instantiateGenericMetadata([[INT]] %0, ptr %1, ptr %2, ptr undef, ptr @"$s17generic_metatypes7TwoArgsVMn")
// CHECK:   [[METADATA:%.*]] = extractvalue %swift.metadata_response [[T0]], 0

// CHECK-LABEL: define hidden swiftcc %swift.metadata_response @"$s17generic_metatypes9ThreeArgsVMa"
// CHECK-SAME:    ({{i[0-9]+}} %0, ptr %1, ptr %2, ptr %3)
// CHECK:   [[T0:%.*]] = call swiftcc %swift.metadata_response @__swift_instantiateGenericMetadata([[INT]] %0, ptr %1, ptr %2, ptr %3, ptr @"$s17generic_metatypes9ThreeArgsVMn")
// CHECK:   [[METADATA:%.*]] = extractvalue %swift.metadata_response [[T0]], 0

// CHECK-LABEL: define hidden swiftcc %swift.metadata_response @"$s17generic_metatypes8FiveArgsVMa"
// CHECK-SAME:    ([[INT]] %0, ptr %1) [[NOUNWIND_OPT:#[0-9]+]]
// CHECK-NOT: alloc
// CHECK:   call swiftcc %swift.metadata_response @swift_getGenericMetadata([[INT]] %0, ptr {{.*}}, ptr @"$s17generic_metatypes8FiveArgsVMn")
// CHECK-NOT: call void @llvm.lifetime.end
// CHECK:   ret %swift.metadata_response

// CHECK-DAG: attributes [[NOUNWIND_READNONE]] = { nounwind readonly }
// CHECK-DAG: attributes [[NOUNWIND_OPT]] = { noinline nounwind "
