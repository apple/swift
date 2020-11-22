// REQUIRES: objc_interop
// RUN: %empty-directory(%t.mod)
// RUN: %target-swift-frontend -swift-version 4 -emit-module -o %t.mod/Cities.swiftmodule %S/Inputs/Cities.swift -module-name Cities -parse-as-library
// RUN: %empty-directory(%t) && %target-swift-frontend -c -swift-version 4 -update-code -disable-migrator-fixits -primary-file %s  -I %t.mod -api-diff-data-file %S/Inputs/API.json -emit-migrated-file-path %t/rename-func-decl.swift.result -o %t/rename-func-decl.swift.remap
// RUN: diff -u %S/rename-func-decl.swift.expected %t/rename-func-decl.swift.result

import Cities

class MyCities : MoreCities {
  func setZooLocation(x ix: Int, y iy: Int, z iz: Int) {}
  func addZooAt(_ x: Int, y: Int, z: Int) {}
}

class MySubTopLevelType: ToplevelType {
  override func member(_ x: @escaping ([Any]?) -> Void) {}
}

class MySubTopLevelType2: ToplevelType {
  override func member(_ x: @escaping (((([(Any)])?))) -> Void) {}
}

class SubCities: Cities {
  override var yogurt: Int { return 2 }
}

func boo(_ a: ToplevelType) {
  a.trailing {  print($0!) }
}
