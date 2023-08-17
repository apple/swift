//===--- MirrorWithPacks.swift -----------------------------------------------------===//
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
// RUN: %empty-directory(%t)
// RUN: cp %s %t/main.swift
// RUN: %target-build-swift %t/main.swift -o %t/Mirror -Xfrontend -disable-availability-checking
// RUN: %target-codesign %t/Mirror
// RUN: %target-run %t/Mirror

// REQUIRES: executable_test
// REQUIRES: shell
// REQUIRES: reflection

// rdar://96439408
// UNSUPPORTED: use_os_stdlib

import StdlibUnittest

var mirrors = TestSuite("MirrorWithPacks")

struct Tuple<each T> {
  var elements: (repeat each T)
  init(_ elements: repeat each T) {
    self.elements = (repeat each elements)
  }
}

mirrors.test("Packs") {
  let value = Tuple("hi", 3, false)
  var output = ""
  dump(value, to: &output)

  let expected =
    "▿ Mirror.Tuple<Pack{Swift.String, Swift.Int, Swift.Bool}>\n" +
    "  ▿ elements: (3 elements)\n" +
    "    - .0: \"hi\"\n" +
    "    - .1: 3\n" +
    "    - .2: false\n"

  expectEqual(expected, output)
}

runAllTests()