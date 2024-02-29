//===--- SimplifyBranch.swift ---------------------------------------------===//
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

extension BranchInst : OnoneSimplifyable {
  func simplify(_ context: SimplifyContext) {
    tryMergeWithTargetBlock(context)
  }
}

private extension BranchInst {
  func tryMergeWithTargetBlock(_ context: SimplifyContext) {
    if canMergeWithTargetBlock {
      mergeWithTargetBlock(context)
    }
  }

  var canMergeWithTargetBlock: Bool {
    // We can only merge if there is a 1:1 relation to the target block.
    guard let pred = targetBlock.singlePredecessor else {
      return false
    }
    assert(pred == parentBlock)

    // Ignore self cycles
    if targetBlock == parentBlock {
      return false
    }

    if hasInvalidDominanceCycle {
      return false
    }

    return true
  }

  func mergeWithTargetBlock(_ context: SimplifyContext) {
    let targetBB = targetBlock
    let parentBB = parentBlock

    for (argIdx, op) in operands.enumerated() {
      let arg = targetBB.arguments[argIdx]
      if let phi = Phi(arg),
         let bfi = phi.borrowedFrom
      {
        bfi.uses.replaceAll(with: op.value, context)
        context.erase(instruction: bfi)
      } else {
        arg.uses.replaceAll(with: op.value, context)
      }
    }
    targetBB.eraseAllArguments(context)

    if context.preserveDebugInfo {
      let builder = Builder(before: self, context)
      builder.createDebugStep()
    }
    context.erase(instruction: self)

    // Move instruction from the smaller block to the larger block.
    // The order is essential because if many blocks are merged and this is done
    // in the wrong order, we end up with quadratic complexity.
    //
    if parentBB.hasLessInstructions(than: targetBB) &&
       parentBB != parentBB.parentFunction.entryBlock {
      for pred in parentBB.predecessors {
        pred.terminator.replaceBranchTarget(from: parentBB, to: targetBB, context)
      }
      parentBB.moveAllInstructions(toBeginOf: targetBB, context)
      parentBB.moveAllArguments(to: targetBB, context)
      context.erase(block: parentBB)
    } else {
      targetBB.moveAllInstructions(toEndOf: parentBB, context)
      context.erase(block: targetBB)
    }
  }
}

private extension BasicBlock {
  func hasLessInstructions(than otherBlock: BasicBlock) -> Bool {
    var insts = instructions
    var otherInsts = otherBlock.instructions
    while true {
      if otherInsts.next() == nil {
        return false
      }
      if insts.next() == nil {
        return true
      }
    }
  }
}

private extension BranchInst {

  // True if this block is part of an unreachable cfg cycle, where an argument dominates itself.
  // For example:
  // ```
  // bb1(arg1): // preds: bb2
  //   br bb2
  //
  // bb2: // preds: bb1
  //   br bb1(arg1)
  // ```
  var hasInvalidDominanceCycle: Bool {
    for (argIdx, op) in operands.enumerated() {
      if targetBlock.arguments[argIdx] == op.value {
        return true
      }
    }
    return false
  }
}
