// RUN: %target-swiftc_driver -c %s -o %t
// RUN: %target-swift-autolink-extract %t -o - | %FileCheck --check-prefix CHECK-%target-object-format %s

// REQUIRES: autolink-extract

// CHECK-elf: -lswiftCore
// CHECK-coff: -lswiftCore
// CHECK-wasm: -lswiftCore
