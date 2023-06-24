// RUN: %empty-directory(%t)
// RUN: %target-build-swift -target %target-cpu-apple-macosx10.15 -parse-as-library -emit-library -emit-module-path %t/AlwaysInlineIntoWithOpaque.swiftmodule -module-name AlwaysInlineIntoWithOpaque -enable-library-evolution %S/Inputs/AlwaysInlineIntoWithOpaque.swift -o %t/%target-library-name(AlwaysInlineIntoWithOpaque)
// RUN: %target-codesign %t/%target-library-name(AlwaysInlineIntoWithOpaque)
// RUN: %target-build-swift -target %target-cpu-apple-macosx10.15 -lAlwaysInlineIntoWithOpaque -module-name main -I %t -L %t %s -o %t/a.out
// RUN: %target-codesign %t/a.out
// RUN: %target-run %t/a.out | %FileCheck %s

// RUN: %target-build-swift -target %target-cpu-apple-macosx10.15 -parse-as-library -emit-library -emit-module-path %t/AlwaysInlineIntoWithOpaque.swiftmodule -module-name AlwaysInlineIntoWithOpaque -enable-library-evolution %S/Inputs/AlwaysInlineIntoWithOpaqueReplacement.swift -o %t/%target-library-name(AlwaysInlineIntoWithOpaque)
// RUN: %target-codesign %t/a.out
// RUN: %target-run %t/a.out | %FileCheck %s

// REQUIRES: OS=macosx && (CPU=x86_64 || CPU=arm64)
// REQUIRES: executable_test

// This test requires executable tests to be run on the same machine as the
// compiler, as it links with a dylib that it doesn't arrange to get uploaded
// to remote executors.
// (rdar://97995151)
// UNSUPPORTED: remote_run || device_run

import AlwaysInlineIntoWithOpaque

public func test() {
  let p = testInlineWithOpaque()
  print(p)
}

test()
// CHECK: 1

public func testUsableFromInline() {
  let p = testInlineWithOpaqueUsableFromInline()
  print(p)
}

testUsableFromInline()
// CHECK: 3
