//===--- OptUtils.swift - Utilities for optimizations ---------------------===//
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

import ASTBridging
import SIL
import OptimizerBridging

extension Value {
  var lookThroughBorrow: Value {
    if let beginBorrow = self as? BeginBorrowInst {
      return beginBorrow.borrowedValue.lookThroughBorrow
    }
    return self
  }

  var lookThroughCopy: Value {
    if let copy = self as? CopyValueInst {
      return copy.fromValue.lookThroughCopy
    }
    return self
  }

  var lookThoughOwnershipInstructions: Value {
    switch self {
    case let beginBorrow as BeginBorrowInst:
      return beginBorrow.borrowedValue.lookThoughOwnershipInstructions
    case let copy as CopyValueInst:
      return copy.fromValue.lookThoughOwnershipInstructions
    case let move as MoveValueInst:
      return move.fromValue.lookThoughOwnershipInstructions
    default:
      return self
    }
  }

  /// Walks over all fields of an aggregate and checks if a reference count
  /// operation for this value is required. This differs from a simple `Type.isTrivial`
  /// check, because it treats a value_to_bridge_object instruction as "trivial".
  /// It can also handle non-trivial enums with trivial cases.
  func isTrivial(_ context: some Context) -> Bool {
    var worklist = ValueWorklist(context)
    defer { worklist.deinitialize() }

    worklist.pushIfNotVisited(self)
    while let v = worklist.pop() {
      if v.type.isTrivial(in: parentFunction) {
        continue
      }
      if v.type.isValueTypeWithDeinit {
        return false
      }
      switch v {
      case is ValueToBridgeObjectInst:
        break
      case let si as StructInst:
        worklist.pushIfNotVisited(contentsOf: si.operands.values)
      case let ti as TupleInst:
        worklist.pushIfNotVisited(contentsOf: ti.operands.values)
      case let en as EnumInst:
        if let payload = en.payload {
          worklist.pushIfNotVisited(payload)
        }
      default:
        return false
      }
    }
    return true
  }

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

  /// True if this value is a valid in a static initializer, including all its operands.
  func isValidGlobalInitValue(_ context: some Context) -> Bool {
    guard let svi = self as? SingleValueInstruction else {
      return false
    }
    if let beginAccess = svi as? BeginAccessInst {
      return beginAccess.address.isValidGlobalInitValue(context)
    }
    if !svi.isValidInStaticInitializerOfGlobal(context) {
      return false
    }
    for op in svi.operands {
      if !op.value.isValidGlobalInitValue(context) {
        return false
      }
    }
    return true
  }
}

extension FullApplySite {
  func isSemanticCall(_ name: StaticString, withArgumentCount: Int) -> Bool {
    if arguments.count == withArgumentCount,
       let callee = referencedFunction,
       callee.hasSemanticsAttribute(name)
    {
      return true
    }
    return false
  }
}

extension Builder {
  static func insert(after inst: Instruction, location: Location,
                     _ context: some MutatingContext, insertFunc: (Builder) -> ()) {
    if inst is TermInst {
      for succ in inst.parentBlock.successors {
        assert(succ.hasSinglePredecessor,
               "the terminator instruction must not have critical successors")
        let builder = Builder(before: succ.instructions.first!, location: location,
                              context)
        insertFunc(builder)
      }
    } else {
      let builder = Builder(after: inst, location: location, context)
      insertFunc(builder)
    }
  }

  func destroyCapturedArgs(for paiOnStack: PartialApplyInst) {
    precondition(paiOnStack.isOnStack, "Function must only be called for `partial_apply`s on stack!")
    self.bridged.destroyCapturedArgs(paiOnStack.bridged)
  }
}

extension Value {
  /// Return true if all elements occur on or after `instruction` in
  /// control flow order. If this returns true, then zero or more uses
  /// of `self` may be operands of `instruction` itself.
  ///
  /// This performs a backward CFG walk from `instruction` to `self`.
  func usesOccurOnOrAfter(instruction: Instruction, _ context: some Context)
  -> Bool {
    var users = InstructionSet(context)
    defer { users.deinitialize() }
    uses.lazy.map({ $0.instruction }).forEach { users.insert($0) }

    var worklist = InstructionWorklist(context)
    defer { worklist.deinitialize() }

    let pushPreds = { (block: BasicBlock) in
      block.predecessors.lazy.map({ pred in pred.terminator }).forEach {
        worklist.pushIfNotVisited($0)
      }
    }
    if let prev = instruction.previous {
      worklist.pushIfNotVisited(prev)
    } else {
      pushPreds(instruction.parentBlock)
    }
    let definingInst = self.definingInstruction
    while let lastInst = worklist.pop() {
      for inst in ReverseInstructionList(first: lastInst) {
        if users.contains(inst) {
          return false
        }
        if inst == definingInst {
          break
        }
      }
      if lastInst.parentBlock != self.parentBlock {
        pushPreds(lastInst.parentBlock)
      }
    }
    return true
  }
}

extension Value {
  /// Makes this new owned value available to be used in the block `destBlock`.
  ///
  /// Inserts required `copy_value` and `destroy_value` operations in case the `destBlock`
  /// is in a different control region than this value. For example, if `destBlock` is
  /// in a loop while this value is not in that loop, the value has to be copied for
  /// each loop iteration.
  func makeAvailable(in destBlock: BasicBlock, _ context: some MutatingContext) -> Value {
    assert(uses.isEmpty)
    assert(ownership == .owned)

    let beginBlock = parentBlock
    var useToDefRange = BasicBlockRange(begin: beginBlock, context)
    defer { useToDefRange.deinitialize() }

    useToDefRange.insert(destBlock)

    // The value needs to be destroyed at every exit of the liverange.
    for exitBlock in useToDefRange.exits {
      let builder = Builder(before: exitBlock.instructions.first!, context)
      builder.createDestroyValue(operand: self)
    }
  
    if useToDefRange.contains(destBlock) {
      // The `destBlock` is within a loop, so we need to copy the value at each iteration.
      let builder = Builder(before: destBlock.instructions.first!, context)
      return builder.createCopyValue(operand: self)
    }
    return self
  }

  /// Copies this value at `insertionPoint` and makes the copy available to be used in `destBlock`.
  ///
  /// For details see `makeAvailable`.
  func copy(at insertionPoint: Instruction, andMakeAvailableIn destBlock: BasicBlock,
            _ context: some MutatingContext) -> Value {
    let builder = Builder(before: insertionPoint, context)
    let copiedValue = builder.createCopyValue(operand: self)
    return copiedValue.makeAvailable(in: destBlock, context)
  }
}

extension Instruction {
  var isTriviallyDead: Bool {
    if results.contains(where: { !$0.uses.isEmpty }) {
      return false
    }
    return self.canBeRemovedIfNotUsed
  }

  var isTriviallyDeadIgnoringDebugUses: Bool {
    if results.contains(where: { !$0.uses.ignoreDebugUses.isEmpty }) {
      return false
    }
    return self.canBeRemovedIfNotUsed
  }

  private var canBeRemovedIfNotUsed: Bool {
    // TODO: it is horrible to hard-code exceptions here, but currently there is no Instruction API for this.
    switch self {
    case is TermInst, is MarkUninitializedInst, is DebugValueInst:
      return false
    case let bi as BuiltinInst:
      if bi.id == .OnFastPath {
        return false
      }
    case is UncheckedEnumDataInst:
      // Don't remove UncheckedEnumDataInst in OSSA in case it is responsible
      // for consuming an enum value.
      return !parentFunction.hasOwnership
    default:
      break
    }
    return !mayReadOrWriteMemory && !hasUnspecifiedSideEffects
  }

  func isValidInStaticInitializerOfGlobal(_ context: some Context) -> Bool {
    // Rule out SILUndef and SILArgument.
    if operands.contains(where: { $0.value.definingInstruction == nil }) {
      return false
    }
    switch self {
    case let bi as BuiltinInst:
      switch bi.id {
      case .ZeroInitializer:
        let type = bi.type.isBuiltinVector ? bi.type.builtinVectorElementType : bi.type
        return type.isBuiltinInteger || type.isBuiltinFloat
      case .PtrToInt:
        return bi.operands[0].value is StringLiteralInst
      case .IntToPtr:
        return bi.operands[0].value is IntegerLiteralInst
      case .StringObjectOr:
        // The first operand can be a string literal (i.e. a pointer), but the
        // second operand must be a constant. This enables creating a
        // a pointer+offset relocation.
        // Note that StringObjectOr requires the or'd bits in the first
        // operand to be 0, so the operation is equivalent to an addition.
        return bi.operands[1].value is IntegerLiteralInst
      case .ZExtOrBitCast:
        return true;
      case .USubOver:
        // Handle StringObjectOr(tuple_extract(usub_with_overflow(x, offset)), bits)
        // This pattern appears in UTF8 String literal construction.
        if let tei = bi.uses.getSingleUser(ofType: TupleExtractInst.self),
           tei.isResultOfOffsetSubtract {
          return true
        }
        return false
      case .OnFastPath:
        return true
      default:
        return false
      }
    case let tei as TupleExtractInst:
      // Handle StringObjectOr(tuple_extract(usub_with_overflow(x, offset)), bits)
      // This pattern appears in UTF8 String literal construction.
      if tei.isResultOfOffsetSubtract,
         let bi = tei.uses.getSingleUser(ofType: BuiltinInst.self),
         bi.id == .StringObjectOr {
        return true
      }
      return false
    case let sli as StringLiteralInst:
      switch sli.encoding {
      case .Bytes, .UTF8, .UTF8_OSLOG:
        return true
      case .ObjCSelector:
        // Objective-C selector string literals cannot be used in static
        // initializers.
        return false
      }
    case let gvi as GlobalValueInst:
      return context.canMakeStaticObjectReadOnly(objectType: gvi.type)
    case is StructInst,
         is TupleInst,
         is EnumInst,
         is IntegerLiteralInst,
         is FloatLiteralInst,
         is ObjectInst,
         is VectorInst,
         is AllocVectorInst,
         is UncheckedRefCastInst,
         is UpcastInst,
         is ValueToBridgeObjectInst,
         is ConvertFunctionInst,
         is ThinToThickFunctionInst,
         is AddressToPointerInst,
         is GlobalAddrInst,
         is FunctionRefInst:
      return true
    default:
      return false
    }
  }
}

// Match the pattern:
// tuple_extract(usub_with_overflow(x, integer_literal, integer_literal 0), 0)
private extension TupleExtractInst {
  var isResultOfOffsetSubtract: Bool {
    if fieldIndex == 0,
       let bi = tuple as? BuiltinInst,
       bi.id == .USubOver,
       bi.operands[1].value is IntegerLiteralInst,
       let overflowLiteral = bi.operands[2].value as? IntegerLiteralInst,
       let overflowValue = overflowLiteral.value,
       overflowValue == 0
    {
      return true
    }
    return false
  }
}

extension StoreInst {
  func trySplit(_ context: FunctionPassContext) {
    let builder = Builder(after: self, context)
    let type = source.type
    if type.isStruct {
      if type.nominal.isStructWithUnreferenceableStorage {
        return
      }
      if parentFunction.hasOwnership && source.ownership != .none {
        let destructure = builder.createDestructureStruct(struct: source)
        for (fieldIdx, fieldValue) in destructure.results.enumerated() {
          let destFieldAddr = builder.createStructElementAddr(structAddress: destination, fieldIndex: fieldIdx)
          builder.createStore(source: fieldValue, destination: destFieldAddr, ownership: splitOwnership(for: fieldValue))
        }
      } else {
        guard let fields = type.getNominalFields(in: parentFunction) else {
          return
        }
        for idx in 0..<fields.count {
          let srcField = builder.createStructExtract(struct: source, fieldIndex: idx)
          let fieldAddr = builder.createStructElementAddr(structAddress: destination, fieldIndex: idx)
          builder.createStore(source: srcField, destination: fieldAddr, ownership: splitOwnership(for: srcField))
        }
      }
    } else if type.isTuple {
      if parentFunction.hasOwnership && source.ownership != .none {
        let destructure = builder.createDestructureTuple(tuple: source)
        for (elementIdx, elementValue) in destructure.results.enumerated() {
          let elementAddr = builder.createTupleElementAddr(tupleAddress: destination, elementIndex: elementIdx)
          builder.createStore(source: elementValue, destination: elementAddr, ownership: splitOwnership(for: elementValue))
        }
      } else {
        for idx in 0..<type.tupleElements.count {
          let srcField = builder.createTupleExtract(tuple: source, elementIndex: idx)
          let destFieldAddr = builder.createTupleElementAddr(tupleAddress: destination, elementIndex: idx)
          builder.createStore(source: srcField, destination: destFieldAddr, ownership: splitOwnership(for: srcField))
        }
      }
    } else {
      return
    }
    context.erase(instruction: self)
  }

  private func splitOwnership(for fieldValue: Value) -> StoreOwnership {
    switch self.storeOwnership {
    case .trivial, .unqualified:
      return self.storeOwnership
    case .assign, .initialize:
      return fieldValue.type.isTrivial(in: parentFunction) ? .trivial : self.storeOwnership
    }
  }
}

extension LoadInst {
  func trySplit(_ context: FunctionPassContext) {
    var elements = [Value]()
    let builder = Builder(before: self, context)
    if type.isStruct {
      if type.nominal.isStructWithUnreferenceableStorage {
        return
      }
      guard let fields = type.getNominalFields(in: parentFunction) else {
        return
      }
      for idx in 0..<fields.count {
        let fieldAddr = builder.createStructElementAddr(structAddress: address, fieldIndex: idx)
        let splitLoad = builder.createLoad(fromAddress: fieldAddr, ownership: self.splitOwnership(for: fieldAddr))
        elements.append(splitLoad)
      }
      let newStruct = builder.createStruct(type: self.type, elements: elements)
      self.uses.replaceAll(with: newStruct, context)
    } else if type.isTuple {
      var elements = [Value]()
      let builder = Builder(before: self, context)
      for idx in 0..<type.tupleElements.count {
        let fieldAddr = builder.createTupleElementAddr(tupleAddress: address, elementIndex: idx)
        let splitLoad = builder.createLoad(fromAddress: fieldAddr, ownership: self.splitOwnership(for: fieldAddr))
        elements.append(splitLoad)
      }
      let newTuple = builder.createTuple(type: self.type, elements: elements)
      self.uses.replaceAll(with: newTuple, context)
    } else {
      return
    }
    context.erase(instruction: self)
  }

  private func splitOwnership(for fieldValue: Value) -> LoadOwnership {
    switch self.loadOwnership {
    case .trivial, .unqualified:
      return self.loadOwnership
    case .copy, .take:
      return fieldValue.type.isTrivial(in: parentFunction) ? .trivial : self.loadOwnership
    }
  }
}

extension FunctionPassContext {
  /// Returns true if any blocks were removed.
  func removeDeadBlocks(in function: Function) -> Bool {
    var reachableBlocks = ReachableBlocks(function: function, self)
    defer { reachableBlocks.deinitialize() }

    var blocksRemoved = false
    for block in function.blocks {
      if !reachableBlocks.isReachable(block: block) {
        block.dropAllReferences(self)
        erase(block: block)
        blocksRemoved = true
      }
    }
    return blocksRemoved
  }

  func removeTriviallyDeadInstructionsPreservingDebugInfo(in function: Function) {
    for inst in function.reversedInstructions {
      if inst.isTriviallyDead {
        erase(instruction: inst)
      }
    }
  }

  func removeTriviallyDeadInstructionsIgnoringDebugUses(in function: Function) {
    for inst in function.reversedInstructions {
      if inst.isTriviallyDeadIgnoringDebugUses {
        erase(instructionIncludingDebugUses: inst)
      }
    }
  }
}

extension BasicBlock {
  func dropAllReferences(_ context: FunctionPassContext) {
    for arg in arguments {
      arg.uses.replaceAll(with: Undef.get(type: arg.type, context), context)
    }
    for inst in instructions.reversed() {
      for result in inst.results {
        result.uses.replaceAll(with: Undef.get(type: result.type, context), context)
      }
      context.erase(instruction: inst)
    }
  }
}

extension SimplifyContext {

  /// Replaces a pair of redudant instructions, like
  /// ```
  ///   %first = enum $E, #E.CaseA!enumelt, %replacement
  ///   %second = unchecked_enum_data %first : $E, #E.CaseA!enumelt
  /// ```
  /// Replaces `%second` with `%replacement` and deletes the instructions if possible - or required.
  /// The operation is not done if it would require to insert a copy due to keep ownership correct.
  func tryReplaceRedundantInstructionPair(first: SingleValueInstruction, second: SingleValueInstruction,
                                          with replacement: Value) {
    let singleUse = preserveDebugInfo ? first.uses.singleUse : first.uses.ignoreDebugUses.singleUse
    let canEraseFirst = singleUse?.instruction == second

    if !canEraseFirst && first.parentFunction.hasOwnership && replacement.ownership == .owned {
      // We cannot add more uses to `replacement` without inserting a copy.
      return
    }

    second.uses.replaceAll(with: replacement, self)
    erase(instruction: second)

    if canEraseFirst {
      erase(instructionIncludingDebugUses: first)
    }
  }
}

extension ProjectedValue {
  /// Returns true if the address can alias with `rhs`.
  ///
  /// Example:
  ///   %1 = struct_element_addr %s, #field1
  ///   %2 = struct_element_addr %s, #field2
  ///
  /// `%s`.canAddressAlias(with: `%1`) -> true
  /// `%s`.canAddressAlias(with: `%2`) -> true
  /// `%1`.canAddressAlias(with: `%2`) -> false
  ///
  func canAddressAlias(with rhs: ProjectedValue, complexityBudget: Int = Int.max, _ context: some Context) -> Bool {
    // self -> rhs will succeed (= return false) if self is a non-escaping "local" object,
    // but not necessarily rhs.
    if !isEscaping(using: EscapesToValueVisitor(target: rhs), complexityBudget: complexityBudget, context) {
      return false
    }
    // The other way round: rhs -> self will succeed if rhs is a non-escaping "local" object,
    // but not necessarily self.
    if !rhs.isEscaping(using: EscapesToValueVisitor(target: self), complexityBudget: complexityBudget, context) {
      return false
    }
    return true
  }
}

private struct EscapesToValueVisitor : EscapeVisitor {
  let target: ProjectedValue

  mutating func visitUse(operand: Operand, path: EscapePath) -> UseResult {
    if operand.value == target.value && path.projectionPath.mayOverlap(with: target.path) {
      return .abort
    }
    if operand.instruction is ReturnInst {
      // Anything which is returned cannot escape to an instruction inside the function.
      return .ignore
    }
    return .continueWalk
  }

  var followTrivialTypes: Bool { true }
  var followLoads: Bool { false }
}

extension Function {
  var globalOfGlobalInitFunction: GlobalVariable? {
    if isGlobalInitFunction,
       let ret = returnInstruction,
       let atp = ret.returnedValue as? AddressToPointerInst,
       let ga = atp.address as? GlobalAddrInst {
      return ga.global
    }
    return nil
  }

  var initializedGlobal: GlobalVariable? {
    if !isGlobalInitOnceFunction {
      return nil
    }
    for inst in entryBlock.instructions {
      if let allocGlobal = inst as? AllocGlobalInst {
        return allocGlobal.global
      }
    }
    return nil
  }

  /// True if this function has a dynamic-self metadata argument and any instruction is type dependent on it.
  var mayBindDynamicSelf: Bool {
    guard let dynamicSelf = self.dynamicSelfMetadata else {
      return false
    }
    return dynamicSelf.uses.contains { $0.isTypeDependent }
  }
}

extension FullApplySite {
  var canInline: Bool {
    // Some checks which are implemented in C++
    if !FullApplySite_canInline(bridged) {
      return false
    }
    // Cannot inline a non-inlinable function it an inlinable function.
    if let calleeFunction = referencedFunction,
       !calleeFunction.canBeInlinedIntoCaller(parentFunction.serializedKind) {
      return false
    }

    // Cannot inline a non-ossa function into an ossa function
    if parentFunction.hasOwnership,
      let calleeFunction = referencedFunction,
      !calleeFunction.hasOwnership {
      return false
    }

    return true
  }

  var inliningCanInvalidateStackNesting: Bool {
    guard let calleeFunction = referencedFunction else {
      return false
    }

    // In OSSA `partial_apply [on_stack]`s are represented as owned values rather than stack locations.
    // It is possible for their destroys to violate stack discipline.
    // When inlining into non-OSSA, those destroys are lowered to dealloc_stacks.
    // This can result in invalid stack nesting.
    if calleeFunction.hasOwnership && !parentFunction.hasOwnership {
      return true
    }
    // Inlining of coroutines can result in improperly nested stack allocations.
    if self is BeginApplyInst {
      return true
    }
    return false
  }
}

extension GlobalVariable {
  /// Removes all `begin_access` and `end_access` instructions from the initializer.
  ///
  /// Access instructions are not allowed in the initializer, because the initializer must not contain
  /// instructions with side effects (initializer instructions are not executed).
  /// Exclusivity checking does not make sense in the initializer.
  ///
  /// The initializer functions of globals, which reference other globals by address, contain access
  /// instructions. After the initializing code is copied to the global's initializer, those access
  /// instructions must be stripped.
  func stripAccessInstructionFromInitializer(_ context: FunctionPassContext) {
    guard let initInsts = staticInitializerInstructions else {
      return
    }
    for initInst in initInsts {
      switch initInst {
      case let beginAccess as BeginAccessInst:
        beginAccess.uses.replaceAll(with: beginAccess.address, context)
        context.erase(instruction: beginAccess)
      case let endAccess as EndAccessInst:
        context.erase(instruction: endAccess)
      default:
        break
      }
    }
  }
}

extension InstructionRange {
  /// Adds the instruction range of a borrow-scope by transitively visiting all (potential) re-borrows.
  mutating func insert(borrowScopeOf borrow: BorrowIntroducingInstruction, _ context: some Context) {
    var worklist = ValueWorklist(context)
    defer { worklist.deinitialize() }

    worklist.pushIfNotVisited(borrow)
    while let value = worklist.pop() {
      for use in value.uses {
        switch use.instruction {
        case let endBorrow as EndBorrowInst:
          self.insert(endBorrow)
        case let branch as BranchInst:
          worklist.pushIfNotVisited(branch.getArgument(for: use).lookThroughBorrowedFromUser)
        default:
          break
        }
      }
    }
  }
}

/// Analyses the global initializer function and returns the `alloc_global` and `store`
/// instructions which initialize the global.
/// Returns nil if `function` has any side-effects beside initializing the global.
///
/// The function's single basic block must contain following code pattern:
/// ```
///   alloc_global @the_global
///   %a = global_addr @the_global
///   %i = some_const_initializer_insts
///   store %i to %a
/// ```
func getGlobalInitialization(
  of function: Function,
  forStaticInitializer: Bool,
  _ context: some Context
) -> (allocInst: AllocGlobalInst, storeToGlobal: StoreInst)? {
  guard let block = function.blocks.singleElement else {
    return nil
  }

  var allocInst: AllocGlobalInst? = nil
  var globalAddr: GlobalAddrInst? = nil
  var store: StoreInst? = nil

  for inst in block.instructions {
    switch inst {
    case is ReturnInst,
         is DebugValueInst,
         is DebugStepInst,
         is BeginAccessInst,
         is EndAccessInst:
      break
    case let agi as AllocGlobalInst:
      if allocInst != nil {
        return nil
      }
      allocInst = agi
    case let ga as GlobalAddrInst:
      if let agi = allocInst, agi.global == ga.global {
        globalAddr = ga
      }
    case let si as StoreInst:
      if store != nil {
        return nil
      }
      guard let ga = globalAddr else {
        return nil
      }
      if si.destination != ga {
        return nil
      }
      store = si
    case is GlobalValueInst where !forStaticInitializer:
      break
    default:
      if !inst.isValidInStaticInitializerOfGlobal(context) {
        return nil
      }
    }
  }
  if let store = store {
    return (allocInst: allocInst!, storeToGlobal: store)
  }
  return nil
}

func canDynamicallyCast(from sourceType: Type, to destType: Type, in function: Function, sourceTypeIsExact: Bool) -> Bool? {
  switch classifyDynamicCastBridged(sourceType.bridged, destType.bridged, function.bridged, sourceTypeIsExact) {
    case .willSucceed: return true
    case .maySucceed:  return nil
    case .willFail:    return false
    default: fatalError("unknown result from classifyDynamicCastBridged")
  }
}

extension CheckedCastAddrBranchInst {
  var dynamicCastResult: Bool? {
    switch classifyDynamicCastBridged(bridged) {
      case .willSucceed: return true
      case .maySucceed:  return nil
      case .willFail:    return false
      default: fatalError("unknown result from classifyDynamicCastBridged")
    }
  }
}
