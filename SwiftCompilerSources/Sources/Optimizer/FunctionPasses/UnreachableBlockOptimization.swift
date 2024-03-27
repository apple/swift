//===--- UnreachableBlockOptimization.swift --------------------------------==//
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

let unreachableBlockOptimization = FunctionPass(name: "unreachable-block-optimization", 
                                                UnreachableBlockOptimization.perform)

// Remove instructions that don't have strongly ordered side effects from
// dead-end blocks.
fileprivate struct UnreachableBlockOptimization {
  static func perform(function: Function, context: FunctionPassContext) {
    var pass = UnreachableBlockOptimization(function: function, context: context)
    defer { pass.deinitialize() }
    pass.run()
  }

  let function: Function
  let context: FunctionPassContext
  let deadEndBlocks: DeadEndBlocksAnalysis
  var worklist: InstructionWorklist
  var found: [Instruction]

  init(function: Function, context: FunctionPassContext) {
    self.function = function
    self.context = context

    self.deadEndBlocks = context.deadEndBlocks
    self.worklist = .init(context)
    self.found = []
  }

  mutating func run() {
    // Walk backwards, within dead-end blocks, from each unreachable until
    // finding an "interesting" instruction, deleting each uninteresting
    // instruction along the way.
    initializeWorklist()
    findInstructions()
    deleteInstructions()
  }

  mutating func deinitialize() {
    worklist.deinitialize()
  }

  mutating func initializeWorklist() {
    for block in function.blocks where block.terminator is UnreachableInst {
      addInstructions(before: block.terminator)
    }
  }

  mutating func findInstructions() {
    while let instruction = worklist.pop() {
      if canSkip(instruction) {
        addInstructions(before: instruction)
        continue
      }
      if !canDelete(instruction) {
        continue
      }
      found.append(instruction)
      addInstructions(before: instruction)
    }
  }

  func deleteInstructions() {
    for instruction in found {
      context.erase(instruction: instruction)
    }
  }

  mutating func addInstructions(before instruction: Instruction) {
    if let previous = instruction.previous {
      worklist.pushIfNotVisited(previous)
      return
    }
    for predecessor in instruction.parentBlock.predecessors {
      if !deadEndBlocks.isDeadEnd(predecessor) {
        continue
      }
      worklist.pushIfNotVisited(predecessor.terminator)
    }
  }

  func canDelete(_ instruction: Instruction) -> Bool {
    return instruction is RefCountingInst
        || instruction is DestroyAddrInst
        || instruction is DeallocStackInst
  }

  func canSkip(_ instruction: Instruction) -> Bool {
    return instruction is BranchInst
        || instruction is CondBranchInst
  }
}
