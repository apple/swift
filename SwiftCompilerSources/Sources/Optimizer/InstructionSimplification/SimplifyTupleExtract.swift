//===--- SimplifyTupleExtract.swift ---------------------------------------===//
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

extension TupleExtractInst : OnoneSimplifyable {
  func simplify(_ context: SimplifyContext) {

    // Replace tuple_extract(tuple(x)) -> x

    guard let tupleInst = tuple as? TupleInst else {
      return
    }
    context.tryReplaceRedundantInstructionPair(first: tupleInst, second: self,
                                               with: tupleInst.operands[fieldIndex].value)
  }
}
