// RUN: %swift %use_no_opaque_pointers -target wasm32-unknown-wasi -parse-stdlib -emit-ir -o - %s | %FileCheck %s
// RUN: %swift -target wasm32-unknown-wasi -parse-stdlib -emit-ir -o - %s

// REQUIRES: CODEGENERATOR=WebAssembly

// CHECK: @"\01l_entry_point" = private constant { i32{{.*}} { i32 ptrtoint (i32 (i32, i8*)* @main to i32){{.*}} }, section "swift5_entry", align 4

