// RUN: %empty-directory(%t)
// RUN: split-file %s %t

// RUN: %target-swift-frontend -emit-module %t/Lib.swift \
// RUN:   -module-name Lib -I %t \
// RUN:   -emit-module-path %t/Lib.swiftmodule

// RUN: %target-swift-frontend -typecheck -verify %t/Client.swift -I %t


//--- Lib.swift
@_spi(core)
public protocol SPIProto {
  func foo()
}

@_spi(core)
extension SPIProto {
  public func foo() {}
}

//--- Client.swift

@_spi(core) import Lib

public class Klass {}

@_spi(core)
extension Klass: SPIProto {
  func bar() {}
}
