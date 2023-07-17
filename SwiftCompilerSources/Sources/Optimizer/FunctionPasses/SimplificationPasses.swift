//===--- SimplificationPasses.swift ----------------------------------------==//
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

//===--------------------------------------------------------------------===//
//                        Instruction protocols
//===--------------------------------------------------------------------===//

/// Instructions which can be simplified at all optimization levels
protocol Simplifyable : Instruction {
  func simplify(_ context: SimplifyContext)
}

/// Instructions which can be simplified at -Onone
protocol OnoneSimplifyable : Simplifyable {
}

/// Instructions which can only be simplified at the end of the -Onone pipeline
protocol LateOnoneSimplifyable : Instruction {
  func simplifyLate(_ context: SimplifyContext)
}

//===--------------------------------------------------------------------===//
//                        Simplification passes
//===--------------------------------------------------------------------===//

let ononeSimplificationPass = FunctionPass(name: "onone-simplification") {
  (function: Function, context: FunctionPassContext) in

  _ = runSimplification(on: function, context, preserveDebugInfo: true) {
    if let i = $0 as? OnoneSimplifyable {
      i.simplify($1)
    }
  }
}

let simplificationPass = FunctionPass(name: "simplification") {
  (function: Function, context: FunctionPassContext) in

  _ = runSimplification(on: function, context, preserveDebugInfo: false) {
    if let i = $0 as? Simplifyable {
      i.simplify($1)
    }
  }
}

let lateOnoneSimplificationPass = FunctionPass(name: "late-onone-simplification") {
  (function: Function, context: FunctionPassContext) in

  _ = runSimplification(on: function, context, preserveDebugInfo: true) {
    if let i = $0 as? LateOnoneSimplifyable {
      i.simplifyLate($1)
    } else if let i = $0 as? OnoneSimplifyable {
      i.simplify($1)
    }
  }
}

//===--------------------------------------------------------------------===//
//                         Pass implementation
//===--------------------------------------------------------------------===//


func runSimplification(on function: Function, _ context: FunctionPassContext,
                       preserveDebugInfo: Bool,
                       _ simplify: (Instruction, SimplifyContext) -> ()) -> Bool {
  var worklist = InstructionWorklist(context)
  defer { worklist.deinitialize() }

  var changed = false
  let simplifyCtxt = context.createSimplifyContext(preserveDebugInfo: preserveDebugInfo,
                                                   notifyInstructionChanged: {
    worklist.pushIfNotVisited($0)
    changed = true
  })

  // Push in reverse order so that popping from the tail of the worklist visits instruction in forward order again.
  worklist.pushIfNotVisited(contentsOf: function.reversedInstructions)

  // Run multiple iterations because cleanupDeadCode can add new candidates to the worklist.
  repeat {

    // The core worklist-loop.
    while let instruction = worklist.popAndForget() {
      if instruction.isDeleted {
        continue
      }
      if !context.options.enableSimplification(for: instruction) {
        continue
      }
      if !context.continueWithNextSubpassRun(for: instruction) {
        return changed
      }
      simplify(instruction, simplifyCtxt)
    }

    cleanupDeadInstructions(in: function, preserveDebugInfo, context)
    cleanupDeadBlocks(in: function, pushNewCandidatesTo: &worklist, context)

  } while !worklist.isEmpty

  if context.needFixStackNesting {
    function.fixStackNesting(context)
  }
  
  return changed
}

private func cleanupDeadInstructions(in function: Function,
                                     _ preserveDebugInfo: Bool,
                                     _ context: FunctionPassContext) {
  if preserveDebugInfo {
    context.removeTriviallyDeadInstructionsPreservingDebugInfo(in: function)
  } else {
    context.removeTriviallyDeadInstructionsIgnoringDebugUses(in: function)
  }
}

private func cleanupDeadBlocks(in function: Function,
                               pushNewCandidatesTo worklist: inout InstructionWorklist,
                               _ context: FunctionPassContext) {
  if context.removeDeadBlocks(in: function) {
    // After deleting dead blocks their (still alive) successor blocks may become eligible for block merging.
    // Therefore we re-run simplification for all branch instructions.
    for block in function.blocks.reversed() {
      if let bi = block.terminator as? BranchInst {
        worklist.pushIfNotVisited(bi)
      }
    }
  }
}
