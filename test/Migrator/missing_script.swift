// RUN: %empty-directory(%t) && not %target-swift-frontend -c -swift-version 4 -update-code -primary-file %s -F %S/mock-sdk -api-diff-data-file %S/Inputs/NoSuchFile.json -emit-migrated-file-path %t/missing_script.swift.result -emit-remap-file-path %t/missing_script.swift.remap -o /dev/null &> %t.diag
// RUN: %FileCheck %s < %t.diag
// CHECK: missing migration script from path
