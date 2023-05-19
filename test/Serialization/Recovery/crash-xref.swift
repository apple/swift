/// Test xref error description by removing a type from a module after building
/// a client. This test disables deserialization recovery to hit a crash
/// reliably.
// RUN: %empty-directory(%t)
// RUN: %empty-directory(%t/partials)
// RUN: %empty-directory(%t/normal)
// RUN: %empty-directory(%t/errors)
// RUN: %empty-directory(%t/cache)

/// Compile module A with a type and an empty module B.
// RUN: %target-swift-frontend %s -emit-module-path %t/partials/A.swiftmodule -module-name A -D LIB -enable-library-evolution
// RUN: %target-swift-frontend %s -emit-module-path %t/partials/B.swiftmodule -module-name B -enable-library-evolution

/// Compile a client using the type from A.
// RUN: %target-swift-frontend %s -emit-module-path %t/normal/Client.swiftmodule -module-name Client -D CLIENT -I %t/partials -enable-library-evolution -emit-module-interface-path  %t/normal/Client.swiftinterface
// RUN: %target-swift-frontend %s -emit-module-path %t/errors/Client.swiftmodule -module-name Client -D CLIENT -I %t/partials -experimental-allow-module-with-compiler-errors

/// Force rebuilding from the swiftinterface.
// RUN: mv %t/normal/Client.swiftmodule %t/swap-Client.swiftmodule
// RUN: echo "import Client" | %target-swift-frontend -typecheck - -I %t/partials -I %t/normal -module-cache-path %t/cache/
//2> /dev/null
// RUN: mv %t/swap-Client.swiftmodule %t/normal/Client.swiftmodule

/// Swap A and B around! A is now empty and B defines the type.
// RUN: %target-swift-frontend %s -emit-module-path %t/partials/A.swiftmodule -module-name A
// RUN: %target-swift-frontend %s -emit-module-path %t/partials/B.swiftmodule -module-name B -D LIB

/// Read from the client to get an xref error to the type missing from A.
// RUN: not --crash %target-swift-frontend -emit-sil %t/normal/Client.swiftmodule -module-name Client -I %t/partials -disable-deserialization-recovery 2> %t/normal_stderr
// RUN: not --crash %target-swift-frontend -emit-sil %t/errors/Client.swiftmodule -module-name Client -I %t/partials -disable-deserialization-recovery 2> %t/error_stderr

// RUN: cat %t/normal_stderr | %FileCheck %s -check-prefixes=NORMALFAILURE
// NORMALFAILURE-LABEL: *** DESERIALIZATION FAILURE ***
// NORMALFAILURE-LABEL: *** If any module named here was modified in the SDK, please delete the ***
// NORMALFAILURE-LABEL: *** new swiftmodule files from the SDK and keep only swiftinterfaces.   ***
// NORMALFAILURE-NEXT: module 'Client', builder version {{.*}}', built from source, resilient, loaded from
// NORMALFAILURE-NEXT: Could not deserialize type for 'foo()'
// NORMALFAILURE-NEXT: Caused by: modularization issue on 'SomeType', reference from 'Client' not resolvable: expected in 'A' but found in 'B'
// NORMALFAILURE-NEXT: Cross-reference to module 'A'
// NORMALFAILURE-NEXT: ... SomeType

// RUN: cat %t/error_stderr | %FileCheck %s -check-prefixes=ALLOWFAILURE
// ALLOWFAILURE-LABEL: *** DESERIALIZATION FAILURE ***
// ALLOWFAILURE-LABEL: *** If any module named here was modified in the SDK, please delete the ***
// ALLOWFAILURE-LABEL: *** new swiftmodule files from the SDK and keep only swiftinterfaces.   ***
// ALLOWFAILURE-NEXT: module 'Client', builder version {{.*}}', built from source, non-resilient, built with -experimental-allow-module-with-compiler-errors, loaded from
// ALLOWFAILURE-NEXT: Could not deserialize type for 'foo()'
// ALLOWFAILURE-NEXT: Caused by: modularization issue on 'SomeType', reference from 'Client' not resolvable: expected in 'A' but found in 'B'
// ALLOWFAILURE-NEXT: Cross-reference to module 'A'
// ALLOWFAILURE-NEXT: ... SomeType

/// Test a swiftmodule rebuilt from the swiftinterface.
// RUN: not --crash %target-swift-frontend -emit-sil %t/cache/Client-*.swiftmodule -module-name Client -I %t/partials -disable-deserialization-recovery 2> %t/cache_stderr
// RUN: cat %t/cache_stderr | %FileCheck %s -check-prefixes=CACHEFAILURE
// CACHEFAILURE: module 'Client', builder version {{.*}}', built from swiftinterface, resilient

#if LIB
public struct SomeType {
    public init() {}
}
#elseif CLIENT
import A
import B

public func foo() -> A.SomeType { fatalError() }
#endif // Empty
