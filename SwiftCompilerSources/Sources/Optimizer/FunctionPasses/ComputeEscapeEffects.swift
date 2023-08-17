//===--- ComputeEscapeEffects.swift ----------------------------------------==//
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

/// Computes escape effects for function arguments.
///
/// For example, if an argument does not escape, adds a non-escaping effect,
/// ```
///   sil @foo : $@convention(thin) (@guaranteed X) -> () {
///   [%0: noecape **]
///   bb0(%0 : $X):
///     %1 = tuple ()
///     return %1 : $()
///   }
/// ```
/// The pass does not try to change or re-compute _defined_ effects.
///
let computeEscapeEffects = FunctionPass(name: "compute-escape-effects") {
  (function: Function, context: FunctionPassContext) in

  var newEffects = function.effects.escapeEffects.arguments.filter {!$0.isDerived }

  let returnInst = function.returnInstruction
  let argsWithDefinedEffects = getArgIndicesWithDefinedEscapingEffects(of: function)

  for arg in function.arguments {
    // We are not interested in arguments with trivial types.
    if arg.hasTrivialNonPointerType { continue }
    
    // Also, we don't want to override defined effects.
    if argsWithDefinedEffects.contains(arg.index) { continue }

    struct IgnoreRecursiveCallVisitor : EscapeVisitor {
      func visitUse(operand: Operand, path: EscapePath) -> UseResult {
        return isOperandOfRecursiveCall(operand) ? .ignore : .continueWalk
      }
    }

    // First check: is the argument (or a projected value of it) escaping at all?
    if !arg.at(.anything).isEscapingWhenWalkingDown(using: IgnoreRecursiveCallVisitor(),
                                                    context) {
      let effect = EscapeEffects.ArgumentEffect(.notEscaping, argumentIndex: arg.index,
                                                pathPattern: SmallProjectionPath(.anything))
      newEffects.append(effect)
      continue
    }
  
    // Now compute effects for two important cases:
    //   * the argument itself + any value projections, and...
    if addArgEffects(arg, argPath: SmallProjectionPath(), to: &newEffects, returnInst, context) {
      //   * single class indirections
      _ = addArgEffects(arg, argPath: SmallProjectionPath(.anyValueFields).push(.anyClassField),
                        to: &newEffects, returnInst, context)
    }
  }

  // Don't modify the effects if they didn't change. This avoids sending a change notification
  // which can trigger unnecessary other invalidations.
  if newEffects == function.effects.escapeEffects.arguments {
    return
  }

  context.modifyEffects(in: function) { (effects: inout FunctionEffects) in
    effects.escapeEffects.arguments = newEffects
  }
}

/// Returns true if an argument effect was added.
private
func addArgEffects(_ arg: FunctionArgument, argPath ap: SmallProjectionPath,
                   to newEffects: inout [EscapeEffects.ArgumentEffect],
                   _ returnInst: ReturnInst?, _ context: FunctionPassContext) -> Bool {
  // Correct the path if the argument is not a class reference itself, but a value type
  // containing one or more references.
  let argPath = arg.type.isClass ? ap : ap.push(.anyValueFields)
  
  guard let result = arg.at(argPath).visitByWalkingDown(using: ArgEffectsVisitor(), context) else {
    return false
  }
  
  // If the function never returns, the argument can not escape to another arg/return.
  guard let returnInst = arg.parentFunction.returnInstruction else {
    return false
  }

  let effect: EscapeEffects.ArgumentEffect
  switch result {
  case .notSet:
    effect = EscapeEffects.ArgumentEffect(.notEscaping, argumentIndex: arg.index, pathPattern: argPath)

  case .toReturn(let toPath):
    let visitor = IsExclusiveReturnEscapeVisitor(argument: arg, argumentPath: argPath, returnPath: toPath)
    let exclusive = visitor.isExclusiveEscape(returnInst: returnInst, context)
    effect = EscapeEffects.ArgumentEffect(.escapingToReturn(toPath: toPath, isExclusive: exclusive),
                                          argumentIndex: arg.index, pathPattern: argPath)

  case .toArgument(let toArgIdx, let toPath):
    // Exclusive argument -> argument effects cannot appear because such an effect would
    // involve a store which is not permitted for exclusive escapes.
    effect = EscapeEffects.ArgumentEffect(.escapingToArgument(toArgumentIndex: toArgIdx, toPath: toPath),
                                          argumentIndex: arg.index, pathPattern: argPath)
  }
  newEffects.append(effect)
  return true
}

/// Returns a set of argument indices for which there are "defined" effects (as opposed to derived effects).
private func getArgIndicesWithDefinedEscapingEffects(of function: Function) -> Set<Int> {
  var argsWithDefinedEffects = Set<Int>()

  for effect in function.effects.escapeEffects.arguments {
    if effect.isDerived { continue }

    argsWithDefinedEffects.insert(effect.argumentIndex)
    switch effect.kind {
    case .notEscaping, .escapingToReturn:
      break
    case .escapingToArgument(let toArgIdx, _):
      argsWithDefinedEffects.insert(toArgIdx)
    }
  }
  return argsWithDefinedEffects
}

/// Returns true if `op` is passed to a recursive call to the current function -
/// at the same argument index.
private func isOperandOfRecursiveCall(_ op: Operand) -> Bool {
  let inst = op.instruction
  if let applySite = inst as? FullApplySite,
     let callee = applySite.referencedFunction,
     callee == inst.parentFunction,
     let argIdx = applySite.argumentIndex(of: op),
     op.value == callee.arguments[argIdx] {
    return true
  }
  return false
}

private struct ArgEffectsVisitor : EscapeVisitorWithResult {
  enum EscapeDestination {
    case notSet
    case toReturn(SmallProjectionPath)
    case toArgument(Int, SmallProjectionPath) // argument index, path
  }
  var result = EscapeDestination.notSet

  mutating func visitUse(operand: Operand, path: EscapePath) -> UseResult {
    if operand.instruction is ReturnInst {
      // The argument escapes to the function return
      if path.followStores {
        // The escaping path must not introduce a followStores.
        return .abort
      }
      switch result {
        case .notSet:
          result = .toReturn(path.projectionPath)
        case .toReturn(let oldPath):
          result = .toReturn(oldPath.merge(with: path.projectionPath))
        case .toArgument:
          return .abort
      }
      return .ignore
    }
    if isOperandOfRecursiveCall(operand) {
      return .ignore
    }
    return .continueWalk
  }

  mutating func visitDef(def: Value, path: EscapePath) -> DefResult {
    guard let destArg = def as? FunctionArgument else {
      return .continueWalkUp
    }
    // The argument escapes to another argument (e.g. an out or inout argument)
    if path.followStores {
      // The escaping path must not introduce a followStores.
      return .abort
    }
    let argIdx = destArg.index
    switch result {
      case .notSet:
        result = .toArgument(argIdx, path.projectionPath)
      case .toArgument(let oldArgIdx, let oldPath) where oldArgIdx == argIdx:
        result = .toArgument(argIdx, oldPath.merge(with: path.projectionPath))
      default:
        return .abort
    }
    return .walkDown
  }
}

/// Returns true if when walking up from the return instruction, the `fromArgument`
/// is the one and only argument which is reached - with a matching `fromPath`.
private struct IsExclusiveReturnEscapeVisitor : EscapeVisitorWithResult {
  let argument: Argument
  let argumentPath: SmallProjectionPath
  let returnPath: SmallProjectionPath
  var result = false

  func isExclusiveEscape(returnInst: ReturnInst, _ context: FunctionPassContext) -> Bool {
    return returnInst.returnedValue.at(returnPath).visit(using: self, context) ?? false
  }

  mutating func visitUse(operand: Operand, path: EscapePath) -> UseResult {
    switch operand.instruction {
    case is ReturnInst:
      if path.followStores { return .abort }
      if path.projectionPath.matches(pattern: returnPath) {
        return .ignore
      }
      return .abort
    case let si as StoringInstruction:
      // Don't allow store instructions because this allows the EscapeUtils to walk up
      // an apply result with `followStores`.
      if operand == si.destinationOperand {
        return .abort
      }
    case let ca as CopyAddrInst:
      // `copy_addr` is like a store.
      if operand == ca.destinationOperand {
        return .abort
      }
    default:
      break
    }
    return .continueWalk
  }

  mutating func visitDef(def: Value, path: EscapePath) -> DefResult {
    guard let arg = def as? FunctionArgument else {
      return .continueWalkUp
    }
    if path.followStores { return .abort }
    if arg == argument && path.projectionPath.matches(pattern: argumentPath) {
      result = true
      return .walkDown
    }
    return .abort
  }
}
