// RUN: %empty-directory(%t)
// RUN: split-file %s %t
// RUN: %target-swift-frontend -scan-dependencies -module-cache-path %t/clang-module-cache %t/main.swift -module-name Test -o %t/deps.json -I %S/Inputs/CHeaders -I %S/Inputs/Swift -I %t/include -swift-version 4

// RUN: %{python} %S/../CAS/Inputs/SwiftDepsExtractor.py %t/deps.json Test directDependencies | %FileCheck %s

// CHECK-DAG: "swift": "A"
// CHECK-DAG: "clang": "ClangTest"
// CHECK-NOT: "swift": "G"
// CHECK-NOT: "swift": "B"
// CHECK-NOT: "Missing"

// RUN: %{python} %S/../CAS/Inputs/BuildCommandExtractor.py %t/deps.json Test | %FileCheck %s -check-prefix=CMD
// CMD: "-module-can-import"
// CMD-NEXT: "C"
// CMD-NEXT: "-module-can-import"
// CMD-NEXT: "ClangTest.Sub"
// CMD-NEXT: "-module-can-import"
// CMD-NEXT: "F"
// CMD-NEXT: "-module-can-import-version"
// CMD-NEXT: "Version"
// CMD-NEXT: "100.1"
// CMD-NEXT: "0"

//--- main.swift

#if canImport(Missing)
import G
#endif

#if canImport(C)
import A
#else
import B
#endif

#if !canImport(F)
import Missing
#endif

// B is not dependency
#if false && canImport(B)
import Missing
#endif

// B is short circuited
#if canImport(C) || canImport(B)
import B
#endif

// Check clang submodule
#if canImport(ClangTest.Sub)
import ClangTest.Sub
#endif

// Versioned check
#if canImport(Version, _version: 10.0)
#endif

//--- include/module.modulemap
module ClangTest {
  module Sub {
    header "sub.h"
    export *
  }
}

//--- include/sub.h
void notused(void);

//--- include/Version.swiftinterface
// swift-interface-format-version: 1.0
// swift-module-flags: -module-name Version -O -disable-implicit-string-processing-module-import -disable-implicit-concurrency-module-import -parse-stdlib -user-module-version 100.1
