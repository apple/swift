//===--- SimplifyRefCasts.swift -------------------------------------------===//
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

// Note: this simplifications are not SILCombineSimplifyable, because SILCombine has
// its own simplifications for those cast instructions which are not ported to Swift, yet.

extension CheckedCastBranchInst : OnoneSimplifyable {
  func simplify(_ context: SimplifyContext) {
    // Has only an effect if the source is an (existential) reference.
    simplifySourceOperandOfRefCast(context)
  }
}

extension UncheckedRefCastInst : OnoneSimplifyable {
  func simplify(_ context: SimplifyContext) {
    simplifySourceOperandOfRefCast(context)
  }
}

private extension UnaryInstruction {

  /// Look through `upcast` and `init_existential_ref` instructions and replace the
  /// operand of this cast instruction with the original value.
  /// For example:
  /// ```
  ///   %2 = upcast %1 : $Derived to $Base
  ///   %3 = init_existential_ref %2 : $Base : $Base, $AnyObject
  ///   checked_cast_br %3 : $AnyObject to Derived, bb1, bb2
  /// ```
  ///
  /// This makes it more likely that the cast can be constant folded because the source
  /// operand's type is more accurate. In the example above, the cast reduces to
  /// ```
  ///   checked_cast_br %1 : $Derived to Derived, bb1, bb2
  /// ```
  /// which can be trivially folded to always-succeeds.
  ///
  func simplifySourceOperandOfRefCast(_ context: SimplifyContext) {
    while true {
      switch operand.value {
      case let ier as InitExistentialRefInst:
        if !tryReplaceSource(withOperandOf: ier, context) {
          return
        }
      case let uc as UpcastInst:
        if !tryReplaceSource(withOperandOf: uc, context) {
          return
        }
      default:
        return
      }
    }

  }

  func tryReplaceSource(withOperandOf inst: SingleValueInstruction, _ context: SimplifyContext) -> Bool {
    let singleUse = context.preserveDebugInfo ? inst.uses.singleUse : inst.uses.singleNonDebugUse
    let canEraseInst = singleUse?.instruction == self
    let replacement = inst.operands[0].value

    if parentFunction.hasOwnership {
      if !canEraseInst && replacement.ownership == .owned {
        // We cannot add more uses to `replacement` without inserting a copy.
        return false
      }

      operand.set(to: replacement, context)

      if let ccb = self as? CheckedCastBranchInst {
        // In OSSA, the source value is passed as block argument to the failure block.
        // We have to re-create the skipped source instruction in the failure block.
        insertCompensatingInstructions(for: inst, in: ccb.failureBlock, context)
      }
    } else {
      operand.set(to: replacement, context)
    }

    if canEraseInst {
      context.erase(instructionIncludingDebugUses: inst)
    }
    return true
  }
}

/// Compensate a removed source value instruction in the failure block.
/// For example:
/// ```
///   %inst = upcast %sourceValue : $Derived to $Base
///   checked_cast_br %inst : $Base to Derived, success_block, failure_block
///   ...
/// failure_block(%oldArg : $Base):
/// ```
/// is converted to:
/// ```
///   checked_cast_br %sourceValue : $Derived to Derived, success_block, failure_block
///   ...
/// failure_block(%newArg : $Derived):
///   %3 = upcast %newArg : $Derived to $Base
/// ```
private func insertCompensatingInstructions(for inst: Instruction, in failureBlock: BasicBlock, _ context: SimplifyContext) {
  assert(failureBlock.arguments.count == 1)
  let sourceValue = inst.operands[0].value
  let newArg = failureBlock.addBlockArgument(type: sourceValue.type, ownership: sourceValue.ownership, context)
  let builder = Builder(atBeginOf: failureBlock, context)
  let newInst: SingleValueInstruction
  switch inst {
  case let ier as InitExistentialRefInst:
    newInst = builder.createInitExistentialRef(instance: newArg, existentialType: ier.type, useConformancesOf: ier)
  case let uc as UpcastInst:
    newInst = builder.createUpcast(from: newArg, to: uc.type)
  default:
    fatalError("unhandled instruction")
  }
  let oldArg = failureBlock.arguments[0]
  oldArg.uses.replaceAll(with: newInst, context)
  failureBlock.eraseArgument(at: 0, context)
}
