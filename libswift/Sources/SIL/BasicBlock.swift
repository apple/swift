//===--- BasicBlock.swift - Defines the BasicBlock class ------------------===//
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

import SILBridging

final public class BasicBlock : ListNode, CustomStringConvertible {
  public var next: BasicBlock? { SILBasicBlock_next(bridged).block }
  public var previous: BasicBlock? { SILBasicBlock_previous(bridged).block }
  
  public var description: String {
    SILBasicBlock_debugDescription(bridged).takeString()
  }

  public var arguments: ArgumentArray { ArgumentArray(block: self) }

  public var instructions: List<Instruction> {
    List(startAt: SILBasicBlock_firstInst(bridged).instruction)
  }

  public var reverseInstructions: ReverseList<Instruction> {
    ReverseList(startAt: SILBasicBlock_lastInst(bridged).instruction)
  }

  var bridged: BridgedBasicBlock { BridgedBasicBlock(obj: SwiftObject(self)) }
}

public struct ArgumentArray : RandomAccessCollection {
  fileprivate let block: BasicBlock

  public var startIndex: Int { return 0 }
  public var endIndex: Int { SILBasicBlock_getNumArguments(block.bridged) }

  public subscript(_ index: Int) -> BlockArgument {
    SILBasicBlock_getArgument(block.bridged, index).argument
  }
}

// Bridging utilities

extension BridgedBasicBlock {
  var block: BasicBlock { obj.getAs(BasicBlock.self) }
}

extension OptionalBridgedBasicBlock {
  var block: BasicBlock? { obj.getAs(BasicBlock.self) }
}
