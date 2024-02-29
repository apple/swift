// rdar://119964830 Temporarily disabling in Linux
// UNSUPPORTED: OS=linux-gnu

// RUN: %empty-directory(%t)

// RUN: %target-swift-frontend -scan-dependencies -module-name Test -O -import-objc-header %S/Inputs/objc.h \
// RUN:   -disable-implicit-string-processing-module-import -disable-implicit-concurrency-module-import \
// RUN:   %s -o %t/deps.json -cache-compile-job -cas-path %t/cas -module-load-mode prefer-serialized

// RUN: %{python} %S/Inputs/BuildCommandExtractor.py %t/deps.json clang:SwiftShims > %t/shim.cmd
// RUN: %swift_frontend_plain @%t/shim.cmd

// RUN: %{python} %S/Inputs/BuildCommandExtractor.py %t/deps.json bridgingHeader | tail -n +2  > %t/header.cmd
// RUN: %target-swift-frontend @%t/header.cmd -disable-implicit-swift-modules %S/Inputs/objc.h -O -o %t/objc.pch 2>&1 | %FileCheck %s -check-prefix CHECK-BRIDGE
// RUN: %cache-tool -cas-path %t/cas -cache-tool-action print-output-keys -- \
// RUN:   %target-swift-frontend @%t/header.cmd -disable-implicit-swift-modules %S/Inputs/objc.h -O -o %t/objc.pch > %t/keys.json
// RUN: %{python} %S/Inputs/ExtractOutputKey.py %t/keys.json %S/Inputs/objc.h > %t/key

// RUN: %{python} %S/Inputs/GenerateExplicitModuleMap.py %t/deps.json > %t/map.json
// RUN: llvm-cas --cas %t/cas --make-blob --data %t/map.json > %t/map.casid

// RUN: %{python} %S/Inputs/BuildCommandExtractor.py %t/deps.json Test > %t/MyApp.cmd
// RUN: echo "\"-disable-implicit-string-processing-module-import\"" >> %t/MyApp.cmd
// RUN: echo "\"-disable-implicit-concurrency-module-import\"" >> %t/MyApp.cmd
// RUN: echo "\"-disable-implicit-swift-modules\"" >> %t/MyApp.cmd
// RUN: echo "\"-import-objc-header\"" >> %t/MyApp.cmd
// RUN: echo "\"%t/objc.pch\"" >> %t/MyApp.cmd
// RUN: echo "\"-bridging-header-pch-key\"" >> %t/MyApp.cmd
// RUN: echo "\"@%t/key\"" >> %t/MyApp.cmd
// RUN: echo "\"-explicit-swift-module-map-file\"" >> %t/MyApp.cmd
// RUN: echo "\"@%t/map.casid\"" >> %t/MyApp.cmd

// RUN: %target-swift-frontend  -cache-compile-job -module-name Test -O -cas-path %t/cas @%t/MyApp.cmd %s \
// RUN:   -emit-module -o %t/test.swiftmodule 2>&1 | %FileCheck %s
// RUN: %cache-tool -cas-path %t/cas -cache-tool-action print-output-keys -- %target-swift-frontend -cache-compile-job -module-name Test -O -cas-path %t/cas @%t/MyApp.cmd %s \
// RUN:   -emit-module -o %t/test.swiftmodule > %t/cache_key.json
// RUN: %cache-tool -cas-path %t/cas -cache-tool-action render-diags %t/cache_key.json -- %target-swift-frontend -cache-compile-job -module-name Test -O -cas-path %t/cas @%t/MyApp.cmd %s \
// RUN:   -emit-module -o %t/test.swiftmodule 2>&1 | %FileCheck %s

#warning("this is a warning")  // expected-warning {{this is a warning}}

enum MyEnum {
    case kind
    case none
}

let _ : MyEnum? = .none // expected-warning {{assuming you mean 'Optional<MyEnum>.none'; did you mean 'MyEnum.none' instead?}}
// expected-note@-1 {{explicitly specify 'Optional' to silence this warning}} {{19-19=Optional}}
// expected-note@-2 {{use 'MyEnum.none' instead}} {{19-19=MyEnum}}

// CHECK-BRIDGE: warning: warning in bridging header
// CHECK: warning: this is a warning

/// Check other DiagnosticConsumers.
// RUN: %target-swift-frontend -c -cache-compile-job -module-name Test -O -cas-path %t/cas @%t/MyApp.cmd %s \
// RUN:   -typecheck -serialize-diagnostics -serialize-diagnostics-path %t/test.diag -verify
// RUN: %FileCheck %s -check-prefix CHECK-SERIALIZED <%t/test.diag

/// Verify the serialized diags have the right magic at the top.
// CHECK-SERIALIZED: DIA
