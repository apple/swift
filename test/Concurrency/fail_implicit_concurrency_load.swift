// This test ensures that if implicit import of the Concurrency module is enabled,
// but no such module can be located (here verified by forcing explicit modules),
// a warning diagnostic is emitted.
// UNSUPPORTED: OS=windows-msvc
// REQUIRES: concurrency
// RUN: %empty-directory(%t)
// RUN: mkdir -p %t/inputs
// RUN: %target-swift-emit-pcm -module-name SwiftShims %swift-lib-dir/swift/shims/module.modulemap -o %t/inputs/SwiftShims.pcm
// RUN: %target-swift-emit-pcm -module-name _SwiftConcurrencyShims %swift-lib-dir/swift/shims/module.modulemap -o %t/inputs/_SwiftConcurrencyShims.pcm

// RUN: echo "[{" > %/t/inputs/map.json
// RUN: echo "\"moduleName\": \"Swift\"," >> %/t/inputs/map.json
// RUN: echo "\"modulePath\": \"%/stdlib_module\"," >> %/t/inputs/map.json
// RUN: echo "\"isFramework\": false" >> %/t/inputs/map.json
// RUN: echo "}," >> %/t/inputs/map.json
// RUN: echo "{" >> %/t/inputs/map.json
// RUN: echo "\"moduleName\": \"SwiftShims\"," >> %/t/inputs/map.json
// RUN: echo "\"isFramework\": false," >> %/t/inputs/map.json
// RUN: echo "\"clangModuleMapPath\": \"%swift-lib-dir/swift/shims/module.modulemap\"," >> %/t/inputs/map.json
// RUN: echo "\"clangModulePath\": \"%t/inputs/SwiftShims.pcm\"" >> %/t/inputs/map.json
// RUN: echo "}," >> %/t/inputs/map.json
// RUN: echo "{" >> %/t/inputs/map.json
// RUN: echo "\"moduleName\": \"_SwiftConcurrencyShims\"," >> %/t/inputs/map.json
// RUN: echo "\"isFramework\": false," >> %/t/inputs/map.json
// RUN: echo "\"clangModuleMapPath\": \"%swift-lib-dir/swift/shims/module.modulemap\"," >> %/t/inputs/map.json
// RUN: echo "\"clangModulePath\": \"%t/inputs/_SwiftConcurrencyShims.pcm\"" >> %/t/inputs/map.json
// RUN: echo "}," >> %/t/inputs/map.json
// RUN: echo "{" >> %/t/inputs/map.json
// RUN: echo "\"moduleName\": \"SwiftOnoneSupport\"," >> %/t/inputs/map.json
// RUN: echo "\"modulePath\": \"%/ononesupport_module\"," >> %/t/inputs/map.json
// RUN: echo "\"isFramework\": false" >> %/t/inputs/map.json
// RUN: echo "}]" >> %/t/inputs/map.json

// RUN: %target-swift-frontend -typecheck %s -explicit-swift-module-map-file %t/inputs/map.json -disable-implicit-swift-modules  -disable-availability-checking 2>&1 | %FileCheck %s
import Swift
// CHECK: warning: unable to perform implicit import of "_Concurrency" module: no such module found
