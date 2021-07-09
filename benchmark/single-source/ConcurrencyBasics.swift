//===--- ConcurrencyBasics.swift -------------------------------------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2020 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//

import TestsUtils

public let ConcurrencyBasics = [
  BenchmarkInfo(name: "AsyncFibonacci", runFunction: run_AsyncFibonacci, tags: [.validation, .api]),
  BenchmarkInfo(name: "AsyncLetFibonacci", runFunction: run_AsyncLetFibonacci, tags: [.validation, .api]),
  BenchmarkInfo(name: "ActorFibonacci", runFunction: run_ActorFibonacci, tags: [.validation, .api]),
  BenchmarkInfo(name: "ActorAsyncLetFibonacci", runFunction: run_ActorAsyncLetFibonacci, tags: [.validation, .api]),
  BenchmarkInfo(name: "SyncFibonacci", runFunction: run_SyncFibonacci, tags: [.validation, .api]),
  BenchmarkInfo(name: "CallbackFibonacci", runFunction: run_CallbackFibonacci, tags: [.validation, .api]),
]

let m = 19
let expected = 4181

//               m:    18      19      20
// --------------------------------------
// expected result:  2584    4181    6765
//     async calls:  8360   13528   21890
//  Actor a0 calls:  1597    2584    4181
//  Actor a1 calls:  2584    4181    6765

@inline(never)
public func run_AsyncFibonacci(N: Int) {
  runAsyncAndBlock {
    for _ in 0 ..< N {
      let result = await asyncFib(identity(m))
      CheckResults(result == expected)
    }
  }
}

@inline(never)
func asyncFib(_ n: Int) async -> Int {
  if n == 0 || n == 1 {
    return n
  }

  let first = await asyncFib(n-2)
  let second = await asyncFib(n-1)

  let result = first &+ second

  return result
}

@inline(never)
public func run_AsyncLetFibonacci(N: Int) {
  runAsyncAndBlock {
    for _ in 0 ..< N {
      let result = await asyncLetFib(identity(m))
      CheckResults(result == expected)
    }
  }
}

@inline(never)
func asyncLetFib(_ n: Int) async -> Int {
  if n == 0 || n == 1 {
    return n
  }

  async let first = asyncLetFib(n-2)
  async let second = asyncLetFib(n-1)

  let result = await first &+ second

  return result
}

@inline(never)
public func run_ActorFibonacci(N: Int) {
  runAsyncAndBlock {
    let a0 = Actor()
    let a1 = Actor()
    for _ in 0 ..< N {
      let result = await actorFib(identity(m), a0, a1)
      CheckResults(result == expected)
    }
  }
}

@inline(never)
func actorFib(_ n: Int, _ a0: Actor, _ a1: Actor) async -> Int {
  if n == 0 {
    return await a0.get(n)
  }
  if n == 1 {
    return await a1.get(n)
  }

  // Executed 10945 times for n = 18
  let first = await actorFib(n-2, a0, a1)
  let second = await actorFib(n-1, a0, a1)

  let result = first &+ second

  return result
}

actor Actor {
  var x: Int = 0

  @inline(never)
  func get(_ i: Int ) async -> Int {
    x += 1
    return i
  }
}

@inline(never)
public func run_ActorAsyncLetFibonacci(N: Int) {
  runAsyncAndBlock {
    let a1 = Actor()
    let a2 = Actor()
    for _ in 0 ..< N {
      let result = await actorFib(identity(m), a1, a2)
      CheckResults(result == expected)
    }
  }
}

@inline(never)
func actorAsyncLetFib(_ n: Int, _ a1: Actor, _ a2: Actor) async -> Int {
  if n == 0 {
    return await a1.get(n)
  }
  if n == 1 {
    return await a2.get(n)
  }

  // Executed 10945 times for n = 18
  async let first = actorAsyncLetFib(n-2, a1, a2)
  async let second = actorAsyncLetFib(n-1, a1, a2)

  let result = await first &+ second

  return result
}

@inline(never)
public func run_SyncFibonacci(N: Int) {
  for _ in 0 ..< N {
    let result = syncFib(identity(m))
    CheckResults(result == expected)
  }
}

@inline(never)
func syncFib(_ n: Int) -> Int {
  if n == 0 || n == 1 {
    return n
  }

  let first = syncFib(n-2)
  let second = syncFib(n-1)

  let result = first &+ second
  return result
}

@inline(never)
public func run_CallbackFibonacci(N: Int) {
  for _ in 0 ..< N {
    callbackFib(identity(m), identityClosure({ result in
      CheckResults(result == expected)
    }))
  }
}

@inline(never)
func identityClosure(_ c: @escaping (Int) -> ()) -> (Int) -> () {
  return c
}

@inline(never)
func callbackFib(_ n: Int, _ continuation: @escaping (Int) -> ()) {
  if n == 0 || n == 1 {
    return continuation(n)
  }

  callbackFib(n-2, identityClosure({ first in
    callbackFib(n-1, identityClosure( { second in
      let result = first &+ second
      continuation(result)
    }))
  }))
}

