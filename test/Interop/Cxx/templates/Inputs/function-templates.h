#ifndef TEST_INTEROP_CXX_TEMPLATES_INPUTS_FUNCTION_TEMPLATES_H
#define TEST_INTEROP_CXX_TEMPLATES_INPUTS_FUNCTION_TEMPLATES_H

template <class T> T addSameTypeParams(T a, T b) { return a + b; }

template <class A, class B> A addMixedTypeParams(A a, B b) { return a + b; }

template <class T> T passThrough(T value) { return value; }

template <class T> const T passThroughConst(const T value) { return value; }

void takesString(const char *) {}
template <class T> void expectsConstCharPtr(T str) { takesString(str); }

template <long x> void hasNonTypeTemplateParameter() {}
template <long x = 0> void hasDefaultedNonTypeTemplateParameter() {}

// We cannot yet use this in Swift but, make sure we don't crash when parsing
// it.
template <class R, class T, class U> R templateParameterReturnType(T a, U b) {
  return a + b;
}

// Same here:
template <class T> void cannotInferTemplate() {}

struct HasVariadicMember {
  void test1(...) {}
  void test2(int, ...) {}
};

// TODO: We should support these types. Until then, make sure we don't crash when importing.
template<class... Ts>
void testPackExpansion(Ts...) { }

template<class T>
void testTypeOfExpr(T a, typeof(a + 1) b) { }

template<class T>
void testTypeOf(T a, typeof a b) { }

template<class T>
decltype(auto) testAuto(T arg) {
  return arg;
}

template <typename T>
struct ClassTemplate {
  T t;
};

template <typename T>
void takesPointerToDependent(ClassTemplate<T> *ct) {
  ct->t++;
}

template <typename T>
T usedInDeclType(T) {}

template <typename T>
void takesDeclTypePointer(decltype(usedInDeclType<T>()) *) {}

// TODO: Add tests for Decltype, UnaryTransform, and TemplateSpecialization with
// a dependent type once those are supported.

// TODO: Add test for DeducedTemplateSpecializationType once we support class templates.

// TODO: We don't yet support dependent types but we still shouldn't
// crash when importing one (https://github.com/apple/swift/issues/56206).
template <class T> struct Dep { using TT = T; };

template <class T> void useDependentType(typename Dep<T>::TT) {}

template <class T> void takesValue(T value) { }

template <class T> void lvalueReference(T &ref) { ref = 42; }
template <class T> void lvalueReferenceZero(T &ref) { ref = 0; }

template <class T> void constLvalueReference(const T &) {}

template <class T> bool constLvalueReferenceToBool(const T &t) { return t; }

template <class T> void forwardingReference(T &&) {}

template <class T> void PointerTemplateParameter(T*){}

namespace Orbiters {

template<class T>
void galileo(T) { }

template<class T, class U>
void cassini(T, U) { }

template<class T>
void magellan(T&) { }

} // namespace Orbiters

// We can't import these (and may never be able to in the case of "_Atomic"),
// but don't crash while trying.
namespace Unimportable {

template <class> struct Dependent {};
template <class T> void takesDependent(Dependent<T> d) {}

void takesAtomic(_Atomic(int) a) {}

struct HasImposibleMember {
  void memberTakesAtomic(_Atomic(int) a) {}
};

} // namespace Unimportable

#endif // TEST_INTEROP_CXX_TEMPLATES_INPUTS_FUNCTION_TEMPLATES_H
