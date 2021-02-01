// RUN: echo "-target x86_64-apple-macos11.0 -swift-version 5" >> %t-commonflags

// RUN: %swift @%t-commonflags -module-name MyModule -emit-ir %s -o - | %FileCheck %s
// RUN: %swift @%t-commonflags -module-name MyModule -emit-ir %s -o - -emit-dead-strippable-symbols | %FileCheck %s -check-prefix CHECK-DEADSTRIPPABLE

// REQUIRES: OS=macosx

public func myfunc() {
}

// CHECK:      @llvm.used = appending global [
// CHECK-SAME:   @"$s8MyModule6myfuncyyF"
// CHECK-SAME:   @__swift_reflection_version
// CHECK-SAME: ], section "llvm.metadata", align 8

// CHECK-DEADSTRIPPABLE:      @llvm.used = appending global [
// CHECK-DEADSTRIPPABLE-NOT:    @"$s8MyModule6myfuncyyF"
// CHECK-DEADSTRIPPABLE-SAME:   @__swift_reflection_version
// CHECK-DEADSTRIPPABLE-SAME: ], section "llvm.metadata", align 8
