// RUN: %empty-directory(%t/cache)
// RUN: %empty-directory(%t/build)
// RUN: %empty-directory(%t/SDKs/A.sdk)
// RUN: %empty-directory(%t/SDKs/B.sdk)
// RUN: %{python} %utils/split_file.py -o %t %s

/// Build Lib against SDK A.
// RUN: %target-swift-frontend -emit-module %t/Lib.swift -swift-version 5 -sdk %t/SDKs/A.sdk -o %t/build -parse-stdlib -module-cache-path %t/cache

/// Building Client against SDK A should work fine as expected.
// RUN: %target-swift-frontend -typecheck %t/Client.swift -swift-version 5 -sdk %t/SDKs/A.sdk -I %t/build -parse-stdlib -module-cache-path %t/cache

/// Build Client against SDK B, this should fail at loading Lib against a different SDK than A.
// RUN: not %target-swift-frontend -typecheck %t/Client.swift -swift-version 5 -sdk %t/SDKs/B.sdk -I %t/build -parse-stdlib -module-cache-path %t/cache 2>&1 | %FileCheck %s
// CHECK: cannot load module 'Lib' built with SDK 'A' when using SDK 'B': {{.*}}/Lib.swiftmodule

// BEGIN Lib.swift
public func foo() {}

// BEGIN Client.swift
import Lib
foo()
