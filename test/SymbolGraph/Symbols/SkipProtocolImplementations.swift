// RUN: %empty-directory(%t)
// RUN: %target-swift-frontend %s -module-name SkipProtocolImplementations -emit-module -emit-module-path %t/SkipProtocolImplementations.swiftmodule -emit-symbol-graph -emit-symbol-graph-dir %t/ -skip-protocol-implementations
// RUN: %validate-json %t/SkipProtocolImplementations.symbols.json %t/SkipProtocolImplementations.formatted.symbols.json
// RUN: %FileCheck %s --input-file %t/SkipProtocolImplementations.formatted.symbols.json
// RUN: %FileCheck %s --input-file %t/SkipProtocolImplementations.formatted.symbols.json --check-prefix COUNT

// RUN: %empty-directory(%t)
// RUN: %target-swift-frontend %s -module-name SkipProtocolImplementations -emit-module -emit-module-path %t/SkipProtocolImplementations.swiftmodule -emit-module-doc-path %t/SkipProtocolImplementations.swiftdoc
// RUN: %target-swift-symbolgraph-extract -module-name SkipProtocolImplementations -I %t -skip-protocol-implementations -pretty-print -output-dir %t
// RUN: %FileCheck %s --input-file %t/SkipProtocolImplementations.symbols.json
// RUN: %FileCheck %s --input-file %t/SkipProtocolImplementations.symbols.json --check-prefix COUNT

// make sure that using `-skip-protocol-implementations` removes the functions from `SomeProtocol` on `SomeStruct`
// CHECK-NOT: s:27SkipProtocolImplementations04SomeB0PAAE9bonusFuncyyF::SYNTHESIZED::s:27SkipProtocolImplementations10SomeStructV
// CHECK-NOT: s:27SkipProtocolImplementations10SomeStructV8someFuncyyF

// ...as well as the inner type from `OtherProtocol` on `OtherStruct`
// CHECK-NOT: "s:27SkipProtocolImplementations11OtherStructV5InnerV"

// CHECK-LABEL: "symbols": [

// SomeStruct.otherFunc() should be present because it has its own doc comment
// CHECK-DAG: s:27SkipProtocolImplementations10SomeStructV9otherFuncyyF

// Same for ExtraStruct.Inner
// CHECK-DAG: s:27SkipProtocolImplementations11ExtraStructV5InnerV

// CHECK-LABEL: "relationships": [

// we want to make sure that the conformance relationship itself stays
// CHECK-DAG: conformsTo

// SomeStruct.otherFunc() and ExtraStruct.Inner should be the only ones with sourceOrigin information
// (ExtraStruct.Inner will have two sourceOrigins because it has two relationships: a memberOf and a
// conformsTo)
// COUNT-COUNT-3: sourceOrigin
// COUNT-NOT: sourceOrigin

public protocol SomeProtocol {
    /// Base docs
    func someFunc()

    /// Base docs
    func otherFunc()
}

public extension SomeProtocol {
    func bonusFunc() {}
}

public struct SomeStruct: SomeProtocol {
    public func someFunc() {}

    /// Local docs
    public func otherFunc() {}
}

// Make sure that protocol conformances added in extensions don't create bogus symbol relationships (rdar://107432084)

public protocol OtherProtocol {
    associatedtype Inner
}

public struct OtherStruct: OtherProtocol {
    public struct Inner {}
}

extension OtherStruct.Inner: Sendable {}

public struct ExtraStruct: OtherProtocol {
    /// This time with a doc comment!
    public struct Inner {}
}

extension ExtraStruct.Inner: Sendable {}
