// RUN: %empty-directory(%t)
// RUN: %target-build-swift %s -o %t/a.out
// RUN: %target-codesign %t/a.out
// RUN: %target-run %t/a.out | %FileCheck %s
// RUN: %target-swift-frontend -primary-file %s -emit-ir | %FileCheck -check-prefix=CHECKIR %s
// REQUIRES: executable_test

protocol Proto {
}

struct Mystruct : Proto {
}

// CHECKIR-LABEL: define hidden {{.*}} @"$s05meta_A5_type6testityAA5Proto_pXpXpAaC_pF"
// CHECKIR: [[M1:%[0-9]+]] = call {{.*}} @swift_getDynamicType
// CHECKIR: [[M2:%[0-9]+]] = call {{.*}} @swift_getMetatypeMetadata(ptr [[M1]])
// CHECKIR: [[R1:%[0-9]+]] = insertvalue {{.*}} [[M2]]
// CHECKIR: [[R2:%[0-9]+]] = insertvalue {{.*}} [[R1]]
// CHECKIR: ret { ptr, ptr } [[R2]]
func testit(_ p: Proto) -> Proto.Type.Type {
  return type(of: type(of: p))
}

// CHECKIR-LABEL: define hidden {{.*}} @"$s05meta_A5_type7testit2yAA5Proto_pXpXpXpAaC_pF"
// CHECKIR: [[M1:%[0-9]+]] = call {{.*}} @swift_getDynamicType
// CHECKIR: [[M2:%[0-9]+]] = call {{.*}} @swift_getMetatypeMetadata(ptr [[M1]])
// CHECKIR: [[M3:%[0-9]+]] = call {{.*}} @swift_getMetatypeMetadata(ptr [[M2]])
// CHECKIR: [[R1:%[0-9]+]] = insertvalue {{.*}} [[M3]]
// CHECKIR: [[R2:%[0-9]+]] = insertvalue {{.*}} [[R1]]
// CHECKIR: ret { ptr, ptr } [[R2]]
func testit2(_ p: Proto) -> Proto.Type.Type.Type {
  return type(of: type(of: type(of: p)))
}

var tt = testit(Mystruct())
var tt2 = testit2(Mystruct())

// CHECK: a.Mystruct.Type
debugPrint(tt)

// CHECK: a.Mystruct.Type.Type
debugPrint(tt2)
