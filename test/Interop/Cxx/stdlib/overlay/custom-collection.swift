// RUN: %target-run-simple-swift(-I %S/Inputs -Xfrontend -enable-experimental-cxx-interop)
//
// REQUIRES: executable_test

import StdlibUnittest
import CustomSequence
import Cxx

var CxxCollectionTestSuite = TestSuite("CxxCollection")

CxxCollectionTestSuite.test("SimpleCollectionNoSubscript as Swift.Collection") {
  let c = SimpleCollectionNoSubscript()
  expectEqual(c.first, 1)
  expectEqual(c.last, 5)

  // This subscript is a default implementation added in CxxRandomAccessCollection.
  expectEqual(c[0], 1)
  expectEqual(c[1], 2)
  expectEqual(c[4], 5)

  var array: [Int32] = []
  c.forEach {
    array.append($0)
  }
  expectEqual([1, 2, 3, 4, 5] as [Int32], array)
}

CxxCollectionTestSuite.test("SimpleCollectionReadOnly as Swift.Collection") {
  let c = SimpleCollectionReadOnly()
  expectEqual(c.first, 1)
  expectEqual(c.last, 5)

  let slice = c[1..<3]
  expectEqual(slice.first, 2)
  expectEqual(slice.last, 3)
}

CxxCollectionTestSuite.test("SimpleArrayWrapper as Swift.Collection") {
  let c = SimpleArrayWrapper()
  expectEqual(c.first, 10)
  expectEqual(c.last, 50)

  let reduced = c.reduce(0, +)
  expectEqual(reduced, 150)

  let mapped = c.map { $0 + 1 }
  expectEqual(mapped.first, 11)
  expectEqual(mapped.last, 51)
}

CxxCollectionTestSuite.test("HasInheritedTemplatedConstRACIterator as Swift.Collection") {
  let c = HasInheritedTemplatedConstRACIteratorInt()
  expectEqual(c.first, 1)
  expectEqual(c.last, 5)

  let reduced = c.reduce(0, +)
  expectEqual(reduced, 15)
}

CxxCollectionTestSuite.test("HasInheritedTemplatedConstRACIteratorOutOfLineOps as Swift.Collection") {
  let c = HasInheritedTemplatedConstRACIteratorOutOfLineOpsInt()
  expectEqual(c.first, 1)
  expectEqual(c.last, 3)

  let reduced = c.reduce(0, +)
  expectEqual(reduced, 6)
}

runAllTests()
