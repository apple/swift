// RUN: %empty-directory(%t)
// RUN: %{python} %utils/chex.py < %s > %t/generic_vtable.swift
// RUN: %target-swift-frontend -enable-objc-interop  %t/generic_vtable.swift -emit-ir | %FileCheck %t/generic_vtable.swift --check-prefixes=CHECK,CHECK-objc,CHECK-objc%target-ptrsize,CHECK-%target-ptrsize,CHECK-%target-import-type,CHECK-%target-abi -DINT=i%target-ptrsize
// RUN: %target-swift-frontend -disable-objc-interop %t/generic_vtable.swift -emit-ir | %FileCheck %t/generic_vtable.swift --check-prefixes=CHECK,CHECK-native,CHECK-native%target-ptrsize,CHECK-%target-ptrsize,CHECK-%target-import-type,CHECK-%target-abi -DINT=i%target-ptrsize

public class Base {
  public func m1() {}
  public func m2() {}
}

public class Derived<T> : Base {
  public override func m2() {}
  public func m3() {}
}

public class Concrete : Derived<Int> {
  public override func m3() {}
  public func m4() {}
}


//// Nominal type descriptor for 'Base' with method descriptors.

// CHECK-LABEL: @"$s14generic_vtable4BaseCMn" = {{(dllexport )?}}{{(protected )?}}constant
// -- flags: has vtable, is class, is unique
// CHECK-DIRECT-SAME: <i32 0x8000_0050>,
// CHECK-INDIRECT-SAME: <i32 0x8001_0050>,
// -- vtable offset
// CHECK-objc32-SAME: i32 16,
// CHECK-native32-SAME: i32 13,
// CHECK-objc64-SAME: i32 10,
// CHECK-native64-SAME: i32 7,
// -- vtable size
// CHECK-SAME: i32 3,
// -- vtable entry for m1()
// CHECK-SAME: ptr @"$s14generic_vtable4BaseC2m1yyF"
// -- vtable entry for m2()
// CHECK-SAME: ptr @"$s14generic_vtable4BaseC2m2yyF"
// --
// CHECK-SAME: section "{{.*}}",{{.*}} align 4

//// Type metadata for 'Base' has a static vtable.

// CHECK-LABEL: @"$s14generic_vtable4BaseCMf" = internal global
// -- destructor
// CHECK-SAME: @"$s14generic_vtable4BaseCfD{{(.ptrauth)?}}"
// -- value witness table
// CHECK-SAME: ptr {{@"\$sBoWV"|null}}
// -- vtable entry for 'm1()'
// CHECK-SAME: ptr {{@"\$s14generic_vtable4BaseC2m1yyF"|@"\$s14generic_vtable4BaseC2m1yyF.ptrauth[.0-9]*"}}
// -- vtable entry for 'm2()'
// CHECK-SAME: ptr {{@"\$s14generic_vtable4BaseC2m2yyF"|@"\$s14generic_vtable4BaseC2m2yyF.ptrauth[.0-9]*"}}
// -- vtable entry for 'init()'
// CHECK-SAME: ptr {{@"\$s14generic_vtable4BaseCACycfC"|@"\$s14generic_vtable4BaseCACycfC.ptrauth[.0-9]*"}}
// --
// CHECK-SAME: , align


//// Nominal type descriptor for 'Derived' with method descriptors.

// CHECK-LABEL: @"$s14generic_vtable7DerivedCMn" = {{(dllexport )?}}{{(protected )?}}constant
// -- flags: has vtable, has override table, is class, is unique, is generic
// CHECK-SAME: <i32 0xC000_00D0>,
// -- vtable offset
// CHECK-objc32-SAME: i32 17,
// CHECK-native32-SAME: i32 14,
// CHECK-objc64-SAME: i32 14,
// CHECK-native64-SAME: i32 11,
// -- vtable size
// CHECK-SAME: i32 1,
// -- vtable entry for m3()
// CHECK-SAME: ptr @"$s14generic_vtable7DerivedC2m3yyF"
// -- override table size
// CHECK-SAME: i32 2,
// -- override for m2()
// CHECK-SAME: @"$s14generic_vtable4BaseCMn"
// CHECK-SYSV-SAME: @"$s14generic_vtable4BaseCMn", i32 0, i32 14
// CHECK-WIN-SAME: @"$s14generic_vtable4BaseCMn", i32 0, i32 17
// CHECK-SAME: @"$s14generic_vtable7DerivedC2m2yyF"
// -- override for constructor
// CHECK-SAME: @"$s14generic_vtable4BaseCMn"
// CHECK-SYSV-SAME: @"$s14generic_vtable4BaseCMn", i32 0, i32 15
// CHECK-WIN-SAME: @"$s14generic_vtable4BaseCMn", i32 0, i32 18
// CHECK-SAME: @"$s14generic_vtable7DerivedCACyxGycfC"
// CHECK-SAME: section "{{.*}}",{{.*}} align 4

//// Type metadata pattern for 'Derived' has an empty vtable, filled in at
//// instantiation time.

// CHECK-LABEL: @"$s14generic_vtable7DerivedCMP" = internal constant <{{.*}}> <{
// -- ivar destroyer
// CHECK-SAME: i32 0
// --
// CHECK-SAME: }>, align


//// Nominal type descriptor for 'Concrete' with method descriptors.

// CHECK-LABEL: @"$s14generic_vtable8ConcreteCMn" = {{(dllexport )?}}{{(protected )?}}constant
// -- flags: has vtable, has override table, in-place initialization, is class, is unique
// CHECK-SAME: <i32 0xC001_0050>,
// -- vtable offset
// CHECK-objc32-SAME: i32 19,
// CHECK-native32-SAME: i32 16,
// CHECK-objc64-SAME: i32 15,
// CHECK-native64-SAME: i32 12,
// -- vtable size
// CHECK-SAME: i32 1,
// -- vtable entry for m4()
// CHECK-SAME: ptr @"$s14generic_vtable8ConcreteC2m4yyF"
// -- override table size
// CHECK-SAME: i32 2,
// -- override for m3()
// CHECK-SAME: @"$s14generic_vtable7DerivedCMn"
// CHECK-SAME: @"$s14generic_vtable7DerivedCMn", i32 0, i32 23
// CHECK-SAME: @"$s14generic_vtable8ConcreteC2m3yyF"
// -- override for constructor
// CHECK-SAME: @"$s14generic_vtable4BaseCMn"
// CHECK-SYSV-SAME: @"$s14generic_vtable4BaseCMn", i32 0, i32 15
// CHECK-WIN-SAME: @"$s14generic_vtable4BaseCMn", i32 0, i32 18
// CHECK-SAME: @"$s14generic_vtable8ConcreteCACycfC"
// --
// CHECK-SAME: section "{{.*}}",{{.*}} align 4

//// Type metadata for 'Concrete' has a static vtable.

// CHECK-LABEL: @"$s14generic_vtable8ConcreteCMf" = internal global <{{.*}}> <{
// -- destructor
// CHECK-SAME: @"$s14generic_vtable8ConcreteCfD{{(.ptrauth)?}}"
// -- value witness table is filled in at runtime
// CHECK-SAME: ptr null,
// -- nominal type descriptor
// CHECK-SAME: @"$s14generic_vtable8ConcreteCMn{{(.ptrauth)?}}"
// -- vtable entry for 'm1()'
// CHECK-SAME: ptr {{@"\$s14generic_vtable4BaseC2m1yyF"|@"\$s14generic_vtable4BaseC2m1yyF.ptrauth[.0-9]*"}}
// -- vtable entry for 'm2()'
// CHECK-SAME: ptr {{@"\$s14generic_vtable7DerivedC2m2yyF"|@"\$s14generic_vtable7DerivedC2m2yyF.ptrauth[.0-9]*"}}
// -- vtable entry for 'init()'
// CHECK-SAME: ptr {{@"\$s14generic_vtable8ConcreteCACycfC"|@"\$s14generic_vtable8ConcreteCACycfC.ptrauth[.0-9]*"}}
// -- vtable entry for 'm3()'
// CHECK-SAME: ptr {{@"\$s14generic_vtable8ConcreteC2m3yyF"|@"\$s14generic_vtable8ConcreteC2m3yyF.ptrauth[.0-9]*"}}
// -- vtable entry for 'm4()'
// CHECK-SAME: ptr {{@"\$s14generic_vtable8ConcreteC2m4yyF"|@"\$s14generic_vtable8ConcreteC2m4yyF.ptrauth[.0-9]*"}}
// --
// CHECK-SAME: }>, align


//// Method descriptors

// CHECK-LABEL: @"$s14generic_vtable4BaseC2m1yyFTq" ={{( dllexport)?}}{{( protected)?}} alias %swift.method_descriptor, getelementptr inbounds (<{{.*}}>, ptr @"$s14generic_vtable4BaseCMn", i32 0, i32 {{(13|16)}})
// CHECK-LABEL: @"$s14generic_vtable4BaseC2m2yyFTq" ={{( dllexport)?}}{{( protected)?}} alias %swift.method_descriptor, getelementptr inbounds (<{{.*}}>, ptr @"$s14generic_vtable4BaseCMn", i32 0, i32 {{(14|17)}})
// CHECK-LABEL: @"$s14generic_vtable4BaseCACycfCTq" = hidden alias %swift.method_descriptor, getelementptr inbounds (<{{.*}}>, ptr @"$s14generic_vtable4BaseCMn", i32 0, i32 {{(15|18)}})

// CHECK-LABEL: @"$s14generic_vtable7DerivedC2m3yyFTq" ={{( dllexport)?}}{{( protected)?}} alias %swift.method_descriptor, getelementptr inbounds (<{{.*}}>, ptr @"$s14generic_vtable7DerivedCMn", i32 0, i32 23)

// CHECK-LABEL: @"$s14generic_vtable8ConcreteC2m4yyFTq" ={{( dllexport)?}}{{( protected)?}} alias %swift.method_descriptor, getelementptr inbounds (<{{.*}}>, ptr @"$s14generic_vtable8ConcreteCMn", i32 0, i32 16)


//// Metadata initialization function for 'Derived' copies superclass vtable
//// and installs overrides for 'm2()' and 'init()'.

// CHECK-LABEL: define internal ptr @"$s14generic_vtable7DerivedCMi"(ptr %0, ptr %1, ptr %2)

// - 2 immediate members:
//   - type metadata for generic parameter T,
//   - and vtable entry for 'm3()'
// CHECK: [[METADATA:%.*]] = call ptr @swift_allocateGenericClassMetadata(ptr {{.*}}, ptr %1, ptr %2)
// CHECK: ret ptr [[METADATA]]

// CHECK-LABEL: define internal swiftcc %swift.metadata_response @"$s14generic_vtable7DerivedCMr"
// CHECK-SAME:    (ptr [[METADATA:%.*]], ptr %0, ptr %1) {{.*}} {
// CHECK: call swiftcc %swift.metadata_response @swift_initClassMetadata2(ptr [[METADATA]], [[INT]] 0, {{.*}})

// CHECK: ret %swift.metadata_response


// CHECK-LABEL: define {{(dllexport )?}}{{(protected )?}}swiftcc %swift.metadata_response @"$s14generic_vtable8ConcreteCMa"
// CHECK: call swiftcc %swift.metadata_response @swift_getSingletonMetadata([[INT]] %0, ptr @"$s14generic_vtable8ConcreteCMn")
// CHECK: ret

//// Metadata response function for 'Concrete' is fairly simple.

// CHECK-LABEL: define internal swiftcc %swift.metadata_response @"$s14generic_vtable8ConcreteCMr"(ptr %0, ptr %1, ptr %2)
// -- ClassLayoutFlags is 256 / 0x100, HasStaticVTable
// CHECK: call swiftcc %swift.metadata_response @swift_initClassMetadata2(ptr %0, [[INT]] 256, {{.*}})
// CHECK: ret %swift.metadata_response
