// First test: functional correctness

// RUN: %empty-directory(%t) 
// RUN: %target-build-swift -O -wmo -parse-as-library -cross-module-optimization -emit-module -emit-module-path=%t/Submodule.swiftmodule -module-name=Submodule %S/Inputs/cross-module/cross-submodule.swift -c -o %t/submodule.o
// RUN: %target-build-swift -O -wmo -parse-as-library -cross-module-optimization -emit-module -emit-module-path=%t/PrivateSubmodule.swiftmodule -module-name=PrivateSubmodule %S/Inputs/cross-module/cross-private-submodule.swift -c -o %t/privatesubmodule.o
// RUN: %target-clang -c --language=c %S/Inputs/cross-module/c-module.c -o %t/c-module.o
// RUN: %target-build-swift -O -wmo -parse-as-library -cross-module-optimization -emit-module -emit-module-path=%t/Test.swiftmodule -module-name=Test -I%t -I%S/Inputs/cross-module %S/Inputs/cross-module/cross-module.swift -c -o %t/test.o
// RUN: %target-build-swift -O -wmo -module-name=Main -I%t %s -c -o %t/main.o
// RUN: %target-swiftc_driver %t/main.o %t/test.o %t/submodule.o %t/privatesubmodule.o %t/c-module.o -o %t/a.out
// RUN: %target-codesign %t/a.out
// RUN: %target-run %t/a.out | %FileCheck %s -check-prefix=CHECK-OUTPUT

// Check if it also works if the main module is compiled with -Onone:

// RUN: %target-build-swift -Onone -wmo -module-name=Main -I%t %s -c -o %t/main-onone.o
// RUN: %target-swiftc_driver %t/main-onone.o %t/test.o %t/submodule.o %t/privatesubmodule.o %t/c-module.o -o %t/a.out
// RUN: %target-codesign %t/a.out
// RUN: %target-run %t/a.out | %FileCheck %s -check-prefix=CHECK-OUTPUT

// REQUIRES: executable_test
// REQUIRES: swift_in_compiler

// Second test: check if CMO really imports the SIL of functions in other modules.

// RUN: %target-build-swift -O -wmo -module-name=Main -I%t %s -Xllvm -sil-disable-pass=FunctionSignatureOpts -emit-sil -o %t/out.sil
// RUN: %FileCheck %s -check-prefix=CHECK-SIL < %t/out.sil
// RUN: %FileCheck %s -check-prefix=CHECK-SIL2 < %t/out.sil

import Test

// CHECK-SIL: sil_global public_external [serialized] @_swiftEmptySetSingleton : $_SwiftEmptySetSingleton

func testNestedTypes() {
  let c = Container()

  // CHECK-OUTPUT [Test.Container.Base]
  // CHECK-OUTPUT: 27
  // CHECK-SIL-DAG: sil shared [noinline] @$s4Test9ContainerV9testclassyxxlFSi_Tg5
  print(c.testclass(27))
  // CHECK-OUTPUT [Test.Container.Base]
  // CHECK-OUTPUT: 27
  // CHECK-SIL-DAG: sil public_external {{.*}} @$s4Test9ContainerV13testclass_genyxxlF
  print(c.testclass_gen(27))
  // CHECK-OUTPUT [Test.PE<Swift.Int>.B(27)]
  // CHECK-OUTPUT: 27
  // CHECK-SIL-DAG: sil shared [noinline] @$s4Test9ContainerV8testenumyxxlFSi_Tg5
  print(c.testenum(27))
  // CHECK-OUTPUT [Test.PE<Swift.Int>.B(27)]
  // CHECK-OUTPUT: 27
  // CHECK-SIL-DAG: sil public_external {{.*}} @$s4Test9ContainerV12testenum_genyxxlF
  print(c.testenum_gen(27))
}


func testClass() {
  // CHECK-OUTPUT: 28
  // CHECK-SIL-DAG: sil shared [noinline] @$s4Test11createClassySixlFSi_Tg5
  // CHECK-SIL-DAG: sil shared [noinline] @${{.*Test.*getClass}}
  print(createClass(0))
  // CHECK-OUTPUT: 28
  // CHECK-SIL-DAG: sil public_external {{.*}} @$s4Test15createClass_genySixlF
  print(createClass_gen(0))
}

// CHECK-SIL2-LABEL: sil hidden [noinline] @$s4Main9testErroryyF
@inline(never)
func testError() {
  // CHECK-OUTPUT: PrivateError()
  // CHECK-SIL2: struct $PrivateError ()
  // CHECK-SIL2: alloc_existential_box $any Error, $PrivateError
  print(returnPrivateError(27))
  // CHECK-OUTPUT: InternalError()
  // CHECK-SIL2: struct $InternalError ()
  // CHECK-SIL2: alloc_existential_box $any Error, $InternalError
  print(returnInternalError(27))
  // CHECK-SIL2: } // end sil function '$s4Main9testErroryyF'
}

class DerivedFromOpen<T> : OpenClass<T> { }

func testProtocolsAndClasses() {
  // CHECK-OUTPUT: false
  // CHECK-SIL-DAG: sil shared [noinline] @$s4Test20checkIfClassConformsyyxlFSi_Tg5
  checkIfClassConforms(27)
  // CHECK-OUTPUT: false
  // CHECK-SIL-DAG: sil public_external {{.*}} @$s4Test24checkIfClassConforms_genyyxlF
  checkIfClassConforms_gen(27)
  // CHECK-OUTPUT: 123
  // CHECK-OUTPUT: 1234
  // CHECK-SIL-DAG: sil shared [noinline] @$s4Test7callFooyyxlFSi_Tg5
  // CHECK-SIL-DAG: sil [{{.*}}] @$s4Test19printFooExistential33_{{.*}} : $@convention(thin)
  callFoo(27)
  // CHECK-OUTPUT: 123
  // CHECK-OUTPUT: 1234
  // CHECK-SIL-DAG: sil public_external {{.*}} @$s4Test11callFoo_genyyxlF
  callFoo_gen(27)
  // CHECK-OUTPUT: 55
  callClassMethod(55)
  // CHECK-OUTPUT: 321
  callFooViaConformance(0)
}

func testSubModule() {
  // CHECK-OUTPUT: 10
  // CHECK-SIL-DAG: sil shared [noinline] @$s4Test24callGenericSubmoduleFuncyyxlFSi_Tg5
  // CHECK-SIL-DAG: sil shared [noinline] @$s9Submodule07genericA4FuncyyxlF
  callGenericSubmoduleFunc(10)
  // CHECK-OUTPUT: 101
  // CHECK-SIL-DAG: sil public_external {{.*}} @$s4Test28callGenericSubmoduleFunc_genyyxlF
  callGenericSubmoduleFunc_gen(101)
}

func testClosures() {
  // CHECK-OUTPUT: 23
  // CHECK-SIL-DAG: sil shared [noinline] @$s4Test14genericClosureyxxlFSi_Tg5
  print(genericClosure(23))
  // CHECK-OUTPUT: 24
  // CHECK-SIL-DAG: sil public_external {{.*}} @$s4Test18genericClosure_genyxxlF
  print(genericClosure_gen(24))
}

func testKeypath() {
  // CHECK-OUTPUT: 27
  print(useStructKeypath(0))
  // CHECK-OUTPUT: 29
  print(useClassKeypath(0))
}

func testMisc() {
  // CHECK-OUTPUT: 43
  // CHECK-OUTPUT: 42
  // CHECK-SIL-DAG: sil shared {{.*}} @$s4Test13callUnrelatedyxxlFSi_Tg5
  print(callUnrelated(42))

  // CHECK-OUTPUT: 27
  print(classWithPublicProperty(33))

  // CHECK-OUTPUT: []
  print(getEmptySet())
}

// CHECK-SIL2-LABEL: sil hidden [noinline] @$s4Main10testGlobalyyF
@inline(never)
func testGlobal() {
  // CHECK-OUTPUT: 529387
  // CHECK-SIL2: integer_literal $Builtin.Int{{[0-9]+}}, 529387
  print(globalLet)
  // CHECK-OUTPUT: 41
  print(StructWithClosure.c(41))
  // CHECK-SIL2: } // end sil function '$s4Main10testGlobalyyF'
}

// CHECK-SIL2-LABEL: sil hidden [noinline] @$s4Main22testImplementationOnlyyyF
@inline(never)
func testImplementationOnly() {
  // CHECK-OUTPUT: 27
  // CHECK-SIL2: function_ref @$s4Test26callImplementationOnlyTypeyxxlF
  print(callImplementationOnlyType(27))
  // CHECK-OUTPUT: 40
  // CHECK-SIL2: function_ref @$s4Test26callImplementationOnlyFuncySixlF
  print(callImplementationOnlyFunc(0))
  // CHECK-OUTPUT: 123
  // CHECK-SIL2: function_ref @$s4Test23callCImplementationOnlyySixlF
  print(callCImplementationOnly(0))
  // CHECK-SIL2: } // end sil function '$s4Main22testImplementationOnlyyyF'
}

testNestedTypes()
testClass()
testError()
testProtocolsAndClasses()
testSubModule()
testClosures()
testKeypath()
testMisc()
testGlobal()
testImplementationOnly()

