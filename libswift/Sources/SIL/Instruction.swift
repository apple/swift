//===--- Instruction.swift - Defines the Instruction classes --------------===//
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

//===----------------------------------------------------------------------===//
//                       Instruction base classes
//===----------------------------------------------------------------------===//

public class Instruction : ListNode, CustomStringConvertible {
  final public var next: Instruction? {
    SILInstruction_next(bridged).instruction
  }

  final public var previous: Instruction? {
    SILInstruction_previous(bridged).instruction
  }

  final public var block: BasicBlock {
    SILInstruction_getParent(bridged).block
  }

  final public var description: String {
    SILNode_debugDescription(bridgedNode).takeString()
  }
  
  final public var operands: OperandArray {
    return OperandArray(opArray: SILInstruction_getOperands(bridged))
  }
  
  fileprivate var resultCount: Int { 0 }
  fileprivate func getResult(index: Int) -> Value { fatalError() }

  final public var results: InstructionResults { InstructionResults(self) }

  final public var location: Location {
    return Location(bridgedLocation: SILInstruction_getLocation(bridged))
  }

  final public var mayHaveSideEffects: Bool {
    return SILInstruction_mayHaveSideEffects(bridged) != 0
  }
  final public var mayReadFromMemory: Bool {
    return SILInstruction_mayReadFromMemory(bridged) != 0
  }
  final public var mayWriteToMemory: Bool {
    return SILInstruction_mayWriteToMemory(bridged) != 0
  }
  final public var mayReadOrWriteMemory: Bool {
    return SILInstruction_mayReadOrWriteMemory(bridged) != 0
  }

  public var bridged: BridgedInstruction {
    BridgedInstruction(obj: SwiftObject(self))
  }
  var bridgedNode: BridgedNode { BridgedNode(obj: SwiftObject(self)) }
}

extension BridgedInstruction {
  public var instruction: Instruction { obj.getAs(Instruction.self) }
  public func getAs<T: Instruction>(_ instType: T.Type) -> T { obj.getAs(T.self) }
}

extension OptionalBridgedInstruction {
  var instruction: Instruction? { obj.getAs(Instruction.self) }
}

public struct InstructionResults : Sequence, IteratorProtocol {
  let inst: Instruction
  let numResults: Int
  var index: Int = 0

  init(_ inst: Instruction) {
    self.inst = inst
    numResults = inst.resultCount
  }

  public mutating func next() -> Value? {
    let idx = index
    if idx < numResults {
      index += 1
      return inst.getResult(index: idx)
    }
    return nil
  }
}

public class SingleValueInstruction : Instruction, Value {
  final public var definingInstruction: Instruction? { self }

  fileprivate final override var resultCount: Int { 1 }
  fileprivate final override func getResult(index: Int) -> Value { self }
}

public final class MultipleValueInstructionResult : Value {
  final public var description: String {
    SILNode_debugDescription(bridgedNode).takeString()
  }

  public var definingInstruction: Instruction? {
    MultiValueInstResult_getParent(bridged).instruction
  }

  var bridged: BridgedMultiValueResult {
    BridgedMultiValueResult(obj: SwiftObject(self))
  }
  var bridgedNode: BridgedNode { BridgedNode(obj: SwiftObject(self)) }
}

extension BridgedMultiValueResult {
  var result: MultipleValueInstructionResult {
    obj.getAs(MultipleValueInstructionResult.self)
  }
}

public class MultipleValueInstruction : Instruction {
  fileprivate final override var resultCount: Int {
    return MultipleValueInstruction_getNumResults(bridged)
  }
  fileprivate final override func getResult(index: Int) -> Value {
    MultipleValueInstruction_getResult(bridged, index).result
  }
}

//===----------------------------------------------------------------------===//
//                             no-value instructions
//===----------------------------------------------------------------------===//

/// Used for all non-value instructions which are not implemented here, yet.
/// See registerBridgedClass() in SILBridgingUtils.cpp.
final public class UnimplementedInstruction : Instruction {
}

final public class CondFailInst : Instruction {
  public var condition: Value { operands[0].value }
  
  public var message: String { CondFailInst_getMessage(bridged).string }
}

public class RefCountingInst : Instruction {
  final public var global: GlobalVariable {
    GlobalAccessInst_getGlobal(bridged).globalVar
  }
}

final public class DebugValueInst : Instruction {
  public var value: Value { operands[0].value }
}

final public class UnimplementedRefCountingInst : RefCountingInst {}

//===----------------------------------------------------------------------===//
//                           single-value instructions
//===----------------------------------------------------------------------===//

/// Used for all SingleValueInstructions which are not implemented here, yet.
/// See registerBridgedClass() in SILBridgingUtils.cpp.
final public class UnimplementedSingleValueInst : SingleValueInstruction {
}

final public class BuiltinInst : SingleValueInstruction {}

final public class UpcastInst : SingleValueInstruction {
  var source: Value { operands[0].value }
}

public class GlobalAccessInst : SingleValueInstruction {
  final public var global: GlobalVariable {
    GlobalAccessInst_getGlobal(bridged).globalVar
  }
}

final public class GlobalAddrInst : GlobalAccessInst {}

final public class GlobalValueInst : GlobalAccessInst {}

final public class TupleExtractInst : SingleValueInstruction {
  public var tuple: Value { operands[0].value }
}

final public class RefElementAddrInst : SingleValueInstruction {
  var ref: Value { operands[0].value }
}

final public class RefTailAddrInst : SingleValueInstruction {
  var ref: Value { operands[0].value }
}

//===----------------------------------------------------------------------===//
//                            multi-value instructions
//===----------------------------------------------------------------------===//

/// Used for all UnimplementedMultiValueInst which are not implemented here, yet.
/// See registerBridgedClass() in SILBridgingUtils.cpp.
final public class UnimplementedMultiValueInst : MultipleValueInstruction {
}
