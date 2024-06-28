// RUN: %empty-directory(%t)
// RUN: split-file %s %t

// RUN: %target-swift-frontend -scan-dependencies -module-name Test -O \
// RUN:   -disable-implicit-string-processing-module-import -disable-implicit-concurrency-module-import -parse-stdlib \
// RUN:   %t/a.swift %t/b.swift %t/c.swift -o %t/deps.json -swift-version 5 -cache-compile-job -cas-path %t/cas

// RUN: %{python} %S/Inputs/BuildCommandExtractor.py %t/deps.json Test > %t/MyApp.cmd
// RUN: echo "\"-disable-implicit-string-processing-module-import\"" >> %t/MyApp.cmd
// RUN: echo "\"-disable-implicit-concurrency-module-import\"" >> %t/MyApp.cmd
// RUN: echo "\"-parse-stdlib\"" >> %t/MyApp.cmd

// RUN: %swift-scan-test -action compute_cache_key_from_index -cas-path %t/cas -input 0 -- \
// RUN:   %target-swift-frontend -cache-compile-job -primary-file %t/a.swift -primary-file %t/b.swift %t/c.swift \
// RUN:   -c -emit-dependencies -module-name Test -o %t/a.o -o %t/b.o -cas-path %t/cas \
// RUN:   @%t/MyApp.cmd > %t/key0.casid

// RUN: %swift-scan-test -action compute_cache_key_from_index -cas-path %t/cas -input 1 -- \
// RUN:   %target-swift-frontend -cache-compile-job -primary-file %t/a.swift -primary-file %t/b.swift %t/c.swift \
// RUN:   -c -emit-dependencies -module-name Test -o %t/a.o -o %t/b.o -cas-path %t/cas \
// RUN:   @%t/MyApp.cmd > %t/key1.casid

// RUN: %target-swift-frontend -cache-compile-job \
// RUN:  -primary-file %t/a.swift -primary-file %t/b.swift %t/c.swift \
// RUN:  -c -emit-dependencies \
// RUN:  -module-name Test -o %t/a.o -o %t/b.o -cas-path %t/cas @%t/MyApp.cmd

// RUN: %target-swift-frontend -cache-compile-job \
// RUN:  %t/a.swift %t/b.swift -primary-file %t/c.swift \
// RUN:  -c -emit-dependencies \
// RUN:  -module-name Test -o %t/c.o -cas-path %t/cas @%t/MyApp.cmd

// RUN: %swift-scan-test -action replay_result -cas-path %t/cas -id @%t/key0.casid -- %target-swift-frontend -cache-compile-job \
// RUN:   -primary-file %t/a.swift -primary-file %t/b.swift  %t/c.swift \
// RUN:   -c -emit-dependencies -module-name Test -o %t/a2.o -o %t/b2.o -cas-path %t/cas \
// RUN:   -serialize-diagnostics -serialize-diagnostics-path %t/a2.dia -serialize-diagnostics-path %t/b2.dia \
// RUN:   @%t/MyApp.cmd -frontend-parseable-output 2>&1 | %FileCheck %s --check-prefix=PARSEABLE

// RUN: c-index-test -read-diagnostics %t/a2.dia 2>&1 | %FileCheck %s --check-prefix=A-WARN
// RUN: c-index-test -read-diagnostics %t/b2.dia 2>&1 | %FileCheck %s --check-prefix=B-WARN

// A-WARN: warning: This is a warning
// B-WARN: warning: This is also a warning

// RUN: %swift-scan-test -action replay_result -cas-path %t/cas -id @%t/key1.casid -- %target-swift-frontend -cache-compile-job \
// RUN:   -primary-file %t/a.swift -primary-file %t/b.swift  %t/c.swift \
// RUN:   -c -emit-dependencies -module-name Test -o %t/a2.o -o %t/b2.o -cas-path %t/cas \
// RUN:   -serialize-diagnostics -serialize-diagnostics-path %t/a2.dia -serialize-diagnostics-path %t/b2.dia \
// RUN:   @%t/MyApp.cmd -frontend-parseable-output 2>&1 | %FileCheck %s --check-prefix=NO-OUTPUT --allow-empty

// PARSEABLE-COUNT-1:   "kind": "began",
// PARSEABLE-COUNT-1:   "kind": "finished",
// PRASEABLE-NOT: "kind": "began",
// PRASEABLE-NOT: "kind": "finished",

// NO-OUTPUT-NOT: "kind": "began",

//--- a.swift
#warning("This is a warning")
//--- b.swift
#warning("This is also a warning")
//--- c.swift
#warning("This is another warning")
