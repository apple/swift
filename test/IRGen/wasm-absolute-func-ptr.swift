// RUN: %swift -target wasm32-unknown-wasi -parse-stdlib -emit-ir -o - %s | %FileCheck %s

// REQUIRES: CODEGENERATOR=WebAssembly

// CHECK: @"\01l_entry_point" = private constant { i32{{.*}} { i32 ptrtoint (ptr @main to i32){{.*}} }, section "swift5_entry", align 4

