// RUN: %empty-directory(%t)
// RUN: %target-swift-frontend -swift-version 5 %s -emit-module -emit-module-path %t/OpaqueParams.swiftmodule -emit-symbol-graph -emit-symbol-graph-dir %t/
// RUN: %validate-json %t/OpaqueParams.symbols.json %t/OpaqueParams.formatted.symbols.json
// RUN: %FileCheck %s --input-file %t/OpaqueParams.formatted.symbols.json

// CHECK: "precise": "s:12OpaqueParams7MyClassC6myFunc5param10otherParamyq__xtAA0C8ProtocolRzAaGR_r0_lF"

public protocol MyProtocol {}

public class MyClass {
    public func myFunc<S: MyProtocol>(
        param: some MyProtocol,
        otherParam: S
    ) {}
}
