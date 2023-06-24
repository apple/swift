// RUN: %empty-directory(%t)
// RUN: %target-swift-frontend -I %t -emit-ir  -disable-availability-checking -enable-library-evolution %s | %IRGenFileCheck %s
// REQUIRES: concurrency

// CHECK: @"$s13default_actor1ACMn" = hidden constant
//   0x81800050: 0x01800000 IsActor + IsDefaultActor
//   0x81810050: the same, but using a singleton metadata initialization
// CHECK-SAME: i32 {{-2122317744|-2122252208}},

// CHECK-LABEL: define hidden swiftcc void @"$s13default_actor1ACfD"(ptr swiftself %0)
// CHECK-NOT: ret void
// CHECK:     call swiftcc void @swift_defaultActor_deallocate(
// CHECK:     ret void
actor A {}
