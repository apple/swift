// RUN: %empty-directory(%t)
// RUN: %target-swift-frontend %S/Inputs/SwiftTextCheckingResult.swift -I %S/Inputs -module-name SwiftTextCheckingResult -enable-objc-interop -emit-module -emit-module-path %t/SwiftTextCheckingResult.swiftmodule
// RUN: %target-typecheck-verify-swift -verify-ignore-unknown -I %t -I %S/Inputs -module-name SwiftTest -enable-objc-interop -enable-experimental-cxx-interop

// REQUIRES: objc_interop

import SwiftTextCheckingResult

let _ = foo()

// CHECK: @"$sSy10FoundationE20replacingOccurrences2of4with7options5rangeSSqd___qd_0_So22NSStringCompareOptionsVSnySS5IndexVGSgtSyRd__SyRd_0_r0_lF"
