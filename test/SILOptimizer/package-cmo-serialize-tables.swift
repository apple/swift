/// This tests serializing v-table and witness-table with Package CMO in resilient mode.
///
// RUN: %empty-directory(%t)
// RUN: split-file %s %t

// RUN: %target-build-swift %t/Lib.swift \
// RUN: -module-name=Lib -package-name Pkg \
// RUN: -parse-as-library -emit-module -emit-module-path %t/Lib.swiftmodule -I%t \
// RUN: -Xfrontend -experimental-package-cmo -Xfrontend -experimental-allow-non-resilient-access \
// RUN: -enable-library-evolution -wmo

// RUN: %target-sil-opt %t/Lib.swiftmodule -sil-verify-all -o %t/Lib.sil
// RUN: %FileCheck %s < %t/Lib.sil

// RUN: %target-build-swift -module-name=Main -package-name Pkg -enable-library-evolution -I%t -emit-sil %t/main.swift -o %t/Main.sil
// RUN: %FileCheck %s --check-prefix=CHECK-MAIN < %t/Main.sil

// REQUIRES: swift_in_compiler


//--- main.swift

import Lib

// CHECK-MAIN-NOT: witness_method
// CHECK-MAIN-NOT: class_method
runPub([PubStruct(rawValue: 2), PubKlassZ(rawValue: 3)])
runPkg([PkgStruct(rawValue: 2), PkgKlassZ(rawValue: 3)])

 
//--- Lib.swift

public class ParentPubKlass {
  public var parentPubVar: Int
  public init(_ arg: Int) {
    parentPubVar = arg
  }
  public func parentPubFunc() {
    // CHECK-DAG: sil [serialized_for_package] [canonical] [ossa] @$s3Lib14ParentPubKlassC06parentC4FuncyyF
    print(parentPubVar)
  }
}

public class PubKlass: ParentPubKlass {
  public var pubVar: String = "publicVar"
  public init(_ arg: String) {
    super.init(1)
    pubVar = arg
  }

  public func pubFunc() {
    // CHECK-DAG: sil [serialized_for_package] [canonical] [ossa] @$s3Lib8PubKlassC7pubFuncyyF
    print(pubVar)
  }

  override public func parentPubFunc() {
    // CHECK-DAG: sil [serialized_for_package] [canonical] [ossa] @$s3Lib8PubKlassC06parentB4FuncyyF
    print(pubVar)
  }
}

public class ParentPubKlassWithInternalMemberX {
  public var parentPubVar: Int
  var parentIntVar: Int
  public init(_ arg: Int) {
    // CHECK-DAG: sil [serialized] [exact_self_class] [canonical] [ossa] @$s3Lib33ParentPubKlassWithInternalMemberXCyACSicfC : $@convention(method) (Int, @thick ParentPubKlassWithInternalMemberX.Type) -> @owned ParentPubKlassWithInternalMemberX {
    // CHECK-DAG: sil [canonical] @$s3Lib33ParentPubKlassWithInternalMemberXCyACSicfc : $@convention(method) (Int, @owned ParentPubKlassWithInternalMemberX) -> @owned ParentPubKlassWithInternalMemberX
    parentPubVar = arg
    parentIntVar = arg
  }
  public func parentPubFuncA() {
    // CHECK-DAG: sil [serialized_for_package] [canonical] [ossa] @$s3Lib33ParentPubKlassWithInternalMemberXC06parentC5FuncAyyF
    print(parentPubVar)
  }

  public func parentPubFuncB() {
    print(parentPubVar, parentIntVar) /// NOTE: parentPubFuncB() can't be serialized since it contains an internal var
  }

  func parentIntFunc() { /// NOTE: internal, so not serialized
    print(parentIntVar)
  }
}

public class PubKlassX: ParentPubKlassWithInternalMemberX {
  public var pubVar: String = "publicVar"

  override public var parentPubVar: Int {
    // CHECK-DAG: sil [serialized_for_package] [canonical] [ossa] @$s3Lib9PubKlassXC06parentB3VarSivg
    didSet { print ("newValue") }
  }

  public init() {
    // CHECK-DAG: sil [serialized] [exact_self_class] [canonical] [ossa] @$s3Lib9PubKlassXCACycfC : $@convention(method) (@thick PubKlassX.Type) -> @owned PubKlassX {
    // CHECK-DAG: sil [serialized_for_package] [canonical] [ossa] @$s3Lib9PubKlassXCACycfc : $@convention(method) (@owned PubKlassX) -> @owned PubKlassX {
    super.init(1)
  }

  override public func parentPubFuncA() {
    print(pubVar, parentIntVar)  /// NOTE: contains internal; not serialized
  }
  override public func parentPubFuncB() {
    // CHECK-DAG: sil [serialized_for_package] [canonical] [ossa] @$s3Lib9PubKlassXC06parentB5FuncByyF
    print(pubVar)
  }
  override func parentIntFunc() { /// NOTE: contains internal; not serialized
    print(pubVar)
  }
  public func pubFunc() {
    // CHECK-DAG: sil [serialized_for_package] [canonical] [ossa] @$s3Lib9PubKlassXC7pubFuncyyF
    print(pubVar)
  }
}

public class ParentPubKlassWithInternalMemberY {
  var parentIntVar: Int   /// NOTE: internal; not serialized
  public init(_ arg: Int) { /// NOTE: contains internal; not serialized
    // CHECK-DAG: sil [serialized] [exact_self_class] [canonical] [ossa] @$s3Lib33ParentPubKlassWithInternalMemberYCyACSicfC : $@convention(method) (Int, @thick ParentPubKlassWithInternalMemberY.Type) -> @owned ParentPubKlassWithInternalMemberY {
    // CHECK-DAG: sil [canonical] @$s3Lib33ParentPubKlassWithInternalMemberYCyACSicfc : $@convention(method) (Int, @owned ParentPubKlassWithInternalMemberY) -> @owned ParentPubKlassWithInternalMemberY
    parentIntVar = arg
  }

  func parentIntFunc() { /// NOTE: contains internal; not serialized
    print(parentIntVar)
  }
}

public class PubKlassY: ParentPubKlassWithInternalMemberY {
  public var pubVar: String = "publicVar"
  public init() { super.init(1) }
}

package class ParentPkgKlass {
  package var parentPkgVar: Int
  package init(_ arg: Int) {
    // CHECK-DAG: sil package [serialized_for_package] [canonical] [ossa] @$s3Lib14ParentPkgKlassCyACSicfc : $@convention(method) (Int, @owned ParentPkgKlass) -> @owned ParentPkgKlass {
    parentPkgVar = arg
  }
  package func parentPkgFunc() -> Int {
    // CHECK-DAG: sil package [serialized_for_package] [canonical] [ossa] @$s3Lib14ParentPkgKlassC06parentC4FuncSiyF
    parentPkgVar
  }
}

package class PkgKlass: ParentPkgKlass {
  package var pkgVar: String = "pkgVar"
  package init(_ arg: String) {
    // CHECK-DAG: sil package [serialized_for_package] [canonical] [ossa] @$s3Lib8PkgKlassCyACSScfc
    super.init(1)
    pkgVar = arg
  }
  package func pkgFunc() { 
    // CHECK-DAG: sil package [serialized_for_package] [canonical] [ossa] @$s3Lib8PkgKlassC7pkgFuncyyF
    print(pkgVar)
  }
  override package func parentPkgFunc() -> Int {
    // CHECK-DAG: sil package [serialized_for_package] [canonical] [ossa] @$s3Lib8PkgKlassC06parentB4FuncSiyF
    pkgVar.count
  }
}

package class ParentPkgKlassWithInternalMemberX {
  package var parentPkgVar: Int
  var parentIntVar: Int /// NOTE: internal so not serialized

  package init(_ arg: Int) {
    // CHECK-DAG: sil package [serialized_for_package] [exact_self_class] [canonical] [ossa] @$s3Lib33ParentPkgKlassWithInternalMemberXCyACSicfC : $@convention(method) (Int, @thick ParentPkgKlassWithInternalMemberX.Type) -> @owned ParentPkgKlassWithInternalMemberX {
    // CHECK-DAG: sil package_external [canonical] @$s3Lib33ParentPkgKlassWithInternalMemberXCyACSicfc : $@convention(method) (Int, @owned ParentPkgKlassWithInternalMemberX) -> @owned ParentPkgKlassWithInternalMemberX
    parentPkgVar = arg
    parentIntVar = arg
  }

  package func parentPkgFuncA() {
    // CHECK-DAG: sil package [serialized_for_package] [canonical] [ossa] @$s3Lib33ParentPkgKlassWithInternalMemberXC06parentC5FuncAyyF
    print(parentPkgVar)
  }
  package func parentPkgFuncB() { /// NOTE: contains internal; not serialized
    print(parentPkgVar, parentIntVar)
  }
  func parentIntFunc() { /// NOTE: internal so not serialized
    print(parentIntVar)
  }
}

package class PkgKlassX: ParentPkgKlassWithInternalMemberX {
  package var pkgVar: String = "pkgVar"
  override package var parentPkgVar: Int {
    // CHECK-DAG: sil package [serialized_for_package] [canonical] [ossa] @$s3Lib9PkgKlassXC06parentB3VarSivg
    didSet { print ("newValue") }
  }
  package init() {
    // CHECK-DAG: sil package [serialized_for_package] [exact_self_class] [canonical] [ossa] @$s3Lib9PkgKlassXCyACSicfC : $@convention(method) (Int, @thick PkgKlassX.Type) -> @owned PkgKlassX {
    // CHECK-DAG: sil package [serialized_for_package] [canonical] [ossa] @$s3Lib9PkgKlassXCyACSicfc : $@convention(method) (Int, @owned PkgKlassX) -> @owned PkgKlassX {
    super.init(1)
  }
  override package func parentPkgFuncA() {
    print(pkgVar, parentIntVar)
  }
  override package func parentPkgFuncB() {
    // CHECK-DAG: sil package [serialized_for_package] [canonical] [ossa] @$s3Lib9PkgKlassXC06parentB5FuncByyF
    print(pkgVar)
  }
  override func parentIntFunc() { /// NOTE: contains internal; not serialized
    print(pkgVar)
  }
  package func pubFunc() {
    // CHECK-DAG: sil package [serialized_for_package] [canonical] [ossa] @$s3Lib9PkgKlassXC7pubFuncyyF
    print(pkgVar)
  }
}

package class ParentPkgKlassWithInternalMemberY {
  var parentIntVar: Int
  package init(_ arg: Int) {
    // CHECK-DAG: sil package [serialized_for_package] [exact_self_class] [canonical] [ossa] @$s3Lib33ParentPkgKlassWithInternalMemberYCyACSicfC : $@convention(method) (Int, @thick ParentPkgKlassWithInternalMemberY.Type) -> @owned ParentPkgKlassWithInternalMemberY {
    // CHECK-DAG: sil package_external [canonical] @$s3Lib33ParentPkgKlassWithInternalMemberYCyACSicfc : $@convention(method) (Int, @owned ParentPkgKlassWithInternalMemberY) -> @owned ParentPkgKlassWithInternalMemberY
    parentIntVar = arg
  }
  func parentIntFunc() {
    print(parentIntVar)
  }
}

package class PkgKlassY: ParentPkgKlassWithInternalMemberY {
  package var pkgVar: String = "pkgVar"
  package init() {
    // CHECK-DAG: sil package [serialized_for_package] [exact_self_class] [canonical] [ossa] @$s3Lib9PkgKlassYCACycfC : $@convention(method) (@thick PkgKlassY.Type) -> @owned PkgKlassY {
    // CHECK-DAG: sil package [serialized_for_package] [canonical] [ossa] @$s3Lib9PkgKlassYCACycfc : $@convention(method) (@owned PkgKlassY) -> @owned PkgKlassY {
    super.init(1)
  }
}

public protocol PubProto {
  associatedtype Element = Self
  static var root: UInt16 { get }
  var env: UInt16 { get set }
  init(rawValue: UInt16)
  func pubFunc()
}

/// NOTE: witness thunks get `shared` linkage
public class PubKlassZ: PubProto {
  // CHECK-DAG: sil shared [transparent] [serialized] [thunk] [canonical] [ossa] @$s3Lib9PubKlassZCAA0B5ProtoA2aDP4roots6UInt16VvgZTW
  public static let root: UInt16 = 1 << 0

  // CHECK-DAG: sil shared [transparent] [serialized] [thunk] [canonical] [ossa] @$s3Lib9PubKlassZCAA0B5ProtoA2aDP3envs6UInt16VvgTW
  // CHECK-DAG: sil shared [transparent] [serialized] [thunk] [canonical] [ossa] @$s3Lib9PubKlassZCAA0B5ProtoA2aDP3envs6UInt16VvsTW
  // CHECK-DAG: sil shared [transparent] [serialized] [thunk] [canonical] [ossa] @$s3Lib9PubKlassZCAA0B5ProtoA2aDP3envs6UInt16VvMTW
  // CHECK-DAG: sil [serialized_for_package] [canonical] [ossa] @$s3Lib9PubKlassZC3envs6UInt16Vvg
  // CHECK-DAG: sil [serialized_for_package] [canonical] [ossa] @$s3Lib9PubKlassZC3envs6UInt16Vvs
  // CHECK-DAG: sil [serialized_for_package] [canonical] [ossa] @$s3Lib9PubKlassZC3envs6UInt16VvM
  public var env: UInt16

  // CHECK-DAG: sil [serialized_for_package] [canonical] [ossa] @$s3Lib9PubKlassZC8rawValues6UInt16Vvg
  public let rawValue: UInt16

  required public init(rawValue: UInt16) {
    // CHECK-DAG: sil shared [transparent] [serialized] [thunk] [canonical] [ossa] @$s3Lib9PubKlassZCAA0B5ProtoA2aDP8rawValuexs6UInt16V_tcfCTW
    // CHECK-DAG: sil [serialized_for_package] [canonical] [ossa] @$s3Lib9PubKlassZC8rawValueACs6UInt16V_tcfc
    self.rawValue = rawValue
    self.env = 1 << rawValue
  }
  public func pubFunc() {
    // CHECK-DAG: sil shared [transparent] [serialized] [thunk] [canonical] [ossa] @$s3Lib9PubKlassZCAA0B5ProtoA2aDP7pubFuncyyFTW
    // CHECK-DAG: sil [serialized_for_package] [canonical] [ossa] @$s3Lib9PubKlassZC7pubFuncyyF 
    print(env)
  }
}

public struct PubStruct: PubProto {
  // FIXME: rdar://130103572 witness thunks should get [serialized_for_package] in package-cmo.
  // protocol witness for static PubProto.root.getter in conformance PubStruct
  // CHECK-DAG: sil shared [transparent] [serialized] [thunk] [canonical] [ossa] @$s3Lib9PubStructVAA0B5ProtoA2aDP4roots6UInt16VvgZTW : $@convention(witness_method: PubProto) (@thick PubStruct.Type) -> UInt16 {
  // CHECK-DAG: function_ref @$s3Lib9PubStructV4roots6UInt16VvgZ : $@convention(method) (@thin PubStruct.Type) -> UInt16
  // CHECK-DAG: sil [canonical] @$s3Lib9PubStructV4roots6UInt16VvgZ : $@convention(method) (@thin PubStruct.Type) -> UInt16
  public static let root: UInt16 = 1 << 0

  // CHECK-DAG: sil shared [transparent] [serialized] [thunk] [canonical] [ossa] @$s3Lib9PubStructVAA0B5ProtoA2aDP3envs6UInt16VvgTW : $@convention(witness_method: PubProto) (@in_guaranteed PubStruct) -> UInt16 {
  // CHECK-DAG: sil shared [transparent] [serialized] [thunk] [canonical] [ossa] @$s3Lib9PubStructVAA0B5ProtoA2aDP3envs6UInt16VvsTW : $@convention(witness_method: PubProto) (UInt16, @inout PubStruct) -> () {
  // CHECK-DAG: sil shared [transparent] [serialized] [thunk] [canonical] [ossa] @$s3Lib9PubStructVAA0B5ProtoA2aDP3envs6UInt16VvMTW : $@yield_once @convention(witness_method: PubProto) @substituted <τ_0_0> (@inout τ_0_0) -> @yields @inout UInt16 for <PubStruct> {
  // CHECK-DAG: sil [serialized_for_package] [canonical] [ossa] @$s3Lib9PubStructV3envs6UInt16Vvg
  // CHECK-DAG: sil [serialized_for_package] [canonical] [ossa] @$s3Lib9PubStructV3envs6UInt16Vvs
  public var env: UInt16

  // CHECK-DAG: sil [serialized_for_package] [canonical] [ossa] @$s3Lib9PubStructV8rawValues6UInt16Vvg
  public let rawValue: UInt16

  public init(rawValue: UInt16) {
    // CHECK-DAG: sil shared [transparent] [serialized] [thunk] [canonical] [ossa] @$s3Lib9PubStructVAA0B5ProtoA2aDP8rawValuexs6UInt16V_tcfCTW : $@convention(witness_method: PubProto) (UInt16, @thick PubStruct.Type) -> @out PubStruct {
    // CHECK-DAG: sil [serialized_for_package] [canonical] [ossa] @$s3Lib9PubStructV8rawValueACs6UInt16V_tcfC
    self.rawValue = rawValue
    self.env = 1 << rawValue
  }

  public func pubFunc() {
    // CHECK-DAG: sil shared [transparent] [serialized] [thunk] [canonical] [ossa] @$s3Lib9PubStructVAA0B5ProtoA2aDP7pubFuncyyFTW
    // CHECK-DAG: sil [serialized_for_package] [canonical] [ossa] @$s3Lib9PubStructV7pubFuncyyF
    print(env)
  }
}

public protocol PubSimpleProto {
  var pubVar: Int { get set }
  func pubFunc() -> Int
}

struct InternalStruct {
  var name: String = "internalVar"
}

protocol InternalProto {
  var intVar: InternalStruct { get set }
  func intFunc() -> InternalStruct
}

public struct PubStructX: PubSimpleProto, InternalProto { /// NOTE: witness table serialized only for PubSimpleProto

  // CHECK-DAG: sil shared [transparent] [serialized] [thunk] [canonical] [ossa] @$s3Lib10PubStructXVAA0B11SimpleProtoA2aDP6pubVarSivgTW : $@convention(witness_method: PubSimpleProto) (@in_guaranteed PubStructX) -> Int {
  // CHECK-DAG: sil shared [transparent] [serialized] [thunk] [canonical] [ossa] @$s3Lib10PubStructXVAA0B11SimpleProtoA2aDP6pubVarSivsTW : $@convention(witness_method: PubSimpleProto) (Int, @inout PubStructX) -> () {
  // CHECK-DAG: sil shared [transparent] [serialized] [thunk] [canonical] [ossa] @$s3Lib10PubStructXVAA0B11SimpleProtoA2aDP6pubVarSivMTW : $@yield_once @convention(witness_method: PubSimpleProto) @substituted <τ_0_0> (@inout τ_0_0) -> @yields @inout Int for <PubStructX> {
  // CHECK-DAG: sil [serialized_for_package] [canonical] [ossa] @$s3Lib10PubStructXV6pubVarSivg
  // CHECK-DAG: sil [serialized_for_package] [canonical] [ossa] @$s3Lib10PubStructXV6pubVarSivs
  // CHECK-DAG: sil [serialized_for_package] [canonical] [ossa] @$s3Lib10PubStructXV6pubVarSivM
  public var pubVar: Int

  var intVar: InternalStruct /// NOTE: internal; not serialized

  public init(_ arg: Int) { /// NOTE: contains internal; not serialized
    self.intVar = InternalStruct(name: "foo")
    self.pubVar = arg
  }
  public func pubFunc() -> Int { 
    // CHECK-DAG: sil [serialized_for_package] [canonical] [ossa] @$s3Lib10PubStructXV7pubFuncSiyF
    return pubVar
  }

  func intFunc() -> InternalStruct { /// NOTE: internal; not serialized
    return intVar
  }
}

public struct PubStructY: InternalProto { /// NOTE: conforms to internal proto; not serialized
  var intVar: InternalStruct
  public init() {
    self.intVar = InternalStruct(name: "foo")
  }
  func intFunc() -> InternalStruct {
    return intVar
  }
}

package protocol PkgProto {
  associatedtype Element = Self
  static var root: UInt16 { get }
  var env: UInt16 { get set }
  init(rawValue: UInt16)
  func pkgFunc()
}

/// NOTE: witness thunks get `shared` linkage
package class PkgKlassZ: PkgProto {
  // protocol witness for static PkgProto.root.getter in conformance PkgKlassZ
  // CHECK-DAG: sil shared [transparent] [serialized] [thunk] [canonical] [ossa] @$s3Lib9PkgKlassZCAA0B5ProtoA2aDP4roots6UInt16VvgZTW : $@convention(witness_method: PkgProto) (@thick PkgKlassZ.Type) -> UInt16 {
  // CHECK-DAG: function_ref @$s3Lib9PkgKlassZC4roots6UInt16VvgZ : $@convention(method) (@thick PkgKlassZ.Type) -> UInt16
  // CHECK-DAG: sil package_external [canonical] @$s3Lib9PkgKlassZC4roots6UInt16VvgZ : $@convention(method) (@thick PkgKlassZ.Type) -> UInt16
  package static let root: UInt16 = 1 << 0

  // CHECK-DAG: sil shared [transparent] [serialized] [thunk] [canonical] [ossa] @$s3Lib9PkgKlassZCAA0B5ProtoA2aDP3envs6UInt16VvgTW : $@convention(witness_method: PkgProto) (@in_guaranteed PkgKlassZ) -> UInt16 {
  // CHECK-DAG: sil shared [transparent] [serialized] [thunk] [canonical] [ossa] @$s3Lib9PkgKlassZCAA0B5ProtoA2aDP3envs6UInt16VvsTW : $@convention(witness_method: PkgProto) (UInt16, @inout PkgKlassZ) -> () {
  // CHECK-DAG: sil package [serialized_for_package] [canonical] [ossa] @$s3Lib9PkgKlassZC3envs6UInt16Vvg
  // CHECK-DAG: sil package [serialized_for_package] [canonical] [ossa] @$s3Lib9PkgKlassZC3envs6UInt16Vvs
  // CHECK-DAG: sil package [serialized_for_package] [canonical] [ossa] @$s3Lib9PkgKlassZC3envs6UInt16VvM
  package var env: UInt16

  // CHECK-DAG: sil package [serialized_for_package] [canonical] [ossa] @$s3Lib9PkgKlassZC8rawValues6UInt16Vvg
  package let rawValue: UInt16

  required package init(rawValue: UInt16) {
    // CHECK-DAG: sil shared [transparent] [serialized] [thunk] [canonical] [ossa] @$s3Lib9PkgKlassZCAA0B5ProtoA2aDP8rawValuexs6UInt16V_tcfCTW : $@convention(witness_method: PkgProto) (UInt16, @thick PkgKlassZ.Type) -> @out PkgKlassZ {
    // CHECK-DAG: sil package [serialized_for_package] [canonical] [ossa] @$s3Lib9PkgKlassZC8rawValueACs6UInt16V_tcfc : $@convention(method) (UInt16, @owned PkgKlassZ) -> @owned PkgKlassZ {
    self.rawValue = rawValue
    self.env = 1 << rawValue
  }
  package func pkgFunc() {
    // CHECK-DAG: sil shared [transparent] [serialized] [thunk] [canonical] [ossa] @$s3Lib9PkgKlassZCAA0B5ProtoA2aDP7pkgFuncyyFTW
    // CHECK-DAG: sil package [serialized_for_package] [canonical] [ossa] @$s3Lib9PkgKlassZC7pkgFuncyyF
    print(env)
  }
}

package struct PkgStruct: PkgProto { /// NOTE: witness thunks get `shared` linkage
  // protocol witness for static PkgProto.root.getter in conformance PkgStruct
  // CHECK-DAG: sil shared [transparent] [serialized] [thunk] [canonical] [ossa] @$s3Lib9PkgStructVAA0B5ProtoA2aDP4roots6UInt16VvgZTW : $@convention(witness_method: PkgProto) (@thick PkgStruct.Type) -> UInt16 {
  // CHECK-DAG: function_ref @$s3Lib9PkgStructV4roots6UInt16VvgZ : $@convention(method) (@thin PkgStruct.Type)
  // static PkgStruct.root.getter
  // CHECK-DAG: sil package_external [canonical] @$s3Lib9PkgStructV4roots6UInt16VvgZ : $@convention(method) (@thin PkgStruct.Type) -> UInt16
  package static let root: UInt16 = 1 << 0

  // CHECK-DAG: sil shared [transparent] [serialized] [thunk] [canonical] [ossa] @$s3Lib9PkgStructVAA0B5ProtoA2aDP3envs6UInt16VvsTW : $@convention(witness_method: PkgProto) (UInt16, @inout PkgStruct) -> () {
  // CHECK-DAG: sil package [serialized_for_package] [canonical] [ossa] @$s3Lib9PkgStructV3envs6UInt16Vvs
  package var env: UInt16

  // CHECK-DAG: sil package [serialized_for_package] [canonical] [ossa] @$s3Lib9PkgStructV8rawValues6UInt16Vvg
  package let rawValue: UInt16

  package init(rawValue: UInt16) {
    // CHECK-DAG: sil shared [transparent] [serialized] [thunk] [canonical] [ossa] @$s3Lib9PkgStructVAA0B5ProtoA2aDP8rawValuexs6UInt16V_tcfCTW : $@convention(witness_method: PkgProto) (UInt16, @thick PkgStruct.Type) -> @out PkgStruct {
    // CHECK-DAG: sil package [serialized_for_package] [canonical] [ossa] @$s3Lib9PkgStructV8rawValueACs6UInt16V_tcfC
    self.rawValue = rawValue
    self.env = 1 << rawValue
  }
  package func pkgFunc() {
    // CHECK-DAG: sil package [serialized_for_package] [canonical] [ossa] @$s3Lib9PkgStructV7pkgFuncyyF
    print(env)
  }
}

package protocol PkgSimpleProto {
  var pkgVar: Int { get set }
  func pkgFunc() -> Int
}

/// NOTE: only witness table of conformance to PkgSimpleProto is serialized.
package struct PkgStructX: PkgSimpleProto, InternalProto {
  // CHECK-DAG: sil shared [transparent] [serialized] [thunk] [canonical] [ossa] @$s3Lib10PkgStructXVAA0B11SimpleProtoA2aDP6pkgVarSivgTW : $@convention(witness_method: PkgSimpleProto) (@in_guaranteed PkgStructX) -> Int {
  // CHECK-DAG: sil shared [transparent] [serialized] [thunk] [canonical] [ossa] @$s3Lib10PkgStructXVAA0B11SimpleProtoA2aDP6pkgVarSivsTW : $@convention(witness_method: PkgSimpleProto) (Int, @inout PkgStructX) -> () {
  // CHECK-DAG: sil shared [transparent] [serialized] [thunk] [canonical] [ossa] @$s3Lib10PkgStructXVAA0B11SimpleProtoA2aDP6pkgVarSivMTW : $@yield_once @convention(witness_method: PkgSimpleProto) @substituted <τ_0_0> (@inout τ_0_0) -> @yields @inout Int for <PkgStructX> {
  // CHECK-DAG: sil package [serialized_for_package] [canonical] [ossa] @$s3Lib10PkgStructXV6pkgVarSivM
  // CHECK-DAG: sil package [serialized_for_package] [canonical] [ossa] @$s3Lib10PkgStructXV6pkgVarSivg
  // CHECK-DAG: sil package [serialized_for_package] [canonical] [ossa] @$s3Lib10PkgStructXV6pkgVarSivs
  package var pkgVar: Int
  var intVar: InternalStruct
  package init(_ arg: Int) {
    self.intVar = InternalStruct(name: "foo")
    self.pkgVar = arg
  }
  package func pkgFunc() -> Int { 
    // CHECK-DAG: sil shared [transparent] [serialized] [thunk] [canonical] [ossa] @$s3Lib10PkgStructXVAA0B11SimpleProtoA2aDP7pkgFuncSiyFTW
    // CHECK-DAG: sil package [serialized_for_package] [canonical] [ossa] @$s3Lib10PkgStructXV7pkgFuncSiyF
    return pkgVar
  }
  func intFunc() -> InternalStruct {
    return intVar
  }
}

/// NOTE: conforms to internal protocol, so not serialized
package struct PkgStructY: InternalProto {
  var intVar: InternalStruct
  package init() {
    self.intVar = InternalStruct(name: "foo")
  }
  func intFunc() -> InternalStruct {
    return intVar
  }
}

public func runPub(_ arg: [any PubProto]) {
  print(arg)
}

package func runPkg(_ arg: [any PkgProto]) {
  print(arg)
}



// CHECK-LABEL: sil_vtable [serialized_for_package] ParentPubKlass {
// CHECK-NEXT:  #ParentPubKlass.parentPubVar!getter: (ParentPubKlass) -> () -> Int : @$s3Lib14ParentPubKlassC06parentC3VarSivg  // ParentPubKlass.parentPubVar.getter
// CHECK-NEXT:  #ParentPubKlass.parentPubVar!setter: (ParentPubKlass) -> (Int) -> () : @$s3Lib14ParentPubKlassC06parentC3VarSivs  // ParentPubKlass.parentPubVar.setter
// CHECK-NEXT:  #ParentPubKlass.parentPubVar!modify: (ParentPubKlass) -> () -> () : @$s3Lib14ParentPubKlassC06parentC3VarSivM // ParentPubKlass.parentPubVar.modify
// CHECK-NEXT:  #ParentPubKlass.init!allocator: (ParentPubKlass.Type) -> (Int) -> ParentPubKlass : @$s3Lib14ParentPubKlassCyACSicfC // ParentPubKlass.__allocating_init(_:)
// CHECK-NEXT:  #ParentPubKlass.parentPubFunc: (ParentPubKlass) -> () -> () : @$s3Lib14ParentPubKlassC06parentC4FuncyyF // ParentPubKlass.parentPubFunc()
// CHECK-NEXT:  #ParentPubKlass.deinit!deallocator: @$s3Lib14ParentPubKlassCfD  // ParentPubKlass.__deallocating_deinit

// CHECK-LABEL: sil_vtable [serialized_for_package] PubKlass {
// CHECK-NEXT:  #ParentPubKlass.parentPubVar!getter: (ParentPubKlass) -> () -> Int : @$s3Lib14ParentPubKlassC06parentC3VarSivg [inherited]  // ParentPubKlass.parentPubVar.getter
// CHECK-NEXT:  #ParentPubKlass.parentPubVar!setter: (ParentPubKlass) -> (Int) -> () : @$s3Lib14ParentPubKlassC06parentC3VarSivs [inherited]  // ParentPubKlass.parentPubVar.setter
// CHECK-NEXT:  #ParentPubKlass.parentPubVar!modify: (ParentPubKlass) -> () -> () : @$s3Lib14ParentPubKlassC06parentC3VarSivM [inherited]  // ParentPubKlass.parentPubVar.modify
// CHECK-NEXT:  #ParentPubKlass.init!allocator: (ParentPubKlass.Type) -> (Int) -> ParentPubKlass : @$s3Lib8PubKlassCyACSicfC [override]  // PubKlass.__allocating_init(_:)
// CHECK-NEXT:  #ParentPubKlass.parentPubFunc: (ParentPubKlass) -> () -> () : @$s3Lib8PubKlassC06parentB4FuncyyF [override]  // PubKlass.parentPubFunc()
// CHECK-NEXT:  #PubKlass.pubVar!getter: (PubKlass) -> () -> String : @$s3Lib8PubKlassC6pubVarSSvg  // PubKlass.pubVar.getter
// CHECK-NEXT:  #PubKlass.pubVar!setter: (PubKlass) -> (String) -> () : @$s3Lib8PubKlassC6pubVarSSvs  // PubKlass.pubVar.setter
// CHECK-NEXT:  #PubKlass.pubVar!modify: (PubKlass) -> () -> () : @$s3Lib8PubKlassC6pubVarSSvM  // PubKlass.pubVar.modify
// CHECK-NEXT:  #PubKlass.init!allocator: (PubKlass.Type) -> (String) -> PubKlass : @$s3Lib8PubKlassCyACSScfC  // PubKlass.__allocating_init(_:)
// CHECK-NEXT:  #PubKlass.pubFunc: (PubKlass) -> () -> () : @$s3Lib8PubKlassC7pubFuncyyF  // PubKlass.pubFunc()
// CHECK-NEXT:  #PubKlass.deinit!deallocator: @$s3Lib8PubKlassCfD  // PubKlass.__deallocating_deinit
// CHECK-NEXT:  #PubKlass!ivardestroyer: @$s3Lib8PubKlassCfE  // PubKlass.__ivar_destroyer

// CHECK-LABEL: sil_vtable [serialized_for_package] ParentPkgKlass {
// CHECK-NEXT:  #ParentPkgKlass.parentPkgVar!getter: (ParentPkgKlass) -> () -> Int : @$s3Lib14ParentPkgKlassC06parentC3VarSivg  // ParentPkgKlass.parentPkgVar.getter
// CHECK-NEXT:  #ParentPkgKlass.parentPkgVar!setter: (ParentPkgKlass) -> (Int) -> () : @$s3Lib14ParentPkgKlassC06parentC3VarSivs  // ParentPkgKlass.parentPkgVar.setter
// CHECK-NEXT:  #ParentPkgKlass.parentPkgVar!modify: (ParentPkgKlass) -> () -> () : @$s3Lib14ParentPkgKlassC06parentC3VarSivM // ParentPkgKlass.parentPkgVar.modify
// CHECK-NEXT:  #ParentPkgKlass.init!allocator: (ParentPkgKlass.Type) -> (Int) -> ParentPkgKlass : @$s3Lib14ParentPkgKlassCyACSicfC // ParentPkgKlass.__allocating_init(_:)
// CHECK-NEXT:  #ParentPkgKlass.parentPkgFunc: (ParentPkgKlass) -> () -> Int : @$s3Lib14ParentPkgKlassC06parentC4FuncSiyF // ParentPkgKlass.parentPkgFunc()
// CHECK-NEXT:  #ParentPkgKlass.deinit!deallocator: @$s3Lib14ParentPkgKlassCfD  // ParentPkgKlass.__deallocating_deinit

// CHECK-LABEL: sil_vtable [serialized_for_package] PkgKlass {
// CHECK-NEXT:  #ParentPkgKlass.parentPkgVar!getter: (ParentPkgKlass) -> () -> Int : @$s3Lib14ParentPkgKlassC06parentC3VarSivg [inherited]  // ParentPkgKlass.parentPkgVar.getter
// CHECK-NEXT:  #ParentPkgKlass.parentPkgVar!setter: (ParentPkgKlass) -> (Int) -> () : @$s3Lib14ParentPkgKlassC06parentC3VarSivs [inherited]  // ParentPkgKlass.parentPkgVar.setter
// CHECK-NEXT:  #ParentPkgKlass.parentPkgVar!modify: (ParentPkgKlass) -> () -> () : @$s3Lib14ParentPkgKlassC06parentC3VarSivM [inherited]  // ParentPkgKlass.parentPkgVar.modify
// CHECK-NEXT:  #ParentPkgKlass.init!allocator: (ParentPkgKlass.Type) -> (Int) -> ParentPkgKlass : @$s3Lib8PkgKlassCyACSicfC [override]  // PkgKlass.__allocating_init(_:)
// CHECK-NEXT:  #ParentPkgKlass.parentPkgFunc: (ParentPkgKlass) -> () -> Int : @$s3Lib8PkgKlassC06parentB4FuncSiyF [override]  // PkgKlass.parentPkgFunc()
// CHECK-NEXT:  #PkgKlass.pkgVar!getter: (PkgKlass) -> () -> String : @$s3Lib8PkgKlassC6pkgVarSSvg  // PkgKlass.pkgVar.getter
// CHECK-NEXT:  #PkgKlass.pkgVar!setter: (PkgKlass) -> (String) -> () : @$s3Lib8PkgKlassC6pkgVarSSvs  // PkgKlass.pkgVar.setter
// CHECK-NEXT:  #PkgKlass.pkgVar!modify: (PkgKlass) -> () -> () : @$s3Lib8PkgKlassC6pkgVarSSvM  // PkgKlass.pkgVar.modify
// CHECK-NEXT:  #PkgKlass.init!allocator: (PkgKlass.Type) -> (String) -> PkgKlass : @$s3Lib8PkgKlassCyACSScfC  // PkgKlass.__allocating_init(_:)
// CHECK-NEXT:  #PkgKlass.pkgFunc: (PkgKlass) -> () -> () : @$s3Lib8PkgKlassC7pkgFuncyyF  // PkgKlass.pkgFunc()
// CHECK-NEXT:  #PkgKlass.deinit!deallocator: @$s3Lib8PkgKlassCfD  // PkgKlass.__deallocating_deinit
// CHECK-NEXT:  #PkgKlass!ivardestroyer: @$s3Lib8PkgKlassCfE  // PkgKlass.__ivar_destroyer

// CHECK-LABEL: sil_vtable [serialized_for_package] PubKlassZ {
// CHECK-NEXT:  #PubKlassZ.env!getter: (PubKlassZ) -> () -> UInt16 : @$s3Lib9PubKlassZC3envs6UInt16Vvg  // PubKlassZ.env.getter
// CHECK-NEXT:  #PubKlassZ.env!setter: (PubKlassZ) -> (UInt16) -> () : @$s3Lib9PubKlassZC3envs6UInt16Vvs  // PubKlassZ.env.setter
// CHECK-NEXT:  #PubKlassZ.env!modify: (PubKlassZ) -> () -> () : @$s3Lib9PubKlassZC3envs6UInt16VvM  // PubKlassZ.env.modify
// CHECK-NEXT:  #PubKlassZ.init!allocator: (PubKlassZ.Type) -> (UInt16) -> PubKlassZ : @$s3Lib9PubKlassZC8rawValueACs6UInt16V_tcfC  // PubKlassZ.__allocating_init(rawValue:)
// CHECK-NEXT:  #PubKlassZ.pubFunc: (PubKlassZ) -> () -> () : @$s3Lib9PubKlassZC7pubFuncyyF // PubKlassZ.pubFunc()
// CHECK-NEXT:  #PubKlassZ.deinit!deallocator: @$s3Lib9PubKlassZCfD // PubKlassZ.__deallocating_deinit


// CHECK-LABEL: sil_vtable [serialized_for_package] PkgKlassZ {
// CHECK-NEXT:  #PkgKlassZ.env!getter: (PkgKlassZ) -> () -> UInt16 : @$s3Lib9PkgKlassZC3envs6UInt16Vvg  // PkgKlassZ.env.getter
// CHECK-NEXT:  #PkgKlassZ.env!setter: (PkgKlassZ) -> (UInt16) -> () : @$s3Lib9PkgKlassZC3envs6UInt16Vvs  // PkgKlassZ.env.setter
// CHECK-NEXT:  #PkgKlassZ.env!modify: (PkgKlassZ) -> () -> () : @$s3Lib9PkgKlassZC3envs6UInt16VvM  // PkgKlassZ.env.modify
// CHECK-NEXT:  #PkgKlassZ.init!allocator: (PkgKlassZ.Type) -> (UInt16) -> PkgKlassZ : @$s3Lib9PkgKlassZC8rawValueACs6UInt16V_tcfC  // PkgKlassZ.__allocating_init(rawValue:)
// CHECK-NEXT:  #PkgKlassZ.pkgFunc: (PkgKlassZ) -> () -> () : @$s3Lib9PkgKlassZC7pkgFuncyyF // PkgKlassZ.pkgFunc()
// CHECK-NEXT:  #PkgKlassZ.deinit!deallocator: @$s3Lib9PkgKlassZCfD // PkgKlassZ.__deallocating_deinit


//CHECK-LABEL:  sil_witness_table [serialized_for_package] PubKlassZ: PubProto module Lib {
//CHECK-NEXT:   associated_type Element: PubKlassZ
//CHECK-NEXT:   method #PubProto.root!getter: <Self where Self : PubProto> (Self.Type) -> () -> UInt16 : @$s3Lib9PubKlassZCAA0B5ProtoA2aDP4roots6UInt16VvgZTW
//CHECK-NEXT:   method #PubProto.env!getter: <Self where Self : PubProto> (Self) -> () -> UInt16 : @$s3Lib9PubKlassZCAA0B5ProtoA2aDP3envs6UInt16VvgTW
//CHECK-NEXT:   method #PubProto.env!setter: <Self where Self : PubProto> (inout Self) -> (UInt16) -> () : @$s3Lib9PubKlassZCAA0B5ProtoA2aDP3envs6UInt16VvsTW
//CHECK-NEXT:   method #PubProto.env!modify: <Self where Self : PubProto> (inout Self) -> () -> () : @$s3Lib9PubKlassZCAA0B5ProtoA2aDP3envs6UInt16VvMTW
//CHECK-NEXT:   method #PubProto.init!allocator: <Self where Self : PubProto> (Self.Type) -> (UInt16) -> Self : @$s3Lib9PubKlassZCAA0B5ProtoA2aDP8rawValuexs6UInt16V_tcfCTW
//CHECK-NEXT:   method #PubProto.pubFunc: <Self where Self : PubProto> (Self) -> () -> () : @$s3Lib9PubKlassZCAA0B5ProtoA2aDP7pubFuncyyFTW

//CHECK-LABEL:  sil_witness_table [serialized_for_package] PubStruct: PubProto module Lib {
//CHECK-NEXT:   associated_type Element: PubStruct
//CHECK-NEXT:   method #PubProto.root!getter: <Self where Self : PubProto> (Self.Type) -> () -> UInt16 : @$s3Lib9PubStructVAA0B5ProtoA2aDP4roots6UInt16VvgZTW
//CHECK-NEXT:   method #PubProto.env!getter: <Self where Self : PubProto> (Self) -> () -> UInt16 : @$s3Lib9PubStructVAA0B5ProtoA2aDP3envs6UInt16VvgTW
//CHECK-NEXT:   method #PubProto.env!setter: <Self where Self : PubProto> (inout Self) -> (UInt16) -> () : @$s3Lib9PubStructVAA0B5ProtoA2aDP3envs6UInt16VvsTW
//CHECK-NEXT:   method #PubProto.env!modify: <Self where Self : PubProto> (inout Self) -> () -> () : @$s3Lib9PubStructVAA0B5ProtoA2aDP3envs6UInt16VvMTW
//CHECK-NEXT:   method #PubProto.init!allocator: <Self where Self : PubProto> (Self.Type) -> (UInt16) -> Self : @$s3Lib9PubStructVAA0B5ProtoA2aDP8rawValuexs6UInt16V_tcfCTW
//CHECK-NEXT:   method #PubProto.pubFunc: <Self where Self : PubProto> (Self) -> () -> () : @$s3Lib9PubStructVAA0B5ProtoA2aDP7pubFuncyyFTW

//CHECK-LABEL:  sil_witness_table [serialized_for_package] PubStructX: PubSimpleProto module Lib {
//CHECK-NEXT:   method #PubSimpleProto.pubVar!getter: <Self where Self : PubSimpleProto> (Self) -> () -> Int : @$s3Lib10PubStructXVAA0B11SimpleProtoA2aDP6pubVarSivgTW
//CHECK-NEXT:   method #PubSimpleProto.pubVar!setter: <Self where Self : PubSimpleProto> (inout Self) -> (Int) -> () : @$s3Lib10PubStructXVAA0B11SimpleProtoA2aDP6pubVarSivsTW
//CHECK-NEXT:   method #PubSimpleProto.pubVar!modify: <Self where Self : PubSimpleProto> (inout Self) -> () -> () : @$s3Lib10PubStructXVAA0B11SimpleProtoA2aDP6pubVarSivMTW
//CHECK-NEXT:   method #PubSimpleProto.pubFunc: <Self where Self : PubSimpleProto> (Self) -> () -> Int : @$s3Lib10PubStructXVAA0B11SimpleProtoA2aDP7pubFuncSiyFTW

//CHECK-LABEL:  sil_witness_table package [serialized_for_package] PkgKlassZ: PkgProto module Lib {
//CHECK-NEXT:   associated_type Element: PkgKlassZ
//CHECK-NEXT:   method #PkgProto.root!getter: <Self where Self : PkgProto> (Self.Type) -> () -> UInt16 : @$s3Lib9PkgKlassZCAA0B5ProtoA2aDP4roots6UInt16VvgZTW
//CHECK-NEXT:   method #PkgProto.env!getter: <Self where Self : PkgProto> (Self) -> () -> UInt16 : @$s3Lib9PkgKlassZCAA0B5ProtoA2aDP3envs6UInt16VvgTW
//CHECK-NEXT:   method #PkgProto.env!setter: <Self where Self : PkgProto> (inout Self) -> (UInt16) -> () : @$s3Lib9PkgKlassZCAA0B5ProtoA2aDP3envs6UInt16VvsTW
//CHECK-NEXT:   method #PkgProto.env!modify: <Self where Self : PkgProto> (inout Self) -> () -> () : @$s3Lib9PkgKlassZCAA0B5ProtoA2aDP3envs6UInt16VvMTW
//CHECK-NEXT:   method #PkgProto.init!allocator: <Self where Self : PkgProto> (Self.Type) -> (UInt16) -> Self : @$s3Lib9PkgKlassZCAA0B5ProtoA2aDP8rawValuexs6UInt16V_tcfCTW
//CHECK-NEXT:   method #PkgProto.pkgFunc: <Self where Self : PkgProto> (Self) -> () -> () : @$s3Lib9PkgKlassZCAA0B5ProtoA2aDP7pkgFuncyyFTW

//CHECK-LABEL:  sil_witness_table package [serialized_for_package] PkgStruct: PkgProto module Lib {
//CHECK-NEXT:   associated_type Element: PkgStruct
//CHECK-NEXT:  method #PkgProto.root!getter: <Self where Self : PkgProto> (Self.Type) -> () -> UInt16 : @$s3Lib9PkgStructVAA0B5ProtoA2aDP4roots6UInt16VvgZTW
//CHECK-NEXT:   method #PkgProto.env!getter: <Self where Self : PkgProto> (Self) -> () -> UInt16 : @$s3Lib9PkgStructVAA0B5ProtoA2aDP3envs6UInt16VvgTW
//CHECK-NEXT:   method #PkgProto.env!setter: <Self where Self : PkgProto> (inout Self) -> (UInt16) -> () : @$s3Lib9PkgStructVAA0B5ProtoA2aDP3envs6UInt16VvsTW
//CHECK-NEXT:   method #PkgProto.env!modify: <Self where Self : PkgProto> (inout Self) -> () -> () : @$s3Lib9PkgStructVAA0B5ProtoA2aDP3envs6UInt16VvMTW
//CHECK-NEXT:   method #PkgProto.init!allocator: <Self where Self : PkgProto> (Self.Type) -> (UInt16) -> Self : @$s3Lib9PkgStructVAA0B5ProtoA2aDP8rawValuexs6UInt16V_tcfCTW
//CHECK-NEXT:   method #PkgProto.pkgFunc: <Self where Self : PkgProto> (Self) -> () -> () : @$s3Lib9PkgStructVAA0B5ProtoA2aDP7pkgFuncyyFTW

//CHECK-LABEL:  sil_witness_table package [serialized_for_package] PkgStructX: PkgSimpleProto module Lib {
//CHECK-NEXT:   method #PkgSimpleProto.pkgVar!getter: <Self where Self : PkgSimpleProto> (Self) -> () -> Int : @$s3Lib10PkgStructXVAA0B11SimpleProtoA2aDP6pkgVarSivgTW 
//CHECK-NEXT:   method #PkgSimpleProto.pkgVar!setter: <Self where Self : PkgSimpleProto> (inout Self) -> (Int) -> () : @$s3Lib10PkgStructXVAA0B11SimpleProtoA2aDP6pkgVarSivsTW
//CHECK-NEXT:   method #PkgSimpleProto.pkgVar!modify: <Self where Self : PkgSimpleProto> (inout Self) -> () -> () : @$s3Lib10PkgStructXVAA0B11SimpleProtoA2aDP6pkgVarSivMTW
//CHECK-NEXT:   method #PkgSimpleProto.pkgFunc: <Self where Self : PkgSimpleProto> (Self) -> () -> Int : @$s3Lib10PkgStructXVAA0B11SimpleProtoA2aDP7pkgFuncSiyFTW
