// RUN: %empty-directory(%t)
// RUN: not %target-swift-frontend -module-cache-path %t -enable-objc-interop -typecheck %s -F %S/Inputs/frameworks -serialize-diagnostics-path %t.dia -Xcc -D -Xcc FOO -diagnostic-style llvm 2>&1 | %FileCheck %s
// RUN: test -s %t.dia
// RUN: c-index-test -read-diagnostics %t.dia 2>&1 | %FileCheck %s

// RUN: %empty-directory(%t)
// RUN: %target-swift-frontend -module-cache-path %t -enable-objc-interop -typecheck %s -F %S/Inputs/frameworks -serialize-diagnostics-path %t.warn.dia -diagnostic-style llvm 2>&1 | %FileCheck %s -check-prefix CHECK-WARN
// RUN: test -s %t.warn.dia
// RUN: c-index-test -read-diagnostics %t.warn.dia 2>&1 | %FileCheck %s -check-prefix CHECK-WARN


// Also check batch mode (multiple primary files).
// RUN: %empty-directory(%t)
// RUN: not %target-swift-frontend -module-cache-path %t -enable-objc-interop -typecheck -primary-file %s -primary-file %S/../Inputs/empty.swift -F %S/Inputs/frameworks -serialize-diagnostics-path %t.1.dia -serialize-diagnostics-path %t.2.dia -Xcc -D -Xcc FOO -diagnostic-style llvm 2>&1 | %FileCheck %s
// RUN: test -s %t.1.dia
// RUN: c-index-test -read-diagnostics %t.1.dia 2>&1 | %FileCheck %s
// RUN: c-index-test -read-diagnostics %t.1.dia 2>&1 | %FileCheck %s -check-prefix CHECK-PRIMARY
// RUN: test -s %t.2.dia
// RUN: c-index-test -read-diagnostics %t.2.dia 2>&1 | %FileCheck %s

// RUN: %empty-directory(%t)
// RUN: %target-swift-frontend -module-cache-path %t -enable-objc-interop -typecheck -primary-file %s -primary-file %S/../Inputs/empty.swift -F %S/Inputs/frameworks -serialize-diagnostics-path %t.warn.1.dia -serialize-diagnostics-path %t.warn.2.dia -diagnostic-style llvm 2>&1 | %FileCheck %s -check-prefix CHECK-WARN
// RUN: test -s %t.1.dia
// RUN: c-index-test -read-diagnostics %t.warn.1.dia 2>&1 | %FileCheck %s -check-prefix=CHECK-WARN
// RUN: test -s %t.2.dia
// RUN: c-index-test -read-diagnostics %t.warn.2.dia 2>&1 | %FileCheck %s -check-prefix=CHECK-WARN


// Verify that -Wno-* options are applied.
// RUN: %empty-directory(%t)
// RUN: %target-swift-frontend -module-cache-path %t -enable-objc-interop -typecheck %s -F %S/Inputs/frameworks -Xcc -Wno-#warnings -diagnostic-style llvm 2>&1 | %FileCheck -check-prefix CHECK-NO-WARN -allow-empty %s

import Module

// CHECK: Another.h:2:4: error: Module should have been built without -DFOO
// CHECK: Sub2.h:2:9: error: could not build module 'Another'
// CHECK-PRIMARY: diags_from_module.swift:[[@LINE-4]]:8: error: could not build Objective-C module 'Module'

// CHECK-WARN: Sub2.h:7:2: warning: here is some warning about something
// CHECK-WARN: Module.h:22:1: warning: umbrella header for module 'Module' does not include header 'NotInModule.h'
// FIXME: show [-Wincomplete-umbrella]

// CHECK-NO-WARN-NOT: warning about something
