// REQUIRES: objc_interop
// RUN: %empty-directory(%t) && %target-swift-frontend -c -swift-version 4 -update-code -primary-file %s -F %S/mock-sdk -api-diff-data-file %S/Inputs/API.json -emit-migrated-file-path %t/member.swift.result -emit-remap-file-path %t/member.swift.remap -o /dev/null
// RUN: diff -u %S/member.swift.expected %t/member.swift.result

import Bar

func foo(_ b: BarForwardDeclaredClass, _ s: SomeItemSet) -> Int32 {
  let _ = s.theSimpleOldName
  let _ = s.theSimpleOldNameNotToRename
  return barGlobalVariable
}
