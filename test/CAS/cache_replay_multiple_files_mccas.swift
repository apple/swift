// REQUIRES: OS=macosx
// RUN: %empty-directory(%t)
// RUN: split-file %s %t

// RUN: %target-swift-frontend -scan-dependencies -module-name Test -O \
// RUN:   -disable-implicit-string-processing-module-import -disable-implicit-concurrency-module-import -parse-stdlib \
// RUN:   %t/test.swift %t/foo.swift -o %t/deps.json -cache-compile-job -cas-backend -cas-backend-mode=verify -cas-path %t/cas

// RUN: %{python} %S/Inputs/GenerateExplicitModuleMap.py %t/deps.json > %t/map.json
// RUN: llvm-cas --cas %t/cas --make-blob --data %t/map.json > %t/map.casid

// RUN: %{python} %S/Inputs/BuildCommandExtractor.py %t/deps.json Test > %t/MyApp.cmd
// RUN: echo "\"-disable-implicit-string-processing-module-import\"" >> %t/MyApp.cmd
// RUN: echo "\"-disable-implicit-concurrency-module-import\"" >> %t/MyApp.cmd
// RUN: echo "\"-parse-stdlib\"" >> %t/MyApp.cmd

/// Test compile multiple inputs with batch mode.
// RUN: %target-swift-frontend -cache-compile-job -cas-backend -cas-backend-mode=verify -Rcache-compile-job %t/test.swift %t/foo.swift -emit-module -o %t/Test.swiftmodule \
// RUN:  -module-name Test -cas-path %t/cas @%t/MyApp.cmd 2>&1 | %FileCheck --check-prefix=CACHE-MISS %s
// RUN: %target-swift-frontend -cache-compile-job -cas-backend -cas-backend-mode=verify -Rcache-compile-job -primary-file %t/test.swift %t/foo.swift -c -o %t/test.o  \
// RUN:  -module-name Test -cas-path %t/cas @%t/MyApp.cmd 2>&1 | %FileCheck --check-prefix=CACHE-MISS %s
// RUN: %target-swift-frontend -cache-compile-job -cas-backend -cas-backend-mode=verify -Rcache-compile-job %t/test.swift -primary-file %t/foo.swift -c -o %t/foo.o  \
// RUN:  -module-name Test -cas-path %t/cas @%t/MyApp.cmd 2>&1 | %FileCheck --check-prefix=CACHE-MISS %s

/// Expect cache hit second time
// RUN: %target-swift-frontend -cache-compile-job -cas-backend -cas-backend-mode=verify -Rcache-compile-job %t/test.swift %t/foo.swift -emit-module -o %t/Test.swiftmodule \
// RUN:  -module-name Test -cas-path %t/cas @%t/MyApp.cmd 2>&1 | %FileCheck --check-prefix=CACHE-HIT %s
// RUN: %target-swift-frontend -cache-compile-job -cas-backend -cas-backend-mode=verify -Rcache-compile-job -primary-file %t/test.swift %t/foo.swift -c -o %t/test.o  \
// RUN:  -module-name Test -cas-path %t/cas @%t/MyApp.cmd 2>&1 | %FileCheck --check-prefix=CACHE-HIT %s
// RUN: %target-swift-frontend -cache-compile-job -cas-backend -cas-backend-mode=verify -Rcache-compile-job %t/test.swift -primary-file %t/foo.swift -c -o %t/foo.o  \
// RUN:  -module-name Test -cas-path %t/cas @%t/MyApp.cmd 2>&1 | %FileCheck --check-prefix=CACHE-HIT %s

//--- test.swift
func testFunc() {}

//--- foo.swift
func foo() {}

// CACHE-MISS: remark: cache miss for input
// CACHE-MISS-NOT: remark: replay output file
// CACHE-HIT: remark: replay output file
// CACHE-HIT-NOT: remark: cache miss for input
