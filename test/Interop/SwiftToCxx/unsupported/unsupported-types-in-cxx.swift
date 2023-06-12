// RUN: %empty-directory(%t)
// RUN: %target-swift-frontend %s -typecheck -module-name Functions -clang-header-expose-decls=all-public -emit-clang-header-path %t/functions.h
// RUN: %FileCheck %s < %t/functions.h

// RUN: %check-interop-cxx-header-in-clang(%t/functions.h)

public func takeFloat(_ x: Float) {}

public func takesTuple(_ x: (Float, Float)) {}

public func takesVoid(_ x: ()) {}

// CHECK:     takeFloat

public enum MoveOnlyEnum: ~Copyable {
    case a
}

// CHECK: class MoveOnlyEnum { } SWIFT_UNAVAILABLE_MSG("noncopyable enum 'MoveOnlyEnum' can not yet be represented in C++");

public struct MoveOnlyStruct: ~Copyable {
    let x: Int
}

// CHECK: class MoveOnlyStruct { } SWIFT_UNAVAILABLE_MSG("noncopyable struct 'MoveOnlyStruct' can not yet be represented in C++");

public protocol TestProtocol {}

// CHECK: class TestProtocol { } SWIFT_UNAVAILABLE_MSG("protocol 'TestProtocol' can not yet be represented in C++");

// CHECK: // Unavailable in C++: Swift global function 'takesTuple(_:)'.
// CHECK: // Unavailable in C++: Swift global function 'takesVoid(_:)'.

public typealias unsupportedTypeAlias = () -> (Float, Float)

// CHECK: // Unavailable in C++: Swift type alias 'unsupportedTypeAlias'
