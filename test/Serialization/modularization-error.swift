/// Simulate typical modularization issues using Swift modules.
// RUN: %empty-directory(%t)
// RUN: split-file %s %t

/// Compile two library modules A and B, and a client.
// RUN: %target-swift-frontend %t/LibOriginal.swift -emit-module-path %t/A.swiftmodule -module-name A
// RUN: %target-swift-frontend %t/Empty.swift -emit-module-path %t/B.swiftmodule -module-name B
// RUN: %target-swift-frontend %t/LibWithXRef.swift -emit-module-path %t/LibWithXRef.swiftmodule -module-name LibWithXRef -I %t

/// Move MyType from A to B.
// RUN: %target-swift-frontend %t/Empty.swift -emit-module-path %t/A.swiftmodule -module-name A
// RUN: %target-swift-frontend %t/LibOriginal.swift -emit-module-path %t/B.swiftmodule -module-name B
// RUN: not %target-swift-frontend -emit-sil %t/LibWithXRef.swiftmodule -module-name LibWithXRef -I %t 2>&1 \
// RUN:   | %FileCheck --check-prefixes CHECK,CHECK-MOVED %s
// CHECK-MOVED: LibWithXRef.swiftmodule:1:1: error: reference to type 'MyType' broken by a context change; 'MyType' was expected to be in 'A', but now a candidate is found only in 'B'

/// Change MyType into a function.
// RUN: %target-swift-frontend %t/LibTypeChanged.swift -emit-module-path %t/A.swiftmodule -module-name A
// RUN: %target-swift-frontend %t/Empty.swift -emit-module-path %t/B.swiftmodule -module-name B
// RUN: not %target-swift-frontend -emit-sil %t/LibWithXRef.swiftmodule -module-name LibWithXRef -I %t 2>&1 \
// RUN:   | %FileCheck --check-prefixes CHECK,CHECK-KIND-CHANGED %s
// CHECK-KIND-CHANGED: LibWithXRef.swiftmodule:1:1: error: reference to type 'MyType' broken by a context change; the details of 'MyType' from 'A' changed since building 'LibWithXRef'

/// Change MyType into a function and move it.
// RUN: %target-swift-frontend %t/Empty.swift -emit-module-path %t/A.swiftmodule -module-name A
// RUN: %target-swift-frontend %t/LibTypeChanged.swift -emit-module-path %t/B.swiftmodule -module-name B
// RUN: not %target-swift-frontend -emit-sil %t/LibWithXRef.swiftmodule -module-name LibWithXRef -I %t 2>&1 \
// RUN:   | %FileCheck --check-prefixes CHECK,CHECK-KIND-CHANGED-AND-MOVED %s
// CHECK-KIND-CHANGED-AND-MOVED: LibWithXRef.swiftmodule:1:1: error: reference to type 'MyType' broken by a context change; the details of 'MyType' changed since building 'LibWithXRef', it was in 'A' and is now found in 'B'

/// Remove MyType from all imported modules.
// RUN: %target-swift-frontend %t/Empty.swift -emit-module-path %t/A.swiftmodule -module-name A
// RUN: %target-swift-frontend %t/Empty.swift -emit-module-path %t/B.swiftmodule -module-name B
// RUN: not %target-swift-frontend -emit-sil %t/LibWithXRef.swiftmodule -module-name LibWithXRef -I %t 2>&1 \
// RUN:   | %FileCheck --check-prefixes CHECK,CHECK-NOT-FOUND %s
// CHECK-NOT-FOUND: LibWithXRef.swiftmodule:1:1: error: reference to type 'MyType' broken by a context change; 'MyType' is not found, it was expected to be in 'A'

// CHECK: LibWithXRef.swiftmodule:1:1: note: could not deserialize extension

//--- Empty.swift
//--- LibOriginal.swift
public struct MyType {
    public init() {}
}

//--- LibTypeChanged.swift
/// Make it a function to fail filtering.
public func MyType() {}

//--- LibWithXRef.swift
import A
import B

public protocol Proto {}
extension MyType : Proto {}

@available(SwiftStdlib 5.1, *)
public func foo() -> some Proto { return MyType() }
