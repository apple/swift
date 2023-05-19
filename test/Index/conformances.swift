// RUN: %target-swift-ide-test -print-indexed-symbols -source-filename %s | %FileCheck %s

protocol P1 { // CHECK: [[@LINE]]:10 | protocol/Swift | P1 | [[P1_USR:.*]] | Def |
  func foo() // CHECK: [[@LINE]]:8 | instance-method/Swift | foo() | [[P1_foo_USR:.*]] | Def
}

struct DirectConf: P1 { // CHECK: [[@LINE]]:8 | struct/Swift | DirectConf | [[DirectConf_USR:.*]] | Def
  func foo() {} // CHECK: [[@LINE]]:8 | instance-method/Swift | foo() | [[DirectConf_foo_USR:.*]] | Def,RelChild,RelOver | rel: 2
    // CHECK-NEXT: RelOver | instance-method/Swift | foo() | [[P1_foo_USR]]
    // CHECK-NEXT: RelChild | struct/Swift | DirectConf | [[DirectConf_USR]]
}

struct ConfFromExtension {}
extension ConfFromExtension: P1 { // CHECK: [[@LINE]]:11 | extension/ext-struct/Swift | ConfFromExtension | [[ConfFromExtension_ext_USR:.*]] | Def
  func foo() {} // CHECK: [[@LINE]]:8 | instance-method/Swift | foo() | [[ConfFromExtension_ext_foo_USR:.*]] | Def,RelChild,RelOver | rel: 2
    // CHECK-NEXT: RelOver | instance-method/Swift | foo() | [[P1_foo_USR]]
    // CHECK-NEXT: RelChild | extension/ext-struct/Swift | ConfFromExtension | [[ConfFromExtension_ext_USR]]
}

struct ImplicitConfFromExtension { // CHECK: [[@LINE]]:8 | struct/Swift | ImplicitConfFromExtension | [[ImplicitConfFromExtension_USR:.*]] | Def
  func foo() {} // CHECK: [[@LINE]]:8 | instance-method/Swift | foo() | [[ImplicitConfFromExtension_foo_USR:.*]] | Def,RelChild | rel: 1
    // CHECK-NEXT: RelChild | struct/Swift | ImplicitConfFromExtension | [[ImplicitConfFromExtension_USR]]
}
extension ImplicitConfFromExtension: P1 { // CHECK: [[@LINE]]:11 | extension/ext-struct/Swift | ImplicitConfFromExtension | [[ImplicitConfFromExtension_USR:.*]] | Def
  // CHECK: [[@LINE-1]]:11 | instance-method/Swift | foo() | [[ImplicitConfFromExtension_foo_USR]] | Impl,RelOver,RelCont | rel: 2
  // CHECK-NEXT: RelOver | instance-method/Swift | foo() | [[P1_foo_USR]]
  // CHECK-NEXT: RelCont | extension/ext-struct/Swift | ImplicitConfFromExtension | [[ImplicitConfFromExtension_USR]]
}

class BaseConfFromBase { // CHECK: [[@LINE]]:7 | class/Swift | BaseConfFromBase | [[BaseConfFromBase_USR:.*]] | Def
  func foo() {} // CHECK: [[@LINE]]:8 | instance-method/Swift | foo() | [[BaseConfFromBase_foo_USR:.*]] | Def,Dyn,RelChild | rel: 1
    // CHECK-NEXT: RelChild | class/Swift | BaseConfFromBase | [[BaseConfFromBase_USR]]
}
class SubConfFromBase: BaseConfFromBase, P1 { // CHECK: [[@LINE]]:7 | class/Swift | SubConfFromBase | [[SubConfFromBase_USR:.*]] | Def
  // CHECK: [[@LINE-1]]:7 | instance-method/Swift | foo() | [[BaseConfFromBase_foo_USR]] | Impl,RelOver,RelCont | rel: 2
  // CHECK-NEXT: RelOver | instance-method/Swift | foo() | [[P1_foo_USR]]
  // CHECK-NEXT: RelCont | class/Swift | SubConfFromBase | [[SubConfFromBase_USR]]
}

protocol P2 { // CHECK: [[@LINE]]:10 | protocol/Swift | P2 | [[P2_USR:.*]] | Def |
  func foo() // CHECK: [[@LINE]]:8 | instance-method/Swift | foo() | [[P2_foo_USR:.*]] | Def
}
extension P2 { // CHECK: [[@LINE]]:11 | extension/ext-protocol/Swift | P2 | [[P2_ext_USR:.*]] | Def
  func foo() {} // CHECK: [[@LINE]]:8 | instance-method/Swift | foo() | [[P2_ext_foo_USR:.*]] | Def,Dyn,RelChild,RelOver | rel: 2
    // CHECK-NEXT: RelOver | instance-method/Swift | foo() | [[P2_foo_USR]]
    // CHECK-NEXT: RelChild | extension/ext-protocol/Swift | P2 | [[P2_ext_USR]]
}

struct ConfFromDefaultImpl: P2 { // CHECK: [[@LINE]]:8 | struct/Swift | ConfFromDefaultImpl | [[ConfFromDefaultImpl_USR:.*]] | Def
  // CHECK: [[@LINE-1]]:8 | instance-method/Swift | foo() | [[P2_ext_foo_USR]] | Impl,RelOver,RelCont | rel: 2
  // CHECK-NEXT: RelOver | instance-method/Swift | foo() | [[P2_foo_USR]]
  // CHECK-NEXT: RelCont | struct/Swift | ConfFromDefaultImpl | [[ConfFromDefaultImpl_USR]]
}

protocol P3 {
  func meth1() // CHECK: [[@LINE]]:8 | instance-method/Swift | meth1() | [[P3_meth1_USR:.*]] | Def
  func meth2() // CHECK: [[@LINE]]:8 | instance-method/Swift | meth2() | [[P3_meth2_USR:.*]] | Def
}

class BaseMultiConf { // CHECK: [[@LINE]]:7 | class/Swift | BaseMultiConf | [[BaseMultiConf_USR:.*]] | Def
  func meth2() {} // CHECK: [[@LINE]]:8 | instance-method/Swift | meth2() | [[BaseMultiConf_meth2_USR:.*]] | Def
}
extension SubMultiConf {
  func meth1() {} // CHECK: [[@LINE]]:8 | instance-method/Swift | meth1() | [[SubMultiConf_ext_meth1_USR:.*]] | Def
}
class SubMultiConf: BaseMultiConf,P2,P1,P3 { // CHECK: [[@LINE]]:7 | class/Swift | SubMultiConf | [[SubMultiConf_USR:.*]] | Def
  // CHECK: [[@LINE-1]]:7 | instance-method/Swift | foo() | [[P2_ext_foo_USR]] | Impl,RelOver,RelCont | rel: 2
    // CHECK-NEXT: RelOver | instance-method/Swift | foo() | [[P2_foo_USR]]
    // CHECK-NEXT: RelCont | class/Swift | SubMultiConf | [[SubMultiConf_USR]]
  // CHECK: [[@LINE-4]]:7 | instance-method/Swift | foo() | [[P2_ext_foo_USR]] | Impl,RelOver,RelCont | rel: 2
    // CHECK-NEXT: RelOver | instance-method/Swift | foo() | [[P1_foo_USR]]
    // CHECK-NEXT: RelCont | class/Swift | SubMultiConf | [[SubMultiConf_USR]]
  // CHECK: [[@LINE-7]]:7 | instance-method/Swift | meth1() | [[SubMultiConf_ext_meth1_USR]] | Impl,RelOver,RelCont | rel: 2
    // CHECK-NEXT: RelOver | instance-method/Swift | meth1() | [[P3_meth1_USR]]
    // CHECK-NEXT: RelCont | class/Swift | SubMultiConf | [[SubMultiConf_USR]]
  // CHECK: [[@LINE-10]]:7 | instance-method/Swift | meth2() | [[BaseMultiConf_meth2_USR]] | Impl,RelOver,RelCont | rel: 2
    // CHECK-NEXT: RelOver | instance-method/Swift | meth2() | [[P3_meth2_USR]]
    // CHECK-NEXT: RelCont | class/Swift | SubMultiConf | [[SubMultiConf_USR]]
  // CHECK-NOT: [[@LINE-13]]:7 | instance-method
}

class CompositionType: BaseMultiConf & P1 { // CHECK: [[@LINE]]:7 | class/Swift | CompositionType | [[CompositionType_USR:.*]] | Def
  // CHECK: [[@LINE-1]]:24 | class/Swift | BaseMultiConf | [[BaseMultiConf_USR]] | Ref,RelBase | rel: 1
  // CHECK: [[@LINE-2]]:24 | protocol/Swift | P1 | [[P1_USR]] | Ref,RelBase | rel: 1
  func foo() {}
}

typealias CompositionTypeAlias = BaseMultiConf & P1 // CHECK: [[@LINE]]:11 | type-alias/Swift | CompositionTypeAlias | [[CompositionTypeAlias_USR:.*]] | Def
  // CHECK: [[@LINE-1]]:34 | class/Swift | BaseMultiConf | [[BaseMultiConf_USR]] | Ref | rel: 0
  // CHECK: [[@LINE-2]]:50 | protocol/Swift | P1 | [[P1_USR]] | Ref | rel: 0

class CompositionTypeViaAlias: CompositionTypeAlias { // CHECK: [[@LINE]]:7 | class/Swift | CompositionTypeViaAlias | [[CompositionTypeViaAlias_USR:.*]] | Def
  // CHECK: [[@LINE-1]]:32 | type-alias/Swift | CompositionTypeAlias | [[CompositionTypeAlias_USR]] | Ref | rel: 0
  // CHECK: [[@LINE-2]]:32 | class/Swift | BaseMultiConf | [[BaseMultiConf_USR]] | Ref,Impl,RelBase | rel: 1
  // CHECK: [[@LINE-3]]:32 | protocol/Swift | P1 | [[P1_USR]] | Ref,Impl,RelBase | rel: 1
  func foo() {}
}

typealias NestedCompositionTypeAlias = CompositionTypeAlias & P2 // CHECK: [[@LINE]]:11 | type-alias/Swift | NestedCompositionTypeAlias | [[NestedCompositionTypeAlias_USR:.*]] | Def
  // CHECK: [[@LINE-1]]:40 | type-alias/Swift | CompositionTypeAlias | [[CompositionTypeAlias_USR]] | Ref | rel: 0
  // CHECK: [[@LINE-2]]:63 | protocol/Swift | P2 | [[P2_USR]] | Ref | rel: 0

class CompositionViaNestedAlias: NestedCompositionTypeAlias { // CHECK: [[@LINE]]:7 | class/Swift | CompositionViaNestedAlias | [[CompositionViaNestedAlias_USR:.*]] | Def
  // CHECK: [[@LINE-1]]:34 | type-alias/Swift | NestedCompositionTypeAlias | [[NestedCompositionTypeAlias_USR]] | Ref | rel: 0
  // CHECK: [[@LINE-2]]:34 | class/Swift | BaseMultiConf | [[BaseMultiConf_USR]] | Ref,Impl,RelBase | rel: 1
  // CHECK: [[@LINE-3]]:34 | protocol/Swift | P1 | [[P1_USR]] | Ref,Impl,RelBase | rel: 1
  // CHECK: [[@LINE-4]]:34 | protocol/Swift | P2 | [[P2_USR]] | Ref,Impl,RelBase | rel: 1
  func foo() {}
}

typealias ProtocolsOnly = P1 & P2 // CHECK: [[@LINE]]:11 | type-alias/Swift | ProtocolsOnly | [[ProtocolsOnly_USR:.*]] | Def
  // CHECK: [[@LINE-1]]:27 | protocol/Swift | P1 | [[P1_USR]] | Ref | rel: 0
  // CHECK: [[@LINE-2]]:32 | protocol/Swift | P2 | [[P2_USR]] | Ref | rel: 0

class NoInherited {} // CHECK: [[@LINE]]:7 | class/Swift | NoInherited | [[NoInherited_USR:.*]] | Def
extension NoInherited: ProtocolsOnly { // CHECK: [[@LINE]]:11 | class/Swift | NoInherited | [[NoInherited_USR:.*]] | Ref
  // CHECK: [[@LINE-1]]:24 | type-alias/Swift | ProtocolsOnly | [[ProtocolsOnly_USR]] | Ref | rel: 0
  // CHECK: [[@LINE-2]]:24 | protocol/Swift | P1 | [[P1_USR]] | Ref,Impl,RelBase | rel: 1
  // CHECK: [[@LINE-3]]:24 | protocol/Swift | P2 | [[P2_USR]] | Ref,Impl,RelBase | rel: 1
  func foo() {}
}

struct WithCodable: Codable {} // CHECK: [[@LINE]]:21 | type-alias/Swift | Codable | [[Codable_USR:.*]] | Ref | rel: 0

protocol InheritingP: P1 { // CHECK: [[@LINE]]:10 | protocol/Swift | InheritingP | [[InheritingP_USR:.*]] | Def
  func foo() // CHECK: [[@LINE]]:8 | instance-method/Swift | foo() | [[InheritingP_foo_USR:.*]] | Def,Dyn,RelChild,RelOver | rel: 2
    // CHECK-NEXT: RelOver | instance-method/Swift | foo() | s:14swift_ide_test2P1P3fooyyF
    // CHECK-NEXT: RelChild | protocol/Swift | InheritingP | [[InheritingP_USR]]
}

struct DirectConf2: InheritingP { // CHECK: [[@LINE]]:8 | struct/Swift | DirectConf2 | [[DirectConf2_USR:.*]] | Def
  // FIXME: Should only override InheritingP.foo()
  func foo() {} // CHECK: [[@LINE]]:8 | instance-method/Swift | foo() | [[DirectConf2_foo_USR:.*]] | Def,RelChild,RelOver | rel: 3
    // CHECK-NEXT: RelOver | instance-method/Swift | foo() | [[InheritingP_foo_USR]]
    // CHECK-NEXT: RelOver | instance-method/Swift | foo() | [[P1_foo_USR]]
    // CHECK-NEXT: RelChild | struct/Swift | DirectConf2 | [[DirectConf2_USR]]
}

extension InheritingP { // CHECK: [[@LINE]]:11 | extension/ext-protocol/Swift | InheritingP | [[InheritingP_USR:.*]] | Def
  func foo() {} // CHECK: [[@LINE]]:8 | instance-method/Swift | foo() | [[InheritingP_ext_foo_USR:.*]] | Def,Dyn,RelChild,RelOver | rel: 2
    // CHECK-NEXT: RelOver | instance-method/Swift | foo() | [[InheritingP_foo_USR]]
    // CHECK-NEXT: RelChild | extension/ext-protocol/Swift | InheritingP | [[InheritingP_USR]]
}

protocol WithAssocType {
  associatedtype T // CHECK: [[@LINE]]:18 | type-alias/associated-type/Swift | T | [[WithAssocT_USR:.*]] | Def
  func foo() -> T // CHECK: [[@LINE]]:17 | type-alias/associated-type/Swift | T | [[WithAssocT_USR]] | Ref
}

struct SAssocTypeAlias: WithAssocType {
  typealias T = Int // CHECK: [[@LINE]]:13 | type-alias/Swift | T | [[SAssocT:.*]] | Def,RelChild,RelOver | rel: 2
    // CHECK-NEXT: RelOver | type-alias/associated-type/Swift | T | [[WithAssocT_USR]]
    // CHECK-NEXT: RelChild | struct/Swift | SAssocTypeAlias
  func foo() -> T { return 0 } // CHECK: [[@LINE]]:17 | type-alias/Swift | T | [[SAssocT:.*]] | Ref
}

struct SAssocTypeInferred: WithAssocType {
  func foo() -> Int { return 1 }
  func bar() -> T { return 2 } // CHECK: [[@LINE]]:17 |  type-alias/associated-type/Swift | T | [[WithAssocT_USR]] | Ref
}

struct AssocViaExtension {
  struct T {} // CHECK: [[@LINE]]:10 | struct/Swift | T | [[AssocViaExtensionT_USR:.*]] | Def
  func foo() -> T { return T() }
}

extension AssocViaExtension: WithAssocType {} // CHECK: [[@LINE]]:11 | struct/Swift | T | [[AssocViaExtensionT_USR]] | Impl,RelOver,RelCont | rel: 2
  // CHECK-NEXT: RelOver | type-alias/associated-type/Swift | T | [[WithAssocT_USR]]
  // CHECK-NEXT: RelCont | extension/ext-struct/Swift | AssocViaExtension

func returnOpaqueResultType() -> some BaseConfFromBase & P1 & WithAssocType {} // CHECK: [[@LINE]]:6 | function/Swift | returnOpaqueResultType() | s:14swift_ide_test22returnOpaqueResultTypeQryF | Def | rel: 0
// CHECK: [[@LINE-1]]:39 | class/Swift | BaseConfFromBase | s:14swift_ide_test012BaseConfFromD0C | Ref,RelCont | rel: 1
// CHECK-NEXT:   RelCont | function/Swift | returnOpaqueResultType() | s:14swift_ide_test22returnOpaqueResultTypeQryF
// CHECK: [[@LINE-3]]:58 | protocol/Swift | P1 | s:14swift_ide_test2P1P | Ref,RelCont | rel: 1
// CHECK-NEXT:   RelCont | function/Swift | returnOpaqueResultType() | s:14swift_ide_test22returnOpaqueResultTypeQryF
// CHECK: [[@LINE-5]]:63 | protocol/Swift | WithAssocType | s:14swift_ide_test13WithAssocTypeP | Ref,RelCont | rel: 1
// CHECK-NEXT:   RelCont | function/Swift | returnOpaqueResultType() | s:14swift_ide_test22returnOpaqueResultTypeQryF
