// RUN: %swift -target arm64-apple-macos11.0 -parse-stdlib %s -module-name Swift -emit-ir -o - | %FileCheck %s

public protocol Simple {}

public struct Other : Simple {}

// CHECK:      @"$ss6SimpleHr" = private constant
// CHECK-SAME: @"$ss6SimpleMp"
// CHECK-SAME: , section "__TEXT, __swift5_protos, regular"

// CHECK:      @"$ss5OtherVs6SimplesHc" = private constant
// CHECK-SAME: @"$ss5OtherVs6SimplesMc"
// CHECK-SAME: , section "__TEXT, __swift5_proto, regular"

// CHECK:      @"$ss5OtherVHn" = private constant
// CHECK-SAME: @"$ss5OtherVMn"
// CHECK-SAME: , section "__TEXT, __swift5_types, regular"

// CHECK:      @llvm.used = appending global [{{.*}} x ptr] [
// CHECK-SAME: @"$ss6SimpleHr"
// CHECK-SAME: @"$ss5OtherVs6SimplesHc"
// CHECK-SAME: @"$ss5OtherVHn"
// CHECK-SAME: ], section "llvm.metadata"
