//===--- BorrowedFromUpdater.swift ----------------------------------------===//
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
import OptimizerBridging

/// Update all borrowed-from instructions in the `function`
func updateBorrowedFrom(in function: Function, _ context: some MutatingContext) {
  if !function.hasOwnership {
    return
  }
  var guaranteedPhis = Stack<Phi>(context)
  defer { guaranteedPhis.deinitialize() }

  for block in function.blocks {
    for arg in block.arguments {
      if let phi = Phi(arg), phi.value.ownership == .guaranteed {
        guaranteedPhis.append(phi)
      }
    }
  }
  updateBorrowedFrom(for: guaranteedPhis, context)
}

/// Update borrowed-from instructions for a set of phi arguments.
func updateBorrowedFrom(for phis: some Sequence<Phi>, _ context: some MutatingContext) {
  for phi in phis {
    if !phi.value.parentFunction.hasOwnership {
      return
    }
    if phi.value.ownership == .guaranteed {
      createBorrowedFrom(for: phi, context)
    }
  }

  var changed: Bool
  repeat {
    changed = false

    for phi in phis {
      if phi.value.ownership == .guaranteed {
        changed = updateBorrowedFrom(for: phi, context) || changed
      }
    }
  } while changed
}

private func updateBorrowedFrom(for phi: Phi, _ context: some MutatingContext) -> Bool {
  var computedEVs = Stack<Value>(context)
  defer { computedEVs.deinitialize() }
  gatherEnclosingValuesFromPredecessors(for: phi, in: &computedEVs, context)

  var changed = false
  for use in phi.value.uses {
    if let bfi = use.forwardingBorrowedFromUser {
      changed = addEnclosingValues(computedEVs, to: bfi, context) || changed
    }
  }
  return changed
}

private func createBorrowedFrom(for phi: Phi, _ context: some MutatingContext) {
  if !phi.value.uses.contains(where: { $0.forwardingBorrowedFromUser != nil }) {
    let builder = Builder(atBeginOf: phi.value.parentBlock, context)
    let bfi = builder.createBorrowedFrom(borrowedValue: phi.value, enclosingValues: [])
    phi.value.uses.ignore(user: bfi).replaceAll(with: bfi, context)
  }
}

private func addEnclosingValues(
  _ newEVs: some Sequence<Value>,
  to borrowedFrom: BorrowedFromInst,
  _ context: some MutatingContext) -> Bool
{
  var existingEVs = ValueSet(insertContentsOf: borrowedFrom.enclosingValues, context)
  defer { existingEVs.deinitialize() }

  if newEVs.allSatisfy({ existingEVs.contains($0) }) {
    return false
  }

  var evs = Array<Value>(borrowedFrom.enclosingValues)
  evs.append(contentsOf: newEVs.lazy.filter { !existingEVs.contains($0) })

  let builder = Builder(before: borrowedFrom, context)
  let newBfi = builder.createBorrowedFrom(borrowedValue: borrowedFrom.borrowedValue, enclosingValues: evs)
  borrowedFrom.uses.replaceAll(with: newBfi, context)
  context.erase(instruction: borrowedFrom)
  return true
}

func registerBorrowedFromUpdater() {
  BridgedUtilities.registerBorrowedFromUpdater(
    { (bridgedCtxt: BridgedPassContext, bridgedFunction: BridgedFunction) in
      let context = FunctionPassContext(_bridged: bridgedCtxt)
      let function = bridgedFunction.function;
      updateBorrowedFrom(in: function, context)
    },
    { (bridgedCtxt: BridgedPassContext, bridgedPhiArray: BridgedArrayRef) in
      let context = FunctionPassContext(_bridged: bridgedCtxt)
      var guaranteedPhis = Stack<Phi>(context)
      defer { guaranteedPhis.deinitialize() }
      bridgedPhiArray.withElements(ofType: BridgedValue.self) {
        for bridgedVal in $0 {
          let phi = Phi(bridgedVal.value)!
          if phi.value.ownership == .guaranteed {
            guaranteedPhis.append(phi)
          }
        }
      }
      updateBorrowedFrom(for: guaranteedPhis, context)
    }
  )
}

/// This pass is only used for testing.
/// In the regular pipeline it's not needed because optimization passes must make sure that borrowed-from
/// instructions are updated once the pass finishes.
let updateBorrowedFromPass = FunctionPass(name: "update-borrowed-from") {
  (function: Function, context: FunctionPassContext) in

  updateBorrowedFrom(in: function, context)
}
