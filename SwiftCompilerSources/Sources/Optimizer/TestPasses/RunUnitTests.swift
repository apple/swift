//===--- UnitTests.swift - A pseudo pass for running the unit tests -------===//
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

import SIL

/// This pass should only be used by sil-opt to run all the unit tests.
///
let runUnitTests = ModulePass(name: "run-unit-tests") {
    (context: ModulePassContext) in

  print("--- Run unit tests ---")
  
  print("test ProjectionPath")
  SmallProjectionPath.runUnitTests()
}
