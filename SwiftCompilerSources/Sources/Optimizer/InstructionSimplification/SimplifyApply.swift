//===--- SimplifyApply.swift ----------------------------------------------===//
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

extension ApplyInst : OnoneSimplifyable {
  func simplify(_ context: SimplifyContext) {
    _ = context.tryDevirtualize(apply: self, isMandatory: false)
  }
}

extension TryApplyInst : OnoneSimplifyable {
  func simplify(_ context: SimplifyContext) {
    _ = context.tryDevirtualize(apply: self, isMandatory: false)
  }
}

extension BeginApplyInst : OnoneSimplifyable {
  func simplify(_ context: SimplifyContext) {
    _ = context.tryDevirtualize(apply: self, isMandatory: false)
  }
}
