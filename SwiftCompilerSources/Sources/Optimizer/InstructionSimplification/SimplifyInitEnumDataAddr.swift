//===--- SimplifyInitEnumDataAddr.swift -----------------------------------===//
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

extension InitEnumDataAddrInst : OnoneSimplifyable {
  func simplify(_ context: SimplifyContext) {

    // Optimize the sequence
    // ```
    //   %1 = init_enum_data_addr %enum_addr, #someCaseWithPayload
    //   ...
    //   store %payload to %1
    //   inject_enum_addr %enum_addr, #someCaseWithPayload
    // ```
    // to
    // ```
    //   %1 = enum  $E, #someCaseWithPayload, %payload
    //   store %1 to %enum_addr
    // ```
    // This store and inject instructions must appear in consecutive order.
    // But usually this is the case, because it's generated this way by SILGen.
    // We also check that between the init and the store, no instruction writes to memory.
    //
    if let store = self.uses.singleUse?.instruction as? StoreInst,
       store.destination == self,
       let inject = store.next as? InjectEnumAddrInst,
       inject.enum == self.enum,
       inject.enum.type.isLoadable(in: parentFunction),
       !anyInstructionMayWriteToMemory(between: self, and: store) {

      assert(self.caseIndex == inject.caseIndex, "mismatching case indices when creating an enum")

      let builder = Builder(before: store, context)
      let enumInst = builder.createEnum(caseIndex: self.caseIndex, payload: store.source, enumType: self.enum.type.objectType)
      let storeOwnership = StoreInst.StoreOwnership(for: self.enum.type, in: parentFunction, initialize: true)
      builder.createStore(source: enumInst, destination: self.enum, ownership: storeOwnership)
      context.erase(instruction: store)
      context.erase(instruction: inject)
      context.erase(instruction: self)
    }
  }
}

// Returns false if `first` and `last` are in the same basic block and no instructions between them write to memory. True otherwise.
private func anyInstructionMayWriteToMemory(between first: Instruction, and last: Instruction) -> Bool {
  var nextInstruction = first.next
  while let i = nextInstruction {
    if i == last {
      return false
    }
    if i.mayWriteToMemory {
      return true
    }
    nextInstruction = i.next
  }
  // End of basic block, and we did not find `last`
  return true
}
