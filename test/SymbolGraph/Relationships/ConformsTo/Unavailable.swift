// RUN: %empty-directory(%t)
// RUN: %target-build-swift %s -module-name Unavailable -emit-module -emit-module-path %t/
// RUN: %target-swift-symbolgraph-extract -module-name Unavailable -I %t -pretty-print -output-dir %t
// RUN: %FileCheck %s --input-file %t/Unavailable.symbols.json

public class MyClass {}

@available(*, unavailable)
extension MyClass: Sendable {}

// CHECK-NOT: conformsTo
