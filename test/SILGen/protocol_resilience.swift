
// RUN: %empty-directory(%t)
// RUN: %target-swift-frontend -module-name protocol_resilience -emit-module -enable-library-evolution -emit-module-path=%t/resilient_protocol.swiftmodule -module-name=resilient_protocol %S/../Inputs/resilient_protocol.swift
// RUN: %target-swift-emit-silgen -module-name protocol_resilience -I %t -enable-library-evolution %s | %FileCheck %s

import resilient_protocol

prefix operator ~~~
infix operator <*>
infix operator <**>
infix operator <===>

public protocol P {}


// Protocol is public -- needs resilient witness table
public protocol ResilientMethods {
  associatedtype AssocType : P

  func defaultWitness()
  func anotherDefaultWitness(_ x: Int) -> Self
  func defaultWitnessWithAssociatedType(_ a: AssocType)
  func defaultWitnessMoreAbstractThanRequirement(_ a: AssocType, b: Int)
  func defaultWitnessMoreAbstractThanGenericRequirement<T>(_ a: AssocType, t: T)

  func noDefaultWitness()
  func defaultWitnessIsNotPublic()

  static func staticDefaultWitness(_ x: Int) -> Self
}

extension ResilientMethods {

// CHECK-LABEL: sil private [transparent] [thunk] [ossa] @$s19protocol_resilience16ResilientMethodsP14defaultWitnessyyF
// CHECK-LABEL: sil [ossa] @$s19protocol_resilience16ResilientMethodsPAAE14defaultWitnessyyF
  public func defaultWitness() {}

// CHECK-LABEL: sil private [transparent] [thunk] [ossa] @$s19protocol_resilience16ResilientMethodsP21anotherDefaultWitnessyxSiF
// CHECK-LABEL: sil [ossa] @$s19protocol_resilience16ResilientMethodsPAAE21anotherDefaultWitnessyxSiF
  public func anotherDefaultWitness(_ x: Int) -> Self {}

// CHECK-LABEL: sil private [transparent] [thunk] [ossa] @$s19protocol_resilience16ResilientMethodsP32defaultWitnessWithAssociatedTypeyy05AssocI0QzF
// CHECK-LABEL: sil [ossa] @$s19protocol_resilience16ResilientMethodsPAAE32defaultWitnessWithAssociatedTypeyy05AssocI0QzF
  public func defaultWitnessWithAssociatedType(_ a: AssocType) {}

// CHECK-LABEL: sil private [transparent] [thunk] [ossa] @$s19protocol_resilience16ResilientMethodsP41defaultWitnessMoreAbstractThanRequirement_1by9AssocTypeQz_SitF
// CHECK-LABEL: sil [ossa] @$s19protocol_resilience16ResilientMethodsPAAE41defaultWitnessMoreAbstractThanRequirement_1byqd___qd_0_tr0_lF
  public func defaultWitnessMoreAbstractThanRequirement<A, T>(_ a: A, b: T) {}

// CHECK-LABEL: sil private [transparent] [thunk] [ossa] @$s19protocol_resilience16ResilientMethodsP48defaultWitnessMoreAbstractThanGenericRequirement_1ty9AssocTypeQz_qd__tlF
// CHECK-LABEL: sil [ossa] @$s19protocol_resilience16ResilientMethodsPAAE48defaultWitnessMoreAbstractThanGenericRequirement_1tyqd___qd_0_tr0_lF
  public func defaultWitnessMoreAbstractThanGenericRequirement<A, T>(_ a: A, t: T) {}

// CHECK-LABEL: sil private [transparent] [thunk] [ossa] @$s19protocol_resilience16ResilientMethodsP20staticDefaultWitnessyxSiFZ
// CHECK-LABEL: sil [ossa] @$s19protocol_resilience16ResilientMethodsPAAE20staticDefaultWitnessyxSiFZ
  public static func staticDefaultWitness(_ x: Int) -> Self {}

// CHECK-LABEL: sil private [ossa] @$s19protocol_resilience16ResilientMethodsPAAE25defaultWitnessIsNotPublic{{.*}}F
  private func defaultWitnessIsNotPublic() {}

}


public protocol ResilientConstructors {
  init(noDefault: ())
  init(default: ())
  init?(defaultIsOptional: ())
  init?(defaultNotOptional: ())
  init(optionalityMismatch: ())
}

extension ResilientConstructors {

// CHECK-LABEL: sil private [transparent] [thunk] [ossa] @$s19protocol_resilience21ResilientConstructorsP7defaultxyt_tcfC
// CHECK-LABEL: sil [ossa] @$s19protocol_resilience21ResilientConstructorsPAAE7defaultxyt_tcfC
  public init(default: ()) {
    self.init(noDefault: ())
  }

// CHECK-LABEL: sil private [transparent] [thunk] [ossa] @$s19protocol_resilience21ResilientConstructorsP17defaultIsOptionalxSgyt_tcfC
// CHECK-LABEL: sil [ossa] @$s19protocol_resilience21ResilientConstructorsPAAE17defaultIsOptionalxSgyt_tcfC
  public init?(defaultIsOptional: ()) {
    self.init(noDefault: ())
  }

// CHECK-LABEL: sil [ossa] @$s19protocol_resilience21ResilientConstructorsPAAE20defaultIsNotOptionalxyt_tcfC
  public init(defaultIsNotOptional: ()) {
    self.init(noDefault: ())
  }

// CHECK-LABEL: sil [ossa] @$s19protocol_resilience21ResilientConstructorsPAAE19optionalityMismatchxSgyt_tcfC
  public init?(optionalityMismatch: ()) {
    self.init(noDefault: ())
  }
}


public protocol ResilientStorage {
  associatedtype T : ResilientConstructors

  var propertyWithDefault: Int { get }
  var propertyWithNoDefault: Int { get }
  var mutablePropertyWithDefault: Int { get set }
  var mutablePropertyNoDefault: Int { get set }
  var mutableGenericPropertyWithDefault: T { get set }

  subscript(x: T) -> T { get set }

  var mutatingGetterWithNonMutatingDefault: Int { mutating get set }
}

extension ResilientStorage {

// CHECK-LABEL: sil private [transparent] [thunk] [ossa] @$s19protocol_resilience16ResilientStorageP19propertyWithDefaultSivg
// CHECK-LABEL: sil [ossa] @$s19protocol_resilience16ResilientStoragePAAE19propertyWithDefaultSivg
  public var propertyWithDefault: Int {
    get { return 0 }
  }

// CHECK-LABEL: sil private [transparent] [thunk] [ossa] @$s19protocol_resilience16ResilientStorageP26mutablePropertyWithDefaultSivg
// CHECK-LABEL: sil [ossa] @$s19protocol_resilience16ResilientStoragePAAE26mutablePropertyWithDefaultSivg
// CHECK-LABEL: sil private [transparent] [thunk] [ossa] @$s19protocol_resilience16ResilientStorageP26mutablePropertyWithDefaultSivs
// CHECK-LABEL: sil [ossa] @$s19protocol_resilience16ResilientStoragePAAE26mutablePropertyWithDefaultSivs
// CHECK-LABEL: sil private [transparent] [thunk] [ossa] @$s19protocol_resilience16ResilientStorageP26mutablePropertyWithDefaultSivM
  public var mutablePropertyWithDefault: Int {
    get { return 0 }
    set { }
  }

  public private(set) var mutablePropertyNoDefault: Int {
    get { return 0 }
    set { }
  }

// CHECK-LABEL: sil private [transparent] [thunk] [ossa] @$s19protocol_resilience16ResilientStorageP33mutableGenericPropertyWithDefault1TQzvg
// CHECK-LABEL: sil [ossa] @$s19protocol_resilience16ResilientStoragePAAE33mutableGenericPropertyWithDefault1TQzvg
// CHECK-LABEL: sil private [transparent] [thunk] [ossa] @$s19protocol_resilience16ResilientStorageP33mutableGenericPropertyWithDefault1TQzvs
// CHECK-LABEL: sil [ossa] @$s19protocol_resilience16ResilientStoragePAAE33mutableGenericPropertyWithDefault1TQzvs
// CHECK-LABEL: sil private [transparent] [thunk] [ossa] @$s19protocol_resilience16ResilientStorageP33mutableGenericPropertyWithDefault1TQzvM
  public var mutableGenericPropertyWithDefault: T {
    get {
      return T(default: ())
    }
    set { }
  }

// CHECK-LABEL: sil private [transparent] [thunk] [ossa] @$s19protocol_resilience16ResilientStoragePy1TQzAEcig
// CHECK-LABEL: sil [ossa] @$s19protocol_resilience16ResilientStoragePAAEy1TQzAEcig
// CHECK-LABEL: sil private [transparent] [thunk] [ossa] @$s19protocol_resilience16ResilientStoragePy1TQzAEcis
// CHECK-LABEL: sil [ossa] @$s19protocol_resilience16ResilientStoragePAAEy1TQzAEcis
// CHECK-LABEL: sil private [transparent] [thunk] [ossa] @$s19protocol_resilience16ResilientStoragePy1TQzAEciM
  public subscript(x: T) -> T {
    get {
      return x
    }
    set { }
  }

// CHECK-LABEL: sil private [transparent] [thunk] [ossa] @$s19protocol_resilience16ResilientStorageP36mutatingGetterWithNonMutatingDefaultSivg
// CHECK-LABEL: sil [ossa] @$s19protocol_resilience16ResilientStoragePAAE36mutatingGetterWithNonMutatingDefaultSivg
// CHECK-LABEL: sil private [transparent] [thunk] [ossa] @$s19protocol_resilience16ResilientStorageP36mutatingGetterWithNonMutatingDefaultSivs
// CHECK-LABEL: sil [ossa] @$s19protocol_resilience16ResilientStoragePAAE36mutatingGetterWithNonMutatingDefaultSivs
// CHECK-LABEL: sil private [transparent] [thunk] [ossa] @$s19protocol_resilience16ResilientStorageP36mutatingGetterWithNonMutatingDefaultSivM
  public var mutatingGetterWithNonMutatingDefault: Int {
    get {
      return 0
    }
    set { }
  }
}


public protocol ResilientOperators {
  associatedtype AssocType : P

  static prefix func ~~~(s: Self)
  static func <*><T>(s: Self, t: T)
  static func <**><T>(t: T, s: Self) -> AssocType
  static func <===><T : ResilientOperators>(t: T, s: Self) -> T.AssocType
}

// CHECK-LABEL: sil private [transparent] [thunk] [ossa] @$s19protocol_resilience18ResilientOperatorsP3tttopyyxFZ
// CHECK-LABEL: sil [ossa] @$s19protocol_resilience3tttopyyxlF
public prefix func ~~~<S>(s: S) {}

// CHECK-LABEL: sil private [transparent] [thunk] [ossa] @$s19protocol_resilience18ResilientOperatorsP3lmgoiyyx_qd__tlFZ
// CHECK-LABEL: sil [ossa] @$s19protocol_resilience3lmgoiyyq__xtr0_lF
public func <*><T, S>(s: S, t: T) {}

// Swap the generic parameters to make sure we don't mix up our DeclContexts
// when mapping interface types in and out

// CHECK-LABEL: sil private [transparent] [thunk] [ossa] @$s19protocol_resilience18ResilientOperatorsP4lmmgoiy9AssocTypeQzqd___xtlFZ
// CHECK-LABEL: sil [ossa] @$s19protocol_resilience4lmmgoiy9AssocTypeQzq__xtAA18ResilientOperatorsRzr0_lF
public func <**><S : ResilientOperators, T>(t: T, s: S) -> S.AssocType {}

// CHECK-LABEL: sil private [transparent] [thunk] [ossa] @$s19protocol_resilience18ResilientOperatorsP5leeegoiy9AssocTypeQyd__qd___xtAaBRd__lFZ
// CHECK-LABEL: sil [ossa] @$s19protocol_resilience5leeegoiy9AssocTypeQzx_q_tAA18ResilientOperatorsRzAaER_r0_lF
public func <===><T : ResilientOperators, S : ResilientOperators>(t: T, s: S) -> T.AssocType {}


public protocol ReabstractSelfBase {
  // No requirements
}

public protocol ReabstractSelfRefined : class, ReabstractSelfBase {
  // A requirement with 'Self' abstracted as a class instance
  var callback: (Self) -> Self { get set }
}

func id<T>(_ t: T) -> T {}

extension ReabstractSelfBase {
  // A witness for the above requirement, but with 'Self' maximally abstracted
  public var callback: (Self) -> Self {
    get { return id }
    nonmutating set { }
  }
}

// CHECK-LABEL: sil private [transparent] [thunk] [ossa] @$s19protocol_resilience21ReabstractSelfRefinedP8callbackyxxcvg :
// CHECK: [[SELF_BOX:%.*]] = alloc_stack $τ_0_0
// CHECK-NEXT: [[SELF_BOX_BORROW:%.*]] = store_borrow %0 to [[SELF_BOX]]
// CHECK: [[WITNESS:%.*]] = function_ref @$s19protocol_resilience18ReabstractSelfBasePAAE8callbackyxxcvg
// CHECK-NEXT: [[RESULT:%.*]] = apply [[WITNESS]]<τ_0_0>([[SELF_BOX_BORROW]])
// CHECK-NEXT: [[RESULT_CONV:%.*]] = convert_function [[RESULT]]
// CHECK: [[THUNK_FN:%.*]] = function_ref
// CHECK-NEXT: [[THUNK:%.*]] = partial_apply [callee_guaranteed] [[THUNK_FN]]<τ_0_0>([[RESULT_CONV]])
// CHECK-NEXT: [[THUNK_CONV:%.*]] = convert_function [[THUNK]]
// CHECK-NEXT: end_borrow [[SELF_BOX_BORROW]]
// CHECK-NEXT: dealloc_stack [[SELF_BOX]]
// CHECK-NEXT: return [[THUNK_CONV]]

final class X : ReabstractSelfRefined {}

func inoutFunc(_ x: inout Int) {}

// CHECK-LABEL: sil hidden [ossa] @$s19protocol_resilience22inoutResilientProtocolyy010resilient_A005OtherdE0_pzF
func inoutResilientProtocol(_ x: inout OtherResilientProtocol) {
  // CHECK: function_ref @$s18resilient_protocol22OtherResilientProtocolPAAE19propertyInExtensionSivM
  inoutFunc(&x.propertyInExtension)
}

struct OtherConformingType : OtherResilientProtocol {}

// CHECK-LABEL: sil hidden [ossa] @$s19protocol_resilience22inoutResilientProtocolyyAA19OtherConformingTypeVzF
func inoutResilientProtocol(_ x: inout OtherConformingType) {
  // CHECK: function_ref @$s18resilient_protocol22OtherResilientProtocolPAAE19propertyInExtensionSivM
  inoutFunc(&x.propertyInExtension)

  // CHECK: function_ref @$s18resilient_protocol22OtherResilientProtocolPAAE25staticPropertyInExtensionSivM
  inoutFunc(&OtherConformingType.staticPropertyInExtension)
}

// Protocol is public -- needs resilient witness table
public struct ConformsToP: P { }

public protocol ResilientAssocTypes {
  associatedtype AssocType: P = ConformsToP
}

// CHECK-LABEL: sil_default_witness_table P {
// CHECK-NEXT: }

// CHECK-LABEL: sil_default_witness_table ResilientMethods {
// CHECK-NEXT:    no_default
// CHECK-NEXT:    no_default
// CHECK-NEXT:    method #ResilientMethods.defaultWitness: {{.*}} : @$s19protocol_resilience16ResilientMethodsP14defaultWitnessyyF
// CHECK-NEXT:    method #ResilientMethods.anotherDefaultWitness: {{.*}} : @$s19protocol_resilience16ResilientMethodsP21anotherDefaultWitnessyxSiF
// CHECK-NEXT:    method #ResilientMethods.defaultWitnessWithAssociatedType: {{.*}} : @$s19protocol_resilience16ResilientMethodsP32defaultWitnessWithAssociatedTypeyy05AssocI0QzF
// CHECK-NEXT:    method #ResilientMethods.defaultWitnessMoreAbstractThanRequirement: {{.*}} : @$s19protocol_resilience16ResilientMethodsP41defaultWitnessMoreAbstractThanRequirement_1by9AssocTypeQz_SitF
// CHECK-NEXT:    method #ResilientMethods.defaultWitnessMoreAbstractThanGenericRequirement: {{.*}} : @$s19protocol_resilience16ResilientMethodsP48defaultWitnessMoreAbstractThanGenericRequirement_1ty9AssocTypeQz_qd__tlF
// CHECK-NEXT:    no_default
// CHECK-NEXT:    no_default
// CHECK-NEXT:    method #ResilientMethods.staticDefaultWitness: {{.*}} : @$s19protocol_resilience16ResilientMethodsP20staticDefaultWitnessyxSiFZ
// CHECK-NEXT: }

// CHECK-LABEL: sil_default_witness_table ResilientConstructors {
// CHECK-NEXT:    no_default
// CHECK-NEXT:    method #ResilientConstructors.init!allocator: {{.*}} : @$s19protocol_resilience21ResilientConstructorsP7defaultxyt_tcfC
// CHECK-NEXT:    method #ResilientConstructors.init!allocator: {{.*}} : @$s19protocol_resilience21ResilientConstructorsP17defaultIsOptionalxSgyt_tcfC
// CHECK-NEXT:    no_default
// CHECK-NEXT:    no_default
// CHECK-NEXT: }

// CHECK-LABEL: sil_default_witness_table ResilientStorage {
// CHECK-NEXT:   no_default
// CHECK-NEXT:   no_default
// CHECK-NEXT:   method #ResilientStorage.propertyWithDefault!getter: {{.*}} : @$s19protocol_resilience16ResilientStorageP19propertyWithDefaultSivg
// CHECK-NEXT:   no_default
// CHECK-NEXT:   method #ResilientStorage.mutablePropertyWithDefault!getter: {{.*}} : @$s19protocol_resilience16ResilientStorageP26mutablePropertyWithDefaultSivg
// CHECK-NEXT:   method #ResilientStorage.mutablePropertyWithDefault!setter: {{.*}} : @$s19protocol_resilience16ResilientStorageP26mutablePropertyWithDefaultSivs
// CHECK-NEXT:   method #ResilientStorage.mutablePropertyWithDefault!modify: {{.*}} : @$s19protocol_resilience16ResilientStorageP26mutablePropertyWithDefaultSivM
// CHECK-NEXT:   no_default
// CHECK-NEXT:   no_default
// CHECK-NEXT:   no_default
// CHECK-NEXT:   method #ResilientStorage.mutableGenericPropertyWithDefault!getter: {{.*}} : @$s19protocol_resilience16ResilientStorageP33mutableGenericPropertyWithDefault1TQzvg
// CHECK-NEXT:   method #ResilientStorage.mutableGenericPropertyWithDefault!setter: {{.*}} : @$s19protocol_resilience16ResilientStorageP33mutableGenericPropertyWithDefault1TQzvs
// CHECK-NEXT:   method #ResilientStorage.mutableGenericPropertyWithDefault!modify: {{.*}} : @$s19protocol_resilience16ResilientStorageP33mutableGenericPropertyWithDefault1TQzvM
// CHECK-NEXT:   method #ResilientStorage.subscript!getter: {{.*}} : @$s19protocol_resilience16ResilientStoragePy1TQzAEcig
// CHECK-NEXT:   method #ResilientStorage.subscript!setter: {{.*}} : @$s19protocol_resilience16ResilientStoragePy1TQzAEcis
// CHECK-NEXT:   method #ResilientStorage.subscript!modify: {{.*}} : @$s19protocol_resilience16ResilientStoragePy1TQzAEciM
// CHECK-NEXT:   method #ResilientStorage.mutatingGetterWithNonMutatingDefault!getter: {{.*}} : @$s19protocol_resilience16ResilientStorageP36mutatingGetterWithNonMutatingDefaultSivg
// CHECK-NEXT:   method #ResilientStorage.mutatingGetterWithNonMutatingDefault!setter: {{.*}} : @$s19protocol_resilience16ResilientStorageP36mutatingGetterWithNonMutatingDefaultSivs
// CHECK-NEXT:   method #ResilientStorage.mutatingGetterWithNonMutatingDefault!modify: {{.*}} : @$s19protocol_resilience16ResilientStorageP36mutatingGetterWithNonMutatingDefaultSivM
// CHECK-NEXT: }

// CHECK-LABEL: sil_default_witness_table ResilientOperators {
// CHECK-NEXT:    no_default
// CHECK-NEXT:    no_default
// CHECK-NEXT:    method #ResilientOperators."~~~": {{.*}} : @$s19protocol_resilience18ResilientOperatorsP3tttopyyxFZ
// CHECK-NEXT:    method #ResilientOperators."<*>": {{.*}} : @$s19protocol_resilience18ResilientOperatorsP3lmgoiyyx_qd__tlFZ
// CHECK-NEXT:    method #ResilientOperators."<**>": {{.*}} : @$s19protocol_resilience18ResilientOperatorsP4lmmgoiy9AssocTypeQzqd___xtlFZ
// CHECK-NEXT:    method #ResilientOperators."<===>": {{.*}} : @$s19protocol_resilience18ResilientOperatorsP5leeegoiy9AssocTypeQyd__qd___xtAaBRd__lFZ
// CHECK-NEXT: }

// CHECK-LABEL: sil_default_witness_table ReabstractSelfRefined {
// CHECK-NEXT:   no_default
// CHECK-NEXT:   method #ReabstractSelfRefined.callback!getter: {{.*}} : @$s19protocol_resilience21ReabstractSelfRefinedP8callbackyxxcvg
// CHECK-NEXT:   method #ReabstractSelfRefined.callback!setter: {{.*}} : @$s19protocol_resilience21ReabstractSelfRefinedP8callbackyxxcvs
// CHECK-NEXT:   method #ReabstractSelfRefined.callback!modify: {{.*}} : @$s19protocol_resilience21ReabstractSelfRefinedP8callbackyxxcvM
// CHECK-NEXT: }

// CHECK-LABEL: sil_default_witness_table ResilientAssocTypes {
// CHECK-NEXT:   associated_type_protocol (AssocType: P): ConformsToP: P module protocol_resilience
// CHECK-NEXT:   associated_type AssocType: ConformsToP
// CHECK-NEXT: }
