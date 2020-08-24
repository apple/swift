// RUN: %empty-directory(%t)
// RUN: cd %t && %target-swiftc_driver -emit-module %S/Inputs/lib.swift
// RUN: cd %t && %target-swift-frontend -emit-sib %S/Inputs/lib.swift -parse-as-library
// RUN: cd %t && %target-swift-frontend -emit-sib -I%t  %S/Inputs/main.swift

// Examine loading order
// RUN: cd %t && %swift-lto main.sib lib.sib
// RUN: cd %t && %llvm-dis lib.bc -o - | %FileCheck %s -check-prefix=CHECK-LIB
// RUN: cd %t && %llvm-dis main.bc -o - | %FileCheck %s -check-prefix=CHECK-MAIN

// RUN: cd %t && %swift-lto lib.sib main.sib
// RUN: cd %t && %llvm-dis lib.bc -o - | %FileCheck %s -check-prefix=CHECK-LIB
// RUN: cd %t && %llvm-dis main.bc -o - | %FileCheck %s -check-prefix=CHECK-MAIN

// CHECK-LIB: ModuleID = 'lib.bc'
// CHECK-LIB: define hidden swiftcc void @"$s3lib4LibXVACycfC"()
// CHECK-LIB: define swiftcc void @"$s3lib7getLibXAA0C1XVyF"()
// CHECK-LIB:   call swiftcc void @"$s3lib4LibXVACycfC"

// CHECK-MAIN: ModuleID = 'main.bc'
// CHECK-MAIN: define i32 @main
// CHECK-MAIN:   call swiftcc void @"$s3lib7getLibXAA0C1XVyF"
// CHECK-MAIN: declare swiftcc void @"$s3lib7getLibXAA0C1XVyF"()

