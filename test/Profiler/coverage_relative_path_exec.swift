// REQUIRES: profile_runtime
// REQUIRES: executable_test
// REQUIRES: OS=macosx

// This test is to make sure llvm-cov can deal with a coverage-prefix-map.

// To make sure this test is resilient to directory changes, we create nested
// directories inside of the temporary test directory.
//
// RUN: %empty-directory(%t)
// RUN: mkdir -p %t/root/nested
// RUN: echo "func coverage() {}" > %t/root/nested/coverage_relative_path.swift
// RUN: cd %t/root

// RUN: %target-build-swift -profile-generate -profile-coverage-mapping -Xfrontend -coverage-prefix-map -Xfrontend %t/root=. -Xfrontend -disable-incremental-llvm-codegen -o %t/main %t/root/nested/coverage_relative_path.swift

// This unusual use of 'sh' allows the path of the profraw file to be
// substituted by %target-run.
// RUN: %target-codesign %t/main
// RUN: %target-run sh -c 'env LLVM_PROFILE_FILE=$1 $2' -- %t/default.profraw %t/main

// RUN: %llvm-profdata merge %t/default.profraw -o %t/default.profdata
// RUN: %llvm-cov show %t/main -instr-profile=%t/default.profdata | %FileCheck --check-prefix SHOW %s
// RUN: %llvm-cov report %t/main -instr-profile=%t/default.profdata | %FileCheck --check-prefix REPORT %s

// SHOW: func coverage
// REPORT: coverage_relative_path.swift
