//===--- NaiveRangeReplaceableCollectionConformance.swift -----------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2023 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//

import TestsUtils

var contiguous:[UInt8] = []

public let benchmarks = [
    BenchmarkInfo(name: "NaiveRRC.append.largeContiguous",
                  runFunction: runAppendLargeContiguous,
                  tags: [.validation, .api],
                  setUpFunction: { contiguous = [UInt8](repeating: 7, count: 1_000) }),
    BenchmarkInfo(name: "NaiveRRC.append.smallContiguousRepeated",
                  runFunction: runAppendSmallContiguousRepeatedly,
                  tags: [.validation, .api],
                  setUpFunction: { contiguous = [UInt8](repeating: 7, count: 1) }),
    BenchmarkInfo(name: "NaiveRRC.init.largeContiguous",
                  runFunction: runInitLargeContiguous,
                  tags: [.validation, .api],
                  setUpFunction: { contiguous = [UInt8](repeating: 7, count: 1_000) })
]

struct NaiveRRC : RangeReplaceableCollection {
  
  var storage:[UInt8] = []
  
  init() {}
  
  func index(after i: Int) -> Int {
    i + 1
  }
  
  func index(before i: Int) -> Int {
    i - 1
  }
  
  var startIndex: Int {
    0
  }
  
  var endIndex: Int {
    count
  }
  
  var count: Int {
    storage.count
  }
  
  subscript(position: Int) -> UInt8 {
    get {
      storage[position]
    }
    set(newValue) {
      storage[position] = newValue
    }
  }
  
  mutating func replaceSubrange(_ subrange: Range<Int>, with newElements: some Collection<UInt8>) {
    storage.replaceSubrange(subrange, with: newElements)
  }
  
  mutating func reserveCapacity(_ n: Int) {
    storage.reserveCapacity(n)
  }
}

@inline(never)
public func runAppendLargeContiguous(N: Int) {
  for _ in 1...N {
    var rrc = NaiveRRC()
    rrc.append(contentsOf: contiguous)
    blackHole(rrc)
  }
}

@inline(never)
public func runAppendSmallContiguousRepeatedly(N: Int) {
  for _ in 1...N {
    var rrc = NaiveRRC()
    for _ in 1...5_000 {
      rrc.append(contentsOf: contiguous)
    }
    blackHole(rrc)
  }
}

@inline(never)
public func runInitLargeContiguous(N: Int) {
  for _ in 1...N {
    blackHole(NaiveRRC(contiguous))
  }
}
