// REQUIRES: objc_interop
// RUN: %empty-directory(%t)
// RUN: %empty-directory(%t/inputs)
// RUN: %empty-directory(%t/clang-module-cache)
// RUN: %empty-directory(%t/PCH)
// RUN: %empty-directory(%t/HEADER)
// RUN: %empty-directory(%t/SwiftModules)
// RUN: split-file %s %t

// - Fixup the input module file map
// RUN: sed -e "s|INPUTSDIR|%/t/inputs|g" %t/map.json.template > %t/map.json.template1
// RUN: sed -e "s|STDLIBMOD|%/stdlib_module|g" %t/map.json.template1 > %t/map.json.template2
// RUN: sed -e "s|ONONEMOD|%/ononesupport_module|g" %t/map.json.template2 > %t/map.json.template3
// RUN: sed -e "s|SWIFTLIBDIR|%swift-lib-dir|g" %t/map.json.template3 > %t/map.json

// - Compile bridging header
// RUN: %target-swift-frontend -enable-objc-interop -emit-pch %t/HEADER/foo.h -o %t/PCH/foo.pch -disable-implicit-swift-modules -disable-implicit-concurrency-module-import -disable-implicit-string-processing-module-import -I %S/Inputs/CHeaders

// - Set up explicit dependencies for Foo
// RUN: %target-swift-emit-pcm -module-name SwiftShims %swift-lib-dir/swift/shims/module.modulemap -o %t/inputs/SwiftShims.pcm

// - Build Foo module dependency, explicitly
// RUN: %target-swift-frontend -emit-module -emit-module-path %t/SwiftModules/Foo.swiftmodule %t/foo.swift -module-name Foo -import-objc-header %t/PCH/foo.pch -disable-implicit-concurrency-module-import -disable-implicit-string-processing-module-import -disable-implicit-swift-modules -explicit-swift-module-map-file %t/map.json

// - Scan main module and ensure that the header dependencies point to .h and not .pch file
// RUN: %target-swift-frontend -scan-dependencies -module-load-mode prefer-interface %t/header_deps_of_binary.swift -I %t/SwiftModules -I %S/Inputs/Swift -I %S/Inputs/CHeaders -o %t/deps.json
// RUN: %validate-json %t/deps.json | %FileCheck %s

// CHECK: "swift": "FooClient"
// CHECK: "swift": "FooClient"
// CHECK: "swiftPrebuiltExternal": "Foo"
// CHECK: "swiftPrebuiltExternal": "Foo"
// CHECK: "modulePath": "{{.*}}Foo.swiftmodule",
// CHECK-NEXT: "directDependencies": [
// CHECK-DAG:    "swift": "Swift"
// CHECK-DAG:    "swift": "SwiftOnoneSupport"
// CHECK-DAG:    "clang": "X"
// CHECK: ],
// CHECK:      "headerDependency": "{{.*}}{{/|\\}}HEADER{{/|\\}}foo.h"
// CHECK:      "headerModuleDependencies": [
// CHECK-NEXT:   "X"
// CHECK-NEXT: ],
// CHECK:      "headerDependenciesSourceFiles": [
// CHECK-NEXT:   "{{.*}}{{/|\\}}HEADER{{/|\\}}foo.h"
// CHECK-NEXT: ],

//--- foo.swift
extension Profiler {
    public static let count: Int = 42
}

//--- HEADER/foo.h
#include "X.h"
struct Profiler { void* ptr; };

//--- map.json.template
[
  {
      "moduleName": "Swift",
      "modulePath": "STDLIBMOD",
      "isFramework": false
  },
  {
      "moduleName": "SwiftOnoneSupport",
      "modulePath": "ONONEMOD",
      "isFramework": false
  },
  {
      "moduleName": "SwiftShims",
      "isFramework": false,
      "clangModuleMapPath": "SWIFTLIBDIR/swift/shims/module.modulemap",
      "clangModulePath": "INPUTSDIR/SwiftShims.pcm"
}]

//--- header_deps_of_binary.swift
import FooClient
