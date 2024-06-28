// RUN: %empty-directory(%t)

// RUN: %target-swift-frontend -scan-dependencies -module-name Test -O \
// RUN:   -disable-implicit-string-processing-module-import -disable-implicit-concurrency-module-import -parse-stdlib \
// RUN:   %s -o %t/deps.json -swift-version 5 -cache-compile-job -cas-path %t/cas

// RUN: %{python} %S/Inputs/BuildCommandExtractor.py %t/deps.json Test > %t/MyApp.cmd
// RUN: echo "\"-disable-implicit-string-processing-module-import\"" >> %t/MyApp.cmd
// RUN: echo "\"-disable-implicit-concurrency-module-import\"" >> %t/MyApp.cmd
// RUN: echo "\"-parse-stdlib\"" >> %t/MyApp.cmd

// RUN: %swift-scan-test -action compute_cache_key -cas-path %t/cas -input %s -- %target-swift-frontend -cache-compile-job -Rcache-compile-job %s \
// RUN:   -emit-module -emit-module-path %t/Test.swiftmodule -c -emit-dependencies -module-name Test -o %t/test.o -cas-path %t/cas \
// RUN:   @%t/MyApp.cmd > %t/key.casid

// RUN: %swift-scan-test -action compute_cache_key_from_index -cas-path %t/cas -input 0 -- %target-swift-frontend -cache-compile-job -Rcache-compile-job %s \
// RUN:   -emit-module -emit-module-path %t/Test.swiftmodule -c -emit-dependencies -module-name Test -o %t/test.o -cas-path %t/cas \
// RUN:   @%t/MyApp.cmd > %t/key1.casid

// RUN: diff %t/key.casid %t/key1.casid

// RUN: not %swift-scan-test -action cache_query -id @%t/key.casid -cas-path %t/cas 2>&1 | %FileCheck %s --check-prefix=CHECK-QUERY-NOT-FOUND

// RUN: %target-swift-frontend -cache-compile-job -Rcache-compile-job %s -emit-module -emit-module-path %t/Test.swiftmodule -c -emit-dependencies \
// RUN:  -module-name Test -o %t/test.o -cas-path %t/cas @%t/MyApp.cmd

// RUN: %swift-scan-test -action cache_query -id @%t/key.casid -cas-path %t/cas | %FileCheck %s --check-prefix=CHECK-QUERY

// RUN: %swift-scan-test -action replay_result -cas-path %t/cas -id @%t/key.casid -- %target-swift-frontend -cache-compile-job -Rcache-compile-job %s \
// RUN:   -emit-module -emit-module-path %t/Test2.swiftmodule -c -emit-dependencies -module-name Test -o %t/test2.o -cas-path %t/cas \
// RUN:   @%t/MyApp.cmd

// RUN: diff %t/Test.swiftmodule %t/Test2.swiftmodule
// RUN: diff %t/test.o %t/test2.o

// CHECK-QUERY-NOT-FOUND: cached output not found
// CHECK-QUERY: Cached Compilation for key "llvmcas://{{.*}}" has 4 outputs:
// CHECK-QUERY-NEXT: object: llvmcas://
// CHECK-QUERY-NEXT: dependencies: llvmcas://
// CHECK-QUERY-NEXT: swiftmodule: llvmcas://
// CHECK-QUERY-NEXT: cached-diagnostics: llvmcas://

func testFunc() {}
