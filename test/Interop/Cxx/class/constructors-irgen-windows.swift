// Target-specific tests for C++ constructor call code generation.

// RUN: %swift -module-name MySwift -target x86_64-unknown-windows-msvc -dump-clang-diagnostics -I %S/Inputs -enable-experimental-cxx-interop -emit-ir %s -parse-stdlib -parse-as-library -disable-legacy-type-info | %FileCheck %s -check-prefix=MICROSOFT_X64

// REQUIRES: OS=windows-msvc
// REQUIRES: CPU=x86_64

import Constructors
import TypeClassification

public func createHasVirtualBase() -> HasVirtualBase {
  // MICROSOFT_X64: define dllexport swiftcc void @"$s7MySwift20createHasVirtualBaseSo0{{bcD0VyF|deF0VyF}}"(ptr noalias nocapture sret({{.*}}) %0)
  // MICROSOFT_X64-NOT: define
  // Note `this` return type and implicit "most derived" argument.
  // MICROSOFT_X64: call ptr @"??0HasVirtualBase@@QEAA@UArgType@@@Z"(ptr %{{[0-9]+}}, i32 %{{[0-9]+}}, i32 1)
  return HasVirtualBase(ArgType())
}

public func createImplicitDefaultConstructor() -> ImplicitDefaultConstructor {
  // MICROSOFT_X64: define dllexport swiftcc i32 @"$s7MySwift32createImplicitDefaultConstructorSo0{{bcD0VyF|deF0VyF}}"()
  // MICROSOFT_X64-NOT: define
  // Note `this` return type but no implicit "most derived" argument.
  // MICROSOFT_X64: call ptr @"??0ImplicitDefaultConstructor@@QEAA@XZ"(ptr %{{[0-9]+}})
  return ImplicitDefaultConstructor()
}

public func createStructWithSubobjectCopyConstructorAndValue() {
  // MICROSOFT_X64-LABEL: define dllexport swiftcc void @"$s7MySwift48createStructWithSubobjectCopyConstructorAndValueyyF"()
  // MICROSOFT_X64: [[MEMBER:%.*]] = alloca %TSo33StructWithCopyConstructorAndValueV
  // MICROSOFT_X64: [[OBJ:%.*]] = alloca %TSo42StructWithSubobjectCopyConstructorAndValueV
  // MICROSOFT_X64: [[TMP:%.*]] = alloca %TSo33StructWithCopyConstructorAndValueV
  // MICROSOFT_X64: call ptr @"??0StructWithCopyConstructorAndValue@@QEAA@XZ"(ptr [[MEMBER]])
  // MICROSOFT_X64: call ptr @"??0StructWithCopyConstructorAndValue@@QEAA@AEBU0@@Z"(ptr [[TMP]], ptr [[MEMBER]])
  // MICROSOFT_X64: ret void
  let member = StructWithCopyConstructorAndValue()
  let obj = StructWithSubobjectCopyConstructorAndValue(member: member)
}

public func createTemplatedConstructor() {
  // MICROSOFT_X64-LABEL: define dllexport swiftcc void @"$s7MySwift26createTemplatedConstructoryyF"()
  // MICROSOFT_X64: [[OBJ:%.*]] = alloca %TSo20TemplatedConstructorV
  // MICROSOFT_X64: [[IVAL:%.*]] = load i32, ptr
  // MICROSOFT_X64: call ptr @"??$?0UArgType@@@TemplatedConstructor@@QEAA@UArgType@@@Z"(ptr [[OBJ]], i32 [[IVAL]])
  // MICROSOFT_X64: ret void
  
  // MICROSOFT_X64-LABEL: define {{.*}}ptr @"??$?0UArgType@@@TemplatedConstructor@@QEAA@UArgType@@@Z"(ptr {{.*}}, i32 {{.*}})
  let templated = TemplatedConstructor(ArgType())
}
