// RUN: %target-swiftxx-frontend -I %S/Inputs -emit-ir -o - %s | %FileCheck %s

// XFAIL: OS=linux-android
// XFAIL: OS=linux-androideabi

import MemoryLayout

var v = PrivateMemberLayout()
// Check that even though the private member variable `a` is not imported, it is
// still taken into account for computing the layout of the class. In other
// words, we use Clang's, not Swift's view of the class to compute the memory
// layout.
// The important point here is that the second index is 1, not 0.
// CHECK: store i32 42, ptr getelementptr inbounds (%TSo19PrivateMemberLayoutV, ptr @"$s4main1vSo19PrivateMemberLayoutVvp", i32 0, i32 1), align 4
v.b = 42
