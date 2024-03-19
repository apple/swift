// RUN: %empty-directory(%t)
// RUN: split-file %s %t

// RUN: %target-swift-frontend -emit-module -module-name B -o %t/B.swiftmodule -swift-version 5 \
// RUN:   -disable-implicit-string-processing-module-import -disable-implicit-concurrency-module-import -parse-stdlib \
// RUN:   -emit-module-interface-path %t/B.swiftinterface -enable-library-evolution \
// RUN:   %t/B.swift

// RUN: %target-swift-frontend -emit-module -module-name A -o %t/A.swiftmodule -swift-version 5 \
// RUN:   -disable-implicit-string-processing-module-import -disable-implicit-concurrency-module-import -parse-stdlib \
// RUN:   -emit-module-interface-path %t/A.swiftinterface -enable-library-evolution -I %t \
// RUN:   %t/A.swift

// RUN: %target-swift-frontend -scan-dependencies -module-name Test -module-cache-path %t/clang-module-cache %t/main.swift \
// RUN:   -disable-implicit-string-processing-module-import -disable-implicit-concurrency-module-import -parse-stdlib \
// RUN:   -o %t/deps.json -I %t -cache-compile-job -cas-path %t/cas -swift-version 5

// RUN: %{python} %S/Inputs/BuildCommandExtractor.py %t/deps.json B > %t/B.cmd
// RUN: %swift_frontend_plain @%t/B.cmd
// RUN: %{python} %S/Inputs/BuildCommandExtractor.py %t/deps.json A > %t/A.cmd
// RUN: %swift_frontend_plain @%t/A.cmd

// RUN: %FileCheck %s --input-file=%t/B.cmd
// RUN: %FileCheck %s --input-file=%t/A.cmd

// CHECK-NOT: -candidate-module-file

// RUN: %{python} %S/Inputs/GenerateExplicitModuleMap.py %t/deps.json > %t/map.json
// RUN: llvm-cas --cas %t/cas --make-blob --data %t/map.json > %t/map.casid

// RUN: %{python} %S/Inputs/BuildCommandExtractor.py %t/deps.json Test > %t/MyApp.cmd

// RUN: %target-swift-frontend \
// RUN:   -typecheck -cache-compile-job -cas-path %t/cas \
// RUN:   -disable-implicit-swift-modules \
// RUN:   -disable-implicit-string-processing-module-import -disable-implicit-concurrency-module-import -parse-stdlib \
// RUN:   -module-name Test -explicit-swift-module-map-file @%t/map.casid \
// RUN:   %t/main.swift @%t/MyApp.cmd -emit-loaded-module-trace -emit-loaded-module-trace-path %t/test.trace.json

// RUN: %FileCheck %s --check-prefix=TRACE --input-file=%t/test.trace.json
// TRACE-DAG: A.swiftinterface
// TRACE-DAG: B.swiftinterface

//--- main.swift
import A

//--- A.swift
import B
func test() {}

//--- B.swift
func b() {}
