// RUN: %empty-directory(%t)
// RUN: split-file %s %t
// RUN: %sourcekitd-test -req=diags %t/test.swift -- %t/test.swift -Xcc -fmodule-map-file=%t/module.modulemap -module-cache-path %t/module-cache | %FileCheck %s
// Sleep for 1 second so that touching the modulemap modifies its timestamp on the second level.
// RUN: sleep 1
// RUN: touch %t/module.modulemap
// RUN: %sourcekitd-test -req=diags %t/test.swift -- %t/test.swift -Xcc -fmodule-map-file=%t/module.modulemap -module-cache-path %t/module-cache | %FileCheck %s

// CHECK: 'guard' body must not fall through

//--- test.swift

import Lib

func test(value: Bool) {
  guard value else {

  }
}

//--- module.modulemap

module Lib [system] {
  export *
}
