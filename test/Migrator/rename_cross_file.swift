// REQUIRES: objc_interop
// RUN: %empty-directory(%t.mod) && %target-swift-frontend -swift-version 4 -emit-module -o %t.mod/Cities.swiftmodule %S/Inputs/Cities.swift -module-name Cities -parse-as-library
// RUN: %empty-directory(%t) && %target-swift-frontend -c -swift-version 4 -update-code -primary-file %s %S/Inputs/rename_cross_file_2.swift -F %S/mock-sdk -I %t.mod -api-diff-data-file %S/Inputs/API.json -emit-migrated-file-path %t/rename_cross_file.swift.result -emit-remap-file-path %t/rename_cross_file.swift.remap -o /dev/null
// RUN: diff -u %S/rename_cross_file.swift.expected %t/rename_cross_file.swift.result

import Cities

extension FontWeight {
  init(x: Int, y: Int) { fatalError() }
  init(x: Int, y: Int, _ z: Int) { fatalError() }
}

func foo() {
  _ = FontWeight(rawValue: 1)
  _ = FontWeight(string: "light")
  _ = FontWeight(x: 2, y: 2, z: 2)
  _ = FontWeight(x: 2, 2)
}
