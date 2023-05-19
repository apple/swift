#ifndef TEST_INTEROP_CXX_REFERENCE_INPUTS_REFERENCE_H
#define TEST_INTEROP_CXX_REFERENCE_INPUTS_REFERENCE_H

int getStaticInt();
int &getStaticIntRef();
int &&getStaticIntRvalueRef();
const int &getConstStaticIntRef();
const int &&getConstStaticIntRvalueRef();

void setStaticInt(int);
void setStaticIntRef(int &);
void setStaticIntRvalueRef(int &&);
void setConstStaticIntRef(const int &);
void setConstStaticIntRvalueRef(const int &&);

auto getFuncRef() -> int (&)();
auto getFuncRvalueRef() -> int (&&)();

using ConstIntRefTypealias = const int &;

void setConstStaticIntRefTypealias(ConstIntRefTypealias ref);

using IntRefTypealias = int &;

void setStaticIntRefTypealias(IntRefTypealias ref);

template<class T>
struct ClassTemplate {};

template<class T>
const ClassTemplate<T> &refToDependent() { return ClassTemplate<T>(); }

// We cannot import "_Atomic" types. Make sure we fail gracefully instead of
// crashing when we have an "_Atomic" type or a reference to one.
void dontImportAtomicRef(_Atomic(int)&) { }

void takeConstRef(const int &);

template<class T>
T &refToTemplate(T &t) { return t; }

template<class T>
const T &constRefToTemplate(const T &t) { return t; }

template<class T>
void refToDependentParam(ClassTemplate<T> &param) { }

#endif // TEST_INTEROP_CXX_REFERENCE_INPUTS_REFERENCE_H
