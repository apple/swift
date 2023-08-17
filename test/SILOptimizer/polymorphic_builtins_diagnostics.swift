// RUN: %target-swift-frontend -parse-stdlib -emit-sil -verify %s

import Swift

struct MyInt {
  var i: Builtin.Int64
}

@_transparent
func _isConcrete<T>(type: T.Type) -> Bool {
  return Bool(_builtinBooleanLiteral: Builtin.isConcrete(type))
}

func addVectorsNoDiagnostic(lhs: Builtin.Vec4xInt32, rhs: Builtin.Vec4xInt32) -> Builtin.Vec4xInt32 {
  return Builtin.generic_add(lhs, rhs)
}

func addVectorsEmitDiagnostic(lhs: MyInt, rhs: MyInt) -> MyInt {
  return Builtin.generic_add(lhs, rhs) // expected-error {{Argument of type 'MyInt' can not be passed as an argument to a Polymorphic builtin. Polymorphic builtins can only be passed arguments that are trivial builtin typed}}
}

func addVectorsGeneric<T>(lhs: T, rhs: T) -> T {
  return Builtin.generic_add(lhs, rhs) // expected-error {{Argument of type 'T' can not be passed as an argument to a Polymorphic builtin. Polymorphic builtins can only be passed arguments that are trivial builtin typed}}
}

@_transparent
func calleeAddVectorsGenericTransparentGuarded<T>(_ lhs: T, _ rhs: T) -> T {
  // This will be eliminated during constant propagation ensuring that when we
  // call in callerAddVectorsGenericTransparent, we do not get an error from our
  // underlying call.
  if _isConcrete(T.self) {
    return Builtin.generic_add(lhs, rhs)
  }
  return lhs
}

func callerAddVectorsGenericTransparent(_ lhs: Builtin.Vec4xInt32, _ rhs: Builtin.Vec4xInt32) -> Builtin.Vec4xInt32 {
  // Since after transparent inlining, we have the correct type, we should get an error here.q
  return calleeAddVectorsGenericTransparentGuarded(lhs, rhs)
}

@_transparent
func calleeAddVectorsGenericTransparentUnguarded<T>(_ lhs: T, _ rhs: T) -> T {
  return Builtin.generic_add(lhs, rhs)
}

func callerAddVectorsGenericTransparentUnguardedNoError(_ lhs: Builtin.Vec4xInt32, _ rhs: Builtin.Vec4xInt32) -> Builtin.Vec4xInt32 {
  return calleeAddVectorsGenericTransparentUnguarded(lhs, rhs)
}

func callerAddVectorsGenericTransparentUnguardedError(_ lhs: MyInt, _ rhs: MyInt) -> MyInt {
  return calleeAddVectorsGenericTransparentUnguarded(lhs, rhs) // expected-error {{Argument of type 'MyInt' can not be passed as an argument to a Polymorphic builtin. Polymorphic builtins can only be passed arguments that are trivial builtin typed}}
}
