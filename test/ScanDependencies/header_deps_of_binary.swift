// REQUIRES: objc_interop, OS=macosx
// RUN: %empty-directory(%t)
// RUN: %empty-directory(%t/clang-module-cache)
// RUN: %empty-directory(%t/PCH)
// RUN: %empty-directory(%t/SwiftModules)

// - Set up Foo Swift dependency
// RUN: echo "extension Profiler {" >> %t/foo.swift
// RUN: echo "    public static let count: Int = 42" >> %t/foo.swift
// RUN: echo "}" >> %t/foo.swift

// - Set up Foo bridging header
// RUN: echo "struct Profiler { void* ptr; };" >> %t/foo.h

// - Compile bridging header
// RUN: %target-swift-frontend -enable-objc-interop -emit-pch %t/foo.h -o %t/PCH/foo.pch -disable-implicit-swift-modules

// - Set up explicit dependencies for Foo
// RUN: %target-swift-emit-pcm -module-name SwiftShims %swift-lib-dir/swift/shims/module.modulemap -o %t/inputs/SwiftShims.pcm
// RUN: %target-swift-emit-pcm -module-name _SwiftConcurrencyShims %swift-lib-dir/swift/shims/module.modulemap -o %t/inputs/_SwiftConcurrencyShims.pcm
// RUN: echo "[{" > %t/foo_inputs_map.json
// RUN: echo "\"moduleName\": \"Swift\"," >> %t/foo_inputs_map.json
// RUN: echo "\"modulePath\": \"%/stdlib_module\"," >> %t/foo_inputs_map.json
// RUN: echo "\"isFramework\": false" >> %t/foo_inputs_map.json
// RUN: echo "}," >> %t/foo_inputs_map.json
// RUN: echo "{" >> %t/foo_inputs_map.json
// RUN: echo "\"moduleName\": \"SwiftOnoneSupport\"," >> %t/foo_inputs_map.json
// RUN: echo "\"modulePath\": \"%/ononesupport_module\"," >> %t/foo_inputs_map.json
// RUN: echo "\"isFramework\": false" >> %t/foo_inputs_map.json
// RUN: echo "}," >> %t/foo_inputs_map.json
// RUN: echo "{" >> %t/foo_inputs_map.json
// RUN: echo "\"moduleName\": \"_StringProcessing\"," >> %t/foo_inputs_map.json
// RUN: echo "\"modulePath\": \"%/string_processing_module\"," >> %t/foo_inputs_map.json
// RUN: echo "\"isFramework\": false" >> %t/foo_inputs_map.json
// RUN: echo "}," >> %t/foo_inputs_map.json
// RUN: echo "{" >> %t/foo_inputs_map.json
// RUN: echo "\"moduleName\": \"SwiftShims\"," >> %t/foo_inputs_map.json
// RUN: echo "\"isFramework\": false," >> %t/foo_inputs_map.json
// RUN: echo "\"clangModuleMapPath\": \"%swift-lib-dir/swift/shims/module.modulemap\"," >> %t/foo_inputs_map.json
// RUN: echo "\"clangModulePath\": \"%t/inputs/SwiftShims.pcm\"" >> %t/foo_inputs_map.json
// RUN: echo "}," >> %t/foo_inputs_map.json
// RUN: echo "{" >> %t/foo_inputs_map.json
// RUN: echo "\"moduleName\": \"_SwiftConcurrencyShims\"," >> %t/foo_inputs_map.json
// RUN: echo "\"isFramework\": false," >> %t/foo_inputs_map.json
// RUN: echo "\"clangModuleMapPath\": \"%swift-lib-dir/swift/shims/module.modulemap\"," >> %t/foo_inputs_map.json
// RUN: echo "\"clangModulePath\": \"%t/inputs/_SwiftConcurrencyShims.pcm\"" >> %t/foo_inputs_map.json
// RUN: echo "}]" >> %t/foo_inputs_map.json

// - Build Foo module dependency, explicitly
// RUN: %target-swift-frontend -emit-module -emit-module-path %t/SwiftModules/Foo.swiftmodule %t/foo.swift -module-name Foo -import-objc-header %t/PCH/foo.pch -disable-implicit-concurrency-module-import -disable-implicit-string-processing-module-import -disable-implicit-swift-modules -explicit-swift-module-map-file %t/foo_inputs_map.json 

// - Scan main module
// RUN: %target-swift-frontend -scan-dependencies %s -I %t/SwiftModules -I %S/Inputs/Swift -o %t/deps.json
// RUN: %validate-json %t/deps.json | %FileCheck %s

// CHECK: "swift": "FooClient"
// CHECK: "swift": "FooClient"
// CHECK: "swiftPrebuiltExternal": "Foo"
// CHECK:     "commandLine": [
// CHECK:            "-include-pch",
// CHECK-NEXT:       "-Xcc",
// CHECK-NEXT:       "{{.*}}{{/|\\}}PCH{{/|\\}}foo.pch"


// CHECK: "swiftPrebuiltExternal": "Foo"
// CHECK:   "headerDependencies": [
// CHECK:      "{{.*}}{{/|\\}}PCH{{/|\\}}foo.pch"
// CHECK:   ],

import FooClient
