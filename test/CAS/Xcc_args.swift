// RUN: %empty-directory(%t)
// RUN: split-file %s %t

// RUN: %target-swift-frontend -scan-dependencies -module-name Test -module-cache-path %t/clang-module-cache -O \
// RUN:   -disable-implicit-string-processing-module-import -disable-implicit-concurrency-module-import \
// RUN:   %t/test.swift -o %t/deps.json -swift-version 5 -cache-compile-job -cas-path %t/cas -Xcc -D_VERSION=1 \
// RUN:   -Xcc -fmodule-map-file=%t/include/module.modulemap -Xcc -ivfsoverlay -Xcc %t/empty.yaml \
// RUN:   -Xcc -I%t/empty.hmap -module-load-mode prefer-serialized

// RUN: %{python} %S/Inputs/BuildCommandExtractor.py %t/deps.json clang:SwiftShims > %t/shims.cmd
// RUN: %swift_frontend_plain @%t/shims.cmd

// RUN: %{python} %S/Inputs/BuildCommandExtractor.py %t/deps.json clang:_Macro > %t/Macro.cmd
// RUN: %swift_frontend_plain @%t/Macro.cmd

// RUN: %{python} %S/Inputs/GenerateExplicitModuleMap.py %t/deps.json > %t/map.json
// RUN: llvm-cas --cas %t/cas --make-blob --data %t/map.json > %t/map.casid

// RUN: %{python} %S/Inputs/BuildCommandExtractor.py %t/deps.json Test > %t/MyApp.cmd
// RUN: %FileCheck %s --input-file=%t/MyApp.cmd

// CHECK: "-direct-clang-cc1-module-build"
// CHECK: "_VERSION=1"
// CHECK-NOT: hmap.json
// CHECK-NOT: -ivfsoverlay
// CHECK-NOT: -fmodule-map-file

// RUN: %target-swift-frontend \
// RUN:   -typecheck -cache-compile-job -cas-path %t/cas \
// RUN:   -swift-version 5 -disable-implicit-swift-modules \
// RUN:   -disable-implicit-string-processing-module-import -disable-implicit-concurrency-module-import \
// RUN:   -module-name Test -explicit-swift-module-map-file @%t/map.casid \
// RUN:   %t/test.swift @%t/MyApp.cmd

//--- test.swift
private import _Macro

public func test() {
  let _ = VERSION
}

//--- include/module.modulemap
module _Macro {
  umbrella "."
  export *
}

//--- include/Macro.h
#if defined(_VERSION)
#define VERSION _VERSION
#else
#define VERSION 0
#endif

//--- hmap.json
{
  "mappings": {}
}

//--- empty.yaml
{
  "version": 0,
  "case-sensitive": "false",
  "use-external-names": true,
  "roots": []
}
