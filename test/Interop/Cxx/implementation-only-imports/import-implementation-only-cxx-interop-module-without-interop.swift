// RUN: %empty-directory(%t)

// RUN: %target-swift-frontend %S/Inputs/use-module-a-impl-only.swift -I %S/Inputs/ -module-name UseModuleAImplOnly -emit-module -emit-module-path %t/UseModuleAImplOnly.swiftmodule -cxx-interoperability-mode=default

// RUN: %target-swift-frontend %s -typecheck -module-name TestMod -I %t -I %S/Inputs

// Check that we have used something from CxxShim in 'UseModuleAImplOnly'
// RUN: %target-swift-frontend %S/Inputs/use-module-a-impl-only.swift -I %S/Inputs/ -module-name UseModuleAImplOnly -emit-module -emit-module-path %t/UseModuleAImplOnly.swiftmodule -cxx-interoperability-mode=default -emit-sil -o - | %FileCheck %s
// CHECK: __swift_interopStaticCast

import UseModuleAImplOnly

public func testCallsAPI() {
    testUsesA()
}
