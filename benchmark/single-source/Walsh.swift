//===--- Walsh.swift ------------------------------------------------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2021 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//

import TestsUtils
#if canImport(Glibc)
import Glibc
#elseif canImport(Musl)
import Musl
#elseif os(Windows)
import MSVCRT
#else
import Darwin
#endif

public let benchmarks =
  BenchmarkInfo(
    name: "Walsh",
    runFunction: run_Walsh,
    tags: [.validation, .algorithm])

func isPowerOfTwo(_ x: Int) -> Bool { return (x & (x - 1)) == 0 }

// Fast Walsh Hadamard Transform
func walshTransform(_ data: inout [Double]) {
  assert(isPowerOfTwo(data.count), "Not a power of two")
  var temp = [Double](repeating: 0, count: data.count)
  let ret = walshImpl(&data, &temp, 0, data.count)
  for i in 0..<data.count {
    data[i] = ret[i]
  }
}

func scale(_ data: inout [Double], _ scalar : Double) {
  for i in 0..<data.count {
    data[i] = data[i] * scalar
  }
}

func inverseWalshTransform(_ data: inout [Double]) {
  walshTransform(&data)
  scale(&data, Double(1)/Double(data.count))
}

func walshImpl(_ data: inout [Double], _ temp: inout [Double], _ start: Int, _ size: Int) -> [Double] {
  if (size == 1) { return data }

  let stride = size/2
  for i in 0..<stride {
    temp[start + i]          = data[start + i + stride] + data[start + i]
    temp[start + i + stride] = data[start + i] - data[start + i + stride]
  }

  _ = walshImpl(&temp, &data, start, stride)
  return walshImpl(&temp, &data, start + stride, stride)
}

func checkCorrectness() {
  let input: [Double] = [1,0,1,0,0,1,1,0]
  let output: [Double] = [4,2,0,-2,0,2,0,2]
  var data: [Double] = input
  walshTransform(&data)
  let mid = data
  inverseWalshTransform(&data)
  for i in 0..<input.count {
    // Check encode.
    check(abs(data[i] - input[i]) < 0.0001)
    // Check decode.
    check(abs(mid[i] - output[i]) < 0.0001)
  }
}

@inline(never)
public func run_Walsh(_ n: Int) {
  checkCorrectness()

  // Generate data.
  var data2 : [Double] = []
  for i in 0..<1024 {
    data2.append(Double(sin(Float(i))))
  }

  // Transform back and forth.
  for _ in 1...10*n {
    walshTransform(&data2)
    inverseWalshTransform(&data2)
  }
}
