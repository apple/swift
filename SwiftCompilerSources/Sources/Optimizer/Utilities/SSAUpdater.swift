//===--- SSAUpdater.swift -------------------------------------------------===//
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
import OptimizerBridging

/// Utility for updating SSA for a set of SIL instructions defined in multiple blocks.
struct SSAUpdater<Context: MutatingContext> {
  let context: Context

  init(type: Type, ownership: Ownership, _ context: Context) {
    self.context = context
    context._bridged.SSAUpdater_initialize(type.bridged, ownership._bridged)
  }

  mutating func addAvailableValue(_ value: Value, in block: BasicBlock) {
    context._bridged.SSAUpdater_addAvailableValue(block.bridged, value.bridged)
  }

  /// Construct SSA for a value that is live at the *end* of a basic block.
  mutating func getValue(atEndOf block: BasicBlock) -> Value {
    context.notifyInstructionsChanged()
    return context._bridged.SSAUpdater_getValueAtEndOfBlock(block.bridged).value
  }

  /// Construct SSA for a value that is live in the *middle* of a block.
  /// This handles the case where the use is before a definition of the value in the same block.
  ///
  ///   bb1:
  ///     %1 = def
  ///     br bb2
  ///   bb2:
  ///       = use(?)
  ///    %2 = def
  ///    cond_br bb2, bb3
  ///
  /// In this case we need to insert a phi argument in bb2, merging %1 and %2.
  mutating func getValue(inMiddleOf block: BasicBlock) -> Value {
    context.notifyInstructionsChanged()
    return context._bridged.SSAUpdater_getValueInMiddleOfBlock(block.bridged).value
  }
}
