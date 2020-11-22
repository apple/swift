// REQUIRES: objc_interop
// RUN: %empty-directory(%t) && %target-swift-frontend -c -swift-version 4 -update-code -primary-file %s -F %S/mock-sdk -api-diff-data-dir %S/Inputs/api-diff-data-dir -emit-migrated-file-path %t/api-diff-data-dir.swift.result -emit-remap-file-path %t/api-diff-data-dir.swift.remap -o /dev/null
// RUN: %FileCheck %s -input-file %t/api-diff-data-dir.swift.result -match-full-lines -check-prefix=SWIFT4

import Bar

func foo(_ b: BarForwardDeclaredClass) {
  b.barInstanceFunc1(0, anotherValue: 1, anotherValue1: 2, anotherValue2: 3)
  // SWIFT3:  b.barNewSwift3InstanceFunc1(newlabel1: 0, newlabel2: 1, newlabel3: 2, newlabel4: 3)
  // SWIFT4:  b.barNewSwift4InstanceFunc1(newlabel1: 0, newlabel2: 1, newlabel3: 2, newlabel4: 3)
  barGlobalFuncOldName(2)
  // SWIFT3:  barGlobalFuncNewSwift3OverlayName(newlabel: 2)
  // SWIFT4:  barGlobalFuncNewSwift4OverlayName(newlabel: 2)
}
