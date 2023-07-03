// RUN: %empty-directory(%t)

/// Run the command first time, expect cache miss.
/// FIXME: This command doesn't use `-cas-fs` so it is not a good cache entry. It is currently allowed so it is easier to write tests.
// RUN: %target-swift-frontend -cache-compile-job -cache-remarks %s -emit-module -emit-module-path %t/Test.swiftmodule -c -emit-dependencies \
// RUN:  -module-name Test -o %t/test.o -cas-path %t/cas -allow-unstable-cache-key-for-testing 2>&1 | %FileCheck --check-prefix=CACHE-MISS %s

/// Expect cache hit for second time.
// RUN: %target-swift-frontend -cache-compile-job -cache-remarks %s -emit-module -emit-module-path %t/Test.swiftmodule -c -emit-dependencies \
// RUN:  -module-name Test -o %t/test.o -cas-path %t/cas -allow-unstable-cache-key-for-testing 2>&1 | %FileCheck --check-prefix=CACHE-HIT %s

/// Expect cache hit a subset of outputs.
// RUN: %target-swift-frontend -cache-compile-job -cache-remarks %s -emit-module -emit-module-path %t/Test.swiftmodule -c \
// RUN:  -module-name Test -o %t/test.o -cas-path %t/cas -allow-unstable-cache-key-for-testing 2>&1 | %FileCheck --check-prefix=CACHE-HIT %s

/// Skip cache
// RUN: %target-swift-frontend -cache-compile-job -cache-remarks -cache-disable-replay %s -emit-module -emit-module-path %t/Test.swiftmodule -c \
// RUN:  -module-name Test -o %t/test.o -cas-path %t/cas -allow-unstable-cache-key-for-testing 2>&1 | %FileCheck --allow-empty --check-prefix=SKIP-CACHE %s

// CACHE-MISS: remark: cache miss output file
// CACHE-HIT: remark: replay output file
// SKIP-CACHE-NOT: remark:

func testFunc() {}
