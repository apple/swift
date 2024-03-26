// RUN: %empty-directory(%t)
// RUN: split-file %s %t

// RUN: %target-swift-frontend -emit-module %t/Foo.swift -emit-module-path %t/Foo.swiftmodule/%target-swiftmodule-name -module-name Foo -emit-module-interface-path %t/Foo.swiftmodule/%target-swiftinterface-name -disable-implicit-string-processing-module-import -disable-implicit-concurrency-module-import -parse-stdlib

// RUN: %target-swift-frontend -scan-dependencies -module-load-mode only-serialized \
// RUN:   -disable-implicit-string-processing-module-import -disable-implicit-concurrency-module-import -parse-stdlib \
// RUN:   %t/main.swift -I %t -o - | %FileCheck %s --check-prefix=SERIALIZED

// RUN: %target-swift-frontend -scan-dependencies -module-load-mode only-interface \
// RUN:   -disable-implicit-string-processing-module-import -disable-implicit-concurrency-module-import -parse-stdlib \
// RUN:   %t/main.swift -I %t -o - | %FileCheck %s --check-prefix=INTERFACE

// SERIALIZED: "swiftPrebuiltExternal": "Foo"
// INTERFACE: "swift": "Foo"

//--- main.swift
import Foo

//--- Foo.swift
public func foo() {}
