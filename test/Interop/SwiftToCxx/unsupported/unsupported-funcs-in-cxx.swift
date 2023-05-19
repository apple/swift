// RUN: %empty-directory(%t)
// RUN: %target-swift-frontend %s -typecheck -module-name Functions -verify -clang-header-expose-decls=has-expose-attr -disable-availability-checking -emit-clang-header-path %t/functions.h

// RUN: cat %s | grep -v _expose > %t/clean.swift
// RUN: %target-swift-frontend %t/clean.swift -typecheck -module-name Functions -clang-header-expose-decls=all-public -disable-availability-checking -emit-clang-header-path %t/header.h
// RUN: %FileCheck %s < %t/header.h

// RUN: %check-interop-cxx-header-in-clang(%t/header.h)

public func supported() {}

@_expose(Cxx) // expected-error {{global function 'unsupportedAEIC()' can not be exposed to C++ as it requires code to be emitted into client}}
@_alwaysEmitIntoClient
public func unsupportedAEIC() {}

@_expose(Cxx) // expected-error {{global function 'unsupportedThrows()' can not yet be represented in C++ as it may throw an error}}
public func unsupportedThrows() throws {}

@_expose(Cxx)
public struct HasMethods {
    let x: Int

    @_expose(Cxx) // expected-error {{instance method 'unsupportedThrowsMethod()' can not yet be represented in C++ as it may throw an error}}
    public func unsupportedThrowsMethod() throws {}

    @_expose(Cxx) // expected-error {{initializer 'init()' can not yet be represented in C++ as it may throw an error}}
    public init() throws {
        self.x = 0
    }

    @_expose(Cxx) // expected-error {{property 'unsupportedProp' can not yet be represented in C++ as it may throw an error}}
    public var unsupportedProp: Int {
        get throws {
            return 42
        }
    }

    @_expose(Cxx) // expected-error {{property 'unsupportedAEICProp' can not be exposed to C++ as it requires code to be emitted into client}}
    @_alwaysEmitIntoClient
    public var unsupportedAEICProp: Bool {
        return false
    }
}

// CHECK: HasMethods
// CHECK: supported

// CHECK: // Unavailable in C++: Swift global function 'unsupportedAEIC()'.
// CHECK-EMPTY:
// CHECK-NEXT: // Unavailable in C++: Swift global function 'unsupportedThrows()'.
