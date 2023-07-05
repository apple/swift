// Target-specific tests for C++ constructor call code generation.

// RUN: %swift -module-name MySwift -target x86_64-apple-macosx10.9 -dump-clang-diagnostics -I %S/Inputs -enable-experimental-cxx-interop -emit-ir %s -parse-stdlib -parse-as-library -disable-legacy-type-info -Xcc -fignore-exceptions | %FileCheck %s -check-prefix=ITANIUM_X64

// REQUIRES: OS=macosx
// REQUIRES: CPU=x86_64

import Constructors
import TypeClassification

public func createHasVirtualBase() -> HasVirtualBase {
  // ITANIUM_X64: define swiftcc void @"$s7MySwift20createHasVirtualBaseSo0deF0VyF"(ptr noalias nocapture sret({{.*}}) %0)
  // ITANIUM_X64-NOT: define
  // ITANIUM_X64: call void @_ZN14HasVirtualBaseC1E7ArgType(ptr %{{[0-9]+}}, i32 %{{[0-9]+}})
  return HasVirtualBase(ArgType())
}

public func createImplicitDefaultConstructor() -> ImplicitDefaultConstructor {
  // ITANIUM_X64: define swiftcc i32 @"$s7MySwift32createImplicitDefaultConstructorSo0deF0VyF"()
  // ITANIUM_X64-NOT: define
  // ITANIUM_X64: call void @_ZN26ImplicitDefaultConstructorC1Ev(ptr %{{[0-9]+}})
  return ImplicitDefaultConstructor()
}

public func createStructWithSubobjectCopyConstructorAndValue() {
  // ITANIUM_X64-LABEL: define swiftcc void @"$s7MySwift48createStructWithSubobjectCopyConstructorAndValueyyF"()
  // ITANIUM_X64: [[MEMBER:%.*]] = alloca %TSo33StructWithCopyConstructorAndValueV
  // ITANIUM_X64: [[OBJ:%.*]] = alloca %TSo42StructWithSubobjectCopyConstructorAndValueV
  // ITANIUM_X64: [[TMP:%.*]] = alloca %TSo33StructWithCopyConstructorAndValueV
  // ITANIUM_X64: void @_ZN33StructWithCopyConstructorAndValueC1Ev(ptr [[MEMBER]])
  // ITANIUM_X64: call void @_ZN33StructWithCopyConstructorAndValueC1ERKS_(ptr [[TMP]], ptr [[MEMBER]])
  // ITANIUM_X64: ret void
  let member = StructWithCopyConstructorAndValue()
  let obj = StructWithSubobjectCopyConstructorAndValue(member: member)
}

public func createTemplatedConstructor() {
  // ITANIUM_X64-LABEL: define swiftcc void @"$s7MySwift26createTemplatedConstructoryyF"()
  // ITANIUM_X64: [[OBJ:%.*]] = alloca %TSo20TemplatedConstructorV
  // ITANIUM_X64: [[IVAL:%.*]] = load i32, ptr
  // ITANIUM_X64: call void @_ZN20TemplatedConstructorC1I7ArgTypeEET_(ptr [[OBJ]], i32 [[IVAL]])
  // ITANIUM_X64: ret void
  
  // ITANIUM_X64-LABEL: define {{.*}}void @_ZN20TemplatedConstructorC1I7ArgTypeEET_(ptr {{.*}}, i32 {{.*}})
  let templated = TemplatedConstructor(ArgType())
}
