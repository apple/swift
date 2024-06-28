// RUN: %empty-directory(%t)
// RUN: split-file %s %t

// RUN: %target-swift-frontend -scan-dependencies -module-name Test -module-cache-path %t/clang-module-cache -O \
// RUN:   -disable-implicit-string-processing-module-import -disable-implicit-concurrency-module-import -parse-stdlib \
// RUN:   %t/main.swift -o %t/deps.json -swift-version 5 -cache-compile-job -cas-path %t/cas -I %t/include -F %t/frameworks

// RUN: %{python} %S/Inputs/BuildCommandExtractor.py %t/deps.json C > %t/C.cmd
// RUN: %swift_frontend_plain @%t/C.cmd

// RUN: %{python} %S/Inputs/GenerateExplicitModuleMap.py %t/deps.json > %t/map.json
// RUN: llvm-cas --cas %t/cas --make-blob --data %t/map.json > %t/map.casid

// RUN: %{python} %S/Inputs/BuildCommandExtractor.py %t/deps.json Test > %t/MyApp.cmd
// RUN: %FileCheck %s --check-prefix=CMD --input-file=%t/MyApp.cmd
// CMD: "-module-can-import"
// CMD-NEXT: "A.Sub"
// CMD-NEXT: "-module-can-import"
// CMD-NEXT: "B"
// CMD-NEXT: "-module-can-import-version"
// CMD-NEXT: "C"
// CMD-NEXT: "1.0"
// CMD-NEXT: "0"
// CMD-NEXT: "-module-can-import"
// CMD-NEXT: "D"
// CMD-NEXT: "-module-can-import-version"
// CMD-NEXT: "Simple"
// CMD-NEXT: "0"
// CMD-NEXT: "1000.0.0"

// RUN: %target-swift-frontend \
// RUN:   -typecheck -cache-compile-job -cas-path %t/cas \
// RUN:   -swift-version 5 -disable-implicit-swift-modules \
// RUN:   -disable-implicit-string-processing-module-import -disable-implicit-concurrency-module-import -parse-stdlib \
// RUN:   -module-name Test -explicit-swift-module-map-file @%t/map.casid \
// RUN:   %t/main.swift @%t/MyApp.cmd

//--- main.swift
#if canImport(A.Sub)
func a() {}
#endif

#if canImport(A.Missing)
import A.Missing
#endif

#if canImport(B)
func b() {}
#endif

// check version.
#if canImport(C, _version: 1.0)
import C
#endif

#if canImport(C, _version: 2.0)
import Missing
#endif

// check succeeded canImport followed by unsuccessful versioned canImport check.
#if canImport(D)
func imported() {}
#endif

#if canImport(D, _version: 2.0)
import Missing
#endif

// check underlyingVersion.
#if canImport(Simple, _underlyingVersion: 10)
func simple() {}
#endif

#if canImport(Simple, _underlyingVersion: 2000)
#error("wrong version")
#endif

func useA() {
  a()
  b()
  c()
  simple()
}

//--- include/module.modulemap
module A {
  module Sub {
    header "sub.h"
    export *
  }
}

module B {
  header "B.h"
  export *
}

//--- include/sub.h
void notused(void);

//--- include/B.h
void notused2(void);

//--- include/C.swiftinterface
// swift-interface-format-version: 1.0
// swift-module-flags: -module-name C -O -disable-implicit-string-processing-module-import -disable-implicit-concurrency-module-import -parse-stdlib -user-module-version 1.0
public func c() { }

//--- include/D.swiftinterface
// swift-interface-format-version: 1.0
// swift-module-flags: -module-name D -O -disable-implicit-string-processing-module-import -disable-implicit-concurrency-module-import -parse-stdlib -user-module-version 1.0
public func d() { }

//--- frameworks/Simple.framework/Modules/module.modulemap
framework module Simple {
  umbrella header "Simple.h"
  export *
  module * {
    export *
  }
}

//--- frameworks/Simple.framework/Headers/Simple.h
void simple(void);

//--- frameworks/Simple.framework/Simple.tbd
--- !tapi-tbd
tbd-version:     4
targets:         [ arm64-macos, arm64-ios, arm64-watchos, arm64-tvos, 
                   arm64-ios-simulator, arm64-watchos-simulator, arm64-tvos-simulator ]
flags:           [ not_app_extension_safe ]
install-name:    '/System/Library/Frameworks/Simple.framework/Versions/A/Simple'
current-version: 1000
...
