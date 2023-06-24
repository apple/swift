// Target-specific tests for C++ constructor call code generation.

// RUN: %swift %use_no_opaque_pointers -module-name MySwift -target x86_64-unknown-windows-msvc -dump-clang-diagnostics -I %S/Inputs -enable-experimental-cxx-interop -emit-ir %s -parse-stdlib -parse-as-library -disable-legacy-type-info | %FileCheck %s -check-prefix=MICROSOFT_X64
// RUN: %swift -module-name MySwift -target x86_64-unknown-windows-msvc -dump-clang-diagnostics -I %S/Inputs -enable-experimental-cxx-interop -emit-ir %s -parse-stdlib -parse-as-library -disable-legacy-type-info

// REQUIRES: OS=windows-msvc
// REQUIRES: CPU=x86_64

import Constructors
import TypeClassification

public func createHasVirtualBase() -> HasVirtualBase {
  // MICROSOFT_X64: define dllexport swiftcc void @"$s7MySwift20createHasVirtualBaseSo0{{bcD0VyF|deF0VyF}}"(%TSo14HasVirtualBaseV* noalias nocapture sret({{.*}}) %0)
  // MICROSOFT_X64-NOT: define
  // Note `this` return type and implicit "most derived" argument.
  // MICROSOFT_X64: call %struct.HasVirtualBase* @"??0HasVirtualBase@@QEAA@UArgType@@@Z"(%struct.HasVirtualBase* %{{[0-9]+}}, i32 %{{[0-9]+}}, i32 1)
  return HasVirtualBase(ArgType())
}

public func createImplicitDefaultConstructor() -> ImplicitDefaultConstructor {
  // MICROSOFT_X64: define dllexport swiftcc i32 @"$s7MySwift32createImplicitDefaultConstructorSo0{{bcD0VyF|deF0VyF}}"()
  // MICROSOFT_X64-NOT: define
  // Note `this` return type but no implicit "most derived" argument.
  // MICROSOFT_X64: call %struct.ImplicitDefaultConstructor* @"??0ImplicitDefaultConstructor@@QEAA@XZ"(%struct.ImplicitDefaultConstructor* %{{[0-9]+}})
  return ImplicitDefaultConstructor()
}

public func createStructWithSubobjectCopyConstructorAndValue() {
  // MICROSOFT_X64-LABEL: define dllexport swiftcc void @"$s7MySwift48createStructWithSubobjectCopyConstructorAndValueyyF"()
  // MICROSOFT_X64: [[MEMBER:%.*]] = alloca %TSo33StructWithCopyConstructorAndValueV
  // MICROSOFT_X64: [[OBJ:%.*]] = alloca %TSo42StructWithSubobjectCopyConstructorAndValueV
  // MICROSOFT_X64: [[TMP:%.*]] = alloca %TSo33StructWithCopyConstructorAndValueV
  // MICROSOFT_X64: [[MEMBER_AS_STRUCT:%.*]] = bitcast %TSo33StructWithCopyConstructorAndValueV* [[MEMBER]] to %struct.StructWithCopyConstructorAndValue*
  // MICROSOFT_X64: call %struct.StructWithCopyConstructorAndValue* @"??0StructWithCopyConstructorAndValue@@QEAA@XZ"(%struct.StructWithCopyConstructorAndValue* [[MEMBER_AS_STRUCT]])
  // MICROSOFT_X64: [[TMP_STRUCT:%.*]] = bitcast %TSo33StructWithCopyConstructorAndValueV* [[TMP]] to %struct.StructWithCopyConstructorAndValue*
  // MICROSOFT_X64: [[MEMBER_AS_STRUCT_2:%.*]] = bitcast %TSo33StructWithCopyConstructorAndValueV* [[MEMBER]] to %struct.StructWithCopyConstructorAndValue*
  // MICROSOFT_X64: call %struct.StructWithCopyConstructorAndValue* @"??0StructWithCopyConstructorAndValue@@QEAA@AEBU0@@Z"(%struct.StructWithCopyConstructorAndValue* [[TMP_STRUCT]], %struct.StructWithCopyConstructorAndValue* [[MEMBER_AS_STRUCT_2]])
  // MICROSOFT_X64: ret void
  let member = StructWithCopyConstructorAndValue()
  let obj = StructWithSubobjectCopyConstructorAndValue(member: member)
}

public func createTemplatedConstructor() {
  // MICROSOFT_X64-LABEL: define dllexport swiftcc void @"$s7MySwift26createTemplatedConstructoryyF"()
  // MICROSOFT_X64: [[OBJ:%.*]] = alloca %TSo20TemplatedConstructorV
  // MICROSOFT_X64: [[IVAL:%.*]] = load i32, i32*
  // MICROSOFT_X64: [[OBJ_AS_STRUCT:%.*]] = bitcast %TSo20TemplatedConstructorV* [[OBJ]] to %struct.TemplatedConstructor*
  // MICROSOFT_X64: call %struct.TemplatedConstructor* @"??$?0UArgType@@@TemplatedConstructor@@QEAA@UArgType@@@Z"(%struct.TemplatedConstructor* [[OBJ_AS_STRUCT]], i32 [[IVAL]])
  // MICROSOFT_X64: ret void
  
  // MICROSOFT_X64-LABEL: define {{.*}}%struct.TemplatedConstructor* @"??$?0UArgType@@@TemplatedConstructor@@QEAA@UArgType@@@Z"(%struct.TemplatedConstructor* {{.*}}, i32 {{.*}})
  let templated = TemplatedConstructor(ArgType())
}
