//===--- StaticInitCloner.swift --------------------------------------------==//
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

/// Clones the initializer value of a GlobalVariable.
///
/// Used to transitively clone "constant" instructions, including their operands,
/// from or to the static initializer value of a GlobalVariable.
///
struct StaticInitCloner<Context: MutatingContext> {
  private var bridged: BridgedCloner
  private let context: Context
  private let cloningIntoFunction: Bool

  init(cloneTo global: GlobalVariable, _ context: Context) {
    self.bridged = BridgedCloner(global.bridged, context._bridged)
    self.context = context
    self.cloningIntoFunction = false
  }

  init(cloneBefore inst: Instruction, _ context: Context) {
    self.bridged = BridgedCloner(inst.bridged, context._bridged)
    self.context = context
    self.cloningIntoFunction = true
  }

  mutating func deinitialize() {
    bridged.destroy(context._bridged)
  }

  /// Transitively clones `value` including its defining instruction's operands.
  mutating func clone(_ value: Value) -> Value {

    if isCloned(value: value) {
      return getClonedValue(of: value)
    }

    let inst = value.definingInstruction!
    for op in inst.operands {
      _ = clone(op.value)
    }

    bridged.clone(inst.bridged)
    let clonedValue = getClonedValue(of: value)
    if cloningIntoFunction {
      context.notifyInstructionChanged(clonedValue.definingInstruction!)
    }
    return clonedValue
  }

  mutating func getClonedValue(of value: Value) -> Value {
    bridged.getClonedValue(value.bridged).value
  }

  func isCloned(value: Value) -> Bool {
    bridged.isValueCloned(value.bridged)
  }
}
