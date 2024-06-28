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

public struct Struct1<IntType: FixedWidthInteger> {
  private var value: IntType

  public init(rawValue: IntType) {
    self.value = rawValue
  }
}

public class Class1 {
  public var index1: Struct1<UInt32> { .init(rawValue: 123) }
}

public struct GlyphIndex<IntType: FixedWidthInteger & UnsignedInteger> {
    private var value: IntType

    init(value: IntType) {
        self.value = value
    }
}

public struct QueryResult<GlyphIndexInt: UnsignedInteger & FixedWidthInteger> {
    public var glyphIDs: [GlyphIndex<GlyphIndexInt>]
}

public func makeQueryResult() -> QueryResult<UInt32> { .init(glyphIDs: []) }

// CHECK: supported

// CHECK: class SWIFT_SYMBOL("s:5Decls6Class1C") Class1 : public swift::_impl::RefCountedClass {
// CHECK: 'index1' cannot be printed

// CHECK: namespace Decls SWIFT_PRIVATE_ATTR SWIFT_SYMBOL_MODULE("Decls") {
// CHECK: namespace Decls SWIFT_PRIVATE_ATTR SWIFT_SYMBOL_MODULE("Decls") {
// CHECK: SWIFT_INLINE_THUNK void supportedFunc(const T_0_0& x) noexcept SWIFT_SYMBOL("s:5Decls13supportedFuncyyxlF") {

// CHECK: template<class T_0_0>
// CHECK-NEXT: #ifdef __cpp_concepts
// CHECK-NEXT: requires swift::isUsableInGenericContext<T_0_0>
// CHECK-NEXT: #endif // __cpp_concepts
// CHECK-NEXT: class GlyphIndex { } SWIFT_UNAVAILABLE_MSG("generic requirements for generic struct 'GlyphIndex' can not yet be represented in C++");

// CHECK: class Proto { } SWIFT_UNAVAILABLE_MSG("protocol 'Proto' can not yet be represented in C++");

// CHECK: template<class T_0_0>
// CHECK-NEXT: #ifdef __cpp_concepts
// CHECK-NEXT: requires swift::isUsableInGenericContext<T_0_0>
// CHECK-NEXT: #endif // __cpp_concepts
// CHECK-NEXT: class QueryResult { } SWIFT_UNAVAILABLE_MSG("generic requirements for generic struct 'QueryResult' can not yet be represented in C++");

// CHECK: class Struct1 { } SWIFT_UNAVAILABLE_MSG("generic requirements for generic struct 'Struct1' can not yet be represented in C++");

// CHECK: // Unavailable in C++: Swift global function 'unsupportedFunc(_:)'.

// CHECK: template<class T_0_0>
// CHECK-NEXT: #ifdef __cpp_concepts
// CHECK-NEXT: requires swift::isUsableInGenericContext<T_0_0>
// CHECK-NEXT: #endif // __cpp_concepts
// CHECK-NEXT: class unsupportedGenericClass { } SWIFT_UNAVAILABLE_MSG("generic generic class 'unsupportedGenericClass' can not yet be exposed to C++");
// CHECK-EMPTY:
// CHECK-NEXT: template<class T_0_0>
// CHECK-NEXT: #ifdef __cpp_concepts
// CHECK-NEXT: requires swift::isUsableInGenericContext<T_0_0>
// CHECK-NEXT: #endif // __cpp_concepts
// CHECK-NEXT: class unsupportedGenericEnum { } SWIFT_UNAVAILABLE_MSG("generic requirements for generic enum 'unsupportedGenericEnum' can not yet be represented in C++");
// CHECK-EMPTY:
// CHECK-NEXT: template<class T_0_0>
// CHECK-NEXT: #ifdef __cpp_concepts
// CHECK-NEXT: requires swift::isUsableInGenericContext<T_0_0>
// CHECK-NEXT: #endif // __cpp_concepts
// CHECK-NEXT: class unsupportedGenericStruct { } SWIFT_UNAVAILABLE_MSG("generic requirements for generic struct 'unsupportedGenericStruct' can not yet be represented in C++");
