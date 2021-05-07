// RUN: %cxx-all-targets(-I %S/Inputs %s -emit-ir | %FileCheck %s)

// This test checks that the compiler uses well-known witness tables for types
// that have the appropriate size/shape (for example, { i8 x 4 } -> i32).

// These witness tables look very differnt on Windows so it doesn't make sense
// to test them in the same file.
// XFAIL: OS=windows-msvc

// Failing on 32-bit simulator, disable for now. rdar://73829982
// REQUIRES: rdar73829982

import WitnessLayoutOpts

protocol Dummy { }

// CHECK-DAG: @"$sBi8_WV" = external global i8*, align 8
// CHECK-DAG: @"$sSo5EmptyVMf" = linkonce_odr hidden constant {{.*}}@"$sBi8_WV"

extension Empty : Dummy { }

// CHECK-DAG: @"$sSo6I8SizeVMf" = linkonce_odr hidden constant {{.*}}@"$sBi8_WV"
extension I8Size : Dummy { }

// CHECK-DAG: @"$sSo7I32SizeVWV" = linkonce_odr hidden constant %swift.vwtable { {{.*}} i8* bitcast (i32 (%swift.opaque*, i32, %swift.type*)* @"$sSo7I32SizeVwet" to i8*), i8* bitcast (void (%swift.opaque*, i32, i32, %swift.type*)* @"$sSo7I32SizeVwst" to i8*)
// CHECK-DAG: "$sSo7I32SizeVMf" = linkonce_odr hidden constant {{.*}}@"$sSo7I32SizeVWV"
extension I32Size : Dummy { }
