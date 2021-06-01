//===--- Operand.swift - Instruction operands -----------------------------===//
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

/// An operand of an instruction.
public struct Operand : CustomStringConvertible, CustomReflectable {
  fileprivate let bridged: BridgedOperand

  init(_ bridged: BridgedOperand) {
    self.bridged = bridged
  }

  public var value: Value {
    let v = Operand_getValue(bridged).getAs(AnyObject.self)
    switch v {
      case let inst as SingleValueInstruction:
        return inst
      case let arg as BlockArgument:
        return arg
      case let mvr as MultipleValueInstructionResult:
        return mvr
      case let undef as Undef:
        return undef
      default:
        fatalError("unknown Value type")
    }
  }

  public var instruction: Instruction {
    return Operand_getUser(bridged).instruction
  }
  
  public var index: Int { instruction.operands.getIndex(of: self) }
  
  public var description: String { "operand #\(index) of \(instruction)" }
  
  public var customMirror: Mirror { Mirror(self, children: []) }
}

public struct OperandArray : RandomAccessCollection, CustomReflectable {
  private let opArray: BridgedOperandArray
  
  init(opArray: BridgedOperandArray) {
    self.opArray = opArray
  }
  
  public var startIndex: Int { return 0 }
  public var endIndex: Int { return Int(opArray.numOperands) }
  
  public subscript(_ index: Int) -> Operand {
    precondition(index >= 0 && index < endIndex)
    return Operand(BridgedOperand(op: opArray.data + index &* BridgedOperandSize))
  }
  
  public func getIndex(of operand: Operand) -> Int {
    let idx = (operand.bridged.op - UnsafeRawPointer(opArray.data)) /
                BridgedOperandSize
    precondition(self[idx].bridged.op == operand.bridged.op)
    return idx
  }
  
  public var customMirror: Mirror {
    let c: [Mirror.Child] = map { (label: nil, value: $0.value) }
    return Mirror(self, children: c)
  }
}

public struct UseList : Sequence, CustomReflectable {
  public struct Iterator : IteratorProtocol {
    var currentOpPtr: UnsafeRawPointer?
    
    public mutating func next() -> Operand? {
      if let opPtr = currentOpPtr {
        let bridged = BridgedOperand(op: opPtr)
        currentOpPtr = Operand_nextUse(bridged).op
        return Operand(bridged)
      }
      return nil
    }
  }

  private let firstOpPtr: UnsafeRawPointer?

  init(_ firstOpPtr: OptionalBridgedOperand) {
    self.firstOpPtr = firstOpPtr.op
  }

  public func makeIterator() -> Iterator {
    return Iterator(currentOpPtr: firstOpPtr)
  }

  public var customMirror: Mirror {
    let c: [Mirror.Child] = map { (label: "use", value: $0) }
    return Mirror(self, children: c)
  }
}
