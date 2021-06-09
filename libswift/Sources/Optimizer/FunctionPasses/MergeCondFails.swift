//===--- MergeCondFail.swift -  Merge cond_fail instructions --------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2021 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//

import SIL

let mergeCondFailsPass = FunctionPass(name: "merge-cond_fails", runMergeCondFails)

/// Return true if the operand of the cond_fail instruction looks like
/// the overflow bit of an arithmetic instruction.
private func hasOverflowConditionOperand(_ cfi: CondFailInst) -> Bool {
  if let tei = cfi.condition as? TupleExtractInst {
    return tei.tuple is BuiltinInst
  }
  return false
}

/// Merge cond_fail instructions.
///
/// We can merge cond_fail instructions if there is no side-effect or memory
/// write in between them.
/// This pass merges cond_fail instructions by building the disjunction of
/// their operands.
private func runMergeCondFails(function: Function, context: FunctionPassContext) {

  // Merge cond_fail instructions if there is no side-effect or read in
  // between them.
  for block in function.blocks {
    // Per basic block list of cond_fails to merge.
    var condFailToMerge = StackList<CondFailInst>(context)

    for inst in block.instructions {
      if let cfi = inst as? CondFailInst {
        // Do not process arithmetic overflow checks. We typically generate more
        // efficient code with separate jump-on-overflow.
        if !hasOverflowConditionOperand(cfi) &&
           (condFailToMerge.isEmpty || cfi.message == condFailToMerge.first!.message) {
          condFailToMerge.push(cfi)
        }
      } else if inst.mayHaveSideEffects || inst.mayReadFromMemory {
        // Stop merging at side-effects or reads from memory.
        mergeCondFails(&condFailToMerge, context: context)
      }
    }
    // Process any remaining cond_fail instructions in the current basic
    // block.
    mergeCondFails(&condFailToMerge, context: context)
  }
}

/// Try to merge the cond_fail instructions. Returns true if any could
/// be merge.
private func mergeCondFails(_ condFailToMerge: inout StackList<CondFailInst>,
                            context: FunctionPassContext) {
  guard let lastCFI = condFailToMerge.last else {
    return
  }
  var mergedCond: Value? = nil
  var didMerge = false
  let builder = Builder(at: lastCFI.next!, location: lastCFI.location, context)

  // Merge conditions and remove the merged cond_fail instructions.
  for cfi in condFailToMerge {
    if let prevCond = mergedCond {
      mergedCond = builder.createBuiltinBinaryFunction(name: "or",
                                        operandType: prevCond.type,
                                        resultType: prevCond.type,
                                        arguments: [prevCond, cfi.condition])
      didMerge = true
    } else {
      mergedCond = cfi.condition
    }
  }
  if !didMerge {
    condFailToMerge.removeAll()
    return
  }

  // Create a new cond_fail using the merged condition.
  _ = builder.createCondFail(condition: mergedCond!,
                             message: lastCFI.message)

  while let cfi = condFailToMerge.pop() {
    context.erase(instruction: cfi)
  }
}
