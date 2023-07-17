// RUN: %empty-directory(%t)
// RUN: %empty-directory(%t/inputs)
// RUN: %empty-directory(%t/outputs)
// RUN: mkdir -p %t/clang-module-cache

// RUN: echo "[{" > %/t/inputs/input.json
// RUN: echo "\"clangModuleName\": \"G\"," >> %/t/inputs/input.json
// RUN: echo "\"arguments\": \"-Xcc -target -Xcc %target-cpu-apple-macosx11.0\"," >> %/t/inputs/input.json
// RUN: echo "\"output\": \"%/t/outputs/G_110.pcm.json\"" >> %/t/inputs/input.json
// RUN: echo "}," >> %/t/inputs/input.json
// RUN: echo "{" >> %/t/inputs/input.json
// RUN: echo "\"clangModuleName\": \"G\"," >> %/t/inputs/input.json
// RUN: echo "\"arguments\": \"-Xcc -target -Xcc %target-cpu-apple-macosx10.9\"," >> %/t/inputs/input.json
// RUN: echo "\"output\": \"%/t/outputs/G_109.pcm.json\"" >> %/t/inputs/input.json
// RUN: echo "}]" >> %/t/inputs/input.json

// RUN: %target-swift-frontend -scan-dependencies -disable-implicit-concurrency-module-import -disable-implicit-string-processing-module-import -target %target-cpu-apple-macosx11.0 -module-cache-path %t/clang-module-cache %s -o %t/deps.json -I %S/Inputs/CHeaders -I %S/Inputs/Swift -emit-dependencies -emit-dependencies-path %t/deps.d -import-objc-header %S/Inputs/CHeaders/Bridging.h -swift-version 4 -batch-scan-input-file %/t/inputs/input.json

// Check the contents of the JSON output
// RUN: %validate-json %t/outputs/G_109.pcm.json | %FileCheck %s -check-prefix=CHECK-PCM109
// RUN: %validate-json %t/outputs/G_110.pcm.json &>/dev/null
// RUN: %FileCheck %s -check-prefix=CHECK-PCM110 < %t/outputs/G_110.pcm.json

// CHECK-PCM109: 		{
// CHECK-PCM109-NEXT:  "mainModuleName": "G",
// CHECK-PCM109-NEXT:  "modules": [
// CHECK-PCM109-NEXT:    {
// CHECK-PCM109-NEXT:      "clang": "G"
// CHECK-PCM109-NEXT:    },
// CHECK-PCM109-NEXT:    {
// CHECK-PCM109-NEXT:      "modulePath": "{{.*}}{{/|\\}}G-{{.*}}.pcm",
// CHECK-PCM109:       "directDependencies": [
// CHECK-PCM109-NEXT:    {
// CHECK-PCM109-NEXT:      "clang": "X"
// CHECK-PCM109-NEXT:    }
// CHECK-PCM109-NEXT:  ],

// CHECK-PCM110: 		{
// CHECK-PCM110-NEXT:  "mainModuleName": "G",
// CHECK-PCM110-NEXT:  "modules": [
// CHECK-PCM110-NEXT:    {
// CHECK-PCM110-NEXT:      "clang": "G"
// CHECK-PCM110-NEXT:    },
// CHECK-PCM110-NEXT:    {
// CHECK-PCM110-NEXT:      "modulePath": "{{.*}}{{/|\\}}G-{{.*}}.pcm",
// CHECK-PCM110:       "directDependencies": [
// CHECK-PCM110-NEXT:  ],
// CHECK-PCM110-NOT: "clang": "X"
