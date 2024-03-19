// RUN: %target-run-simple-swift(-I %S/Inputs -Xfrontend -enable-experimental-cxx-interop)
//
// REQUIRES: executable_test

import SameNameMetadata
import StdlibUnittest

var SameTypeMetadataTestSuite = TestSuite("Class Template Metadata")

func cmpMetadata<T, U>(_ _: T, _ _: U) -> Bool { "\(T.self)" == "\(U.self)" }

SameTypeMetadataTestSuite.test("RecordRecord") {
  let a = RecordRecord()
  let b = ParentRecord.RecordRecord()

  expectFalse(cmpMetadata(a, b))
  expectTrue(cmpMetadata(a, a))
}

SameTypeMetadataTestSuite.test("RecordEnumClass") {
  let a = RecordEnumClass.a
  let b = ParentRecord.RecordEnumClass.a

  expectFalse(cmpMetadata(a, b))
  expectTrue(cmpMetadata(a, a))
}

SameTypeMetadataTestSuite.test("NamespaceRecord") {
  let a = NamespaceRecord()
  let b = Namespace.NamespaceRecord()

  expectFalse(cmpMetadata(a, b))
  expectTrue(cmpMetadata(a, a))
}

SameTypeMetadataTestSuite.test("NamespaceEnumClass") {
  let a = NamespaceEnumClass.a
  let b = Namespace.NamespaceEnumClass.a

  expectFalse(cmpMetadata(a, b))
  expectTrue(cmpMetadata(a, a))
}

SameTypeMetadataTestSuite.test("NamespaceNamespace.NamespaceRecord") {
  let a = NamespaceRecord()
  let b = Namespace.NamespaceNamespace.NamespaceRecord()

  expectFalse(cmpMetadata(a, b))
  expectTrue(cmpMetadata(a, a))
}

runAllTests()
