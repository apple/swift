#ifndef TEST_INTEROP_CXX_TEMPLATES_INPUTS_CLASS_TEMPLATE_NON_TYPE_PARAMETER_H
#define TEST_INTEROP_CXX_TEMPLATES_INPUTS_CLASS_TEMPLATE_NON_TYPE_PARAMETER_H

#if defined(__clang__)
using size_t = __SIZE_TYPE__;
#endif

template <class T, size_t Size> struct MagicArray { T t[Size]; };

typedef MagicArray<int, 2> MagicIntPair;
typedef MagicArray<int, 3> MagicIntTriple;

#endif // TEST_INTEROP_CXX_TEMPLATES_INPUTS_CLASS_TEMPLATE_NON_TYPE_PARAMETER_H
