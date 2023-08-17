// RUN: %empty-directory(%t)
// RUN: %target-swift-frontend -enable-library-evolution -module-name NewModule -emit-module-path %t/NewModule.swiftmodule -emit-module %S/Inputs/library_evolution_property_originally_defined_in_current_module_NewModule.swift
// RUN: %target-swift-frontend -target %target-cpu-apple-macos12 -I %t -module-name OldModule -emit-silgen %s | %FileCheck %s

// REQUIRES: OS=macosx

import NewModule

@inline(never)
public func use2<T>(_ t: T) {
    print(t)
}

// CHECK-NOT: @{{.*}}8property{{.*}}vau :

public func use() {
    // We should access `OldModuleType.property` via its getter exported from
    // NewModule, even though it was _originallyDefinedIn our module.
    // CHECK: function_ref @{{.*}}8property{{.*}}vgZ :
    use2(OldModuleType.property)
    print(OldModuleType.property)
}
// CHECK-NOT: @{{.*}}8property{{.*}}vau :
