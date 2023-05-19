//===--- SimplifyDestructure.swift ----------------------------------------===//
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

extension DestructureTupleInst : OnoneSimplifyable {
  func simplify(_ context: SimplifyContext) {

    // Eliminate the redundant instruction pair
    // ```
    //   %t = tuple (%0, %1, %2)
    //   (%3, %4, %5) = destructure_tuple %t
    // ```
    // and replace the results %3, %4, %5 with %0, %1, %2, respectively
    //
    if let tuple = self.tuple as? TupleInst {
      tryReplaceConstructDestructPair(construct: tuple, destruct: self, context)
    }
  }
}

extension DestructureStructInst : OnoneSimplifyable {
  func simplify(_ context: SimplifyContext) {

    // Eliminate the redundant instruction pair
    // ```
    //   %s = struct (%0, %1, %2)
    //   (%3, %4, %5) = destructure_struct %s
    // ```
    // and replace the results %3, %4, %5 with %0, %1, %2, respectively
    //
    if let str = self.struct as? StructInst {
      tryReplaceConstructDestructPair(construct: str, destruct: self, context)
    }
  }
}

private func tryReplaceConstructDestructPair(construct: SingleValueInstruction,
                                             destruct: MultipleValueInstruction,
                                             _ context: SimplifyContext) {
  let singleUse = context.preserveDebugInfo ? construct.uses.singleUse : construct.uses.singleNonDebugUse
  let canEraseFirst = singleUse?.instruction == destruct

  if !canEraseFirst && construct.parentFunction.hasOwnership && construct.ownership == .owned {
    // We cannot add more uses to this tuple without inserting a copy.
    return
  }

  for (result, operand) in zip(destruct.results, construct.operands) {
    result.uses.replaceAll(with: operand.value, context)
  }

  if canEraseFirst {
    context.erase(instructionIncludingDebugUses: destruct)
  }
}
