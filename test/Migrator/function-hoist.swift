// REQUIRES: objc_interop
// RUN: %empty-directory(%t.mod)
// RUN: %target-swift-frontend -swift-version 4 -emit-module -o %t.mod/Cities.swiftmodule %S/Inputs/Cities.swift -module-name Cities -parse-as-library
// RUN: %empty-directory(%t) && %target-swift-frontend -c -swift-version 4 -update-code -disable-migrator-fixits -I %t.mod -primary-file %s -api-diff-data-file %S/Inputs/API-function-hoist.json -emit-migrated-file-path %t/function-hoist.swift.result -emit-remap-file-path %t/function-hoist.swift.remap -o /dev/null
// RUN: diff -u %S/function-hoist.swift.expected %t/function-hoist.swift.result

import Cities

func getCities() -> [Cities] { return [] }

func foo(_ c : Cities) {
  setCityProperty1(c, 1)
  setCityProperty1(getCities()[0], 1)
  globalCityFunc()
  setCityProperty2(c, 1+1+1, 2+2)
  setCityProperty2(getCities()[0], 1+1+1, 2+2)
  globalCityFunc2(c)
  globalCityFunc2(getCities()[0])
  _ = globalCityFunc3(c, 1) + 2
  _ = globalCityFunc4(c, 1, 2) + 2
  _ = globalCityFunc5() + 2 + 1
  var vc = Cities(x : 1)
  _ = globalCityPointerTaker(&vc, 2, 2)
  _ = globalCityPointerTaker(getCities(), 2, 2)
}

func foo1(_ flag: Bool, _ c1: Cities, _ c2: Cities) {
  _ = globalCityFunc4(flag ? c1 : c2, 1 + 1, 2 + 2)
}
