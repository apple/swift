//===--- SILPrinter.swift - Example swift function pass -------------------===//
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

let silPrinterPass = FunctionPass(name: "sil-printer", runSILPrinter)

func runSILPrinter(function: Function, context: FunctionPassContext) {
  print("run SILPrinter on function: \(function.name)")

  for (bbIdx, block) in function.blocks.enumerated() {
    print("bb\(bbIdx):")

    print("  arguments:")
    for arg in block.arguments {
      print("    arg: \(arg)")
      for use in arg.uses {
        print("      user: \(use.instruction)")
      }
    }

    print("  instructions:")
    for inst in block.instructions {
      print("  \(inst)")
      for op in inst.operands {
        print("      op: \(op.value)")
      }
      for (resultIdx, result) in inst.results.enumerated() {
        for use in result.uses {
          print("      user of result \(resultIdx): \(use.instruction)")
        }
      }
    }
    print()
  }
}
