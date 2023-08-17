// REQUIRES: swift_swift_parser, executable_test

// For _Concurrency.
// UNSUPPORTED: use_os_stdlib
// UNSUPPORTED: back_deployment_runtime

// RUN: %empty-directory(%t)
// RUN: %host-build-swift -swift-version 5 -emit-library -o %t/%target-library-name(MacroDefinition) -parse-as-library -module-name=MacroDefinition %S/Inputs/syntax_macro_definitions.swift -g -no-toolchain-stdlib-rpath
// RUN: %target-typecheck-verify-swift -swift-version 5 -load-plugin-library %t/%target-library-name(MacroDefinition) -parse-as-library -disable-availability-checking -DTEST_DIAGNOSTICS

// Check with the imported macro library vs. the local declaration of the macro.
// RUN: %target-swift-frontend -enable-experimental-feature ExtensionMacros -swift-version 5 -emit-module -o %t/macro_library.swiftmodule %S/Inputs/macro_library.swift -module-name macro_library -load-plugin-library %t/%target-library-name(MacroDefinition)

// RUN: %target-typecheck-verify-swift -swift-version 5 -load-plugin-library %t/%target-library-name(MacroDefinition) -parse-as-library -disable-availability-checking -DIMPORT_MACRO_LIBRARY -I %t -DTEST_DIAGNOSTICS


// RUN: %target-swift-frontend -swift-version 5 -typecheck -load-plugin-library %t/%target-library-name(MacroDefinition) -parse-as-library %s -disable-availability-checking -dump-macro-expansions > %t/expansions-dump.txt 2>&1
// RUN: %FileCheck -check-prefix=CHECK-DUMP %s < %t/expansions-dump.txt

// RUN: %target-build-swift -swift-version 5 -Xfrontend -disable-availability-checking -load-plugin-library %t/%target-library-name(MacroDefinition) -parse-as-library %s -o %t/main -module-name MacroUser -g
// RUN: %target-codesign %t/main
// RUN: %target-run %t/main | %FileCheck %s -check-prefix=CHECK-EXEC

// Emit module while skipping function bodies
// RUN: %target-swift-frontend -swift-version 5 -emit-module -load-plugin-library %t/%target-library-name(MacroDefinition) -parse-as-library %s -disable-availability-checking -o %t/macro_expand_peers.swiftmodule -experimental-skip-non-inlinable-function-bodies-without-types

#if IMPORT_MACRO_LIBRARY
import macro_library
#else
@attached(peer, names: overloaded)
macro addCompletionHandler() = #externalMacro(module: "MacroDefinition", type: "AddCompletionHandler")
@attached(peer, names: suffixed(Builder))
macro AddClassReferencingSelf() = #externalMacro(module: "MacroDefinition", type: "AddClassReferencingSelfMacro")
@attached(peer, names: named(value))
macro declareVarValuePeer() = #externalMacro(module: "MacroDefinition", type: "VarValueMacro")
#endif

@attached(peer, names: arbitrary)
macro addCompletionHandlerArbitrarily(_: Int) = #externalMacro(module: "MacroDefinition", type: "AddCompletionHandler")

struct S {
  @addCompletionHandler
  func f(a: Int, for b: String, _ value: Double) async -> String {
    return b
  }

  // CHECK-DUMP: @__swiftmacro_18macro_expand_peers1SV1f20addCompletionHandlerfMp_.swift
  // CHECK-DUMP: func f(a: Int, for b: String, _ value: Double, completionHandler: @escaping (String) -> Void) {
  // CHECK-DUMP:   Task {
  // CHECK-DUMP:     completionHandler(await f(a: a, for: b, value))
  // CHECK-DUMP:   }
  // CHECK-DUMP: }

  func useOverload(_ body: @escaping (String) -> Void) {
    self.f(a: 1, for: "hahaha local", 2.0) {
      body($0)
    }
  }
}

extension S {
  @addCompletionHandler
  func g(a: Int, for b: String, _ value: Double) async -> String {
    return b
  }

  // CHECK-DUMP: @__swiftmacro_18macro_expand_peers1SV1g20addCompletionHandlerfMp_.swift
  // CHECK-DUMP: func g(a: Int, for b: String, _ value: Double, completionHandler: @escaping (String) -> Void) {
  // CHECK-DUMP:   Task {
  // CHECK-DUMP:     completionHandler(await g(a: a, for: b, value))
  // CHECK-DUMP:   }
  // CHECK-DUMP: }

}

func useCompletionHandlerG(s: S, _ body: @escaping (String) -> Void) {
  s.g(a: 1, for: "hahaha local", 2.0) {
    body($0)
  }
}

class C {
  @addCompletionHandler
  func f(a: Int, for b: String, _ value: Double) async -> String {
    return b
  }
}


@addCompletionHandler
func f(a: Int, for b: String, _ value: Double) async -> String {
  return b
}

func useOverload(_ body: @escaping (String) -> Void) {
  f(a: 1, for: "hahaha global", 2.0) {
    body($0)
  }
}

@attached(peer)
macro wrapInType() = #externalMacro(module: "MacroDefinition", type: "WrapInType")

@wrapInType
func global(a: Int, b: String) {
  print(a, b)
}

// CHECK-DUMP: @__swiftmacro_18macro_expand_peers6global10wrapInTypefMp_.swift
// CHECK-DUMP: struct $s18macro_expand_peers6global10wrapInTypefMp_6globalfMu0_ {
// CHECK-DUMP:   func $s18macro_expand_peers6global10wrapInTypefMp_6globalfMu_(a: Int, b: String)  {
// CHECK-DUMP:     global(a: a, b: b)
// CHECK-DUMP:   }
// CHECK-DUMP: }

@main
struct Main {
  static func main() async {
    let result1 = await withCheckedContinuation { cont in
      S().useOverload {
        cont.resume(returning: $0)
      }
    }
    print(result1)
    // CHECK-EXEC: hahaha local
    let result2 = await withCheckedContinuation { cont in
      useOverload {
        cont.resume(returning: $0)
      }
    }
    print(result2)
    // CHECK-EXEC: hahaha global

    // CHECK-EXEC: MyWrapperThingy<Swift.Int>(storage: 5)
    print(S3(x: 5))
  }
}

@AddClassReferencingSelf
protocol MyProto { }

// Reference cycles amongst arbitrary peer macros and macro arguments.
let x = 10
let y = 10
struct S2 {
  @addCompletionHandlerArbitrarily(x)
  func f(a: Int, for b: String, _ value: Double) async -> String {
    return b
  }

  @addCompletionHandlerArbitrarily(y)
  func g(a: Int, for b: String, _ value: Double) async -> String {
    return b
  }

  #if TEST_DIAGNOSTICS
  // expected-error@+1 {{cannot find 'nonexistent' in scope}}
  @addCompletionHandlerArbitrarily(nonexistent)
  func h(a: Int, for b: String, _ value: Double) async -> String {
    return b
  }
  #endif
}

#if TEST_DIAGNOSTICS
// Peer macros cannot introduce arbitrary names at global scope

//expected-error@+1 {{'peer' macros are not allowed to introduce arbitrary names at global scope}}
@addCompletionHandlerArbitrarily(x)
func h(a: Int, for b: String, _ value: Double) async -> String {
  return b
}
#endif

// Stored properties generated by a peer macro
@attached(accessor)
@attached(peer, names: prefixed(_))
macro myPropertyWrapper() =
    #externalMacro(module: "MacroDefinition", type: "PropertyWrapperMacro")

struct MyWrapperThingy<T> {
  var storage: T

  var wrappedValue: T {
    get {
      print("Getting value \(storage)")
      return storage
    }

    set {
      print("Setting value \(newValue)")
      storage = newValue
    }
  }
}

struct S3 {
  @myPropertyWrapper
  var x: Int = 0

  init(x: Int) {
    self._x = MyWrapperThingy(storage: x)
  }
}

@declareVarValuePeer
struct Date {
  @declareVarValuePeer
  func foo() {}
}

func testVarPeer() {
  _ = value
  _ = Date().value
}

#if TEST_DIAGNOSTICS
// Macros cannot be attached to function parameters

// expected-error@+1{{'peer' macro cannot be attached to parameter ('x')}}
func test(@declareVarValuePeer x: Int) {}
#endif

// Stored properties added via peer macros.
@attached(peer, names: named(_foo))
macro AddPeerStoredProperty() =
  #externalMacro(module: "MacroDefinition", type: "AddPeerStoredPropertyMacro")

struct SomeStructWithPeerProperties {
  @AddPeerStoredProperty
  var foo: String = "hello"
}

func testStructWithPeers() {
  let x = SomeStructWithPeerProperties()
  print(x)
}


#if TEST_DIAGNOSTICS
@available(*, deprecated, message: "This macro is deprecated.")
@attached(peer, names: overloaded)
macro deprecatedAddCompletionHandler() = #externalMacro(module: "MacroDefinition", type: "AddCompletionHandler")


// expected-warning@+1{{'deprecatedAddCompletionHandler()' is deprecated: This macro is deprecated.}}
@deprecatedAddCompletionHandler
func fDeprecated(a: Int, for b: String, _ value: Double) async -> String {
  return b
}

#endif
