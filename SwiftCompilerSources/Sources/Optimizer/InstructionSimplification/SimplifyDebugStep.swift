//===--- SimplifyDebugStep.swift ------------------------------------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2023 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//

import SIL

extension DebugStepInst : Simplifyable {
  func simplify(_ context: SimplifyContext) {
    // When compiling with optimizations (note: it's not a OnoneSimplifyable transformation),
    // unconditionally remove debug_step instructions.
    context.erase(instruction: self)
  }
}

