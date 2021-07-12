//===--- UTF16Decode.swift -------------------------------------------------===//
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

import TestsUtils
import Foundation

public let UTF16Decode = [
    BenchmarkInfo(
      name: "UTF16Decode",
      runFunction: run_UTF16Decode,
      tags: [.validation, .api, .String]),
    BenchmarkInfo(
      name: "UTF16Decode_InitFromCustom_contiguous",
      runFunction: run_UTF16Decode_InitFromCustom_contiguous,
      tags: [.validation, .api, .String]),
    BenchmarkInfo(
      name: "UTF16Decode_InitFromCustom_contiguous_ascii",
      runFunction: run_UTF16Decode_InitFromCustom_contiguous_ascii,
      tags: [.validation, .api, .String]),
    BenchmarkInfo(
      name: "UTF16Decode_InitFromCustom_noncontiguous",
      runFunction: run_UTF16Decode_InitFromCustom_noncontiguous,
      tags: [.validation, .api, .String]),
    BenchmarkInfo(
      name: "UTF16Decode_InitFromCustom_noncontiguous_ascii",
      runFunction: run_UTF16Decode_InitFromCustom_noncontiguous_ascii,
      tags: [.validation, .api, .String]),
    BenchmarkInfo(
      name: "UTF16Decode_InitFromData",
      runFunction: run_UTF16Decode_InitFromData,
      tags: [.validation, .api, .String]),
    BenchmarkInfo(
      name: "UTF16Decode_InitDecoding",
      runFunction: run_UTF16Decode_InitDecoding,
      tags: [.validation, .api, .String]),
    BenchmarkInfo(
      name: "UTF16Decode_InitFromData_ascii",
      runFunction: run_UTF16Decode_InitFromData_ascii,
      tags: [.validation, .api, .String]),
    BenchmarkInfo(
      name: "UTF16Decode_InitDecoding_ascii",
      runFunction: run_UTF16Decode_InitDecoding_ascii,
      tags: [.validation, .api, .String]),
    BenchmarkInfo(
      name: "UTF16Decode_InitFromData_ascii_as_ascii",
      runFunction: run_UTF16Decode_InitFromData_ascii_as_ascii,
      tags: [.validation, .api, .String]),
]

typealias CodeUnit = UInt16

// 1-byte sequences
// This test case is the longest as it's the most performance sensitive.
let ascii = "Swift is a multi-paradigm, compiled programming language created for iOS, OS X, watchOS, tvOS and Linux development by Apple Inc. Swift is designed to work with Apple's Cocoa and Cocoa Touch frameworks and the large body of existing Objective-C code written for Apple products. Swift is intended to be more resilient to erroneous code (\"safer\") than Objective-C and also more concise. It is built with the LLVM compiler framework included in Xcode 6 and later and uses the Objective-C runtime, which allows C, Objective-C, C++ and Swift code to run within a single program."
let asciiCodeUnits: [CodeUnit] = Array(ascii.utf16)
let asciiData: Data = asciiCodeUnits.withUnsafeBytes { Data($0) }

// 2-byte sequences
let russian = "Ру́сский язы́к один из восточнославянских языков, национальный язык русского народа."
// 3-byte sequences
let japanese = "日本語（にほんご、にっぽんご）は、主に日本国内や日本人同士の間で使われている言語である。"
// 4-byte sequences
// Most commonly emoji, which are usually mixed with other text.
let emoji = "Panda 🐼, Dog 🐶, Cat 🐱, Mouse 🐭."

let allStrings: [[CodeUnit]] = [ascii, russian, japanese, emoji].map { Array($0.utf16) }
let allStringsCodeUnits: [CodeUnit] = Array(allStrings.joined())
let allStringsData: Data = allStringsCodeUnits.withUnsafeBytes { Data($0) }


@inline(never)
public func run_UTF16Decode(_ N: Int) {
  let strings = allStrings

  func isEmpty(_ result: UnicodeDecodingResult) -> Bool {
    switch result {
    case .emptyInput:
      return true
    default:
      return false
    }
  }

  for _ in 1...200*N {
    for string in strings {
      var it = string.makeIterator()
      var utf16 = UTF16()
      while !isEmpty(utf16.decode(&it)) { }
    }
  }
}

@inline(never)
public func run_UTF16Decode_InitFromData(_ N: Int) {
  let input = allStringsData
  for _ in 0..<200*N {
    blackHole(String(data: input, encoding: .utf16))
  }
}

@inline(never)
public func run_UTF16Decode_InitDecoding(_ N: Int) {
    let input: [CodeUnit] = allStringsCodeUnits
  for _ in 0..<200*N {
    blackHole(String(decoding: input, as: UTF16.self))
  }
}

@inline(never)
public func run_UTF16Decode_InitFromData_ascii(_ N: Int) {
  let input = asciiData
  for _ in 0..<1_000*N {
    blackHole(String(data: input, encoding: .utf16))
  }
}

@inline(never)
public func run_UTF16Decode_InitDecoding_ascii(_ N: Int) {
  let input = asciiCodeUnits
  for _ in 0..<1_000*N {
    blackHole(String(decoding: input, as: UTF16.self))
  }
}

@inline(never)
public func run_UTF16Decode_InitFromData_ascii_as_ascii(_ N: Int) {
  let input = asciiData
  for _ in 0..<1_000*N {
    blackHole(String(data: input, encoding: .ascii))
  }
}

struct CustomContiguousCollection: Collection {
  let storage: [CodeUnit]
  typealias Index = Int
  typealias Element = CodeUnit

  init(_ codeUnits: [CodeUnit]) { self.storage = codeUnits }
  subscript(position: Int) -> Element { self.storage[position] }
  var startIndex: Index { 0 }
  var endIndex: Index { storage.count }
  func index(after i: Index) -> Index { i+1 }

  @inline(never)
  func withContiguousStorageIfAvailable<R>(
    _ body: (UnsafeBufferPointer<CodeUnit>) throws -> R
  ) rethrows -> R? {
    try storage.withContiguousStorageIfAvailable(body)
  }
}
struct CustomNoncontiguousCollection: Collection {
  let storage: [CodeUnit]
  typealias Index = Int
  typealias Element = CodeUnit

  init(_ codeUnits: [CodeUnit]) { self.storage = codeUnits }
  subscript(position: Int) -> Element { self.storage[position] }
  var startIndex: Index { 0 }
  var endIndex: Index { storage.count }
  func index(after i: Index) -> Index { i+1 }

  @inline(never)
  func withContiguousStorageIfAvailable<R>(
    _ body: (UnsafeBufferPointer<UInt8>) throws -> R
  ) rethrows -> R? {
    nil
  }
}
let allStringsCustomContiguous = CustomContiguousCollection(allStringsCodeUnits)
let asciiCustomContiguous = CustomContiguousCollection(Array(ascii.utf16))
let allStringsCustomNoncontiguous = CustomNoncontiguousCollection(allStringsCodeUnits)
let asciiCustomNoncontiguous = CustomNoncontiguousCollection(Array(ascii.utf16))

@inline(never)
public func run_UTF16Decode_InitFromCustom_contiguous(_ N: Int) {
  let input = allStringsCustomContiguous
  for _ in 0..<200*N {
    blackHole(String(decoding: input, as: UTF16.self))
  }
}

@inline(never)
public func run_UTF16Decode_InitFromCustom_contiguous_ascii(_ N: Int) {
  let input = asciiCustomContiguous
  for _ in 0..<1_000*N {
    blackHole(String(decoding: input, as: UTF16.self))
  }
}

@inline(never)
public func run_UTF16Decode_InitFromCustom_noncontiguous(_ N: Int) {
  let input = allStringsCustomNoncontiguous
  for _ in 0..<200*N {
    blackHole(String(decoding: input, as: UTF16.self))
  }
}

@inline(never)
public func run_UTF16Decode_InitFromCustom_noncontiguous_ascii(_ N: Int) {
  let input = asciiCustomNoncontiguous
  for _ in 0..<1_000*N {
    blackHole(String(decoding: input, as: UTF16.self))
  }
}
