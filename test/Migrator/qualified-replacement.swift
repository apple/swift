// REQUIRES: objc_interop
// RUN: %empty-directory(%t.mod)
// RUN: %target-swift-frontend -swift-version 4 -emit-module -o %t.mod/Cities.swiftmodule %S/Inputs/Cities.swift -module-name Cities -parse-as-library
// RUN: %empty-directory(%t) && %target-swift-frontend -c -swift-version 4 -update-code -primary-file %s -F %S/mock-sdk -I %t.mod -api-diff-data-file %S/Inputs/qualified.json -emit-migrated-file-path %t/qualified-replacement.swift.result -emit-remap-file-path %t/qualified-replacement.swift.remap -o /dev/null
// RUN: diff -u %S/qualified-replacement.swift.expected %t/qualified-replacement.swift.result

import Cities
import Bar
func foo() {
  _ = PropertyUserInterface.fieldPlus
  PropertyUserInterface.methodPlus(1)
  _ = FooComparisonResult.orderedSame
  let _ : FooComparisonResult = .orderedSame
  _ = Cities.CityKind.Town
  _ = Cities.CityKind.Village
  _ = ToplevelType()
  _ = ToplevelType(recordName: "")
  bar(.orderedSame)
  bar(.orderedMemberSame)
  bar(FooComparisonResult.orderedSame)
  bar(FooComparisonResult.orderedMemberSame)
  bar(FooComparisonResult.orderedMovedToGlobal)
  bar(.orderedMovedToGlobal)
}

func foo(_: ToplevelType) {}
func bar(_ : FooComparisonResult) {}
