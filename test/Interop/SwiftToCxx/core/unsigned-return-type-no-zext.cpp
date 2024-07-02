// RUN: %empty-directory(%t)

// RUN: %target-interop-build-clangxx -c %s -I %t -S -emit-llvm -o %t/ir.ll
// RUN: %FileCheck %s < %t/ir.ll

// UNSUPPORTED: OS=windows-msvc

unsigned char getEnumTagi8(void *p);
unsigned getEnumTagi32(void *p);

void test(void *p) {
  getEnumTagi8(p);
  getEnumTagi32(p);
}

// NOTE: it's important to verify that i32 function does not zeroext/signext return value.

// CHECK: declare{{( noundef)?}}{{( zeroext)?}} i8 @_Z12getEnumTagi8Pv(ptr noundef)
// CHECK: declare{{( noundef)?}} i32 @_Z13getEnumTagi32Pv(ptr noundef)
