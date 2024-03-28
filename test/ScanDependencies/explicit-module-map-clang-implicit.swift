// REQUIRES: objc_interop
// RUN: %empty-directory(%t)
// RUN: mkdir -p %t/clang-module-cache
// RUN: mkdir -p %t/inputs

// RUN: echo "[{" > %/t/inputs/map.json
// RUN: echo "\"moduleName\": \"A\"," >> %/t/inputs/map.json
// RUN: echo "\"isFramework\": false," >> %/t/inputs/map.json
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
// RUN: echo "\"moduleName\": \"SwiftShims\"," >> %/t/inputs/map.json
// RUN: echo "\"isFramework\": false," >> %/t/inputs/map.json
// RUN: echo "\"clangModuleMapPath\": \"%swift-lib-dir/swift/shims/module.modulemap\"," >> %/t/inputs/map.json
// RUN: echo "}]" >> %/t/inputs/map.json

// RUN: %target-swift-frontend -emit-module -emit-module-path %t/Foo.swiftmodule -module-cache-path %t.module-cache -explicit-swift-module-map-file %t/inputs/map.json %s -disable-implicit-concurrency-module-import -disable-implicit-string-processing-module-import

import A

func callA() {
  funcA()
}
