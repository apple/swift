//===--- MemBehaviorDumper.swift ------------------------------------------===//
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

/// Prints the memory behavior of relevant instructions in relation to address values of the function.
let memBehaviorDumper = FunctionPass(name: "dump-mem-behavior") {
  (function: Function, context: FunctionPassContext) in

  let aliasAnalysis = context.aliasAnalysis

  print("@\(function.name)")

  let values = function.allValues

  var currentPair = 0
  for inst in function.instructions where inst.shouldTest {

    for value in values where value.definingInstruction != inst {

      if value.type.isAddress || value is AddressToPointerInst {
        let read = inst.mayRead(fromAddress: value, aliasAnalysis)
        let write = inst.mayWrite(toAddress: value, aliasAnalysis)
        print("PAIR #\(currentPair).")
        print("  \(inst)")
        print("  \(value)")
        print("  r=\(read ? 1 : 0),w=\(write ? 1 : 0)")
        currentPair += 1
      }
    }
  }
  print()
}

private extension Function {
  var allValues: [Value] {
    var values: [Value] = []
    for block in blocks {
      values.append(contentsOf: block.arguments.map { $0 })
      for inst in block.instructions {
        values.append(contentsOf: inst.results)
      }
    }
    return values
  }
}

private extension Instruction {
  var shouldTest: Bool {
    switch self {
    case is ApplyInst,
         is TryApplyInst,
         is EndApplyInst,
         is BeginApplyInst,
         is AbortApplyInst,
         is BeginAccessInst,
         is EndAccessInst,
         is EndCOWMutationInst,
         is CopyValueInst,
         is DestroyValueInst,
         is EndBorrowInst,
         is LoadInst,
         is StoreInst,
         is CopyAddrInst,
         is BuiltinInst,
         is DebugValueInst:
      return true
    default:
      return false
    }
  }
}
