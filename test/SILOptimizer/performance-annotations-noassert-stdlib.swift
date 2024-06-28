// RUN: %target-swift-frontend -parse-as-library -disable-availability-checking -import-objc-header %S/Inputs/perf-annotations.h -emit-sil %s -o /dev/null -verify

// REQUIRES: swift_in_compiler
// REQUIRES: swift_stdlib_no_asserts,optimized_stdlib

@_noAllocation
func createEmptyArray() {
  _ = [Int]() // expected-error {{ending the lifetime of a value of type}}
}