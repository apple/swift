// RUN: %target-swift-frontend %use_no_opaque_pointers -module-name class_bounded_generics -enable-objc-interop -emit-ir -primary-file %s -disable-objc-attr-requires-foundation-module | %FileCheck %s -DINT=i%target-ptrsize
// RUN: %target-swift-frontend -module-name class_bounded_generics -enable-objc-interop -emit-ir -primary-file %s -disable-objc-attr-requires-foundation-module

// REQUIRES: CPU=x86_64

protocol ClassBound : class {
  func classBoundMethod()
}
protocol ClassBound2 : class {
  func classBoundMethod2()
}
protocol ClassBoundBinary : ClassBound {
  func classBoundBinaryMethod(_ x: Self)
}
@objc protocol ObjCClassBound {
  func objCClassBoundMethod()
}
@objc protocol ObjCClassBound2 {
  func objCClassBoundMethod2()
}
protocol NotClassBound {
  func notClassBoundMethod()
  func notClassBoundBinaryMethod(_ x: Self)
}

struct ClassGenericFieldStruct<T:ClassBound> {
  var x : Int
  var y : T
  var z : Int
}

struct ClassProtocolFieldStruct {
  var x : Int
  var y : ClassBound
  var z : Int
}

class ClassGenericFieldClass<T:ClassBound> {
  final var x : Int = 0
  final var y : T
  final var z : Int = 0

  init(t: T) {
    y = t
  }
}

class ClassProtocolFieldClass {
  var x : Int = 0
  var y : ClassBound
  var z : Int = 0

  init(classBound cb: ClassBound) {
    y = cb
  }
}

// CHECK-DAG: %T22class_bounded_generics017ClassGenericFieldD0C = type <{ %swift.refcounted, %TSi, %objc_object*, %TSi }>
// CHECK-DAG: %T22class_bounded_generics23ClassGenericFieldStructV = type <{ %TSi, %objc_object*, %TSi }>
// CHECK-DAG: %T22class_bounded_generics24ClassProtocolFieldStructV = type <{ %TSi, %T22class_bounded_generics10ClassBoundP, %TSi }>

// CHECK-LABEL: define hidden swiftcc %objc_object* @"$s22class_bounded_generics0a1_B10_archetype{{[_0-9a-zA-Z]*}}F"(%objc_object* %0, %swift.type* %T, i8** %T.ClassBound)
func class_bounded_archetype<T : ClassBound>(_ x: T) -> T {
  return x
}

class SomeClass {}
class SomeSubclass : SomeClass {}

// CHECK-LABEL: define hidden swiftcc %T22class_bounded_generics9SomeClassC* @"$s22class_bounded_generics011superclass_B10_archetype{{[_0-9a-zA-Z]*}}F"(%T22class_bounded_generics9SomeClassC* %0, %swift.type* %T)
func superclass_bounded_archetype<T : SomeClass>(_ x: T) -> T {
  return x
}

// CHECK-LABEL: define hidden swiftcc { %T22class_bounded_generics9SomeClassC*, %T22class_bounded_generics12SomeSubclassC* } @"$s22class_bounded_generics011superclass_B15_archetype_call{{[_0-9a-zA-Z]*}}F"(%T22class_bounded_generics9SomeClassC* %0, %T22class_bounded_generics12SomeSubclassC* %1)
func superclass_bounded_archetype_call(_ x: SomeClass, y: SomeSubclass) -> (SomeClass, SomeSubclass) {
  return (superclass_bounded_archetype(x),
          superclass_bounded_archetype(y));
  // CHECK: [[SOMECLASS_RESULT:%.*]] = call swiftcc %T22class_bounded_generics9SomeClassC* @"$s22class_bounded_generics011superclass_B10_archetype{{[_0-9a-zA-Z]*}}F"(%T22class_bounded_generics9SomeClassC* {{%.*}}, {{.*}})
  // CHECK: [[SOMESUPERCLASS_IN:%.*]] = bitcast %T22class_bounded_generics12SomeSubclassC* {{%.*}} to %T22class_bounded_generics9SomeClassC*
  // CHECK: [[SOMESUPERCLASS_RESULT:%.*]] = call swiftcc %T22class_bounded_generics9SomeClassC* @"$s22class_bounded_generics011superclass_B10_archetype{{[_0-9a-zA-Z]*}}F"(%T22class_bounded_generics9SomeClassC* [[SOMESUPERCLASS_IN]], {{.*}})
  // CHECK: bitcast %T22class_bounded_generics9SomeClassC* [[SOMESUPERCLASS_RESULT]] to %T22class_bounded_generics12SomeSubclassC*
}

// CHECK-LABEL: define hidden swiftcc void @"$s22class_bounded_generics0a1_B17_archetype_method{{[_0-9a-zA-Z]*}}F"(%objc_object* %0, %objc_object* %1, %swift.type* %T, i8** %T.ClassBoundBinary)
func class_bounded_archetype_method<T : ClassBoundBinary>(_ x: T, y: T) {
  x.classBoundMethod()
  // CHECK: [[INHERITED_GEP:%.*]] = getelementptr inbounds i8*, i8** %T.ClassBoundBinary, i32 1
  // CHECK: [[INHERITED:%.*]] = load i8*, i8** [[INHERITED_GEP]]
  // CHECK: [[INHERITED_WTBL:%.*]] = bitcast i8* [[INHERITED]] to i8**
  // CHECK: [[WITNESS_GEP:%.*]] = getelementptr inbounds i8*, i8** [[INHERITED_WTBL]], i32 1
  // CHECK: [[WITNESS:%.*]] = load i8*, i8** [[WITNESS_GEP]], align 8
  // CHECK: [[WITNESS_FUNC:%.*]] = bitcast i8* [[WITNESS]] to void (%objc_object*, %swift.type*, i8**)
  // CHECK: call swiftcc void [[WITNESS_FUNC]](%objc_object* swiftself %0, %swift.type* {{.*}}, i8** [[INHERITED_WTBL]])
  x.classBoundBinaryMethod(y)
  // CHECK-NOT: call %objc_object* @swift_unknownObjectRetain(%objc_object* returned [[Y:%.*]])
  // CHECK: [[WITNESS_ENTRY:%.*]] = getelementptr inbounds i8*, i8** %T.ClassBoundBinary, i32 2
  // CHECK: [[WITNESS:%.*]] = load i8*, i8** [[WITNESS_ENTRY]], align 8
  // CHECK: [[WITNESS_FUNC:%.*]] = bitcast i8* [[WITNESS]] to void (%objc_object*, %objc_object*, %swift.type*, i8**)
  // CHECK: call swiftcc void [[WITNESS_FUNC]](%objc_object* %1, %objc_object* swiftself %0, %swift.type* %T, i8** %T.ClassBoundBinary)
}

// CHECK-LABEL: define hidden swiftcc { %objc_object*, %objc_object* } @"$s22class_bounded_generics0a1_B16_archetype_tuple{{[_0-9a-zA-Z]*}}F"(%objc_object* %0, %swift.type* %T, i8** %T.ClassBound)
func class_bounded_archetype_tuple<T : ClassBound>(_ x: T) -> (T, T) {
  return (x, x)
}

class ConcreteClass : ClassBoundBinary, NotClassBound {
  func classBoundMethod() {}
  func classBoundBinaryMethod(_ x: ConcreteClass) {}
  func notClassBoundMethod() {}
  func notClassBoundBinaryMethod(_ x: ConcreteClass) {}
}

// CHECK-LABEL: define hidden swiftcc %T22class_bounded_generics13ConcreteClassC* @"$s22class_bounded_generics05call_a1_B10_archetype{{[_0-9a-zA-Z]*}}F"(%T22class_bounded_generics13ConcreteClassC* %0) {{.*}} {
func call_class_bounded_archetype(_ x: ConcreteClass) -> ConcreteClass {
  return class_bounded_archetype(x)
  // CHECK: [[IN:%.*]] = bitcast %T22class_bounded_generics13ConcreteClassC* {{%.*}} to %objc_object*
  // CHECK: [[OUT_ORIG:%.*]] = call swiftcc %objc_object* @"$s22class_bounded_generics0a1_B10_archetype{{[_0-9a-zA-Z]*}}F"(%objc_object* [[IN]], {{.*}})
  // CHECK: [[OUT:%.*]] = bitcast %objc_object* [[OUT_ORIG]] to %T22class_bounded_generics13ConcreteClassC*
  // CHECK: ret %T22class_bounded_generics13ConcreteClassC* [[OUT]]
}

// CHECK: define hidden swiftcc void @"$s22class_bounded_generics04not_a1_B10_archetype{{[_0-9a-zA-Z]*}}F"(%swift.opaque* noalias nocapture sret({{.*}}) %0, %swift.opaque* noalias nocapture %1, %swift.type* %T, i8** %T.NotClassBound)
func not_class_bounded_archetype<T : NotClassBound>(_ x: T) -> T {
  return x
}

// CHECK-LABEL: define hidden swiftcc %objc_object* @"$s22class_bounded_generics0a1_b18_archetype_to_not_a1_B0{{[_0-9a-zA-Z]*}}F"(%objc_object* %0, %swift.type* %T, i8** %T.ClassBound, i8** %T.NotClassBound) {{.*}} {
func class_bounded_archetype_to_not_class_bounded
<T: ClassBound & NotClassBound>(_ x:T) -> T {
  // CHECK: alloca %objc_object*, align 8
  return not_class_bounded_archetype(x)
}

/* TODO Abstraction remapping to non-class-bounded witnesses
func class_and_not_class_bounded_archetype_methods
<T: ClassBound & NotClassBound>(_ x:T, y:T) {
  x.classBoundMethod()
  x.classBoundBinaryMethod(y)
  x.notClassBoundMethod()
  x.notClassBoundBinaryMethod(y)
}
*/

// CHECK-LABEL: define hidden swiftcc { %objc_object*, i8** } @"$s22class_bounded_generics0a1_B8_erasure{{[_0-9a-zA-Z]*}}F"(%T22class_bounded_generics13ConcreteClassC* %0) {{.*}} {
func class_bounded_erasure(_ x: ConcreteClass) -> ClassBound {
  return x
  // CHECK: [[INSTANCE_OPAQUE:%.*]] = bitcast %T22class_bounded_generics13ConcreteClassC* [[INSTANCE:%.*]] to %objc_object*
  // CHECK: [[T0:%.*]] = insertvalue { %objc_object*, i8** } undef, %objc_object* [[INSTANCE_OPAQUE]], 0
  // CHECK: [[T1:%.*]] = insertvalue { %objc_object*, i8** } [[T0]], i8** getelementptr inbounds ([2 x i8*], [2 x i8*]* @"$s22class_bounded_generics13ConcreteClassCAA0E5BoundAAWP", i32 0, i32 0), 1
  // CHECK: ret { %objc_object*, i8** } [[T1]]
}

// CHECK-LABEL: define hidden swiftcc void @"$s22class_bounded_generics0a1_B16_protocol_method{{[_0-9a-zA-Z]*}}F"(%objc_object* %0, i8** %1) {{.*}} {
func class_bounded_protocol_method(_ x: ClassBound) {
  x.classBoundMethod()
  // CHECK: [[METADATA:%.*]] = call %swift.type* @swift_getObjectType(%objc_object* %0)
  // CHECK: [[WITNESS_GEP:%.*]] = getelementptr inbounds i8*, i8** [[WITNESS_TABLE:%.*]], i32 1
  // CHECK: [[WITNESS:%.*]] = load i8*, i8** [[WITNESS_GEP]], align 8
  // CHECK: [[WITNESS_FN:%.*]] = bitcast i8* [[WITNESS]] to void (%objc_object*, %swift.type*, i8**)
  // CHECK: call swiftcc void [[WITNESS_FN]](%objc_object* swiftself %0, %swift.type* [[METADATA]], i8** [[WITNESS_TABLE]])
}

// CHECK-LABEL: define hidden swiftcc %T22class_bounded_generics13ConcreteClassC* @"$s22class_bounded_generics0a1_B15_archetype_cast{{[_0-9a-zA-Z]*}}F"(%objc_object* %0, %swift.type* %T, i8** %T.ClassBound)
func class_bounded_archetype_cast<T : ClassBound>(_ x: T) -> ConcreteClass {
  return x as! ConcreteClass
  // CHECK: [[IN_PTR:%.*]] = bitcast %objc_object* {{%.*}} to i8*
  // CHECK: [[T0R:%.*]] = call swiftcc %swift.metadata_response @"$s22class_bounded_generics13ConcreteClassCMa"([[INT]] 0)
  // CHECK: [[T0:%.*]] = extractvalue %swift.metadata_response [[T0R]], 0
  // CHECK: [[T1:%.*]] = bitcast %swift.type* [[T0]] to i8*
  // CHECK: [[OUT_PTR:%.*]] = call i8* @swift_dynamicCastClassUnconditional(i8* [[IN_PTR]], i8* [[T1]], {{.*}})
  // CHECK: [[OUT:%.*]] = bitcast i8* [[OUT_PTR]] to %T22class_bounded_generics13ConcreteClassC*
  // CHECK: ret %T22class_bounded_generics13ConcreteClassC* [[OUT]]
}

// CHECK-LABEL: define hidden swiftcc %T22class_bounded_generics13ConcreteClassC* @"$s22class_bounded_generics0a1_B14_protocol_cast{{[_0-9a-zA-Z]*}}F"(%objc_object* %0, i8** %1)
func class_bounded_protocol_cast(_ x: ClassBound) -> ConcreteClass {
  return x as! ConcreteClass
  // CHECK: [[IN_PTR:%.*]] = bitcast %objc_object* {{%.*}} to i8*
  // CHECK: [[T0R:%.*]] = call swiftcc %swift.metadata_response @"$s22class_bounded_generics13ConcreteClassCMa"([[INT]] 0)
  // CHECK: [[T0:%.*]] = extractvalue %swift.metadata_response [[T0R]], 0
  // CHECK: [[T1:%.*]] = bitcast %swift.type* [[T0]] to i8*
  // CHECK: [[OUT_PTR:%.*]] = call i8* @swift_dynamicCastClassUnconditional(i8* [[IN_PTR]], i8* [[T1]], {{.*}})
  // CHECK: [[OUT:%.*]] = bitcast i8* [[OUT_PTR]] to %T22class_bounded_generics13ConcreteClassC*
  // CHECK: ret %T22class_bounded_generics13ConcreteClassC* [[OUT]]
}

// CHECK-LABEL: define hidden swiftcc { %objc_object*, i8** } @"$s22class_bounded_generics0a1_B22_protocol_conversion_{{.*}}"(%objc_object* %0, i8** %1, i8** %2) {{.*}} {
func class_bounded_protocol_conversion_1(_ x: ClassBound & ClassBound2)
-> ClassBound {
  return x
}
// CHECK-LABEL: define hidden swiftcc { %objc_object*, i8** } @"$s22class_bounded_generics0a1_B22_protocol_conversion_{{.*}}"(%objc_object* %0, i8** %1, i8** %2) {{.*}} {
func class_bounded_protocol_conversion_2(_ x: ClassBound & ClassBound2)
-> ClassBound2 {
  return x
}

// CHECK-LABEL: define hidden swiftcc { %objc_object*, i8** } @"$s22class_bounded_generics05objc_a1_B22_protocol_conversion_{{.*}}"(%objc_object* %0, i8** %1) {{.*}} {
func objc_class_bounded_protocol_conversion_1
(_ x: ClassBound & ObjCClassBound) -> ClassBound {
  return x
}
// CHECK-LABEL: define hidden swiftcc %objc_object* @"$s22class_bounded_generics05objc_a1_B22_protocol_conversion_{{.*}}"(%objc_object* %0, i8** %1) {{.*}} {
func objc_class_bounded_protocol_conversion_2
(_ x: ClassBound & ObjCClassBound) -> ObjCClassBound {
  return x
}
// CHECK-LABEL: define hidden swiftcc %objc_object* @"$s22class_bounded_generics05objc_a1_B22_protocol_conversion_{{.*}}"(%objc_object* %0)
func objc_class_bounded_protocol_conversion_3
(_ x: ObjCClassBound & ObjCClassBound2) -> ObjCClassBound {
  return x
}
// CHECK-LABEL: define hidden swiftcc %objc_object* @"$s22class_bounded_generics05objc_a1_B22_protocol_conversion_{{.*}}"(%objc_object* %0)
func objc_class_bounded_protocol_conversion_4
(_ x: ObjCClassBound & ObjCClassBound2) -> ObjCClassBound2 {
  return x
}

// CHECK-LABEL: define hidden swiftcc { i64, %objc_object*, i64 } @"$s22class_bounded_generics0A28_generic_field_struct_fields{{[_0-9a-zA-Z]*}}F"(i64 %0, %objc_object* %1, i64 %2, %swift.type* %T, i8** %T.ClassBound)
func class_generic_field_struct_fields<T>
(_ x:ClassGenericFieldStruct<T>) -> (Int, T, Int) {
  return (x.x, x.y, x.z)
}

// CHECK-LABEL: define hidden swiftcc { i64, %objc_object*, i8**, i64 } @"$s22class_bounded_generics0A29_protocol_field_struct_fields{{[_0-9a-zA-Z]*}}F"(i64 %0, %objc_object* %1, i8** %2, i64 %3)
func class_protocol_field_struct_fields
(_ x:ClassProtocolFieldStruct) -> (Int, ClassBound, Int) {
  return (x.x, x.y, x.z)
}

// CHECK-LABEL: define hidden swiftcc { i64, %objc_object*, i64 } @"$s22class_bounded_generics0a15_generic_field_A7_fields{{[_0-9a-zA-Z]*}}F"(%T22class_bounded_generics017ClassGenericFieldD0C* %0)
func class_generic_field_class_fields<T>
(_ x:ClassGenericFieldClass<T>) -> (Int, T, Int) {
  return (x.x, x.y, x.z)
  // CHECK: getelementptr inbounds %T22class_bounded_generics017ClassGenericFieldD0C, %T22class_bounded_generics017ClassGenericFieldD0C* %0, i32 0, i32 1
  // CHECK: getelementptr inbounds %T22class_bounded_generics017ClassGenericFieldD0C, %T22class_bounded_generics017ClassGenericFieldD0C* %0, i32 0, i32 2
  // CHECK: getelementptr inbounds %T22class_bounded_generics017ClassGenericFieldD0C, %T22class_bounded_generics017ClassGenericFieldD0C* %0, i32 0, i32 3
}

// CHECK-LABEL: define hidden swiftcc { i64, %objc_object*, i8**, i64 } @"$s22class_bounded_generics0a16_protocol_field_A7_fields{{[_0-9a-zA-Z]*}}F"(%T22class_bounded_generics018ClassProtocolFieldD0C* %0)
func class_protocol_field_class_fields(_ x: ClassProtocolFieldClass)
-> (Int, ClassBound, Int) {
  return (x.x, x.y, x.z)
  // CHECK:  = call swiftcc i64 %{{[0-9]+}}
  // CHECK:  = call swiftcc { %objc_object*, i8** } %{{[0-9]+}}
  // CHECK:  = call swiftcc i64 %{{[0-9]+}}
}

class SomeSwiftClass {
  class func foo() {}
}

// T must have a Swift layout, so we can load this metatype with a direct access.
// CHECK-LABEL: define hidden swiftcc void @"$s22class_bounded_generics0a1_B9_metatype{{[_0-9a-zA-Z]*}}F"
// CHECK:      [[T0:%.*]] = bitcast %T22class_bounded_generics14SomeSwiftClassC* {{%.*}}
// CHECK-NEXT: [[T1:%.*]] = load %swift.type*, %swift.type** [[T0]], align 8
// CHECK-NEXT: [[T2:%.*]] = bitcast %swift.type* [[T1]] to void (%swift.type*)**
// CHECK-NEXT: [[T3:%.*]] = getelementptr inbounds void (%swift.type*)*, void (%swift.type*)** [[T2]], i64 10
// CHECK-NEXT: load void (%swift.type*)*, void (%swift.type*)** [[T3]], align 8
func class_bounded_metatype<T: SomeSwiftClass>(_ t : T) {
  type(of: t).foo()
}

class WeakRef<T: AnyObject> {
  weak var value: T?
}

class A<T> {
  required init() {}
}

class M<T, S: A<T>> {
  private var s: S
  init() {
    // Don't crash generating the reference to 's'.
    s = S.init()
  }
}

// CHECK-LABEL: define hidden swiftcc void @"$s22class_bounded_generics14takes_metatypeyyxmlF"(%swift.type* %0, %swift.type* %T)
func takes_metatype<T>(_: T.Type) {}

// CHECK-LABEL: define hidden swiftcc void @"$s22class_bounded_generics023archetype_with_generic_A11_constraint1tyx_tAA1ACyq_GRbzr0_lF"(%T22class_bounded_generics1AC.1* %0, %swift.type* %T)
// CHECK:      [[ISA_ADDR:%.*]] = bitcast %T22class_bounded_generics1AC.1* %0 to %swift.type**
// CHECK-NEXT: [[ISA:%.*]] = load %swift.type*, %swift.type** [[ISA_ADDR]]
// CHECK:      call swiftcc void @"$s22class_bounded_generics14takes_metatypeyyxmlF"(%swift.type* %T, %swift.type* %T)
// CHECK-NEXT: [[T:%.*]] = bitcast %swift.type* %T to %swift.type**
// CHECK-NEXT: [[U_ADDR:%.*]] = getelementptr inbounds %swift.type*, %swift.type** [[T]], i64 10
// CHECK-NEXT: [[U:%.*]] = load %swift.type*, %swift.type** [[U_ADDR]]
// CHECK:      call swiftcc void @"$s22class_bounded_generics14takes_metatypeyyxmlF"(%swift.type* %U, %swift.type* %U)
// CHECK:      ret void

func archetype_with_generic_class_constraint<T, U>(t: T) where T : A<U> {
  takes_metatype(T.self)
  takes_metatype(U.self)
}

// CHECK-LABEL: define hidden swiftcc void @"$s22class_bounded_generics029calls_archetype_with_generic_A11_constraint1ayAA1ACyxG_tlF"(%T22class_bounded_generics1AC* %0) {{.*}} {
// CHECK:      alloca
// CHECK:      memset
// CHECK:      [[ISA_ADDR:%.*]] = bitcast %T22class_bounded_generics1AC* %0
// CHECK-NEXT: [[ISA:%.*]] = load %swift.type*, %swift.type** [[ISA_ADDR]]
// CHECK:      [[ISA_PTR:%.*]] = bitcast %swift.type* [[ISA]] to %swift.type**
// CHECK-NEXT: [[T_ADDR:%.*]] = getelementptr inbounds %swift.type*, %swift.type** [[ISA_PTR]], i64 10
// CHECK-NEXT: [[T:%.*]] = load %swift.type*, %swift.type** [[T_ADDR]]
// CHECK:      [[SELF:%.*]] = bitcast %T22class_bounded_generics1AC* %0 to %T22class_bounded_generics1AC.1*
// CHECK-NEXT: [[T0:%.*]] = call swiftcc %swift.metadata_response @"$s22class_bounded_generics1ACMa"([[INT]] 0, %swift.type* [[T]])
// CHECK-NEXT: [[A_OF_T:%.*]] = extractvalue %swift.metadata_response [[T0]], 0
// CHECK-NEXT: call swiftcc void @"$s22class_bounded_generics023archetype_with_generic_A11_constraint1tyx_tAA1ACyq_GRbzr0_lF"(%T22class_bounded_generics1AC.1* [[SELF]], %swift.type* [[A_OF_T]])
// CHECK:      ret void

func calls_archetype_with_generic_class_constraint<T>(a: A<T>) {
  archetype_with_generic_class_constraint(t: a)
}
