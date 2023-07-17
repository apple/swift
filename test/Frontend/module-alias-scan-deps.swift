/// Test -scan-dependencies module aliasing.
///
/// Module 'Lib' imports module 'XLogging' via module aliasing and with various import attributes.
/// Module 'User' imports 'Lib'.

// RUN: %empty-directory(%t)
// RUN: %{python} %utils/split_file.py -o %t %s

/// Create AppleLogging.swiftmodule by aliasing XLogging
// RUN: %target-swift-frontend -module-name AppleLogging -module-alias XLogging=AppleLogging %t/FileLogging.swift -emit-module -emit-module-path %t/AppleLogging.swiftmodule
// RUN: test -f %t/AppleLogging.swiftmodule

/// Scanned dependencies should contain real name AppleLogging
// RUN: %target-swift-frontend -scan-dependencies  %t/FileLib.swift -module-alias XLogging=AppleLogging -I %t > %t/scandump.output
// RUN: %validate-json %t/scandump.output | %FileCheck %s -check-prefix=CHECK-REAL-NAME
// CHECK-REAL-NAME-NOT: "swiftPrebuiltExternal": "XLogging"
// CHECK-REAL-NAME-NOT: "compiledModulePath":{{.*}}XLogging.swiftmodule",
// CHECK-REAL-NAME: "swiftPrebuiltExternal": "AppleLogging"
// CHECK-REAL-NAME: "compiledModulePath":{{.*}}AppleLogging.swiftmodule",

/// Create AppleLoggingIF.swiftinterface by aliasing XLogging
///
// RUN: %target-swift-frontend -module-name AppleLoggingIF %t/FileLogging.swift -module-alias XLogging=AppleLoggingIF -I %t -emit-module -emit-module-interface-path %t/AppleLoggingIF.swiftinterface -swift-version 5 -enable-library-evolution -I %t
// RUN: test -f %t/AppleLoggingIF.swiftinterface

/// Scanned dependencies should contain real name AppleLoggingIF
// RUN: %target-swift-frontend -scan-dependencies  %t/FileLib.swift -module-alias XLogging=AppleLoggingIF -I %t > %t/scandumpIF.output
// RUN: %validate-json %t/scandumpIF.output | %FileCheck %s -check-prefix=CHECK-REAL-NAME-IF
// CHECK-REAL-NAME-IF-NOT: "swift": "XLogging"
// CHECK-REAL-NAME-IF-NOT: "moduleInterfacePath":{{.*}}XLogging.swiftinterface
// CHECK-REAL-NAME-IF: "swift": "AppleLoggingIF"
// CHECK-REAL-NAME-IF: "moduleInterfacePath":{{.*}}AppleLoggingIF.swiftinterface

// BEGIN FileLogging.swift
public struct Logger {
  public init() {}
  public func startLogging() {}
}
public func setup() -> XLogging.Logger? {
  return Logger()
}

// BEGIN FileLib.swift
import XLogging

public func start() {
  let it = Logger()
  it.startLogging()
}

