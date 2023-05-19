// REQUIRES: swift_swift_parser, asserts
//
// RUN: %empty-directory(%t)
// RUN: %empty-directory(%t-scratch)
// RUN: %host-build-swift -swift-version 5 -emit-library -o %t/%target-library-name(MacroDefinition) -module-name=MacroDefinition %S/Inputs/syntax_macro_definitions.swift -g -no-toolchain-stdlib-rpath
// RUN: %target-swift-frontend -swift-version 5 -emit-module -o %t/macro_library.swiftmodule %S/Inputs/macro_library.swift -module-name macro_library -load-plugin-library %t/%target-library-name(MacroDefinition)
// RUN: %target-swift-frontend -swift-version 5 -typecheck -I%t -verify -primary-file %s %S/Inputs/macro_expand_other.swift -verify-ignore-unknown  -load-plugin-library %t/%target-library-name(MacroDefinition) -dump-macro-expansions > %t/expansions-dump.txt 2>&1
// RUN: %FileCheck -check-prefix=CHECK-DUMP %s < %t/expansions-dump.txt

import macro_library

struct Treat {}

@Observable
final class Dog: Observable {
  // CHECK-DUMP: public nonisolated func addObserver
  // CHECK-DUMP: public nonisolated func removeObserver

  // CHECK-DUMP: private struct Storage {
  // CHECK-DUMP:   var name: String?
  // CHECK-DUMP:   var treat: Treat?
  // CHECK-DUMP:   var isHappy: Bool = true
  // CHECK-DUMP: }

  var name: String?
  // CHECK-DUMP: get {
  // CHECK-DUMP:   _registrar.beginAccess(\.name)
  // CHECK-DUMP:   defer { _registrar.endAccess() }
  // CHECK-DUMP:   return _storage.name
  // CHECK-DUMP: }
  // CHECK-DUMP: set {
  // CHECK-DUMP:   _registrar.beginAccess(\.name)
  // CHECK-DUMP:   _registrar.register(observable: self, willSet: \.name, to: newValue)
  // CHECK-DUMP:   defer {
  // CHECK-DUMP:     _registrar.register(observable: self, didSet: \.name)
  // CHECK-DUMP:     _registrar.endAccess()
  // CHECK-DUMP:   }
  // CHECK-DUMP:   _storage.name = newValue
  // CHECK-DUMP: }


  var treat: Treat?
  // CHECK-DUMP: get {
  // CHECK-DUMP:   _registrar.beginAccess(\.treat)
  // CHECK-DUMP:   defer { _registrar.endAccess() }
  // CHECK-DUMP:   return _storage.treat
  // CHECK-DUMP: }
  // CHECK-DUMP: set {
  // CHECK-DUMP:   _registrar.beginAccess(\.treat)
  // CHECK-DUMP:   _registrar.register(observable: self, willSet: \.treat, to: newValue)
  // CHECK-DUMP:   defer {
  // CHECK-DUMP:     _registrar.register(observable: self, didSet: \.treat)
  // CHECK-DUMP:     _registrar.endAccess()
  // CHECK-DUMP:   }
  // CHECK-DUMP:   _storage.treat = newValue
  // CHECK-DUMP: }


  var isHappy: Bool = true
  // CHECK-DUMP: get {
  // CHECK-DUMP:   _registrar.beginAccess(\.isHappy)
  // CHECK-DUMP:   defer {
  // CHECK-DUMP:     _registrar.endAccess()
  // CHECK-DUMP:   }
  // CHECK-DUMP:   return _storage.isHappy
  // CHECK-DUMP: }
  // CHECK-DUMP: set {
  // CHECK-DUMP:   _registrar.beginAccess(\.isHappy)
  // CHECK-DUMP:   _registrar.register(observable: self, willSet: \.isHappy, to: newValue)
  // CHECK-DUMP:   defer {
  // CHECK-DUMP:     _registrar.register(observable: self, didSet: \.isHappy)
  // CHECK-DUMP:     _registrar.endAccess()
  // CHECK-DUMP:   }
  // CHECK-DUMP:   _storage.isHappy = newValue
  // CHECK-DUMP: }


  init() {}

  func bark() {
    print("bork bork")
  }
}

func test() {
  observeDog()
}
