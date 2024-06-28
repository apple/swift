// REQUIRES: objc_interop
// RUN: %empty-directory(%t)
// RUN: %empty-directory(%t/FooInputs)
// RUN: %empty-directory(%t/BridgingHeaderDir)
// RUN: %empty-directory(%t/TestCHeaders)
// RUN: %empty-directory(%t/TestSwiftInterfaces)
// RUN: %empty-directory(%t/FooModuleDir)
// RUN: split-file %s %t

// - Fixup the input module file map
// RUN: sed -e "s|INPUTSDIR|%/t/FooInputs|g" %t/map.json.template > %t/map.json.template1
// RUN: sed -e "s|STDLIBMOD|%/stdlib_module|g" %t/map.json.template1 > %t/map.json.template2
// RUN: sed -e "s|ONONEMOD|%/ononesupport_module|g" %t/map.json.template2 > %t/map.json.template3
// RUN: sed -e "s|SWIFTLIBDIR|%swift-lib-dir|g" %t/map.json.template3 > %t/map.json

// - Set up explicit dependencies for Foo
// RUN: %target-swift-emit-pcm -module-name SwiftShims %swift-lib-dir/swift/shims/module.modulemap -o %t/FooInputs/SwiftShims.pcm
// - Build Foo module dependency, explicitly, non-resiliently
// RUN: %target-swift-frontend -emit-module -emit-module-path %t/FooModuleDir/Foo.swiftmodule %t/foo.swift -module-name Foo -import-objc-header %t/BridgingHeaderDir/foo.h -disable-implicit-concurrency-module-import -disable-implicit-string-processing-module-import -disable-implicit-swift-modules -explicit-swift-module-map-file %t/map.json -I %S/Inputs/CHeaders

// - Scan main module and ensure that the "FooClient" recipe includes the modulemap for Foo's briding header's module dependencies
// but not other dependencies
// RUN: %target-swift-frontend -scan-dependencies -module-load-mode prefer-interface %t/bridging_header_dep_module_map.swift -I %t/FooModuleDir -I %t/TestSwiftInterfaces -I %t/TestCHeaders -I %S/Inputs/CHeaders -o %t/deps.json
// RUN: %validate-json %t/deps.json | %FileCheck %s

// Given the following dependency graph:
//
//           main
//            |
//          FooClient (.swiftinterface)
//            |                 \
//          Foo(.swiftmodule)   Dart (.pcm)
//
// Given that 'Foo.swiftmodule' is built with a bridging header which imports 'X.h' ('X' clang module)
// We expect that 'Foo' will have a dependency on module 'X', and the scanner will ensure that 'FooClient' is built
// with the modulemap file for 'X' as an explicit input. 'Dart' Clang module however, must not result in an
// explicitly-specified modulemap file because no headers of this module will be ingested into the Swift
// compiler.

// Dependency of the main module
// CHECK: "swift": "FooClient"

// Definition of 'FooClient' in the dependency graph
// CHECK: "swift": "FooClient"
// CHECK: "modulePath": "{{.*}}FooClient-{{.*}}.swiftmodule",
// CHECK: "directDependencies": [
// CHECK-DAG:    "swiftPrebuiltExternal": "Foo"
// CHECK-DAG:    "swift": "SwiftOnoneSupport"
// CHECK-DAG:    "clang": "Dart"
// CHECK: ],
// CHECK: "commandLine": [
// CHECK: "-Xcc"
// CHECK-NEXT: "-fno-implicit-modules"
// CHECK: "-Xcc"
// CHECK-NEXT: "-fno-implicit-module-maps"
// CHECK-DAG: "-Xcc",
// CHECK-NEXT: "-fmodule-file=Dart={{.*}}"
// CHECK-DAG: "-Xcc"
// CHECK-NEXT: "-fmodule-map-file={{.*}}{{/|\\}}CHeaders{{/|\\}}module.modulemap"
// CHECK-DAG: "-Xcc",
// CHECK-NEXT: "-fmodule-file=SwiftShims={{.*}}"
// CHECK-DAG: "-Xcc",
// CHECK-NEXT: "-fmodule-file=X={{.*}}"
// CHECK-NOT: "-fmodule-map-file={{.*}}{{/|\\}}TestCHeaders{{/|\\}}module.modulemap"
// CHECK: ]

// Definition of 'Foo' in the dependency graph
// CHECK: "swiftPrebuiltExternal": "Foo"
// CHECK: "modulePath": "{{.*}}Foo.swiftmodule",
// CHECK-NEXT: "directDependencies": [
// CHECK-DAG:    "swift": "Swift"
// CHECK-DAG:    "swift": "SwiftOnoneSupport"
// CHECK-DAG:    "clang": "X"
// CHECK: ],
// CHECK:      "headerDependency": "{{.*}}{{/|\\}}BridgingHeaderDir{{/|\\}}foo.h"
// CHECK:      "headerModuleDependencies": [
// CHECK-NEXT:   "X"
// CHECK-NEXT: ],
// CHECK:      "headerDependenciesSourceFiles": [
// CHECK-NEXT:   "{{.*}}{{/|\\}}BridgingHeaderDir{{/|\\}}foo.h"
// CHECK-NEXT: ],

//--- foo.swift
extension Profiler {
    public static let count: Int = 42
}

//--- BridgingHeaderDir/foo.h
#include "X.h"
struct Profiler { void* ptr; };

//--- TestCHeaders/Dart.h
struct Dart { void* ptr; };
//--- TestCHeaders/module.modulemap
module Dart {
  header "Dart.h"
  export *
}

//--- TestSwiftInterfaces/FooClient.swiftinterface
// swift-interface-format-version: 1.0
// swift-module-flags: -module-name FooClient
import Foo
import Dart

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

//--- bridging_header_dep_module_map.swift
import FooClient
