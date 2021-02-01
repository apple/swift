// RUN: echo "-target x86_64-apple-macos11.0 -swift-version 5" >> %t-commonflags

// RUN: %swift @%t-commonflags -module-name MyModule -emit-ir %s -o - | %FileCheck %s
// RUN: %swift @%t-commonflags -module-name MyModule -emit-ir %s -o - -emit-dead-strippable-symbols | %FileCheck %s -check-prefix CHECK-DEADSTRIPPABLE

// REQUIRES: OS=macosx

public class MyClass {
}

// CHECK:      @"\01l_type_metadata_table" = private constant [1 x
// CHECK-SAME:   @"$s8MyModule0A5ClassCMn"
// CHECK-SAME: ], section "__TEXT, __swift5_types, regular, no_dead_strip", align 4

// CHECK:      @llvm.used = appending global [
// CHECK-SAME:   %swift.refcounted* (%T8MyModule0A5ClassC*)* @"$s8MyModule0A5ClassCfd"
// CHECK-SAME:   void (%T8MyModule0A5ClassC*)* @"$s8MyModule0A5ClassCfD"
// CHECK-SAME:   @"$s8MyModule0A5ClassCMn"
// CHECK-SAME:   @"$s8MyModule0A5ClassCMa"
// CHECK-SAME:   @"$s8MyModule0A5ClassCN"
// CHECK-SAME:   @"\01l_type_metadata_table"
// CHECK-SAME:   @__swift_reflection_version
// CHECK-SAME: ], section "llvm.metadata", align 8

// CHECK-DEADSTRIPPABLE:      @"\01l_type_metadata_$s8MyModule0A5ClassCMn" = private constant %swift.type_metadata_record
// CHECK-DEADSTRIPPABLE-SAME: @"$s8MyModule0A5ClassCMn"
// CHECK-DEADSTRIPPABLE-SAME: section "__TEXT, __swift5_types, regular, live_support", align 4

// CHECK-DEADSTRIPPABLE:      @llvm.used = appending global [
// CHECK-DEADSTRIPPABLE-NOT:    @"\01l_type_metadata_table"
// CHECK-DEADSTRIPPABLE-NOT:    @"\01l_type_metadata_$s8MyModule0A5ClassCMn"
// CHECK-DEADSTRIPPABLE-NOT:    %swift.refcounted* (%T8MyModule0A5ClassC*)* @"$s8MyModule0A5ClassCfd"
// CHECK-DEADSTRIPPABLE-NOT:    void (%T8MyModule0A5ClassC*)* @"$s8MyModule0A5ClassCfD"
// CHECK-DEADSTRIPPABLE-NOT:    @"$s8MyModule0A5ClassCMn"
// CHECK-DEADSTRIPPABLE-NOT:    @"$s8MyModule0A5ClassCMa"
// CHECK-DEADSTRIPPABLE-NOT:    @"$s8MyModule0A5ClassCN"
// CHECK-DEADSTRIPPABLE-SAME:   @__swift_reflection_version
// CHECK-DEADSTRIPPABLE-SAME: ], section "llvm.metadata", align 8
