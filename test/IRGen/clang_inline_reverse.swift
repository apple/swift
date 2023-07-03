// Same test as clang_inline.swift, but with the order swapped.

// RUN: %empty-directory(%t)
// RUN: %build-irgen-test-overlays
// RUN: %target-swift-frontend -sdk %S/Inputs -primary-file %s -enable-objc-interop -emit-ir -module-name clang_inline -I %t | %FileCheck %s

// REQUIRES: CPU=i386 || CPU=x86_64
// REQUIRES: objc_interop

import gizmo

// CHECK-LABEL: define hidden swiftcc i64 @"$s12clang_inline16CallStaticInlineC10ReturnZeros5Int64VyF"(ptr swiftself %0) {{.*}} {
class CallStaticInline {
  func ReturnZero() -> Int64 { return Int64(wrappedZero()) }
}

// CHECK-LABEL: define internal i32 @wrappedZero() {{#[0-9]+}} {

// CHECK-LABEL: define hidden swiftcc i64 @"$s12clang_inline17CallStaticInline2C10ReturnZeros5Int64VyF"(ptr swiftself %0) {{.*}} {
class CallStaticInline2 {
  func ReturnZero() -> Int64 { return Int64(zero()) }
}

// CHECK-LABEL: define internal i32 @zero() {{#[0-9]+}} {

// CHECK-LABEL: define internal i32 @innerZero() {{#[0-9]+}} {
