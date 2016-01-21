import Swift
import StdlibUnittest

func acceptsAnySet<T : Hashable>(s: Set<T>) {}

func acceptsAnyDictionary<KeyTy : Hashable, ValueTy>(
  d: Dictionary<KeyTy, ValueTy>) {
}

// Compare two arrays as sets.
func equalsUnordered<T : Comparable>(
  lhs: Array<(T, T)>, _ rhs: Array<(T, T)>
) -> Bool {
  func comparePair(lhs: (T, T), _ rhs: (T, T)) -> Bool {
    return [ lhs.0, lhs.1 ].lexicographicalCompare([ rhs.0, rhs.1 ])
  }
  return lhs.sort(comparePair).elementsEqual(rhs.sort(comparePair)) {
    (lhs: (T, T), rhs: (T, T)) -> Bool in
    lhs.0 == rhs.0 && lhs.1 == rhs.1
  }
}

func equalsUnordered<T : Comparable>(lhs: [T], _ rhs: [T]) -> Bool {
  return lhs.sort().elementsEqual(rhs.sort())
}

var _keyCount = _stdlib_AtomicInt(0)
var _keySerial = _stdlib_AtomicInt(0)

// A wrapper class that can help us track allocations and find issues with
// object lifetime.
class TestKeyTy : Equatable, Hashable, CustomStringConvertible {
  class var objectCount: Int {
    get {
      return _keyCount.load()
    }
    set {
      _keyCount.store(newValue)
    }
  }

  init(_ value: Int) {
    _keyCount.fetchAndAdd(1)
    serial = _keySerial.addAndFetch(1)
    self.value = value
    self._hashValue = value
  }

  convenience init(value: Int, hashValue: Int) {
    self.init(value)
    self._hashValue = hashValue
  }

  deinit {
    assert(serial > 0, "double destruction")
    _keyCount.fetchAndAdd(-1)
    serial = -serial
  }

  var description: String {
    assert(serial > 0, "dead TestKeyTy")
    return value.description
  }

  var hashValue: Int {
    return _hashValue
  }

  var value: Int
  var _hashValue: Int
  var serial: Int
}

func == (lhs: TestKeyTy, rhs: TestKeyTy) -> Bool {
  return lhs.value == rhs.value
}

var _valueCount = _stdlib_AtomicInt(0)
var _valueSerial = _stdlib_AtomicInt(0)

class TestValueTy : CustomStringConvertible {
  class var objectCount: Int {
    get {
      return _valueCount.load()
    }
    set {
      _valueCount.store(newValue)
    }
  }

  init(_ value: Int) {
    _valueCount.fetchAndAdd(1)
    serial = _valueSerial.addAndFetch(1)
    self.value = value
  }

  deinit {
    assert(serial > 0, "double destruction")
    _valueCount.fetchAndAdd(-1)
    serial = -serial
  }

  var description: String {
    assert(serial > 0, "dead TestValueTy")
    return value.description
  }

  var value: Int
  var serial: Int
}

var _equatableValueCount = _stdlib_AtomicInt(0)
var _equatableValueSerial = _stdlib_AtomicInt(0)

class TestEquatableValueTy : Equatable, CustomStringConvertible {
  class var objectCount: Int {
    get {
      return _equatableValueCount.load()
    }
    set {
      _equatableValueCount.store(newValue)
    }
  }

  init(_ value: Int) {
    _equatableValueCount.fetchAndAdd(1)
    serial = _equatableValueSerial.addAndFetch(1)
    self.value = value
  }

  deinit {
    assert(serial > 0, "double destruction")
    _equatableValueCount.fetchAndAdd(-1)
    serial = -serial
  }

  var description: String {
    assert(serial > 0, "dead TestEquatableValueTy")
    return value.description
  }

  var value: Int
  var serial: Int
}

func == (lhs: TestEquatableValueTy, rhs: TestEquatableValueTy) -> Bool {
  return lhs.value == rhs.value
}

func resetLeaksOfDictionaryKeysValues() {
  TestKeyTy.objectCount = 0
  TestValueTy.objectCount = 0
  TestEquatableValueTy.objectCount = 0
}

func expectNoLeaksOfDictionaryKeysValues() {
  expectEqual(0, TestKeyTy.objectCount, "TestKeyTy leak")
  expectEqual(0, TestValueTy.objectCount, "TestValueTy leak")
  expectEqual(0, TestEquatableValueTy.objectCount, "TestEquatableValueTy leak")
}

struct ExpectedArrayElement : Comparable, CustomStringConvertible {
  var value: Int
  var valueIdentity: UInt

  init(value: Int, valueIdentity: UInt = 0) {
    self.value = value
    self.valueIdentity = valueIdentity
  }

  var description: String {
    return "(\(value), \(valueIdentity))"
  }
}

func == (
  lhs: ExpectedArrayElement,
  rhs: ExpectedArrayElement
) -> Bool {
  return
    lhs.value == rhs.value &&
    lhs.valueIdentity == rhs.valueIdentity
}

func < (
  lhs: ExpectedArrayElement,
  rhs: ExpectedArrayElement
) -> Bool {
  let lhsElements = [ lhs.value, Int(bitPattern: lhs.valueIdentity) ]
  let rhsElements = [ rhs.value, Int(bitPattern: rhs.valueIdentity) ]
  return lhsElements.lexicographicalCompare(rhsElements)
}

func _equalsWithoutElementIdentity(
  lhs: [ExpectedArrayElement], _ rhs: [ExpectedArrayElement]
) -> Bool {
  func stripIdentity(
    list: [ExpectedArrayElement]
  ) -> [ExpectedArrayElement] {
    return list.map { ExpectedArrayElement(value: $0.value) }
  }

  return stripIdentity(lhs).elementsEqual(stripIdentity(rhs))
}

func _makeExpectedArrayContents(
  expected: [Int]
) -> [ExpectedArrayElement] {
  var result = [ExpectedArrayElement]()
  for value in expected {
    result.append(ExpectedArrayElement(value: value))
  }
  return result
}

struct ExpectedSetElement : Comparable, CustomStringConvertible {
  var value: Int
  var valueIdentity: UInt

  init(value: Int, valueIdentity: UInt = 0) {
    self.value = value
    self.valueIdentity = valueIdentity
  }

  var description: String {
    return "(\(value), \(valueIdentity))"
  }
}

func == (
  lhs: ExpectedSetElement,
  rhs: ExpectedSetElement
) -> Bool {
  return
    lhs.value == rhs.value &&
    lhs.valueIdentity == rhs.valueIdentity
}

func < (
  lhs: ExpectedSetElement,
  rhs: ExpectedSetElement
) -> Bool {
  let lhsElements = [ lhs.value, Int(bitPattern: lhs.valueIdentity) ]
  let rhsElements = [ rhs.value, Int(bitPattern: rhs.valueIdentity) ]
  return lhsElements.lexicographicalCompare(rhsElements)
}

func _makeExpectedSetContents(
  expected: [Int]
) -> [ExpectedSetElement] {
  var result = [ExpectedSetElement]()
  for value in expected {
    result.append(ExpectedSetElement(value: value))
  }
  return result
}

func _equalsUnorderedWithoutElementIdentity(
  lhs: [ExpectedSetElement], _ rhs: [ExpectedSetElement]
) -> Bool {
  func stripIdentity(
    list: [ExpectedSetElement]
  ) -> [ExpectedSetElement] {
    return list.map { ExpectedSetElement(value: $0.value) }
  }

  return equalsUnordered(stripIdentity(lhs), stripIdentity(rhs))
}

struct ExpectedDictionaryElement : Comparable, CustomStringConvertible {
  var key: Int
  var value: Int
  var keyIdentity: UInt
  var valueIdentity: UInt

  init(key: Int, value: Int, keyIdentity: UInt = 0, valueIdentity: UInt = 0) {
    self.key = key
    self.value = value
    self.keyIdentity = keyIdentity
    self.valueIdentity = valueIdentity
  }

  var description: String {
    return "(\(key), \(value), \(keyIdentity), \(valueIdentity))"
  }
}

func == (
  lhs: ExpectedDictionaryElement,
  rhs: ExpectedDictionaryElement
) -> Bool {
  return
    lhs.key == rhs.key &&
    lhs.value == rhs.value &&
    lhs.keyIdentity == rhs.keyIdentity &&
    lhs.valueIdentity == rhs.valueIdentity
}

func < (
  lhs: ExpectedDictionaryElement,
  rhs: ExpectedDictionaryElement
) -> Bool {
  let lhsElements = [
    lhs.key, lhs.value, Int(bitPattern: lhs.keyIdentity),
    Int(bitPattern: lhs.valueIdentity)
  ]
  let rhsElements = [
    rhs.key, rhs.value, Int(bitPattern: rhs.keyIdentity),
    Int(bitPattern: rhs.valueIdentity)
  ]
  return lhsElements.lexicographicalCompare(rhsElements)
}

func _equalsUnorderedWithoutElementIdentity(
  lhs: [ExpectedDictionaryElement], _ rhs: [ExpectedDictionaryElement]
) -> Bool {
  func stripIdentity(
    list: [ExpectedDictionaryElement]
  ) -> [ExpectedDictionaryElement] {
    return list.map { ExpectedDictionaryElement(key: $0.key, value: $0.value) }
  }

  return equalsUnordered(stripIdentity(lhs), stripIdentity(rhs))
}

func _makeExpectedDictionaryContents(
  expected: [(Int, Int)]
) -> [ExpectedDictionaryElement] {
  var result = [ExpectedDictionaryElement]()
  for (key, value) in expected {
    result.append(ExpectedDictionaryElement(key: key, value: value))
  }
  return result
}

