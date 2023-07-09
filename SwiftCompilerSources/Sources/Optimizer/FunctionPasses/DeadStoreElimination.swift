//===--- DeadStoreElimination.swift ----------------------------------------==//
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

/// Eliminates dead store instructions.
///
/// A store is dead if after the store has occurred:
///
/// 1. The value in memory is not read until the memory object is deallocated:
///
///     %1 = alloc_stack
///     ...
///     store %2 to %1
///     ...               // no reads from %1
///     dealloc_stack %1
///
/// 2. The value in memory is overwritten by another store before any potential read:
///
///     store %2 to %1
///     ...               // no reads from %1
///     store %3 to %1
///
/// In case of a partial dead store, the store is split so that some of the new
/// individual stores can be eliminated in the next round of the optimization:
///
///     store %2 to %1                          // partially dead
///     ...               // no reads from %1
///     %3 = struct_element_addr %1, #field1
///     store %7 to %3
/// ->
///     %3 = struct_extract %2, #field1
///     %4 = struct_element_addr %1, #field1
///     store %3 to %4                          // this store is dead now
///     %5 = struct_extract %2, #field2
///     %6 = struct_element_addr %1, #field2
///     store %5 to %6
///     ...              // no reads from %1
///     store %7 to %3
///
let deadStoreElimination = FunctionPass(name: "dead-store-elimination") {
  (function: Function, context: FunctionPassContext) in

  for block in function.blocks {

    // We cannot use for-in iteration here because if the store is split, the new
    // individual stores are inserted right afterwards and they would be ignored by a for-in iteration.
    var inst = block.instructions.first
    while let i = inst {
      if let store = i as? StoreInst {
        if !context.continueWithNextSubpassRun(for: store) {
          return
        }
        tryEliminate(store: store, context)
      }
      inst = i.next
    }
  }
}

private func tryEliminate(store: StoreInst, _ context: FunctionPassContext) {
  if !store.hasValidOwnershipForDeadStoreElimination {
    return
  }

  switch store.isDead(context) {
    case .alive:
      break
    case .dead:
      context.erase(instruction: store)
    case .maybePartiallyDead(let subPath):
      // Check if the a partial store would really be dead to avoid unnecessary splitting.
      switch store.isDead(at: subPath, context) {
        case .alive, .maybePartiallyDead:
          break
        case .dead:
          // The new individual stores are inserted right after the current store and
          // will be optimized in the following loop iterations.
          store.trySplit(context)
      }
  }
}

private extension StoreInst {

  enum DataflowResult {
    case alive
    case dead
    case maybePartiallyDead(AccessPath)

    init(aliveWith subPath: AccessPath?) {
      if let subPath = subPath {
        self = .maybePartiallyDead(subPath)
      } else {
        self = .alive
      }
    }
  }

  func isDead( _ context: FunctionPassContext) -> DataflowResult {
    return isDead(at: destination.accessPath, context)
  }

  func isDead(at accessPath: AccessPath, _ context: FunctionPassContext) -> DataflowResult {
    var worklist = InstructionWorklist(context)
    defer { worklist.deinitialize() }

    worklist.pushIfNotVisited(self.next!)

    let storageDefBlock = accessPath.base.reference?.referenceRoot.parentBlock
    var scanner = InstructionScanner(storePath: accessPath, storeAddress: self.destination, context.aliasAnalysis)

    while let startInstInBlock = worklist.pop() {
      let block = startInstInBlock.parentBlock
      switch scanner.scan(instructions: InstructionList(first: startInstInBlock)) {
      case .transparent:
        // Abort if we find the storage definition of the access in case of a loop, e.g.
        //
        //   bb1:
        //     %storage_root = apply
        //     %2 = ref_element_addr %storage_root
        //     store %3 to %2
        //     cond_br %c, bb1, bb2
        //
        // The storage root is different in each loop iteration. Therefore the store of a
        // successive loop iteration does not overwrite the store of the previous iteration.
        if let storageDefBlock = storageDefBlock,
           block.successors.contains(storageDefBlock) {
          return DataflowResult(aliveWith: scanner.potentiallyDeadSubpath)
        }
        worklist.pushIfNotVisited(contentsOf: block.successors.lazy.map { $0.instructions.first! })
      case .dead:
        break
      case .alive:
        return DataflowResult(aliveWith: scanner.potentiallyDeadSubpath)
      }
    }
    return .dead
  }

  func trySplit(_ context: FunctionPassContext) {
    let type = source.type
    if type.isStruct {
      let builder = Builder(after: self, context)
      for idx in 0..<type.getNominalFields(in: parentFunction).count {
        let srcField = builder.createStructExtract(struct: source, fieldIndex: idx)
        let destFieldAddr = builder.createStructElementAddr(structAddress: destination, fieldIndex: idx)
        builder.createStore(source: srcField, destination: destFieldAddr, ownership: destinationOwnership)
      }
      context.erase(instruction: self)
    } else if type.isTuple {
      let builder = Builder(after: self, context)
      for idx in 0..<type.tupleElements.count {
        let srcField = builder.createTupleExtract(tuple: source, elementIndex: idx)
        let destFieldAddr = builder.createTupleElementAddr(tupleAddress: destination, elementIndex: idx)
        builder.createStore(source: srcField, destination: destFieldAddr, ownership: destinationOwnership)
      }
      context.erase(instruction: self)
    }
  }

  var hasValidOwnershipForDeadStoreElimination: Bool {
    switch destinationOwnership {
    case .unqualified, .trivial:
      return true
    case .initialize, .assign:
      // In OSSA, non-trivial values cannot be dead-store eliminated because that could shrink
      // the lifetime of the original stored value (because it's not kept in memory anymore).
      return false
    }
  }
}

private struct InstructionScanner {
  let storePath: AccessPath
  let storeAddress: Value
  let aliasAnalysis: AliasAnalysis

  var potentiallyDeadSubpath: AccessPath? = nil

  // Avoid quadratic complexity by limiting the number of visited instructions for each store.
  // The limit of 1000 instructions is not reached by far in "real-world" functions.
  private var budget = 1000

  init(storePath: AccessPath, storeAddress: Value, _ aliasAnalysis: AliasAnalysis) {
    self.storePath = storePath
    self.storeAddress = storeAddress
    self.aliasAnalysis = aliasAnalysis
  }

  enum Result {
    case alive
    case dead
    case transparent
  }

  mutating func scan(instructions: InstructionList) -> Result {
    for inst in instructions {
      switch inst {
      case let successiveStore as StoreInst:
        let successivePath = successiveStore.destination.accessPath
        if successivePath.isEqualOrOverlaps(storePath) {
          return .dead
        }
        if storePath.isEqualOrOverlaps(successivePath),
           potentiallyDeadSubpath == nil {
          // Storing to a sub-field of the original store doesn't make the original store dead.
          // But when we split the original store, then one of the new individual stores might be
          // overwritten by this store.
          potentiallyDeadSubpath = successivePath
        }
      case is DeallocRefInst, is DeallocStackRefInst, is DeallocBoxInst:
        if (inst as! Deallocation).isDeallocation(of: storePath.base) {
          return .dead
        }
      case let ds as DeallocStackInst:
        if ds.isStackDeallocation(of: storePath.base) {
          return .dead
        }
      case is FixLifetimeInst, is EndAccessInst:
        break
      case let term as TermInst:
        if term.isFunctionExiting {
          return .alive
        }
        fallthrough
      default:
        budget -= 1
        if budget == 0 {
          return .alive
        }
        if inst.mayRead(fromAddress: storeAddress, aliasAnalysis) {
          return .alive
        }
      }
    }
    return .transparent
  }
}

private extension Deallocation {
  func isDeallocation(of base: AccessBase) -> Bool {
    if let accessReference = base.reference,
       accessReference.referenceRoot == self.allocatedValue.referenceRoot {
      return true
    }
    return false
  }
}

private extension DeallocStackInst {
  func isStackDeallocation(of base: AccessBase) -> Bool {
    if case .stack(let allocStack) = base, allocstack == allocStack {
      return true
    }
    return false
  }
}
