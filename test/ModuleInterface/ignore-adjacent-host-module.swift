// RUN: %empty-directory(%t)
// RUN: %empty-directory(%t/build/host)
// RUN: %empty-directory(%t/cache)
// RUN: split-file %s %t

/// Modules loaded from within lib/swift/host should also be rebuilt from
/// their interface (which actually means anything within resource-dir/host).

// RUN: %target-swift-frontend -emit-module %t/Lib.swift \
// RUN:   -swift-version 5 -enable-library-evolution \
// RUN:   -parse-stdlib -module-cache-path %t/cache \
// RUN:   -o %t/build/host -emit-module-interface-path %t/build/host/Lib.swiftinterface

// RUN: %target-swift-frontend -typecheck %t/Client.swift \
// RUN:   -resource-dir %t/build -I %t/build/host \
// RUN:   -parse-stdlib -module-cache-path %t/cache \
// RUN:   -Rmodule-loading 2>&1 | %FileCheck %s

// CHECK: remark: loaded module 'Lib'; source: '{{.*}}{{/|\\}}host{{/|\\}}Lib.swiftinterface', loaded: '{{.*}}{{/|\\}}cache{{/|\\}}Lib-{{.*}}.swiftmodule'

//--- Lib.swift
public func foo() {}

//--- Client.swift
import Lib
foo()
