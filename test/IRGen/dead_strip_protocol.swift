// RUN: echo "-target x86_64-apple-macos11.0 -swift-version 5 -disable-reflection-metadata" >> %t-commonflags

// RUN: %swift @%t-commonflags -module-name MyModule -emit-ir %s -o - | %FileCheck %s
// RUN: %swift @%t-commonflags -module-name MyModule -emit-ir %s -o - -emit-dead-strippable-symbols | %FileCheck %s -check-prefix CHECK-DEADSTRIPPABLE

// REQUIRES: OS=macosx

public protocol MyProtocol {
}

// CHECK:      @"\01l_protocols" = private constant [1 x
// CHECK-SAME:   @"$s8MyModule0A8ProtocolMp"
// CHECK-SAME: ], section "__TEXT, __swift5_protos, regular, no_dead_strip", align 4

// CHECK:      @llvm.used = appending global [
// CHECK-SAME:   @"$s8MyModule0A8ProtocolMp"
// CHECK-SAME:   @"\01l_protocols"
// CHECK-SAME:   @__swift_reflection_version
// CHECK-SAME: ], section "llvm.metadata", align 8

// CHECK-DEADSTRIPPABLE:      @"\01l_protocol_$s8MyModule0A8ProtocolMp" = private constant %swift.protocolref
// CHECK-DEADSTRIPPABLE-SAME: @"$s8MyModule0A8ProtocolMp"
// CHECK-DEADSTRIPPABLE-SAME: section "__TEXT, __swift5_protos, regular, live_support", align 4

// CHECK-DEADSTRIPPABLE:      @llvm.used = appending global [
// CHECK-DEADSTRIPPABLE-NOT:    @"$s8MyModule0A8ProtocolMp"
// CHECK-DEADSTRIPPABLE-NOT:    @"\01l_protocols"
// CHECK-DEADSTRIPPABLE-SAME:   @__swift_reflection_version
// CHECK-DEADSTRIPPABLE-SAME: ], section "llvm.metadata", align 8
