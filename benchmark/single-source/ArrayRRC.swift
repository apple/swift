//===--- ArrayRRC.swift ----------------------------------------------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2024 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//

import TestsUtils
import Darwin

fileprivate class DummyObject {}

fileprivate func makePOD(count: Int) -> [Int] {
  var result = Array(repeating: 42, count: count - 1)
  blackHole(result)
  result.append(42) //force a resize if result was perfectly fitted
  return result
}

fileprivate func makeRefcounted(count: Int) -> [DummyObject] {
  var result = (0 ..< count - 1).map { _ in DummyObject() }
  blackHole(result)
  result.append(DummyObject())
  return result
}

fileprivate func ranges(count: Int) -> [(Range<Int>, String)] {
  var results:[(Range<Int>, String)] = []
  results.append((0 ..< (count / 3), "pre"))
  results.append(((count / 3) ..< ((count / 3) * 2), "mid"))
  results.append((((count / 3) * 2) ..< count, "end"))
  return results
}

fileprivate let sourceSizes = [(10_000, "lg"), (99, "sm")/*, (1, "xs")*/]
fileprivate let destSizes = [(10_000, "lg"), (99, "sm")]


fileprivate func configs() -> [BenchmarkInfo] {
  var configs: [BenchmarkInfo] = []
  for (refcounted, refcountedName) in [(true, "ref"), (false, "pod")] {
    for (sourceCount, sourceName) in sourceSizes {
      for (destCount, destName) in destSizes {
        for (subrange, rangeName) in ranges(count: destCount) {
          for (unique, uniqueName) in [(true, "unq"), (false, "non")] {
      //      for (destUnique, destUniqueName) in [(true, "du"), (false, "dn")] {
              let runFunction = switch (refcounted, unique) {
              case (true, true): runArrayRRCRefcountedUniqueDest
              case (true, false): runArrayRRCRefcountedSharedDest
              case (false, true): runArrayRRCPODUniqueDest
              case (false, false): runArrayRRCPODSharedDest
              }
              configs.append(
                BenchmarkInfo(
                  name:"ArrayRRC.\(refcountedName).\(destName).\(sourceName).\(rangeName).\(uniqueName)",
                  runFunction: runFunction,
                  tags: [.api, .Array, .algorithm],
                  setUpFunction: {
                    if sourceCount < 1000 && destCount < 1000 {
                      extraIters = 100
                    }
                    if sourceCount > 1000 && destCount > 1000 {
                      extraIters = 2
                    }
                    if !refcounted {
                      extraIters *= 10
                    }
                    //It looks like the first iteration is having to do a single
                    //CoW copy, so we amortize that a bit by letting it get more
                    //non-copying iterations. Ideally we wouldn't need this.
                    if unique && extraIters < 20 {
                      extraIters *= 2
                    }
                    //These are different code paths, but I'm trying to have a less overwhelming number of total tests
                    useUniqueDest = unique
                    useUniqueSource = unique
                    range = subrange
                    if refcounted {
                      refcountedDest = makeRefcounted(count: destCount)
                      refcountedSource = makeRefcounted(count: sourceCount)
                      if useUniqueDest {
                        refcountedOriginalRangeContents = Array(refcountedDest[range])
                      }
                    } else {
                      podDest = makePOD(count: destCount)
                      podSource = makePOD(count: sourceCount)
                      if useUniqueDest {
                        podOriginalRangeContents = Array(podDest[range])
                      }
                    }
                  },
                  tearDownFunction: {
                    refcountedDest = []
                    refcountedSource = []
                    refcountedOriginalRangeContents = []
                    podDest = []
                    podSource = []
                    podOriginalRangeContents = []
                    extraIters = 10
                  }
                )
              )
          //  }
          }
        }
      }
    }
  }
  return configs
}

fileprivate var range:Range<Int> = 0 ..< 0
fileprivate var refcountedDest:[DummyObject] = []
fileprivate var podDest:[Int] = []
fileprivate var refcountedSource:[DummyObject] = []
fileprivate var podSource:[Int] = []
fileprivate var refcountedOriginalRangeContents:[DummyObject] = []
fileprivate var podOriginalRangeContents:[Int] = []
fileprivate var useUniqueDest = false
fileprivate var useUniqueSource = false
fileprivate var extraIters = 10

public let benchmarks: [BenchmarkInfo] = configs()

/*
 Note: the work done by the unique and non-unique variants is different.
 Only compare like to like.
 */
@inline(__always)
fileprivate func runArrayRRCImplNonUniqueDest<A>(
  n: Int,
  dest destination: A,
  source: A,
  sourceLocation: inout A
) where A:RangeReplaceableCollection, A.Index == Int  {
  let subrange = range
  var dest = destination
  for _ in 0 ..< n {
    for _ in 0 ..< extraIters {
      let destCopy = dest
      let sourceCopy = useUniqueSource ? nil : source
      dest.replaceSubrange(subrange, with: source)
      blackHole(dest)
      if !useUniqueSource {
        blackHole(sourceCopy)
      }
      dest = destCopy
    }
  }
  sourceLocation = source
}

/*
 Keeping the destination unique requires restoring it to its original state each
 iteration, which necessarily involves a different replaceSubrange than the one
 being tested. Unfortunately this makes this variant less precise than the
 other, but it's still useful to be able to see the non-CoW case
 */
@inline(__always)
fileprivate func runArrayRRCImplUniqueDest<A>(
  n: Int,
  dest destination: A,
  source: A,
  originalRangeContents: A,
  destLocation: inout A,
  sourceLocation: inout A,
  originalRangeContentsLocation: inout A
) where A:RangeReplaceableCollection, A.Index == Int {
  let subrange = range
  var dest = destination
  for _ in 0 ..< n {
    for _ in 0 ..< extraIters {
      let sourceCopy = useUniqueSource ? nil : source
      let originalRangeContentsCopy = useUniqueSource ? nil : originalRangeContents
      dest.replaceSubrange(subrange, with: source)
      blackHole(dest)
      dest.replaceSubrange(
        subrange.lowerBound ..< subrange.lowerBound + source.count,
        with: originalRangeContents
      )
      if !useUniqueSource {
        blackHole(originalRangeContentsCopy)
        blackHole(sourceCopy)
      }
    }
  }
  destLocation = dest
  sourceLocation = source
  originalRangeContentsLocation = originalRangeContents
}

public func runArrayRRCRefcountedUniqueDest(n: Int) {
  let source = refcountedSource
  refcountedSource = []
  let dest = refcountedDest
  refcountedDest = []
  let originalRangeContents = refcountedOriginalRangeContents
  refcountedOriginalRangeContents = []
  runArrayRRCImplUniqueDest(
    n: n,
    dest: dest,
    source: source,
    originalRangeContents: originalRangeContents,
    destLocation: &refcountedDest,
    sourceLocation: &refcountedSource,
    originalRangeContentsLocation: &refcountedOriginalRangeContents
  )
}

public func runArrayRRCRefcountedSharedDest(n: Int) {
  let source = refcountedSource
  refcountedSource = []
  runArrayRRCImplNonUniqueDest(
    n: n,
    dest: refcountedDest,
    source: source,
    sourceLocation: &refcountedSource
  )
}

public func runArrayRRCPODUniqueDest(n: Int) {
  let source = podSource
  podSource = []
  let dest = podDest
  podDest = []
  let originalRangeContents = podOriginalRangeContents
  podOriginalRangeContents = []
  assert(dest.count > 0)
  runArrayRRCImplUniqueDest(
    n: n,
    dest: dest,
    source: source,
    originalRangeContents: originalRangeContents,
    destLocation: &podDest,
    sourceLocation: &podSource,
    originalRangeContentsLocation: &podOriginalRangeContents
  )
}

public func runArrayRRCPODSharedDest(n: Int) {
  let source = podSource
  podSource = []
  runArrayRRCImplNonUniqueDest(
    n: n,
    dest: podDest,
    source: source,
    sourceLocation: &podSource
  )
}
