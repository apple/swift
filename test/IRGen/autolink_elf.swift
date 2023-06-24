// RUN: %empty-directory(%t)
// RUN: %swift -disable-legacy-type-info -target x86_64-unknown-linux-gnu -emit-module -parse-stdlib -o %t -module-name Empty -module-link-name swiftEmpty -public-autolink-library anotherLib %S/../Inputs/empty.swift
// RUN: %swift -disable-legacy-type-info -target x86_64-unknown-linux-gnu %s -I %t -parse-stdlib -disable-objc-interop -module-name main -emit-ir -o - | %FileCheck %s

// REQUIRES: CODEGENERATOR=X86

import Empty

// Check that on ELF targets autolinking information is emitted and marked
// as used.

// CHECK-DAG: @_swift1_autolink_entries = private constant [26 x i8] c"-lswiftEmpty\00-lanotherLib\00", section ".swift1_autolink_entries",{{.*}} align 8
// CHECK-DAG: @llvm.compiler.used = appending global [{{.*}} x ptr] [{{.*}}ptr @_swift1_autolink_entries{{.*}}], section "llvm.metadata"

