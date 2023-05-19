//===--- SimplifyCondFail.swift -------------------------------------------===//
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

extension CondFailInst : OnoneSimplifyable {
  func simplify(_ context: SimplifyContext) {

    /// Eliminates
    /// ```
    ///   %0 = integer_literal 0
    ///   cond_fail %0, "message"
    /// ```
    if let literal = condition as? IntegerLiteralInst,
       literal.value.isZero() {

      context.erase(instruction: self)
    }
  }
}
