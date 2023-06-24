// RUN: %empty-directory(%t)
// RUN: %target-swift-frontend -module-name protocol_overrides -emit-module -enable-library-evolution -emit-module-path=%t/protocol_overrides.swiftmodule %S/../SILGen/Inputs/protocol_overrides.swift
// RUN: %target-swift-frontend -module-name keypath_witness_overrides -emit-ir %s -I %t | %FileCheck %s

import protocol_overrides

// CHECK: @keypath = private global
// CHECK-SAME: ptr @"{{got.|\\01__imp__?}}$s18protocol_overrides14OriginalGetterPy7ElementQz5IndexQzcigTq"
public func getWritableKeyPath<OS: OverridesSetter>(_ c: OS, index: OS.Index) -> AnyKeyPath
where OS.Index: Hashable {
  let keypath = \OS.[index]
  return keypath
}
