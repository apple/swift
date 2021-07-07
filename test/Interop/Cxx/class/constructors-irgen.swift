// Target-specific tests for C++ constructor call code generation.

// REQUIRES: CODEGENERATOR=ARM && CODEGENERATOR=X86

// RUN: %swift -module-name Swift -target x86_64-apple-macosx10.9 -dump-clang-diagnostics -I %S/Inputs -enable-cxx-interop -emit-ir %s -parse-stdlib -parse-as-library -disable-legacy-type-info | %FileCheck %s -check-prefix=ITANIUM_X64
// RUN: %swift -module-name Swift -target armv7-unknown-linux-androideabi -dump-clang-diagnostics -I %S/Inputs -enable-cxx-interop -emit-ir %s -parse-stdlib -parse-as-library -disable-legacy-type-info | %FileCheck %s -check-prefix=ITANIUM_ARM
// RUN: %swift -module-name Swift -target x86_64-unknown-windows-msvc -dump-clang-diagnostics -I %S/Inputs -enable-cxx-interop -emit-ir %s -parse-stdlib -parse-as-library -disable-legacy-type-info | %FileCheck %s -check-prefix=MICROSOFT_X64

import Constructors
import TypeClassification

typealias Void = ()
struct UnsafePointer<T> { }
struct UnsafeMutablePointer<T> { }

public func createHasVirtualBase() -> HasVirtualBase {
  // ITANIUM_X64: define swiftcc void @"$ss20createHasVirtualBaseSo0bcD0VyF"(%TSo14HasVirtualBaseV* noalias nocapture sret({{.*}}) %0)
  // ITANIUM_X64-NOT: define
  // ITANIUM_X64: call void @_ZN14HasVirtualBaseC1E7ArgType(%struct.HasVirtualBase* %{{[0-9]+}}, i32 %{{[0-9]+}})
  //
  // ITANIUM_ARM: define protected swiftcc void @"$ss20createHasVirtualBaseSo0bcD0VyF"(%TSo14HasVirtualBaseV* noalias nocapture sret({{.*}}) %0)
  // To verify that the thunk is inlined, make sure there's no intervening
  // `define`, i.e. the call to the C++ constructor happens in
  // createHasVirtualBase(), not some later function.
  // ITANIUM_ARM-NOT: define
  // Note `this` return type.
  // ITANIUM_ARM: call %struct.HasVirtualBase* @_ZN14HasVirtualBaseC1E7ArgType(%struct.HasVirtualBase* %{{[0-9]+}}, [1 x i32] %{{[0-9]+}})
  //
  // MICROSOFT_X64: define dllexport swiftcc void @"$ss20createHasVirtualBaseSo0bcD0VyF"(%TSo14HasVirtualBaseV* noalias nocapture sret({{.*}}) %0)
  // MICROSOFT_X64-NOT: define
  // Note `this` return type and implicit "most derived" argument.
  // MICROSOFT_X64: call %struct.HasVirtualBase* @"??0HasVirtualBase@@QEAA@UArgType@@@Z"(%struct.HasVirtualBase* %{{[0-9]+}}, i32 %{{[0-9]+}}, i32 1)
  return HasVirtualBase(ArgType())
}

public func createImplicitDefaultConstructor() -> ImplicitDefaultConstructor {
  // ITANIUM_X64: define swiftcc i32 @"$ss32createImplicitDefaultConstructorSo0bcD0VyF"()
  // ITANIUM_X64-NOT: define
  // ITANIUM_X64: call void @_ZN26ImplicitDefaultConstructorC1Ev(%struct.ImplicitDefaultConstructor* %{{[0-9]+}})
  //
  // ITANIUM_ARM: define protected swiftcc i32 @"$ss32createImplicitDefaultConstructorSo0bcD0VyF"()
  // ITANIUM_ARM-NOT: define
  // Note `this` return type.
  // ITANIUM_ARM: call %struct.ImplicitDefaultConstructor* @_ZN26ImplicitDefaultConstructorC2Ev(%struct.ImplicitDefaultConstructor* %{{[0-9]+}})
  //
  // MICROSOFT_X64: define dllexport swiftcc i32 @"$ss32createImplicitDefaultConstructorSo0bcD0VyF"()
  // MICROSOFT_X64-NOT: define
  // Note `this` return type but no implicit "most derived" argument.
  // MICROSOFT_X64: call %struct.ImplicitDefaultConstructor* @"??0ImplicitDefaultConstructor@@QEAA@XZ"(%struct.ImplicitDefaultConstructor* %{{[0-9]+}})
  return ImplicitDefaultConstructor()
}

public func createStructWithSubobjectCopyConstructorAndValue() {
  // ITANIUM_X64-LABEL: define swiftcc void @"$ss48createStructWithSubobjectCopyConstructorAndValueyyF"()
  // ITANIUM_X64: [[MEMBER:%.*]] = alloca %TSo33StructWithCopyConstructorAndValueV
  // ITANIUM_X64: [[OBJ:%.*]] = alloca %TSo42StructWithSubobjectCopyConstructorAndValueV
  // ITANIUM_X64: [[TMP:%.*]] = alloca %TSo33StructWithCopyConstructorAndValueV
  // ITANIUM_X64: [[MEMBER_AS_STRUCT:%.*]] = bitcast %TSo33StructWithCopyConstructorAndValueV* [[MEMBER]] to %struct.StructWithCopyConstructorAndValue*
  // ITANIUM_X64: void @_ZN33StructWithCopyConstructorAndValueC1Ev(%struct.StructWithCopyConstructorAndValue* [[MEMBER_AS_STRUCT]])
  // ITANIUM_X64: [[TMP_STRUCT:%.*]] = bitcast %TSo33StructWithCopyConstructorAndValueV* [[TMP]] to %struct.StructWithCopyConstructorAndValue*
  // ITANIUM_X64: [[MEMBER_AS_STRUCT_2:%.*]] = bitcast %TSo33StructWithCopyConstructorAndValueV* [[MEMBER]] to %struct.StructWithCopyConstructorAndValue*
  // ITANIUM_X64: call void @_ZN33StructWithCopyConstructorAndValueC1ERKS_(%struct.StructWithCopyConstructorAndValue* [[TMP_STRUCT]], %struct.StructWithCopyConstructorAndValue* [[MEMBER_AS_STRUCT_2]])
  // ITANIUM_X64: [[OBJ_MEMBER:%.*]] = getelementptr inbounds %TSo42StructWithSubobjectCopyConstructorAndValueV, %TSo42StructWithSubobjectCopyConstructorAndValueV* [[OBJ]], i32 0, i32 0
  // ITANIUM_X64: call %TSo33StructWithCopyConstructorAndValueV* @"$sSo33StructWithCopyConstructorAndValueVWOb"(%TSo33StructWithCopyConstructorAndValueV* [[TMP]], %TSo33StructWithCopyConstructorAndValueV* [[OBJ_MEMBER]])
  // ITANIUM_X64: ret void
  
  // ITANIUM_ARM-LABEL: define protected swiftcc void @"$ss48createStructWithSubobjectCopyConstructorAndValueyyF"()
  // ITANIUM_ARM: [[MEMBER:%.*]] = alloca %TSo33StructWithCopyConstructorAndValueV
  // ITANIUM_ARM: [[OBJ:%.*]] = alloca %TSo42StructWithSubobjectCopyConstructorAndValueV
  // ITANIUM_ARM: [[TMP:%.*]] = alloca %TSo33StructWithCopyConstructorAndValueV
  // ITANIUM_ARM: [[MEMBER_AS_STRUCT:%.*]] = bitcast %TSo33StructWithCopyConstructorAndValueV* [[MEMBER]] to %struct.StructWithCopyConstructorAndValue*
  // ITANIUM_ARM: call %struct.StructWithCopyConstructorAndValue* @_ZN33StructWithCopyConstructorAndValueC2Ev(%struct.StructWithCopyConstructorAndValue* [[MEMBER_AS_STRUCT]])
  // ITANIUM_ARM: [[TMP_STRUCT:%.*]] = bitcast %TSo33StructWithCopyConstructorAndValueV* [[TMP]] to %struct.StructWithCopyConstructorAndValue*
  // ITANIUM_ARM: [[MEMBER_AS_STRUCT_2:%.*]] = bitcast %TSo33StructWithCopyConstructorAndValueV* [[MEMBER]] to %struct.StructWithCopyConstructorAndValue*
  // ITANIUM_ARM: call %struct.StructWithCopyConstructorAndValue* @_ZN33StructWithCopyConstructorAndValueC2ERKS_(%struct.StructWithCopyConstructorAndValue* [[TMP_STRUCT]], %struct.StructWithCopyConstructorAndValue* [[MEMBER_AS_STRUCT_2]])
  // ITANIUM_ARM: [[OBJ_MEMBER:%.*]] = getelementptr inbounds %TSo42StructWithSubobjectCopyConstructorAndValueV, %TSo42StructWithSubobjectCopyConstructorAndValueV* [[OBJ]], i32 0, i32 0
  // ITANIUM_ARM: call %TSo33StructWithCopyConstructorAndValueV* @"$sSo33StructWithCopyConstructorAndValueVWOb"(%TSo33StructWithCopyConstructorAndValueV* [[TMP]], %TSo33StructWithCopyConstructorAndValueV* [[OBJ_MEMBER]])
  // ITANIUM_ARM: ret void

  // MICROSOFT_X64-LABEL: define dllexport swiftcc void @"$ss48createStructWithSubobjectCopyConstructorAndValueyyF"()
  // MICROSOFT_X64: [[MEMBER:%.*]] = alloca %TSo33StructWithCopyConstructorAndValueV
  // MICROSOFT_X64: [[OBJ:%.*]] = alloca %TSo42StructWithSubobjectCopyConstructorAndValueV
  // MICROSOFT_X64: [[TMP:%.*]] = alloca %TSo33StructWithCopyConstructorAndValueV
  // MICROSOFT_X64: [[MEMBER_AS_STRUCT:%.*]] = bitcast %TSo33StructWithCopyConstructorAndValueV* [[MEMBER]] to %struct.StructWithCopyConstructorAndValue*
  // MICROSOFT_X64: call %struct.StructWithCopyConstructorAndValue* @"??0StructWithCopyConstructorAndValue@@QEAA@XZ"(%struct.StructWithCopyConstructorAndValue* [[MEMBER_AS_STRUCT]])
  // MICROSOFT_X64: [[TMP_STRUCT:%.*]] = bitcast %TSo33StructWithCopyConstructorAndValueV* [[TMP]] to %struct.StructWithCopyConstructorAndValue*
  // MICROSOFT_X64: [[MEMBER_AS_STRUCT_2:%.*]] = bitcast %TSo33StructWithCopyConstructorAndValueV* [[MEMBER]] to %struct.StructWithCopyConstructorAndValue*
  // MICROSOFT_X64: call %struct.StructWithCopyConstructorAndValue* @"??0StructWithCopyConstructorAndValue@@QEAA@AEBU0@@Z"(%struct.StructWithCopyConstructorAndValue* [[TMP_STRUCT]], %struct.StructWithCopyConstructorAndValue* [[MEMBER_AS_STRUCT_2]])
  // MICROSOFT_X64: [[OBJ_MEMBER:%.*]] = getelementptr inbounds %TSo42StructWithSubobjectCopyConstructorAndValueV, %TSo42StructWithSubobjectCopyConstructorAndValueV* [[OBJ]], i32 0, i32 0
  // MICROSOFT_X64: call %TSo33StructWithCopyConstructorAndValueV* @"$sSo33StructWithCopyConstructorAndValueVWOb"(%TSo33StructWithCopyConstructorAndValueV* [[TMP]], %TSo33StructWithCopyConstructorAndValueV* [[OBJ_MEMBER]])
  // MICROSOFT_X64: ret void
  let member = StructWithCopyConstructorAndValue()
  let obj = StructWithSubobjectCopyConstructorAndValue(member: member)
}

public func createTemplatedConstructor() {
  // ITANIUM_X64-LABEL: define swiftcc void @"$ss26createTemplatedConstructoryyF"()
  // ITANIUM_X64: [[OBJ:%.*]] = alloca %TSo20TemplatedConstructorV
  // ITANIUM_X64: [[IVAL:%.*]] = load i32, i32*
  // ITANIUM_X64: [[OBJ_AS_STRUCT:%.*]] = bitcast %TSo20TemplatedConstructorV* [[OBJ]] to %struct.TemplatedConstructor*
  // ITANIUM_X64: call void @_ZN20TemplatedConstructorC1I7ArgTypeEET_(%struct.TemplatedConstructor* [[OBJ_AS_STRUCT]], i32 [[IVAL]])
  // ITANIUM_X64: ret void
  
  // ITANIUM_X64-LABEL: define linkonce_odr void @_ZN20TemplatedConstructorC1I7ArgTypeEET_(%struct.TemplatedConstructor* nonnull dereferenceable(4) {{.*}}, i32 {{.*}})
  
  // ITANIUM_ARM-LABEL: define protected swiftcc void @"$ss26createTemplatedConstructoryyF"()
  // ITANIUM_ARM: [[OBJ:%.*]] = alloca %TSo20TemplatedConstructorV
  // ITANIUM_ARM: [[IVAL:%.*]] = load [1 x i32], [1 x i32]*
  // ITANIUM_ARM: [[OBJ_AS_STRUCT:%.*]] = bitcast %TSo20TemplatedConstructorV* [[OBJ]] to %struct.TemplatedConstructor*
  // ITANIUM_ARM:  call %struct.TemplatedConstructor* @_ZN20TemplatedConstructorC2I7ArgTypeEET_(%struct.TemplatedConstructor* [[OBJ_AS_STRUCT]], [1 x i32] [[IVAL]])
  // ITANIUM_ARM: ret void
  
  // ITANIUM_ARM-LABEL: define linkonce_odr %struct.TemplatedConstructor* @_ZN20TemplatedConstructorC2I7ArgTypeEET_(%struct.TemplatedConstructor* nonnull returned dereferenceable(4) {{.*}}, [1 x i32] {{.*}})

  // MICROSOFT_X64-LABEL: define dllexport swiftcc void @"$ss26createTemplatedConstructoryyF"()
  // MICROSOFT_X64: [[OBJ:%.*]] = alloca %TSo20TemplatedConstructorV
  // MICROSOFT_X64: [[IVAL:%.*]] = load i32, i32*
  // MICROSOFT_X64: [[OBJ_AS_STRUCT:%.*]] = bitcast %TSo20TemplatedConstructorV* [[OBJ]] to %struct.TemplatedConstructor*
  // MICROSOFT_X64: call %struct.TemplatedConstructor* @"??$?0UArgType@@@TemplatedConstructor@@QEAA@UArgType@@@Z"(%struct.TemplatedConstructor* [[OBJ_AS_STRUCT]], i32 [[IVAL]])
  // MICROSOFT_X64: ret void
  
  // MICROSOFT_X64-LABEL: define linkonce_odr dso_local %struct.TemplatedConstructor* @"??$?0UArgType@@@TemplatedConstructor@@QEAA@UArgType@@@Z"(%struct.TemplatedConstructor* nonnull returned dereferenceable(4) {{.*}}, i32 {{.*}})
  let templated = TemplatedConstructor(ArgType())
}
