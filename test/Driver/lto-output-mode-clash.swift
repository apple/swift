// REQUIRES: lld_lto

// RUN: %swiftc_driver -driver-print-jobs %S/../Inputs/empty.swift -lto=llvm-full -static -emit-library -target x86_64-apple-macosx10.9 | %FileCheck %s
// RUN: %swiftc_driver -driver-print-jobs %S/../Inputs/empty.swift -lto=llvm-full -emit-library -target x86_64-apple-macosx10.9 | %FileCheck %s
// RUN: %swiftc_driver -driver-print-jobs %S/../Inputs/empty.swift -lto=llvm-full -c -target x86_64-apple-macosx10.9 | %FileCheck %s
// RUN: %swiftc_driver -driver-print-jobs %S/../Inputs/empty.swift -c -lto=llvm-full -target x86_64-apple-macosx10.9 | %FileCheck %s
// RUN: %swiftc_driver -driver-print-jobs %S/../Inputs/empty.swift -c -lto=llvm-full -emit-bc -target x86_64-apple-macosx10.9 | %FileCheck %s
// RUN: %swiftc_driver -driver-print-jobs %S/../Inputs/empty.swift -emit-bc -c -lto=llvm-full -target x86_64-apple-macosx10.9 | %FileCheck %s

// CHECK: swift
// CHECK-DAG: -emit-bc
// CHECK-DAG: -lto=llvm-full
// CHECK-DAG: -o [[BITCODEFILE:.*\.bc]]
