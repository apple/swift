//===--- Builder.swift -  Building and modifying SIL ----------------------===//
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

/// A utility to create new instructions at a given insertion point.
public struct Builder {

  public enum InsertionPoint {
    case before(Instruction)
    case atEndOf(BasicBlock)
  }

  let insertAt: InsertionPoint
  let location: Location
  private let notificationHandler: BridgedChangeNotificationHandler
  private let notifyNewInstruction: (Instruction) -> ()

  public var bridged: BridgedBuilder {
    switch insertAt {
    case .before(let inst):
      return BridgedBuilder(insertBefore: inst.bridged.optional,
                            insertAtEnd: OptionalBridgedBasicBlock.none,
                            loc: location.bridged)
    case .atEndOf(let block):
      return BridgedBuilder(insertBefore: OptionalBridgedInstruction(),
                            insertAtEnd: block.bridged.optional,
                            loc: location.bridged)
    }
  }

  private func notifyNew<I: Instruction>(_ instruction: I) -> I {
    notificationHandler.notifyChanges(.instructionsChanged)
    if instruction is FullApplySite {
      notificationHandler.notifyChanges(.callsChanged)
    }
    if instruction is TermInst {
      notificationHandler.notifyChanges(.branchesChanged)
    }
    notifyNewInstruction(instruction)
    return instruction
  }

  public init(insertAt: InsertionPoint, location: Location,
              _ notifyNewInstruction: @escaping (Instruction) -> (),
              _ notificationHandler: BridgedChangeNotificationHandler) {
    self.insertAt = insertAt
    self.location = location;
    self.notifyNewInstruction = notifyNewInstruction
    self.notificationHandler = notificationHandler
  }

  public func createBuiltinBinaryFunction(name: String,
      operandType: Type, resultType: Type, arguments: [Value]) -> BuiltinInst {
    return arguments.withBridgedValues { valuesRef in
      return name._withStringRef { nameStr in
        let bi = bridged.createBuiltinBinaryFunction(
          nameStr, operandType.bridged, resultType.bridged, valuesRef)
        return notifyNew(bi.getAs(BuiltinInst.self))
      }
    }
  }

  public func createCondFail(condition: Value, message: String) -> CondFailInst {
    return message._withStringRef { messageStr in
      let cf = bridged.createCondFail(condition.bridged, messageStr)
      return notifyNew(cf.getAs(CondFailInst.self))
    }
  }

  public func createIntegerLiteral(_ value: Int, type: Type) -> IntegerLiteralInst {
    let literal = bridged.createIntegerLiteral(type.bridged, value)
    return notifyNew(literal.getAs(IntegerLiteralInst.self))
  }

  public func createAllocStack(_ type: Type, hasDynamicLifetime: Bool = false,
                               isLexical: Bool = false, usesMoveableValueDebugInfo: Bool = false) -> AllocStackInst {
    let dr = bridged.createAllocStack(type.bridged, hasDynamicLifetime, isLexical, usesMoveableValueDebugInfo)
    return notifyNew(dr.getAs(AllocStackInst.self))
  }

  @discardableResult
  public func createDeallocStack(_ operand: Value) -> DeallocStackInst {
    let dr = bridged.createDeallocStack(operand.bridged)
    return notifyNew(dr.getAs(DeallocStackInst.self))
  }

  @discardableResult
  public func createDeallocStackRef(_ operand: Value) -> DeallocStackRefInst {
    let dr = bridged.createDeallocStackRef(operand.bridged)
    return notifyNew(dr.getAs(DeallocStackRefInst.self))
  }

  public func createUncheckedRefCast(object: Value, type: Type) -> UncheckedRefCastInst {
    let object = bridged.createUncheckedRefCast(object.bridged, type.bridged)
    return notifyNew(object.getAs(UncheckedRefCastInst.self))
  }

  @discardableResult
  public func createSetDeallocating(operand: Value, isAtomic: Bool) -> SetDeallocatingInst {
    let setDeallocating = bridged.createSetDeallocating(operand.bridged, isAtomic)
    return notifyNew(setDeallocating.getAs(SetDeallocatingInst.self))
  }

  public func createFunctionRef(_ function: Function) -> FunctionRefInst {
    let functionRef = bridged.createFunctionRef(function.bridged)
    return notifyNew(functionRef.getAs(FunctionRefInst.self))
  }

  public func createCopyValue(operand: Value) -> CopyValueInst {
    return notifyNew(bridged.createCopyValue(operand.bridged).getAs(CopyValueInst.self))
  }

  @discardableResult
  public func createCopyAddr(from fromAddr: Value, to toAddr: Value,
                             takeSource: Bool = false, initializeDest: Bool = false) -> CopyAddrInst {
    return notifyNew(bridged.createCopyAddr(fromAddr.bridged, toAddr.bridged,
                                            takeSource, initializeDest).getAs(CopyAddrInst.self))
  }

  @discardableResult
  public func createDestroyValue(operand: Value) -> DestroyValueInst {
    return notifyNew(bridged.createDestroyValue(operand.bridged).getAs(DestroyValueInst.self))
  }

  @discardableResult
  public func createDebugStep() -> DebugStepInst {
    return notifyNew(bridged.createDebugStep().getAs(DebugStepInst.self))
  }

  @discardableResult
  public func createApply(
    function: Value,
    _ substitutionMap: SubstitutionMap,
    arguments: [Value],
    isNonThrowing: Bool = false,
    isNonAsync: Bool = false,
    specializationInfo: ApplyInst.SpecializationInfo = nil
  ) -> ApplyInst {
    let apply = arguments.withBridgedValues { valuesRef in
      bridged.createApply(function.bridged, substitutionMap.bridged, valuesRef,
                          isNonThrowing, isNonAsync, specializationInfo)
    }
    return notifyNew(apply.getAs(ApplyInst.self))
  }
  
  public func createUncheckedEnumData(enum enumVal: Value,
                                      caseIndex: Int,
                                      resultType: Type) -> UncheckedEnumDataInst {
    let ued = bridged.createUncheckedEnumData(enumVal.bridged, caseIndex, resultType.bridged)
    return notifyNew(ued.getAs(UncheckedEnumDataInst.self))
  }
  @discardableResult
  public func createSwitchEnum(enum enumVal: Value,
                               cases: [(Int, BasicBlock)],
                               defaultBlock: BasicBlock? = nil) -> SwitchEnumInst {
    let se = cases.withUnsafeBufferPointer { caseBuffer in
      bridged.createSwitchEnumInst(enumVal.bridged, defaultBlock.bridged,
                                   caseBuffer.baseAddress, caseBuffer.count)
    }
    return notifyNew(se.getAs(SwitchEnumInst.self))
  }
  
  @discardableResult
  public func createBranch(to destBlock: BasicBlock, arguments: [Value] = []) -> BranchInst {
    return arguments.withBridgedValues { valuesRef in
      let bi = bridged.createBranch(destBlock.bridged, valuesRef)
      return notifyNew(bi.getAs(BranchInst.self))
    }
  }

  @discardableResult
  public func createUnreachable() -> UnreachableInst {
    let ui = bridged.createUnreachable()
    return notifyNew(ui.getAs(UnreachableInst.self))
  }
}
