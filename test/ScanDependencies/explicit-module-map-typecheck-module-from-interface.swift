// RUN: %empty-directory(%t)
// RUN: mkdir -p %t/clang-module-cache
// RUN: mkdir -p %t/inputs
// RUN: echo "/// Some cool comments" > %t/foo.swift
// RUN: echo "public func foo() {}" >> %t/foo.swift
// RUN: %target-swift-frontend -emit-module -emit-module-path %t/inputs/Foo.swiftmodule -emit-module-interface-path %t/Foo.swiftinterface \
// RUN:   -swift-version 5 -enable-library-evolution -module-cache-path %t.module-cache %t/foo.swift -module-name Foo

// RUN: echo "[{" > %/t/inputs/map.json
// RUN: echo "\"moduleName\": \"Foo\"," >> %/t/inputs/map.json
// RUN: echo "\"modulePath\": \"%/t/inputs/Foo.swiftmodule\"," >> %/t/inputs/map.json
// RUN: echo "\"docPath\": \"%/t/inputs/Foo.swiftdoc\"," >> %/t/inputs/map.json
// RUN: echo "\"sourceInfoPath\": \"%/t/inputs/Foo.swiftsourceinfo\"," >> %/t/inputs/map.json
// RUN: echo "\"isFramework\": false" >> %/t/inputs/map.json
// RUN: echo "}," >> %/t/inputs/map.json
// RUN: echo "{" >> %/t/inputs/map.json
// RUN: echo "\"moduleName\": \"Swift\"," >> %/t/inputs/map.json
// RUN: echo "\"modulePath\": \"%/stdlib_module\"," >> %/t/inputs/map.json
// RUN: echo "\"isFramework\": false" >> %/t/inputs/map.json
// RUN: echo "}," >> %/t/inputs/map.json
// RUN: echo "{" >> %/t/inputs/map.json
// RUN: echo "\"moduleName\": \"SwiftOnoneSupport\"," >> %/t/inputs/map.json
// RUN: echo "\"modulePath\": \"%/ononesupport_module\"," >> %/t/inputs/map.json
// RUN: echo "\"isFramework\": false" >> %/t/inputs/map.json
// RUN: echo "}," >> %/t/inputs/map.json
// RUN: echo "{" >> %/t/inputs/map.json
// RUN: echo "\"moduleName\": \"_Concurrency\"," >> %/t/inputs/map.json
// RUN: echo "\"modulePath\": \"%/concurrency_module\"," >> %/t/inputs/map.json
// RUN: echo "\"isFramework\": false" >> %/t/inputs/map.json
// RUN: echo "}," >> %/t/inputs/map.json
// RUN: echo "{" >> %/t/inputs/map.json
// RUN: echo "\"moduleName\": \"_StringProcessing\"," >> %/t/inputs/map.json
// RUN: echo "\"modulePath\": \"%/string_processing_module\"," >> %/t/inputs/map.json
// RUN: echo "\"isFramework\": false" >> %/t/inputs/map.json
// RUN: echo "}]" >> %/t/inputs/map.json

// RUN: %target-swift-frontend -typecheck-module-from-interface %t/Foo.swiftinterface -module-cache-path %t.module-cache \
// RUN:   -explicit-interface-module-build -explicit-swift-module-map-file %t/inputs/map.json -Rmodule-loading -Xcc -Rmodule-import 2>&1 | %FileCheck %s

// CHECK-DAG: loaded module 'Swift'
// CHECK-DAG: loaded module '_StringProcessing'
// CHECK-DAG: loaded module '_Concurrency'
