// RUN: %target-swift-frontend %s -emit-ir > %t.txt
// RUN: %FileCheck %s --check-prefix=CHECK < %t.txt
// RUN: %FileCheck %s --check-prefix=CHECK-CONSTANTS < %t.txt

// REQUIRES: CPU=x86_64

func blah<T>(_: T.Type) {}

// CHECK-LABEL: define{{( dllexport)?}}{{( protected)?}} swiftcc void @"$s15nested_generics13makeAMetadatayyF"()
public func makeAMetadata() {
  blah(OuterGenericStruct<Int>.InnerGenericStruct<String>.self)
  blah(OuterGenericStruct<Int>.InnerConcreteStruct.self)

  blah(OuterGenericClass<Int>.InnerGenericClass<String>.self)
  blah(OuterGenericClass<Int>.InnerConcreteClass.self)
}

// Type constructor for OuterGenericStruct<T>
// CHECK-LABEL: define{{( dllexport)?}}{{( protected)?}} swiftcc %swift.metadata_response @"$s15nested_generics18OuterGenericStructVMa"(i64 %0, ptr %1)

// Type constructor for OuterGenericStruct<T>.InnerGenericStruct<U>
// CHECK-LABEL: define{{( dllexport)?}}{{( protected)?}} swiftcc %swift.metadata_response @"$s15nested_generics18OuterGenericStructV05InnerdE0VMa"(i64 %0, ptr %1, ptr %2)

// Type constructor for OuterGenericStruct<T>.InnerConcreteStruct
// CHECK-LABEL: define{{( dllexport)?}}{{( protected)?}} swiftcc %swift.metadata_response @"$s15nested_generics18OuterGenericStructV013InnerConcreteE0VMa"(i64 %0, ptr %1)

public struct OuterGenericStruct<T> {
  public struct InnerGenericStruct<U> {
    public func method() {
      blah(T.self)
      blah(U.self)
    }
  }

  public struct InnerConcreteStruct {
    public func method() {
      blah(T.self)
    }
  }
}

// Type constructor for OuterGenericClass<T>
// CHECK-LABEL: define{{( dllexport)?}}{{( protected)?}} swiftcc %swift.metadata_response @"$s15nested_generics17OuterGenericClassCMa"(i64 %0, ptr %1)

// Type constructor for OuterGenericClass<T>.InnerGenericClass<U>
// CHECK-LABEL: define{{( dllexport)?}}{{( protected)?}} swiftcc %swift.metadata_response @"$s15nested_generics17OuterGenericClassC05InnerdE0CMa"(i64 %0, ptr %1, ptr %2)

// Type constructor for OuterGenericClass<T>.InnerConcreteClass
// CHECK-LABEL: define{{( dllexport)?}}{{( protected)?}} swiftcc %swift.metadata_response @"$s15nested_generics17OuterGenericClassC013InnerConcreteE0CMa"(i64 %0, ptr %1)

public class OuterGenericClass<T> {
  public class InnerGenericClass<U> {
    public func method() {
      blah(T.self)
      blah(U.self)
    }
  }

  public class InnerConcreteClass {
    public func method() {
      blah(T.self)
    }
  }
}

// This used to crash while emitting value witnesses.

public struct Fish<Water> {}

public protocol Wet {}

extension Fish where Water : Wet {
  public enum Fillet {
    case grilled
    case fried
  }
}

// <rdar://problem/51627403> Superclass demangling failure when instantiating
// nested generic subclass constrained to outer type generic argument

protocol TagProtocol {}
enum Outer : TagProtocol {}

protocol HasAssoc {
  associatedtype Assoc
}

class SeparateGenericSuperclass<T> {}

enum Container<T : TagProtocol> {
  class _Superclass {}
  // CHECK-CONSTANTS-LABEL: @"$s15nested_generics9ContainerO9_SubclassCMn" =
  // Check the superclass...
  // CHECK-CONSTANTS-SAME: @"symbolic _____y______G 15nested_generics9ContainerO11_SuperclassC AA5OuterO"
  // ...and the requirements.
  // CHECK-CONSTANTS-SAME: @"symbolic x"
  // CHECK-CONSTANTS-SAME: @"symbolic _____ 15nested_generics5OuterO"
  // CHECK-CONSTANTS-LABEL: @"$s15nested_generics9ContainerO9_SubclassCMF" =
  // CHECK-CONSTANTS-SAME: @"symbolic _____y______G 15nested_generics9ContainerO11_SuperclassC AA5OuterO"
  class _Subclass<U>: _Superclass where T == Outer {
    // CHECK-CONSTANTS-LABEL: @"$s15nested_generics9ContainerO9_SubclassC11ExtraNestedCMn" =
    // CHECK-CONSTANTS-SAME: @"symbolic _____y______G 15nested_generics9ContainerO11_SuperclassC AA5OuterO"
    // CHECK-CONSTANTS-LABEL: @"$s15nested_generics9ContainerO9_SubclassC11ExtraNestedCMF" =
    // CHECK-CONSTANTS-SAME: @"symbolic _____y______G 15nested_generics9ContainerO11_SuperclassC AA5OuterO"
    class ExtraNested: _Superclass {}
  }

  // CHECK-CONSTANTS-LABEL: @"$s15nested_generics9ContainerO10_Subclass2CMn" =
  // CHECK-CONSTANTS-SAME: @"symbolic _____yx_G 15nested_generics9ContainerO11_SuperclassC"
  class _Subclass2<U: Collection>: _Superclass where T == U.Element {}

  // CHECK-CONSTANTS-LABEL: @"$s15nested_generics9ContainerO10_Subclass3CMn" =
  // CHECK-CONSTANTS-SAME: @"symbolic _____y______qd__G 15nested_generics9ContainerO18_GenericSuperclassC AA5OuterO"
  class _GenericSuperclass<U> {}
  class _Subclass3<U>: _GenericSuperclass<U> where T == Outer {}

  class MoreNesting {
    // CHECK-CONSTANTS-LABEL: @"$s15nested_generics9ContainerO11MoreNestingC9_SubclassCMn" =
    // CHECK-CONSTANTS-SAME: @"symbolic _____y______G 15nested_generics9ContainerO11_SuperclassC AA5OuterO"
    class _Subclass<U>: _Superclass where T == Outer {}
  }

  // CHECK-CONSTANTS-LABEL: @"$s15nested_generics9ContainerO24_SeparateGenericSubclassCMn" =
  // CHECK-CONSTANTS-SAME: @"symbolic _____yxSgG 15nested_generics25SeparateGenericSuperclassC"
  class _SeparateGenericSubclass: SeparateGenericSuperclass<T?> {}

  // CHECK-CONSTANTS-LABEL: @"$s15nested_generics9ContainerO6FieldsVMF" =
  // CHECK-CONSTANTS-SAME: @"symbolic _____ 15nested_generics5OuterO"
  // CHECK-CONSTANTS-SAME: @"symbolic qd__"
  struct Fields<U> where T == Outer {
    var x: T
    var y: U
  }

  // CHECK-CONSTANTS-LABEL: @"$s15nested_generics9ContainerO5CasesOMF" =
  // CHECK-CONSTANTS-SAME: @"symbolic qd__"
  enum Cases<U> where T == Outer {
    case a(T)
    case b(U)
  }

  struct Conformancy<U>: HasAssoc where T == Outer {
    typealias Assoc = T
  }

  struct Conformancy2<U> {}
  struct Conformancy3 {}
}

extension Container.Conformancy2: HasAssoc where T == Outer {
  typealias Assoc = T
}
extension Container.Conformancy3: HasAssoc where T == Outer {
  typealias Assoc = T
}

// CHECK-CONSTANTS-LABEL: @"$s15nested_generics9ContainerO12Conformancy3Vyx_GAA8HasAssocA2A5OuterORszrlWP" =
// CHECK-CONSTANTS-SAME: @"symbolic{{.*}}15nested_generics5OuterO"

// CHECK-CONSTANTS-LABEL: @"$s15nested_generics9ContainerO12Conformancy2Vyx_qd__GAA8HasAssocA2A5OuterORszrlWP" =
// CHECK-CONSTANTS-SAME: @"symbolic{{.*}}15nested_generics5OuterO"

// CHECK-CONSTANTS-LABEL: @"$s15nested_generics9ContainerO11ConformancyVyx_qd__GAA8HasAssocAAWP" =
// CHECK-CONSTANTS-SAME: @"symbolic{{.*}}15nested_generics5OuterO"
