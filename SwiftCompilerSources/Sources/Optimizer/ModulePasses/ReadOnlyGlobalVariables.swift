//===--- ReadOnlyGlobalVariables.swift ------------------------------------===//
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

/// Marks global `var` variables as `let` if they are never written.
///
/// Note that this pass relies on the initialize-static-globals pass which converts lazily
/// initialized globals to statically initialized globals.
/// This pass does not mark lazily initialized globals as `let`, because such globals _are_
/// written: in their initializers.
///
let readOnlyGlobalVariablesPass = ModulePass(name: "read-only-global-variables") {
  (moduleContext: ModulePassContext) in

  var writtenGlobals = Set<GlobalVariable>()

  for f in moduleContext.functions {
    for inst in f.instructions {
      if let gAddr = inst as? GlobalAddrInst {
        if findWrites(toAddress: gAddr) {
          writtenGlobals.insert(gAddr.global)
        }
      }
    }
  }

  for g in moduleContext.globalVariables {
    if !g.isAvailableExternally,
       !g.isPossiblyUsedExternally,
       !g.isLet,
       !writtenGlobals.contains(g) {
      g.setIsLet(to: true, moduleContext)
    }
  }
}

private func findWrites(toAddress: Value) -> Bool {
  var walker = FindWrites()
  return walker.walkDownUses(ofAddress: toAddress, path: UnusedWalkingPath()) == .abortWalk
}

private struct FindWrites : AddressDefUseWalker {
  mutating func leafUse(address: Operand, path: UnusedWalkingPath) -> WalkResult {
    switch address.instruction {
    case is LoadInst, is LoadBorrowInst:
      return .continueWalk

    case let ca as CopyAddrInst:
      return address == ca.sourceOperand ? .continueWalk : .abortWalk

    case let apply as FullApplySite:
      if let callerArgIdx = apply.argumentIndex(of: address) {
        let calleeArgIdx = apply.calleeArgIndex(callerArgIndex: callerArgIdx)
        let convention = apply.getArgumentConvention(calleeArgIndex: calleeArgIdx)
        if convention.isIndirectIn {
          return .continueWalk
        }
      }
      return .abortWalk
    default:
      return .abortWalk
    }
  }
}
