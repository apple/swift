//===--- SimplifyLoad.swift -----------------------------------------------===//
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

extension LoadInst : OnoneSimplifyable {
  func simplify(_ context: SimplifyContext) {
    replaceLoadOfGlobalLet(context)
  }

  private func replaceLoadOfGlobalLet(_ context: SimplifyContext) {
    guard let globalInitVal = getGlobalInitValue(address: address) else {
      return
    }
    let builder = Builder(before: self, context)
    guard let initVal = context.copyStaticInitializer(fromInitValue: globalInitVal, to: builder) else {
      return
    }
    uses.replaceAll(with: initVal, context)
    transitivelyErase(load: self, context)
  }
}

/// Returns the init value of a global which is loaded from `address`.
private func getGlobalInitValue(address: Value) -> Value? {
  switch address {
  case let gai as GlobalAddrInst:
    if gai.global.isLet {
      return gai.global.staticInitValue
    }
  case let pta as PointerToAddressInst:
    return globalLoadedViaAddressor(pointer: pta.pointer)?.staticInitValue
  case let sea as StructElementAddrInst:
    if let structVal = getGlobalInitValue(address: sea.struct) as? StructInst {
      return structVal.operands[sea.fieldIndex].value
    }
  case let tea as TupleElementAddrInst:
    if let tupleVal = getGlobalInitValue(address: tea.tuple) as? TupleInst {
      return tupleVal.operands[tea.fieldIndex].value
    }
  case let bai as BeginAccessInst:
    return getGlobalInitValue(address: bai.address)
  default:
    break
  }
  return nil
}

private func globalLoadedViaAddressor(pointer: Value) -> GlobalVariable? {
  if let ai = pointer as? ApplyInst,
     let callee = ai.referencedFunction,
     let global = callee.globalOfGlobalInitFunction,
     global.isLet {
    return global
  }
  return nil
}

private func transitivelyErase(load: LoadInst, _ context: SimplifyContext) {
  var inst: SingleValueInstruction = load
  while inst.uses.isEmpty {
    if inst.operands.count != 1 {
      context.erase(instruction: inst)
      return
    }
    let operandInst = inst.operands[0].value as! SingleValueInstruction
    context.erase(instruction: inst)
    inst = operandInst
  }
}
