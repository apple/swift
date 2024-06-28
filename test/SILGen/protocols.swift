
// RUN: %target-swift-emit-silgen -module-name protocols %s | %FileCheck %s

//===----------------------------------------------------------------------===//
// Calling Existential Subscripts
//===----------------------------------------------------------------------===//

protocol SubscriptableGet {
  subscript(a : Int) -> Int { get }
}

protocol SubscriptableGetSet {
  subscript(a : Int) -> Int { get set }
}

var subscriptableGet : SubscriptableGet
var subscriptableGetSet : SubscriptableGetSet

func use_subscript_rvalue_get(_ i : Int) -> Int {
  return subscriptableGet[i]
}

// CHECK-LABEL: sil hidden [ossa] @{{.*}}use_subscript_rvalue_get
// CHECK: bb0(%0 : $Int):
// CHECK: [[GLOB:%[0-9]+]] = global_addr @$s9protocols16subscriptableGetAA013SubscriptableC0_pvp : $*any SubscriptableGet
// CHECK: [[READ:%.*]] = begin_access [read] [dynamic] [[GLOB]] : $*any SubscriptableGet
// CHECK: [[PROJ:%[0-9]+]] = open_existential_addr immutable_access [[READ]] : $*any SubscriptableGet to $*[[OPENED:@opened\(.*, any SubscriptableGet\) Self]]
// CHECK: [[ALLOCSTACK:%[0-9]+]] = alloc_stack $[[OPENED]]
// CHECK: copy_addr [[PROJ]] to [init] [[ALLOCSTACK]] : $*[[OPENED]]
// CHECK-NEXT: end_access [[READ]] : $*any SubscriptableGet
// CHECK-NEXT: [[METH:%[0-9]+]] = witness_method $[[OPENED]], #SubscriptableGet.subscript!getter
// CHECK-NEXT: [[RESULT:%[0-9]+]] = apply [[METH]]<[[OPENED]]>(%0, [[ALLOCSTACK]])
// CHECK-NEXT: destroy_addr [[ALLOCSTACK]]
// CHECK-NEXT: dealloc_stack [[ALLOCSTACK]] : $*[[OPENED]]
// CHECK-NEXT: return [[RESULT]]

func use_subscript_lvalue_get(_ i : Int) -> Int {
  return subscriptableGetSet[i]
}

// CHECK-LABEL: sil hidden [ossa] @{{.*}}use_subscript_lvalue_get
// CHECK: bb0(%0 : $Int):
// CHECK: [[GLOB:%[0-9]+]] = global_addr @$s9protocols19subscriptableGetSetAA013SubscriptablecD0_pvp : $*any SubscriptableGetSet
// CHECK: [[READ:%.*]] = begin_access [read] [dynamic] [[GLOB]] : $*any SubscriptableGetSet
// CHECK: [[PROJ:%[0-9]+]] = open_existential_addr immutable_access [[READ]] : $*any SubscriptableGetSet to $*[[OPENED:@opened\(.*, any SubscriptableGetSet\) Self]]
// CHECK-NEXT: [[METH:%[0-9]+]] = witness_method $[[OPENED]], #SubscriptableGetSet.subscript!getter
// CHECK-NEXT: [[RESULT:%[0-9]+]] = apply [[METH]]<[[OPENED]]>(%0, [[PROJ]])
// CHECK-NEXT: end_access [[READ]] : $*any SubscriptableGetSet
// CHECK-NEXT: return [[RESULT]]

func use_subscript_lvalue_set(_ i : Int) {
  subscriptableGetSet[i] = i
}

// CHECK-LABEL: sil hidden [ossa] @{{.*}}use_subscript_lvalue_set
// CHECK: bb0(%0 : $Int):
// CHECK: [[GLOB:%[0-9]+]] = global_addr @$s9protocols19subscriptableGetSetAA013SubscriptablecD0_pvp : $*any SubscriptableGetSet
// CHECK: [[READ:%.*]] = begin_access [modify] [dynamic] [[GLOB]] : $*any SubscriptableGetSet
// CHECK: [[PROJ:%[0-9]+]] = open_existential_addr mutable_access [[READ]] : $*any SubscriptableGetSet to $*[[OPENED:@opened\(.*, any SubscriptableGetSet\) Self]]
// CHECK-NEXT: [[METH:%[0-9]+]] = witness_method $[[OPENED]], #SubscriptableGetSet.subscript!setter
// CHECK-NEXT: apply [[METH]]<[[OPENED]]>(%0, %0, [[PROJ]])


//===----------------------------------------------------------------------===//
// Calling Archetype Subscripts
//===----------------------------------------------------------------------===//

func use_subscript_archetype_rvalue_get<T : SubscriptableGet>(_ generic : T, idx : Int) -> Int {
  return generic[idx]
}
// CHECK-LABEL: sil hidden [ossa] @{{.*}}use_subscript_archetype_rvalue_get
// CHECK: bb0(%0 : $*T, %1 : $Int):
// CHECK: [[METH:%[0-9]+]] = witness_method $T, #SubscriptableGet.subscript!getter
// CHECK-NEXT: apply [[METH]]<T>(%1, %0)
// CHECK: } // end sil function '${{.*}}use_subscript_archetype_rvalue_get


func use_subscript_archetype_lvalue_get<T : SubscriptableGetSet>(_ generic: inout T, idx : Int) -> Int {
  return generic[idx]
}
// CHECK-LABEL: sil hidden [ossa] @{{.*}}use_subscript_archetype_lvalue_get
// CHECK: bb0(%0 : $*T, %1 : $Int):
// CHECK: [[READ:%.*]] = begin_access [read] [unknown] %0 : $*T
// CHECK: [[METH:%[0-9]+]] = witness_method $T, #SubscriptableGetSet.subscript!getter
// CHECK-NEXT: [[APPLYRESULT:%[0-9]+]] = apply [[METH]]<T>(%1, [[READ]])
// CHECK-NEXT: end_access [[READ]]
// CHECK: return [[APPLYRESULT]]


func use_subscript_archetype_lvalue_set<T : SubscriptableGetSet>(_ generic: inout T, idx : Int) {
  generic[idx] = idx
}
// CHECK-LABEL: sil hidden [ossa] @{{.*}}use_subscript_archetype_lvalue_set
// CHECK: bb0(%0 : $*T, %1 : $Int):
// CHECK: [[WRITE:%.*]] = begin_access [modify] [unknown] %0 : $*T
// CHECK: [[METH:%[0-9]+]] = witness_method $T, #SubscriptableGetSet.subscript!setter
// CHECK-NEXT: apply [[METH]]<T>(%1, %1, [[WRITE]])


//===----------------------------------------------------------------------===//
// Calling Existential Properties
//===----------------------------------------------------------------------===//

protocol PropertyWithGetter {
  var a : Int { get }
}

protocol PropertyWithGetterSetter {
  var b : Int { get set }
}


var propertyGet : PropertyWithGetter
var propertyGetSet : PropertyWithGetterSetter

func use_property_rvalue_get() -> Int {
  return propertyGet.a
}
// CHECK-LABEL: sil hidden [ossa] @{{.*}}use_property_rvalue_get
// CHECK: [[GLOB:%[0-9]+]] = global_addr @$s9protocols11propertyGetAA18PropertyWithGetter_pvp : $*any PropertyWithGetter
// CHECK: [[READ:%.*]] = begin_access [read] [dynamic] [[GLOB]] : $*any PropertyWithGetter
// CHECK: [[PROJ:%[0-9]+]] = open_existential_addr immutable_access [[READ]] : $*any PropertyWithGetter to $*[[OPENED:@opened\(.*, any PropertyWithGetter\) Self]]
// CHECK: [[COPY:%.*]] = alloc_stack $[[OPENED]]
// CHECK-NEXT: copy_addr [[PROJ]] to [init] [[COPY]] : $*[[OPENED]]
// CHECK-NEXT: end_access [[READ]] : $*any PropertyWithGetter
// CHECK-NEXT: [[METH:%[0-9]+]] = witness_method $[[OPENED]], #PropertyWithGetter.a!getter
// CHECK-NEXT: apply [[METH]]<[[OPENED]]>([[COPY]])

func use_property_lvalue_get() -> Int {
  return propertyGetSet.b
}
// CHECK-LABEL: sil hidden [ossa] @{{.*}}use_property_lvalue_get
// CHECK: [[GLOB:%[0-9]+]] = global_addr @$s9protocols14propertyGetSetAA24PropertyWithGetterSetter_pvp : $*any PropertyWithGetterSetter
// CHECK: [[READ:%.*]] = begin_access [read] [dynamic] [[GLOB]] : $*any PropertyWithGetterSetter
// CHECK: [[PROJ:%[0-9]+]] = open_existential_addr immutable_access [[READ]] : $*any PropertyWithGetterSetter to $*[[OPENED:@opened\(.*, any PropertyWithGetterSetter\) Self]]
// CHECK-NEXT: [[METH:%[0-9]+]] = witness_method $[[OPENED]], #PropertyWithGetterSetter.b!getter
// CHECK: apply [[METH]]<[[OPENED]]>([[PROJ]])

func use_property_lvalue_set(_ x : Int) {
  propertyGetSet.b = x
}

// CHECK-LABEL: sil hidden [ossa] @{{.*}}use_property_lvalue_set
// CHECK: bb0(%0 : $Int):
// CHECK: [[GLOB:%[0-9]+]] = global_addr @$s9protocols14propertyGetSetAA24PropertyWithGetterSetter_pvp : $*any PropertyWithGetterSetter
// CHECK: [[READ:%.*]] = begin_access [modify] [dynamic] [[GLOB]] : $*any PropertyWithGetterSetter
// CHECK: [[PROJ:%[0-9]+]] = open_existential_addr mutable_access [[READ]] : $*any PropertyWithGetterSetter to $*[[OPENED:@opened\(.*, any PropertyWithGetterSetter\) Self]]
// CHECK-NEXT: [[METH:%[0-9]+]] = witness_method $[[OPENED]], #PropertyWithGetterSetter.b!setter
// CHECK-NEXT: apply [[METH]]<[[OPENED]]>(%0, [[PROJ]])

//===----------------------------------------------------------------------===//
// Calling Archetype Properties
//===----------------------------------------------------------------------===//

func use_property_archetype_rvalue_get<T : PropertyWithGetter>(_ generic : T) -> Int {
  return generic.a
}

// CHECK-LABEL: sil hidden [ossa] @{{.*}}use_property_archetype_rvalue_get
// CHECK: bb0(%0 : $*T):
// CHECK: [[METH:%[0-9]+]] = witness_method $T, #PropertyWithGetter.a!getter
// CHECK-NEXT: apply [[METH]]<T>(%0)
// CHECK: } // end sil function '{{.*}}use_property_archetype_rvalue_get


func use_property_archetype_lvalue_get<T : PropertyWithGetterSetter>(_ generic : T) -> Int {
  return generic.b
}

// CHECK-LABEL: sil hidden [ossa] @{{.*}}use_property_archetype_lvalue_get
// CHECK: bb0(%0 : $*T):
// CHECK: [[METH:%[0-9]+]] = witness_method $T, #PropertyWithGetterSetter.b!getter
// CHECK-NEXT: apply [[METH]]<T>(%0)
// CHECK: } // end sil function '${{.*}}use_property_archetype_lvalue_get


func use_property_archetype_lvalue_set<T : PropertyWithGetterSetter>(_ generic: inout T, v : Int) {
  generic.b = v
}
// CHECK-LABEL: sil hidden [ossa] @{{.*}}use_property_archetype_lvalue_set
// CHECK: bb0(%0 : $*T, %1 : $Int):
// CHECK: [[WRITE:%.*]] = begin_access [modify] [unknown] %0 : $*T
// CHECK: [[METH:%[0-9]+]] = witness_method $T, #PropertyWithGetterSetter.b!setter
// CHECK-NEXT: apply [[METH]]<T>(%1, [[WRITE]])

//===----------------------------------------------------------------------===//
// Calling Initializers
//===----------------------------------------------------------------------===//
protocol Initializable {
  init(int: Int)
}

// CHECK-LABEL: sil hidden [ossa] @$s9protocols27use_initializable_archetype{{[_0-9a-zA-Z]*}}F
func use_initializable_archetype<T: Initializable>(_ t: T, i: Int) {
  // CHECK:   [[T_RESULT:%[0-9]+]] = alloc_stack $T
  // CHECK:   [[T_META:%[0-9]+]] = metatype $@thick T.Type
  // CHECK:   [[T_INIT:%[0-9]+]] = witness_method $T, #Initializable.init!allocator : {{.*}} : $@convention(witness_method: Initializable) <τ_0_0 where τ_0_0 : Initializable> (Int, @thick τ_0_0.Type) -> @out τ_0_0
  // CHECK:   [[T_RESULT_ADDR:%[0-9]+]] = apply [[T_INIT]]<T>([[T_RESULT]], %1, [[T_META]]) : $@convention(witness_method: Initializable) <τ_0_0 where τ_0_0 : Initializable> (Int, @thick τ_0_0.Type) -> @out τ_0_0
  // CHECK:   destroy_addr [[T_RESULT]] : $*T
  // CHECK:   dealloc_stack [[T_RESULT]] : $*T
  // CHECK:   [[RESULT:%[0-9]+]] = tuple ()
  // CHECK:   return [[RESULT]] : $()
  T(int: i)
}

// CHECK: sil hidden [ossa] @$s9protocols29use_initializable_existential{{[_0-9a-zA-Z]*}}F
func use_initializable_existential(_ im: Initializable.Type, i: Int) {
// CHECK: bb0([[IM:%[0-9]+]] : $@thick any Initializable.Type, [[I:%[0-9]+]] : $Int):
// CHECK:   [[ARCHETYPE_META:%[0-9]+]] = open_existential_metatype [[IM]] : $@thick any Initializable.Type to $@thick (@opened([[N:".*"]], any Initializable) Self).Type
// CHECK:   [[TEMP_VALUE:%[0-9]+]] = alloc_stack $any Initializable
// CHECK:   [[INIT_WITNESS:%[0-9]+]] = witness_method $@opened([[N]], any Initializable) Self, #Initializable.init!allocator : {{.*}}, [[ARCHETYPE_META]]{{.*}} : $@convention(witness_method: Initializable) <τ_0_0 where τ_0_0 : Initializable> (Int, @thick τ_0_0.Type) -> @out τ_0_0
// CHECK:   [[TEMP_ADDR:%[0-9]+]] = init_existential_addr [[TEMP_VALUE]] : $*any Initializable, $@opened([[N]], any Initializable) Self
// CHECK:   [[INIT_RESULT:%[0-9]+]] = apply [[INIT_WITNESS]]<@opened([[N]], any Initializable) Self>([[TEMP_ADDR]], [[I]], [[ARCHETYPE_META]]) : $@convention(witness_method: Initializable) <τ_0_0 where τ_0_0 : Initializable> (Int, @thick τ_0_0.Type) -> @out τ_0_0
// CHECK:   destroy_addr [[TEMP_VALUE]] : $*any Initializable
// CHECK:   dealloc_stack [[TEMP_VALUE]] : $*any Initializable
  im.init(int: i)
// CHECK:   [[RESULT:%[0-9]+]] = tuple ()
// CHECK:   return [[RESULT]] : $()
}

//===----------------------------------------------------------------------===//
// Protocol conformance and witness table generation
//===----------------------------------------------------------------------===//

class ClassWithGetter : PropertyWithGetter {
  var a: Int {
    get {
      return 42
    }
  }
}

// Make sure we are generating a protocol witness that calls the class method on
// ClassWithGetter.
// CHECK-LABEL: sil private [transparent] [thunk] [ossa] @$s9protocols15ClassWithGetterCAA08PropertycD0A2aDP1aSivgTW :
// CHECK: bb0([[C:%.*]] : $*ClassWithGetter):
// CHECK-NEXT: [[CCOPY_LOADED:%.*]] = load_borrow %0
// CHECK-NEXT: [[FUN:%.*]] = class_method [[CCOPY_LOADED]] : $ClassWithGetter, #ClassWithGetter.a!getter : (ClassWithGetter) -> () -> Int, $@convention(method) (@guaranteed ClassWithGetter) -> Int
// CHECK-NEXT: apply [[FUN]]([[CCOPY_LOADED]])
// CHECK-NEXT: end_borrow [[CCOPY_LOADED]]
// CHECK-NEXT: return

class ClassWithGetterSetter : PropertyWithGetterSetter, PropertyWithGetter {
  var a: Int {
    get {
      return 1
    }
    set {}
  }
  var b: Int {
    get {
      return 2
    }
    set {}
  }
}

// CHECK-LABEL: sil private [transparent] [thunk] [ossa] @$s9protocols21ClassWithGetterSetterCAA08PropertycdE0A2aDP1bSivgTW :
// CHECK: bb0([[C:%.*]] : $*ClassWithGetterSetter):
// CHECK-NEXT: [[CCOPY_LOADED:%.*]] = load_borrow %0
// CHECK-NEXT: [[FUN:%.*]] = class_method [[CCOPY_LOADED]] : $ClassWithGetterSetter, #ClassWithGetterSetter.b!getter : (ClassWithGetterSetter) -> () -> Int, $@convention(method) (@guaranteed ClassWithGetterSetter) -> Int
// CHECK-NEXT: apply [[FUN]]([[CCOPY_LOADED]])
// CHECK-NEXT: end_borrow [[CCOPY_LOADED]]
// CHECK-NEXT: return

// Stored variables fulfilling property requirements
//
class ClassWithStoredProperty : PropertyWithGetter {
  var a : Int = 0

  // Make sure that accesses go through the generated accessors for classes.
  func methodUsingProperty() -> Int {
    return a
  }
  // CHECK-LABEL: sil hidden [ossa] @$s9protocols23ClassWithStoredPropertyC011methodUsingE0SiyF
  // CHECK: bb0([[ARG:%.*]] : @guaranteed $ClassWithStoredProperty):
  // CHECK-NEXT: debug_value [[ARG]]
  // CHECK-NOT: copy_value
  // CHECK-NEXT: [[FUN:%.*]] = class_method [[ARG]] : $ClassWithStoredProperty, #ClassWithStoredProperty.a!getter : (ClassWithStoredProperty) -> () -> Int, $@convention(method) (@guaranteed ClassWithStoredProperty) -> Int
  // CHECK-NEXT: [[RESULT:%.*]] = apply [[FUN]]([[ARG]])
  // CHECK-NOT: destroy_value
  // CHECK-NEXT: return [[RESULT]] : $Int
}

struct StructWithStoredProperty : PropertyWithGetter {
  var a : Int

  // Make sure that accesses aren't going through the generated accessors.
  func methodUsingProperty() -> Int {
    return a
  }
  // CHECK-LABEL: sil hidden [ossa] @$s9protocols24StructWithStoredPropertyV011methodUsingE0SiyF
  // CHECK: bb0(%0 : $StructWithStoredProperty):
  // CHECK-NEXT: debug_value %0
  // CHECK-NEXT: %2 = struct_extract %0 : $StructWithStoredProperty, #StructWithStoredProperty.a
  // CHECK-NEXT: return %2 : $Int
}

// Make sure that we generate direct function calls for out struct protocol
// witness since structs don't do virtual calls for methods.
//
// *NOTE* Even though at first glance the copy_addr looks like a leak
// here, StructWithStoredProperty is a trivial struct implying that no
// leak is occurring. See the test with StructWithStoredClassProperty
// that makes sure in such a case we don't leak. This is due to the
// thunking code being too dumb but it is harmless to program
// correctness.
//
// CHECK-LABEL: sil private [transparent] [thunk] [ossa] @$s9protocols24StructWithStoredPropertyVAA0eC6GetterA2aDP1aSivgTW :
// CHECK: bb0([[C:%.*]] : $*StructWithStoredProperty):
// CHECK-NEXT: [[CCOPY_LOADED:%.*]] = load [trivial] [[C]]
// CHECK-NEXT: function_ref
// CHECK-NEXT: [[FUN:%.*]] = function_ref @$s9protocols24StructWithStoredPropertyV1aSivg : $@convention(method) (StructWithStoredProperty) -> Int
// CHECK-NEXT: apply [[FUN]]([[CCOPY_LOADED]])
// CHECK-NEXT: return

class C {}

// Make sure that if the getter has a class property, we pass it in
// in_guaranteed and don't leak.
struct StructWithStoredClassProperty : PropertyWithGetter {
  var a : Int
  var c: C = C()

  // Make sure that accesses aren't going through the generated accessors.
  func methodUsingProperty() -> Int {
    return a
  }
  // CHECK-LABEL: sil hidden [ossa] @$s9protocols29StructWithStoredClassPropertyV011methodUsingF0SiyF
  // CHECK: bb0(%0 : @guaranteed $StructWithStoredClassProperty):
  // CHECK-NEXT: debug_value %0
  // CHECK-NEXT: %2 = struct_extract %0 : $StructWithStoredClassProperty, #StructWithStoredClassProperty.a
  // CHECK-NEXT: return %2 : $Int
}

// CHECK-LABEL: sil private [transparent] [thunk] [ossa] @$s9protocols29StructWithStoredClassPropertyVAA0fC6GetterA2aDP1aSivgTW :
// CHECK: bb0([[C:%.*]] : $*StructWithStoredClassProperty):
// CHECK-NEXT: [[CCOPY_LOADED:%.*]] = load_borrow [[C]]
// CHECK-NEXT: function_ref
// CHECK-NEXT: [[FUN:%.*]] = function_ref @$s9protocols29StructWithStoredClassPropertyV1aSivg : $@convention(method) (@guaranteed StructWithStoredClassProperty) -> Int
// CHECK-NEXT: apply [[FUN]]([[CCOPY_LOADED]])
// CHECK-NEXT: end_borrow [[CCOPY_LOADED]]
// CHECK-NEXT: return

// rdar://22676810

protocol ExistentialProperty {
  var p: PropertyWithGetterSetter { get set }
}

func testExistentialPropertyRead<T: ExistentialProperty>(_ t: inout T) {
    let b = t.p.b
}
// CHECK-LABEL: sil hidden [ossa] @$s9protocols27testExistentialPropertyRead{{[_0-9a-zA-Z]*}}F
// CHECK:      [[READ:%.*]] = begin_access [read] [unknown] %0 : $*T
// CHECK:      [[P_TEMP:%.*]] = alloc_stack $any PropertyWithGetterSetter
// CHECK:      [[P_GETTER:%.*]] = witness_method $T, #ExistentialProperty.p!getter :
// CHECK-NEXT: apply [[P_GETTER]]<T>([[P_TEMP]], [[READ]])
// CHECK-NEXT: [[OPEN:%.*]] = open_existential_addr immutable_access [[P_TEMP]] : $*any PropertyWithGetterSetter to $*[[P_OPENED:@opened\(.*, any PropertyWithGetterSetter\) Self]]
// CHECK-NEXT: [[B_GETTER:%.*]] = witness_method $[[P_OPENED]], #PropertyWithGetterSetter.b!getter
// CHECK-NEXT: apply [[B_GETTER]]<[[P_OPENED]]>([[OPEN]])
// CHECK-NEXT: debug_value
// CHECK-NOT:  witness_method
// CHECK:      return

func modify(_ x: inout Int) {}

func modifyProperty<T : PropertyWithGetterSetter>(_ x: inout T) {
  modify(&x.b)
}
// CHECK-LABEL: sil hidden [ossa] @$s9protocols14modifyPropertyyyxzAA0C16WithGetterSetterRzlF
// CHECK:      [[WRITE:%.*]] = begin_access [modify] [unknown] %0 : $*T
// CHECK:      [[WITNESS_FN:%.*]] = witness_method $T, #PropertyWithGetterSetter.b!modify
// CHECK:      ([[ADDR:%.*]], [[TOKEN:%.*]]) = begin_apply [[WITNESS_FN]]<T>
// CHECK:      [[MODIFY_FN:%.*]] = function_ref @$s9protocols6modifyyySizF
// CHECK:      apply [[MODIFY_FN]]([[ADDR]])
// CHECK:      end_apply [[TOKEN]]

public struct Val {
  public var x: Int = 0
}

public protocol Proto {
  var val: Val { get nonmutating set}
}

public func test(_ p: Proto) {
  p.val.x += 1
}

// CHECK-LABEL: sil [ossa] @$s9protocols4testyyAA5Proto_pF : $@convention(thin) (@in_guaranteed any Proto) -> ()
// CHECK: [[OPEN:%.*]] = open_existential_addr immutable_access
// CHECK: [[MAT:%.*]] = witness_method $@opened("{{.*}}", any Proto) Self, #Proto.val!modify
// CHECK: ([[BUF:%.*]], [[TOKEN:%.*]]) = begin_apply [[MAT]]
// CHECK: end_apply [[TOKEN]]
// CHECK: return

// https://github.com/apple/swift/issues/54155

protocol SelfReturningSubscript {
  subscript(b: Int) -> Self { get }
}

public func testSelfReturningSubscript() {
  // CHECK-LABEL: sil private [ossa] @$s9protocols26testSelfReturningSubscriptyyFAA0cdE0_pAaC_pXEfU_
  // CHECK: [[OPEN:%.*]] = open_existential_addr immutable_access
  // CHECK: [[WIT_M:%.*]] = witness_method $@opened("{{.*}}", any SelfReturningSubscript) Self, #SelfReturningSubscript.subscript!getter
  // CHECK: apply [[WIT_M]]<@opened("{{.*}}", any SelfReturningSubscript) Self>({{%.*}}, {{%.*}}, [[OPEN]])
  _ = [String: SelfReturningSubscript]().mapValues { $0[2] }
}

// CHECK-LABEL: sil_witness_table hidden ClassWithGetter: PropertyWithGetter module protocols {
// CHECK-NEXT:  method #PropertyWithGetter.a!getter: {{.*}} : @$s9protocols15ClassWithGetterCAA08PropertycD0A2aDP1aSivgTW
// CHECK-NEXT: }

// CHECK-LABEL: sil_witness_table hidden ClassWithGetterSetter: PropertyWithGetterSetter module protocols {
// CHECK-NEXT:  method #PropertyWithGetterSetter.b!getter: {{.*}} : @$s9protocols21ClassWithGetterSetterCAA08PropertycdE0A2aDP1bSivgTW
// CHECK-NEXT:  method #PropertyWithGetterSetter.b!setter: {{.*}} : @$s9protocols21ClassWithGetterSetterCAA08PropertycdE0A2aDP1bSivsTW
// CHECK-NEXT:  method #PropertyWithGetterSetter.b!modify: {{.*}} : @$s9protocols21ClassWithGetterSetterCAA08PropertycdE0A2aDP1bSivMTW
// CHECK-NEXT: }

// CHECK-LABEL: sil_witness_table hidden ClassWithGetterSetter: PropertyWithGetter module protocols {
// CHECK-NEXT:  method #PropertyWithGetter.a!getter: {{.*}} : @$s9protocols21ClassWithGetterSetterCAA08PropertycD0A2aDP1aSivgTW
// CHECK-NEXT: }

// CHECK-LABEL: sil_witness_table hidden StructWithStoredProperty: PropertyWithGetter module protocols {
// CHECK-NEXT:  method #PropertyWithGetter.a!getter: {{.*}} : @$s9protocols24StructWithStoredPropertyVAA0eC6GetterA2aDP1aSivgTW
// CHECK-NEXT: }

// CHECK-LABEL: sil_witness_table hidden StructWithStoredClassProperty: PropertyWithGetter module protocols {
// CHECK-NEXT:  method #PropertyWithGetter.a!getter: {{.*}} : @$s9protocols29StructWithStoredClassPropertyVAA0fC6GetterA2aDP1aSivgTW
// CHECK-NEXT: }
