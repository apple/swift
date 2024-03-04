// UNSUPPORTED: linker_overridden

// RUN: %swiftc_driver -driver-print-jobs %S/../Inputs/empty.swift -lto=llvm-thin -target x86_64-unknown-linux-gnu    | %FileCheck %s --check-prefix=CHECK-SIMPLE-THIN --check-prefix=CHECK-SIMPLE-THIN-linux-gnu
// RUN: %swiftc_driver -driver-print-jobs %S/../Inputs/empty.swift -lto=llvm-thin -target x86_64-unknown-windows-msvc | %FileCheck %s --check-prefix=CHECK-SIMPLE-THIN --check-prefix=CHECK-SIMPLE-THIN-windows-msvc

// CHECK-SIMPLE-THIN: swift
// CHECK-SIMPLE-THIN-DAG: -emit-bc
// CHECK-SIMPLE-THIN-DAG: -lto=llvm-thin
// CHECK-SIMPLE-THIN-DAG: -o [[BITCODEFILE:.*\.bc]]

// CHECK-SIMPLE-THIN-windows-msvc: clang
// CHECK-SIMPLE-THIN-windows-msvc-DAG: -fuse-ld=lld
// CHECK-SIMPLE-THIN-windows-msvc-DAG: -flto=thin
// CHECK-SIMPLE-THIN-windows-msvc-DAG: [[BITCODEFILE]]

// CHECK-SIMPLE-THIN-linux-gnu: clang
// CHECK-SIMPLE-THIN-linux-gnu-DAG: -flto=thin
// CHECK-SIMPLE-THIN-linux-gnu-DAG: -fuse-ld=lld
// CHECK-SIMPLE-THIN-linux-gnu-DAG: [[BITCODEFILE]]
// CHECK-SIMPLE-THIN-linux-gnu-NOT: swift-autolink-extract



// RUN: %swiftc_driver -driver-print-jobs %S/../Inputs/empty.swift -lto=llvm-full -target x86_64-unknown-linux-gnu    | %FileCheck %s --check-prefix=CHECK-SIMPLE-FULL --check-prefix=CHECK-SIMPLE-FULL-linux-gnu
// RUN: %swiftc_driver -driver-print-jobs %S/../Inputs/empty.swift -lto=llvm-full -target x86_64-unknown-windows-msvc | %FileCheck %s --check-prefix=CHECK-SIMPLE-FULL --check-prefix=CHECK-SIMPLE-FULL-windows-msvc

// CHECK-SIMPLE-FULL: swift
// CHECK-SIMPLE-FULL-DAG: -emit-bc
// CHECK-SIMPLE-FULL-DAG: -lto=llvm-full
// CHECK-SIMPLE-FULL-DAG: -o [[BITCODEFILE:.*\.bc]]

// CHECK-SIMPLE-FULL-windows-msvc: clang
// CHECK-SIMPLE-FULL-windows-msvc-DAG: -fuse-ld=lld
// CHECK-SIMPLE-FULL-windows-msvc-DAG: -flto=full
// CHECK-SIMPLE-FULL-windows-msvc-DAG: [[BITCODEFILE]]

// CHECK-SIMPLE-FULL-linux-gnu: clang
// CHECK-SIMPLE-FULL-linux-gnu-DAG: -flto=full
// CHECK-SIMPLE-FULL-linux-gnu-DAG: -fuse-ld=lld
// CHECK-SIMPLE-FULL-linux-gnu-DAG: [[BITCODEFILE]]
// CHECK-SIMPLE-FULL-linux-gnu-NOT: swift-autolink-extract



// RUN: %swiftc_driver -driver-print-jobs %S/../Inputs/empty.swift -lto=llvm-thin -static -emit-library -target x86_64-apple-macosx10.9  | %FileCheck %s --check-prefix=CHECK-STATIC-LIB-THIN --check-prefix=CHECK-STATIC-LIB-THIN-macosx
// RUN: %swiftc_driver -driver-print-jobs %S/../Inputs/empty.swift -lto=llvm-thin -static -emit-library -target x86_64-unknown-linux-gnu | %FileCheck %s --check-prefix=CHECK-STATIC-LIB-THIN --check-prefix=CHECK-STATIC-LIB-THIN-linux-gnu

// CHECK-STATIC-LIB-THIN: swift
// CHECK-STATIC-LIB-THIN-DAG: -emit-bc
// CHECK-STATIC-LIB-THIN-DAG: -lto=llvm-thin
// CHECK-STATIC-LIB-THIN-DAG: -o [[BITCODEFILE:.*\.bc]]

// CHECK-STATIC-LIB-THIN-macosx: libtool
// CHECK-STATIC-LIB-THIN-macosx-DAG: [[BITCODEFILE]]

// CHECK-STATIC-LIB-THIN-linux-gnu: llvm-ar
// CHECK-STATIC-LIB-THIN-linux-gnu-DAG: [[BITCODEFILE]]
// CHECK-STATIC-LIB-THIN-linux-gnu-NOT: swift-autolink-extract



// RUN: %swiftc_driver -driver-print-jobs %S/../Inputs/empty.swift -lto=llvm-full -static -emit-library -target x86_64-apple-macosx10.9  | %FileCheck %s --check-prefix=CHECK-STATIC-LIB-FULL --check-prefix=CHECK-STATIC-LIB-FULL-macosx
// RUN: %swiftc_driver -driver-print-jobs %S/../Inputs/empty.swift -lto=llvm-full -static -emit-library -target x86_64-unknown-linux-gnu | %FileCheck %s --check-prefix=CHECK-STATIC-LIB-FULL --check-prefix=CHECK-STATIC-LIB-FULL-linux-gnu

// CHECK-STATIC-LIB-FULL: swift
// CHECK-STATIC-LIB-FULL-DAG: -emit-bc
// CHECK-STATIC-LIB-FULL-DAG: -lto=llvm-full
// CHECK-STATIC-LIB-FULL-DAG: -o [[BITCODEFILE:.*\.bc]]

// CHECK-STATIC-LIB-FULL-macosx: libtool
// CHECK-STATIC-LIB-FULL-macosx-DAG: [[BITCODEFILE]]

// CHECK-STATIC-LIB-FULL-linux-gnu: llvm-ar
// CHECK-STATIC-LIB-FULL-linux-gnu-DAG: [[BITCODEFILE]]
// CHECK-STATIC-LIB-FULL-linux-gnu-NOT: swift-autolink-extract



// Ensure that -use-ld wins even if getting -lto option
// RUN: %swiftc_driver -driver-print-jobs %s -lto=llvm-thin -use-ld=gold -target x86_64-unknown-linux-gnu    | %FileCheck -check-prefix PREFER_USE_LD %s
// RUN: %swiftc_driver -driver-print-jobs %s -lto=llvm-thin -use-ld=gold -target x86_64-unknown-windows-msvc | %FileCheck -check-prefix PREFER_USE_LD %s
// PREFER_USE_LD: -fuse-ld=gold
