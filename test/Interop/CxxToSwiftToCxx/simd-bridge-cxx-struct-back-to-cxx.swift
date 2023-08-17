// RUN: %empty-directory(%t)
// RUN: split-file %s %t

// RUN: %target-swift-frontend(mock-sdk: %clang-importer-sdk) -typecheck %t/use-cxx-types.swift -typecheck -module-name UseCxxTy -emit-clang-header-path %t/UseCxxTy.h -I %t -enable-experimental-cxx-interop -disable-availability-checking -Xcc -DSIMD_NO_CODE

// RUN: %FileCheck %s < %t/UseCxxTy.h

// FIXME: remove once https://github.com/apple/swift/pull/60971 lands.
// RUN: echo "#include \"header.h\"" > %t/full-cxx-swift-cxx-bridging.h
// RUN: cat %t/UseCxxTy.h >> %t/full-cxx-swift-cxx-bridging.h

// RUN: %check-interop-cxx-header-in-clang(%t/full-cxx-swift-cxx-bridging.h -Wno-reserved-identifier -DSIMD_NO_CODE)

// This is required to verify that `Struct` is returned and passed directly.
// REQUIRES: OS=macosx
// REQUIRES: PTRSIZE=64

//--- header.h

#include <simd.h>

using simd_float4x4 = float4x4;

//--- module.modulemap
module CxxTest {
    header "header.h"
    requires cplusplus
}

//--- use-cxx-types.swift
import CxxTest

public struct Struct {
    private let transform: simd_float4x4

    public init() {
        transform = simd_float4x4()
    }
}

public func passStruct(_ x : Struct) {
    
}

// CHECK: class SWIFT_SYMBOL("s:8UseCxxTy6StructV") Struct final {
// CHECK-NOT: init(
// CHECK:  // Unavailable in C++: Swift global function 'passStruct(_:)'
