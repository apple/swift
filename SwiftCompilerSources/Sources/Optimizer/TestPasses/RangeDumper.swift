//===--- RangeDumper.swift - Dumps escape information ----------------===//
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

let rangeDumper = FunctionPass(name: "dump-ranges", {
  (function: Function, context: FunctionPassContext) in

  var begin: Instruction?
  var ends = Stack<Instruction>(context)
  defer { ends.deinitialize() }
  var interiors = Stack<Instruction>(context)
  defer { interiors.deinitialize() }
  var ins = Stack<Instruction>(context)
  defer { ins.deinitialize() }
  var outs = Stack<Instruction>(context)
  defer { outs.deinitialize() }

  for inst in function.instructions {
    if let sli = inst as? StringLiteralInst {
      switch sli.value {
        case "begin":
          assert(begin == nil, "more than one begin instruction")
          begin = sli
        case "end":
          ends.append(sli)
        case "interior":
          interiors.append(sli)
        case "inside":
          ins.append(sli)
        case "outside":
          outs.append(sli)
        default:
          break
      }
    }
  }
  
  guard let begin = begin else { return }
  
  var instRange = InstructionRange(begin: begin, context)
  defer { instRange.deinitialize() }

  instRange.insert(contentsOf: ends)
  instRange.insert(contentsOf: interiors)

  print("Instruction range in \(function.name):")
  print(instRange)
  print("Block range in \(function.name):")
  print(instRange.blockRange)
  print("End function \(function.name)\n")

  verify(instRange.blockRange, context)
  
  for i in ins {
    assert(instRange.contains(i))
    assert(instRange.inclusiveRangeContains(i))
  }
  for e in ends {
    assert(!instRange.contains(e))
    assert(instRange.inclusiveRangeContains(e))
  }
  for o in outs {
    assert(!instRange.contains(o))
    assert(!instRange.inclusiveRangeContains(o))
  }
})

private func verify(_ blockRange: BasicBlockRange, _ context: FunctionPassContext) {
  var inRange = BasicBlockSet(context)
  defer { inRange.deinitialize() }
  for b in blockRange.range {
    inRange.insert(b)
  }

  var inInclusiveRange = BasicBlockSet(context)
  defer { inInclusiveRange.deinitialize() }
  for b in blockRange.inclusiveRange {
    inInclusiveRange.insert(b)
  }

  for b in blockRange.begin.parentFunction.blocks {
    assert(blockRange.contains(b) == inRange.contains(b))
    assert(blockRange.inclusiveRangeContains(b) == inInclusiveRange.contains(b))
  }
}

