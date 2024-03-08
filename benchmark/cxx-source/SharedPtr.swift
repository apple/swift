//===--- CxxVectorSum.swift -----------------------------------------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2012 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//
// This is a benchmark that tracks how quickly Swift can sum up a C++ vector
// as compared to the C++ implementation of such sum.
import TestsUtils
import CxxSharedPtr
import Cxx
import std.memory

public let benchmarks = [
  BenchmarkInfo(
      name: "SharedPtr.Tuple.x5.Cxx",
      runFunction: run_SharedPtr_Tuple_Cxx,
      tags: [.validation, .bridging, .cxxInterop],
      setUpFunction: makeSharedOnce),
  BenchmarkInfo(
    name: "SharedPtr.Tuple.x5.Swift",
    runFunction: run_SharedPtr_Tuple_Swift,
    tags: [.validation, .bridging, .cxxInterop],
    setUpFunction: makeSharedOnce)
]

func makeSharedOnce() {
    initPtr()
}

@inline(never)
public func run_SharedPtr_Tuple_Cxx(_ n: Int) {
  for _ in 0..<n * 10_000 {
    let sum = testSharedPtrCopies()
    blackHole(sum)
  }
}

func swiftCopyX5() -> (SharedPtrT, SharedPtrT, SharedPtrT, SharedPtrT, SharedPtrT) {
  return (ptr, ptr, ptr, ptr, ptr);
}


func swiftTestSharedPtrCopies() -> UInt32 {
  let (p1, p2, p3, p4, p5) = swiftCopyX5()

  let v1 = p1.pointee.value
  let v2 = p2.pointee.value
  let v3 = p3.pointee.value
  let v4 = p4.pointee.value
  let v5 = p5.pointee.value

  return v1 + v2 + v3 + v4 + v5
}

@inline(never)
public func run_SharedPtr_Tuple_Swift(_ n: Int) {
    for _ in 0..<n * 10_000 {
      let sum = swiftTestSharedPtrCopies()
      blackHole(sum)
    }
}