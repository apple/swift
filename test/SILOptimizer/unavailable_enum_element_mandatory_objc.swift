// RUN: %target-swift-emit-sil -module-name Test -parse-as-library %s -verify -unavailable-decl-optimization=none -Onone -enable-objc-interop -disable-objc-attr-requires-foundation-module -import-objc-header %S/Inputs/switch_enum_objc.h | %FileCheck %s
// RUN: %target-swift-emit-sil -module-name Test -parse-as-library %s -verify -unavailable-decl-optimization=complete -Onone -enable-objc-interop -disable-objc-attr-requires-foundation-module -import-objc-header %S/Inputs/switch_enum_objc.h | %FileCheck %s

// CHECK-LABEL: sil @$s4Test30testFullyCoveredSwitchOpenEnumyySo9DimensionVF : $@convention(thin) (Dimension) -> () {
// CHECK:         switch_enum %0 : $Dimension, case #Dimension.x!enumelt: [[XBB:bb[0-9]+]], case #Dimension.z!enumelt: [[ZBB:bb[0-9]+]], case #Dimension.y!enumelt: [[YBB:bb[0-9]+]], default [[DEFAULTBB:bb[0-9]+]]
// CHECK:       [[ZBB]]:
// CHECK-NOT:     unreachable
// CHECK:       [[YBB]]:
// CHECK:       } // end sil function '$s4Test30testFullyCoveredSwitchOpenEnumyySo9DimensionVF'
public func testFullyCoveredSwitchOpenEnum(_ e: Dimension) {
  switch e {
  case .x: ()
  case .z: ()
  case .y: ()
  }
}

// CHECK-LABEL: sil @$s4Test32testFullyCoveredSwitchClosedEnumyySo10UnfairCoinVF : $@convention(thin) (UnfairCoin) -> () {
// CHECK:         switch_enum %0 : $UnfairCoin, case #UnfairCoin.heads!enumelt: [[HEADSBB:bb[0-9]+]], case #UnfairCoin.tails!enumelt: [[TAILSBB:bb[0-9]+]], default [[DEFAULTBB:bb[0-9]+]]
// CHECK:       [[TAILSBB]]:
// CHECK-NOT:     unreachable
// CHECK:       [[DEFAULTBB]]:
// CHECK:       } // end sil function '$s4Test32testFullyCoveredSwitchClosedEnumyySo10UnfairCoinVF'
public func testFullyCoveredSwitchClosedEnum(_ e: UnfairCoin) {
  switch e {
  case .heads: ()
  case .tails: ()
  }
}
