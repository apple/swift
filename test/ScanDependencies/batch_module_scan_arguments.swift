// RUN: %empty-directory(%t)
// RUN: %empty-directory(%t/inputs)
// RUN: %empty-directory(%t/outputs)
// RUN: mkdir -p %t/clang-module-cache

// RUN: echo "[{" > %/t/inputs/input.json
// RUN: echo "\"clangModuleName\": \"H\"," >> %/t/inputs/input.json
// RUN: echo "\"arguments\": \"-target x86_64-apple-macosx10.9\"," >> %/t/inputs/input.json
// RUN: echo "\"output\": \"%/t/outputs/H.10.9.pcm.json\"" >> %/t/inputs/input.json
// RUN: echo "}," >> %/t/inputs/input.json
// RUN: echo "{" >> %/t/inputs/input.json
// RUN: echo "\"clangModuleName\": \"H\"," >> %/t/inputs/input.json
// RUN: echo "\"arguments\": \"-target x86_64-apple-macosx11.0\"," >> %/t/inputs/input.json
// RUN: echo "\"output\": \"%/t/outputs/H.11.0.pcm.json\"" >> %/t/inputs/input.json
// RUN: echo "}]" >> %/t/inputs/input.json

// RUN: %target-swift-frontend -scan-dependencies -module-cache-path %t/clang-module-cache %s -o %t/deps.json -I %S/Inputs/CHeaders -I %S/Inputs/Swift -emit-dependencies -emit-dependencies-path %t/deps.d -import-objc-header %S/Inputs/CHeaders/Bridging.h -swift-version 4 -batch-scan-input-file %/t/inputs/input.json

// Check the contents of the JSON output
// RUN: %validate-json %t/outputs/H.10.9.pcm.json | %FileCheck %s -check-prefix=CHECK-TEN
// RUN: %validate-json %t/outputs/H.11.0.pcm.json | %FileCheck %s -check-prefix=CHECK-ELEVEN

// CHECK-TEN: "clang": "I"
// CHECK-ELEVEN-NOT: "clang": "I"
