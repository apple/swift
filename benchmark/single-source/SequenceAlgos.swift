//===--- SequenceAlgos.swift ----------------------------------------------===//
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

// This benchmark tests closureless versions of min and max, both contains,
// repeatElement and reduce, on a number of different sequence types.
// To avoid too many little micro benchmarks, it measures them all together
// for each sequence type.

// Benchmark for reversed was added later.

let t: [BenchmarkCategory] = [.validation, .api]

public let benchmarks = [
  BenchmarkInfo(name: "SequenceAlgosList", runFunction: { for _ in 0..<$0 {
      benchmarkSequenceAlgos(s: l, n: n)
      benchmarkEquatableSequenceAlgos(s: l, n: n)
    }}, tags: t, setUpFunction: { blackHole(l) }, legacyFactor: 10),
  BenchmarkInfo(name: "SequenceAlgosArray", runFunction: { for _ in 0..<$0 {
      benchmarkSequenceAlgos(s: a, n: a.count)
      benchmarkEquatableSequenceAlgos(s: a, n: a.count)
    }}, tags: t, setUpFunction: { blackHole(a) }, legacyFactor: 10),
  BenchmarkInfo(name: "SequenceAlgosContiguousArray",
    runFunction: { for _ in 0..<$0 {
        benchmarkSequenceAlgos(s: c, n: c.count)
        benchmarkEquatableSequenceAlgos(s: c, n: c.count)
      }}, tags: t, setUpFunction: { blackHole(c) }, legacyFactor: 10),
  BenchmarkInfo(name: "SequenceAlgosRange", runFunction: { for _ in 0..<$0 {
      benchmarkSequenceAlgos(s: r, n: r.count)
      benchmarkEquatableSequenceAlgos(s: r, n: r.count)
    }}, tags: t, legacyFactor: 10),
  BenchmarkInfo(name: "SequenceAlgosUnfoldSequence",
    runFunction: { for _ in 0..<$0 {
        benchmarkSequenceAlgos(s: s, n: n)
      }}, tags: t, setUpFunction: { blackHole(s) }, legacyFactor: 10),
  BenchmarkInfo(name: "SequenceAlgosAnySequence",
    runFunction: { for _ in 0..<$0 {
        benchmarkSequenceAlgos(s: y, n: n/10)
      }}, tags: t, setUpFunction: { blackHole(y) }, legacyFactor: 100),

  BenchmarkInfo(name: "Sequence.reversed.List", runFunction: { for _ in 0..<$0 {
      benchmarkReversed(s: l, n: n)
    }}, tags: t, setUpFunction: { blackHole(l) }),
  BenchmarkInfo(name: "Sequence.reversed.Array", runFunction: { for _ in 0..<$0 {
      benchmarkReversed(s: a, n: a.count)
    }}, tags: t, setUpFunction: { blackHole(a) }),
  BenchmarkInfo(name: "Sequence.reversed.ContiguousArray",
    runFunction: { for _ in 0..<$0 {
        benchmarkReversed(s: c, n: c.count)
      }}, tags: t, setUpFunction: { blackHole(c) }),
  BenchmarkInfo(name: "Sequence.reversed.Range", runFunction: { for _ in 0..<$0 {
      benchmarkReversed(s: r, n: r.count)
      }}, tags: t),
  BenchmarkInfo(name: "Sequence.reversed.UnfoldSequence",
    runFunction: { for _ in 0..<$0 {
        benchmarkReversed(s: s, n: n)
      }}, tags: t, setUpFunction: { blackHole(s) }),
  BenchmarkInfo(name: "Sequence.reversed.AnySequence",
    runFunction: { for _ in 0..<$0 {
        benchmarkReversed(s: y, n: n/10)
      }}, tags: t, setUpFunction: { blackHole(y) }),
]

extension List: Sequence {
  struct Iterator: IteratorProtocol {
    var _list: List<Element>
    mutating func next() -> Element? {
      guard case let .node(x,xs) = _list else { return nil }
      _list = xs
      return x
    }
  }
  func makeIterator() -> Iterator {
    return Iterator(_list: self)
  }
}

extension List: Equatable where Element: Equatable {
  static func == (lhs: List<Element>, rhs: List<Element>) -> Bool {
    return lhs.elementsEqual(rhs)
  }
}

func benchmarkSequenceAlgos<S: Sequence>(s: S, n: Int) where S.Element == Int {
  check(s.reduce(0, &+) == (n*(n-1))/2)
  let mn = s.min()
  let mx = s.max()
  check(mn == 0 && mx == n-1)
  check(s.starts(with: s))
}

let n = 1_000
let r = 0..<(n*100)
let l = List(0..<n)
let c = ContiguousArray(0..<(n*100))
let a = Array(0..<(n*100))
let y = AnySequence(0..<n/10)
let s = sequence(first: 0, next: { $0 < n&-1 ? $0&+1 : nil})

func benchmarkEquatableSequenceAlgos<S: Sequence>(s: S, n: Int)
  where S.Element == Int, S: Equatable {
  check(repeatElement(s, count: 1).contains(s))
  check(!repeatElement(s, count: 1).contains { $0 != s })
}

enum List<Element> {
  case end
  indirect case node(Element, List<Element>)

  init<S: BidirectionalCollection>(_ elements: S) where S.Element == Element {
    self = elements.reversed().reduce(.end) { .node($1,$0) }
  }
}

func benchmarkReversed<S: Sequence>(s: S, n: Int) where S.Element == Int {
  check(s.reversed()[n-1] == 0)
}
