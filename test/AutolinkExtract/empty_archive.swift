// RUN: %target-swiftc_driver -c %s -o %t
// RUN: rm -f %t.a
// RUN: llvm-ar cr %t.a %t
// RUN: %target-swift-autolink-extract %t.a -o - | %FileCheck --check-prefix CHECK-%target-object-format %s

// REQUIRES: autolink-extract

// CHECK-elf: -lswiftCore
// CHECK-coff: -lswiftCore
// CHECK-wasm: -lswiftCore
