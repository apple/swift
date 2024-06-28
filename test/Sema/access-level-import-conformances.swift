// RUN: %empty-directory(%t)
// RUN: split-file --leading-lines %s %t

/// Build the libraries.
// RUN: %target-swift-frontend -emit-module %t/ConformanceBaseTypes.swift -o %t
// RUN: %target-swift-frontend -emit-module %t/ConformanceDefinition.swift -o %t -I %t

/// Check diagnostics.
// RUN: %target-swift-frontend -typecheck -verify %t/Client.swift -I %t

//--- ConformanceBaseTypes.swift
public protocol Proto {}
public struct ConformingType {
    public init () {}
}

//--- ConformanceDefinition.swift
import ConformanceBaseTypes
extension ConformingType : Proto  {}

//--- Client.swift
public import ConformanceBaseTypes
internal import ConformanceDefinition // expected-note 2 {{extension of struct 'ConformingType' imported as 'internal' from 'ConformanceDefinition' here}}

public func useInAPI(a: any Proto = ConformingType()) { // expected-error {{cannot use conformance of 'ConformingType' to 'Proto' here; 'ConformanceDefinition' was not imported publicly}}
}

@inlinable public func inlinableFunc() {
  let _: any Proto = ConformingType() // expected-error {{cannot use conformance of 'ConformingType' to 'Proto' here; 'ConformanceDefinition' was not imported publicly}}
}
