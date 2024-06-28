// RUN: %empty-directory(%t)
// RUN: %target-swift-frontend -emit-module -enable-experimental-feature RawLayout -module-name raw_layout_fred -o %t %S/Inputs/raw_layout.swift
// RUN: %target-swift-frontend -I %t -I %S/Inputs -cxx-interoperability-mode=upcoming-swift -emit-ir %s -verify | %FileCheck %s

import raw_layout_fred
import RawLayoutCXX

// CHECK: %T15raw_layout_fred4FredVySbG = type <{ [1 x i8] }>

// CHECK-LABEL: @"$s{{[A-Za-z0-9_]*}}16WeirdCXXTypeCellVWV" = {{.*}} %swift.vwtable
// initializeWithTake
// CHECK-SAME:  , ptr @"$s10raw_layout16WeirdCXXTypeCellVwtk
// assignWithTake
// CHECK-SAME:  , ptr @"$s10raw_layout16WeirdCXXTypeCellVwta
// size
// CHECK-SAME:  , {{i64|i32}} 1
// stride
// CHECK-SAME:  , {{i64|i32}} 1
// flags: not copyable, not bitwise takable, not pod, not inline
// CHECK-SAME:  , i32 9633792
struct WeirdCXXTypeCell: ~Copyable {
  let cell: CellThatMovesLike<NonBitwiseTakableCXXType>
}

do {
  // CHECK: {{%.*}} = alloca %T15raw_layout_fred4FredVySbG
  // CHECK: call swiftcc void @"$s15raw_layout_fred4FredVACyxGycfC"
  let _ = Fred<Bool>()
}
