// REQUIRES: objc_interop
// RUN: %empty-directory(%t)
// RUN: %empty-directory(%t/TestInputs)
// RUN: split-file %s %t

// - Fixup the input module file map
// RUN: sed -e "s|INPUTSDIR|%/t/TestInputs|g" %t/map.json.template > %t/map.json.template1
// RUN: sed -e "s|STDLIBMOD|%/stdlib_module|g" %t/map.json.template1 > %t/map.json.template2
// RUN: sed -e "s|ONONEMOD|%/ononesupport_module|g" %t/map.json.template2 > %t/map.json.template3
// RUN: sed -e "s|CHEADERSDIR|%/S/Inputs/CHeaders|g" %t/map.json.template3 > %t/map.json.template4
// RUN: sed -e "s|SWIFTLIBDIR|%swift-lib-dir|g" %t/map.json.template4 > %t/map.json

// - Pre-compile explicit module dependency inputs
// RUN: %target-swift-emit-pcm -module-name A -o %t/TestInputs/A.pcm %S/Inputs/CHeaders/module.modulemap
// RUN: %target-swift-emit-pcm -module-name SwiftShims %swift-lib-dir/swift/shims/module.modulemap -o %t/TestInputs/SwiftShims.pcm

// RUN: %target-swift-frontend -c -disable-implicit-swift-modules -disable-implicit-concurrency-module-import -disable-implicit-string-processing-module-import -explicit-swift-module-map-file %t/map.json -primary-file %t/bridging_header_modulemap_only.swift -o %t/bridging_header_modulemap_only.o -dump-clang-diagnostics 2>&1 | %FileCheck %s --check-prefix=CHECK-CLANG-COMMAND

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
      "isBridgingHeaderDependency": false,
      "clangModuleMapPath": "SWIFTLIBDIR/swift/shims/module.modulemap",
      "clangModulePath": "INPUTSDIR/SwiftShims.pcm"
  },
  {
      "moduleName": "A",
      "isFramework": false,
      "isBridgingHeaderDependency": true,      
      "clangModulePath": "INPUTSDIR/A.pcm",
      "clangModuleMapPath": "CHEADERSDIR/module.modulemap"
  }
]

//--- bridging_header_modulemap_only.swift
import A

// CHECK-CLANG-COMMAND: -fmodule-map-file={{.*}}{{/|\\}}CHeaders{{/|\\}}module.modulemap
// CHECK-CLANG-COMMAND-NOT: -fmodule-map-file={{.*}}{{/|\\}}swift{{/|\\}}shims{{/|\\}}module.modulemap
