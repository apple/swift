// RUN: %target-run-simple-swift(-I %S/Inputs -Xfrontend -enable-cxx-interop)
//
// REQUIRES: executable_test

import DefaultedTemplateTypeParameter
import StdlibUnittest

// The purpose of this test is to make sure that we correctly IRGen these
// templates and link them. The behavior is not important here (we test that
// elsewhere).
var DefaultedTemplateTestSuite = TestSuite("Defaulted Template Type Parameters")

DefaultedTemplateTestSuite.test("Correct ctor picked") {
  let x1 = X(0)
  expectEqual(x1.picked, .arg)
  
  let x2 = X()
  expectEqual(x2.picked, .empty)
}

DefaultedTemplateTestSuite.test("Function with defaulted template type parameters") {
  defaultedTemplateTypeParam()
  defaultedTemplateTypeParamUsedInArgs(0)
  let _: Int = defaultedTemplateTypeParamUsedInReturn()
  defaultedTemplateTypeParamAndDefaultedParam(0)
  functionTemplateWithDefaultedParam(0)
  defaultedTemplateTypeParamUsedInSignatureAndUnrealtedParam(0, 0)
  defaultedTemplateTypeParamAndUnrealtedParam(0)
}

DefaultedTemplateTestSuite.test("Overloaded function template is not ambiguous") {
  overloadedDefaultedTemplate(X())
  overloadedDefaultedTemplate(0)
}

DefaultedTemplateTestSuite.test("Pointer types") {
  var x = 0
  defaultedTemplateReferenceTypeParam(&x)
}

runAllTests()
