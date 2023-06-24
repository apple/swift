// RUN: %target-swift-frontend %use_no_opaque_pointers -enable-objc-interop %s -emit-ir | %FileCheck %s --check-prefixes=CHECK,CHECK-objc
// RUN: %target-swift-frontend %use_no_opaque_pointers -disable-objc-interop %s -emit-ir | %FileCheck %s --check-prefixes=CHECK,CHECK-native
// RUN: %target-swift-frontend -enable-objc-interop %s -emit-ir
// RUN: %target-swift-frontend -disable-objc-interop %s -emit-ir

// REQUIRES: CPU=x86_64

// CHECK: [[A:%T13generic_types1AC]] = type <{ [[REF:%swift.refcounted]], [[INT:%TSi]] }>
// CHECK: [[INT]] = type <{ i64 }>
// CHECK: [[B:%T13generic_types1BC]] = type <{ [[REF:%swift.refcounted]], [[UNSAFE:%TSp]] }>
// CHECK: [[C:%T13generic_types1CC]] = type
// CHECK: [[D:%T13generic_types1DC]] = type

// CHECK-LABEL: @"$s13generic_types1ACMI" = internal global [16 x i8*] zeroinitializer, align 8

// CHECK-LABEL: @"$s13generic_types1ACMn" = hidden constant
// CHECK-SAME:   i32 -2147483440,
// CHECK-SAME:   @"$s13generic_typesMXM"
//               <name>
// CHECK-SAME:   @"$s13generic_types1ACMa"
// -- superclass
// CHECK-SAME:   i32 0,
// -- negative size in words
// CHECK-SAME:   i32 3,
// -- positive size in words
// CHECK-objc-SAME:   i32 17,
// CHECK-native-SAME:   i32 14,
// -- num immediate members
// CHECK-SAME:   i32 7,
// -- num fields
// CHECK-SAME:   i32 1,
// -- field offset vector offset
// CHECK-objc-SAME:   i32 11,
// CHECK-native-SAME:   i32 8,
// -- instantiation cache
// CHECK-SAME:   @"$s13generic_types1ACMI"
// -- instantiation pattern
// CHECK-SAME:   @"$s13generic_types1ACMP"
// -- num generic params
// CHECK-SAME:   i16 1,
// -- num generic requirement
// CHECK-SAME:   i16 0,
// -- num key arguments
// CHECK-SAME:   i16 1,
// -- num extra arguments
// CHECK-SAME:   i16 0,
// -- parameter descriptor 1
// CHECK-SAME:   i8 -128,

// CHECK-LABEL: @"$s13generic_types1ACMP" = internal constant
// -- instantiation function
// CHECK-SAME:   @"$s13generic_types1ACMi"
// -- heap destructor
// CHECK-SAME:   void ([[A]]*)* @"$s13generic_types1ACfD"
// -- ivar destroyer
// CHECK-SAME:   i32 0,
// -- flags
// CHECK-SAME:   i32 {{3|2}},
// CHECK-SAME: }

// CHECK-LABEL: @"$s13generic_types1BCMI" = internal global [16 x i8*] zeroinitializer, align 8

// CHECK-LABEL: @"$s13generic_types1BCMn" = hidden constant
// CHECK-SAME:   @"$s13generic_types1BCMa"
// CHECK-SAME:   @"$s13generic_types1BCMI"
// CHECK-SAME:   @"$s13generic_types1BCMP"

// CHECK-LABEL: @"$s13generic_types1BCMP" = internal constant
// -- instantiation function
// CHECK-SAME:   @"$s13generic_types1BCMi"
// -- heap destructor
// CHECK-SAME:   void ([[B]]*)* @"$s13generic_types1BCfD"
// -- ivar destroyer
// CHECK-SAME:   i32 0,
// -- class flags
// CHECK-SAME:   i32 {{3|2}},
// CHECK-SAME: }

// CHECK-LABEL: @"$s13generic_types1CCMP" = internal constant
// -- instantiation function
// CHECK-SAME:   @"$s13generic_types1CCMi"
// -- heap destructor
// CHECK-SAME:   void ([[C]]*)* @"$s13generic_types1CCfD"
// -- ivar destroyer
// CHECK-SAME:   i32 0,
// -- class flags
// CHECK-SAME:   i32 {{3|2}},
// CHECK-SAME: }

// CHECK-LABEL: @"$s13generic_types1DCMP" = internal constant
// -- instantiation function
// CHECK-SAME:   @"$s13generic_types1DCMi"
// -- heap destructor
// CHECK-SAME:   void ([[D]]*)* @"$s13generic_types1DCfD"
// -- ivar destroyer
// CHECK-SAME:   i32 0,
// -- class flags
// CHECK-SAME:   i32 {{3|2}},
// CHECK-SAME: }

class A<T> {
  var x = 0

  func run(_ t: T) {}
  init(y : Int) {}
}

class B<T> {
  var ptr : UnsafeMutablePointer<T>
  init(ptr: UnsafeMutablePointer<T>) {
    self.ptr = ptr
  }
  deinit {
    ptr.deinitialize(count: 1)
  }
}

class C<T> : A<Int> {}

class D<T> : A<Int> {
  override func run(_ t: Int) {}
}

struct E<T> {
  var x : Int
  func foo() { bar() }
  func bar() {}
}

class ClassA {}
class ClassB {}

// This type is fixed-size across specializations, but it needs to use
// a different implementation in IR-gen so that types match up.
// It just asserts if we get it wrong.
struct F<T: AnyObject> {
  var value: T
}
func testFixed() {
  var a = F(value: ClassA()).value
  var b = F(value: ClassB()).value
}

// Checking generic requirement encoding
protocol P1 { }
protocol P2 {
  associatedtype A
}

struct X1: P1 { }
struct X2: P2 {
  typealias A = X1
}

// Check for correct generic parameters in the nominal type descriptor
// CHECK-LABEL: @"$s13generic_types2X3VMn" =

// Root: generic parameter 1
// CHECK-SAME: @"symbolic q_"

// U.A (via P2)
// CHECK-SAME: @"symbolic 1A_____Qy_ 13generic_types2P2P

struct X3<T, U> where U: P2, U.A: P1 { }

// CHECK-LABEL: define{{( dllexport)?}}{{( protected)?}} internal %swift.type* @"$s13generic_types1ACMi"(%swift.type_descriptor* %0, i8** %1, i8* %2) {{.*}} {
// CHECK:   [[T0:%.*]] = bitcast i8** %1 to %swift.type**
// CHECK:   %T = load %swift.type*, %swift.type** [[T0]],
// CHECK:   [[METADATA:%.*]] = call %swift.type* @swift_allocateGenericClassMetadata(%swift.type_descriptor* %0, i8** %1, i8* %2)
// CHECK-NEXT:   ret %swift.type* [[METADATA]]
// CHECK: }

// CHECK-LABEL: define{{( dllexport)?}}{{( protected)?}} internal %swift.type* @"$s13generic_types1BCMi"(%swift.type_descriptor* %0, i8** %1, i8* %2) {{.*}} {
// CHECK:   [[T0:%.*]] = bitcast i8** %1 to %swift.type**
// CHECK:   %T = load %swift.type*, %swift.type** [[T0]],
// CHECK:   [[METADATA:%.*]] = call %swift.type* @swift_allocateGenericClassMetadata(%swift.type_descriptor* %0, i8** %1, i8* %2)
// CHECK-NEXT: ret %swift.type* [[METADATA]]
// CHECK: }
