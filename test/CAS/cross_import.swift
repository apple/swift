// RUN: %empty-directory(%t)
// RUN: split-file %s %t

// RUN: %target-swift-frontend -emit-module -module-name A -o %t/A.swiftmodule -swift-version 5 \
// RUN:   -disable-implicit-string-processing-module-import -disable-implicit-concurrency-module-import -parse-stdlib \
// RUN:   -emit-module-interface-path %t/A.swiftinterface -enable-library-evolution -I %t %t/A.swift

// RUN: %target-swift-frontend -emit-module -module-name B -o %t/A.swiftmodule -swift-version 5 \
// RUN:   -disable-implicit-string-processing-module-import -disable-implicit-concurrency-module-import -parse-stdlib \
// RUN:   -emit-module-interface-path %t/B.swiftinterface -enable-library-evolution -I %t %t/B.swift

// RUN: %target-swift-frontend -emit-module -module-name _Cross -o %t/A.swiftmodule -swift-version 5 \
// RUN:   -disable-implicit-string-processing-module-import -disable-implicit-concurrency-module-import -parse-stdlib \
// RUN:   -emit-module-interface-path %t/_Cross.swiftinterface -enable-library-evolution -I %t %t/cross.swift

// RUN: %target-swift-frontend -scan-dependencies -module-name Test -module-cache-path %t/clang-module-cache %t/main.swift \
// RUN:   -disable-implicit-string-processing-module-import -disable-implicit-concurrency-module-import -parse-stdlib \
// RUN:   -o %t/deps.json -I %t -cache-compile-job -cas-path %t/cas -swift-version 5 -enable-cross-import-overlays -module-load-mode prefer-interface

// RUN: %{python} %S/Inputs/BuildCommandExtractor.py %t/deps.json A > %t/A.cmd
// RUN: %swift_frontend_plain @%t/A.cmd
// RUN: %{python} %S/Inputs/BuildCommandExtractor.py %t/deps.json B > %t/B.cmd
// RUN: %swift_frontend_plain @%t/B.cmd
// RUN: %{python} %S/Inputs/BuildCommandExtractor.py %t/deps.json _Cross > %t/Cross.cmd
// RUN: %swift_frontend_plain @%t/Cross.cmd

// RUN: %{python} %S/Inputs/GenerateExplicitModuleMap.py %t/deps.json > %t/map.json
// RUN: llvm-cas --cas %t/cas --make-blob --data %t/map.json > %t/map.casid

// RUN: %{python} %S/Inputs/BuildCommandExtractor.py %t/deps.json Test > %t/MyApp.cmd

// RUN: %target-swift-frontend -emit-module -o %t/Test.swiftmodule  \
// RUN:   -emit-module-interface-path %t/Test.swiftinterface \
// RUN:   -cache-compile-job -cas-path %t/cas \
// RUN:   -disable-implicit-swift-modules -swift-version 5 -enable-cross-import-overlays \
// RUN:   -disable-implicit-string-processing-module-import -disable-implicit-concurrency-module-import -parse-stdlib \
// RUN:   -module-name Test -explicit-swift-module-map-file @%t/map.casid \
// RUN:   %t/main.swift @%t/MyApp.cmd

// RUN: %FileCheck %s --input-file=%t/Test.swiftinterface

/// Check to make sure the implicit cross import turned into explicit import in the interface file.
// CHECK: import _Cross

//--- A.swift
public func a() {}

//--- B.swift
public func b() {}

//--- cross.swift
public func cross() {}

//--- main.swift
import A
import B

func test () {
  cross()
}

//--- B.swiftcrossimport/A.swiftoverlay
%YAML 1.2
---
version: 1
modules:
  - name: _Cross
