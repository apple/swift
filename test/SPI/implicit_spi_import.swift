// RUN: %empty-directory(%t)
// RUN: split-file %s %t

// RUN: %target-swift-frontend -emit-module %t/Lib.swift \
// RUN:   -module-name Lib -swift-version 5 -I %t \
// RUN:   -enable-library-evolution \
// RUN:   -emit-module-path %t/Lib.swiftmodule \
// RUN:   -emit-module-interface-path %t/Lib.swiftinterface \
// RUN:   -emit-private-module-interface-path %t/Lib.private.swiftinterface

// RUN: %target-swift-frontend -typecheck -verify %t/ClientA.swift -I %t
// RUN: %target-swift-frontend -typecheck -verify %t/ClientB.swift -I %t
// RUN: %target-swift-frontend -typecheck -verify %t/ClientC.swift -I %t

// RUN: rm %t/Lib.swiftmodule
// RUN: %target-swift-frontend -typecheck -verify %t/ClientA.swift -I %t
// RUN: %target-swift-frontend -typecheck -verify %t/ClientB.swift -I %t
// RUN: %target-swift-frontend -typecheck -verify %t/ClientC.swift -I %t

// RUN: %target-swift-frontend -emit-module %t/ClientA.swift \
// RUN:   -module-name ClientA -swift-version 5 -I %t \
// RUN:   -enable-library-evolution \
// RUN:   -emit-module-interface-path %t/ClientA.swiftinterface \
// RUN:   -emit-private-module-interface-path %t/ClientA.private.swiftinterface
// RUN: %FileCheck %s --check-prefix CHECK-A < %t/ClientA.private.swiftinterface
// CHECK-A-NOT: @_spi(_) import Lib
// CHECK-A: import Lib
// CHECK-A: @_spi(_) public func useImplicit() -> Lib._Klass

// RUN: %target-swift-frontend -emit-module %t/ClientB.swift \
// RUN:   -module-name ClientB -swift-version 5 -I %t \
// RUN:   -enable-library-evolution \
// RUN:   -emit-module-interface-path %t/ClientB.swiftinterface \
// RUN:   -emit-private-module-interface-path %t/ClientB.private.swiftinterface
// RUN: %FileCheck %s --check-prefix CHECK-B < %t/ClientB.private.swiftinterface
// CHECK-B-NOT: @_spi(_) @_spi(core) import Lib
// CHECK-B-NOT: @_spi(core) @_spi(_) import Lib
// CHECK-B: @_spi(core) import Lib
// CHECK-B: @_spi(_) public func useImplicit() -> Lib._Klass
// CHECK-B: @_spi(core) public func useSPICore() -> Lib.CoreStruct

// RUN: %target-swift-frontend -emit-module %t/ClientZ.swift \
// RUN:   -module-name ClientZ -swift-version 5 -I %t \
// RUN:   -enable-library-evolution \
// RUN:   -emit-module-interface-path %t/ClientZ.swiftinterface \
// RUN:   -emit-private-module-interface-path %t/ClientZ.private.swiftinterface
// RUN: %FileCheck %s --check-prefix CHECK-Z < %t/ClientZ.private.swiftinterface
// CHECK-Z: @_spi(_) import Lib
// CHECK-Z: @_spi(_) public func useImplicit() -> Lib._Klass


//--- Lib.swift

@_spi(core)
public struct CoreStruct {
  public init() {}
}

@_spi(_)
public class _Klass {
  public init() {}
}

public protocol APIProtocol {
}


//--- ClientA.swift

import Lib

@_spi(_)
public func useImplicit() -> _Klass { return _Klass() }

public func useMain() -> APIProtocol? { return nil }


//--- ClientB.swift

@_spi(core) import Lib

@_spi(_)
public func useImplicit() -> _Klass { return _Klass() }

@_spi(core)
public func useSPICore() -> CoreStruct { return CoreStruct() }

public func useMain() -> APIProtocol? { return nil }


//--- ClientC.swift

import Lib

public func useImplicit() -> _Klass { return _Klass() } // expected-error{{cannot use class '_Klass' here; it is an SPI imported from 'Lib'}}

@_spi(core)
public func useSPICore() -> CoreStruct { return CoreStruct() } // expected-error{{cannot find type 'CoreStruct' in scope}}

public func useMain() -> APIProtocol? { return nil }


//--- ClientZ.swift

@_spi(_) import Lib

@_spi(_)
public func useImplicit() -> _Klass { return _Klass() }

public func useMain() -> APIProtocol? { return nil }
