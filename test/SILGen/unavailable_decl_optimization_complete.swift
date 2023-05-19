// RUN: %target-swift-emit-silgen -module-name Test -parse-as-library %s -verify | %FileCheck %s --check-prefixes=CHECK,CHECK-NO-STRIP
// RUN: %target-swift-emit-silgen -module-name Test -parse-as-library %s -verify -unavailable-decl-optimization=none | %FileCheck %s --check-prefixes=CHECK,CHECK-NO-STRIP
// RUN: %target-swift-emit-silgen -module-name Test -parse-as-library %s -verify -unavailable-decl-optimization=complete | %FileCheck %s --check-prefixes=CHECK,CHECK-STRIP

// CHECK-NO-STRIP: s4Test14globalConstant_Wz
// CHECK-NO-STRIP: s4Test14globalConstantSbvp
// CHECK-NO-STRIP: s4Test14globalConstant_WZ
// CHECK-NO-STRIP: s4Test14globalConstantSbvau
// CHECK-STRIP-NOT: s4Test14globalConstant_Wz
// CHECK-STRIP-NOT: s4Test14globalConstantSbvp
// CHECK-STRIP-NOT: s4Test14globalConstant_WZ
// CHECK-STRIP-NOT: s4Test14globalConstantSbvau
@available(*, unavailable)
public let globalConstant = true

// CHECK-NO-STRIP: s4Test15unavailableFuncyyF
// CHECK-STRIP-NOT: s4Test15unavailableFuncyyF
@available(*, unavailable)
public func unavailableFunc() {}

@available(*, unavailable)
public struct UnavailableStruct<T> {
  // CHECK-NO-STRIP: s4Test17UnavailableStructV8propertyxvg
  // CHECK-NO-STRIP: s4Test17UnavailableStructV8propertyxvs
  // CHECK-NO-STRIP: s4Test17UnavailableStructV8propertyxvM
  // CHECK-STRIP-NOT: s4Test17UnavailableStructV8propertyxvg
  // CHECK-STRIP-NOT: s4Test17UnavailableStructV8propertyxvs
  // CHECK-STRIP-NOT: s4Test17UnavailableStructV8propertyxvM
  public var property: T

  // CHECK-NO-STRIP: s4Test17UnavailableStructVyACyxGxcfC
  // CHECK-STRIP-NOT: s4Test17UnavailableStructVyACyxGxcfC
  public init(_ t: T) {
    self.property = t
  }

  // CHECK-NO-STRIP: s4Test17UnavailableStructV6methodyyF
  // CHECK-STRIP-NOT: s4Test17UnavailableStructV6methodyyF
  public func method() {}
}

@available(*, unavailable)
extension UnavailableStruct {
  // CHECK-NO-STRIP: s4Test17UnavailableStructV15extensionMethodyyF
  // CHECK-STRIP-NOT: s4Test17UnavailableStructV15extensionMethodyyF
  public func extensionMethod() {}
}

@available(*, unavailable)
public enum UnavailableEnum {
  case a, b

  // CHECK-NO-STRIP: s4Test15UnavailableEnumO6methodyyF
  // CHECK-STRIP-NOT: s4Test15UnavailableEnumO6methodyyF
  public func method() {}
}

@available(*, unavailable)
public class UnavailableClass<T> {
  // CHECK-NO-STRIP: s4Test16UnavailableClassC8propertyxvg
  // CHECK-NO-STRIP: s4Test16UnavailableClassC8propertyxvs
  // CHECK-NO-STRIP: s4Test16UnavailableClassC8propertyxvM
  // CHECK-STRIP-NOT: s4Test16UnavailableClassC8propertyxvg
  // CHECK-STRIP-NOT: s4Test16UnavailableClassC8propertyxvs
  // CHECK-STRIP-NOT: s4Test16UnavailableClassC8propertyxvM
  public var property: T

  // CHECK-NO-STRIP: s4Test16UnavailableClassCyACyxGxcfC
  // CHECK-NO-STRIP: s4Test16UnavailableClassCyACyxGxcfc
  // CHECK-STRIP-NOT: s4Test16UnavailableClassCyACyxGxcfC
  // CHECK-STRIP-NOT: s4Test16UnavailableClassCyACyxGxcfc
  public init(_ t: T) {
    self.property = t
  }

  // CHECK-NO-STRIP: s4Test16UnavailableClassCfd
  // CHECK-NO-STRIP: s4Test16UnavailableClassCfD
  // CHECK-STRIP-NOT: s4Test16UnavailableClassCfd
  // CHECK-STRIP-NOT: s4Test16UnavailableClassCfD
  deinit {}
}

public struct S<T> {
  // CHECK-NO-STRIP: s4Test1SV19unavailablePropertyxvg
  // CHECK-NO-STRIP: s4Test1SV19unavailablePropertyxvs
  // CHECK-NO-STRIP: s4Test1SV19unavailablePropertyxvM
  // CHECK-STRIP-NOT: s4Test1SV19unavailablePropertyxvg
  // CHECK-STRIP-NOT: s4Test1SV19unavailablePropertyxvs
  // CHECK-STRIP-NOT: s4Test1SV19unavailablePropertyxvM
  @available(*, unavailable)
  public var unavailableProperty: T {
    get { fatalError() }
    set { fatalError() }
    _modify { fatalError() }
  }

  // CHECK-NO-STRIP: s4Test1SVyACyxGxcfC
  // CHECK-STRIP-NOT: s4Test1SVyACyxGxcfC
  @available(*, unavailable)
  public init(_ t: T) {}

  // CHECK-NO-STRIP: s4Test1SV17unavailableMethodyyF
  // CHECK-STRIP-NOT: s4Test1SV17unavailableMethodyyF
  @available(*, unavailable)
  public func unavailableMethod() {}
}

@available(*, unavailable)
extension S {
  // CHECK-NO-STRIP: s4Test1SV28methodInUnavailableExtensionyyF
  // CHECK-STRIP-NOT: s4Test1SV28methodInUnavailableExtensionyyF
  public func methodInUnavailableExtension() {}
}

public enum E {
  case a

  @available(*, unavailable)
  case b

  // CHECK-NO-STRIP: s4Test1EO17unavailableMethodyyF
  // CHECK-STRIP-NOT: s4Test1EO17unavailableMethodyyF
  @available(*, unavailable)
  public func unavailableMethod() {}
}

public class C<T> {
  // CHECK-NO-STRIP: s4Test1CC19unavailablePropertyxvg
  // CHECK-NO-STRIP: s4Test1CC19unavailablePropertyxvs
  // CHECK-NO-STRIP: s4Test1CC19unavailablePropertyxvM
  // CHECK-STRIP-NOT: s4Test1CC19unavailablePropertyxvg
  // CHECK-STRIP-NOT: s4Test1CC19unavailablePropertyxvs
  // CHECK-STRIP-NOT: s4Test1CC19unavailablePropertyxvM
  @available(*, unavailable)
  public var unavailableProperty: T {
    get { fatalError() }
    set { fatalError() }
    _modify { fatalError() }
  }

  // CHECK-NO-STRIP: s4Test1CCyACyxGxcfC
  // CHECK-NO-STRIP: s4Test1CCyACyxGxcfc
  // CHECK-STRIP-NOT: s4Test1CCyACyxGxcfC
  // CHECK-STRIP-NOT: s4Test1CCyACyxGxcfc
  @available(*, unavailable)
  public init(_ t: T) {}

  // CHECK: s4Test1CCfd
  // CHECK: s4Test1CCfD
  deinit {}
}

public protocol P {
  func requirement()
}

@available(*, unavailable)
extension S: P {
  // CHECK-NO-STRIP: s4Test1SV11requirementyyF
  // CHECK-STRIP-NOT: s4Test1SV11requirementyyF
  public func requirement() {}
}

// CHECK-NO-STRIP: s4Test29unavailableFuncWithNestedTypeyyF
// CHECK-STRIP-NOT: s4Test29unavailableFuncWithNestedTypeyyF
@available(*, unavailable)
public func unavailableFuncWithNestedType() {
  // NOTE: Check lines for this decl are below since local types are emitted
  // after top level decls.
  struct Nested {
    // s4Test29unavailableFuncWithNestedTypeyyF0E0L_V6methodyyF
    public func method() {}
  }
}


// MARK: Local types SIL

// CHECK-NO-STRIP: s4Test29unavailableFuncWithNestedTypeyyF0E0L_V6methodyyF
// CHECK-STRIP-NOT: s4Test29unavailableFuncWithNestedTypeyyF0E0L_V6methodyyF

// MARK: SIL vtables

// CHECK-NO-STRIP: sil_vtable [serialized] UnavailableClass
// CHECK-STRIP-NOT: sil_vtable [serialized] UnavailableClass

// CHECK-NO-STRIP:      sil_vtable [serialized] C {
// CHECK-NO-STRIP-NEXT:   #C.unavailableProperty!getter:
// CHECK-NO-STRIP-NEXT:   #C.unavailableProperty!setter:
// CHECK-NO-STRIP-NEXT:   #C.unavailableProperty!modify:
// CHECK-NO-STRIP-NEXT:   #C.init!allocator:
// CHECK-NO-STRIP-NEXT:   #C.deinit!deallocator:
// CHECK-NO-STRIP-NEXT: }

// CHECK-STRIP:     sil_vtable [serialized] C {
// CHECK-STRIP-NOT:   #C.unavailableProperty!getter:
// CHECK-STRIP-NOT:   #C.unavailableProperty!setter:
// CHECK-STRIP-NOT:   #C.unavailableProperty!modify:
// CHECK-STRIP-NOT:   #C.init!allocator:
// CHECK-STRIP:       #C.deinit!deallocator:
// CHECK-STRIP:     }

// MARK: SIL witness tables

// CHECK-NO-STRIP: sil_witness_table [serialized] UnavailableEnum: Equatable module Test
// CHECK-STRIP-NOT: sil_witness_table [serialized] UnavailableEnum: Equatable module Test

// CHECK-NO-STRIP: sil_witness_table [serialized] UnavailableEnum: Hashable module Test
// CHECK-STRIP-NOT: sil_witness_table [serialized] UnavailableEnum: Hashable module Test

// CHECK: sil_witness_table [serialized] E: Equatable module Test

// CHECK: sil_witness_table [serialized] E: Hashable module Test

// CHECK-NO-STRIP: sil_witness_table [serialized] <T> S<T>: P module Test
// CHECK-STRIP-NOT: sil_witness_table [serialized] <T> S<T>: P module Test

// MARK: SIL properties

// CHECK-NO-STRIP: sil_property #UnavailableStruct.property<τ_0_0> ()
// CHECK-STRIP-NOT: sil_property #UnavailableStruct.property<τ_0_0> ()

// CHECK-NO-STRIP: sil_property #UnavailableEnum.hashValue ()
// CHECK-STRIP-NOT: sil_property #UnavailableEnum.hashValue ()

// CHECK-NO-STRIP: sil_property #UnavailableClass.property<τ_0_0> ()
// CHECK-STRIP-NOT: sil_property #UnavailableClass.property<τ_0_0> ()

// CHECK-NO-STRIP: sil_property #S.unavailableProperty<τ_0_0> ()
// CHECK-STRIP-NOT: sil_property #S.unavailableProperty<τ_0_0> ()

// CHECK: sil_property #E.hashValue ()

// CHECK-NO-STRIP: sil_property #C.unavailableProperty<τ_0_0> ()
// CHECK-STRIP-NOT: sil_property #C.unavailableProperty<τ_0_0> ()
