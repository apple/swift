// Waiting for support for dependent types to be added back: rdar://103530256&90587703&89090706&89090631&89034704&89034440&83406001&83367285
// XFAIL: *

// RUN: %target-run-simple-swift(-I %S/Inputs -Xfrontend -enable-experimental-cxx-interop -Xfrontend -validate-tbd-against-ir=none -Xfrontend -disable-availability-checking)
//
// REQUIRES: executable_test
//
// Failing on 32-bit platforms (rdar://89296327) (rdar://89166707)
// XFAIL: PTRSIZE=32

import DependentTypes
import StdlibUnittest

var DependentTypesTestSuite = TestSuite("DependentTypesTestSuite")

DependentTypesTestSuite.test("Different dependent arg and return type.") {
  let m1 = differentDependentArgAndRet(M<Int>(value: 42), T: Int.self, U: Int.self) as! M<Int>
  expectEqual(m1.getValue(), 42)

  let m2 = dependantReturnTypeSameAsArg(M<Int>(value: 42), T: Int.self) as! M<Int>
  expectEqual(m2.getValue(), 42)
}

DependentTypesTestSuite.test("Different dependent inferred by arg.") {
  let m = dependantReturnTypeInferred(42) as! M<Int>
  expectEqual(m.getValue(), 42)
}

DependentTypesTestSuite.test("Instantiate the same function twice") {
  // Intentionally test the same thing twice.
  let m = dependantReturnTypeInferred(42) as! M<Int>
  expectEqual(m.getValue(), 42)

  let m2 = dependantReturnTypeInferred(42) as! M<Int>
  expectEqual(m2.getValue(), 42)
}

DependentTypesTestSuite.test("Multiple arguments (inferred type).") {
  let m = multipleArgs(M<Int>(value: 40), 2, 10) as! M<Int>
  expectEqual(m.getValue(), 42)
}

DependentTypesTestSuite.test("Multiple dependent arguments (inferred type).") {
  let m = multipleDependentArgsInferred(M<Int>(value: 42), M<CInt>(value: 0), 1, CInt(2)) as! M<Int>
  expectEqual(m.getValue(), 42)
}

DependentTypesTestSuite.test("Multiple dependent arguments (not inferred).") {
  let m = multipleDependentArgs(M<Int>(value: 42), M<CInt>(value: 0), T: Int.self, U: CInt.self) as! M<Int>
  expectEqual(m.getValue(), 42)
}

DependentTypesTestSuite.test("Takes inout argument and returns dependent type.") {
  var x = 42
  let m = refToDependent(&x) as! M<Int>
  expectEqual(m.getValue(), 42)
}

DependentTypesTestSuite.test("Takes const ref and returns dependent type.") {
  let m = constRefToDependent(42) as! M<Int>
  expectEqual(m.getValue(), 42)
}

DependentTypesTestSuite.test("Function template methods") {
  let m = M<Int>(value: 42)
  let m2 = m.memberDependentReturnType(CInt(32)) as! M<CInt>
  let val: Int = m2.memberDependentParamType(m)

  expectEqual(m2.getValue(), 32)
  expectEqual(val, 42)
}

DependentTypesTestSuite.test("Function template methods (mutable)") {
  var m = M<CInt>(value: 2)
  let m2 = m.memberDependentReturnTypeMutable(42) as! M<Int>
  expectEqual(m2.getValue(), 42)
}

DependentTypesTestSuite.test("Function template methods (static)") {
  let m = M<CInt>.memberDependentReturnTypeStatic(42) as! M<Int>
  expectEqual(m.getValue(), 42)

  let m2 = M<Int>.memberDependentReturnTypeStatic(CInt(32)) as! M<CInt>
  expectEqual(m2.getValue(), 32)
}

DependentTypesTestSuite.test("Complex different dependent return type inferrd.") {
  let m = complexDependantReturnTypeInferred(M<Int>(value: 42)) as! M<Int>
  expectEqual(m.getValue(), 42)
}

// TODO: Currently still failing: Could not cast value of type '__C.__CxxTemplateInst1MIlE' to 'Swift.Int'
DependentTypesTestSuite.test("Complex different dependent argument and return type") {
  let m = complexDifferentDependentArgAndRet(42, T: Int.self, U: Int.self) as! Int
  expectEqual(m, 42)

  let m2 = complexDependantReturnTypeSameAsArg(42, T: Int.self) as! Int
  expectEqual(m2, 42)
}

//TODO: Import issue: rdar://89028943
// DependentTypesTestSuite.test("Dependent to Reference") {
//   var x = 42
//   let m = dependentToRef(x) as! M<Int>
//   expectEqual(m.getValue(), 42)
// }

//TODO: Not imported: rdar://89034440
// DependentTypesTestSuite.test("Dependent Reference.") {
//   let m = dependentRef()
//   expectEqual(m.getValue(), 42)
// }

//TODO: Not imported: rdar://89034440
// DependentTypesTestSuite.test("Dependent reference and reference inferred") {
  // let m = dependentRefAndRefInferred(M<Int>(value: 40), 2) as! M<Int>
  // expectEqual(m.getValue(), 42)
// }

runAllTests()
