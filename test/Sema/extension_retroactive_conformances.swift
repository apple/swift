// RUN: %empty-directory(%t)
// RUN: %target-swift-frontend -swift-version 5 -enable-library-evolution -DLIBRARY %s -emit-module -module-name Library -o %t/Library.swiftmodule
// RUN: %target-typecheck-verify-swift -swift-version 5 -enable-library-evolution -I %t

#if LIBRARY
// Define a couple protocols with no requirements that're easy to conform to
public protocol SampleProtocol1 {}
public protocol SampleProtocol2 {}

public struct Sample1 {}
public struct Sample2 {}
public struct Sample3 {}
public struct Sample4 {}
public struct Sample5 {}
public struct Sample6 {}

public struct SampleAlreadyConforms: SampleProtocol1 {}
#else

import Library

extension Sample1: SampleProtocol1 {} // expected-warning {{extension declares a conformance of imported type 'Sample1' to imported protocol 'SampleProtocol1'}}
// expected-note @-1 {{explicitly qualify all imported types involved in this extension to silence this warning}} {{11-18=Library.Sample1}} {{20-35=Library.SampleProtocol1}}

protocol InheritsSampleProtocol: SampleProtocol1 {}
protocol NestedInheritsSampleProtocol: InheritsSampleProtocol {}

protocol InheritsMultipleSampleProtocols: SampleProtocol1 {}
protocol NestedInheritsMultipleSampleProtocols: InheritsSampleProtocol, SampleProtocol2 {}

extension Sample2: InheritsSampleProtocol {} // expected-warning {{extension declares a conformance of imported type 'Sample2' to imported protocol 'SampleProtocol1'}}
// expected-note @-1 {{explicitly qualify all imported types involved in this extension to silence this warning}} {{11-18=Library.Sample2}} {{1-1=extension Library.Sample2: Library.SampleProtocol1 {\}\n}}

extension SampleAlreadyConforms: InheritsSampleProtocol {} // ok, SampleAlreadyConforms already conforms in the source module

extension Sample3: NestedInheritsSampleProtocol {} // expected-warning {{extension declares a conformance of imported type 'Sample3' to imported protocol 'SampleProtocol1'}}
// expected-note @-1 {{explicitly qualify all imported types involved in this extension to silence this warning}} {{11-18=Library.Sample3}} {{1-1=extension Library.Sample3: Library.SampleProtocol1 {\}\n}}

extension Sample4: NestedInheritsMultipleSampleProtocols {} // expected-warning {{extension declares a conformance of imported type 'Sample4' to imported protocols 'SampleProtocol2', 'SampleProtocol1'}}
// expected-note @-1 {{explicitly qualify all imported types involved in this extension to silence this warning}} {{11-18=Library.Sample4}} {{1-1=extension Library.Sample4: Library.SampleProtocol2 {\}\nextension Library.Sample4: Library.SampleProtocol1 {\}\n}}

extension Library.Sample5: Library.SampleProtocol2, Library.SampleProtocol1 {}

// ok, explicit module qualification in previous extension silences the warning
extension Sample5: NestedInheritsMultipleSampleProtocols {}

// Check that looking through typealiases replaces the underlying type

typealias MySample6 = Sample6

extension MySample6: SampleProtocol1 {} // expected-warning {{extension declares a conformance of imported type 'Sample6' to imported protocol 'SampleProtocol1'}}
// expected-note @-1 {{explicitly qualify all imported types involved in this extension to silence this warning}} {{11-20=Library.Sample6}} {{22-37=Library.SampleProtocol1}}

#endif