// RUN: %empty-directory(%t)
// RUN: %target-swift-frontend -emit-module -module-name ClangTargetModule -emit-module-path %t/has_clang_target.swiftmodule -parse-as-library -serialize-debugging-options -clang-target arm64e-apple-macos12.12 %s

// Check the serialized flags paths.
// RUN: llvm-bcanalyzer -dump %t/has_clang_target.swiftmodule > %t/has_clang_target.swiftmodule.txt
// RUN: %FileCheck %s < %t/has_clang_target.swiftmodule.txt

// CHECK-LABEL: <OPTIONS_BLOCK
// CHECK:      <XCC abbrevid={{[0-9]+}}/> blob data = '-triple'
// CHECK-NEXT: <XCC abbrevid={{[0-9]+}}/> blob data = 'arm64e-apple-macos12.12'
// CHECK: </OPTIONS_BLOCK>
