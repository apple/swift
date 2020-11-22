// REQUIRES: objc_interop
// RUN: %empty-directory(%t)
// RUN: %target-swift-frontend -c -swift-version 4 -update-code -primary-file %s -F %S/mock-sdk -api-diff-data-file %S/Inputs/API.json -emit-migrated-file-path %t/property.swift.result -disable-migrator-fixits -o /dev/null
// RUN: diff -u %S/property.swift.expected %t/property.swift.result

import Bar

func foo(_ a : PropertyUserInterface) {
  a.setField(1)
  a.setURL(1)
  _ = a.field()
}

class C: PropertyUserInterface {
  public override func field() -> Int32 { return 1 }
  public override func field2() -> UnsafeMutablePointer<Int32>? { return nil }
}
