// RUN: %empty-directory(%t)
// RUN: split-file %s %t

// RUN: %target-swift-frontend -emit-module %t/A.swift -module-name A \
// RUN:   -disable-implicit-string-processing-module-import -disable-implicit-concurrency-module-import -parse-stdlib \
// RUN:   -enable-library-evolution -swift-version 5 \
// RUN:   -emit-module-interface-path %t/A.swiftinterface \
// RUN:   -o %t/A.swiftmodule

// RUN: %target-swift-frontend -scan-dependencies -module-load-mode prefer-interface -o %t/deps.json -I %t \
// RUN:   -disable-implicit-string-processing-module-import -disable-implicit-concurrency-module-import -parse-stdlib \
// RUN:   %t/test.swift -module-name Test -swift-version 5
// RUN: %{python} %S/../CAS/Inputs/BuildCommandExtractor.py %t/deps.json A | %FileCheck %s --check-prefix CHECK-NO-DIRECT-CC1
// RUN: %{python} %S/../CAS/Inputs/BuildCommandExtractor.py %t/deps.json Test | %FileCheck %s --allow-empty --check-prefix CHECK-NO-DIRECT-CC1

// RUN: %target-swift-frontend -scan-dependencies -module-load-mode prefer-interface -o %t/deps2.json -I %t \
// RUN:   -disable-implicit-string-processing-module-import -disable-implicit-concurrency-module-import -parse-stdlib \
// RUN:   -g -file-compilation-dir %t -Xcc -ferror-limit=1 \
// RUN:   %t/test.swift -module-name Test -swift-version 5 -experimental-clang-importer-direct-cc1-scan
// RUN: %{python} %S/../CAS/Inputs/BuildCommandExtractor.py %t/deps2.json A | %FileCheck %s --check-prefix CHECK-DIRECT-CC1-SCAN
// RUN: %{python} %S/../CAS/Inputs/BuildCommandExtractor.py %t/deps2.json Test | %FileCheck %s --check-prefix CHECK-DIRECT-CC1-SCAN

// CHECK-NO-DIRECT-CC1-NOT: -direct-clang-cc1-module-build
// CHECK-DIRECT-CC1-SCAN: -direct-clang-cc1-module-build
// CHECK-DIRECT-CC1-SCAN-NOT: -ffile-compilation-dir
// CHECK-DIRECT-CC1-SCAN-NOT: -ferror-limit=1

//--- A.swift
func a() {}

//--- test.swift
import A
func test() {}
