//===--- OptUtils.swift - Utilities for optimizations ----------------------===//
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

extension Value {
  var nonDebugUses: LazyFilterSequence<UseList> {
    uses.lazy.filter { !($0.instruction is DebugValueInst) }
  }
}

extension Builder {
  static func insert(after inst: Instruction, location: Location,
                     _ context: some MutatingContext, insertFunc: (Builder) -> ()) {
    if inst is TermInst {
      for succ in inst.parentBlock.successors {
        assert(succ.hasSinglePredecessor,
               "the terminator instruction must not have critical successors")
        let builder = Builder(before: succ.instructions.first!, location: location,
                              context)
        insertFunc(builder)
      }
    } else {
      let builder = Builder(after: inst, location: location, context)
      insertFunc(builder)
    }
  }
}

extension Value {
  /// Makes this new owned value available to be used in the block `destBlock`.
  ///
  /// Inserts required `copy_value` and `destroy_value` operations in case the `destBlock`
  /// is in a different control region than this value. For example, if `destBlock` is
  /// in a loop while this value is not in that loop, the value has to be copied for
  /// each loop iteration.
  func makeAvailable(in destBlock: BasicBlock, _ context: some MutatingContext) -> Value {
    assert(uses.isEmpty)
    assert(ownership == .owned)

    let beginBlock = parentBlock
    var useToDefRange = BasicBlockRange(begin: beginBlock, context)
    defer { useToDefRange.deinitialize() }

    useToDefRange.insert(destBlock)

    // The value needs to be destroyed at every exit of the liferange.
    for exitBlock in useToDefRange.exits {
      let builder = Builder(before: exitBlock.instructions.first!, context)
      builder.createDestroyValue(operand: self)
    }
  
    if useToDefRange.contains(destBlock) {
      // The `destBlock` is within a loop, so we need to copy the value at each iteration.
      let builder = Builder(before: destBlock.instructions.first!, context)
      return builder.createCopyValue(operand: self)
    }
    return self
  }

  /// Copies this value at `insertionPoint` and makes the copy available to be used in `destBlock`.
  ///
  /// For details see `makeAvailable`.
  func copy(at insertionPoint: Instruction, andMakeAvailableIn destBlock: BasicBlock,
            _ context: some MutatingContext) -> Value {
    let builder = Builder(before: insertionPoint, context)
    let copiedValue = builder.createCopyValue(operand: self)
    return copiedValue.makeAvailable(in: destBlock, context)
  }
}

extension Instruction {
  var isTriviallyDead: Bool {
    if results.contains(where: { !$0.uses.isEmpty }) {
      return false
    }
    return self.canBeRemovedIfNotUsed
  }

  var isTriviallyDeadIgnoringDebugUses: Bool {
    if results.contains(where: { !$0.uses.isEmptyIgnoringDebugUses }) {
      return false
    }
    return self.canBeRemovedIfNotUsed
  }

  private var canBeRemovedIfNotUsed: Bool {
    // TODO: it is horrible to hard-code exceptions here, but currently there is no Instruction API for this.
    switch self {
    case is TermInst, is MarkUninitializedInst, is DebugValueInst:
      return false
    case let bi as BuiltinInst:
      if bi.id == .OnFastPath {
        return false
      }
    default:
      break
    }
    return !mayReadOrWriteMemory && !hasUnspecifiedSideEffects
  }
}

extension UseList {
  var singleNonDebugUse: Operand? {
    var singleUse: Operand?
    for use in self {
      if use.instruction is DebugValueInst {
        continue
      }
      if singleUse != nil {
        return nil
      }
      singleUse = use
    }
    return singleUse
  }

  var isEmptyIgnoringDebugUses: Bool {
    for use in self {
      if !(use.instruction is DebugValueInst) {
        return false
      }
    }
    return true
  }
}

extension FunctionPassContext {
  /// Returns true if any blocks were removed.
  func removeDeadBlocks(in function: Function) -> Bool {
    var reachableBlocks = ReachableBlocks(function: function, self)
    defer { reachableBlocks.deinitialize() }

    var blocksRemoved = false
    for block in function.blocks {
      if !reachableBlocks.isReachable(block: block) {
        block.dropAllReferences(self)
        erase(block: block)
        blocksRemoved = true
      }
    }
    return blocksRemoved
  }

  func removeTriviallyDeadInstructionsPreservingDebugInfo(in function: Function) {
    for inst in function.reversedInstructions {
      if inst.isTriviallyDead {
        erase(instruction: inst)
      }
    }
  }

  func removeTriviallyDeadInstructionsIgnoringDebugUses(in function: Function) {
    for inst in function.reversedInstructions {
      if inst.isTriviallyDeadIgnoringDebugUses {
        erase(instructionIncludingDebugUses: inst)
      }
    }
  }
}

extension BasicBlock {
  func dropAllReferences(_ context: FunctionPassContext) {
    for arg in arguments {
      arg.uses.replaceAll(with: Undef.get(type: arg.type, context), context)
    }
    for inst in instructions.reversed() {
      for result in inst.results {
        result.uses.replaceAll(with: Undef.get(type: result.type, context), context)
      }
      context.erase(instruction: inst)
    }
  }
}

extension SimplifyContext {

  /// Replaces a pair of redudant instructions, like
  /// ```
  ///   %first = enum $E, #E.CaseA!enumelt, %replacement
  ///   %second = unchecked_enum_data %first : $E, #E.CaseA!enumelt
  /// ```
  /// Replaces `%second` with `%replacement` and deletes the instructions if possible - or required.
  /// The operation is not done if it would require to insert a copy due to keep ownership correct.
  func tryReplaceRedundantInstructionPair(first: SingleValueInstruction, second: SingleValueInstruction,
                                          with replacement: Value) {
    let singleUse = preserveDebugInfo ? first.uses.singleUse : first.uses.singleNonDebugUse
    let canEraseFirst = singleUse?.instruction == second

    if !canEraseFirst && first.parentFunction.hasOwnership && replacement.ownership == .owned {
      // We cannot add more uses to `replacement` without inserting a copy.
      return
    }

    second.uses.replaceAll(with: replacement, self)
    erase(instruction: second)

    if canEraseFirst {
      erase(instructionIncludingDebugUses: first)
    }
  }
}

extension ProjectedValue {
  /// Returns true if the address can alias with `rhs`.
  ///
  /// Example:
  ///   %1 = struct_element_addr %s, #field1
  ///   %2 = struct_element_addr %s, #field2
  ///
  /// `%s`.canAddressAlias(with: `%1`) -> true
  /// `%s`.canAddressAlias(with: `%2`) -> true
  /// `%1`.canAddressAlias(with: `%2`) -> false
  ///
  func canAddressAlias(with rhs: ProjectedValue, complexityBudget: Int = Int.max, _ context: some Context) -> Bool {
    // self -> rhs will succeed (= return false) if self is a non-escaping "local" object,
    // but not necessarily rhs.
    if !isEscaping(using: EscapesToValueVisitor(target: rhs), complexityBudget: complexityBudget, context) {
      return false
    }
    // The other way round: rhs -> self will succeed if rhs is a non-escaping "local" object,
    // but not necessarily self.
    if !rhs.isEscaping(using: EscapesToValueVisitor(target: self), complexityBudget: complexityBudget, context) {
      return false
    }
    return true
  }
}

private struct EscapesToValueVisitor : EscapeVisitor {
  let target: ProjectedValue

  mutating func visitUse(operand: Operand, path: EscapePath) -> UseResult {
    if operand.value == target.value && path.projectionPath.mayOverlap(with: target.path) {
      return .abort
    }
    if operand.instruction is ReturnInst {
      // Anything which is returned cannot escape to an instruction inside the function.
      return .ignore
    }
    return .continueWalk
  }

  var followTrivialTypes: Bool { true }
  var followLoads: Bool { false }
}

extension Function {
  var globalOfGlobalInitFunction: GlobalVariable? {
    if isGlobalInitFunction,
       let ret = returnInstruction,
       let atp = ret.returnedValue as? AddressToPointerInst,
       let ga = atp.address as? GlobalAddrInst {
      return ga.global
    }
    return nil
  }
}

