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
public struct Operand : CustomStringConvertible, NoReflectionChildren {
  fileprivate let bridged: BridgedOperand

  init(bridged: BridgedOperand) {
    self.bridged = bridged
  }

  public var value: Value { bridged.getValue().value }

  public static func ==(lhs: Operand, rhs: Operand) -> Bool {
    return lhs.bridged.op == rhs.bridged.op
  }

  public var instruction: Instruction {
    return bridged.getUser().instruction
  }
  
  public var index: Int { instruction.operands.getIndex(of: self) }
  
  /// True if the operand is used to describe a type dependency, but it's not
  /// used as value.
  public var isTypeDependent: Bool { bridged.isTypeDependent() }
  
  public var description: String { "operand #\(index) of \(instruction)" }
}

public struct OperandArray : RandomAccessCollection, CustomReflectable {
  private let base: OptionalBridgedOperand
  public let count: Int
  
  init(base: OptionalBridgedOperand, count: Int) {
    self.base = base
    self.count = count
  }

  public var startIndex: Int { return 0 }
  public var endIndex: Int { return count }
  
  public subscript(_ index: Int) -> Operand {
    assert(index >= startIndex && index < endIndex)
    return Operand(bridged: base.advancedBy(index))
  }
  
  public func getIndex(of operand: Operand) -> Int {
    let idx = base.distanceTo(operand.bridged)
    assert(self[idx].bridged.op == operand.bridged.op)
    return idx
  }
  
  public var customMirror: Mirror {
    let c: [Mirror.Child] = map { (label: nil, value: $0.value) }
    return Mirror(self, children: c)
  }
  
  /// Returns a sub-array defined by `bounds`.
  ///
  /// Note: this does not return a Slice. The first index of the returnd array is always 0.
  public subscript(bounds: Range<Int>) -> OperandArray {
    assert(bounds.lowerBound >= startIndex && bounds.upperBound <= endIndex)
    return OperandArray(
      base: OptionalBridgedOperand(op: base.advancedBy(bounds.lowerBound).op),
      count: bounds.upperBound - bounds.lowerBound)
  }

  public var values: LazyMapSequence<LazySequence<OperandArray>.Elements, Value> {
    self.lazy.map { $0.value }
  }
}

public struct UseList : CollectionLikeSequence {
  public struct Iterator : IteratorProtocol {
    var currentOpPtr: OptionalBridgedOperand
    
    public mutating func next() -> Operand? {
      if let op = currentOpPtr.operand {
        currentOpPtr = op.getNextUse()
        return Operand(bridged: op)
      }
      return nil
    }
  }

  private let firstOpPtr: OptionalBridgedOperand

  init(_ firstOpPtr: OptionalBridgedOperand) {
    self.firstOpPtr = firstOpPtr
  }

  public var singleUse: Operand? {
    if let op = firstOpPtr.operand {
      if op.getNextUse().operand != nil {
        return nil
      }
      return Operand(bridged: op)
    }
    return nil
  }

  public func getSingleUser<I: Instruction>(ofType: I.Type) -> I? {
    var result: I? = nil
    for use in self {
      if let user = use.instruction as? I {
        if result != nil {
          return nil
        }
        result = user
      }
    }
    return result
  }

  public var isSingleUse: Bool { singleUse != nil }

  public func makeIterator() -> Iterator {
    return Iterator(currentOpPtr: firstOpPtr)
  }
}

extension OptionalBridgedOperand {
  var operand: BridgedOperand? {
    if let op = op {
      return BridgedOperand(op: op)
    }
    return nil
  }
}
