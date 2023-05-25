// RUN: %empty-directory(%t)
// RUN: %target-swift-frontend -emit-module -emit-module-path %t/test.module \
// RUN:   -enable-cas -cas-path %t/cas -allow-unstable-cache-key-for-testing %s 2>&1 | %FileCheck %s
// RUN: %cache-tool -cas-path %t/cas -cache-tool-action print-output-keys -- \
// RUN:   %target-swift-frontend -emit-module -emit-module-path %t/test.module -enable-cas -cas-path %t/cas \
// RUN:   -allow-unstable-cache-key-for-testing %s > %t/cache_key.json
// RUN: %cache-tool -cas-path %t/cas -cache-tool-action render-diags %t/cache_key.json --  \
// RUN:   %target-swift-frontend -emit-module -emit-module-path %t/test.module -enable-cas -cas-path %t/cas \
// RUN:   -allow-unstable-cache-key-for-testing %s 2>&1 | %FileCheck %s

#sourceLocation(file: "anything.swift", line: 1)
#warning("this is a warning")
#sourceLocation()

// CHECK: anything.swift:1:10: warning: this is a warning
