// RUN: %empty-directory(%t)
// RUN: %target-swift-frontend -emit-module -o %t -module-name Lib -I %S/Inputs/custom-modules -swift-version 5 %s

// RUN: %target-swift-ide-test -source-filename=x -print-module -module-to-print Lib -I %t -I %S/Inputs/custom-modules -swift-version 5 | %FileCheck %s

// RUN: %target-swift-ide-test -source-filename=x -print-module -module-to-print Lib -I %t -I %S/Inputs/custom-modules -swift-version 4 | %FileCheck %s

// RUN: %target-swift-frontend -typecheck %s -I %t -I %S/Inputs/custom-modules  -swift-version 4 -D TEST -verify

// REQUIRES: objc_interop

#if TEST

import Lib

func requiresConformance(_: B_RequiresConformance<B_ConformsToProto>) {}
func requiresConformance(_: B_RequiresConformance<C_RelyOnConformanceImpl.Assoc>) {}

class Sub: Base {} // expected-error {{cannot inherit from class 'Base' (compiled with Swift 5.9) because it has overridable members that could not be loaded in Swift 4.1.50}}
class Impl: Proto {} // expected-error {{type 'Impl' cannot conform to protocol 'Proto' (compiled with Swift 5.9) because it has requirements that could not be loaded in Swift 4.1.50}}

#else // TEST

import Types

// Please use prefixes to keep the printed parts of this file in alphabetical
// order.

public func A_renameAllTheThings(
  a: RenamedClass?,
  b: RenamedGenericClass<AnyObject>?,
  c: RenamedTypedef,
  d: RenamedStruct,
  e: RenamedEnum,
  f: RenamedProtocol,
  g: RenamedWrappedTypedef
) {}

// CHECK-LABEL: func A_renameAllTheThings(
// CHECK-SAME: a: RenamedClass?
// CHECK-SAME: b: RenamedGenericClass<AnyObject>?
// CHECK-SAME: c: RenamedTypedef
// CHECK-SAME: d: RenamedStruct
// CHECK-SAME: e: RenamedEnum
// CHECK-SAME: f: any RenamedProtocol
// CHECK-SAME: g: RenamedWrappedTypedef
// CHECK-SAME: )


public func A_renameGeneric<T: RenamedProtocol>(obj: T) {}

// CHECK-LABEL: func A_renameGeneric<T>(
// CHECK-SAME: where T : RenamedProtocol

public class B_ConformsToProto: RenamedProtocol {}
// CHECK-LABEL: class B_ConformsToProto : RenamedProtocol

public struct B_RequiresConformance<T: RenamedProtocol> {}
// CHECK-LABEL: struct B_RequiresConformance<T> where T : RenamedProtocol

public protocol C_RelyOnConformance {
  associatedtype Assoc: RenamedProtocol
}

public class C_RelyOnConformanceImpl: C_RelyOnConformance {
  public typealias Assoc = RenamedClass
}

open class Base {
  public init(wrapped: NewlyWrappedTypedef) {}
}
public protocol Proto {
  func useWrapped(_ wrapped: NewlyWrappedTypedef)
}

#endif
