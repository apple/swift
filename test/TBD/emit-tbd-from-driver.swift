// REQUIRES: VENDOR=apple
// UNSUPPORTED: CPU=i386

// RUN: %empty-directory(%t)

// RUN: echo "public func foo() -> some CustomStringConvertible { 32 }" > %t/source1.swift
// RUN: echo "" > %t/source2.swift

// RUN: %target-build-swift -target %target-cpu-apple-macosx10.15 -module-name multifile %t/source1.swift  %t/source2.swift -Xfrontend -validate-tbd-against-ir=all -emit-tbd -emit-tbd-path %t/multifile.tbd -enable-library-evolution -emit-module

// RUN: %FileCheck %s < %t/multifile.tbd

// CHECK: s9multifile3fooQryFQOMQ
