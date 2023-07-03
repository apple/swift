// RUN: %empty-directory(%t)
// RUN: mkdir -p %t/cas

// RUN: not %target-swift-frontend -c -cache-compile-job -cas-path %t/cas %s -o %t/test.o 2>&1 | %FileCheck %s --check-prefix=NO-CASFS
// NO-CASFS: caching is enabled without -cas-fs option

// RUN: %target-swift-frontend -c -cache-compile-job -cas-path %t/cas %s -o %t/test.o -allow-unstable-cache-key-for-testing
// RUN: %cache-tool -cas-path %t/cas -cache-tool-action print-output-keys -- \
// RUN:   %target-swift-frontend -c -cache-compile-job -cas-path %t/cas %s -o %t/test.o -allow-unstable-cache-key-for-testing > %t/cache_key.json
// RUN: %cache-tool -cas-path %t/cas -cache-tool-action validate-outputs %t/cache_key.json

/// make sure validate fails if the cas is cleared.
// RUN: rm -rf %t/cas
// RUN: not %cache-tool -cas-path %t/cas -cache-tool-action validate-outputs %t/cache_key.json 2>&1 | %FileCheck %s

// CHECK: failed to find output for cache key

func testFunc() {}
