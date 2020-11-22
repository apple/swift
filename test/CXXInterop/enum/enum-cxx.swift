// RUN: %target-swift-frontend -emit-ir -primary-file %s -I %S/../../ClangImporter/Inputs/ -module-cache-path %t -enable-cxx-interop -o - | %FileCheck %s

import CXXInterop

// CHECK-LABEL: define hidden swiftcc i8 @"$s4main19testEmptyEnumImport5values4Int8VSo04OpencD0V_tF"(i8 %0)
// CHECK:  %1 = call swiftcc i8 @"$sSo13OpenEmptyEnumV8rawValues4Int8Vvg"(i8 %0)
// CHECK:  ret i8 %1
func testEmptyEnumImport(value: OpenEmptyEnum) -> Int8 {
  // Should still compile even though the enum is uninhabited in c++.
  return value.rawValue
}
