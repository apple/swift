// RUN: %empty-directory(%t)
// RUN: %target-swift-frontend -swift-version 4 -emit-module -o %t/regex.swiftmodule %S/Inputs/deserialize-clang-importer-witness-tables/regex.swift
// RUN: %target-swift-frontend -swift-version 4 -emit-ir %s -I %t | %FileCheck %s
// REQUIRES: objc_interop
import regex

public func foo(line: String) {
  // The NSRegularExpressionOptions: SetAlgebra conformance is used indirectly
  // from the default argument expression passed to `RegEx(pattern:options:)`
  // below. Ensure that a local copy of the definition was deserialized
  // and lowered to IR.
  // CHECK-LABEL: define {{.*}} void @"$sSo26NSRegularExpressionOptionsVs10SetAlgebraSCsACPxycfCTW"
  // CHECK-LABEL: define {{.*}} ptr @"$sSo26NSRegularExpressionOptionsVABSQSCWl"()
  let versionRegex = try! RegEx(pattern: "Apple")
  _ = versionRegex.firstMatch(in: line)  
}
