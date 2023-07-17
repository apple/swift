// RUN: %empty-directory(%t)
// RUN: %empty-directory(%t/clang-module-cache)
// RUN: %empty-directory(%t/Foo.swiftmodule)
// RUN: %empty-directory(%t/binaryModuleOnly)
// RUN: echo "public func foo() {}" > %t/Foo.swift
// REQUIRES: executable_test
// REQUIRES: objc_interop

import Foo

// BINARY_MODULE_ONLY: "swiftPrebuiltExternal": "Foo"
// BINARY_MODULE_ONLY:  "swiftPrebuiltExternal": {
// BINARY_MODULE_ONLY-NEXT:  "compiledModulePath": "BUILD_DIR/{{.*}}/ScanDependencies/Output/binary_module_only.swift.tmp/binaryModuleOnly/Foo.swiftmodule",

// HAS_NO_COMPILED-NOT: "{{.*}}Foo.swiftmodule{{.*}}.swiftmodule"

// Step 1: build swift interface and swift module side by side
// RUN: %target-swift-frontend -emit-module %t/Foo.swift -emit-module-path %t/Foo.swiftmodule/%target-swiftmodule-name -module-name Foo -emit-module-interface-path %t/Foo.swiftmodule/%target-swiftinterface-name

// Step 2: build module from interface and put it in a location separate from the interface file
// RUN: %target-swift-frontend -compile-module-from-interface -o %t/binaryModuleOnly/Foo.swiftmodule -module-name Foo -disable-interface-lock %t/Foo.swiftmodule/%target-swiftinterface-name

// Step 3: scan dependencies, pointed only at the binary module file should detect it as a swiftBinaryModule kind of dependency
// RUN: %target-swift-frontend -scan-dependencies %s -o %t/deps.json -I %t/binaryModuleOnly -emit-dependencies -emit-dependencies-path %t/deps.d -sdk %t -prebuilt-module-cache-path %t/ResourceDir/%target-sdk-name/prebuilt-modules
// RUN: %validate-json %t/deps.json | %FileCheck %s -check-prefix=BINARY_MODULE_ONLY

// Step 4: Ensure that round-trip serialization does not affect result
// RUN: %target-swift-frontend -scan-dependencies -test-dependency-scan-cache-serialization %s -o %t/deps.json -I %t/binaryModuleOnly -emit-dependencies -emit-dependencies-path %t/deps.d -sdk %t -prebuilt-module-cache-path %t/ResourceDir/%target-sdk-name/prebuilt-modules
// RUN: %validate-json %t/deps.json | %FileCheck %s -check-prefix=BINARY_MODULE_ONLY
