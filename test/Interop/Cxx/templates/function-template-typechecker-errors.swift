// RUN: not %target-typecheck-verify-swift -I %S/Inputs -enable-experimental-cxx-interop 2>&1 | %FileCheck %s

// README: If you just added support for protocol composition to the
// ClangTypeConverter, please update this test to use a different type that we
// don't support so the error messages here are still tested.


import FunctionTemplates

// Make sure we don't import non-type template parameters.
// CHECK: error: unexpected error produced: cannot find 'hasNonTypeTemplateParameter' in scope
// CHECK: error: unexpected error produced: cannot find 'hasDefaultedNonTypeTemplateParameter' in scope
public func callIntegerTemplates() {
  hasNonTypeTemplateParameter()
  hasDefaultedNonTypeTemplateParameter()
}

// Use protocol composition to create a type that we cannot (yet) turn into a clang::QualType.
public protocol A { }
public protocol B { }
public protocol C { }

// CHECK: error: unexpected error produced: could not generate C++ types from the generic Swift types provided; the following Swift type(s) provided to 'passThrough' could not be converted: any A & B
public func caller1(x: A & B) -> A & B {
  return passThrough(x)
}

// CHECK: error: unexpected error produced: could not generate C++ types from the generic Swift types provided; the following Swift type(s) provided to 'addMixedTypeParams' could not be converted: any A & B, any A & C
public func caller2(x: A & B, y: A & C) -> A & B {
  return addMixedTypeParams(x, y)
}

// Make sure we emit an error and don't crash when failing to instantiate a function.
// CHECK: error: diagnostic produced elsewhere: no matching function for call to 'takesString'
// CHECK: note: diagnostic produced elsewhere: in instantiation of function template specialization 'expectsConstCharPtr<int>' requested here
// CHECK: note: diagnostic produced elsewhere: candidate function not viable: no known conversion from 'int' to 'const char *' for 1st argument
public func callexpectsConstCharPtr() {
  expectsConstCharPtr(0 as Int32)
}
