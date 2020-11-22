// REQUIRES: objc_interop
// RUN: %empty-directory(%t) && %target-swift-frontend -c -swift-version 4 -update-code -primary-file %s -F %S/mock-sdk -api-diff-data-file %S/Inputs/API.json -emit-migrated-file-path %t/rename.swift.result -emit-remap-file-path %t/rename.swift.remap -o /dev/null -disable-migrator-fixits
// RUN: diff -u %S/rename.swift.expected %t/rename.swift.result

import Bar

func foo(_ b: BarForwardDeclaredClass) {
  b.barInstanceFunc1(0, anotherValue: 1, anotherValue1: 2, anotherValue2: 3)
  barGlobalFuncOldName(2)
  b.barInstanceFunc1(0, anotherValue: 1, anotherValue1: 2, anotherValue2: 3)
  barGlobalFuncOldName(2)
  b.barInstanceFunc1(0, anotherValue: 1, anotherValue1: 2, anotherValue2: 3)
  barGlobalFuncOldName(2)
  b.barInstanceFunc1(0, anotherValue: 1, anotherValue1: 2, anotherValue2: 3)
  barGlobalFuncOldName(2)
  b.barInstanceFunc1(0, anotherValue: 1, anotherValue1: 2, anotherValue2: 3)
  barGlobalFuncOldName(2)
  _ = barGlobalVariableOldEnumElement
  _ = PropertyUserInterface.methodPlus()
  let _: BarBaseNested
  _ = barGlobalHoistedFuncOldName(0, 1, 2)
}

func foo1(_ b: BarForwardDeclaredClass) {
  b.barInstanceFunc2(0,toRemove:1,toRemove1:2,toRemove2:3)
}
