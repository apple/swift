// RUN: %target-swift-frontend -enable-copy-propagation=requested-passes-only -enable-lexical-lifetimes=false -sdk %S/Inputs -O -emit-sil -I %S/Inputs -enable-source-import -primary-file %s | %FileCheck %s

import gizmo

// CHECK: ModifyStruct
// CHECK-NOT:   = alloc_stack $Drill
// CHECK: ret
func ModifyStruct(inDrill : Drill) -> Int32 {
  var D : Drill = inDrill
  D.x += 3
  return D.x
}
