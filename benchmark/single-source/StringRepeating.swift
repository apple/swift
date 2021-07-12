//===--- StringRepeating.swift -------------------------------------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2019 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//

import TestsUtils

public let StringRepeating = [
  BenchmarkInfo(name: "StringRepeatingSingleAsciiCharacterCount10",
                runFunction: run_singleAsciiCharacterCount10,
                tags: [.validation, .api, .String]),
  BenchmarkInfo(name: "StringRepeatingSingleAsciiCharacterCount1",
                runFunction: run_singleAsciiCharacterCount1,
                tags: [.validation, .api, .String]),
  BenchmarkInfo(name: "StringRepeatingEmptyStringCount10",
                runFunction: run_emptyStringCount10,
                tags: [.validation, .api, .String]),
  BenchmarkInfo(name: "StringRepeatingSingleAsciiCharacterCount0",
                runFunction: run_singleAsciiCharacterCount0,
                tags: [.validation, .api, .String]),
  BenchmarkInfo(name: "StringRepeating26AsciiCharactersCount2",
                runFunction: run_26AsciiCharactersCount2,
                tags: [.validation, .api, .String]),
  BenchmarkInfo(name: "StringRepeatingSingleCyrilicCharacterCount5",
                runFunction: run_singleCyrilicCharacterCount5,
                tags: [.validation, .api, .String]),
  BenchmarkInfo(name: "StringRepeating33CyrilicCharactersCount2",
                runFunction: run_33CyrilicCharactersCount2,
                tags: [.validation, .api, .String]),
  BenchmarkInfo(name: "StringRepeatingU1F1F8U1F1FACount2",
                runFunction: run_U1F1F8U1F1FACount2,
                tags: [.validation, .api, .String]),
  BenchmarkInfo(name: "StringRepeatingU301cafeCount5",
                runFunction: run_U301cafeCount5,
                tags: [.validation, .api, .String]),
  BenchmarkInfo(name: "StringRepeatingLongMixedStringCount100",
                runFunction: run_longMixedStringCount100,
                tags: [.validation, .api, .String])
]

@inline(never)
func repeating(_ i: String, count: Int) -> String {
  let s = String(repeating: getString(i), count: count)
  return s
}

@inline(never)
public func run_singleAsciiCharacterCount10(N: Int) {
    for _ in 1...5000*N {
        blackHole(repeating("x", count: 10))
    }
}

@inline(never)
public func run_singleAsciiCharacterCount1(N: Int) {
    for _ in 1...5000*N {
        blackHole(repeating("x", count: 1))
    }
}

@inline(never)
public func run_emptyStringCount10(N: Int) {
    for _ in 1...5000*N {
        blackHole(repeating("", count: 10))
    }
}

@inline(never)
public func run_singleAsciiCharacterCount0(N: Int) {
    for _ in 1...5000*N {
        blackHole(repeating("x", count: 0))
    }
}

@inline(never)
public func run_26AsciiCharactersCount2(N: Int) {
    for _ in 1...5000*N {
        blackHole(repeating("abcdefghijklmnopqrstuvwxyz", count: 2))
    }
}

@inline(never)
public func run_singleCyrilicCharacterCount5(N: Int) {
    for _ in 1...5000*N {
        blackHole(repeating("я", count: 5))
    }
}

@inline(never)
public func run_33CyrilicCharactersCount2(N: Int) {
    for _ in 1...5000*N {
        blackHole(repeating("абвгґдеєжзиіїйклмнопрстуфхцчшщьюя", count: 2))
    }
}

@inline(never)
public func run_U1F1F8U1F1FACount2(N: Int) {
    for _ in 1...5000*N {
        blackHole(repeating("\u{1F1F8}\u{1F1FA}\u{1F1F8}\u{1F1FA}", count: 2))
    }
}

@inline(never)
public func run_U301cafeCount5(N: Int) {
    for _ in 1...5000*N {
        blackHole(repeating("\u{301}cafe", count: 5))
    }
}

func getLongString() -> String {
  let long = """
    Swift is a multi-paradigm, compiled programming language created for
     iOS, OS X, watchOS, tvOS and Linux development by Apple Inc. Swift is
     designed to work with Apple's Cocoa and Cocoa Touch frameworks and the
     large body of existing Objective-C code written for Apple products. Swift
     is intended to be more resilient to erroneous code (\"safer\") than
     Objective-C and also more concise. It is built with the LLVM compiler
     framework included in Xcode 6 and later and uses the Objective-C runtime,
     which allows C, Objective-C, C++ and Swift code to run within a single
     program.
     Існує багато варіацій уривків з Lorem Ipsum, але більшість з них зазнала
     певних змін на кшталт жартівливих вставок або змішування слів, які навіть
     не виглядають правдоподібно.
     日本語の場合はランダムに生成された文章以外に、
     著作権が切れた小説などが利用されることもある。
     🦩
    """
  return getString(long)
}

@inline(never)
public func run_longMixedStringCount100(N: Int) {
    for _ in 1...5000*N {
        blackHole(repeating(getLongString(), count: 100))
    }
}
