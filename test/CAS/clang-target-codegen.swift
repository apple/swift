// REQUIRES: OS=macosx

// RUN: %empty-directory(%t)
// RUN: split-file %s %t

// RUN: %target-swift-frontend -scan-dependencies -target arm64-apple-macos11 -module-name Test -module-cache-path %t/clang-module-cache -O \
// RUN:   -disable-implicit-string-processing-module-import -disable-implicit-concurrency-module-import -parse-stdlib \
// RUN:   %t/test.swift -o %t/deps.json -swift-version 5 -cache-compile-job -cas-path %t/cas \
// RUN:   -I %t/include -clang-target arm64-apple-macos13

// RUN: %{python} %S/Inputs/BuildCommandExtractor.py %t/deps.json clang:A > %t/a.cmd
// RUN: %swift_frontend_plain @%t/a.cmd
// RUN: %{python} %S/Inputs/BuildCommandExtractor.py %t/deps.json Foo > %t/foo.cmd
// RUN: %swift_frontend_plain @%t/foo.cmd

// RUN: %{python} %S/Inputs/GenerateExplicitModuleMap.py %t/deps.json > %t/map.json
// RUN: llvm-cas --cas %t/cas --make-blob --data %t/map.json > %t/map.casid

// RUN: %{python} %S/Inputs/BuildCommandExtractor.py %t/deps.json Test > %t/MyApp.cmd

// RUN: %target-swift-frontend -c -o %t/test.o -target arm64-apple-macos11 \
// RUN:   -clang-target arm64-apple-macos13 -cache-compile-job -cas-path %t/cas \
// RUN:   -disable-implicit-swift-modules -swift-version 5 -parse-stdlib -O \
// RUN:   -disable-implicit-string-processing-module-import -disable-implicit-concurrency-module-import \
// RUN:   -module-name Test -explicit-swift-module-map-file @%t/map.casid \
// RUN:   %t/test.swift @%t/MyApp.cmd

// RUN: llvm-objdump --macho --all-headers %t/test.o | %FileCheck %s

// CHECK: cmd LC_BUILD_VERSION
// CHECK-NEXT:   cmdsize
// CHECK-NEXT:  platform macos
// CHECK-NEXT:       sdk
// CHECK-NEXT:     minos 11.0

//--- include/module.modulemap
module A {
  header "a.h"
  export *
}
//--- include/a.h
void a(void);

//--- include/Foo.swiftinterface
// swift-interface-format-version: 1.0
// swift-module-flags: -target arm64-apple-macos13 -enable-library-evolution -swift-version 5 -O -module-name Foo -disable-implicit-string-processing-module-import -disable-implicit-concurrency-module-import -parse-stdlib
import A
public func foo()

//--- test.swift
import Foo
