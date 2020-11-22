// REQUIRES: objc_interop
// RUN: %empty-directory(%t) && %target-swift-frontend -c -swift-version 4 -update-code -primary-file %s -F %S/mock-sdk -api-diff-data-file %S/Inputs/API.json -emit-migrated-file-path %t/rename-init.swift.result -disable-migrator-fixits -o /dev/null
// RUN: diff -u %S/rename-init.swift.expected %t/rename-init.swift.result

import Bar

func foo() {
  _ = BarForwardDeclaredClass(oldLabel0:1)
}
