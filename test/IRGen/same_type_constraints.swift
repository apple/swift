// RUN: %target-swift-frontend -emit-ir -primary-file %s -disable-objc-attr-requires-foundation-module | %FileCheck %s
// RUN: %target-swift-frontend -Osize -emit-ir -primary-file %s -disable-objc-attr-requires-foundation-module | %FileCheck %s --check-prefix=OSIZE

// Ensure that same-type constraints between generic arguments get reflected
// correctly in the type context descriptor.
// CHECK-LABEL: @"$s21same_type_constraints4SG11VA2A2P2Rzq_RszrlE13InnerTEqualsUVMn" = 
//                  T       U(==T) V        padding
// CHECK-SAME:    , i8 -128, i8 0, i8 -128, i8 0,

// <rdar://problem/21665983> IRGen crash with protocol extension involving same-type constraint to X<T>
public struct DefaultFoo<T> {
  var t: T?
}

public protocol P {
  associatedtype Foo
}

public extension P where Foo == DefaultFoo<Self> {
  public func foo() -> DefaultFoo<Self> {
    return DefaultFoo()
  }
}


// CHECK-LABEL: define{{( dllexport)?}}{{( protected)?}} swiftcc void @"$s21same_type_constraints1PPA2A10DefaultFooVyxG0E0RtzrlE3fooAFyF"

// <rdar://26873036> IRGen crash with derived class declaring same-type constraint on constrained associatedtype.
public class C1<T: Equatable> { }
public class C2<T: Equatable, U: P>: C1<T> where T == U.Foo {}

// CHECK: define{{( dllexport)?}}{{( protected)?}} swiftcc void @"$s21same_type_constraints2C1CfD"

public protocol MyHashable {}
public protocol DataType : MyHashable {}

public protocol E {
  associatedtype Data: DataType
}

struct Dict<V : MyHashable, K> {}
struct Val {}

public class GenericKlazz<T: DataType, R: E> : E where R.Data == T
{
  public typealias Data = T

  var d: Dict<T, Val>
  init() {
     d = Dict()
  }
}

// This used to hit an infinite loop - <rdar://problem/27018457>
public protocol CodingType {
    associatedtype ValueType
}

public protocol ValueCoding {
    associatedtype Coder: CodingType
}

func foo<Self>(s: Self)
where Self : CodingType,
      Self.ValueType: ValueCoding,
      Self.ValueType.Coder == Self {
  print(Self.ValueType.self)
}

// OSIZE: define internal swiftcc ptr @"$s21same_type_constraints12GenericKlazzCyxq_GAA1EAA4DataAaEP_AA0F4TypePWT"(ptr readnone %"GenericKlazz<T, R>.Data", ptr nocapture readonly %"GenericKlazz<T, R>", ptr nocapture readnone %"GenericKlazz<T, R>.E") [[ATTRS:#[0-9]+]] {
// OSIZE: [[ATTRS]] = {{{.*}}noinline

// Check that same-typing two generic parameters together lowers correctly.

protocol P1 {}
protocol P2 {}
protocol P3 {}
struct ConformsToP1: P1 {}
struct ConformsToP2: P2 {}
struct ConformsToP3: P3 {}

struct SG11<T: P1, U: P2> {}

struct ConformsToP1AndP2 : P1, P2 { }

extension SG11 where U == T {
  struct InnerTEqualsU<V: P3> { }
}

extension SG11 where T == ConformsToP1 {
  struct InnerTEqualsConformsToP1<V: P3> { }
}

extension SG11 where U == ConformsToP2 {
  struct InnerUEqualsConformsToP2<V: P3> { }
}

func inner1() -> Any.Type {
  return SG11<ConformsToP1AndP2, ConformsToP1AndP2>.InnerTEqualsU<ConformsToP3>.self
}

func inner2() -> Any.Type {
  return SG11<ConformsToP1, ConformsToP2>.InnerTEqualsConformsToP1<ConformsToP3>.self
}

func inner3() -> Any.Type {
  return SG11<ConformsToP1, ConformsToP2>.InnerTEqualsConformsToP1<ConformsToP3>.self
}
