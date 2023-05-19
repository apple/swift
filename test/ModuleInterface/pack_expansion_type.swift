// RUN: %empty-directory(%t)
// RUN: %target-swift-emit-module-interface(%t/PackExpansionType.swiftinterface) %s -module-name PackExpansionType -disable-availability-checking
// RUN: %FileCheck %s < %t/PackExpansionType.swiftinterface

// CHECK: #if compiler(>=5.3) && $ParameterPacks
// CHECK-NEXT: public func variadicFunction<each T, each U>(t: repeat each T, u: repeat each U) -> (repeat (each T, each U)) where (repeat (each T, each U)) : Any
public func variadicFunction<each T, each U>(t: repeat each T, u: repeat each U) -> (repeat (each T, each U)) {}
// CHECK-NEXT: #endif

// CHECK: #if compiler(>=5.3) && $ParameterPacks
// CHECK-NEXT: public func variadicFunctionWithRequirement<each T>(t: repeat each T) where repeat each T : Swift.Equatable
public func variadicFunctionWithRequirement<each T: Equatable>(t: repeat each T) {}
// CHECK-NEXT: #endif

// CHECK: #if compiler(>=5.3) && $ParameterPacks
// CHECK-NEXT: public struct VariadicType<each T> {
public struct VariadicType<each T> {
  // CHECK: public func variadicMethod<each U>(t: repeat each T, u: repeat each U) -> (repeat (each T, each U)) where (repeat (each T, each U)) : Any
  public func variadicMethod<each U>(t: repeat each T, u: repeat each  U) -> (repeat (each T, each U)) {}

  // CHECK: public func returnsSelf() -> PackExpansionType.VariadicType<repeat each T>
  public func returnsSelf() -> Self {}
}
// CHECK: }
// CHECK-NEXT: #endif

// CHECK: public func returnsVariadicType() -> PackExpansionType.VariadicType<>
public func returnsVariadicType() -> VariadicType< > {}

// CHECK: public func returnsVariadicType() -> PackExpansionType.VariadicType<Swift.Int, Swift.String, Swift.Float>
public func returnsVariadicType() -> VariadicType<Int, String, Float> {}

// CHECK: #if compiler(>=5.3) && $ParameterPacks
// CHECK-NEXT: public func returnsVariadicType<each T>() -> PackExpansionType.VariadicType<repeat each T>
public func returnsVariadicType<each T>() -> VariadicType<repeat each T> {}
// CHECK-NEXT: #endif

// CHECK: #if compiler(>=5.3) && $ParameterPacks
// CHECK-NEXT: public typealias VariadicAlias<each T> = PackExpansionType.VariadicType<repeat Swift.Array<each T>>
// CHECK-NEXT: #endif
public typealias VariadicAlias<each T> = VariadicType<repeat Array<each T>>

// CHECK: #if compiler(>=5.3) && $ParameterPacks
// CHECK-NEXT: public func returnsVariadicAlias() -> PackExpansionType.VariadicAlias<>
// CHECK-NEXT: #endif
public func returnsVariadicAlias() -> VariadicAlias< > {}

// CHECK: #if compiler(>=5.3) && $ParameterPacks
// CHECK-NEXT: public func returnsVariadicAlias() -> PackExpansionType.VariadicAlias<Swift.Int, Swift.String, Swift.Float>
// CHECK-NEXT: #endif
public func returnsVariadicAlias() -> VariadicAlias<Int, String, Float> {}

// CHECK: #if compiler(>=5.3) && $ParameterPacks
// CHECK-NEXT: public func returnsVariadicAlias<each T>() -> PackExpansionType.VariadicAlias<repeat each T>
// CHECK-NEXT: #endif
public func returnsVariadicAlias<each T>() -> VariadicAlias<repeat each T> {}
