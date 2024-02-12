//===--- AsyncTree.swift -------------------------------------------------===//
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
import Dispatch

public var benchmarks: [BenchmarkInfo] {
  guard #available(macOS 10.15, iOS 13, tvOS 13, watchOS 6, *) else {
    return []
  }
  return [
    BenchmarkInfo(
      name: "AsyncTree.100",
      runFunction: run_AsyncTree(treeSize: 100),
      tags: [.concurrency]
    ),
    BenchmarkInfo(
      name: "AsyncTree.5000",
      runFunction: run_AsyncTree(treeSize: 5000),
      tags: [.concurrency]
    )
  ]
}

@available(macOS 10.15, iOS 13, tvOS 13, watchOS 6, *)
private actor MyActor {
  let g: DispatchGroup

  init(_ g: DispatchGroup) {
    self.g = g
  }

  func test(_ n: Int) {
    let L = n / 2
    let R = n - 1 - L

    if L > 0 {
      Task {
        self.test(L)
      }
    }

    if R > 0 {
      Task {
        self.test(R)
      }
    }

    g.leave()
  }
}

@available(macOS 10.15, iOS 13, tvOS 13, watchOS 6, *)
private func run_AsyncTree(treeSize: Int) -> (Int) -> Void {
  return { n in
    for _ in 0..<n {
      let g = DispatchGroup()
      for _ in 0..<treeSize {
        g.enter()
      }
      let actor = MyActor(g)
      Task {
        await actor.test(treeSize)
      }
      g.wait()
    }
  }
}
