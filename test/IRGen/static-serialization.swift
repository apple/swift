// RUN: %empty-directory(%t)
// RUN: %swift-target-frontend -static -emit-module -emit-module-path %t/StaticLibrary.swiftmodule -module-name StaticLibrary -DSTATIC_LIBRARY %s
// RUN: %swift-target-frontend -I%t -S %s -emit-ir -o - | %FileCheck %s

#if STATIC_LIBRARY
public final class S {
    public init() { }
    deinit {}
}

@_transparent
public func f() -> S { S() }
#else
import StaticLibrary
internal let s = f()
#endif

// CHECK-NOT: declare dllimport swiftcc ptr @"$s13StaticLibrary1SCACycfC"(ptr swiftself)
// CHECK: declare swiftcc ptr @"$s13StaticLibrary1SCACycfC"(ptr swiftself)
