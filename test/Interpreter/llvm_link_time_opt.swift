// UNSUPPORTED: OS=windows-msvc
// static library is not well supported yet on Windows

// UNSUPPORTED: OS=xros

// For LTO, the linker dlopen()'s the libLTO library, which is a scenario that
// ASan cannot work in ("Interceptors are not working, AddressSanitizer is
// loaded too late").
// REQUIRES: no_asan

// UNSUPPORTED: linker_overridden

// RUN: %empty-directory(%t)
// RUN: %use_just_built_liblto %target-swiftc_driver -emit-library -static -lto=llvm-full %lto_flags -emit-module %S/Inputs/lto/module1.swift -working-directory %t
// RUN: %use_just_built_liblto %target-swiftc_driver -lto=llvm-full %lto_flags %s -I%t -L%t -lmodule1 -module-name main -o %t/main
// RUN: %llvm-nm --defined-only %t/main | %FileCheck %s

// CHECK-NOT: _$s7module120unusedPublicFunctionyyF

import module1
