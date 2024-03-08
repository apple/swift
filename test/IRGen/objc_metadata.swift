// RUN: %target-swift-frontend -module-name main -emit-ir %s | %FileCheck %s

// REQUIRES: objc_interop

import Foundation

@_silgen_name("useMetadata")
func useMetadata<T>(_: T.Type)

private final class Final: NSObject {
    @objc dynamic func foo() {
        // CHECK: call swiftcc %swift.metadata_response @"$s4main5Final
        useMetadata(Final.self)
    }
}

private class NotFinal: NSObject {
    @objc dynamic func foo() {
        // CHECK: call swiftcc %swift.metadata_response @"$s4main8NotFinal
        useMetadata(NotFinal.self)
    }
}

// CHECK-NOT: %.Type = call %swift.type* @swift_getObjectType