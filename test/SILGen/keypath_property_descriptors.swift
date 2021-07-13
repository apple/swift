// RUN: %target-swift-emit-silgen %s | %FileCheck --check-prefix=CHECK --check-prefix=NONRESILIENT %s
// RUN: %target-swift-emit-silgen -enable-library-evolution %s | %FileCheck --check-prefix=CHECK --check-prefix=RESILIENT %s
// RUN: %target-swift-emit-silgen -enable-private-imports %s | %FileCheck --check-prefix=PRIVATEIMPORTS %s

// TODO: globals should get descriptors
public var a: Int = 0

@inlinable
public var b: Int { return 0 }

@usableFromInline
internal var c: Int = 0

// no descriptor
// CHECK-NOT: sil_property #d
// PRIVATEIMPORTS-NOT: sil_property #d
internal var d: Int = 0
// CHECK-NOT: sil_property #e
// PRIVATEIMPORTS-NOT: sil_property #e
private var e: Int = 0

public struct A {
  // NONRESILIENT-LABEL: sil_property #A.a ()
  // RESILIENT-LABEL: sil_property #A.a (stored_property
	// PRIVATEIMPORTS-LABEL: sil_property #A.a ()
  public var a: Int = 0

  // CHECK-LABEL: sil_property #A.b ()
	// PRIVATEIMPORTS-LABEL: sil_property #A.b ()
  @inlinable
  public var b: Int { return 0 }

  // NONRESILIENT-LABEL: sil_property #A.c ()
  // RESILIENT-LABEL: sil_property #A.c (stored_property
  // PRIVATEIMPORTS-LABEL: sil_property #A.c ()
  @usableFromInline
  internal var c: Int = 0

  // no descriptor
  // CHECK-NOT: sil_property #A.d
  // PRIVATEIMPORTS-LABEL: sil_property #A.d ()
  internal var d: Int = 0
  // CHECK-NOT: sil_property #A.e
  // PRIVATEIMPORTS-LABEL: sil_property #A.e ()
  fileprivate var e: Int = 0
  // CHECK-NOT: sil_property #A.f
  // PRIVATEIMPORTS-LABEL: sil_property #A.f ()
  private var f: Int = 0

  // TODO: static vars should get descriptors
  public static var a: Int = 0
  @inlinable
  public static var b: Int { return 0 }
  @usableFromInline
  internal static var c: Int = 0

  // no descriptor
  // CHECK-NOT: sil_property #A.d
  internal static var d: Int = 0
  // CHECK-NOT: sil_property #A.e
  fileprivate static var e: Int = 0
  // CHECK-NOT: sil_property #A.f
  private static var f: Int = 0

  // CHECK-LABEL: sil_property #A.subscript{{.*}} (){{$}}
  public subscript(a x: Int) -> Int { return x }
  // CHECK-LABEL: sil_property #A.subscript{{.*}} (){{$}}
  @inlinable
  public subscript(b x: Int) -> Int { return x }
  // CHECK-LABEL: sil_property #A.subscript{{.*}} (){{$}}
  @usableFromInline
  internal subscript(c x: Int) -> Int { return x }
  
  // no descriptor
  // CHECK-NOT: sil_property #A.subscript
  internal subscript(d x: Int) -> Int { return x }
  fileprivate subscript(e x: Int) -> Int { return x }
  private subscript(f x: Int) -> Int { return x }

  // CHECK-LABEL: sil_property #A.subscript{{.*}} (){{$}}
  public subscript<T>(a x: T) -> T { return x }
  // CHECK-LABEL: sil_property #A.subscript{{.*}} (){{$}}
  @inlinable
  public subscript<T>(b x: T) -> T { return x }
  // CHECK-LABEL: sil_property #A.subscript{{.*}} (){{$}}
  @usableFromInline
  internal subscript<T>(c x: T) -> T { return x }
  
  // no descriptor
  // CHECK-NOT: sil_property #A.subscript
  internal subscript<T>(d x: T) -> T { return x }
  fileprivate subscript<T>(e x: T) -> T { return x }
  private subscript<T>(f x: T) -> T { return x }

  // no descriptor
  // CHECK-NOT: sil_property #A.count
  public var count: Int {
    mutating get {
      _count += 1
      return _count
    }
    set {
      _count = newValue
    }
  }

  // CHECK-NOT: sil_property #A._count
  private var _count: Int = 0

  // NONRESILIENT-LABEL: sil_property #A.getSet ()
  // PRIVATEIMPORTS-LABEL: sil_property #A.getSet ()
  // RESILIENT-LABEL: sil_property #A.getSet (settable_property
  public var getSet: Int {
    get { return 0 }
    set { }
  }

  // CHECK-LABEL: sil_property #A.hiddenSetter (settable_property
  // PRIVATEIMPORTS-LABEL: sil_property #A.hiddenSetter (settable_property
  public internal(set) var hiddenSetter: Int {
    get { return 0 }
    set { }
  }

  // PRIVATEIMPORTS-LABEL: sil_property #A.privateSetter (gettable_property
  public private(set) var privateSetter: Int {
    get { return 0 }
    set { }
  }
  // PRIVATEIMPORTS-LABEL: sil_property #A.fileprivateSetter (gettable_property
  public fileprivate(set) var fileprivateSetter: Int {
    get { return 0 }
    set { }
  }

  // NONRESILIENT-LABEL: sil_property #A.usableFromInlineSetter ()
  // PRIVATEIMPORTS-LABEL: sil_property #A.usableFromInlineSetter ()
  // RESILIENT-LABEL: sil_property #A.usableFromInlineSetter (settable_property
  public internal(set) var usableFromInlineSetter: Int {
    get { return 0 }
    @usableFromInline set { }
  }
}

@_fixed_layout
public struct FixedLayout {
  // NONRESILIENT-LABEL: sil_property #FixedLayout.a ()
  // RESILIENT-LABEL: sil_property #FixedLayout.a (stored_property
  public var a: Int
  // NONRESILIENT-LABEL: sil_property #FixedLayout.b ()
  // RESILIENT-LABEL: sil_property #FixedLayout.b (stored_property
  public var b: Int
  // NONRESILIENT-LABEL: sil_property #FixedLayout.c ()
  // RESILIENT-LABEL: sil_property #FixedLayout.c (stored_property
  public var c: Int
}

public class Foo {}
extension Array where Element == Foo {
  public class Bar {
    // NONRESILIENT-LABEL: sil_property #Array.Bar.dontCrash<τ_0_0 where τ_0_0 == Foo> (gettable_property $Int
    public private(set) var dontCrash : Int {
      get {
        return 10
      }
      set {
      }
    }
  }
}
