// RUN: %empty-directory(%t)
// RUN: %target-swiftc_driver -emit-module -emit-module-path %t/empty.swiftmodule -module-name empty -module-link-name empty %S/empty.swift
// RUN: %target-swiftc_driver -c %s -I %t -o %t/import_experimental.o
// RUN: llvm-ar cr %t/import_experimental.a %t/import_experimental.o
// RUN: %target-swift-autolink-extract %t/import_experimental.a -o - | %FileCheck --check-prefix CHECK-%target-object-format %s

// REQUIRES: autolink-extract

// CHECK-elf-DAG: -lswiftCore
// CHECK-elf-DAG: -lempty

// CHECK-coff-DAG: -lswiftCore
// CHECK-coff-DAG: -lempty

// CHECK-wasm-DAG: -lswiftCore
// CHECK-wasm-DAG: -lempty

import empty
