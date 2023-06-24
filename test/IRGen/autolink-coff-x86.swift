// RUN: %empty-directory(%t)

// RUN: %swift -target x86_64--windows-gnu -parse-as-library -disable-legacy-type-info -parse-stdlib -emit-module-path %t/module.swiftmodule -module-name module -module-link-name module %s
// RUN: %swift -target x86_64--windows-gnu -parse-as-library -disable-legacy-type-info -parse-stdlib -module-name autolink -I %t -D MAIN_MODULE -emit-ir -o - %s | %FileCheck %s -check-prefix CHECK-GNU-IR
// RUN: %swift -target x86_64--windows-gnu -parse-as-library -disable-legacy-type-info -parse-stdlib -module-name autolink -I %t -D MAIN_MODULE -S -o - %s | %FileCheck %s -check-prefix CHECK-GNU-ASM

// REQUIRES: CODEGENERATOR=X86

#if MAIN_MODULE
import module
#endif

// CHECK-GNU-IR: @_swift1_autolink_entries = private constant [9 x i8] c"-lmodule\00", section ".swift1_autolink_entries",{{.*}} align 8
// CHECK-GNU-IR: @llvm.used = appending global [{{.*}} x ptr] [{{.*}}ptr @_swift1_autolink_entries{{.*}}], section "llvm.metadata"

// CHECK-GNU-ASM: .section	.swift1_autolink_entries{{.*}}
// CHECK-GNU-ASM: .asciz  "-lmodule"
