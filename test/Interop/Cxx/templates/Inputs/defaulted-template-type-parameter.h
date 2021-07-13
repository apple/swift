#ifndef TEST_INTEROP_CXX_TEMPLATES_INPUTS_DEFAULTED_TEMPLATE_TYPE_PARAMETER_H
#define TEST_INTEROP_CXX_TEMPLATES_INPUTS_DEFAULTED_TEMPLATE_TYPE_PARAMETER_H

template <class>
struct ClassTemplate {};

struct X {
  enum class CtorPicked { dependent, arg, empty } picked;

  // Make sure we don't crash for dependent types.
  template <class T = void>
  X(ClassTemplate<T>) : picked(CtorPicked::dependent) {}

  template <class T = void>
  X(T) : picked(CtorPicked::arg) {}

  template <class = void>
  X() : picked(CtorPicked::empty) {}
};

template <class = void>
void defaultedTemplateTypeParam() {}

template <class T = void>
void defaultedTemplateTypeParamUsedInArgs(T) {}

template <class T = void>
T defaultedTemplateTypeParamUsedInReturn() {
  return 0;
}

template <class T = void>
void defaultedTemplateTypeParamAndDefaultedParam(T = 0) {}

template <class T>
void functionTemplateWithDefaultedParam(T = 0) {}

template <class T = void>
void defaultedTemplateTypeParamUsedInSignatureAndUnrealtedParam(int, T) {}

template <class = void>
void defaultedTemplateTypeParamAndUnrealtedParam(int) {}

template <class T = int>
void overloadedDefaultedTemplate(T) {}

void overloadedDefaultedTemplate(int) {}

template <typename T = int>
void defaultedTemplateReferenceTypeParam(T &t) {}

template <typename T = int>
void defaultedTemplatePointerTypeParam(T *t) {}

template <typename T = int>
void defaultedTemplatePointerReferenceTypeParam(T *&t) {}

template <typename T = int>
void defaultedTemplatePointerPointerTypeParam(T **t) {}

#endif // TEST_INTEROP_CXX_TEMPLATES_INPUTS_DEFAULTED_TEMPLATE_TYPE_PARAMETER_H
