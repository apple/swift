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
}

#if !os(Linux) // TODO: enable on Linux (rdar://105220600)
StdMapTestSuite.test("UnorderedMap.subscript") {
  // This relies on the `std::unordered_map` conformance to `CxxDictionary` protocol.
  var m = initUnorderedMap()
  expectEqual(m[1], 3)
  expectEqual(m[2], 2)
  expectEqual(m[3], 3)
  expectNil(m[-1])
  expectNil(m[5])
}
#endif

runAllTests()
