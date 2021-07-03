//===--- DictionaryRemove.swift -------------------------------------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2017 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//

// Dictionary element removal benchmark
// rdar://problem/19804127
import TestsUtils

let t: [BenchmarkCategory] = [.validation, .api, .Dictionary]

let size = 100
let numberMap = Dictionary(uniqueKeysWithValues: zip(1...size, 1...size))
var temporaryNumberMap: [Int:Int] = [:]
let boxedNums = (1...size).lazy.map { Box($0) }
let boxedNumMap = Dictionary(uniqueKeysWithValues: zip(boxedNums, boxedNums))

public let DictionaryRemove = [
  BenchmarkInfo(name: "DictionaryRemove",
    runFunction: remove, tags: t, legacyFactor: 10),
  BenchmarkInfo(name: "DictionaryRemoveOfObjects",
    runFunction: removeObjects, tags: t, legacyFactor: 100),
  BenchmarkInfo(name: "DictionaryRemoveWhere",
    runFunction: removeWhere, tags: t, setUpFunction: setup_removeWhere),
  BenchmarkInfo(name: "DictionaryRemoveWhereKeepingCapacity",
    runFunction: removeWhereKeepingCapacity, tags: t, setUpFunction: setup_removeWhere)
]

class Box<T : Hashable> : Hashable {
  var value: T

  init(_ v: T) {
    value = v
  }

  func hash(into hasher: inout Hasher) {
    hasher.combine(value)
  }

  static func ==(lhs: Box, rhs: Box) -> Bool {
    return lhs.value == rhs.value
  }
}

func remove(N: Int) {
  for _ in 1...100*N {
    var dict = numberMap
    for i in 1...size { dict.removeValue(forKey: i) }
    CheckResults(dict.isEmpty)
  }
}

func removeObjects(N: Int) {
  for _ in 1...10*N {
    var dict = boxedNumMap
    for i in 1...size { dict.removeValue(forKey: Box(i)) }
    CheckResults(dict.isEmpty)
  }
}

func setup_removeWhere() {
  temporaryNumberMap = numberMap
  temporaryNumberMap[1000] = 1000 //force a copy
}

func _removeWhere(N: Int, keepCapacity: Bool) {
  var map = temporaryNumberMap
  temporaryNumberMap = [:] //let map be unique
  for i in 1...N {
    map.removeAll(where: { $0 == i }, keepingCapacity: keepCapacity)
  }
}

func removeWhere(N: Int) {
  _removeWhere(N: N, keepCapacity: false)
}

func removeWhereKeepingCapacity(N: Int) {
  _removeWhere(N: N, keepCapacity: true)
}
