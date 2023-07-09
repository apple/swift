//===--- CString.swift ----------------------------------------------------===//
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

public let benchmarks = [
  BenchmarkInfo(name: "CStringLongAscii", runFunction: run_CStringLongAscii, tags: [.validation, .api, .String, .bridging]),
  BenchmarkInfo(name: "CStringLongNonAscii", runFunction: run_CStringLongNonAscii, tags: [.validation, .api, .String, .bridging]),
  BenchmarkInfo(name: "CStringShortAscii", runFunction: run_CStringShortAscii, tags: [.validation, .api, .String, .bridging], legacyFactor: 10),
  BenchmarkInfo(name: "StringWithCString2", runFunction: run_StringWithCString, tags: [.validation, .api, .String, .bridging],
      setUpFunction: { blackHole(repeatedStr) }, tearDownFunction: { repeatedStr = nil })
]

let ascii = "Swift is a multi-paradigm, compiled programming language created for iOS, OS X, watchOS, tvOS and Linux development by Apple Inc. Swift is designed to work with Apple's Cocoa and Cocoa Touch frameworks and the large body of existing Objective-C code written for Apple products. Swift is intended to be more resilient to erroneous code (\"safer\") than Objective-C and also more concise. It is built with the LLVM compiler framework included in Xcode 6 and later and uses the Objective-C runtime, which allows C, Objective-C, C++ and Swift code to run within a single program."
let japanese = "日本語（にほんご、にっぽんご）は、主に日本国内や日本人同士の間で使われている言語である。"

var repeatedStr: String! = String(repeating: "x", count: 5 * (1 << 16))

@inline(never)
public func run_StringWithCString(_ n: Int) {
  let str: String = repeatedStr
  for _ in 0 ..< n {
    str.withCString { blackHole($0) }
  }
}

@inline(never)
public func run_CStringLongAscii(_ n: Int) {
  var res = 0
  for _ in 1...n*500 {
    // static string to c -> from c to String -> implicit conversion
    res &= strlen(ascii.withCString(String.init(cString:)))
  }
  check(res == 0)
}

@inline(never)
public func run_CStringLongNonAscii(_ n: Int) {
  var res = 0
  for _ in 1...n*500 {
    res &= strlen(japanese.withCString(String.init(cString:)))
  }
  check(res == 0)
}


let input = ["-237392", "293715", "126809", "333779", "-362824", "144198",
             "-394973", "-163669", "-7236", "376965", "-400783", "-118670",
             "454728", "-38915", "136285", "-448481", "-499684", "68298",
             "382671", "105432", "-38385", "39422", "-267849", "-439886",
             "292690", "87017", "404692", "27692", "486408", "336482",
             "-67850", "56414", "-340902", "-391782", "414778", "-494338",
             "-413017", "-377452", "-300681", "170194", "428941", "-291665",
             "89331", "329496", "-364449", "272843", "-10688", "142542",
             "-417439", "167337", "96598", "-264104", "-186029", "98480",
             "-316727", "483808", "300149", "-405877", "-98938", "283685",
             "-247856", "-46975", "346060", "160085",]
let reference = 517492

@inline(never)
public func run_CStringShortAscii(_ n: Int) {

  func doOneIter(_ arr: [String]) -> Int {
    var r = 0
    for n in arr {
      r += Int(atoi(n))
    }
    if r < 0 {
      r = -r
    }
    return r
  }

  var res = Int.max
  for _ in 1...10*n {
    let strings = input.map {
      $0.withCString(String.init(cString:))
    }
    res = res & doOneIter(strings)
  }
  check(res == reference)
}
