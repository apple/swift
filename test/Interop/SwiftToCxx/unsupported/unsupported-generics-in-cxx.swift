// RUN: %empty-directory(%t)
// RUN: %target-swift-frontend %s -typecheck -module-name Decls -verify -clang-header-expose-decls=has-expose-attr -disable-availability-checking -emit-clang-header-path %t/decls.h

// RUN: cat %s | grep -v _expose > %t/clean.swift
// RUN: %target-swift-frontend %t/clean.swift -typecheck -module-name Decls -clang-header-expose-decls=all-public -disable-availability-checking -emit-clang-header-path %t/decls.h
// RUN: %FileCheck %s < %t/decls.h

// RUN: %check-interop-cxx-header-in-clang(%t/decls.h)

public protocol Proto { init() }

@_expose(Cxx)
public func supportedFunc<T>(_ x: T) {}

@_expose(Cxx) // expected-error {{generic requirements for global function 'unsupportedFunc' can not yet be represented in C++}}
public func unsupportedFunc<T: Proto>(_ x: T) {}

@_expose(Cxx) // expected-error {{generic generic class 'unsupportedGenericClass' can not yet be exposed to C++}}
public class unsupportedGenericClass<T> {
    var v: T?

    public init() {
        v = nil
    }
}

@_expose(Cxx) // expected-error {{generic requirements for generic struct 'unsupportedGenericStruct' can not yet be represented in C++}}
public struct unsupportedGenericStruct<T: Proto> {
    var v: T
}

@_expose(Cxx) // expected-error {{generic requirements for generic enum 'unsupportedGenericEnum' can not yet be represented in C++}}
public enum unsupportedGenericEnum<T: Proto> {
    case A
    case B(T)
}

// CHECK: supported

// CHECK: // Unavailable in C++: Swift global function 'unsupportedFunc(_:)'.

// CHECK: class unsupportedGenericClass { } SWIFT_UNAVAILABLE_MSG("generic generic class 'unsupportedGenericClass' can not yet be exposed to C++");
// CHECK-EMPTY:
// CHECK-NEXT: class unsupportedGenericEnum { } SWIFT_UNAVAILABLE_MSG("generic requirements for generic enum 'unsupportedGenericEnum' can not yet be represented in C++");
// CHECK-EMPTY:
// CHECK-NEXT: class unsupportedGenericStruct { } SWIFT_UNAVAILABLE_MSG("generic requirements for generic struct 'unsupportedGenericStruct' can not yet be represented in C++");
