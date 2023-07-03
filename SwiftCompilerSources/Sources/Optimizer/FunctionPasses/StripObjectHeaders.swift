//===--- StripObjectHeaders.swift ------------------------------------------==//
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

/// Sets the `[bare]` attribute for `alloc_ref` and `global_value` instructions
/// if their header (reference count and metatype) is not used throughout the
/// lifetime of the object.
///
let stripObjectHeadersPass = FunctionPass(name: "strip-object-headers") {
  (function: Function, context: FunctionPassContext) in

  for inst in function.instructions {
    switch inst {
    case let gv as GlobalValueInst:
      if !gv.isBare && !gv.needObjectHeader() {
        gv.setIsBare(context)
      }
    case let ar as AllocRefInst:
      if !ar.isBare && !ar.needObjectHeader() {
        ar.setIsBare(context)
      }
    default:
      break
    }
  }
}

private extension Value {
  func needObjectHeader() -> Bool {
    var walker = IsBareObjectWalker()
    return walker.walkDownUses(ofValue: self, path: SmallProjectionPath()) == .abortWalk
  }
}

private struct IsBareObjectWalker : ValueDefUseWalker {
  var walkDownCache = WalkerCache<SmallProjectionPath>()

  mutating func walkDown(value operand: Operand, path: Path) -> WalkResult {
    switch operand.instruction {
    // White-list all instructions which don't use the object header.
    case is StructInst, is TupleInst, is EnumInst,
         is StructExtractInst, is TupleExtractInst, is UncheckedEnumDataInst,
         is DestructureStructInst, is DestructureTupleInst,
         is BeginBorrowInst, is MarkDependenceInst,
         is BranchInst, is CondBranchInst, is SwitchEnumInst,
         is UpcastInst, is UncheckedRefCastInst,
         is EndCOWMutationInst:
      return walkDownDefault(value: operand, path: path)
    default:
      return leafUse(value: operand, path: path)
    }
  }

  mutating func leafUse(value operand: Operand, path: SmallProjectionPath) -> WalkResult {
    switch operand.instruction {
    // White-list all instructions which don't use the object header.
    case is RefElementAddrInst, is RefTailAddrInst,
         is DeallocRefInst, is DeallocStackRefInst, is SetDeallocatingInst,
         is DebugValueInst, is FixLifetimeInst:
      return .continueWalk
    default:
      return .abortWalk
    }
  }
}
