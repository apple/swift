//===--- Verifier.swift ---------------------------------------------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2024 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//

import SIL
import OptimizerBridging

private protocol VerifyableInstruction : Instruction {
  func verify(_ context: FunctionPassContext)
}

private func require(_ condition: Bool, _ message: @autoclosure () -> String) {
  if !condition {
    fatalError(message())
  }
}

extension Function {
  func verify(_ context: FunctionPassContext) {
    for block in blocks {
      for arg in block.arguments {
        arg.verify(context)
      }
      for inst in block.instructions {

        inst.checkForwardingConformance()

        if let verifyableInst = inst as? VerifyableInstruction {
          verifyableInst.verify(context)
        }
      }
    }
  }
}

private extension Instruction {
  func checkForwardingConformance() {
    if bridged.shouldBeForwarding() {
      require(self is ForwardingInstruction, "instruction \(self)\nshould conform to ForwardingInstruction")
    } else {
      require(!(self is ForwardingInstruction), "instruction \(self)\nshould not conform to ForwardingInstruction")
    }
  }
}

extension BorrowedFromInst : VerifyableInstruction {
  func verify(_ context: FunctionPassContext) {
    var computedEVs = Stack<Value>(context)
    defer { computedEVs.deinitialize() }

    guard let phi = Phi(borrowedValue) else {
      fatalError("borrowed value of borrowed-from must be a phi: \(self)")
    }
    gatherEnclosingValuesFromPredecessors(for: phi, in: &computedEVs, context)

    var existingEVs = ValueSet(insertContentsOf: enclosingValues, context)
    defer { existingEVs.deinitialize() }

    for computedEV in enclosingValues {
      require(existingEVs.contains(computedEV),
                   "\(computedEV)\n  missing in enclosing values of \(self)")
    }
  }
}

private extension Argument {
  func verify(_ context: FunctionPassContext) {
    if let phi = Phi(self), phi.value.ownership == .guaranteed {
      var forwardingBorrowedFromFound = false
      for use in phi.value.uses {
        require(use.instruction is BorrowedFromInst,
                     "guaranteed phi: \(self)\n has non borrowed-from use: \(use)")
        if use.index == 0 {
          require(!forwardingBorrowedFromFound, "phi \(self) has multiple forwarding borrowed-from uses")
          forwardingBorrowedFromFound = true
        }
      }
      require (forwardingBorrowedFromFound,
                   "missing forwarding borrowed-from user of guaranteed phi \(phi)")
    }
  }
}

func registerVerifier() {
  BridgedUtilities.registerVerifier(
    { (bridgedCtxt: BridgedPassContext, bridgedFunction: BridgedFunction) in
      let context = FunctionPassContext(_bridged: bridgedCtxt)
      bridgedFunction.function.verify(context)
    }
  )
}
