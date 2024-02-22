//===--- ManglingUtils.swift --------------------------------------------==//
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

import OptimizerBridging
import SIL

/// The mangler for functions where arguments or effects are specialized.
struct FunctionSignatureSpecializationMangler {
  private var bridged: BridgedFunctionSignatureSpecializationMangler

  init(specializationPass: SpecializationPass, isSerialized: Bool, function: Function) {
    bridged = BridgedFunctionSignatureSpecializationMangler(
      specializationPass, isSerialized, function.bridged)
  }

  public func setArgumentClosureProp(argIndex: Int, instruction: Instruction) {
    bridged.setArgumentClosureProp(argIndex, instruction.bridged)
  }

  public func mangle() -> String {
    return String(taking: bridged.mangle())
  }
}
