//===--- RedundantLoadElimination.swift ------------------------------------==//
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

/// Replaces redundant load instructions with already available values.
///
/// A load is redundant if the loaded value is already available at that point.
/// This can be via a preceding store to the same address:
///
///     store %1 to %addr
///     ...               // no writes to %addr
///     %2 = load %addr
/// ->
///     store %1 to %addr
///     ...               // no writes to %addr
///     // replace uses of %2 with the available value %1
///
/// or a preceding load from the same address:
///
///     %1 = load %addr
///     ...               // no writes to %addr
///     %2 = load %addr
/// ->
///     %1 = load %addr
///     ...               // no writes to %addr
///     // replace uses of %2 with the available value %1
///
/// In case of a partial redundant load, the load is split so that some of the new
/// individual loads can be eliminated in the next round of the optimization:
///
///     %fa1 = struct_element_addr %addr, #field1
///     store %1 to %fa1
///     ...               // no writes to %fa1
///     %2 = load %addr   // partially redundant
/// ->
///     %fa1 = struct_extract %addr, #field1
///     store %1 to %fa1
///     ...               // no writes to %fa1
///     %fa1 = struct_element_addr %addr, #field1
///     %f1 = load %fa1                              // this load is redundant now
///     %fa2 = struct_element_addr %addr, #field2
///     %f2 = load %fa2
///     %2 = struct (%f1, %f2)
///
/// The algorithm is a data flow analysis which starts at the original load and searches
/// for preceding stores or loads by following the control flow in backward direction.
/// The preceding stores and loads provide the "available values" with which the original
/// load can be replaced.
///
/// If the function is in OSSA, redundant loads are replaced in a way that no additional
/// copies of the loaded value are introduced. If this is not possible, the redundant load
/// is not replaced.
///
let redundantLoadElimination = FunctionPass(name: "redundant-load-elimination") {
    (function: Function, context: FunctionPassContext) in
  eliminateRedundantLoads(in: function, ignoreArrays: false, context)
}

// Early RLE does not touch loads from Arrays. This is important because later array optimizations,
// like ABCOpt, get confused if an array load in a loop is converted to a pattern with a phi argument.
let earlyRedundantLoadElimination = FunctionPass(name: "early-redundant-load-elimination") {
    (function: Function, context: FunctionPassContext) in
  eliminateRedundantLoads(in: function, ignoreArrays: true, context)
}

private func eliminateRedundantLoads(in function: Function, ignoreArrays: Bool, _ context: FunctionPassContext) {
  for block in function.blocks.reversed() {

    // We cannot use for-in iteration here because if the load is split, the new
    // individual loads are inserted right before and they would be ignored by a for-in iteration.
    var inst = block.instructions.reversed().first
    while let i = inst {
      defer { inst = i.previous }

      if let load = inst as? LoadInst {
        if !context.continueWithNextSubpassRun(for: load) {
          return
        }
        if ignoreArrays && load.type.isNominal && load.type.nominal == context.swiftArrayDecl {
          continue
        }
        tryEliminate(load: load, context)
      }
    }
  }
}

private func tryEliminate(load: LoadInst, _ context: FunctionPassContext) {
  switch load.isRedundant(context) {
  case .notRedundant:
    break
  case .redundant(let availableValues):
    replace(load: load, with: availableValues, context)
  case .maybePartiallyRedundant(let subPath):
    // Check if the a partial load would really be redundant to avoid unnecessary splitting.
    switch load.isRedundant(at: subPath, context) {
      case .notRedundant, .maybePartiallyRedundant:
        break
      case .redundant:
        // The new individual loads are inserted right before the current load and
        // will be optimized in the following loop iterations.
        load.trySplit(context)
    }
  }
}

private extension LoadInst {

  enum DataflowResult {
    case notRedundant
    case redundant([AvailableValue])
    case maybePartiallyRedundant(AccessPath)

    init(notRedundantWith subPath: AccessPath?) {
      if let subPath = subPath {
        self = .maybePartiallyRedundant(subPath)
      } else {
        self = .notRedundant
      }
    }
  }

  func isRedundant(_ context: FunctionPassContext) -> DataflowResult {
    return isRedundant(at: address.accessPath, context)
  }

  func isRedundant(at accessPath: AccessPath, _ context: FunctionPassContext) -> DataflowResult {
    var scanner = InstructionScanner(load: self, accessPath: accessPath, context.aliasAnalysis)

    switch scanner.scan(instructions: ReverseInstructionList(first: self.previous), in: parentBlock) {
    case .overwritten:
      return DataflowResult(notRedundantWith: scanner.potentiallyRedundantSubpath)
    case .available:
      return .redundant(scanner.availableValues)
    case .transparent:
      return self.isRedundantInPredecessorBlocks(scanner: &scanner, context)
    }
  }

  private func isRedundantInPredecessorBlocks(
    scanner: inout InstructionScanner,
    _ context: FunctionPassContext
  ) -> DataflowResult {

    var liferange = Liferange(endBlock: self.parentBlock, context)
    defer { liferange.deinitialize() }
    liferange.pushPredecessors(of: self.parentBlock)

    while let block = liferange.pop() {
      switch scanner.scan(instructions: block.instructions.reversed(), in: block) {
      case .overwritten:
        return DataflowResult(notRedundantWith: scanner.potentiallyRedundantSubpath)
      case .available:
        liferange.add(beginBlock: block)
      case .transparent:
        liferange.pushPredecessors(of: block)
      }
    }
    if !self.canReplaceWithoutInsertingCopies(liferange: liferange, context) {
      return DataflowResult(notRedundantWith: scanner.potentiallyRedundantSubpath)
    }
    return .redundant(scanner.availableValues)
  }

  func canReplaceWithoutInsertingCopies(liferange: Liferange,_ context: FunctionPassContext) -> Bool {
    switch self.loadOwnership {
    case .trivial, .unqualified:
      return true

    case .copy, .take:
      let deadEndBlocks = context.deadEndBlocks

      // The liferange of the value has an "exit", i.e. a path which doesn't lead to the load,
      // it means that we would have to insert a destroy on that exit to satisfy ownership rules.
      // But an inserted destroy also means that we would need to insert copies of the value which
      // were not there originally. For example:
      //
      //     store %1 to [init] %addr
      //     cond_br bb1, bb2
      //   bb1:
      //     %2 = load [take] %addr
      //   bb2:                      // liferange exit
      //
      // TODO: we could extend OSSA to transfer ownership to support liferange exits without copying. E.g.:
      //
      //     %b = store_and_borrow %1 to [init] %addr   // %b is borrowed from %addr
      //     cond_br bb1, bb2
      //   bb1:
      //     %o = borrowed_to_owned %b take_ownership_from %addr
      //     // replace %2 with %o
      //   bb2:
      //     end_borrow %b
      //
      if liferange.hasExits(deadEndBlocks) {
        return false
      }

      // Handle a corner case: if the load is in an infinite loop, the liferange doesn't have an exit,
      // but we still would need to insert a copy. For example:
      //
      //     store %1 to [init] %addr
      //     br bb1
      //   bb1:
      //     %2 = load [copy] %addr   // would need to insert a copy here
      //    br bb1                    // no exit from the liferange
      //
      // For simplicity, we don't handle this in OSSA.
      if deadEndBlocks.isDeadEnd(parentBlock) {
        return false
      }
      return true
    }
  }
}

private func replace(load: LoadInst, with availableValues: [AvailableValue], _ context: FunctionPassContext) {
  var ssaUpdater = SSAUpdater(type: load.type, ownership: load.ownership, context)

  for availableValue in availableValues {
    let block = availableValue.instruction.parentBlock
    let availableValue = provideValue(for: load, from: availableValue, context)
    ssaUpdater.addAvailableValue(availableValue, in: block)
  }

  let newValue: Value
  if availableValues.count == 1 {
    // A single available value means that this available value is located _before_ the load. E.g.:
    //
    //     store %1 to %addr   // a single available value
    //     ...
    //     %2 = load %addr     // The load
    //
    newValue = ssaUpdater.getValue(atEndOf: load.parentBlock)
  } else {
    // In case of multiple available values, if an available value is defined in the same basic block
    // as the load, this available is located _after_ the load. E.g.:
    //
    //     store %1 to %addr   // an available value
    //     br bb1
    //   bb1:
    //     %2 = load %addr     // The load
    //     store %3 to %addr   // another available value
    //     cond_br bb1, bb2
    //
    newValue = ssaUpdater.getValue(inMiddleOf: load.parentBlock)
  }
  load.uses.replaceAll(with: newValue, context)
  context.erase(instruction: load)
}

private func provideValue(
  for load: LoadInst,
  from availableValue: AvailableValue,
  _ context: FunctionPassContext
) -> Value {
  let projectionPath = availableValue.address.accessPath.getMaterializableProjection(to: load.address.accessPath)!

  switch load.loadOwnership {
  case .unqualified:
    return availableValue.value.createProjection(path: projectionPath,
                                                 builder: availableValue.getBuilderForProjections(context))
  case .copy, .trivial:
    // Note: even if the load is trivial, the available value may be projected out of a non-trivial value.
    return availableValue.value.createProjectionAndCopy(path: projectionPath,
                                                        builder: availableValue.getBuilderForProjections(context))
  case .take:
    if projectionPath.isEmpty {
      return shrinkMemoryLifetime(from: load, to: availableValue, context)
    } else {
      return shrinkMemoryLifetimeAndSplit(from: load, to: availableValue, projectionPath: projectionPath, context)
    }
  }
}

/// In case of a `load [take]` shrink lifetime of the value in memory back to the `availableValue`
/// and return the (possibly projected) available value. For example:
///
///     store %1 to [assign] %addr
///     ...
///     %2 = load [take] %addr
/// ->
///     destroy_addr %addr
///     ...
///     // replace %2 with %1
///
private func shrinkMemoryLifetime(from load: LoadInst, to availableValue: AvailableValue, _ context: FunctionPassContext) -> Value {
  switch availableValue {
  case .viaLoad(let availableLoad):
    assert(availableLoad.loadOwnership == .copy)
    let builder = Builder(after: availableLoad, context)
    availableLoad.set(ownership: .take, context)
    return builder.createCopyValue(operand: availableLoad)
  case .viaStore(let availableStore):
    let builder = Builder(after: availableStore, context)
    let valueToAdd = availableStore.source
    switch availableStore.storeOwnership {
    case .assign:
      builder.createDestroyAddr(address: availableStore.destination)
      context.erase(instruction: availableStore)
    case .initialize:
      context.erase(instruction: availableStore)
    case .trivial, .unqualified:
      fatalError("store ownership doesn't fit a later load [take]")
    }
    return valueToAdd
  }
}

/// Like `shrinkMemoryLifetime`, but the available value must be projected.
/// In this case we cannot just shrink the lifetime and reuse the available value.
/// Therefore, we split the available load or store and load the projected available value.
/// The inserted load can be optimized with the split value in the next iteration.
///
///     store %1 to [assign] %addr
///     ...
///     %2 = struct_element_addr %addr, #field1
///     %3 = load [take] %2
/// ->
///     %f1 = struct_extract %1, #field1
///     %fa1 = struct_element_addr %addr, #field1
///     store %f1 to [assign] %fa1
///     %f2 = struct_extract %1, #field2
///     %fa2 = struct_element_addr %addr, #field2
///     store %f2 to [assign] %fa2
///     %1 = load [take] %fa1         // will be combined with `store %f1 to [assign] %fa1` in the next iteration
///     ...
///     // replace %3 with %1
///
private func shrinkMemoryLifetimeAndSplit(from load: LoadInst, to availableValue: AvailableValue, projectionPath: SmallProjectionPath, _ context: FunctionPassContext) -> Value {
  switch availableValue {
  case .viaLoad(let availableLoad):
    assert(availableLoad.loadOwnership == .copy)
    let builder = Builder(after: availableLoad, context)
    let addr = availableLoad.address.createAddressProjection(path: projectionPath, builder: builder)
    let valueToAdd = builder.createLoad(fromAddress: addr, ownership: .take)
    availableLoad.trySplit(context)
    return valueToAdd
  case .viaStore(let availableStore):
    let builder = Builder(after: availableStore, context)
    let addr = availableStore.destination.createAddressProjection(path: projectionPath, builder: builder)
    let valueToAdd = builder.createLoad(fromAddress: addr, ownership: .take)
    availableStore.trySplit(context)
    return valueToAdd
  }
}

/// Either a `load` or `store` which is preceding the original load and provides the loaded value.
private enum AvailableValue {
  case viaLoad(LoadInst)
  case viaStore(StoreInst)

  var value: Value {
    switch self {
    case .viaLoad(let load):   return load
    case .viaStore(let store): return store.source
    }
  }

  var address: Value {
    switch self {
    case .viaLoad(let load):   return load.address
    case .viaStore(let store): return store.destination
    }
  }

  var instruction: Instruction {
    switch self {
    case .viaLoad(let load):   return load
    case .viaStore(let store): return store
    }
  }

  func getBuilderForProjections(_ context: FunctionPassContext) -> Builder {
    switch self {
    case .viaLoad(let load):   return Builder(after: load, context)
    case .viaStore(let store): return Builder(before: store, context)
    }
  }
}

private struct InstructionScanner {
  private let load: LoadInst
  private let accessPath: AccessPath
  private let storageDefBlock: BasicBlock?
  private let aliasAnalysis: AliasAnalysis

  private(set) var potentiallyRedundantSubpath: AccessPath? = nil
  private(set) var availableValues = Array<AvailableValue>()

  // Avoid quadratic complexity by limiting the number of visited instructions for each store.
  // The limit of 1000 instructions is not reached by far in "real-world" functions.
  private var budget = 1000

  init(load: LoadInst, accessPath: AccessPath, _ aliasAnalysis: AliasAnalysis) {
    self.load = load
    self.accessPath = accessPath
    self.storageDefBlock = accessPath.base.reference?.referenceRoot.parentBlock
    self.aliasAnalysis = aliasAnalysis
  }

  enum ScanResult {
    case overwritten
    case available
    case transparent
  }

  mutating func scan(instructions: ReverseInstructionList, in block: BasicBlock) -> ScanResult {
    for inst in instructions {
      switch visit(instruction: inst) {
        case .available:   return .available
        case .overwritten: return .overwritten
        case .transparent: break
      }
    }

    // Abort if we find the storage definition of the access in case of a loop, e.g.
    //
    //   bb1:
    //     %storage_root = apply
    //     %2 = ref_element_addr %storage_root
    //     %3 = load %2
    //     cond_br %c, bb1, bb2
    //
    // The storage root is different in each loop iteration. Therefore the load in a
    // successive loop iteration does not load from the same address as in the previous iteration.
    if let storageDefBlock = storageDefBlock,
       block == storageDefBlock {
      return .overwritten
    }
    if block.predecessors.isEmpty {
      // We reached the function entry without finding an available value.
      return .overwritten
    }
    return .transparent
  }

  private mutating func visit(instruction: Instruction) -> ScanResult {
    switch instruction {
    case is FixLifetimeInst, is EndAccessInst, is BeginBorrowInst, is EndBorrowInst:
      return .transparent

    case let precedingLoad as LoadInst:
      if precedingLoad == load {
        // We need to stop the data flow analysis when we visit the original load again.
        // This happens if the load is in a loop.
        return .available
      }
      let precedingLoadPath = precedingLoad.address.accessPath
      if precedingLoadPath.getMaterializableProjection(to: accessPath) != nil {
        availableValues.append(.viaLoad(precedingLoad))
        return .available
      }
      if accessPath.getMaterializableProjection(to: precedingLoadPath) != nil,
         potentiallyRedundantSubpath == nil {
        potentiallyRedundantSubpath = precedingLoadPath
      }
      if load.loadOwnership != .take {
        return .transparent
      }

    case let precedingStore as StoreInst:
      if precedingStore.source is Undef {
        return .overwritten
      }
      let precedingStorePath = precedingStore.destination.accessPath
      if precedingStorePath.getMaterializableProjection(to: accessPath) != nil {
        availableValues.append(.viaStore(precedingStore))
        return .available
      }
      if accessPath.getMaterializableProjection(to: precedingStorePath) != nil,
         potentiallyRedundantSubpath == nil {
        potentiallyRedundantSubpath = precedingStorePath
      }

    default:
      break
    }
    budget -= 1
    if budget == 0 {
      return .overwritten
    }
    if load.loadOwnership == .take {
      // In case of `take`, don't allow reading instructions in the liferange.
      // Otherwise we cannot shrink the memory liferange afterwards.
      if instruction.mayReadOrWrite(address: load.address, aliasAnalysis) {
        return .overwritten
      }
    } else {
      if instruction.mayWrite(toAddress: load.address, aliasAnalysis) {
        return .overwritten
      }
    }
    return .transparent
  }
}

/// Represents the liferange (in terms of basic blocks) of the loaded value.
///
/// In contrast to a BlockRange, this liferange has multiple begin blocks (containing the
/// available values) and a single end block (containing the original load). For example:
///
///   bb1:
///     store %1 to %addr   // begin block
///     br bb3
///   bb2:
///     store %2 to %addr   // begin block
///     br bb3
///   bb3:
///     %3 = load %addr     // end block
///
private struct Liferange {
  private var worklist: BasicBlockWorklist
  private var containingBlocks: Stack<BasicBlock> // doesn't include the end-block
  private var beginBlocks: BasicBlockSet
  private let endBlock: BasicBlock

  init(endBlock: BasicBlock, _ context: FunctionPassContext) {
    self.worklist = BasicBlockWorklist(context)
    self.containingBlocks = Stack(context)
    self.beginBlocks = BasicBlockSet(context)
    self.endBlock = endBlock
    pushPredecessors(of: endBlock)
  }

  mutating func deinitialize() {
    worklist.deinitialize()
    containingBlocks.deinitialize()
    beginBlocks.deinitialize()
  }

  mutating func pushPredecessors(of block: BasicBlock) {
    worklist.pushIfNotVisited(contentsOf: block.predecessors)
    containingBlocks.append(contentsOf: block.predecessors)
  }

  mutating func pop() -> BasicBlock? { worklist.pop() }

  mutating func add(beginBlock: BasicBlock) {
    beginBlocks.insert(beginBlock)
  }

  /// Returns true if there is some path from a begin block to a function exit which doesn't
  /// go through the end-block. For example:
  ///
  ///     store %1 to %addr   // begin
  ///     cond_br bb1, bb2
  ///   bb1:
  ///     %2 = load %addr     // end
  ///   bb2:
  ///     ...                 // exit
  ///
  func hasExits(_ deadEndBlocks: DeadEndBlocksAnalysis) -> Bool {
    for block in containingBlocks {
      for succ in block.successors {
        if succ != endBlock,
           (!worklist.hasBeenPushed(succ) || beginBlocks.contains(succ)),
           !deadEndBlocks.isDeadEnd(succ) {
          return true
        }
      }
    }
    return false
  }
}

private extension Value {
  func createProjection(path: SmallProjectionPath, builder: Builder) -> Value {
    let (kind, index, subPath) = path.pop()
    switch kind {
    case .root:
      return self
    case .structField:
      let structExtract = builder.createStructExtract(struct: self, fieldIndex: index)
      return structExtract.createProjection(path: subPath, builder: builder)
    case .tupleField:
      let tupleExtract = builder.createTupleExtract(tuple: self, elementIndex: index)
      return tupleExtract.createProjection(path: subPath, builder: builder)
    default:
      fatalError("path is not materializable")
    }
  }

  func createAddressProjection(path: SmallProjectionPath, builder: Builder) -> Value {
    let (kind, index, subPath) = path.pop()
    switch kind {
    case .root:
      return self
    case .structField:
      let structExtract = builder.createStructElementAddr(structAddress: self, fieldIndex: index)
      return structExtract.createAddressProjection(path: subPath, builder: builder)
    case .tupleField:
      let tupleExtract = builder.createTupleElementAddr(tupleAddress: self, elementIndex: index)
      return tupleExtract.createAddressProjection(path: subPath, builder: builder)
    default:
      fatalError("path is not materializable")
    }
  }

  func createProjectionAndCopy(path: SmallProjectionPath, builder: Builder) -> Value {
    if path.isEmpty {
      return self.copyIfNotTrivial(builder)
    }
    if self.ownership == .owned {
      let borrow = builder.createBeginBorrow(of: self)
      let projectedValue = borrow.createProjection(path: path, builder: builder)
      let result = projectedValue.copyIfNotTrivial(builder)
      builder.createEndBorrow(of: borrow)
      return result
    }
    let projectedValue = self.createProjection(path: path, builder: builder)
    return projectedValue.copyIfNotTrivial(builder)
  }

  func copyIfNotTrivial(_ builder: Builder) -> Value {
    if type.isTrivial(in: parentFunction) {
      return self
    }
    return builder.createCopyValue(operand: self)
  }
}
