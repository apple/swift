// RUN: %empty-directory(%t)

// Module A code generation.
// RUN: %target-swift-frontend %use_no_opaque_pointers -emit-ir -primary-file %S/Inputs/pre_specialize_module.swift -module-name A | %FileCheck %s -check-prefix=CHECK-A -check-prefix=CHECK-A-FRAG
// RUN: %target-swift-frontend %use_no_opaque_pointers -O -emit-ir -primary-file %S/Inputs/pre_specialize_module.swift -module-name A | %FileCheck %s -check-prefix=CHECK-A -check-prefix=CHECK-A-FRAG
// RUN: %target-swift-frontend %use_no_opaque_pointers -enable-library-evolution -emit-ir -primary-file %S/Inputs/pre_specialize_module.swift -module-name A | %FileCheck %s -check-prefix=CHECK-A -check-prefix=CHECK-A-RES
// RUN: %target-swift-frontend %use_no_opaque_pointers -O -enable-library-evolution -emit-ir -primary-file %S/Inputs/pre_specialize_module.swift -module-name A | %FileCheck %s -check-prefix=CHECK-A -check-prefix=CHECK-A-RES
// RUN: %target-swift-frontend -emit-ir -primary-file %S/Inputs/pre_specialize_module.swift -module-name A
// RUN: %target-swift-frontend -O -emit-ir -primary-file %S/Inputs/pre_specialize_module.swift -module-name A
// RUN: %target-swift-frontend -enable-library-evolution -emit-ir -primary-file %S/Inputs/pre_specialize_module.swift -module-name A
// RUN: %target-swift-frontend -O -enable-library-evolution -emit-ir -primary-file %S/Inputs/pre_specialize_module.swift -module-name A

// Module B code generation with A.swiftmodule.
// RUN: %empty-directory(%t)
// RUN: %target-build-swift -Xfrontend -validate-tbd-against-ir=missing -emit-module -emit-module-path=%t/A.swiftmodule -module-name A %S/Inputs/pre_specialize_module.swift -emit-library -o %t/%target-library-name(A)
// RUN: %target-swift-frontend %use_no_opaque_pointers -I %t -emit-ir -primary-file %S/Inputs/pre_specialize_module_B.swift -module-name B | %FileCheck %s -check-prefix=CHECK-B
// RUN: %target-swift-frontend %use_no_opaque_pointers -I %t -O -emit-ir -primary-file %S/Inputs/pre_specialize_module_B.swift -module-name B | %FileCheck %s -check-prefix=CHECK-B
// RUN: %target-swift-frontend -I %t -emit-ir -primary-file %S/Inputs/pre_specialize_module_B.swift -module-name B
// RUN: %target-swift-frontend -I %t -O -emit-ir -primary-file %S/Inputs/pre_specialize_module_B.swift -module-name B
// RUN: %target-build-swift -I %t -Xfrontend -validate-tbd-against-ir=missing -emit-module -emit-module-path=%t/B.swiftmodule -module-name B %S/Inputs/pre_specialize_module_B.swift -emit-library -o %t/%target-library-name(B) -L %t -lA
// RUN: %target-build-swift -swift-version 5 -I %t -Xfrontend -validate-tbd-against-ir=all -enable-library-evolution -emit-module-interface-path %t/B.swiftinterface -module-name B %S/Inputs/pre_specialize_module_B.swift -emit-library -o %t/%target-library-name(B) -L %t -lA

// Module B code generation with A.swiftmodule with library evolution.
// RUN: %empty-directory(%t)
// RUN: %target-build-swift -enable-library-evolution -Xfrontend -validate-tbd-against-ir=all -emit-module -emit-module-path=%t/A.swiftmodule -module-name A %S/Inputs/pre_specialize_module.swift -emit-library -o %t/%target-library-name(A)
// RUN: %target-swift-frontend %use_no_opaque_pointers -I %t -emit-ir -primary-file %S/Inputs/pre_specialize_module_B.swift -module-name B | %FileCheck %s -check-prefix=CHECK-B
// RUN: %target-swift-frontend %use_no_opaque_pointers -I %t -O -emit-ir -primary-file %S/Inputs/pre_specialize_module_B.swift -module-name B | %FileCheck %s -check-prefix=CHECK-B
// RUN: %target-swift-frontend -I %t -emit-ir -primary-file %S/Inputs/pre_specialize_module_B.swift -module-name B
// RUN: %target-swift-frontend -I %t -O -emit-ir -primary-file %S/Inputs/pre_specialize_module_B.swift -module-name B
// RUN: %target-build-swift -I %t -Xfrontend -validate-tbd-against-ir=missing -emit-module -emit-module-path=%t/B.swiftmodule -module-name B %S/Inputs/pre_specialize_module_B.swift -emit-library -o %t/%target-library-name(B) -L %t -lA
// RUN: %target-build-swift -swift-version 5 -I %t -Xfrontend -validate-tbd-against-ir=all -enable-library-evolution -emit-module-interface-path %t/B.swiftinterface -module-name B %S/Inputs/pre_specialize_module_B.swift -emit-library -o %t/%target-library-name(B) -L %t -lA

// Module B code generation with A.swiftinterface with library evolution.
// RUN: %empty-directory(%t)
// RUN: %target-build-swift -enable-library-evolution -Xfrontend -validate-tbd-against-ir=all -emit-module-interface-path %t/A.swiftinterface -module-name A %S/Inputs/pre_specialize_module.swift -emit-library -o %t/%target-library-name(A) -swift-version 5
// RUN: %target-swift-frontend %use_no_opaque_pointers -I %t -emit-ir -primary-file %S/Inputs/pre_specialize_module_B.swift -module-name B | %FileCheck %s -check-prefix=CHECK-B
// RUN: %target-swift-frontend %use_no_opaque_pointers -I %t -O -emit-ir -primary-file %S/Inputs/pre_specialize_module_B.swift -module-name B | %FileCheck %s -check-prefix=CHECK-B
// RUN: %target-swift-frontend -I %t -emit-ir -primary-file %S/Inputs/pre_specialize_module_B.swift -module-name B
// RUN: %target-swift-frontend -I %t -O -emit-ir -primary-file %S/Inputs/pre_specialize_module_B.swift -module-name B
// RUN: %target-build-swift -I %t -Xfrontend -validate-tbd-against-ir=missing -emit-module -emit-module-path=%t/B.swiftmodule -module-name B %S/Inputs/pre_specialize_module_B.swift -emit-library -o %t/%target-library-name(B) -L %t -lA
// RUN: %target-build-swift -swift-version 5 -I %t -Xfrontend -validate-tbd-against-ir=all -enable-library-evolution -emit-module-interface-path %t/B.swiftinterface -module-name B %S/Inputs/pre_specialize_module_B.swift -emit-library -o %t/%target-library-name(B) -L %t -lA

// Module A tests
// --------------

// specialized InternalThing.compute()
// CHECK-A-FRAG-DAG: define{{( dllexport)?}}{{( protected)?}} swiftcc [[INT:(i64|i32)]] @"$s1A13InternalThingV7computexyFAA09Resilienta5BoxedB0VySiG_Ts5"({{(i64|i32)}}{{( returned)?}} %0)
// CHECK-A-RES-DAG: define{{( dllexport)?}}{{( protected)?}} swiftcc void @"$s1A13InternalThingV7computexyFAA09Resilienta5BoxedB0VySiG_Ts5"(%T1A27ResilientInternalBoxedThingVySiG* {{.*}}sret({{.*}}){{.*}} %0, [[INT:(i64|i32)]] %1)
// CHECK-A-DAG: define{{( dllexport)?}}{{( protected)?}} swiftcc i1 @"$s1A13InternalThingV7computexyFSb_Ts5"(i1{{( returned)?}} %0)
// CHECK-A-DAG: define{{( dllexport)?}}{{( protected)?}} swiftcc [[INT]] @"$s1A13InternalThingV7computexyFSi_Ts5"([[INT]]{{( returned)?}} %0)

// specialized InternalThing.computedX.getter
// CHECK-A-DAG: define{{( dllexport)?}}{{( protected)?}} swiftcc [[INT]] @"$s1A13InternalThingV9computedXxvgSi_Ts5"([[INT]]{{( returned)?}} %0)
// specialized InternalThing.computedX.setter
// CHECK-A-DAG: define{{( dllexport)?}}{{( protected)?}} swiftcc void @"$s1A13InternalThingV9computedXxvsSi_Ts5"([[INT]] %0, %T1A13InternalThingVySiG* nocapture swiftself {{(writeonly )?}}dereferenceable({{(4|8)}}) %1)

// specialized InternalThing.subscript.getter
// CHECK-A-DAG: define{{( dllexport)?}}{{( protected)?}} swiftcc [[INT]] @"$s1A13InternalThingVyxSicigSi_Ts5"([[INT]] %0, [[INT]]{{( returned)?}} %1)
// specialized InternalThing.subscript.setter
// CHECK-A-DAG: define{{( dllexport)?}}{{( protected)?}} swiftcc void @"$s1A13InternalThingVyxSicisSi_Ts5"([[INT]] %0, [[INT]] %1, %T1A13InternalThingVySiG* nocapture swiftself {{(writeonly )?}}dereferenceable({{(4|8)}}) %2)

// specialized InternalRef.compute()
// CHECK-A-FRAG-DAG: define{{( dllexport)?}}{{( protected)?}} swiftcc [[INT:(i64|i32)]] @"$s1A11InternalRefC7computexyFAA09ResilientA10BoxedThingVySiG_Ts5"
// CHECK-A-RES-DAG: define{{( dllexport)?}}{{( protected)?}} swiftcc void @"$s1A11InternalRefC7computexyFAA09ResilientA10BoxedThingVySiG_Ts5"
// CHECK-A-DAG: define{{( dllexport)?}}{{( protected)?}} swiftcc i1 @"$s1A11InternalRefC7computexyFSb_Ts5"
// CHECK-A-DAG: define{{( dllexport)?}}{{( protected)?}} swiftcc [[INT]] @"$s1A11InternalRefC7computexyFSi_Ts5"

// specialized InternalRef.computedX.getter
// CHECK-A-DAG: define{{( dllexport)?}}{{( protected)?}} swiftcc [[INT]] @"$s1A11InternalRefC9computedXxvgSi_Ts5"
// specialized InternalRef.computedX.setter
// CHECK-A-DAG: define{{( dllexport)?}}{{( protected)?}} swiftcc void @"$s1A11InternalRefC9computedXxvsSi_Ts5"

// specialized InternalRef.subscript.getter
// CHECK-A-DAG: define{{( dllexport)?}}{{( protected)?}} swiftcc [[INT]] @"$s1A11InternalRefCyxSicigSi_Ts5"
// specialized InternalRef.subscript.setter
// CHECK-A-DAG: define{{( dllexport)?}}{{( protected)?}} swiftcc void @"$s1A11InternalRefCyxSicisSi_Ts5"

// Module B tests
// --------------

// specialized InternalThing.compute()
// CHECK-B-DAG: define{{( dllexport)?}}{{( protected)?}} swiftcc {{(%T1B12AnotherThingC\*|void)}} @"$s1A13InternalThingV7computexyFAA09Resilienta5BoxedB0Vy1B07AnotherB0CG_Ts5"
// CHECK-B-DAG: define{{( dllexport)?}}{{( protected)?}} swiftcc %T1B12AnotherThingC* @"$s1A13InternalThingV7computexyF1B07AnotherB0C_Ts5"(%T1B12AnotherThingC*{{( returned)?}} %0)

// specialized InternalThing.computedX.getter
// CHECK-B-DAG: define{{( dllexport)?}}{{( protected)?}} swiftcc %T1B12AnotherThingC* @"$s1A13InternalThingV9computedXxvg1B07AnotherB0C_Ts5"(%T1B12AnotherThingC*{{( returned)?}} %0)

// specialized InternalThing.computedX.setter
// CHECK-B-DAG: define{{( dllexport)?}}{{( protected)?}} swiftcc void @"$s1A13InternalThingV9computedXxvs1B07AnotherB0C_Ts5"(%T1B12AnotherThingC* %0, %T1A13InternalThingVy1B07AnotherB0CG* nocapture swiftself dereferenceable({{(4|8)}}) %1)

// specialized InternalThing.subscript.getter
// CHECK-B-DAG: define{{( dllexport)?}}{{( protected)?}} swiftcc %T1B12AnotherThingC* @"$s1A13InternalThingVyxSicig1B07AnotherB0C_Ts5"([[INT:(i64|i32)]] %0, %T1B12AnotherThingC*{{( returned)?}} %1)

// specialized InternalThing.subscript.setter
// CHECK-B-DAG: define{{( dllexport)?}}{{( protected)?}} swiftcc void @"$s1A13InternalThingVyxSicis1B07AnotherB0C_Ts5"(%T1B12AnotherThingC* %0, [[INT]] %1, %T1A13InternalThingVy1B07AnotherB0CG* nocapture swiftself dereferenceable({{(4|8)}}) %2)

// specialized InternalRef.compute()
// CHECK-B-DAG: define{{( dllexport)?}}{{( protected)?}} swiftcc %T1B12AnotherThingC* @"$s1A11InternalRefC7computexyF1B12AnotherThingC_Ts5

// specialized InternalRef.computedX.getter
// CHECK-B-DAG: define{{( dllexport)?}}{{( protected)?}} swiftcc %T1B12AnotherThingC* @"$s1A11InternalRefC9computedXxvg1B12AnotherThingC_Ts5"

// specialized InternalRef.computedX.setter
// CHECK-B-DAG: define{{( dllexport)?}}{{( protected)?}} swiftcc void @"$s1A11InternalRefC9computedXxvs1B12AnotherThingC_Ts5"

// specialized InternalRef.subscript.getter
// CHECK-B-DAG: define{{( dllexport)?}}{{( protected)?}} swiftcc %T1B12AnotherThingC* @"$s1A11InternalRefCyxSicig1B12AnotherThingC_Ts5"

// specialized InternalRef.subscript.setter
// CHECK-B-DAG: define{{( dllexport)?}}{{( protected)?}} swiftcc void @"$s1A11InternalRefCyxSicis1B12AnotherThingC_Ts5"


// Test pre-specialized use.

// Fragile .swiftmodule
// RUN: %empty-directory(%t)
// RUN: %target-build-swift -emit-module -emit-module-path=%t/A.swiftmodule -module-name A %S/Inputs/pre_specialize_module.swift
// RUN: %target-build-swift -I %t -emit-module -emit-module-path=%t/B.swiftmodule -module-name B %S/Inputs/pre_specialize_module_B.swift
// RUN: %target-swift-frontend %use_no_opaque_pointers -O -I %t -emit-ir -primary-file %s -module-name C | %FileCheck %s -check-prefix=CHECK-C
// RUN: %target-swift-frontend -O -I %t -emit-ir -primary-file %s -module-name C

// Fragile optimized .swiftmodule
// RUN: %empty-directory(%t)
// RUN: %target-build-swift -O -emit-module -emit-module-path=%t/A.swiftmodule -module-name A %S/Inputs/pre_specialize_module.swift
// RUN: %target-build-swift -O -I %t -emit-module -emit-module-path=%t/B.swiftmodule -module-name B %S/Inputs/pre_specialize_module_B.swift
// RUN: %target-swift-frontend %use_no_opaque_pointers -O -I %t -emit-ir -primary-file %s -module-name C | %FileCheck %s -check-prefix=CHECK-C
// RUN: %target-swift-frontend -O -I %t -emit-ir -primary-file %s -module-name C

// Resilient .swiftmodule
// RUN: %empty-directory(%t)
// RUN: %target-build-swift -enable-library-evolution -emit-module -emit-module-path=%t/A.swiftmodule -module-name A %S/Inputs/pre_specialize_module.swift
// RUN: %target-build-swift -enable-library-evolution -I %t -emit-module -emit-module-path=%t/B.swiftmodule -module-name B %S/Inputs/pre_specialize_module_B.swift
// RUN: %target-swift-frontend %use_no_opaque_pointers -O -I %t -emit-ir -primary-file %s -module-name C | %FileCheck %s -check-prefix=CHECK-C
// RUN: %target-swift-frontend -O -I %t -emit-ir -primary-file %s -module-name C

// Resilient optimized .swiftmodule
// RUN: %empty-directory(%t)
// RUN: %target-build-swift -O -enable-library-evolution -emit-module -emit-module-path=%t/A.swiftmodule -module-name A %S/Inputs/pre_specialize_module.swift
// RUN: %target-build-swift -O -enable-library-evolution -I %t -emit-module -emit-module-path=%t/B.swiftmodule -module-name B %S/Inputs/pre_specialize_module_B.swift
// RUN: %target-swift-frontend %use_no_opaque_pointers -O -I %t -emit-ir -primary-file %s -module-name C | %FileCheck %s -check-prefix=CHECK-C
// RUN: %target-swift-frontend -O -I %t -emit-ir -primary-file %s -module-name C

// .swiftinterface
// RUN: %empty-directory(%t)
// RUN: %target-build-swift -c -enable-library-evolution -emit-module-interface-path %t/A.swiftinterface -module-name A %S/Inputs/pre_specialize_module.swift -o  %t/A.o -swift-version 5
// RUN: %target-build-swift -c -enable-library-evolution -I %t -emit-module-interface-path %t/B.swiftinterface -module-name B %S/Inputs/pre_specialize_module_B.swift -o %t/B.o -swift-version 5
// RUN: %target-swift-frontend %use_no_opaque_pointers -O -I %t -emit-ir -primary-file %s -module-name C | %FileCheck %s -check-prefix=CHECK-C
// RUN: %target-swift-frontend -O -I %t -emit-ir -primary-file %s -module-name C

// Optimized .swiftinterface
// RUN: %empty-directory(%t)
// RUN: %target-build-swift -O -c -enable-library-evolution -emit-module-interface-path %t/A.swiftinterface -module-name A %S/Inputs/pre_specialize_module.swift -o  %t/A.o -swift-version 5
// RUN: %target-build-swift -O -c -enable-library-evolution -I %t -emit-module-interface-path %t/B.swiftinterface -module-name B %S/Inputs/pre_specialize_module_B.swift -o %t/B.o -swift-version 5
// RUN: %target-swift-frontend %use_no_opaque_pointers -O -I %t -emit-ir -primary-file %s -module-name C | %FileCheck %s -check-prefix=CHECK-C
// RUN: %target-swift-frontend -O -I %t -emit-ir -primary-file %s -module-name C

import A
import B


public func testPrespecializedUse() {

  // Test pre-specialization in library A.

  // CHECK-C-LABEL: define{{.*}}s1A18testSpecializationyyxlFSi_Tg5
  // CHECK-C: call{{.*}}s1A13InternalThingV7computexyFAA09Resilienta5BoxedB0VySiG_Ts5
  // CHECK-C: call{{.*}}s1A13InternalThingV7computexyFSi_Ts5
  // CHECK-C: call{{.*}}s1A13InternalThingV9computedXxvsSi_Ts5
  // CHECK-C: call{{.*}}s1A13InternalThingV9computedXxvgSi_Ts5
  // CHECK-C: call{{.*}}s1A13InternalThingVyxSicisSi_Ts5
  // CHECK-C: call{{.*}}s1A13InternalThingVyxSicigSi_Ts5

  testSpecialization(5)

  // Test pre-specialization in library B.

  // CHECK-C-LABEL: define{{.*}}s1A18testSpecializationyyxlF1B12AnotherThingC_Tg5
  // CHECK-C: call{{.*}}s1A13InternalThingV7computexyFAA09Resilienta5BoxedB0Vy1B07AnotherB0CG_Ts5
  // CHECK-C: call{{.*}}s1A13InternalThingV7computexyF1B07AnotherB0C_Ts5
  // CHECK-C: call{{.*}}s1A13InternalThingV9computedXxvs1B07AnotherB0C_Ts5
  // CHECK-C: call{{.*}}s1A13InternalThingV9computedXxvg1B07AnotherB0C_Ts5
  // CHECK-C: call{{.*}}s1A13InternalThingVyxSicis1B07AnotherB0C_Ts5
  // CHECK-C: call{{.*}}s1A13InternalThingVyxSicig1B07AnotherB0C_Ts5

  testSpecialization(AnotherThing())
}
