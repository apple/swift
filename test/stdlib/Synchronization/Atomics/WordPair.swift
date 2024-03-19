// RUN: %target-run-simple-swift
// REQUIRES: executable_test
// REQUIRES: synchronization

import Synchronization
import StdlibUnittest

let suite = TestSuite("WordPairTests")

struct UIntPair: Equatable {
  var first: UInt
  var second: UInt

  init(_ first: UInt, _ second: UInt) {
    self.first = first
    self.second = second
  }
}

@available(SwiftStdlib 6.0, *)
func componentsInMemoryOrder(of dword: WordPair) -> UIntPair {
  let p = UnsafeMutableRawPointer.allocate(
    byteCount: MemoryLayout<WordPair>.size,
    alignment: MemoryLayout<WordPair>.alignment)
  p.storeBytes(of: dword, as: WordPair.self)
  let first = p.load(as: UInt.self)
  let second = p.load(fromByteOffset: MemoryLayout<UInt>.stride, as: UInt.self)
  return UIntPair(first, second)
}

if #available(SwiftStdlib 6.0, *) {

suite.test("basics") {
  expectEqual(MemoryLayout<WordPair>.size, 2 * MemoryLayout<UInt>.size)
  expectEqual(MemoryLayout<WordPair>.stride, MemoryLayout<WordPair>.size)
  //expectEqual(MemoryLayout<WordPair>.alignment, 2 * MemoryLayout<UInt>.alignment)

  let value0 = WordPair(first: 2, second: 1)
#if _endian(little)
  expectEqual(componentsInMemoryOrder(of: value0), UIntPair(2, 1))
#else
  expectEqual(componentsInMemoryOrder(of: value0), UIntPair(1, 2))
#endif

  let value1 = WordPair(first: .max, second: 0)
  expectEqual(value1.first, .max)
  expectEqual(value1.second, 0)
}

} // if #available(SwiftStdlib 6.0, *)

runAllTests()
