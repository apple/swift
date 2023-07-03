// RUN: %target-swift-frontend -module-name class_bounded_generics -enable-objc-interop -emit-ir -primary-file %s -disable-objc-attr-requires-foundation-module | %FileCheck %s -DINT=i%target-ptrsize

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

// CHECK-DAG: %T22class_bounded_generics017ClassGenericFieldD0C = type <{ %swift.refcounted, %TSi, ptr, %TSi }>
// CHECK-DAG: %T22class_bounded_generics23ClassGenericFieldStructV = type <{ %TSi, ptr, %TSi }>
// CHECK-DAG: %T22class_bounded_generics24ClassProtocolFieldStructV = type <{ %TSi, %T22class_bounded_generics10ClassBoundP, %TSi }>

// CHECK-LABEL: define hidden swiftcc ptr @"$s22class_bounded_generics0a1_B10_archetype{{[_0-9a-zA-Z]*}}F"(ptr %0, ptr %T, ptr %T.ClassBound)
func class_bounded_archetype<T : ClassBound>(_ x: T) -> T {
  return x
}

class SomeClass {}
class SomeSubclass : SomeClass {}

// CHECK-LABEL: define hidden swiftcc ptr @"$s22class_bounded_generics011superclass_B10_archetype{{[_0-9a-zA-Z]*}}F"(ptr %0, ptr %T)
func superclass_bounded_archetype<T : SomeClass>(_ x: T) -> T {
  return x
}

// CHECK-LABEL: define hidden swiftcc { ptr, ptr } @"$s22class_bounded_generics011superclass_B15_archetype_call{{[_0-9a-zA-Z]*}}F"(ptr %0, ptr %1)
func superclass_bounded_archetype_call(_ x: SomeClass, y: SomeSubclass) -> (SomeClass, SomeSubclass) {
  return (superclass_bounded_archetype(x),
          superclass_bounded_archetype(y));
  // CHECK: [[SOMECLASS_RESULT:%.*]] = call swiftcc ptr @"$s22class_bounded_generics011superclass_B10_archetype{{[_0-9a-zA-Z]*}}F"(ptr {{%.*}}, {{.*}})
  // CHECK: [[SOMESUPERCLASS_RESULT:%.*]] = call swiftcc ptr @"$s22class_bounded_generics011superclass_B10_archetype{{[_0-9a-zA-Z]*}}F"(ptr {{%.*}}, {{.*}})
}

// CHECK-LABEL: define hidden swiftcc void @"$s22class_bounded_generics0a1_B17_archetype_method{{[_0-9a-zA-Z]*}}F"(ptr %0, ptr %1, ptr %T, ptr %T.ClassBoundBinary)
func class_bounded_archetype_method<T : ClassBoundBinary>(_ x: T, y: T) {
  x.classBoundMethod()
  // CHECK: [[INHERITED_GEP:%.*]] = getelementptr inbounds ptr, ptr %T.ClassBoundBinary, i32 1
  // CHECK: [[INHERITED:%.*]] = load ptr, ptr [[INHERITED_GEP]]
  // CHECK: [[WITNESS_GEP:%.*]] = getelementptr inbounds ptr, ptr [[INHERITED]], i32 1
  // CHECK: [[WITNESS:%.*]] = load ptr, ptr [[WITNESS_GEP]], align 8
  // CHECK: call swiftcc void [[WITNESS]](ptr swiftself %0, ptr {{.*}}, ptr [[INHERITED]])
  x.classBoundBinaryMethod(y)
  // CHECK-NOT: call ptr @swift_unknownObjectRetain(ptr returned [[Y:%.*]])
  // CHECK: [[WITNESS_ENTRY:%.*]] = getelementptr inbounds ptr, ptr %T.ClassBoundBinary, i32 2
  // CHECK: [[WITNESS:%.*]] = load ptr, ptr [[WITNESS_ENTRY]], align 8
  // CHECK: call swiftcc void [[WITNESS]](ptr %1, ptr swiftself %0, ptr %T, ptr %T.ClassBoundBinary)
}

// CHECK-LABEL: define hidden swiftcc { ptr, ptr } @"$s22class_bounded_generics0a1_B16_archetype_tuple{{[_0-9a-zA-Z]*}}F"(ptr %0, ptr %T, ptr %T.ClassBound)
func class_bounded_archetype_tuple<T : ClassBound>(_ x: T) -> (T, T) {
  return (x, x)
}

class ConcreteClass : ClassBoundBinary, NotClassBound {
  func classBoundMethod() {}
  func classBoundBinaryMethod(_ x: ConcreteClass) {}
  func notClassBoundMethod() {}
  func notClassBoundBinaryMethod(_ x: ConcreteClass) {}
}

// CHECK-LABEL: define hidden swiftcc ptr @"$s22class_bounded_generics05call_a1_B10_archetype{{[_0-9a-zA-Z]*}}F"(ptr %0) {{.*}} {
func call_class_bounded_archetype(_ x: ConcreteClass) -> ConcreteClass {
  return class_bounded_archetype(x)
  // CHECK: [[OUT_ORIG:%.*]] = call swiftcc ptr @"$s22class_bounded_generics0a1_B10_archetype{{[_0-9a-zA-Z]*}}F"(ptr {{%.*}}, {{.*}})
  // CHECK: ret ptr [[OUT_ORIG]]
}

// CHECK: define hidden swiftcc void @"$s22class_bounded_generics04not_a1_B10_archetype{{[_0-9a-zA-Z]*}}F"(ptr noalias nocapture sret({{.*}}) %0, ptr noalias nocapture %1, ptr %T, ptr %T.NotClassBound)
func not_class_bounded_archetype<T : NotClassBound>(_ x: T) -> T {
  return x
}

// CHECK-LABEL: define hidden swiftcc ptr @"$s22class_bounded_generics0a1_b18_archetype_to_not_a1_B0{{[_0-9a-zA-Z]*}}F"(ptr %0, ptr %T, ptr %T.ClassBound, ptr %T.NotClassBound) {{.*}} {
func class_bounded_archetype_to_not_class_bounded
<T: ClassBound & NotClassBound>(_ x:T) -> T {
  // CHECK: alloca ptr, align 8
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

// CHECK-LABEL: define hidden swiftcc { ptr, ptr } @"$s22class_bounded_generics0a1_B8_erasure{{[_0-9a-zA-Z]*}}F"(ptr %0) {{.*}} {
func class_bounded_erasure(_ x: ConcreteClass) -> ClassBound {
  return x
  // CHECK: [[T0:%.*]] = insertvalue { ptr, ptr } undef, ptr [[INSTANCE:%.*]], 0
  // CHECK: [[T1:%.*]] = insertvalue { ptr, ptr } [[T0]], ptr @"$s22class_bounded_generics13ConcreteClassCAA0E5BoundAAWP", 1
  // CHECK: ret { ptr, ptr } [[T1]]
}

// CHECK-LABEL: define hidden swiftcc void @"$s22class_bounded_generics0a1_B16_protocol_method{{[_0-9a-zA-Z]*}}F"(ptr %0, ptr %1) {{.*}} {
func class_bounded_protocol_method(_ x: ClassBound) {
  x.classBoundMethod()
  // CHECK: [[METADATA:%.*]] = call ptr @swift_getObjectType(ptr %0)
  // CHECK: [[WITNESS_GEP:%.*]] = getelementptr inbounds ptr, ptr [[WITNESS_TABLE:%.*]], i32 1
  // CHECK: [[WITNESS:%.*]] = load ptr, ptr [[WITNESS_GEP]], align 8
  // CHECK: call swiftcc void [[WITNESS]](ptr swiftself %0, ptr [[METADATA]], ptr [[WITNESS_TABLE]])
}

// CHECK-LABEL: define hidden swiftcc ptr @"$s22class_bounded_generics0a1_B15_archetype_cast{{[_0-9a-zA-Z]*}}F"(ptr %0, ptr %T, ptr %T.ClassBound)
func class_bounded_archetype_cast<T : ClassBound>(_ x: T) -> ConcreteClass {
  return x as! ConcreteClass
  // CHECK: [[T0R:%.*]] = call swiftcc %swift.metadata_response @"$s22class_bounded_generics13ConcreteClassCMa"([[INT]] 0)
  // CHECK: [[T0:%.*]] = extractvalue %swift.metadata_response [[T0R]], 0
  // CHECK: [[OUT_PTR:%.*]] = call ptr @swift_dynamicCastClassUnconditional(ptr {{%.*}}, ptr [[T0]], {{.*}})
  // CHECK: ret ptr [[OUT_PTR]]
}

// CHECK-LABEL: define hidden swiftcc ptr @"$s22class_bounded_generics0a1_B14_protocol_cast{{[_0-9a-zA-Z]*}}F"(ptr %0, ptr %1)
func class_bounded_protocol_cast(_ x: ClassBound) -> ConcreteClass {
  return x as! ConcreteClass
  // CHECK: [[T0R:%.*]] = call swiftcc %swift.metadata_response @"$s22class_bounded_generics13ConcreteClassCMa"([[INT]] 0)
  // CHECK: [[T0:%.*]] = extractvalue %swift.metadata_response [[T0R]], 0
  // CHECK: [[OUT_PTR:%.*]] = call ptr @swift_dynamicCastClassUnconditional(ptr {{%.*}}, ptr [[T0]], {{.*}})
  // CHECK: ret ptr [[OUT_PTR]]
}

// CHECK-LABEL: define hidden swiftcc { ptr, ptr } @"$s22class_bounded_generics0a1_B22_protocol_conversion_{{.*}}"(ptr %0, ptr %1, ptr %2) {{.*}} {
func class_bounded_protocol_conversion_1(_ x: ClassBound & ClassBound2)
-> ClassBound {
  return x
}
// CHECK-LABEL: define hidden swiftcc { ptr, ptr } @"$s22class_bounded_generics0a1_B22_protocol_conversion_{{.*}}"(ptr %0, ptr %1, ptr %2) {{.*}} {
func class_bounded_protocol_conversion_2(_ x: ClassBound & ClassBound2)
-> ClassBound2 {
  return x
}

// CHECK-LABEL: define hidden swiftcc { ptr, ptr } @"$s22class_bounded_generics05objc_a1_B22_protocol_conversion_{{.*}}"(ptr %0, ptr %1) {{.*}} {
func objc_class_bounded_protocol_conversion_1
(_ x: ClassBound & ObjCClassBound) -> ClassBound {
  return x
}
// CHECK-LABEL: define hidden swiftcc ptr @"$s22class_bounded_generics05objc_a1_B22_protocol_conversion_{{.*}}"(ptr %0, ptr %1) {{.*}} {
func objc_class_bounded_protocol_conversion_2
(_ x: ClassBound & ObjCClassBound) -> ObjCClassBound {
  return x
}
// CHECK-LABEL: define hidden swiftcc ptr @"$s22class_bounded_generics05objc_a1_B22_protocol_conversion_{{.*}}"(ptr %0)
func objc_class_bounded_protocol_conversion_3
(_ x: ObjCClassBound & ObjCClassBound2) -> ObjCClassBound {
  return x
}
// CHECK-LABEL: define hidden swiftcc ptr @"$s22class_bounded_generics05objc_a1_B22_protocol_conversion_{{.*}}"(ptr %0)
func objc_class_bounded_protocol_conversion_4
(_ x: ObjCClassBound & ObjCClassBound2) -> ObjCClassBound2 {
  return x
}

// CHECK-LABEL: define hidden swiftcc { i64, ptr, i64 } @"$s22class_bounded_generics0A28_generic_field_struct_fields{{[_0-9a-zA-Z]*}}F"(i64 %0, ptr %1, i64 %2, ptr %T, ptr %T.ClassBound)
func class_generic_field_struct_fields<T>
(_ x:ClassGenericFieldStruct<T>) -> (Int, T, Int) {
  return (x.x, x.y, x.z)
}

// CHECK-LABEL: define hidden swiftcc { i64, ptr, ptr, i64 } @"$s22class_bounded_generics0A29_protocol_field_struct_fields{{[_0-9a-zA-Z]*}}F"(i64 %0, ptr %1, ptr %2, i64 %3)
func class_protocol_field_struct_fields
(_ x:ClassProtocolFieldStruct) -> (Int, ClassBound, Int) {
  return (x.x, x.y, x.z)
}

// CHECK-LABEL: define hidden swiftcc { i64, ptr, i64 } @"$s22class_bounded_generics0a15_generic_field_A7_fields{{[_0-9a-zA-Z]*}}F"(ptr %0)
func class_generic_field_class_fields<T>
(_ x:ClassGenericFieldClass<T>) -> (Int, T, Int) {
  return (x.x, x.y, x.z)
  // CHECK: getelementptr inbounds %T22class_bounded_generics017ClassGenericFieldD0C, ptr %0, i32 0, i32 1
  // CHECK: getelementptr inbounds %T22class_bounded_generics017ClassGenericFieldD0C, ptr %0, i32 0, i32 2
  // CHECK: getelementptr inbounds %T22class_bounded_generics017ClassGenericFieldD0C, ptr %0, i32 0, i32 3
}

// CHECK-LABEL: define hidden swiftcc { i64, ptr, ptr, i64 } @"$s22class_bounded_generics0a16_protocol_field_A7_fields{{[_0-9a-zA-Z]*}}F"(ptr %0)
func class_protocol_field_class_fields(_ x: ClassProtocolFieldClass)
-> (Int, ClassBound, Int) {
  return (x.x, x.y, x.z)
  // CHECK:  = call swiftcc i64 %{{[0-9]+}}
  // CHECK:  = call swiftcc { ptr, ptr } %{{[0-9]+}}
  // CHECK:  = call swiftcc i64 %{{[0-9]+}}
}

class SomeSwiftClass {
  class func foo() {}
}

// T must have a Swift layout, so we can load this metatype with a direct access.
// CHECK-LABEL: define hidden swiftcc void @"$s22class_bounded_generics0a1_B9_metatype{{[_0-9a-zA-Z]*}}F"
// CHECK: [[T1:%.*]] = load ptr, ptr
// CHECK-NEXT: [[T3:%.*]] = getelementptr inbounds ptr, ptr [[T1]], i64 10
// CHECK-NEXT: load ptr, ptr [[T3]], align 8
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

// CHECK-LABEL: define hidden swiftcc void @"$s22class_bounded_generics14takes_metatypeyyxmlF"(ptr %0, ptr %T)
func takes_metatype<T>(_: T.Type) {}

// CHECK-LABEL: define hidden swiftcc void @"$s22class_bounded_generics023archetype_with_generic_A11_constraint1tyx_tAA1ACyq_GRbzr0_lF"(ptr %0, ptr %T)
// CHECK: [[ISA:%.*]] = load ptr, ptr %0
// CHECK:      call swiftcc void @"$s22class_bounded_generics14takes_metatypeyyxmlF"(ptr %T, ptr %T)
// CHECK-NEXT: [[U_ADDR:%.*]] = getelementptr inbounds ptr, ptr %T, i64 10
// CHECK-NEXT: [[U:%.*]] = load ptr, ptr [[U_ADDR]]
// CHECK:      call swiftcc void @"$s22class_bounded_generics14takes_metatypeyyxmlF"(ptr %U, ptr %U)
// CHECK:      ret void

func archetype_with_generic_class_constraint<T, U>(t: T) where T : A<U> {
  takes_metatype(T.self)
  takes_metatype(U.self)
}

// CHECK-LABEL: define hidden swiftcc void @"$s22class_bounded_generics029calls_archetype_with_generic_A11_constraint1ayAA1ACyxG_tlF"(ptr %0) {{.*}} {
// CHECK:      alloca
// CHECK:      memset
// CHECK: [[ISA:%.*]] = load ptr, ptr
// CHECK: [[T_ADDR:%.*]] = getelementptr inbounds ptr, ptr [[ISA]], i64 10
// CHECK-NEXT: [[T:%.*]] = load ptr, ptr [[T_ADDR]]
// CHECK: [[T0:%.*]] = call swiftcc %swift.metadata_response @"$s22class_bounded_generics1ACMa"([[INT]] 0, ptr [[T]])
// CHECK-NEXT: [[A_OF_T:%.*]] = extractvalue %swift.metadata_response [[T0]], 0
// CHECK-NEXT: call swiftcc void @"$s22class_bounded_generics023archetype_with_generic_A11_constraint1tyx_tAA1ACyq_GRbzr0_lF"(ptr %0, ptr [[A_OF_T]])
// CHECK:      ret void

func calls_archetype_with_generic_class_constraint<T>(a: A<T>) {
  archetype_with_generic_class_constraint(t: a)
}
