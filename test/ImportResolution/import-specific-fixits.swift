// RUN: rm -f %t.*
// RUN: %empty-directory(%t)
// RUN: %target-swift-frontend -emit-module -o %t %S/Inputs/ambiguous_left.swift
// RUN: %target-swift-frontend -emit-module -o %t %S/Inputs/ambiguous_right.swift
// RUN: %target-swift-frontend -emit-module -o %t -I %t %S/Inputs/ambiguous.swift
// RUN: %target-swift-frontend -emit-module -o %t %S/Inputs/DeclsUsedWrongly.swift

// RUN: %target-swift-frontend -typecheck -I %t -serialize-diagnostics-path %t.dia %s -verify
// RUN: c-index-test -read-diagnostics %t.dia > %t.deserialized_diagnostics.txt 2>&1
// RUN: %FileCheck --input-file=%t.deserialized_diagnostics.txt %s

import typealias Swift.Int
import struct Swift.Int

import class Swift.Int // expected-error {{'Int' was imported as 'class', but is a struct}} {{8-13=struct}}
import func Swift.Int // expected-error {{'Int' was imported as 'func', but is a struct}} {{8-12=struct}}
import var Swift.Int // expected-error {{'Int' was imported as 'var', but is a struct}} {{8-11=struct}}

// CHECK: [[@LINE-4]]:14: error: 'Int' was imported as 'class', but is a struct
// CHECK-NEXT: Number FIXITs = 1
// CHECK-NEXT: FIXIT: ([[FILE:.*import-specific-fixits.swift]]:[[@LINE-6]]:8 - [[FILE]]:[[@LINE-6]]:13): "struct"
// CHECK-NEXT: note: 'Int' declared here

import typealias Swift.IteratorProtocol // expected-error {{'IteratorProtocol' was imported as 'typealias', but is a protocol}} {{8-17=protocol}}
import struct Swift.IteratorProtocol // expected-error {{'IteratorProtocol' was imported as 'struct', but is a protocol}} {{8-14=protocol}}
import func Swift.IteratorProtocol // expected-error {{'IteratorProtocol' was imported as 'func', but is a protocol}} {{8-12=protocol}}

import class Swift.Int64 // expected-error {{'Int64' was imported as 'class', but is a struct}} {{8-13=struct}}

import class Swift.Bool // expected-error {{'Bool' was imported as 'class', but is a struct}} {{8-13=struct}}

import struct DeclsUsedWrongly.x // expected-error {{'x' was imported as 'struct', but is a variable}} {{8-14=var}}

import struct DeclsUsedWrongly.Choice // expected-error {{'Choice' was imported as 'struct', but is an enum}} {{8-14=enum}}

import struct DeclsUsedWrongly.Callback // expected-error {{type alias 'Callback' (aka '() -> ()') cannot be imported as 'struct'}} {{8-14=typealias}}
import var DeclsUsedWrongly.Callback // expected-error {{'Callback' was imported as 'var', but is a type}} {{8-11=typealias}}

import struct DeclsUsedWrongly.Pair // expected-error {{type alias 'Pair<T>' (aka '(T, T)') cannot be imported as 'struct'}} {{8-14=typealias}}
import var DeclsUsedWrongly.Pair // expected-error {{'Pair' was imported as 'var', but is a type}} {{8-11=typealias}}

import protocol DeclsUsedWrongly.TopLevelProtocol  // no-warning
import protocol DeclsUsedWrongly.AnyTopLevelProtocol // expected-error {{type alias 'AnyTopLevelProtocol' (aka 'any TopLevelProtocol') cannot be imported as 'protocol'}} {{8-16=typealias}}
import typealias DeclsUsedWrongly.AnyTopLevelProtocol // no-warning
import protocol DeclsUsedWrongly.NestedProtocol  // no-warning
import typealias DeclsUsedWrongly.AnyNestedProtocol // no-warning
import protocol DeclsUsedWrongly.AnyNestedProtocol // expected-error {{type alias 'AnyNestedProtocol' (aka 'any NamespaceStruct.NestedProtocol') cannot be imported as 'protocol'}} {{8-16=typealias}}

import struct Swift.print // expected-error {{'print' was imported as 'struct', but is a function}} {{8-14=func}}

// CHECK: [[@LINE-2]]:15: error: 'print' was imported as 'struct', but is a function
// CHECK-NEXT: Number FIXITs = 1
// CHECK-NEXT: FIXIT: ([[FILE]]:[[@LINE-4]]:8 - [[FILE]]:[[@LINE-4]]:14): "func"
// CHECK-NOT: note: 'print' declared here


import func ambiguous.funcOrVar // expected-error{{ambiguous name 'funcOrVar' in module 'ambiguous'}}
import var ambiguous.funcOrVar // expected-error{{ambiguous name 'funcOrVar' in module 'ambiguous'}}
import struct ambiguous.funcOrVar // expected-error{{ambiguous name 'funcOrVar' in module 'ambiguous'}}

// CHECK: [[@LINE-4]]:13: error: ambiguous name 'funcOrVar' in module 'ambiguous'
// CHECK-NEXT: Number FIXITs = 0
// CHECK-NEXT: note: found this candidate
// CHECK-NEXT: Number FIXITs = 0
// CHECK-NEXT: CONTENTS OF FILE ambiguous_right.funcOrVar:
// CHECK: public var funcOrVar: Int
// CHECK: END CONTENTS OF FILE
// CHECK-NEXT: note: found this candidate

import func ambiguous.someVar // expected-error{{ambiguous name 'someVar' in module 'ambiguous'}}
import var ambiguous.someVar // expected-error{{ambiguous name 'someVar' in module 'ambiguous'}}
import struct ambiguous.someVar // expected-error{{ambiguous name 'someVar' in module 'ambiguous'}}

import struct ambiguous.SomeStruct // expected-error{{ambiguous name 'SomeStruct' in module 'ambiguous'}}
import typealias ambiguous.SomeStruct // expected-error{{ambiguous name 'SomeStruct' in module 'ambiguous'}}
import class ambiguous.SomeStruct // expected-error{{ambiguous name 'SomeStruct' in module 'ambiguous'}}

import func ambiguous.overloadedFunc // no-warning
