// RUN: %target-swift-emit-ir %s -I %S/Inputs -enable-experimental-cxx-interop | %FileCheck %s

import ClassTemplateNonTypeParameter

let p = MagicIntPair()
let t = MagicIntTriple()

// CHECK: @"${{s4main1pSo0034MagicArrayInt32_UInt_2_zoAFhhiEngcVvp|s4main1pSo0036MagicArrayInt32_UInt64_2_JsAEiFiuomcVvp}}"
// CHECK: @"${{s4main1tSo0034MagicArrayInt32_UInt_3_zoAFhhiEngcVvp|s4main1tSo0036MagicArrayInt32_UInt64_3_JsAEiFiuomcVvp}}"
