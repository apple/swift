// RUN: %empty-directory(%t)
// RUN: %target-swiftc_driver -emit-module -emit-module-path %t/empty.swiftmodule -module-name empty -module-link-name empty %S/empty.swift
// RUN: %target-swiftc_driver -c %s -I %t -o %t/import_experimental.o
// RUN: %target-swift-autolink-extract %t/import_experimental.o -o - | %FileCheck --check-prefix CHECK-%target-object-format %s
// RUN: %target-swiftc_driver -c %s -I %t -o %t/import_experimental_again.o
// RUN: %target-swift-autolink-extract %t/import_experimental.o %t/import_experimental_again.o -o - | %FileCheck --check-prefix CHECK-%target-object-format %s
// RUN: %target-swift-autolink-extract %t/import_experimental.o %t/import_experimental_again.o -o - | %FileCheck --check-prefix UNIQUECORE %s
// RUN: %target-swift-autolink-extract %t/import_experimental.o %t/import_experimental_again.o -o - | %FileCheck --check-prefix UNIQUESTRING %s

// REQUIRES: autolink-extract

// UNIQUECORE-COUNT-1: -lswiftCore
// UNIQUESTRING-COUNT-1: -lswift_StringProcessing

// CHECK-elf-DAG: -lswiftCore
// CHECK-elf-DAG: -lempty

// CHECK-coff-DAG: -lswiftCore
// CHECK-coff-DAG: -lempty

// CHECK-wasm-DAG: -lswiftCore
// CHECK-wasm-DAG: -lempty

import empty
