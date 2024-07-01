// REQUIRES: OS=macosx
// RUN: %empty-directory(%t)
// RUN: %empty-directory(%t/clang-module-cache)
// RUN: %empty-directory(%t/inputs)

// RUN: split-file %s %t
// RUN: sed -e "s|INPUTSDIR|%/t/inputs|g" %t/map.json.template > %t/map.json.template1
// RUN: sed -e "s|STDLIBMOD|%/stdlib_module|g" %t/map.json.template1 > %t/map.json.template2
// RUN: sed -e "s|ONONEMOD|%/ononesupport_module|g" %t/map.json.template2 > %t/map.json.template3
// RUN: sed -e "s|CHEADERSDIR|%/S/Inputs/CHeaders|g" %t/map.json.template3 > %t/map.json.template4
// RUN: sed -e "s|SWIFTLIBDIR|%swift-lib-dir|g" %t/map.json.template4 > %t/inputs/map.json

// Pre-build common mandatory deps
// RUN: %target-swift-emit-pcm -target %target-cpu-apple-macosx12.0 -module-name SwiftShims %swift-lib-dir/swift/shims/module.modulemap -o %t/inputs/SwiftShims.pcm
// Pre-build the clang deps with versioned code
// RUN: %target-swift-emit-pcm -target %target-cpu-apple-macosx12.0 -module-name Bar %S/Inputs/CHeaders/ExtraCModules/module.modulemap -o %t/inputs/Bar.pcm 

// RUN: %swift-frontend -target %target-cpu-apple-macosx10.14 -O -emit-ir -disable-implicit-concurrency-module-import -disable-implicit-string-processing-module-import -disable-implicit-swift-modules -explicit-swift-module-map-file %t/inputs/map.json -o %t/explicit-clang-target-irgen.ll %t/test.swift -clang-target %target-cpu-apple-macosx12.0
// RUN: %FileCheck %s < %t/explicit-clang-target-irgen.ll

// Ensure that the platform version check is not optimized away, which it would, if we code-generate for '-clang-target' of macosx12.0
// CHECK: {{%[0-9]+}} = tail call i32 @__isPlatformVersionAtLeast(i32 1, i32 11, i32 0, i32 0)

//--- map.json.template
[
  {
      "moduleName": "Bar",
      "clangModulePath": "INPUTSDIR/Bar.pcm",
      "clangModuleMapPath": "CHEADERSDIR/ExtraCModules/module.modulemap",
      "isFramework": false
  },
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


//--- test.swift
import Bar
print(Bar.funcBar())
