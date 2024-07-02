// RUN: %empty-directory(%t)
// RUN: %target-build-swift-dylib(%t/%target-library-name(Def)) -module-name Def -emit-module -emit-module-path %t/Def.swiftmodule %S/Inputs/protocol-conformance-redundant-def.swift
// RUN: %target-build-swift-dylib(%t/%target-library-name(Ext)) -module-name Ext -emit-module -emit-module-path %t/Ext.swiftmodule -I%t -L%t -lDef %S/Inputs/protocol-conformance-redundant-ext.swift
// RUN: %target-build-swift -I%t -L%t -lDef -o %t/main %target-rpath(%t) %s
// RUN: %target-codesign %t/main %t/%target-library-name(Def) %t/%target-library-name(Ext)
// RUN: %target-run %t/main %t/%target-library-name(Def) %t/%target-library-name(Ext) 2>&1 | %FileCheck %s

// REQUIRES: executable_test
// XFAIL: OS=windows-msvc

// CHECK: Warning: 'main.Sub' conforms to protocol 'Hello', but it also inherits conformance from 'Def.Super'.  Relying on a particular conformance is undefined behaviour.
// CHECK: Hello

import StdlibUnittest

#if canImport(Darwin)
import Darwin
#elseif canImport(Glibc)
import Glibc
#elseif canImport(Android)
import Android
#else
#error("Unsupported platform")
#endif

import Def

let dylibPath = CommandLine.arguments.last!
let openRes = dlopen(dylibPath, RTLD_NOW|RTLD_LOCAL)
assert(openRes != nil, "unable to open extension dylib")

class Sub : Super, Hello {
  func hello() {
    print("Hello")
  }
}

let s = Sub() as AnyObject as! Hello

s.hello()

