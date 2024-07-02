// RUN: %target-run-simple-swift(-I %S/Inputs/ -Xfrontend -enable-experimental-cxx-interop)
//
// REQUIRES: executable_test

import StdlibUnittest
import Fields

var FieldsTestSuite = TestSuite("Getting and setting fields in base classes")

struct SwiftStructWrapper {
  let value: DerivedFromOneField
}

func optionalDerivedFromAll() -> DerivedFromAll? { DerivedFromAll() }

FieldsTestSuite.test("Fields from derived from all") {
  let derived = DerivedFromAll()
  expectEqual(derived.a, 1)
  expectEqual(derived.b, 2)
  expectEqual(derived.c, 3)
  expectEqual(derived.d, 4)
  expectEqual(derived.e, 5)
  expectEqual(derived.f, 6)

  var mutable = DerivedFromAll()
  mutable.a = 42
  mutable.d = 44
  mutable.e = 46
  mutable.f = 48

  expectEqual(mutable.a, 42)
  expectEqual(mutable.d, 44)
  expectEqual(mutable.e, 46)
  expectEqual(mutable.f, 48)
}

FieldsTestSuite.test("Optional") {
  let derived = optionalDerivedFromAll()
  expectEqual(derived!.a, 1)
  expectEqual(derived!.b, 2)
  expectEqual(derived!.c, 3)
  expectEqual(derived!.d, 4)
  expectEqual(derived!.e, 5)
  expectEqual(derived!.f, 6)
}

FieldsTestSuite.test("Struct holding derived from one field") {
  let derived = DerivedFromOneField()
  let s = SwiftStructWrapper(value: derived)

  expectEqual(s.value.value, 42)
}

FieldsTestSuite.test("Fields from derived from non trivial") {
  let derived = NonTrivialDerivedFromAll()
  expectEqual(derived.a, 1)
  expectEqual(derived.b, 2)
  expectEqual(derived.c, 3)
  expectEqual(derived.d, 4)
  expectEqual(derived.e, 5)
  expectEqual(derived.f, 6)

  var mutable = NonTrivialDerivedFromAll()
  mutable.a = 42
  mutable.d = 44
  mutable.e = 46
  mutable.f = 48

  expectEqual(mutable.a, 42)
  expectEqual(mutable.d, 44)
  expectEqual(mutable.e, 46)
  expectEqual(mutable.f, 48)
}

FieldsTestSuite.test("Derived from class template") {
  var derived = DerivedFromClassTemplate()
  derived.value = 42
  expectEqual(derived.value, 42)
}

FieldsTestSuite.test("Same field from derived") {
  var derived = DerivedWithSameField()
  derived.a = 42
  expectEqual(derived.a, 42)
}

extension CopyTrackedBaseClass {
    var swiftProp: CInt {
        return x
    }
}

FieldsTestSuite.test("Get field without copying base in the getter accessor") {
  let base = CopyTrackedBaseClass(0)
  var copyCounter = getCopyCounter().pointee
  expectEqual(base.swiftProp, 0)
  // Measure copy counter of a regular
  // property access for a C++ type to compare
  // it to see if any additional copies are
  // needed to access the property from the base class.
  let expectedCopyCountDiff = getCopyCounter().pointee - copyCounter

  let derived = CopyTrackedDerivedClass(234)
  copyCounter = getCopyCounter().pointee
  expectEqual(derived.x, 234)
  expectEqual(copyCounter, getCopyCounter().pointee - expectedCopyCountDiff)

  let derivedDerived = CopyTrackedDerivedDerivedClass(-11)
  copyCounter = getCopyCounter().pointee
  expectEqual(derivedDerived.x, -11)
  expectEqual(copyCounter, getCopyCounter().pointee - expectedCopyCountDiff)
}

FieldsTestSuite.test("Structs with virtual methods") {
  var derived = InheritFromStructsWithVirtualMethod()
  derived.d = 42
  expectEqual(derived.d, 42)
}

runAllTests()
