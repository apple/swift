// RUN: %target-swift-frontend -primary-file %s -target %target-cpu-apple-macosx10.15 -emit-ir -o - | %FileCheck %s

// Marker protocols should have no ABI impact at all, so this source file checks
// for the absence of symbols related to marker protocols.

// CHECK-NOT: $s26marker_protocol_backdeploy1PP
// CHECK-NOT: $s26marker_protocol_backdeploy1PMp

// REQUIRES: PTRSIZE=64
// REQUIRES: OS=macosx

// Temporarily disable on arm (rdar://89910199)
// UNSUPPORTED: CPU=arm64, CPU=arm64e

@_marker public protocol P { }
public protocol Q: P { }
protocol R { }


// Suppress marker protocols when forming existentials at runtime
public func takeAnyType<T>(_: T.Type) { }

// CHECK-LABEL: define {{.*}}@"$ss8Sendable_26marker_protocol_backdeploy1QAB1RpMa"
// CHECK: ss8Sendable_26marker_protocol_backdeploy1QAB1RpML
// CHECK-NOT: Sendable
// CHECK: s26marker_protocol_backdeploy1QMp
// CHECK-NOT: Sendable
// CHECK: s26marker_protocol_backdeploy1RMp
// CHECK-NOT: SENDABLE
// CHECK: swift_getExistentialTypeMetadata
public func passExistentialType() {
  typealias Fn = (Sendable & P & Q & R) async -> Void
  takeAnyType(Fn.self)
}
