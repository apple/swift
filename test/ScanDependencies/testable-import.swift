// RUN: %empty-directory(%t)
// RUN: split-file %s %t

// RUN: %target-swift-frontend -emit-module -module-name C -o %t/C.swiftmodule -swift-version 5 \
// RUN:   -disable-implicit-string-processing-module-import -disable-implicit-concurrency-module-import -parse-stdlib \
// RUN:   -emit-module-interface-path %t/C.swiftinterface -enable-library-evolution -I %t -enable-testing \
// RUN:   %t/C.swift

// RUN: %target-swift-frontend -emit-module -module-name B -o %t/B.swiftmodule -swift-version 5 \
// RUN:   -disable-implicit-string-processing-module-import -disable-implicit-concurrency-module-import -parse-stdlib \
// RUN:   -emit-module-interface-path %t/B.swiftinterface -enable-library-evolution -I %t -enable-testing \
// RUN:   %t/B.swift

// RUN: %target-swift-frontend -emit-module -module-name A -o %t/A.swiftmodule -swift-version 5 \
// RUN:   -disable-implicit-string-processing-module-import -disable-implicit-concurrency-module-import -parse-stdlib \
// RUN:   -emit-module-interface-path %t/A.swiftinterface -enable-library-evolution -I %t -enable-testing \
// RUN:   %t/A.swift

// RUN: %target-swift-frontend -scan-dependencies -module-load-mode prefer-interface -module-name Test %t/test1.swift \
// RUN:   -disable-implicit-string-processing-module-import -disable-implicit-concurrency-module-import -parse-stdlib -enable-testing \
// RUN:   -o %t/deps1.json -I %t -swift-version 5
// RUN: %{python} %S/../CAS/Inputs/SwiftDepsExtractor.py %t/deps1.json Test directDependencies | %FileCheck %s --check-prefix TEST1
// TEST1-DAG: "swiftPrebuiltExternal": "B"
// TEST1-DAG: "swift": "C"

// RUN: %target-swift-frontend -scan-dependencies -module-load-mode prefer-interface -module-name Test %t/test2.swift \
// RUN:   -disable-implicit-string-processing-module-import -disable-implicit-concurrency-module-import -parse-stdlib -enable-testing \
// RUN:   -o %t/deps2.json -I %t -swift-version 5
// RUN: %{python} %S/../CAS/Inputs/SwiftDepsExtractor.py %t/deps2.json Test directDependencies | %FileCheck %s --check-prefix TEST2
// TEST2-DAG: "swift": "A"
// TEST2-DAG: "swiftPrebuiltExternal": "B"

/// Verify that the interface module inside A is overwritten to be binary module due to the deps in main module.
// RUN: %{python} %S/../CAS/Inputs/SwiftDepsExtractor.py %t/deps2.json A directDependencies | %FileCheck %s --check-prefix TEST2-A
// TEST2-A-DAG: "swiftPrebuiltExternal": "B"
// TEST2-A-DAG: "swift": "C"

/// An indirect @testable import is still interface deps.
// RUN: %target-swift-frontend -scan-dependencies -module-load-mode prefer-interface -module-name Test %t/test3.swift \
// RUN:   -disable-implicit-string-processing-module-import -disable-implicit-concurrency-module-import -parse-stdlib -enable-testing \
// RUN:   -o %t/deps3.json -I %t -swift-version 5
// RUN: %{python} %S/../CAS/Inputs/SwiftDepsExtractor.py %t/deps3.json Test directDependencies | %FileCheck %s --check-prefix TEST3
// TEST3-DAG: "swiftPrebuiltExternal": "A"
// TEST3-DAG: "swift": "C"

//--- test1.swift
@testable import B
import C

//--- test2.swift
import A
@testable import B

//--- test3.swift
@testable import A
import C

//--- A.swift
import B
@testable import C

//--- B.swift
@_spi(Testing) public func b() {}

//--- C.swift
@_spi(Testing) public func c() {}
