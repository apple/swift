// RUN: %empty-directory(%t)
// RUN: echo "public struct X {}; public var x = X()" | %target-swift-frontend -module-name import_builtin -parse-stdlib -emit-module -o %t -
// RUN: echo "public func foo() -> Int { return false }" > %t/import_text.swift
// RUN: echo "public func phoûx() -> Int { return false }" > %t/français.swift
// RUN: %target-swift-frontend -typecheck %s -I %t -I %S/../../Inputs/ -sdk "" -enable-source-import -module-name main -verify -show-diagnostics-after-fatal -verify-ignore-unknown

// -verify-ignore-unknown is for:
// <unknown>:0: error: unexpected note produced: did you forget to set an SDK using -sdk or SDKROOT?
// <unknown>:0: error: unexpected note produced: use "xcrun swiftc" to select the default macOS SDK installed with Xcode
// <unknown>:0: error: unexpected note produced: did you forget to set an SDK using -sdk or SDKROOT?
// <unknown>:0: error: unexpected note produced: use "xcrun swiftc" to select the default macOS SDK installed with Xcode
// <unknown>:0: error: unexpected note produced: did you forget to set an SDK using -sdk or SDKROOT?
// <unknown>:0: error: unexpected note produced: use "xcrun swiftc" to select the default macOS SDK installed with Xcode

import Builtin  // expected-error {{no such module 'Builtin'}}

import import_builtin

extension Int32 {
  init(_: import_builtin.X) { }
}

func indirectBuiltin() {
  Int(Int32(import_builtin.x)) // expected-warning{{unused}}
}

func f0() {
  import Swift // expected-error{{declaration is only valid at file scope}}
}

import func Swift.print
func f1(_ a: Swift.Int) -> Swift.Void { print(a) }

import func Swift.print

// rdar://14418336
#import something_nonexistent
// expected-error@-1 {{no macro named 'import'}}
// expected-error@-2 {{consecutive statements on a line}} 
// expected-error@-3 {{cannot find 'something_nonexistent' in scope}}

// Import specific decls
import typealias Swift.Int
import struct Swift.Int
import typealias Swift.ManagedBuffer
import class Swift.ManagedBuffer
import typealias Swift.Bool
import struct Swift.Bool
import protocol Swift.IteratorProtocol
import var import_builtin.x
import func Swift.min

import var x // expected-error {{expected module name}}
import struct Swift.nonexistent // expected-error {{struct 'nonexistent' does not exist in module 'Swift'}}
import func SwiftUI.Text.++ // expected-error{{cannot include postfix operator in import declaration}}{{25-28=}}

import Swift.import.abc // expected-error {{expected identifier in import declaration}}
// expected-error @-1 {{keyword 'import' cannot be used as an identifier here}}
// expected-note @-2 {{if this name is unavoidable, use backticks to escape it}}
import where Swift.Int // expected-error {{expected identifier}}
// expected-error @-1 {{keyword 'where' cannot be used as an identifier here}}
// expected-note @-2 {{if this name is unavoidable, use backticks to escape it}}
import 2 // expected-error {{expected identifier}}

import really.nonexistent // expected-error {{no such module 'really.nonexistent'}}


import import_text // no-warning despite function body problems
var _ : Int = foo()

import français
import func français.phoûx

import main // expected-warning {{file 'import.swift' is part of module 'main'; ignoring import}}

@_exported @_implementationOnly import empty // expected-error {{module 'empty' cannot be both exported and implementation-only}} {{12-33=}}
// expected-warning @-1 {{using '@_implementationOnly' without enabling library evolution for 'main' may lead to instability during execution}}
