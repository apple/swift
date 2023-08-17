// RUN: %empty-directory(%t)
// RUN: %target-build-swift %s -module-name UnavailableExtension -emit-module -emit-module-path %t/
// RUN: %target-swift-symbolgraph-extract -module-name UnavailableExtension -I %t -pretty-print -output-dir %t
// RUN: %FileCheck %s --input-file %t/UnavailableExtension.symbols.json 

public class MyClass {}

@available(*, unavailable)
public extension MyClass {
    func myFunc() {}
}

// CHECK-NOT: myFunc
