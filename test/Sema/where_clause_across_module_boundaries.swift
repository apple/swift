// RUN: %empty-directory(%t)
// RUN: %target-swift-frontend -emit-module -o %t/ModuleA.swiftmodule %S/Inputs/where_clause_across_module_boundaries_module.swift
// RUN: %target-typecheck-verify-swift -I %t

// https://github.com/apple/swift/issues/58084
// Associated Type Inference fails across module boundaries
// Self bounds from where clause cannot be accessed across modules.
// This test is intended to test whether it can use generic signature to get self bounds.
import ModuleA

struct ModuleBFoo: Codable, DefaultsSerializable {
}

enum ModuleBBar: Int, Codable, DefaultsSerializable { // expected-error {{type 'ModuleBBar' does not conform to protocol 'DefaultsSerializable'}}
  case foo, bar
}

func foo() {
  _ = AliasTest<Int>.A.self // expected-error {{type 'Int' does not conform to protocol 'Collection'}}
  _ = AliasTest<Int>.B.self // expected-error {{type 'Int' does not conform to protocol 'Collection'}}
  _ = AliasTest<String>.A.self
  _ = AliasTest<String>.B.self
}