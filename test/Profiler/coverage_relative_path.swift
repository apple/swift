// To make sure this test is resilient to directory changes, we create nested directories inside of the
// temporary test directory and assert those exist, or don't exist, in the emitted ir
//
// RUN: %empty-directory(%t)
// RUN: mkdir -p %t/root/nested
// RUN: echo "func coverage() {}" > %t/root/nested/coverage_relative_path.swift
// RUN: cd %t/root

// RUN: %target-swift-frontend -profile-generate -profile-coverage-mapping -Xllvm -enable-name-compression=false -emit-ir nested/coverage_relative_path.swift | %FileCheck -check-prefix=ABSOLUTE %s
//
// ABSOLUTE: @__llvm_coverage_mapping = {{.*"\\02.*root[^/\\]*nested[/\\]*coverage_relative_path\.swift}}

// RUN: %target-swift-frontend -profile-generate -profile-coverage-mapping -Xllvm -enable-name-compression=false -coverage-prefix-map %t/root=. -emit-ir %t/root/nested/coverage_relative_path.swift | %FileCheck -check-prefix=RELATIVE %s
//
// RELATIVE: @__llvm_coverage_mapping = {{.*"\\02.*\\01[^/]*\.[/\\]*nested[/\\]*coverage_relative_path\.swift}}
