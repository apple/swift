// REQUIRES: objc_interop
// RUN: %empty-directory(%t.mod)
// RUN: %target-swift-frontend -swift-version 4 -emit-module -o %t.mod/Cities.swiftmodule %S/Inputs/Cities.swift -module-name Cities -parse-as-library
// RUN: %empty-directory(%t) && %target-swift-frontend -c -swift-version 4 -update-code -disable-migrator-fixits -primary-file %s  -I %t.mod -api-diff-data-file %S/Inputs/CallExpr.json -emit-migrated-file-path %t/call_expr_result.swift.result -o /dev/null
// RUN: diff -u %S/call_expr_result.swift.expected %t/call_expr_result.swift.result

import Cities

func foo() {
  let c1 = Cities(x: 3)
  _ = Cities.init(x: 3)
  _ = c1.noosa()
  _ = c1.name
  bar(c1.name)
}

func bar(_ n: String) {}
