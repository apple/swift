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

import Basic
import SILBridging

@_semantics("arc.immortal")
final public class BasicBlock : CustomStringConvertible, HasShortDescription {
  public var next: BasicBlock? { bridged.getNext().block }
  public var previous: BasicBlock? { bridged.getPrevious().block }

  public var parentFunction: Function { bridged.getFunction().function }

  public var description: String {
    let stdString = bridged.getDebugDescription()
    return String(_cxxString: stdString)
  }
  public var shortDescription: String { name }

  public var arguments: ArgumentArray { ArgumentArray(block: self) }

  public var instructions: InstructionList {
    InstructionList(first: bridged.getFirstInst().instruction)
  }

  public var terminator: TermInst {
    bridged.getLastInst().instruction as! TermInst
  }

  public var successors: SuccessorArray { terminator.successors }

  public var predecessors: PredecessorList {
    PredecessorList(startAt: bridged.getFirstPred())
  }

  public var singlePredecessor: BasicBlock? {
    var preds = predecessors
    if let p = preds.next() {
      if preds.next() == nil {
        return p
      }
    }
    return nil
  }
  
  public var hasSinglePredecessor: Bool { singlePredecessor != nil }

  /// The index of the basic block in its function.
  /// This has O(n) complexity. Only use it for debugging
  public var index: Int {
    for (idx, block) in parentFunction.blocks.enumerated() {
      if block == self { return idx }
    }
    fatalError()
  }
 
  public var name: String { "bb\(index)" }

  public var bridged: BridgedBasicBlock { BridgedBasicBlock(SwiftObject(self)) }
}

public func == (lhs: BasicBlock, rhs: BasicBlock) -> Bool { lhs === rhs }
public func != (lhs: BasicBlock, rhs: BasicBlock) -> Bool { lhs !== rhs }

/// The list of instructions in a BasicBlock.
///
/// It's allowed to delete the current, next or any other instructions while
/// iterating over the instruction list.
public struct InstructionList : CollectionLikeSequence, IteratorProtocol {
  private var currentInstruction: Instruction?

  public init(first: Instruction?) { currentInstruction = first }

  public mutating func next() -> Instruction? {
    if var inst = currentInstruction {
      while inst.isDeleted {
        guard let nextInst = inst.next else {
          return nil
        }
        inst = nextInst
      }
      currentInstruction = inst.next
      return inst
    }
    return nil
  }

  public var first: Instruction? { currentInstruction }

  public func reversed() -> ReverseInstructionList {
    if let inst = currentInstruction {
      let lastInst = inst.parentBlock.bridged.getLastInst().instruction
      return ReverseInstructionList(first: lastInst)
    }
    return ReverseInstructionList(first: nil)
  }
}

/// The list of instructions in a BasicBlock in reverse order.
///
/// It's allowed to delete the current, next or any other instructions while
/// iterating over the instruction list.
public struct ReverseInstructionList : CollectionLikeSequence, IteratorProtocol {
  private var currentInstruction: Instruction?

  public init(first: Instruction?) { currentInstruction = first }

  public mutating func next() -> Instruction? {
    if var inst = currentInstruction {
      while inst.isDeleted {
        guard let nextInst = inst.previous else {
          return nil
        }
        inst = nextInst
      }
      currentInstruction = inst.previous
      return inst
    }
    return nil
  }

  public var first: Instruction? { currentInstruction }
}

public struct ArgumentArray : RandomAccessCollection {
  fileprivate let block: BasicBlock

  public var startIndex: Int { return 0 }
  public var endIndex: Int { block.bridged.getNumArguments() }

  public subscript(_ index: Int) -> Argument {
    block.bridged.getArgument(index).argument
  }
}

public struct SuccessorArray : RandomAccessCollection, FormattedLikeArray {
  private let base: OptionalBridgedSuccessor
  public let count: Int

  init(base: OptionalBridgedSuccessor, count: Int) {
    self.base = base
    self.count = count
  }

  public var startIndex: Int { return 0 }
  public var endIndex: Int { return count }

  public subscript(_ index: Int) -> BasicBlock {
    assert(index >= startIndex && index < endIndex)
    return base.advancedBy(index).getTargetBlock().block
  }
}

public struct PredecessorList : CollectionLikeSequence, IteratorProtocol {
  private var currentSucc: OptionalBridgedSuccessor

  public init(startAt: OptionalBridgedSuccessor) { currentSucc = startAt }

  public mutating func next() -> BasicBlock? {
    if let succ = currentSucc.successor {
      currentSucc = succ.getNext()
      return succ.getContainingInst().instruction.parentBlock
    }
    return nil
  }
}


// Bridging utilities

extension BridgedBasicBlock {
  public var block: BasicBlock { obj.getAs(BasicBlock.self) }
  public var optional: OptionalBridgedBasicBlock {
    OptionalBridgedBasicBlock(obj: self.obj)
  }
}

extension OptionalBridgedBasicBlock {
  public var block: BasicBlock? { obj.getAs(BasicBlock.self) }
  public static var none: OptionalBridgedBasicBlock {
    OptionalBridgedBasicBlock(obj: nil)
  }
}

extension Optional where Wrapped == BasicBlock {
  public var bridged: OptionalBridgedBasicBlock {
    OptionalBridgedBasicBlock(obj: self?.bridged.obj)
  }
}

extension OptionalBridgedSuccessor {
  var successor: BridgedSuccessor? {
    if let succ = succ {
      return BridgedSuccessor(succ: succ)
    }
    return nil
  }
}
