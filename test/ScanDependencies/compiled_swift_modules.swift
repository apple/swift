// RUN: %empty-directory(%t)
// RUN: %empty-directory(%t/Foo.swiftmodule)
// RUN: %empty-directory(%t/ResourceDir/%target-sdk-name/prebuilt-modules/Foo.swiftmodule)
// RUN: echo "public func foo() {}" > %t/Foo.swift

import Foo

// HAS_COMPILED: "compiledModuleCandidates": [
// HAS_COMPILED-NEXT: "{{.*}}Foo.swiftmodule{{.*}}.swiftmodule"

// HAS_NO_COMPILED-NOT: "{{.*}}Foo.swiftmodule{{.*}}.swiftmodule"

// Step 1: build swift interface and swift module side by side
// RUN: %target-swift-frontend -emit-module %t/Foo.swift -emit-module-path %t/Foo.swiftmodule/%target-swiftmodule-name -module-name Foo -emit-module-interface-path %t/Foo.swiftmodule/%target-swiftinterface-name

// Step 2: scan dependency should give us the binary module adjacent to the interface file.
// RUN: %target-swift-frontend -scan-dependencies %s -o %t/deps.json -I %t -emit-dependencies -emit-dependencies-path %t/deps.d
// RUN: %validate-json %t/deps.json | %FileCheck %s -check-prefix=HAS_COMPILED

// Step 3: remove the adjacent module.
// RUN: rm %t/Foo.swiftmodule/%target-swiftmodule-name

// Step 4: scan dependency should give us the interface file.
// RUN: %target-swift-frontend -scan-dependencies %s -o %t/deps.json -I %t -emit-dependencies -emit-dependencies-path %t/deps.d
// RUN: %validate-json %t/deps.json | %FileCheck %s -check-prefix=HAS_NO_COMPILED

// Step 4: build prebuilt module cache using the interface.
// RUN: %target-swift-frontend -compile-module-from-interface -o %t/ResourceDir/%target-sdk-name/prebuilt-modules/Foo.swiftmodule/%target-swiftmodule-name -module-name Foo -disable-interface-lock %t/Foo.swiftmodule/%target-swiftinterface-name

// Step 5: scan dependency now should give us the prebuilt module cache
// RUN: %target-swift-frontend -scan-dependencies %s -o %t/deps.json -I %t -emit-dependencies -emit-dependencies-path %t/deps.d -sdk %t -prebuilt-module-cache-path %t/ResourceDir/%target-sdk-name/prebuilt-modules
// RUN: %validate-json %t/deps.json | %FileCheck %s -check-prefix=HAS_COMPILED

// Step 6: update the interface file from where the prebuilt module cache was built.
// RUN: touch %t/Foo.swiftmodule/%target-swiftinterface-name

// Step 7: scan dependency should give us the prebuilt module file even though it's out-of-date.
// RUN: %target-swift-frontend -scan-dependencies %s -o %t/deps.json -I %t -emit-dependencies -emit-dependencies-path %t/deps.d -sdk %t -prebuilt-module-cache-path %t/ResourceDir/%target-sdk-name/prebuilt-modules
// RUN: %validate-json %t/deps.json | %FileCheck %s -check-prefix=HAS_COMPILED
