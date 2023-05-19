// UNSUPPORTED: OS=windows-msvc
// RUN: %empty-directory(%t)
// RUN: mkdir -p %t/clang-module-cache
// RUN: mkdir -p %t/inputs
// RUN: echo "public func anotherFuncA() {}" > %t/A.swift
// RUN: %target-swift-frontend -emit-module -emit-module-path %t/inputs/A.swiftmodule -emit-module-doc-path %t/inputs/A.swiftdoc -emit-module-source-info -emit-module-source-info-path %t/inputs/A.swiftsourceinfo -import-underlying-module -I%S/Inputs/CHeaders -module-cache-path %t.module-cache %t/A.swift -module-name A
// RUN: %target-swift-emit-pcm -module-name A -o %t/inputs/A.pcm %S/Inputs/CHeaders/module.modulemap
// RUN: %target-swift-emit-pcm -module-name SwiftShims %swift_obj_root/lib/swift/shims/module.modulemap -o %t/inputs/SwiftShims.pcm
// RUN: %target-swift-emit-pcm -module-name _SwiftConcurrencyShims %swift_obj_root/lib/swift/shims/module.modulemap -o %t/inputs/_SwiftConcurrencyShims.pcm

// RUN: echo "[{" > %/t/inputs/map.json
// RUN: echo "\"moduleName\": \"A\"," >> %/t/inputs/map.json
// RUN: echo "\"modulePath\": \"%/t/inputs/A.swiftmodule\"," >> %/t/inputs/map.json
// RUN: echo "\"docPath\": \"%/t/inputs/A.swiftdoc\"," >> %/t/inputs/map.json
// RUN: echo "\"sourceInfoPath\": \"%/t/inputs/A.swiftsourceinfo\"," >> %/t/inputs/map.json
// RUN: echo "\"isFramework\": false," >> %/t/inputs/map.json
// RUN: echo "}," >> %/t/inputs/map.json
// RUN: echo "{" >> %/t/inputs/map.json
// RUN: echo "\"moduleName\": \"A\"," >> %/t/inputs/map.json
// RUN: echo "\"clangModulePath\": \"%/t/inputs/A.pcm\"," >> %/t/inputs/map.json
// RUN: echo "\"clangModuleMapPath\": \"%/S/Inputs/CHeaders/module.modulemap\"" >> %/t/inputs/map.json
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
// RUN: echo "\"moduleName\": \"SwiftShims\"," >> %/t/inputs/map.json
// RUN: echo "\"isFramework\": false," >> %/t/inputs/map.json
// RUN: echo "\"clangModuleMapPath\": \"%swift_obj_root/lib/swift/shims/module.modulemap\"," >> %/t/inputs/map.json
// RUN: echo "\"clangModulePath\": \"%t/inputs/SwiftShims.pcm\"" >> %/t/inputs/map.json
// RUN: echo "}," >> %/t/inputs/map.json
// RUN: echo "{" >> %/t/inputs/map.json
// RUN: echo "\"moduleName\": \"_SwiftConcurrencyShims\"," >> %/t/inputs/map.json
// RUN: echo "\"isFramework\": false," >> %/t/inputs/map.json
// RUN: echo "\"clangModuleMapPath\": \"%swift_obj_root/lib/swift/shims/module.modulemap\"," >> %/t/inputs/map.json
// RUN: echo "\"clangModulePath\": \"%t/inputs/_SwiftConcurrencyShims.pcm\"" >> %/t/inputs/map.json
// RUN: echo "}," >> %/t/inputs/map.json
// RUN: echo "{" >> %/t/inputs/map.json
// RUN: echo "\"moduleName\": \"_StringProcessing\"," >> %/t/inputs/map.json
// RUN: echo "\"modulePath\": \"%/string_processing_module\"," >> %/t/inputs/map.json
// RUN: echo "\"isFramework\": false" >> %/t/inputs/map.json
// RUN: echo "}]" >> %/t/inputs/map.json

// RUN: %target-swift-frontend -emit-module -emit-module-path %t/Foo.swiftmodule -disable-implicit-swift-modules -module-cache-path %t.module-cache -explicit-swift-module-map-file %t/inputs/map.json -Rmodule-loading -Xcc -Rmodule-import %s 2>&1 | %FileCheck %s

// CHECK: <unknown>:0: remark: importing module 'A' from {{.*}}{{/|\\}}explicit-module-map-clang-and-swift.swift.tmp{{/|\\}}inputs{{/|\\}}A.pcm'

import A

func callA() {
  funcA()
  anotherFuncA()
}
