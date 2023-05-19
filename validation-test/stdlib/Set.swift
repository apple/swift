// RUN: %empty-directory(%t)
//
// RUN: %gyb %s -o %t/main.swift
// RUN: %target-clang -fobjc-arc %S/Inputs/SlurpFastEnumeration/SlurpFastEnumeration.m -c -o %t/SlurpFastEnumeration.o
// RUN: %line-directive %t/main.swift -- %target-build-swift %S/Inputs/DictionaryKeyValueTypes.swift %S/Inputs/DictionaryKeyValueTypesObjC.swift %t/main.swift -I %S/Inputs/SlurpFastEnumeration/ -Xlinker %t/SlurpFastEnumeration.o -o %t/Set -Xfrontend -disable-access-control -swift-version 4.2
//
// RUN: %target-codesign %t/Set && %line-directive %t/main.swift -- %target-run %t/Set
// REQUIRES: executable_test

import StdlibUnittest
import StdlibCollectionUnittest

#if _runtime(_ObjC)
import Foundation
#endif

extension Set {
  func _rawIdentifier() -> Int {
    return unsafeBitCast(self, to: Int.self)
  }
}

// Check that the generic parameter is called 'Element'.
protocol TestProtocol1 {}

extension Set where Element : TestProtocol1 {
  var _elementIsTestProtocol1: Bool {
    fatalError("not implemented")
  }
}

extension SetIndex where Element : TestProtocol1 {
  var _elementIsTestProtocol1: Bool {
    fatalError("not implemented")
  }
}

extension SetIterator where Element : TestProtocol1 {
  var _elementIsTestProtocol1: Bool {
    fatalError("not implemented")
  }
}

let hugeNumberArray = Array(0..<500)

var SetTestSuite = TestSuite("Set")

SetTestSuite.setUp {
  resetLeaksOfDictionaryKeysValues()
#if _runtime(_ObjC)
  resetLeaksOfObjCDictionaryKeysValues()
#endif
}

SetTestSuite.tearDown {
  expectNoLeaksOfDictionaryKeysValues()
#if _runtime(_ObjC)
  expectNoLeaksOfObjCDictionaryKeysValues()
#endif
}

func getCOWFastSet(_ members: [Int] = [1010, 2020, 3030]) -> Set<Int> {
  var s = Set<Int>(minimumCapacity: 10)
  for member in members {
    s.insert(member)
  }
  expectTrue(isNativeSet(s))
  return s
}

func getCOWSlowSet(_ members: [Int] = [1010, 2020, 3030]) -> Set<TestKeyTy> {
  var s = Set<TestKeyTy>(minimumCapacity: 10)
  for member in members {
    s.insert(TestKeyTy(member))
  }
  expectTrue(isNativeSet(s))
  return s
}

func equalsUnordered(_ lhs: Set<Int>, _ rhs: Set<Int>) -> Bool {
  return lhs.sorted().elementsEqual(rhs.sorted()) {
    $0 == $1
  }
}

func isNativeSet<T : Hashable>(_ s: Set<T>) -> Bool {
#if _runtime(_ObjC)
  return s._variant.isNative
#else
  return true
#endif
}

#if _runtime(_ObjC)
func isNativeNSSet(_ s: NSSet) -> Bool {
  let className: NSString = NSStringFromClass(type(of: s)) as NSString
  return [
    "_SwiftDeferredNSSet",
    "_EmptySetSingleton",
    "_SetStorage"
  ].contains {
    className.range(of: $0).length > 0
  }
}

func isCocoaNSSet(_ s: NSSet) -> Bool {
  let className: NSString = NSStringFromClass(type(of: s)) as NSString
  return className.range(of: "NSSet").length > 0 ||
    className.range(of: "NSCFSet").length > 0
}

func getBridgedEmptyNSSet() -> NSSet {
  let s = Set<TestObjCKeyTy>()

  let bridged = convertSetToNSSet(s)
  expectTrue(isNativeNSSet(bridged))

  return bridged
}

func isCocoaSet<T : Hashable>(_ s: Set<T>) -> Bool {
  return !isNativeSet(s)
}

/// Get an NSSet of TestObjCKeyTy values
func getAsNSSet(_ members: [Int] = [1010, 2020, 3030]) -> NSSet {
  let nsArray = NSMutableArray()
  for member in members {
    nsArray.add(TestObjCKeyTy(member))
  }
  return NSMutableSet(array: nsArray as [AnyObject])
}

/// Get an NSMutableSet of TestObjCKeyTy values
func getAsNSMutableSet(_ members: [Int] = [1010, 2020, 3030]) -> NSMutableSet {
  let nsArray = NSMutableArray()
  for member in members {
    nsArray.add(TestObjCKeyTy(member))
  }
  return NSMutableSet(array: nsArray as [AnyObject])
}

public func convertSetToNSSet<T>(_ s: Set<T>) -> NSSet {
  return s._bridgeToObjectiveC()
}

public func convertNSSetToSet<T : Hashable>(_ s: NSSet?) -> Set<T> {
  if _slowPath(s == nil) { return [] }
  var result: Set<T>?
  Set._forceBridgeFromObjectiveC(s!, result: &result)
  return result!
}

/// Get a Set<NSObject> (Set<TestObjCKeyTy>) backed by Cocoa storage
func getBridgedVerbatimSet(_ members: [Int] = [1010, 2020, 3030])
  -> Set<NSObject> {
  let nss = getAsNSSet(members)
  let result: Set<NSObject> = convertNSSetToSet(nss)
  expectTrue(isCocoaSet(result))
  return result
}

/// Get a Set<NSObject> (Set<TestObjCKeyTy>) backed by native storage
func getNativeBridgedVerbatimSet(_ members: [Int] = [1010, 2020, 3030]) ->
  Set<NSObject> {
  let result: Set<NSObject> = Set(members.map({ TestObjCKeyTy($0) }))
  expectTrue(isNativeSet(result))
  return result
}

/// Get a Set<NSObject> (Set<TestObjCKeyTy>) backed by Cocoa storage
func getHugeBridgedVerbatimSet() -> Set<NSObject> {
  let nss = getAsNSSet(hugeNumberArray)
  let result: Set<NSObject> = convertNSSetToSet(nss)
  expectTrue(isCocoaSet(result))
  return result
}

/// Get a Set<TestBridgedKeyTy> backed by native storage
func getBridgedNonverbatimSet(_ members: [Int] = [1010, 2020, 3030]) ->
  Set<TestBridgedKeyTy> {
  let nss = getAsNSSet(members)
  let _ = unsafeBitCast(nss, to: Int.self)
  let result: Set<TestBridgedKeyTy> =
    Swift._forceBridgeFromObjectiveC(nss, Set.self)
  expectTrue(isNativeSet(result))
  return result
}

/// Get a larger Set<TestBridgedKeyTy> backed by native storage
func getHugeBridgedNonverbatimSet() -> Set<TestBridgedKeyTy> {
  let nss = getAsNSSet(hugeNumberArray)
  let _ = unsafeBitCast(nss, to: Int.self)
  let result: Set<TestBridgedKeyTy> =
    Swift._forceBridgeFromObjectiveC(nss, Set.self)
  expectTrue(isNativeSet(result))
  return result
}

func getBridgedVerbatimSetAndNSMutableSet() -> (Set<NSObject>, NSMutableSet) {
  let nss = getAsNSMutableSet()
  return (convertNSSetToSet(nss), nss)
}

func getBridgedNonverbatimSetAndNSMutableSet()
    -> (Set<TestBridgedKeyTy>, NSMutableSet) {
  let nss = getAsNSMutableSet()
  return (Swift._forceBridgeFromObjectiveC(nss, Set.self), nss)
}

func getBridgedNSSetOfRefTypesBridgedVerbatim() -> NSSet {
  expectTrue(_isBridgedVerbatimToObjectiveC(TestObjCKeyTy.self))

  var s = Set<TestObjCKeyTy>(minimumCapacity: 32)
  s.insert(TestObjCKeyTy(1010))
  s.insert(TestObjCKeyTy(2020))
  s.insert(TestObjCKeyTy(3030))

  let bridged = convertSetToNSSet(s)

  expectTrue(isNativeNSSet(bridged))

  return bridged
}

func getBridgedNSSet_ValueTypesCustomBridged(
  numElements: Int = 3
) -> NSSet {
  expectTrue(!_isBridgedVerbatimToObjectiveC(TestBridgedKeyTy.self))

  var s = Set<TestBridgedKeyTy>()
  for i in 1..<(numElements + 1) {
    s.insert(TestBridgedKeyTy(i * 1000 + i * 10))
  }

  let bridged = convertSetToNSSet(s)
  expectTrue(isNativeNSSet(bridged))

  return bridged
}

func getRoundtripBridgedNSSet() -> NSSet {
  let items = NSMutableArray()
  items.add(TestObjCKeyTy(1010))
  items.add(TestObjCKeyTy(2020))
  items.add(TestObjCKeyTy(3030))

  let nss = NSSet(array: items as [AnyObject])

  let s: Set<NSObject> = convertNSSetToSet(nss)

  let bridgedBack = convertSetToNSSet(s)
  expectTrue(isCocoaNSSet(bridgedBack))
  // FIXME: this should be true.
  //expectTrue(unsafeBitCast(nsd, to: Int.self) == unsafeBitCast(bridgedBack, to: Int.self))

  return bridgedBack
}

func getBridgedNSSet_MemberTypesCustomBridged() -> NSSet {
  expectFalse(_isBridgedVerbatimToObjectiveC(TestBridgedKeyTy.self))

  var s = Set<TestBridgedKeyTy>()
  s.insert(TestBridgedKeyTy(1010))
  s.insert(TestBridgedKeyTy(2020))
  s.insert(TestBridgedKeyTy(3030))

  let bridged = convertSetToNSSet(s)
  expectTrue(isNativeNSSet(bridged))

  return bridged
}
#endif // _runtime(_ObjC)

SetTestSuite.test("AssociatedTypes") {
  typealias Collection = Set<MinimalHashableValue>
  expectCollectionAssociatedTypes(
    collectionType: Collection.self,
    iteratorType: SetIterator<MinimalHashableValue>.self,
    subSequenceType: Slice<Collection>.self,
    indexType: SetIndex<MinimalHashableValue>.self,
    indicesType: DefaultIndices<Collection>.self)
}

SetTestSuite.test("sizeof") {
  var s = Set(["Hello", "world"])
#if _pointerBitWidth(_32)
  expectEqual(4, MemoryLayout.size(ofValue: s))
#else
  expectEqual(8, MemoryLayout.size(ofValue: s))
#endif
}

SetTestSuite.test("Index.Hashable") {
  let s: Set = [1, 2, 3, 4, 5]
  let t = Set(s.indices)
  expectEqual(s.count, t.count)
  expectTrue(t.contains(s.startIndex))
}

SetTestSuite.test("COW.Smoke") {
  var s1 = Set<TestKeyTy>(minimumCapacity: 10)
  for i in [1010, 2020, 3030]{ s1.insert(TestKeyTy(i)) }
  let identity1 = s1._rawIdentifier()

  var s2 = s1
  _fixLifetime(s2)
  expectEqual(identity1, s2._rawIdentifier())

  s2.insert(TestKeyTy(4040))
  expectNotEqual(identity1, s2._rawIdentifier())

  s2.insert(TestKeyTy(5050))
  expectEqual(identity1, s1._rawIdentifier())

  // Keep variables alive.
  _fixLifetime(s1)
  _fixLifetime(s2)
}

SetTestSuite.test("COW.Fast.IndexesDontAffectUniquenessCheck") {
  var s = getCOWFastSet()
  let identity1 = s._rawIdentifier()

  let startIndex = s.startIndex
  let endIndex = s.endIndex
  expectNotEqual(startIndex, endIndex)
  expectTrue(startIndex < endIndex)
  expectTrue(startIndex <= endIndex)
  expectFalse(startIndex >= endIndex)
  expectFalse(startIndex > endIndex)

  expectEqual(identity1, s._rawIdentifier())

  s.insert(4040)
  expectEqual(identity1, s._rawIdentifier())

  s.update(with: 5050)
  expectEqual(identity1, s._rawIdentifier())

  // Keep indexes alive during the calls above.
  _fixLifetime(startIndex)
  _fixLifetime(endIndex)
}

SetTestSuite.test("COW.Slow.IndexesDontAffectUniquenessCheck") {
  var s = getCOWSlowSet()
  let identity1 = s._rawIdentifier()

  let startIndex = s.startIndex
  let endIndex = s.endIndex
  expectNotEqual(startIndex, endIndex)
  expectTrue(startIndex < endIndex)
  expectTrue(startIndex <= endIndex)
  expectFalse(startIndex >= endIndex)
  expectFalse(startIndex > endIndex)

  expectEqual(identity1, s._rawIdentifier())
  s.insert(TestKeyTy(4040))
  expectEqual(identity1, s._rawIdentifier())
  s.update(with: TestKeyTy(5050))
  expectEqual(identity1, s._rawIdentifier())

  // Keep indexes alive during the calls above.
  _fixLifetime(startIndex)
  _fixLifetime(endIndex)
}

SetTestSuite.test("COW.Fast.SubscriptWithIndexDoesNotReallocate") {
  let s = getCOWFastSet()
  let identity1 = s._rawIdentifier()

  let startIndex = s.startIndex
  let empty = startIndex == s.endIndex
  expectNotEqual(empty, (s.startIndex < s.endIndex))
  expectTrue(s.startIndex <= s.endIndex)
  expectEqual(empty, (s.startIndex >= s.endIndex))
  expectFalse(s.startIndex > s.endIndex)
  expectEqual(identity1, s._rawIdentifier())

  expectNotEqual(0, s[startIndex])
  expectEqual(identity1, s._rawIdentifier())
}

SetTestSuite.test("COW.Slow.SubscriptWithIndexDoesNotReallocate") {
  let s = getCOWSlowSet()
  let identity1 = s._rawIdentifier()

  let startIndex = s.startIndex
  let empty = startIndex == s.endIndex
  expectNotEqual(empty, (s.startIndex < s.endIndex))
  expectTrue(s.startIndex <= s.endIndex)
  expectEqual(empty, (s.startIndex >= s.endIndex))
  expectFalse(s.startIndex > s.endIndex)
  expectEqual(identity1, s._rawIdentifier())

  expectNotEqual(TestKeyTy(0), s[startIndex])
  expectEqual(identity1, s._rawIdentifier())
}

SetTestSuite.test("COW.Fast.ContainsDoesNotReallocate") {
  let s = getCOWFastSet()
  let identity1 = s._rawIdentifier()

  expectTrue(s.contains(1010))
  expectEqual(identity1, s._rawIdentifier())

  do {
    let s2: Set<MinimalHashableValue> = []
    MinimalHashableValue.timesEqualEqualWasCalled = 0
    MinimalHashableValue.timesHashIntoWasCalled = 0
    expectFalse(s2.contains(MinimalHashableValue(42)))

    // If the set is empty, we shouldn't be computing the hash value of the
    // provided key.
    expectEqual(0, MinimalHashableValue.timesEqualEqualWasCalled)
    expectEqual(0, MinimalHashableValue.timesHashIntoWasCalled)
  }
}

SetTestSuite.test("COW.Slow.ContainsDoesNotReallocate")
  .code {
  var s = getCOWSlowSet()
  let identity1 = s._rawIdentifier()

  expectTrue(s.contains(TestKeyTy(1010)))
  expectEqual(identity1, s._rawIdentifier())

  // Insert a new key-value pair.
  s.insert(TestKeyTy(4040))
  expectEqual(identity1, s._rawIdentifier())
  expectEqual(4, s.count)
  expectTrue(s.contains(TestKeyTy(1010)))
  expectTrue(s.contains(TestKeyTy(2020)))
  expectTrue(s.contains(TestKeyTy(3030)))
  expectTrue(s.contains(TestKeyTy(4040)))

  // Delete an existing key.
  s.remove(TestKeyTy(1010))
  expectEqual(identity1, s._rawIdentifier())
  expectEqual(3, s.count)
  expectTrue(s.contains(TestKeyTy(2020)))
  expectTrue(s.contains(TestKeyTy(3030)))
  expectTrue(s.contains(TestKeyTy(4040)))

  // Try to delete a key that does not exist.
  s.remove(TestKeyTy(777))
  expectEqual(identity1, s._rawIdentifier())
  expectEqual(3, s.count)
  expectTrue(s.contains(TestKeyTy(2020)))
  expectTrue(s.contains(TestKeyTy(3030)))
  expectTrue(s.contains(TestKeyTy(4040)))

  // Replace an existing key.
  s.update(with: TestKeyTy(2020))
  expectEqual(identity1, s._rawIdentifier())
  expectEqual(3, s.count)
  expectTrue(s.contains(TestKeyTy(2020)))
  expectTrue(s.contains(TestKeyTy(3030)))
  expectTrue(s.contains(TestKeyTy(4040)))

  do {
    let s2: Set<MinimalHashableClass> = []
    MinimalHashableClass.timesEqualEqualWasCalled = 0
    MinimalHashableClass.timesHashIntoWasCalled = 0
    expectFalse(s2.contains(MinimalHashableClass(42)))

    // If the set is empty, we shouldn't be computing the hash value of the
    // provided key.
    expectEqual(0, MinimalHashableClass.timesEqualEqualWasCalled)
    expectEqual(0, MinimalHashableClass.timesHashIntoWasCalled)
  }
}

SetTestSuite.test("COW.Fast.InsertDoesNotReallocate") {
  var s1 = getCOWFastSet()

  let identity1 = s1._rawIdentifier()
  let count1 = s1.count

  // Inserting a redundant element should not create new storage
  s1.insert(2020)
  expectEqual(identity1, s1._rawIdentifier())
  expectEqual(count1, s1.count)

  s1.insert(4040)
  s1.insert(5050)
  s1.insert(6060)
  expectEqual(count1 + 3, s1.count)
  expectEqual(identity1, s1._rawIdentifier())
}

SetTestSuite.test("COW.Fast.UpdateWithDoesNotReallocate") {
  var s1 = getCOWFastSet()

  let identity1 = s1._rawIdentifier()
  let count1 = s1.count

  // Updating an existing element should not create new storage
  s1.update(with: 2020)
  expectEqual(identity1, s1._rawIdentifier())
  expectEqual(count1, s1.count)

  // Updating with new elements should not create new storage
  s1.update(with: 4040)
  s1.update(with: 5050)
  s1.update(with: 6060)
  expectEqual(count1 + 3, s1.count)
  expectEqual(identity1, s1._rawIdentifier())
}

SetTestSuite.test("COW.Slow.InsertDoesNotReallocate") {
  do {
    var s1 = getCOWSlowSet()

    let identity1 = s1._rawIdentifier()
    let count1 = s1.count

    // Inserting a redundant element should not create new storage
    s1.insert(TestKeyTy(2020))
    expectEqual(identity1, s1._rawIdentifier())
    expectEqual(count1, s1.count)

    s1.insert(TestKeyTy(4040))
    s1.insert(TestKeyTy(5050))
    s1.insert(TestKeyTy(6060))
    expectEqual(count1 + 3, s1.count)
    expectEqual(identity1, s1._rawIdentifier())
  }

  do {
    let s1 = getCOWSlowSet()
    let identity1 = s1._rawIdentifier()

    var s2 = s1
    expectEqual(identity1, s1._rawIdentifier())
    expectEqual(identity1, s2._rawIdentifier())

    s2.insert(TestKeyTy(2040))
    expectEqual(identity1, s1._rawIdentifier())
    expectNotEqual(identity1, s2._rawIdentifier())

    expectEqual(3, s1.count)
    expectTrue(s1.contains(TestKeyTy(1010)))
    expectTrue(s1.contains(TestKeyTy(2020)))
    expectTrue(s1.contains(TestKeyTy(3030)))
    expectFalse(s1.contains(TestKeyTy(2040)))

    expectEqual(4, s2.count)
    expectTrue(s2.contains(TestKeyTy(1010)))
    expectTrue(s2.contains(TestKeyTy(2020)))
    expectTrue(s2.contains(TestKeyTy(3030)))
    expectTrue(s2.contains(TestKeyTy(2040)))

    // Keep variables alive.
    _fixLifetime(s1)
    _fixLifetime(s2)
  }
}

SetTestSuite.test("COW.Slow.UpdateWithDoesNotReallocate") {
  do {
    var s1 = getCOWSlowSet()
    let identity1 = s1._rawIdentifier()

    // Replace a redundant element.
    s1.update(with: TestKeyTy(2020))
    expectEqual(identity1, s1._rawIdentifier())

    expectEqual(3, s1.count)
    expectTrue(s1.contains(TestKeyTy(1010)))
    expectTrue(s1.contains(TestKeyTy(2020)))
    expectTrue(s1.contains(TestKeyTy(3030)))

    // Update with new elements.
    s1.update(with: TestKeyTy(4040))
    s1.update(with: TestKeyTy(5050))
    s1.update(with: TestKeyTy(6060))

    expectEqual(identity1, s1._rawIdentifier())
    expectEqual(6, s1.count)

    // Keep variables alive.
    _fixLifetime(s1)
  }

  do {
    let s1 = getCOWSlowSet()
    let identity1 = s1._rawIdentifier()

    var s2 = s1
    expectEqual(identity1, s1._rawIdentifier())
    expectEqual(identity1, s2._rawIdentifier())

    // Replace a redundant element.
    s2.update(with: TestKeyTy(2020))
    expectEqual(identity1, s1._rawIdentifier())
    expectNotEqual(identity1, s2._rawIdentifier())

    expectEqual(3, s1.count)
    expectTrue(s1.contains(TestKeyTy(1010)))
    expectTrue(s1.contains(TestKeyTy(2020)))
    expectTrue(s1.contains(TestKeyTy(3030)))

    expectEqual(3, s2.count)
    expectTrue(s2.contains(TestKeyTy(1010)))
    expectTrue(s2.contains(TestKeyTy(2020)))
    expectTrue(s2.contains(TestKeyTy(3030)))

    // Keep variables alive.
    _fixLifetime(s1)
    _fixLifetime(s2)
  }
}

SetTestSuite.test("COW.Fast.IndexForMemberDoesNotReallocate") {
  let s = getCOWFastSet()
  let identity1 = s._rawIdentifier()

  // Find an existing key.
  do {
    let foundIndex1 = s.firstIndex(of: 1010)!
    expectEqual(identity1, s._rawIdentifier())

    let foundIndex2 = s.firstIndex(of: 1010)!
    expectEqual(foundIndex1, foundIndex2)

    expectEqual(1010, s[foundIndex1])
    expectEqual(identity1, s._rawIdentifier())
  }

  // Try to find a key that is not present.
  do {
    let foundIndex1 = s.firstIndex(of: 1111)
    expectNil(foundIndex1)
    expectEqual(identity1, s._rawIdentifier())
  }

  do {
    let s2: Set<MinimalHashableValue> = []
    MinimalHashableValue.timesEqualEqualWasCalled = 0
    MinimalHashableValue.timesHashIntoWasCalled = 0
    expectNil(s2.firstIndex(of: MinimalHashableValue(42)))

    // If the set is empty, we shouldn't be computing the hash value of the
    // provided key.
    expectEqual(0, MinimalHashableValue.timesEqualEqualWasCalled)
    expectEqual(0, MinimalHashableValue.timesHashIntoWasCalled)
  }
}

SetTestSuite.test("COW.Slow.IndexForMemberDoesNotReallocate") {
  let s = getCOWSlowSet()
  let identity1 = s._rawIdentifier()

  // Find an existing key.
  do {
    let foundIndex1 = s.firstIndex(of: TestKeyTy(1010))!
    expectEqual(identity1, s._rawIdentifier())

    let foundIndex2 = s.firstIndex(of: TestKeyTy(1010))!
    expectEqual(foundIndex1, foundIndex2)

    expectEqual(TestKeyTy(1010), s[foundIndex1])
    expectEqual(identity1, s._rawIdentifier())
  }

  // Try to find a key that is not present.
  do {
    let foundIndex1 = s.firstIndex(of: TestKeyTy(1111))
    expectNil(foundIndex1)
    expectEqual(identity1, s._rawIdentifier())
  }

  do {
    let s2: Set<MinimalHashableClass> = []
    MinimalHashableClass.timesEqualEqualWasCalled = 0
    MinimalHashableClass.timesHashIntoWasCalled = 0
    expectNil(s2.firstIndex(of: MinimalHashableClass(42)))

    // If the set is empty, we shouldn't be computing the hash value of the
    // provided key.
    expectEqual(0, MinimalHashableClass.timesEqualEqualWasCalled)
    expectEqual(0, MinimalHashableClass.timesHashIntoWasCalled)
  }
}

SetTestSuite.test("COW.Fast.RemoveAtDoesNotReallocate")
  .code {
  do {
    var s = getCOWFastSet()
    let identity1 = s._rawIdentifier()

    let foundIndex1 = s.firstIndex(of: 1010)!
    expectEqual(identity1, s._rawIdentifier())

    expectEqual(1010, s[foundIndex1])

    let removed = s.remove(at: foundIndex1)
    expectEqual(1010, removed)

    expectEqual(identity1, s._rawIdentifier())
    expectNil(s.firstIndex(of: 1010))
  }

  do {
    let s1 = getCOWFastSet()
    let identity1 = s1._rawIdentifier()

    var s2 = s1
    expectEqual(identity1, s1._rawIdentifier())
    expectEqual(identity1, s2._rawIdentifier())

    let foundIndex1 = s2.firstIndex(of: 1010)!
    expectEqual(1010, s2[foundIndex1])
    expectEqual(identity1, s1._rawIdentifier())
    expectEqual(identity1, s2._rawIdentifier())

    let removed = s2.remove(at: foundIndex1)
    expectEqual(1010, removed)

    expectEqual(identity1, s1._rawIdentifier())
    expectNotEqual(identity1, s2._rawIdentifier())
    expectNil(s2.firstIndex(of: 1010))
  }
}

SetTestSuite.test("COW.Slow.RemoveAtDoesNotReallocate")
  .code {
  do {
    var s = getCOWSlowSet()
    let identity1 = s._rawIdentifier()

    let foundIndex1 = s.firstIndex(of: TestKeyTy(1010))!
    expectEqual(identity1, s._rawIdentifier())

    expectEqual(TestKeyTy(1010), s[foundIndex1])

    let removed = s.remove(at: foundIndex1)
    expectEqual(TestKeyTy(1010), removed)

    expectEqual(identity1, s._rawIdentifier())
    expectNil(s.firstIndex(of: TestKeyTy(1010)))
  }

  do {
    let s1 = getCOWSlowSet()
    let identity1 = s1._rawIdentifier()

    var s2 = s1
    expectEqual(identity1, s1._rawIdentifier())
    expectEqual(identity1, s2._rawIdentifier())

    let foundIndex1 = s2.firstIndex(of: TestKeyTy(1010))!
    expectEqual(TestKeyTy(1010), s2[foundIndex1])
    expectEqual(identity1, s1._rawIdentifier())
    expectEqual(identity1, s2._rawIdentifier())

    let removed = s2.remove(at: foundIndex1)
    expectEqual(TestKeyTy(1010), removed)

    expectEqual(identity1, s1._rawIdentifier())
    expectNotEqual(identity1, s2._rawIdentifier())
    expectNil(s2.firstIndex(of: TestKeyTy(1010)))
  }
}

SetTestSuite.test("COW.Fast.RemoveDoesNotReallocate")
  .code {
  do {
    var s1 = getCOWFastSet()
    let identity1 = s1._rawIdentifier()

    var deleted = s1.remove(0)
    expectNil(deleted)
    expectEqual(identity1, s1._rawIdentifier())

    deleted = s1.remove(1010)
    expectEqual(1010, deleted)
    expectEqual(identity1, s1._rawIdentifier())

    // Keep variables alive.
    _fixLifetime(s1)
  }

  do {
    let s1 = getCOWFastSet()
    let identity1 = s1._rawIdentifier()

    var s2 = s1
    var deleted = s2.remove(0)
    expectNil(deleted)
    expectEqual(identity1, s1._rawIdentifier())
    expectEqual(identity1, s2._rawIdentifier())

    deleted = s2.remove(1010)
    expectEqual(1010, deleted)
    expectEqual(identity1, s1._rawIdentifier())
    expectNotEqual(identity1, s2._rawIdentifier())

    // Keep variables alive.
    _fixLifetime(s1)
    _fixLifetime(s2)
  }
}

SetTestSuite.test("COW.Slow.RemoveDoesNotReallocate")
  .code {
  do {
    var s1 = getCOWSlowSet()
    let identity1 = s1._rawIdentifier()

    var deleted = s1.remove(TestKeyTy(0))
    expectNil(deleted)
    expectEqual(identity1, s1._rawIdentifier())

    deleted = s1.remove(TestKeyTy(1010))
    expectEqual(TestKeyTy(1010), deleted)
    expectEqual(identity1, s1._rawIdentifier())

    // Keep variables alive.
    _fixLifetime(s1)
  }

  do {
    let s1 = getCOWSlowSet()
    let identity1 = s1._rawIdentifier()

    var s2 = s1
    var deleted = s2.remove(TestKeyTy(0))
    expectNil(deleted)
    expectEqual(identity1, s1._rawIdentifier())
    expectEqual(identity1, s2._rawIdentifier())

    deleted = s2.remove(TestKeyTy(1010))
    expectEqual(TestKeyTy(1010), deleted)
    expectEqual(identity1, s1._rawIdentifier())
    expectNotEqual(identity1, s2._rawIdentifier())

    // Keep variables alive.
    _fixLifetime(s1)
    _fixLifetime(s2)
  }
}

SetTestSuite.test("COW.Fast.UnionInPlaceSmallSetDoesNotReallocate") {
  var s1 = getCOWFastSet()
  let s2 = Set([4040, 5050, 6060])
  let s3 = Set([1010, 2020, 3030, 4040, 5050, 6060])

  let identity1 = s1._rawIdentifier()

  // Adding the empty set should obviously not allocate
  s1.formUnion(Set<Int>())
  expectEqual(identity1, s1._rawIdentifier())

  // adding a small set shouldn't cause a reallocation
  s1.formUnion(s2)
  expectEqual(s1, s3)
  expectEqual(identity1, s1._rawIdentifier())
}

SetTestSuite.test("COW.Fast.RemoveAllDoesNotReallocate") {
  do {
    var s = getCOWFastSet()
    let originalCapacity = s.capacity
    expectEqual(3, s.count)
    expectTrue(s.contains(1010))

    s.removeAll()
    // We cannot expectTrue that identity changed, since the new buffer of
    // smaller size can be allocated at the same address as the old one.
    let identity1 = s._rawIdentifier()
    expectTrue(s.capacity < originalCapacity)
    expectEqual(0, s.count)
    expectFalse(s.contains(1010))

    s.removeAll()
    expectEqual(identity1, s._rawIdentifier())
    expectEqual(0, s.count)
    expectFalse(s.contains(1010))
  }

  do {
    var s = getCOWFastSet()
    let identity1 = s._rawIdentifier()
    let originalCapacity = s.capacity
    expectEqual(3, s.count)
    expectTrue(s.contains(1010))

    s.removeAll(keepingCapacity: true)
    expectEqual(identity1, s._rawIdentifier())
    expectEqual(originalCapacity, s.capacity)
    expectEqual(0, s.count)
    expectFalse(s.contains(1010))

    s.removeAll(keepingCapacity: true)
    expectEqual(identity1, s._rawIdentifier())
    expectEqual(originalCapacity, s.capacity)
    expectEqual(0, s.count)
    expectFalse(s.contains(1010))
  }

  do {
    let s1 = getCOWFastSet()
    let identity1 = s1._rawIdentifier()
    expectEqual(3, s1.count)
    expectTrue(s1.contains(1010))

    var s2 = s1
    s2.removeAll()
    let identity2 = s2._rawIdentifier()
    expectEqual(identity1, s1._rawIdentifier())
    expectNotEqual(identity1, identity2)
    expectEqual(3, s1.count)
    expectTrue(s1.contains(1010))
    expectEqual(0, s2.count)
    expectFalse(s2.contains(1010))

    // Keep variables alive.
    _fixLifetime(s1)
    _fixLifetime(s2)
  }

  do {
    let s1 = getCOWFastSet()
    let identity1 = s1._rawIdentifier()
    let originalCapacity = s1.capacity
    expectEqual(3, s1.count)
    expectTrue(s1.contains(1010))

    var s2 = s1
    s2.removeAll(keepingCapacity: true)
    let identity2 = s2._rawIdentifier()
    expectEqual(identity1, s1._rawIdentifier())
    expectNotEqual(identity1, identity2)
    expectEqual(3, s1.count)
    expectTrue(s1.contains(1010))
    expectEqual(originalCapacity, s2.capacity)
    expectEqual(0, s2.count)
    expectFalse(s2.contains(1010))

    // Keep variables alive.
    _fixLifetime(s1)
    _fixLifetime(s2)
  }
}

SetTestSuite.test("COW.Slow.RemoveAllDoesNotReallocate") {
  do {
    var s = getCOWSlowSet()
    let originalCapacity = s.capacity
    expectEqual(3, s.count)
    expectTrue(s.contains(TestKeyTy(1010)))

    s.removeAll()
    // We cannot expectTrue that identity changed, since the new buffer of
    // smaller size can be allocated at the same address as the old one.
    let identity1 = s._rawIdentifier()
    expectTrue(s.capacity < originalCapacity)
    expectEqual(0, s.count)
    expectFalse(s.contains(TestKeyTy(1010)))

    s.removeAll()
    expectEqual(identity1, s._rawIdentifier())
    expectEqual(0, s.count)
    expectFalse(s.contains(TestKeyTy(1010)))
  }

  do {
    var s = getCOWSlowSet()
    let identity1 = s._rawIdentifier()
    let originalCapacity = s.capacity
    expectEqual(3, s.count)
    expectTrue(s.contains(TestKeyTy(1010)))

    s.removeAll(keepingCapacity: true)
    expectEqual(identity1, s._rawIdentifier())
    expectEqual(originalCapacity, s.capacity)
    expectEqual(0, s.count)
    expectFalse(s.contains(TestKeyTy(1010)))

    s.removeAll(keepingCapacity: true)
    expectEqual(identity1, s._rawIdentifier())
    expectEqual(originalCapacity, s.capacity)
    expectEqual(0, s.count)
    expectFalse(s.contains(TestKeyTy(1010)))
  }

  do {
    let s1 = getCOWSlowSet()
    let identity1 = s1._rawIdentifier()
    expectEqual(3, s1.count)
    expectTrue(s1.contains(TestKeyTy(1010)))

    var s2 = s1
    s2.removeAll()
    let identity2 = s2._rawIdentifier()
    expectEqual(identity1, s1._rawIdentifier())
    expectNotEqual(identity1, identity2)
    expectEqual(3, s1.count)
    expectTrue(s1.contains(TestKeyTy(1010)))
    expectEqual(0, s2.count)
    expectFalse(s2.contains(TestKeyTy(1010)))

    // Keep variables alive.
    _fixLifetime(s1)
    _fixLifetime(s2)
  }

  do {
    let s1 = getCOWSlowSet()
    let identity1 = s1._rawIdentifier()
    let originalCapacity = s1.capacity
    expectEqual(3, s1.count)
    expectTrue(s1.contains(TestKeyTy(1010)))

    var s2 = s1
    s2.removeAll(keepingCapacity: true)
    let identity2 = s2._rawIdentifier()
    expectEqual(identity1, s1._rawIdentifier())
    expectNotEqual(identity1, identity2)
    expectEqual(3, s1.count)
    expectTrue(s1.contains(TestKeyTy(1010)))
    expectEqual(originalCapacity, s2.capacity)
    expectEqual(0, s2.count)
    expectFalse(s2.contains(TestKeyTy(1010)))

    // Keep variables alive.
    _fixLifetime(s1)
    _fixLifetime(s2)
  }
}

SetTestSuite.test("COW.Fast.FirstDoesNotReallocate") {
  let s = getCOWFastSet()
  let identity1 = s._rawIdentifier()

  expectNotNil(s.first)
  expectEqual(identity1, s._rawIdentifier())
}

SetTestSuite.test("COW.Fast.CountDoesNotReallocate") {
  let s = getCOWFastSet()
  let identity1 = s._rawIdentifier()

  expectEqual(3, s.count)
  expectEqual(identity1, s._rawIdentifier())
}

SetTestSuite.test("COW.Slow.FirstDoesNotReallocate") {
  let s = getCOWSlowSet()
  let identity1 = s._rawIdentifier()

  expectNotNil(s.first)
  expectEqual(identity1, s._rawIdentifier())
}

SetTestSuite.test("COW.Slow.CountDoesNotReallocate") {
  let s = getCOWSlowSet()
  let identity1 = s._rawIdentifier()

  expectEqual(3, s.count)
  expectEqual(identity1, s._rawIdentifier())
}

SetTestSuite.test("COW.Fast.GenerateDoesNotReallocate") {
  let s = getCOWFastSet()
  let identity1 = s._rawIdentifier()

  var iter = s.makeIterator()
  var items: [Int] = []
  while let value = iter.next() {
    items += [value]
  }
  expectTrue(equalsUnordered(items, [ 1010, 2020, 3030 ]))
  expectEqual(identity1, s._rawIdentifier())
}

SetTestSuite.test("COW.Slow.GenerateDoesNotReallocate") {
  let s = getCOWSlowSet()
  let identity1 = s._rawIdentifier()

  var iter = s.makeIterator()
  var items: [Int] = []
  while let value = iter.next() {
    items.append(value.value)
  }
  expectTrue(equalsUnordered(items, [ 1010, 2020, 3030 ]))
  expectEqual(identity1, s._rawIdentifier())
}

SetTestSuite.test("COW.Fast.EqualityTestDoesNotReallocate") {
  let s1 = getCOWFastSet()
  let identity1 = s1._rawIdentifier()

  let s2 = getCOWFastSet()
  let identity2 = s2._rawIdentifier()

  expectEqual(s1, s2)
  expectEqual(identity1, s1._rawIdentifier())
  expectEqual(identity2, s2._rawIdentifier())
}

SetTestSuite.test("COW.Slow.EqualityTestDoesNotReallocate") {
  let s1 = getCOWFastSet()
  let identity1 = s1._rawIdentifier()

  let s2 = getCOWFastSet()
  let identity2 = s2._rawIdentifier()

  expectEqual(s1, s2)
  expectEqual(identity1, s1._rawIdentifier())
  expectEqual(identity2, s2._rawIdentifier())
}

//===---
// Native set tests.
//===---

func helperDeleteThree(
  _ k1: RawTestKeyTy,
  _ k2: RawTestKeyTy,
  _ k3: RawTestKeyTy
) {
  var s1 = Set<RawTestKeyTy>(minimumCapacity: 10)

  s1.insert(k1)
  s1.insert(k2)
  s1.insert(k3)

  expectTrue(s1.contains(k1))
  expectTrue(s1.contains(k2))
  expectTrue(s1.contains(k3))

  s1.remove(k1)
  expectFalse(s1.contains(k1))
  expectTrue(s1.contains(k2))
  expectTrue(s1.contains(k3))

  s1.remove(k2)
  expectFalse(s1.contains(k1))
  expectFalse(s1.contains(k2))
  expectTrue(s1.contains(k3))

  s1.remove(k3)
  expectFalse(s1.contains(k1))
  expectFalse(s1.contains(k2))
  expectFalse(s1.contains(k3))

  expectEqual(0, s1.count)
}

SetTestSuite.test("deleteChainCollision") {
  let k1 = RawTestKeyTy(value: 1010, hashValue: 0)
  let k2 = RawTestKeyTy(value: 2020, hashValue: 0)
  let k3 = RawTestKeyTy(value: 3030, hashValue: 0)

  helperDeleteThree(k1, k2, k3)
}

SetTestSuite.test("deleteChainNoCollision") {
  let k1 = RawTestKeyTy(value: 1010, hashValue: 0)
  let k2 = RawTestKeyTy(value: 2020, hashValue: 1)
  let k3 = RawTestKeyTy(value: 3030, hashValue: 2)

  helperDeleteThree(k1, k2, k3)
}

SetTestSuite.test("deleteChainCollision2") {
  let k1_0 = RawTestKeyTy(value: 1010, hashValue: 0)
  let k2_0 = RawTestKeyTy(value: 2020, hashValue: 0)
  let k3_2 = RawTestKeyTy(value: 3030, hashValue: 2)
  let k4_0 = RawTestKeyTy(value: 4040, hashValue: 0)
  let k5_2 = RawTestKeyTy(value: 5050, hashValue: 2)
  let k6_0 = RawTestKeyTy(value: 6060, hashValue: 0)

  var s = Set<RawTestKeyTy>(minimumCapacity: 10)

  s.insert(k1_0) // in bucket 0
  s.insert(k2_0) // in bucket 1
  s.insert(k3_2) // in bucket 2
  s.insert(k4_0) // in bucket 3
  s.insert(k5_2) // in bucket 4
  s.insert(k6_0) // in bucket 5

  s.remove(k3_2)

  expectTrue(s.contains(k1_0))
  expectTrue(s.contains(k2_0))
  expectFalse(s.contains(k3_2))
  expectTrue(s.contains(k4_0))
  expectTrue(s.contains(k5_2))
  expectTrue(s.contains(k6_0))
}

SetTestSuite.test("deleteChainCollisionRandomized") {
  let seed = UInt64.random(in: .min ... .max)
  var generator = LinearCongruentialGenerator(seed: seed)
  print("using LinearCongruentialGenerator(seed: \(seed))")

  func check(_ s: Set<RawTestKeyTy>) {
    let keys = Array(s)
    for i in 0..<keys.count {
      for j in 0..<i {
        expectNotEqual(keys[i], keys[j])
      }
    }

    for k in keys {
      expectTrue(s.contains(k))
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
    let hashValue = Int.random(
      in: 0 ..< (chainLength - chainOverlap),
      using: &generator) * collisionChains
    let k = RawTestKeyTy(value: value, hashValue: hashValue)
    knownKeys += [k]
    return k
  }

  var s = Set<RawTestKeyTy>(minimumCapacity: 30)
  for _ in 1..<300 {
    let value = Int.random(
      in: 0 ..< (collisionChains * chainLength),
      using: &generator)
    let key = getKey(value)
    if Int.random(in: 0 ..< (chainLength * 2), using: &generator) == 0 {
      s.remove(key)
    } else {
      s.insert(key)
    }
    check(s)
  }
}

#if _runtime(_ObjC)
@objc
class CustomImmutableNSSet : NSSet {
  init(_privateInit: ()) {
    super.init()
  }

  override init() {
    expectUnreachable()
    super.init()
  }

  override init(objects: UnsafePointer<AnyObject>?, count: Int) {
    expectUnreachable()
    super.init(objects: objects, count: count)
  }

  required init(coder aDecoder: NSCoder) {
    fatalError("init(coder:) not implemented by CustomImmutableNSSet")
  }

  @objc(copyWithZone:)
  override func copy(with zone: NSZone?) -> Any {
    CustomImmutableNSSet.timesCopyWithZoneWasCalled += 1
    return self
  }

  override func member(_ object: Any) -> Any? {
    CustomImmutableNSSet.timesMemberWasCalled += 1
    return getAsNSSet([ 1010, 1020, 1030 ]).member(object)
  }

  override func objectEnumerator() -> NSEnumerator {
    CustomImmutableNSSet.timesObjectEnumeratorWasCalled += 1
    return getAsNSSet([ 1010, 1020, 1030 ]).objectEnumerator()
  }

  override var count: Int {
    CustomImmutableNSSet.timesCountWasCalled += 1
    return 3
  }

  static var timesCopyWithZoneWasCalled = 0
  static var timesMemberWasCalled = 0
  static var timesObjectEnumeratorWasCalled = 0
  static var timesCountWasCalled = 0
}

SetTestSuite.test("BridgedFromObjC.Verbatim.SetIsCopied") {
  let (s, nss) = getBridgedVerbatimSetAndNSMutableSet()
  expectTrue(isCocoaSet(s))

  expectTrue(s.contains(TestObjCKeyTy(1010)))
  expectNotNil(nss.member(TestObjCKeyTy(1010)))

  nss.remove(TestObjCKeyTy(1010))
  expectNil(nss.member(TestObjCKeyTy(1010)))

  expectTrue(s.contains(TestObjCKeyTy(1010)))
}

SetTestSuite.test("BridgedFromObjC.Nonverbatim.SetIsCopied") {
  let (s, nss) = getBridgedNonverbatimSetAndNSMutableSet()
  expectTrue(isNativeSet(s))

  expectTrue(s.contains(TestBridgedKeyTy(1010)))
  expectNotNil(nss.member(TestBridgedKeyTy(1010) as AnyObject))

  nss.remove(TestBridgedKeyTy(1010) as AnyObject)
  expectNil(nss.member(TestBridgedKeyTy(1010) as AnyObject))

  expectTrue(s.contains(TestBridgedKeyTy(1010)))
}


SetTestSuite.test("BridgedFromObjC.Verbatim.NSSetIsRetained") {
  let nss: NSSet = NSSet(set: getAsNSSet([ 1010, 1020, 1030 ]))

  let s: Set<NSObject> = convertNSSetToSet(nss)

  let bridgedBack: NSSet = convertSetToNSSet(s)

  expectEqual(
    unsafeBitCast(nss, to: Int.self),
    unsafeBitCast(bridgedBack, to: Int.self))

  _fixLifetime(nss)
  _fixLifetime(s)
  _fixLifetime(bridgedBack)
}

SetTestSuite.test("BridgedFromObjC.Nonverbatim.NSSetIsCopied") {
  let nss: NSSet = NSSet(set: getAsNSSet([ 1010, 1020, 1030 ]))

  let s: Set<TestBridgedKeyTy> = convertNSSetToSet(nss)

  let bridgedBack: NSSet = convertSetToNSSet(s)

  expectNotEqual(
    unsafeBitCast(nss, to: Int.self),
    unsafeBitCast(bridgedBack, to: Int.self))

  _fixLifetime(nss)
  _fixLifetime(s)
  _fixLifetime(bridgedBack)
}


SetTestSuite.test("BridgedFromObjC.Verbatim.ImmutableSetIsRetained") {
  let nss: NSSet = CustomImmutableNSSet(_privateInit: ())

  CustomImmutableNSSet.timesCopyWithZoneWasCalled = 0
  CustomImmutableNSSet.timesMemberWasCalled = 0
  CustomImmutableNSSet.timesObjectEnumeratorWasCalled = 0
  CustomImmutableNSSet.timesCountWasCalled = 0
  let s: Set<NSObject> = convertNSSetToSet(nss)
  expectEqual(1, CustomImmutableNSSet.timesCopyWithZoneWasCalled)
  expectEqual(0, CustomImmutableNSSet.timesMemberWasCalled)
  expectEqual(0, CustomImmutableNSSet.timesObjectEnumeratorWasCalled)
  expectEqual(0, CustomImmutableNSSet.timesCountWasCalled)

  let bridgedBack: NSSet = convertSetToNSSet(s)
  expectEqual(
    unsafeBitCast(nss, to: Int.self),
    unsafeBitCast(bridgedBack, to: Int.self))

  _fixLifetime(nss)
  _fixLifetime(s)
  _fixLifetime(bridgedBack)
}

SetTestSuite.test("BridgedFromObjC.Nonverbatim.ImmutableSetIsCopied") {
  let nss: NSSet = CustomImmutableNSSet(_privateInit: ())

  CustomImmutableNSSet.timesCopyWithZoneWasCalled = 0
  CustomImmutableNSSet.timesMemberWasCalled = 0
  CustomImmutableNSSet.timesObjectEnumeratorWasCalled = 0
  CustomImmutableNSSet.timesCountWasCalled = 0
  let s: Set<TestBridgedKeyTy> = convertNSSetToSet(nss)
  expectEqual(0, CustomImmutableNSSet.timesCopyWithZoneWasCalled)
  expectEqual(0, CustomImmutableNSSet.timesMemberWasCalled)
  expectEqual(1, CustomImmutableNSSet.timesObjectEnumeratorWasCalled)
  expectNotEqual(0, CustomImmutableNSSet.timesCountWasCalled)

  let bridgedBack: NSSet = convertSetToNSSet(s)
  expectNotEqual(
    unsafeBitCast(nss, to: Int.self),
    unsafeBitCast(bridgedBack, to: Int.self))

  _fixLifetime(nss)
  _fixLifetime(s)
  _fixLifetime(bridgedBack)
}


SetTestSuite.test("BridgedFromObjC.Verbatim.IndexForMember") {
  let s = getBridgedVerbatimSet()
  let identity1 = s._rawIdentifier()
  expectTrue(isCocoaSet(s))

  // Find an existing key.
  var member = s[s.firstIndex(of: TestObjCKeyTy(1010))!]
  expectEqual(TestObjCKeyTy(1010), member)

  member = s[s.firstIndex(of: TestObjCKeyTy(2020))!]
  expectEqual(TestObjCKeyTy(2020), member)

  member = s[s.firstIndex(of: TestObjCKeyTy(3030))!]
  expectEqual(TestObjCKeyTy(3030), member)

  // Try to find a key that does not exist.
  expectNil(s.firstIndex(of: TestObjCKeyTy(4040)))
  expectEqual(identity1, s._rawIdentifier())
}

SetTestSuite.test("BridgedFromObjC.Nonverbatim.IndexForMember") {
  let s = getBridgedNonverbatimSet()
  let identity1 = s._rawIdentifier()

  do {
    var member = s[s.firstIndex(of: TestBridgedKeyTy(1010))!]
    expectEqual(TestBridgedKeyTy(1010), member)

    member = s[s.firstIndex(of: TestBridgedKeyTy(2020))!]
    expectEqual(TestBridgedKeyTy(2020), member)

    member = s[s.firstIndex(of: TestBridgedKeyTy(3030))!]
    expectEqual(TestBridgedKeyTy(3030), member)
  }

  expectNil(s.firstIndex(of: TestBridgedKeyTy(4040)))
  expectEqual(identity1, s._rawIdentifier())
}

SetTestSuite.test("BridgedFromObjC.Verbatim.Insert") {
  do {
    var s = getBridgedVerbatimSet()
    let identity1 = s._rawIdentifier()
    expectTrue(isCocoaSet(s))

    expectFalse(s.contains(TestObjCKeyTy(2040)))
    s.insert(TestObjCKeyTy(2040))

    let identity2 = s._rawIdentifier()
    expectNotEqual(identity1, identity2)
    expectTrue(isNativeSet(s))
    expectEqual(4, s.count)

    expectTrue(s.contains(TestObjCKeyTy(1010)))
    expectTrue(s.contains(TestObjCKeyTy(2020)))
    expectTrue(s.contains(TestObjCKeyTy(3030)))
    expectTrue(s.contains(TestObjCKeyTy(2040)))
  }
}

SetTestSuite.test("BridgedFromObjC.Verbatim.UpdateWith") {
  do {
    var s = getBridgedVerbatimSet()
    let identity1 = s._rawIdentifier()
    expectTrue(isCocoaSet(s))

    expectFalse(s.contains(TestObjCKeyTy(2040)))
    s.update(with: TestObjCKeyTy(2040))

    let identity2 = s._rawIdentifier()
    expectNotEqual(identity1, identity2)
    expectTrue(isNativeSet(s))
    expectEqual(4, s.count)

    expectTrue(s.contains(TestObjCKeyTy(1010)))
    expectTrue(s.contains(TestObjCKeyTy(2020)))
    expectTrue(s.contains(TestObjCKeyTy(3030)))
    expectTrue(s.contains(TestObjCKeyTy(2040)))
  }
}

SetTestSuite.test("BridgedFromObjC.Nonverbatim.Insert") {
  do {
    var s = getBridgedNonverbatimSet()
    let identity1 = s._rawIdentifier()
    let count1 = s.count
    let capacity1 = s.capacity
    expectTrue(isNativeSet(s))

    expectFalse(s.contains(TestBridgedKeyTy(2040)))
    s.insert(TestObjCKeyTy(2040) as TestBridgedKeyTy)

    let identity2 = s._rawIdentifier()
    // Storage identity may or may not change depending on allocation behavior.
    // (s is eagerly bridged to a regular uniquely referenced native Set.)
    expectTrue((count1 < capacity1) == (identity1 == identity2))

    expectTrue(isNativeSet(s))
    expectEqual(4, s.count)

    expectTrue(s.contains(TestBridgedKeyTy(1010)))
    expectTrue(s.contains(TestBridgedKeyTy(2020)))
    expectTrue(s.contains(TestBridgedKeyTy(3030)))
    expectTrue(s.contains(TestBridgedKeyTy(2040)))
  }
}

SetTestSuite.test("BridgedFromObjC.Nonverbatim.UpdateWith") {
  do {
    var s = getBridgedNonverbatimSet()
    let identity1 = s._rawIdentifier()
    let count1 = s.count
    let capacity1 = s.capacity
    expectTrue(isNativeSet(s))

    expectFalse(s.contains(TestBridgedKeyTy(2040)))
    s.update(with: TestObjCKeyTy(2040) as TestBridgedKeyTy)

    let identity2 = s._rawIdentifier()
    // Storage identity may or may not change depending on allocation behavior.
    // (s is eagerly bridged to a regular uniquely referenced native Set.)
    expectTrue((count1 < capacity1) == (identity1 == identity2))

    expectTrue(isNativeSet(s))
    expectEqual(4, s.count)

    expectTrue(s.contains(TestBridgedKeyTy(1010)))
    expectTrue(s.contains(TestBridgedKeyTy(2020)))
    expectTrue(s.contains(TestBridgedKeyTy(3030)))
    expectTrue(s.contains(TestBridgedKeyTy(2040)))
  }
}

SetTestSuite.test("BridgedFromObjC.Verbatim.SubscriptWithIndex") {
  let s = getBridgedVerbatimSet()
  let identity1 = s._rawIdentifier()
  expectTrue(isCocoaSet(s))

  let startIndex = s.startIndex
  let endIndex = s.endIndex
  expectNotEqual(startIndex, endIndex)
  expectTrue(startIndex < endIndex)
  expectTrue(startIndex <= endIndex)
  expectFalse(startIndex >= endIndex)
  expectFalse(startIndex > endIndex)
  expectEqual(identity1, s._rawIdentifier())

  var members = [Int]()
  for i in s.indices {
    let foundMember: AnyObject = s[i]
    let member = foundMember as! TestObjCKeyTy
    members.append(member.value)
  }
  expectTrue(equalsUnordered(members, [1010, 2020, 3030]))
  expectEqual(identity1, s._rawIdentifier())

  // Keep indexes alive during the calls above.
  _fixLifetime(startIndex)
  _fixLifetime(endIndex)
}

SetTestSuite.test("BridgedFromObjC.Nonverbatim.SubscriptWithIndex") {
  let s = getBridgedNonverbatimSet()
  let identity1 = s._rawIdentifier()
  expectTrue(isNativeSet(s))

  let startIndex = s.startIndex
  let endIndex = s.endIndex
  expectNotEqual(startIndex, endIndex)
  expectTrue(startIndex < endIndex)
  expectTrue(startIndex <= endIndex)
  expectFalse(startIndex >= endIndex)
  expectFalse(startIndex > endIndex)
  expectEqual(identity1, s._rawIdentifier())

  var members = [Int]()
  for i in s.indices {
    let foundMember: AnyObject = s[i] as NSObject
    let member = foundMember as! TestObjCKeyTy
    members.append(member.value)
  }
  expectTrue(equalsUnordered(members, [1010, 2020, 3030]))
  expectEqual(identity1, s._rawIdentifier())

  // Keep indexes alive during the calls above.
  _fixLifetime(startIndex)
  _fixLifetime(endIndex)
}

SetTestSuite.test("BridgedFromObjC.Verbatim.SubscriptWithIndex_Empty") {
  let s = getBridgedVerbatimSet([])
  let identity1 = s._rawIdentifier()
  expectTrue(isCocoaSet(s))

  let startIndex = s.startIndex
  let endIndex = s.endIndex
  expectEqual(startIndex, endIndex)
  expectFalse(startIndex < endIndex)
  expectTrue(startIndex <= endIndex)
  expectTrue(startIndex >= endIndex)
  expectFalse(startIndex > endIndex)
  expectEqual(identity1, s._rawIdentifier())

  // Keep indexes alive during the calls above.
  _fixLifetime(startIndex)
  _fixLifetime(endIndex)
}

SetTestSuite.test("BridgedFromObjC.Nonverbatim.SubscriptWithIndex_Empty") {
  let s = getBridgedNonverbatimSet([])
  let identity1 = s._rawIdentifier()
  expectTrue(isNativeSet(s))

  let startIndex = s.startIndex
  let endIndex = s.endIndex
  expectEqual(startIndex, endIndex)
  expectFalse(startIndex < endIndex)
  expectTrue(startIndex <= endIndex)
  expectTrue(startIndex >= endIndex)
  expectFalse(startIndex > endIndex)
  expectEqual(identity1, s._rawIdentifier())

  // Keep indexes alive during the calls above.
  _fixLifetime(startIndex)
  _fixLifetime(endIndex)
}

SetTestSuite.test("BridgedFromObjC.Verbatim.Contains") {
  var s = getBridgedVerbatimSet()
  let identity1 = s._rawIdentifier()
  expectTrue(isCocoaSet(s))

  expectTrue(s.contains(TestObjCKeyTy(1010)))
  expectTrue(s.contains(TestObjCKeyTy(2020)))
  expectTrue(s.contains(TestObjCKeyTy(3030)))

  expectEqual(identity1, s._rawIdentifier())

  // Inserting an item should now create storage unique from the bridged set.
  s.insert(TestObjCKeyTy(4040))
  let identity2 = s._rawIdentifier()
  expectNotEqual(identity1, identity2)
  expectTrue(isNativeSet(s))
  expectEqual(4, s.count)

  // Verify items are still present.
  expectTrue(s.contains(TestObjCKeyTy(1010)))
  expectTrue(s.contains(TestObjCKeyTy(2020)))
  expectTrue(s.contains(TestObjCKeyTy(3030)))
  expectTrue(s.contains(TestObjCKeyTy(4040)))

  // Insert a redundant item to the set.
  // A copy should *not* occur here.
  s.insert(TestObjCKeyTy(1010))
  expectEqual(identity2, s._rawIdentifier())
  expectTrue(isNativeSet(s))
  expectEqual(4, s.count)

  // Again, verify items are still present.
  expectTrue(s.contains(TestObjCKeyTy(1010)))
  expectTrue(s.contains(TestObjCKeyTy(2020)))
  expectTrue(s.contains(TestObjCKeyTy(3030)))
  expectTrue(s.contains(TestObjCKeyTy(4040)))
}

SetTestSuite.test("BridgedFromObjC.Nonverbatim.Contains") {
  var s = getBridgedNonverbatimSet()
  let identity1 = s._rawIdentifier()
  let count1 = s.count
  let capacity1 = s.capacity
  expectTrue(isNativeSet(s))

  expectTrue(s.contains(TestBridgedKeyTy(1010)))
  expectTrue(s.contains(TestBridgedKeyTy(2020)))
  expectTrue(s.contains(TestBridgedKeyTy(3030)))

  expectEqual(identity1, s._rawIdentifier())

  s.insert(TestBridgedKeyTy(4040))
  let identity2 = s._rawIdentifier()

  // Storage identity may or may not change depending on allocation behavior.
  // (s is eagerly bridged to a regular uniquely referenced native Set.)
  expectTrue((count1 < capacity1) == (identity1 == identity2))

  expectTrue(isNativeSet(s))
  expectEqual(4, s.count)

  // Verify items are still present.
  expectTrue(s.contains(TestBridgedKeyTy(1010)))
  expectTrue(s.contains(TestBridgedKeyTy(2020)))
  expectTrue(s.contains(TestBridgedKeyTy(3030)))
  expectTrue(s.contains(TestBridgedKeyTy(4040)))

  // Insert a redundant item to the set.
  // A copy should *not* occur here.
  s.insert(TestBridgedKeyTy(1010))
  expectEqual(identity2, s._rawIdentifier())
  expectTrue(isNativeSet(s))
  expectEqual(4, s.count)

  // Again, verify items are still present.
  expectTrue(s.contains(TestBridgedKeyTy(1010)))
  expectTrue(s.contains(TestBridgedKeyTy(2020)))
  expectTrue(s.contains(TestBridgedKeyTy(3030)))
  expectTrue(s.contains(TestBridgedKeyTy(4040)))
}

SetTestSuite.test("BridgedFromObjC.Nonverbatim.SubscriptWithMember") {
  var s = getBridgedNonverbatimSet()
  let identity1 = s._rawIdentifier()
  expectTrue(isNativeSet(s))

  expectTrue(s.contains(TestBridgedKeyTy(1010)))
  expectTrue(s.contains(TestBridgedKeyTy(2020)))
  expectTrue(s.contains(TestBridgedKeyTy(3030)))

  expectEqual(identity1, s._rawIdentifier())

  // Insert a new member.
  // This should trigger a copy.
  s.insert(TestBridgedKeyTy(4040))
  let identity2 = s._rawIdentifier()

  expectTrue(isNativeSet(s))
  expectEqual(4, s.count)

  // Verify all items in place.
  expectTrue(s.contains(TestBridgedKeyTy(1010)))
  expectTrue(s.contains(TestBridgedKeyTy(2020)))
  expectTrue(s.contains(TestBridgedKeyTy(3030)))
  expectTrue(s.contains(TestBridgedKeyTy(4040)))

  // Insert a redundant member.
  // This should *not* trigger a copy.
  s.insert(TestBridgedKeyTy(1010))
  expectEqual(identity2, s._rawIdentifier())
  expectTrue(isNativeSet(s))
  expectEqual(4, s.count)

  // Again, verify all items in place.
  expectTrue(s.contains(TestBridgedKeyTy(1010)))
  expectTrue(s.contains(TestBridgedKeyTy(2020)))
  expectTrue(s.contains(TestBridgedKeyTy(3030)))
  expectTrue(s.contains(TestBridgedKeyTy(4040)))
}

SetTestSuite.test("BridgedFromObjC.Verbatim.RemoveAt") {
  var s = getBridgedVerbatimSet()
  let identity1 = s._rawIdentifier()
  expectTrue(isCocoaSet(s))

  let foundIndex1 = s.firstIndex(of: TestObjCKeyTy(1010))!
  expectEqual(TestObjCKeyTy(1010), s[foundIndex1])
  expectEqual(identity1, s._rawIdentifier())

  let removedElement = s.remove(at: foundIndex1)
  expectNotEqual(identity1, s._rawIdentifier())
  expectTrue(isNativeSet(s))
  expectEqual(2, s.count)
  expectEqual(TestObjCKeyTy(1010), removedElement)
  expectNil(s.firstIndex(of: TestObjCKeyTy(1010)))
}

SetTestSuite.test("BridgedFromObjC.Nonverbatim.RemoveAt")
  .code {
  var s = getBridgedNonverbatimSet()
  let identity1 = s._rawIdentifier()
  expectTrue(isNativeSet(s))

  let foundIndex1 = s.firstIndex(of: TestBridgedKeyTy(1010))!
  expectEqual(1010, s[foundIndex1].value)
  expectEqual(identity1, s._rawIdentifier())

  let removedElement = s.remove(at: foundIndex1)
  expectEqual(identity1, s._rawIdentifier())
  expectTrue(isNativeSet(s))
  expectEqual(1010, removedElement.value)
  expectEqual(2, s.count)
  expectNil(s.firstIndex(of: TestBridgedKeyTy(1010)))
}

SetTestSuite.test("BridgedFromObjC.Verbatim.Remove") {
  do {
    var s = getBridgedVerbatimSet()
    let identity1 = s._rawIdentifier()
    expectTrue(isCocoaSet(s))

    expectNil(s.remove(TestObjCKeyTy(0)))
    expectEqual(identity1, s._rawIdentifier())
    expectTrue(isCocoaSet(s))

    let deleted = s.remove(TestObjCKeyTy(1010)) as? TestObjCKeyTy
    expectEqual(1010, deleted?.value)
    let identity2 = s._rawIdentifier()
    expectNotEqual(identity1, identity2)
    expectTrue(isNativeSet(s))
    expectEqual(2, s.count)

    expectFalse(s.contains(TestObjCKeyTy(1010)))
    expectTrue(s.contains(TestObjCKeyTy(2020)))
    expectTrue(s.contains(TestObjCKeyTy(3030)))
    expectEqual(identity2, s._rawIdentifier())
  }

  do {
    let s1 = getBridgedVerbatimSet()
    let identity1 = s1._rawIdentifier()

    var s2 = s1
    expectTrue(isCocoaSet(s1))
    expectTrue(isCocoaSet(s2))

    expectNil(s2.remove(TestObjCKeyTy(0)))
    expectEqual(identity1, s1._rawIdentifier())
    expectEqual(identity1, s2._rawIdentifier())
    expectTrue(isCocoaSet(s1))
    expectTrue(isCocoaSet(s2))

    let deleted = s2.remove(TestObjCKeyTy(1010)) as? TestObjCKeyTy
    expectEqual(1010, deleted?.value)
    let identity2 = s2._rawIdentifier()
    expectNotEqual(identity1, identity2)
    expectTrue(isCocoaSet(s1))
    expectTrue(isNativeSet(s2))
    expectEqual(2, s2.count)

    expectTrue(s1.contains(TestObjCKeyTy(1010)))
    expectTrue(s1.contains(TestObjCKeyTy(2020)))
    expectTrue(s1.contains(TestObjCKeyTy(3030)))
    expectEqual(identity1, s1._rawIdentifier())

    expectFalse(s2.contains(TestObjCKeyTy(1010)))
    expectTrue(s2.contains(TestObjCKeyTy(2020)))
    expectTrue(s2.contains(TestObjCKeyTy(3030)))

    expectEqual(identity2, s2._rawIdentifier())
  }
}

SetTestSuite.test("BridgedFromObjC.Nonverbatim.Remove")
  .code {

  do {
    var s = getBridgedNonverbatimSet()
    let identity1 = s._rawIdentifier()
    expectTrue(isNativeSet(s))

    // Trying to remove something not in the set should
    // leave it completely unchanged.
    var deleted = s.remove(TestBridgedKeyTy(0))
    expectNil(deleted)
    expectEqual(identity1, s._rawIdentifier())
    expectTrue(isNativeSet(s))

    // Now remove an item that is in the set. This should
    // not create a new set altogether, however.
    deleted = s.remove(TestBridgedKeyTy(1010))
    expectEqual(1010, deleted!.value)
    let identity2 = s._rawIdentifier()
    expectEqual(identity1, identity2)
    expectTrue(isNativeSet(s))
    expectEqual(2, s.count)

    // Double-check - the removed member should not be found.
    expectFalse(s.contains(TestBridgedKeyTy(1010)))

    // ... but others not removed should be found.
    expectTrue(s.contains(TestBridgedKeyTy(2020)))
    expectTrue(s.contains(TestBridgedKeyTy(3030)))

    // Triple-check - we should still be working with the same object.
    expectEqual(identity2, s._rawIdentifier())
  }

  do {
    let s1 = getBridgedNonverbatimSet()
    let identity1 = s1._rawIdentifier()

    var s2 = s1
    expectTrue(isNativeSet(s1))
    expectTrue(isNativeSet(s2))

    var deleted = s2.remove(TestBridgedKeyTy(0))
    expectNil(deleted)
    expectEqual(identity1, s1._rawIdentifier())
    expectEqual(identity1, s2._rawIdentifier())
    expectTrue(isNativeSet(s1))
    expectTrue(isNativeSet(s2))

    deleted = s2.remove(TestBridgedKeyTy(1010))
    expectEqual(1010, deleted!.value)
    let identity2 = s2._rawIdentifier()
    expectNotEqual(identity1, identity2)
    expectTrue(isNativeSet(s1))
    expectTrue(isNativeSet(s2))
    expectEqual(2, s2.count)

    expectTrue(s1.contains(TestBridgedKeyTy(1010)))
    expectTrue(s1.contains(TestBridgedKeyTy(2020)))
    expectTrue(s1.contains(TestBridgedKeyTy(3030)))
    expectEqual(identity1, s1._rawIdentifier())

    expectFalse(s2.contains(TestBridgedKeyTy(1010)))
    expectTrue(s2.contains(TestBridgedKeyTy(2020)))
    expectTrue(s2.contains(TestBridgedKeyTy(3030)))
    expectEqual(identity2, s2._rawIdentifier())
  }
}

SetTestSuite.test("BridgedFromObjC.Verbatim.RemoveAll") {
  do {
    var s = getBridgedVerbatimSet([])
    expectTrue(isCocoaSet(s))
    expectEqual(0, s.count)

    let emptySet = Set<Int>()
    expectNotEqual(emptySet._rawIdentifier(), s._rawIdentifier())

    s.removeAll()
    expectEqual(emptySet._rawIdentifier(), s._rawIdentifier())
    expectEqual(0, s.count)
  }

  do {
    var s = getBridgedVerbatimSet()
    let identity1 = s._rawIdentifier()
    expectTrue(isCocoaSet(s))
    expectEqual(3, s.count)
    expectTrue(s.contains(TestBridgedKeyTy(1010) as NSObject))

    s.removeAll()
    expectEqual(0, s.count)
    expectFalse(s.contains(TestBridgedKeyTy(1010) as NSObject))
    expectNotEqual(identity1, s._rawIdentifier())
  }

  do {
    let s1 = getBridgedVerbatimSet()
    let identity1 = s1._rawIdentifier()
    expectTrue(isCocoaSet(s1))
    expectEqual(3, s1.count)
    expectTrue(s1.contains(TestBridgedKeyTy(1010) as NSObject))

    var s2 = s1
    s2.removeAll()
    let identity2 = s2._rawIdentifier()
    expectEqual(identity1, s1._rawIdentifier())
    expectNotEqual(identity2, identity1)
    expectEqual(3, s1.count)
    expectTrue(s1.contains(TestBridgedKeyTy(1010) as NSObject))
    expectEqual(0, s2.count)
    expectFalse(s2.contains(TestBridgedKeyTy(1010) as NSObject))
  }

  do {
    let s1 = getBridgedVerbatimSet()
    let identity1 = s1._rawIdentifier()
    expectTrue(isCocoaSet(s1))
    expectEqual(3, s1.count)
    expectTrue(s1.contains(TestBridgedKeyTy(1010) as NSObject))

    var s2 = s1
    s2.removeAll(keepingCapacity: true)
    let identity2 = s2._rawIdentifier()
    expectEqual(identity1, s1._rawIdentifier())
    expectNotEqual(identity2, identity1)
    expectEqual(3, s1.count)
    expectTrue(s1.contains(TestBridgedKeyTy(1010) as NSObject))
    expectEqual(0 , s2.count)
    expectFalse(s2.contains(TestBridgedKeyTy(1010) as NSObject))
  }
}

SetTestSuite.test("BridgedFromObjC.Nonverbatim.RemoveAll") {
  if #available(macOS 15.0, iOS 13.0, watchOS 6.0, tvOS 13.0, *) {
    // Identity of empty sets changed in https://github.com/apple/swift/pull/22527
    var s = getBridgedNonverbatimSet([])
    expectTrue(isNativeSet(s))
    expectEqual(0, s.count)

    let emptySet = Set<Int>()
    expectEqual(emptySet._rawIdentifier(), s._rawIdentifier())

    s.removeAll()
    expectEqual(emptySet._rawIdentifier(), s._rawIdentifier())
    expectEqual(0, s.count)
  }

  do {
    var s = getBridgedNonverbatimSet()
    let identity1 = s._rawIdentifier()
    expectTrue(isNativeSet(s))
    expectEqual(3, s.count)
    expectTrue(s.contains(TestBridgedKeyTy(1010)))

    s.removeAll()
    expectEqual(0, s.count)
    expectFalse(s.contains(TestBridgedKeyTy(1010)))
    expectNotEqual(identity1, s._rawIdentifier())
  }

  do {
    let s1 = getBridgedNonverbatimSet()
    let identity1 = s1._rawIdentifier()
    expectTrue(isNativeSet(s1))
    expectEqual(3, s1.count)
    expectTrue(s1.contains(TestBridgedKeyTy(1010)))

    var s2 = s1
    s2.removeAll()
    let identity2 = s2._rawIdentifier()
    expectEqual(identity1, s1._rawIdentifier())
    expectNotEqual(identity2, identity1)
    expectEqual(3, s1.count)
    expectTrue(s1.contains(TestBridgedKeyTy(1010)))
    expectEqual(0, s2.count)
    expectFalse(s2.contains(TestBridgedKeyTy(1010)))
  }

  do {
    let s1 = getBridgedNonverbatimSet()
    let identity1 = s1._rawIdentifier()
    expectTrue(isNativeSet(s1))
    expectEqual(3, s1.count)
    expectTrue(s1.contains(TestBridgedKeyTy(1010)))

    var s2 = s1
    s2.removeAll(keepingCapacity: true)
    let identity2 = s2._rawIdentifier()
    expectEqual(identity1, s1._rawIdentifier())
    expectNotEqual(identity2, identity1)
    expectEqual(3, s1.count)
    expectTrue(s1.contains(TestObjCKeyTy(1010) as TestBridgedKeyTy))
    expectEqual(0, s2.count)
    expectFalse(s2.contains(TestObjCKeyTy(1010) as TestBridgedKeyTy))
  }
}

SetTestSuite.test("BridgedFromObjC.Verbatim.Count") {
  let s = getBridgedVerbatimSet()
  let identity1 = s._rawIdentifier()
  expectTrue(isCocoaSet(s))

  expectEqual(3, s.count)
  expectEqual(identity1, s._rawIdentifier())
}

SetTestSuite.test("BridgedFromObjC.Nonverbatim.Count") {
  let s = getBridgedNonverbatimSet()
  let identity1 = s._rawIdentifier()
  expectTrue(isNativeSet(s))

  expectEqual(3, s.count)
  expectEqual(identity1, s._rawIdentifier())
}

SetTestSuite.test("BridgedFromObjC.Verbatim.Generate") {
  let s = getBridgedVerbatimSet()
  let identity1 = s._rawIdentifier()
  expectTrue(isCocoaSet(s))

  var iter = s.makeIterator()
  var members: [Int] = []
  while let member = iter.next() {
    members.append((member as! TestObjCKeyTy).value)
  }
  expectTrue(equalsUnordered(members, [1010, 2020, 3030]))
  expectNil(iter.next())
  expectNil(iter.next())
  expectNil(iter.next())
  expectEqual(identity1, s._rawIdentifier())
}

SetTestSuite.test("BridgedFromObjC.Nonverbatim.Generate") {
  let s = getBridgedNonverbatimSet()
  let identity1 = s._rawIdentifier()
  expectTrue(isNativeSet(s))

  var iter = s.makeIterator()
  var members: [Int] = []
  while let member = iter.next() {
    members.append(member.value)
  }
  expectTrue(equalsUnordered(members, [1010, 2020, 3030]))
  expectNil(iter.next())
  expectNil(iter.next())
  expectNil(iter.next())
  expectEqual(identity1, s._rawIdentifier())
}

SetTestSuite.test("BridgedFromObjC.Verbatim.Generate_Empty") {
  let s = getBridgedVerbatimSet([])
  let identity1 = s._rawIdentifier()
  expectTrue(isCocoaSet(s))

  var iter = s.makeIterator()
  expectNil(iter.next())
  expectNil(iter.next())
  expectNil(iter.next())
  expectNil(iter.next())
  expectEqual(identity1, s._rawIdentifier())
}

SetTestSuite.test("BridgedFromObjC.Nonverbatim.Generate_Empty") {
  let s = getBridgedNonverbatimSet([])
  let identity1 = s._rawIdentifier()
  expectTrue(isNativeSet(s))

  var iter = s.makeIterator()
  expectNil(iter.next())
  expectNil(iter.next())
  expectNil(iter.next())
  expectNil(iter.next())
  expectEqual(identity1, s._rawIdentifier())
}

SetTestSuite.test("BridgedFromObjC.Verbatim.Generate_Huge") {
  let s = getHugeBridgedVerbatimSet()
  let identity1 = s._rawIdentifier()
  expectTrue(isCocoaSet(s))

  var iter = s.makeIterator()
  var members = [Int]()
  while let member = iter.next() {
    members.append((member as! TestObjCKeyTy).value)
  }
  expectTrue(equalsUnordered(members, hugeNumberArray))
  expectNil(iter.next())
  expectNil(iter.next())
  expectNil(iter.next())
  expectEqual(identity1, s._rawIdentifier())
}

SetTestSuite.test("BridgedFromObjC.Nonverbatim.Generate_Huge") {
  let s = getHugeBridgedNonverbatimSet()
  let identity1 = s._rawIdentifier()
  expectTrue(isNativeSet(s))

  var iter = s.makeIterator()
  var members = [Int]()
  while let member = iter.next() as AnyObject? {
    members.append((member as! TestBridgedKeyTy).value)
  }
  expectTrue(equalsUnordered(members, hugeNumberArray))
  expectNil(iter.next())
  expectNil(iter.next())
  expectNil(iter.next())
  expectEqual(identity1, s._rawIdentifier())
}

SetTestSuite.test("BridgedFromObjC.Verbatim.EqualityTest_Empty") {
  let s1 = getBridgedVerbatimSet([])
  expectTrue(isCocoaSet(s1))

  var s2 = getBridgedVerbatimSet([])
  expectTrue(isCocoaSet(s2))

  expectEqual(s1, s2)

  s2.insert(TestObjCKeyTy(4040))
  expectTrue(isNativeSet(s2))

  expectNotEqual(s1, s2)
}

SetTestSuite.test("BridgedFromObjC.Nonverbatim.EqualityTest_Empty") {
  guard #available(macOS 15.0, iOS 13.0, watchOS 6.0, tvOS 13.0, *) else {
    // Identity of empty sets changed in https://github.com/apple/swift/pull/22527
    return
  }
  let s1 = getBridgedNonverbatimSet([])
  let identity1 = s1._rawIdentifier()
  expectTrue(isNativeSet(s1))

  var s2 = getBridgedNonverbatimSet([])
  let identity2 = s2._rawIdentifier()
  expectTrue(isNativeSet(s2))
  expectEqual(identity1, identity2)

  expectEqual(s1, s2)
  expectEqual(identity1, s1._rawIdentifier())
  expectEqual(identity2, s2._rawIdentifier())

  s2.insert(TestObjCKeyTy(4040) as TestBridgedKeyTy)
  expectTrue(isNativeSet(s2))
  expectNotEqual(identity2, s2._rawIdentifier())

  expectNotEqual(s1, s2)
  expectEqual(identity1, s1._rawIdentifier())
  expectNotEqual(identity2, s2._rawIdentifier())
}

SetTestSuite.test("BridgedFromObjC.Verbatim.EqualityTest_Small") {
  let s1 = getBridgedVerbatimSet()
  let identity1 = s1._rawIdentifier()
  expectTrue(isCocoaSet(s1))

  let s2 = getBridgedVerbatimSet()
  let identity2 = s2._rawIdentifier()
  expectTrue(isCocoaSet(s2))
  expectNotEqual(identity1, identity2)

  expectEqual(s1, s2)
  expectEqual(identity1, s1._rawIdentifier())
  expectEqual(identity2, s2._rawIdentifier())
}

SetTestSuite.test("BridgedFromObjC.Nonverbatim.EqualityTest_Small") {
  let s1 = getBridgedNonverbatimSet()
  let identity1 = s1._rawIdentifier()
  expectTrue(isNativeSet(s1))

  let s2 = getBridgedNonverbatimSet()
  let identity2 = s2._rawIdentifier()
  expectTrue(isNativeSet(s2))
  expectNotEqual(identity1, identity2)

  expectEqual(s1, s2)
  expectEqual(identity1, s1._rawIdentifier())
  expectEqual(identity2, s2._rawIdentifier())
}

SetTestSuite.test("BridgedFromObjC.Verbatim.ArrayOfSets") {
  let nsa = NSMutableArray()
  for i in 0..<3 {
    nsa.add(
        getAsNSSet([1 + i,  2 + i, 3 + i]))
  }

  let a = nsa as [AnyObject] as! [Set<NSObject>]
  for i in 0..<3 {
    let s = a[i]
    var iter = s.makeIterator()
    var items: [Int] = []
    while let value = iter.next() {
      let v = (value as! TestObjCKeyTy).value
      items.append(v)
    }
    let expectedItems = [1 + i, 2 + i, 3 + i]
    expectTrue(equalsUnordered(items, expectedItems))
  }
}

SetTestSuite.test("BridgedFromObjC.Nonverbatim.ArrayOfSets") {
  let nsa = NSMutableArray()
  for i in 0..<3 {
    nsa.add(
        getAsNSSet([1 + i, 2 + i, 3 + i]))
  }

  let a = nsa as [AnyObject] as! [Set<TestBridgedKeyTy>]
  for i in 0..<3 {
    let d = a[i]
    var iter = d.makeIterator()
    var items: [Int] = []
    while let value = iter.next() {
      items.append(value.value)
    }
    let expectedItems = [1 + i, 2 + i, 3 + i]
    expectTrue(equalsUnordered(items, expectedItems))
  }
}

SetTestSuite.test("BridgedFromObjC.Nonverbatim.StringEqualityMismatch") {
  // NSString's isEqual(_:) implementation is stricter than Swift's String, so
  // Set values bridged over from Objective-C may have duplicate keys.
  // rdar://problem/35995647
  let cafe1 = "Cafe\u{301}" as NSString
  let cafe2 = "Café" as NSString

  let nsset = NSMutableSet()
  nsset.add(cafe1)
  expectTrue(nsset.contains(cafe1))
  expectFalse(nsset.contains(cafe2))
  nsset.add(cafe2)
  expectEqual(2, nsset.count)
  expectTrue(nsset.contains(cafe1))
  expectTrue(nsset.contains(cafe2))
  
  let s: Set<String> = convertNSSetToSet(nsset)
  expectEqual(1, s.count)
  expectTrue(s.contains("Cafe\u{301}"))
  expectTrue(s.contains("Café"))
  expectTrue(Array(s) == ["Café"])
}

//===---
// Dictionary -> NSDictionary bridging tests.
//
// Value is bridged verbatim.
//===---

SetTestSuite.test("BridgedToObjC.Verbatim.Count") {
  let s = getBridgedNSSetOfRefTypesBridgedVerbatim()

  expectEqual(3, s.count)
}

SetTestSuite.test("BridgedToObjC.Verbatim.Contains") {
  let s = getBridgedNSSetOfRefTypesBridgedVerbatim()

  var v: AnyObject? = s.member(TestObjCKeyTy(1010)).map { $0 as AnyObject }
  expectEqual(1010, (v as! TestObjCKeyTy).value)
  let idValue10 = unsafeBitCast(v, to: UInt.self)

  v = s.member(TestObjCKeyTy(2020)).map { $0 as AnyObject }
  expectEqual(2020, (v as! TestObjCKeyTy).value)
  let idValue20 = unsafeBitCast(v, to: UInt.self)

  v = s.member(TestObjCKeyTy(3030)).map { $0 as AnyObject }
  expectEqual(3030, (v as! TestObjCKeyTy).value)
  let idValue30 = unsafeBitCast(v, to: UInt.self)

  expectNil(s.member(TestObjCKeyTy(4040)))

  // NSSet can store mixed key types.  Swift's Set is typed, but when bridged
  // to NSSet, it should behave like one, and allow queries for mismatched key
  // types.
  expectNil(s.member(TestObjCInvalidKeyTy()))

  for _ in 0..<3 {
    expectEqual(idValue10,
      unsafeBitCast(s.member(TestObjCKeyTy(1010)).map { $0 as AnyObject }, to: UInt.self))

    expectEqual(idValue20,
      unsafeBitCast(s.member(TestObjCKeyTy(2020)).map { $0 as AnyObject }, to: UInt.self))

    expectEqual(idValue30,
      unsafeBitCast(s.member(TestObjCKeyTy(3030)).map { $0 as AnyObject }, to: UInt.self))
  }

  expectAutoreleasedKeysAndValues(unopt: (3, 0))
}

SetTestSuite.test("BridgingRoundtrip") {
  let s = getRoundtripBridgedNSSet()
  let enumerator = s.objectEnumerator()

  var items: [Int] = []
  while let value = enumerator.nextObject() {
    let v = (value as! TestObjCKeyTy).value
    items.append(v)
  }
  expectTrue(equalsUnordered([1010, 2020, 3030], items))
}

SetTestSuite.test("BridgedToObjC.Verbatim.ObjectEnumerator.FastEnumeration.UseFromSwift") {
  let s = getBridgedNSSetOfRefTypesBridgedVerbatim()

  checkSetFastEnumerationFromSwift(
    [ 1010, 2020, 3030 ],
    s, { s.objectEnumerator() },
    { ($0 as! TestObjCKeyTy).value })

  expectAutoreleasedKeysAndValues(unopt: (3, 0))
}

SetTestSuite.test("BridgedToObjC.Verbatim.ObjectEnumerator.FastEnumeration.UseFromObjC") {
  let s = getBridgedNSSetOfRefTypesBridgedVerbatim()

  checkSetFastEnumerationFromObjC(
    [ 1010, 2020, 3030 ],
    s, { s.objectEnumerator() },
    { ($0 as! TestObjCKeyTy).value })

  expectAutoreleasedKeysAndValues(unopt: (3, 0))
}

SetTestSuite.test("BridgedToObjC.Verbatim.ObjectEnumerator.FastEnumeration_Empty") {
  let s = getBridgedEmptyNSSet()

  checkSetFastEnumerationFromSwift(
    [], s, { s.objectEnumerator() },
    { ($0 as! TestObjCKeyTy).value })

  checkSetFastEnumerationFromObjC(
    [], s, { s.objectEnumerator() },
    { ($0 as! TestObjCKeyTy).value })
}

SetTestSuite.test("BridgedToObjC.Custom.ObjectEnumerator.FastEnumeration.UseFromObjC") {
  let s = getBridgedNSSet_MemberTypesCustomBridged()

  checkSetFastEnumerationFromObjC(
    [ 1010, 2020, 3030 ],
    s, { s.objectEnumerator() },
    { ($0 as! TestObjCKeyTy).value })

  expectAutoreleasedKeysAndValues(unopt: (3, 0))
}

SetTestSuite.test("BridgedToObjC.Custom.ObjectEnumerator.FastEnumeration.UseFromSwift") {
  let s = getBridgedNSSet_MemberTypesCustomBridged()

  checkSetFastEnumerationFromSwift(
    [ 1010, 2020, 3030 ],
    s, { s.objectEnumerator() },
    { ($0 as! TestObjCKeyTy).value })

  expectAutoreleasedKeysAndValues(unopt: (3, 0))
}

SetTestSuite.test("BridgedToObjC.Verbatim.FastEnumeration.UseFromSwift") {
  let s = getBridgedNSSetOfRefTypesBridgedVerbatim()

  checkSetFastEnumerationFromSwift(
    [ 1010, 2020, 3030 ],
    s, { s },
    { ($0 as! TestObjCKeyTy).value })
}

SetTestSuite.test("BridgedToObjC.Verbatim.FastEnumeration.UseFromObjC") {
  let s = getBridgedNSSetOfRefTypesBridgedVerbatim()

  checkSetFastEnumerationFromObjC(
    [ 1010, 2020, 3030 ],
    s, { s },
    { ($0 as! TestObjCKeyTy).value })
}

SetTestSuite.test("BridgedToObjC.Verbatim.FastEnumeration_Empty") {
  let s = getBridgedEmptyNSSet()

  checkSetFastEnumerationFromSwift(
    [], s, { s },
    { ($0 as! TestObjCKeyTy).value })

  checkSetFastEnumerationFromObjC(
    [], s, { s },
    { ($0 as! TestObjCKeyTy).value })
}

SetTestSuite.test("BridgedToObjC.Custom.FastEnumeration.UseFromSwift") {
  let s = getBridgedNSSet_ValueTypesCustomBridged()

  checkSetFastEnumerationFromSwift(
    [ 1010, 2020, 3030 ],
    s, { s },
    { ($0 as! TestObjCKeyTy).value })
}

SetTestSuite.test("BridgedToObjC.Custom.FastEnumeration.UseFromObjC") {
  let s = getBridgedNSSet_ValueTypesCustomBridged()

  checkSetFastEnumerationFromObjC(
    [ 1010, 2020, 3030 ],
    s, { s },
    { ($0 as! TestObjCKeyTy).value })
}

SetTestSuite.test("BridgedToObjC.Custom.FastEnumeration_Empty") {
  let s = getBridgedNSSet_ValueTypesCustomBridged(
    numElements: 0)

  checkSetFastEnumerationFromSwift(
    [], s, { s },
    { ($0 as! TestObjCKeyTy).value })

  checkSetFastEnumerationFromObjC(
    [], s, { s },
    { ($0 as! TestObjCKeyTy).value })
}

SetTestSuite.test("BridgedToObjC.Count") {
  let s = getBridgedNSSetOfRefTypesBridgedVerbatim()
  expectEqual(3, s.count)
}

SetTestSuite.test("BridgedToObjC.ObjectEnumerator.NextObject") {
  let s = getBridgedNSSetOfRefTypesBridgedVerbatim()
  let enumerator = s.objectEnumerator()

  var members = [Int]()
  while let nextObject = enumerator.nextObject() {
    members.append((nextObject as! TestObjCKeyTy).value)
  }
  expectTrue(equalsUnordered([1010, 2020, 3030], members))

  expectNil(enumerator.nextObject())
  expectNil(enumerator.nextObject())
  expectNil(enumerator.nextObject())

  expectAutoreleasedKeysAndValues(unopt: (3, 0))
}

SetTestSuite.test("BridgedToObjC.ObjectEnumerator.NextObject.Empty") {
  let s = getBridgedEmptyNSSet()
  let enumerator = s.objectEnumerator()

  expectNil(enumerator.nextObject())
  expectNil(enumerator.nextObject())
  expectNil(enumerator.nextObject())
}

//
// Set -> NSSet Bridging
//

SetTestSuite.test("BridgedToObjC.MemberTypesCustomBridged") {
  let s = getBridgedNSSet_MemberTypesCustomBridged()
  let enumerator = s.objectEnumerator()

  var members = [Int]()
  while let nextObject = enumerator.nextObject() {
    members.append((nextObject as! TestObjCKeyTy).value)
  }
  expectTrue(equalsUnordered([1010, 2020, 3030], members))

  expectAutoreleasedKeysAndValues(unopt: (3, 0))
}

//
// NSSet -> Set -> NSSet Round trip bridging
//

SetTestSuite.test("BridgingRoundTrip") {
  let s = getRoundtripBridgedNSSet()
  let enumerator = s.objectEnumerator()

  var members = [Int]()
  while let nextObject = enumerator.nextObject() {
    members.append((nextObject as! TestObjCKeyTy).value)
  }
  expectTrue(equalsUnordered([1010, 2020, 3030] ,members))
}

//
// NSSet -> Set implicit conversion
//

SetTestSuite.test("NSSetToSetConversion") {
  let nsArray = NSMutableArray()
  for i in [1010, 2020, 3030] {
    nsArray.add(TestObjCKeyTy(i))
  }

  let nss = NSSet(array: nsArray as [AnyObject])

  let s = nss as Set

  var members = [Int]()
  for member: AnyObject in s {
    members.append((member as! TestObjCKeyTy).value)
  }
  expectTrue(equalsUnordered(members, [1010, 2020, 3030]))
}

SetTestSuite.test("SetToNSSetConversion") {
  var s = Set<TestObjCKeyTy>(minimumCapacity: 32)
  for i in [1010, 2020, 3030] {
    s.insert(TestObjCKeyTy(i))
  }
  let nss: NSSet = s as NSSet

  expectTrue(equalsUnordered(Array(nss).map {  ($0 as! TestObjCKeyTy).value }, [1010, 2020, 3030]))
}

//
// Set Casts
//

SetTestSuite.test("SetUpcastEntryPoint") {
  var s = Set<TestObjCKeyTy>(minimumCapacity: 32)
  for i in [1010, 2020, 3030] {
      s.insert(TestObjCKeyTy(i))
  }

  let sAsAnyObject: Set<NSObject> = _setUpCast(s)

  expectEqual(3, sAsAnyObject.count)
  expectTrue(sAsAnyObject.contains(TestObjCKeyTy(1010)))
  expectTrue(sAsAnyObject.contains(TestObjCKeyTy(2020)))
  expectTrue(sAsAnyObject.contains(TestObjCKeyTy(3030)))
}

SetTestSuite.test("SetUpcast") {
  var s = Set<TestObjCKeyTy>(minimumCapacity: 32)
  for i in [1010, 2020, 3030] {
      s.insert(TestObjCKeyTy(i))
  }

  let sAsAnyObject: Set<NSObject> = s

  expectEqual(3, sAsAnyObject.count)
  expectTrue(sAsAnyObject.contains(TestObjCKeyTy(1010)))
  expectTrue(sAsAnyObject.contains(TestObjCKeyTy(2020)))
  expectTrue(sAsAnyObject.contains(TestObjCKeyTy(3030)))
}

SetTestSuite.test("SetUpcastBridgedEntryPoint") {
  var s = Set<TestBridgedKeyTy>(minimumCapacity: 32)
  for i in [1010, 2020, 3030] {
      s.insert(TestBridgedKeyTy(i))
  }

  do {
    let so = s as Set<NSObject>

    expectTrue(so.contains(TestBridgedKeyTy(1010) as NSObject))
    expectTrue(so.contains(TestBridgedKeyTy(2020) as NSObject))
    expectTrue(so.contains(TestBridgedKeyTy(3030) as NSObject))
  }

  do {
    let st = s as Set<TestObjCKeyTy>

    expectEqual(3, st.count)
    expectTrue(st.contains(TestBridgedKeyTy(1010) as TestObjCKeyTy))
    expectTrue(st.contains(TestBridgedKeyTy(2020) as TestObjCKeyTy))
    expectTrue(st.contains(TestBridgedKeyTy(3030) as TestObjCKeyTy))
  }
}

SetTestSuite.test("SetUpcastBridged") {
  var s = Set<TestBridgedKeyTy>(minimumCapacity: 32)
  for i in [1010, 2020, 3030] {
      s.insert(TestBridgedKeyTy(i))
  }

  do {
    let s = s as Set<NSObject>

    expectEqual(3, s.count)
    expectTrue(s.contains(TestBridgedKeyTy(1010) as NSObject))
    expectTrue(s.contains(TestBridgedKeyTy(2020) as NSObject))
    expectTrue(s.contains(TestBridgedKeyTy(3030) as NSObject))
  }

  do {
    let s = s as Set<TestObjCKeyTy>

    expectEqual(3, s.count)
    expectTrue(s.contains(TestBridgedKeyTy(1010) as TestObjCKeyTy))
    expectTrue(s.contains(TestBridgedKeyTy(2020) as TestObjCKeyTy))
    expectTrue(s.contains(TestBridgedKeyTy(3030) as TestObjCKeyTy))
  }
}

//
// Set downcasts
//

SetTestSuite.test("SetDowncastEntryPoint") {
  var s = Set<NSObject>(minimumCapacity: 32)
  for i in [1010, 2020, 3030] {
      s.insert(TestObjCKeyTy(i))
  }

  // Successful downcast.
  let sCC: Set<TestObjCKeyTy> = _setDownCast(s)
  expectEqual(3, sCC.count)
  expectTrue(sCC.contains(TestObjCKeyTy(1010)))
  expectTrue(sCC.contains(TestObjCKeyTy(2020)))
  expectTrue(sCC.contains(TestObjCKeyTy(3030)))

  expectAutoreleasedKeysAndValues(unopt: (3, 0))
}

SetTestSuite.test("SetDowncast") {
  var s = Set<NSObject>(minimumCapacity: 32)
  for i in [1010, 2020, 3030] {
      s.insert(TestObjCKeyTy(i))
  }

  // Successful downcast.
  let sCC = s as! Set<TestObjCKeyTy>
  expectEqual(3, sCC.count)
  expectTrue(sCC.contains(TestObjCKeyTy(1010)))
  expectTrue(sCC.contains(TestObjCKeyTy(2020)))
  expectTrue(sCC.contains(TestObjCKeyTy(3030)))

  expectAutoreleasedKeysAndValues(unopt: (3, 0))
}

SetTestSuite.test("SetDowncastConditionalEntryPoint") {
  var s = Set<NSObject>(minimumCapacity: 32)
  for i in [1010, 2020, 3030] {
      s.insert(TestObjCKeyTy(i))
  }

  // Successful downcast.
  if let sCC  = _setDownCastConditional(s) as Set<TestObjCKeyTy>? {
    expectEqual(3, sCC.count)
    expectTrue(sCC.contains(TestObjCKeyTy(1010)))
    expectTrue(sCC.contains(TestObjCKeyTy(2020)))
    expectTrue(sCC.contains(TestObjCKeyTy(3030)))
  } else {
    expectTrue(false)
  }

  // Unsuccessful downcast
  s.insert("Hello, world" as NSString)
  expectNil(_setDownCastConditional(s) as Set<TestObjCKeyTy>?)
}

SetTestSuite.test("SetDowncastConditional") {
  var s = Set<NSObject>(minimumCapacity: 32)
  for i in [1010, 2020, 3030] {
      s.insert(TestObjCKeyTy(i))
  }

  // Successful downcast.
  if let sCC = s as? Set<TestObjCKeyTy> {
    expectEqual(3, sCC.count)
    expectTrue(sCC.contains(TestObjCKeyTy(1010)))
    expectTrue(sCC.contains(TestObjCKeyTy(2020)))
    expectTrue(sCC.contains(TestObjCKeyTy(3030)))
  } else {
    expectTrue(false)
  }

  // Unsuccessful downcast
  s.insert("Hello, world, I'm your wild girl. I'm your ch-ch-ch-ch-ch-ch cherry bomb" as NSString)
  if s is Set<TestObjCKeyTy> {
    expectTrue(false)
  }
}

SetTestSuite.test("SetBridgeFromObjectiveCEntryPoint") {
  var s = Set<NSObject>(minimumCapacity: 32)
  for i in [1010, 2020, 3030] {
      s.insert(TestObjCKeyTy(i))
  }

  // Successful downcast.
  let sCV = s as! Set<TestBridgedKeyTy>
  do {
    expectEqual(3, sCV.count)
    expectTrue(sCV.contains(TestBridgedKeyTy(1010)))
    expectTrue(sCV.contains(TestBridgedKeyTy(2020)))
    expectTrue(sCV.contains(TestBridgedKeyTy(3030)))
  }
}

SetTestSuite.test("SetBridgeFromObjectiveC") {
  var s = Set<NSObject>(minimumCapacity: 32)
  for i in [1010, 2020, 3030] {
      s.insert(TestObjCKeyTy(i))
  }

  // Successful downcast.
  let sCV = s as! Set<TestObjCKeyTy>
  do {
    expectEqual(3, sCV.count)
    expectTrue(sCV.contains(TestObjCKeyTy(1010)))
    expectTrue(sCV.contains(TestObjCKeyTy(2020)))
    expectTrue(sCV.contains(TestObjCKeyTy(3030)))
  }

  // Successful downcast.
  let sVC = s as! Set<TestBridgedKeyTy>
  do {
    expectEqual(3, sVC.count)
    expectTrue(sVC.contains(TestBridgedKeyTy(1010)))
    expectTrue(sVC.contains(TestBridgedKeyTy(2020)))
    expectTrue(sVC.contains(TestBridgedKeyTy(3030)))
  }

  expectAutoreleasedKeysAndValues(unopt: (3, 0))
}

SetTestSuite.test("SetBridgeFromObjectiveCConditionalEntryPoint") {
  var s = Set<NSObject>(minimumCapacity: 32)
  for i in [1010, 2020, 3030] {
      s.insert(TestObjCKeyTy(i))
  }

  // Successful downcast.
  if let sVC = s as? Set<TestBridgedKeyTy> {
    expectEqual(3, sVC.count)
    expectTrue(sVC.contains(TestBridgedKeyTy(1010)))
    expectTrue(sVC.contains(TestBridgedKeyTy(2020)))
    expectTrue(sVC.contains(TestBridgedKeyTy(3030)))
  } else {
    expectTrue(false)
  }

  // Unsuccessful downcasts
  s.insert("Hello, world, I'm your wild girl. I'm your ch-ch-ch-ch-ch-ch cherry bomb" as NSString)
  expectNil(s as? Set<TestBridgedKeyTy>)
}

SetTestSuite.test("SetBridgeFromObjectiveCConditional") {
  var s = Set<NSObject>(minimumCapacity: 32)
  for i in [1010, 2020, 3030] {
      s.insert(TestObjCKeyTy(i))
  }

  // Successful downcast.
  if let sCV = s as? Set<TestObjCKeyTy> {
    expectEqual(3, sCV.count)
    expectTrue(sCV.contains(TestObjCKeyTy(1010)))
    expectTrue(sCV.contains(TestObjCKeyTy(2020)))
    expectTrue(sCV.contains(TestObjCKeyTy(3030)))
  } else {
    expectTrue(false)
  }

  // Successful downcast.
  if let sVC = s as? Set<TestBridgedKeyTy> {
    expectEqual(3, sVC.count)
    expectTrue(sVC.contains(TestBridgedKeyTy(1010)))
    expectTrue(sVC.contains(TestBridgedKeyTy(2020)))
    expectTrue(sVC.contains(TestBridgedKeyTy(3030)))
  } else {
    expectTrue(false)
  }

  // Unsuccessful downcasts
  s.insert("Hello, world, I'm your wild girl. I'm your ch-ch-ch-ch-ch-ch cherry bomb" as NSString)
  if s is Set<TestObjCKeyTy> {
    expectTrue(false)
  }
  if s is Set<TestBridgedKeyTy> {
    expectTrue(false)
  }
  if s is Set<TestBridgedKeyTy> {
    expectTrue(false)
  }
}
#endif // _runtime(_ObjC)

// Public API

SetTestSuite.test("init(Sequence:)") {
  let s1 = Set([1010, 2020, 3030])
  var s2 = Set<Int>()
  s2.insert(1010)
  s2.insert(2020)
  s2.insert(3030)
  expectEqual(s1, s2)

  // Test the uniquing capabilities of a set
  let s3 = Set([
    1010, 1010, 1010, 1010, 1010, 1010,
    1010, 1010, 1010, 1010, 1010, 1010,
    2020, 2020, 2020, 3030, 3030, 3030
  ])
  expectEqual(s1, s3)
}

SetTestSuite.test("init(arrayLiteral:)") {
  let s1: Set<Int> = [1010, 2020, 3030, 1010, 2020, 3030]
  let s2 = Set([1010, 2020, 3030])
  var s3 = Set<Int>()
  s3.insert(1010)
  s3.insert(2020)
  s3.insert(3030)
  expectEqual(s1, s2)
  expectEqual(s2, s3)
}

SetTestSuite.test("isSubsetOf.Set.Set") {
  let s1 = Set([1010, 2020, 3030, 4040, 5050, 6060])
  let s2 = Set([1010, 2020, 3030])
  expectTrue(Set<Int>().isSubset(of: s1))
  expectFalse(s1.isSubset(of: Set<Int>()))
  expectTrue(Set<Int>().isSubset(of: Set<Int>()))
  expectTrue(s1.isSubset(of: s1))
  expectFalse(s1.isSubset(of: s2))
  expectTrue(s2.isSubset(of: s1))
}

SetTestSuite.test("isSubsetOf.Set.Sequence") {
  typealias Seq = Array<Int>
  let s1 = Set([1010, 2020, 3030])
  expectTrue(Set<Int>().isSubset(of: [1010] as Seq))
  expectFalse(s1.isSubset(of: [] as Seq))
  expectTrue(Set<Int>().isSubset(of: [] as Seq))
  expectTrue(s1.isSubset(of: [1010, 2020, 3030] as Seq))
  expectFalse(s1.isSubset(of: [1010, 2020] as Seq))
  expectTrue(s1.isSubset(of: [1010, 2020, 3030, 4040] as Seq))
}

SetTestSuite.test("isStrictSubsetOf.Set.Set") {
  let s1 = Set([1010, 2020, 3030, 4040, 5050, 6060])
  let s2 = Set([1010, 2020, 3030])
  expectTrue(Set<Int>().isStrictSubset(of: s1))
  expectFalse(s1.isStrictSubset(of: Set<Int>()))
  expectFalse(Set<Int>().isStrictSubset(of: Set<Int>()))
  expectFalse(s1.isStrictSubset(of: s1))
  expectFalse(s1.isStrictSubset(of: s2))
  expectTrue(s2.isStrictSubset(of: s1))
}

SetTestSuite.test("isStrictSubsetOf.Set.Sequence") {
  typealias Seq = Array<Int>
  let s1 = Set([1010, 2020, 3030])
  expectTrue(Set<Int>().isStrictSubset(of: [1010] as Seq))
  expectFalse(s1.isStrictSubset(of: [] as Seq))
  expectFalse(Set<Int>().isStrictSubset(of: [] as Seq))
  expectFalse(s1.isStrictSubset(of: [1010, 2020, 3030] as Seq))
  expectFalse(s1.isStrictSubset(of: [1010, 2020] as Seq))
  expectTrue(s1.isStrictSubset(of: [1010, 2020, 3030, 4040] as Seq))
}

SetTestSuite.test("isSupersetOf.Set.Set") {
  let s1 = Set([1010, 2020, 3030, 4040, 5050, 6060])
  let s2 = Set([1010, 2020, 3030])
  expectTrue(s1.isSuperset(of: Set<Int>()))
  expectFalse(Set<Int>().isSuperset(of: s1))
  expectTrue(Set<Int>().isSuperset(of: Set<Int>()))
  expectTrue(s1.isSuperset(of: s1))
  expectTrue(s1.isSuperset(of: s2))
  expectFalse(s2.isSuperset(of: s1))
}

SetTestSuite.test("isSupersetOf.Set.Sequence") {
  typealias Seq = Array<Int>
  let s1 = Set([1010, 2020, 3030])
  expectTrue(s1.isSuperset(of: [] as Seq))
  expectFalse(Set<Int>().isSuperset(of: [1010] as Seq))
  expectTrue(Set<Int>().isSuperset(of: [] as Seq))
  expectTrue(s1.isSuperset(of: [1010, 2020, 3030] as Seq))
  expectTrue(s1.isSuperset(of: [1010, 2020] as Seq))
  expectFalse(s1.isSuperset(of: [1010, 2020, 3030, 4040] as Seq))
}

SetTestSuite.test("isStrictSuperset.Set.Set") {
  let s1 = Set([1010, 2020, 3030, 4040, 5050, 6060])
  let s2 = Set([1010, 2020, 3030])
  expectTrue(s1.isStrictSuperset(of: Set<Int>()))
  expectFalse(Set<Int>().isStrictSuperset(of: s1))
  expectFalse(Set<Int>().isStrictSuperset(of: Set<Int>()))
  expectFalse(s1.isStrictSuperset(of: s1))
  expectTrue(s1.isStrictSuperset(of: s2))
  expectFalse(s2.isStrictSuperset(of: s1))
}

SetTestSuite.test("isStrictSuperset.Set.Sequence") {
  typealias Seq = Array<Int>
  let s1 = Set([1010, 2020, 3030])
  expectTrue(s1.isStrictSuperset(of: [] as Seq))
  expectFalse(Set<Int>().isStrictSuperset(of: [1010] as Seq))
  expectFalse(Set<Int>().isStrictSuperset(of: [] as Seq))
  expectFalse(s1.isStrictSuperset(of: [1010, 2020, 3030] as Seq))
  expectTrue(s1.isStrictSuperset(of: [1010, 2020] as Seq))
  expectFalse(s1.isStrictSuperset(of: [1010, 2020, 3030, 4040] as Seq))
}

SetTestSuite.test("Equatable.Native.Native") {
  let s1 = getCOWFastSet()
  let s2 = getCOWFastSet([1010, 2020, 3030, 4040, 5050, 6060])

  checkEquatable(true, s1, s1)
  checkEquatable(false, s1, Set<Int>())
  checkEquatable(true, Set<Int>(), Set<Int>())
  checkEquatable(false, s1, s2)
}

#if _runtime(_ObjC)
SetTestSuite.test("Equatable.Native.BridgedVerbatim") {
  let s1 = getNativeBridgedVerbatimSet()
  let bvs1 = getBridgedVerbatimSet()
  let bvs2 = getBridgedVerbatimSet([1010, 2020, 3030, 4040, 5050, 6060])
  let bvsEmpty = getBridgedVerbatimSet([])

  checkEquatable(true, s1, bvs1)
  checkEquatable(false, s1, bvs2)
  checkEquatable(false, s1, bvsEmpty)
}

SetTestSuite.test("Equatable.BridgedVerbatim.BridgedVerbatim") {
  let bvs1 = getBridgedVerbatimSet()
  let bvs2 = getBridgedVerbatimSet([1010, 2020, 3030, 4040, 5050, 6060])
  let bvsEmpty = getBridgedVerbatimSet([])

  checkEquatable(true, bvs1, bvs1)
  checkEquatable(false, bvs1, bvs2)
  checkEquatable(false, bvs1, bvsEmpty)
}

SetTestSuite.test("Equatable.BridgedNonverbatim.BridgedNonverbatim") {
  let bnvs1 = getBridgedNonverbatimSet()
  let bnvs2 = getBridgedNonverbatimSet([1010, 2020, 3030, 4040, 5050, 6060])
  let bnvsEmpty = getBridgedNonverbatimSet([])

  checkEquatable(true, bnvs1, bnvs1)
  checkEquatable(false, bnvs1, bnvs2)
  checkEquatable(false, bnvs1, bnvsEmpty)
  checkEquatable(false, bnvs2, bnvsEmpty)
}
#endif // _runtime(_ObjC)

SetTestSuite.test("isDisjointWith.Set.Set") {
  let s1 = Set([1010, 2020, 3030, 4040, 5050, 6060])
  let s2 = Set([1010, 2020, 3030])
  let s3 = Set([7070, 8080, 9090])
  expectTrue(s1.isDisjoint(with: s3))
  expectFalse(s1.isDisjoint(with: s2))
  expectTrue(Set<Int>().isDisjoint(with: s1))
  expectTrue(Set<Int>().isDisjoint(with: Set<Int>()))
}

SetTestSuite.test("isDisjointWith.Set.Sequence") {
  let s1 = Set([1010, 2020, 3030, 4040, 5050, 6060])
  let s2 = AnySequence([1010, 2020, 3030])
  let s3 = AnySequence([7070, 8080, 9090])
  expectTrue(s1.isDisjoint(with: s3))
  expectFalse(s1.isDisjoint(with: s2))
  expectTrue(Set<Int>().isDisjoint(with: s1))
  expectTrue(Set<Int>().isDisjoint(with: Set<Int>()))
}

SetTestSuite.test("insert") {
  // These are anagrams - they should amount to the same sets.
  var s1 = Set<TestKeyTy>([1010, 2020, 3030])

  let fortyForty: TestKeyTy = 4040
  do {
    // Inserting an element that isn't present
    let (inserted, currentMember) = s1.insert(fortyForty)
    expectTrue(inserted)
    expectTrue(currentMember === fortyForty)
  }
  
  do {
    // Inserting an element that is already present
    let (inserted, currentMember) = s1.insert(4040)
    expectFalse(inserted)
    expectTrue(currentMember === fortyForty)
  }
  
  expectEqual(4, s1.count)
  expectTrue(s1.contains(1010))
  expectTrue(s1.contains(2020))
  expectTrue(s1.contains(3030))
  expectTrue(s1.contains(4040))
}

SetTestSuite.test("update(with:)") {
  // These are anagrams - they should amount to the same sets.
  var s1 = Set<TestKeyTy>([1010, 2020, 3030])

  let fortyForty: TestKeyTy = 4040
  do {
    // Replacing an element that isn't present
    let oldMember = s1.update(with: fortyForty)
    expectNil(oldMember)
  }
  
  do {
    // Replacing an element that is already present
    let oldMember = s1.update(with: 4040)
    expectTrue(oldMember === fortyForty)
  }
  
  expectEqual(4, s1.count)
  expectTrue(s1.contains(1010))
  expectTrue(s1.contains(2020))
  expectTrue(s1.contains(3030))
  expectTrue(s1.contains(4040))
}

SetTestSuite.test("union") {
  let s1 = Set([1010, 2020, 3030])
  let s2 = Set([4040, 5050, 6060])
  let s3 = Set([1010, 2020, 3030, 4040, 5050, 6060])

  let identity1 = s1._rawIdentifier()

  let s4 = s1.union(s2)
  expectEqual(s4, s3)

  // s1 should be unchanged
  expectEqual(identity1, s1._rawIdentifier())
  expectEqual(Set([1010, 2020, 3030]), s1)

  // s4 should be a fresh set
  expectNotEqual(identity1, s4._rawIdentifier())
  expectEqual(s4, s3)

  let s5 = s1.union(s1)
  expectEqual(s5, s1)
  expectEqual(identity1, s1._rawIdentifier())

  expectEqual(s1, s1.union(Set<Int>()))
  expectEqual(s1, Set<Int>().union(s1))
}

SetTestSuite.test("formUnion") {
  // These are anagrams - they should amount to the same sets.
  var s1 = Set("the morse code")
  let s2 = Set("here come dots")
  let s3 = Set("and then dashes")

  let identity1 = s1._rawIdentifier()

  s1.formUnion("")
  expectEqual(identity1, s1._rawIdentifier())

  expectEqual(s1, s2)
  s1.formUnion(s2)
  expectEqual(s1, s2)
  expectEqual(identity1, s1._rawIdentifier())

  s1.formUnion(s3)
  expectNotEqual(s1, s2)
  expectEqual(identity1, s1._rawIdentifier())
}

SetTestSuite.test("subtract")
  .code {
  let s1 = Set([1010, 2020, 3030])
  let s2 = Set([4040, 5050, 6060])
  let s3 = Set([1010, 2020, 3030, 4040, 5050, 6060])

  let identity1 = s1._rawIdentifier()

  // Subtracting a disjoint set should not create a
  // unique reference
  let s4 = s1.subtracting(s2)
  expectEqual(s1, s4)
  expectEqual(identity1, s1._rawIdentifier())
  expectEqual(identity1, s4._rawIdentifier())

  // Subtracting a superset will leave the set empty
  let s5 = s1.subtracting(s3)
  expectTrue(s5.isEmpty)
  expectEqual(identity1, s1._rawIdentifier())
  expectNotEqual(identity1, s5._rawIdentifier())

  // Subtracting the empty set does nothing
  expectEqual(s1, s1.subtracting(Set<Int>()))
  expectEqual(Set<Int>(), Set<Int>().subtracting(s1))
  expectEqual(identity1, s1._rawIdentifier())
}

SetTestSuite.test("subtract")
  .code {
  var s1 = Set([1010, 2020, 3030, 4040, 5050, 6060])
  let s2 = Set([1010, 2020, 3030])
  let s3 = Set([4040, 5050, 6060])

  let identity1 = s1._rawIdentifier()

  s1.subtract(Set<Int>())
  expectEqual(identity1, s1._rawIdentifier())

  s1.subtract(s3)
  expectEqual(identity1, s1._rawIdentifier())

  expectEqual(s1, s2)
  expectEqual(identity1, s1._rawIdentifier())
}

SetTestSuite.test("intersect") {
  let s1 = Set([1010, 2020, 3030])
  let s2 = Set([1010, 2020, 3030, 4040, 5050, 6060])

  let identity1 = s1._rawIdentifier()
  expectEqual(Set([1010, 2020, 3030]),
    Set([1010, 2020, 3030]).intersection(Set([1010, 2020, 3030])) as Set<Int>)
  expectEqual(identity1, s1._rawIdentifier())

  expectEqual(s1, s1.intersection(s2))
  expectEqual(identity1, s1._rawIdentifier())

  expectEqual(Set<Int>(), Set<Int>().intersection(Set<Int>()))
  expectEqual(Set<Int>(), s1.intersection(Set<Int>()))
  expectEqual(Set<Int>(), Set<Int>().intersection(s1))
}

SetTestSuite.test("formIntersection") {
  var s1 = Set([1010, 2020, 3030])
  let s2 = Set([4040, 5050, 6060])
  var s3 = Set([1010, 2020, 3030, 4040, 5050, 6060])
  var s4 = Set([1010, 2020, 3030])

  let identity1 = s1._rawIdentifier()
  s1.formIntersection(s4)
  expectEqual(s1, s4)
  expectEqual(identity1, s1._rawIdentifier())

  s4.formIntersection(s2)
  expectEqual(Set<Int>(), s4)

  let identity2 = s3._rawIdentifier()
  s3.formIntersection(s2)
  expectEqual(s3, s2)
  expectTrue(s1.isDisjoint(with: s3))
  expectNotEqual(identity2, s3._rawIdentifier())

  var s5 = Set<Int>()
  s5.formIntersection(s5)
  expectEqual(s5, Set<Int>())
  s5.formIntersection(s1)
  expectEqual(s5, Set<Int>())
}

SetTestSuite.test("symmetricDifference") {

  // Overlap with 4040, 5050, 6060
  let s1 = Set([1010, 2020, 3030, 4040, 5050, 6060])
  let s2 = Set([4040, 5050, 6060, 7070, 8080, 9090])
  let result = Set([1010, 2020, 3030, 7070, 8080, 9090])
  let universe = Set([1010, 2020, 3030, 4040, 5050, 6060,
                       7070, 8080, 9090])

  let identity1 = s1._rawIdentifier()

  let s3 = s1.symmetricDifference(s2)

  expectEqual(identity1, s1._rawIdentifier())

  expectEqual(s3, result)

  expectEqual(s1.symmetricDifference(s2),
    s1.union(s2).intersection(universe.subtracting(s1.intersection(s2))))

  expectEqual(s1.symmetricDifference(s2),
    s1.intersection(universe.subtracting(s2)).union(universe.subtracting(s1).intersection(s2)))

  expectTrue(s1.symmetricDifference(s1).isEmpty)
}

SetTestSuite.test("formSymmetricDifference")
  .code {
  // Overlap with 4040, 5050, 6060
  var s1 = Set([1010, 2020, 3030, 4040, 5050, 6060])
  let s1_copy = s1
  let s2 = Set([1010])
  let result = Set([2020, 3030, 4040, 5050, 6060])

  // s1 ⨁ s2 == result
  let identity1 = s1._rawIdentifier()
  s1.formSymmetricDifference(s2)

  // COW should trigger a copy
  expectNotEqual(identity1, s1._rawIdentifier())

  expectEqual(s1, result)
  expectEqual(s1_copy, Set([1010, 2020, 3030, 4040, 5050, 6060]))

  // A ⨁ A == {}
  s1.formSymmetricDifference(s1)
  expectTrue(s1.isEmpty)

  // Removing all elements should cause an identity change
  expectNotEqual(identity1, s1._rawIdentifier())

  // Without a fixLifetime here, the (non-ObjC) optimizer destroys
  // 's1' after its last uses and recycles the storage so the identity
  // check above fails.
  _fixLifetime(s1_copy)
}

SetTestSuite.test("removeFirst") {
  var s1 = Set([1010, 2020, 3030])
  let s2 = s1
  let empty = Set<Int>()

  let a1 = s1.removeFirst()

  expectFalse(s1.contains(a1))
  expectTrue(s2.contains(a1))
  expectNotEqual(s1._rawIdentifier(), s2._rawIdentifier())
  expectTrue(s1.isSubset(of: s2))
  expectNil(empty.first)
}

SetTestSuite.test("remove(member)")
  .code {

  let s1 : Set<TestKeyTy> = [1010, 2020, 3030]
  var s2 = Set<TestKeyTy>(minimumCapacity: 10)
  for i in [1010, 2020, 3030] {
    s2.insert(TestKeyTy(i))
  }
  let identity1 = s2._rawIdentifier()
  
  // remove something that's not there.
  let fortyForty = s2.remove(4040)
  expectEqual(s2, s1)
  expectNil(fortyForty)
  expectEqual(identity1, s2._rawIdentifier())

  // Remove things that are there.
  let thirtyThirty = s2.remove(3030)
  expectEqual(3030, thirtyThirty)
  expectEqual(identity1, s2._rawIdentifier())

  s2.remove(2020)
  expectEqual(identity1, s2._rawIdentifier())

  s2.remove(1010)
  expectEqual(identity1, s2._rawIdentifier())
  expectEqual(Set(), s2)
  expectTrue(s2.isEmpty)
}

SetTestSuite.test("contains") {
  let s1 = Set([1010, 2020, 3030])
  expectTrue(s1.contains(1010))
  expectFalse(s1.contains(999))
}

SetTestSuite.test("memberAtIndex") {
  let s1 = Set([1010, 2020, 3030])

  let foundIndex = s1.firstIndex(of: 1010)!
  expectEqual(1010, s1[foundIndex])
}

SetTestSuite.test("first") {
  let s1 = Set([1010, 2020, 3030])
  let emptySet = Set<Int>()

  expectTrue(s1.contains(s1.first!))
  expectNil(emptySet.first)
}

SetTestSuite.test("capacity/init(minimumCapacity:)") {
  let s0 = Set<String>(minimumCapacity: 0)
  expectGE(s0.capacity, 0)

  let s1 = Set<String>(minimumCapacity: 1)
  expectGE(s1.capacity, 1)

  let s3 = Set<String>(minimumCapacity: 3)
  expectGE(s3.capacity, 3)

  let s4 = Set<String>(minimumCapacity: 4)
  expectGE(s4.capacity, 4)

  let s10 = Set<String>(minimumCapacity: 10)
  expectGE(s10.capacity, 10)

  let s100 = Set<String>(minimumCapacity: 100)
  expectGE(s100.capacity, 100)

  let s1024 = Set<String>(minimumCapacity: 1024)
  expectGE(s1024.capacity, 1024)
}

SetTestSuite.test("capacity/reserveCapacity(_:)") {
  var s1: Set = [10, 20, 30]
  expectEqual(3, s1.capacity)
  s1.insert(40)
  expectEqual(6, s1.capacity)

  // Reserving new capacity jumps up to next limit.
  s1.reserveCapacity(7)
  expectEqual(12, s1.capacity)

  // Can reserve right up to a limit.
  s1.reserveCapacity(24)
  expectEqual(24, s1.capacity)

  // Fill up to the limit, no reallocation.
  s1.formUnion(stride(from: 50, through: 240, by: 10))
  expectEqual(24, s1.count)
  expectEqual(24, s1.capacity)
  s1.insert(250)
  expectEqual(48, s1.capacity)
}

SetTestSuite.test("isEmpty") {
  let s1 = Set([1010, 2020, 3030])
  expectFalse(s1.isEmpty)

  let emptySet = Set<Int>()
  expectTrue(emptySet.isEmpty)
}

#if _runtime(_ObjC)
@objc
class MockSetWithCustomCount : NSSet {
  init(count: Int) {
    self._count = count
    super.init()
  }

  override init() {
    expectUnreachable()
    super.init()
  }

  override init(objects: UnsafePointer<AnyObject>?, count: Int) {
    expectUnreachable()
    super.init(objects: objects, count: count)
  }

  required init(coder aDecoder: NSCoder) {
    fatalError("init(coder:) not implemented by MockSetWithCustomCount")
  }

  @objc(copyWithZone:)
  override func copy(with zone: NSZone?) -> Any {
    // Ensure that copying this set produces an object of the same
    // dynamic type.
    return self
  }

  override func member(_ object: Any) -> Any? {
    expectUnreachable()
    return object
  }

  override func objectEnumerator() -> NSEnumerator {
    expectUnreachable()
    return getAsNSSet([1010, 1020, 1030]).objectEnumerator()
  }

  override var count: Int {
    MockSetWithCustomCount.timesCountWasCalled += 1
    return _count
  }

  var _count: Int = 0

  static var timesCountWasCalled = 0
}

func getMockSetWithCustomCount(count: Int)
  -> Set<NSObject> {

  return MockSetWithCustomCount(count: count) as Set
}

func callGenericIsEmpty<C : Collection>(_ collection: C) -> Bool {
  return collection.isEmpty
}

SetTestSuite.test("isEmpty/ImplementationIsCustomized") {
  do {
    let d = getMockSetWithCustomCount(count: 0)
    MockSetWithCustomCount.timesCountWasCalled = 0
    expectTrue(d.isEmpty)
    expectEqual(1, MockSetWithCustomCount.timesCountWasCalled)
  }
  do {
    let d = getMockSetWithCustomCount(count: 0)
    MockSetWithCustomCount.timesCountWasCalled = 0
    expectTrue(callGenericIsEmpty(d))
    expectEqual(1, MockSetWithCustomCount.timesCountWasCalled)
  }

  do {
    let d = getMockSetWithCustomCount(count: 4)
    MockSetWithCustomCount.timesCountWasCalled = 0
    expectFalse(d.isEmpty)
    expectEqual(1, MockSetWithCustomCount.timesCountWasCalled)
  }
  do {
    let d = getMockSetWithCustomCount(count: 4)
    MockSetWithCustomCount.timesCountWasCalled = 0
    expectFalse(callGenericIsEmpty(d))
    expectEqual(1, MockSetWithCustomCount.timesCountWasCalled)
  }
}
#endif // _runtime(_ObjC)

SetTestSuite.test("count") {
  let s1 = Set([1010, 2020, 3030])
  expectEqual(0, Set<Int>().count)
  expectEqual(3, s1.count)
}

SetTestSuite.test("contains") {
  let s1 = Set([1010, 2020, 3030, 4040, 5050, 6060])
  expectTrue(s1.contains(1010))
  expectFalse(s1.contains(999))
  expectFalse(Set<Int>().contains(1010))
}

SetTestSuite.test("_customContainsEquatableElement") {
  let s1 = Set([1010, 2020, 3030, 4040, 5050, 6060])
  expectTrue(s1._customContainsEquatableElement(1010)!)
  expectFalse(s1._customContainsEquatableElement(999)!)
  expectFalse(Set<Int>()._customContainsEquatableElement(1010)!)
}

SetTestSuite.test("firstIndex(of:)") {
  let s1 = Set([1010, 2020, 3030, 4040, 5050, 6060])
  let foundIndex1 = s1.firstIndex(of: 1010)!
  expectEqual(1010, s1[foundIndex1])

  expectNil(s1.firstIndex(of: 999))
}

SetTestSuite.test("popFirst") {
  // Empty
  do {
    var s = Set<Int>()
    let popped = s.popFirst()
    expectNil(popped)
    expectTrue(s.isEmpty)
  }

  do {
    var popped = [Int]()
    var s = Set([1010, 2020, 3030])
    let expected = [1010, 2020, 3030]
    while let element = s.popFirst() {
      popped.append(element)
    }
    // Note that removing an element may reorder remaining items, so we
    // can't compare ordering here.
    popped.sort()
    expectEqualSequence(expected, popped)
    expectTrue(s.isEmpty)
  }
}

SetTestSuite.test("removeAt") {
  // Test removing from the startIndex, the middle, and the end of a set.
  for i in 1...3 {
    var s = Set<Int>([1010, 2020, 3030])
    let removed = s.remove(at: s.firstIndex(of: i*1010)!)
    expectEqual(i*1010, removed)
    expectEqual(2, s.count)
    expectNil(s.firstIndex(of: i*1010))
    let origKeys: [Int] = [1010, 2020, 3030]
    expectEqual(origKeys.filter { $0 != (i*1010) }, [Int](s).sorted())
  }
}

SetTestSuite.test("_customIndexOfEquatableElement") {
  let s1 = Set([1010, 2020, 3030, 4040, 5050, 6060])
  let foundIndex1 = s1._customIndexOfEquatableElement(1010)!!
  expectEqual(1010, s1[foundIndex1])

  expectNil(s1._customIndexOfEquatableElement(999)!)
}

SetTestSuite.test("commutative") {
  let s1 = Set([1010, 2020, 3030])
  let s2 = Set([2020, 3030])
  expectTrue(equalsUnordered(s1.intersection(s2), s2.intersection(s1)))
  expectTrue(equalsUnordered(s1.union(s2), s2.union(s1)))
}

SetTestSuite.test("associative") {
  let s1 = Set([1010, 2020, 3030])
  let s2 = Set([2020, 3030])
  let s3 = Set([1010, 2020, 3030])
  let s4 = Set([2020, 3030])
  let s5 = Set([7070, 8080, 9090])

  expectTrue(equalsUnordered(s1.intersection(s2).intersection(s3),
    s1.intersection(s2.intersection(s3))))
  expectTrue(equalsUnordered(s3.union(s4).union(s5), s3.union(s4.union(s5))))
}

SetTestSuite.test("distributive") {
  let s1 = Set([1010])
  let s2 = Set([1010, 2020, 3030, 4040, 5050, 6060])
  let s3 = Set([2020, 3030])
  expectTrue(equalsUnordered(s1.union(s2.intersection(s3)),
    s1.union(s2).intersection(s1.union(s3))))

  let s4 = Set([2020, 3030])
  expectTrue(equalsUnordered(s4.intersection(s1.union(s3)),
    s4.intersection(s1).union(s4.intersection(s3))))
}

SetTestSuite.test("idempotent") {
  let s1 = Set([1010, 2020, 3030, 4040, 5050, 6060])
  expectTrue(equalsUnordered(s1, s1.intersection(s1)))
  expectTrue(equalsUnordered(s1, s1.union(s1)))
}

SetTestSuite.test("absorption") {
  let s1 = Set([1010, 2020, 3030])
  let s2 = Set([4040, 5050, 6060])
  let s3 = Set([2020, 3030])
  expectTrue(equalsUnordered(s1, s1.union(s1.intersection(s2))))
  expectTrue(equalsUnordered(s1, s1.intersection(s1.union(s3))))
}

SetTestSuite.test("misc") {
  // Set with other types
  do {
    var s = Set([1.1, 2.2, 3.3])
    s.insert(4.4)
    expectTrue(s.contains(1.1))
    expectTrue(s.contains(2.2))
    expectTrue(s.contains(3.3))
  }

  do {
    let s = Set(["Hello", "world"])
    expectTrue(s.contains("Hello"))
    expectTrue(s.contains("world"))
  }
}

SetTestSuite.test("Hashable") {
  let s1 = Set([1010])
  let s2 = Set([2020])
  checkHashable([s1, s2], equalityOracle: { $0 == $1 })

  // Explicit types help the type checker quite a bit.
  let ss1 = Set([Set([1010] as [Int]), Set([2020] as [Int]), Set([3030] as [Int])])
  let ss11 = Set([Set([2020] as [Int]), Set([3030] as [Int]), Set([2020] as [Int])])
  let ss2 = Set([Set([9090] as [Int])])
  checkHashable([ss1, ss11, ss2], equalityOracle: { $0 == $1 })

  // Set should hash itself in a way that ensures instances get correctly
  // delineated even when they are nested in other commutative collections.
  // These are different Sets, so they should produce different hashes:
  let remix: [Set<Set<Int>>] = [
    [[1, 2], [3, 4]],
    [[1, 3], [2, 4]],
    [[1, 4], [2, 3]],
  ]
  checkHashable(remix, equalityOracle: { $0 == $1 })

  // Set ordering is not guaranteed to be consistent across equal instances. In
  // particular, ordering is highly sensitive to the size of the allocated
  // storage buffer.
  var variants: [Set<Int>] = []
  for i in 4 ..< 12 {
    var set: Set<Int> = [1, 2, 3, 4, 5, 6]
    set.reserveCapacity(1 << i)
    variants.append(set)
  }
  checkHashable(variants, equalityOracle: { _, _ in true })
}

//===---
// Check that iterators traverse a snapshot of the collection.
//===---

SetTestSuite.test("mutationDoesNotAffectIterator/remove,1") {
  var set = Set([1010, 1020, 1030])
  let iter = set.makeIterator()
  expectEqual(1010, set.remove(1010))

  expectEqualsUnordered([1010, 1020, 1030], Array(IteratorSequence(iter)))
}

SetTestSuite.test("mutationDoesNotAffectIterator/remove,all") {
  var set = Set([1010, 1020, 1030])
  let iter = set.makeIterator()
  expectEqual(1010, set.remove(1010))
  expectEqual(1020, set.remove(1020))
  expectEqual(1030, set.remove(1030))

  expectEqualsUnordered([1010, 1020, 1030], Array(IteratorSequence(iter)))
}

SetTestSuite.test("mutationDoesNotAffectIterator/removeAll,keepingCapacity=false") {
  var set = Set([1010, 1020, 1030])
  let iter = set.makeIterator()
  set.removeAll(keepingCapacity: false)

  expectEqualsUnordered([1010, 1020, 1030], Array(IteratorSequence(iter)))
}

SetTestSuite.test("mutationDoesNotAffectIterator/removeAll,keepingCapacity=true") {
  var set = Set([1010, 1020, 1030])
  let iter = set.makeIterator()
  set.removeAll(keepingCapacity: true)

  expectEqualsUnordered([1010, 1020, 1030], Array(IteratorSequence(iter)))
}

//===---
// Check SetAlgebra conformance
//===---

// Make sure Set conforms to the SetAlgebra protocol

SetTestSuite.test("SetAlgebra.conformance") {
  func ensureConformance<T: SetAlgebra>(_ s: T) {
    expectFalse(s.isEmpty)
  }
  
  let s: Set<Int> = [1,2,3,4,5]
  ensureConformance(s)
}

// Test isEmpty

SetTestSuite.test("SetAlgebra.IsEmpty.SingleEntry") {
  let s = Set<Int>([1050])
  
  expectFalse(s.isEmpty)
}

SetTestSuite.test("SetAlgebra.IsEmpty.MultipleEntries") {
  let s: Set<Int> = [1010, 1020, 1030, 1040, 1050]
  
  expectFalse(s.isEmpty)
}

SetTestSuite.test("SetAlgebra.IsEmpty.EmptySet") {
  var s: Set<Int> = []
  
  expectTrue(s.isEmpty)
  
  s.insert(1010)
  
  expectFalse(s.isEmpty)
}

// Test equality operator

SetTestSuite.test("SetAlgebra.==.SingleEntry") {
  let s1 = Set<Int>([1010])
  let s2 = Set<Int>([1010])
  let s3: Set<Int> = [1010, 1020, 1030]

  expectEqual(s1, s2)
  expectNotEqual(s1, s3)
}

SetTestSuite.test("SetAlgebra.==.MultipleEntries") {
  let s1: Set<Int> = [1010, 1020, 1030]
  let s2: Set<Int> = [1010, 1020, 1030]
  let s3: Set<Int> = [1030, 1040, 1050]

  expectEqual(s1, s2)
  expectNotEqual(s1, s3)
}

SetTestSuite.test("SetAlgebra.==.EmptySet") {
  let s1: Set<Int> = []
  let s2: Set<Int> = []
  let s3: Set<Int> = [1010, 1020, 1030]

  expectEqual(s1, s2)
  expectNotEqual(s1, s3)
}

// Test contains()

SetTestSuite.test("SetAlgebra.Contains.SingleEntry") {
  let s = Set<Int>([1050])
  
  expectFalse(s.contains(1010))
  expectTrue(s.contains(1050))
}

SetTestSuite.test("SetAlgebra.Contains.MultipleEntries") {
  let s: Set<Int> = [1010, 1020, 1030, 1040, 1050]

  expectFalse(s.contains(1060))
  expectFalse(s.contains(1070))
  expectTrue(s.contains(1010))
  expectTrue(s.contains(1020))
  expectTrue(s.contains(1030))
  expectTrue(s.contains(1040))
  expectTrue(s.contains(1050))
}

SetTestSuite.test("SetAlgebra.Contains.EmptySet") {
  let s: Set<Int> = []
  
  expectFalse(s.contains(1010))
  expectFalse(s.contains(1020))
  expectFalse(s.contains(1030))
  expectFalse(s.contains(1040))
  expectFalse(s.contains(1050))
  expectFalse(s.contains(1060))
  expectFalse(s.contains(1070))
}

// Test formIntersection()

SetTestSuite.test("SetAlgebra.FormIntersection.SingleEntry") {
  do {
    var s1 = Set<Int>([1010])
    let s2: Set<Int> = [1010, 1020, 1030]
  
    s1.formIntersection(s2)
    
    expectTrue(s1.contains(1010))
    expectFalse(s1.contains(1020))
    expectFalse(s1.contains(1070))
  }
  do {
    var s1 = Set<Int>([1010])
    let s2: Set<Int> = [1020, 1030, 1050]
  
    s1.formIntersection(s2)
    
    expectFalse(s1.contains(1010))
    expectFalse(s1.contains(1020))
    expectFalse(s1.contains(1070))
  }
}

SetTestSuite.test("SetAlgebra.FormIntersection.MultipleEntries") {
  do {
    var s1: Set<Int> = [1010, 1020, 1030]
    let s2: Set<Int> = [1030, 1040, 1050]
  
    s1.formIntersection(s2)
    
    expectTrue(s1.contains(1030))
    expectFalse(s1.contains(1020))
    expectFalse(s1.contains(1070))
  }
  do {
    var s1: Set<Int> = [1010, 1020, 1030]
    let s2: Set<Int> = [1040, 1050, 1060]
  
    s1.formIntersection(s2)
    
    expectFalse(s1.contains(1030))
    expectFalse(s1.contains(1040))
    expectFalse(s1.contains(1070))
  }
}

SetTestSuite.test("SetAlgebra.FormIntersection.EmptySet") {
  var s1: Set<Int> = []
  let s2: Set<Int> = [1010, 1020, 1030]

  s1.formIntersection(s2)
  
  expectFalse(s1.contains(1030))
  expectFalse(s1.contains(1040))
}

// Test formSymmetricDifference()

SetTestSuite.test("SetAlgebra.FormSymmetricDifference.SingleEntry") {
  do {
    var s1 = Set<Int>([1010])
    let s2: Set<Int> = [1010, 1020, 1030]
  
    s1.formSymmetricDifference(s2)

    expectTrue(s1.contains(1020))
    expectFalse(s1.contains(1010))
    expectFalse(s1.contains(1070))
  }
  do {
    var s1 = Set<Int>([1010])
    let s2: Set<Int> = [1020, 1030, 1050]
  
    s1.formSymmetricDifference(s2)
    
    expectTrue(s1.contains(1010))
    expectTrue(s1.contains(1020))
    expectFalse(s1.contains(1070))
  }
}

SetTestSuite.test("SetAlgebra.FormSymmetricDifference.MultipleEntries") {
  do {
    var s1: Set<Int> = [1010, 1020, 1030]
    let s2: Set<Int> = [1030, 1040, 1050]
  
    s1.formSymmetricDifference(s2)

    expectTrue(s1.contains(1020))
    expectFalse(s1.contains(1030))
    expectFalse(s1.contains(1070))
  }
  do {
    var s1: Set<Int> = [1010, 1020, 1030]
    let s2: Set<Int> = [1040, 1050, 1060]
  
    s1.formSymmetricDifference(s2)
    expectTrue(s1.contains(1030))
    expectTrue(s1.contains(1040))
    expectFalse(s1.contains(1070))
  }
}

SetTestSuite.test("SetAlgebra.FormSymmetricDifference.EmptySet") {
  var s1: Set<Int> = []
  let s2: Set<Int> = [1010, 1020, 1030]

  s1.formSymmetricDifference(s2)
  expectTrue(s1.contains(1030))
  expectFalse(s1.contains(1040))
}

// Test formUnion()

SetTestSuite.test("SetAlgebra.FormUnion.SingleEntry") {
  do {
    var s1 = Set<Int>([1010])
    let s2: Set<Int> = [1010, 1020, 1030]
  
    s1.formUnion(s2)
    expectTrue(s1.contains(1010))
    expectTrue(s1.contains(1020))
    expectFalse(s1.contains(1070))
  }
  do {
    var s1 = Set<Int>([1010])
    let s2: Set<Int> = [1020, 1030, 1050]
  
    s1.formUnion(s2)
    expectTrue(s1.contains(1010))
    expectTrue(s1.contains(1020))
    expectFalse(s1.contains(1070))
  }
}

SetTestSuite.test("SetAlgebra.FormUnion.MultipleEntries") {
  do {
    var s1: Set<Int> = [1010, 1020, 1030]
    let s2: Set<Int> = [1030, 1040, 1050]
  
    s1.formUnion(s2)
    expectTrue(s1.contains(1030))
    expectTrue(s1.contains(1020))
    expectFalse(s1.contains(1070))
  }
  do {
    var s1: Set<Int> = [1010, 1020, 1030]
    let s2: Set<Int> = [1040, 1050, 1060]
  
    s1.formUnion(s2)
    expectTrue(s1.contains(1030))
    expectTrue(s1.contains(1040))
    expectFalse(s1.contains(1070))
  }
}

SetTestSuite.test("SetAlgebra.FormUnion.EmptySet") {
  var s1: Set<Int> = []
  let s2: Set<Int> = [1010, 1020, 1030]

  s1.formUnion(s2)
  expectTrue(s1.contains(1030))
  expectFalse(s1.contains(1040))
}

// Test insert()

SetTestSuite.test("SetAlgebra.Insert.SingleEntry") {
  var s = Set<Int>([1010])
  
  expectFalse(s.contains(1020))
  
  let (inserted1, member1) = s.insert(1020)

  expectTrue(s.contains(1010))
  expectTrue(s.contains(1020))
  expectFalse(s.contains(1070))
  expectTrue(inserted1)
  expectEqual(1020, member1)
  
  let (inserted2, member2) = s.insert(1020)
  expectFalse(inserted2)
  expectEqual(1020, member2)
}

SetTestSuite.test("SetAlgebra.Insert.MultipleEntries") {
  var s: Set<Int> = [1010, 1020, 1030]
  
  expectFalse(s.contains(1050))
  
  let (inserted1, member1) = s.insert(1050)
  
  expectTrue(s.contains(1010))
  expectTrue(s.contains(1050))
  expectFalse(s.contains(1070))
  expectTrue(inserted1)
  expectEqual(1050, member1)
  
  let (inserted2, member2) = s.insert(1050)
  expectFalse(inserted2)
  expectEqual(1050, member2)
}

SetTestSuite.test("SetAlgebra.Insert.EmptySet") {
  var s: Set<Int> = []

  expectFalse(s.contains(1010))
  
  let (inserted1, member1) = s.insert(1010)
  
  expectTrue(s.contains(1010))
  expectTrue(inserted1)
  expectEqual(1010, member1)
  
  let (inserted2, member2) = s.insert(1010)
  expectFalse(inserted2)
  expectEqual(1010, member2)
}

// Test intersection()

SetTestSuite.test("SetAlgebra.Intersection.SingleEntry") {
  do {
    let s1 = Set<Int>([1010])
    let s2: Set<Int> = [1010, 1020, 1030]
  
    let intersection = s1.intersection(s2)
    expectTrue(intersection.contains(1010))
    expectFalse(intersection.contains(1020))
    expectFalse(intersection.contains(1070))
  }
  do {
    let s1 = Set<Int>([1010])
    let s2: Set<Int> = [1020, 1030, 1050]
  
    let intersection = s1.intersection(s2)
    expectFalse(intersection.contains(1010))
    expectFalse(intersection.contains(1020))
    expectFalse(intersection.contains(1070))
  }
}

SetTestSuite.test("SetAlgebra.Intersection.MultipleEntries") {
  do {
    let s1: Set<Int> = [1010, 1020, 1030]
    let s2: Set<Int> = [1030, 1040, 1050]
  
    let intersection = s1.intersection(s2)
    expectTrue(intersection.contains(1030))
    expectFalse(intersection.contains(1020))
    expectFalse(intersection.contains(1070))
  }
  do {
    let s1: Set<Int> = [1010, 1020, 1030]
    let s2: Set<Int> = [1040, 1050, 1060]
  
    let intersection = s1.intersection(s2)
    expectFalse(intersection.contains(1030))
    expectFalse(intersection.contains(1040))
    expectFalse(intersection.contains(1070))
  }
}

SetTestSuite.test("SetAlgebra.Intersection.EmptySet") {
  let s1: Set<Int> = []
  let s2: Set<Int> = [1010, 1020, 1030]

  let intersection = s1.intersection(s2)
  expectFalse(intersection.contains(1030))
  expectFalse(intersection.contains(1040))
}

// Test isDisjointUnion(with:)

SetTestSuite.test("SetAlgebra.IsDisjointWith.SingleEntry") {
  let s1 = Set<Int>([1010])
  let s2: Set<Int> = [1010, 1020, 1030]
  let s3: Set<Int> = [1020, 1030]

  expectFalse(s1.isDisjoint(with: s2))
  expectTrue(s1.isDisjoint(with: s3))
}

SetTestSuite.test("SetAlgebra.IsDisjointWith.MultipleEntries") {
  let s1: Set<Int> = [1010, 1020, 1030]
  let s2: Set<Int> = [1020, 1030]
  let s3: Set<Int> = [1040, 1050, 1060]

  expectFalse(s1.isDisjoint(with: s2))
  expectTrue(s1.isDisjoint(with: s3))
}

SetTestSuite.test("SetAlgebra.IsDisjointWith.EmptySet") {
  let s1: Set<Int> = []
  let s2: Set<Int> = [1020, 1030]

  expectTrue(s1.isDisjoint(with: s2))
}

// Test isSubset(of:)

SetTestSuite.test("SetAlgebra.IsSubsetOf.SingleEntry") {
  let s1 = Set<Int>([1010])
  let s2: Set<Int> = [1010, 1020, 1030]
  let s3: Set<Int> = [1020, 1030]

  expectTrue(s1.isSubset(of: s2))
  expectFalse(s1.isSubset(of: s3))
}

SetTestSuite.test("SetAlgebra.IsSubsetOf.MultipleEntries") {
  let s1: Set<Int> = [1010, 1020]
  let s2: Set<Int> = [1010, 1020, 1030]
  let s3: Set<Int> = [1040, 1050, 1060]

  expectTrue(s1.isSubset(of: s2))
  expectFalse(s1.isSubset(of: s3))
}

SetTestSuite.test("SetAlgebra.IsSubsetOf.EmptySet") {
  let s1: Set<Int> = []
  let s2: Set<Int> = [1020, 1030]

  expectTrue(s1.isSubset(of: s2))
}

// Test isSuperset(of:)

SetTestSuite.test("SetAlgebra.IsSupersetOf.SingleEntry") {
  let s1 = Set<Int>([1010])
  let s2: Set<Int> = [1010]
  let s3: Set<Int> = [1020, 1030]
  let s4: Set<Int> = []

  expectTrue(s1.isSuperset(of: s2))
  expectFalse(s1.isSuperset(of: s3))
  expectTrue(s1.isSuperset(of: s4))
}

SetTestSuite.test("SetAlgebra.IsSupersetOf.MultipleEntries") {
  let s1: Set<Int> = [1010, 1020]
  let s2: Set<Int> = [1010, 1020]
  let s3: Set<Int> = [1010]
  let s4: Set<Int> = [1040, 1050, 1060]

  expectTrue(s1.isSuperset(of: s2))
  expectTrue(s1.isSuperset(of: s3))
  expectFalse(s1.isSuperset(of: s4))
}

SetTestSuite.test("SetAlgebra.IsSupersetOf.EmptySet") {
  let s1: Set<Int> = []
  let s2: Set<Int> = [1020, 1030]
  let s3: Set<Int> = []

  expectFalse(s1.isSuperset(of: s2))
  expectTrue(s1.isSuperset(of: s3))
}

// Test filter(_:)

SetTestSuite.test("SetAlgebra.Filter.SingleEntry") {
  var s = Set<Int>([1010])

  expectTrue(s.contains(1010))

  let filter = s.filter { $0 > 1000 }

  expectTrue(s.contains(1010))
  expectTrue(filter.contains(1010))
}

SetTestSuite.test("SetAlgebra.Filter.MultipleEntries") {
  var s: Set<Int> = [1010, 1020, 1030]

  let filter = s.filter { $0 > 1010 }

  expectFalse(filter.contains(1010))
  expectTrue(filter.contains(1020))
  expectTrue(filter.contains(1030))
}

SetTestSuite.test("SetAlgebra.Filter.EmptySet") {
  var s: Set<Int> = []

  let filter = s.filter { $0 > 1000 }

  expectFalse(filter.contains(1010))
  expectTrue(filter.isEmpty)
}

// Test remove()

SetTestSuite.test("SetAlgebra.Remove.SingleEntry") {
  var s = Set<Int>([1010])
  
  expectTrue(s.contains(1010))
  
  let removed = s.remove(1010)

  expectFalse(s.contains(1010))
  expectFalse(s.contains(1070))
  expectEqual(1010, removed)
}

SetTestSuite.test("SetAlgebra.Remove.MultipleEntries") {
  var s: Set<Int> = [1010, 1020, 1030]
  
  expectTrue(s.contains(1020))
  
  let removed = s.remove(1020)
  
  expectTrue(s.contains(1010))
  expectFalse(s.contains(1020))
  expectFalse(s.contains(1070))
  expectEqual(1020, removed)
}

SetTestSuite.test("SetAlgebra.Remove.EmptySet") {
  var s: Set<Int> = []

  expectFalse(s.contains(1010))
  
  let removed = s.remove(1010)
  
  expectFalse(s.contains(1010))
  expectNil(removed)
}

// Test subtract()

SetTestSuite.test("SetAlgebra.Subtract.SingleEntry") {
  do {
    var s = Set<Int>([1010])
    
    s.subtract([1010])
    
    expectFalse(s.contains(1010))
  }
  do {
    var s = Set<Int>([1010])
    
    s.subtract([1020])
    
    expectTrue(s.contains(1010))
  }
}

SetTestSuite.test("SetAlgebra.Subtract.MultipleEntries") {
  do {
    var s: Set<Int> = [1010, 1020, 1030]
    
    s.subtract([1010])
    
    expectFalse(s.contains(1010))
    expectTrue(s.contains(1020))
  }
  do {
    var s: Set<Int> = [1010, 1020, 1030]
    
    s.subtract([1050])
    
    expectTrue(s.contains(1010))
    expectFalse(s.contains(1050))
  }
}

SetTestSuite.test("SetAlgebra.Subtract.EmptySet") {
  var s: Set<Int> = []
  
  s.subtract([1010])
  
  expectFalse(s.contains(1010))
  expectFalse(s.contains(1020))
}

// Test subtracting()

SetTestSuite.test("SetAlgebra.Subtracting.SingleEntry") {
  do {
    let s = Set<Int>([1010])
    
    let difference = s.subtracting([1010])
    
    expectFalse(difference.contains(1010))
  }
  do {
    let s = Set<Int>([1010])
    
    let difference = s.subtracting([1020])
    
    expectTrue(difference.contains(1010))
  }
}

SetTestSuite.test("SetAlgebra.Subtracting.MultipleEntries") {
  do {
    let s: Set<Int> = [1010, 1020, 1030]
    
    let difference = s.subtracting([1010])
    
    expectFalse(difference.contains(1010))
    expectTrue(difference.contains(1020))
  }
  do {
    let s: Set<Int> = [1010, 1020, 1030]
    
    let difference = s.subtracting([1050])
    
    expectTrue(difference.contains(1010))
    expectFalse(difference.contains(1050))
  }
}

SetTestSuite.test("SetAlgebra.Subtracting.EmptySet") {
  let s: Set<Int> = []
  
  let difference = s.subtracting([1010])
  
  expectFalse(difference.contains(1010))
  expectFalse(difference.contains(1020))
}

// Test symmetricDifference()

SetTestSuite.test("SetAlgebra.SymmetricDifference.SingleEntry") {
  do {
    let s1 = Set<Int>([1010])
    let s2: Set<Int> = [1010, 1020, 1030]
  
    let difference = s1.symmetricDifference(s2)
    expectFalse(difference.contains(1010))
    expectTrue(difference.contains(1020))
    expectFalse(difference.contains(1070))
  }
  do {
    let s1 = Set<Int>([1010])
    let s2: Set<Int> = [1020, 1030, 1050]
  
    let difference = s1.symmetricDifference(s2)
    expectTrue(difference.contains(1010))
    expectTrue(difference.contains(1020))
    expectFalse(difference.contains(1070))
  }
}

SetTestSuite.test("SetAlgebra.SymmetricDifference.MultipleEntries") {
  do {
    let s1: Set<Int> = [1010, 1020, 1030]
    let s2: Set<Int> = [1030, 1040, 1050]
  
    let difference = s1.symmetricDifference(s2)
    expectFalse(difference.contains(1030))
    expectTrue(difference.contains(1020))
    expectFalse(difference.contains(1070))
  }
  do {
    let s1: Set<Int> = [1010, 1020, 1030]
    let s2: Set<Int> = [1040, 1050, 1060]
  
    let difference = s1.symmetricDifference(s2)
    expectTrue(difference.contains(1030))
    expectTrue(difference.contains(1040))
    expectFalse(difference.contains(1070))
  }
}

SetTestSuite.test("SetAlgebra.SymmetricDifference.EmptySet") {
  let s1: Set<Int> = []
  let s2: Set<Int> = [1010, 1020, 1030]

  let difference = s1.symmetricDifference(s2)
  expectTrue(difference.contains(1030))
  expectFalse(difference.contains(1040))
}

// Test union()

SetTestSuite.test("SetAlgebra.Union.SingleEntry") {
  do {
    let s1 = Set<Int>([1010])
    let s2: Set<Int> = [1010, 1020, 1030]
  
    let union = s1.union(s2)
    expectTrue(union.contains(1010))
    expectTrue(union.contains(1020))
    expectFalse(union.contains(1070))
  }
  do {
    let s1 = Set<Int>([1010])
    let s2: Set<Int> = [1020, 1030, 1050]
  
    let union = s1.union(s2)
    expectTrue(union.contains(1010))
    expectTrue(union.contains(1020))
    expectFalse(union.contains(1070))
  }
}

SetTestSuite.test("SetAlgebra.Union.MultipleEntries") {
  do {
    let s1: Set<Int> = [1010, 1020, 1030]
    let s2: Set<Int> = [1030, 1040, 1050]
  
    let union = s1.union(s2)
    expectTrue(union.contains(1030))
    expectTrue(union.contains(1020))
    expectFalse(union.contains(1070))
  }
  do {
    let s1: Set<Int> = [1010, 1020, 1030]
    let s2: Set<Int> = [1040, 1050, 1060]
  
    let union = s1.union(s2)
    expectTrue(union.contains(1030))
    expectTrue(union.contains(1040))
    expectFalse(union.contains(1070))
  }
}

SetTestSuite.test("SetAlgebra.Union.EmptySet") {
  let s1: Set<Int> = []
  let s2: Set<Int> = [1010, 1020, 1030]

  let union = s1.union(s2)
  expectTrue(union.contains(1030))
  expectFalse(union.contains(1040))
}

// Test update(with:)

SetTestSuite.test("SetAlgebra.UpdateWith.SingleEntry") {
  var s = Set<Int>([1010])
  
  expectFalse(s.contains(1020))
  
  let member1 = s.update(with: 1020)

  expectTrue(s.contains(1010))
  expectTrue(s.contains(1020))
  expectFalse(s.contains(1070))
  expectNil(member1)
  
  let member2 = s.update(with: 1020)
  expectEqual(1020, member2)
}

SetTestSuite.test("SetAlgebra.UpdateWith.MultipleEntries") {
  var s: Set<Int> = [1010, 1020, 1030]
  
  expectFalse(s.contains(1050))
  
  let member1 = s.update(with: 1050)
  
  expectTrue(s.contains(1010))
  expectTrue(s.contains(1050))
  expectFalse(s.contains(1070))
  expectNil(member1)
  
  let member2 = s.update(with: 1050)
  expectEqual(1050, member2)
}

SetTestSuite.test("SetAlgebra.UpdateWith.EmptySet") {
  var s: Set<Int> = []

  expectFalse(s.contains(1010))
  
  let member1 = s.update(with: 1010)
  
  expectTrue(s.contains(1010))
  expectNil(member1)
  
  let member2 = s.update(with: 1010)
  expectEqual(1010, member2)
}

SetTestSuite.test("localHashSeeds") {
  // With global hashing, copying elements in hash order between hash tables
  // can become quadratic (see https://github.com/apple/swift/issues/45856).
  //
  // We defeat this by mixing the local storage capacity into the global hash
  // seed, thereby breaking the correlation between bucket indices across
  // hash tables with different sizes.
  //
  // Verify this works by copying a small sampling of elements near the
  // beginning of a large Set into a smaller one. If the elements end up in the
  // same order in the smaller Set, then that indicates we do not use
  // size-dependent seeding.

  let count = 100_000
  // Set a large table size to reduce frequency/length of collision chains.
  var large = Set<Int>(minimumCapacity: 4 * count)
  for i in 1 ..< count {
    large.insert(i)
  }

  let bunch = count / 100 // 1 percent's worth of elements

  // Copy two bunches of elements into another set that's half the size of the
  // first. We start after the initial bunch because the hash table may begin
  // with collided elements wrapped over from the end, and these would be sorted
  // into irregular slots in the smaller table.
  let slice = large.prefix(3 * bunch).dropFirst(bunch)
  var small = Set<Int>(minimumCapacity: large.capacity / 2)
  expectLT(small.capacity, large.capacity)
  for element in slice {
    small.insert(element)
  }

  // Compare the second halves of the new set and the slice.  Ignore the first
  // halves; the first few elements may not be in the correct order if we
  // happened to start copying from the middle of a collision chain.
  let smallElements = small.dropFirst(bunch)
  let sliceElements = slice.dropFirst(bunch)
  // If this test fails, there is a problem with local hash seeding.
  expectFalse(smallElements.elementsEqual(sliceElements))
}

SetTestSuite.test("RemoveAt.InvalidatesIndices") {
  var s: Set<Int> = [10, 20, 30, 40]
  let i = s.firstIndex(of: 20)!
  let j = s.firstIndex(of: 10)!

  s.remove(at: j)

  expectCrashLater()
  _ = s[i]
}

SetTestSuite.test("RemoveValueForKey.InvalidatesIndices") {
  var s: Set<Int> = [10, 20, 30, 40]
  let i = s.firstIndex(of: 20)!

  s.remove(10)

  expectCrashLater()
  _ = s[i]
}

SetTestSuite.test("ResizeOnInsertion.InvalidatesIndices") {
  var s: Set<Int> = [10, 20, 30, 40]
  let i = s.firstIndex(of: 20)!
  expectEqual(s[i], 20)

  for i in 0 ..< (s.capacity - s.count) {
    s.insert(100 + i)
  }
  expectEqual(s[i], 20)
  s.insert(0)

  expectCrashLater()
  _ = s[i]
}

SetTestSuite.test("ResizeOnUpdate.InvalidatesIndices") {
  var s: Set<Int> = [10, 20, 30, 40]
  let i = s.firstIndex(of: 20)!
  expectEqual(s[i], 20)

  for i in 0 ..< (s.capacity - s.count) {
    s.update(with: 100 + i)
  }
  expectEqual(s[i], 20)
  s.update(with: 0)

  expectCrashLater()
  _ = s[i]
}

SetTestSuite.test("RemoveAll.InvalidatesIndices") {
  var s: Set<Int> = [10, 20, 30, 40]
  let i = s.firstIndex(of: 20)!
  expectEqual(s[i], 20)

  s.removeAll(keepingCapacity: true)

  expectCrashLater()
  _ = s[i]
}

SetTestSuite.test("ReserveCapacity.InvalidatesIndices") {
  var s: Set<Int> = [10, 20, 30, 40]
  let i = s.firstIndex(of: 20)!
  expectEqual(s[i], 20)

  s.reserveCapacity(0)
  expectEqual(s[i], 20)

  s.reserveCapacity(s.capacity)
  expectEqual(s[i], 20)

  s.reserveCapacity(s.capacity * 10)

  expectCrashLater()
  _ = s[i]
}

SetTestSuite.test("IndexValidation.Subscript.Getter.AcrossInstances") {
  // The mutation count may happen to be the same across any two sets.
  // The probability of this is low, but it could happen -- so check a bunch of
  // these cases at once; a trap will definitely occur at least once.
  let sets = (0 ..< 10).map { _ in Set<Int>([10, 20, 30, 40]) }
  let indices = sets.map { $0.firstIndex(of: 20)! }
  let s: Set<Int> = [10, 20, 30, 40]

  expectCrashLater()
  for i in indices {
    _ = s[i]
  }
  _fixLifetime(sets)
}

SetTestSuite.test("IndexValidation.Subscript.Getter.AfterRemoval") {
  var s: Set<Int> = [10, 20, 30, 40]
  let i = s.firstIndex(of: 20)!
  expectEqual(s[i], 20)

  s.remove(10)

  expectCrashLater()
  _ = s[i]
}

SetTestSuite.test("IndexValidation.Subscript.Getter.AfterGrow") {
  var s: Set<Int> = [10, 20, 30, 40]
  let i = s.firstIndex(of: 20)!
  let identifier = s._rawIdentifier()
  expectEqual(s[i], 20)

  for i in 0 ..< (s.capacity - s.count) {
    s.insert(100 + i)
  }
  expectEqual(s._rawIdentifier(), identifier)
  expectEqual(s[i], 20)

  s.insert(0)

  expectNotEqual(s._rawIdentifier(), identifier)
  expectCrashLater()
  _ = s[i]
}

SetTestSuite.test("IndexValidation.RangeSubscript.AfterRemoval") {
  var s: Set<Int> = [10, 20, 30, 40]
  let i = s.firstIndex(of: 20)!
  let j = s.index(after: i)
  expectTrue(i < j)

  s.remove(40)

  expectTrue(i < j)
  expectCrashLater()
  _ = s[i..<j]
}

SetTestSuite.test("IndexValidation.RangeSubscript.AfterGrow") {
  var s: Set<Int> = [10, 20, 30, 40]
  let i = s.firstIndex(of: 20)!
  let j = s.index(after: i)
  expectTrue(i < j)
  let identifier = s._rawIdentifier()

  for i in 0 ..< (s.capacity - s.count) {
    s.insert(100 + i)
  }
  expectEqual(s._rawIdentifier(), identifier)
  expectTrue(i < j)

  s.insert(0)

  expectTrue(i < j)
  expectNotEqual(s._rawIdentifier(), identifier)
  expectCrashLater()
  _ = s[i..<j]
}

SetTestSuite.test("IndexValidation.RemoveAt.AfterRemoval") {
  var s: Set<Int> = [10, 20, 30, 40]
  let i = s.firstIndex(of: 20)!
  expectEqual(s[i], 20)

  s.remove(10)

  expectCrashLater()
  s.remove(at: i)
}

SetTestSuite.test("IndexValidation.RemoveAt.AfterGrow") {
  var s: Set<Int> = [10, 20, 30, 40]
  let i = s.firstIndex(of: 20)!
  let identifier = s._rawIdentifier()
  expectEqual(s[i], 20)

  for i in 0 ..< (s.capacity - s.count) {
    s.insert(100 + i)
  }
  expectEqual(s._rawIdentifier(), identifier)
  expectEqual(s[i], 20)

  s.insert(0)

  expectNotEqual(s._rawIdentifier(), identifier)
  expectCrashLater()
  s.remove(at: i)
}

#if _runtime(_ObjC)
if #available(SwiftStdlib 5.1, *) {
  // https://github.com/apple/swift/pull/23174
  SetTestSuite.test("ForcedNonverbatimBridge.Trap.String")
  .skip(.custom(
      { _isFastAssertConfiguration() },
      reason: "this trap is not guaranteed to happen in -Ounchecked"))
  .crashOutputMatches("Could not cast value of type")
  .code {
    let s1: NSSet = [
      "Gordon" as NSString,
      "William" as NSString,
      "Katherine" as NSString,
      "Lynn" as NSString,
      "Brian" as NSString,
      1756 as NSNumber]

    expectCrashLater()
    _ = s1 as! Set<String>
  }
}
#endif

#if _runtime(_ObjC)
if #available(SwiftStdlib 5.1, *) {
  // https://github.com/apple/swift/pull/23174
  SetTestSuite.test("ForcedNonverbatimBridge.Trap.Int")
  .skip(.custom(
    { _isFastAssertConfiguration() },
    reason: "this trap is not guaranteed to happen in -Ounchecked"))
  .crashOutputMatches("Could not cast value of type")
  .code {
    let s1: NSSet = [
      4 as NSNumber,
      8 as NSNumber,
      15 as NSNumber,
      16 as NSNumber,
      23 as NSNumber,
      42 as NSNumber,
      "John" as NSString]

    expectCrashLater()
    _ = s1 as! Set<Int>
  }
}
#endif

#if _runtime(_ObjC)
SetTestSuite.test("ForcedVerbatimBridge.Trap.NSString")
  .skip(.custom(
    { _isFastAssertConfiguration() },
    reason: "this trap is not guaranteed to happen in -Ounchecked"))
  .crashOutputMatches("Could not cast value of type")
  .code {

  let s1: NSSet = [
    "Gordon" as NSString,
    "William" as NSString,
    "Katherine" as NSString,
    "Lynn" as NSString,
    "Brian" as NSString,
    1756 as NSNumber]

  // With the ObjC runtime, the verbatim downcast is O(1); it performs no
  // runtime checks.
  let s2 = s1 as! Set<NSString>
  // Element access goes through the bridged path and performs forced downcasts.
  // This is where the odd numeric value is caught.
  expectCrashLater()
  for string in s2 {
    _ = string
  }
}
#endif

#if _runtime(_ObjC)
SetTestSuite.test("ForcedVerbatimBridge.Trap.NSNumber")
  .skip(.custom(
    { _isFastAssertConfiguration() },
    reason: "this trap is not guaranteed to happen in -Ounchecked"))
  .crashOutputMatches("Could not cast value of type")
  .code {
  let s1: NSSet = [
    4 as NSNumber,
    8 as NSNumber,
    15 as NSNumber,
    16 as NSNumber,
    23 as NSNumber,
    42 as NSNumber,
    "John" as NSString]
  // With the ObjC runtime, the verbatim downcast is O(1); it performs no
  // runtime checks.
  let s2 = s1 as! Set<NSNumber>
  // Element access goes through the bridged path and performs forced downcasts.
  // This is where the odd numeric value is caught.
  expectCrashLater()
  for string in s2 {
    _ = string
  }
}
#endif

#if _runtime(_ObjC)
SetTestSuite.test("ForcedVerbatimDowncast.Trap.String")
  .skip(.custom(
    { _isFastAssertConfiguration() },
    reason: "this trap is not guaranteed to happen in -Ounchecked"))
  .crashOutputMatches("Could not cast value of type")
  .code {
  let s1: Set<NSObject> = [
    "Gordon" as NSString,
    "William" as NSString,
    "Katherine" as NSString,
    "Lynn" as NSString,
    "Brian" as NSString,
    1756 as NSNumber]
  // With the ObjC runtime, the verbatim downcast is O(1); it performs no
  // runtime checks.
  let s2 = s1 as! Set<NSString>
  // Element access goes through the bridged path and performs forced downcasts.
  // This is where the odd numeric value is caught.
  expectCrashLater()
  for string in s2 {
    _ = string
  }
}
#endif

#if _runtime(_ObjC)
SetTestSuite.test("ForcedVerbatimDowncast.Trap.Int")
  .skip(.custom(
    { _isFastAssertConfiguration() },
    reason: "this trap is not guaranteed to happen in -Ounchecked"))
  .crashOutputMatches("Could not cast value of type")
  .code {
  let s1: Set<NSObject> = [
    4 as NSNumber,
    8 as NSNumber,
    15 as NSNumber,
    16 as NSNumber,
    23 as NSNumber,
    42 as NSNumber,
    "John" as NSString]
  // With the ObjC runtime, the verbatim downcast is O(1); it performs no
  // runtime checks.
  let s2 = s1 as! Set<NSNumber>
  expectCrashLater()
  // Element access goes through the bridged path and performs forced downcasts.
  // This is where the odd string value is caught.
  for string in s2 {
    _ = string
  }
}
#endif

#if _runtime(_ObjC)
if #available(SwiftStdlib 5.1, *) {
  // https://github.com/apple/swift/pull/23174
  SetTestSuite.test("ForcedBridgingNonverbatimDowncast.Trap.String")
  .skip(.custom(
    { _isFastAssertConfiguration() },
    reason: "this trap is not guaranteed to happen in -Ounchecked"))
  .crashOutputMatches("Could not cast value of type")
  .code {
    let s1: Set<NSObject> = [
      "Gordon" as NSString,
      "William" as NSString,
      "Katherine" as NSString,
      "Lynn" as NSString,
      "Brian" as NSString,
      1756 as NSNumber]
    expectCrashLater()
    // Nonverbatim downcasts are greedy and they trap immediately.
    let s2 = s1 as! Set<String>
    _ = s2.contains("Gordon")
  }
}
#endif

#if _runtime(_ObjC)
if #available(SwiftStdlib 5.1, *) {
  // https://github.com/apple/swift/pull/23174
  SetTestSuite.test("ForcedBridgingNonverbatimDowncast.Trap.Int")
  .skip(.custom(
    { _isFastAssertConfiguration() },
    reason: "this trap is not guaranteed to happen in -Ounchecked"))
  .crashOutputMatches("Could not cast value of type")
  .code {
    let s1: Set<NSObject> = [
      4 as NSNumber,
      8 as NSNumber,
      15 as NSNumber,
      16 as NSNumber,
      23 as NSNumber,
      42 as NSNumber,
      "John" as NSString]
    expectCrashLater()
    // Nonverbatim downcasts are greedy and they trap immediately.
    let s2 = s1 as! Set<Int>
    _ = s2.contains(23)
  }
}
#endif

#if _runtime(_ObjC)
if #available(SwiftStdlib 5.1, *) {
  // https://github.com/apple/swift/pull/23683
  SetTestSuite.test("Upcast.StringEqualityMismatch") {
    // Upcasting from NSString to String keys changes their concept of equality,
    // resulting in two equal keys, one of which should be discarded by the
    // downcast. (Along with its associated value.)
    // rdar://problem/35995647
    let s: Set<NSString> = [
      "cafe\u{301}",
      "café"
    ]
    expectEqual(s.count, 2)
    let s2 = s as Set<String>
    expectEqual(s2.count, 1)
  }
}
#endif

runAllTests()
