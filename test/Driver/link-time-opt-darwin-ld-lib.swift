// REQUIRES: OS=macosx

// Check that ld gets "-lto_library"

// RUN: %swiftc_driver -driver-print-jobs %S/../Inputs/empty.swift -lto=llvm-thin -target x86_64-apple-macosx10.9     | %FileCheck %s --check-prefix=CHECK-SIMPLE-THIN --check-prefix=CHECK-SIMPLE-THIN-macosx

// CHECK-SIMPLE-THIN: swift
// CHECK-SIMPLE-THIN-DAG: -emit-bc
// CHECK-SIMPLE-THIN-DAG: -lto=llvm-thin
// CHECK-SIMPLE-THIN-DAG: -o [[OBJECTFILE:.*\.bc]]

// CHECK-SIMPLE-THIN-macosx: ld
// CHECK-SIMPLE-THIN-macosx-DAG: -lto_library {{.+}}/lib/libLTO.dylib
// CHECK-SIMPLE-THIN-macosx-DAG: [[OBJECTFILE]]


// RUN: %swiftc_driver -driver-print-jobs %S/../Inputs/empty.swift -lto=llvm-full -target x86_64-apple-macosx10.9     | %FileCheck %s --check-prefix=CHECK-SIMPLE-FULL --check-prefix=CHECK-SIMPLE-FULL-macosx 

// CHECK-SIMPLE-FULL: swift
// CHECK-SIMPLE-FULL-DAG: -emit-bc
// CHECK-SIMPLE-FULL-DAG: -lto=llvm-full
// CHECK-SIMPLE-FULL-DAG: -o [[OBJECTFILE:.*\.bc]]

// CHECK-SIMPLE-FULL-macosx: ld
// CHECK-SIMPLE-FULL-macosx-DAG: -lto_library {{.+}}/lib/libLTO.dylib
// CHECK-SIMPLE-FULL-macosx-DAG: [[OBJECTFILE]]


// Check that driver does not see libLTO.dylib as an input

// RUN: %swiftc_driver -driver-print-jobs %S/../Inputs/empty.swift -lto=llvm-full -lto-library /foo/libLTO.dylib -target x86_64-apple-macosx10.9     | %FileCheck %s --check-prefix=CHECK-SIMPLE-LTO-LIB --check-prefix=CHECK-SIMPLE-LTO-LIB-macosx

// CHECK-SIMPLE-LTO-LIB: swift
// CHECK-SIMPLE-LTO-LIB-DAG: -emit-bc
// CHECK-SIMPLE-LTO-LIB-DAG: -lto=llvm-full
// CHECK-SIMPLE-LTO-LIB-NOT: -lto-library /foo/libLTO.dylib
// CHECK-SIMPLE-LTO-LIB-DAG: -o [[OBJECTFILE:.*\.bc]]

// CHECK-SIMPLE-LTO-LIB-macosx: ld
// CHECK-SIMPLE-LTO-LIB-macosx-DAG: -lto_library /foo/libLTO.dylib
// CHECK-SIMPLE-LTO-LIB-macosx-DAG: [[OBJECTFILE]]
