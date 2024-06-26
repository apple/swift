//===--- SIMDRandomMask.swift ---------------------------------------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2021 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//

import TestsUtils

public let benchmarks = [
  BenchmarkInfo(
    name: "SIMDRandomMask.Int8x16",
    runFunction: run_SIMDRandomMaskInt8x16,
    tags: [.validation, .SIMD]
  ),
  BenchmarkInfo(
    name: "SIMDRandomMask.Int8x16.Generic",
    runFunction: run_SIMDRandomMaskInt8x16_generic,
    tags: [.validation, .SIMD],
    legacyFactor: 10
  ),
  BenchmarkInfo(
    name: "SIMDRandomMask.Int8x64",
    runFunction: run_SIMDRandomMaskInt8x64,
    tags: [.validation, .SIMD]
  ),
  BenchmarkInfo(
    name: "SIMDRandomMask.Int8x64.Generic",
    runFunction: run_SIMDRandomMaskInt8x64_generic,
    tags: [.validation, .SIMD],
    legacyFactor: 10
  ),
  BenchmarkInfo(
    name: "SIMDRandomMask.Int64x2",
    runFunction: run_SIMDRandomMaskInt64x2,
    tags: [.validation, .SIMD]
  ),
  BenchmarkInfo(
    name: "SIMDRandomMask.Int64x2.Generic",
    runFunction: run_SIMDRandomMaskInt64x2_generic,
    tags: [.validation, .SIMD],
    legacyFactor: 10
  ),
  BenchmarkInfo(
    name: "SIMDRandomMask.Int64x8",
    runFunction: run_SIMDRandomMaskInt64x8,
    tags: [.validation, .SIMD]
  ),
  BenchmarkInfo(
    name: "SIMDRandomMask.Int64x8.Generic",
    runFunction: run_SIMDRandomMaskInt64x8_generic,
    tags: [.validation, .SIMD],
    legacyFactor: 10
  ),
  BenchmarkInfo(
    name: "SIMDRandomMask.Int64x64",
    runFunction: run_SIMDRandomMaskInt64x64,
    tags: [.validation, .SIMD]
  ),
  BenchmarkInfo(
    name: "SIMDRandomMask.Int64x64.Generic",
    runFunction: run_SIMDRandomMaskInt64x64_generic,
    tags: [.validation, .SIMD],
    legacyFactor: 10 
  )
]

@inline(never)
public func run_SIMDRandomMaskInt8x16(_ n: Int) {
  var g = SplitMix64(seed: 0)
  var accum = SIMDMask<SIMD16<Int8>>()
  for _ in 0 ..< 10000*n {
    accum .^= SIMDMask.random(using: &g)
  }
  blackHole(accum)
}

@inline(never)
public func run_SIMDRandomMaskInt8x64(_ n: Int) {
  var g = SplitMix64(seed: 0)
  var accum = SIMDMask<SIMD64<Int8>>()
  for _ in 0 ..< 10000*n {
    accum .^= SIMDMask.random(using: &g)
  }
  blackHole(accum)
}

@inline(never)
public func run_SIMDRandomMaskInt64x2(_ n: Int) {
  var g = SplitMix64(seed: 0)
  var accum = SIMDMask<SIMD2<Int64>>()
  for _ in 0 ..< 10000*n {
    accum .^= SIMDMask.random(using: &g)
  }
  blackHole(accum)
}

@inline(never)
public func run_SIMDRandomMaskInt64x8(_ n: Int) {
  var g = SplitMix64(seed: 0)
  var accum = SIMDMask<SIMD8<Int64>>()
  for _ in 0 ..< 10000*n {
    accum .^= SIMDMask.random(using: &g)
  }
  blackHole(accum)
}

@inline(never)
public func run_SIMDRandomMaskInt64x64(_ n: Int) {
  var g = SplitMix64(seed: 0)
  var accum = SIMDMask<SIMD64<Int64>>()
  for _ in 0 ..< 10000*n {
    accum .^= SIMDMask.random(using: &g)
  }
  blackHole(accum)
}

@inline(__always)
internal func generic_random<T>() -> SIMDMask<T>
where T: SIMD, T.Scalar: FixedWidthInteger & SignedInteger {
  SIMDMask<T>.random()
}

@inline(never)
public func run_SIMDRandomMaskInt8x16_generic(_ N: Int) {
  var accum = SIMDMask<SIMD16<Int8>>()
  for _ in 0 ..< 1000*N {
    accum .^= generic_random()
  }
  blackHole(accum)
}

@inline(never)
public func run_SIMDRandomMaskInt8x64_generic(_ N: Int) {
  var accum = SIMDMask<SIMD64<Int8>>()
  for _ in 0 ..< 1000*N {
    accum .^= generic_random()
  }
  blackHole(accum)
}

@inline(never)
public func run_SIMDRandomMaskInt64x2_generic(_ N: Int) {
  var accum = SIMDMask<SIMD2<Int64>>()
  for _ in 0 ..< 1000*N {
    accum .^= generic_random()
  }
  blackHole(accum)
}

@inline(never)
public func run_SIMDRandomMaskInt64x8_generic(_ N: Int) {
  var accum = SIMDMask<SIMD8<Int64>>()
  for _ in 0 ..< 1000*N {
    accum .^= generic_random()
  }
  blackHole(accum)
}

@inline(never)
public func run_SIMDRandomMaskInt64x64_generic(_ N: Int) {
  var accum = SIMDMask<SIMD64<Int64>>()
  for _ in 0 ..< 1000*N {
    accum .^= generic_random()
  }
  blackHole(accum)
}
 
