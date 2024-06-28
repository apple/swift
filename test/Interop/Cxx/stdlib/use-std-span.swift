// RUN: %target-run-simple-swift(-I %S/Inputs -Xfrontend -enable-experimental-cxx-interop -Xcc -std=c++20)

// FIXME swift-ci linux tests do not support std::span
// UNSUPPORTED: OS=linux-gnu

// REQUIRES: executable_test

import StdlibUnittest
#if !BRIDGING_HEADER
import StdSpan
#endif
import CxxStdlib

var StdSpanTestSuite = TestSuite("StdSpan")

StdSpanTestSuite.test("EmptySpan") {
  let s = Span()
  expectEqual(s.size(), 0)
  expectTrue(s.empty())
}

StdSpanTestSuite.test("InitSpan") {
  let s = initSpan()
  expectEqual(s.size(), 3)
  expectFalse(s.empty())
}

StdSpanTestSuite.test("InitStaticSpan") {
  expectEqual(ispan.size(), 3)
  expectFalse(ispan.empty())

  expectEqual(ispan[0], 1)
  expectEqual(ispan[1], 2)
  expectEqual(ispan[2], 3)
}

StdSpanTestSuite.test("InitStringSpan") {
  expectEqual(sspan.size(), 3)
  expectFalse(sspan.empty())

  expectEqual(sspan[0], "")
  expectEqual(sspan[1], "ab")
  expectEqual(sspan[2], "abc")
}

runAllTests()
