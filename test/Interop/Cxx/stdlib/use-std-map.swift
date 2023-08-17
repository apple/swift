// RUN: %target-run-simple-swift(-I %S/Inputs -Xfrontend -enable-experimental-cxx-interop)
//
// REQUIRES: executable_test
//
// REQUIRES: OS=macosx || OS=linux-gnu

import StdlibUnittest
import StdMap
import CxxStdlib
import Cxx

var StdMapTestSuite = TestSuite("StdMap")

StdMapTestSuite.test("init") {
  let m = Map()
  expectEqual(m.size(), 0)
  expectTrue(m.empty())
}

StdMapTestSuite.test("Map.subscript") {
  // This relies on the `std::map` conformance to `CxxDictionary` protocol.
  var m = initMap()
  let at1 = m[1]
  expectNotNil(at1)
  expectEqual(at1, 3)
  expectEqual(m[2], 2)
  expectEqual(m[3], 3)
  expectNil(m[-1])
  expectNil(m[5])
  
  m[1] = 111
  expectEqual(m[1], 111)

  m[5] = 555
  expectEqual(m[5], 555)

  m[5] = nil
  expectNil(m[5])
  expectNil(m[5])
}

StdMapTestSuite.test("MapStrings.subscript") {
  var m = MapStrings()
  expectNil(m[std.string()])
  expectNil(m[std.string()])
  m[std.string()] = std.string()
  expectNotNil(m[std.string()])

  m[std.string("abc")] = std.string("qwe")
  expectEqual(m[std.string("abc")], std.string("qwe"))
}

StdMapTestSuite.test("NestedMap.subscript") {
  var m = NestedMap()
  expectNil(m[0])
  expectNil(m[0])
  m[1] = Map()
  expectNotNil(m[1])

  expectNil(m[1]![0])
  m[1]![0] = 123
  expectEqual(m[1]![0], 123)

  m[1]![0] = nil
  expectNil(m[1]![0])

  m[1] = nil
  expectNil(m[1])
}

StdMapTestSuite.test("UnorderedMap.subscript") {
  // This relies on the `std::unordered_map` conformance to `CxxDictionary` protocol.
  var m = initUnorderedMap()
  expectEqual(m[1], 3)
  expectEqual(m[2], 2)
  expectEqual(m[3], 3)
  expectNil(m[-1])
  expectNil(m[5])

  m[1] = 777
  expectEqual(m[1], 777)

  m[-1] = 228
  expectEqual(m[-1], 228)

  m[-1] = nil
  expectNil(m[-1])
  expectNil(m[-1])
}

StdMapTestSuite.test("Map.erase") {
  var m = initMap()
  expectNotNil(m[1])
  m.erase(1)
  expectNil(m[1])
  m.erase(1)
  expectNil(m[1])
}

StdMapTestSuite.test("UnorderedMap.erase") {
  var m = initUnorderedMap()
  expectNotNil(m[2])
  m.erase(2)
  expectNil(m[2])
  m.erase(2)
  expectNil(m[2])
}

runAllTests()
