// RUN: %empty-directory(%t)

// RUN: %target-swift-frontend -c -cache-compile-job -cas-path %t/cas -allow-unstable-cache-key-for-testing %s \
// RUN:   -import-objc-header %S/Inputs/objc.h -emit-module -emit-module-path %t/test.swiftmodule 2>&1 | %FileCheck %s
// RUN: %cache-tool -cas-path %t/cas -cache-tool-action print-output-keys -- %target-swift-frontend -c -cache-compile-job -cas-path %t/cas -allow-unstable-cache-key-for-testing %s \
// RUN:   -import-objc-header %S/Inputs/objc.h -emit-module -emit-module-path %t/test.swiftmodule > %t/cache_key.json
// RUN: %cache-tool -cas-path %t/cas -cache-tool-action render-diags %t/cache_key.json -- %target-swift-frontend -c -cache-compile-job -cas-path %t/cas -allow-unstable-cache-key-for-testing %s \
// RUN:   -import-objc-header %S/Inputs/objc.h -emit-module -emit-module-path %t/test.swiftmodule 2>&1 | %FileCheck %s

#warning("this is a warning")  // expected-warning {{this is a warning}}

// CHECK: warning: warning in bridging header
// CHECK: warning: this is a warning

/// Check other DiagnosticConsumers.
// RUN: %target-swift-frontend -c -cache-compile-job -cas-path %t/cas -allow-unstable-cache-key-for-testing %s \
// RUN:   -typecheck -serialize-diagnostics -serialize-diagnostics-path %t/test.diag -verify
// RUN: %FileCheck %s -check-prefix CHECK-SERIALIZED <%t/test.diag

// Verify the serialized diags have the right magic at the top.
// CHECK-SERIALIZED: DIA
