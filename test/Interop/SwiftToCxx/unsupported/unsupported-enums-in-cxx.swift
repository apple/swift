// RUN: %empty-directory(%t)
// RUN: %target-swift-frontend %s -typecheck -module-name Functions -verify -clang-header-expose-decls=has-expose-attr -disable-availability-checking -emit-clang-header-path %t/functions.h

// RUN: cat %s | grep -v _expose > %t/clean.swift
// RUN: %target-swift-frontend %t/clean.swift -typecheck -module-name Functions -clang-header-expose-decls=all-public -disable-availability-checking -emit-clang-header-path %t/header.h
// RUN: %FileCheck %s < %t/header.h

// RUN: %check-interop-cxx-header-in-clang(%t/header.h)

public typealias FnType = () -> ()

@_expose(Cxx) // expected-error {{enum 'unsupportedEnumAssociatedValueType' can not be represented in C++ as one of its cases has an associated value with type that can't be represented in C++}}
public enum unsupportedEnumAssociatedValueType {
    case A
    case B(FnType)
}

@_expose(Cxx) // expected-error {{enum 'unsupportedEnumMultipleAssociatedValues' can not yet be represented in C++ as one of its cases has multiple associated values}}
public enum unsupportedEnumMultipleAssociatedValues {
    case A
    case B(Int, Int)
}

@_expose(Cxx) // expected-error {{indirect enum 'unsupportedEnumIndirect' can not yet be represented in C++}}
public indirect enum unsupportedEnumIndirect {
    case A
    case B
}

@_expose(Cxx) // expected-error {{enum 'unsupportedEnumProtocolType' can not be represented in C++ as one of its cases has an associated value with type that can't be represented in C++}}
public enum unsupportedEnumProtocolType {
    case A
    case B(Error)
}

// CHECK: class unsupportedEnumAssociatedValueType { } SWIFT_UNAVAILABLE_MSG("enum 'unsupportedEnumAssociatedValueType' can not be represented in C++ as one of its cases has an associated value with type that can't be represented in C++");
// CHECK-EMPTY:
// CHECK-NEXT: class unsupportedEnumIndirect { } SWIFT_UNAVAILABLE_MSG("indirect enum 'unsupportedEnumIndirect' can not yet be represented in C++");
// CHECK-EMPTY:
// CHECK-NEXT: class unsupportedEnumMultipleAssociatedValues { } SWIFT_UNAVAILABLE_MSG("enum 'unsupportedEnumMultipleAssociatedValues' can not yet be represented in C++ as one of its cases has multiple associated values");
// CHECK-EMPTY:
// CHECK-NEXT: class unsupportedEnumProtocolType { } SWIFT_UNAVAILABLE_MSG("enum 'unsupportedEnumProtocolType' can not be represented in C++ as one of its cases has an associated value with type that can't be represented in C++");
