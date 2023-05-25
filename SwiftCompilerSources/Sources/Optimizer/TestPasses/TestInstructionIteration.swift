//===--- TestInstructionIteration.swift -----------------------------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2022 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//

import SIL

/// Tests instruction iteration while modifying the instruction list.
///
/// This pass iterates over the instruction list of the function's block and performs
/// modifications of the instruction list - mostly deleting instructions.
/// Modifications are triggered by `string_literal` instructions with known "commands".
/// E.g. if a
/// ```
///   %1 = string_literal utf8 "delete_strings"
/// ```
/// is encountered during the iteration, it triggers the deletion of all `string_literal`
/// instructions of the basic block (including the current one).
let testInstructionIteration = FunctionPass(name: "test-instruction-iteration") {
  (function: Function, context: FunctionPassContext) in

  print("Test instruction iteration in \(function.name):")

  let reverse = function.name.string.hasSuffix("backward")

  for block in function.blocks {
    print("\(block.name):")
    let termLoc = block.terminator.location
    if reverse {
      for inst in block.instructions.reversed() {
        handle(instruction: inst, context)
      }
    } else {
      for inst in block.instructions {
        handle(instruction: inst, context)
      }
    }
    if block.instructions.isEmpty || !(block.instructions.reversed().first is TermInst) {
      let builder = Builder(atEndOf: block, location: termLoc, context)
      builder.createUnreachable()
    }
  }
  print("End function \(function.name):")
}

private func handle(instruction: Instruction, _ context: FunctionPassContext) {
  print(instruction)
  if let sl = instruction as? StringLiteralInst {
    switch sl.value {
      case "delete_strings":
        deleteAllInstructions(ofType: StringLiteralInst.self, in: instruction.parentBlock, context)
      case "delete_ints":
        deleteAllInstructions(ofType: IntegerLiteralInst.self, in: instruction.parentBlock, context)
      case "delete_branches":
        deleteAllInstructions(ofType: BranchInst.self, in: instruction.parentBlock, context)
      case "split_block":
        _ = context.splitBlock(at: instruction)
      default:
        break
    }
  }
}

private func deleteAllInstructions<InstType: Instruction>(ofType: InstType.Type, in block: BasicBlock, _ context: FunctionPassContext) {
  for inst in block.instructions {
    if inst is InstType {
      context.erase(instruction: inst)
    }
  }
}
