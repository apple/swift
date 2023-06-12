// RUN: %empty-directory(%t)
// RUN: %empty-directory(%t/sdk)
// RUN: split-file %s %t

/// Compile two library modules A and A_related, and a middle library LibWithXRef with a reference to a type in A.
// RUN: %target-swift-frontend %t/LibOriginal.swift -emit-module-path %t/A.swiftmodule -module-name A -I %t
// RUN: %target-swift-frontend %t/Empty.swift -emit-module-path %t/A_related.swiftmodule -module-name A_related
// RUN: %target-swift-frontend %t/LibWithXRef.swift -emit-module-path %t/sdk/LibWithXRef.swiftmodule -module-name LibWithXRef -I %t -swift-version 5 -enable-library-evolution

/// Move MyType from A to A_related, triggering most notes.
// RUN: %target-swift-frontend %t/EmptyOverlay.swift -emit-module-path %t/A.swiftmodule -module-name A -I %t
// RUN: %target-swift-frontend %t/LibOriginal.swift -emit-module-path %t/A_related.swiftmodule -module-name A_related -I %t
// RUN: not %target-swift-frontend -c -O %t/Client.swift -I %t -I %t/sdk -Rmodule-recovery -sdk %t/sdk -swift-version 4 2>&1 \
// RUN:   | %FileCheck --check-prefixes CHECK-MOVED %s

/// Main error downgraded to a remark.
// CHECK-MOVED: LibWithXRef.swiftmodule:1:1: remark: reference to type 'MyType' broken by a context change; 'MyType' was expected to be in 'A', but now a candidate is found only in 'A_related'

/// Contextual notes about the modules involved.
// CHECK-MOVED: <unknown>:0: note: the type was expected to be found in module 'A' at '
// CHECK-MOVED-SAME: A.swiftmodule'
// CHECK-MOVED: <unknown>:0: note: or expected to be found in the underlying module 'A' defined at '
// CHECK-MOVED-SAME: module.modulemap'
// CHECK-MOVED: <unknown>:0: note: the type was actually found in module 'A_related' at '
// CHECK-MOVED-SAME: A_related.swiftmodule'

/// More notes depending on the context
// CHECK-MOVED: <unknown>:0: note: the module 'LibWithXRef' was built with a Swift language version set to 5
// CHECK-MOVED-SAME: while the current invocation uses 4

// CHECK-MOVED: <unknown>:0: note: the module 'LibWithXRef' has enabled library-evolution; the following file may need to be deleted if the SDK was modified: '
// CHECK-MOVED-SAME: LibWithXRef.swiftmodule'
// CHECK-MOVED: <unknown>:0: note: declarations in the underlying clang module 'A' may be hidden by clang preprocessor macros
// CHECK-MOVED: <unknown>:0: note: the distributed module 'LibWithXRef' refers to the local module 'A'; this may be caused by header maps or search paths
// CHECK-MOVED: <unknown>:0: note: the type 'MyType' moved between related modules; clang preprocessor macros may affect headers shared between these modules
// CHECK-MOVED: LibWithXRef.swiftmodule:1:1: note: could not deserialize type for 'foo()'
// CHECK-MOVED: error: cannot find 'foo' in scope

/// Move A to the SDK, triggering a different note about layering.
// RUN: mv %t/A.swiftmodule %t/sdk/A.swiftmodule
// RUN: not %target-swift-frontend -c -O %t/Client.swift -I %t -I %t/sdk -Rmodule-recovery -sdk %t/sdk 2>&1 \
// RUN:   | %FileCheck --check-prefixes CHECK-LAYERING-FOUND %s
// CHECK-LAYERING-FOUND: <unknown>:0: note: the reference may break layering; the candidate was found in the local module 'A_related' for a reference from the distributed module 'LibWithXRef'
// CHECK-LAYERING-FOUND: error: cannot find 'foo' in scope

/// Delete A, keep only the underlying clangmodule for notes about clang modules.
// RUN: rm %t/sdk/A.swiftmodule
// RUN: %target-swift-frontend %t/Empty.swift -emit-module-path %t/A_related.swiftmodule -module-name A_related
// RUN: not %target-swift-frontend -c -O %t/Client.swift -I %t -I %t/sdk -Rmodule-recovery -sdk %t/sdk 2>&1 \
// RUN:   | %FileCheck --check-prefixes CHECK-CLANG %s
// CHECK-CLANG: <unknown>:0: note: declarations in the clang module 'A' may be hidden by clang preprocessor macros
// CHECK-CLANG: error: cannot find 'foo' in scope


//--- module.modulemap
module A {
    header "A.h"
}

//--- A.h
void foo() {}

//--- Empty.swift

//--- EmptyOverlay.swift
@_exported import A

//--- LibOriginal.swift
@_exported import A

public struct MyType {
    public init() {}
}

//--- LibWithXRef.swift
import A
import A_related

public func foo() -> MyType {
    fatalError()
}

//--- Client.swift
import LibWithXRef

foo()
