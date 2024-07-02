// REQUIRES: OS=macosx
// RUN: %empty-directory(%t)
// RUN: split-file %s %t

// RUN: %target-swift-frontend -scan-dependencies -module-name Test -O -module-cache-path %t/clang-module-cache \
// RUN:   -disable-implicit-string-processing-module-import -disable-implicit-concurrency-module-import -parse-stdlib \
// RUN:   %t/test.swift -I %t -o %t/deps.json -cache-compile-job -cas-backend -cas-backend-mode=verify -cas-path %t/cas

/// Check clang module
// RUN: %{python} %S/Inputs/BuildCommandExtractor.py %t/deps.json clang:Dummy > %t/dummy.cmd
// RUN: %swift_frontend_plain @%t/dummy.cmd -Rcache-compile-job 2>&1 | %FileCheck --check-prefix=CACHE-MISS %s
// RUN: %swift_frontend_plain @%t/dummy.cmd -Rcache-compile-job 2>&1 | %FileCheck --check-prefix=CACHE-HIT-CLANG %s

// RUN: %{python} %S/Inputs/GenerateExplicitModuleMap.py %t/deps.json > %t/map.json
// RUN: llvm-cas --cas %t/cas --make-blob --data %t/map.json > %t/map.casid

// RUN: %{python} %S/Inputs/BuildCommandExtractor.py %t/deps.json Test > %t/MyApp.cmd
// RUN: echo "\"-disable-implicit-string-processing-module-import\"" >> %t/MyApp.cmd
// RUN: echo "\"-disable-implicit-concurrency-module-import\"" >> %t/MyApp.cmd
// RUN: echo "\"-disable-implicit-swift-modules\"" >> %t/MyApp.cmd
// RUN: echo "\"-parse-stdlib\"" >> %t/MyApp.cmd
// RUN: echo "\"-explicit-swift-module-map-file\"" >> %t/MyApp.cmd
// RUN: echo "\"@%t/map.casid\"" >> %t/MyApp.cmd

/// Run the command first time, expect cache miss.
// RUN: %target-swift-frontend -cache-compile-job -cas-backend -cas-backend-mode=verify -Rcache-compile-job %t/test.swift -O -emit-module -emit-module-path %t/Test.swiftmodule -c -emit-dependencies \
// RUN:  -module-name Test -o %t/test.o -cas-path %t/cas @%t/MyApp.cmd 2>&1 | %FileCheck --check-prefix=CACHE-MISS %s

/// Expect cache hit for second time.
// RUN: %target-swift-frontend -cache-compile-job -cas-backend -cas-backend-mode=verify -Rcache-compile-job %t/test.swift -O -emit-module -emit-module-path %t/Test.swiftmodule -c -emit-dependencies \
// RUN:  -module-name Test -o %t/test.o -cas-path %t/cas @%t/MyApp.cmd 2>&1 | %FileCheck --check-prefix=CACHE-HIT %s

/// Expect cache miss a subset of outputs.
// RUN %target-swift-frontend -cache-compile-job -cas-backend -cas-backend-mode=verify -Rcache-compile-job %t/test.swift -O -emit-module -emit-module-path %t/Test.swiftmodule -c \
// RUN  -module-name Test -o %t/test.o -cas-path %t/cas @%t/MyApp.cmd 2>&1 | %FileCheck --check-prefix=CACHE-MISS %s

/// Cache hit for retry.
// RUN %target-swift-frontend -cache-compile-job -cas-backend -cas-backend-mode=verify -Rcache-compile-job %t/test.swift -O -emit-module -emit-module-path %t/Test.swiftmodule -c \
// RUN  -module-name Test -o %t/test.o -cas-path %t/cas @%t/MyApp.cmd 2>&1 | %FileCheck --check-prefix=CACHE-HIT %s

/// Skip cache
// RUN %target-swift-frontend -cache-compile-job -cas-backend -cas-backend-mode=verify -Rcache-compile-job -cache-disable-replay %t/test.swift -O -emit-module -emit-module-path %t/Test.swiftmodule -c \
// RUN  -module-name Test -o %t/test.o -cas-path %t/cas @%t/MyApp.cmd 2>&1 | %FileCheck --allow-empty --check-prefix=SKIP-CACHE %s

// CACHE-MISS: remark: cache miss for input
// CACHE-HIT: remark: replay output file '<cached-diagnostics>': key 'llvmcas://{{.*}}'
// CACHE-HIT: remark: replay output file '{{.*}}{{/|\\}}test.o': key 'llvmcas://{{.*}}'
// CACHE-HIT: remark: replay output file '{{.*}}{{/|\\}}Test.swiftmodule': key 'llvmcas://{{.*}}'
// CACHE-HIT-CLANG: remark: replay output file '<cached-diagnostics>': key 'llvmcas://{{.*}}'
// CACHE-HIT-CLANG: remark: replay output file '{{.*}}{{/|\\}}Dummy-{{.*}}.pcm': key 'llvmcas://{{.*}}'
// SKIP-CACHE-NOT: remark:

//--- test.swift
import Dummy
func testFunc() {}

//--- module.modulemap
module Dummy {
 umbrella header "Dummy.h"
}

//--- Dummy.h
void dummy(void);
