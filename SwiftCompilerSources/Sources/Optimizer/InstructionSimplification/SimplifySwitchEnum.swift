//===--- SimplifySwitchEnum.swift -----------------------------------------===//
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

// Removes an `enum` - `switch_enum` pair:
// ```
//     %1 = enum $E, #someCase, %payload
//     switch_enum %1, case #someCase: bb1, ...
//   bb1(%payloadArgument):
// ```
// ->
// ```
//   br bb1(%payload)
//   bb1(%payloadArgument):
// ```
//
// Other case blocks of the switch_enum become dead.
//
extension SwitchEnumInst : OnoneSimplifyable {
  func simplify(_ context: SimplifyContext) {
    guard let enumInst = enumOp as? EnumInst,
          let caseBlock = getUniqueSuccessor(forCaseIndex: enumInst.caseIndex) else
    {
      return
    }

    let singleUse = context.preserveDebugInfo ? enumInst.uses.singleUse : enumInst.uses.ignoreDebugUses.singleUse
    let canEraseEnumInst = singleUse?.instruction == self

    if !canEraseEnumInst && parentFunction.hasOwnership && enumInst.ownership == .owned {
      // We cannot add more uses to the `enum` instruction without inserting a copy.
      return
    }

    let builder = Builder(before: self, context)
    switch caseBlock.arguments.count {
    case 0:
      precondition(enumInst.payload == nil || !parentFunction.hasOwnership,
                   "missing payload argument in switch_enum case block")
      builder.createBranch(to: caseBlock)
      context.erase(instruction: self)
    case 1:
      builder.createBranch(to: caseBlock, arguments: [enumInst.payload!])
      context.erase(instruction: self)
      updateBorrowedFrom(for: [Phi(caseBlock.arguments[0])!], context)
    default:
      fatalError("case block of switch_enum cannot have more than 1 argument")
    }

    if canEraseEnumInst {
      context.erase(instructionIncludingDebugUses: enumInst)
    }
  }
}
