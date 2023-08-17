//===--- AliasAnalysis.swift - the alias analysis -------------------------===//
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

import OptimizerBridging
import SIL

struct AliasAnalysis {
  let bridged: BridgedAliasAnalysis

  /// Returns the correct path for address-alias functions.
  static func getPtrOrAddressPath(for value: Value) -> SmallProjectionPath {
    let ty = value.type
    if ty.isAddress {
      // This is the regular case: the path selects any sub-fields of an address.
      return SmallProjectionPath(.anyValueFields)
    }
    // Some optimizations use the address-alias APIs with non-address SIL values.
    // TODO: this is non-intuitive and we should eliminate those API uses.
    if ty.isClass {
    // If the value is a (non-address) reference it means: all addresses within the class instance.
      return SmallProjectionPath(.anyValueFields).push(.anyClassField)
    }
    // Any other non-address value means: all addresses of any referenced class instances within the value.
    return SmallProjectionPath(.anyValueFields).push(.anyClassField).push(.anyValueFields)
  }
  
  static func register() {
    BridgedAliasAnalysis.registerAnalysis(
      // getMemEffectsFn
      { (bridgedCtxt: BridgedPassContext, bridgedVal: BridgedValue, bridgedInst: BridgedInstruction, complexityBudget: Int) -> swift.MemoryBehavior in
        let context = FunctionPassContext(_bridged: bridgedCtxt)
        let inst = bridgedInst.instruction
        let val = bridgedVal.value
        let path = AliasAnalysis.getPtrOrAddressPath(for: val)
        switch inst {
        case let apply as ApplySite:
          return getMemoryEffect(ofApply: apply, for: val, path: path, context).bridged
        case let builtin as BuiltinInst:
          return getMemoryEffect(ofBuiltin: builtin, for: val, path: path, context).bridged
        default:
          if val.at(path).isEscaping(using: EscapesToInstructionVisitor(target: inst, isAddress: true),
                                     complexityBudget: complexityBudget, context) {
            return .MayReadWrite
          }
          return .None
        }
      },

      // isObjReleasedFn
      { (bridgedCtxt: BridgedPassContext, bridgedObj: BridgedValue, bridgedInst: BridgedInstruction) -> Bool in
        let context = FunctionPassContext(_bridged: bridgedCtxt)
        let inst = bridgedInst.instruction
        let obj = bridgedObj.value
        let path = SmallProjectionPath(.anyValueFields)
        if let apply = inst as? ApplySite {
          let effect = getOwnershipEffect(of: apply, for: obj, path: path, context)
          return effect.destroy
        }
        return obj.at(path).isEscaping(using: EscapesToInstructionVisitor(target: inst, isAddress: false), context)
      },

      // isAddrVisibleFromObj
      { (bridgedCtxt: BridgedPassContext, bridgedAddr: BridgedValue, bridgedObj: BridgedValue, complexityBudget: Int) -> Bool in
        let context = FunctionPassContext(_bridged: bridgedCtxt)
        let addr = bridgedAddr.value.at(AliasAnalysis.getPtrOrAddressPath(for: bridgedAddr.value))

        // This is similar to `canReferenceSameFieldFn`, except that all addresses of all objects are
        // considered which are transitively visible from `bridgedObj`.
        let anythingReachableFromObj = bridgedObj.value.at(SmallProjectionPath(.anything))
        return addr.canAddressAlias(with: anythingReachableFromObj, complexityBudget: complexityBudget, context)
      },

      // canReferenceSameFieldFn
      { (bridgedCtxt: BridgedPassContext, bridgedLhs: BridgedValue, bridgedRhs: BridgedValue) -> Bool in
        let context = FunctionPassContext(_bridged: bridgedCtxt)

        // If `lhs` or `rhs` is not an address, but an object, it means: check for alias of any class
        // field address of the object.
        let lhs = bridgedLhs.value.at(AliasAnalysis.getPtrOrAddressPath(for: bridgedLhs.value))
        let rhs = bridgedRhs.value.at(AliasAnalysis.getPtrOrAddressPath(for: bridgedRhs.value))
        return lhs.canAddressAlias(with: rhs, context)
      }
    )
  }
}

extension Instruction {
  func mayRead(fromAddress: Value, _ aliasAnalysis: AliasAnalysis) -> Bool {
    switch aliasAnalysis.bridged.getMemBehavior(bridged, fromAddress.bridged) {
      case .MayRead, .MayReadWrite, .MayHaveSideEffects:
        return true
      default:
        return false
    }
  }

  func mayWrite(toAddress: Value, _ aliasAnalysis: AliasAnalysis) -> Bool {
    switch aliasAnalysis.bridged.getMemBehavior(bridged, toAddress.bridged) {
      case .MayWrite, .MayReadWrite, .MayHaveSideEffects:
        return true
      default:
        return false
    }
  }

  func mayReadOrWrite(address: Value, _ aliasAnalysis: AliasAnalysis) -> Bool {
    switch aliasAnalysis.bridged.getMemBehavior(bridged, address.bridged) {
      case .MayRead, .MayWrite, .MayReadWrite, .MayHaveSideEffects:
        return true
      default:
        return false
    }
  }
}

private func getMemoryEffect(ofApply apply: ApplySite, for address: Value, path: SmallProjectionPath, _ context: FunctionPassContext) -> SideEffects.Memory {
  let calleeAnalysis = context.calleeAnalysis
  let visitor = SideEffectsVisitor(apply: apply, calleeAnalysis: calleeAnalysis, isAddress: true)
  let memoryEffects: SideEffects.Memory

  // First try to figure out to which argument(s) the address "escapes" to.
  if let result = address.at(path).visit(using: visitor, context) {
    // The resulting effects are the argument effects to which `address` escapes to.
    memoryEffects = result.memory
  } else {
    // `address` has unknown escapes. So we have to take the global effects of the called function(s).
    memoryEffects = calleeAnalysis.getSideEffects(ofApply: apply).memory
  }
  // Do some magic for `let` variables. Function calls cannot modify let variables.
  // The only exception is that the let variable is directly passed to an indirect out of the
  // apply.
  // TODO: make this a more formal and verified approach.
  if memoryEffects.write && address.accessBase.isLet && !address.isIndirectResult(of: apply) {
    return SideEffects.Memory(read: memoryEffects.read, write: false)
  }
  return memoryEffects
}

private func getMemoryEffect(ofBuiltin builtin: BuiltinInst, for address: Value, path: SmallProjectionPath, _ context: FunctionPassContext) -> SideEffects.Memory {

  switch builtin.id {
  case .Once, .OnceWithContext:
    if !address.at(path).isEscaping(using: AddressVisibleByBuiltinOnceVisitor(), context) {
      return SideEffects.Memory()
    }
    let callee = builtin.operands[1].value
    return context.calleeAnalysis.getSideEffects(ofCallee: callee).memory
  default:
    return builtin.memoryEffects
  }
}

private func getOwnershipEffect(of apply: ApplySite, for value: Value, path: SmallProjectionPath, _ context: FunctionPassContext) -> SideEffects.Ownership {
  let visitor = SideEffectsVisitor(apply: apply, calleeAnalysis: context.calleeAnalysis, isAddress: false)
  if let result = value.at(path).visit(using: visitor, context) {
    // The resulting effects are the argument effects to which `value` escapes to.
    return result.ownership
  } else {
    // `value` has unknown escapes. So we have to take the global effects of the called function(s).
    return visitor.calleeAnalysis.getSideEffects(ofApply: apply).ownership
  }
}

private struct SideEffectsVisitor : EscapeVisitorWithResult {
  let apply: ApplySite
  let calleeAnalysis: CalleeAnalysis
  let isAddress: Bool
  var result = SideEffects.GlobalEffects()

  mutating func visitUse(operand: Operand, path: EscapePath) -> UseResult {
    let user = operand.instruction
    if user is ReturnInst {
      // Anything which is returned cannot escape to an instruction inside the function.
      return .ignore
    }
    if user == apply {
      if let argIdx = apply.argumentIndex(of: operand) {
        let e = calleeAnalysis.getSideEffects(of: apply, forArgument: argIdx, path: path.projectionPath)
        result.merge(with: e)
      }
    }
    return .continueWalk
  }

  var followTrivialTypes: Bool { isAddress }
  var followLoads: Bool { !isAddress }
}

private struct AddressVisibleByBuiltinOnceVisitor : EscapeVisitor {
  var followTrivialTypes: Bool { true }
  var followLoads: Bool { false }
}

/// Lets `ProjectedValue.isEscaping` return true if the value is "escaping" to the `target` instruction.
private struct EscapesToInstructionVisitor : EscapeVisitor {
  let target: Instruction
  let isAddress: Bool

  mutating func visitUse(operand: Operand, path: EscapePath) -> UseResult {
    let user = operand.instruction
    if user == target {
      return .abort
    }
    if user is ReturnInst {
      // Anything which is returned cannot escape to an instruction inside the function.
      return .ignore
    }
    return .continueWalk
  }

  var followTrivialTypes: Bool { isAddress }
  var followLoads: Bool { !isAddress }
}

private extension Value {
  /// Returns true if this address is passed as indirect out of `apply`.
  func isIndirectResult(of apply: ApplySite) -> Bool {
    guard let fullApply = apply as? FullApplySite else {
      return false
    }
    if fullApply.numIndirectResultArguments == 0 {
      return false
    }

    var walker = IsIndirectResultWalker(apply: fullApply)
    return walker.walkDownUses(ofAddress: self, path: UnusedWalkingPath()) == .abortWalk
  }
}

private struct IsIndirectResultWalker: AddressDefUseWalker {
  let apply: FullApplySite

  mutating func leafUse(address: Operand, path: UnusedWalkingPath) -> WalkResult {
    if address.instruction == apply,
       let argIdx = apply.argumentIndex(of: address),
       argIdx < apply.numIndirectResultArguments {
      return .abortWalk
    }
    return .continueWalk
  }
}

private extension SideEffects.Memory {
  var bridged: swift.MemoryBehavior {
    switch (read, write) {
      case (false, false): return .None
      case (true, false):  return .MayRead
      case (false, true):  return .MayWrite
      case (true, true):   return .MayReadWrite
    }
  }
}
