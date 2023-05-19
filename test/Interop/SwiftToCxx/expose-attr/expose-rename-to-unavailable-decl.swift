// RUN: %empty-directory(%t)
// RUN: %target-swift-frontend %s -typecheck -module-name Expose -enable-experimental-cxx-interop -emit-clang-header-path %t/expose.h
// RUN: %FileCheck %s < %t/expose.h

// RUN: %check-interop-cxx-header-in-clang(%t/expose.h -Wno-error=unused-function)

// Verify that we do not emit unavailable C++ decl
// with a colliding name.

@_expose(Cxx, "Renamed")
public class TestMe {
}

public class Renamed<T> {}

// CHECK: class SWIFT_SYMBOL("s:6Expose6TestMeC") Renamed

// CHECK: // Unavailable in C++: Swift generic class 'Renamed'.
