// RUN: %target-run-simple-swift(-I %S/Inputs/ -Xfrontend -enable-experimental-cxx-interop)
//
// REQUIRES: executable_test

import StdlibUnittest
import Functions

var FunctionsTestSuite = TestSuite("Calling functions in base classes")

FunctionsTestSuite.test("Basic methods from derived") {
  let derived = Derived()
  expectEqual(String(cString: derived.constInBase()!), "Base::constInBase")
  expectEqual(String(cString: derived.takesArgsInBase(0, 1, 2)!), "Base::takesArgsInBase")

  var mutableDerived = derived
  expectEqual(String(cString: mutableDerived.mutatingInBase()!), "Base::mutatingInBase")
}

FunctionsTestSuite.test("NonTrivial methods from derived") {
  let derived = Derived()
  expectEqual(String(cString: derived.takesNonTrivialInBase(NonTrivial())!), "Base::takesNonTrivialInBase")
  _ = derived.returnsNonTrivialInBase()
}

// TODO: eventually support templates.
//FunctionsTestSuite.test("Template from derived") {
//  let derived = Derived()
// expectEqual(String(cString: derived.templateInBase(0)!), "Base::templateInBase")
//}

// TODO: eventually support static functions.
//FunctionsTestSuite.test("Static from derived") {
// expectEqual(String(cString: Derived.staticInBase()!), "Base::staticInBase")
//}

FunctionsTestSuite.test("Other base member from derived") {
  let derived = Derived()
  expectEqual(String(cString: derived.inOtherBase()!), "OtherBase::inOtherBase")
}

FunctionsTestSuite.test("Unambiguous members from derived") {
  let derived = Derived()
  expectEqual(derived.sameMethodNameSameSignature(), 21)
  expectEqual(derived.sameMethodDifferentSignature(1), 2)
  expectEqual(derived.sameMethodDifferentSignature(), 18)
}

FunctionsTestSuite.test("Basic methods from derived * 2") {
  let dd = DerivedFromDerived()
  expectEqual(String(cString: dd.constInBase()!), "Base::constInBase")
  expectEqual(String(cString: dd.takesArgsInBase(0, 1, 2)!), "Base::takesArgsInBase")

  var mutableDerived = dd
  expectEqual(String(cString: mutableDerived.mutatingInBase()!), "Base::mutatingInBase")
}

FunctionsTestSuite.test("NonTrivial methods from derived * 2") {
  let dd = DerivedFromDerived()
  expectEqual(String(cString: dd.takesNonTrivialInBase(NonTrivial())!), "Base::takesNonTrivialInBase")
  _ = dd.returnsNonTrivialInBase()
}

// TODO: eventually support static functions.
//FunctionsTestSuite.test("Static from derived * 2") {
// expectEqual(String(cString: DerivedFromDerived.staticInBase()!), "Base::staticInBase")
//}

FunctionsTestSuite.test("Other base member from derived * 2") {
  let dd = DerivedFromDerived()
  expectEqual(String(cString: dd.inOtherBase()!), "OtherBase::inOtherBase")
}

FunctionsTestSuite.test("base member from derived from non trivial") {
  let dnt = DerivedFromNonTrivial()
  expectEqual(String(cString: dnt.inNonTrivial()!), "NonTrivial::inNonTrivial")
  expectEqual(String(cString: dnt.inNonTrivialWithArgs(0, 1)!), "NonTrivial::inNonTrivialWithArgs")
}

FunctionsTestSuite.test("non-empty derived from empty class") {
  let derived = DerivedFromEmptyBaseClass()
  expectEqual(String(cString: derived.inBase()!), "EmptyBaseClass::inBase")
  expectEqual(derived.b, 42)
}

runAllTests()
