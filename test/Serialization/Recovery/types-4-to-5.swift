// RUN: %empty-directory(%t)
// RUN: %target-swift-frontend -emit-module -o %t -module-name Lib -I %S/Inputs/custom-modules -swift-version 4 %s

// RUN: %target-swift-ide-test -source-filename=x -print-module -module-to-print Lib -I %t -I %S/Inputs/custom-modules -swift-version 4 | %FileCheck -check-prefix=CHECK-4 %s

// RUN: %target-swift-ide-test -source-filename=x -print-module -module-to-print Lib -I %t -I %S/Inputs/custom-modules -swift-version 5 | %FileCheck -check-prefix=CHECK-5 %s

// RUN: %target-swift-frontend -typecheck %s -I %t -I %S/Inputs/custom-modules  -swift-version 5 -D TEST -verify

// REQUIRES: objc_interop

#if TEST

import Lib

func requiresConformance(_: B_RequiresConformance<B_ConformsToProto>) {}
func requiresConformance(_: B_RequiresConformance<C_RelyOnConformanceImpl.Assoc>) {}

class Sub: Base {} // okay
class Impl: Proto {} // expected-error {{type 'Impl' does not conform to protocol 'Proto'}}

#else // TEST

import Types

// Please use prefixes to keep the printed parts of this file in alphabetical
// order.

public func A_renameAllTheThings(
  a: Swift4RenamedClass?,
  b: Swift4RenamedGenericClass<AnyObject>?,
  c: Swift4RenamedTypedef,
  d: Swift4RenamedStruct,
  e: Swift4RenamedEnum,
  f: Swift4RenamedProtocol,
  g: Swift4RenamedWrappedTypedef
) {}

// CHECK-5-LABEL: func A_renameAllTheThings(
// CHECK-5-SAME: a: RenamedClass?
// CHECK-5-SAME: b: RenamedGenericClass<AnyObject>?
// CHECK-5-SAME: c: RenamedTypedef
// CHECK-5-SAME: d: RenamedStruct
// CHECK-5-SAME: e: RenamedEnum
// CHECK-5-SAME: f: any RenamedProtocol
// CHECK-5-SAME: g: RenamedWrappedTypedef
// CHECK-5-SAME: )


// CHECK-4-LABEL: func A_renameAllTheThings(
// CHECK-4-SAME: a: Swift4RenamedClass?
// CHECK-4-SAME: b: Swift4RenamedGenericClass<AnyObject>?
// CHECK-4-SAME: c: Swift4RenamedTypedef
// CHECK-4-SAME: d: Swift4RenamedStruct
// CHECK-4-SAME: e: Swift4RenamedEnum
// CHECK-4-SAME: f: any Swift4RenamedProtocol
// CHECK-4-SAME: g: Swift4RenamedWrappedTypedef
// CHECK-4-SAME: )


public func A_renameGeneric<T: Swift4RenamedProtocol>(obj: T) {}

// CHECK-5-LABEL: func A_renameGeneric<T>(
// CHECK-5-SAME: where T : RenamedProtocol

// FIXME: Preserve sugar in requirements.
// CHECK-4-LABEL: func A_renameGeneric<T>(
// CHECK-4-SAME: where T : RenamedProtocol

public class B_ConformsToProto: Swift4RenamedProtocol {}

// CHECK-5-LABEL: class B_ConformsToProto : RenamedProtocol
// CHECK-4-LABEL: class B_ConformsToProto : Swift4RenamedProtocol

public struct B_RequiresConformance<T: Swift4RenamedProtocol> {}

// CHECK-5-LABEL: struct B_RequiresConformance<T> where T : RenamedProtocol

// FIXME: Preserve sugar in requirements.
// CHECK-4-LABEL: struct B_RequiresConformance<T> where T : RenamedProtocol

public protocol C_RelyOnConformance {
  associatedtype Assoc: Swift4RenamedProtocol
}

public class C_RelyOnConformanceImpl: C_RelyOnConformance {
  public typealias Assoc = Swift4RenamedClass
}

open class Base {
  public init(wrapped: NewlyWrappedTypedef) {}
}
public protocol Proto {
  func useWrapped(_ wrapped: NewlyWrappedTypedef)
}

#endif
