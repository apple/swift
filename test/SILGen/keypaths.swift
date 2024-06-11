// RUN: %target-swift-emit-silgen -disable-availability-checking -parse-stdlib -module-name keypaths %s | %FileCheck %s

import Swift

struct S<T> {
  var x: T
  let y: String
  var z: C<T>

  var computed: C<T> { fatalError() }
  var observed: C<T> { didSet { fatalError() } }
  var reabstracted: () -> ()
}
class C<T> {
  final var x: T
  final let y: String
  final var z: S<T>

  var nonfinal: S<T>
  var computed: S<T> { fatalError() }
  var observed: S<T> { didSet { fatalError() } }
  final var reabstracted: () -> ()

  init() { fatalError() }
}

extension C {
  var `extension`: S<T> { fatalError() }
}

protocol P {
  var x: Int { get }
  var y: String { get set }
}

extension P {
  var z: String {
    return y
  }
  var w: String {
    get { return "" }
    nonmutating set { }
  }
}

struct T {
  var a: (Int, String)
  let b: (f: String, g: Int)
  let c: (x: C<Int>, y: C<String>)
}

/* TODO: When we support superclass requirements on protocols, we should test
 * this case as well.
protocol PoC : C<Int> {}
*/

// CHECK-LABEL: sil hidden [ossa] @{{.*}}storedProperties
func storedProperties<T>(_: T) {
  // CHECK: keypath $WritableKeyPath<S<T>, T>, <τ_0_0> (root $S<τ_0_0>; stored_property #S.x : $τ_0_0) <T>
  _ = \S<T>.x
  // CHECK: keypath $KeyPath<S<T>, String>, <τ_0_0> (root $S<τ_0_0>; stored_property #S.y : $String) <T>
  _ = \S<T>.y
  // CHECK: keypath $ReferenceWritableKeyPath<S<T>, T>, <τ_0_0> (root $S<τ_0_0>; stored_property #S.z : $C<τ_0_0>; stored_property #C.x : $τ_0_0) <T>
  _ = \S<T>.z.x
  // CHECK: keypath $ReferenceWritableKeyPath<C<T>, T>, <τ_0_0> (root $C<τ_0_0>; stored_property #C.x : $τ_0_0) <T>
  _ = \C<T>.x
  // CHECK: keypath $KeyPath<C<T>, String>, <τ_0_0> (root $C<τ_0_0>; stored_property #C.y : $String) <T>
  _ = \C<T>.y
  // CHECK: keypath $ReferenceWritableKeyPath<C<T>, T>, <τ_0_0> (root $C<τ_0_0>; stored_property #C.z : $S<τ_0_0>; stored_property #S.x : $τ_0_0) <T>
  _ = \C<T>.z.x
  // CHECK: keypath $KeyPath<C<T>, String>, <τ_0_0> (root $C<τ_0_0>; stored_property #C.z : $S<τ_0_0>; stored_property #S.z : $C<τ_0_0>; stored_property #C.y : $String) <T>
  _ = \C<T>.z.z.y
}

// CHECK-LABEL: sil hidden [ossa] @{{.*}}computedProperties
func computedProperties<T: P>(_: T) {
  // CHECK: keypath $ReferenceWritableKeyPath<C<T>, S<T>>, <τ_0_0 where τ_0_0 : P> (
  // CHECK-SAME: root $C<τ_0_0>;
  // CHECK-SAME: settable_property $S<τ_0_0>, 
  // CHECK-SAME:   id #C.nonfinal!getter : <T> (C<T>) -> () -> S<T>,
  // CHECK-SAME:   getter @$s8keypaths1CC8nonfinalAA1SVyxGvpAA1PRzlACyxGTK : $@convention(keypath_accessor_getter) <τ_0_0 where τ_0_0 : P> (@in_guaranteed C<τ_0_0>) -> @out S<τ_0_0>,
  // CHECK-SAME:   setter @$s8keypaths1CC8nonfinalAA1SVyxGvpAA1PRzlACyxGTk : $@convention(keypath_accessor_setter) <τ_0_0 where τ_0_0 : P> (@in_guaranteed S<τ_0_0>, @in_guaranteed C<τ_0_0>) -> ()
  // CHECK-SAME: ) <T>
  _ = \C<T>.nonfinal

  // CHECK: keypath $KeyPath<C<T>, S<T>>, <τ_0_0 where τ_0_0 : P> (
  // CHECK-SAME: root $C<τ_0_0>;
  // CHECK-SAME: gettable_property $S<τ_0_0>,
  // CHECK-SAME:   id #C.computed!getter : <T> (C<T>) -> () -> S<T>,
  // CHECK-SAME:   getter @$s8keypaths1CC8computedAA1SVyxGvpAA1PRzlACyxGTK : $@convention(keypath_accessor_getter) <τ_0_0 where τ_0_0 : P> (@in_guaranteed C<τ_0_0>) -> @out S<τ_0_0>
  // CHECK-SAME: ) <T>
  _ = \C<T>.computed

  // CHECK: keypath $ReferenceWritableKeyPath<C<T>, S<T>>, <τ_0_0 where τ_0_0 : P> (
  // CHECK-SAME: root $C<τ_0_0>;
  // CHECK-SAME: settable_property $S<τ_0_0>, 
  // CHECK-SAME:   id #C.observed!getter : <T> (C<T>) -> () -> S<T>,
  // CHECK-SAME:   getter @$s8keypaths1CC8observedAA1SVyxGvpAA1PRzlACyxGTK : $@convention(keypath_accessor_getter) <τ_0_0 where τ_0_0 : P> (@in_guaranteed C<τ_0_0>) -> @out S<τ_0_0>,
  // CHECK-SAME:   setter @$s8keypaths1CC8observedAA1SVyxGvpAA1PRzlACyxGTk : $@convention(keypath_accessor_setter) <τ_0_0 where τ_0_0 : P> (@in_guaranteed S<τ_0_0>, @in_guaranteed C<τ_0_0>) -> ()
  // CHECK-SAME: ) <T>
  _ = \C<T>.observed

  _ = \C<T>.nonfinal.x
  _ = \C<T>.computed.x
  _ = \C<T>.observed.x
  _ = \C<T>.z.computed
  _ = \C<T>.z.observed
  _ = \C<T>.observed.x

  // CHECK: keypath $ReferenceWritableKeyPath<C<T>, () -> ()>, <τ_0_0 where τ_0_0 : P> (
  // CHECK-SAME: root $C<τ_0_0>;
  // CHECK-SAME: settable_property $() -> (), 
  // CHECK-SAME:   id ##C.reabstracted,
  // CHECK-SAME:   getter @$s8keypaths1CC12reabstractedyycvpAA1PRzlACyxGTK : $@convention(keypath_accessor_getter) <τ_0_0 where τ_0_0 : P> (@in_guaranteed C<τ_0_0>) -> @out @callee_guaranteed @substituted <τ_0_0> () -> @out τ_0_0 for <()>,
  // CHECK-SAME:   setter @$s8keypaths1CC12reabstractedyycvpAA1PRzlACyxGTk : $@convention(keypath_accessor_setter) <τ_0_0 where τ_0_0 : P> (@in_guaranteed @callee_guaranteed @substituted <τ_0_0> () -> @out τ_0_0 for <()>, @in_guaranteed C<τ_0_0>) -> ()
  // CHECK-SAME: ) <T>
  _ = \C<T>.reabstracted

  // CHECK: keypath $KeyPath<S<T>, C<T>>, <τ_0_0 where τ_0_0 : P> (
  // CHECK-SAME: root $S<τ_0_0>; gettable_property $C<τ_0_0>,
  // CHECK-SAME: id @$s8keypaths1SV8computedAA1CCyxGvg : $@convention(method) <τ_0_0> (@in_guaranteed S<τ_0_0>) -> @owned C<τ_0_0>,
  // CHECK-SAME:   getter @$s8keypaths1SV8computedAA1CCyxGvpAA1PRzlACyxGTK : $@convention(keypath_accessor_getter) <τ_0_0 where τ_0_0 : P> (@in_guaranteed S<τ_0_0>) -> @out C<τ_0_0>
  // CHECK-SAME: ) <T>
  _ = \S<T>.computed

  // CHECK: keypath $WritableKeyPath<S<T>, C<T>>, <τ_0_0 where τ_0_0 : P> (
  // CHECK-SAME: root $S<τ_0_0>;
  // CHECK-SAME: settable_property $C<τ_0_0>,
  // CHECK-SAME:   id @$s8keypaths1SV8observedAA1CCyxGvg : $@convention(method) <τ_0_0> (@in_guaranteed S<τ_0_0>) -> @owned C<τ_0_0>,
  // CHECK-SAME:   getter @$s8keypaths1SV8observedAA1CCyxGvpAA1PRzlACyxGTK : $@convention(keypath_accessor_getter) <τ_0_0 where τ_0_0 : P> (@in_guaranteed S<τ_0_0>) -> @out C<τ_0_0>,
  // CHECK-SAME:   setter @$s8keypaths1SV8observedAA1CCyxGvpAA1PRzlACyxGTk : $@convention(keypath_accessor_setter) <τ_0_0 where τ_0_0 : P> (@in_guaranteed C<τ_0_0>, @inout S<τ_0_0>) -> ()
  // CHECK-SAME: ) <T>
  _ = \S<T>.observed
  _ = \S<T>.z.nonfinal
  _ = \S<T>.z.computed
  _ = \S<T>.z.observed
  _ = \S<T>.computed.x
  _ = \S<T>.computed.y
  // CHECK: keypath $WritableKeyPath<S<T>, () -> ()>, <τ_0_0 where τ_0_0 : P> (
  // CHECK-SAME:  root $S<τ_0_0>;
  // CHECK-SAME:  settable_property $() -> (),
  // CHECK-SAME:    id ##S.reabstracted,
  // CHECK-SAME:    getter @$s8keypaths1SV12reabstractedyycvpAA1PRzlACyxGTK : $@convention(keypath_accessor_getter) <τ_0_0 where τ_0_0 : P> (@in_guaranteed S<τ_0_0>) -> @out @callee_guaranteed @substituted <τ_0_0> () -> @out τ_0_0 for <()>,
  // CHECK-SAME:    setter @$s8keypaths1SV12reabstractedyycvpAA1PRzlACyxGTk : $@convention(keypath_accessor_setter) <τ_0_0 where τ_0_0 : P> (@in_guaranteed @callee_guaranteed @substituted <τ_0_0> () -> @out τ_0_0 for <()>, @inout S<τ_0_0>) -> ()
  // CHECK-SAME: ) <T>
  _ = \S<T>.reabstracted

  // CHECK: keypath $KeyPath<T, Int>, <τ_0_0 where τ_0_0 : P> (
  // CHECK-SAME: root $τ_0_0;
  // CHECK-SAME: gettable_property $Int, 
  // CHECK-SAME:   id #P.x!getter : <Self where Self : P> (Self) -> () -> Int,
  // CHECK-SAME:   getter @$s8keypaths1PP1xSivpAaBRzlxTK : $@convention(keypath_accessor_getter) <τ_0_0 where τ_0_0 : P> (@in_guaranteed τ_0_0) -> @out Int
  // CHECK-SAME: ) <T>
  _ = \T.x
  // CHECK: keypath $WritableKeyPath<T, String>, <τ_0_0 where τ_0_0 : P> (
  // CHECK-SAME: root $τ_0_0;
  // CHECK-SAME: settable_property $String,
  // CHECK-SAME:   id #P.y!getter : <Self where Self : P> (Self) -> () -> String,
  // CHECK-SAME:   getter @$s8keypaths1PP1ySSvpAaBRzlxTK : $@convention(keypath_accessor_getter) <τ_0_0 where τ_0_0 : P> (@in_guaranteed τ_0_0) -> @out String,
  // CHECK-SAME:   setter @$s8keypaths1PP1ySSvpAaBRzlxTk : $@convention(keypath_accessor_setter) <τ_0_0 where τ_0_0 : P> (@in_guaranteed String, @inout τ_0_0) -> ()
  // CHECK-SAME: ) <T>
  _ = \T.y

  // CHECK: keypath $KeyPath<T, String>, <τ_0_0 where τ_0_0 : P> (
  // CHECK-SAME: root $τ_0_0;
  // CHECK-SAME: gettable_property $String,
  // CHECK-SAME:   id @$s8keypaths1PPAAE1zSSvg
  _ = \T.z
}

struct Concrete: P {
  var x: Int
  var y: String
}

// CHECK-LABEL: sil hidden [ossa] @$s8keypaths35keyPathsWithSpecificGenericInstanceyyF
func keyPathsWithSpecificGenericInstance() {
  // CHECK: keypath $KeyPath<Concrete, String>, (
  // CHECK-SAME: gettable_property $String,
  // CHECK-SAME:   id @$s8keypaths1PPAAE1zSSvg
  // CHECK-SAME:   getter @$s8keypaths1PPAAE1zSSvpAA8ConcreteVTK : $@convention(keypath_accessor_getter) (@in_guaranteed Concrete) -> @out String
  _ = \Concrete.z
  _ = \S<Concrete>.computed
}

class AA<T> {
  var a: Int { get { return 0 } set { } }
}
class BB<U, V>: AA<V> {
}

func keyPathForInheritedMember() {
  _ = \BB<Int, String>.a
}

func keyPathForExistentialMember() {
  _ = \P.x
  _ = \P.y
  _ = \P.z
  _ = \P.w
}

struct OptionalFields {
  var x: S<Int>?
}
struct OptionalFields2 {
  var y: OptionalFields?
}

// CHECK-LABEL: sil hidden [ossa] @$s8keypaths18keyPathForOptionalyyF
func keyPathForOptional() {
  // CHECK: keypath $WritableKeyPath<OptionalFields, S<Int>>, (
  // CHECK-SAME:   stored_property #OptionalFields.x : $Optional<S<Int>>;
  // CHECK-SAME:   optional_force : $S<Int>)
  _ = \OptionalFields.x!
  // CHECK: keypath $KeyPath<OptionalFields, Optional<String>>, (
  // CHECK-SAME:   stored_property #OptionalFields.x : $Optional<S<Int>>;
  // CHECK-SAME:   optional_chain : $S<Int>;
  // CHECK-SAME:   stored_property #S.y : $String;
  // CHECK-SAME:   optional_wrap : $Optional<String>)
  _ = \OptionalFields.x?.y
  // CHECK: keypath $KeyPath<OptionalFields2, Optional<S<Int>>>, (
  // CHECK-SAME:   root $OptionalFields2;
  // CHECK-SAME:   stored_property #OptionalFields2.y : $Optional<OptionalFields>;
  // CHECK-SAME:   optional_chain : $OptionalFields;
  // CHECK-SAME:   stored_property #OptionalFields.x : $Optional<S<Int>>)
  _ = \OptionalFields2.y?.x
}

class StorageQualified {
  weak var tooWeak: StorageQualified?
  unowned var disowned: StorageQualified
  
  init() { fatalError() }
}

final class FinalStorageQualified {
  weak var tooWeak: StorageQualified?
  unowned var disowned: StorageQualified
  
  init() { fatalError() }
}

// CHECK-LABEL: sil hidden [ossa] @{{.*}}keyPathForStorageQualified
func keyPathForStorageQualified() {
  // CHECK: = keypath $ReferenceWritableKeyPath<StorageQualified, Optional<StorageQualified>>,
  // CHECK-SAME: settable_property $Optional<StorageQualified>, id #StorageQualified.tooWeak!getter
  _ = \StorageQualified.tooWeak
  // CHECK: = keypath $ReferenceWritableKeyPath<StorageQualified, StorageQualified>,
  // CHECK-SAME: settable_property $StorageQualified, id #StorageQualified.disowned!getter
  _ = \StorageQualified.disowned

  // CHECK: = keypath $ReferenceWritableKeyPath<FinalStorageQualified, Optional<StorageQualified>>,
  // CHECK-SAME: settable_property $Optional<StorageQualified>, id ##FinalStorageQualified.tooWeak
  _ = \FinalStorageQualified.tooWeak
  // CHECK: = keypath $ReferenceWritableKeyPath<FinalStorageQualified, StorageQualified>,
  // CHECK-SAME: settable_property $StorageQualified, id ##FinalStorageQualified.disowned
  _ = \FinalStorageQualified.disowned
}

struct IUOProperty {
  var iuo: IUOBlob!
}

struct IUOBlob {
  var x: Int
  subscript(y: String) -> String {
    get { return y }
    set {}
  }
}

// CHECK-LABEL: sil hidden [ossa] @{{.*}}11iuoKeyPaths
func iuoKeyPaths() {
  // CHECK: = keypath $WritableKeyPath<IUOProperty, Int>,
  // CHECK-SAME: stored_property #IUOProperty.iuo
  // CHECK-SAME: optional_force
  // CHECK-SAME: stored_property #IUOBlob.x
  _ = \IUOProperty.iuo.x
  // CHECK: = keypath $WritableKeyPath<IUOProperty, Int>,
  // CHECK-SAME: stored_property #IUOProperty.iuo
  // CHECK-SAME: optional_force
  // CHECK-SAME: stored_property #IUOBlob.x
  _ = \IUOProperty.iuo!.x
}

class Bass: Hashable {
  static func ==(_: Bass, _: Bass) -> Bool { return false }
  func hash(into hasher: inout Hasher) {}
}

class Treble: Bass { }

struct Subscripts<T> {
  subscript() -> T {
    get { fatalError() }
    set { fatalError() }
  }
  subscript(generic x: T) -> T {
    get { fatalError() }
    set { fatalError() }
  }
  subscript(concrete x: String) -> String {
    get { fatalError() }
    set { fatalError() }
  }
  subscript(x: String, y: String) -> String {
    get { fatalError() }
    set { fatalError() }
  }
  subscript<U>(subGeneric z: U) -> U {
    get { fatalError() }
    set { fatalError() }
  }
  subscript(mutable x: T) -> T {
    get { fatalError() }
    set { fatalError() }
  }
  subscript(bass: Bass) -> Bass {
    get { return bass }
    set { }
  }
}

struct SubscriptDefaults1 {
  subscript(x: Int = 0) -> Int {
    get { fatalError() }
    set { fatalError() }
  }
  subscript(x: Int, y: Int, z: Int = 0) -> Int {
    get { fatalError() }
    set { fatalError() }
  }
  subscript(x: Bool, bool y: Bool = false) -> Bool {
    get { fatalError() }
    set { fatalError() }
  }
  subscript(bool x: Bool, y: Int, z: Int = 0) -> Int {
    get { fatalError() }
    set { fatalError() }
  }
}

struct SubscriptDefaults2 {
  subscript(x: Int? = nil) -> Int {
    get { fatalError() }
    set { fatalError() }
  }
}

struct SubscriptDefaults3 {
  subscript(x: Int = #line) -> Int {
    get { fatalError() }
    set { fatalError() }
  }
}

struct SubscriptDefaults4 {
  subscript<T : Numeric>(x x: T, y y: T = 0) -> T {
    get { fatalError() }
    set { fatalError() }
  }
}

struct SubscriptDefaults5 {
  subscript<T : ExpressibleByStringLiteral>(x x: T, y y: T =  #function) -> T {
    get { fatalError() }
    set { fatalError() }
  }
}

// CHECK-LABEL: sil hidden [ossa] @{{.*}}10subscripts1x1y1syx_q_SStSHRzSHR_r0_lF
func subscripts<T: Hashable, U: Hashable>(x: T, y: U, s: String) {
  _ = \Subscripts<T>.[]
  _ = \Subscripts<T>.[generic: x]
  _ = \Subscripts<T>.[concrete: s]
  _ = \Subscripts<T>.[s, s]
  _ = \Subscripts<T>.[subGeneric: s]
  _ = \Subscripts<T>.[subGeneric: x]
  _ = \Subscripts<T>.[subGeneric: y]

  _ = \Subscripts<U>.[]
  _ = \Subscripts<U>.[generic: y]
  _ = \Subscripts<U>.[concrete: s]
  _ = \Subscripts<U>.[s, s]
  _ = \Subscripts<U>.[subGeneric: s]
  _ = \Subscripts<U>.[subGeneric: x]
  _ = \Subscripts<U>.[subGeneric: y]

  _ = \Subscripts<String>.[]
  _ = \Subscripts<String>.[generic: s]
  _ = \Subscripts<String>.[concrete: s]
  _ = \Subscripts<String>.[s, s]
  _ = \Subscripts<String>.[subGeneric: s]
  _ = \Subscripts<String>.[subGeneric: x]
  _ = \Subscripts<String>.[subGeneric: y]

  _ = \Subscripts<T>.[s, s].count

  _ = \Subscripts<T>.[Bass()]
  _ = \Subscripts<T>.[Treble()]

  _ = \SubscriptDefaults1.[]
  _ = \SubscriptDefaults1.[0]
  _ = \SubscriptDefaults1.[0, 0]
  _ = \SubscriptDefaults1.[0, 0, 0]
  _ = \SubscriptDefaults1.[false]
  _ = \SubscriptDefaults1.[false, bool: false]
  _ = \SubscriptDefaults1.[bool: false, 0]
  _ = \SubscriptDefaults1.[bool: false, 0, 0]
  
  _ = \SubscriptDefaults2.[]
  _ = \SubscriptDefaults2.[0]
  _ = \SubscriptDefaults3.[]
  _ = \SubscriptDefaults3.[0]
  _ = \SubscriptDefaults4.[x: 0]
  _ = \SubscriptDefaults4.[x: 0, y: 0]
  _ = \SubscriptDefaults5.[x: ""]
  _ = \SubscriptDefaults5.[x: "", y: ""]
}

// CHECK-LABEL: sil hidden [ossa] @{{.*}}check_default_subscripts
func check_default_subscripts() {
  // CHECK: [[INTX:%[0-9]+]] = integer_literal $Builtin.IntLiteral, 0
  // CHECK: [[IX:%[0-9]+]] = apply %{{[0-9]+}}([[INTX]], {{.*}}
  // CHECK: [[INTY:%[0-9]+]] = integer_literal $Builtin.IntLiteral, 0
  // CHECK: [[IY:%[0-9]+]] = apply %{{[0-9]+}}([[INTY]], {{.*}}
  // CHECK: [[KEYPATH:%[0-9]+]] = keypath $WritableKeyPath<SubscriptDefaults4, Int>, (root $SubscriptDefaults4; settable_property $Int, id @$s8keypaths18SubscriptDefaults4V1x1yxx_xtcSjRzluig : $@convention(method) <τ_0_0 where τ_0_0 : Numeric> (@in_guaranteed τ_0_0, @in_guaranteed τ_0_0, SubscriptDefaults4) -> @out τ_0_0, getter @$s8keypaths18SubscriptDefaults4V1x1yxx_xtcSjRzluipACSiTK : $@convention(keypath_accessor_getter) (@in_guaranteed SubscriptDefaults4, @in_guaranteed (Int, Int)) -> @out Int, setter @$s8keypaths18SubscriptDefaults4V1x1yxx_xtcSjRzluipACSiTk : $@convention(keypath_accessor_setter) (@in_guaranteed Int, @inout SubscriptDefaults4, @in_guaranteed (Int, Int)) -> (), indices [%$0 : $Int : $Int, %$1 : $Int : $Int], indices_equals @$sS2iTH : $@convention(keypath_accessor_equals) (@in_guaranteed (Int, Int), @in_guaranteed (Int, Int)) -> Bool, indices_hash @$sS2iTh : $@convention(keypath_accessor_hash) (@in_guaranteed (Int, Int)) -> Int) ([[IX]], [[IY]])
  _ = \SubscriptDefaults4.[x: 0, y: 0]

  // CHECK: [[INTINIT:%[0-9]+]] = integer_literal $Builtin.IntLiteral, 0
  // CHECK: [[I:%[0-9]+]] = apply %{{[0-9]+}}([[INTINIT]], {{.*}}
  // CHECK: [[DFN:%[0-9]+]] = function_ref @$s8keypaths18SubscriptDefaults4V1x1yxx_xtcSjRzluipfA0_ : $@convention(thin) <τ_0_0 where τ_0_0 : Numeric> () -> @out τ_0_0
  // CHECK: [[ALLOC:%[0-9]+]] = alloc_stack $Int
  // CHECK: apply [[DFN]]<Int>([[ALLOC]]) : $@convention(thin) <τ_0_0 where τ_0_0 : Numeric> () -> @out τ_0_0
  // CHECK: [[LOAD:%[0-9]+]] = load [trivial] [[ALLOC]] : $*Int
  // CHECK: [[KEYPATH:%[0-9]+]] = keypath $WritableKeyPath<SubscriptDefaults4, Int>, (root $SubscriptDefaults4; settable_property $Int, id @$s8keypaths18SubscriptDefaults4V1x1yxx_xtcSjRzluig : $@convention(method) <τ_0_0 where τ_0_0 : Numeric> (@in_guaranteed τ_0_0, @in_guaranteed τ_0_0, SubscriptDefaults4) -> @out τ_0_0, getter @$s8keypaths18SubscriptDefaults4V1x1yxx_xtcSjRzluipACSiTK : $@convention(keypath_accessor_getter) (@in_guaranteed SubscriptDefaults4, @in_guaranteed (Int, Int)) -> @out Int, setter @$s8keypaths18SubscriptDefaults4V1x1yxx_xtcSjRzluipACSiTk : $@convention(keypath_accessor_setter) (@in_guaranteed Int, @inout SubscriptDefaults4, @in_guaranteed (Int, Int)) -> (), indices [%$0 : $Int : $Int, %$1 : $Int : $Int], indices_equals @$sS2iTH : $@convention(keypath_accessor_equals) (@in_guaranteed (Int, Int), @in_guaranteed (Int, Int)) -> Bool, indices_hash @$sS2iTh : $@convention(keypath_accessor_hash) (@in_guaranteed (Int, Int)) -> Int) ([[I]], [[LOAD]])
  _ = \SubscriptDefaults4.[x: 0]
  
  // CHECK: [[STRX_LIT:%[0-9]+]] = string_literal utf8 ""
  // CHECK: [[STRX:%[0-9]+]] = apply %{{[0-9]+}}([[STRX_LIT]], {{.*}}
  // CHECK: [[STRY_LIT:%[0-9]+]] = string_literal utf8 "check_default_subscripts()"
  // CHECK: [[DEF_ARG:%[0-9]+]] = apply %{{[0-9]+}}([[STRY_LIT]], {{.*}}
  // CHECK: keypath $WritableKeyPath<SubscriptDefaults5, String>, (root $SubscriptDefaults5; settable_property $String, id @$s8keypaths18SubscriptDefaults5V1x1yxx_xtcs26ExpressibleByStringLiteralRzluig : $@convention(method) <τ_0_0 where τ_0_0 : ExpressibleByStringLiteral> (@in_guaranteed τ_0_0, @in_guaranteed τ_0_0, SubscriptDefaults5) -> @out τ_0_0, getter @$s8keypaths18SubscriptDefaults5V1x1yxx_xtcs26ExpressibleByStringLiteralRzluipACSSTK : $@convention(keypath_accessor_getter) (@in_guaranteed SubscriptDefaults5, @in_guaranteed (String, String)) -> @out String, setter @$s8keypaths18SubscriptDefaults5V1x1yxx_xtcs26ExpressibleByStringLiteralRzluipACSSTk : $@convention(keypath_accessor_setter) (@in_guaranteed String, @inout SubscriptDefaults5, @in_guaranteed (String, String)) -> (), indices [%$0 : $String : $String, %$1 : $String : $String], indices_equals @$sS2STH : $@convention(keypath_accessor_equals) (@in_guaranteed (String, String), @in_guaranteed (String, String)) -> Bool, indices_hash @$sS2STh : $@convention(keypath_accessor_hash) (@in_guaranteed (String, String)) -> Int) ([[STRX]], [[DEF_ARG]])
  _ = \SubscriptDefaults5.[x: ""]
  
  // CHECK: [[STRX_LIT:%[0-9]+]] = string_literal utf8 ""
  // CHECK: [[STRX:%[0-9]+]] = apply %{{[0-9]+}}([[STRX_LIT]], {{.*}}
  // CHECK: [[STRY_LIT:%[0-9]+]] = string_literal utf8 ""
  // CHECK: [[STRY:%[0-9]+]] = apply %{{[0-9]+}}([[STRY_LIT]], {{.*}}
  // CHECK: keypath $WritableKeyPath<SubscriptDefaults5, String>, (root $SubscriptDefaults5; settable_property $String, id @$s8keypaths18SubscriptDefaults5V1x1yxx_xtcs26ExpressibleByStringLiteralRzluig : $@convention(method) <τ_0_0 where τ_0_0 : ExpressibleByStringLiteral> (@in_guaranteed τ_0_0, @in_guaranteed τ_0_0, SubscriptDefaults5) -> @out τ_0_0, getter @$s8keypaths18SubscriptDefaults5V1x1yxx_xtcs26ExpressibleByStringLiteralRzluipACSSTK : $@convention(keypath_accessor_getter) (@in_guaranteed SubscriptDefaults5, @in_guaranteed (String, String)) -> @out String, setter @$s8keypaths18SubscriptDefaults5V1x1yxx_xtcs26ExpressibleByStringLiteralRzluipACSSTk : $@convention(keypath_accessor_setter) (@in_guaranteed String, @inout SubscriptDefaults5, @in_guaranteed (String, String)) -> (), indices [%$0 : $String : $String, %$1 : $String : $String], indices_equals @$sS2STH : $@convention(keypath_accessor_equals) (@in_guaranteed (String, String), @in_guaranteed (String, String)) -> Bool, indices_hash @$sS2STh : $@convention(keypath_accessor_hash) (@in_guaranteed (String, String)) -> Int) ([[STRX]], [[STRY]])
  _ = \SubscriptDefaults5.[x: "", y: ""]
}

struct SubscriptVariadic1 {
  subscript(x: Int...) -> Int { x[0] }
}

struct SubscriptVariadic2 {
  subscript<T : ExpressibleByStringLiteral>(x: T...) -> T { x[0] }
}

struct SubscriptVariadic3<T : ExpressibleByStringLiteral> {
  subscript(x: T...) -> T { x[0] }
}

// CHECK-LABEL: sil hidden [ossa] @{{.*}}test_variadics
func test_variadics() {
  // CHECK: [[ARR_COUNT:%[0-9]+]] = integer_literal $Builtin.Word, 3
  // CHECK: [[FN_REF:%[0-9]+]] = function_ref @$ss27_allocateUninitializedArrayySayxG_BptBwlF
  // CHECK: [[MAKE_ARR:%[0-9]+]] = apply [[FN_REF]]<Int>([[ARR_COUNT]])
  // CHECK: ([[ARR:%[0-9]+]], %{{[0-9]+}}) = destructure_tuple [[MAKE_ARR]] : $(Array<Int>, Builtin.RawPointer)
  // CHECK: [[FIN_REF:%[0-9]+]] = function_ref @$ss27_finalizeUninitializedArrayySayxGABnlF
  // CHECK: [[FIN_ARR:%[0-9]+]] = apply [[FIN_REF]]<Int>([[ARR]])
  // CHECK: keypath $KeyPath<SubscriptVariadic1, Int>, (root $SubscriptVariadic1; gettable_property $Int,  id @$s8keypaths18SubscriptVariadic1VyS2id_tcig : $@convention(method) (@guaranteed Array<Int>, SubscriptVariadic1) -> Int, getter @$s8keypaths18SubscriptVariadic1VyS2id_tcipACTK : $@convention(keypath_accessor_getter) (@in_guaranteed SubscriptVariadic1, @in_guaranteed Array<Int>) -> @out Int, indices [%$0 : $Array<Int> : $Array<Int>], indices_equals @$sSaySiGTH : $@convention(keypath_accessor_equals) (@in_guaranteed Array<Int>, @in_guaranteed Array<Int>) -> Bool, indices_hash @$sSaySiGTh : $@convention(keypath_accessor_hash) (@in_guaranteed Array<Int>) -> Int) ([[FIN_ARR]])
  _ = \SubscriptVariadic1.[1, 2, 3]
  // CHECK: [[ARR_COUNT:%[0-9]+]] = integer_literal $Builtin.Word, 1
  // CHECK: [[FN_REF:%[0-9]+]] = function_ref @$ss27_allocateUninitializedArrayySayxG_BptBwlF
  // CHECK: [[MAKE_ARR:%[0-9]+]] = apply [[FN_REF]]<Int>([[ARR_COUNT]])
  // CHECK: ([[ARR:%[0-9]+]], %{{[0-9]+}}) = destructure_tuple [[MAKE_ARR]] : $(Array<Int>, Builtin.RawPointer)
  // CHECK: [[FIN_REF:%[0-9]+]] = function_ref @$ss27_finalizeUninitializedArrayySayxGABnlF
  // CHECK: [[FIN_ARR:%[0-9]+]] = apply [[FIN_REF]]<Int>([[ARR]])
  // CHECK: keypath $KeyPath<SubscriptVariadic1, Int>, (root $SubscriptVariadic1; gettable_property $Int,  id @$s8keypaths18SubscriptVariadic1VyS2id_tcig : $@convention(method) (@guaranteed Array<Int>, SubscriptVariadic1) -> Int, getter @$s8keypaths18SubscriptVariadic1VyS2id_tcipACTK : $@convention(keypath_accessor_getter) (@in_guaranteed SubscriptVariadic1, @in_guaranteed Array<Int>) -> @out Int, indices [%$0 : $Array<Int> : $Array<Int>], indices_equals @$sSaySiGTH : $@convention(keypath_accessor_equals) (@in_guaranteed Array<Int>, @in_guaranteed Array<Int>) -> Bool, indices_hash @$sSaySiGTh : $@convention(keypath_accessor_hash) (@in_guaranteed Array<Int>) -> Int) ([[FIN_ARR]])
  _ = \SubscriptVariadic1.[1]
  // CHECK: [[ARR_COUNT:%[0-9]+]] = integer_literal $Builtin.Word, 0
  // CHECK: [[FN_REF:%[0-9]+]] = function_ref @$ss27_allocateUninitializedArrayySayxG_BptBwlF
  // CHECK: [[MAKE_ARR:%[0-9]+]] = apply [[FN_REF]]<Int>([[ARR_COUNT]])
  // CHECK: ([[ARR:%[0-9]+]], %{{[0-9]+}}) = destructure_tuple [[MAKE_ARR]] : $(Array<Int>, Builtin.RawPointer)
  // CHECK: keypath $KeyPath<SubscriptVariadic1, Int>, (root $SubscriptVariadic1; gettable_property $Int,  id @$s8keypaths18SubscriptVariadic1VyS2id_tcig : $@convention(method) (@guaranteed Array<Int>, SubscriptVariadic1) -> Int, getter @$s8keypaths18SubscriptVariadic1VyS2id_tcipACTK : $@convention(keypath_accessor_getter) (@in_guaranteed SubscriptVariadic1, @in_guaranteed Array<Int>) -> @out Int, indices [%$0 : $Array<Int> : $Array<Int>], indices_equals @$sSaySiGTH : $@convention(keypath_accessor_equals) (@in_guaranteed Array<Int>, @in_guaranteed Array<Int>) -> Bool, indices_hash @$sSaySiGTh : $@convention(keypath_accessor_hash) (@in_guaranteed Array<Int>) -> Int) ([[ARR]])
  _ = \SubscriptVariadic1.[]
  
  _ = \SubscriptVariadic2.["", "1"]
  _ = \SubscriptVariadic2.[""]
  // CHECK: [[ARR_COUNT:%[0-9]+]] = integer_literal $Builtin.Word, 2
  // CHECK: [[FN_REF:%[0-9]+]] = function_ref @$ss27_allocateUninitializedArrayySayxG_BptBwlF
  // CHECK: [[MAKE_ARR:%[0-9]+]] = apply [[FN_REF]]<String>([[ARR_COUNT]])
  // CHECK: ([[ARR:%[0-9]+]], %{{[0-9]+}}) = destructure_tuple [[MAKE_ARR]] : $(Array<String>, Builtin.RawPointer)
  // CHECK: [[FIN_REF:%[0-9]+]] = function_ref @$ss27_finalizeUninitializedArrayySayxGABnlF
  // CHECK: [[FIN_ARR:%[0-9]+]] = apply [[FIN_REF]]<String>([[ARR]])
  // CHECK: keypath $KeyPath<SubscriptVariadic2, String>, (root $SubscriptVariadic2; gettable_property $String,  id @$s8keypaths18SubscriptVariadic2Vyxxd_tcs26ExpressibleByStringLiteralRzluig : $@convention(method) <τ_0_0 where τ_0_0 : ExpressibleByStringLiteral> (@guaranteed Array<τ_0_0>, SubscriptVariadic2) -> @out τ_0_0, getter @$s8keypaths18SubscriptVariadic2Vyxxd_tcs26ExpressibleByStringLiteralRzluipACSSTK : $@convention(keypath_accessor_getter) (@in_guaranteed SubscriptVariadic2, @in_guaranteed Array<String>) -> @out String, indices [%$0 : $Array<String> : $Array<String>], indices_equals @$sSaySSGTH : $@convention(keypath_accessor_equals) (@in_guaranteed Array<String>, @in_guaranteed Array<String>) -> Bool, indices_hash @$sSaySSGTh : $@convention(keypath_accessor_hash) (@in_guaranteed Array<String>) -> Int) ([[FIN_ARR]])
  _ = \SubscriptVariadic2.["", #function]
  
  _ = \SubscriptVariadic3<String>.[""]
  _ = \SubscriptVariadic3<String>.["", "1"]
  _ = \SubscriptVariadic3<String>.[]
}

// CHECK-LABEL: sil hidden [ossa] @{{.*}}subclass_generics
func subclass_generics<T: C<Int>, U: C<V>, V/*: PoC*/>(_: T, _: U, _: V) {
  _ = \T.x
  _ = \T.z
  _ = \T.computed
  _ = \T.extension

  _ = \U.x
  _ = \U.z
  _ = \U.computed
  _ = \U.extension

/*
  _ = \V.x
  _ = \V.z
  _ = \V.computed
  _ = \V.extension
 */

  _ = \(C<Int> & P).x
  _ = \(C<Int> & P).z
  _ = \(C<Int> & P).computed
  _ = \(C<Int> & P).extension

  _ = \(C<V> & P).x
  _ = \(C<V> & P).z
  _ = \(C<V> & P).computed
  _ = \(C<V> & P).extension

/* TODO: When we support superclass requirements on protocols, we should test
 * this case as well.
  _ = \PoC.x
  _ = \PoC.z
  _ = \PoC.computed
  _ = \PoC.extension
 */
}

// CHECK-LABEL: sil hidden [ossa] @{{.*}}identity
func identity<T>(_: T) {
  // CHECK: keypath $WritableKeyPath<T, T>, <τ_0_0> ({{.*}}root $τ_0_0) <T>
  let _: WritableKeyPath<T, T> = \T.self
  // CHECK: keypath $WritableKeyPath<Array<T>, Array<T>>, <τ_0_0> ({{.*}}root $Array<τ_0_0>) <T>
  let _: WritableKeyPath<[T], [T]> = \[T].self
  // CHECK: keypath $WritableKeyPath<String, String>, ({{.*}}root $String)
  let _: WritableKeyPath<String, String> = \String.self
}

// CHECK-LABEL: sil hidden [ossa] @{{.*}}tuples
func tuples(_: T) {
  // CHECK: keypath $WritableKeyPath<T, Int>, (root $T; stored_property #T.a : $(Int, String); tuple_element #0 : $Int)
  let _: WritableKeyPath<T, Int> = \T.a.0
  // CHECK: keypath $WritableKeyPath<T, String>, (root $T; stored_property #T.a : $(Int, String); tuple_element #1 : $String)
  let _: WritableKeyPath<T, String> = \T.a.1
  // CHECK: keypath $KeyPath<T, String>, (root $T; stored_property #T.b : $(f: String, g: Int); tuple_element #0 : $String)
  let _: KeyPath<T, String> = \T.b.f
  // CHECK: keypath $KeyPath<T, Int>, (root $T; stored_property #T.b : $(f: String, g: Int); tuple_element #1 : $Int)
  let _: KeyPath<T, Int> = \T.b.g
  // CHECK: keypath $KeyPath<T, C<Int>>, (root $T; stored_property #T.c : $(x: C<Int>, y: C<String>); tuple_element #0 : $C<Int>)
  let _: KeyPath<T, C<Int>> = \T.c.x
  // CHECK: keypath $KeyPath<T, C<String>>, (root $T; stored_property #T.c : $(x: C<Int>, y: C<String>); tuple_element #1 : $C<String>)
  let _: KeyPath<T, C<String>> = \T.c.y

  // CHECK: keypath $ReferenceWritableKeyPath<T, Int>, (root $T; stored_property #T.c : $(x: C<Int>, y: C<String>); tuple_element #0 : $C<Int>; stored_property #C.x : $Int)
  let _: ReferenceWritableKeyPath<T, Int> = \T.c.x.x
  // CHECK: keypath $KeyPath<T, String>, (root $T; stored_property #T.c : $(x: C<Int>, y: C<String>); tuple_element #0 : $C<Int>; stored_property #C.y : $String)
  let _: KeyPath<T, String> = \T.c.x.y

  typealias Thing = (type: Any.Type, fn: () -> ())

  // CHECK: keypath $WritableKeyPath<(type: any Any.Type, fn: () -> ()), any Any.Type>, (root $(type: any Any.Type, fn: () -> ()); tuple_element #0 : $any Any.Type)
  let _: WritableKeyPath<Thing, Any.Type> = \Thing.type

  // CHECK: keypath $WritableKeyPath<(type: any Any.Type, fn: () -> ()), () -> ()>, (root $(type: any Any.Type, fn: () -> ()); tuple_element #1 : $() -> ())
  let _: WritableKeyPath<Thing, () -> ()> = \Thing.fn
}

// CHECK-LABEL: sil hidden [ossa] @{{.*}}tuples_generic
func tuples_generic<T, U, V>(_: T, _: U, _: V) {
  typealias TUC = (T, U, C<V>)

  // CHECK: keypath $WritableKeyPath<(T, U, C<V>), T>, <τ_0_0, τ_0_1, τ_0_2> (root $(τ_0_0, τ_0_1, C<τ_0_2>); tuple_element #0 : $τ_0_0) <T, U, V>
  let _: WritableKeyPath<TUC, T> = \TUC.0
  // CHECK: keypath $WritableKeyPath<(T, U, C<V>), U>, <τ_0_0, τ_0_1, τ_0_2> (root $(τ_0_0, τ_0_1, C<τ_0_2>); tuple_element #1 : $τ_0_1) <T, U, V>
  let _: WritableKeyPath<TUC, U> = \TUC.1
  // CHECK: keypath $ReferenceWritableKeyPath<(T, U, C<V>), V>, <τ_0_0, τ_0_1, τ_0_2> (root $(τ_0_0, τ_0_1, C<τ_0_2>); tuple_element #2 : $C<τ_0_2>; stored_property #C.x : $τ_0_2) <T, U, V>
  let _: ReferenceWritableKeyPath<TUC, V> = \TUC.2.x
  // CHECK: keypath $KeyPath<(T, U, C<V>), String>, <τ_0_0, τ_0_1, τ_0_2> (root $(τ_0_0, τ_0_1, C<τ_0_2>); tuple_element #2 : $C<τ_0_2>; stored_property #C.y : $String) <T, U, V>
  let _: KeyPath<TUC, String> = \TUC.2.y
}

protocol DefineSomeType {
  associatedtype SomeType
  func defineSome() -> SomeType
}

protocol A {}
extension Int : A {}

struct TestKeyPathWithSomeType : DefineSomeType {
  func defineSome() -> some A {
    return 0
  }

  func testKeyPathWithSome() {
    _ = \S<SomeType>.y
    _ = \S<SomeType>.z.x
    _ = \C<SomeType>.x
    _ = \C<SomeType>.y
    _ = \C<SomeType>.z.x
    _ = \C<SomeType>.z.z.y
    _ = \C<SomeType>.nonfinal
    _ = \C<SomeType>.computed
    _ = \C<SomeType>.observed
    _ = \C<SomeType>.nonfinal.x
    _ = \C<SomeType>.computed.x
    _ = \C<SomeType>.observed.x
    _ = \C<SomeType>.z.computed
    _ = \C<SomeType>.z.observed
    _ = \C<SomeType>.observed.x
    _ = \C<SomeType>.reabstracted
    _ = \S<SomeType>.computed
    _ = \S<SomeType>.observed
    _ = \S<SomeType>.z.nonfinal
    _ = \S<SomeType>.z.computed
    _ = \S<SomeType>.z.observed
    _ = \S<SomeType>.computed.x
    _ = \S<SomeType>.computed.y
    _ = \S<SomeType>.reabstracted

  }
}

struct N {
  static let kelvin = 293
}

class M {
  static var chanceRain = 10
  static let isSunny = true
  private(set) static var isCloudy = false
  static subscript(day: Int) -> String { "Monday" }
  subscript(temp: Int) -> N.Type { N.self }
  static subscript(kelvinTemp: Int) -> N.Type { N.self }
  var degrees: N.Type? { return N.self }
}

// CHECK-LABEL: // test_metatype_keypaths()
// CHECK-LABEL: sil hidden [ossa] @{{.*}} : $@convention(thin) () -> () {
func test_metatype_keypaths() {
  // CHECK: keypath $ReferenceWritableKeyPath<M.Type, Int>, (root $M.Type; settable_property $Int, id @$s8keypaths1MC10chanceRainSivgZ : $@convention(method) (@thick M.Type) -> Int, getter @$s8keypaths1MC10chanceRainSivpZACmTK : $@convention(keypath_accessor_getter) (@in_guaranteed @thick M.Type) -> @out Int, setter @$s8keypaths1MC10chanceRainSivpZACmTk : $@convention(keypath_accessor_setter) (@in_guaranteed Int, @in_guaranteed @thick M.Type) -> ()) // user: %1
  let _: KeyPath<M.Type, Int> = \M.Type.chanceRain
  // CHECK: keypath $KeyPath<M.Type, Bool>, (root $M.Type; gettable_property $Bool, id @$s8keypaths1MC7isSunnySbvgZ : $@convention(method) (@thick M.Type) -> Bool, getter @$s8keypaths1MC7isSunnySbvpZACmTK : $@convention(keypath_accessor_getter) (@in_guaranteed @thick M.Type) -> @out Bool) // user: %4
  let _: KeyPath<M.Type, Bool> = \M.Type.isSunny
  // CHECK: keypath $ReferenceWritableKeyPath<M.Type, Bool>, (root $M.Type; settable_property $Bool, id @$s8keypaths1MC8isCloudySbvgZ : $@convention(method) (@thick M.Type) -> Bool, getter @$s8keypaths1MC8isCloudySbvpZACmTK : $@convention(keypath_accessor_getter) (@in_guaranteed @thick M.Type) -> @out Bool, setter @$s8keypaths1MC8isCloudySbvpZACmTk : $@convention(keypath_accessor_setter) (@in_guaranteed Bool, @in_guaranteed @thick M.Type) -> ()) // user: %6
  let _: KeyPath<M.Type, Bool> = \M.Type.isCloudy
  // CHECK: keypath $KeyPath<M.Type, String>, (root $M.Type; gettable_property $String, id @$s8keypaths1MCySSSicigZ : $@convention(method) (Int, @thick M.Type) -> @owned String, getter @$s8keypaths1MCySSSicipZACmTK : $@convention(keypath_accessor_getter) (@in_guaranteed @thick M.Type, @in_guaranteed Int) -> @out String, indices [%$0 : $Int : $Int], indices_equals @$sSiTH : $@convention(keypath_accessor_equals) (@in_guaranteed Int, @in_guaranteed Int) -> Bool, indices_hash @$sSiTh : $@convention(keypath_accessor_hash) (@in_guaranteed Int) -> Int) (%11) // user: %13
  let _: KeyPath<M.Type, String> = \M.Type.[2]
  // CHECK: keypath $KeyPath<M, N.Type>, (root $M; gettable_property $N.Type, id #M.subscript!getter : (M) -> (Int) -> N.Type, getter @$s8keypaths1MCyAA1NVmSicipACTK : $@convention(keypath_accessor_getter) (@in_guaranteed M, @in_guaranteed Int) -> @out @thick N.Type, indices [%$0 : $Int : $Int], indices_equals @$sSiTH : $@convention(keypath_accessor_equals) (@in_guaranteed Int, @in_guaranteed Int) -> Bool, indices_hash @$sSiTh : $@convention(keypath_accessor_hash) (@in_guaranteed Int) -> Int) (%17) // user: %19
  let _: KeyPath<M, N.Type> = \M.[76]
  // CHECK: keypath $KeyPath<M.Type, N.Type>, (root $M.Type; gettable_property $N.Type, id @$s8keypaths1MCyAA1NVmSicigZ : $@convention(method) (Int, @thick M.Type) -> @thin N.Type, getter @$s8keypaths1MCyAA1NVmSicipZACmTK : $@convention(keypath_accessor_getter) (@in_guaranteed @thick M.Type, @in_guaranteed Int) -> @out @thick N.Type, indices [%$0 : $Int : $Int], indices_equals @$sSiTH : $@convention(keypath_accessor_equals) (@in_guaranteed Int, @in_guaranteed Int) -> Bool, indices_hash @$sSiTh : $@convention(keypath_accessor_hash) (@in_guaranteed Int) -> Int) (%23) // user: %25
  let _: KeyPath<M.Type, N.Type> = \M.Type.[76]
  // CHECK: keypath $KeyPath<M, Optional<Int>>, (root $M; gettable_property $Optional<N.Type>, id #M.degrees!getter : (M) -> () -> N.Type?, getter @$s8keypaths1MC7degreesAA1NVmSgvpACTK : $@convention(keypath_accessor_getter) (@in_guaranteed M) -> @out Optional<@thick N.Type>; optional_chain : $N.Type; gettable_property $Int, id @$s8keypaths1NV6kelvinSivgZ : $@convention(method) (@thin N.Type) -> Int, getter @$s8keypaths1NV6kelvinSivpZACmTK : $@convention(keypath_accessor_getter) (@in_guaranteed @thick N.Type) -> @out Int; optional_wrap : $Optional<Int>) // user: %27 
  let _: KeyPath<M, Int?> = \.degrees?.kelvin
}


// apple/swift#71423
protocol CodingKey {}

struct URICoderCodingKey : CodingKey {}

struct CodingStackEntry {
   var key: URICoderCodingKey
}

struct Test {
  var codingStack: [CodingStackEntry]
  // CHECK-LABEL: sil hidden [ossa] @{{.*}}codingPathAny
  var codingPathAny: [any CodingKey] { codingStack.map(\.key) }
  // CHECK: keypath $KeyPath<CodingStackEntry, URICoderCodingKey>, (root $CodingStackEntry; stored_property #CodingStackEntry.key : $URICoderCodingKey)

  // CHECK-LABEL: sil hidden [ossa] @{{.*}}codingPathOpt
  var codingPathOpt: [URICoderCodingKey?] { codingStack.map(\.key) }
  // CHECK: keypath $KeyPath<CodingStackEntry, URICoderCodingKey>, (root $CodingStackEntry; stored_property #CodingStackEntry.key : $URICoderCodingKey)
}

// rdar://123638701 - Make sure that optional chaining forces loads.
func test_optional_chaining_with_function_conversion() {
  class Storage {}

  class Elements {
    var db: Storage = Storage()
  }

  class Source {
    var elements: Elements? = nil
  }

  func test(data: [Source]) {
    // CHECK: {{.*}} = keypath $KeyPath<Source, Optional<Storage>>
    _ = data.compactMap(\.elements?.db)
    // CHECK: {{.*}} = keypath $KeyPath<Source, Storage>
    _ = data.compactMap(\.elements!.db)
  }
}
