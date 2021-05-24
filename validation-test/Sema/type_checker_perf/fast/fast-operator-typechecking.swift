// RUN: %target-typecheck-verify-swift -swift-version 5 -solver-expression-time-threshold=1

// rdar://problem/32998180
func checksum(value: UInt16) -> UInt16 {
  var checksum = (((value >> 2) ^ (value >> 8) ^ (value >> 12) ^ (value >> 14)) & 0x01) << 1
  checksum |= (((value) ^ (value >> UInt16(4)) ^ (value >> UInt16(6)) ^ (value >> UInt16(10))) & 0x01)
  checksum ^= 0x02
  return checksum
}

// rdar://problem/42672829
func f(tail: UInt64, byteCount: UInt64) {
  if tail & ~(1 << ((byteCount & 7) << 3) - 1) == 0 { }
}

// rdar://problem/32547805
func size(count: Int) {
  // Size of the buffer we need to allocate
  let _ = count * MemoryLayout<Float>.size * (4 + 3 + 3 + 2 + 4)
}
