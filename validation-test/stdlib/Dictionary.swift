// RUN: %empty-directory(%t)
//
// RUN: %gyb %s -o %t/main.swift
// RUN: %target-clang -fobjc-arc %S/Inputs/SlurpFastEnumeration/SlurpFastEnumeration.m -c -o %t/SlurpFastEnumeration.o
// RUN: %line-directive %t/main.swift -- %target-build-swift %S/Inputs/DictionaryKeyValueTypes.swift %S/Inputs/DictionaryKeyValueTypesObjC.swift %t/main.swift -I %S/Inputs/SlurpFastEnumeration/ -Xlinker %t/SlurpFastEnumeration.o -o %t/Dictionary -Xfrontend -disable-access-control
//
// RUN: %target-codesign %t/Dictionary && %line-directive %t/main.swift -- %target-run %t/Dictionary
// REQUIRES: executable_test

import StdlibUnittest
import StdlibCollectionUnittest


#if _runtime(_ObjC)
import Foundation
import StdlibUnittestFoundationExtras
#endif

extension Dictionary {
  func _rawIdentifier() -> Int {
    return unsafeBitCast(self, to: Int.self)
  }
}

// Check that the generic parameters are called 'Key' and 'Value'.
protocol TestProtocol1 {}

struct TestError: Error {}

extension Dictionary where Key : TestProtocol1, Value : TestProtocol1 {
  var _keyValueAreTestProtocol1: Bool {
    fatalError("not implemented")
  }
}

extension DictionaryIndex where Key : TestProtocol1, Value : TestProtocol1 {
  var _keyValueAreTestProtocol1: Bool {
    fatalError("not implemented")
  }
}

extension DictionaryIterator
  where Key : TestProtocol1, Value : TestProtocol1 {

  var _keyValueAreTestProtocol1: Bool {
    fatalError("not implemented")
  }
}

var DictionaryTestSuite = TestSuite("Dictionary")

DictionaryTestSuite.test("AssociatedTypes") {
  typealias Collection = Dictionary<MinimalHashableValue, OpaqueValue<Int>>
  expectCollectionAssociatedTypes(
    collectionType: Collection.self,
    iteratorType: DictionaryIterator<MinimalHashableValue, OpaqueValue<Int>>.self,
    subSequenceType: Slice<Collection>.self,
    indexType: DictionaryIndex<MinimalHashableValue, OpaqueValue<Int>>.self,
    indicesType: DefaultIndices<Collection>.self)
}

DictionaryTestSuite.test("sizeof") {
  var dict = [1: "meow", 2: "meow"]
#if _pointerBitWidth(_32)
  expectEqual(4, MemoryLayout.size(ofValue: dict))
#else
  expectEqual(8, MemoryLayout.size(ofValue: dict))
#endif
}

DictionaryTestSuite.test("Index.Hashable") {
  let d = [1: "meow", 2: "meow", 3: "meow"]
  let e = Dictionary(uniqueKeysWithValues: zip(d.indices, d))
  expectEqual(d.count, e.count)
  expectNotNil(e[d.startIndex])
}

DictionaryTestSuite.test("valueDestruction") {
  var d1 = Dictionary<Int, TestValueTy>()
  for i in 100...110 {
    d1[i] = TestValueTy(i)
  }

  var d2 = Dictionary<TestKeyTy, TestValueTy>()
  for i in 100...110 {
    d2[TestKeyTy(i)] = TestValueTy(i)
  }
}

DictionaryTestSuite.test("COW.Smoke") {
  var d1 = Dictionary<TestKeyTy, TestValueTy>(minimumCapacity: 10)
  let identity1 = d1._rawIdentifier()

  d1[TestKeyTy(10)] = TestValueTy(1010)
  d1[TestKeyTy(20)] = TestValueTy(1020)
  d1[TestKeyTy(30)] = TestValueTy(1030)

  var d2 = d1
  _fixLifetime(d2)
  assert(identity1 == d2._rawIdentifier())

  d2[TestKeyTy(40)] = TestValueTy(2040)
  assert(identity1 != d2._rawIdentifier())

  d1[TestKeyTy(50)] = TestValueTy(1050)
  assert(identity1 == d1._rawIdentifier())

  // Keep variables alive.
  _fixLifetime(d1)
  _fixLifetime(d2)
}

func getCOWFastDictionary() -> Dictionary<Int, Int> {
  var d = Dictionary<Int, Int>(minimumCapacity: 10)
  d[10] = 1010
  d[20] = 1020
  d[30] = 1030
  return d
}

func getCOWFastDictionaryWithCOWValues() -> Dictionary<Int, TestValueCOWTy> {
  var d = Dictionary<Int, TestValueCOWTy>(minimumCapacity: 10)
  d[10] = TestValueCOWTy(1010)
  d[20] = TestValueCOWTy(1020)
  d[30] = TestValueCOWTy(1030)
  return d
}

func getCOWSlowDictionary() -> Dictionary<TestKeyTy, TestValueTy> {
  var d = Dictionary<TestKeyTy, TestValueTy>(minimumCapacity: 10)
  d[TestKeyTy(10)] = TestValueTy(1010)
  d[TestKeyTy(20)] = TestValueTy(1020)
  d[TestKeyTy(30)] = TestValueTy(1030)
  return d
}

func getCOWSlowEquatableDictionary()
    -> Dictionary<TestKeyTy, TestEquatableValueTy> {
  var d = Dictionary<TestKeyTy, TestEquatableValueTy>(minimumCapacity: 10)
  d[TestKeyTy(10)] = TestEquatableValueTy(1010)
  d[TestKeyTy(20)] = TestEquatableValueTy(1020)
  d[TestKeyTy(30)] = TestEquatableValueTy(1030)
  return d
}

func expectUnique<T: AnyObject>(_ v: inout T) {
  expectTrue(isKnownUniquelyReferenced(&v))
}

func expectUnique<T: AnyObject>(_ v: inout T?) {
  guard v != nil else { return }
  expectTrue(isKnownUniquelyReferenced(&v))
}

DictionaryTestSuite.test("COW.Fast.IndexesDontAffectUniquenessCheck") {
  var d = getCOWFastDictionary()
  let identity1 = d._rawIdentifier()

  let startIndex = d.startIndex
  let endIndex = d.endIndex
  assert(startIndex != endIndex)
  assert(startIndex < endIndex)
  assert(startIndex <= endIndex)
  assert(!(startIndex >= endIndex))
  assert(!(startIndex > endIndex))

  assert(identity1 == d._rawIdentifier())

  d[40] = 2040
  assert(identity1 == d._rawIdentifier())

  // Keep indexes alive during the calls above.
  _fixLifetime(startIndex)
  _fixLifetime(endIndex)
}

DictionaryTestSuite.test("COW.Slow.IndexesDontAffectUniquenessCheck") {
  var d = getCOWSlowDictionary()
  let identity1 = d._rawIdentifier()

  let startIndex = d.startIndex
  let endIndex = d.endIndex
  assert(startIndex != endIndex)
  assert(startIndex < endIndex)
  assert(startIndex <= endIndex)
  assert(!(startIndex >= endIndex))
  assert(!(startIndex > endIndex))
  assert(identity1 == d._rawIdentifier())

  d[TestKeyTy(40)] = TestValueTy(2040)
  assert(identity1 == d._rawIdentifier())

  // Keep indexes alive during the calls above.
  _fixLifetime(startIndex)
  _fixLifetime(endIndex)
}


DictionaryTestSuite.test("COW.Fast.SubscriptWithIndexDoesNotReallocate") {
  let d = getCOWFastDictionary()
  let identity1 = d._rawIdentifier()

  let startIndex = d.startIndex
  let empty = startIndex == d.endIndex
  assert((d.startIndex < d.endIndex) == !empty)
  assert(d.startIndex <= d.endIndex)
  assert((d.startIndex >= d.endIndex) == empty)
  assert(!(d.startIndex > d.endIndex))
  assert(identity1 == d._rawIdentifier())

  assert(d[startIndex].1 != 0)
  assert(identity1 == d._rawIdentifier())
}

DictionaryTestSuite.test("COW.Slow.SubscriptWithIndexDoesNotReallocate") {
  let d = getCOWSlowDictionary()
  let identity1 = d._rawIdentifier()

  let startIndex = d.startIndex
  let empty = startIndex == d.endIndex
  assert((d.startIndex < d.endIndex) == !empty)
  assert(d.startIndex <= d.endIndex)
  assert((d.startIndex >= d.endIndex) == empty)
  assert(!(d.startIndex > d.endIndex))
  assert(identity1 == d._rawIdentifier())

  assert(d[startIndex].1.value != 0)
  assert(identity1 == d._rawIdentifier())
}


DictionaryTestSuite.test("COW.Fast.SubscriptWithKeyDoesNotReallocate")
  .code {
  var d = getCOWFastDictionary()
  let identity1 = d._rawIdentifier()

  assert(d[10]! == 1010)
  assert(identity1 == d._rawIdentifier())

  // Insert a new key-value pair.
  d[40] = 2040
  assert(identity1 == d._rawIdentifier())
  assert(d.count == 4)
  assert(d[10]! == 1010)
  assert(d[20]! == 1020)
  assert(d[30]! == 1030)
  assert(d[40]! == 2040)

  // Overwrite a value in existing binding.
  d[10] = 2010
  assert(identity1 == d._rawIdentifier())
  assert(d.count == 4)
  assert(d[10]! == 2010)
  assert(d[20]! == 1020)
  assert(d[30]! == 1030)
  assert(d[40]! == 2040)

  // Delete an existing key.
  d[10] = nil
  assert(identity1 == d._rawIdentifier())
  assert(d.count == 3)
  assert(d[20]! == 1020)
  assert(d[30]! == 1030)
  assert(d[40]! == 2040)

  // Try to delete a key that does not exist.
  d[42] = nil
  assert(identity1 == d._rawIdentifier())
  assert(d.count == 3)
  assert(d[20]! == 1020)
  assert(d[30]! == 1030)
  assert(d[40]! == 2040)

  do {
    var d2: [MinimalHashableValue : OpaqueValue<Int>] = [:]
    MinimalHashableValue.timesEqualEqualWasCalled = 0
    MinimalHashableValue.timesHashIntoWasCalled = 0
    expectNil(d2[MinimalHashableValue(42)])

    // If the dictionary is empty, we shouldn't be computing the hash value of
    // the provided key.
    expectEqual(0, MinimalHashableValue.timesEqualEqualWasCalled)
    expectEqual(0, MinimalHashableValue.timesHashIntoWasCalled)
  }
}

DictionaryTestSuite.test("COW.Slow.SubscriptWithKeyDoesNotReallocate")
  .code {

  var d = getCOWSlowDictionary()
  let identity1 = d._rawIdentifier()

  assert(d[TestKeyTy(10)]!.value == 1010)
  assert(identity1 == d._rawIdentifier())

  // Insert a new key-value pair.
  d[TestKeyTy(40)] = TestValueTy(2040)
  assert(identity1 == d._rawIdentifier())
  assert(d.count == 4)
  assert(d[TestKeyTy(10)]!.value == 1010)
  assert(d[TestKeyTy(20)]!.value == 1020)
  assert(d[TestKeyTy(30)]!.value == 1030)
  assert(d[TestKeyTy(40)]!.value == 2040)

  // Overwrite a value in existing binding.
  d[TestKeyTy(10)] = TestValueTy(2010)
  assert(identity1 == d._rawIdentifier())
  assert(d.count == 4)
  assert(d[TestKeyTy(10)]!.value == 2010)
  assert(d[TestKeyTy(20)]!.value == 1020)
  assert(d[TestKeyTy(30)]!.value == 1030)
  assert(d[TestKeyTy(40)]!.value == 2040)

  // Delete an existing key.
  d[TestKeyTy(10)] = nil
  assert(identity1 == d._rawIdentifier())
  assert(d.count == 3)
  assert(d[TestKeyTy(20)]!.value == 1020)
  assert(d[TestKeyTy(30)]!.value == 1030)
  assert(d[TestKeyTy(40)]!.value == 2040)

  // Try to delete a key that does not exist.
  d[TestKeyTy(42)] = nil
  assert(identity1 == d._rawIdentifier())
  assert(d.count == 3)
  assert(d[TestKeyTy(20)]!.value == 1020)
  assert(d[TestKeyTy(30)]!.value == 1030)
  assert(d[TestKeyTy(40)]!.value == 2040)

  do {
    var d2: [MinimalHashableClass : OpaqueValue<Int>] = [:]
    MinimalHashableClass.timesEqualEqualWasCalled = 0
    MinimalHashableClass.timesHashIntoWasCalled = 0

    expectNil(d2[MinimalHashableClass(42)])

    // If the dictionary is empty, we shouldn't be computing the hash value of
    // the provided key.
    expectEqual(0, MinimalHashableClass.timesEqualEqualWasCalled)
    expectEqual(0, MinimalHashableClass.timesHashIntoWasCalled)
  }
}

DictionaryTestSuite.test("COW.Slow.SubscriptWithKey.Uniqueness") {
  var d = getCOWSlowEquatableDictionary()
  expectUnique(&d[TestKeyTy(20)])
}

DictionaryTestSuite.test("COW.Fast.UpdateValueForKeyDoesNotReallocate") {
  do {
    var d1 = getCOWFastDictionary()
    let identity1 = d1._rawIdentifier()

    // Insert a new key-value pair.
    assert(d1.updateValue(2040, forKey: 40) == .none)
    assert(identity1 == d1._rawIdentifier())
    assert(d1[40]! == 2040)

    // Overwrite a value in existing binding.
    assert(d1.updateValue(2010, forKey: 10)! == 1010)
    assert(identity1 == d1._rawIdentifier())
    assert(d1[10]! == 2010)
  }

  do {
    var d1 = getCOWFastDictionary()
    let identity1 = d1._rawIdentifier()

    var d2 = d1
    assert(identity1 == d1._rawIdentifier())
    assert(identity1 == d2._rawIdentifier())

    // Insert a new key-value pair.
    d2.updateValue(2040, forKey: 40)
    assert(identity1 == d1._rawIdentifier())
    assert(identity1 != d2._rawIdentifier())

    assert(d1.count == 3)
    assert(d1[10]! == 1010)
    assert(d1[20]! == 1020)
    assert(d1[30]! == 1030)
    assert(d1[40] == .none)

    assert(d2.count == 4)
    assert(d2[10]! == 1010)
    assert(d2[20]! == 1020)
    assert(d2[30]! == 1030)
    assert(d2[40]! == 2040)

    // Keep variables alive.
    _fixLifetime(d1)
    _fixLifetime(d2)
  }

  do {
    var d1 = getCOWFastDictionary()
    let identity1 = d1._rawIdentifier()

    var d2 = d1
    assert(identity1 == d1._rawIdentifier())
    assert(identity1 == d2._rawIdentifier())

    // Overwrite a value in existing binding.
    d2.updateValue(2010, forKey: 10)
    assert(identity1 == d1._rawIdentifier())
    assert(identity1 != d2._rawIdentifier())

    assert(d1.count == 3)
    assert(d1[10]! == 1010)
    assert(d1[20]! == 1020)
    assert(d1[30]! == 1030)

    assert(d2.count == 3)
    assert(d2[10]! == 2010)
    assert(d2[20]! == 1020)
    assert(d2[30]! == 1030)

    // Keep variables alive.
    _fixLifetime(d1)
    _fixLifetime(d2)
  }
}

DictionaryTestSuite.test("COW.Slow.UpdateValueForKeyDoesNotReallocate") {
  do {
    var d1 = getCOWSlowDictionary()
    let identity1 = d1._rawIdentifier()

    // Insert a new key-value pair.
    assert(d1.updateValue(TestValueTy(2040), forKey: TestKeyTy(40)) == nil)
    assert(identity1 == d1._rawIdentifier())
    assert(d1.count == 4)
    assert(d1[TestKeyTy(40)]!.value == 2040)

    // Overwrite a value in existing binding.
    assert(d1.updateValue(TestValueTy(2010), forKey: TestKeyTy(10))!.value == 1010)
    assert(identity1 == d1._rawIdentifier())
    assert(d1.count == 4)
    assert(d1[TestKeyTy(10)]!.value == 2010)
  }

  do {
    var d1 = getCOWSlowDictionary()
    let identity1 = d1._rawIdentifier()

    var d2 = d1
    assert(identity1 == d1._rawIdentifier())
    assert(identity1 == d2._rawIdentifier())

    // Insert a new key-value pair.
    d2.updateValue(TestValueTy(2040), forKey: TestKeyTy(40))
    assert(identity1 == d1._rawIdentifier())
    assert(identity1 != d2._rawIdentifier())

    assert(d1.count == 3)
    assert(d1[TestKeyTy(10)]!.value == 1010)
    assert(d1[TestKeyTy(20)]!.value == 1020)
    assert(d1[TestKeyTy(30)]!.value == 1030)
    assert(d1[TestKeyTy(40)] == nil)

    assert(d2.count == 4)
    assert(d2[TestKeyTy(10)]!.value == 1010)
    assert(d2[TestKeyTy(20)]!.value == 1020)
    assert(d2[TestKeyTy(30)]!.value == 1030)
    assert(d2[TestKeyTy(40)]!.value == 2040)

    // Keep variables alive.
    _fixLifetime(d1)
    _fixLifetime(d2)
  }

  do {
    var d1 = getCOWSlowDictionary()
    let identity1 = d1._rawIdentifier()

    var d2 = d1
    assert(identity1 == d1._rawIdentifier())
    assert(identity1 == d2._rawIdentifier())

    // Overwrite a value in existing binding.
    d2.updateValue(TestValueTy(2010), forKey: TestKeyTy(10))
    assert(identity1 == d1._rawIdentifier())
    assert(identity1 != d2._rawIdentifier())

    assert(d1.count == 3)
    assert(d1[TestKeyTy(10)]!.value == 1010)
    assert(d1[TestKeyTy(20)]!.value == 1020)
    assert(d1[TestKeyTy(30)]!.value == 1030)

    assert(d2.count == 3)
    assert(d2[TestKeyTy(10)]!.value == 2010)
    assert(d2[TestKeyTy(20)]!.value == 1020)
    assert(d2[TestKeyTy(30)]!.value == 1030)

    // Keep variables alive.
    _fixLifetime(d1)
    _fixLifetime(d2)
  }
}

DictionaryTestSuite.test("COW.Fast.MergeSequenceDoesNotReallocate")
  .code {

  do {
    var d1 = getCOWFastDictionary()
    let identity1 = d1._rawIdentifier()

    // Merge some new values.
    d1.merge([(40, 2040), (50, 2050)]) { _, y in y }
    assert(identity1 == d1._rawIdentifier())
    assert(d1.count == 5)
    assert(d1[50]! == 2050)

    // Merge and overwrite some existing values.
    d1.merge([(10, 2010), (60, 2060)]) { _, y in y }
    assert(identity1 == d1._rawIdentifier())
    assert(d1.count == 6)
    assert(d1[10]! == 2010)
    assert(d1[60]! == 2060)

    // Merge, keeping existing values.
    d1.merge([(30, 2030), (70, 2070)]) { x, _ in x }
    assert(identity1 == d1._rawIdentifier())
    assert(d1.count == 7)
    assert(d1[30]! == 1030)
    assert(d1[70]! == 2070)

    let d2 = d1.merging([(40, 3040), (80, 3080)]) { _, y in y }
    assert(identity1 == d1._rawIdentifier())
    assert(identity1 != d2._rawIdentifier())
    assert(d2.count == 8)
    assert(d2[40]! == 3040)
    assert(d2[80]! == 3080)
  }

  do {
    var d1 = getCOWFastDictionary()
    let identity1 = d1._rawIdentifier()

    var d2 = d1
    assert(identity1 == d1._rawIdentifier())
    assert(identity1 == d2._rawIdentifier())

    // Merge some new values.
    d2.merge([(40, 2040), (50, 2050)]) { _, y in y }
    assert(identity1 == d1._rawIdentifier())
    assert(identity1 != d2._rawIdentifier())

    assert(d1.count == 3)
    assert(d1[10]! == 1010)
    assert(d1[20]! == 1020)
    assert(d1[30]! == 1030)
    assert(d1[40] == nil)

    assert(d2.count == 5)
    assert(d2[10]! == 1010)
    assert(d2[20]! == 1020)
    assert(d2[30]! == 1030)
    assert(d2[40]! == 2040)
    assert(d2[50]! == 2050)

    // Keep variables alive.
    _fixLifetime(d1)
    _fixLifetime(d2)
  }

  do {
    var d1 = getCOWFastDictionary()
    let identity1 = d1._rawIdentifier()

    var d2 = d1
    assert(identity1 == d1._rawIdentifier())
    assert(identity1 == d2._rawIdentifier())

    // Merge and overwrite some existing values.
    d2.merge([(10, 2010)]) { _, y in y }
    assert(identity1 == d1._rawIdentifier())
    assert(identity1 != d2._rawIdentifier())

    assert(d1.count == 3)
    assert(d1[10]! == 1010)
    assert(d1[20]! == 1020)
    assert(d1[30]! == 1030)

    assert(d2.count == 3)
    assert(d2[10]! == 2010)
    assert(d2[20]! == 1020)
    assert(d2[30]! == 1030)

    // Keep variables alive.
    _fixLifetime(d1)
    _fixLifetime(d2)
  }

  do {
    var d1 = getCOWFastDictionary()
    let identity1 = d1._rawIdentifier()

    var d2 = d1
    assert(identity1 == d1._rawIdentifier())
    assert(identity1 == d2._rawIdentifier())

    // Merge, keeping existing values.
    d2.merge([(10, 2010)]) { x, _ in x }
    assert(identity1 == d1._rawIdentifier())
    assert(identity1 != d2._rawIdentifier())

    assert(d1.count == 3)
    assert(d1[10]! == 1010)
    assert(d1[20]! == 1020)
    assert(d1[30]! == 1030)

    assert(d2.count == 3)
    assert(d2[10]! == 1010)
    assert(d2[20]! == 1020)
    assert(d2[30]! == 1030)

    // Keep variables alive.
    _fixLifetime(d1)
    _fixLifetime(d2)
  }
}

DictionaryTestSuite.test("COW.Fast.MergeDictionaryDoesNotReallocate")
  .code {

  do {
    var d1 = getCOWFastDictionary()
    let identity1 = d1._rawIdentifier()

    // Merge some new values.
    d1.merge([40: 2040, 50: 2050]) { _, y in y }
    assert(identity1 == d1._rawIdentifier())
    assert(d1.count == 5)
    assert(d1[50]! == 2050)

    // Merge and overwrite some existing values.
    d1.merge([10: 2010, 60: 2060]) { _, y in y }
    assert(identity1 == d1._rawIdentifier())
    assert(d1.count == 6)
    assert(d1[10]! == 2010)
    assert(d1[60]! == 2060)

    // Merge, keeping existing values.
    d1.merge([30: 2030, 70: 2070]) { x, _ in x }
    assert(identity1 == d1._rawIdentifier())
    assert(d1.count == 7)
    assert(d1[30]! == 1030)
    assert(d1[70]! == 2070)

    let d2 = d1.merging([40: 3040, 80: 3080]) { _, y in y }
    assert(identity1 == d1._rawIdentifier())
    assert(identity1 != d2._rawIdentifier())
    assert(d2.count == 8)
    assert(d2[40]! == 3040)
    assert(d2[80]! == 3080)
  }

  do {
    var d1 = getCOWFastDictionary()
    let identity1 = d1._rawIdentifier()

    var d2 = d1
    assert(identity1 == d1._rawIdentifier())
    assert(identity1 == d2._rawIdentifier())

    // Merge some new values.
    d2.merge([40: 2040, 50: 2050]) { _, y in y }
    assert(identity1 == d1._rawIdentifier())
    assert(identity1 != d2._rawIdentifier())

    assert(d1.count == 3)
    assert(d1[10]! == 1010)
    assert(d1[20]! == 1020)
    assert(d1[30]! == 1030)
    assert(d1[40] == nil)

    assert(d2.count == 5)
    assert(d2[10]! == 1010)
    assert(d2[20]! == 1020)
    assert(d2[30]! == 1030)
    assert(d2[40]! == 2040)
    assert(d2[50]! == 2050)

    // Keep variables alive.
    _fixLifetime(d1)
    _fixLifetime(d2)
  }

  do {
    var d1 = getCOWFastDictionary()
    let identity1 = d1._rawIdentifier()

    var d2 = d1
    assert(identity1 == d1._rawIdentifier())
    assert(identity1 == d2._rawIdentifier())

    // Merge and overwrite some existing values.
    d2.merge([10: 2010]) { _, y in y }
    assert(identity1 == d1._rawIdentifier())
    assert(identity1 != d2._rawIdentifier())

    assert(d1.count == 3)
    assert(d1[10]! == 1010)
    assert(d1[20]! == 1020)
    assert(d1[30]! == 1030)

    assert(d2.count == 3)
    assert(d2[10]! == 2010)
    assert(d2[20]! == 1020)
    assert(d2[30]! == 1030)

    // Keep variables alive.
    _fixLifetime(d1)
    _fixLifetime(d2)
  }

  do {
    var d1 = getCOWFastDictionary()
    let identity1 = d1._rawIdentifier()

    var d2 = d1
    assert(identity1 == d1._rawIdentifier())
    assert(identity1 == d2._rawIdentifier())

    // Merge, keeping existing values.
    d2.merge([10: 2010]) { x, _ in x }
    assert(identity1 == d1._rawIdentifier())
    assert(identity1 != d2._rawIdentifier())

    assert(d1.count == 3)
    assert(d1[10]! == 1010)
    assert(d1[20]! == 1020)
    assert(d1[30]! == 1030)

    assert(d2.count == 3)
    assert(d2[10]! == 1010)
    assert(d2[20]! == 1020)
    assert(d2[30]! == 1030)

    // Keep variables alive.
    _fixLifetime(d1)
    _fixLifetime(d2)
  }
}


DictionaryTestSuite.test("Merge.ThrowingIsSafe") {
  var d: [TestKeyTy: TestValueTy] = [
    TestKeyTy(10): TestValueTy(1),
    TestKeyTy(20): TestValueTy(2),
    TestKeyTy(30): TestValueTy(3),
  ]

  let d2: [TestKeyTy: TestValueTy] = [
    TestKeyTy(40): TestValueTy(4),
    TestKeyTy(50): TestValueTy(5),
    TestKeyTy(10): TestValueTy(1),
  ]

  struct TE: Error {}
  do {
    // Throwing must not leave the dictionary in an inconsistent state.
    try d.merge(d2) { v1, v2 in throw TE() }
    expectTrue(false, "merge did not throw")
  } catch {
    expectTrue(error is TE)
  }
}

DictionaryTestSuite.test("COW.Fast.DefaultedSubscriptDoesNotReallocate") {
  do {
    var d1 = getCOWFastDictionary()
    let identity1 = d1._rawIdentifier()

    // No mutation on access.
    assert(d1[10, default: 0] + 1 == 1011)
    assert(d1[40, default: 0] + 1 == 1)
    assert(identity1 == d1._rawIdentifier())
    assert(d1[10]! == 1010)

    // Increment existing in place.
    d1[10, default: 0] += 1
    assert(identity1 == d1._rawIdentifier())
    assert(d1[10]! == 1011)

    // Add incremented default value.
    d1[40, default: 0] += 1
    assert(identity1 == d1._rawIdentifier())
    assert(d1[40]! == 1)
  }

  do {
    var d1 = getCOWFastDictionary()
    let identity1 = d1._rawIdentifier()

    var d2 = d1
    assert(identity1 == d1._rawIdentifier())
    assert(identity1 == d2._rawIdentifier())

    // No mutation on access.
    assert(d2[10, default: 0] + 1 == 1011)
    assert(d2[40, default: 0] + 1 == 1)
    assert(identity1 == d1._rawIdentifier())
    assert(identity1 == d2._rawIdentifier())

    // Increment existing in place.
    d2[10, default: 0] += 1
    assert(identity1 == d1._rawIdentifier())
    assert(identity1 != d2._rawIdentifier())

    assert(d1[10]! == 1010)
    assert(d2[10]! == 1011)

    // Keep variables alive.
    _fixLifetime(d1)
    _fixLifetime(d2)
  }

  do {
    var d1 = getCOWFastDictionary()
    let identity1 = d1._rawIdentifier()

    var d2 = d1
    assert(identity1 == d1._rawIdentifier())
    assert(identity1 == d2._rawIdentifier())

    // Add incremented default value.
    d2[40, default: 0] += 1
    assert(identity1 == d1._rawIdentifier())
    assert(identity1 != d2._rawIdentifier())

    assert(d1[40] == nil)
    assert(d2[40]! == 1)

    // Keep variables alive.
    _fixLifetime(d1)
    _fixLifetime(d2)
  }
}

DictionaryTestSuite.test("COW.Fast.DefaultedSubscriptDoesNotCopyValue") {
  do {
    var d = getCOWFastDictionaryWithCOWValues()
    let identityValue30 = d[30]!.baseAddress

    // Increment the value without having to reallocate the underlying Base
    // instance, as uniquely referenced.
    d[30, default: TestValueCOWTy()].value += 1
    assert(identityValue30 == d[30]!.baseAddress)
    assert(d[30]!.value == 1031)

    let value40 = TestValueCOWTy()
    let identityValue40 = value40.baseAddress

    // Increment the value, reallocating the underlying Base, as not uniquely
    // referenced.
    d[40, default: value40].value += 1
    assert(identityValue40 != d[40]!.baseAddress)
    assert(d[40]!.value == 1)

    // Keep variables alive.
    _fixLifetime(d)
    _fixLifetime(value40)
  }
}

DictionaryTestSuite.test("COW.Slow.DefaultedSubscript.Uniqueness") {
  var d = getCOWSlowEquatableDictionary()

  expectUnique(&d[TestKeyTy(20), default: TestEquatableValueTy(0)])
  expectUnique(&d[TestKeyTy(40), default: TestEquatableValueTy(0)])
}

func bumpValue(_ value: inout TestEquatableValueTy) {
  value = TestEquatableValueTy(value.value + 1)
}

func bumpValueAndThrow(_ value: inout TestEquatableValueTy) throws {
  value = TestEquatableValueTy(value.value + 1)
  throw TestError()
}

DictionaryTestSuite.test("COW.Slow.DefaultedSubscript.Insertion.modify") {
  var d = getCOWSlowEquatableDictionary()

  bumpValue(&d[TestKeyTy(40), default: TestEquatableValueTy(1040)])
  expectEqual(TestEquatableValueTy(1041), d[TestKeyTy(40)])

  // Note: Leak tests are done in tearDown.
}

DictionaryTestSuite.test("COW.Slow.DefaultedSubscript.Mutation.modify") {
  var d = getCOWSlowEquatableDictionary()

  bumpValue(&d[TestKeyTy(10), default: TestEquatableValueTy(2000)])
  expectEqual(TestEquatableValueTy(1011), d[TestKeyTy(10)])

  // Note: Leak tests are done in tearDown.
}

DictionaryTestSuite.test("COW.Slow.DefaultedSubscript.Insertion.modifyThrow") {
  var d = getCOWSlowEquatableDictionary()

  do {
    try bumpValueAndThrow(
      &d[TestKeyTy(40), default: TestEquatableValueTy(1040)])
    expectTrue(false, "Did not throw")
  } catch {
    expectTrue(error is TestError)
  }
  expectEqual(TestEquatableValueTy(1041), d[TestKeyTy(40)])

  // Note: Leak tests are done in tearDown.
}

DictionaryTestSuite.test("COW.Slow.DefaultedSubscript.Mutation.modifyThrow") {
  var d = getCOWSlowEquatableDictionary()

  do {
    try bumpValueAndThrow(
      &d[TestKeyTy(10), default: TestEquatableValueTy(2000)])
    expectTrue(false, "Did not throw")
  } catch {
    expectTrue(error is TestError)
  }
  expectEqual(TestEquatableValueTy(1011), d[TestKeyTy(10)])

  // Note: Leak tests are done in tearDown.
}


DictionaryTestSuite.test("COW.Fast.IndexForKeyDoesNotReallocate") {
  let d = getCOWFastDictionary()
  let identity1 = d._rawIdentifier()

  // Find an existing key.
  do {
    let foundIndex1 = d.index(forKey: 10)!
    assert(identity1 == d._rawIdentifier())

    let foundIndex2 = d.index(forKey: 10)!
    assert(foundIndex1 == foundIndex2)

    assert(d[foundIndex1].0 == 10)
    assert(d[foundIndex1].1 == 1010)
    assert(identity1 == d._rawIdentifier())
  }

  // Try to find a key that is not present.
  do {
    let foundIndex1 = d.index(forKey: 1111)
    assert(foundIndex1 == nil)
    assert(identity1 == d._rawIdentifier())
  }

  do {
    let d2: [MinimalHashableValue : OpaqueValue<Int>] = [:]
    MinimalHashableValue.timesEqualEqualWasCalled = 0
    MinimalHashableValue.timesHashIntoWasCalled = 0
    expectNil(d2.index(forKey: MinimalHashableValue(42)))

    // If the dictionary is empty, we shouldn't be computing the hash value of
    // the provided key.
    expectEqual(0, MinimalHashableValue.timesEqualEqualWasCalled)
    expectEqual(0, MinimalHashableValue.timesHashIntoWasCalled)
  }
}

DictionaryTestSuite.test("COW.Slow.IndexForKeyDoesNotReallocate") {
  let d = getCOWSlowDictionary()
  let identity1 = d._rawIdentifier()

  // Find an existing key.
  do {
    let foundIndex1 = d.index(forKey: TestKeyTy(10))!
    assert(identity1 == d._rawIdentifier())

    let foundIndex2 = d.index(forKey: TestKeyTy(10))!
    assert(foundIndex1 == foundIndex2)

    assert(d[foundIndex1].0 == TestKeyTy(10))
    assert(d[foundIndex1].1.value == 1010)
    assert(identity1 == d._rawIdentifier())
  }

  // Try to find a key that is not present.
  do {
    let foundIndex1 = d.index(forKey: TestKeyTy(1111))
    assert(foundIndex1 == nil)
    assert(identity1 == d._rawIdentifier())
  }

  do {
    let d2: [MinimalHashableClass : OpaqueValue<Int>] = [:]
    MinimalHashableClass.timesEqualEqualWasCalled = 0
    MinimalHashableClass.timesHashIntoWasCalled = 0
    expectNil(d2.index(forKey: MinimalHashableClass(42)))

    // If the dictionary is empty, we shouldn't be computing the hash value of
    // the provided key.
    expectEqual(0, MinimalHashableClass.timesEqualEqualWasCalled)
    expectEqual(0, MinimalHashableClass.timesHashIntoWasCalled)
  }
}


DictionaryTestSuite.test("COW.Fast.RemoveAtDoesNotReallocate")
  .code {
  do {
    var d = getCOWFastDictionary()
    let identity1 = d._rawIdentifier()

    let foundIndex1 = d.index(forKey: 10)!
    assert(identity1 == d._rawIdentifier())

    assert(d[foundIndex1].0 == 10)
    assert(d[foundIndex1].1 == 1010)

    let removed = d.remove(at: foundIndex1)
    assert(removed.0 == 10)
    assert(removed.1 == 1010)

    assert(identity1 == d._rawIdentifier())
    assert(d.index(forKey: 10) == nil)
  }

  do {
    let d1 = getCOWFastDictionary()
    let identity1 = d1._rawIdentifier()

    var d2 = d1
    assert(identity1 == d1._rawIdentifier())
    assert(identity1 == d2._rawIdentifier())

    let foundIndex1 = d2.index(forKey: 10)!
    assert(d2[foundIndex1].0 == 10)
    assert(d2[foundIndex1].1 == 1010)
    assert(identity1 == d1._rawIdentifier())
    assert(identity1 == d2._rawIdentifier())

    let removed = d2.remove(at: foundIndex1)
    assert(removed.0 == 10)
    assert(removed.1 == 1010)

    assert(identity1 == d1._rawIdentifier())
    assert(identity1 != d2._rawIdentifier())
    assert(d2.index(forKey: 10) == nil)
  }
}

DictionaryTestSuite.test("COW.Slow.RemoveAtDoesNotReallocate")
  .code {
  do {
    var d = getCOWSlowDictionary()
    let identity1 = d._rawIdentifier()

    let foundIndex1 = d.index(forKey: TestKeyTy(10))!
    assert(identity1 == d._rawIdentifier())

    assert(d[foundIndex1].0 == TestKeyTy(10))
    assert(d[foundIndex1].1.value == 1010)

    let removed = d.remove(at: foundIndex1)
    assert(removed.0 == TestKeyTy(10))
    assert(removed.1.value == 1010)

    assert(identity1 == d._rawIdentifier())
    assert(d.index(forKey: TestKeyTy(10)) == nil)
  }

  do {
    let d1 = getCOWSlowDictionary()
    let identity1 = d1._rawIdentifier()

    var d2 = d1
    assert(identity1 == d1._rawIdentifier())
    assert(identity1 == d2._rawIdentifier())

    let foundIndex1 = d2.index(forKey: TestKeyTy(10))!
    assert(d2[foundIndex1].0 == TestKeyTy(10))
    assert(d2[foundIndex1].1.value == 1010)

    let removed = d2.remove(at: foundIndex1)
    assert(removed.0 == TestKeyTy(10))
    assert(removed.1.value == 1010)

    assert(identity1 == d1._rawIdentifier())
    assert(identity1 != d2._rawIdentifier())
    assert(d2.index(forKey: TestKeyTy(10)) == nil)
  }
}


DictionaryTestSuite.test("COW.Fast.RemoveValueForKeyDoesNotReallocate")
  .code {
  do {
    var d1 = getCOWFastDictionary()
    let identity1 = d1._rawIdentifier()

    var deleted = d1.removeValue(forKey: 0)
    assert(deleted == nil)
    assert(identity1 == d1._rawIdentifier())

    deleted = d1.removeValue(forKey: 10)
    assert(deleted! == 1010)
    assert(identity1 == d1._rawIdentifier())

    // Keep variables alive.
    _fixLifetime(d1)
  }

  do {
    let d1 = getCOWFastDictionary()
    let identity1 = d1._rawIdentifier()

    var d2 = d1
    var deleted = d2.removeValue(forKey: 0)
    assert(deleted == nil)
    assert(identity1 == d1._rawIdentifier())
    assert(identity1 == d2._rawIdentifier())

    deleted = d2.removeValue(forKey: 10)
    assert(deleted! == 1010)
    assert(identity1 == d1._rawIdentifier())
    assert(identity1 != d2._rawIdentifier())

    // Keep variables alive.
    _fixLifetime(d1)
    _fixLifetime(d2)
  }
}

DictionaryTestSuite.test("COW.Slow.RemoveValueForKeyDoesNotReallocate")
  .code {
  do {
    var d1 = getCOWSlowDictionary()
    let identity1 = d1._rawIdentifier()

    var deleted = d1.removeValue(forKey: TestKeyTy(0))
    assert(deleted == nil)
    assert(identity1 == d1._rawIdentifier())

    deleted = d1.removeValue(forKey: TestKeyTy(10))
    assert(deleted!.value == 1010)
    assert(identity1 == d1._rawIdentifier())

    // Keep variables alive.
    _fixLifetime(d1)
  }

  do {
    let d1 = getCOWSlowDictionary()
    let identity1 = d1._rawIdentifier()

    var d2 = d1
    var deleted = d2.removeValue(forKey: TestKeyTy(0))
    assert(deleted == nil)
    assert(identity1 == d1._rawIdentifier())
    assert(identity1 == d2._rawIdentifier())

    deleted = d2.removeValue(forKey: TestKeyTy(10))
    assert(deleted!.value == 1010)
    assert(identity1 == d1._rawIdentifier())
    assert(identity1 != d2._rawIdentifier())

    // Keep variables alive.
    _fixLifetime(d1)
    _fixLifetime(d2)
  }
}


DictionaryTestSuite.test("COW.Fast.RemoveAllDoesNotReallocate") {
  do {
    var d = getCOWFastDictionary()
    let originalCapacity = d.capacity
    assert(d.count == 3)
    assert(d[10]! == 1010)

    d.removeAll()
    // We cannot assert that identity changed, since the new buffer of smaller
    // size can be allocated at the same address as the old one.
    let identity1 = d._rawIdentifier()
    assert(d.capacity < originalCapacity)
    assert(d.count == 0)
    assert(d[10] == nil)

    d.removeAll()
    assert(identity1 == d._rawIdentifier())
    assert(d.count == 0)
    assert(d[10] == nil)
  }

  do {
    var d = getCOWFastDictionary()
    let identity1 = d._rawIdentifier()
    let originalCapacity = d.capacity
    assert(d.count == 3)
    assert(d[10]! == 1010)

    d.removeAll(keepingCapacity: true)
    assert(identity1 == d._rawIdentifier())
    assert(d.capacity == originalCapacity)
    assert(d.count == 0)
    assert(d[10] == nil)

    d.removeAll(keepingCapacity: true)
    assert(identity1 == d._rawIdentifier())
    assert(d.capacity == originalCapacity)
    assert(d.count == 0)
    assert(d[10] == nil)
  }

  do {
    var d1 = getCOWFastDictionary()
    let identity1 = d1._rawIdentifier()
    assert(d1.count == 3)
    assert(d1[10]! == 1010)

    var d2 = d1
    d2.removeAll()
    let identity2 = d2._rawIdentifier()
    assert(identity1 == d1._rawIdentifier())
    assert(identity2 != identity1)
    assert(d1.count == 3)
    assert(d1[10]! == 1010)
    assert(d2.count == 0)
    assert(d2[10] == nil)

    // Keep variables alive.
    _fixLifetime(d1)
    _fixLifetime(d2)
  }

  do {
    var d1 = getCOWFastDictionary()
    let identity1 = d1._rawIdentifier()
    let originalCapacity = d1.capacity
    assert(d1.count == 3)
    assert(d1[10] == 1010)

    var d2 = d1
    d2.removeAll(keepingCapacity: true)
    let identity2 = d2._rawIdentifier()
    assert(identity1 == d1._rawIdentifier())
    assert(identity2 != identity1)
    assert(d1.count == 3)
    assert(d1[10]! == 1010)
    assert(d2.capacity == originalCapacity)
    assert(d2.count == 0)
    assert(d2[10] == nil)

    // Keep variables alive.
    _fixLifetime(d1)
    _fixLifetime(d2)
  }
}

DictionaryTestSuite.test("COW.Slow.RemoveAllDoesNotReallocate") {
  do {
    var d = getCOWSlowDictionary()
    let originalCapacity = d.capacity
    assert(d.count == 3)
    assert(d[TestKeyTy(10)]!.value == 1010)

    d.removeAll()
    // We cannot assert that identity changed, since the new buffer of smaller
    // size can be allocated at the same address as the old one.
    let identity1 = d._rawIdentifier()
    assert(d.capacity < originalCapacity)
    assert(d.count == 0)
    assert(d[TestKeyTy(10)] == nil)

    d.removeAll()
    assert(identity1 == d._rawIdentifier())
    assert(d.count == 0)
    assert(d[TestKeyTy(10)] == nil)
  }

  do {
    var d = getCOWSlowDictionary()
    let identity1 = d._rawIdentifier()
    let originalCapacity = d.capacity
    assert(d.count == 3)
    assert(d[TestKeyTy(10)]!.value == 1010)

    d.removeAll(keepingCapacity: true)
    assert(identity1 == d._rawIdentifier())
    assert(d.capacity == originalCapacity)
    assert(d.count == 0)
    assert(d[TestKeyTy(10)] == nil)

    d.removeAll(keepingCapacity: true)
    assert(identity1 == d._rawIdentifier())
    assert(d.capacity == originalCapacity)
    assert(d.count == 0)
    assert(d[TestKeyTy(10)] == nil)
  }

  do {
    var d1 = getCOWSlowDictionary()
    let identity1 = d1._rawIdentifier()
    assert(d1.count == 3)
    assert(d1[TestKeyTy(10)]!.value == 1010)

    var d2 = d1
    d2.removeAll()
    let identity2 = d2._rawIdentifier()
    assert(identity1 == d1._rawIdentifier())
    assert(identity2 != identity1)
    assert(d1.count == 3)
    assert(d1[TestKeyTy(10)]!.value == 1010)
    assert(d2.count == 0)
    assert(d2[TestKeyTy(10)] == nil)

    // Keep variables alive.
    _fixLifetime(d1)
    _fixLifetime(d2)
  }

  do {
    var d1 = getCOWSlowDictionary()
    let identity1 = d1._rawIdentifier()
    let originalCapacity = d1.capacity
    assert(d1.count == 3)
    assert(d1[TestKeyTy(10)]!.value == 1010)

    var d2 = d1
    d2.removeAll(keepingCapacity: true)
    let identity2 = d2._rawIdentifier()
    assert(identity1 == d1._rawIdentifier())
    assert(identity2 != identity1)
    assert(d1.count == 3)
    assert(d1[TestKeyTy(10)]!.value == 1010)
    assert(d2.capacity == originalCapacity)
    assert(d2.count == 0)
    assert(d2[TestKeyTy(10)] == nil)

    // Keep variables alive.
    _fixLifetime(d1)
    _fixLifetime(d2)
  }
}


DictionaryTestSuite.test("COW.Fast.CountDoesNotReallocate") {
  let d = getCOWFastDictionary()
  let identity1 = d._rawIdentifier()

  assert(d.count == 3)
  assert(identity1 == d._rawIdentifier())
}

DictionaryTestSuite.test("COW.Slow.CountDoesNotReallocate") {
  let d = getCOWSlowDictionary()
  let identity1 = d._rawIdentifier()

  assert(d.count == 3)
  assert(identity1 == d._rawIdentifier())
}


DictionaryTestSuite.test("COW.Fast.GenerateDoesNotReallocate") {
  let d = getCOWFastDictionary()
  let identity1 = d._rawIdentifier()

  var iter = d.makeIterator()
  var pairs = Array<(Int, Int)>()
  while let (key, value) = iter.next() {
    pairs += [(key, value)]
  }
  assert(equalsUnordered(pairs, [ (10, 1010), (20, 1020), (30, 1030) ]))
  assert(identity1 == d._rawIdentifier())
}

DictionaryTestSuite.test("COW.Slow.GenerateDoesNotReallocate") {
  let d = getCOWSlowDictionary()
  let identity1 = d._rawIdentifier()

  var iter = d.makeIterator()
  var pairs = Array<(Int, Int)>()
  while let (key, value) = iter.next() {
    pairs += [(key.value, value.value)]
  }
  assert(equalsUnordered(pairs, [ (10, 1010), (20, 1020), (30, 1030) ]))
  assert(identity1 == d._rawIdentifier())
}


DictionaryTestSuite.test("COW.Fast.EqualityTestDoesNotReallocate") {
  let d1 = getCOWFastDictionary()
  let identity1 = d1._rawIdentifier()

  var d2 = getCOWFastDictionary()
  let identity2 = d2._rawIdentifier()

  assert(d1 == d2)
  assert(identity1 == d1._rawIdentifier())
  assert(identity2 == d2._rawIdentifier())

  d2[40] = 2040
  assert(d1 != d2)
  assert(identity1 == d1._rawIdentifier())
  assert(identity2 == d2._rawIdentifier())
}

DictionaryTestSuite.test("COW.Slow.EqualityTestDoesNotReallocate") {
  let d1 = getCOWSlowEquatableDictionary()
  let identity1 = d1._rawIdentifier()

  var d2 = getCOWSlowEquatableDictionary()
  let identity2 = d2._rawIdentifier()

  assert(d1 == d2)
  assert(identity1 == d1._rawIdentifier())
  assert(identity2 == d2._rawIdentifier())

  d2[TestKeyTy(40)] = TestEquatableValueTy(2040)
  assert(d1 != d2)
  assert(identity1 == d1._rawIdentifier())
  assert(identity2 == d2._rawIdentifier())
}

//===---
// Keys and Values collection tests.
//===---

DictionaryTestSuite.test("COW.Fast.Values.AccessDoesNotReallocate") {
  var d1 = getCOWFastDictionary()
  let identity1 = d1._rawIdentifier()
  
  assert([1010, 1020, 1030] == d1.values.sorted())
  assert(identity1 == d1._rawIdentifier())
  
  var d2 = d1
  assert(identity1 == d2._rawIdentifier())
  
  let i = d2.index(forKey: 10)!
  assert(d1.values[i] == 1010)
  assert(d1[i] == (10, 1010))
  
  d2.values[i] += 1
  assert(d2.values[i] == 1011)
  assert(d2[10]! == 1011)
  assert(identity1 != d2._rawIdentifier())
  
  assert(d1[10]! == 1010)
  assert(identity1 == d1._rawIdentifier())
  
  checkCollection(
    Array(d1.values),
    d1.values,
    stackTrace: SourceLocStack())
  { $0 == $1 }
}

DictionaryTestSuite.test("COW.Slow.Values.Modify") {
  var d1 = getCOWSlowEquatableDictionary()
  var d2: [TestKeyTy: TestEquatableValueTy] = [
    TestKeyTy(40): TestEquatableValueTy(1040),
    TestKeyTy(50): TestEquatableValueTy(1050),
    TestKeyTy(60): TestEquatableValueTy(1060),
  ]
  d1.values = d2.values
  expectEqual(d1, d2)
  expectNil(d1[TestKeyTy(10)])
  expectNil(d1[TestKeyTy(20)])
  expectNil(d1[TestKeyTy(30)])
  expectEqual(TestEquatableValueTy(1040), d1[TestKeyTy(40)])
  expectEqual(TestEquatableValueTy(1050), d1[TestKeyTy(50)])
  expectEqual(TestEquatableValueTy(1060), d1[TestKeyTy(60)])
}

@inline(never)
func replaceValuesThenThrow<K: Hashable, V>(
  _ v: inout Dictionary<K, V>.Values,
  with v2: Dictionary<K, V>.Values
) throws {
  v = v2
  throw TestError()
}

DictionaryTestSuite.test("COW.Slow.Values.ModifyThrow") {
  var d1 = getCOWSlowEquatableDictionary()
  var d2: [TestKeyTy: TestEquatableValueTy] = [
    TestKeyTy(40): TestEquatableValueTy(1040),
    TestKeyTy(50): TestEquatableValueTy(1050),
    TestKeyTy(60): TestEquatableValueTy(1060),
  ]
  do {
    try replaceValuesThenThrow(&d1.values, with: d2.values)
    expectTrue(false, "Did not throw")
  } catch {
    expectTrue(error is TestError)
  }
  expectEqual(d1, d2)
  expectNil(d1[TestKeyTy(10)])
  expectNil(d1[TestKeyTy(20)])
  expectNil(d1[TestKeyTy(30)])
  expectEqual(TestEquatableValueTy(1040), d1[TestKeyTy(40)])
  expectEqual(TestEquatableValueTy(1050), d1[TestKeyTy(50)])
  expectEqual(TestEquatableValueTy(1060), d1[TestKeyTy(60)])
}

DictionaryTestSuite.test("COW.Slow.Values.Uniqueness") {
  var d = getCOWSlowEquatableDictionary()
  let i = d.index(forKey: TestKeyTy(20))!
  expectUnique(&d.values[i])
}

DictionaryTestSuite.test("COW.Fast.Keys.AccessDoesNotReallocate") {
  let d1 = getCOWFastDictionary()
  let identity1 = d1._rawIdentifier()
  
  assert([10, 20, 30] == d1.keys.sorted())

  let i = d1.index(forKey: 10)!
  assert(d1.keys[i] == 10)
  assert(d1[i] == (10, 1010))
  assert(identity1 == d1._rawIdentifier())

  checkCollection(
    Array(d1.keys),
    d1.keys,
    stackTrace: SourceLocStack())
  { $0 == $1 }
  
  do {
    var d2: [MinimalHashableValue : Int] = [
      MinimalHashableValue(10): 1010,
      MinimalHashableValue(20): 1020,
      MinimalHashableValue(30): 1030,
      MinimalHashableValue(40): 1040,
      MinimalHashableValue(50): 1050,
      MinimalHashableValue(60): 1060,
      MinimalHashableValue(70): 1070,
      MinimalHashableValue(80): 1080,
      MinimalHashableValue(90): 1090,
    ]
    // Make collisions less likely
    d2.reserveCapacity(1000)
    
    // Find the last key in the dictionary
    var lastKey: MinimalHashableValue = d2.first!.key
    for i in d2.indices { lastKey = d2[i].key }

    // firstIndex(where:) - linear search
    MinimalHashableValue.timesEqualEqualWasCalled = 0
    let j = d2.firstIndex(where: { (k, _) in k == lastKey })!
    expectGE(MinimalHashableValue.timesEqualEqualWasCalled, 8)

    // index(forKey:) - O(1) bucket + linear search
    MinimalHashableValue.timesEqualEqualWasCalled = 0
    let k = d2.index(forKey: lastKey)!
    expectLE(MinimalHashableValue.timesEqualEqualWasCalled, 4)
    
    // keys.firstIndex(of:) - O(1) bucket + linear search
    MinimalHashableValue.timesEqualEqualWasCalled = 0
    let l = d2.keys.firstIndex(of: lastKey)!
    expectLE(MinimalHashableValue.timesEqualEqualWasCalled, 4)

    expectEqual(j, k)
    expectEqual(k, l)
  }
}

DictionaryTestSuite.test("COW.Slow.SubscriptWithKeys.Insertion") {
  var d = getCOWSlowEquatableDictionary()

  d[TestKeyTy(40)] = TestEquatableValueTy(1040)
  expectEqual(TestEquatableValueTy(1040), d[TestKeyTy(40)])

  // Note: Leak tests are done in tearDown.
}

DictionaryTestSuite.test("COW.Slow.SubscriptWithKeys.Mutation") {
  var d = getCOWSlowEquatableDictionary()

  d[TestKeyTy(10)] = TestEquatableValueTy(2010)
  expectEqual(TestEquatableValueTy(2010), d[TestKeyTy(10)])

  // Note: Leak tests are done in tearDown.
}

DictionaryTestSuite.test("COW.Slow.SubscriptWithKeys.Removal") {
  var d = getCOWSlowEquatableDictionary()

  d[TestKeyTy(10)] = nil
  expectNil(d[TestKeyTy(10)])

  // Note: Leak tests are done in tearDown.
}

DictionaryTestSuite.test("COW.Slow.SubscriptWithKeys.Noop") {
  var d = getCOWSlowEquatableDictionary()

  d[TestKeyTy(40)] = nil
  expectNil(d[TestKeyTy(40)])

  // Note: Leak tests are done in tearDown.
}

extension Optional {
  @inline(never)
  mutating func setWrapped(to value: Wrapped) {
    self = .some(value)
  }

  @inline(never)
  mutating func setWrappedThenThrow(to value: Wrapped) throws {
    self = .some(value)
    throw TestError()
  }

  @inline(never)
  mutating func clear() {
    self = .none
  }

  @inline(never)
  mutating func clearThenThrow() throws {
    self = .none
    throw TestError()
  }
}

DictionaryTestSuite.test("COW.Slow.SubscriptWithKeys.Insertion.modify") {
  var d = getCOWSlowEquatableDictionary()

  d[TestKeyTy(40)].setWrapped(to: TestEquatableValueTy(1040))
  expectEqual(TestEquatableValueTy(1040), d[TestKeyTy(40)])

  // Note: Leak tests are done in tearDown.
}

DictionaryTestSuite.test("COW.Slow.SubscriptWithKeys.Mutation.modify") {
  var d = getCOWSlowEquatableDictionary()

  d[TestKeyTy(10)].setWrapped(to: TestEquatableValueTy(2010))
  expectEqual(TestEquatableValueTy(2010), d[TestKeyTy(10)])

  // Note: Leak tests are done in tearDown.
}

DictionaryTestSuite.test("COW.Slow.SubscriptWithKeys.Removal.modify") {
  var d = getCOWSlowEquatableDictionary()

  d[TestKeyTy(10)].clear()
  expectNil(d[TestKeyTy(10)])

  // Note: Leak tests are done in tearDown.
}

DictionaryTestSuite.test("COW.Slow.SubscriptWithKeys.Noop.modify") {
  var d = getCOWSlowEquatableDictionary()

  d[TestKeyTy(40)].clear()
  expectNil(d[TestKeyTy(40)])

  // Note: Leak tests are done in tearDown.
}

DictionaryTestSuite.test("COW.Slow.SubscriptWithKeys.Insertion.modifyThrow") {
  var d = getCOWSlowEquatableDictionary()

  do {
    try d[TestKeyTy(40)].setWrappedThenThrow(to: TestEquatableValueTy(1040))
    expectTrue(false, "Did not throw")
  } catch {
    expectTrue(error is TestError)
  }

  expectEqual(TestEquatableValueTy(1040), d[TestKeyTy(40)])

  // Note: Leak tests are done in tearDown.
}

DictionaryTestSuite.test("COW.Slow.SubscriptWithKeys.Mutation.modifyThrow") {
  var d = getCOWSlowEquatableDictionary()

  do {
    try d[TestKeyTy(10)].setWrappedThenThrow(to: TestEquatableValueTy(2010))
    expectTrue(false, "Did not throw")
  } catch {
    expectTrue(error is TestError)
  }

  expectEqual(TestEquatableValueTy(2010), d[TestKeyTy(10)])

  // Note: Leak tests are done in tearDown.
}

DictionaryTestSuite.test("COW.Slow.SubscriptWithKeys.Removal.modifyThrow") {
  var d = getCOWSlowEquatableDictionary()

  do {
    try d[TestKeyTy(10)].clearThenThrow()
    expectTrue(false, "Did not throw")
  } catch {
    expectTrue(error is TestError)
  }

  expectNil(d[TestKeyTy(10)])

  // Note: Leak tests are done in tearDown.
}

DictionaryTestSuite.test("COW.Slow.SubscriptWithKeys.Noop.modifyThrow") {
  var d = getCOWSlowEquatableDictionary()

  do {
    try d[TestKeyTy(40)].clearThenThrow()
    expectTrue(false, "Did not throw")
  } catch {
    expectTrue(error is TestError)
  }

  expectNil(d[TestKeyTy(40)])

  // Note: Leak tests are done in tearDown.
}


//===---
// Native dictionary tests.
//===---

func helperDeleteThree(
  _ k1: RawTestKeyTy,
  _ k2: RawTestKeyTy,
  _ k3: RawTestKeyTy
) {
  var d1 = Dictionary<RawTestKeyTy, TestValueTy>(minimumCapacity: 10)

  d1[k1] = TestValueTy(1010)
  d1[k2] = TestValueTy(1020)
  d1[k3] = TestValueTy(1030)

  assert(d1[k1]?.value == 1010)
  assert(d1[k2]?.value == 1020)
  assert(d1[k3]?.value == 1030)

  d1[k1] = nil
  assert(d1[k1]?.value == nil)
  assert(d1[k2]?.value == 1020)
  assert(d1[k3]?.value == 1030)

  d1[k2] = nil
  assert(d1[k1]?.value == nil)
  assert(d1[k2]?.value == nil)
  assert(d1[k3]?.value == 1030)

  d1[k3] = nil
  assert(d1[k1]?.value == nil)
  assert(d1[k2]?.value == nil)
  assert(d1[k3]?.value == nil)
  assert(d1.count == 0)
}

DictionaryTestSuite.test("deleteChainCollision") {
  let k1 = RawTestKeyTy(value: 10, hashValue: 0)
  let k2 = RawTestKeyTy(value: 20, hashValue: 0)
  let k3 = RawTestKeyTy(value: 30, hashValue: 0)

  helperDeleteThree(k1, k2, k3)
}

DictionaryTestSuite.test("deleteChainNoCollision") {
  let k1 = RawTestKeyTy(value: 10, hashValue: 0)
  let k2 = RawTestKeyTy(value: 20, hashValue: 1)
  let k3 = RawTestKeyTy(value: 30, hashValue: 2)

  helperDeleteThree(k1, k2, k3)
}

DictionaryTestSuite.test("deleteChainCollision2") {
  let k1_0 = RawTestKeyTy(value: 10, hashValue: 0)
  let k2_0 = RawTestKeyTy(value: 20, hashValue: 0)
  let k3_2 = RawTestKeyTy(value: 30, hashValue: 2)
  let k4_0 = RawTestKeyTy(value: 40, hashValue: 0)
  let k5_2 = RawTestKeyTy(value: 50, hashValue: 2)
  let k6_0 = RawTestKeyTy(value: 60, hashValue: 0)

  var d = Dictionary<RawTestKeyTy, TestValueTy>(minimumCapacity: 10)

  d[k1_0] = TestValueTy(1010) // in bucket 0
  d[k2_0] = TestValueTy(1020) // in bucket 1
  d[k3_2] = TestValueTy(1030) // in bucket 2
  d[k4_0] = TestValueTy(1040) // in bucket 3
  d[k5_2] = TestValueTy(1050) // in bucket 4
  d[k6_0] = TestValueTy(1060) // in bucket 5

  d[k3_2] = nil

  assert(d[k1_0]!.value == 1010)
  assert(d[k2_0]!.value == 1020)
  assert(d[k3_2] == nil)
  assert(d[k4_0]!.value == 1040)
  assert(d[k5_2]!.value == 1050)
  assert(d[k6_0]!.value == 1060)
}

DictionaryTestSuite.test("deleteChainCollisionRandomized") {
  let seed = UInt64.random(in: .min ... .max)
  var generator = LinearCongruentialGenerator(seed: seed)
  print("using LinearCongruentialGenerator(seed: \(seed))")

  func check(_ d: Dictionary<RawTestKeyTy, TestValueTy>) {
    let keys = Array(d.keys)
    for i in 0..<keys.count {
      for j in 0..<i {
        expectNotEqual(keys[i], keys[j])
      }
    }

    for k in keys {
      expectNotNil(d[k])
    }
  }

  let collisionChains = Int.random(in: 1...8, using: &generator)
  let chainOverlap = Int.random(in: 0...5, using: &generator)
  let chainLength = 7

  var knownKeys: [RawTestKeyTy] = []
  func getKey(_ value: Int) -> RawTestKeyTy {
    for k in knownKeys {
      if k.value == value {
        return k
      }
    }
    let hashValue = Int.random(in: 0 ..< (chainLength - chainOverlap), using: &generator) * collisionChains
    let k = RawTestKeyTy(value: value, hashValue: hashValue)
    knownKeys += [k]
    return k
  }

  var d = Dictionary<RawTestKeyTy, TestValueTy>(minimumCapacity: 30)
  for _ in 1..<300 {
    let key = getKey(Int.random(in: 0 ..< (collisionChains * chainLength), using: &generator))
    if Int.random(in: 0 ..< (chainLength * 2), using: &generator) == 0 {
      d[key] = nil
    } else {
      d[key] = TestValueTy(key.value * 10)
    }
    check(d)
  }
}

DictionaryTestSuite.test("init(dictionaryLiteral:)") {
  do {
    var empty = Dictionary<Int, Int>()
    assert(empty.count == 0)
    assert(empty[1111] == nil)
  }
  do {
    var d = Dictionary(dictionaryLiteral: (10, 1010))
    assert(d.count == 1)
    assert(d[10]! == 1010)
    assert(d[1111] == nil)
  }
  do {
    var d = Dictionary(dictionaryLiteral: 
        (10, 1010), (20, 1020))
    assert(d.count == 2)
    assert(d[10]! == 1010)
    assert(d[20]! == 1020)
    assert(d[1111] == nil)
  }
  do {
    var d = Dictionary(dictionaryLiteral: 
        (10, 1010), (20, 1020), (30, 1030))
    assert(d.count == 3)
    assert(d[10]! == 1010)
    assert(d[20]! == 1020)
    assert(d[30]! == 1030)
    assert(d[1111] == nil)
  }
  do {
    var d = Dictionary(dictionaryLiteral: 
        (10, 1010), (20, 1020), (30, 1030), (40, 1040))
    assert(d.count == 4)
    assert(d[10]! == 1010)
    assert(d[20]! == 1020)
    assert(d[30]! == 1030)
    assert(d[40]! == 1040)
    assert(d[1111] == nil)
  }
  do {
    var d: Dictionary<Int, Int> = [ 10: 1010, 20: 1020, 30: 1030 ]
    assert(d.count == 3)
    assert(d[10]! == 1010)
    assert(d[20]! == 1020)
    assert(d[30]! == 1030)
  }
}

DictionaryTestSuite.test("init(uniqueKeysWithValues:)") {
  do {
    var d = Dictionary(uniqueKeysWithValues: [(10, 1010), (20, 1020), (30, 1030)])
    expectEqual(d.count, 3)
    expectEqual(d[10]!, 1010)
    expectEqual(d[20]!, 1020)
    expectEqual(d[30]!, 1030)
    expectNil(d[1111])
  }
  do {
    var d = Dictionary<Int, Int>(uniqueKeysWithValues: EmptyCollection<(Int, Int)>())
    expectEqual(d.count, 0)
    expectNil(d[1111])
  }
  do {
    expectCrashLater()
    _ = Dictionary(uniqueKeysWithValues: [(10, 1010), (20, 1020), (10, 2010)])
  }
}

DictionaryTestSuite.test("init(_:uniquingKeysWith:)") {
  do {
    let d = Dictionary(
      [(10, 1010), (20, 1020), (30, 1030), (10, 2010)], uniquingKeysWith: min)
    expectEqual(d.count, 3)
    expectEqual(d[10]!, 1010)
    expectEqual(d[20]!, 1020)
    expectEqual(d[30]!, 1030)
    expectNil(d[1111])
  }
  do {
    let d = Dictionary(
      [(10, 1010), (20, 1020), (30, 1030), (10, 2010)] as [(Int, Int)],
      uniquingKeysWith: +)
    expectEqual(d.count, 3)
    expectEqual(d[10]!, 3020)
    expectEqual(d[20]!, 1020)
    expectEqual(d[30]!, 1030)
    expectNil(d[1111])
  }
  do {
    let d = Dictionary([(10, 1010), (20, 1020), (30, 1030), (10, 2010)]) {
      (a, b) in Int("\(a)\(b)")!
    }
    expectEqual(d.count, 3)
    expectEqual(d[10]!, 10102010)
    expectEqual(d[20]!, 1020)
    expectEqual(d[30]!, 1030)
    expectNil(d[1111])
  }
  do {
    let d = Dictionary([(10, 1010), (10, 2010), (10, 3010), (10, 4010)]) { $1 }
    expectEqual(d.count, 1)
    expectEqual(d[10]!, 4010)
    expectNil(d[1111])
  }
  do {
    let d = Dictionary(EmptyCollection<(Int, Int)>(), uniquingKeysWith: min)
    expectEqual(d.count, 0)
    expectNil(d[1111])
  }

  struct TE: Error {}
  do {
    // No duplicate keys, so no error thrown.
    let d1 = try Dictionary([(10, 1), (20, 2), (30, 3)]) { (_,_) in throw TE() }
    expectEqual(d1.count, 3)
    // Duplicate keys, should throw error.
    _ = try Dictionary([(10, 1), (10, 2)]) { (_,_) in throw TE() }
    _ = assertionFailure()
  } catch {
    assert(error is TE)
  }
}

DictionaryTestSuite.test("init(grouping:by:)") {
  let r = 0..<10

  let d1 = Dictionary(grouping: r, by: { $0 % 3 })
  expectEqual(3, d1.count)
  expectEqual([0, 3, 6, 9], d1[0]!)
  expectEqual([1, 4, 7], d1[1]!)
  expectEqual([2, 5, 8], d1[2]!)

  let d2 = Dictionary(grouping: r, by: { $0 })
  expectEqual(10, d2.count)

  let d3 = Dictionary(grouping: 0..<0, by: { $0 })
  expectEqual(0, d3.count)
}

DictionaryTestSuite.test("mapValues(_:)") {
  let d1 = [10: 1010, 20: 1020, 30: 1030]
  let d2 = d1.mapValues(String.init)

  expectEqual(d1.count, d2.count)
  expectEqual(d1.keys.first, d2.keys.first)

  for (key, _) in d1 {
    expectEqual(String(d1[key]!), d2[key]!)
  }

  do {
    let d3: [MinimalHashableValue : Int] = Dictionary(
      uniqueKeysWithValues: d1.lazy.map { (MinimalHashableValue($0), $1) })
    expectEqual(d3.count, 3)
    MinimalHashableValue.timesEqualEqualWasCalled = 0
    MinimalHashableValue.timesHashIntoWasCalled = 0

    // Calling mapValues shouldn't ever recalculate any hashes.
    let d4 = d3.mapValues(String.init)
    expectEqual(d4.count, d3.count)
    expectEqual(0, MinimalHashableValue.timesEqualEqualWasCalled)
    expectEqual(0, MinimalHashableValue.timesHashIntoWasCalled)
  }
}

DictionaryTestSuite.test("filter(_:)") {
  let d1 = [1: 1, 2: 2, 3: 100, 4: 4, 5: 100, 6: 6]
  let d2 = d1.filter() {key, value in key == value}

  expectEqual(d2.count, 4)
  for (key, value) in d2 {
    expectEqual(key, value)
  }

  expectNil(d2[3])
  expectNil(d2[5])
}

DictionaryTestSuite.test("capacity/init(minimumCapacity:)") {
  let d0 = Dictionary<String, Int>(minimumCapacity: 0)
  expectGE(d0.capacity, 0)

  let d1 = Dictionary<String, Int>(minimumCapacity: 1)
  expectGE(d1.capacity, 1)

  let d3 = Dictionary<String, Int>(minimumCapacity: 3)
  expectGE(d3.capacity, 3)

  let d4 = Dictionary<String, Int>(minimumCapacity: 4)
  expectGE(d4.capacity, 4)

  let d10 = Dictionary<String, Int>(minimumCapacity: 10)
  expectGE(d10.capacity, 10)

  let d100 = Dictionary<String, Int>(minimumCapacity: 100)
  expectGE(d100.capacity, 100)

  let d1024 = Dictionary<String, Int>(minimumCapacity: 1024)
  expectGE(d1024.capacity, 1024)
}

DictionaryTestSuite.test("capacity/reserveCapacity(_:)") {
  var d1 = [10: 1010, 20: 1020, 30: 1030]
  expectEqual(3, d1.capacity)
  d1[40] = 1040
  expectEqual(6, d1.capacity)

  // Reserving new capacity jumps up to next limit.
  d1.reserveCapacity(7)
  expectEqual(12, d1.capacity)

  // Can reserve right up to a limit.
  d1.reserveCapacity(24)
  expectEqual(24, d1.capacity)

  // Fill up to the limit, no reallocation.
  d1.merge(stride(from: 50, through: 240, by: 10).lazy.map { ($0, 1000 + $0) },
    uniquingKeysWith: { (_,_) in fatalError() })
  expectEqual(24, d1.count)
  expectEqual(24, d1.capacity)
  d1[250] = 1250
  expectEqual(48, d1.capacity)
}

#if _runtime(_ObjC)
//===---
// NSDictionary -> Dictionary bridging tests.
//===---

func getAsNSDictionary(_ d: Dictionary<Int, Int>) -> NSDictionary {
  let keys = Array(d.keys.map { TestObjCKeyTy($0) })
  let values = Array(d.values.map { TestObjCValueTy($0) })

  // Return an `NSMutableDictionary` to make sure that it has a unique
  // pointer identity.
  return NSMutableDictionary(objects: values, forKeys: keys)
}

func getAsEquatableNSDictionary(_ d: Dictionary<Int, Int>) -> NSDictionary {
  let keys = Array(d.keys.map { TestObjCKeyTy($0) })
  let values = Array(d.values.map { TestObjCEquatableValueTy($0) })

  // Return an `NSMutableDictionary` to make sure that it has a unique
  // pointer identity.
  return NSMutableDictionary(objects: values, forKeys: keys)
}

func getAsNSMutableDictionary(_ d: Dictionary<Int, Int>) -> NSMutableDictionary {
  let keys = Array(d.keys.map { TestObjCKeyTy($0) })
  let values = Array(d.values.map { TestObjCValueTy($0) })

  return NSMutableDictionary(objects: values, forKeys: keys)
}

func getBridgedVerbatimDictionary() -> Dictionary<NSObject, AnyObject> {
  let nsd = getAsNSDictionary([10: 1010, 20: 1020, 30: 1030])
  return convertNSDictionaryToDictionary(nsd)
}

func getBridgedVerbatimDictionary(_ d: Dictionary<Int, Int>) -> Dictionary<NSObject, AnyObject> {
  let nsd = getAsNSDictionary(d)
  return convertNSDictionaryToDictionary(nsd)
}

func getBridgedVerbatimDictionaryAndNSMutableDictionary()
    -> (Dictionary<NSObject, AnyObject>, NSMutableDictionary) {
  let nsd = getAsNSMutableDictionary([10: 1010, 20: 1020, 30: 1030])
  return (convertNSDictionaryToDictionary(nsd), nsd)
}

func getBridgedNonverbatimDictionary() -> Dictionary<TestBridgedKeyTy, TestBridgedValueTy> {
  let nsd = getAsNSDictionary([10: 1010, 20: 1020, 30: 1030 ])
  return Swift._forceBridgeFromObjectiveC(nsd, Dictionary.self)
}

func getBridgedNonverbatimDictionary(_ d: Dictionary<Int, Int>) -> Dictionary<TestBridgedKeyTy, TestBridgedValueTy> {
  let nsd = getAsNSDictionary(d)
  return Swift._forceBridgeFromObjectiveC(nsd, Dictionary.self)
}

func getBridgedNonverbatimDictionaryAndNSMutableDictionary()
    -> (Dictionary<TestBridgedKeyTy, TestBridgedValueTy>, NSMutableDictionary) {
  let nsd = getAsNSMutableDictionary([10: 1010, 20: 1020, 30: 1030])
  return (Swift._forceBridgeFromObjectiveC(nsd, Dictionary.self), nsd)
}

func getBridgedVerbatimEquatableDictionary(_ d: Dictionary<Int, Int>) -> Dictionary<NSObject, TestObjCEquatableValueTy> {
  let nsd = getAsEquatableNSDictionary(d)
  return convertNSDictionaryToDictionary(nsd)
}

func getBridgedNonverbatimEquatableDictionary(_ d: Dictionary<Int, Int>) -> Dictionary<TestBridgedKeyTy, TestBridgedEquatableValueTy> {
  let nsd = getAsEquatableNSDictionary(d)
  return Swift._forceBridgeFromObjectiveC(nsd, Dictionary.self)
}

func getHugeBridgedVerbatimDictionaryHelper() -> NSDictionary {
  let keys = (1...32).map { TestObjCKeyTy($0) }
  let values = (1...32).map { TestObjCValueTy(1000 + $0) }

  return NSMutableDictionary(objects: values, forKeys: keys)
}

func getHugeBridgedVerbatimDictionary() -> Dictionary<NSObject, AnyObject> {
  let nsd = getHugeBridgedVerbatimDictionaryHelper()
  return convertNSDictionaryToDictionary(nsd)
}

func getHugeBridgedNonverbatimDictionary() -> Dictionary<TestBridgedKeyTy, TestBridgedValueTy> {
  let nsd = getHugeBridgedVerbatimDictionaryHelper()
  return Swift._forceBridgeFromObjectiveC(nsd, Dictionary.self)
}

/// A mock dictionary that stores its keys and values in parallel arrays, which
/// allows it to return inner pointers to the keys array in fast enumeration.
@objc
class ParallelArrayDictionary : NSDictionary {
  struct Keys {
    var key0: AnyObject = TestObjCKeyTy(10)
    var key1: AnyObject = TestObjCKeyTy(20)
    var key2: AnyObject = TestObjCKeyTy(30)
    var key3: AnyObject = TestObjCKeyTy(40)
  }
  var keys = [ Keys() ]
  var value: AnyObject = TestObjCValueTy(1111)

  override init() {
    super.init()
  }

  override init(
    objects: UnsafePointer<AnyObject>?,
    forKeys keys: UnsafePointer<NSCopying>?,
    count: Int) {
    super.init(objects: objects, forKeys: keys, count: count)
  }

  required init(coder aDecoder: NSCoder) {
    fatalError("init(coder:) not implemented by ParallelArrayDictionary")
  }

  @objc(copyWithZone:)
  override func copy(with zone: NSZone?) -> Any {
    // Ensure that copying this dictionary does not produce a CoreFoundation
    // object.
    return self
  }

  override func countByEnumerating(
    with state: UnsafeMutablePointer<NSFastEnumerationState>,
    objects: AutoreleasingUnsafeMutablePointer<AnyObject?>,
    count: Int
  ) -> Int {
    var theState = state.pointee
    if theState.state == 0 {
      theState.state = 1
      theState.itemsPtr = AutoreleasingUnsafeMutablePointer(keys._baseAddressIfContiguous)
      theState.mutationsPtr = _fastEnumerationStorageMutationsPtr
      state.pointee = theState
      return 4
    }
    return 0
  }

  override func object(forKey aKey: Any) -> Any? {
    return value
  }

  override var count: Int {
    return 4
  }
}

func getParallelArrayBridgedVerbatimDictionary() -> Dictionary<NSObject, AnyObject> {
  let nsd: NSDictionary = ParallelArrayDictionary()
  return convertNSDictionaryToDictionary(nsd)
}

func getParallelArrayBridgedNonverbatimDictionary() -> Dictionary<TestBridgedKeyTy, TestBridgedValueTy> {
  let nsd: NSDictionary = ParallelArrayDictionary()
  return Swift._forceBridgeFromObjectiveC(nsd, Dictionary.self)
}

@objc
class CustomImmutableNSDictionary : NSDictionary {
  init(_privateInit: ()) {
    super.init()
  }

  override init() {
    expectUnreachable()
    super.init()
  }

  override init(
    objects: UnsafePointer<AnyObject>?,
    forKeys keys: UnsafePointer<NSCopying>?,
    count: Int) {
    expectUnreachable()
    super.init(objects: objects, forKeys: keys, count: count)
  }

  required init(coder aDecoder: NSCoder) {
    fatalError("init(coder:) not implemented by CustomImmutableNSDictionary")
  }

  @objc(copyWithZone:)
  override func copy(with zone: NSZone?) -> Any {
    CustomImmutableNSDictionary.timesCopyWithZoneWasCalled += 1
    return self
  }

  override func object(forKey aKey: Any) -> Any? {
    CustomImmutableNSDictionary.timesObjectForKeyWasCalled += 1
    return getAsNSDictionary([10: 1010, 20: 1020, 30: 1030]).object(forKey: aKey)
  }

  override func keyEnumerator() -> NSEnumerator {
    CustomImmutableNSDictionary.timesKeyEnumeratorWasCalled += 1
    return getAsNSDictionary([10: 1010, 20: 1020, 30: 1030]).keyEnumerator()
  }

  override var count: Int {
    CustomImmutableNSDictionary.timesCountWasCalled += 1
    return 3
  }

  static var timesCopyWithZoneWasCalled = 0
  static var timesObjectForKeyWasCalled = 0
  static var timesKeyEnumeratorWasCalled = 0
  static var timesCountWasCalled = 0
}

DictionaryTestSuite.test("BridgedFromObjC.Verbatim.DictionaryIsCopied") {
  let (d, nsd) = getBridgedVerbatimDictionaryAndNSMutableDictionary()
  assert(isCocoaDictionary(d))

  // Find an existing key.
  do {
    let kv = d[d.index(forKey: TestObjCKeyTy(10))!]
    assert(kv.0 == TestObjCKeyTy(10))
    assert((kv.1 as! TestObjCValueTy).value == 1010)
  }

  // Delete the key from the NSMutableDictionary.
  assert(nsd[TestObjCKeyTy(10)] != nil)
  nsd.removeObject(forKey: TestObjCKeyTy(10))
  assert(nsd[TestObjCKeyTy(10)] == nil)

  // Find an existing key, again.
  do {
    let kv = d[d.index(forKey: TestObjCKeyTy(10))!]
    assert(kv.0 == TestObjCKeyTy(10))
    assert((kv.1 as! TestObjCValueTy).value == 1010)
  }
}

DictionaryTestSuite.test("BridgedFromObjC.Nonverbatim.DictionaryIsCopied") {
  let (d, nsd) = getBridgedNonverbatimDictionaryAndNSMutableDictionary()
  assert(isNativeDictionary(d))

  // Find an existing key.
  do {
    let kv = d[d.index(forKey: TestBridgedKeyTy(10))!]
    assert(kv.0 == TestBridgedKeyTy(10))
    assert(kv.1.value == 1010)
  }

  // Delete the key from the NSMutableDictionary.
  assert(nsd[TestBridgedKeyTy(10) as NSCopying] != nil)
  nsd.removeObject(forKey: TestBridgedKeyTy(10) as NSCopying)
  assert(nsd[TestBridgedKeyTy(10) as NSCopying] == nil)

  // Find an existing key, again.
  do {
    let kv = d[d.index(forKey: TestBridgedKeyTy(10))!]
    assert(kv.0 == TestBridgedKeyTy(10))
    assert(kv.1.value == 1010)
  }
}


DictionaryTestSuite.test("BridgedFromObjC.Verbatim.NSDictionaryIsRetained") {
  let nsd: NSDictionary =
    NSDictionary(dictionary:
      getAsNSDictionary([10: 1010, 20: 1020, 30: 1030]))

  let d: [NSObject : AnyObject] = convertNSDictionaryToDictionary(nsd)

  let bridgedBack: NSDictionary = convertDictionaryToNSDictionary(d)

  expectEqual(
    unsafeBitCast(nsd, to: Int.self),
    unsafeBitCast(bridgedBack, to: Int.self))

  _fixLifetime(nsd)
  _fixLifetime(d)
  _fixLifetime(bridgedBack)
}

DictionaryTestSuite.test("BridgedFromObjC.Nonverbatim.NSDictionaryIsCopied") {
  let nsd: NSDictionary =
    NSDictionary(dictionary:
      getAsNSDictionary([10: 1010, 20: 1020, 30: 1030]))

  let d: [TestBridgedKeyTy : TestBridgedValueTy] =
    convertNSDictionaryToDictionary(nsd)

  let bridgedBack: NSDictionary = convertDictionaryToNSDictionary(d)

  expectNotEqual(
    unsafeBitCast(nsd, to: Int.self),
    unsafeBitCast(bridgedBack, to: Int.self))

  _fixLifetime(nsd)
  _fixLifetime(d)
  _fixLifetime(bridgedBack)
}


DictionaryTestSuite.test("BridgedFromObjC.Verbatim.ImmutableDictionaryIsRetained") {
  let nsd: NSDictionary = CustomImmutableNSDictionary(_privateInit: ())

  CustomImmutableNSDictionary.timesCopyWithZoneWasCalled = 0
  CustomImmutableNSDictionary.timesObjectForKeyWasCalled = 0
  CustomImmutableNSDictionary.timesKeyEnumeratorWasCalled = 0
  CustomImmutableNSDictionary.timesCountWasCalled = 0
  let d: [NSObject : AnyObject] = convertNSDictionaryToDictionary(nsd)
  expectEqual(1, CustomImmutableNSDictionary.timesCopyWithZoneWasCalled)
  expectEqual(0, CustomImmutableNSDictionary.timesObjectForKeyWasCalled)
  expectEqual(0, CustomImmutableNSDictionary.timesKeyEnumeratorWasCalled)
  expectEqual(0, CustomImmutableNSDictionary.timesCountWasCalled)

  let bridgedBack: NSDictionary = convertDictionaryToNSDictionary(d)
  expectEqual(
    unsafeBitCast(nsd, to: Int.self),
    unsafeBitCast(bridgedBack, to: Int.self))

  _fixLifetime(nsd)
  _fixLifetime(d)
  _fixLifetime(bridgedBack)
}

DictionaryTestSuite.test("BridgedFromObjC.Nonverbatim.ImmutableDictionaryIsCopied") {
  //some bridged NSDictionary operations on non-standard NSDictionary subclasses
  //autorelease keys and values. Make sure the leak checker isn't confused
  autoreleasepool {
  let nsd: NSDictionary = CustomImmutableNSDictionary(_privateInit: ())

  CustomImmutableNSDictionary.timesCopyWithZoneWasCalled = 0
  CustomImmutableNSDictionary.timesObjectForKeyWasCalled = 0
  CustomImmutableNSDictionary.timesKeyEnumeratorWasCalled = 0
  CustomImmutableNSDictionary.timesCountWasCalled = 0
  TestBridgedValueTy.bridgeOperations = 0
  let d: [TestBridgedKeyTy : TestBridgedValueTy] =
    convertNSDictionaryToDictionary(nsd)
  expectEqual(0, CustomImmutableNSDictionary.timesCopyWithZoneWasCalled)
  expectEqual(3, CustomImmutableNSDictionary.timesObjectForKeyWasCalled)
  expectEqual(1, CustomImmutableNSDictionary.timesKeyEnumeratorWasCalled)
  expectNotEqual(0, CustomImmutableNSDictionary.timesCountWasCalled)
  expectEqual(3, TestBridgedValueTy.bridgeOperations)

  let bridgedBack: NSDictionary = convertDictionaryToNSDictionary(d)
  expectNotEqual(
    unsafeBitCast(nsd, to: Int.self),
    unsafeBitCast(bridgedBack, to: Int.self))

  _fixLifetime(nsd)
  _fixLifetime(d)
  _fixLifetime(bridgedBack)
  }
}


DictionaryTestSuite.test("BridgedFromObjC.Verbatim.IndexForKey") {
  let d = getBridgedVerbatimDictionary()
  let identity1 = d._rawIdentifier()
  assert(isCocoaDictionary(d))

  // Find an existing key.
  do {
    var kv = d[d.index(forKey: TestObjCKeyTy(10))!]
    assert(kv.0 == TestObjCKeyTy(10))
    assert((kv.1 as! TestObjCValueTy).value == 1010)

    kv = d[d.index(forKey: TestObjCKeyTy(20))!]
    assert(kv.0 == TestObjCKeyTy(20))
    assert((kv.1 as! TestObjCValueTy).value == 1020)

    kv = d[d.index(forKey: TestObjCKeyTy(30))!]
    assert(kv.0 == TestObjCKeyTy(30))
    assert((kv.1 as! TestObjCValueTy).value == 1030)
  }

  // Try to find a key that does not exist.
  assert(d.index(forKey: TestObjCKeyTy(40)) == nil)
  assert(identity1 == d._rawIdentifier())
}

DictionaryTestSuite.test("BridgedFromObjC.Nonverbatim.IndexForKey") {
  let d = getBridgedNonverbatimDictionary()
  let identity1 = d._rawIdentifier()
  assert(isNativeDictionary(d))

  // Find an existing key.
  do {
    var kv = d[d.index(forKey: TestBridgedKeyTy(10))!]
    assert(kv.0 == TestBridgedKeyTy(10))
    assert(kv.1.value == 1010)

    kv = d[d.index(forKey: TestBridgedKeyTy(20))!]
    assert(kv.0 == TestBridgedKeyTy(20))
    assert(kv.1.value == 1020)

    kv = d[d.index(forKey: TestBridgedKeyTy(30))!]
    assert(kv.0 == TestBridgedKeyTy(30))
    assert(kv.1.value == 1030)
  }

  // Try to find a key that does not exist.
  assert(d.index(forKey: TestBridgedKeyTy(40)) == nil)
  assert(identity1 == d._rawIdentifier())
}

DictionaryTestSuite.test("BridgedFromObjC.Verbatim.SubscriptWithIndex") {
  let d = getBridgedVerbatimDictionary()
  let identity1 = d._rawIdentifier()
  assert(isCocoaDictionary(d))

  let startIndex = d.startIndex
  let endIndex = d.endIndex
  assert(startIndex != endIndex)
  assert(startIndex < endIndex)
  assert(startIndex <= endIndex)
  assert(!(startIndex >= endIndex))
  assert(!(startIndex > endIndex))
  assert(identity1 == d._rawIdentifier())

  var pairs = Array<(Int, Int)>()
  for i in d.indices {
    let (key, value) = d[i]
    let kv = ((key as! TestObjCKeyTy).value, (value as! TestObjCValueTy).value)
    pairs += [kv]
  }
  assert(equalsUnordered(pairs, [ (10, 1010), (20, 1020), (30, 1030) ]))
  assert(identity1 == d._rawIdentifier())

  // Keep indexes alive during the calls above.
  _fixLifetime(startIndex)
  _fixLifetime(endIndex)
}

DictionaryTestSuite.test("BridgedFromObjC.Nonverbatim.SubscriptWithIndex") {
  let d = getBridgedNonverbatimDictionary()
  let identity1 = d._rawIdentifier()
  assert(isNativeDictionary(d))

  let startIndex = d.startIndex
  let endIndex = d.endIndex
  assert(startIndex != endIndex)
  assert(startIndex < endIndex)
  assert(startIndex <= endIndex)
  assert(!(startIndex >= endIndex))
  assert(!(startIndex > endIndex))
  assert(identity1 == d._rawIdentifier())

  var pairs = Array<(Int, Int)>()
  for i in d.indices {
    let (key, value) = d[i]
    let kv = (key.value, value.value)
    pairs += [kv]
  }
  assert(equalsUnordered(pairs, [ (10, 1010), (20, 1020), (30, 1030) ]))
  assert(identity1 == d._rawIdentifier())

  // Keep indexes alive during the calls above.
  _fixLifetime(startIndex)
  _fixLifetime(endIndex)
}

DictionaryTestSuite.test("BridgedFromObjC.Verbatim.SubscriptWithIndex_Empty") {
  let d = getBridgedVerbatimDictionary([:])
  let identity1 = d._rawIdentifier()
  assert(isCocoaDictionary(d))

  let startIndex = d.startIndex
  let endIndex = d.endIndex
  assert(startIndex == endIndex)
  assert(!(startIndex < endIndex))
  assert(startIndex <= endIndex)
  assert(startIndex >= endIndex)
  assert(!(startIndex > endIndex))
  assert(identity1 == d._rawIdentifier())

  // Keep indexes alive during the calls above.
  _fixLifetime(startIndex)
  _fixLifetime(endIndex)
}

DictionaryTestSuite.test("BridgedFromObjC.Nonverbatim.SubscriptWithIndex_Empty") {
  let d = getBridgedNonverbatimDictionary([:])
  let identity1 = d._rawIdentifier()
  assert(isNativeDictionary(d))

  let startIndex = d.startIndex
  let endIndex = d.endIndex
  assert(startIndex == endIndex)
  assert(!(startIndex < endIndex))
  assert(startIndex <= endIndex)
  assert(startIndex >= endIndex)
  assert(!(startIndex > endIndex))
  assert(identity1 == d._rawIdentifier())

  // Keep indexes alive during the calls above.
  _fixLifetime(startIndex)
  _fixLifetime(endIndex)
}

DictionaryTestSuite.test("BridgedFromObjC.Verbatim.SubscriptWithKey") {
  var d = getBridgedVerbatimDictionary()
  let identity1 = d._rawIdentifier()
  assert(isCocoaDictionary(d))

  // Read existing key-value pairs.
  var v = d[TestObjCKeyTy(10)] as! TestObjCValueTy
  assert(v.value == 1010)

  v = d[TestObjCKeyTy(20)] as! TestObjCValueTy
  assert(v.value == 1020)

  v = d[TestObjCKeyTy(30)] as! TestObjCValueTy
  assert(v.value == 1030)

  assert(identity1 == d._rawIdentifier())

  // Insert a new key-value pair.
  d[TestObjCKeyTy(40)] = TestObjCValueTy(2040)
  let identity2 = d._rawIdentifier()
  assert(identity1 != identity2)
  assert(isNativeDictionary(d))
  assert(d.count == 4)

  v = d[TestObjCKeyTy(10)] as! TestObjCValueTy
  assert(v.value == 1010)

  v = d[TestObjCKeyTy(20)] as! TestObjCValueTy
  assert(v.value == 1020)

  v = d[TestObjCKeyTy(30)] as! TestObjCValueTy
  assert(v.value == 1030)

  v = d[TestObjCKeyTy(40)] as! TestObjCValueTy
  assert(v.value == 2040)

  // Overwrite value in existing binding.
  d[TestObjCKeyTy(10)] = TestObjCValueTy(2010)
  assert(identity2 == d._rawIdentifier())
  assert(isNativeDictionary(d))
  assert(d.count == 4)

  v = d[TestObjCKeyTy(10)] as! TestObjCValueTy
  assert(v.value == 2010)

  v = d[TestObjCKeyTy(20)] as! TestObjCValueTy
  assert(v.value == 1020)

  v = d[TestObjCKeyTy(30)] as! TestObjCValueTy
  assert(v.value == 1030)

  v = d[TestObjCKeyTy(40)] as! TestObjCValueTy
  assert(v.value == 2040)
}

DictionaryTestSuite.test("BridgedFromObjC.Nonverbatim.SubscriptWithKey") {
  var d = getBridgedNonverbatimDictionary()
  let identity1 = d._rawIdentifier()
  assert(isNativeDictionary(d))

  // Read existing key-value pairs.
  var v = d[TestBridgedKeyTy(10)]
  assert(v!.value == 1010)

  v = d[TestBridgedKeyTy(20)]
  assert(v!.value == 1020)

  v = d[TestBridgedKeyTy(30)]
  assert(v!.value == 1030)

  assert(identity1 == d._rawIdentifier())

  // Insert a new key-value pair.
  d[TestBridgedKeyTy(40)] = TestBridgedValueTy(2040)

  let identity2 = d._rawIdentifier()
  // Storage identity may or may not change depending on allocation behavior.
  // (d is eagerly bridged to a regular uniquely referenced native Dictionary.)
  //assert(identity1 != identity2)

  assert(isNativeDictionary(d))
  assert(d.count == 4)

  v = d[TestBridgedKeyTy(10)]
  assert(v!.value == 1010)

  v = d[TestBridgedKeyTy(20)]
  assert(v!.value == 1020)

  v = d[TestBridgedKeyTy(30)]
  assert(v!.value == 1030)

  v = d[TestBridgedKeyTy(40)]
  assert(v!.value == 2040)

  // Overwrite value in existing binding.
  d[TestBridgedKeyTy(10)] = TestBridgedValueTy(2010)
  assert(identity2 == d._rawIdentifier())
  assert(isNativeDictionary(d))
  assert(d.count == 4)

  v = d[TestBridgedKeyTy(10)]
  assert(v!.value == 2010)

  v = d[TestBridgedKeyTy(20)]
  assert(v!.value == 1020)

  v = d[TestBridgedKeyTy(30)]
  assert(v!.value == 1030)

  v = d[TestBridgedKeyTy(40)]
  assert(v!.value == 2040)
}

DictionaryTestSuite.test("BridgedFromObjC.Verbatim.UpdateValueForKey") {
  // Insert a new key-value pair.
  do {
    var d = getBridgedVerbatimDictionary()
    let identity1 = d._rawIdentifier()
    assert(isCocoaDictionary(d))

    let oldValue: AnyObject? =
        d.updateValue(TestObjCValueTy(2040), forKey: TestObjCKeyTy(40))
    assert(oldValue == nil)
    let identity2 = d._rawIdentifier()
    assert(identity1 != identity2)
    assert(isNativeDictionary(d))
    assert(d.count == 4)

    assert((d[TestObjCKeyTy(10)] as! TestObjCValueTy).value == 1010)
    assert((d[TestObjCKeyTy(20)] as! TestObjCValueTy).value == 1020)
    assert((d[TestObjCKeyTy(30)] as! TestObjCValueTy).value == 1030)
    assert((d[TestObjCKeyTy(40)] as! TestObjCValueTy).value == 2040)
  }

  // Overwrite a value in existing binding.
  do {
    var d = getBridgedVerbatimDictionary()
    let identity1 = d._rawIdentifier()
    assert(isCocoaDictionary(d))

    let oldValue: AnyObject? =
        d.updateValue(TestObjCValueTy(2010), forKey: TestObjCKeyTy(10))
    assert((oldValue as! TestObjCValueTy).value == 1010)

    let identity2 = d._rawIdentifier()
    assert(identity1 != identity2)
    assert(isNativeDictionary(d))
    assert(d.count == 3)

    assert((d[TestObjCKeyTy(10)] as! TestObjCValueTy).value == 2010)
    assert((d[TestObjCKeyTy(20)] as! TestObjCValueTy).value == 1020)
    assert((d[TestObjCKeyTy(30)] as! TestObjCValueTy).value == 1030)
  }
}

DictionaryTestSuite.test("BridgedFromObjC.Nonverbatim.UpdateValueForKey") {
  // Insert a new key-value pair.
  do {
    var d = getBridgedNonverbatimDictionary()
    // let identity1 = d._rawIdentifier()
    assert(isNativeDictionary(d))

    let oldValue =
        d.updateValue(TestBridgedValueTy(2040), forKey: TestBridgedKeyTy(40))
    assert(oldValue == nil)
    // let identity2 = d._rawIdentifier()
    // Storage identity may or may not change depending on allocation behavior.
    // (d is eagerly bridged to a regular uniquely referenced native Dictionary.)
    //assert(identity1 != identity2)
    assert(isNativeDictionary(d))
    assert(d.count == 4)

    assert(d[TestBridgedKeyTy(10)]!.value == 1010)
    assert(d[TestBridgedKeyTy(20)]!.value == 1020)
    assert(d[TestBridgedKeyTy(30)]!.value == 1030)
    assert(d[TestBridgedKeyTy(40)]!.value == 2040)
  }

  // Overwrite a value in existing binding.
  do {
    var d = getBridgedNonverbatimDictionary()
    let identity1 = d._rawIdentifier()
    assert(isNativeDictionary(d))

    let oldValue =
        d.updateValue(TestBridgedValueTy(2010), forKey: TestBridgedKeyTy(10))!
    assert(oldValue.value == 1010)

    let identity2 = d._rawIdentifier()
    assert(identity1 == identity2)
    assert(isNativeDictionary(d))
    assert(d.count == 3)

    assert(d[TestBridgedKeyTy(10)]!.value == 2010)
    assert(d[TestBridgedKeyTy(20)]!.value == 1020)
    assert(d[TestBridgedKeyTy(30)]!.value == 1030)
  }
}


DictionaryTestSuite.test("BridgedFromObjC.Verbatim.RemoveAt") {
  var d = getBridgedVerbatimDictionary()
  let identity1 = d._rawIdentifier()
  assert(isCocoaDictionary(d))

  let foundIndex1 = d.index(forKey: TestObjCKeyTy(10))!
  assert(d[foundIndex1].0 == TestObjCKeyTy(10))
  assert((d[foundIndex1].1 as! TestObjCValueTy).value == 1010)
  assert(identity1 == d._rawIdentifier())

  let removedElement = d.remove(at: foundIndex1)
  assert(identity1 != d._rawIdentifier())
  assert(isNativeDictionary(d))
  assert(removedElement.0 == TestObjCKeyTy(10))
  assert((removedElement.1 as! TestObjCValueTy).value == 1010)
  assert(d.count == 2)
  assert(d.index(forKey: TestObjCKeyTy(10)) == nil)
}

DictionaryTestSuite.test("BridgedFromObjC.Nonverbatim.RemoveAt")
  .code {
  var d = getBridgedNonverbatimDictionary()
  let identity1 = d._rawIdentifier()
  assert(isNativeDictionary(d))

  let foundIndex1 = d.index(forKey: TestBridgedKeyTy(10))!
  assert(d[foundIndex1].0 == TestBridgedKeyTy(10))
  assert(d[foundIndex1].1.value == 1010)
  assert(identity1 == d._rawIdentifier())

  let removedElement = d.remove(at: foundIndex1)
  assert(identity1 == d._rawIdentifier())
  assert(isNativeDictionary(d))
  assert(removedElement.0 == TestObjCKeyTy(10) as TestBridgedKeyTy)
  assert(removedElement.1.value == 1010)
  assert(d.count == 2)
  assert(d.index(forKey: TestBridgedKeyTy(10)) == nil)
}


DictionaryTestSuite.test("BridgedFromObjC.Verbatim.RemoveValueForKey") {
  do {
    var d = getBridgedVerbatimDictionary()
    let identity1 = d._rawIdentifier()
    assert(isCocoaDictionary(d))

    var deleted: AnyObject? = d.removeValue(forKey: TestObjCKeyTy(0))
    assert(deleted == nil)
    assert(identity1 == d._rawIdentifier())
    assert(isCocoaDictionary(d))

    deleted = d.removeValue(forKey: TestObjCKeyTy(10))
    assert((deleted as! TestObjCValueTy).value == 1010)
    let identity2 = d._rawIdentifier()
    assert(identity1 != identity2)
    assert(isNativeDictionary(d))
    assert(d.count == 2)

    assert(d[TestObjCKeyTy(10)] == nil)
    assert((d[TestObjCKeyTy(20)] as! TestObjCValueTy).value == 1020)
    assert((d[TestObjCKeyTy(30)] as! TestObjCValueTy).value == 1030)
    assert(identity2 == d._rawIdentifier())
  }

  do {
    var d1 = getBridgedVerbatimDictionary()
    let identity1 = d1._rawIdentifier()

    var d2 = d1
    assert(isCocoaDictionary(d1))
    assert(isCocoaDictionary(d2))

    var deleted: AnyObject? = d2.removeValue(forKey: TestObjCKeyTy(0))
    assert(deleted == nil)
    assert(identity1 == d1._rawIdentifier())
    assert(identity1 == d2._rawIdentifier())
    assert(isCocoaDictionary(d1))
    assert(isCocoaDictionary(d2))

    deleted = d2.removeValue(forKey: TestObjCKeyTy(10))
    assert((deleted as! TestObjCValueTy).value == 1010)
    let identity2 = d2._rawIdentifier()
    assert(identity1 != identity2)
    assert(isCocoaDictionary(d1))
    assert(isNativeDictionary(d2))
    assert(d2.count == 2)

    assert((d1[TestObjCKeyTy(10)] as! TestObjCValueTy).value == 1010)
    assert((d1[TestObjCKeyTy(20)] as! TestObjCValueTy).value == 1020)
    assert((d1[TestObjCKeyTy(30)] as! TestObjCValueTy).value == 1030)
    assert(identity1 == d1._rawIdentifier())

    assert(d2[TestObjCKeyTy(10)] == nil)
    assert((d2[TestObjCKeyTy(20)] as! TestObjCValueTy).value == 1020)
    assert((d2[TestObjCKeyTy(30)] as! TestObjCValueTy).value == 1030)
    assert(identity2 == d2._rawIdentifier())
  }
}

DictionaryTestSuite.test("BridgedFromObjC.Nonverbatim.RemoveValueForKey")
  .code {
  do {
    var d = getBridgedNonverbatimDictionary()
    let identity1 = d._rawIdentifier()
    assert(isNativeDictionary(d))

    var deleted = d.removeValue(forKey: TestBridgedKeyTy(0))
    assert(deleted == nil)
    assert(identity1 == d._rawIdentifier())
    assert(isNativeDictionary(d))

    deleted = d.removeValue(forKey: TestBridgedKeyTy(10))
    assert(deleted!.value == 1010)
    let identity2 = d._rawIdentifier()
    assert(identity1 == identity2)
    assert(isNativeDictionary(d))
    assert(d.count == 2)

    assert(d[TestBridgedKeyTy(10)] == nil)
    assert(d[TestBridgedKeyTy(20)]!.value == 1020)
    assert(d[TestBridgedKeyTy(30)]!.value == 1030)
    assert(identity2 == d._rawIdentifier())
  }

  do {
    var d1 = getBridgedNonverbatimDictionary()
    let identity1 = d1._rawIdentifier()

    var d2 = d1
    assert(isNativeDictionary(d1))
    assert(isNativeDictionary(d2))

    var deleted = d2.removeValue(forKey: TestBridgedKeyTy(0))
    assert(deleted == nil)
    assert(identity1 == d1._rawIdentifier())
    assert(identity1 == d2._rawIdentifier())
    assert(isNativeDictionary(d1))
    assert(isNativeDictionary(d2))

    deleted = d2.removeValue(forKey: TestBridgedKeyTy(10))
    assert(deleted!.value == 1010)
    let identity2 = d2._rawIdentifier()
    assert(identity1 != identity2)
    assert(isNativeDictionary(d1))
    assert(isNativeDictionary(d2))
    assert(d2.count == 2)

    assert(d1[TestBridgedKeyTy(10)]!.value == 1010)
    assert(d1[TestBridgedKeyTy(20)]!.value == 1020)
    assert(d1[TestBridgedKeyTy(30)]!.value == 1030)
    assert(identity1 == d1._rawIdentifier())

    assert(d2[TestBridgedKeyTy(10)] == nil)
    assert(d2[TestBridgedKeyTy(20)]!.value == 1020)
    assert(d2[TestBridgedKeyTy(30)]!.value == 1030)
    assert(identity2 == d2._rawIdentifier())
  }
}


DictionaryTestSuite.test("BridgedFromObjC.Verbatim.RemoveAll") {
  do {
    var d = getBridgedVerbatimDictionary([:])
    assert(isCocoaDictionary(d))
    assert(d.count == 0)

    let empty = Dictionary<Int, Int>()
    expectNotEqual(empty._rawIdentifier(), d._rawIdentifier())

    d.removeAll()
    assert(empty._rawIdentifier() == d._rawIdentifier())
    assert(d.count == 0)
  }

  do {
    var d = getBridgedVerbatimDictionary()
    let identity1 = d._rawIdentifier()
    assert(isCocoaDictionary(d))
    let originalCapacity = d.count
    assert(d.count == 3)
    assert((d[TestObjCKeyTy(10)] as! TestObjCValueTy).value == 1010)

    d.removeAll()
    assert(identity1 != d._rawIdentifier())
    assert(d.capacity < originalCapacity)
    assert(d.count == 0)
    assert(d[TestObjCKeyTy(10)] == nil)
  }

  do {
    var d = getBridgedVerbatimDictionary()
    let identity1 = d._rawIdentifier()
    assert(isCocoaDictionary(d))
    let originalCapacity = d.count
    assert(d.count == 3)
    assert((d[TestObjCKeyTy(10)] as! TestObjCValueTy).value == 1010)

    d.removeAll(keepingCapacity: true)
    assert(identity1 != d._rawIdentifier())
    assert(d.capacity >= originalCapacity)
    assert(d.count == 0)
    assert(d[TestObjCKeyTy(10)] == nil)
  }

  do {
    var d1 = getBridgedVerbatimDictionary()
    let identity1 = d1._rawIdentifier()
    assert(isCocoaDictionary(d1))
    let originalCapacity = d1.count
    assert(d1.count == 3)
    assert((d1[TestObjCKeyTy(10)] as! TestObjCValueTy).value == 1010)

    var d2 = d1
    d2.removeAll()
    let identity2 = d2._rawIdentifier()
    assert(identity1 == d1._rawIdentifier())
    assert(identity2 != identity1)
    assert(d1.count == 3)
    assert((d1[TestObjCKeyTy(10)] as! TestObjCValueTy).value == 1010)
    assert(d2.capacity < originalCapacity)
    assert(d2.count == 0)
    assert(d2[TestObjCKeyTy(10)] == nil)
  }

  do {
    var d1 = getBridgedVerbatimDictionary()
    let identity1 = d1._rawIdentifier()
    assert(isCocoaDictionary(d1))
    let originalCapacity = d1.count
    assert(d1.count == 3)
    assert((d1[TestObjCKeyTy(10)] as! TestObjCValueTy).value == 1010)

    var d2 = d1
    d2.removeAll(keepingCapacity: true)
    let identity2 = d2._rawIdentifier()
    assert(identity1 == d1._rawIdentifier())
    assert(identity2 != identity1)
    assert(d1.count == 3)
    assert((d1[TestObjCKeyTy(10)] as! TestObjCValueTy).value == 1010)
    assert(d2.capacity >= originalCapacity)
    assert(d2.count == 0)
    assert(d2[TestObjCKeyTy(10)] == nil)
  }
}

DictionaryTestSuite.test("BridgedFromObjC.Nonverbatim.RemoveAll") {
  if #available(macOS 15.0, iOS 13.0, watchOS 6.0, tvOS 13.0, *) {
    // Identity of empty dictionaries changed in
    // https://github.com/apple/swift/pull/22527
    var d = getBridgedNonverbatimDictionary([:])
    assert(isNativeDictionary(d))
    assert(d.count == 0)

    let empty = Dictionary<Int, Int>()
    expectEqual(empty._rawIdentifier(), d._rawIdentifier())

    d.removeAll()
    assert(empty._rawIdentifier() == d._rawIdentifier())
    assert(d.count == 0)
  }

  do {
    var d = getBridgedNonverbatimDictionary()
    let identity1 = d._rawIdentifier()
    assert(isNativeDictionary(d))
    let originalCapacity = d.count
    assert(d.count == 3)
    assert(d[TestBridgedKeyTy(10)]!.value == 1010)

    d.removeAll()
    assert(identity1 != d._rawIdentifier())
    assert(d.capacity < originalCapacity)
    assert(d.count == 0)
    assert(d[TestBridgedKeyTy(10)] == nil)
  }

  do {
    var d = getBridgedNonverbatimDictionary()
    let identity1 = d._rawIdentifier()
    assert(isNativeDictionary(d))
    let originalCapacity = d.count
    assert(d.count == 3)
    assert(d[TestBridgedKeyTy(10)]!.value == 1010)

    d.removeAll(keepingCapacity: true)
    assert(identity1 == d._rawIdentifier())
    assert(d.capacity >= originalCapacity)
    assert(d.count == 0)
    assert(d[TestBridgedKeyTy(10)] == nil)
  }

  do {
    let d1 = getBridgedNonverbatimDictionary()
    let identity1 = d1._rawIdentifier()
    assert(isNativeDictionary(d1))
    let originalCapacity = d1.count
    assert(d1.count == 3)
    assert(d1[TestBridgedKeyTy(10)]!.value == 1010)

    var d2 = d1
    d2.removeAll()
    let identity2 = d2._rawIdentifier()
    assert(identity1 == d1._rawIdentifier())
    assert(identity2 != identity1)
    assert(d1.count == 3)
    assert(d1[TestBridgedKeyTy(10)]!.value == 1010)
    assert(d2.capacity < originalCapacity)
    assert(d2.count == 0)
    assert(d2[TestBridgedKeyTy(10)] == nil)
  }

  do {
    let d1 = getBridgedNonverbatimDictionary()
    let identity1 = d1._rawIdentifier()
    assert(isNativeDictionary(d1))
    let originalCapacity = d1.count
    assert(d1.count == 3)
    assert(d1[TestBridgedKeyTy(10)]!.value == 1010)

    var d2 = d1
    d2.removeAll(keepingCapacity: true)
    let identity2 = d2._rawIdentifier()
    assert(identity1 == d1._rawIdentifier())
    assert(identity2 != identity1)
    assert(d1.count == 3)
    assert(d1[TestBridgedKeyTy(10)]!.value == 1010)
    assert(d2.capacity >= originalCapacity)
    assert(d2.count == 0)
    assert(d2[TestBridgedKeyTy(10)] == nil)
  }
}

DictionaryTestSuite.test("BridgedFromObjC.Verbatim.Count") {
  let d = getBridgedVerbatimDictionary()
  let identity1 = d._rawIdentifier()
  assert(isCocoaDictionary(d))

  assert(d.count == 3)
  assert(identity1 == d._rawIdentifier())
}

DictionaryTestSuite.test("BridgedFromObjC.Nonverbatim.Count") {
  let d = getBridgedNonverbatimDictionary()
  let identity1 = d._rawIdentifier()
  assert(isNativeDictionary(d))

  assert(d.count == 3)
  assert(identity1 == d._rawIdentifier())
}


DictionaryTestSuite.test("BridgedFromObjC.Verbatim.Generate") {
  let d = getBridgedVerbatimDictionary()
  let identity1 = d._rawIdentifier()
  assert(isCocoaDictionary(d))

  var iter = d.makeIterator()
  var pairs = Array<(Int, Int)>()
  while let (key, value) = iter.next() {
    let kv = ((key as! TestObjCKeyTy).value, (value as! TestObjCValueTy).value)
    pairs.append(kv)
  }
  assert(equalsUnordered(pairs, [ (10, 1010), (20, 1020), (30, 1030) ]))
  assert(iter.next() == nil)
  assert(iter.next() == nil)
  assert(iter.next() == nil)
  assert(identity1 == d._rawIdentifier())
}

DictionaryTestSuite.test("BridgedFromObjC.Nonverbatim.Generate") {
  let d = getBridgedNonverbatimDictionary()
  let identity1 = d._rawIdentifier()
  assert(isNativeDictionary(d))

  var iter = d.makeIterator()
  var pairs = Array<(Int, Int)>()
  while let (key, value) = iter.next() {
    let kv = (key.value, value.value)
    pairs.append(kv)
  }
  assert(equalsUnordered(pairs, [ (10, 1010), (20, 1020), (30, 1030) ]))
  assert(iter.next() == nil)
  assert(iter.next() == nil)
  assert(iter.next() == nil)
  assert(identity1 == d._rawIdentifier())
}

DictionaryTestSuite.test("BridgedFromObjC.Verbatim.Generate_Empty") {
  let d = getBridgedVerbatimDictionary([:])
  let identity1 = d._rawIdentifier()
  assert(isCocoaDictionary(d))

  var iter = d.makeIterator()
  // Cannot write code below because of
  // <rdar://problem/16811736> Optional tuples are broken as optionals regarding == comparison
  // assert(iter.next() == .none)
  assert(iter.next() == nil)
  assert(iter.next() == nil)
  assert(iter.next() == nil)
  assert(iter.next() == nil)
  assert(identity1 == d._rawIdentifier())
}

DictionaryTestSuite.test("BridgedFromObjC.Nonverbatim.Generate_Empty") {
  let d = getBridgedNonverbatimDictionary([:])
  let identity1 = d._rawIdentifier()
  assert(isNativeDictionary(d))

  var iter = d.makeIterator()
  // Cannot write code below because of
  // <rdar://problem/16811736> Optional tuples are broken as optionals regarding == comparison
  // assert(iter.next() == .none)
  assert(iter.next() == nil)
  assert(iter.next() == nil)
  assert(iter.next() == nil)
  assert(iter.next() == nil)
  assert(identity1 == d._rawIdentifier())
}


DictionaryTestSuite.test("BridgedFromObjC.Verbatim.Generate_Huge") {
  let d = getHugeBridgedVerbatimDictionary()
  let identity1 = d._rawIdentifier()
  assert(isCocoaDictionary(d))

  var iter = d.makeIterator()
  var pairs = Array<(Int, Int)>()
  while let (key, value) = iter.next() {
    let kv = ((key as! TestObjCKeyTy).value, (value as! TestObjCValueTy).value)
    pairs.append(kv)
  }
  var expectedPairs = Array<(Int, Int)>()
  for i in 1...32 {
    expectedPairs += [(i, 1000 + i)]
  }
  assert(equalsUnordered(pairs, expectedPairs))
  assert(iter.next() == nil)
  assert(iter.next() == nil)
  assert(iter.next() == nil)
  assert(identity1 == d._rawIdentifier())
}

DictionaryTestSuite.test("BridgedFromObjC.Nonverbatim.Generate_Huge") {
  let d = getHugeBridgedNonverbatimDictionary()
  let identity1 = d._rawIdentifier()
  assert(isNativeDictionary(d))

  var iter = d.makeIterator()
  var pairs = Array<(Int, Int)>()
  while let (key, value) = iter.next() {
    let kv = (key.value, value.value)
    pairs.append(kv)
  }
  var expectedPairs = Array<(Int, Int)>()
  for i in 1...32 {
    expectedPairs += [(i, 1000 + i)]
  }
  assert(equalsUnordered(pairs, expectedPairs))
  assert(iter.next() == nil)
  assert(iter.next() == nil)
  assert(iter.next() == nil)
  assert(identity1 == d._rawIdentifier())
}


DictionaryTestSuite.test("BridgedFromObjC.Verbatim.Generate_ParallelArray") {
autoreleasepoolIfUnoptimizedReturnAutoreleased {
  // Add an autorelease pool because ParallelArrayDictionary autoreleases
  // values in objectForKey.

  let d = getParallelArrayBridgedVerbatimDictionary()
  let identity1 = d._rawIdentifier()
  assert(isCocoaDictionary(d))

  var iter = d.makeIterator()
  var pairs = Array<(Int, Int)>()
  while let (key, value) = iter.next() {
    let kv = ((key as! TestObjCKeyTy).value, (value as! TestObjCValueTy).value)
    pairs.append(kv)
  }
  let expectedPairs = [ (10, 1111), (20, 1111), (30, 1111), (40, 1111) ]
  assert(equalsUnordered(pairs, expectedPairs))
  assert(iter.next() == nil)
  assert(iter.next() == nil)
  assert(iter.next() == nil)
  assert(identity1 == d._rawIdentifier())
}
}

DictionaryTestSuite.test("BridgedFromObjC.Nonverbatim.Generate_ParallelArray") {
  //some bridged NSDictionary operations on non-standard NSDictionary subclasses
  //autorelease keys and values. Make sure the leak checker isn't confused
autoreleasepool {

  let d = getParallelArrayBridgedNonverbatimDictionary()
  let identity1 = d._rawIdentifier()
  assert(isNativeDictionary(d))

  var iter = d.makeIterator()
  var pairs = Array<(Int, Int)>()
  while let (key, value) = iter.next() {
    let kv = (key.value, value.value)
    pairs.append(kv)
  }
  let expectedPairs = [ (10, 1111), (20, 1111), (30, 1111), (40, 1111) ]
  assert(equalsUnordered(pairs, expectedPairs))
  assert(iter.next() == nil)
  assert(iter.next() == nil)
  assert(iter.next() == nil)
  assert(identity1 == d._rawIdentifier())
}
}


DictionaryTestSuite.test("BridgedFromObjC.Verbatim.EqualityTest_Empty") {
  let d1 = getBridgedVerbatimEquatableDictionary([:])
  let identity1 = d1._rawIdentifier()
  assert(isCocoaDictionary(d1))

  var d2 = getBridgedVerbatimEquatableDictionary([:])
  var identity2 = d2._rawIdentifier()
  assert(isCocoaDictionary(d2))

  // We can't check that `identity1 != identity2` because Foundation might be
  // returning the same singleton NSDictionary for empty dictionaries.

  assert(d1 == d2)
  assert(identity1 == d1._rawIdentifier())
  assert(identity2 == d2._rawIdentifier())

  d2[TestObjCKeyTy(10)] = TestObjCEquatableValueTy(2010)
  assert(isNativeDictionary(d2))
  assert(identity2 != d2._rawIdentifier())
  identity2 = d2._rawIdentifier()

  assert(d1 != d2)
  assert(identity1 == d1._rawIdentifier())
  assert(identity2 == d2._rawIdentifier())
}

DictionaryTestSuite.test("BridgedFromObjC.Nonverbatim.EqualityTest_Empty") {
  guard #available(macOS 15.0, iOS 13.0, watchOS 6.0, tvOS 13.0, *) else {
    // Identity of empty dictionaries changed in
    // https://github.com/apple/swift/pull/22527
    return
  }

  let d1 = getBridgedNonverbatimEquatableDictionary([:])
  let identity1 = d1._rawIdentifier()
  assert(isNativeDictionary(d1))

  var d2 = getBridgedNonverbatimEquatableDictionary([:])
  let identity2 = d2._rawIdentifier()
  assert(isNativeDictionary(d2))
  assert(identity1 == identity2)

  assert(d1 == d2)
  assert(identity1 == d1._rawIdentifier())
  assert(identity2 == d2._rawIdentifier())

  d2[TestBridgedKeyTy(10)] = TestBridgedEquatableValueTy(2010)
  assert(isNativeDictionary(d2))
  assert(identity2 != d2._rawIdentifier())

  assert(d1 != d2)
  assert(identity1 == d1._rawIdentifier())
  assert(identity2 != d2._rawIdentifier())
}

DictionaryTestSuite.test("BridgedFromObjC.Verbatim.EqualityTest_Small") {
  func helper(_ nd1: Dictionary<Int, Int>, _ nd2: Dictionary<Int, Int>, _ expectedEq: Bool) {
    let d1 = getBridgedVerbatimEquatableDictionary(nd1)
    let identity1 = d1._rawIdentifier()
    assert(isCocoaDictionary(d1))

    var d2 = getBridgedVerbatimEquatableDictionary(nd2)
    var identity2 = d2._rawIdentifier()
    assert(isCocoaDictionary(d2))

    do {
      let eq1 = (d1 == d2)
      assert(eq1 == expectedEq)

      let eq2 = (d2 == d1)
      assert(eq2 == expectedEq)

      let neq1 = (d1 != d2)
      assert(neq1 != expectedEq)

      let neq2 = (d2 != d1)
      assert(neq2 != expectedEq)
    }
    assert(identity1 == d1._rawIdentifier())
    assert(identity2 == d2._rawIdentifier())

    d2[TestObjCKeyTy(1111)] = TestObjCEquatableValueTy(1111)
    d2[TestObjCKeyTy(1111)] = nil
    assert(isNativeDictionary(d2))
    assert(identity2 != d2._rawIdentifier())
    identity2 = d2._rawIdentifier()

    do {
      let eq1 = (d1 == d2)
      assert(eq1 == expectedEq)

      let eq2 = (d2 == d1)
      assert(eq2 == expectedEq)

      let neq1 = (d1 != d2)
      assert(neq1 != expectedEq)

      let neq2 = (d2 != d1)
      assert(neq2 != expectedEq)
    }
    assert(identity1 == d1._rawIdentifier())
    assert(identity2 == d2._rawIdentifier())
  }

  helper([:], [:], true)

  helper([10: 1010],
         [10: 1010],
         true)

  helper([10: 1010, 20: 1020],
         [10: 1010, 20: 1020],
         true)

  helper([10: 1010, 20: 1020, 30: 1030],
         [10: 1010, 20: 1020, 30: 1030],
         true)

  helper([10: 1010, 20: 1020, 30: 1030],
         [10: 1010, 20: 1020, 1111: 1030],
         false)

  helper([10: 1010, 20: 1020, 30: 1030],
         [10: 1010, 20: 1020, 30: 1111],
         false)

  helper([10: 1010, 20: 1020, 30: 1030],
         [10: 1010, 20: 1020],
         false)

  helper([10: 1010, 20: 1020, 30: 1030],
         [10: 1010],
         false)

  helper([10: 1010, 20: 1020, 30: 1030],
         [:],
         false)

  helper([10: 1010, 20: 1020, 30: 1030],
         [10: 1010, 20: 1020, 30: 1030, 40: 1040],
         false)
}


DictionaryTestSuite.test("BridgedFromObjC.Verbatim.ArrayOfDictionaries") {
  let nsa = NSMutableArray()
  for i in 0..<3 {
    nsa.add(
        getAsNSDictionary([10: 1010 + i, 20: 1020 + i, 30: 1030 + i]))
  }

  var a = nsa as [AnyObject] as! [Dictionary<NSObject, AnyObject>]
  for i in 0..<3 {
    let d = a[i]
    var iter = d.makeIterator()
    var pairs = Array<(Int, Int)>()
    while let (key, value) = iter.next() {
      let kv = ((key as! TestObjCKeyTy).value, (value as! TestObjCValueTy).value)
      pairs.append(kv)
    }
    let expectedPairs = [ (10, 1010 + i), (20, 1020 + i), (30, 1030 + i) ]
    assert(equalsUnordered(pairs, expectedPairs))
  }
}

DictionaryTestSuite.test("BridgedFromObjC.Nonverbatim.ArrayOfDictionaries") {
  let nsa = NSMutableArray()
  for i in 0..<3 {
    nsa.add(
        getAsNSDictionary([10: 1010 + i, 20: 1020 + i, 30: 1030 + i]))
  }

  var a = nsa as [AnyObject] as! [Dictionary<TestBridgedKeyTy, TestBridgedValueTy>]
  for i in 0..<3 {
    let d = a[i]
    var iter = d.makeIterator()
    var pairs = Array<(Int, Int)>()
    while let (key, value) = iter.next() {
      let kv = (key.value, value.value)
      pairs.append(kv)
    }
    let expectedPairs = [ (10, 1010 + i), (20, 1020 + i), (30, 1030 + i) ]
    assert(equalsUnordered(pairs, expectedPairs))
  }
}

DictionaryTestSuite.test("BridgedFromObjC.Nonverbatim.StringEqualityMismatch") {
  // NSString's isEqual(_:) implementation is stricter than Swift's String, so
  // Dictionary values bridged over from Objective-C may have duplicate keys.
  // rdar://problem/35995647
  let cafe1 = "Cafe\u{301}" as NSString
  let cafe2 = "Café" as NSString

  let nsd = NSMutableDictionary()
  nsd.setObject(42, forKey: cafe1)
  nsd.setObject(23, forKey: cafe2)
  expectEqual(2, nsd.count)
  expectTrue((42 as NSNumber).isEqual(nsd.object(forKey: cafe1)))
  expectTrue((23 as NSNumber).isEqual(nsd.object(forKey: cafe2)))

  let d = convertNSDictionaryToDictionary(nsd) as [String: Int]
  expectEqual(1, d.count)
  expectEqual(d["Cafe\u{301}"], d["Café"])
  let v = d["Café"]
  expectTrue(v == 42 || v == 23)
}

DictionaryTestSuite.test("Upcast.StringEqualityMismatch") {
  // Upcasting from NSString to String keys changes their concept of equality,
  // resulting in two equal keys, one of which should be discarded by the
  // downcast. (Along with its associated value.)
  // rdar://problem/35995647
  let d: Dictionary<NSString, NSObject> = [
    "cafe\u{301}": 1 as NSNumber,
    "café": 2 as NSNumber,
  ]
  expectEqual(d.count, 2)
  let d2 = d as Dictionary<String, NSObject>
  expectEqual(d2.count, 1)
}


DictionaryTestSuite.test("BridgedFromObjC.Verbatim.OptionalDowncastFailure") {
  let nsd = NSDictionary(
    objects: [1 as NSNumber, 2 as NSNumber, 3 as NSNumber],
    forKeys: ["One" as NSString, "Two" as NSString, "Three" as NSString])
  expectNotNil(nsd as? [NSString: NSNumber])
  expectNotNil(nsd as? [NSString: NSObject])
  expectNotNil(nsd as? [NSObject: NSNumber])
  expectNil(nsd as? [NSNumber: NSObject])
  expectNil(nsd as? [NSObject: NSString])
}

DictionaryTestSuite
  .test("BridgedFromObjC.Verbatim.ForcedDowncastFailure.Keys") {
  let nsd = NSDictionary(
    objects: [1 as NSNumber, 2 as NSNumber, 3 as NSNumber],
    forKeys: ["One" as NSString, "Two" as NSString, "Three" as NSString])
  let bridged = nsd as! [NSNumber: NSObject]
  expectCrashLater()
  // The forced downcast succeeds unconditionally; the cast is instead verified
  // when we access individual elements.
  _ = bridged.first
}

DictionaryTestSuite
  .test("BridgedFromObjC.Verbatim.ForcedDowncastFailure.Values") {
  let nsd = NSDictionary(
    objects: [1 as NSNumber, 2 as NSNumber, 3 as NSNumber],
    forKeys: ["One" as NSString, "Two" as NSString, "Three" as NSString])

  let bridged = nsd as! [NSObject: NSString]
  expectCrashLater()
  // The forced downcast succeeds unconditionally; the cast is instead verified
  // when we access individual elements.
  _ = bridged.first
}

DictionaryTestSuite
  .test("BridgedFromObjC.Verbatim.ForcedDowncastFailure.Both") {
  let nsd = NSDictionary(
    objects: [1 as NSNumber, 2 as NSNumber, 3 as NSNumber],
    forKeys: ["One" as NSString, "Two" as NSString, "Three" as NSString])
  let bridged = nsd as! [NSNumber: NSString]
  expectCrashLater()
  // The forced downcast succeeds unconditionally; the cast is instead verified
  // when we access individual elements.
  _ = bridged.first
}

DictionaryTestSuite
  .test("BridgedFromObjC.Verbatim.ForcedDowncastFailure.Partial")
  .code {
  let nsd = NSMutableDictionary(
    objects: (0..<10).map { "\($0)" as NSString },
    forKeys: (0..<10).map { $0 as NSNumber })
  nsd.setObject("cuckoo" as NSString, forKey: "cuckoo" as NSString)
  let bridged = nsd as! [NSNumber: NSString]
  // The forced downcast succeeds unconditionally; the cast is instead verified
  // when we access individual elements.
  for i in 0 ..< 10 {
    expectEqual("\(i)" as NSString, bridged[i as NSNumber])
  }
  // The item with the unexpected key is only accessible when we iterate over
  // the elements. There should be a trap when we get to it.
  expectCrashLater()
  for (key, value) in bridged {
    _ = key
    _ = value
  }
}

DictionaryTestSuite.test("BridgedFromObjC.Verbatim.DowncastFailure.LeakTest") {
  let nsd = NSMutableDictionary(
    objects: (0..<100).map { TestObjCEquatableValueTy($0) },
    forKeys: (0..<100).map { TestObjCKeyTy($0) })
  expectNotNil(nsd as? [TestObjCKeyTy: TestObjCEquatableValueTy])
  expectNotNil(nsd as? [TestObjCKeyTy: NSObject])
  expectNotNil(nsd as? [NSObject: TestObjCEquatableValueTy])

  // Inserting a single key-value pair of a different type should make all these
  // downcasts fail.
  nsd.setObject("cuckoo" as NSString, forKey: "cuckoo" as NSString)
  expectNil(nsd as? [TestObjCKeyTy: TestObjCEquatableValueTy])
  expectNil(nsd as? [TestObjCKeyTy: NSObject])
  expectNil(nsd as? [NSObject: TestObjCEquatableValueTy])
  // No crash test here -- we're relying on the leak tests in tearDown.
}

DictionaryTestSuite
  .test("BridgedFromObjC.Nonverbatim.OptionalDowncastFailure") {
  let nsd = NSDictionary(
    objects: [1 as NSNumber, 2 as NSNumber, 3 as NSNumber],
    forKeys: ["One" as NSString, "Two" as NSString, "Three" as NSString])
  expectNotNil(nsd as? [String: Int])
  expectNotNil(nsd as? [String: NSObject])
  expectNotNil(nsd as? [NSObject: Int])
  expectNil(nsd as? [Int: NSObject])
  expectNil(nsd as? [NSObject: String])
}

DictionaryTestSuite
  .test("BridgedFromObjC.Nonverbatim.ForcedDowncastFailure.Keys") {
  let nsd = NSDictionary(
    objects: [1 as NSNumber, 2 as NSNumber, 3 as NSNumber],
    forKeys: ["One" as NSString, "Two" as NSString, "Three" as NSString])
  expectCrashLater()
  _ = nsd as! [Int: NSObject]
}

DictionaryTestSuite
  .test("BridgedFromObjC.Nonverbatim.ForcedDowncastFailure.Values") {
  let nsd = NSDictionary(
    objects: [1 as NSNumber, 2 as NSNumber, 3 as NSNumber],
    forKeys: ["One" as NSString, "Two" as NSString, "Three" as NSString])
  expectCrashLater()
  _ = nsd as! [NSObject: String]
}

DictionaryTestSuite
  .test("BridgedFromObjC.Nonverbatim.ForcedDowncastFailure.Both") {
  let nsd = NSDictionary(
    objects: [1 as NSNumber, 2 as NSNumber, 3 as NSNumber],
    forKeys: ["One" as NSString, "Two" as NSString, "Three" as NSString])
  expectCrashLater()
  _ = nsd as! [Int: String]
}

DictionaryTestSuite
  .test("BridgedFromObjC.Nonverbatim.DowncastFailure.LeakTest") {
  let nsd = NSMutableDictionary(
    objects: (0..<100).map { TestObjCEquatableValueTy($0) },
    forKeys: (0..<100).map { TestObjCKeyTy($0) })
  expectNotNil(nsd as? [TestBridgedKeyTy: TestBridgedEquatableValueTy])
  expectNotNil(nsd as? [TestBridgedKeyTy: NSObject])
  expectNotNil(nsd as? [NSObject: TestBridgedEquatableValueTy])

  // Inserting a single key-value pair of a different type should make all these
  // downcasts fail.
  nsd.setObject("cuckoo" as NSString, forKey: "cuckoo" as NSString)
  expectNil(nsd as? [TestBridgedKeyTy: TestBridgedEquatableValueTy])
  expectNil(nsd as? [TestBridgedKeyTy: NSObject])
  expectNil(nsd as? [NSObject: TestBridgedEquatableValueTy])
  // No crash test here -- we're relying on the leak tests in tearDown.
}

//===---
// Dictionary -> NSDictionary bridging tests.
//
// Key and Value are bridged verbatim.
//===---

DictionaryTestSuite.test("BridgedToObjC.Verbatim.Count") {
  let d = getBridgedNSDictionaryOfRefTypesBridgedVerbatim()

  assert(d.count == 3)
}

DictionaryTestSuite.test("BridgedToObjC.Verbatim.ObjectForKey") {
  let d = getBridgedNSDictionaryOfRefTypesBridgedVerbatim()

  var v: AnyObject? = d.object(forKey: TestObjCKeyTy(10)).map { $0 as AnyObject }
  expectEqual(1010, (v as! TestObjCValueTy).value)
  let idValue10 = unsafeBitCast(v, to: UInt.self)

  v = d.object(forKey: TestObjCKeyTy(20)).map { $0 as AnyObject }
  expectEqual(1020, (v as! TestObjCValueTy).value)
  let idValue20 = unsafeBitCast(v, to: UInt.self)

  v = d.object(forKey: TestObjCKeyTy(30)).map { $0 as AnyObject }
  expectEqual(1030, (v as! TestObjCValueTy).value)
  let idValue30 = unsafeBitCast(v, to: UInt.self)

  expectNil(d.object(forKey: TestObjCKeyTy(40)))

  // NSDictionary can store mixed key types.  Swift's Dictionary is typed, but
  // when bridged to NSDictionary, it should behave like one, and allow queries
  // for mismatched key types.
  expectNil(d.object(forKey: TestObjCInvalidKeyTy()))

  for _ in 0..<3 {
    expectEqual(idValue10, unsafeBitCast(
      d.object(forKey: TestObjCKeyTy(10)).map { $0 as AnyObject }, to: UInt.self))

    expectEqual(idValue20, unsafeBitCast(
      d.object(forKey: TestObjCKeyTy(20)).map { $0 as AnyObject }, to: UInt.self))

    expectEqual(idValue30, unsafeBitCast(
      d.object(forKey: TestObjCKeyTy(30)).map { $0 as AnyObject }, to: UInt.self))
  }

  expectAutoreleasedKeysAndValues(unopt: (0, 3))
}

DictionaryTestSuite.test("BridgedToObjC.Verbatim.KeyEnumerator.NextObject") {
  let d = getBridgedNSDictionaryOfRefTypesBridgedVerbatim()

  var capturedIdentityPairs = Array<(UInt, UInt)>()

  for _ in 0..<3 {
    let enumerator = d.keyEnumerator()

    var dataPairs = Array<(Int, Int)>()
    var identityPairs = Array<(UInt, UInt)>()
    while let key = enumerator.nextObject() {
      let keyObj = key as AnyObject
      let value: AnyObject = d.object(forKey: keyObj)! as AnyObject

      let dataPair =
        ((keyObj as! TestObjCKeyTy).value, (value as! TestObjCValueTy).value)
      dataPairs.append(dataPair)

      let identityPair =
        (unsafeBitCast(keyObj, to: UInt.self),
         unsafeBitCast(value, to: UInt.self))
      identityPairs.append(identityPair)
    }
    expectTrue(
      equalsUnordered(dataPairs, [ (10, 1010), (20, 1020), (30, 1030) ]))

    if capturedIdentityPairs.isEmpty {
      capturedIdentityPairs = identityPairs
    } else {
      expectTrue(equalsUnordered(capturedIdentityPairs, identityPairs))
    }

    assert(enumerator.nextObject() == nil)
    assert(enumerator.nextObject() == nil)
    assert(enumerator.nextObject() == nil)
  }

  expectAutoreleasedKeysAndValues(unopt: (3, 3))
}

DictionaryTestSuite.test("BridgedToObjC.Verbatim.KeyEnumerator.NextObject_Empty") {
  let d = getBridgedEmptyNSDictionary()
  let enumerator = d.keyEnumerator()

  assert(enumerator.nextObject() == nil)
  assert(enumerator.nextObject() == nil)
  assert(enumerator.nextObject() == nil)
}

DictionaryTestSuite.test("BridgedToObjC.Verbatim.KeyEnumerator.FastEnumeration.UseFromSwift") {
  let d = getBridgedNSDictionaryOfRefTypesBridgedVerbatim()

  checkDictionaryFastEnumerationFromSwift(
    [ (10, 1010), (20, 1020), (30, 1030) ],
    d, { d.keyEnumerator() },
    { ($0 as! TestObjCKeyTy).value },
    { ($0 as! TestObjCValueTy).value })

  expectAutoreleasedKeysAndValues(unopt: (3, 3))
}

DictionaryTestSuite.test("BridgedToObjC.Verbatim.KeyEnumerator.FastEnumeration.UseFromObjC") {
  let d = getBridgedNSDictionaryOfRefTypesBridgedVerbatim()

  checkDictionaryFastEnumerationFromObjC(
    [ (10, 1010), (20, 1020), (30, 1030) ],
    d, { d.keyEnumerator() },
    { ($0 as! TestObjCKeyTy).value },
    { ($0 as! TestObjCValueTy).value })

  expectAutoreleasedKeysAndValues(unopt: (3, 3))
}

DictionaryTestSuite.test("BridgedToObjC.Verbatim.KeyEnumerator.FastEnumeration_Empty") {
  let d = getBridgedEmptyNSDictionary()

  checkDictionaryFastEnumerationFromSwift(
    [], d, { d.keyEnumerator() },
    { ($0 as! TestObjCKeyTy).value },
    { ($0 as! TestObjCValueTy).value })

  checkDictionaryFastEnumerationFromObjC(
    [], d, { d.keyEnumerator() },
    { ($0 as! TestObjCKeyTy).value },
    { ($0 as! TestObjCValueTy).value })
}

DictionaryTestSuite.test("BridgedToObjC.Verbatim.FastEnumeration.UseFromSwift") {
  let d = getBridgedNSDictionaryOfRefTypesBridgedVerbatim()

  checkDictionaryFastEnumerationFromSwift(
    [ (10, 1010), (20, 1020), (30, 1030) ],
    d, { d },
    { ($0 as! TestObjCKeyTy).value },
    { ($0 as! TestObjCValueTy).value })

  expectAutoreleasedKeysAndValues(unopt: (0, 3))
}

DictionaryTestSuite.test("BridgedToObjC.Verbatim.FastEnumeration.UseFromObjC") {
  let d = getBridgedNSDictionaryOfRefTypesBridgedVerbatim()

  checkDictionaryFastEnumerationFromObjC(
    [ (10, 1010), (20, 1020), (30, 1030) ],
    d, { d },
    { ($0 as! TestObjCKeyTy).value },
    { ($0 as! TestObjCValueTy).value })

  expectAutoreleasedKeysAndValues(unopt: (0, 3))
}

DictionaryTestSuite.test("BridgedToObjC.Verbatim.FastEnumeration_Empty") {
  let d = getBridgedEmptyNSDictionary()

  checkDictionaryFastEnumerationFromSwift(
    [], d, { d },
    { ($0 as! TestObjCKeyTy).value },
    { ($0 as! TestObjCValueTy).value })

  checkDictionaryFastEnumerationFromObjC(
    [], d, { d },
    { ($0 as! TestObjCKeyTy).value },
    { ($0 as! TestObjCValueTy).value })
}

/// Check for buffer overruns/underruns in Swift's
/// `-[NSDictionary getObjects:andKeys:count:]` implementations.
func checkGetObjectsAndKeys(
  _ dictionary: NSDictionary,
  count: Int,
  file: String = #file,
  line: UInt = #line) {
  let canary = NSObject()
  let storageSize = 2 * max(count, dictionary.count) + 2

  // Create buffers for storing keys and values at +0 refcounts,
  // then call getObjects:andKeys:count: via a shim in
  // StdlibUnittestFoundationExtras.
  typealias UnmanagedPointer = UnsafeMutablePointer<Unmanaged<AnyObject>>
  let keys = UnmanagedPointer.allocate(capacity: storageSize)
  keys.initialize(repeating: Unmanaged.passUnretained(canary), count: storageSize)
  let values = UnmanagedPointer.allocate(capacity: storageSize)
  values.initialize(repeating: Unmanaged.passUnretained(canary), count: storageSize)
  keys.withMemoryRebound(to: AnyObject.self, capacity: storageSize) { k in
    values.withMemoryRebound(to: AnyObject.self, capacity: storageSize) { v in
      dictionary.available_getObjects(
        AutoreleasingUnsafeMutablePointer(v),
        andKeys: AutoreleasingUnsafeMutablePointer(k),
        count: count)
    }
  }
  // Check results.
  for i in 0 ..< storageSize {
    let key = keys[i].takeUnretainedValue()
    let value = values[i].takeUnretainedValue()
    if i < min(count, dictionary.count) {
      expectTrue(
        key !== canary,
        """
        Buffer underrun at offset \(i) with count \(count):
        keys[\(i)] was left unchanged
        """,
        file: file, line: line)
      expectTrue(
        value !== canary,
        """
        Buffer underrun at offset \(i) with count \(count):
        values[\(i)] was left unchanged
        """,
        file: file, line: line)
      if key !== canary, value !== canary {
        autoreleasepoolIfUnoptimizedReturnAutoreleased {
          // We need an autorelease pool because objectForKey returns
          // autoreleased values.
          expectTrue(
            value === dictionary.object(forKey: key) as AnyObject,
            """
            Inconsistency at offset \(i) with count \(count):
            values[\(i)] does not match value for keys[\(i)]
            """,
            file: file, line: line)
        }
      }
    } else {
      expectTrue(
        key === canary,
        """
        Buffer overrun at offset \(i) with count \(count):
        keys[\(i)] was overwritten with value \(key)
        """,
        file: file, line: line)
      expectTrue(
        value === canary,
        """
        Buffer overrun at offset \(i) with count \(count):
        values[\(i)] was overwritten with value \(key)
        """,
        file: file, line: line)
    }
  }
  keys.deinitialize(count: storageSize) // noop
  keys.deallocate()
  values.deinitialize(count: storageSize) // noop
  values.deallocate()
  withExtendedLifetime(canary) {}
  // [NSArray getObjects] does not retain the objects, so keep the dictionary alive.
  withExtendedLifetime(dictionary) {}
}

DictionaryTestSuite.test("BridgedToObjC.Verbatim.getObjects:andKeys:count:") {
  let d = getBridgedNSDictionaryOfRefTypesBridgedVerbatim()
  for count in 0 ..< d.count + 2 {
    checkGetObjectsAndKeys(d, count: count)
  }
}

DictionaryTestSuite.test("BridgedToObjC.Verbatim.getObjects:andKeys:count:/InvalidCount") {
  expectCrashLater()
  let d = getBridgedNSDictionaryOfRefTypesBridgedVerbatim()
  checkGetObjectsAndKeys(d, count: -1)
}

//===---
// Dictionary -> NSDictionary bridging tests.
//
// Key type and value type are bridged non-verbatim.
//===---

DictionaryTestSuite.test("BridgedToObjC.KeyValue_ValueTypesCustomBridged") {
  let d = getBridgedNSDictionaryOfKeyValue_ValueTypesCustomBridged()
  let enumerator = d.keyEnumerator()

  var pairs = Array<(Int, Int)>()
  while let key = enumerator.nextObject() {
    let value: AnyObject = d.object(forKey: key)! as AnyObject
    let kv = ((key as! TestObjCKeyTy).value, (value as! TestObjCValueTy).value)
    pairs.append(kv)
  }
  assert(equalsUnordered(pairs, [ (10, 1010), (20, 1020), (30, 1030) ]))

  expectAutoreleasedKeysAndValues(unopt: (3, 3))
}

DictionaryTestSuite.test("BridgedToObjC.Custom.KeyEnumerator.FastEnumeration.UseFromSwift") {
  let d = getBridgedNSDictionaryOfKeyValue_ValueTypesCustomBridged()

  checkDictionaryFastEnumerationFromSwift(
    [ (10, 1010), (20, 1020), (30, 1030) ],
    d, { d.keyEnumerator() },
    { ($0 as! TestObjCKeyTy).value },
    { ($0 as! TestObjCValueTy).value })

  expectAutoreleasedKeysAndValues(unopt: (3, 3))
}

DictionaryTestSuite.test("BridgedToObjC.Custom.KeyEnumerator.FastEnumeration.UseFromSwift.Partial") {
  let d = getBridgedNSDictionaryOfKeyValue_ValueTypesCustomBridged(
    numElements: 9)

  checkDictionaryEnumeratorPartialFastEnumerationFromSwift(
    [ (10, 1010), (20, 1020), (30, 1030), (40, 1040), (50, 1050),
      (60, 1060), (70, 1070), (80, 1080), (90, 1090) ],
    d, maxFastEnumerationItems: 5,
    { ($0 as! TestObjCKeyTy).value },
    { ($0 as! TestObjCValueTy).value })

  expectAutoreleasedKeysAndValues(unopt: (9, 9))
}

DictionaryTestSuite.test("BridgedToObjC.Custom.KeyEnumerator.FastEnumeration.UseFromObjC") {
  let d = getBridgedNSDictionaryOfKeyValue_ValueTypesCustomBridged()

  checkDictionaryFastEnumerationFromObjC(
    [ (10, 1010), (20, 1020), (30, 1030) ],
    d, { d.keyEnumerator() },
    { ($0 as! TestObjCKeyTy).value },
    { ($0 as! TestObjCValueTy).value })

  expectAutoreleasedKeysAndValues(unopt: (3, 3))
}

DictionaryTestSuite.test("BridgedToObjC.Custom.FastEnumeration.UseFromSwift") {
  let d = getBridgedNSDictionaryOfKeyValue_ValueTypesCustomBridged()

  checkDictionaryFastEnumerationFromSwift(
    [ (10, 1010), (20, 1020), (30, 1030) ],
    d, { d },
    { ($0 as! TestObjCKeyTy).value },
    { ($0 as! TestObjCValueTy).value })

  expectAutoreleasedKeysAndValues(unopt: (0, 3))
}

DictionaryTestSuite.test("BridgedToObjC.Custom.FastEnumeration.UseFromObjC") {
  let d = getBridgedNSDictionaryOfKeyValue_ValueTypesCustomBridged()

  checkDictionaryFastEnumerationFromObjC(
    [ (10, 1010), (20, 1020), (30, 1030) ],
    d, { d },
    { ($0 as! TestObjCKeyTy).value },
    { ($0 as! TestObjCValueTy).value })

  expectAutoreleasedKeysAndValues(unopt: (0, 3))
}

DictionaryTestSuite.test("BridgedToObjC.Custom.FastEnumeration_Empty") {
  let d = getBridgedNSDictionaryOfKeyValue_ValueTypesCustomBridged(
    numElements: 0)

  checkDictionaryFastEnumerationFromSwift(
    [], d, { d },
    { ($0 as! TestObjCKeyTy).value },
    { ($0 as! TestObjCValueTy).value })

  checkDictionaryFastEnumerationFromObjC(
    [], d, { d },
    { ($0 as! TestObjCKeyTy).value },
    { ($0 as! TestObjCValueTy).value })
}

DictionaryTestSuite.test("BridgedToObjC.Custom.getObjects:andKeys:count:") {
  let d = getBridgedNSDictionaryOfKeyValue_ValueTypesCustomBridged()
  for count in 0 ..< d.count + 2 {
    checkGetObjectsAndKeys(d, count: count)
  }
}

DictionaryTestSuite.test("BridgedToObjC.Custom.getObjects:andKeys:count:/InvalidCount") {
  expectCrashLater()
  let d = getBridgedNSDictionaryOfKeyValue_ValueTypesCustomBridged()
  checkGetObjectsAndKeys(d, count: -1)
}

func getBridgedNSDictionaryOfKey_ValueTypeCustomBridged() -> NSDictionary {
  assert(!_isBridgedVerbatimToObjectiveC(TestBridgedKeyTy.self))
  assert(_isBridgedVerbatimToObjectiveC(TestObjCValueTy.self))

  var d = Dictionary<TestBridgedKeyTy, TestObjCValueTy>()
  d[TestBridgedKeyTy(10)] = TestObjCValueTy(1010)
  d[TestBridgedKeyTy(20)] = TestObjCValueTy(1020)
  d[TestBridgedKeyTy(30)] = TestObjCValueTy(1030)

  let bridged = convertDictionaryToNSDictionary(d)
  assert(isNativeNSDictionary(bridged))

  return bridged
}

DictionaryTestSuite.test("BridgedToObjC.Key_ValueTypeCustomBridged") {
  let d = getBridgedNSDictionaryOfKey_ValueTypeCustomBridged()
  let enumerator = d.keyEnumerator()

  var pairs = Array<(Int, Int)>()
  while let key = enumerator.nextObject() {
    let value: AnyObject = d.object(forKey: key)! as AnyObject
    let kv = ((key as! TestObjCKeyTy).value, (value as! TestObjCValueTy).value)
    pairs.append(kv)
  }
  assert(equalsUnordered(pairs, [ (10, 1010), (20, 1020), (30, 1030) ]))

  expectAutoreleasedKeysAndValues(unopt: (3, 3))
}

func getBridgedNSDictionaryOfValue_ValueTypeCustomBridged() -> NSDictionary {
  assert(_isBridgedVerbatimToObjectiveC(TestObjCKeyTy.self))
  assert(!_isBridgedVerbatimToObjectiveC(TestBridgedValueTy.self))

  var d = Dictionary<TestObjCKeyTy, TestBridgedValueTy>()
  d[TestObjCKeyTy(10)] = TestBridgedValueTy(1010)
  d[TestObjCKeyTy(20)] = TestBridgedValueTy(1020)
  d[TestObjCKeyTy(30)] = TestBridgedValueTy(1030)

  let bridged = convertDictionaryToNSDictionary(d)
  assert(isNativeNSDictionary(bridged))

  return bridged
}

DictionaryTestSuite.test("BridgedToObjC.Value_ValueTypeCustomBridged") {
  let d = getBridgedNSDictionaryOfValue_ValueTypeCustomBridged()
  let enumerator = d.keyEnumerator()

  var pairs = Array<(Int, Int)>()
  while let key = enumerator.nextObject() {
    let value: AnyObject = d.object(forKey: key)! as AnyObject
    let kv = ((key as! TestObjCKeyTy).value, (value as! TestObjCValueTy).value)
    pairs.append(kv)
  }
  assert(equalsUnordered(pairs, [ (10, 1010), (20, 1020), (30, 1030) ]))

  expectAutoreleasedKeysAndValues(unopt: (3, 3))
}

//===---
// NSDictionary -> Dictionary -> NSDictionary bridging tests.
//===---

func getRoundtripBridgedNSDictionary() -> NSDictionary {
  let keys = [ 10, 20, 30 ].map { TestObjCKeyTy($0) }
  let values = [ 1010, 1020, 1030 ].map { TestObjCValueTy($0) }

  let nsd = NSDictionary(objects: values, forKeys: keys)

  let d: Dictionary<NSObject, AnyObject> = convertNSDictionaryToDictionary(nsd)

  let bridgedBack = convertDictionaryToNSDictionary(d)
  assert(isCocoaNSDictionary(bridgedBack))
  // FIXME: this should be true.
  //assert(unsafeBitCast(nsd, Int.self) == unsafeBitCast(bridgedBack, Int.self))

  return bridgedBack
}

DictionaryTestSuite.test("BridgingRoundtrip") {
  let d = getRoundtripBridgedNSDictionary()
  let enumerator = d.keyEnumerator()

  var pairs = Array<(key: Int, value: Int)>()
  while let key = enumerator.nextObject() {
    let value: AnyObject = d.object(forKey: key)! as AnyObject
    let kv = ((key as! TestObjCKeyTy).value, (value as! TestObjCValueTy).value)
    pairs.append(kv)
  }
  expectEqualsUnordered([ (10, 1010), (20, 1020), (30, 1030) ], pairs)
}

//===---
// NSDictionary -> Dictionary implicit conversion.
//===---

DictionaryTestSuite.test("NSDictionaryToDictionaryConversion") {
  let keys = [ 10, 20, 30 ].map { TestObjCKeyTy($0) }
  let values = [ 1010, 1020, 1030 ].map { TestObjCValueTy($0) }

  let nsd = NSDictionary(objects: values, forKeys: keys)

  let d: Dictionary = nsd as Dictionary

  var pairs = Array<(Int, Int)>()
  for (key, value) in d {
    let kv = ((key as! TestObjCKeyTy).value, (value as! TestObjCValueTy).value)
    pairs.append(kv)
  }
  assert(equalsUnordered(pairs, [ (10, 1010), (20, 1020), (30, 1030) ]))
}

DictionaryTestSuite.test("DictionaryToNSDictionaryConversion") {
  var d = Dictionary<TestObjCKeyTy, TestObjCValueTy>(minimumCapacity: 32)
  d[TestObjCKeyTy(10)] = TestObjCValueTy(1010)
  d[TestObjCKeyTy(20)] = TestObjCValueTy(1020)
  d[TestObjCKeyTy(30)] = TestObjCValueTy(1030)

  checkDictionaryFastEnumerationFromSwift(
    [ (10, 1010), (20, 1020), (30, 1030) ],
    d as NSDictionary, { d as NSDictionary },
    { ($0 as! TestObjCKeyTy).value },
    { ($0 as! TestObjCValueTy).value })

  expectAutoreleasedKeysAndValues(unopt: (0, 3))
}

//===---
// Dictionary upcasts
//===---

DictionaryTestSuite.test("DictionaryUpcastEntryPoint") {
  var d = Dictionary<TestObjCKeyTy, TestObjCValueTy>(minimumCapacity: 32)
  d[TestObjCKeyTy(10)] = TestObjCValueTy(1010)
  d[TestObjCKeyTy(20)] = TestObjCValueTy(1020)
  d[TestObjCKeyTy(30)] = TestObjCValueTy(1030)

  var dAsAnyObject: Dictionary<NSObject, AnyObject> = _dictionaryUpCast(d)

  assert(dAsAnyObject.count == 3)
  var v: AnyObject? = dAsAnyObject[TestObjCKeyTy(10)]
  assert((v! as! TestObjCValueTy).value == 1010)

  v = dAsAnyObject[TestObjCKeyTy(20)]
  assert((v! as! TestObjCValueTy).value == 1020)

  v = dAsAnyObject[TestObjCKeyTy(30)]
  assert((v! as! TestObjCValueTy).value == 1030)
}

DictionaryTestSuite.test("DictionaryUpcast") {
  var d = Dictionary<TestObjCKeyTy, TestObjCValueTy>(minimumCapacity: 32)
  d[TestObjCKeyTy(10)] = TestObjCValueTy(1010)
  d[TestObjCKeyTy(20)] = TestObjCValueTy(1020)
  d[TestObjCKeyTy(30)] = TestObjCValueTy(1030)

  var dAsAnyObject: Dictionary<NSObject, AnyObject> = d

  assert(dAsAnyObject.count == 3)
  var v: AnyObject? = dAsAnyObject[TestObjCKeyTy(10)]
  assert((v! as! TestObjCValueTy).value == 1010)

  v = dAsAnyObject[TestObjCKeyTy(20)]
  assert((v! as! TestObjCValueTy).value == 1020)

  v = dAsAnyObject[TestObjCKeyTy(30)]
  assert((v! as! TestObjCValueTy).value == 1030)
}

DictionaryTestSuite.test("DictionaryUpcastBridgedEntryPoint") {
  var d = Dictionary<TestBridgedKeyTy, TestBridgedValueTy>(minimumCapacity: 32)
  d[TestBridgedKeyTy(10)] = TestBridgedValueTy(1010)
  d[TestBridgedKeyTy(20)] = TestBridgedValueTy(1020)
  d[TestBridgedKeyTy(30)] = TestBridgedValueTy(1030)

  do {
    var dOO = d as Dictionary<NSObject, AnyObject>

    assert(dOO.count == 3)
    var v: AnyObject? = dOO[TestObjCKeyTy(10)]
    assert((v! as! TestBridgedValueTy).value == 1010)

    v = dOO[TestObjCKeyTy(20)]
    assert((v! as! TestBridgedValueTy).value == 1020)

    v = dOO[TestObjCKeyTy(30)]
    assert((v! as! TestBridgedValueTy).value == 1030)
  }

  do {
    var dOV = d as Dictionary<NSObject, TestBridgedValueTy>

    assert(dOV.count == 3)
    var v = dOV[TestObjCKeyTy(10)]
    assert(v!.value == 1010)

    v = dOV[TestObjCKeyTy(20)]
    assert(v!.value == 1020)

    v = dOV[TestObjCKeyTy(30)]
    assert(v!.value == 1030)
  }

  do {
    var dVO = d as Dictionary<TestBridgedKeyTy, AnyObject>

    assert(dVO.count == 3)
    var v: AnyObject? = dVO[TestBridgedKeyTy(10)]
    assert((v! as! TestBridgedValueTy).value == 1010)

    v = dVO[TestBridgedKeyTy(20)]
    assert((v! as! TestBridgedValueTy).value == 1020)

    v = dVO[TestBridgedKeyTy(30)]
    assert((v! as! TestBridgedValueTy).value == 1030)
  }
}

DictionaryTestSuite.test("DictionaryUpcastBridged") {
  var d = Dictionary<TestBridgedKeyTy, TestBridgedValueTy>(minimumCapacity: 32)
  d[TestBridgedKeyTy(10)] = TestBridgedValueTy(1010)
  d[TestBridgedKeyTy(20)] = TestBridgedValueTy(1020)
  d[TestBridgedKeyTy(30)] = TestBridgedValueTy(1030)

  do {
    var dOO = d as Dictionary<NSObject, AnyObject>

    assert(dOO.count == 3)
    var v: AnyObject? = dOO[TestObjCKeyTy(10)]
    assert((v! as! TestBridgedValueTy).value == 1010)

    v = dOO[TestObjCKeyTy(20)]
    assert((v! as! TestBridgedValueTy).value == 1020)

    v = dOO[TestObjCKeyTy(30)]
    assert((v! as! TestBridgedValueTy).value == 1030)
  }

  do {
    var dOV = d as Dictionary<NSObject, TestBridgedValueTy>

    assert(dOV.count == 3)
    var v = dOV[TestObjCKeyTy(10)]
    assert(v!.value == 1010)

    v = dOV[TestObjCKeyTy(20)]
    assert(v!.value == 1020)

    v = dOV[TestObjCKeyTy(30)]
    assert(v!.value == 1030)
  }

  do {
    var dVO = d as Dictionary<TestBridgedKeyTy, AnyObject>

    assert(dVO.count == 3)
    var v: AnyObject? = dVO[TestBridgedKeyTy(10)]
    assert((v! as! TestBridgedValueTy).value == 1010)

    v = dVO[TestBridgedKeyTy(20)]
    assert((v! as! TestBridgedValueTy).value == 1020)

    v = dVO[TestBridgedKeyTy(30)]
    assert((v! as! TestBridgedValueTy).value == 1030)
  }
}

//===---
// Dictionary downcasts
//===---

DictionaryTestSuite.test("DictionaryDowncastEntryPoint") {
  var d = Dictionary<NSObject, AnyObject>(minimumCapacity: 32)
  d[TestObjCKeyTy(10)] = TestObjCValueTy(1010)
  d[TestObjCKeyTy(20)] = TestObjCValueTy(1020)
  d[TestObjCKeyTy(30)] = TestObjCValueTy(1030)

  // Successful downcast.
  let dCC: Dictionary<TestObjCKeyTy, TestObjCValueTy> = _dictionaryDownCast(d)
  assert(dCC.count == 3)
  var v = dCC[TestObjCKeyTy(10)]
  assert(v!.value == 1010)

  v = dCC[TestObjCKeyTy(20)]
  assert(v!.value == 1020)

  v = dCC[TestObjCKeyTy(30)]
  assert(v!.value == 1030)

  expectAutoreleasedKeysAndValues(unopt: (0, 3))
}

DictionaryTestSuite.test("DictionaryDowncast") {
  var d = Dictionary<NSObject, AnyObject>(minimumCapacity: 32)
  d[TestObjCKeyTy(10)] = TestObjCValueTy(1010)
  d[TestObjCKeyTy(20)] = TestObjCValueTy(1020)
  d[TestObjCKeyTy(30)] = TestObjCValueTy(1030)

  // Successful downcast.
  let dCC = d as! Dictionary<TestObjCKeyTy, TestObjCValueTy>
  assert(dCC.count == 3)
  var v = dCC[TestObjCKeyTy(10)]
  assert(v!.value == 1010)

  v = dCC[TestObjCKeyTy(20)]
  assert(v!.value == 1020)

  v = dCC[TestObjCKeyTy(30)]
  assert(v!.value == 1030)

  expectAutoreleasedKeysAndValues(unopt: (0, 3))
}

DictionaryTestSuite.test("DictionaryDowncastConditionalEntryPoint") {
  var d = Dictionary<NSObject, AnyObject>(minimumCapacity: 32)
  d[TestObjCKeyTy(10)] = TestObjCValueTy(1010)
  d[TestObjCKeyTy(20)] = TestObjCValueTy(1020)
  d[TestObjCKeyTy(30)] = TestObjCValueTy(1030)

  // Successful downcast.
  if let dCC
       = _dictionaryDownCastConditional(d) as Dictionary<TestObjCKeyTy, TestObjCValueTy>? {
    assert(dCC.count == 3)
    var v = dCC[TestObjCKeyTy(10)]
    assert(v!.value == 1010)

    v = dCC[TestObjCKeyTy(20)]
    assert(v!.value == 1020)

    v = dCC[TestObjCKeyTy(30)]
    assert(v!.value == 1030)
  } else {
    assert(false)
  }

  // Unsuccessful downcast
  d["hello" as NSString] = 17 as NSNumber
  if let _
       = _dictionaryDownCastConditional(d) as Dictionary<TestObjCKeyTy, TestObjCValueTy>? {
    assert(false)
  }
}

DictionaryTestSuite.test("DictionaryDowncastConditional") {
  var d = Dictionary<NSObject, AnyObject>(minimumCapacity: 32)
  d[TestObjCKeyTy(10)] = TestObjCValueTy(1010)
  d[TestObjCKeyTy(20)] = TestObjCValueTy(1020)
  d[TestObjCKeyTy(30)] = TestObjCValueTy(1030)

  // Successful downcast.
  if let dCC = d as? Dictionary<TestObjCKeyTy, TestObjCValueTy> {
    assert(dCC.count == 3)
    var v = dCC[TestObjCKeyTy(10)]
    assert(v!.value == 1010)

    v = dCC[TestObjCKeyTy(20)]
    assert(v!.value == 1020)

    v = dCC[TestObjCKeyTy(30)]
    assert(v!.value == 1030)
  } else {
    assert(false)
  }

  // Unsuccessful downcast
  d["hello" as NSString] = 17 as NSNumber
  if d is Dictionary<TestObjCKeyTy, TestObjCValueTy> {
    assert(false)
  }
}

DictionaryTestSuite.test("DictionaryBridgeFromObjectiveCEntryPoint") {
  var d = Dictionary<NSObject, AnyObject>(minimumCapacity: 32)
  d[TestObjCKeyTy(10)] = TestObjCValueTy(1010)
  d[TestObjCKeyTy(20)] = TestObjCValueTy(1020)
  d[TestObjCKeyTy(30)] = TestObjCValueTy(1030)

  // Successful downcast.
  let dCV = d as! Dictionary<TestObjCKeyTy, TestBridgedValueTy>
  do {
    assert(dCV.count == 3)
    var v = dCV[TestObjCKeyTy(10)]
    assert(v!.value == 1010)

    v = dCV[TestObjCKeyTy(20)]
    assert(v!.value == 1020)

    v = dCV[TestObjCKeyTy(30)]
    assert(v!.value == 1030)
  }

  // Successful downcast.
  let dVC = d as! Dictionary<TestBridgedKeyTy, TestObjCValueTy>
  do {
    assert(dVC.count == 3)
    var v = dVC[TestBridgedKeyTy(10)]
    assert(v!.value == 1010)

    v = dVC[TestBridgedKeyTy(20)]
    assert(v!.value == 1020)

    v = dVC[TestBridgedKeyTy(30)]
    assert(v!.value == 1030)
  }

  // Successful downcast.
  let dVV = d as! Dictionary<TestBridgedKeyTy, TestBridgedValueTy>
  do {
    assert(dVV.count == 3)
    var v = dVV[TestBridgedKeyTy(10)]
    assert(v!.value == 1010)

    v = dVV[TestBridgedKeyTy(20)]
    assert(v!.value == 1020)

    v = dVV[TestBridgedKeyTy(30)]
    assert(v!.value == 1030)
  }
}

DictionaryTestSuite.test("DictionaryBridgeFromObjectiveC") {
  var d = Dictionary<NSObject, AnyObject>(minimumCapacity: 32)
  d[TestObjCKeyTy(10)] = TestObjCValueTy(1010)
  d[TestObjCKeyTy(20)] = TestObjCValueTy(1020)
  d[TestObjCKeyTy(30)] = TestObjCValueTy(1030)

  // Successful downcast.
  let dCV = d as! Dictionary<TestObjCKeyTy, TestBridgedValueTy>
  do {
    assert(dCV.count == 3)
    var v = dCV[TestObjCKeyTy(10)]
    assert(v!.value == 1010)

    v = dCV[TestObjCKeyTy(20)]
    assert(v!.value == 1020)

    v = dCV[TestObjCKeyTy(30)]
    assert(v!.value == 1030)
  }

  // Successful downcast.
  let dVC = d as! Dictionary<TestBridgedKeyTy, TestObjCValueTy>
  do {
    assert(dVC.count == 3)
    var v = dVC[TestBridgedKeyTy(10)]
    assert(v!.value == 1010)

    v = dVC[TestBridgedKeyTy(20)]
    assert(v!.value == 1020)

    v = dVC[TestBridgedKeyTy(30)]
    assert(v!.value == 1030)
  }

  // Successful downcast.
  let dVV = d as! Dictionary<TestBridgedKeyTy, TestBridgedValueTy>
  do {
    assert(dVV.count == 3)
    var v = dVV[TestBridgedKeyTy(10)]
    assert(v!.value == 1010)

    v = dVV[TestBridgedKeyTy(20)]
    assert(v!.value == 1020)

    v = dVV[TestBridgedKeyTy(30)]
    assert(v!.value == 1030)
  }
}

DictionaryTestSuite.test("DictionaryBridgeFromObjectiveCConditionalEntryPoint") {
  var d = Dictionary<NSObject, AnyObject>(minimumCapacity: 32)
  d[TestObjCKeyTy(10)] = TestObjCValueTy(1010)
  d[TestObjCKeyTy(20)] = TestObjCValueTy(1020)
  d[TestObjCKeyTy(30)] = TestObjCValueTy(1030)

  // Successful downcast.
  if let dCV = d as? Dictionary<TestObjCKeyTy, TestBridgedValueTy> {
    assert(dCV.count == 3)
    var v = dCV[TestObjCKeyTy(10)]
    assert(v!.value == 1010)

    v = dCV[TestObjCKeyTy(20)]
    assert(v!.value == 1020)

    v = dCV[TestObjCKeyTy(30)]
    assert(v!.value == 1030)
  } else {
    assert(false)
  }

  // Successful downcast.
  if let dVC = d as? Dictionary<TestBridgedKeyTy, TestObjCValueTy> {
    assert(dVC.count == 3)
    var v = dVC[TestBridgedKeyTy(10)]
    assert(v!.value == 1010)

    v = dVC[TestBridgedKeyTy(20)]
    assert(v!.value == 1020)

    v = dVC[TestBridgedKeyTy(30)]
    assert(v!.value == 1030)
  } else {
    assert(false)
  }

  // Successful downcast.
  if let dVV = d as? Dictionary<TestBridgedKeyTy, TestBridgedValueTy> {
    assert(dVV.count == 3)
    var v = dVV[TestBridgedKeyTy(10)]
    assert(v!.value == 1010)

    v = dVV[TestBridgedKeyTy(20)]
    assert(v!.value == 1020)

    v = dVV[TestBridgedKeyTy(30)]
    assert(v!.value == 1030)
  } else {
    assert(false)
  }

  // Unsuccessful downcasts
  d["hello" as NSString] = 17 as NSNumber
  if let _ = d as? Dictionary<TestObjCKeyTy, TestBridgedValueTy> {
    assert(false)
  }
  if let _
       = d as? Dictionary<TestBridgedKeyTy, TestObjCValueTy> {
    assert(false)
  }
  if let _
       = d as? Dictionary<TestBridgedKeyTy, TestBridgedValueTy> {
    assert(false)
  }
}

DictionaryTestSuite.test("DictionaryBridgeFromObjectiveCConditional") {
  var d = Dictionary<NSObject, AnyObject>(minimumCapacity: 32)
  d[TestObjCKeyTy(10)] = TestObjCValueTy(1010)
  d[TestObjCKeyTy(20)] = TestObjCValueTy(1020)
  d[TestObjCKeyTy(30)] = TestObjCValueTy(1030)

  // Successful downcast.
  if let dCV = d as? Dictionary<TestObjCKeyTy, TestBridgedValueTy> {
    assert(dCV.count == 3)
    var v = dCV[TestObjCKeyTy(10)]
    assert(v!.value == 1010)

    v = dCV[TestObjCKeyTy(20)]
    assert(v!.value == 1020)

    v = dCV[TestObjCKeyTy(30)]
    assert(v!.value == 1030)
  } else {
    assert(false)
  }

  // Successful downcast.
  if let dVC = d as? Dictionary<TestBridgedKeyTy, TestObjCValueTy> {
    assert(dVC.count == 3)
    var v = dVC[TestBridgedKeyTy(10)]
    assert(v!.value == 1010)

    v = dVC[TestBridgedKeyTy(20)]
    assert(v!.value == 1020)

    v = dVC[TestBridgedKeyTy(30)]
    assert(v!.value == 1030)
  } else {
    assert(false)
  }

  // Successful downcast.
  if let dVV = d as? Dictionary<TestBridgedKeyTy, TestBridgedValueTy> {
    assert(dVV.count == 3)
    var v = dVV[TestBridgedKeyTy(10)]
    assert(v!.value == 1010)

    v = dVV[TestBridgedKeyTy(20)]
    assert(v!.value == 1020)

    v = dVV[TestBridgedKeyTy(30)]
    assert(v!.value == 1030)
  } else {
    assert(false)
  }

  // Unsuccessful downcasts
  d["hello" as NSString] = 17 as NSNumber
  if d is Dictionary<TestObjCKeyTy, TestBridgedValueTy> {
    assert(false)
  }
  if d is Dictionary<TestBridgedKeyTy, TestObjCValueTy> {
    assert(false)
  }
  if d is Dictionary<TestBridgedKeyTy, TestBridgedValueTy> {
    assert(false)
  }
}
#endif // _runtime(_ObjC)

//===---
// Tests for APIs implemented strictly based on public interface.  We only need
// to test them once, not for every storage type.
//===---

func getDerivedAPIsDictionary() -> Dictionary<Int, Int> {
  var d = Dictionary<Int, Int>(minimumCapacity: 10)
  d[10] = 1010
  d[20] = 1020
  d[30] = 1030
  return d
}

var DictionaryDerivedAPIs = TestSuite("DictionaryDerivedAPIs")

DictionaryDerivedAPIs.test("isEmpty") {
  do {
    let empty = Dictionary<Int, Int>()
    expectTrue(empty.isEmpty)
  }
  do {
    let d = getDerivedAPIsDictionary()
    expectFalse(d.isEmpty)
  }
}

#if _runtime(_ObjC)
@objc
class MockDictionaryWithCustomCount : NSDictionary {
  init(count: Int) {
    self._count = count
    super.init()
  }

  override init() {
    expectUnreachable()
    super.init()
  }

  override init(
    objects: UnsafePointer<AnyObject>?,
    forKeys keys: UnsafePointer<NSCopying>?,
    count: Int) {
    expectUnreachable()
    super.init(objects: objects, forKeys: keys, count: count)
  }

  required init(coder aDecoder: NSCoder) {
    fatalError("init(coder:) not implemented by MockDictionaryWithCustomCount")
  }

  @objc(copyWithZone:)
  override func copy(with zone: NSZone?) -> Any {
    // Ensure that copying this dictionary produces an object of the same
    // dynamic type.
    return self
  }

  override func object(forKey aKey: Any) -> Any? {
    expectUnreachable()
    return NSObject()
  }

  override var count: Int {
    MockDictionaryWithCustomCount.timesCountWasCalled += 1
    return _count
  }

  var _count: Int = 0

  static var timesCountWasCalled = 0
}

func getMockDictionaryWithCustomCount(count: Int)
  -> Dictionary<NSObject, AnyObject> {

  return MockDictionaryWithCustomCount(count: count) as Dictionary
}

func callGenericIsEmpty<C : Collection>(_ collection: C) -> Bool {
  return collection.isEmpty
}

DictionaryDerivedAPIs.test("isEmpty/ImplementationIsCustomized") {
  do {
    let d = getMockDictionaryWithCustomCount(count: 0)
    MockDictionaryWithCustomCount.timesCountWasCalled = 0
    expectTrue(d.isEmpty)
    expectEqual(1, MockDictionaryWithCustomCount.timesCountWasCalled)
  }
  do {
    let d = getMockDictionaryWithCustomCount(count: 0)
    MockDictionaryWithCustomCount.timesCountWasCalled = 0
    expectTrue(callGenericIsEmpty(d))
    expectEqual(1, MockDictionaryWithCustomCount.timesCountWasCalled)
  }

  do {
    let d = getMockDictionaryWithCustomCount(count: 4)
    MockDictionaryWithCustomCount.timesCountWasCalled = 0
    expectFalse(d.isEmpty)
    expectEqual(1, MockDictionaryWithCustomCount.timesCountWasCalled)
  }
  do {
    let d = getMockDictionaryWithCustomCount(count: 4)
    MockDictionaryWithCustomCount.timesCountWasCalled = 0
    expectFalse(callGenericIsEmpty(d))
    expectEqual(1, MockDictionaryWithCustomCount.timesCountWasCalled)
  }
}
#endif // _runtime(_ObjC)

DictionaryDerivedAPIs.test("keys") {
  do {
    let empty = Dictionary<Int, Int>()
    let keys = Array(empty.keys)
    expectTrue(equalsUnordered(keys, []))
  }
  do {
    let d = getDerivedAPIsDictionary()
    let keys = Array(d.keys)
    expectTrue(equalsUnordered(keys, [ 10, 20, 30 ]))
  }
}

DictionaryDerivedAPIs.test("values") {
  do {
    let empty = Dictionary<Int, Int>()
    let values = Array(empty.values)
    expectTrue(equalsUnordered(values, []))
  }
  do {
    var d = getDerivedAPIsDictionary()

    var values = Array(d.values)
    expectTrue(equalsUnordered(values, [ 1010, 1020, 1030 ]))

    d[11] = 1010
    values = Array(d.values)
    expectTrue(equalsUnordered(values, [ 1010, 1010, 1020, 1030 ]))
  }
}

#if _runtime(_ObjC)
var ObjCThunks = TestSuite("ObjCThunks")

class ObjCThunksHelper : NSObject {
  @objc dynamic func acceptArrayBridgedVerbatim(_ array: [TestObjCValueTy]) {
    expectEqual(10, array[0].value)
    expectEqual(20, array[1].value)
    expectEqual(30, array[2].value)
  }

  @objc dynamic func acceptArrayBridgedNonverbatim(_ array: [TestBridgedValueTy]) {
    // Cannot check elements because doing so would bridge them.
    expectEqual(3, array.count)
  }

  @objc dynamic func returnArrayBridgedVerbatim() -> [TestObjCValueTy] {
    return [ TestObjCValueTy(10), TestObjCValueTy(20),
        TestObjCValueTy(30) ]
  }

  @objc dynamic func returnArrayBridgedNonverbatim() -> [TestBridgedValueTy] {
    return [ TestBridgedValueTy(10), TestBridgedValueTy(20),
        TestBridgedValueTy(30) ]
  }

  @objc dynamic func acceptDictionaryBridgedVerbatim(
      _ d: [TestObjCKeyTy : TestObjCValueTy]) {
    expectEqual(3, d.count)
    expectEqual(1010, d[TestObjCKeyTy(10)]!.value)
    expectEqual(1020, d[TestObjCKeyTy(20)]!.value)
    expectEqual(1030, d[TestObjCKeyTy(30)]!.value)
  }

  @objc dynamic func acceptDictionaryBridgedNonverbatim(
      _ d: [TestBridgedKeyTy : TestBridgedValueTy]) {
    expectEqual(3, d.count)
    // Cannot check elements because doing so would bridge them.
  }

  @objc dynamic func returnDictionaryBridgedVerbatim() ->
      [TestObjCKeyTy : TestObjCValueTy] {
    return [
        TestObjCKeyTy(10): TestObjCValueTy(1010),
        TestObjCKeyTy(20): TestObjCValueTy(1020),
        TestObjCKeyTy(30): TestObjCValueTy(1030),
    ]
  }

  @objc dynamic func returnDictionaryBridgedNonverbatim() ->
      [TestBridgedKeyTy : TestBridgedValueTy] {
    return [
        TestBridgedKeyTy(10): TestBridgedValueTy(1010),
        TestBridgedKeyTy(20): TestBridgedValueTy(1020),
        TestBridgedKeyTy(30): TestBridgedValueTy(1030),
    ]
  }
}

ObjCThunks.test("Array/Accept") {
  let helper = ObjCThunksHelper()

  do {
    helper.acceptArrayBridgedVerbatim(
        [ TestObjCValueTy(10), TestObjCValueTy(20), TestObjCValueTy(30) ])
  }
  do {
    TestBridgedValueTy.bridgeOperations = 0
    helper.acceptArrayBridgedNonverbatim(
        [ TestBridgedValueTy(10), TestBridgedValueTy(20),
          TestBridgedValueTy(30) ])
    expectEqual(0, TestBridgedValueTy.bridgeOperations)
  }
}

ObjCThunks.test("Array/Return") {
  let helper = ObjCThunksHelper()

  do {
    let a = helper.returnArrayBridgedVerbatim()
    expectEqual(10, a[0].value)
    expectEqual(20, a[1].value)
    expectEqual(30, a[2].value)
  }
  do {
    TestBridgedValueTy.bridgeOperations = 0
    let a = helper.returnArrayBridgedNonverbatim()
    expectEqual(0, TestBridgedValueTy.bridgeOperations)

    TestBridgedValueTy.bridgeOperations = 0
    expectEqual(10, a[0].value)
    expectEqual(20, a[1].value)
    expectEqual(30, a[2].value)
    expectEqual(0, TestBridgedValueTy.bridgeOperations)
  }
}

ObjCThunks.test("Dictionary/Accept") {
  let helper = ObjCThunksHelper()

  do {
    helper.acceptDictionaryBridgedVerbatim(
        [ TestObjCKeyTy(10): TestObjCValueTy(1010),
          TestObjCKeyTy(20): TestObjCValueTy(1020),
          TestObjCKeyTy(30): TestObjCValueTy(1030) ])
  }
  do {
    TestBridgedKeyTy.bridgeOperations = 0
    TestBridgedValueTy.bridgeOperations = 0
    helper.acceptDictionaryBridgedNonverbatim(
        [ TestBridgedKeyTy(10): TestBridgedValueTy(1010),
          TestBridgedKeyTy(20): TestBridgedValueTy(1020),
          TestBridgedKeyTy(30): TestBridgedValueTy(1030) ])
    expectEqual(0, TestBridgedKeyTy.bridgeOperations)
    expectEqual(0, TestBridgedValueTy.bridgeOperations)
  }
}

ObjCThunks.test("Dictionary/Return") {
  let helper = ObjCThunksHelper()

  do {
    let d = helper.returnDictionaryBridgedVerbatim()
    expectEqual(3, d.count)
    expectEqual(1010, d[TestObjCKeyTy(10)]!.value)
    expectEqual(1020, d[TestObjCKeyTy(20)]!.value)
    expectEqual(1030, d[TestObjCKeyTy(30)]!.value)
  }
  do {
    TestBridgedKeyTy.bridgeOperations = 0
    TestBridgedValueTy.bridgeOperations = 0
    let d = helper.returnDictionaryBridgedNonverbatim()
    expectEqual(0, TestBridgedKeyTy.bridgeOperations)
    expectEqual(0, TestBridgedValueTy.bridgeOperations)

    TestBridgedKeyTy.bridgeOperations = 0
    TestBridgedValueTy.bridgeOperations = 0
    expectEqual(3, d.count)
    expectEqual(1010, d[TestBridgedKeyTy(10)]!.value)
    expectEqual(1020, d[TestBridgedKeyTy(20)]!.value)
    expectEqual(1030, d[TestBridgedKeyTy(30)]!.value)
    expectEqual(0, TestBridgedKeyTy.bridgeOperations)
    expectEqual(0, TestBridgedValueTy.bridgeOperations)
  }
}
#endif // _runtime(_ObjC)

//===---
// Check that iterators traverse a snapshot of the collection.
//===---

DictionaryTestSuite.test("mutationDoesNotAffectIterator/subscript/store") {
  var dict = getDerivedAPIsDictionary()
  let iter = dict.makeIterator()
  dict[10] = 1011

  expectEqualsUnordered(
    [ (10, 1010), (20, 1020), (30, 1030) ],
    Array(IteratorSequence(iter)))
}

DictionaryTestSuite.test("mutationDoesNotAffectIterator/removeValueForKey,1") {
  var dict = getDerivedAPIsDictionary()
  let iter = dict.makeIterator()
  expectEqual(1010, dict.removeValue(forKey: 10))

  expectEqualsUnordered(
    [ (10, 1010), (20, 1020), (30, 1030) ],
    Array(IteratorSequence(iter)))
}

DictionaryTestSuite.test("mutationDoesNotAffectIterator/removeValueForKey,all") {
  var dict = getDerivedAPIsDictionary()
  let iter = dict.makeIterator()
  expectEqual(1010, dict.removeValue(forKey: 10))
  expectEqual(1020, dict.removeValue(forKey: 20))
  expectEqual(1030, dict.removeValue(forKey: 30))

  expectEqualsUnordered(
    [ (10, 1010), (20, 1020), (30, 1030) ],
    Array(IteratorSequence(iter)))
}

DictionaryTestSuite.test(
  "mutationDoesNotAffectIterator/removeAll,keepingCapacity=false") {
  var dict = getDerivedAPIsDictionary()
  let iter = dict.makeIterator()
  dict.removeAll(keepingCapacity: false)

  expectEqualsUnordered(
    [ (10, 1010), (20, 1020), (30, 1030) ],
    Array(IteratorSequence(iter)))
}

DictionaryTestSuite.test(
  "mutationDoesNotAffectIterator/removeAll,keepingCapacity=true") {
  var dict = getDerivedAPIsDictionary()
  let iter = dict.makeIterator()
  dict.removeAll(keepingCapacity: true)

  expectEqualsUnordered(
    [ (10, 1010), (20, 1020), (30, 1030) ],
    Array(IteratorSequence(iter)))
}

//===---
// Misc tests.
//===---

DictionaryTestSuite.test("misc") {
  do {
    // Dictionary literal
    var dict = ["Hello": 1, "World": 2]

    // Insertion
    dict["Swift"] = 3

    // Access
    expectEqual(1, dict["Hello"])
    expectEqual(2, dict["World"])
    expectEqual(3, dict["Swift"])
    expectNil(dict["Universe"])

    // Overwriting existing value
    dict["Hello"] = 0
    expectEqual(0, dict["Hello"])
    expectEqual(2, dict["World"])
    expectEqual(3, dict["Swift"])
    expectNil(dict["Universe"])
  }

  do {
    // Dictionaries with other types
    var d = [ 1.2: 1, 2.6: 2 ]
    d[3.3] = 3
    expectEqual(1, d[1.2])
    expectEqual(2, d[2.6])
    expectEqual(3, d[3.3])
  }

  do {
    var d = Dictionary<String, Int>(minimumCapacity: 13)
    d["one"] = 1
    d["two"] = 2
    d["three"] = 3
    d["four"] = 4
    d["five"] = 5
    expectEqual(1, d["one"])
    expectEqual(2, d["two"])
    expectEqual(3, d["three"])
    expectEqual(4, d["four"])
    expectEqual(5, d["five"])

    // Iterate over (key, value) tuples as a silly copy
    var d3 = Dictionary<String,Int>(minimumCapacity: 13)

    for (k, v) in d {
      d3[k] = v
    }
    expectEqual(1, d3["one"])
    expectEqual(2, d3["two"])
    expectEqual(3, d3["three"])
    expectEqual(4, d3["four"])
    expectEqual(5, d3["five"])

    expectEqual(3, d.values[d.keys.firstIndex(of: "three")!])
    expectEqual(4, d.values[d.keys.firstIndex(of: "four")!])

    expectEqual(3, d3.values[d3.keys.firstIndex(of: "three")!])
    expectEqual(4, d3.values[d3.keys.firstIndex(of: "four")!])
  }
}

#if _runtime(_ObjC)
DictionaryTestSuite.test("dropsBridgedCache") {
  // rdar://problem/18544533
  // Previously this code would segfault due to a double free in the Dictionary
  // implementation.
  // This test will only fail in address sanitizer.
  var dict = [0:10]
  do {
    let bridged: NSDictionary = dict as NSDictionary
    expectEqual(10, bridged[0 as NSNumber] as! Int)
  }

  dict[0] = 11
  do {
    let bridged: NSDictionary = dict as NSDictionary
    expectEqual(11, bridged[0 as NSNumber] as! Int)
  }
}

DictionaryTestSuite.test("getObjects:andKeys:count:") {
  let native = [1: "one", 2: "two"] as Dictionary<Int, String>
  let d = native as NSDictionary
  let keys = UnsafeMutableBufferPointer(
    start: UnsafeMutablePointer<NSNumber>.allocate(capacity: 2), count: 2)
  let values = UnsafeMutableBufferPointer(
    start: UnsafeMutablePointer<NSString>.allocate(capacity: 2), count: 2)
  let kp = AutoreleasingUnsafeMutablePointer<AnyObject?>(keys.baseAddress!)
  let vp = AutoreleasingUnsafeMutablePointer<AnyObject?>(values.baseAddress!)
  let null: AutoreleasingUnsafeMutablePointer<AnyObject?>? = nil

  let expectedKeys: [NSNumber]
  let expectedValues: [NSString]
  if native.first?.key == 1 {
    expectedKeys = [1, 2]
    expectedValues = ["one", "two"]
  } else {
    expectedKeys = [2, 1]
    expectedValues = ["two", "one"]
  }

  d.available_getObjects(null, andKeys: null, count: 2) // don't segfault

  d.available_getObjects(null, andKeys: kp, count: 2)
  expectEqual(expectedKeys, Array(keys))

  d.available_getObjects(vp, andKeys: null, count: 2)
  expectEqual(expectedValues, Array(values))

  d.available_getObjects(vp, andKeys: kp, count: 2)
  expectEqual(expectedKeys, Array(keys))
  expectEqual(expectedValues, Array(values))
  // [NSArray getObjects] does not retain the objects, so keep the dictionary alive.
  withExtendedLifetime(d) {}
}
#endif

DictionaryTestSuite.test("popFirst") {
  // Empty
  do {
    var d = [Int: Int]()
    let popped = d.popFirst()
    expectNil(popped)
  }

  do {
    var popped = [(Int, Int)]()
    var d: [Int: Int] = [
      1010: 1010,
      2020: 2020,
      3030: 3030,
    ]
    let expected = [(1010, 1010), (2020, 2020), (3030, 3030)]
    while let element = d.popFirst() {
      popped.append(element)
    }
    // Note that removing an element may reorder remaining items, so we
    // can't compare ordering here.
    popped.sort(by: { $0.0 < $1.0 })
    expectEqualSequence(expected, popped) {
      (lhs: (Int, Int), rhs: (Int, Int)) -> Bool in
      lhs.0 == rhs.0 && lhs.1 == rhs.1
    }
    expectTrue(d.isEmpty)
  }
}

DictionaryTestSuite.test("removeAt") {
  // Test removing from the startIndex, the middle, and the end of a dictionary.
  for i in 1...3 {
    var d: [Int: Int] = [
      10: 1010,
      20: 2020,
      30: 3030,
    ]
    let removed = d.remove(at: d.index(forKey: i*10)!)
    expectEqual(i*10, removed.0)
    expectEqual(i*1010, removed.1)
    expectEqual(2, d.count)
    expectNil(d.index(forKey: i))
    let origKeys: [Int] = [10, 20, 30]
    expectEqual(origKeys.filter { $0 != (i*10) }, d.keys.sorted())
  }
}

DictionaryTestSuite.test("updateValue") {
  let key1 = TestKeyTy(42)
  let key2 = TestKeyTy(42)
  let value1 = TestValueTy(1)
  let value2 = TestValueTy(2)

  var d: [TestKeyTy: TestValueTy] = [:]

  expectNil(d.updateValue(value1, forKey: key1))

  expectEqual(d.count, 1)
  let index1 = d.index(forKey: key2)
  expectNotNil(index1)
  expectTrue(d[index1!].key === key1)
  expectTrue(d[index1!].value === value1)

  expectTrue(d.updateValue(value2, forKey: key2) === value1)

  expectEqual(d.count, 1)
  let index2 = d.index(forKey: key2)
  expectEqual(index1, index2)
  // We expect updateValue to keep the original key in place.
  expectTrue(d[index2!].key === key1) // Not key2
  expectTrue(d[index2!].value === value2)
}

DictionaryTestSuite.test("localHashSeeds") {
  // With global hashing, copying elements in hash order between hash tables
  // can become quadratic (see https://github.com/apple/swift/issues/45856).
  //
  // We defeat this by mixing the local storage capacity into the global hash
  // seed, thereby breaking the correlation between bucket indices across
  // hash tables with different sizes.
  //
  // Verify this works by copying a small sampling of elements near the
  // beginning of a large Dictionary into a smaller one. If the elements end up
  // in the same order in the smaller Dictionary, then that indicates we do not
  // use size-dependent seeding.

  let count = 100_000
  // Set a large table size to reduce frequency/length of collision chains.
  var large = [Int: Int](minimumCapacity: 4 * count)
  for i in 1 ..< count {
    large[i] = 2 * i
  }

  let bunch = count / 100 // 1 percent's worth of elements

  // Copy two bunches of elements into another dictionary that's half the size
  // of the first. We start after the initial bunch because the hash table may
  // begin with collided elements wrapped over from the end, and these would be
  // sorted into irregular slots in the smaller table.
  let slice = large.prefix(3 * bunch).dropFirst(bunch)
  var small = [Int: Int](minimumCapacity: large.capacity / 2)
  expectLT(small.capacity, large.capacity)
  for (key, value) in slice {
    small[key] = value
  }

  // Compare the second halves of the new dictionary and the slice.  Ignore the
  // first halves; the first few elements may not be in the correct order if we
  // happened to start copying from the middle of a collision chain.
  let smallKeys = small.dropFirst(bunch).map { $0.key }
  let sliceKeys = slice.dropFirst(bunch).map { $0.key }
  // If this test fails, there is a problem with local hash seeding.
  expectFalse(smallKeys.elementsEqual(sliceKeys))
}

DictionaryTestSuite.test("Hashable") {
  let d1: [Dictionary<Int, String>] = [
    [1: "meow", 2: "meow", 3: "meow"],
    [1: "meow", 2: "meow", 3: "mooo"],
    [1: "meow", 2: "meow", 4: "meow"],
    [1: "meow", 2: "meow", 4: "mooo"]]
  checkHashable(d1, equalityOracle: { $0 == $1 })

  let d2: [Dictionary<Int, Dictionary<Int, String>>] = [
    [1: [2: "meow"]],
    [2: [1: "meow"]],
    [2: [2: "meow"]],
    [1: [1: "meow"]],
    [2: [2: "mooo"]],
    [2: [:]],
    [:]]
  checkHashable(d2, equalityOracle: { $0 == $1 })

  // Dictionary should hash itself in a way that ensures instances get correctly
  // delineated even when they are nested in other commutative collections.
  // These are different Sets, so they should produce different hashes:
  let remix: [Set<Dictionary<String, Int>>] = [
    [["Blanche": 1, "Rose": 2], ["Dorothy": 3, "Sophia": 4]],
    [["Blanche": 1, "Dorothy": 3], ["Rose": 2, "Sophia": 4]],
    [["Blanche": 1, "Sophia": 4], ["Rose": 2, "Dorothy": 3]]
  ]
  checkHashable(remix, equalityOracle: { $0 == $1 })

  // Dictionary ordering is not guaranteed to be consistent across equal
  // instances. In particular, ordering is highly sensitive to the size of the
  // allocated storage buffer. Generate a few copies of the same dictionary with
  // different capacities, and verify that they compare and hash the same.
  var variants: [Dictionary<String, Int>] = []
  for i in 4 ..< 12 {
    var set: Dictionary<String, Int> = [
      "one": 1,   "two": 2,
      "three": 3, "four": 4,
      "five": 5,  "six": 6]
    set.reserveCapacity(1 << i)
    variants.append(set)
  }
  checkHashable(variants, equalityOracle: { _, _ in true })
}

DictionaryTestSuite.test("Values.MutationDoesNotInvalidateIndices.Native") {
  var d = getCOWFastDictionary()
  let i = d.index(forKey: 20)!
  expectEqual(d[i], (key: 20, value: 1020))
  var expected = 1020
  for _ in 0 ..< 100 {
    expected += 1
    d.values[i] += 1
    expectEqual(d[i], (key: 20, value: expected))
  }
}

#if _runtime(_ObjC)
DictionaryTestSuite.test("Values.MutationDoesNotInvalidateIndices.Bridged") {
  let objects: [NSNumber] = [1, 2, 3, 4]
  let keys: [NSString] = ["Blanche", "Rose", "Dorothy", "Sophia"]
  let ns = NSDictionary(objects: objects, forKeys: keys)
  var d = ns as! Dictionary<NSString, NSNumber>

  let i = d.index(forKey: "Rose")!
  expectEqual(d[i].key, "Rose")
  expectEqual(d[i].value, 2 as NSNumber)

  // Mutating a value through the Values view will convert the bridged
  // NSDictionary instance to native Dictionary storage. However, Values is a
  // MutableCollection, so doing so must not invalidate existing indices.
  d.values[i] = 20 as NSNumber

  // The old Cocoa-based index must still work with the new dictionary.
  expectEqual(d.values[i], 20 as NSNumber)

  let i2 = d.index(forKey: "Rose")

  // You should also be able to advance Cocoa indices.
  let j = d.index(after: i)
  expectLT(i, j)

  // Unfortunately, Cocoa and Native indices aren't comparable, so the
  // Collection conformance is not quite perfect.
  expectCrash() {
    print(i == i2)
  }
}
#endif

DictionaryTestSuite.test("Values.Subscript.Uniqueness") {
  var d = getCOWSlowEquatableDictionary()
  let i = d.index(forKey: TestKeyTy(20))!
  expectUnique(&d.values[i])
}

DictionaryTestSuite.test("Values.Subscript.Modify") {
  var d = getCOWSlowEquatableDictionary()
  let i = d.index(forKey: TestKeyTy(20))!
  bumpValue(&d.values[i])
  expectEqual(TestEquatableValueTy(1021), d[TestKeyTy(20)])
}

DictionaryTestSuite.test("Values.Subscript.ModifyThrow") {
  var d = getCOWSlowEquatableDictionary()
  let i = d.index(forKey: TestKeyTy(20))!
  do {
    try bumpValueAndThrow(&d.values[i])
    expectTrue(false, "Did not throw")
  } catch {
    expectTrue(error is TestError)
  }
  expectEqual(TestEquatableValueTy(1021), d[TestKeyTy(20)])
}

DictionaryTestSuite.test("RemoveAt.InvalidatesIndices") {
  var d = getCOWFastDictionary()
  let i = d.index(forKey: 20)!
  let j = d.index(forKey: 10)!

  d.remove(at: j)

  expectCrashLater()
  _ = d[i]
}

DictionaryTestSuite.test("RemoveValueForKey.InvalidatesIndices") {
  var d = getCOWFastDictionary()
  let i = d.index(forKey: 20)!

  d.removeValue(forKey: 10)

  expectCrashLater()
  _ = d[i]
}

DictionaryTestSuite.test("ResizeOnInsertion.InvalidatesIndices") {
  var d = getCOWFastDictionary()
  let i = d.index(forKey: 20)!
  expectEqual(d[i], (key: 20, value: 1020))

  for i in 0 ..< (d.capacity - d.count) {
    d[100 + i] = 100 + i
  }
  expectEqual(d[i], (key: 20, value: 1020))

  d[0] = 0

  expectCrashLater()
  _ = d[i]
}

DictionaryTestSuite.test("ResizeOnUpdate.InvalidatesIndices") {
  var d = getCOWFastDictionary()
  let i = d.index(forKey: 20)!
  expectEqual(d[i], (key: 20, value: 1020))

  for i in 0 ..< (d.capacity - d.count) {
    d.updateValue(100 + i, forKey: 100 + i)
  }
  expectEqual(d[i], (key: 20, value: 1020))

  d.updateValue(0, forKey: 0)

  expectCrashLater()
  _ = d[i]
}

DictionaryTestSuite.test("RemoveAll.InvalidatesIndices") {
  var d = getCOWFastDictionary()
  let i = d.index(forKey: 20)!
  expectEqual(d[i], (key: 20, value: 1020))

  d.removeAll(keepingCapacity: true)

  expectCrashLater()
  _ = d[i]
}

DictionaryTestSuite.test("ReserveCapacity.InvalidatesIndices") {
  var d = getCOWFastDictionary()
  let i = d.index(forKey: 20)!
  expectEqual(d[i], (key: 20, value: 1020))

  d.reserveCapacity(0)
  expectEqual(d[i], (key: 20, value: 1020))

  d.reserveCapacity(d.capacity)
  expectEqual(d[i], (key: 20, value: 1020))

  d.reserveCapacity(d.capacity * 10)

  expectCrashLater()
  _ = d[i]
}

DictionaryTestSuite.test("IndexValidation.Subscript.Getter.AcrossInstances") {
  // The mutation count may happen to be the same across any two dictionaries.
  // The probability of this is low, but it could happen -- so check a bunch of
  // these cases at once; a trap will definitely occur at least once.
  let dicts = (0 ..< 10).map { _ in getCOWFastDictionary() }
  let indices = dicts.map { $0.index(forKey: 20)! }
  let d = getCOWFastDictionary()

  expectCrashLater()
  for i in indices {
    _ = d[i]
  }
  _fixLifetime(dicts)
}

DictionaryTestSuite.test("IndexValidation.Subscript.Getter.AfterRemoval") {
  var d = getCOWFastDictionary()
  let i = d.index(forKey: 20)!
  expectEqual(d[i], (key: 20, value: 1020))

  d.removeValue(forKey: 10)
  expectCrashLater()
  _ = d[i]
}

DictionaryTestSuite.test("IndexValidation.Subscript.Getter.AfterGrow") {
  var d = getCOWFastDictionary()
  let i = d.index(forKey: 20)!
  let identifier = d._rawIdentifier()
  expectEqual(d[i], (key: 20, value: 1020))
  for i in 0 ..< (d.capacity - d.count) {
    d[100 + i] = 100 + i
  }
  expectEqual(d._rawIdentifier(), identifier)
  expectEqual(d.count, d.capacity)

  expectEqual(d[i], (key: 20, value: 1020))

  d[0] = 0
  expectNotEqual(d._rawIdentifier(), identifier)

  expectCrashLater()
  _ = d[i]
}

DictionaryTestSuite.test("IndexValidation.KeysSubscript.Getter.AfterRemoval") {
  var d = getCOWFastDictionary()
  let i = d.keys.firstIndex(of: 20)!
  expectEqual(d.keys[i], 20)
  expectEqual(d[i], (key: 20, value: 1020))

  d.removeValue(forKey: 10)
  expectCrashLater()
  _ = d.keys[i]
}

DictionaryTestSuite.test("IndexValidation.KeysSubscript.Getter.AfterGrow") {
  var d = getCOWFastDictionary()
  let i = d.keys.firstIndex(of: 20)!
  let identifier = d._rawIdentifier()
  expectEqual(d.keys[i], 20)
  expectEqual(d[i], (key: 20, value: 1020))
  for i in 0 ..< (d.capacity - d.count) {
    d[100 + i] = 100 + i
  }
  expectEqual(d._rawIdentifier(), identifier)
  expectEqual(d.count, d.capacity)

  expectEqual(d.keys[i], 20)
  expectEqual(d[i], (key: 20, value: 1020))

  d[0] = 0
  expectNotEqual(d._rawIdentifier(), identifier)

  expectCrashLater()
  _ = d.keys[i]
}

DictionaryTestSuite.test("IndexValidation.ValuesSubscript.Getter.AfterRemoval") {
  var d = getCOWFastDictionary()
  let i = d.index(forKey: 20)!
  expectEqual(d.values[i], 1020)
  expectEqual(d[i], (key: 20, value: 1020))

  d.removeValue(forKey: 10)
  expectCrashLater()
  _ = d.values[i]
}

DictionaryTestSuite.test("IndexValidation.ValuesSubscript.Getter.AfterGrow") {
  var d = getCOWFastDictionary()
  let i = d.index(forKey: 20)!
  let identifier = d._rawIdentifier()
  expectEqual(d.values[i], 1020)
  expectEqual(d[i], (key: 20, value: 1020))
  for i in 0 ..< (d.capacity - d.count) {
    d[100 + i] = 100 + i
  }
  expectEqual(d._rawIdentifier(), identifier)
  expectEqual(d.count, d.capacity)

  expectEqual(d.values[i], 1020)
  expectEqual(d[i], (key: 20, value: 1020))

  d[0] = 0
  expectNotEqual(d._rawIdentifier(), identifier)

  expectCrashLater()
  _ = d.values[i]
}

DictionaryTestSuite.test("IndexValidation.ValuesSubscript.Setter.AfterRemoval") {
  var d = getCOWFastDictionary()
  let i = d.index(forKey: 20)!
  expectEqual(d.values[i], 1020)
  expectEqual(d[i], (key: 20, value: 1020))

  d.values[i] = 1021
  expectEqual(d.values[i], 1021)
  expectEqual(d[i], (key: 20, value: 1021))

  d.removeValue(forKey: 10)
  expectCrashLater()
  d.values[i] = 1022
}

DictionaryTestSuite.test("IndexValidation.ValuesSubscript.Setter.AfterGrow") {
  var d = getCOWFastDictionary()
  let i = d.index(forKey: 20)!
  let identifier = d._rawIdentifier()
  expectEqual(d.values[i], 1020)
  expectEqual(d[i], (key: 20, value: 1020))

  for i in 0 ..< (d.capacity - d.count) {
    d[100 + i] = 100 + i
  }
  expectEqual(d._rawIdentifier(), identifier)
  expectEqual(d.count, d.capacity)

  d.values[i] = 1021
  expectEqual(d.values[i], 1021)
  expectEqual(d[i], (key: 20, value: 1021))

  d[0] = 0
  expectNotEqual(d._rawIdentifier(), identifier)

  expectCrashLater()
  d.values[i] = 1022
}

DictionaryTestSuite.test("IndexValidation.ValuesSubscript.Modify.AfterRemoval") {
  var d = getCOWFastDictionary()
  let i = d.index(forKey: 20)!
  expectEqual(d.values[i], 1020)
  expectEqual(d[i], (key: 20, value: 1020))

  d.values[i] += 1
  expectEqual(d.values[i], 1021)
  expectEqual(d[i], (key: 20, value: 1021))

  d.removeValue(forKey: 10)
  expectCrashLater()
  d.values[i] += 1
}

DictionaryTestSuite.test("IndexValidation.ValuesSubscript.Modify.AfterGrow") {
  var d = getCOWFastDictionary()
  let i = d.index(forKey: 20)!
  let identifier = d._rawIdentifier()
  expectEqual(d.values[i], 1020)
  expectEqual(d[i], (key: 20, value: 1020))

  for i in 0 ..< (d.capacity - d.count) {
    d[100 + i] = 100 + i
  }
  expectEqual(d._rawIdentifier(), identifier)
  expectEqual(d.count, d.capacity)

  d.values[i] += 1
  expectEqual(d.values[i], 1021)
  expectEqual(d[i], (key: 20, value: 1021))

  d[0] = 0
  expectNotEqual(d._rawIdentifier(), identifier)

  expectCrashLater()
  d.values[i] += 1
}

DictionaryTestSuite.test("IndexValidation.RangeSubscript.AfterRemoval") {
  var d = getCOWFastDictionary()
  let i = d.index(forKey: 20)!
  let j = d.index(after: i)
  expectTrue(i < j)
  d.removeValue(forKey: 10)
  expectTrue(i < j)
  expectCrashLater()
  _ = d[i..<j]
}

DictionaryTestSuite.test("IndexValidation.RangeSubscript.AfterGrow") {
  var d = getCOWFastDictionary()
  let i = d.index(forKey: 20)!
  let j = d.index(after: i)
  expectTrue(i < j)
  let identifier = d._rawIdentifier()
  expectEqual(d[i], (key: 20, value: 1020))
  for i in 0 ..< (d.capacity - d.count) {
    d[100 + i] = 100 + i
  }
  expectEqual(d._rawIdentifier(), identifier)
  expectEqual(d.count, d.capacity)

  expectTrue(i < j)

  d[0] = 0
  expectNotEqual(d._rawIdentifier(), identifier)

  expectTrue(i < j)
  expectCrashLater()
  _ = d[i..<j]
}

DictionaryTestSuite.test("IndexValidation.KeysRangeSubscript.AfterRemoval") {
  var d = getCOWFastDictionary()
  let i = d.keys.firstIndex(of: 20)!
  let j = d.index(after: i)
  expectTrue(i < j)

  d.removeValue(forKey: 10)
  expectTrue(i < j)
  expectCrashLater()
  _ = d.keys[i..<j]
}

DictionaryTestSuite.test("IndexValidation.KeysRangeSubscript.AfterGrow") {
  var d = getCOWFastDictionary()
  let i = d.keys.firstIndex(of: 20)!
  let j = d.index(after: i)
  let identifier = d._rawIdentifier()
  expectTrue(i < j)
  for i in 0 ..< (d.capacity - d.count) {
    d[100 + i] = 100 + i
  }
  expectEqual(d._rawIdentifier(), identifier)
  expectEqual(d.count, d.capacity)

  expectTrue(i < j)

  d[0] = 0
  expectNotEqual(d._rawIdentifier(), identifier)
  expectTrue(i < j)
  expectCrashLater()
  _ = d.keys[i..<j]
}

DictionaryTestSuite.test("IndexValidation.ValuesRangeSubscript.AfterRemoval") {
  var d = getCOWFastDictionary()
  let i = d.index(forKey: 20)!
  let j = d.index(after: i)
  expectTrue(i < j)

  d.removeValue(forKey: 10)
  expectTrue(i < j)
  expectCrashLater()
  _ = d.values[i..<j]
}

DictionaryTestSuite.test("IndexValidation.ValuesRangeSubscript.AfterGrow") {
  var d = getCOWFastDictionary()
  let i = d.index(forKey: 20)!
  let j = d.index(after: i)
  let identifier = d._rawIdentifier()
  expectTrue(i < j)
  for i in 0 ..< (d.capacity - d.count) {
    d[100 + i] = 100 + i
  }
  expectEqual(d._rawIdentifier(), identifier)
  expectEqual(d.count, d.capacity)

  expectTrue(i < j)

  d[0] = 0
  expectNotEqual(d._rawIdentifier(), identifier)
  expectTrue(i < j)
  expectCrashLater()
  _ = d.values[i..<j]
}

DictionaryTestSuite.test("IndexValidation.RemoveAt.AfterRemoval") {
  var d = getCOWFastDictionary()
  let i = d.index(forKey: 20)!
  expectEqual(d[i], (key: 20, value: 1020))

  d.removeValue(forKey: 10)
  expectCrashLater()
  d.remove(at: i)
}

DictionaryTestSuite.test("IndexValidation.RemoveAt.AfterGrow") {
  var d = getCOWFastDictionary()
  let i = d.index(forKey: 20)!
  let identifier = d._rawIdentifier()
  expectEqual(d[i], (key: 20, value: 1020))

  for i in 0 ..< (d.capacity - d.count) {
    d[100 + i] = 100 + i
  }
  expectEqual(d._rawIdentifier(), identifier)
  expectEqual(d.count, d.capacity)

  d[0] = 0
  expectNotEqual(d._rawIdentifier(), identifier)

  expectCrashLater()
  d.remove(at: i)
}

DictionaryTestSuite.test("BulkLoadingInitializer.Unique") {
  for c in [0, 1, 2, 3, 5, 10, 25, 150] {
    let d1 = Dictionary<TestKeyTy, TestEquatableValueTy>(
      _unsafeUninitializedCapacity: c,
      allowingDuplicates: false
    ) { keys, values in
      guard let k = keys.baseAddress, let v = values.baseAddress else {
        return 0
      }

      for i in 0 ..< c {
        (k + i).initialize(to: TestKeyTy(i))
        (v + i).initialize(to: TestEquatableValueTy(i))
      }
      return c
    }

    let d2 = Dictionary(
      uniqueKeysWithValues: (0..<c).map {
        (TestKeyTy($0), TestEquatableValueTy($0))
      })

    for i in 0 ..< c {
      expectEqual(TestEquatableValueTy(i), d1[TestKeyTy(i)])
    }
    expectEqual(d2, d1)
  }
}

DictionaryTestSuite.test("BulkLoadingInitializer.Nonunique") {
  for c in [0, 1, 2, 3, 5, 10, 25, 150] {
    let d1 = Dictionary<TestKeyTy, TestEquatableValueTy>(
      _unsafeUninitializedCapacity: c,
      allowingDuplicates: true
    ) { keys, values in
      guard let k = keys.baseAddress, let v = values.baseAddress else {
        return 0
      }

      for i in 0 ..< c {
        (k + i).initialize(to: TestKeyTy(i / 2))
        (v + i).initialize(to: TestEquatableValueTy(i / 2))
      }
      return c
    }

    let d2 = Dictionary(
      (0 ..< c).map {
        (TestKeyTy($0 / 2), TestEquatableValueTy($0 / 2))
      },
      uniquingKeysWith: { a, b in a })

    expectEqual(d1.count, d2.count)
    for i in 0 ..< c / 2 {
      expectEqual(TestEquatableValueTy(i), d1[TestKeyTy(i)])
    }
    expectEqual(d2, d1)
  }
}

DictionaryTestSuite.setUp {
#if _runtime(_ObjC)
  // Exercise ARC's autoreleased return value optimization in Foundation.
  //
  // On some platforms, when a new process is started, the optimization is
  // expected to fail the first time it is used in each linked
  // dylib. StdlibUnittest takes care of warming up ARC for the stdlib
  // (libswiftCore.dylib), but for this particular test we also need to do it
  // for Foundation, or there will be spurious leaks reported for tests
  // immediately following a crash test.
  //
  // <rdar://problem/42069800> stdlib tests: expectCrashLater() interferes with
  // counting autoreleased live objects
  let d = NSDictionary(objects: [1 as NSNumber], forKeys: [1 as NSNumber])
  _ = d.object(forKey: 1 as NSNumber)
#endif

  resetLeaksOfDictionaryKeysValues()
#if _runtime(_ObjC)
  resetLeaksOfObjCDictionaryKeysValues()
#endif
}

DictionaryTestSuite.tearDown {
  expectNoLeaksOfDictionaryKeysValues()
#if _runtime(_ObjC)
  expectNoLeaksOfObjCDictionaryKeysValues()
#endif
}

runAllTests()

