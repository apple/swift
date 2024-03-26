// RUN: %empty-directory(%t)
// RUN: split-file %s %t

// RUN: %target-swift-frontend -emit-module %t/A.swift -I %t -module-name A \
// RUN:   -disable-implicit-string-processing-module-import -disable-implicit-concurrency-module-import -parse-stdlib \
// RUN:   -enable-library-evolution -swift-version 5 \
// RUN:   -emit-module-interface-path %t/A.swiftinterface \
// RUN:   -o %t/A.swiftmodule

// RUN: %target-swift-frontend -scan-dependencies -module-load-mode prefer-interface -module-name Test -O -module-cache-path %t/clang-module-cache \
// RUN:   -disable-implicit-string-processing-module-import -disable-implicit-concurrency-module-import -parse-stdlib \
// RUN:   %t/test.swift -I %t -o %t/deps.json -cache-compile-job -cas-path %t/cas

// RUN: %{python} %S/Inputs/BuildCommandExtractor.py %t/deps.json clang:B > %t/B.cmd
// RUN: %swift_frontend_plain @%t/B.cmd

// RUN: %{python} %S/Inputs/BuildCommandExtractor.py %t/deps.json A > %t/A.cmd
// RUN: %swift_frontend_plain @%t/A.cmd

// RUN: %{python} %S/Inputs/GenerateExplicitModuleMap.py %t/deps.json > %t/map.json
// RUN: llvm-cas --cas %t/cas --make-blob --data %t/map.json > %t/map.casid
// RUN: %{python} %S/Inputs/BuildCommandExtractor.py %t/deps.json Test > %t/MyApp.cmd

// RUN: %target-swift-frontend -cache-compile-job -Rcache-compile-job %t/test.swift -O -emit-module -o %t/Test.swiftmodule \
// RUN:  -disable-implicit-string-processing-module-import -disable-implicit-concurrency-module-import -parse-stdlib \
// RUN:  -disable-implicit-swift-modules -explicit-swift-module-map-file @%t/map.casid \
// RUN:  -module-name Test -cas-path %t/cas @%t/MyApp.cmd -index-system-modules -index-store-path %t/db 2>&1 | %FileCheck --check-prefix=CACHE-MISS %s
// RUN: ls %t/db

/// Cache hit with a different index-store-path. Note cache hit will skip replay index data.
// RUN: %target-swift-frontend -cache-compile-job -Rcache-compile-job %t/test.swift -O -emit-module -o %t/Test.swiftmodule \
// RUN:  -disable-implicit-string-processing-module-import -disable-implicit-concurrency-module-import -parse-stdlib \
// RUN:  -disable-implicit-swift-modules -explicit-swift-module-map-file @%t/map.casid \
// RUN:  -module-name Test -cas-path %t/cas @%t/MyApp.cmd -index-system-modules -index-store-path %t/db2 2>&1 | %FileCheck --check-prefix=CACHE-HIT %s
// RUN: not ls %t/db2

// CACHE-MISS: remark: cache miss for input
// CACHE-HIT: remark: replay output file

//--- test.swift
import A
import B
func test() {}

//--- A.swift
func a() {}

//--- module.modulemap
module B {
  header "B.h"
  export *
}

//--- B.h
void b(void);
