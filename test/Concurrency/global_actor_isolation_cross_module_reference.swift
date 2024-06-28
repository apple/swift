// RUN: %empty-directory(%t)
// RUN: %empty-directory(%t/src)
// RUN: %empty-directory(%t/lib)
// RUN: split-file %s %t/src

/// Build the library
// RUN: %target-swift-frontend -emit-module %t/src/Lib.swift \
// RUN:   -module-name Lib -swift-version 5 \
// RUN:   -enable-library-evolution \
// RUN:   -emit-module-interface-path %t/lib/Lib.swiftinterface \
// RUN:   -emit-module-path %t/lib/Lib.swiftmodule \
// RUN:   -emit-module-source-info-path %t/lib/Lib.swiftsourceinfo

// Build the client
// RUN: %target-swift-frontend -typecheck -primary-file %t/src/Client.swift \
// RUN:   -module-name Client -I %t/lib \
// RUN:   -swift-version 6

// REQUIRES: asserts
// REQUIRES: concurrency

//--- Lib.swift

public struct Test: Equatable {
  public static let value = Test(x: 0)

  public var x: UInt64

  private init(x: UInt64) {
    self.x = x
  }
}

//--- Client.swift
import Lib

public func test() -> Test {
  .value // Ok (no sendability errors)
}
