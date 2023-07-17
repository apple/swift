// RUN: %empty-directory(%t)
// RUN: mkdir -p %t/clang-module-cache
// RUN: mkdir -p %t/inputs

// RUN: echo "[{" > %/t/inputs/map.json
// RUN: echo "\"moduleName\": \"Darwin\"," >> %/t/inputs/map.json
// RUN: echo "\"modulePath\": \"%/t/inputs/Darwin.swiftmodule\"," >> %/t/inputs/map.json
// RUN: echo "\"docPath\": \"%/t/inputs/Darwin.swiftdoc\"," >> %/t/inputs/map.json
// RUN: echo "\"sourceInfoPath\": \"%/t/inputs/Darwin.swiftsourceinfo\"," >> %/t/inputs/map.json
// RUN: echo "\"isFramework\": false," >> %/t/inputs/map.json
// RUN: echo "\"isSystem\": true" >> %/t/inputs/map.json
// RUN: echo "}]" >> %/t/inputs/map.json

// RUN: %target-swift-frontend -scan-dependencies -module-cache-path %t/clang-module-cache %s -placeholder-dependency-module-map-file %t/inputs/map.json -o %t/deps.json

// Check the contents of the JSON output
// RUN: %validate-json %t/deps.json &>/dev/null
// RUN: %FileCheck %s < %t/deps.json

// REQUIRES: executable_test
// REQUIRES: objc_interop
// REQUIRES: OS=macosx
import Metal

// Ensure the dependency on Darwin is captured even though it is a placeholder

// CHECK:   "modulePath": "{{.*}}{{/|\\}}Metal-{{.*}}.swiftmodule",
// CHECK-NEXT:   "sourceFiles": [
// CHECK-NEXT:   ],
// CHECK:     {
// CHECK:       "swiftPlaceholder": "Darwin"
// CHECK:     },
